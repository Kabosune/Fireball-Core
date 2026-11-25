/*
 * fb_bindings.c — Central Keyboard + Gamepad Action & Chord Binding Manager.
 *
 * Pure C11, zero third-party dependencies.
 */

#include "fb_bindings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_CAPTURE_B_CANCEL_SEC 0.50
#define FB_CAPTURE_B_CANCEL_US  500000ull

static int fb_is_kbd_modifier_vk(uint32_t vk) {
    return vk == 0x10u || vk == 0xA0u || vk == 0xA1u || /* Shift */
           vk == 0x11u || vk == 0xA2u || vk == 0xA3u || /* Ctrl */
           vk == 0x12u || vk == 0xA4u || vk == 0xA5u;   /* Alt */
}

static uint32_t fb_vk_to_mod_bit(uint32_t vk) {
    if (vk == 0x10u || vk == 0xA0u || vk == 0xA1u) {
        return FB_MOD_SHIFT;
    }
    if (vk == 0x11u || vk == 0xA2u || vk == 0xA3u) {
        return FB_MOD_CTRL;
    }
    if (vk == 0x12u || vk == 0xA4u || vk == 0xA5u) {
        return FB_MOD_ALT;
    }
    return 0u;
}

static uint32_t fb_recompute_kbd_mods(const fb_binding_manager_t* mgr) {
    uint32_t m = 0u;
    if (mgr->held_keys[0x10u] || mgr->held_keys[0xA0u] || mgr->held_keys[0xA1u]) {
        m |= FB_MOD_SHIFT;
    }
    if (mgr->held_keys[0x11u] || mgr->held_keys[0xA2u] || mgr->held_keys[0xA3u]) {
        m |= FB_MOD_CTRL;
    }
    if (mgr->held_keys[0x12u] || mgr->held_keys[0xA4u] || mgr->held_keys[0xA5u]) {
        m |= FB_MOD_ALT;
    }
    return m;
}

void fb_bindings_init(fb_binding_manager_t* mgr) {
    if (!mgr) {
        return;
    }
    memset(mgr, 0, sizeof(*mgr));
}

void fb_bindings_free(fb_binding_manager_t* mgr) {
    if (!mgr) {
        return;
    }
    free(mgr->actions);
    free(mgr->overrides);
    memset(mgr, 0, sizeof(*mgr));
}

static fb_binding_override_t* fb_find_override(
    fb_binding_manager_t* mgr,
    const char*           mod_id,
    const char*           action_id)
{
    for (size_t i = 0; i < mgr->override_count; ++i) {
        if (strcmp(mgr->overrides[i].mod_id, mod_id) == 0 &&
            strcmp(mgr->overrides[i].action_id, action_id) == 0)
        {
            return &mgr->overrides[i];
        }
    }
    return NULL;
}

static fb_binding_override_t* fb_get_or_add_override(
    fb_binding_manager_t* mgr,
    const char*           mod_id,
    const char*           action_id)
{
    fb_binding_override_t* existing = fb_find_override(mgr, mod_id, action_id);
    if (existing) {
        return existing;
    }
    if (mgr->override_count == mgr->override_cap) {
        size_t new_cap = (mgr->override_cap == 0) ? 16u : (mgr->override_cap * 2u);
        fb_binding_override_t* grown = (fb_binding_override_t*)realloc(
            mgr->overrides,
            new_cap * sizeof(fb_binding_override_t));
        if (!grown) {
            return NULL;
        }
        mgr->overrides = grown;
        mgr->override_cap = new_cap;
    }
    fb_binding_override_t* slot = &mgr->overrides[mgr->override_count++];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->mod_id, sizeof(slot->mod_id), "%s", mod_id);
    snprintf(slot->action_id, sizeof(slot->action_id), "%s", action_id);
    return slot;
}

static void fb_remove_override(fb_binding_manager_t* mgr, const char* mod_id, const char* action_id) {
    for (size_t i = 0; i < mgr->override_count; ++i) {
        if (strcmp(mgr->overrides[i].mod_id, mod_id) == 0 &&
            strcmp(mgr->overrides[i].action_id, action_id) == 0)
        {
            if (i + 1u < mgr->override_count) {
                memmove(
                    &mgr->overrides[i],
                    &mgr->overrides[i + 1u],
                    (mgr->override_count - i - 1u) * sizeof(fb_binding_override_t));
            }
            mgr->override_count -= 1u;
            return;
        }
    }
}

int fb_bindings_set_override(fb_binding_manager_t* mgr, const fb_binding_override_t* ov) {
    if (!mgr || !ov || !ov->mod_id[0] || !ov->action_id[0]) {
        return FB_ERR_INVALID_ARG;
    }
    fb_binding_override_t* dst = fb_get_or_add_override(mgr, ov->mod_id, ov->action_id);
    if (!dst) {
        return FB_ERR_INTERNAL;
    }
    if (ov->has_key) {
        dst->has_key = 1;
        dst->key = ov->key;
    }
    if (ov->has_pad) {
        dst->has_pad = 1;
        dst->pad = ov->pad;
    }
    if (ov->has_consume) {
        dst->has_consume = 1;
        dst->consume = ov->consume ? 1 : 0;
    }

    size_t idx = 0;
    if (fb_bindings_find(mgr, ov->mod_id, ov->action_id, &idx)) {
        fb_registered_action_t* a = &mgr->actions[idx];
        if (dst->has_key) {
            a->key = dst->key;
        }
        if (dst->has_pad) {
            a->pad = dst->pad;
        }
        if (dst->has_consume) {
            a->consume_input = dst->consume;
        }
    }
    return FB_OK;
}

int fb_bindings_find(const fb_binding_manager_t* mgr, const char* mod_id, const char* action_id, size_t* out_idx) {
    if (!mgr || !mod_id || !action_id) {
        return 0;
    }
    for (size_t i = 0; i < mgr->action_count; ++i) {
        if (strcmp(mgr->actions[i].mod_id, mod_id) == 0 &&
            strcmp(mgr->actions[i].action_id, action_id) == 0)
        {
            if (out_idx) {
                *out_idx = i;
            }
            return 1;
        }
    }
    return 0;
}

int fb_bindings_register(fb_binding_manager_t* mgr, const fb_action_desc_v1_t* desc) {
    if (!mgr || !desc || !desc->mod_id || !desc->mod_id[0] || !desc->action_id || !desc->action_id[0]) {
        return FB_ERR_INVALID_ARG;
    }
    if (desc->struct_size < sizeof(fb_action_desc_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }

    fb_registered_action_t* slot = NULL;
    size_t existing_idx = 0;
    if (fb_bindings_find(mgr, desc->mod_id, desc->action_id, &existing_idx)) {
        slot = &mgr->actions[existing_idx];
    } else {
        if (mgr->action_count == mgr->action_cap) {
            size_t new_cap = (mgr->action_cap == 0) ? 16u : (mgr->action_cap * 2u);
            fb_registered_action_t* grown = (fb_registered_action_t*)realloc(
                mgr->actions,
                new_cap * sizeof(fb_registered_action_t));
            if (!grown) {
                return FB_ERR_INTERNAL;
            }
            mgr->actions = grown;
            mgr->action_cap = new_cap;
        }
        slot = &mgr->actions[mgr->action_count++];
    }

    memset(slot, 0, sizeof(*slot));
    snprintf(slot->mod_id, sizeof(slot->mod_id), "%s", desc->mod_id);
    snprintf(slot->action_id, sizeof(slot->action_id), "%s", desc->action_id);
    snprintf(slot->label, sizeof(slot->label), "%s", desc->label ? desc->label : desc->action_id);
    snprintf(slot->description, sizeof(slot->description), "%s", desc->description ? desc->description : "");
    slot->trigger_mode = desc->trigger_mode;
    slot->default_consume = desc->consume_input ? 1 : 0;
    slot->consume_input = slot->default_consume;
    slot->default_key = desc->default_key;
    slot->default_pad = desc->default_pad;
    slot->key = desc->default_key;
    slot->pad = desc->default_pad;
    slot->callback = desc->callback;
    slot->user_ctx = desc->user_ctx;

    /* Apply any persisted user override loaded from fireball_user_config.json */
    fb_binding_override_t* ov = fb_find_override(mgr, slot->mod_id, slot->action_id);
    if (ov) {
        if (ov->has_key) {
            slot->key = ov->key;
        }
        if (ov->has_pad) {
            slot->pad = ov->pad;
        }
        if (ov->has_consume) {
            slot->consume_input = ov->consume;
        }
    }
    return FB_OK;
}

static void fb_sync_override_from_action(fb_binding_manager_t* mgr, const fb_registered_action_t* a) {
    int key_diff = (a->key.code != a->default_key.code || a->key.modifiers != a->default_key.modifiers);
    int pad_diff = (a->pad.code != a->default_pad.code || a->pad.modifiers != a->default_pad.modifiers);
    int con_diff = (a->consume_input != a->default_consume);
    if (!key_diff && !pad_diff && !con_diff) {
        fb_remove_override(mgr, a->mod_id, a->action_id);
        return;
    }
    fb_binding_override_t* ov = fb_get_or_add_override(mgr, a->mod_id, a->action_id);
    if (!ov) {
        return;
    }
    ov->has_key = key_diff;
    ov->key = a->key;
    ov->has_pad = pad_diff;
    ov->pad = a->pad;
    ov->has_consume = con_diff;
    ov->consume = a->consume_input;
}

int fb_bindings_set_chord(fb_binding_manager_t* mgr, size_t idx, uint32_t device_kind, fb_binding_chord_t chord) {
    if (!mgr || idx >= mgr->action_count) {
        return FB_ERR_INVALID_ARG;
    }
    if (chord.code == 0u) {
        chord.modifiers = 0u;
    } else if (device_kind == FB_DEV_GAMEPAD) {
        chord.modifiers &= ~chord.code;
    } else if (device_kind == FB_DEV_KEYBOARD) {
        chord.modifiers &= (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT);
    } else {
        return FB_ERR_INVALID_ARG;
    }

    fb_registered_action_t* a = &mgr->actions[idx];
    if (device_kind == FB_DEV_KEYBOARD) {
        a->key = chord;
    } else {
        a->pad = chord;
    }
    fb_sync_override_from_action(mgr, a);
    return FB_OK;
}

int fb_bindings_set_consume(fb_binding_manager_t* mgr, size_t idx, int consume) {
    if (!mgr || idx >= mgr->action_count) {
        return FB_ERR_INVALID_ARG;
    }
    fb_registered_action_t* a = &mgr->actions[idx];
    a->consume_input = consume ? 1 : 0;
    fb_sync_override_from_action(mgr, a);
    return FB_OK;
}

void fb_bindings_reset_action(fb_binding_manager_t* mgr, size_t idx) {
    if (!mgr || idx >= mgr->action_count) {
        return;
    }
    fb_registered_action_t* a = &mgr->actions[idx];
    a->key = a->default_key;
    a->pad = a->default_pad;
    a->consume_input = a->default_consume;
    fb_remove_override(mgr, a->mod_id, a->action_id);
}

void fb_bindings_reset_mod(fb_binding_manager_t* mgr, const char* mod_id) {
    if (!mgr || !mod_id) {
        return;
    }
    for (size_t i = 0; i < mgr->action_count; ++i) {
        if (strcmp(mgr->actions[i].mod_id, mod_id) == 0) {
            fb_bindings_reset_action(mgr, i);
        }
    }
    /* Remove any pending overrides for that mod as well */
    size_t dst = 0;
    for (size_t i = 0; i < mgr->override_count; ++i) {
        if (strcmp(mgr->overrides[i].mod_id, mod_id) != 0) {
            if (dst != i) {
                mgr->overrides[dst] = mgr->overrides[i];
            }
            dst += 1u;
        }
    }
    mgr->override_count = dst;
}

void fb_bindings_reset_all(fb_binding_manager_t* mgr) {
    if (!mgr) {
        return;
    }
    for (size_t i = 0; i < mgr->action_count; ++i) {
        mgr->actions[i].key = mgr->actions[i].default_key;
        mgr->actions[i].pad = mgr->actions[i].default_pad;
        mgr->actions[i].consume_input = mgr->actions[i].default_consume;
    }
    mgr->override_count = 0;
}

/* ---------------------------------------------------------------------------
 * Live input tracking & matching
 * --------------------------------------------------------------------------- */

void fb_bindings_track_input(fb_binding_manager_t* mgr, const fb_input_event_v1_t* ev) {
    if (!mgr || !ev) {
        return;
    }
    if (ev->device_kind == FB_DEV_KEYBOARD) {
        if (ev->code < 256u) {
            if (ev->action == (uint32_t)FB_INPUT_PRESS || ev->action == (uint32_t)FB_INPUT_REPEAT) {
                mgr->held_keys[ev->code] = 1u;
            } else if (ev->action == (uint32_t)FB_INPUT_RELEASE) {
                mgr->held_keys[ev->code] = 0u;
            }
        }
        uint32_t from_keys = fb_recompute_kbd_mods(mgr);
        uint32_t from_ev = ev->modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT);
        if (ev->action == (uint32_t)FB_INPUT_RELEASE && fb_is_kbd_modifier_vk(ev->code)) {
            uint32_t bit = fb_vk_to_mod_bit(ev->code);
            mgr->held_kbd_mods = (from_keys | from_ev) & ~bit;
        } else {
            mgr->held_kbd_mods = from_keys | from_ev;
        }
    } else if (ev->device_kind == FB_DEV_GAMEPAD) {
        if (ev->action == (uint32_t)FB_INPUT_PRESS || ev->action == (uint32_t)FB_INPUT_REPEAT) {
            mgr->held_pad_mask |= ev->code;
        } else if (ev->action == (uint32_t)FB_INPUT_RELEASE) {
            mgr->held_pad_mask &= ~ev->code;
        }
    }
}

static uint32_t fb_effective_kbd_mods(const fb_binding_manager_t* mgr, const fb_input_event_v1_t* ev) {
    uint32_t m = mgr->held_kbd_mods | (ev->modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT));
    /* Do not count the key itself as a modifier if someone bound a modifier key */
    m &= ~fb_vk_to_mod_bit(ev->code);
    return m;
}

static int fb_kbd_chord_matches(
    const fb_binding_manager_t* mgr,
    fb_binding_chord_t          b,
    const fb_input_event_v1_t*  ev)
{
    if (b.code == 0u || ev->code != b.code) {
        return 0;
    }
    uint32_t cur_mods = fb_effective_kbd_mods(mgr, ev);
    return cur_mods == (b.modifiers & (FB_MOD_SHIFT | FB_MOD_CTRL | FB_MOD_ALT));
}

static int fb_pad_chord_matches(
    const fb_binding_manager_t* mgr,
    fb_binding_chord_t          b,
    const fb_input_event_v1_t*  ev,
    int                         require_chord)
{
    if (b.code == 0u || ev->code != b.code) {
        return 0;
    }
    uint32_t other_held = mgr->held_pad_mask & ~ev->code;
    if (require_chord) {
        return b.modifiers != 0u && (other_held & b.modifiers) == b.modifiers;
    }
    return b.modifiers == 0u;
}

int fb_bindings_matches_action_press(
    const fb_binding_manager_t* mgr,
    const char*                 mod_id,
    const char*                 action_id,
    const fb_input_event_v1_t*  ev)
{
    if (!mgr || !ev || ev->action != (uint32_t)FB_INPUT_PRESS) {
        return 0;
    }
    size_t idx = 0;
    if (!fb_bindings_find(mgr, mod_id, action_id, &idx)) {
        return 0;
    }
    const fb_registered_action_t* a = &mgr->actions[idx];
    if (ev->device_kind == FB_DEV_KEYBOARD) {
        return fb_kbd_chord_matches(mgr, a->key, ev);
    }
    if (ev->device_kind == FB_DEV_GAMEPAD) {
        if (a->pad.modifiers != 0u) {
            return fb_pad_chord_matches(mgr, a->pad, ev, 1);
        }
        return fb_pad_chord_matches(mgr, a->pad, ev, 0);
    }
    return 0;
}

int fb_bindings_dispatch_input(fb_binding_manager_t* mgr, const fb_input_event_v1_t* ev, int skip_core_toggle) {
    if (!mgr || !ev) {
        return 0;
    }
    int consumed = 0;

    if (ev->action == (uint32_t)FB_INPUT_PRESS) {
        if (ev->device_kind == FB_DEV_KEYBOARD) {
            for (size_t i = 0; i < mgr->action_count; ++i) {
                fb_registered_action_t* a = &mgr->actions[i];
                if (skip_core_toggle &&
                    strcmp(a->mod_id, "fireball.core") == 0 &&
                    strcmp(a->action_id, "toggle_menu") == 0)
                {
                    continue;
                }
                if (fb_kbd_chord_matches(mgr, a->key, ev)) {
                    if (a->trigger_mode == (uint32_t)FB_TRIGGER_HOLD) {
                        a->active_key = 1;
                    }
                    if (a->callback) {
                        a->callback(a->action_id, 1, a->user_ctx);
                    }
                    if (a->consume_input) {
                        consumed = 1;
                    }
                }
            }
        } else if (ev->device_kind == FB_DEV_GAMEPAD) {
            /* Pass 1: chord bindings (modifiers != 0) take priority over single-button bindings */
            int any_chord = 0;
            for (size_t i = 0; i < mgr->action_count; ++i) {
                fb_registered_action_t* a = &mgr->actions[i];
                if (skip_core_toggle &&
                    strcmp(a->mod_id, "fireball.core") == 0 &&
                    strcmp(a->action_id, "toggle_menu") == 0)
                {
                    continue;
                }
                if (fb_pad_chord_matches(mgr, a->pad, ev, 1)) {
                    any_chord = 1;
                    if (a->trigger_mode == (uint32_t)FB_TRIGGER_HOLD) {
                        a->active_pad = 1;
                    }
                    if (a->callback) {
                        a->callback(a->action_id, 1, a->user_ctx);
                    }
                    if (a->consume_input) {
                        consumed = 1;
                    }
                }
            }
            /* Pass 2: single-button bindings only fire if no chord consumed this button press */
            if (!any_chord) {
                for (size_t i = 0; i < mgr->action_count; ++i) {
                    fb_registered_action_t* a = &mgr->actions[i];
                    if (skip_core_toggle &&
                        strcmp(a->mod_id, "fireball.core") == 0 &&
                        strcmp(a->action_id, "toggle_menu") == 0)
                    {
                        continue;
                    }
                    if (fb_pad_chord_matches(mgr, a->pad, ev, 0)) {
                        if (a->trigger_mode == (uint32_t)FB_TRIGGER_HOLD) {
                            a->active_pad = 1;
                        }
                        if (a->callback) {
                            a->callback(a->action_id, 1, a->user_ctx);
                        }
                        if (a->consume_input) {
                            consumed = 1;
                        }
                    }
                }
            }
        }
    } else if (ev->action == (uint32_t)FB_INPUT_RELEASE) {
        /* Release active HOLD actions when their trigger or a chord modifier is released */
        for (size_t i = 0; i < mgr->action_count; ++i) {
            fb_registered_action_t* a = &mgr->actions[i];
            if (a->trigger_mode != (uint32_t)FB_TRIGGER_HOLD) {
                continue;
            }
            if (ev->device_kind == FB_DEV_KEYBOARD && a->active_key) {
                uint32_t rel_mod = fb_vk_to_mod_bit(ev->code);
                if (ev->code == a->key.code || (rel_mod != 0u && (a->key.modifiers & rel_mod) != 0u)) {
                    a->active_key = 0;
                    if (a->callback) {
                        a->callback(a->action_id, 0, a->user_ctx);
                    }
                }
            } else if (ev->device_kind == FB_DEV_GAMEPAD && a->active_pad) {
                if (ev->code == a->pad.code || (a->pad.modifiers & ev->code) != 0u) {
                    a->active_pad = 0;
                    if (a->callback) {
                        a->callback(a->action_id, 0, a->user_ctx);
                    }
                }
            }
        }
    }

    return consumed;
}

/* ---------------------------------------------------------------------------
 * Interactive rebinding capture
 * --------------------------------------------------------------------------- */

int fb_bindings_start_capture(fb_binding_manager_t* mgr, size_t action_idx, uint32_t device_kind) {
    if (!mgr || action_idx >= mgr->action_count) {
        return FB_ERR_INVALID_ARG;
    }
    if (device_kind != FB_DEV_KEYBOARD && device_kind != FB_DEV_GAMEPAD) {
        return FB_ERR_INVALID_ARG;
    }
    mgr->capture_active = 1;
    mgr->capture_action_idx = action_idx;
    mgr->capture_device = device_kind;
    mgr->capture_pad_pending = 0u;
    mgr->capture_pad_b_held = 0u;
    mgr->capture_pad_b_sec = 0.0;
    mgr->capture_pad_b_ts_us = 0ull;
    return FB_OK;
}

void fb_bindings_cancel_capture(fb_binding_manager_t* mgr) {
    if (!mgr) {
        return;
    }
    mgr->capture_active = 0;
    mgr->capture_pad_pending = 0u;
    mgr->capture_pad_b_held = 0u;
    mgr->capture_pad_b_sec = 0.0;
    mgr->capture_pad_b_ts_us = 0ull;
}

fb_capture_result_t fb_bindings_capture_tick(fb_binding_manager_t* mgr, double dt_sec) {
    if (!mgr || !mgr->capture_active) {
        return FB_CAPTURE_NONE;
    }
    if (mgr->capture_pad_b_held) {
        if (dt_sec > 0.0 && dt_sec < 1.0) {
            mgr->capture_pad_b_sec += dt_sec;
        }
        if (mgr->capture_pad_b_sec >= FB_CAPTURE_B_CANCEL_SEC) {
            fb_bindings_cancel_capture(mgr);
            return FB_CAPTURE_CANCELLED;
        }
    }
    return FB_CAPTURE_WAITING;
}

fb_capture_result_t fb_bindings_capture_input(fb_binding_manager_t* mgr, const fb_input_event_v1_t* ev) {
    if (!mgr || !mgr->capture_active || !ev) {
        return FB_CAPTURE_NONE;
    }

    if (ev->device_kind == FB_DEV_KEYBOARD) {
        if (ev->action != (uint32_t)FB_INPUT_PRESS) {
            return FB_CAPTURE_WAITING;
        }
        /* Esc always cancels capture */
        if (ev->code == 0x1Bu) { /* VK_ESCAPE */
            fb_bindings_cancel_capture(mgr);
            return FB_CAPTURE_CANCELLED;
        }
        /* Backspace unbinds the active target device slot */
        if (ev->code == 0x08u) { /* VK_BACK */
            fb_binding_chord_t empty = { 0u, 0u };
            fb_bindings_set_chord(mgr, mgr->capture_action_idx, mgr->capture_device, empty);
            fb_bindings_cancel_capture(mgr);
            return FB_CAPTURE_UNBOUND;
        }
        /* Ignore pure modifier presses so user can press Ctrl+Shift+Key */
        if (fb_is_kbd_modifier_vk(ev->code)) {
            return FB_CAPTURE_WAITING;
        }
        /* Bind keyboard chord (auto-switches slot to keyboard if user pressed a key) */
        fb_binding_chord_t chord;
        chord.code = ev->code;
        chord.modifiers = fb_effective_kbd_mods(mgr, ev);
        fb_bindings_set_chord(mgr, mgr->capture_action_idx, FB_DEV_KEYBOARD, chord);
        fb_bindings_cancel_capture(mgr);
        return FB_CAPTURE_BOUND;
    }

    if (ev->device_kind == FB_DEV_GAMEPAD) {
        if (ev->action == (uint32_t)FB_INPUT_PRESS) {
            uint32_t other_held = mgr->held_pad_mask & ~ev->code;

            /* Solo X press unbinds the active target device slot */
            if (ev->code == FB_PAD_X && other_held == 0u && mgr->capture_pad_pending == 0u) {
                fb_binding_chord_t empty = { 0u, 0u };
                fb_bindings_set_chord(mgr, mgr->capture_action_idx, mgr->capture_device, empty);
                fb_bindings_cancel_capture(mgr);
                return FB_CAPTURE_UNBOUND;
            }

            /* Second button pressed while another button is held -> commit chord immediately */
            if (mgr->capture_pad_pending != 0u && ev->code != mgr->capture_pad_pending) {
                fb_binding_chord_t chord;
                chord.code = ev->code;
                chord.modifiers = other_held | mgr->capture_pad_pending;
                chord.modifiers &= ~chord.code;
                fb_bindings_set_chord(mgr, mgr->capture_action_idx, FB_DEV_GAMEPAD, chord);
                fb_bindings_cancel_capture(mgr);
                return FB_CAPTURE_BOUND;
            }
            if (other_held != 0u) {
                fb_binding_chord_t chord;
                chord.code = ev->code;
                chord.modifiers = other_held;
                fb_bindings_set_chord(mgr, mgr->capture_action_idx, FB_DEV_GAMEPAD, chord);
                fb_bindings_cancel_capture(mgr);
                return FB_CAPTURE_BOUND;
            }

            /* First button pressed solo: wait for either a 2nd button (chord) or release (single button) */
            mgr->capture_pad_pending = ev->code;
            if (ev->code == FB_PAD_B) {
                mgr->capture_pad_b_held = 1u;
                mgr->capture_pad_b_sec = 0.0;
                mgr->capture_pad_b_ts_us = ev->timestamp_us;
            }
            return FB_CAPTURE_WAITING;
        }

        if (ev->action == (uint32_t)FB_INPUT_RELEASE) {
            if (mgr->capture_pad_pending != 0u && ev->code == mgr->capture_pad_pending) {
                if (ev->code == FB_PAD_B && mgr->capture_pad_b_held) {
                    int timed_out = (mgr->capture_pad_b_sec >= FB_CAPTURE_B_CANCEL_SEC);
                    if (!timed_out && ev->timestamp_us > mgr->capture_pad_b_ts_us &&
                        (ev->timestamp_us - mgr->capture_pad_b_ts_us) >= FB_CAPTURE_B_CANCEL_US)
                    {
                        timed_out = 1;
                    }
                    if (timed_out) {
                        fb_bindings_cancel_capture(mgr);
                        return FB_CAPTURE_CANCELLED;
                    }
                }
                /* Released solo -> bind as single gamepad button */
                fb_binding_chord_t chord;
                chord.code = ev->code;
                chord.modifiers = 0u;
                fb_bindings_set_chord(mgr, mgr->capture_action_idx, FB_DEV_GAMEPAD, chord);
                fb_bindings_cancel_capture(mgr);
                return FB_CAPTURE_BOUND;
            }
        }
    }

    return FB_CAPTURE_WAITING;
}

/* ---------------------------------------------------------------------------
 * Conflict detection
 * --------------------------------------------------------------------------- */

size_t fb_bindings_find_conflicts(
    const fb_binding_manager_t* mgr,
    size_t                      action_idx,
    uint32_t                    device_kind,
    fb_binding_chord_t          chord,
    size_t*                     out_indices,
    size_t                      max_out)
{
    if (!mgr || chord.code == 0u) {
        return 0u;
    }
    size_t count = 0u;
    for (size_t i = 0; i < mgr->action_count; ++i) {
        if (i == action_idx) {
            continue;
        }
        fb_binding_chord_t other = (device_kind == FB_DEV_KEYBOARD) ? mgr->actions[i].key : mgr->actions[i].pad;
        if (other.code == chord.code && other.modifiers == chord.modifiers) {
            if (out_indices && count < max_out) {
                out_indices[count] = i;
            }
            count += 1u;
        }
    }
    return count;
}

/* ---------------------------------------------------------------------------
 * Human-readable formatting
 * --------------------------------------------------------------------------- */

void fb_format_vkey(uint32_t vk, char* buf, size_t cap) {
    if (!buf || cap == 0u) {
        return;
    }
    if (vk == 0u) {
        snprintf(buf, cap, "Unbound");
        return;
    }
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) {
        snprintf(buf, cap, "%c", (char)vk);
        return;
    }
    if (vk >= 0x70u && vk <= 0x87u) {
        snprintf(buf, cap, "F%u", (unsigned)(vk - 0x70u + 1u));
        return;
    }
    if (vk >= 0x60u && vk <= 0x69u) {
        snprintf(buf, cap, "Num%u", (unsigned)(vk - 0x60u));
        return;
    }
    switch (vk) {
    case 0x08u: snprintf(buf, cap, "Backspace"); return;
    case 0x09u: snprintf(buf, cap, "Tab"); return;
    case 0x0Du: snprintf(buf, cap, "Enter"); return;
    case 0x1Bu: snprintf(buf, cap, "Esc"); return;
    case 0x20u: snprintf(buf, cap, "Space"); return;
    case 0x21u: snprintf(buf, cap, "PgUp"); return;
    case 0x22u: snprintf(buf, cap, "PgDn"); return;
    case 0x23u: snprintf(buf, cap, "End"); return;
    case 0x24u: snprintf(buf, cap, "Home"); return;
    case 0x25u: snprintf(buf, cap, "Left"); return;
    case 0x26u: snprintf(buf, cap, "Up"); return;
    case 0x27u: snprintf(buf, cap, "Right"); return;
    case 0x28u: snprintf(buf, cap, "Down"); return;
    case 0x2Du: snprintf(buf, cap, "Ins"); return;
    case 0x2Eu: snprintf(buf, cap, "Del"); return;
    case 0xBAu: snprintf(buf, cap, ";"); return;
    case 0xBBu: snprintf(buf, cap, "="); return;
    case 0xBCu: snprintf(buf, cap, ","); return;
    case 0xBDu: snprintf(buf, cap, "-"); return;
    case 0xBEu: snprintf(buf, cap, "."); return;
    case 0xBFu: snprintf(buf, cap, "/"); return;
    case 0xC0u: snprintf(buf, cap, "`"); return;
    case 0xDBu: snprintf(buf, cap, "["); return;
    case 0xDCu: snprintf(buf, cap, "\\"); return;
    case 0xDDu: snprintf(buf, cap, "]"); return;
    case 0xDEu: snprintf(buf, cap, "'"); return;
    default:    snprintf(buf, cap, "VK_0x%02X", (unsigned)vk); return;
    }
}

void fb_format_pad_button(uint32_t pad_bit, char* buf, size_t cap) {
    if (!buf || cap == 0u) {
        return;
    }
    switch (pad_bit) {
    case FB_PAD_NONE:           snprintf(buf, cap, "Unbound"); return;
    case FB_PAD_DPAD_UP:        snprintf(buf, cap, "DPad Up"); return;
    case FB_PAD_DPAD_DOWN:      snprintf(buf, cap, "DPad Down"); return;
    case FB_PAD_DPAD_LEFT:      snprintf(buf, cap, "DPad Left"); return;
    case FB_PAD_DPAD_RIGHT:     snprintf(buf, cap, "DPad Right"); return;
    case FB_PAD_START:          snprintf(buf, cap, "Menu"); return;
    case FB_PAD_BACK:           snprintf(buf, cap, "View"); return;
    case FB_PAD_LEFT_THUMB:     snprintf(buf, cap, "L3"); return;
    case FB_PAD_RIGHT_THUMB:    snprintf(buf, cap, "R3"); return;
    case FB_PAD_LEFT_SHOULDER:  snprintf(buf, cap, "LB"); return;
    case FB_PAD_RIGHT_SHOULDER: snprintf(buf, cap, "RB"); return;
    case FB_PAD_A:              snprintf(buf, cap, "A"); return;
    case FB_PAD_B:              snprintf(buf, cap, "B"); return;
    case FB_PAD_X:              snprintf(buf, cap, "X"); return;
    case FB_PAD_Y:              snprintf(buf, cap, "Y"); return;
    default:                    snprintf(buf, cap, "Pad_0x%04X", (unsigned)pad_bit); return;
    }
}

static void fb_append_str(char* buf, size_t cap, size_t* pos, const char* s) {
    if (!buf || cap == 0u || !pos || !s) {
        return;
    }
    int n = snprintf(buf + *pos, (*pos < cap) ? (cap - *pos) : 0u, "%s", s);
    if (n > 0) {
        size_t adv = (size_t)n;
        *pos = (*pos + adv < cap) ? (*pos + adv) : (cap - 1u);
    }
}

void fb_format_binding(uint32_t device_kind, fb_binding_chord_t chord, char* buf, size_t cap) {
    if (!buf || cap == 0u) {
        return;
    }
    buf[0] = '\0';
    if (chord.code == 0u) {
        snprintf(buf, cap, "Unbound");
        return;
    }
    size_t pos = 0u;
    if (device_kind == FB_DEV_KEYBOARD) {
        if ((chord.modifiers & FB_MOD_CTRL) != 0u) {
            fb_append_str(buf, cap, &pos, "Ctrl+");
        }
        if ((chord.modifiers & FB_MOD_ALT) != 0u) {
            fb_append_str(buf, cap, &pos, "Alt+");
        }
        if ((chord.modifiers & FB_MOD_SHIFT) != 0u) {
            fb_append_str(buf, cap, &pos, "Shift+");
        }
        char key_name[32];
        fb_format_vkey(chord.code, key_name, sizeof(key_name));
        fb_append_str(buf, cap, &pos, key_name);
        return;
    }
    if (device_kind == FB_DEV_GAMEPAD) {
        static const uint32_t k_pad_order[] = {
            FB_PAD_LEFT_SHOULDER, FB_PAD_RIGHT_SHOULDER, FB_PAD_LEFT_THUMB, FB_PAD_RIGHT_THUMB,
            FB_PAD_BACK,          FB_PAD_START,          FB_PAD_DPAD_UP,    FB_PAD_DPAD_DOWN,
            FB_PAD_DPAD_LEFT,     FB_PAD_DPAD_RIGHT,     FB_PAD_A,          FB_PAD_B,
            FB_PAD_X,             FB_PAD_Y
        };
        uint32_t mods = chord.modifiers & ~chord.code;
        for (size_t i = 0; i < sizeof(k_pad_order) / sizeof(k_pad_order[0]); ++i) {
            if ((mods & k_pad_order[i]) != 0u) {
                char mname[32];
                fb_format_pad_button(k_pad_order[i], mname, sizeof(mname));
                fb_append_str(buf, cap, &pos, mname);
                fb_append_str(buf, cap, &pos, "+");
            }
        }
        char bname[32];
        fb_format_pad_button(chord.code, bname, sizeof(bname));
        fb_append_str(buf, cap, &pos, bname);
        return;
    }
    snprintf(buf, cap, "?");
}
