/*
 * fireball_console.h — Fireball-Core Command Console C11 ABI
 *
 * Pure C11, stable ABI, zero C++ STL across the boundary.
 * Additive extension alongside `include/fireball_plugin.h` (FB_PLUGIN_ABI_VERSION = 1)
 * and `include/fireball_menu.h` (FB_MENU_ABI_VERSION = 1).
 *
 * Existing v1 plugins that do not export `fb_plugin_register_console` continue to load
 * unchanged. Plugins that export `fb_plugin_register_console` receive `fb_console_api_v1_t`
 * right after `fb_plugin_init()` (and `fb_plugin_register_menu()`, if present) succeeds
 * so they can register named console commands callable from the in-game `~` overlay
 * console, programmatic callers, or the optional local named-pipe terminal REPL.
 */

#ifndef FIREBALL_CONSOLE_H
#define FIREBALL_CONSOLE_H

#include "fireball_plugin.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FB_CONSOLE_ABI_VERSION 1u

/* Output writer passed to command callbacks to emit response lines to the active frontend */
typedef struct fb_console_writer_v1 {
    uint32_t struct_size;
    uint32_t reserved;
    void*    user_ctx;
    void (*print)(void* user_ctx, int level, const char* text);
    void (*print_fmt)(void* user_ctx, int level, const char* fmt, ...);
} fb_console_writer_v1_t;

/*
 * Command handler callback signature.
 * `argc` >= 1 and `argv[0]` is the invoked command name.
 * Return FB_OK (0) on success or a negative fb_status_t error code.
 */
typedef int (*fb_command_fn_t)(
    int                           argc,
    const char* const*            argv,
    const fb_console_writer_v1_t* out,
    void*                         user_ctx);

/* Descriptor passed to console->register_command() */
typedef struct fb_command_desc_v1 {
    uint32_t        struct_size;
    uint32_t        reserved;
    const char*     mod_id;      /* Owning mod_id (e.g. "community.time_weather_control") */
    const char*     name;        /* Command name (e.g. "time", "scan.write") */
    const char*     usage;       /* Usage summary (e.g. "time <HH[:MM]|pause|resume|scale <f>>") */
    const char*     description; /* Short help description */
    fb_command_fn_t callback;
    void*           user_ctx;
} fb_command_desc_v1_t;

/* Core Command Console services passed to fb_plugin_register_console() */
typedef struct fb_console_api_v1 {
    uint32_t struct_size;
    uint32_t abi_version; /* Must equal FB_CONSOLE_ABI_VERSION */

    int (*register_command)(const fb_command_desc_v1_t* desc);
    int (*exec)(const char* line);
    int (*is_console_open)(void);
} fb_console_api_v1_t;

/*
 * Optional entry point exported by native plugins (*.fbplugin.dll).
 * Called by fireball_core.dll immediately after fb_plugin_init() succeeds.
 */
FB_API int fb_plugin_register_console(const fb_console_api_v1_t* console);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_CONSOLE_H */
