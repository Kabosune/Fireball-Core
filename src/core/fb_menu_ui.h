/*
 * fb_menu_ui.h — Core-hosted In-Game Overlay Mod Menu (State Machine + Win32 GDI Overlay).
 *
 * Pure C11 + Win32 API (zero third-party dependencies).
 * Uses WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW so VIctory's
 * GameInput keeps receiving keyboard and gamepad events cleanly while the overlay is shown.
 */

#ifndef FIREBALL_CORE_FB_MENU_UI_H
#define FIREBALL_CORE_FB_MENU_UI_H

#include "fb_bindings.h"
#include "fb_settings.h"
#include "fb_plugins.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fb_menu_telemetry_ptrs {
    const volatile LONG64* dstorage_calls;
    const volatile LONG64* dstorage_hits;
    const volatile LONG64* dstorage_buf_too_small;
    const volatile LONG64* file_open_calls;
    const volatile LONG64* file_open_hits;
    const volatile LONG64* input_events;
    const volatile LONG64* input_consumed;
    const volatile LONG64* frames_seen;
} fb_menu_telemetry_ptrs_t;

typedef struct fb_menu_plugin_snapshot {
    char mod_id[128];
    char plugin_id[128];
    char version[64];
    int  has_menu_api;
} fb_menu_plugin_snapshot_t;

typedef struct fb_menu_skipped_snapshot {
    char mod_id[128];
    char plugin_id[128];
    char version[64];
    char reason[256];
} fb_menu_skipped_snapshot_t;

typedef enum fb_menu_row_kind {
    FB_ROW_HEADER = 0,
    FB_ROW_INFO,
    FB_ROW_SETTING,
    FB_ROW_ACTION,
    FB_ROW_CORE_SAVE,
    FB_ROW_CORE_RESET_ALL,
    FB_ROW_MOD_RESET
} fb_menu_row_kind_t;

typedef struct fb_menu_row {
    fb_menu_row_kind_t kind;
    size_t             index;      /* index into settings->settings or bindings->actions */
    char               text[256];
    char               subtext[256];
    int                selectable;
} fb_menu_row_t;

#define FB_MENU_MAX_ROWS 256
#define FB_MENU_MAX_TABS 32

typedef struct fb_menu_ui {
    CRITICAL_SECTION*           lock;
    fb_settings_manager_t*      settings;
    fb_binding_manager_t*       bindings;
    fb_menu_telemetry_ptrs_t    telemetry;

    /* Snapshot of host & plugins for Fireball Core tab (no cross-lock needed) */
    char                        host_name[128];
    char                        host_version[64];
    char                        host_caps_str[512];
    fb_menu_plugin_snapshot_t*  loaded_plugins;
    size_t                      loaded_count;
    fb_menu_skipped_snapshot_t* skipped_plugins;
    size_t                      skipped_count;

    /* Menu navigation state */
    int                         is_open;
    int                         headless;
    size_t                      active_tab;     /* 0 = Fireball Core, 1..N = registered mods */
    size_t                      selected_row;
    int                         action_subcol;  /* 0 = Key bind, 1 = Pad bind, 2 = Consume toggle */
    int                         scroll_offset;
    char                        toast[192];
    double                      toast_sec;

    /* Cached layout of current tab */
    fb_menu_row_t               rows[FB_MENU_MAX_ROWS];
    size_t                      row_count;
    char                        tab_mod_ids[FB_MENU_MAX_TABS][FB_SET_ID_MAX];
    char                        tab_labels[FB_MENU_MAX_TABS][FB_SET_LABEL_MAX];
    size_t                      tab_count;

    /* Win32 UI thread state */
    HANDLE                      ui_thread;
    DWORD                       ui_thread_id;
    HWND                        hwnd;
    volatile LONG               ui_ready;
    volatile LONG               dirty;
    int                         width;
    int                         height;
} fb_menu_ui_t;

void fb_menu_ui_init(
    fb_menu_ui_t*                   ui,
    CRITICAL_SECTION*               lock,
    fb_settings_manager_t*          settings,
    fb_binding_manager_t*           bindings,
    const fb_menu_telemetry_ptrs_t* telemetry);

void fb_menu_ui_set_host_snapshot(
    fb_menu_ui_t*              ui,
    const fb_host_api_v1_t*    host_api,
    const fb_plugin_manager_t* plugin_mgr);

void fb_menu_ui_rebuild(fb_menu_ui_t* ui);
void fb_menu_ui_toggle(fb_menu_ui_t* ui);
void fb_menu_ui_set_open(fb_menu_ui_t* ui, int open);
int  fb_menu_ui_is_open(const fb_menu_ui_t* ui);

/* Process an input event while menu is open (or capture active). Returns 1 if consumed. */
int  fb_menu_ui_handle_input(fb_menu_ui_t* ui, const fb_input_event_v1_t* ev);
void fb_menu_ui_tick(fb_menu_ui_t* ui, double dt_sec);

void fb_menu_ui_shutdown(fb_menu_ui_t* ui);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_CORE_FB_MENU_UI_H */
