/*
 * plugin_examples_test.c — CTest for the example Tier 2 plugins
 * `time_weather.fbplugin.dll` and `state_scan.fbplugin.dll`.
 *
 * Loads each DLL directly and drives it through a mock `fb_core_services_v1_t`:
 * a 4 KiB fake title region, a symbol table, guarded u32 access, a captured
 * event bus, and flat JSON config. No fireball_core.dll, bundle or game needed.
 *
 *   plugin_examples_test --time-weather <dll> --state-scan <dll>
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "fireball_plugin.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(c)                                                                      \
    do {                                                                              \
        if (!(c)) {                                                                   \
            ++g_failures;                                                             \
            printf("  FAIL line %d: %s\n", __LINE__, #c);                             \
        }                                                                             \
    } while (0)

/* ---- mock title memory & services ---- */

#define MEM_SIZE 4096u
static uint8_t  g_mem[MEM_SIZE];
static char     g_cfg[4096];
static int      g_tier2_symbols = 1;
static fb_event_fn_t g_listeners[8];
static void*    g_listener_ctx[8];
static char     g_last_log[1024];

static uint64_t va_of(uint32_t off) {
    return (uint64_t)(uintptr_t)(g_mem + off);
}

static uint32_t mem_u32(uint32_t off) {
    uint32_t v;
    memcpy(&v, g_mem + off, 4);
    return v;
}

static void set_u32(uint32_t off, uint32_t v) {
    memcpy(g_mem + off, &v, 4);
}

static void m_log(const char* mod_id, int level, const char* msg) {
    (void)level;
    snprintf(g_last_log, sizeof(g_last_log), "%s: %s", mod_id, msg);
    printf("    [log] %s\n", g_last_log);
}

static int m_config(const char* mod_id, char* buf, size_t cap) {
    (void)mod_id;
    size_t n = strlen(g_cfg);
    if (n + 1 > cap) {
        return FB_ERR_INVALID_ARG;
    }
    memcpy(buf, g_cfg, n + 1);
    return FB_OK;
}

static int m_listen(uint32_t event_id, fb_event_fn_t fn, void* ctx) {
    if (event_id >= 8) {
        return FB_ERR_INVALID_ARG;
    }
    g_listeners[event_id] = fn;
    g_listener_ctx[event_id] = ctx;
    return FB_OK;
}

static int m_poll(uint32_t d, uint32_t c) {
    (void)d;
    (void)c;
    return 0;
}

static void* m_resolve(const char* name) {
    if (!g_tier2_symbols) {
        return NULL;
    }
    if (strcmp(name, "fb.image") == 0) {
        return g_mem;
    }
    if (strcmp(name, "fb.clock") == 0) {
        return g_mem + 0x100;
    }
    if (strcmp(name, "fb.weather") == 0) {
        return g_mem + 0x200;
    }
    return NULL;
}

static int in_mem(uint64_t va) {
    return va >= va_of(0) && va + 4 <= va_of(MEM_SIZE) && (va & 3u) == 0;
}

static int m_read(uint64_t va, uint32_t* out) {
    if (!in_mem(va)) {
        return FB_ERR_MEM_FAULT;
    }
    memcpy(out, (const void*)(uintptr_t)va, 4);
    return FB_OK;
}

static int m_write(uint64_t va, uint32_t v) {
    if (!in_mem(va)) {
        return FB_ERR_MEM_FAULT;
    }
    memcpy((void*)(uintptr_t)va, &v, 4);
    return FB_OK;
}

static fb_core_services_v1_t make_services(void) {
    fb_core_services_v1_t s;
    memset(&s, 0, sizeof(s));
    s.struct_size = (uint32_t)sizeof(s);
    s.abi_version = FB_PLUGIN_ABI_VERSION;
    s.active_host_caps = FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS | FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW;
    s.log = m_log;
    s.config_get_json = m_config;
    s.register_event_listener = m_listen;
    s.poll_key_state = m_poll;
    s.resolve_engine_symbol = m_resolve;
    s.title_mem_read_u32 = m_read;
    s.title_mem_write_u32 = m_write;
    return s;
}

static uint64_t g_frame = 0;
static void frame(void) {
    fb_frame_event_v1_t fe = {++g_frame, 1.0 / 30.0};
    if (g_listeners[FB_EVENT_FRAME_BEGIN]) {
        g_listeners[FB_EVENT_FRAME_BEGIN](FB_EVENT_FRAME_BEGIN, &fe, g_listener_ctx[FB_EVENT_FRAME_BEGIN]);
    }
}

static int press(uint32_t vk) {
    fb_input_event_v1_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = (uint32_t)sizeof(ev);
    ev.device_kind = 0;
    ev.code = vk;
    ev.action = FB_INPUT_PRESS;
    return g_listeners[FB_EVENT_INPUT_EVENT](FB_EVENT_INPUT_EVENT, &ev, g_listener_ctx[FB_EVENT_INPUT_EVENT]);
}

static int file_contains(const char* path, const char* needle) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

typedef int (*query_fn)(fb_plugin_info_v1_t*);
typedef int (*init_fn)(const fb_core_services_v1_t*);
typedef void (*shutdown_fn)(void);

typedef struct plugin {
    HMODULE     mod;
    query_fn    query;
    init_fn     init;
    shutdown_fn shutdown;
} plugin_t;

static int load_plugin(const wchar_t* path, plugin_t* p) {
    p->mod = LoadLibraryW(path);
    if (!p->mod) {
        printf("  cannot load %ls (error %lu)\n", path, (unsigned long)GetLastError());
        return 0;
    }
    p->query = (query_fn)(void*)GetProcAddress(p->mod, "fb_plugin_query");
    p->init = (init_fn)(void*)GetProcAddress(p->mod, "fb_plugin_init");
    p->shutdown = (shutdown_fn)(void*)GetProcAddress(p->mod, "fb_plugin_shutdown");
    return p->query && p->init && p->shutdown;
}

static void json_path(char* out, size_t cap, const char* path) {
    size_t j = 0;
    for (size_t i = 0; path[i] && j + 2 < cap; ++i) {
        if (path[i] == '\\') {
            out[j++] = '\\';
        }
        out[j++] = path[i];
    }
    out[j] = '\0';
}

/* Hash values as weather_time_inspect.py hashes writes them (joaat_lower); any distinct values do here. */
#define H0 0xF5A87B65u
#define H1 0x614A1F91u
#define H2 0x5974E8E5u

static void test_time_weather(const wchar_t* dll, const char* tmp) {
    printf("[time_weather]\n");
    plugin_t p;
    if (!load_plugin(dll, &p)) {
        CHECK(0);
        return;
    }
    fb_plugin_info_v1_t info;
    CHECK(p.query(&info) == FB_OK);
    CHECK(strcmp(info.mod_id, "community.time_weather_control") == 0);
    CHECK((info.required_caps & FB_CAP_TITLE_MEM_RW) != 0);

    char list[300], log[300], jlist[600], jlog[600];
    snprintf(list, sizeof(list), "%s\\tw_weather_types.txt", tmp);
    snprintf(log, sizeof(log), "%s\\tw_log.csv", tmp);
    DeleteFileA(log);
    FILE* f = fopen(list, "w");
    fprintf(f, "# index joaat_lower joaat name\n0 0x%08X 0x1 HIGHPRESSURE\n1 0x%08X 0x2 SUNNY\n2 0x%08X 0x3 MISTY\n", H0,
            H1, H2);
    fclose(f);
    json_path(jlist, sizeof(jlist), list);
    json_path(jlog, sizeof(jlog), log);
    snprintf(g_cfg, sizeof(g_cfg),
             "{\"clock_layout\":\"h,m,s\",\"clock_ms_per_game_minute\":2000,\"clock_offset\":0,\"clock_symbol\":"
             "\"fb.clock\",\"log_path\":\"%s\",\"ui_enabled\":false,\"verify_frames\":3,\"weather_blend_ms\":100,"
             "\"weather_has_interp\":true,\"weather_interp_offset\":8,\"weather_kind\":\"hash\",\"weather_list_path\":"
             "\"%s\",\"weather_next_offset\":4,\"weather_prev_offset\":0,\"weather_symbol\":\"fb.weather\"}",
             jlog, jlist);

    memset(g_mem, 0, sizeof(g_mem));
    memset(g_listeners, 0, sizeof(g_listeners));
    set_u32(0x100, 10); /* 10:00:00 */
    set_u32(0x200, H0);
    set_u32(0x204, H0);
    g_tier2_symbols = 1;
    fb_core_services_v1_t s = make_services();
    CHECK(p.init(&s) == FB_OK);
    frame();

    /* +1h / -1h, applied on the next frame */
    CHECK(press(0xDD) == 1);
    CHECK(mem_u32(0x100) == 10); /* nothing written before the frame */
    frame();
    CHECK(mem_u32(0x100) == 11);
    CHECK(press(0xDB) == 1);
    CHECK(press(0xDB) == 1);
    frame();
    CHECK(mem_u32(0x100) == 9);
    CHECK(press(0x41) == 0); /* unrelated key passes through */

    /* -1h across midnight */
    set_u32(0x100, 0);
    set_u32(0x104, 30);
    CHECK(press(0xDB) == 1);
    frame();
    CHECK(mem_u32(0x100) == 23 && mem_u32(0x104) == 30);

    /* pause holds the time against the game's own advance */
    set_u32(0x100, 12);
    set_u32(0x104, 0);
    set_u32(0x108, 0);
    CHECK(press(0xDC) == 1);
    frame();
    set_u32(0x104, 1); /* the game ticks a minute */
    frame();
    CHECK(mem_u32(0x100) == 12 && mem_u32(0x104) == 0);
    CHECK(press(0xDD) == 1); /* +1h while paused moves the held time */
    frame();
    set_u32(0x104, 5);
    frame();
    CHECK(mem_u32(0x100) == 13 && mem_u32(0x104) == 0);
    CHECK(press(0xDC) == 1); /* resume: no more holds */
    frame();
    set_u32(0x104, 7);
    frame();
    CHECK(mem_u32(0x104) == 7);

    /* weather: next (current H0 -> list index 1 = H1), instant + locked */
    CHECK(press(0xDE) == 1);
    frame();
    CHECK(mem_u32(0x200) == H1 && mem_u32(0x204) == H1 && mem_u32(0x208) == 0);
    set_u32(0x204, H2); /* the game's weather cycle moves on */
    frame();
    CHECK(mem_u32(0x204) == H1); /* lock re-asserts */
    CHECK(press(0xBA) == 1); /* prev from target 1 -> 0 */
    frame();
    CHECK(mem_u32(0x200) == H0 && mem_u32(0x204) == H0);

    for (int i = 0; i < 5; ++i) {
        frame(); /* let verify reads come due */
    }
    p.shutdown();
    CHECK(file_contains(log, ",set,clock.h,"));
    CHECK(file_contains(log, ",verify,clock.h,"));
    CHECK(file_contains(log, ",weather_set,weather.next,"));
    CHECK(file_contains(log, ",hold_lock,weather.next,"));
    CHECK(file_contains(log, ",hold_pause,clock.m,"));

    /* no symbols: stands by */
    memset(g_listeners, 0, sizeof(g_listeners));
    g_tier2_symbols = 0;
    s = make_services();
    CHECK(p.init(&s) == FB_ERR_NOT_FOUND);
    CHECK(strstr(g_last_log, "standing by") != NULL);
    g_tier2_symbols = 1;
    FreeLibrary(p.mod);
}

static void test_state_scan(const wchar_t* dll, const char* tmp) {
    printf("[state_scan]\n");
    plugin_t p;
    if (!load_plugin(dll, &p)) {
        CHECK(0);
        return;
    }
    fb_plugin_info_v1_t info;
    CHECK(p.query(&info) == FB_OK);
    CHECK((info.required_caps & FB_CAP_TITLE_MEM_RW) == 0); /* Tier 2 optional */

    char req[300], watch[300], tests[300], log[300], jtmp[600], jwatch[600], jtests[600], jlog[600];
    snprintf(req, sizeof(req), "%s\\snap.request", tmp);
    snprintf(watch, sizeof(watch), "%s\\ss_watch.txt", tmp);
    snprintf(tests, sizeof(tests), "%s\\ss_write_tests.txt", tmp);
    snprintf(log, sizeof(log), "%s\\ss_log.csv", tmp);
    DeleteFileA(req);
    DeleteFileA(log);
    FILE* f = fopen(watch, "w");
    fprintf(f, "# rva\n0x300\n");
    fclose(f);
    f = fopen(tests, "w");
    fprintf(f, "0x300 0x2A\n0x304 f0.5\n");
    fclose(f);
    json_path(jtmp, sizeof(jtmp), tmp);
    json_path(jwatch, sizeof(jwatch), watch);
    json_path(jtests, sizeof(jtests), tests);
    json_path(jlog, sizeof(jlog), log);
    snprintf(g_cfg, sizeof(g_cfg),
             "{\"snapshot_dir\":\"%s\",\"verify_frames\":2,\"watch_log\":\"%s\",\"watch_path\":\"%s\","
             "\"write_tests_path\":\"%s\"}",
             jtmp, jlog, jwatch, jtests);
    memset(g_mem, 0, sizeof(g_mem));
    memset(g_listeners, 0, sizeof(g_listeners));
    fb_core_services_v1_t s = make_services();
    CHECK(p.init(&s) == FB_OK);
    frame();
    CHECK(press(0x78) == 1); /* F9 */
    CHECK(file_contains(req, "run"));
    DeleteFileA(req);
    CHECK(press(0x79) == 1); /* F10 */
    CHECK(file_contains(req, "paused"));
    set_u32(0x300, 7);
    frame();
    CHECK(press(0x7A) == 1); /* F11 write test */
    CHECK(mem_u32(0x300) == 0x2A && mem_u32(0x304) == 0x3F000000u);
    frame();
    set_u32(0x300, 9); /* the game overwrites it */
    frame();
    frame();
    frame();
    p.shutdown();
    CHECK(file_contains(log, ",watch,0x00000300,0x00000007,changed"));
    CHECK(file_contains(log, ",write_test,0x00000300,0x0000002A,"));
    CHECK(file_contains(log, "+1 kept 0x0000002A"));
    CHECK(file_contains(log, "+N changed 0x00000009"));
    FreeLibrary(p.mod);
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* tw = NULL;
    const wchar_t* ss = NULL;
    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--time-weather") == 0) {
            tw = argv[++i];
        } else if (wcscmp(argv[i], L"--state-scan") == 0) {
            ss = argv[++i];
        }
    }
    char tmp[MAX_PATH];
    DWORD n = GetTempPathA(sizeof(tmp), tmp);
    if (n == 0 || n >= sizeof(tmp)) {
        return 2;
    }
    if (tmp[n - 1] == '\\') {
        tmp[n - 1] = '\0';
    }
    strncat(tmp, "\\fireball_plugin_examples", sizeof(tmp) - strlen(tmp) - 1);
    CreateDirectoryA(tmp, NULL);
    if (tw) {
        test_time_weather(tw, tmp);
    }
    if (ss) {
        test_state_scan(ss, tmp);
    }
    if (!tw && !ss) {
        printf("usage: plugin_examples_test --time-weather <dll> --state-scan <dll>\n");
        return 2;
    }
    printf(g_failures ? "FAILED (%d)\n" : "OK\n", g_failures);
    return g_failures ? 1 : 0;
}
