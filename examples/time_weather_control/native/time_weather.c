/*
 * time_weather.c — Native C11 Tier 2 Fireball-Core plugin `time_weather.fbplugin.dll`.
 *
 * Single-player time-of-day and weather control through guarded title memory
 * (`title_mem_read_u32` / `title_mem_write_u32`) at addresses named in the
 * host's symbol table (VIctory: `[fireball.symbols]` in victory.toml).
 * Mirrors the reference state machine in `plugins/time_weather.py`.
 *
 * Clock: the time-of-day fields at `clock_symbol + clock_offset`, laid out as
 *   `clock_layout` (h,m,s | s,m,h | h,m | m,h | minutes_of_day | seconds_of_day
 *   | f32_hours), one u32 each. Set / +1h / -1h write them; pause and time
 *   scale are done by holding: every frame the plugin writes the time it owns
 *   (frozen, or advanced at `scale` x the game's rate).
 * Weather: u32 slots `weather_prev_offset` / `weather_next_offset` (and an
 *   optional f32 `weather_interp_offset`) from `weather_symbol`, holding either
 *   the weather's joaat_lower hash or its WeatherTypes index (`weather_kind`).
 *   Instant: prev = next = target, interp = 0. Blend: prev = current,
 *   next = target, interp ramps 0 -> 1 over `weather_blend_ms`. Lock: re-assert
 *   every frame.
 *
 * All writes happen in FB_EVENT_FRAME_BEGIN; hotkeys and the control window
 * only queue commands. Every write is read back at once and again
 * `verify_frames` frames later, and both results go to `log_path` (CSV).
 *
 * Which layout and offsets are right is what the research repo's
 * `state_scan_diff.py` + write tests establish (docs/runtime_clock_weather.md);
 * until then every default here is a placeholder.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "fireball_plugin.h"
#include "time_weather.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TW_MOD_ID       "community.time_weather_control"
#define TW_PLUGIN_ID    "time_weather"
#define TW_VERSION      "0.1.0"
#define TW_DEV_KEYBOARD 0u
#define TW_DAY_SEC      86400.0
#define TW_VERIFY_SLOTS 64

typedef enum tw_layout {
    TW_L_HMS = 0,
    TW_L_SMH,
    TW_L_HM,
    TW_L_MH,
    TW_L_MIN_OF_DAY,
    TW_L_SEC_OF_DAY,
    TW_L_F32_HOURS,
    TW_L_COUNT
} tw_layout_t;

static const char* const k_layout_names[TW_L_COUNT] = {
    "h,m,s", "s,m,h", "h,m", "m,h", "minutes_of_day", "seconds_of_day", "f32_hours",
};
static const uint32_t k_layout_fields[TW_L_COUNT] = {3, 3, 2, 2, 1, 1, 1};

typedef struct tw_verify {
    int      used;
    uint64_t due_frame;
    uint64_t va;
    uint32_t expected;
    char     field[24];
} tw_verify_t;

typedef struct tw_cmds {
    int      has_set_tod;
    double   set_tod;
    int      add_hours;
    int      toggle_pause;
    int      has_scale;
    double   scale;
    int      has_weather;
    int32_t  weather_index;
    int      weather_blend;
    int      has_lock;
    int      lock;
} tw_cmds_t;

typedef struct tw_state {
    fb_core_services_v1_t core;
    int                   initialized;

    /* config */
    char        clock_symbol[128];
    uint64_t    clock_offset;
    tw_layout_t layout;
    double      ms_per_game_minute;
    char        weather_symbol[128];
    int         weather_kind_index; /* 0 = hash, 1 = index */
    int         weather_hash_case_kept; /* 0 = joaat_lower, 1 = joaat */
    uint64_t    weather_prev_offset;
    uint64_t    weather_next_offset;
    uint64_t    weather_interp_offset;
    int         weather_has_interp;
    double      weather_blend_ms;
    int         weather_lock_on_set;
    uint64_t    verify_frames;
    char        log_path[260];
    char        weather_list_path[260];
    uint32_t    key_hour_up, key_hour_down, key_pause, key_weather_prev, key_weather_next, key_window;
    int         ui_enabled;

    /* resolved */
    int      clock_ok;
    uint64_t clock_va;
    int      weather_ok;
    uint64_t prev_va, next_va, interp_va;

    /* runtime (frame thread only) */
    uint64_t frame;
    int      paused;
    double   scale;
    double   owned_tod;      /* time held while paused or scaled */
    int32_t  weather_target; /* list index, -1 none */
    int      weather_locked;
    int      blending;
    double   blend_elapsed_ms;
    uint32_t blend_from_value;
    tw_verify_t verify[TW_VERIFY_SLOTS];
    FILE*    log;
    LARGE_INTEGER qpc_freq, qpc_start;

    /* shared (guarded by lock) */
    SRWLOCK     lock;
    tw_cmds_t   cmds;
    tw_status_t status;

    /* weather list (read-only after init) */
    tw_weather_entry_t weathers[TW_MAX_WEATHERS];
    uint32_t           weather_count;
} tw_state_t;

static tw_state_t g_tw;

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

static void tw_log(int level, const char* msg) {
    if (g_tw.core.log) {
        g_tw.core.log(TW_MOD_ID, level, msg);
    }
}

static double tw_now_ms(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (g_tw.qpc_freq.QuadPart == 0) {
        return 0.0;
    }
    return (double)(now.QuadPart - g_tw.qpc_start.QuadPart) * 1000.0 / (double)g_tw.qpc_freq.QuadPart;
}

static void tw_csv(const char* event, const char* field, uint64_t va, uint32_t written, int rb_rc, uint32_t readback,
                   const char* note) {
    if (!g_tw.log) {
        return;
    }
    fprintf(g_tw.log, "%llu,%.1f,%s,%s,0x%016llX,0x%08X,%s,0x%08X,%s\n", (unsigned long long)g_tw.frame,
            tw_now_ms(), event, field, (unsigned long long)va, written, rb_rc == FB_OK ? "ok" : "fault", readback,
            note ? note : "");
    fflush(g_tw.log);
}

/* ------------------------------------------------------------------------- */
/* Flat JSON config helpers (same contract as fpv_toggle.c)                  */
/* ------------------------------------------------------------------------- */

static const char* tw_json_find(const char* json, const char* key) {
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

static void tw_json_u64(const char* json, const char* key, uint64_t* io) {
    const char* v = tw_json_find(json, key);
    if (v && isdigit((unsigned char)*v)) {
        char* end = NULL;
        unsigned long long parsed = strtoull(v, &end, 0);
        if (end != v) {
            *io = (uint64_t)parsed;
        }
    }
}

static void tw_json_u32(const char* json, const char* key, uint32_t* io) {
    uint64_t v = *io;
    tw_json_u64(json, key, &v);
    *io = (uint32_t)v;
}

static void tw_json_double(const char* json, const char* key, double* io) {
    const char* v = tw_json_find(json, key);
    if (v && (isdigit((unsigned char)*v) || *v == '.')) {
        char* end = NULL;
        double parsed = strtod(v, &end);
        if (end != v) {
            *io = parsed;
        }
    }
}

static void tw_json_bool(const char* json, const char* key, int* io) {
    const char* v = tw_json_find(json, key);
    if (v && strncmp(v, "true", 4) == 0) {
        *io = 1;
    } else if (v && strncmp(v, "false", 5) == 0) {
        *io = 0;
    }
}

static void tw_json_str(const char* json, const char* key, char* out, size_t cap) {
    const char* v = tw_json_find(json, key);
    if (!v || *v != '"' || cap < 2) {
        return;
    }
    ++v;
    char tmp[512];
    size_t len = 0;
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

/* ------------------------------------------------------------------------- */
/* Clock fields                                                              */
/* ------------------------------------------------------------------------- */

static double tw_wrap_day(double t) {
    double r = fmod(t, TW_DAY_SEC);
    return r < 0.0 ? r + TW_DAY_SEC : r;
}

static float tw_f32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

static uint32_t tw_bits(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof(bits));
    return bits;
}

/* Reads the time of day (seconds). Returns 1 when every field is in range. */
static int tw_clock_read(double* out_tod) {
    uint32_t v[3] = {0, 0, 0};
    uint32_t n = k_layout_fields[g_tw.layout];
    for (uint32_t i = 0; i < n; ++i) {
        if (g_tw.core.title_mem_read_u32(g_tw.clock_va + 4ull * i, &v[i]) != FB_OK) {
            return 0;
        }
    }
    uint32_t h = 0, m = 0, s = 0;
    switch (g_tw.layout) {
    case TW_L_HMS: h = v[0]; m = v[1]; s = v[2]; break;
    case TW_L_SMH: s = v[0]; m = v[1]; h = v[2]; break;
    case TW_L_HM: h = v[0]; m = v[1]; break;
    case TW_L_MH: m = v[0]; h = v[1]; break;
    case TW_L_MIN_OF_DAY:
        if (v[0] >= 1440u) {
            return 0;
        }
        *out_tod = (double)v[0] * 60.0;
        return 1;
    case TW_L_SEC_OF_DAY:
        if (v[0] >= 86400u) {
            return 0;
        }
        *out_tod = (double)v[0];
        return 1;
    case TW_L_F32_HOURS: {
        float f = tw_f32(v[0]);
        if (!(f >= 0.0f && f < 24.0f)) {
            return 0;
        }
        *out_tod = (double)f * 3600.0;
        return 1;
    }
    default: return 0;
    }
    if (h >= 24u || m >= 60u || s >= 60u) {
        return 0;
    }
    *out_tod = (double)h * 3600.0 + (double)m * 60.0 + (double)s;
    return 1;
}

static void tw_schedule_verify(uint64_t va, uint32_t expected, const char* field) {
    for (int i = 0; i < TW_VERIFY_SLOTS; ++i) {
        if (!g_tw.verify[i].used) {
            g_tw.verify[i].used = 1;
            g_tw.verify[i].due_frame = g_tw.frame + g_tw.verify_frames;
            g_tw.verify[i].va = va;
            g_tw.verify[i].expected = expected;
            snprintf(g_tw.verify[i].field, sizeof(g_tw.verify[i].field), "%s", field);
            return;
        }
    }
}

/* One guarded write + immediate read-back; `quiet` (holds) logs only when the game had changed it. */
static int tw_write(uint64_t va, uint32_t value, const char* field, const char* event, int quiet) {
    uint32_t before = 0;
    int brc = g_tw.core.title_mem_read_u32(va, &before);
    if (quiet && brc == FB_OK && before == value) {
        return FB_OK;
    }
    int rc = g_tw.core.title_mem_write_u32(va, value);
    uint32_t rb = 0;
    int rrc = g_tw.core.title_mem_read_u32(va, &rb);
    if (rc != FB_OK) {
        char msg[200];
        snprintf(msg, sizeof(msg), "[time_weather] write %s @ 0x%016llX failed (rc=%d)", field,
                 (unsigned long long)va, rc);
        tw_log(FB_LOG_WARN, msg);
        tw_csv(event, field, va, value, rrc, rb, "write_failed");
        return rc;
    }
    char note[48];
    snprintf(note, sizeof(note), "before=0x%08X", before);
    tw_csv(event, field, va, value, rrc, rb, note);
    AcquireSRWLockExclusive(&g_tw.lock);
    g_tw.status.writes += 1;
    if (quiet) {
        g_tw.status.reasserts += 1;
    }
    ReleaseSRWLockExclusive(&g_tw.lock);
    if (!quiet) {
        tw_schedule_verify(va, value, field);
    }
    return FB_OK;
}

static void tw_clock_write(double tod, const char* event, int quiet) {
    tod = tw_wrap_day(tod);
    uint32_t total = (uint32_t)tod;
    uint32_t h = total / 3600u, m = (total % 3600u) / 60u, s = total % 60u;
    uint64_t va = g_tw.clock_va;
    switch (g_tw.layout) {
    case TW_L_HMS:
        tw_write(va, h, "clock.h", event, quiet);
        tw_write(va + 4, m, "clock.m", event, quiet);
        tw_write(va + 8, s, "clock.s", event, quiet);
        break;
    case TW_L_SMH:
        tw_write(va, s, "clock.s", event, quiet);
        tw_write(va + 4, m, "clock.m", event, quiet);
        tw_write(va + 8, h, "clock.h", event, quiet);
        break;
    case TW_L_HM:
        tw_write(va, h, "clock.h", event, quiet);
        tw_write(va + 4, m, "clock.m", event, quiet);
        break;
    case TW_L_MH:
        tw_write(va, m, "clock.m", event, quiet);
        tw_write(va + 4, h, "clock.h", event, quiet);
        break;
    case TW_L_MIN_OF_DAY: tw_write(va, total / 60u, "clock.min_of_day", event, quiet); break;
    case TW_L_SEC_OF_DAY: tw_write(va, total, "clock.sec_of_day", event, quiet); break;
    case TW_L_F32_HOURS: tw_write(va, tw_bits((float)(tod / 3600.0)), "clock.f32_hours", event, quiet); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* Weather                                                                   */
/* ------------------------------------------------------------------------- */

static uint32_t tw_weather_value(int32_t idx) {
    if (g_tw.weather_kind_index) {
        return g_tw.weather_count ? g_tw.weathers[idx].index : (uint32_t)idx;
    }
    return g_tw.weather_hash_case_kept ? g_tw.weathers[idx].joaat : g_tw.weathers[idx].joaat_lower;
}

static int32_t tw_weather_lookup(uint32_t value) {
    for (uint32_t i = 0; i < g_tw.weather_count; ++i) {
        if (tw_weather_value((int32_t)i) == value) {
            return (int32_t)i;
        }
    }
    return -1;
}

static void tw_load_weather_list(void) {
    g_tw.weather_count = 0;
    if (g_tw.weather_list_path[0] == '\0') {
        return;
    }
    FILE* f = fopen(g_tw.weather_list_path, "r");
    if (!f) {
        char msg[400];
        snprintf(msg, sizeof(msg), "[time_weather] weather list '%s' not readable", g_tw.weather_list_path);
        tw_log(FB_LOG_WARN, msg);
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f) && g_tw.weather_count < TW_MAX_WEATHERS) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        unsigned idx = 0, lower = 0, kept = 0;
        char name[TW_NAME_MAX];
        if (sscanf(line, "%u %x %x %47s", &idx, &lower, &kept, name) == 4) {
            tw_weather_entry_t* e = &g_tw.weathers[g_tw.weather_count++];
            e->index = idx;
            e->joaat_lower = lower;
            e->joaat = kept;
            snprintf(e->name, sizeof(e->name), "%s", name);
        }
    }
    fclose(f);
}

static void tw_weather_write_all(uint32_t prev, uint32_t next, float interp, const char* event, int quiet) {
    tw_write(g_tw.prev_va, prev, "weather.prev", event, quiet);
    tw_write(g_tw.next_va, next, "weather.next", event, quiet);
    if (g_tw.weather_has_interp) {
        tw_write(g_tw.interp_va, tw_bits(interp), "weather.interp", event, quiet);
    }
}

/* ------------------------------------------------------------------------- */
/* Commands (any thread)                                                     */
/* ------------------------------------------------------------------------- */

void tw_cmd_set_tod(double tod_sec) {
    AcquireSRWLockExclusive(&g_tw.lock);
    g_tw.cmds.has_set_tod = 1;
    g_tw.cmds.set_tod = tod_sec;
    g_tw.cmds.add_hours = 0;
    ReleaseSRWLockExclusive(&g_tw.lock);
}

void tw_cmd_add_hours(int hours) {
    AcquireSRWLockExclusive(&g_tw.lock);
    g_tw.cmds.add_hours += hours;
    ReleaseSRWLockExclusive(&g_tw.lock);
}

void tw_cmd_toggle_pause(void) {
    AcquireSRWLockExclusive(&g_tw.lock);
    g_tw.cmds.toggle_pause ^= 1;
    ReleaseSRWLockExclusive(&g_tw.lock);
}

void tw_cmd_set_scale(double scale) {
    AcquireSRWLockExclusive(&g_tw.lock);
    g_tw.cmds.has_scale = 1;
    g_tw.cmds.scale = scale < 0.0 ? 0.0 : scale;
    ReleaseSRWLockExclusive(&g_tw.lock);
}

void tw_cmd_set_weather(int32_t list_index, int blend) {
    AcquireSRWLockExclusive(&g_tw.lock);
    g_tw.cmds.has_weather = 1;
    g_tw.cmds.weather_index = list_index;
    g_tw.cmds.weather_blend = blend;
    ReleaseSRWLockExclusive(&g_tw.lock);
}

void tw_cmd_set_lock(int locked) {
    AcquireSRWLockExclusive(&g_tw.lock);
    g_tw.cmds.has_lock = 1;
    g_tw.cmds.lock = locked ? 1 : 0;
    ReleaseSRWLockExclusive(&g_tw.lock);
}

void tw_get_status(tw_status_t* out) {
    AcquireSRWLockShared(&g_tw.lock);
    *out = g_tw.status;
    ReleaseSRWLockShared(&g_tw.lock);
}

uint32_t tw_weather_count(void) {
    return g_tw.weather_count;
}

const tw_weather_entry_t* tw_weather_at(uint32_t i) {
    return i < g_tw.weather_count ? &g_tw.weathers[i] : NULL;
}

double tw_ms_per_game_minute(void) {
    return g_tw.ms_per_game_minute;
}

/* ------------------------------------------------------------------------- */
/* Frame                                                                     */
/* ------------------------------------------------------------------------- */

static void tw_run_verifies(void) {
    for (int i = 0; i < TW_VERIFY_SLOTS; ++i) {
        tw_verify_t* v = &g_tw.verify[i];
        if (!v->used || v->due_frame > g_tw.frame) {
            continue;
        }
        uint32_t rb = 0;
        int rc = g_tw.core.title_mem_read_u32(v->va, &rb);
        tw_csv("verify", v->field, v->va, v->expected, rc, rb,
               rc != FB_OK ? "fault" : (rb == v->expected ? "kept" : "changed"));
        v->used = 0;
    }
}

static void tw_frame(const fb_frame_event_v1_t* fe) {
    g_tw.frame = fe ? fe->frame_index : g_tw.frame + 1;
    double dt = fe ? fe->dt_sec : 0.0;
    if (dt < 0.0 || dt > 1.0) {
        dt = 0.0; /* a stall (load screen, debugger) is not game time */
    }

    tw_cmds_t c;
    AcquireSRWLockExclusive(&g_tw.lock);
    c = g_tw.cmds;
    memset(&g_tw.cmds, 0, sizeof(g_tw.cmds));
    ReleaseSRWLockExclusive(&g_tw.lock);

    /* --- clock --- */
    double tod = 0.0;
    int have_tod = g_tw.clock_ok && tw_clock_read(&tod);
    if (g_tw.clock_ok) {
        double rate = 60000.0 / (g_tw.ms_per_game_minute > 0.0 ? g_tw.ms_per_game_minute : 2000.0);
        int owning = g_tw.paused || g_tw.scale != 1.0;
        if (c.has_scale) {
            if (!owning && have_tod) {
                g_tw.owned_tod = tod;
            }
            g_tw.scale = c.scale;
        }
        if (c.toggle_pause) {
            g_tw.paused = !g_tw.paused;
            if (g_tw.paused && !owning && have_tod) {
                g_tw.owned_tod = tod;
            }
            tw_log(FB_LOG_INFO, g_tw.paused ? "[time_weather] clock paused" : "[time_weather] clock resumed");
        }
        owning = g_tw.paused || g_tw.scale != 1.0;
        double base = owning ? g_tw.owned_tod : tod;
        int explicit_set = 0;
        if (c.has_set_tod) {
            base = c.set_tod;
            explicit_set = 1;
        }
        if (c.add_hours != 0 && (have_tod || owning || explicit_set)) {
            base += 3600.0 * (double)c.add_hours;
            explicit_set = 1;
        }
        if (explicit_set) {
            base = tw_wrap_day(base);
            tw_clock_write(base, "set", 0);
            g_tw.owned_tod = base;
            tod = base;
            have_tod = 1;
        } else if (owning) {
            if (!g_tw.paused) {
                g_tw.owned_tod = tw_wrap_day(g_tw.owned_tod + dt * rate * g_tw.scale);
            }
            tw_clock_write(g_tw.owned_tod, g_tw.paused ? "hold_pause" : "hold_scale", 1);
            tod = g_tw.owned_tod;
        }
    }

    /* --- weather --- */
    uint32_t next_now = 0;
    int have_next = g_tw.weather_ok && g_tw.core.title_mem_read_u32(g_tw.next_va, &next_now) == FB_OK;
    if (g_tw.weather_ok) {
        if (c.has_lock) {
            g_tw.weather_locked = c.lock;
        }
        if (c.has_weather && c.weather_index >= 0 &&
            (g_tw.weather_count == 0 ? g_tw.weather_kind_index != 0 : (uint32_t)c.weather_index < g_tw.weather_count)) {
            uint32_t tv = tw_weather_value(c.weather_index);
            g_tw.weather_target = c.weather_index;
            if (g_tw.weather_lock_on_set) {
                g_tw.weather_locked = 1;
            }
            if (c.weather_blend && g_tw.weather_has_interp) {
                g_tw.blending = 1;
                g_tw.blend_elapsed_ms = 0.0;
                g_tw.blend_from_value = have_next ? next_now : tv;
                tw_weather_write_all(g_tw.blend_from_value, tv, 0.0f, "weather_blend", 0);
            } else {
                g_tw.blending = 0;
                tw_weather_write_all(tv, tv, 0.0f, "weather_set", 0);
            }
        } else if (g_tw.blending && g_tw.weather_target >= 0) {
            uint32_t tv = tw_weather_value(g_tw.weather_target);
            g_tw.blend_elapsed_ms += dt * 1000.0;
            double p = g_tw.weather_blend_ms > 0.0 ? g_tw.blend_elapsed_ms / g_tw.weather_blend_ms : 1.0;
            if (p >= 1.0) {
                g_tw.blending = 0;
                tw_weather_write_all(tv, tv, 0.0f, "weather_blend_done", 0);
            } else {
                tw_weather_write_all(g_tw.blend_from_value, tv, (float)p, "hold_blend", 1);
            }
        } else if (g_tw.weather_locked && g_tw.weather_target >= 0) {
            uint32_t tv = tw_weather_value(g_tw.weather_target);
            tw_weather_write_all(tv, tv, 0.0f, "hold_lock", 1);
        }
    }

    tw_run_verifies();

    AcquireSRWLockExclusive(&g_tw.lock);
    g_tw.status.clock_ok = g_tw.clock_ok && have_tod;
    g_tw.status.tod_sec = tod;
    g_tw.status.paused = g_tw.paused;
    g_tw.status.scale = g_tw.scale;
    g_tw.status.weather_ok = g_tw.weather_ok;
    g_tw.status.weather_cur = have_next ? tw_weather_lookup(next_now) : -1;
    g_tw.status.weather_target = g_tw.weather_target;
    g_tw.status.weather_locked = g_tw.weather_locked;
    g_tw.status.blend_progress =
        g_tw.blending && g_tw.weather_blend_ms > 0.0 ? g_tw.blend_elapsed_ms / g_tw.weather_blend_ms : 1.0;
    ReleaseSRWLockExclusive(&g_tw.lock);
}

/* ------------------------------------------------------------------------- */
/* Input                                                                     */
/* ------------------------------------------------------------------------- */

static int32_t tw_weather_step(int dir) {
    uint32_t n = g_tw.weather_count ? g_tw.weather_count : (g_tw.weather_kind_index ? 168u : 0u);
    if (n == 0) {
        tw_log(FB_LOG_WARN, "[time_weather] no weather list (weather_list_path); weather hotkeys off");
        return -1;
    }
    tw_status_t st;
    tw_get_status(&st);
    int32_t from = st.weather_target >= 0 ? st.weather_target : (st.weather_cur >= 0 ? st.weather_cur : 0);
    int32_t next = (int32_t)(((int64_t)from + dir + (int64_t)n) % (int64_t)n);
    return next;
}

static int tw_input(const fb_input_event_v1_t* ev) {
    if (!ev || ev->struct_size < sizeof(fb_input_event_v1_t) || ev->device_kind != TW_DEV_KEYBOARD ||
        ev->action != (uint32_t)FB_INPUT_PRESS) {
        return 0;
    }
    uint32_t k = ev->code;
    if (k == 0) {
        return 0;
    }
    if (g_tw.clock_ok && k == g_tw.key_hour_up) {
        tw_cmd_add_hours(1);
        return 1;
    }
    if (g_tw.clock_ok && k == g_tw.key_hour_down) {
        tw_cmd_add_hours(-1);
        return 1;
    }
    if (g_tw.clock_ok && k == g_tw.key_pause) {
        tw_cmd_toggle_pause();
        return 1;
    }
    if (g_tw.weather_ok && (k == g_tw.key_weather_prev || k == g_tw.key_weather_next)) {
        int32_t idx = tw_weather_step(k == g_tw.key_weather_next ? 1 : -1);
        if (idx >= 0) {
            tw_cmd_set_weather(idx, 0);
            char msg[160];
            const tw_weather_entry_t* e = tw_weather_at((uint32_t)idx);
            snprintf(msg, sizeof(msg), "[time_weather] weather -> #%d %s", idx, e ? e->name : "");
            tw_log(FB_LOG_INFO, msg);
        }
        return 1;
    }
    if (g_tw.ui_enabled && k == g_tw.key_window) {
        tw_ui_toggle();
        return 1;
    }
    return 0;
}

static int tw_on_event(uint32_t event_id, const void* payload, void* user_ctx) {
    (void)user_ctx;
    if (!g_tw.initialized) {
        return 0;
    }
    if (event_id == (uint32_t)FB_EVENT_INPUT_EVENT) {
        return tw_input((const fb_input_event_v1_t*)payload);
    }
    if (event_id == (uint32_t)FB_EVENT_FRAME_BEGIN) {
        tw_frame((const fb_frame_event_v1_t*)payload);
        return 0;
    }
    if (event_id == (uint32_t)FB_EVENT_SHUTDOWN) {
        fb_plugin_shutdown();
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* ABI entry points                                                          */
/* ------------------------------------------------------------------------- */

FB_API int fb_plugin_query(fb_plugin_info_v1_t* out_info) {
    if (!out_info) {
        return FB_ERR_INVALID_ARG;
    }
    out_info->struct_size = (uint32_t)sizeof(fb_plugin_info_v1_t);
    out_info->abi_version = FB_PLUGIN_ABI_VERSION;
    out_info->mod_id = TW_MOD_ID;
    out_info->plugin_id = TW_PLUGIN_ID;
    out_info->version = TW_VERSION;
    out_info->required_caps =
        FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS | FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW;
    return FB_OK;
}

static void tw_defaults(void) {
    snprintf(g_tw.clock_symbol, sizeof(g_tw.clock_symbol), "%s", "fb.clock");
    g_tw.layout = TW_L_HMS;
    g_tw.ms_per_game_minute = 2000.0;
    snprintf(g_tw.weather_symbol, sizeof(g_tw.weather_symbol), "%s", "fb.weather");
    g_tw.weather_prev_offset = 0;
    g_tw.weather_next_offset = 4;
    g_tw.weather_interp_offset = 8;
    g_tw.weather_has_interp = 1;
    g_tw.weather_blend_ms = 10000.0;
    g_tw.weather_lock_on_set = 1;
    g_tw.verify_frames = 30;
    g_tw.key_hour_up = 0xDD;      /* ']' VK_OEM_6 */
    g_tw.key_hour_down = 0xDB;    /* '[' VK_OEM_4 */
    g_tw.key_pause = 0xDC;        /* '\' VK_OEM_5 */
    g_tw.key_weather_prev = 0xBA; /* ';' VK_OEM_1 */
    g_tw.key_weather_next = 0xDE; /* ''' VK_OEM_7 */
    g_tw.key_window = 0x77;       /* F8 */
    g_tw.ui_enabled = 1;
    g_tw.scale = 1.0;
    g_tw.weather_target = -1;
}

static void tw_read_config(void) {
    if (!g_tw.core.config_get_json) {
        return;
    }
    static char cfg[8192];
    memset(cfg, 0, sizeof(cfg));
    if (g_tw.core.config_get_json(TW_MOD_ID, cfg, sizeof(cfg)) != FB_OK) {
        return;
    }
    char layout[32] = "";
    char kind[16] = "";
    char variant[16] = "";
    tw_json_str(cfg, "clock_symbol", g_tw.clock_symbol, sizeof(g_tw.clock_symbol));
    tw_json_u64(cfg, "clock_offset", &g_tw.clock_offset);
    tw_json_str(cfg, "clock_layout", layout, sizeof(layout));
    tw_json_double(cfg, "clock_ms_per_game_minute", &g_tw.ms_per_game_minute);
    tw_json_str(cfg, "weather_symbol", g_tw.weather_symbol, sizeof(g_tw.weather_symbol));
    tw_json_str(cfg, "weather_kind", kind, sizeof(kind));
    tw_json_str(cfg, "weather_hash_variant", variant, sizeof(variant));
    tw_json_u64(cfg, "weather_prev_offset", &g_tw.weather_prev_offset);
    tw_json_u64(cfg, "weather_next_offset", &g_tw.weather_next_offset);
    tw_json_u64(cfg, "weather_interp_offset", &g_tw.weather_interp_offset);
    tw_json_bool(cfg, "weather_has_interp", &g_tw.weather_has_interp);
    tw_json_double(cfg, "weather_blend_ms", &g_tw.weather_blend_ms);
    tw_json_bool(cfg, "weather_lock_on_set", &g_tw.weather_lock_on_set);
    tw_json_str(cfg, "weather_list_path", g_tw.weather_list_path, sizeof(g_tw.weather_list_path));
    tw_json_u64(cfg, "verify_frames", &g_tw.verify_frames);
    tw_json_str(cfg, "log_path", g_tw.log_path, sizeof(g_tw.log_path));
    tw_json_u32(cfg, "key_hour_up", &g_tw.key_hour_up);
    tw_json_u32(cfg, "key_hour_down", &g_tw.key_hour_down);
    tw_json_u32(cfg, "key_pause", &g_tw.key_pause);
    tw_json_u32(cfg, "key_weather_prev", &g_tw.key_weather_prev);
    tw_json_u32(cfg, "key_weather_next", &g_tw.key_weather_next);
    tw_json_u32(cfg, "key_window", &g_tw.key_window);
    tw_json_bool(cfg, "ui_enabled", &g_tw.ui_enabled);
    for (int i = 0; i < TW_L_COUNT; ++i) {
        if (layout[0] && strcmp(layout, k_layout_names[i]) == 0) {
            g_tw.layout = (tw_layout_t)i;
        }
    }
    if (kind[0]) {
        g_tw.weather_kind_index = strcmp(kind, "index") == 0;
    }
    if (variant[0]) {
        g_tw.weather_hash_case_kept = strcmp(variant, "joaat") == 0;
    }
}

FB_API int fb_plugin_init(const fb_core_services_v1_t* core) {
    if (!core) {
        return FB_ERR_INVALID_ARG;
    }
    if (core->abi_version != FB_PLUGIN_ABI_VERSION || core->struct_size != sizeof(fb_core_services_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (!core->register_event_listener || !core->resolve_engine_symbol || !core->title_mem_read_u32 ||
        !core->title_mem_write_u32) {
        return FB_ERR_UNSUPPORTED;
    }

    memset(&g_tw, 0, sizeof(g_tw));
    InitializeSRWLock(&g_tw.lock);
    g_tw.core = *core;
    QueryPerformanceFrequency(&g_tw.qpc_freq);
    QueryPerformanceCounter(&g_tw.qpc_start);
    tw_defaults();
    tw_read_config();
    tw_load_weather_list();

    void* clock_sym = core->resolve_engine_symbol(g_tw.clock_symbol);
    if (clock_sym) {
        g_tw.clock_va = (uint64_t)(uintptr_t)clock_sym + g_tw.clock_offset;
        g_tw.clock_ok = 1;
    }
    void* weather_sym = core->resolve_engine_symbol(g_tw.weather_symbol);
    if (weather_sym && (g_tw.weather_kind_index || g_tw.weather_count > 0)) {
        uint64_t b = (uint64_t)(uintptr_t)weather_sym;
        g_tw.prev_va = b + g_tw.weather_prev_offset;
        g_tw.next_va = b + g_tw.weather_next_offset;
        g_tw.interp_va = b + g_tw.weather_interp_offset;
        g_tw.weather_ok = 1;
    }
    if (!g_tw.clock_ok && !g_tw.weather_ok) {
        char msg[400];
        snprintf(msg, sizeof(msg),
                 "[time_weather] neither '%s' nor '%s' (with a weather list) resolved; plugin standing by.",
                 g_tw.clock_symbol, g_tw.weather_symbol);
        tw_log(FB_LOG_WARN, msg);
        return FB_ERR_NOT_FOUND;
    }

    if (g_tw.log_path[0]) {
        g_tw.log = fopen(g_tw.log_path, "a");
        if (g_tw.log) {
            fprintf(g_tw.log, "frame,t_ms,event,field,va,written,readback_rc,readback,note\n");
            fflush(g_tw.log);
        }
    }

    int rc1 = core->register_event_listener((uint32_t)FB_EVENT_INPUT_EVENT, tw_on_event, &g_tw);
    int rc2 = core->register_event_listener((uint32_t)FB_EVENT_FRAME_BEGIN, tw_on_event, &g_tw);
    int rc3 = core->register_event_listener((uint32_t)FB_EVENT_SHUTDOWN, tw_on_event, &g_tw);
    if (rc1 != FB_OK || rc2 != FB_OK || rc3 != FB_OK) {
        return FB_ERR_INTERNAL;
    }
    g_tw.initialized = 1;

    char msg[512];
    snprintf(msg, sizeof(msg),
             "[time_weather] Initialized: clock=%s (%s @ 0x%016llX, %s, %.0f ms/game-min) weather=%s (%s @ 0x%016llX, "
             "%s, %u names) log=%s",
             g_tw.clock_ok ? "on" : "off", g_tw.clock_symbol, (unsigned long long)g_tw.clock_va,
             k_layout_names[g_tw.layout], g_tw.ms_per_game_minute, g_tw.weather_ok ? "on" : "off",
             g_tw.weather_symbol, (unsigned long long)g_tw.prev_va, g_tw.weather_kind_index ? "index" : "hash",
             g_tw.weather_count, g_tw.log ? g_tw.log_path : "(none)");
    tw_log(FB_LOG_INFO, msg);
    return FB_OK;
}

FB_API void fb_plugin_shutdown(void) {
    if (!g_tw.initialized) {
        return;
    }
    g_tw.initialized = 0;
    tw_ui_shutdown();
    /* Holds end here: the game's own clock and weather take over from the last written state. */
    g_tw.paused = 0;
    g_tw.scale = 1.0;
    g_tw.weather_locked = 0;
    if (g_tw.log) {
        fclose(g_tw.log);
        g_tw.log = NULL;
    }
    tw_log(FB_LOG_INFO, "[time_weather] Shutdown: holds released");
}
