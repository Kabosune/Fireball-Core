/*
 * state_scan.c — Native C11 Fireball-Core helper plugin `state_scan.fbplugin.dll`
 * for finding engine state (clock, weather) by value. Research aid; changes nothing
 * unless a write test is run.
 *
 *  - Snapshot hotkeys (Tier 0): F9 / F10 write `<snapshot_dir>\snap.request`
 *    containing "run" / "paused". VIctory's diagnostics hook `VICTORY_SNAPSHOT`
 *    (set to the same folder) copies the title's writable sections and removes
 *    the request. Copying from here would cost a VirtualQuery per u32 through
 *    the guarded API (about 1.3 us each, >= 22 s for .data), hence the split.
 *  - Watch (Tier 2): every frame, the u32 at each RVA in `watch_path`
 *    (one hex RVA per line, '#' comments; re-read when the file changes) is
 *    logged to `watch_log` when it changes.
 *  - Write test (Tier 2, F11): each line `rva value` of `write_tests_path`
 *    (value hex, decimal, or f<float>) is written once via title_mem_write_u32,
 *    read back at once, after 1 frame and after `verify_frames` frames; results
 *    go to `watch_log`. The file is re-read on every F11.
 * RVAs are relative to `image_symbol` (default "fb.image", which the host maps
 * to RVA 0, i.e. the image base).
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "fireball_plugin.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SS_MOD_ID    "community.state_scan"
#define SS_PLUGIN_ID "state_scan"
#define SS_VERSION   "0.1.0"
#define SS_MAX_WATCH 64
#define SS_MAX_TESTS 32
#define SS_MAX_VERIFY 128

typedef struct ss_verify {
    int      used;
    uint64_t due;
    uint32_t rva;
    uint32_t expected;
    char     tag[8];
} ss_verify_t;

typedef struct ss_state {
    fb_core_services_v1_t core;
    int      initialized;
    int      tier2;
    char     snapshot_dir[260];
    char     watch_path[260];
    char     write_tests_path[260];
    char     watch_log_path[260];
    char     image_symbol[64];
    uint32_t key_snap_run, key_snap_paused, key_write_test;
    uint64_t verify_frames;

    uint64_t image_va;
    uint64_t frame;
    uint32_t watch_rva[SS_MAX_WATCH];
    uint32_t watch_last[SS_MAX_WATCH];
    int      watch_seen[SS_MAX_WATCH];
    uint32_t watch_count;
    FILETIME watch_mtime;
    ss_verify_t verify[SS_MAX_VERIFY];
    FILE*    log;
    uint32_t snaps_requested;
    LARGE_INTEGER qpc_freq, qpc_start;
} ss_state_t;

static ss_state_t g_ss;

static void ss_log(int level, const char* msg) {
    if (g_ss.core.log) {
        g_ss.core.log(SS_MOD_ID, level, msg);
    }
}

static double ss_now_ms(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return g_ss.qpc_freq.QuadPart
               ? (double)(now.QuadPart - g_ss.qpc_start.QuadPart) * 1000.0 / (double)g_ss.qpc_freq.QuadPart
               : 0.0;
}

static void ss_csv(const char* event, uint32_t rva, uint32_t value, const char* note) {
    if (!g_ss.log) {
        return;
    }
    fprintf(g_ss.log, "%llu,%.1f,%s,0x%08X,0x%08X,%s\n", (unsigned long long)g_ss.frame, ss_now_ms(), event, rva,
            value, note ? note : "");
    fflush(g_ss.log);
}

/* ---- flat JSON config (same contract as fpv_toggle.c) ---- */

static const char* ss_json_find(const char* json, const char* key) {
    char pat[128];
    int n = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(pat)) {
        return NULL;
    }
    const char* p = strstr(json, pat);
    if (!p) {
        return NULL;
    }
    p += (size_t)n;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    if (*p != ':') {
        return NULL;
    }
    ++p;
    while (*p && isspace((unsigned char)*p)) {
        ++p;
    }
    return p;
}

static void ss_json_u64(const char* json, const char* key, uint64_t* io) {
    const char* v = ss_json_find(json, key);
    if (v && isdigit((unsigned char)*v)) {
        char* end = NULL;
        unsigned long long parsed = strtoull(v, &end, 0);
        if (end != v) {
            *io = (uint64_t)parsed;
        }
    }
}

static void ss_json_u32(const char* json, const char* key, uint32_t* io) {
    uint64_t v = *io;
    ss_json_u64(json, key, &v);
    *io = (uint32_t)v;
}

static void ss_json_str(const char* json, const char* key, char* out, size_t cap) {
    const char* v = ss_json_find(json, key);
    if (!v || *v != '"' || cap < 2) {
        return;
    }
    ++v;
    size_t len = 0;
    char tmp[512];
    while (*v && *v != '"' && len + 1 < sizeof(tmp)) {
        if (*v == '\\' && v[1] != '\0') {
            ++v;
        }
        tmp[len++] = *v++;
    }
    if (*v != '"') {
        return;
    }
    tmp[len] = '\0';
    snprintf(out, cap, "%s", tmp);
}

/* ---- snapshots ---- */

static void ss_request_snapshot(const char* label) {
    if (!g_ss.snapshot_dir[0]) {
        ss_log(FB_LOG_WARN, "[state_scan] snapshot_dir not set; F9/F10 do nothing");
        return;
    }
    char path[300];
    snprintf(path, sizeof(path), "%s\\snap.request", g_ss.snapshot_dir);
    FILE* f = fopen(path, "w");
    if (!f) {
        char msg[400];
        snprintf(msg, sizeof(msg), "[state_scan] cannot write %s (does the folder exist?)", path);
        ss_log(FB_LOG_WARN, msg);
        return;
    }
    fputs(label, f);
    fclose(f);
    g_ss.snaps_requested += 1;
    char msg[400];
    snprintf(msg, sizeof(msg), "[state_scan] snapshot #%u requested (%s) at frame %llu", g_ss.snaps_requested, label,
             (unsigned long long)g_ss.frame);
    ss_log(FB_LOG_INFO, msg);
    ss_csv("snapshot_request", 0, g_ss.snaps_requested, label);
}

/* ---- watch list ---- */

static int ss_file_mtime(const char* path, FILETIME* out) {
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) {
        return 0;
    }
    *out = d.ftLastWriteTime;
    return 1;
}

static void ss_load_watch(void) {
    g_ss.watch_count = 0;
    FILE* f = fopen(g_ss.watch_path, "r");
    if (!f) {
        return;
    }
    char line[128];
    while (fgets(line, sizeof(line), f) && g_ss.watch_count < SS_MAX_WATCH) {
        char* p = line;
        while (isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == '#' || *p == '\0') {
            continue;
        }
        char* end = NULL;
        unsigned long rva = strtoul(p, &end, 16);
        if (end != p && (rva & 3u) == 0) {
            g_ss.watch_rva[g_ss.watch_count] = (uint32_t)rva;
            g_ss.watch_seen[g_ss.watch_count] = 0;
            g_ss.watch_count += 1;
        }
    }
    fclose(f);
    char msg[128];
    snprintf(msg, sizeof(msg), "[state_scan] watching %u RVAs", g_ss.watch_count);
    ss_log(FB_LOG_INFO, msg);
}

static void ss_poll_watch_file(void) {
    if (!g_ss.watch_path[0] || (g_ss.frame % 30u) != 0) {
        return;
    }
    FILETIME t;
    if (ss_file_mtime(g_ss.watch_path, &t) && CompareFileTime(&t, &g_ss.watch_mtime) != 0) {
        g_ss.watch_mtime = t;
        ss_load_watch();
    }
}

/* ---- write tests ---- */

static void ss_schedule(uint32_t rva, uint32_t expected, uint64_t after, const char* tag) {
    for (int i = 0; i < SS_MAX_VERIFY; ++i) {
        if (!g_ss.verify[i].used) {
            g_ss.verify[i].used = 1;
            g_ss.verify[i].due = g_ss.frame + after;
            g_ss.verify[i].rva = rva;
            g_ss.verify[i].expected = expected;
            snprintf(g_ss.verify[i].tag, sizeof(g_ss.verify[i].tag), "%s", tag);
            return;
        }
    }
}

static void ss_run_write_tests(void) {
    FILE* f = g_ss.write_tests_path[0] ? fopen(g_ss.write_tests_path, "r") : NULL;
    if (!f) {
        ss_log(FB_LOG_WARN, "[state_scan] write_tests_path not readable; F11 does nothing");
        return;
    }
    char line[160];
    int n = 0;
    while (fgets(line, sizeof(line), f) && n < SS_MAX_TESTS) {
        char* p = line;
        while (isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == '#' || *p == '\0') {
            continue;
        }
        char* end = NULL;
        unsigned long rva = strtoul(p, &end, 16);
        if (end == p || (rva & 3u) != 0) {
            continue;
        }
        p = end;
        while (isspace((unsigned char)*p)) {
            ++p;
        }
        uint32_t value = 0;
        if (*p == 'f') {
            float fv = strtof(p + 1, &end);
            if (end == p + 1) {
                continue;
            }
            memcpy(&value, &fv, sizeof(value));
        } else {
            unsigned long v = strtoul(p, &end, 0);
            if (end == p) {
                continue;
            }
            value = (uint32_t)v;
        }
        uint64_t va = g_ss.image_va + rva;
        uint32_t before = 0, after = 0;
        int brc = g_ss.core.title_mem_read_u32(va, &before);
        int wrc = g_ss.core.title_mem_write_u32(va, value);
        int arc = g_ss.core.title_mem_read_u32(va, &after);
        char note[96];
        snprintf(note, sizeof(note), "before=0x%08X(%s) write_rc=%d readback=0x%08X(%s)", before,
                 brc == FB_OK ? "ok" : "fault", wrc, after, arc == FB_OK ? "ok" : "fault");
        ss_csv("write_test", (uint32_t)rva, value, note);
        if (wrc == FB_OK) {
            ss_schedule((uint32_t)rva, value, 1, "+1");
            ss_schedule((uint32_t)rva, value, g_ss.verify_frames, "+N");
        }
        ++n;
    }
    fclose(f);
    char msg[128];
    snprintf(msg, sizeof(msg), "[state_scan] write test: %d writes at frame %llu", n, (unsigned long long)g_ss.frame);
    ss_log(FB_LOG_INFO, msg);
}

/* ---- events ---- */

static void ss_frame(const fb_frame_event_v1_t* fe) {
    g_ss.frame = fe ? fe->frame_index : g_ss.frame + 1;
    if (!g_ss.tier2) {
        return;
    }
    ss_poll_watch_file();
    for (uint32_t i = 0; i < g_ss.watch_count; ++i) {
        uint32_t v = 0;
        if (g_ss.core.title_mem_read_u32(g_ss.image_va + g_ss.watch_rva[i], &v) != FB_OK) {
            continue;
        }
        if (!g_ss.watch_seen[i] || v != g_ss.watch_last[i]) {
            ss_csv("watch", g_ss.watch_rva[i], v, g_ss.watch_seen[i] ? "changed" : "first");
            g_ss.watch_last[i] = v;
            g_ss.watch_seen[i] = 1;
        }
    }
    for (int i = 0; i < SS_MAX_VERIFY; ++i) {
        ss_verify_t* v = &g_ss.verify[i];
        if (!v->used || v->due > g_ss.frame) {
            continue;
        }
        uint32_t rb = 0;
        int rc = g_ss.core.title_mem_read_u32(g_ss.image_va + v->rva, &rb);
        char note[48];
        snprintf(note, sizeof(note), "%s %s 0x%08X", v->tag,
                 rc != FB_OK ? "fault" : (rb == v->expected ? "kept" : "changed"), rb);
        ss_csv("write_verify", v->rva, v->expected, note);
        v->used = 0;
    }
}

static int ss_input(const fb_input_event_v1_t* ev) {
    if (!ev || ev->struct_size < sizeof(fb_input_event_v1_t) || ev->device_kind != 0u ||
        ev->action != (uint32_t)FB_INPUT_PRESS) {
        return 0;
    }
    if (ev->code == g_ss.key_snap_run) {
        ss_request_snapshot("run");
        return 1;
    }
    if (ev->code == g_ss.key_snap_paused) {
        ss_request_snapshot("paused");
        return 1;
    }
    if (ev->code == g_ss.key_write_test) {
        if (g_ss.tier2) {
            ss_run_write_tests();
        } else {
            ss_log(FB_LOG_WARN, "[state_scan] write test needs Tier 2 (FB_CAP_TITLE_MEM_RW + a symbol)");
        }
        return 1;
    }
    return 0;
}

static int ss_on_event(uint32_t event_id, const void* payload, void* user_ctx) {
    (void)user_ctx;
    if (!g_ss.initialized) {
        return 0;
    }
    if (event_id == (uint32_t)FB_EVENT_INPUT_EVENT) {
        return ss_input((const fb_input_event_v1_t*)payload);
    }
    if (event_id == (uint32_t)FB_EVENT_FRAME_BEGIN) {
        ss_frame((const fb_frame_event_v1_t*)payload);
    } else if (event_id == (uint32_t)FB_EVENT_SHUTDOWN) {
        fb_plugin_shutdown();
    }
    return 0;
}

FB_API int fb_plugin_query(fb_plugin_info_v1_t* out_info) {
    if (!out_info) {
        return FB_ERR_INVALID_ARG;
    }
    out_info->struct_size = (uint32_t)sizeof(fb_plugin_info_v1_t);
    out_info->abi_version = FB_PLUGIN_ABI_VERSION;
    out_info->mod_id = SS_MOD_ID;
    out_info->plugin_id = SS_PLUGIN_ID;
    out_info->version = SS_VERSION;
    /* Tier 2 (symbols, title memory) is optional: watch and write tests use it when present. */
    out_info->required_caps = FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS;
    return FB_OK;
}

FB_API int fb_plugin_init(const fb_core_services_v1_t* core) {
    if (!core) {
        return FB_ERR_INVALID_ARG;
    }
    if (core->abi_version != FB_PLUGIN_ABI_VERSION || core->struct_size != sizeof(fb_core_services_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (!core->register_event_listener) {
        return FB_ERR_UNSUPPORTED;
    }
    memset(&g_ss, 0, sizeof(g_ss));
    g_ss.core = *core;
    QueryPerformanceFrequency(&g_ss.qpc_freq);
    QueryPerformanceCounter(&g_ss.qpc_start);
    snprintf(g_ss.image_symbol, sizeof(g_ss.image_symbol), "%s", "fb.image");
    g_ss.key_snap_run = 0x78;    /* F9 */
    g_ss.key_snap_paused = 0x79; /* F10 */
    g_ss.key_write_test = 0x7A;  /* F11 */
    g_ss.verify_frames = 30;

    if (core->config_get_json) {
        static char cfg[4096];
        memset(cfg, 0, sizeof(cfg));
        if (core->config_get_json(SS_MOD_ID, cfg, sizeof(cfg)) == FB_OK) {
            ss_json_str(cfg, "snapshot_dir", g_ss.snapshot_dir, sizeof(g_ss.snapshot_dir));
            ss_json_str(cfg, "watch_path", g_ss.watch_path, sizeof(g_ss.watch_path));
            ss_json_str(cfg, "write_tests_path", g_ss.write_tests_path, sizeof(g_ss.write_tests_path));
            ss_json_str(cfg, "watch_log", g_ss.watch_log_path, sizeof(g_ss.watch_log_path));
            ss_json_str(cfg, "image_symbol", g_ss.image_symbol, sizeof(g_ss.image_symbol));
            ss_json_u32(cfg, "key_snap_run", &g_ss.key_snap_run);
            ss_json_u32(cfg, "key_snap_paused", &g_ss.key_snap_paused);
            ss_json_u32(cfg, "key_write_test", &g_ss.key_write_test);
            ss_json_u64(cfg, "verify_frames", &g_ss.verify_frames);
        }
    }

    const fb_cap_flags_t t2 = FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW;
    if ((core->active_host_caps & t2) == t2 && core->resolve_engine_symbol && core->title_mem_read_u32 &&
        core->title_mem_write_u32) {
        g_ss.image_va = (uint64_t)(uintptr_t)core->resolve_engine_symbol(g_ss.image_symbol);
        g_ss.tier2 = g_ss.image_va != 0;
    }
    if (g_ss.watch_log_path[0]) {
        g_ss.log = fopen(g_ss.watch_log_path, "a");
        if (g_ss.log) {
            fprintf(g_ss.log, "frame,t_ms,event,rva,value,note\n");
            fflush(g_ss.log);
        }
    }
    if (g_ss.tier2 && g_ss.watch_path[0]) {
        ss_file_mtime(g_ss.watch_path, &g_ss.watch_mtime);
        ss_load_watch();
    }

    int rc1 = core->register_event_listener((uint32_t)FB_EVENT_INPUT_EVENT, ss_on_event, &g_ss);
    int rc2 = core->register_event_listener((uint32_t)FB_EVENT_FRAME_BEGIN, ss_on_event, &g_ss);
    int rc3 = core->register_event_listener((uint32_t)FB_EVENT_SHUTDOWN, ss_on_event, &g_ss);
    if (rc1 != FB_OK || rc2 != FB_OK || rc3 != FB_OK) {
        return FB_ERR_INTERNAL;
    }
    g_ss.initialized = 1;
    char msg[512];
    snprintf(msg, sizeof(msg),
             "[state_scan] Initialized: snapshots -> %s (F9 run, F10 paused), tier2=%s (image %s @ 0x%016llX), "
             "watch=%u, log=%s",
             g_ss.snapshot_dir[0] ? g_ss.snapshot_dir : "(unset)", g_ss.tier2 ? "on" : "off", g_ss.image_symbol,
             (unsigned long long)g_ss.image_va, g_ss.watch_count, g_ss.log ? g_ss.watch_log_path : "(none)");
    ss_log(FB_LOG_INFO, msg);
    return FB_OK;
}

FB_API void fb_plugin_shutdown(void) {
    if (!g_ss.initialized) {
        return;
    }
    g_ss.initialized = 0;
    if (g_ss.log) {
        fclose(g_ss.log);
        g_ss.log = NULL;
    }
}
