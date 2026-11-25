/*
 * fb_console.h — Core Command Registry, In-Game `~` Overlay Console, and Local Named-Pipe REPL.
 *
 * Pure C11 + Win32 API (zero third-party dependencies).
 * Uses WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW so VIctory's
 * GameInput keeps receiving keyboard events cleanly while the console overlay is open.
 */

#ifndef FIREBALL_CORE_FB_CONSOLE_H
#define FIREBALL_CORE_FB_CONSOLE_H

#include "fireball_console.h"
#include "fb_bindings.h"
#include "fb_settings.h"
#include "fb_menu_ui.h"
#include "fb_plugins.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_CONSOLE_NAME_MAX     64
#define FB_CONSOLE_MOD_MAX      96
#define FB_CONSOLE_USAGE_MAX    192
#define FB_CONSOLE_DESC_MAX     256
#define FB_CONSOLE_LINE_MAX     384
#define FB_CONSOLE_INPUT_MAX    256
#define FB_CONSOLE_MAX_ARGS     32
#define FB_CONSOLE_MAX_LINES    256
#define FB_CONSOLE_MAX_HISTORY  64

typedef struct fb_registered_command {
    char            mod_id[FB_CONSOLE_MOD_MAX];
    char            name[FB_CONSOLE_NAME_MAX];
    char            usage[FB_CONSOLE_USAGE_MAX];
    char            description[FB_CONSOLE_DESC_MAX];
    fb_command_fn_t callback;
    void*           user_ctx;
} fb_registered_command_t;

typedef struct fb_console_line {
    int  level;
    char text[FB_CONSOLE_LINE_MAX];
} fb_console_line_t;

/* Callback invoked by the named-pipe thread to execute a command under host/core locks */
typedef int (*fb_console_pipe_exec_fn_t)(
    void*                         user_ctx,
    const char*                   line,
    const fb_console_writer_v1_t* out);

typedef struct fb_console {
    CRITICAL_SECTION*           lock;
    fb_settings_manager_t*      settings;
    fb_binding_manager_t*       bindings;
    fb_menu_ui_t*               menu_ui;
    fb_menu_telemetry_ptrs_t    telemetry;

    /* Snapshot of host & plugins (for `mods` built-in without cross-lock) */
    char                        host_name[128];
    char                        host_version[64];
    char                        host_caps_str[512];
    fb_menu_plugin_snapshot_t*  loaded_plugins;
    size_t                      loaded_count;
    fb_menu_skipped_snapshot_t* skipped_plugins;
    size_t                      skipped_count;

    /* Registered commands */
    fb_registered_command_t*    commands;
    size_t                      command_count;
    size_t                      command_cap;

    /* Scrollback ring buffer */
    fb_console_line_t           lines[FB_CONSOLE_MAX_LINES];
    size_t                      line_head;
    size_t                      line_count;
    size_t                      scroll_offset; /* Lines scrolled up from bottom */

    /* Input line & caret state */
    char                        input[FB_CONSOLE_INPUT_MAX];
    size_t                      input_len;
    size_t                      caret;

    /* Command history */
    char                        history[FB_CONSOLE_MAX_HISTORY][FB_CONSOLE_INPUT_MAX];
    size_t                      history_count;
    int                         history_pos; /* -1 = editing draft; 0..history_count-1 = browsing */
    char                        draft[FB_CONSOLE_INPUT_MAX];

    /* Overlay visibility & headless flag */
    int                         is_open;
    int                         headless;

    /* Win32 UI thread state */
    HANDLE                      ui_thread;
    DWORD                       ui_thread_id;
    HWND                        hwnd;
    volatile LONG               ui_ready;
    volatile LONG               dirty;
    int                         width;
    int                         height;

    /* Optional local named-pipe server state */
    HANDLE                      pipe_thread;
    HANDLE                      pipe_stop_event;
    wchar_t                     pipe_name[256];
    fb_console_pipe_exec_fn_t   pipe_exec_fn;
    void*                       pipe_exec_ctx;
} fb_console_t;

void fb_console_init(
    fb_console_t*                   con,
    CRITICAL_SECTION*               lock,
    fb_settings_manager_t*          settings,
    fb_binding_manager_t*           bindings,
    fb_menu_ui_t*                   menu_ui,
    const fb_menu_telemetry_ptrs_t* telemetry);

void fb_console_set_host_snapshot(
    fb_console_t*              con,
    const fb_host_api_v1_t*    host_api,
    const fb_plugin_manager_t* plugin_mgr);

void fb_console_register_builtins(fb_console_t* con);
int  fb_console_register_command(fb_console_t* con, const fb_command_desc_v1_t* desc);
int  fb_console_find_command(const fb_console_t* con, const char* name, size_t* out_idx);

/* Tokenizer exposed for unit testing: splits `line` into argv using `scratch` buffer.
 * Returns FB_OK on success (setting *out_argc) or FB_ERR_INVALID_ARG on syntax error (e.g. unclosed quote). */
int  fb_console_tokenize(
    const char*  line,
    char*        scratch,
    size_t       scratch_cap,
    const char** out_argv,
    int          max_args,
    int*         out_argc);

/* Append a line to the scrollback ring buffer (thread-safe if caller holds con->lock) */
void fb_console_append_line(fb_console_t* con, int level, const char* text);
void fb_console_clear(fb_console_t* con);
size_t fb_console_line_count(const fb_console_t* con);
const fb_console_line_t* fb_console_line_at(const fb_console_t* con, size_t logical_idx);

/* Execute a command line; if `extra_writer` is non-NULL, output is mirrored to it as well as scrollback */
int  fb_console_exec(fb_console_t* con, const char* line, const fb_console_writer_v1_t* extra_writer);

/* Overlay visibility */
void fb_console_set_open(fb_console_t* con, int open);
void fb_console_toggle(fb_console_t* con);
int  fb_console_is_open(const fb_console_t* con);

/* Input handling while console is open. Returns 1 if consumed (all keyboard events while open). */
int  fb_console_handle_input(fb_console_t* con, const fb_input_event_v1_t* ev);
void fb_console_tick(fb_console_t* con, double dt_sec);

/* Local named-pipe server lifecycle (call stop_pipe BEFORE acquiring exclusive core lock on shutdown) */
void fb_console_start_pipe(fb_console_t* con, fb_console_pipe_exec_fn_t exec_fn, void* exec_ctx);
void fb_console_stop_pipe(fb_console_t* con);

void fb_console_shutdown(fb_console_t* con);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_CORE_FB_CONSOLE_H */
