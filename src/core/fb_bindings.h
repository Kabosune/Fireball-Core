/*
 * fb_bindings.h — Central Keyboard + Gamepad Action & Chord Binding Manager.
 *
 * Pure C11 + Win32 API (zero third-party dependencies).
 */

#ifndef FIREBALL_CORE_FB_BINDINGS_H
#define FIREBALL_CORE_FB_BINDINGS_H

#include "fireball_menu.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_BIND_ID_MAX    96
#define FB_BIND_LABEL_MAX 96
#define FB_BIND_DESC_MAX  192

typedef struct fb_binding_override {
    char               mod_id[FB_BIND_ID_MAX];
    char               action_id[FB_BIND_ID_MAX];
    int                has_key;
    fb_binding_chord_t key;
    int                has_pad;
    fb_binding_chord_t pad;
    int                has_consume;
    int                consume;
} fb_binding_override_t;

typedef struct fb_registered_action {
    char               mod_id[FB_BIND_ID_MAX];
    char               action_id[FB_BIND_ID_MAX];
    char               label[FB_BIND_LABEL_MAX];
    char               description[FB_BIND_DESC_MAX];
    uint32_t           trigger_mode;
    int                default_consume;
    int                consume_input;
    fb_binding_chord_t default_key;
    fb_binding_chord_t default_pad;
    fb_binding_chord_t key;
    fb_binding_chord_t pad;
    fb_action_fn_t     callback;
    void*              user_ctx;

    /* Runtime hold state */
    int                active_key;
    int                active_pad;
} fb_registered_action_t;

typedef enum fb_capture_result {
    FB_CAPTURE_NONE      = 0,
    FB_CAPTURE_WAITING   = 1,
    FB_CAPTURE_BOUND     = 2,
    FB_CAPTURE_UNBOUND   = 3,
    FB_CAPTURE_CANCELLED = 4
} fb_capture_result_t;

typedef struct fb_binding_manager {
    fb_registered_action_t* actions;
    size_t                  action_count;
    size_t                  action_cap;

    /* Persisted overrides loaded before plugins register (also preserves unloaded mods) */
    fb_binding_override_t*  overrides;
    size_t                  override_count;
    size_t                  override_cap;

    /* Live held input tracking */
    uint8_t                 held_keys[256];
    uint32_t                held_kbd_mods;
    uint32_t                held_pad_mask;

    /* Interactive rebind capture state */
    int                     capture_active;
    size_t                  capture_action_idx;
    uint32_t                capture_device;       /* FB_DEV_KEYBOARD or FB_DEV_GAMEPAD */
    uint32_t                capture_pad_pending;  /* First pad button pressed, waiting for chord or release */
    uint32_t                capture_pad_b_held;   /* 1 if B is held solo to cancel */
    double                  capture_pad_b_sec;    /* Hold timer for B cancel */
    uint64_t                capture_pad_b_ts_us;  /* Timestamp when B was pressed */
} fb_binding_manager_t;

void fb_bindings_init(fb_binding_manager_t* mgr);
void fb_bindings_free(fb_binding_manager_t* mgr);

int  fb_bindings_set_override(fb_binding_manager_t* mgr, const fb_binding_override_t* ov);
int  fb_bindings_register(fb_binding_manager_t* mgr, const fb_action_desc_v1_t* desc);
int  fb_bindings_find(const fb_binding_manager_t* mgr, const char* mod_id, const char* action_id, size_t* out_idx);

/* Update a binding (and sync override table) */
int  fb_bindings_set_chord(fb_binding_manager_t* mgr, size_t idx, uint32_t device_kind, fb_binding_chord_t chord);
int  fb_bindings_set_consume(fb_binding_manager_t* mgr, size_t idx, int consume);
void fb_bindings_reset_action(fb_binding_manager_t* mgr, size_t idx);
void fb_bindings_reset_mod(fb_binding_manager_t* mgr, const char* mod_id);
void fb_bindings_reset_all(fb_binding_manager_t* mgr);

/* Track raw input transitions (call for every input event so held state stays accurate) */
void fb_bindings_track_input(fb_binding_manager_t* mgr, const fb_input_event_v1_t* ev);

/* Dispatch normal gameplay input against registered actions. Returns 1 if consumed. */
int  fb_bindings_dispatch_input(fb_binding_manager_t* mgr, const fb_input_event_v1_t* ev, int skip_core_toggle);

/* Check specifically if `ev` triggers the named action (e.g. fireball.core/toggle_menu) */
int  fb_bindings_matches_action_press(
    const fb_binding_manager_t* mgr,
    const char*                 mod_id,
    const char*                 action_id,
    const fb_input_event_v1_t*  ev);

/* Interactive rebinding capture */
int  fb_bindings_start_capture(fb_binding_manager_t* mgr, size_t action_idx, uint32_t device_kind);
void fb_bindings_cancel_capture(fb_binding_manager_t* mgr);
fb_capture_result_t fb_bindings_capture_input(fb_binding_manager_t* mgr, const fb_input_event_v1_t* ev);
fb_capture_result_t fb_bindings_capture_tick(fb_binding_manager_t* mgr, double dt_sec);

/* Conflict detection: returns number of conflicting actions (other than `action_idx`) */
size_t fb_bindings_find_conflicts(
    const fb_binding_manager_t* mgr,
    size_t                      action_idx,
    uint32_t                    device_kind,
    fb_binding_chord_t          chord,
    size_t*                     out_indices,
    size_t                      max_out);

/* Human-readable formatting */
void fb_format_vkey(uint32_t vk, char* buf, size_t cap);
void fb_format_pad_button(uint32_t pad_bit, char* buf, size_t cap);
void fb_format_binding(uint32_t device_kind, fb_binding_chord_t chord, char* buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_CORE_FB_BINDINGS_H */
