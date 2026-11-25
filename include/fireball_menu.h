/*
 * fireball_menu.h — Fireball-Core Mod Menu, Settings & Action Binding C11 ABI
 *
 * Pure C11, stable ABI, zero C++ STL across the boundary.
 * Additive extension alongside `include/fireball_plugin.h` (FB_PLUGIN_ABI_VERSION = 1).
 *
 * Existing v1 plugins that do not export `fb_plugin_register_menu` continue to load
 * unchanged. Plugins that export `fb_plugin_register_menu` receive `fb_menu_api_v1_t`
 * right after `fb_plugin_init()` succeeds so they can register typed settings, live
 * status rows, buttons, and keyboard + gamepad actions (with chord support) managed
 * centrally by `fireball_core.dll`.
 */

#ifndef FIREBALL_MENU_H
#define FIREBALL_MENU_H

#include "fireball_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FB_MENU_ABI_VERSION 1u

/* Input device kinds (matches fb_input_event_v1_t.device_kind) */
#define FB_DEV_KEYBOARD 0u
#define FB_DEV_GAMEPAD  1u

/* Keyboard modifier bitmask (matches fb_input_event_v1_t.modifiers) */
#define FB_MOD_NONE  0u
#define FB_MOD_SHIFT (1u << 0)
#define FB_MOD_CTRL  (1u << 1)
#define FB_MOD_ALT   (1u << 2)

/* Standard XInput gamepad button bitmasks (matches XINPUT_GAMEPAD_*) */
#define FB_PAD_NONE           0x0000u
#define FB_PAD_DPAD_UP        0x0001u
#define FB_PAD_DPAD_DOWN      0x0002u
#define FB_PAD_DPAD_LEFT      0x0004u
#define FB_PAD_DPAD_RIGHT     0x0008u
#define FB_PAD_START          0x0010u
#define FB_PAD_BACK           0x0020u
#define FB_PAD_LEFT_THUMB     0x0040u
#define FB_PAD_RIGHT_THUMB    0x0080u
#define FB_PAD_LEFT_SHOULDER  0x0100u
#define FB_PAD_RIGHT_SHOULDER 0x0200u
#define FB_PAD_A              0x1000u
#define FB_PAD_B              0x2000u
#define FB_PAD_X              0x4000u
#define FB_PAD_Y              0x8000u

/* Action trigger modes */
typedef enum fb_action_trigger {
    FB_TRIGGER_PRESS = 0, /* Fires once (active=1) on press */
    FB_TRIGGER_HOLD  = 1  /* Fires active=1 on press, active=0 on release */
} fb_action_trigger_t;

/* Setting item types */
typedef enum fb_setting_type {
    FB_SETTING_BOOL   = 1,
    FB_SETTING_INT    = 2,
    FB_SETTING_FLOAT  = 3,
    FB_SETTING_ENUM   = 4,
    FB_SETTING_BUTTON = 5,
    FB_SETTING_STATUS = 6
} fb_setting_type_t;

/* Single keyboard or gamepad binding (optionally with modifiers / chord mask) */
typedef struct fb_binding_chord {
    uint32_t code;      /* 0 = unbound; VK_* for keyboard, FB_PAD_* trigger bit for gamepad */
    uint32_t modifiers; /* FB_MOD_* for keyboard, held FB_PAD_* chord mask for gamepad */
} fb_binding_chord_t;

/* Callback invoked when a bound action triggers (active=1 on press, active=0 on hold release) */
typedef void (*fb_action_fn_t)(const char* action_id, int active, void* user_ctx);

/* Descriptor passed to menu->register_action() */
typedef struct fb_action_desc_v1 {
    uint32_t           struct_size;
    uint32_t           trigger_mode;   /* fb_action_trigger_t */
    const char*        mod_id;
    const char*        action_id;
    const char*        label;
    const char*        description;
    int                consume_input;  /* 1 = suppress matched input from game, 0 = pass through */
    uint32_t           reserved;
    fb_binding_chord_t default_key;    /* code=0 for unbound */
    fb_binding_chord_t default_pad;    /* code=0 for unbound */
    fb_action_fn_t     callback;
    void*              user_ctx;
} fb_action_desc_v1_t;

/* Tagged setting value */
typedef struct fb_setting_value {
    uint32_t type; /* fb_setting_type_t */
    uint32_t reserved;
    union {
        int     bool_val;
        int32_t int_val;
        double  float_val;
        int32_t enum_val;
    } v;
} fb_setting_value_t;

typedef void (*fb_setting_change_fn_t)(const char* setting_id, const fb_setting_value_t* new_value, void* user_ctx);
typedef void (*fb_button_fn_t)(const char* setting_id, void* user_ctx);
typedef void (*fb_status_query_fn_t)(const char* setting_id, char* out_buf, size_t out_cap, void* user_ctx);

/* Descriptor passed to menu->register_setting() */
typedef struct fb_setting_desc_v1 {
    uint32_t               struct_size;
    uint32_t               type;          /* fb_setting_type_t */
    const char*            mod_id;
    const char*            section;       /* Optional section heading inside mod tab */
    const char*            setting_id;
    const char*            label;
    const char*            description;

    /* Typed defaults & bounds (only fields matching `type` are used) */
    int                    default_bool;
    int32_t                default_int;
    int32_t                int_min;
    int32_t                int_max;
    int32_t                int_step;
    int32_t                default_enum;
    double                 default_float;
    double                 float_min;
    double                 float_max;
    double                 float_step;
    const char*            float_fmt;     /* Optional printf format, e.g. "%.2f" or "%.0f ms" */
    const char* const*     enum_labels;   /* Array of `enum_count` UTF-8 strings */
    uint32_t               enum_count;
    uint32_t               reserved;

    /* Callbacks */
    fb_setting_change_fn_t on_change;     /* BOOL / INT / FLOAT / ENUM */
    fb_button_fn_t         on_button;     /* BUTTON */
    fb_status_query_fn_t   on_status;     /* STATUS */
    void*                  user_ctx;
} fb_setting_desc_v1_t;

/* Core Mod Menu, Settings & Action Binding services passed to fb_plugin_register_menu() */
typedef struct fb_menu_api_v1 {
    uint32_t struct_size;
    uint32_t abi_version; /* Must equal FB_MENU_ABI_VERSION */

    int (*register_mod)(const char* mod_id, const char* display_name, const char* description);
    int (*register_action)(const fb_action_desc_v1_t* desc);
    int (*register_setting)(const fb_setting_desc_v1_t* desc);

    int (*get_setting)(const char* mod_id, const char* setting_id, fb_setting_value_t* out_val);
    int (*set_setting)(const char* mod_id, const char* setting_id, const fb_setting_value_t* val, int notify);
    int (*save_user_config)(void);
    int (*is_menu_open)(void);
} fb_menu_api_v1_t;

/*
 * Optional entry point exported by native plugins (*.fbplugin.dll).
 * Called by fireball_core.dll immediately after fb_plugin_init() returns FB_OK.
 */
FB_API int fb_plugin_register_menu(const fb_menu_api_v1_t* menu);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_MENU_H */
