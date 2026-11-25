/*
 * fb_settings.c — Typed Mod Settings Registry & Atomic JSON User Config Persistence.
 *
 * Pure C11 + Win32 API (zero third-party dependencies).
 */

#include "fb_settings.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* fb_str_dup_c11(const char* s) {
    if (!s) {
        s = "";
    }
    size_t len = strlen(s);
    char* copy = (char*)malloc(len + 1u);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, s, len + 1u);
    return copy;
}

static void fb_free_setting_entry(fb_registered_setting_t* s) {
    if (!s) {
        return;
    }
    if (s->enum_labels) {
        for (uint32_t i = 0; i < s->enum_count; ++i) {
            free(s->enum_labels[i]);
        }
        free(s->enum_labels);
        s->enum_labels = NULL;
        s->enum_count = 0u;
    }
}

void fb_settings_init(fb_settings_manager_t* mgr) {
    if (!mgr) {
        return;
    }
    memset(mgr, 0, sizeof(*mgr));
}

void fb_settings_free(fb_settings_manager_t* mgr) {
    if (!mgr) {
        return;
    }
    for (size_t i = 0; i < mgr->setting_count; ++i) {
        fb_free_setting_entry(&mgr->settings[i]);
    }
    free(mgr->settings);
    free(mgr->mods);
    free(mgr->overrides);
    memset(mgr, 0, sizeof(*mgr));
}

void fb_settings_set_config_path(fb_settings_manager_t* mgr, const wchar_t* mods_root_dir) {
    if (!mgr) {
        return;
    }
    mgr->config_path[0] = L'\0';
    if (mods_root_dir && mods_root_dir[0] != L'\0') {
        swprintf(
            mgr->config_path,
            sizeof(mgr->config_path) / sizeof(mgr->config_path[0]),
            L"%ls\\fireball_user_config.json",
            mods_root_dir);
    }
}

int fb_settings_register_mod(
    fb_settings_manager_t* mgr,
    const char*            mod_id,
    const char*            display_name,
    const char*            description)
{
    if (!mgr || !mod_id || !mod_id[0]) {
        return FB_ERR_INVALID_ARG;
    }
    for (size_t i = 0; i < mgr->mod_count; ++i) {
        if (strcmp(mgr->mods[i].mod_id, mod_id) == 0) {
            if (display_name && display_name[0]) {
                snprintf(mgr->mods[i].display_name, sizeof(mgr->mods[i].display_name), "%s", display_name);
            }
            if (description && description[0]) {
                snprintf(mgr->mods[i].description, sizeof(mgr->mods[i].description), "%s", description);
            }
            return FB_OK;
        }
    }
    if (mgr->mod_count == mgr->mod_cap) {
        size_t new_cap = (mgr->mod_cap == 0) ? 8u : (mgr->mod_cap * 2u);
        fb_menu_mod_entry_t* grown = (fb_menu_mod_entry_t*)realloc(
            mgr->mods,
            new_cap * sizeof(fb_menu_mod_entry_t));
        if (!grown) {
            return FB_ERR_INTERNAL;
        }
        mgr->mods = grown;
        mgr->mod_cap = new_cap;
    }
    fb_menu_mod_entry_t* m = &mgr->mods[mgr->mod_count++];
    memset(m, 0, sizeof(*m));
    snprintf(m->mod_id, sizeof(m->mod_id), "%s", mod_id);
    snprintf(m->display_name, sizeof(m->display_name), "%s", (display_name && display_name[0]) ? display_name : mod_id);
    snprintf(m->description, sizeof(m->description), "%s", description ? description : "");
    return FB_OK;
}

static fb_setting_override_t* fb_find_setting_override(
    fb_settings_manager_t* mgr,
    const char*            mod_id,
    const char*            setting_id)
{
    for (size_t i = 0; i < mgr->override_count; ++i) {
        if (strcmp(mgr->overrides[i].mod_id, mod_id) == 0 &&
            strcmp(mgr->overrides[i].setting_id, setting_id) == 0)
        {
            return &mgr->overrides[i];
        }
    }
    return NULL;
}

static fb_setting_override_t* fb_get_or_add_setting_override(
    fb_settings_manager_t* mgr,
    const char*            mod_id,
    const char*            setting_id)
{
    fb_setting_override_t* existing = fb_find_setting_override(mgr, mod_id, setting_id);
    if (existing) {
        return existing;
    }
    if (mgr->override_count == mgr->override_cap) {
        size_t new_cap = (mgr->override_cap == 0) ? 16u : (mgr->override_cap * 2u);
        fb_setting_override_t* grown = (fb_setting_override_t*)realloc(
            mgr->overrides,
            new_cap * sizeof(fb_setting_override_t));
        if (!grown) {
            return NULL;
        }
        mgr->overrides = grown;
        mgr->override_cap = new_cap;
    }
    fb_setting_override_t* slot = &mgr->overrides[mgr->override_count++];
    memset(slot, 0, sizeof(*slot));
    snprintf(slot->mod_id, sizeof(slot->mod_id), "%s", mod_id);
    snprintf(slot->setting_id, sizeof(slot->setting_id), "%s", setting_id);
    return slot;
}

static void fb_remove_setting_override(fb_settings_manager_t* mgr, const char* mod_id, const char* setting_id) {
    for (size_t i = 0; i < mgr->override_count; ++i) {
        if (strcmp(mgr->overrides[i].mod_id, mod_id) == 0 &&
            strcmp(mgr->overrides[i].setting_id, setting_id) == 0)
        {
            if (i + 1u < mgr->override_count) {
                memmove(
                    &mgr->overrides[i],
                    &mgr->overrides[i + 1u],
                    (mgr->override_count - i - 1u) * sizeof(fb_setting_override_t));
            }
            mgr->override_count -= 1u;
            return;
        }
    }
}

static fb_setting_value_t fb_clamp_setting_value(const fb_registered_setting_t* s, fb_setting_value_t val) {
    fb_setting_value_t out;
    memset(&out, 0, sizeof(out));
    out.type = s->type;
    switch (s->type) {
    case FB_SETTING_BOOL:
        out.v.bool_val = val.v.bool_val ? 1 : 0;
        break;
    case FB_SETTING_INT: {
        int32_t v = val.v.int_val;
        if (s->int_max > s->int_min) {
            if (v < s->int_min) {
                v = s->int_min;
            }
            if (v > s->int_max) {
                v = s->int_max;
            }
        }
        out.v.int_val = v;
        break;
    }
    case FB_SETTING_FLOAT: {
        double v = val.v.float_val;
        if (isnan(v) || isinf(v)) {
            v = s->default_val.v.float_val;
        }
        if (s->float_max > s->float_min) {
            if (v < s->float_min) {
                v = s->float_min;
            }
            if (v > s->float_max) {
                v = s->float_max;
            }
        }
        out.v.float_val = v;
        break;
    }
    case FB_SETTING_ENUM: {
        int32_t v = val.v.enum_val;
        if (s->enum_count == 0u) {
            v = 0;
        } else {
            if (v < 0) {
                v = 0;
            }
            if ((uint32_t)v >= s->enum_count) {
                v = (int32_t)(s->enum_count - 1u);
            }
        }
        out.v.enum_val = v;
        break;
    }
    default:
        break;
    }
    return out;
}

static int fb_setting_values_equal(const fb_setting_value_t* a, const fb_setting_value_t* b) {
    if (a->type != b->type) {
        return 0;
    }
    switch (a->type) {
    case FB_SETTING_BOOL:  return (a->v.bool_val != 0) == (b->v.bool_val != 0);
    case FB_SETTING_INT:   return a->v.int_val == b->v.int_val;
    case FB_SETTING_FLOAT: return fabs(a->v.float_val - b->v.float_val) < 1e-9;
    case FB_SETTING_ENUM:  return a->v.enum_val == b->v.enum_val;
    default:               return 1;
    }
}

static void fb_sync_setting_override(fb_settings_manager_t* mgr, const fb_registered_setting_t* s) {
    if (s->type == (uint32_t)FB_SETTING_BUTTON || s->type == (uint32_t)FB_SETTING_STATUS) {
        return;
    }
    if (fb_setting_values_equal(&s->current_val, &s->default_val)) {
        fb_remove_setting_override(mgr, s->mod_id, s->setting_id);
        return;
    }
    fb_setting_override_t* ov = fb_get_or_add_setting_override(mgr, s->mod_id, s->setting_id);
    if (ov) {
        ov->value = s->current_val;
    }
}

int fb_settings_find(const fb_settings_manager_t* mgr, const char* mod_id, const char* setting_id, size_t* out_idx) {
    if (!mgr || !mod_id || !setting_id) {
        return 0;
    }
    for (size_t i = 0; i < mgr->setting_count; ++i) {
        if (strcmp(mgr->settings[i].mod_id, mod_id) == 0 &&
            strcmp(mgr->settings[i].setting_id, setting_id) == 0)
        {
            if (out_idx) {
                *out_idx = i;
            }
            return 1;
        }
    }
    return 0;
}

int fb_settings_register(fb_settings_manager_t* mgr, const fb_setting_desc_v1_t* desc) {
    if (!mgr || !desc || !desc->mod_id || !desc->mod_id[0] || !desc->setting_id || !desc->setting_id[0]) {
        return FB_ERR_INVALID_ARG;
    }
    if (desc->struct_size < sizeof(fb_setting_desc_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (desc->type < (uint32_t)FB_SETTING_BOOL || desc->type > (uint32_t)FB_SETTING_STATUS) {
        return FB_ERR_INVALID_ARG;
    }

    fb_settings_register_mod(mgr, desc->mod_id, NULL, NULL);

    fb_registered_setting_t* slot = NULL;
    size_t existing_idx = 0;
    if (fb_settings_find(mgr, desc->mod_id, desc->setting_id, &existing_idx)) {
        slot = &mgr->settings[existing_idx];
        fb_free_setting_entry(slot);
    } else {
        if (mgr->setting_count == mgr->setting_cap) {
            size_t new_cap = (mgr->setting_cap == 0) ? 16u : (mgr->setting_cap * 2u);
            fb_registered_setting_t* grown = (fb_registered_setting_t*)realloc(
                mgr->settings,
                new_cap * sizeof(fb_registered_setting_t));
            if (!grown) {
                return FB_ERR_INTERNAL;
            }
            mgr->settings = grown;
            mgr->setting_cap = new_cap;
        }
        slot = &mgr->settings[mgr->setting_count++];
    }

    memset(slot, 0, sizeof(*slot));
    snprintf(slot->mod_id, sizeof(slot->mod_id), "%s", desc->mod_id);
    snprintf(slot->section, sizeof(slot->section), "%s", desc->section ? desc->section : "");
    snprintf(slot->setting_id, sizeof(slot->setting_id), "%s", desc->setting_id);
    snprintf(slot->label, sizeof(slot->label), "%s", desc->label ? desc->label : desc->setting_id);
    snprintf(slot->description, sizeof(slot->description), "%s", desc->description ? desc->description : "");
    slot->type = desc->type;
    slot->int_min = desc->int_min;
    slot->int_max = desc->int_max;
    slot->int_step = (desc->int_step > 0) ? desc->int_step : 1;
    slot->float_min = desc->float_min;
    slot->float_max = desc->float_max;
    slot->float_step = (desc->float_step > 0.0) ? desc->float_step : 0.1;
    snprintf(slot->float_fmt, sizeof(slot->float_fmt), "%s", (desc->float_fmt && desc->float_fmt[0]) ? desc->float_fmt : "%.2f");
    slot->on_change = desc->on_change;
    slot->on_button = desc->on_button;
    slot->on_status = desc->on_status;
    slot->user_ctx = desc->user_ctx;

    if (desc->type == (uint32_t)FB_SETTING_ENUM && desc->enum_count > 0u && desc->enum_labels) {
        slot->enum_labels = (char**)calloc(desc->enum_count, sizeof(char*));
        if (!slot->enum_labels) {
            return FB_ERR_INTERNAL;
        }
        slot->enum_count = desc->enum_count;
        for (uint32_t i = 0; i < desc->enum_count; ++i) {
            slot->enum_labels[i] = fb_str_dup_c11(desc->enum_labels[i] ? desc->enum_labels[i] : "");
        }
    }

    fb_setting_value_t def;
    memset(&def, 0, sizeof(def));
    def.type = desc->type;
    switch (desc->type) {
    case FB_SETTING_BOOL:  def.v.bool_val = desc->default_bool ? 1 : 0; break;
    case FB_SETTING_INT:   def.v.int_val = desc->default_int; break;
    case FB_SETTING_FLOAT: def.v.float_val = desc->default_float; break;
    case FB_SETTING_ENUM:  def.v.enum_val = desc->default_enum; break;
    default:               break;
    }
    slot->default_val = fb_clamp_setting_value(slot, def);
    slot->current_val = slot->default_val;

    /* Apply persisted override if present in fireball_user_config.json */
    fb_setting_override_t* ov = fb_find_setting_override(mgr, slot->mod_id, slot->setting_id);
    if (ov && slot->type != (uint32_t)FB_SETTING_BUTTON && slot->type != (uint32_t)FB_SETTING_STATUS) {
        fb_setting_value_t converted;
        memset(&converted, 0, sizeof(converted));
        converted.type = slot->type;
        int ok = 0;
        if (slot->type == (uint32_t)FB_SETTING_BOOL && ov->value.type == (uint32_t)FB_SETTING_BOOL) {
            converted.v.bool_val = ov->value.v.bool_val;
            ok = 1;
        } else if (slot->type == (uint32_t)FB_SETTING_INT &&
                   (ov->value.type == (uint32_t)FB_SETTING_INT || ov->value.type == (uint32_t)FB_SETTING_FLOAT))
        {
            converted.v.int_val = (ov->value.type == (uint32_t)FB_SETTING_INT)
                                      ? ov->value.v.int_val
                                      : (int32_t)ov->value.v.float_val;
            ok = 1;
        } else if (slot->type == (uint32_t)FB_SETTING_FLOAT &&
                   (ov->value.type == (uint32_t)FB_SETTING_FLOAT || ov->value.type == (uint32_t)FB_SETTING_INT))
        {
            converted.v.float_val = (ov->value.type == (uint32_t)FB_SETTING_FLOAT)
                                        ? ov->value.v.float_val
                                        : (double)ov->value.v.int_val;
            ok = 1;
        } else if (slot->type == (uint32_t)FB_SETTING_ENUM &&
                   (ov->value.type == (uint32_t)FB_SETTING_INT || ov->value.type == (uint32_t)FB_SETTING_ENUM ||
                    ov->value.type == (uint32_t)FB_SETTING_FLOAT))
        {
            if (ov->value.type == (uint32_t)FB_SETTING_ENUM) {
                converted.v.enum_val = ov->value.v.enum_val;
            } else if (ov->value.type == (uint32_t)FB_SETTING_INT) {
                converted.v.enum_val = ov->value.v.int_val;
            } else {
                converted.v.enum_val = (int32_t)ov->value.v.float_val;
            }
            ok = 1;
        }
        if (ok) {
            slot->current_val = fb_clamp_setting_value(slot, converted);
            ov->value = slot->current_val;
            if (slot->on_change) {
                slot->on_change(slot->setting_id, &slot->current_val, slot->user_ctx);
            }
        }
    }

    return FB_OK;
}

int fb_settings_get(
    const fb_settings_manager_t* mgr,
    const char*                  mod_id,
    const char*                  setting_id,
    fb_setting_value_t*          out_val)
{
    if (!mgr || !mod_id || !setting_id || !out_val) {
        return FB_ERR_INVALID_ARG;
    }
    size_t idx = 0;
    if (!fb_settings_find(mgr, mod_id, setting_id, &idx)) {
        return FB_ERR_NOT_FOUND;
    }
    *out_val = mgr->settings[idx].current_val;
    return FB_OK;
}

int fb_settings_set_by_index(
    fb_settings_manager_t*    mgr,
    size_t                    idx,
    const fb_setting_value_t* val,
    int                       notify)
{
    if (!mgr || !val || idx >= mgr->setting_count) {
        return FB_ERR_INVALID_ARG;
    }
    fb_registered_setting_t* s = &mgr->settings[idx];
    if (s->type == (uint32_t)FB_SETTING_BUTTON || s->type == (uint32_t)FB_SETTING_STATUS) {
        return FB_ERR_UNSUPPORTED;
    }
    fb_setting_value_t clamped = fb_clamp_setting_value(s, *val);
    s->current_val = clamped;
    fb_sync_setting_override(mgr, s);
    if (notify && s->on_change) {
        s->on_change(s->setting_id, &s->current_val, s->user_ctx);
    }
    return FB_OK;
}

int fb_settings_set(
    fb_settings_manager_t*    mgr,
    const char*               mod_id,
    const char*               setting_id,
    const fb_setting_value_t* val,
    int                       notify)
{
    size_t idx = 0;
    if (!fb_settings_find(mgr, mod_id, setting_id, &idx)) {
        return FB_ERR_NOT_FOUND;
    }
    return fb_settings_set_by_index(mgr, idx, val, notify);
}

int fb_settings_step(fb_settings_manager_t* mgr, size_t idx, int dir, int notify) {
    if (!mgr || idx >= mgr->setting_count) {
        return 0;
    }
    fb_registered_setting_t* s = &mgr->settings[idx];
    fb_setting_value_t next = s->current_val;
    switch (s->type) {
    case FB_SETTING_BOOL:
        next.v.bool_val = s->current_val.v.bool_val ? 0 : 1;
        break;
    case FB_SETTING_INT: {
        int64_t step = (int64_t)s->int_step * (int64_t)(dir >= 0 ? 1 : -1);
        int64_t v = (int64_t)s->current_val.v.int_val + step;
        if (s->int_max > s->int_min) {
            if (v < s->int_min) {
                v = s->int_min;
            }
            if (v > s->int_max) {
                v = s->int_max;
            }
        }
        next.v.int_val = (int32_t)v;
        break;
    }
    case FB_SETTING_FLOAT: {
        double v = s->current_val.v.float_val + s->float_step * (dir >= 0 ? 1.0 : -1.0);
        if (s->float_max > s->float_min) {
            if (v < s->float_min) {
                v = s->float_min;
            }
            if (v > s->float_max) {
                v = s->float_max;
            }
        }
        next.v.float_val = v;
        break;
    }
    case FB_SETTING_ENUM: {
        if (s->enum_count == 0u) {
            return 0;
        }
        int32_t n = (int32_t)s->enum_count;
        int32_t cur = s->current_val.v.enum_val;
        next.v.enum_val = (cur + (dir >= 0 ? 1 : -1) + n) % n;
        break;
    }
    default:
        return 0;
    }
    if (fb_setting_values_equal(&next, &s->current_val)) {
        return 0;
    }
    fb_settings_set_by_index(mgr, idx, &next, notify);
    return 1;
}

void fb_settings_reset_one(fb_settings_manager_t* mgr, size_t idx, int notify) {
    if (!mgr || idx >= mgr->setting_count) {
        return;
    }
    fb_registered_setting_t* s = &mgr->settings[idx];
    if (s->type == (uint32_t)FB_SETTING_BUTTON || s->type == (uint32_t)FB_SETTING_STATUS) {
        return;
    }
    s->current_val = s->default_val;
    fb_remove_setting_override(mgr, s->mod_id, s->setting_id);
    if (notify && s->on_change) {
        s->on_change(s->setting_id, &s->current_val, s->user_ctx);
    }
}

void fb_settings_reset_mod(fb_settings_manager_t* mgr, const char* mod_id, int notify) {
    if (!mgr || !mod_id) {
        return;
    }
    for (size_t i = 0; i < mgr->setting_count; ++i) {
        if (strcmp(mgr->settings[i].mod_id, mod_id) == 0) {
            fb_settings_reset_one(mgr, i, notify);
        }
    }
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

void fb_settings_reset_all(fb_settings_manager_t* mgr, int notify) {
    if (!mgr) {
        return;
    }
    for (size_t i = 0; i < mgr->setting_count; ++i) {
        fb_settings_reset_one(mgr, i, notify);
    }
    mgr->override_count = 0;
}

void fb_settings_format_value(const fb_registered_setting_t* s, char* buf, size_t cap) {
    if (!s || !buf || cap == 0u) {
        return;
    }
    switch (s->type) {
    case FB_SETTING_BOOL:
        snprintf(buf, cap, "%s", s->current_val.v.bool_val ? "ON" : "OFF");
        break;
    case FB_SETTING_INT:
        snprintf(buf, cap, "%d", (int)s->current_val.v.int_val);
        break;
    case FB_SETTING_FLOAT:
        snprintf(buf, cap, s->float_fmt[0] ? s->float_fmt : "%.2f", s->current_val.v.float_val);
        break;
    case FB_SETTING_ENUM:
        if (s->enum_labels && s->current_val.v.enum_val >= 0 &&
            (uint32_t)s->current_val.v.enum_val < s->enum_count)
        {
            snprintf(buf, cap, "%s", s->enum_labels[s->current_val.v.enum_val]);
        } else {
            snprintf(buf, cap, "#%d", (int)s->current_val.v.enum_val);
        }
        break;
    case FB_SETTING_BUTTON:
        snprintf(buf, cap, "[Run]");
        break;
    case FB_SETTING_STATUS:
        buf[0] = '\0';
        if (s->on_status) {
            s->on_status(s->setting_id, buf, cap, s->user_ctx);
        }
        break;
    default:
        snprintf(buf, cap, "-");
        break;
    }
}

/* ---------------------------------------------------------------------------
 * Minimal pure-C11 JSON reader / atomic writer for fireball_user_config.json
 * --------------------------------------------------------------------------- */

typedef struct fb_json_cur {
    const char* p;
    const char* end;
} fb_json_cur_t;

static void j_ws(fb_json_cur_t* c) {
    while (c->p < c->end && isspace((unsigned char)*c->p)) {
        c->p++;
    }
}

static int j_char(fb_json_cur_t* c, char ch) {
    j_ws(c);
    if (c->p < c->end && *c->p == ch) {
        c->p++;
        return 1;
    }
    return 0;
}

static int j_str(fb_json_cur_t* c, char* out, size_t cap) {
    j_ws(c);
    if (c->p >= c->end || *c->p != '"') {
        return 0;
    }
    c->p++;
    size_t len = 0u;
    while (c->p < c->end && *c->p != '"') {
        char ch = *c->p++;
        if (ch == '\\' && c->p < c->end) {
            char esc = *c->p++;
            switch (esc) {
            case '"':  ch = '"'; break;
            case '\\': ch = '\\'; break;
            case '/':  ch = '/'; break;
            case 'n':  ch = '\n'; break;
            case 'r':  ch = '\r'; break;
            case 't':  ch = '\t'; break;
            default:   ch = esc; break;
            }
        }
        if (out && len + 1u < cap) {
            out[len++] = ch;
        }
    }
    if (c->p >= c->end || *c->p != '"') {
        return 0;
    }
    c->p++;
    if (out && cap > 0u) {
        out[len] = '\0';
    }
    return 1;
}

static int j_skip_val(fb_json_cur_t* c);

static int j_skip_obj(fb_json_cur_t* c) {
    if (!j_char(c, '{')) {
        return 0;
    }
    if (j_char(c, '}')) {
        return 1;
    }
    for (;;) {
        if (!j_str(c, NULL, 0) || !j_char(c, ':') || !j_skip_val(c)) {
            return 0;
        }
        if (j_char(c, '}')) {
            return 1;
        }
        if (!j_char(c, ',')) {
            return 0;
        }
    }
}

static int j_skip_arr(fb_json_cur_t* c) {
    if (!j_char(c, '[')) {
        return 0;
    }
    if (j_char(c, ']')) {
        return 1;
    }
    for (;;) {
        if (!j_skip_val(c)) {
            return 0;
        }
        if (j_char(c, ']')) {
            return 1;
        }
        if (!j_char(c, ',')) {
            return 0;
        }
    }
}

static int j_skip_val(fb_json_cur_t* c) {
    j_ws(c);
    if (c->p >= c->end) {
        return 0;
    }
    if (*c->p == '"') {
        return j_str(c, NULL, 0);
    }
    if (*c->p == '{') {
        return j_skip_obj(c);
    }
    if (*c->p == '[') {
        return j_skip_arr(c);
    }
    if (c->end - c->p >= 4 && strncmp(c->p, "true", 4) == 0) {
        c->p += 4;
        return 1;
    }
    if (c->end - c->p >= 5 && strncmp(c->p, "false", 5) == 0) {
        c->p += 5;
        return 1;
    }
    if (c->end - c->p >= 4 && strncmp(c->p, "null", 4) == 0) {
        c->p += 4;
        return 1;
    }
    char* end = NULL;
    (void)strtod(c->p, &end);
    if (end == c->p) {
        return 0;
    }
    c->p = end;
    return 1;
}

static int j_read_prim(fb_json_cur_t* c, fb_setting_value_t* out) {
    j_ws(c);
    memset(out, 0, sizeof(*out));
    if (c->p >= c->end) {
        return 0;
    }
    if (c->end - c->p >= 4 && strncmp(c->p, "true", 4) == 0) {
        c->p += 4;
        out->type = (uint32_t)FB_SETTING_BOOL;
        out->v.bool_val = 1;
        return 1;
    }
    if (c->end - c->p >= 5 && strncmp(c->p, "false", 5) == 0) {
        c->p += 5;
        out->type = (uint32_t)FB_SETTING_BOOL;
        out->v.bool_val = 0;
        return 1;
    }
    const char* start = c->p;
    char* end = NULL;
    double dv = strtod(start, &end);
    if (end == start) {
        return 0;
    }
    int is_float = 0;
    for (const char* q = start; q < end; ++q) {
        if (*q == '.' || *q == 'e' || *q == 'E') {
            is_float = 1;
            break;
        }
    }
    c->p = end;
    if (is_float) {
        out->type = (uint32_t)FB_SETTING_FLOAT;
        out->v.float_val = dv;
    } else {
        out->type = (uint32_t)FB_SETTING_INT;
        out->v.int_val = (int32_t)strtoll(start, NULL, 10);
    }
    return 1;
}

static int j_parse_settings_section(fb_json_cur_t* c, fb_settings_manager_t* settings) {
    if (!j_char(c, '{')) {
        return 0;
    }
    if (j_char(c, '}')) {
        return 1;
    }
    for (;;) {
        char mod_id[FB_SET_ID_MAX];
        if (!j_str(c, mod_id, sizeof(mod_id)) || !j_char(c, ':') || !j_char(c, '{')) {
            return 0;
        }
        if (!j_char(c, '}')) {
            for (;;) {
                char set_id[FB_SET_ID_MAX];
                if (!j_str(c, set_id, sizeof(set_id)) || !j_char(c, ':')) {
                    return 0;
                }
                fb_setting_value_t val;
                if (j_read_prim(c, &val)) {
                    if (settings) {
                        fb_setting_override_t* ov = fb_get_or_add_setting_override(settings, mod_id, set_id);
                        if (ov) {
                            ov->value = val;
                        }
                    }
                } else if (!j_skip_val(c)) {
                    return 0;
                }
                if (j_char(c, '}')) {
                    break;
                }
                if (!j_char(c, ',')) {
                    return 0;
                }
            }
        }
        if (j_char(c, '}')) {
            return 1;
        }
        if (!j_char(c, ',')) {
            return 0;
        }
    }
}

static int j_parse_bindings_section(fb_json_cur_t* c, fb_binding_manager_t* bindings) {
    if (!j_char(c, '{')) {
        return 0;
    }
    if (j_char(c, '}')) {
        return 1;
    }
    for (;;) {
        char mod_id[FB_BIND_ID_MAX];
        if (!j_str(c, mod_id, sizeof(mod_id)) || !j_char(c, ':') || !j_char(c, '{')) {
            return 0;
        }
        if (!j_char(c, '}')) {
            for (;;) {
                char act_id[FB_BIND_ID_MAX];
                if (!j_str(c, act_id, sizeof(act_id)) || !j_char(c, ':') || !j_char(c, '{')) {
                    return 0;
                }
                fb_binding_override_t ov;
                memset(&ov, 0, sizeof(ov));
                snprintf(ov.mod_id, sizeof(ov.mod_id), "%s", mod_id);
                snprintf(ov.action_id, sizeof(ov.action_id), "%s", act_id);
                if (!j_char(c, '}')) {
                    for (;;) {
                        char field[32];
                        if (!j_str(c, field, sizeof(field)) || !j_char(c, ':')) {
                            return 0;
                        }
                        fb_setting_value_t pv;
                        if (j_read_prim(c, &pv)) {
                            if (strcmp(field, "key_code") == 0) {
                                ov.has_key = 1;
                                ov.key.code = (uint32_t)pv.v.int_val;
                            } else if (strcmp(field, "key_mods") == 0) {
                                ov.has_key = 1;
                                ov.key.modifiers = (uint32_t)pv.v.int_val;
                            } else if (strcmp(field, "pad_code") == 0) {
                                ov.has_pad = 1;
                                ov.pad.code = (uint32_t)pv.v.int_val;
                            } else if (strcmp(field, "pad_mods") == 0) {
                                ov.has_pad = 1;
                                ov.pad.modifiers = (uint32_t)pv.v.int_val;
                            } else if (strcmp(field, "consume") == 0) {
                                ov.has_consume = 1;
                                ov.consume = (pv.type == (uint32_t)FB_SETTING_BOOL) ? pv.v.bool_val : (pv.v.int_val != 0);
                            }
                        } else if (!j_skip_val(c)) {
                            return 0;
                        }
                        if (j_char(c, '}')) {
                            break;
                        }
                        if (!j_char(c, ',')) {
                            return 0;
                        }
                    }
                }
                if (bindings && (ov.has_key || ov.has_pad || ov.has_consume)) {
                    fb_bindings_set_override(bindings, &ov);
                }
                if (j_char(c, '}')) {
                    break;
                }
                if (!j_char(c, ',')) {
                    return 0;
                }
            }
        }
        if (j_char(c, '}')) {
            return 1;
        }
        if (!j_char(c, ',')) {
            return 0;
        }
    }
}

int fb_user_config_load(
    fb_settings_manager_t* settings,
    fb_binding_manager_t*  bindings,
    const wchar_t*         path_or_null)
{
    const wchar_t* path = path_or_null ? path_or_null : (settings ? settings->config_path : NULL);
    if (!path || path[0] == L'\0') {
        return FB_ERR_NOT_FOUND;
    }

    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return FB_ERR_NOT_FOUND;
    }
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart <= 0 || li.QuadPart > (4ll * 1024ll * 1024ll)) {
        CloseHandle(h);
        return FB_ERR_INVALID_ARG;
    }
    size_t sz = (size_t)li.QuadPart;
    char* buf = (char*)malloc(sz + 1u);
    if (!buf) {
        CloseHandle(h);
        return FB_ERR_INTERNAL;
    }
    DWORD got = 0;
    BOOL ok = ReadFile(h, buf, (DWORD)sz, &got, NULL);
    CloseHandle(h);
    if (!ok || (size_t)got != sz) {
        free(buf);
        return FB_ERR_INTERNAL;
    }
    buf[sz] = '\0';

    fb_json_cur_t cur;
    cur.p = buf;
    cur.end = buf + sz;

    int parsed_ok = 0;
    if (j_char(&cur, '{')) {
        if (j_char(&cur, '}')) {
            parsed_ok = 1;
        } else {
            parsed_ok = 1;
            for (;;) {
                char key[64];
                if (!j_str(&cur, key, sizeof(key)) || !j_char(&cur, ':')) {
                    parsed_ok = 0;
                    break;
                }
                if (strcmp(key, "settings") == 0) {
                    if (!j_parse_settings_section(&cur, settings)) {
                        parsed_ok = 0;
                        break;
                    }
                } else if (strcmp(key, "bindings") == 0) {
                    if (!j_parse_bindings_section(&cur, bindings)) {
                        parsed_ok = 0;
                        break;
                    }
                } else {
                    if (!j_skip_val(&cur)) {
                        parsed_ok = 0;
                        break;
                    }
                }
                if (j_char(&cur, '}')) {
                    break;
                }
                if (!j_char(&cur, ',')) {
                    parsed_ok = 0;
                    break;
                }
            }
        }
    }

    free(buf);
    return parsed_ok ? FB_OK : FB_ERR_INVALID_ARG;
}

typedef struct fb_strbuf {
    char*  data;
    size_t len;
    size_t cap;
    int    oom;
} fb_strbuf_t;

static void sb_append(fb_strbuf_t* sb, const char* s) {
    if (sb->oom || !s) {
        return;
    }
    size_t n = strlen(s);
    if (sb->len + n + 1u > sb->cap) {
        size_t new_cap = (sb->cap == 0) ? 1024u : sb->cap;
        while (new_cap < sb->len + n + 1u) {
            new_cap *= 2u;
        }
        char* grown = (char*)realloc(sb->data, new_cap);
        if (!grown) {
            sb->oom = 1;
            return;
        }
        sb->data = grown;
        sb->cap = new_cap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_json_str(fb_strbuf_t* sb, const char* s) {
    sb_append(sb, "\"");
    if (s) {
        for (const char* p = s; *p; ++p) {
            if (*p == '"') {
                sb_append(sb, "\\\"");
            } else if (*p == '\\') {
                sb_append(sb, "\\\\");
            } else if (*p == '\n') {
                sb_append(sb, "\\n");
            } else if (*p == '\r') {
                sb_append(sb, "\\r");
            } else if (*p == '\t') {
                sb_append(sb, "\\t");
            } else {
                char ch[2] = { *p, '\0' };
                sb_append(sb, ch);
            }
        }
    }
    sb_append(sb, "\"");
}

int fb_user_config_save(
    const fb_settings_manager_t* settings,
    const fb_binding_manager_t*  bindings,
    const wchar_t*               path_or_null)
{
    const wchar_t* path = path_or_null ? path_or_null : (settings ? settings->config_path : NULL);
    if (!path || path[0] == L'\0') {
        return FB_ERR_INVALID_ARG;
    }

    fb_strbuf_t sb;
    memset(&sb, 0, sizeof(sb));
    sb_append(&sb, "{\n  \"schema_version\": 1,\n  \"settings\": {");

    /* Write settings grouped by mod_id */
    if (settings && settings->override_count > 0u) {
        int first_mod = 1;
        for (size_t i = 0; i < settings->override_count; ++i) {
            const char* mid = settings->overrides[i].mod_id;
            int seen_before = 0;
            for (size_t prev = 0; prev < i; ++prev) {
                if (strcmp(settings->overrides[prev].mod_id, mid) == 0) {
                    seen_before = 1;
                    break;
                }
            }
            if (seen_before) {
                continue;
            }
            sb_append(&sb, first_mod ? "\n    " : ",\n    ");
            first_mod = 0;
            sb_json_str(&sb, mid);
            sb_append(&sb, ": {");

            int first_set = 1;
            for (size_t j = i; j < settings->override_count; ++j) {
                if (strcmp(settings->overrides[j].mod_id, mid) != 0) {
                    continue;
                }
                const fb_setting_override_t* ov = &settings->overrides[j];
                sb_append(&sb, first_set ? "\n      " : ",\n      ");
                first_set = 0;
                sb_json_str(&sb, ov->setting_id);
                sb_append(&sb, ": ");
                char vbuf[64];
                switch (ov->value.type) {
                case FB_SETTING_BOOL:
                    sb_append(&sb, ov->value.v.bool_val ? "true" : "false");
                    break;
                case FB_SETTING_INT:
                    snprintf(vbuf, sizeof(vbuf), "%d", (int)ov->value.v.int_val);
                    sb_append(&sb, vbuf);
                    break;
                case FB_SETTING_ENUM:
                    snprintf(vbuf, sizeof(vbuf), "%d", (int)ov->value.v.enum_val);
                    sb_append(&sb, vbuf);
                    break;
                case FB_SETTING_FLOAT:
                    snprintf(vbuf, sizeof(vbuf), "%.6f", ov->value.v.float_val);
                    sb_append(&sb, vbuf);
                    break;
                default:
                    sb_append(&sb, "0");
                    break;
                }
            }
            sb_append(&sb, "\n    }");
        }
        sb_append(&sb, "\n  ");
    }
    sb_append(&sb, "},\n  \"bindings\": {");

    /* Write bindings grouped by mod_id */
    if (bindings && bindings->override_count > 0u) {
        int first_mod = 1;
        for (size_t i = 0; i < bindings->override_count; ++i) {
            const char* mid = bindings->overrides[i].mod_id;
            int seen_before = 0;
            for (size_t prev = 0; prev < i; ++prev) {
                if (strcmp(bindings->overrides[prev].mod_id, mid) == 0) {
                    seen_before = 1;
                    break;
                }
            }
            if (seen_before) {
                continue;
            }
            sb_append(&sb, first_mod ? "\n    " : ",\n    ");
            first_mod = 0;
            sb_json_str(&sb, mid);
            sb_append(&sb, ": {");

            int first_act = 1;
            for (size_t j = i; j < bindings->override_count; ++j) {
                if (strcmp(bindings->overrides[j].mod_id, mid) != 0) {
                    continue;
                }
                const fb_binding_override_t* ov = &bindings->overrides[j];
                sb_append(&sb, first_act ? "\n      " : ",\n      ");
                first_act = 0;
                sb_json_str(&sb, ov->action_id);
                sb_append(&sb, ": {");
                int first_f = 1;
                char nbuf[64];
                if (ov->has_key) {
                    snprintf(nbuf, sizeof(nbuf), "\"key_code\": %u, \"key_mods\": %u",
                             (unsigned)ov->key.code, (unsigned)ov->key.modifiers);
                    sb_append(&sb, nbuf);
                    first_f = 0;
                }
                if (ov->has_pad) {
                    if (!first_f) {
                        sb_append(&sb, ", ");
                    }
                    snprintf(nbuf, sizeof(nbuf), "\"pad_code\": %u, \"pad_mods\": %u",
                             (unsigned)ov->pad.code, (unsigned)ov->pad.modifiers);
                    sb_append(&sb, nbuf);
                    first_f = 0;
                }
                if (ov->has_consume) {
                    if (!first_f) {
                        sb_append(&sb, ", ");
                    }
                    sb_append(&sb, ov->consume ? "\"consume\": true" : "\"consume\": false");
                }
                sb_append(&sb, "}");
            }
            sb_append(&sb, "\n    }");
        }
        sb_append(&sb, "\n  ");
    }
    sb_append(&sb, "}\n}\n");

    if (sb.oom || !sb.data) {
        free(sb.data);
        return FB_ERR_INTERNAL;
    }

    wchar_t tmp_path[540];
    int n = swprintf(tmp_path, sizeof(tmp_path) / sizeof(tmp_path[0]), L"%ls.tmp", path);
    if (n <= 0) {
        free(sb.data);
        return FB_ERR_INVALID_ARG;
    }

    HANDLE h = CreateFileW(tmp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        free(sb.data);
        return FB_ERR_INTERNAL;
    }
    DWORD written = 0;
    BOOL wok = WriteFile(h, sb.data, (DWORD)sb.len, &written, NULL);
    FlushFileBuffers(h);
    CloseHandle(h);
    free(sb.data);

    if (!wok || (size_t)written != sb.len) {
        DeleteFileW(tmp_path);
        return FB_ERR_INTERNAL;
    }
    if (!MoveFileExW(tmp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_path);
        return FB_ERR_INTERNAL;
    }
    return FB_OK;
}
