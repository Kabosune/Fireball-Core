/*
 * fb_plugins.h — Dynamic `*.fbplugin.dll` discovery, capability enforcement, and event bus.
 *
 * Pure C11 + Win32 API.
 */

#ifndef FIREBALL_CORE_FB_PLUGINS_H
#define FIREBALL_CORE_FB_PLUGINS_H

#include "fireball_plugin.h"
#include "fireball_menu.h"
#include "fireball_console.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int  (*fb_plugin_query_fn)(fb_plugin_info_v1_t* out_info);
typedef int  (*fb_plugin_init_fn)(const fb_core_services_v1_t* core);
typedef void (*fb_plugin_shutdown_fn)(void);
typedef int  (*fb_plugin_register_menu_fn)(const fb_menu_api_v1_t* menu);
typedef int  (*fb_plugin_register_console_fn)(const fb_console_api_v1_t* console);

typedef struct fb_loaded_plugin {
    HMODULE               module;
    fb_plugin_shutdown_fn shutdown_fn;
    char                  mod_id[128];
    char                  plugin_id[128];
    char                  version[64];
    fb_cap_flags_t        required_caps;
    int                   has_menu_api;
    int                   has_console_api;
} fb_loaded_plugin_t;

typedef struct fb_skipped_plugin {
    char mod_id[128];
    char plugin_id[128];
    char version[64];
    char reason[256];
} fb_skipped_plugin_t;

typedef struct fb_event_listener {
    uint32_t      event_id;
    fb_event_fn_t fn;
    void*         user_ctx;
    int           owner_plugin_index;
} fb_event_listener_t;

typedef struct fb_plugin_manager {
    fb_loaded_plugin_t*  plugins;
    size_t               plugin_count;
    size_t               plugin_cap;

    fb_skipped_plugin_t* skipped;
    size_t               skipped_count;
    size_t               skipped_cap;

    fb_event_listener_t* listeners;
    size_t               listener_count;
    size_t               listener_cap;

    int                  loading_plugin_index;
} fb_plugin_manager_t;

void fb_format_caps(fb_cap_flags_t mask, char* out_buf, size_t out_cap);

void fb_plugin_mgr_init(fb_plugin_manager_t* mgr);
void fb_plugin_mgr_discover_and_load(
    fb_plugin_manager_t*         mgr,
    const wchar_t*               mods_root_dir,
    const fb_host_api_v1_t*      host_api,
    const fb_core_services_v1_t* services,
    const fb_menu_api_v1_t*      menu_api,
    const fb_console_api_v1_t*   console_api);

int  fb_plugin_mgr_add_listener(fb_plugin_manager_t* mgr, uint32_t event_id, fb_event_fn_t fn, void* user_ctx);
int  fb_plugin_mgr_dispatch(const fb_plugin_manager_t* mgr, uint32_t event_id, const void* payload);
void fb_plugin_mgr_shutdown_all(fb_plugin_manager_t* mgr, const fb_host_api_v1_t* host_api);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_CORE_FB_PLUGINS_H */
