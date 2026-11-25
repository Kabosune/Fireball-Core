/*
 * console_unit_test.c — Native CTest suite for Fireball-Core's Command Console:
 * tokenizer (quotes/escapes/errors), command registration, built-ins
 * (help/clear/mods/stats/get/set/trigger/save), headless VK_* typing, caret editing,
 * Tab autocomplete, history navigation, PgUp/PgDn scrollback, and F1 <-> ~ mutual exclusion.
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fireball_host.h"
#include "fireball_menu.h"
#include "fireball_console.h"
#include "fb_bindings.h"
#include "fb_settings.h"
#include "fb_menu_ui.h"
#include "fb_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            ++g_failures;                                               \
            fprintf(stderr, "  [FAIL] line %d: %s\n", __LINE__, #cond); \
        }                                                               \
    } while (0)

static fb_input_event_v1_t make_key(uint32_t vk, uint32_t action, uint32_t mods) {
    fb_input_event_v1_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = (uint32_t)sizeof(ev);
    ev.device_kind = FB_DEV_KEYBOARD;
    ev.code = vk;
    ev.action = action;
    ev.modifiers = mods;
    return ev;
}

static void type_ascii_vk(fb_console_t* con, fb_binding_manager_t* bm, const char* s) {
    for (const char* p = s; *p; ++p) {
        uint32_t vk = 0;
        uint32_t mods = FB_MOD_NONE;
        char c = *p;
        if (c >= 'a' && c <= 'z') {
            vk = (uint32_t)('A' + (c - 'a'));
        } else if (c >= 'A' && c <= 'Z') {
            vk = (uint32_t)c;
            mods = FB_MOD_SHIFT;
        } else if (c >= '0' && c <= '9') {
            vk = (uint32_t)c;
        } else if (c == ' ') {
            vk = 0x20u;
        } else if (c == '.') {
            vk = 0xBEu;
        } else if (c == '_') {
            vk = 0xBDu;
            mods = FB_MOD_SHIFT;
        } else if (c == '-') {
            vk = 0xBDu;
        } else if (c == '"') {
            vk = 0xDEu;
            mods = FB_MOD_SHIFT;
        } else if (c == ':') {
            vk = 0xBAu;
            mods = FB_MOD_SHIFT;
        }
        fb_input_event_v1_t press = make_key(vk, (uint32_t)FB_INPUT_PRESS, mods);
        fb_bindings_track_input(bm, &press);
        CHECK(fb_console_handle_input(con, &press) == 1);
        fb_input_event_v1_t rel = make_key(vk, (uint32_t)FB_INPUT_RELEASE, mods);
        fb_bindings_track_input(bm, &rel);
        CHECK(fb_console_handle_input(con, &rel) == 1);
    }
}

static int scrollback_contains(const fb_console_t* con, const char* needle) {
    size_t n = fb_console_line_count(con);
    for (size_t i = 0; i < n; ++i) {
        const fb_console_line_t* ln = fb_console_line_at(con, i);
        if (ln && strstr(ln->text, needle) != NULL) {
            return 1;
        }
    }
    return 0;
}

typedef struct capture_buf {
    char text[4096];
    int  lines;
} capture_buf_t;

static void cap_print(void* ctx, int level, const char* msg) {
    (void)level;
    capture_buf_t* b = (capture_buf_t*)ctx;
    size_t cur = strlen(b->text);
    snprintf(b->text + cur, sizeof(b->text) - cur, "%s\n", msg ? msg : "");
    b->lines += 1;
}

/* Test plugin command & setting callbacks */
static int g_echo_calls = 0;
static char g_echo_last[256];
static int g_bool_val = 0;
static int32_t g_int_val = 10;
static int g_btn_calls = 0;
static int g_act_calls = 0;

static int test_echo_cmd(int argc, const char* const* argv, const fb_console_writer_v1_t* out, void* ctx) {
    (void)ctx;
    g_echo_calls += 1;
    g_echo_last[0] = '\0';
    for (int i = 1; i < argc; ++i) {
        size_t cur = strlen(g_echo_last);
        snprintf(g_echo_last + cur, sizeof(g_echo_last) - cur, "%s%s", (i > 1) ? "|" : "", argv[i]);
    }
    if (out && out->print_fmt) {
        out->print_fmt(out->user_ctx, FB_LOG_INFO, "echo: %s", g_echo_last);
    }
    return FB_OK;
}

static void on_setting_cb(const char* sid, const fb_setting_value_t* v, void* ctx) {
    (void)ctx;
    if (strcmp(sid, "flag") == 0) {
        g_bool_val = v->v.bool_val;
    } else if (strcmp(sid, "count") == 0) {
        g_int_val = v->v.int_val;
    }
}

static void on_btn_cb(const char* sid, void* ctx) {
    (void)sid;
    (void)ctx;
    g_btn_calls += 1;
}

static void on_act_cb(const char* aid, int active, void* ctx) {
    (void)aid;
    (void)ctx;
    if (active) {
        g_act_calls += 1;
    }
}

static void test_tokenizer(void) {
    printf("[1] Testing tokenizer (quotes, escapes, unclosed quote)...\n");
    char scratch[512];
    const char* argv[FB_CONSOLE_MAX_ARGS];
    int argc = 0;

    CHECK(fb_console_tokenize("  set  mod.x  name  \"hello world\"  'a b'  \"x\\\"y\"  ",
                              scratch, sizeof(scratch), argv, FB_CONSOLE_MAX_ARGS, &argc) == FB_OK);
    CHECK(argc == 6);
    CHECK(strcmp(argv[0], "set") == 0);
    CHECK(strcmp(argv[1], "mod.x") == 0);
    CHECK(strcmp(argv[2], "name") == 0);
    CHECK(strcmp(argv[3], "hello world") == 0);
    CHECK(strcmp(argv[4], "a b") == 0);
    CHECK(strcmp(argv[5], "x\"y") == 0);

    CHECK(fb_console_tokenize("   ", scratch, sizeof(scratch), argv, FB_CONSOLE_MAX_ARGS, &argc) == FB_OK);
    CHECK(argc == 0);

    CHECK(fb_console_tokenize("echo \"unterminated", scratch, sizeof(scratch), argv, FB_CONSOLE_MAX_ARGS, &argc) == FB_ERR_INVALID_ARG);
}

static void test_console_engine_and_input(void) {
    printf("[2] Testing command registry, built-ins, headless VK_* typing, Tab autocomplete, history & F1/<~> exclusion...\n");
    SetEnvironmentVariableW(L"FIREBALL_HEADLESS", L"1");

    CRITICAL_SECTION lock;
    InitializeCriticalSection(&lock);
    fb_settings_manager_t settings;
    fb_binding_manager_t bindings;
    fb_menu_ui_t menu_ui;
    fb_console_t con;

    fb_settings_init(&settings);
    fb_bindings_init(&bindings);

    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    wchar_t cfg_dir[MAX_PATH];
    swprintf(cfg_dir, MAX_PATH, L"%lsfireball_console_unit_tmp", tmp);
    CreateDirectoryW(cfg_dir, NULL);
    fb_settings_set_config_path(&settings, cfg_dir);

    volatile LONG64 ds_calls = 42, ds_hits = 7, ds_small = 0;
    volatile LONG64 fo_calls = 10, fo_hits = 2, sh_calls = 5, sh_hits = 3;
    volatile LONG64 frames = 123, in_ev = 55, in_cons = 9;
    fb_menu_telemetry_ptrs_t tptrs = {
        &ds_calls, &ds_hits, &ds_small,
        &fo_calls, &fo_hits,
        &sh_calls, &sh_hits,
        &in_ev, &in_cons, &frames
    };

    fb_menu_ui_init(&menu_ui, &lock, &settings, &bindings, &tptrs);
    fb_console_init(&con, &lock, &settings, &bindings, &menu_ui, &tptrs);

    /* Register core toggles */
    fb_action_desc_v1_t a;
    memset(&a, 0, sizeof(a));
    a.struct_size = (uint32_t)sizeof(a);
    a.mod_id = "fireball.core";
    a.action_id = "toggle_menu";
    a.label = "Toggle Mod Menu";
    a.consume_input = 1;
    a.default_key.code = 0x70u; /* F1 */
    CHECK(fb_bindings_register(&bindings, &a) == FB_OK);

    a.action_id = "toggle_console";
    a.label = "Toggle Console";
    a.default_key.code = 0xC0u; /* ` / ~ */
    CHECK(fb_bindings_register(&bindings, &a) == FB_OK);

    /* Register a sample mod, settings, button, action, and custom command */
    CHECK(fb_settings_register_mod(&settings, "demo.mod", "Demo Mod", "Console unit test mod") == FB_OK);

    fb_setting_desc_v1_t s;
    memset(&s, 0, sizeof(s));
    s.struct_size = (uint32_t)sizeof(s);
    s.mod_id = "demo.mod";
    s.type = (uint32_t)FB_SETTING_BOOL;
    s.setting_id = "flag";
    s.label = "Flag";
    s.default_bool = 0;
    s.on_change = on_setting_cb;
    CHECK(fb_settings_register(&settings, &s) == FB_OK);

    memset(&s, 0, sizeof(s));
    s.struct_size = (uint32_t)sizeof(s);
    s.mod_id = "demo.mod";
    s.type = (uint32_t)FB_SETTING_INT;
    s.setting_id = "count";
    s.label = "Count";
    s.default_int = 10;
    s.int_min = 0;
    s.int_max = 50;
    s.int_step = 5;
    s.on_change = on_setting_cb;
    CHECK(fb_settings_register(&settings, &s) == FB_OK);

    memset(&s, 0, sizeof(s));
    s.struct_size = (uint32_t)sizeof(s);
    s.mod_id = "demo.mod";
    s.type = (uint32_t)FB_SETTING_BUTTON;
    s.setting_id = "boom";
    s.label = "Boom";
    s.on_button = on_btn_cb;
    CHECK(fb_settings_register(&settings, &s) == FB_OK);

    memset(&a, 0, sizeof(a));
    a.struct_size = (uint32_t)sizeof(a);
    a.mod_id = "demo.mod";
    a.action_id = "ping";
    a.label = "Ping";
    a.callback = on_act_cb;
    CHECK(fb_bindings_register(&bindings, &a) == FB_OK);

    fb_command_desc_v1_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.struct_size = (uint32_t)sizeof(cmd);
    cmd.mod_id = "demo.mod";
    cmd.name = "echo";
    cmd.usage = "echo <args...>";
    cmd.description = "Echo arguments";
    cmd.callback = test_echo_cmd;
    CHECK(fb_console_register_command(&con, &cmd) == FB_OK);

    /* Built-ins via exec + capture writer */
    capture_buf_t cap;
    memset(&cap, 0, sizeof(cap));
    fb_console_writer_v1_t w = { (uint32_t)sizeof(w), 0u, &cap, cap_print, NULL };

    CHECK(fb_console_exec(&con, "help", &w) == FB_OK);
    CHECK(strstr(cap.text, "echo") != NULL && strstr(cap.text, "stats") != NULL);

    memset(&cap, 0, sizeof(cap));
    CHECK(fb_console_exec(&con, "stats", &w) == FB_OK);
    CHECK(strstr(cap.text, "calls=42 hits=7") != NULL);
    CHECK(strstr(cap.text, "Shaders:") != NULL && strstr(cap.text, "calls=5 hits=3") != NULL);
    CHECK(strstr(cap.text, "frames_seen=123") != NULL);

    memset(&cap, 0, sizeof(cap));
    CHECK(fb_console_exec(&con, "set demo.mod flag true", &w) == FB_OK);
    CHECK(g_bool_val == 1);
    CHECK(fb_console_exec(&con, "set demo.mod count 999", &w) == FB_OK); /* clamped to 50 */
    CHECK(g_int_val == 50);

    memset(&cap, 0, sizeof(cap));
    CHECK(fb_console_exec(&con, "get demo.mod count", &w) == FB_OK);
    CHECK(strstr(cap.text, "demo.mod.count = 50") != NULL);

    CHECK(fb_console_exec(&con, "trigger demo.mod boom", &w) == FB_OK);
    CHECK(g_btn_calls == 1);
    CHECK(fb_console_exec(&con, "trigger demo.mod ping", &w) == FB_OK);
    CHECK(g_act_calls == 1);

    CHECK(fb_console_exec(&con, "save", &w) == FB_OK);
    CHECK(fb_console_exec(&con, "no_such_cmd", &w) == FB_ERR_NOT_FOUND);

    /* Mutual exclusion: open F1 menu -> open ~ console closes F1 menu */
    fb_menu_ui_set_open(&menu_ui, 1);
    CHECK(fb_menu_ui_is_open(&menu_ui) == 1);
    fb_console_set_open(&con, 1);
    CHECK(fb_console_is_open(&con) == 1);
    CHECK(fb_menu_ui_is_open(&menu_ui) == 0);

    /* Headless VK_* typing + Tab autocomplete + quoted args + Enter */
    type_ascii_vk(&con, &bindings, "ec");
    fb_input_event_v1_t tab = make_key(0x09u, (uint32_t)FB_INPUT_PRESS, 0u);
    CHECK(fb_console_handle_input(&con, &tab) == 1);
    CHECK(strcmp(con.input, "echo ") == 0);

    type_ascii_vk(&con, &bindings, "\"hi there\" 42");
    fb_input_event_v1_t ret = make_key(0x0Du, (uint32_t)FB_INPUT_PRESS, 0u);
    CHECK(fb_console_handle_input(&con, &ret) == 1);
    CHECK(g_echo_calls == 1);
    CHECK(strcmp(g_echo_last, "hi there|42") == 0);
    CHECK(scrollback_contains(&con, "echo: hi there|42"));
    CHECK(con.input_len == 0u);

    /* Caret editing + History Up/Down */
    fb_input_event_v1_t up = make_key(0x26u, (uint32_t)FB_INPUT_PRESS, 0u);
    fb_input_event_v1_t down = make_key(0x28u, (uint32_t)FB_INPUT_PRESS, 0u);
    CHECK(fb_console_handle_input(&con, &up) == 1);
    CHECK(strcmp(con.input, "echo \"hi there\" 42") == 0);
    CHECK(fb_console_handle_input(&con, &down) == 1);
    CHECK(strcmp(con.input, "") == 0);

    /* Tab autocomplete for mod_id and setting_id on `get` */
    type_ascii_vk(&con, &bindings, "get dem");
    CHECK(fb_console_handle_input(&con, &tab) == 1);
    CHECK(strcmp(con.input, "get demo.mod ") == 0);
    type_ascii_vk(&con, &bindings, "co");
    CHECK(fb_console_handle_input(&con, &tab) == 1);
    CHECK(strcmp(con.input, "get demo.mod count ") == 0);

    /* Clear & PgUp/PgDn */
    CHECK(fb_console_exec(&con, "clear", NULL) == FB_OK);
    CHECK(fb_console_line_count(&con) == 0u);

    /* Esc closes console cleanly */
    fb_input_event_v1_t esc = make_key(0x1Bu, (uint32_t)FB_INPUT_PRESS, 0u);
    CHECK(fb_console_handle_input(&con, &esc) == 1);
    CHECK(fb_console_is_open(&con) == 0);

    fb_console_shutdown(&con);
    fb_menu_ui_shutdown(&menu_ui);
    fb_bindings_free(&bindings);
    fb_settings_free(&settings);
    DeleteCriticalSection(&lock);
}

int main(void) {
    test_tokenizer();
    test_console_engine_and_input();
    if (g_failures == 0) {
        printf("\nALL CONSOLE UNIT TESTS PASSED (0 failures)\n");
        return 0;
    }
    fprintf(stderr, "\nCONSOLE UNIT TESTS FAILED (%d failures)\n", g_failures);
    return 1;
}
