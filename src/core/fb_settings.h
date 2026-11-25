/*
 * fb_settings.h — Typed Mod Settings Registry & Atomic JSON User Config Persistence.
 *
 * Pure C11 + Win32 API (zero third-party dependencies).
 */

#ifndef FIREBALL_CORE_FB_SETTINGS_H
#define FIREBALL_CORE_FB_SETTINGS_H

#include "fireball_menu.h"
#include "fb_bindings.h"

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_SET_ID_MAX      96
#define FB_SET_LABEL_MAX   96
#define FB_SET_DESC_MAX    192
#define FB_SET_SECTION_MAX 64
#define FB_SET_FMT_MAX     32

typedef struct fb_menu_mod_entry {
    char mod_id[FB_SET_ID_MAX];
    char display_name[FB_SET_LABEL_MAX];
    char description[FB_SET_DESC_MAX];
} fb_menu_mod_entry_t;

typedef struct fb_setting_override {
    char               mod_id[FB_SET_ID_MAX];
    char               setting_id[FB_SET_ID_MAX];
    fb_setting_value_t value;
} fb_setting_override_t;

typedef struct fb_registered_setting {
    char                   mod_id[FB_SET_ID_MAX];
    char                   section[FB_SET_SECTION_MAX];
    char                   setting_id[FB_SET_ID_MAX];
    char                   label[FB_SET_LABEL_MAX];
    char                   description[FB_SET_DESC_MAX];
    uint32_t               type; /* fb_setting_type_t */

    fb_setting_value_t     default_val;
    fb_setting_value_t     current_val;

    int32_t                int_min;
    int32_t                int_max;
    int32_t                int_step;
    double                 float_min;
    double                 float_max;
    double                 float_step;
    char                   float_fmt[FB_SET_FMT_MAX];

    char**                 enum_labels;
    uint32_t               enum_count;

    fb_setting_change_fn_t on_change;
    fb_button_fn_t         on_button;
    fb_status_query_fn_t   on_status;
    void*                  user_ctx;
} fb_registered_setting_t;

typedef struct fb_settings_manager {
    fb_menu_mod_entry_t*     mods;
    size_t                   mod_count;
    size_t                   mod_cap;

    fb_registered_setting_t* settings;
    size_t                   setting_count;
    size_t                   setting_cap;

    fb_setting_override_t*   overrides;
    size_t                   override_count;
    size_t                   override_cap;

    wchar_t                  config_path[520];
} fb_settings_manager_t;

void fb_settings_init(fb_settings_manager_t* mgr);
void fb_settings_free(fb_settings_manager_t* mgr);
void fb_settings_set_config_path(fb_settings_manager_t* mgr, const wchar_t* mods_root_dir);

int  fb_settings_register_mod(
    fb_settings_manager_t* mgr,
    const char*            mod_id,
    const char*            display_name,
    const char*            description);

int  fb_settings_register(fb_settings_manager_t* mgr, const fb_setting_desc_v1_t* desc);
int  fb_settings_find(const fb_settings_manager_t* mgr, const char* mod_id, const char* setting_id, size_t* out_idx);

int  fb_settings_get(
    const fb_settings_manager_t* mgr,
    const char*                  mod_id,
    const char*                  setting_id,
    fb_setting_value_t*          out_val);

int  fb_settings_set_by_index(
    fb_settings_manager_t*    mgr,
    size_t                    idx,
    const fb_setting_value_t* val,
    int                       notify);

int  fb_settings_set(
    fb_settings_manager_t*    mgr,
    const char*               mod_id,
    const char*               setting_id,
    const fb_setting_value_t* val,
    int                       notify);

/* Adjust setting value by +/-1 step (BOOL toggles, INT/FLOAT/ENUM step). Returns 1 if changed. */
int  fb_settings_step(fb_settings_manager_t* mgr, size_t idx, int dir, int notify);

void fb_settings_reset_one(fb_settings_manager_t* mgr, size_t idx, int notify);
void fb_settings_reset_mod(fb_settings_manager_t* mgr, const char* mod_id, int notify);
void fb_settings_reset_all(fb_settings_manager_t* mgr, int notify);

/* Format current setting value for display in the Mod Menu */
void fb_settings_format_value(const fb_registered_setting_t* s, char* buf, size_t cap);

/* Atomic JSON persistence for both settings and bindings */
int  fb_user_config_load(
    fb_settings_manager_t* settings,
    fb_binding_manager_t*  bindings,
    const wchar_t*         path_or_null);

int  fb_user_config_save(
    const fb_settings_manager_t* settings,
    const fb_binding_manager_t*  bindings,
    const wchar_t*               path_or_null);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_CORE_FB_SETTINGS_H */
