/*
 * menu_system_test.c — Native CTest suite for Fireball-Core's Mod Menu,
 * Typed Settings Registry, Keyboard + Gamepad Chord Binding Manager, Atomic JSON
 * Persistence, and End-to-End Plugin Integration (including v1 backward compat).
 */

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "fireball_host.h"
#include "fireball_menu.h"
#include "fb_bindings.h"
#include "fb_settings.h"
#include "fb_menu_ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            fprintf(stderr, "  [FAIL] line %d: %s\n", __LINE__, #cond);          \
        }                                                                        \
    } while (0)

static fb_input_event_v1_t make_ev(uint32_t dev, uint32_t code, uint32_t action, uint32_t mods, uint64_t ts_us) {
    fb_input_event_v1_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = (uint32_t)sizeof(ev);
    ev.device_kind = dev;
    ev.code = code;
    ev.action = action;
    ev.modifiers = mods;
    ev.timestamp_us = ts_us;
    return ev;
}

/* ---------------------------------------------------------------------------
 * 1. Action Binding Manager tests (keyboard mods, gamepad chords, hold, capture, conflicts)
 * --------------------------------------------------------------------------- */

typedef struct act_log {
    int press_count;
    int release_count;
    int last_active;
    char last_id[64];
} act_log_t;

static void on_test_action(const char* action_id, int active, void* ctx) {
    act_log_t* log = (act_log_t*)ctx;
    if (active) {
        log->press_count += 1;
    } else {
        log->release_count += 1;
    }
    log->last_active = active;
    snprintf(log->last_id, sizeof(log->last_id), "%s", action_id ? action_id : "");
}

static void test_bindings_unit(void) {
    printf("[1] Testing action bindings, keyboard modifiers, gamepad chords, hold, capture & conflicts...\n");
    fb_binding_manager_t bm;
    fb_bindings_init(&bm);

    act_log_t log_plain = { 0 };
    act_log_t log_mod   = { 0 };
    act_log_t log_chord = { 0 };
    act_log_t log_hold  = { 0 };

    fb_action_desc_v1_t d;
    memset(&d, 0, sizeof(d));
    d.struct_size = (uint32_t)sizeof(d);
    d.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
    d.mod_id = "mod.test";
    d.consume_input = 1;
    d.callback = on_test_action;

    /* Action 0: plain 'K' and single DPad Right */
    d.action_id = "plain_act";
    d.label = "Plain Action";
    d.default_key.code = 'K';
    d.default_key.modifiers = FB_MOD_NONE;
    d.default_pad.code = FB_PAD_DPAD_RIGHT;
    d.default_pad.modifiers = FB_PAD_NONE;
    d.user_ctx = &log_plain;
    CHECK(fb_bindings_register(&bm, &d) == FB_OK);

    /* Action 1: Ctrl+Shift+K, pass-through (consume=0) */
    d.action_id = "mod_act";
    d.label = "Modified Action";
    d.consume_input = 0;
    d.default_key.code = 'K';
    d.default_key.modifiers = FB_MOD_CTRL | FB_MOD_SHIFT;
    d.default_pad.code = FB_PAD_NONE;
    d.default_pad.modifiers = FB_PAD_NONE;
    d.user_ctx = &log_mod;
    CHECK(fb_bindings_register(&bm, &d) == FB_OK);

    /* Action 2: Gamepad chord LB + DPad Right (must take priority over plain DPad Right!) */
    d.action_id = "chord_act";
    d.label = "Chord Action";
    d.consume_input = 1;
    d.default_key.code = 0;
    d.default_key.modifiers = 0;
    d.default_pad.code = FB_PAD_DPAD_RIGHT;
    d.default_pad.modifiers = FB_PAD_LEFT_SHOULDER;
    d.user_ctx = &log_chord;
    CHECK(fb_bindings_register(&bm, &d) == FB_OK);

    /* Action 3: HOLD action on keyboard 'H' and gamepad LB + Y */
    d.trigger_mode = (uint32_t)FB_TRIGGER_HOLD;
    d.action_id = "hold_act";
    d.label = "Hold Action";
    d.consume_input = 1;
    d.default_key.code = 'H';
    d.default_key.modifiers = FB_MOD_NONE;
    d.default_pad.code = FB_PAD_Y;
    d.default_pad.modifiers = FB_PAD_LEFT_SHOULDER;
    d.user_ctx = &log_hold;
    CHECK(fb_bindings_register(&bm, &d) == FB_OK);

    /* Plain 'K' fires plain_act (consumed=1), NOT mod_act */
    fb_input_event_v1_t ev = make_ev(FB_DEV_KEYBOARD, 'K', FB_INPUT_PRESS, FB_MOD_NONE, 100);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_dispatch_input(&bm, &ev, 0) == 1);
    CHECK(log_plain.press_count == 1 && log_mod.press_count == 0);
    ev = make_ev(FB_DEV_KEYBOARD, 'K', FB_INPUT_RELEASE, FB_MOD_NONE, 110);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_dispatch_input(&bm, &ev, 0) == 0);

    /* Ctrl+Shift+K fires mod_act (consume=0 -> returns 0), NOT plain_act */
    ev = make_ev(FB_DEV_KEYBOARD, 'K', FB_INPUT_PRESS, FB_MOD_CTRL | FB_MOD_SHIFT, 200);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_dispatch_input(&bm, &ev, 0) == 0);
    CHECK(log_plain.press_count == 1 && log_mod.press_count == 1);
    ev = make_ev(FB_DEV_KEYBOARD, 'K', FB_INPUT_RELEASE, FB_MOD_NONE, 210);
    fb_bindings_track_input(&bm, &ev);

    /* Gamepad chord: hold LB, press DPad Right -> fires chord_act ONLY (priority over single DPad Right!) */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_PRESS, 0, 300);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_dispatch_input(&bm, &ev, 0) == 0);

    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_RIGHT, FB_INPUT_PRESS, 0, 310);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_dispatch_input(&bm, &ev, 0) == 1);
    CHECK(log_chord.press_count == 1 && log_plain.press_count == 1);

    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_RIGHT, FB_INPUT_RELEASE, 0, 320);
    fb_bindings_track_input(&bm, &ev);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_RELEASE, 0, 330);
    fb_bindings_track_input(&bm, &ev);

    /* Solo DPad Right now fires plain_act */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_RIGHT, FB_INPUT_PRESS, 0, 340);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_dispatch_input(&bm, &ev, 0) == 1);
    CHECK(log_plain.press_count == 2 && log_chord.press_count == 1);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_RIGHT, FB_INPUT_RELEASE, 0, 350);
    fb_bindings_track_input(&bm, &ev);

    /* HOLD action: press 'H' -> active=1, release 'H' -> active=0 */
    ev = make_ev(FB_DEV_KEYBOARD, 'H', FB_INPUT_PRESS, 0, 400);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_dispatch_input(&bm, &ev, 0) == 1);
    CHECK(log_hold.press_count == 1 && log_hold.release_count == 0 && log_hold.last_active == 1);
    ev = make_ev(FB_DEV_KEYBOARD, 'H', FB_INPUT_RELEASE, 0, 450);
    fb_bindings_track_input(&bm, &ev);
    fb_bindings_dispatch_input(&bm, &ev, 0);
    CHECK(log_hold.release_count == 1 && log_hold.last_active == 0);

    /* Interactive Capture: keyboard Ctrl+G */
    CHECK(fb_bindings_start_capture(&bm, 0, FB_DEV_KEYBOARD) == FB_OK);
    ev = make_ev(FB_DEV_KEYBOARD, 0x11u /* VK_CONTROL */, FB_INPUT_PRESS, FB_MOD_CTRL, 500);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_capture_input(&bm, &ev) == FB_CAPTURE_WAITING);
    ev = make_ev(FB_DEV_KEYBOARD, 'G', FB_INPUT_PRESS, FB_MOD_CTRL, 510);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_capture_input(&bm, &ev) == FB_CAPTURE_BOUND);
    CHECK(bm.actions[0].key.code == 'G' && bm.actions[0].key.modifiers == FB_MOD_CTRL);
    ev = make_ev(FB_DEV_KEYBOARD, 'G', FB_INPUT_RELEASE, FB_MOD_CTRL, 515);
    fb_bindings_track_input(&bm, &ev);
    ev = make_ev(FB_DEV_KEYBOARD, 0x11u, FB_INPUT_RELEASE, 0, 520);
    fb_bindings_track_input(&bm, &ev);

    /* Interactive Capture: Esc cancels without changing bind */
    CHECK(fb_bindings_start_capture(&bm, 0, FB_DEV_KEYBOARD) == FB_OK);
    ev = make_ev(FB_DEV_KEYBOARD, 0x1Bu /* VK_ESCAPE */, FB_INPUT_PRESS, 0, 530);
    CHECK(fb_bindings_capture_input(&bm, &ev) == FB_CAPTURE_CANCELLED);
    CHECK(bm.actions[0].key.code == 'G');

    /* Interactive Capture: Backspace unbinds keyboard slot */
    CHECK(fb_bindings_start_capture(&bm, 0, FB_DEV_KEYBOARD) == FB_OK);
    ev = make_ev(FB_DEV_KEYBOARD, 0x08u /* VK_BACK */, FB_INPUT_PRESS, 0, 540);
    CHECK(fb_bindings_capture_input(&bm, &ev) == FB_CAPTURE_UNBOUND);
    CHECK(bm.actions[0].key.code == 0u && bm.actions[0].key.modifiers == 0u);

    /* Interactive Capture: Gamepad chord (RB + Y) */
    CHECK(fb_bindings_start_capture(&bm, 0, FB_DEV_GAMEPAD) == FB_OK);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_RIGHT_SHOULDER, FB_INPUT_PRESS, 0, 600);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_capture_input(&bm, &ev) == FB_CAPTURE_WAITING);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_Y, FB_INPUT_PRESS, 0, 610);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_capture_input(&bm, &ev) == FB_CAPTURE_BOUND);
    CHECK(bm.actions[0].pad.code == FB_PAD_Y && bm.actions[0].pad.modifiers == FB_PAD_RIGHT_SHOULDER);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_Y, FB_INPUT_RELEASE, 0, 620);
    fb_bindings_track_input(&bm, &ev);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_RIGHT_SHOULDER, FB_INPUT_RELEASE, 0, 630);
    fb_bindings_track_input(&bm, &ev);

    /* Interactive Capture: Hold B >= 0.5s cancels capture */
    CHECK(fb_bindings_start_capture(&bm, 0, FB_DEV_GAMEPAD) == FB_OK);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_B, FB_INPUT_PRESS, 0, 700000);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_capture_input(&bm, &ev) == FB_CAPTURE_WAITING);
    CHECK(fb_bindings_capture_tick(&bm, 0.30) == FB_CAPTURE_WAITING);
    CHECK(fb_bindings_capture_tick(&bm, 0.25) == FB_CAPTURE_CANCELLED);
    CHECK(bm.actions[0].pad.code == FB_PAD_Y);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_B, FB_INPUT_RELEASE, 0, 1300000);
    fb_bindings_track_input(&bm, &ev);

    /* Interactive Capture: Solo X unbinds gamepad slot */
    CHECK(fb_bindings_start_capture(&bm, 0, FB_DEV_GAMEPAD) == FB_OK);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_X, FB_INPUT_PRESS, 0, 1400000);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_bindings_capture_input(&bm, &ev) == FB_CAPTURE_UNBOUND);
    CHECK(bm.actions[0].pad.code == 0u);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_X, FB_INPUT_RELEASE, 0, 1410000);
    fb_bindings_track_input(&bm, &ev);

    /* Conflict detection */
    fb_binding_chord_t dup = { FB_PAD_DPAD_RIGHT, FB_PAD_LEFT_SHOULDER };
    CHECK(fb_bindings_set_chord(&bm, 0, FB_DEV_GAMEPAD, dup) == FB_OK);
    size_t conf[4] = { 0 };
    CHECK(fb_bindings_find_conflicts(&bm, 0, FB_DEV_GAMEPAD, dup, conf, 4) == 1u);
    CHECK(conf[0] == 2u);

    /* Formatting check */
    char fmt[64];
    fb_format_binding(FB_DEV_GAMEPAD, dup, fmt, sizeof(fmt));
    CHECK(strcmp(fmt, "LB+DPad Right") == 0);
    fb_binding_chord_t kfmt = { 'V', FB_MOD_CTRL | FB_MOD_SHIFT };
    fb_format_binding(FB_DEV_KEYBOARD, kfmt, fmt, sizeof(fmt));
    CHECK(strcmp(fmt, "Ctrl+Shift+V") == 0);

    fb_bindings_free(&bm);
}

/* ---------------------------------------------------------------------------
 * 2. Settings + Atomic JSON Persistence Round-Trip & Menu Navigation tests
 * --------------------------------------------------------------------------- */

typedef struct set_cb_state {
    int     bool_v;
    int32_t int_v;
    double  float_v;
    int32_t enum_v;
    int     btn_clicks;
} set_cb_state_t;

static void on_set_change(const char* sid, const fb_setting_value_t* val, void* ctx) {
    set_cb_state_t* st = (set_cb_state_t*)ctx;
    if (strcmp(sid, "flag") == 0) {
        st->bool_v = val->v.bool_val;
    } else if (strcmp(sid, "count") == 0) {
        st->int_v = val->v.int_val;
    } else if (strcmp(sid, "scale") == 0) {
        st->float_v = val->v.float_val;
    } else if (strcmp(sid, "mode") == 0) {
        st->enum_v = val->v.enum_val;
    }
}

static void on_set_btn(const char* sid, void* ctx) {
    (void)sid;
    ((set_cb_state_t*)ctx)->btn_clicks += 1;
}

static void on_set_status(const char* sid, char* buf, size_t cap, void* ctx) {
    (void)sid;
    set_cb_state_t* st = (set_cb_state_t*)ctx;
    snprintf(buf, cap, "clicks=%d", st->btn_clicks);
}

static void test_settings_persistence_and_menu_nav(const wchar_t* tmp_dir) {
    printf("[2] Testing typed settings, atomic JSON save/reload, and headless menu navigation...\n");
    fb_settings_manager_t sm;
    fb_binding_manager_t bm;
    fb_settings_init(&sm);
    fb_bindings_init(&bm);
    fb_settings_set_config_path(&sm, tmp_dir);

    DeleteFileW(sm.config_path);

    set_cb_state_t st = { 1, 10, 1.5, 0, 0 };
    static const char* const k_modes[] = { "Normal", "Fast", "Ultra" };

    fb_settings_register_mod(&sm, "mod.alpha", "Alpha Mod", "Test mod");

    fb_setting_desc_v1_t sd;
    memset(&sd, 0, sizeof(sd));
    sd.struct_size = (uint32_t)sizeof(sd);
    sd.mod_id = "mod.alpha";
    sd.user_ctx = &st;

    sd.type = (uint32_t)FB_SETTING_STATUS;
    sd.setting_id = "stat";
    sd.label = "Live";
    sd.on_status = on_set_status;
    CHECK(fb_settings_register(&sm, &sd) == FB_OK);

    sd.section = "General";
    sd.type = (uint32_t)FB_SETTING_BOOL;
    sd.setting_id = "flag";
    sd.label = "Enable Flag";
    sd.default_bool = 1;
    sd.on_change = on_set_change;
    CHECK(fb_settings_register(&sm, &sd) == FB_OK);

    sd.type = (uint32_t)FB_SETTING_INT;
    sd.setting_id = "count";
    sd.label = "Count";
    sd.default_int = 10;
    sd.int_min = 0;
    sd.int_max = 100;
    sd.int_step = 5;
    CHECK(fb_settings_register(&sm, &sd) == FB_OK);

    sd.type = (uint32_t)FB_SETTING_FLOAT;
    sd.setting_id = "scale";
    sd.label = "Scale";
    sd.default_float = 1.5;
    sd.float_min = 0.5;
    sd.float_max = 4.0;
    sd.float_step = 0.25;
    sd.float_fmt = "%.2fx";
    CHECK(fb_settings_register(&sm, &sd) == FB_OK);

    sd.type = (uint32_t)FB_SETTING_ENUM;
    sd.setting_id = "mode";
    sd.label = "Mode";
    sd.default_enum = 0;
    sd.enum_labels = k_modes;
    sd.enum_count = 3;
    CHECK(fb_settings_register(&sm, &sd) == FB_OK);

    sd.type = (uint32_t)FB_SETTING_BUTTON;
    sd.setting_id = "go";
    sd.label = "Trigger";
    sd.on_button = on_set_btn;
    CHECK(fb_settings_register(&sm, &sd) == FB_OK);

    /* Core toggle action + mod action */
    fb_action_desc_v1_t ad;
    memset(&ad, 0, sizeof(ad));
    ad.struct_size = (uint32_t)sizeof(ad);
    ad.mod_id = "fireball.core";
    ad.action_id = "toggle_menu";
    ad.label = "Toggle Mod Menu";
    ad.consume_input = 1;
    ad.default_key.code = 0x70u; /* F1 */
    ad.default_pad.code = FB_PAD_BACK;
    ad.default_pad.modifiers = FB_PAD_LEFT_SHOULDER;
    CHECK(fb_bindings_register(&bm, &ad) == FB_OK);

    ad.mod_id = "mod.alpha";
    ad.action_id = "ping";
    ad.label = "Ping";
    ad.default_key.code = 'P';
    ad.default_key.modifiers = 0;
    ad.default_pad.code = FB_PAD_A;
    ad.default_pad.modifiers = FB_PAD_LEFT_SHOULDER;
    CHECK(fb_bindings_register(&bm, &ad) == FB_OK);

    /* Headless Menu UI navigation test */
    CRITICAL_SECTION cs;
    InitializeCriticalSection(&cs);
    fb_menu_ui_t ui;
    fb_menu_ui_init(&ui, &cs, &sm, &bm, NULL);
    ui.headless = 1;
    fb_menu_ui_rebuild(&ui);

    fb_menu_ui_set_open(&ui, 1);
    CHECK(fb_menu_ui_is_open(&ui) == 1);
    CHECK(ui.tab_count == 2u && ui.active_tab == 0u);

    /* Switch to tab 1 (Alpha Mod) via gamepad RB */
    fb_input_event_v1_t ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_RIGHT_SHOULDER, FB_INPUT_PRESS, 0, 1);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(ui.active_tab == 1u);

    /* First selectable row on Alpha Mod is 'flag' (BOOL). Press A to toggle 1 -> 0 */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_A, FB_INPUT_PRESS, 0, 2);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(st.bool_v == 0);

    /* DPad Down -> 'count' (INT). DPad Right steps 10 -> 15 -> 20 */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, FB_INPUT_PRESS, 0, 3);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_RIGHT, FB_INPUT_PRESS, 0, 4);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(st.int_v == 20);

    /* DPad Down -> 'scale' (FLOAT). DPad Right steps 1.50 -> 1.75 */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, FB_INPUT_PRESS, 0, 5);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_RIGHT, FB_INPUT_PRESS, 0, 6);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(fabs(st.float_v - 1.75) < 1e-6);

    /* DPad Down -> 'mode' (ENUM). DPad Right steps 0 -> 1 ("Fast") */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, FB_INPUT_PRESS, 0, 7);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_RIGHT, FB_INPUT_PRESS, 0, 8);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(st.enum_v == 1);

    /* DPad Down -> 'go' (BUTTON). Press A -> triggers callback */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, FB_INPUT_PRESS, 0, 9);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_A, FB_INPUT_PRESS, 0, 10);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(st.btn_clicks == 1);

    /* DPad Down -> 'ping' action. Rebind Key slot to 'J' via Enter -> 'J' */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, FB_INPUT_PRESS, 0, 11);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    ev = make_ev(FB_DEV_KEYBOARD, 0x0Du, FB_INPUT_PRESS, 0, 12);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(bm.capture_active == 1);
    ev = make_ev(FB_DEV_KEYBOARD, 'J', FB_INPUT_PRESS, FB_MOD_ALT, 13);
    fb_bindings_track_input(&bm, &ev);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(bm.capture_active == 0);
    CHECK(bm.actions[1].key.code == 'J' && bm.actions[1].key.modifiers == FB_MOD_ALT);

    /* Close menu via B */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_B, FB_INPUT_PRESS, 0, 14);
    CHECK(fb_menu_ui_handle_input(&ui, &ev) == 1);
    CHECK(fb_menu_ui_is_open(&ui) == 0);

    /* Verify atomic config reload into fresh managers restores every setting & binding */
    fb_settings_manager_t sm2;
    fb_binding_manager_t bm2;
    fb_settings_init(&sm2);
    fb_bindings_init(&bm2);
    fb_settings_set_config_path(&sm2, tmp_dir);
    CHECK(fb_user_config_load(&sm2, &bm2, NULL) == FB_OK);

    set_cb_state_t st2 = { 1, 10, 1.5, 0, 0 };
    sd.user_ctx = &st2;
    sd.type = (uint32_t)FB_SETTING_BOOL;  sd.setting_id = "flag";  sd.default_bool = 1;     CHECK(fb_settings_register(&sm2, &sd) == FB_OK);
    sd.type = (uint32_t)FB_SETTING_INT;   sd.setting_id = "count"; sd.default_int = 10;     CHECK(fb_settings_register(&sm2, &sd) == FB_OK);
    sd.type = (uint32_t)FB_SETTING_FLOAT; sd.setting_id = "scale"; sd.default_float = 1.5;  CHECK(fb_settings_register(&sm2, &sd) == FB_OK);
    sd.type = (uint32_t)FB_SETTING_ENUM;  sd.setting_id = "mode";  sd.default_enum = 0;     CHECK(fb_settings_register(&sm2, &sd) == FB_OK);
    CHECK(fb_bindings_register(&bm2, &ad) == FB_OK);

    CHECK(st2.bool_v == 0);
    CHECK(st2.int_v == 20);
    CHECK(fabs(st2.float_v - 1.75) < 1e-6);
    CHECK(st2.enum_v == 1);
    CHECK(bm2.actions[0].key.code == 'J' && bm2.actions[0].key.modifiers == FB_MOD_ALT);

    /* Reset mod restores defaults */
    fb_settings_reset_mod(&sm2, "mod.alpha", 1);
    fb_bindings_reset_mod(&bm2, "mod.alpha");
    CHECK(st2.bool_v == 1 && st2.int_v == 10 && fabs(st2.float_v - 1.5) < 1e-6 && st2.enum_v == 0);
    CHECK(bm2.actions[0].key.code == 'P' && bm2.actions[0].key.modifiers == 0u);

    fb_menu_ui_shutdown(&ui);
    DeleteCriticalSection(&cs);
    fb_settings_free(&sm);
    fb_bindings_free(&bm);
    fb_settings_free(&sm2);
    fb_bindings_free(&bm2);
}

/* ---------------------------------------------------------------------------
 * 3. End-to-End fireball_core.dll Integration (all 3 migrated plugins + v1_compat)
 * --------------------------------------------------------------------------- */

typedef int (*fb_attach_host_fn)(const fb_host_api_v1_t*, fb_modcore_callbacks_v1_t*);

static uint8_t g_title_mem[4096];
static int     g_v1_f6_seen = 0;

static void e2e_log(int level, const char* sub, const char* msg) {
    (void)level;
    if (sub && strcmp(sub, "test.v1_compat") == 0 && msg && strstr(msg, "v1_compat handled F6")) {
        g_v1_f6_seen += 1;
    }
    printf("    [core][%s] %s\n", sub ? sub : "?", msg ? msg : "");
}

static void* e2e_resolve(const char* sym) {
    if (!sym) {
        return NULL;
    }
    if (strcmp(sym, "rage::camGameplayDirector::s_Instance") == 0) {
        return g_title_mem + 0x000;
    }
    if (strcmp(sym, "fb.clock") == 0) {
        return g_title_mem + 0x100;
    }
    if (strcmp(sym, "fb.weather") == 0) {
        return g_title_mem + 0x200;
    }
    if (strcmp(sym, "fb.image") == 0) {
        return g_title_mem;
    }
    return NULL;
}

static int e2e_read_u32(uint64_t va, uint32_t* out) {
    uint64_t base = (uint64_t)(uintptr_t)g_title_mem;
    if (va < base || va + 4ull > base + sizeof(g_title_mem) || (va & 3ull) != 0ull) {
        return FB_ERR_MEM_FAULT;
    }
    memcpy(out, (const void*)(uintptr_t)va, 4);
    return FB_OK;
}

static int e2e_write_u32(uint64_t va, uint32_t val) {
    uint64_t base = (uint64_t)(uintptr_t)g_title_mem;
    if (va < base || va + 4ull > base + sizeof(g_title_mem) || (va & 3ull) != 0ull) {
        return FB_ERR_MEM_FAULT;
    }
    memcpy((void*)(uintptr_t)va, &val, 4);
    return FB_OK;
}

static uint32_t e2e_peek(uint32_t off) {
    uint32_t v = 0;
    memcpy(&v, g_title_mem + off, 4);
    return v;
}

static void e2e_poke(uint32_t off, uint32_t v) {
    memcpy(g_title_mem + off, &v, 4);
}

static int copy_plugin_into(const wchar_t* src, const wchar_t* dir, const wchar_t* name) {
    wchar_t dst[MAX_PATH * 2];
    swprintf(dst, sizeof(dst) / sizeof(dst[0]), L"%ls\\%ls", dir, name);
    return CopyFileW(src, dst, FALSE) ? 1 : 0;
}

static void test_end_to_end_core(
    const wchar_t* core_dll,
    const wchar_t* fpv_dll,
    const wchar_t* tw_dll,
    const wchar_t* ss_dll,
    const wchar_t* v1_dll,
    const wchar_t* tmp_dir)
{
    printf("[3] Testing end-to-end fireball_core.dll with fpv_toggle + time_weather + state_scan + v1_compat...\n");
    SetEnvironmentVariableW(L"FIREBALL_HEADLESS", L"1");

    wchar_t mods_dir[MAX_PATH * 2];
    swprintf(mods_dir, sizeof(mods_dir) / sizeof(mods_dir[0]), L"%ls\\e2e_mods", tmp_dir);
    CreateDirectoryW(mods_dir, NULL);

    wchar_t cfg_file[MAX_PATH * 2];
    swprintf(cfg_file, sizeof(cfg_file) / sizeof(cfg_file[0]), L"%ls\\fireball_user_config.json", mods_dir);
    DeleteFileW(cfg_file);

    CHECK(copy_plugin_into(fpv_dll, mods_dir, L"fpv_toggle.fbplugin.dll"));
    CHECK(copy_plugin_into(tw_dll, mods_dir, L"time_weather.fbplugin.dll"));
    CHECK(copy_plugin_into(ss_dll, mods_dir, L"state_scan.fbplugin.dll"));
    CHECK(copy_plugin_into(v1_dll, mods_dir, L"v1_compat.fbplugin.dll"));

    HMODULE core = LoadLibraryW(core_dll);
    CHECK(core != NULL);
    if (!core) {
        return;
    }
    fb_attach_host_fn attach_fn = (fb_attach_host_fn)(void*)GetProcAddress(core, "fb_attach_host");
    CHECK(attach_fn != NULL);
    if (!attach_fn) {
        FreeLibrary(core);
        return;
    }

    memset(g_title_mem, 0, sizeof(g_title_mem));
    e2e_poke(0x000, 0u);  /* ThirdPerson */
    e2e_poke(0x100, 12u); /* 12:00:00 */
    g_v1_f6_seen = 0;

    fb_host_api_v1_t api;
    memset(&api, 0, sizeof(api));
    api.struct_size = (uint32_t)sizeof(api);
    api.abi_version = FB_HOST_ABI_VERSION;
    api.host_name = "VIctory-E2E-MenuTest";
    api.host_version = "0.1.0";
    api.game_root_dir = L"D:\\Games\\GTA6";
    api.mods_root_dir = mods_dir;
    api.capabilities = FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS | FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW;
    api.log = e2e_log;
    api.resolve_symbol = e2e_resolve;
    api.title_mem_read_u32 = e2e_read_u32;
    api.title_mem_write_u32 = e2e_write_u32;

    fb_modcore_callbacks_v1_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.struct_size = (uint32_t)sizeof(cb);
    CHECK(attach_fn(&api, &cb) == FB_OK);

    /* 1. FPV Toggle via gamepad chord LB + R3! */
    fb_input_event_v1_t ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_PRESS, 0, 1000);
    CHECK(cb.on_input_event(&ev) == 0);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_RIGHT_THUMB, FB_INPUT_PRESS, 0, 1010);
    CHECK(cb.on_input_event(&ev) == 1);
    CHECK(e2e_peek(0x000) == 2u); /* Flipped to FirstPerson via gamepad chord */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_RIGHT_THUMB, FB_INPUT_RELEASE, 0, 1020);
    cb.on_input_event(&ev);

    /* 2. Time & Weather +1h via gamepad chord LB + DPad Up (LB still held!) */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_UP, FB_INPUT_PRESS, 0, 1030);
    CHECK(cb.on_input_event(&ev) == 1);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_UP, FB_INPUT_RELEASE, 0, 1040);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_RELEASE, 0, 1050);
    cb.on_input_event(&ev);
    cb.on_frame_begin(1u, 1.0 / 60.0);
    CHECK(e2e_peek(0x100) == 13u); /* Clock advanced 12 -> 13! */

    /* 3. Legacy v1 plugin (no fb_plugin_register_menu) receives & consumes F6 */
    ev = make_ev(FB_DEV_KEYBOARD, 0x75u /* F6 */, FB_INPUT_PRESS, 0, 1100);
    CHECK(cb.on_input_event(&ev) == 1);
    CHECK(g_v1_f6_seen == 1);

    /* 4. Open Mod Menu via LB + View -> navigation inputs are consumed & 'V' does NOT toggle FPV while menu is open */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_PRESS, 0, 1200);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_BACK, FB_INPUT_PRESS, 0, 1210);
    CHECK(cb.on_input_event(&ev) == 1); /* Menu opened! */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_BACK, FB_INPUT_RELEASE, 0, 1220);
    cb.on_input_event(&ev);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_LEFT_SHOULDER, FB_INPUT_RELEASE, 0, 1230);
    cb.on_input_event(&ev);

    /* DPad Down & RB are consumed by open menu */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_DPAD_DOWN, FB_INPUT_PRESS, 0, 1240);
    CHECK(cb.on_input_event(&ev) == 1);
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_RIGHT_SHOULDER, FB_INPUT_PRESS, 0, 1250);
    CHECK(cb.on_input_event(&ev) == 1);

    /* Pressing 'V' or legacy 'F6' while menu is open does NOT trigger plugin actions */
    ev = make_ev(FB_DEV_KEYBOARD, 'V', FB_INPUT_PRESS, 0, 1260);
    cb.on_input_event(&ev);
    CHECK(e2e_peek(0x000) == 2u); /* Still 2, not toggled back to 0! */
    ev = make_ev(FB_DEV_KEYBOARD, 0x75u /* F6 */, FB_INPUT_PRESS, 0, 1265);
    cb.on_input_event(&ev);
    CHECK(g_v1_f6_seen == 1); /* Still 1, suppressed while menu is open! */

    /* On tab 1 (FPV Unlocker), toggle hold_guard_enabled OFF via A, which saves fireball_user_config.json */
    ev = make_ev(FB_DEV_GAMEPAD, FB_PAD_A, FB_INPUT_PRESS, 0, 1268);
    CHECK(cb.on_input_event(&ev) == 1);

    /* Close menu via F1, then 'V' toggles FPV 2 -> 0 again */
    ev = make_ev(FB_DEV_KEYBOARD, 0x70u /* F1 */, FB_INPUT_PRESS, 0, 1270);
    CHECK(cb.on_input_event(&ev) == 1);
    ev = make_ev(FB_DEV_KEYBOARD, 'V', FB_INPUT_PRESS, 0, 1280);
    CHECK(cb.on_input_event(&ev) == 1);
    CHECK(e2e_peek(0x000) == 0u);

    /* Verify fireball_user_config.json was written and persists hold_guard_enabled=false across re-attach! */
    CHECK(GetFileAttributesW(cfg_file) != INVALID_FILE_ATTRIBUTES);
    cb.on_shutdown();

    /* Re-attach core: hold_guard_enabled=false must reload from fireball_user_config.json */
    CHECK(attach_fn(&api, &cb) == FB_OK);
    ev = make_ev(FB_DEV_KEYBOARD, 'V', FB_INPUT_PRESS, 0, 2000);
    CHECK(cb.on_input_event(&ev) == 1);
    CHECK(e2e_peek(0x000) == 2u);
    /* Game resets view_mode -> 0; since persisted hold_guard_enabled is false, frame tick must NOT overwrite 0! */
    e2e_poke(0x000, 0u);
    cb.on_frame_begin(1u, 1.0 / 60.0);
    CHECK(e2e_peek(0x000) == 0u);

    cb.on_shutdown();
    FreeLibrary(core);
}

int wmain(int argc, wchar_t** argv) {
    const wchar_t* core_dll = NULL;
    const wchar_t* fpv_dll  = NULL;
    const wchar_t* tw_dll   = NULL;
    const wchar_t* ss_dll   = NULL;
    const wchar_t* v1_dll   = NULL;

    for (int i = 1; i + 1 < argc; ++i) {
        if (wcscmp(argv[i], L"--core") == 0) {
            core_dll = argv[++i];
        } else if (wcscmp(argv[i], L"--fpv") == 0) {
            fpv_dll = argv[++i];
        } else if (wcscmp(argv[i], L"--time-weather") == 0) {
            tw_dll = argv[++i];
        } else if (wcscmp(argv[i], L"--state-scan") == 0) {
            ss_dll = argv[++i];
        } else if (wcscmp(argv[i], L"--v1-compat") == 0) {
            v1_dll = argv[++i];
        }
    }

    wchar_t tmp[MAX_PATH];
    DWORD n = GetTempPathW((DWORD)(sizeof(tmp) / sizeof(tmp[0])), tmp);
    if (n == 0 || n >= MAX_PATH) {
        return 2;
    }
    wchar_t work[MAX_PATH * 2];
    swprintf(work, sizeof(work) / sizeof(work[0]), L"%lsfireball_menu_test", tmp);
    CreateDirectoryW(work, NULL);

    test_bindings_unit();
    test_settings_persistence_and_menu_nav(work);
    if (core_dll && fpv_dll && tw_dll && ss_dll && v1_dll) {
        test_end_to_end_core(core_dll, fpv_dll, tw_dll, ss_dll, v1_dll, work);
    }

    printf("\nMenu system tests: %s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
