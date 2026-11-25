/*
 * fb_console.c — Core Command Registry, Built-in Commands, In-Game `~` Overlay Console,
 * and Optional Local Named-Pipe Transport (`\\.\pipe\fireball_console`).
 *
 * Pure C11 + Win32 API (zero third-party dependencies).
 */

#include "fb_console.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008u
#endif

#define FB_CONSOLE_WND_CLASS L"FireballCoreConsoleOverlay"
#define WM_FB_CON_SYNC       (WM_APP + 20)
#define WM_FB_CON_QUIT       (WM_APP + 21)
#define FB_CON_TIMER_ID      2u

#define FB_CON_WIDTH         860
#define FB_CON_HEIGHT        380
#define FB_CON_HEADER_H      36
#define FB_CON_INPUT_H       36
#define FB_CON_PAD_X         14
#define FB_CON_LINE_H        18

/* Dark slate palette matching fb_menu_ui.c */
#define CON_COL_BG           RGB(18, 21, 28)
#define CON_COL_SURFACE      RGB(26, 31, 43)
#define CON_COL_BORDER       RGB(48, 57, 76)
#define CON_COL_ACCENT       RGB(255, 107, 53)
#define CON_COL_TEXT         RGB(232, 236, 244)
#define CON_COL_MUTED        RGB(150, 162, 184)
#define CON_COL_GREEN        RGB(52, 211, 153)
#define CON_COL_AMBER        RGB(245, 158, 11)
#define CON_COL_RED          RGB(248, 113, 113)

/* ---------------------------------------------------------------------------
 * Environment helpers
 * --------------------------------------------------------------------------- */

static int fb_con_env_is_headless(void) {
    wchar_t val[16];
    DWORD n = GetEnvironmentVariableW(L"FIREBALL_HEADLESS", val, (DWORD)(sizeof(val) / sizeof(val[0])));
    if (n > 0 && val[0] != L'\0' && wcscmp(val, L"0") != 0) {
        return 1;
    }
    return 0;
}

static int fb_con_resolve_pipe_name(int headless, wchar_t* out_name, size_t out_cap) {
    wchar_t env[220];
    DWORD n = GetEnvironmentVariableW(L"FIREBALL_CONSOLE_PIPE", env, (DWORD)(sizeof(env) / sizeof(env[0])));
    if (n > 0 && n < (DWORD)(sizeof(env) / sizeof(env[0]))) {
        if (wcscmp(env, L"0") == 0 || _wcsicmp(env, L"off") == 0 || _wcsicmp(env, L"false") == 0) {
            out_name[0] = L'\0';
            return 0;
        }
        if (wcscmp(env, L"1") == 0 || _wcsicmp(env, L"on") == 0 || _wcsicmp(env, L"true") == 0) {
            swprintf(out_name, out_cap, L"\\\\.\\pipe\\fireball_console");
            return 1;
        }
        if (wcsncmp(env, L"\\\\.\\pipe\\", 9) == 0) {
            swprintf(out_name, out_cap, L"%ls", env);
        } else {
            swprintf(out_name, out_cap, L"\\\\.\\pipe\\%ls", env);
        }
        return 1;
    }
    if (headless) {
        out_name[0] = L'\0';
        return 0;
    }
    swprintf(out_name, out_cap, L"\\\\.\\pipe\\fireball_console");
    return 1;
}

/* ---------------------------------------------------------------------------
 * Scrollback ring buffer & writer plumbing
 * --------------------------------------------------------------------------- */

void fb_console_append_line(fb_console_t* con, int level, const char* text) {
    if (!con || !text) {
        return;
    }
    /* Split embedded newlines into separate scrollback lines */
    const char* p = text;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        while (len > 0 && p[len - 1] == '\r') {
            --len;
        }
        size_t slot = 0u;
        if (con->line_count < FB_CONSOLE_MAX_LINES) {
            slot = (con->line_head + con->line_count) % FB_CONSOLE_MAX_LINES;
            con->line_count += 1u;
        } else {
            slot = con->line_head;
            con->line_head = (con->line_head + 1u) % FB_CONSOLE_MAX_LINES;
        }
        con->lines[slot].level = level;
        if (len >= FB_CONSOLE_LINE_MAX) {
            len = FB_CONSOLE_LINE_MAX - 1u;
        }
        memcpy(con->lines[slot].text, p, len);
        con->lines[slot].text[len] = '\0';

        if (!nl) {
            break;
        }
        p = nl + 1;
        if (*p == '\0') {
            break;
        }
    }
    InterlockedExchange(&con->dirty, 1);
}

void fb_console_clear(fb_console_t* con) {
    if (!con) {
        return;
    }
    con->line_head = 0u;
    con->line_count = 0u;
    con->scroll_offset = 0u;
    InterlockedExchange(&con->dirty, 1);
}

size_t fb_console_line_count(const fb_console_t* con) {
    return con ? con->line_count : 0u;
}

const fb_console_line_t* fb_console_line_at(const fb_console_t* con, size_t logical_idx) {
    if (!con || logical_idx >= con->line_count) {
        return NULL;
    }
    size_t slot = (con->line_head + logical_idx) % FB_CONSOLE_MAX_LINES;
    return &con->lines[slot];
}

typedef struct fb_dispatch_writer_ctx {
    fb_console_t*                 con;
    const fb_console_writer_v1_t* mirror;
} fb_dispatch_writer_ctx_t;

static void fb_dispatch_print(void* user_ctx, int level, const char* text) {
    fb_dispatch_writer_ctx_t* dw = (fb_dispatch_writer_ctx_t*)user_ctx;
    if (!dw || !text) {
        return;
    }
    if (dw->con) {
        fb_console_append_line(dw->con, level, text);
    }
    if (dw->mirror && dw->mirror->print) {
        dw->mirror->print(dw->mirror->user_ctx, level, text);
    }
}

static void fb_dispatch_print_fmt(void* user_ctx, int level, const char* fmt, ...) {
    if (!fmt) {
        return;
    }
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fb_dispatch_print(user_ctx, level, buf);
}

static fb_console_writer_v1_t fb_make_dispatch_writer(fb_dispatch_writer_ctx_t* dw) {
    fb_console_writer_v1_t w;
    memset(&w, 0, sizeof(w));
    w.struct_size = (uint32_t)sizeof(w);
    w.user_ctx = dw;
    w.print = fb_dispatch_print;
    w.print_fmt = fb_dispatch_print_fmt;
    return w;
}

/* ---------------------------------------------------------------------------
 * Tokenizer (supports single/double quotes and backslash escapes)
 * --------------------------------------------------------------------------- */

int fb_console_tokenize(
    const char*  line,
    char*        scratch,
    size_t       scratch_cap,
    const char** out_argv,
    int          max_args,
    int*         out_argc)
{
    if (!line || !scratch || scratch_cap == 0 || !out_argv || max_args <= 0 || !out_argc) {
        return FB_ERR_INVALID_ARG;
    }
    int argc = 0;
    size_t dst = 0u;
    const char* p = line;

    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            ++p;
        }
        if (*p == '\0') {
            break;
        }
        if (argc >= max_args) {
            return FB_ERR_INVALID_ARG;
        }
        out_argv[argc++] = scratch + dst;
        char quote = '\0';

        while (*p) {
            char c = *p;
            if (quote != '\0') {
                if (c == quote) {
                    quote = '\0';
                    ++p;
                    continue;
                }
                if (c == '\\' && quote == '"' && p[1] != '\0') {
                    ++p;
                    char esc = *p++;
                    if (esc == 'n') {
                        esc = '\n';
                    } else if (esc == 't') {
                        esc = '\t';
                    } else if (esc == 'r') {
                        esc = '\r';
                    }
                    if (dst + 1u >= scratch_cap) {
                        return FB_ERR_INVALID_ARG;
                    }
                    scratch[dst++] = esc;
                    continue;
                }
                if (dst + 1u >= scratch_cap) {
                    return FB_ERR_INVALID_ARG;
                }
                scratch[dst++] = *p++;
            } else {
                if (isspace((unsigned char)c)) {
                    break;
                }
                if (c == '"' || c == '\'') {
                    quote = c;
                    ++p;
                    continue;
                }
                if (c == '\\' && p[1] != '\0') {
                    ++p;
                }
                if (dst + 1u >= scratch_cap) {
                    return FB_ERR_INVALID_ARG;
                }
                scratch[dst++] = *p++;
            }
        }
        if (quote != '\0') {
            *out_argc = 0;
            return FB_ERR_INVALID_ARG; /* Unclosed quote */
        }
        if (dst >= scratch_cap) {
            return FB_ERR_INVALID_ARG;
        }
        scratch[dst++] = '\0';
    }

    *out_argc = argc;
    return FB_OK;
}

/* ---------------------------------------------------------------------------
 * Command registration & lookup
 * --------------------------------------------------------------------------- */

int fb_console_find_command(const fb_console_t* con, const char* name, size_t* out_idx) {
    if (!con || !name || !name[0]) {
        return 0;
    }
    for (size_t i = 0; i < con->command_count; ++i) {
        if (_stricmp(con->commands[i].name, name) == 0) {
            if (out_idx) {
                *out_idx = i;
            }
            return 1;
        }
    }
    return 0;
}

int fb_console_register_command(fb_console_t* con, const fb_command_desc_v1_t* desc) {
    if (!con || !desc) {
        return FB_ERR_INVALID_ARG;
    }
    if (desc->struct_size < sizeof(fb_command_desc_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (!desc->name || !desc->name[0] || !desc->callback) {
        return FB_ERR_INVALID_ARG;
    }
    for (const char* p = desc->name; *p; ++p) {
        if (isspace((unsigned char)*p)) {
            return FB_ERR_INVALID_ARG;
        }
    }

    size_t existing = 0u;
    fb_registered_command_t* slot = NULL;
    if (fb_console_find_command(con, desc->name, &existing)) {
        slot = &con->commands[existing];
    } else {
        if (con->command_count == con->command_cap) {
            size_t new_cap = (con->command_cap == 0u) ? 16u : (con->command_cap * 2u);
            fb_registered_command_t* grown = (fb_registered_command_t*)realloc(
                con->commands,
                new_cap * sizeof(fb_registered_command_t));
            if (!grown) {
                return FB_ERR_INTERNAL;
            }
            con->commands = grown;
            con->command_cap = new_cap;
        }
        slot = &con->commands[con->command_count++];
    }

    memset(slot, 0, sizeof(*slot));
    snprintf(slot->mod_id, sizeof(slot->mod_id), "%s", (desc->mod_id && desc->mod_id[0]) ? desc->mod_id : "fireball.core");
    snprintf(slot->name, sizeof(slot->name), "%s", desc->name);
    snprintf(slot->usage, sizeof(slot->usage), "%s", (desc->usage && desc->usage[0]) ? desc->usage : desc->name);
    snprintf(slot->description, sizeof(slot->description), "%s", desc->description ? desc->description : "");
    slot->callback = desc->callback;
    slot->user_ctx = desc->user_ctx;
    return FB_OK;
}

/* ---------------------------------------------------------------------------
 * Built-in core commands
 * --------------------------------------------------------------------------- */

static int cmd_help(int argc, const char* const* argv, const fb_console_writer_v1_t* out, void* user_ctx) {
    fb_console_t* con = (fb_console_t*)user_ctx;
    if (!con || !out || !out->print_fmt) {
        return FB_ERR_INTERNAL;
    }
    if (argc >= 2) {
        size_t idx = 0u;
        if (!fb_console_find_command(con, argv[1], &idx)) {
            out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Unknown command '%s'", argv[1]);
            return FB_ERR_NOT_FOUND;
        }
        const fb_registered_command_t* c = &con->commands[idx];
        out->print_fmt(out->user_ctx, FB_LOG_INFO, "%s (%s)", c->name, c->mod_id);
        out->print_fmt(out->user_ctx, FB_LOG_INFO, "  usage: %s", c->usage);
        if (c->description[0]) {
            out->print_fmt(out->user_ctx, FB_LOG_INFO, "  %s", c->description);
        }
        return FB_OK;
    }

    out->print_fmt(out->user_ctx, FB_LOG_INFO, "Registered commands (%zu):", con->command_count);
    for (size_t i = 0; i < con->command_count; ++i) {
        const fb_registered_command_t* c = &con->commands[i];
        out->print_fmt(out->user_ctx, FB_LOG_INFO, "  %-16s %s", c->name, c->description);
    }
    return FB_OK;
}

static int cmd_clear(int argc, const char* const* argv, const fb_console_writer_v1_t* out, void* user_ctx) {
    (void)argc;
    (void)argv;
    (void)out;
    fb_console_clear((fb_console_t*)user_ctx);
    return FB_OK;
}

static int cmd_mods(int argc, const char* const* argv, const fb_console_writer_v1_t* out, void* user_ctx) {
    (void)argc;
    (void)argv;
    fb_console_t* con = (fb_console_t*)user_ctx;
    if (!con || !out || !out->print_fmt) {
        return FB_ERR_INTERNAL;
    }
    out->print_fmt(
        out->user_ctx,
        FB_LOG_INFO,
        "Host: %s v%s | caps: %s",
        con->host_name[0] ? con->host_name : "unknown",
        con->host_version[0] ? con->host_version : "0.0.0",
        con->host_caps_str[0] ? con->host_caps_str : "FB_CAP_NONE");

    out->print_fmt(out->user_ctx, FB_LOG_INFO, "Loaded plugins (%zu):", con->loaded_count);
    if (con->loaded_count == 0u) {
        out->print(out->user_ctx, FB_LOG_INFO, "  (none)");
    } else {
        for (size_t i = 0; i < con->loaded_count; ++i) {
            const fb_menu_plugin_snapshot_t* p = &con->loaded_plugins[i];
            out->print_fmt(
                out->user_ctx,
                FB_LOG_INFO,
                "  %s (%s v%s)%s%s",
                p->plugin_id,
                p->mod_id,
                p->version,
                p->has_menu_api ? " [menu]" : " [v1]",
                p->has_console_api ? " [console]" : "");
        }
    }

    if (con->skipped_count > 0u) {
        out->print_fmt(out->user_ctx, FB_LOG_WARN, "Skipped plugins (%zu):", con->skipped_count);
        for (size_t i = 0; i < con->skipped_count; ++i) {
            const fb_menu_skipped_snapshot_t* s = &con->skipped_plugins[i];
            out->print_fmt(out->user_ctx, FB_LOG_WARN, "  %s (%s): %s", s->plugin_id, s->mod_id, s->reason);
        }
    }
    return FB_OK;
}

static int cmd_stats(int argc, const char* const* argv, const fb_console_writer_v1_t* out, void* user_ctx) {
    (void)argc;
    (void)argv;
    fb_console_t* con = (fb_console_t*)user_ctx;
    if (!con || !out || !out->print_fmt) {
        return FB_ERR_INTERNAL;
    }
    long long ds_calls = con->telemetry.dstorage_calls ? (long long)*con->telemetry.dstorage_calls : 0ll;
    long long ds_hits  = con->telemetry.dstorage_hits  ? (long long)*con->telemetry.dstorage_hits  : 0ll;
    long long ds_small = con->telemetry.dstorage_buf_too_small ? (long long)*con->telemetry.dstorage_buf_too_small : 0ll;
    long long fo_calls = con->telemetry.file_open_calls ? (long long)*con->telemetry.file_open_calls : 0ll;
    long long fo_hits  = con->telemetry.file_open_hits  ? (long long)*con->telemetry.file_open_hits  : 0ll;
    long long frames   = con->telemetry.frames_seen     ? (long long)*con->telemetry.frames_seen     : 0ll;
    long long in_ev    = con->telemetry.input_events    ? (long long)*con->telemetry.input_events    : 0ll;
    long long in_cons  = con->telemetry.input_consumed  ? (long long)*con->telemetry.input_consumed  : 0ll;

    out->print_fmt(
        out->user_ctx,
        FB_LOG_INFO,
        "DirectStorage: calls=%lld hits=%lld buf_too_small=%lld",
        ds_calls, ds_hits, ds_small);
    out->print_fmt(
        out->user_ctx,
        FB_LOG_INFO,
        "FileOpen:      calls=%lld hits=%lld",
        fo_calls, fo_hits);
    out->print_fmt(
        out->user_ctx,
        FB_LOG_INFO,
        "Frames/Input:  frames_seen=%lld input_events=%lld input_consumed=%lld",
        frames, in_ev, in_cons);
    return FB_OK;
}

static int cmd_get(int argc, const char* const* argv, const fb_console_writer_v1_t* out, void* user_ctx) {
    fb_console_t* con = (fb_console_t*)user_ctx;
    if (!con || !con->settings || !out || !out->print_fmt) {
        return FB_ERR_INTERNAL;
    }
    if (argc < 2) {
        out->print(out->user_ctx, FB_LOG_ERROR, "Usage: get <mod_id> [setting_id]");
        return FB_ERR_INVALID_ARG;
    }
    const char* mod_id = argv[1];
    if (argc == 2) {
        size_t found = 0u;
        for (size_t i = 0; i < con->settings->setting_count; ++i) {
            const fb_registered_setting_t* s = &con->settings->settings[i];
            if (_stricmp(s->mod_id, mod_id) == 0) {
                char vbuf[160];
                fb_settings_format_value(s, vbuf, sizeof(vbuf));
                out->print_fmt(out->user_ctx, FB_LOG_INFO, "  %s.%s = %s", s->mod_id, s->setting_id, vbuf);
                ++found;
            }
        }
        if (found == 0u) {
            out->print_fmt(out->user_ctx, FB_LOG_WARN, "No settings registered for mod '%s'", mod_id);
            return FB_ERR_NOT_FOUND;
        }
        return FB_OK;
    }

    size_t idx = 0u;
    if (!fb_settings_find(con->settings, mod_id, argv[2], &idx)) {
        out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Setting '%s.%s' not found", mod_id, argv[2]);
        return FB_ERR_NOT_FOUND;
    }
    const fb_registered_setting_t* s = &con->settings->settings[idx];
    char vbuf[192];
    fb_settings_format_value(s, vbuf, sizeof(vbuf));
    out->print_fmt(out->user_ctx, FB_LOG_INFO, "%s.%s = %s", s->mod_id, s->setting_id, vbuf);
    return FB_OK;
}

static int cmd_set(int argc, const char* const* argv, const fb_console_writer_v1_t* out, void* user_ctx) {
    fb_console_t* con = (fb_console_t*)user_ctx;
    if (!con || !con->settings || !out || !out->print_fmt) {
        return FB_ERR_INTERNAL;
    }
    if (argc < 4) {
        out->print(out->user_ctx, FB_LOG_ERROR, "Usage: set <mod_id> <setting_id> <value>");
        return FB_ERR_INVALID_ARG;
    }
    size_t idx = 0u;
    if (!fb_settings_find(con->settings, argv[1], argv[2], &idx)) {
        out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Setting '%s.%s' not found", argv[1], argv[2]);
        return FB_ERR_NOT_FOUND;
    }
    const fb_registered_setting_t* s = &con->settings->settings[idx];
    fb_setting_value_t val;
    memset(&val, 0, sizeof(val));
    val.type = s->type;
    const char* raw = argv[3];

    if (s->type == (uint32_t)FB_SETTING_BOOL) {
        if (_stricmp(raw, "1") == 0 || _stricmp(raw, "true") == 0 || _stricmp(raw, "on") == 0 || _stricmp(raw, "yes") == 0) {
            val.v.bool_val = 1;
        } else if (_stricmp(raw, "0") == 0 || _stricmp(raw, "false") == 0 || _stricmp(raw, "off") == 0 || _stricmp(raw, "no") == 0) {
            val.v.bool_val = 0;
        } else {
            out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Invalid boolean value '%s' (expected true/false/1/0/on/off)", raw);
            return FB_ERR_INVALID_ARG;
        }
    } else if (s->type == (uint32_t)FB_SETTING_INT) {
        char* end = NULL;
        long parsed = strtol(raw, &end, 0);
        if (end == raw || *end != '\0') {
            out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Invalid integer value '%s'", raw);
            return FB_ERR_INVALID_ARG;
        }
        val.v.int_val = (int32_t)parsed;
    } else if (s->type == (uint32_t)FB_SETTING_FLOAT) {
        char* end = NULL;
        double parsed = strtod(raw, &end);
        if (end == raw || *end != '\0') {
            out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Invalid float value '%s'", raw);
            return FB_ERR_INVALID_ARG;
        }
        val.v.float_val = parsed;
    } else if (s->type == (uint32_t)FB_SETTING_ENUM) {
        int matched = 0;
        for (uint32_t i = 0; i < s->enum_count; ++i) {
            if (s->enum_labels && s->enum_labels[i] && _stricmp(s->enum_labels[i], raw) == 0) {
                val.v.enum_val = (int32_t)i;
                matched = 1;
                break;
            }
        }
        if (!matched) {
            char* end = NULL;
            long parsed = strtol(raw, &end, 0);
            if (end == raw || *end != '\0') {
                out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Invalid enum value '%s' (expected index or label)", raw);
                return FB_ERR_INVALID_ARG;
            }
            val.v.enum_val = (int32_t)parsed;
        }
    } else {
        out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Setting '%s.%s' is not writable (use `trigger` for buttons)", s->mod_id, s->setting_id);
        return FB_ERR_INVALID_ARG;
    }

    int rc = fb_settings_set_by_index(con->settings, idx, &val, 1);
    if (rc != FB_OK) {
        out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Failed to set '%s.%s' (rc=%d)", s->mod_id, s->setting_id, rc);
        return rc;
    }
    if (con->menu_ui) {
        InterlockedExchange(&con->menu_ui->dirty, 1);
    }
    char vbuf[192];
    fb_settings_format_value(s, vbuf, sizeof(vbuf));
    out->print_fmt(out->user_ctx, FB_LOG_INFO, "%s.%s = %s", s->mod_id, s->setting_id, vbuf);
    return FB_OK;
}

static int cmd_trigger(int argc, const char* const* argv, const fb_console_writer_v1_t* out, void* user_ctx) {
    fb_console_t* con = (fb_console_t*)user_ctx;
    if (!con || !out || !out->print_fmt) {
        return FB_ERR_INTERNAL;
    }
    if (argc < 3) {
        out->print(out->user_ctx, FB_LOG_ERROR, "Usage: trigger <mod_id> <action_or_button_id>");
        return FB_ERR_INVALID_ARG;
    }
    const char* mod_id = argv[1];
    const char* target_id = argv[2];

    if (con->settings) {
        size_t sidx = 0u;
        if (fb_settings_find(con->settings, mod_id, target_id, &sidx)) {
            const fb_registered_setting_t* s = &con->settings->settings[sidx];
            if (s->type == (uint32_t)FB_SETTING_BUTTON && s->on_button) {
                s->on_button(s->setting_id, s->user_ctx);
                if (con->menu_ui) {
                    InterlockedExchange(&con->menu_ui->dirty, 1);
                }
                out->print_fmt(out->user_ctx, FB_LOG_INFO, "Triggered button %s.%s", s->mod_id, s->setting_id);
                return FB_OK;
            }
        }
    }
    if (con->bindings) {
        size_t aidx = 0u;
        if (fb_bindings_find(con->bindings, mod_id, target_id, &aidx)) {
            const fb_registered_action_t* a = &con->bindings->actions[aidx];
            if (a->callback) {
                a->callback(a->action_id, 1, a->user_ctx);
                if (a->trigger_mode == (uint32_t)FB_TRIGGER_HOLD) {
                    a->callback(a->action_id, 0, a->user_ctx);
                }
                if (con->menu_ui) {
                    InterlockedExchange(&con->menu_ui->dirty, 1);
                }
                out->print_fmt(out->user_ctx, FB_LOG_INFO, "Triggered action %s.%s", a->mod_id, a->action_id);
                return FB_OK;
            }
        }
    }

    out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Action or button '%s.%s' not found", mod_id, target_id);
    return FB_ERR_NOT_FOUND;
}

static int cmd_save(int argc, const char* const* argv, const fb_console_writer_v1_t* out, void* user_ctx) {
    (void)argc;
    (void)argv;
    fb_console_t* con = (fb_console_t*)user_ctx;
    if (!con || !con->settings || !con->bindings || !out || !out->print_fmt) {
        return FB_ERR_INTERNAL;
    }
    int rc = fb_user_config_save(con->settings, con->bindings, NULL);
    if (rc == FB_OK) {
        out->print(out->user_ctx, FB_LOG_INFO, "Saved fireball_user_config.json");
    } else {
        out->print_fmt(out->user_ctx, FB_LOG_ERROR, "Failed to save fireball_user_config.json (rc=%d)", rc);
    }
    return rc;
}

void fb_console_register_builtins(fb_console_t* con) {
    if (!con) {
        return;
    }
    const struct {
        const char*     name;
        const char*     usage;
        const char*     desc;
        fb_command_fn_t fn;
    } builtins[] = {
        { "help",    "help [command]",                         "List commands or show detailed usage",                      cmd_help },
        { "clear",   "clear",                                  "Clear the console scrollback buffer",                       cmd_clear },
        { "mods",    "mods",                                   "List host capabilities, loaded plugins, and skipped mods",  cmd_mods },
        { "stats",   "stats",                                  "Print live Fireball-Core atomic telemetry counters",        cmd_stats },
        { "get",     "get <mod_id> [setting_id]",              "Read one or all registered mod settings",                   cmd_get },
        { "set",     "set <mod_id> <setting_id> <value>",      "Update a registered BOOL/INT/FLOAT/ENUM setting",           cmd_set },
        { "trigger", "trigger <mod_id> <action_or_button_id>", "Fire a registered menu button or bound action",             cmd_trigger },
        { "save",    "save",                                   "Atomically persist fireball_user_config.json",              cmd_save }
    };

    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
        fb_command_desc_v1_t d;
        memset(&d, 0, sizeof(d));
        d.struct_size = (uint32_t)sizeof(d);
        d.mod_id = "fireball.core";
        d.name = builtins[i].name;
        d.usage = builtins[i].usage;
        d.description = builtins[i].desc;
        d.callback = builtins[i].fn;
        d.user_ctx = con;
        fb_console_register_command(con, &d);
    }
}

/* ---------------------------------------------------------------------------
 * Execution & history
 * --------------------------------------------------------------------------- */

static void fb_console_push_history(fb_console_t* con, const char* line) {
    if (!con || !line || !line[0]) {
        return;
    }
    if (con->history_count > 0u && strcmp(con->history[con->history_count - 1u], line) == 0) {
        return;
    }
    if (con->history_count == FB_CONSOLE_MAX_HISTORY) {
        memmove(con->history[0], con->history[1], (FB_CONSOLE_MAX_HISTORY - 1u) * FB_CONSOLE_INPUT_MAX);
        con->history_count = FB_CONSOLE_MAX_HISTORY - 1u;
    }
    snprintf(con->history[con->history_count++], FB_CONSOLE_INPUT_MAX, "%s", line);
}

int fb_console_exec(fb_console_t* con, const char* line, const fb_console_writer_v1_t* extra_writer) {
    if (!con || !line) {
        return FB_ERR_INVALID_ARG;
    }

    /* Skip leading whitespace */
    while (*line && isspace((unsigned char)*line)) {
        ++line;
    }
    if (*line == '\0') {
        return FB_OK;
    }

    con->scroll_offset = 0u;
    fb_console_push_history(con, line);

    {
        char echo[FB_CONSOLE_LINE_MAX];
        snprintf(echo, sizeof(echo), "> %s", line);
        fb_console_append_line(con, FB_LOG_DEBUG, echo);
    }

    fb_dispatch_writer_ctx_t dw;
    dw.con = con;
    dw.mirror = extra_writer;
    fb_console_writer_v1_t writer = fb_make_dispatch_writer(&dw);

    char scratch[1024];
    const char* argv[FB_CONSOLE_MAX_ARGS];
    int argc = 0;
    int trc = fb_console_tokenize(line, scratch, sizeof(scratch), argv, FB_CONSOLE_MAX_ARGS, &argc);
    if (trc != FB_OK) {
        writer.print(writer.user_ctx, FB_LOG_ERROR, "Parse error: unclosed quote or too many arguments");
        return trc;
    }
    if (argc <= 0) {
        return FB_OK;
    }

    size_t idx = 0u;
    if (!fb_console_find_command(con, argv[0], &idx)) {
        writer.print_fmt(writer.user_ctx, FB_LOG_ERROR, "Unknown command '%s' (type 'help')", argv[0]);
        return FB_ERR_NOT_FOUND;
    }

    fb_command_fn_t cb = con->commands[idx].callback;
    void* cb_ctx = con->commands[idx].user_ctx;
    return cb(argc, argv, &writer, cb_ctx);
}

/* ---------------------------------------------------------------------------
 * Prefix Tab Autocomplete
 * --------------------------------------------------------------------------- */

static int fb_starts_with_ci(const char* str, const char* prefix, size_t prefix_len) {
    if (!str) {
        return 0;
    }
    if (prefix_len == 0u) {
        return 1;
    }
    return _strnicmp(str, prefix, prefix_len) == 0;
}

static size_t fb_common_prefix_len_ci(const char* a, const char* b) {
    size_t n = 0u;
    while (a[n] && b[n] && tolower((unsigned char)a[n]) == tolower((unsigned char)b[n])) {
        ++n;
    }
    return n;
}

static void fb_console_autocomplete(fb_console_t* con) {
    if (!con) {
        return;
    }
    /* Find token start at or before caret */
    size_t end = con->caret;
    size_t start = end;
    while (start > 0u && !isspace((unsigned char)con->input[start - 1u])) {
        --start;
    }
    const char* prefix = con->input + start;
    size_t prefix_len = end - start;

    /* Determine token index (0 = command, 1 = arg1, 2 = arg2) and preceding tokens */
    char head[FB_CONSOLE_INPUT_MAX];
    size_t head_len = start < sizeof(head) ? start : (sizeof(head) - 1u);
    memcpy(head, con->input, head_len);
    head[head_len] = '\0';

    char scratch[512];
    const char* prev_argv[8];
    int prev_argc = 0;
    (void)fb_console_tokenize(head, scratch, sizeof(scratch), prev_argv, 8, &prev_argc);

    const char* matches[128];
    size_t match_count = 0u;

    if (prev_argc == 0 || (prev_argc == 1 && _stricmp(prev_argv[0], "help") == 0)) {
        for (size_t i = 0; i < con->command_count && match_count < 128u; ++i) {
            if (fb_starts_with_ci(con->commands[i].name, prefix, prefix_len)) {
                matches[match_count++] = con->commands[i].name;
            }
        }
    } else if (prev_argc == 1 &&
               (_stricmp(prev_argv[0], "get") == 0 ||
                _stricmp(prev_argv[0], "set") == 0 ||
                _stricmp(prev_argv[0], "trigger") == 0))
    {
        if (con->settings) {
            for (size_t i = 0; i < con->settings->mod_count && match_count < 128u; ++i) {
                const char* mid = con->settings->mods[i].mod_id;
                if (fb_starts_with_ci(mid, prefix, prefix_len)) {
                    matches[match_count++] = mid;
                }
            }
        }
        if (con->bindings) {
            for (size_t i = 0; i < con->bindings->action_count && match_count < 128u; ++i) {
                const char* mid = con->bindings->actions[i].mod_id;
                if (!fb_starts_with_ci(mid, prefix, prefix_len)) {
                    continue;
                }
                int dup = 0;
                for (size_t j = 0; j < match_count; ++j) {
                    if (_stricmp(matches[j], mid) == 0) {
                        dup = 1;
                        break;
                    }
                }
                if (!dup) {
                    matches[match_count++] = mid;
                }
            }
        }
    } else if (prev_argc == 2 && (_stricmp(prev_argv[0], "get") == 0 || _stricmp(prev_argv[0], "set") == 0)) {
        if (con->settings) {
            for (size_t i = 0; i < con->settings->setting_count && match_count < 128u; ++i) {
                const fb_registered_setting_t* s = &con->settings->settings[i];
                if (_stricmp(s->mod_id, prev_argv[1]) == 0 && fb_starts_with_ci(s->setting_id, prefix, prefix_len)) {
                    matches[match_count++] = s->setting_id;
                }
            }
        }
    } else if (prev_argc == 2 && _stricmp(prev_argv[0], "trigger") == 0) {
        if (con->settings) {
            for (size_t i = 0; i < con->settings->setting_count && match_count < 128u; ++i) {
                const fb_registered_setting_t* s = &con->settings->settings[i];
                if (s->type == (uint32_t)FB_SETTING_BUTTON &&
                    _stricmp(s->mod_id, prev_argv[1]) == 0 &&
                    fb_starts_with_ci(s->setting_id, prefix, prefix_len))
                {
                    matches[match_count++] = s->setting_id;
                }
            }
        }
        if (con->bindings) {
            for (size_t i = 0; i < con->bindings->action_count && match_count < 128u; ++i) {
                const fb_registered_action_t* a = &con->bindings->actions[i];
                if (_stricmp(a->mod_id, prev_argv[1]) == 0 && fb_starts_with_ci(a->action_id, prefix, prefix_len)) {
                    matches[match_count++] = a->action_id;
                }
            }
        }
    }

    if (match_count == 0u) {
        return;
    }

    /* Compute replacement string (full match + space if unique, else longest common prefix) */
    char replacement[FB_CONSOLE_INPUT_MAX];
    size_t rep_len = 0u;
    int append_space = 0;
    if (match_count == 1u) {
        rep_len = strlen(matches[0]);
        if (rep_len >= sizeof(replacement) - 2u) {
            rep_len = sizeof(replacement) - 2u;
        }
        memcpy(replacement, matches[0], rep_len);
        append_space = 1;
    } else {
        rep_len = strlen(matches[0]);
        for (size_t i = 1; i < match_count; ++i) {
            size_t c = fb_common_prefix_len_ci(matches[0], matches[i]);
            if (c < rep_len) {
                rep_len = c;
            }
        }
        if (rep_len >= sizeof(replacement) - 1u) {
            rep_len = sizeof(replacement) - 1u;
        }
        memcpy(replacement, matches[0], rep_len);
    }
    replacement[rep_len] = '\0';

    if (append_space && (end == con->input_len || !isspace((unsigned char)con->input[end]))) {
        replacement[rep_len++] = ' ';
        replacement[rep_len] = '\0';
    }

    /* Replace [start..end) with replacement if it extends or changes the token */
    int changed = (rep_len != prefix_len || _strnicmp(replacement, prefix, prefix_len) != 0);
    if (changed) {
        size_t tail_len = con->input_len - end;
        if (start + rep_len + tail_len < FB_CONSOLE_INPUT_MAX) {
            memmove(con->input + start + rep_len, con->input + end, tail_len + 1u);
            memcpy(con->input + start, replacement, rep_len);
            con->input_len = start + rep_len + tail_len;
            con->caret = start + rep_len;
            InterlockedExchange(&con->dirty, 1);
        }
    }

    if (match_count > 1u && !changed) {
        char list[FB_CONSOLE_LINE_MAX];
        size_t pos = 0u;
        list[0] = '\0';
        for (size_t i = 0; i < match_count; ++i) {
            int w = snprintf(list + pos, sizeof(list) - pos, "%s%s", (pos > 0u) ? "  " : "", matches[i]);
            if (w <= 0 || pos + (size_t)w >= sizeof(list)) {
                break;
            }
            pos += (size_t)w;
        }
        fb_console_append_line(con, FB_LOG_DEBUG, list);
    }
}

/* ---------------------------------------------------------------------------
 * Keyboard VK_* + Shift -> printable ASCII mapping
 * --------------------------------------------------------------------------- */

static char fb_vk_to_ascii(uint32_t vk, int shift) {
    if (vk >= 'A' && vk <= 'Z') {
        return (char)(shift ? vk : (vk + ('a' - 'A')));
    }
    if (vk >= '0' && vk <= '9') {
        static const char k_shifted_digits[10] = { ')', '!', '@', '#', '$', '%', '^', '&', '*', '(' };
        return shift ? k_shifted_digits[vk - '0'] : (char)vk;
    }
    if (vk >= 0x60u && vk <= 0x69u) { /* VK_NUMPAD0..9 */
        return (char)('0' + (vk - 0x60u));
    }
    switch (vk) {
    case 0x20u: return ' ';                    /* VK_SPACE */
    case 0x6Au: return '*';                    /* VK_MULTIPLY */
    case 0x6Bu: return '+';                    /* VK_ADD */
    case 0x6Du: return '-';                    /* VK_SUBTRACT */
    case 0x6Eu: return '.';                    /* VK_DECIMAL */
    case 0x6Fu: return '/';                    /* VK_DIVIDE */
    case 0xBDu: return shift ? '_' : '-';      /* VK_OEM_MINUS */
    case 0xBBu: return shift ? '+' : '=';      /* VK_OEM_PLUS */
    case 0xDBu: return shift ? '{' : '[';      /* VK_OEM_4 */
    case 0xDDu: return shift ? '}' : ']';      /* VK_OEM_6 */
    case 0xDCu: return shift ? '|' : '\\';     /* VK_OEM_5 */
    case 0xBAu: return shift ? ':' : ';';      /* VK_OEM_1 */
    case 0xDEu: return shift ? '"' : '\'';     /* VK_OEM_7 */
    case 0xBCu: return shift ? '<' : ',';      /* VK_OEM_COMMA */
    case 0xBEu: return shift ? '>' : '.';      /* VK_OEM_PERIOD */
    case 0xBFu: return shift ? '?' : '/';      /* VK_OEM_2 */
    case 0xC0u: return shift ? '~' : '`';      /* VK_OEM_3 */
    default:    return '\0';
    }
}

static void fb_console_insert_char(fb_console_t* con, char ch) {
    if (!con || ch < 0x20 || ch > 0x7E) {
        return;
    }
    if (con->input_len + 1u >= FB_CONSOLE_INPUT_MAX) {
        return;
    }
    memmove(con->input + con->caret + 1u, con->input + con->caret, con->input_len - con->caret + 1u);
    con->input[con->caret] = ch;
    con->caret += 1u;
    con->input_len += 1u;
    InterlockedExchange(&con->dirty, 1);
}

static void fb_console_paste_clipboard(fb_console_t* con) {
    if (!con || con->headless) {
        return;
    }
    if (!OpenClipboard(NULL)) {
        return;
    }
    HANDLE hData = GetClipboardData(CF_TEXT);
    if (hData) {
        const char* clip = (const char*)GlobalLock(hData);
        if (clip) {
            for (const char* p = clip; *p; ++p) {
                if (*p == '\r' || *p == '\n') {
                    break;
                }
                if ((unsigned char)*p >= 0x20u && (unsigned char)*p <= 0x7Eu) {
                    fb_console_insert_char(con, *p);
                }
            }
            GlobalUnlock(hData);
        }
    }
    CloseClipboard();
}

/* ---------------------------------------------------------------------------
 * Win32 GDI Overlay Window (WS_EX_NOACTIVATE — never steals GameInput focus)
 * --------------------------------------------------------------------------- */

static void fb_con_fill_rect(HDC dc, int l, int t, int r, int b, COLORREF col) {
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(col);
    FillRect(dc, &rc, br);
    DeleteObject(br);
}

static void fb_con_frame_rect(HDC dc, int l, int t, int r, int b, COLORREF col) {
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(col);
    FrameRect(dc, &rc, br);
    DeleteObject(br);
}

static void fb_con_draw_text(HDC dc, const char* utf8, int l, int t, int r, int b, COLORREF col, UINT fmt) {
    if (!utf8 || !utf8[0]) {
        return;
    }
    wchar_t wbuf[512];
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, (int)(sizeof(wbuf) / sizeof(wbuf[0])));
    if (wn <= 0) {
        return;
    }
    RECT rc = { l, t, r, b };
    SetTextColor(dc, col);
    SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, wbuf, -1, &rc, fmt | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

static void fb_console_paint(fb_console_t* con, HDC hdc) {
    int W = con->width;
    int H = con->height;

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
    HGDIOBJ old_bmp = SelectObject(mem, bmp);

    HFONT f_hdr = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT f_mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               FIXED_PITCH | FF_MODERN, L"Consolas");
    HFONT f_small = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    fb_con_fill_rect(mem, 0, 0, W, H, CON_COL_BG);
    fb_con_frame_rect(mem, 0, 0, W, H, CON_COL_BORDER);

    /* Header bar */
    fb_con_fill_rect(mem, 1, 1, W - 1, FB_CON_HEADER_H, CON_COL_SURFACE);
    fb_con_fill_rect(mem, 1, FB_CON_HEADER_H - 2, W - 1, FB_CON_HEADER_H, CON_COL_ACCENT);
    HGDIOBJ old_font = SelectObject(mem, f_hdr);
    fb_con_draw_text(mem, "FIREBALL CONSOLE", FB_CON_PAD_X, 6, 260, FB_CON_HEADER_H - 4, CON_COL_ACCENT, DT_LEFT | DT_VCENTER);
    SelectObject(mem, f_small);
    fb_con_draw_text(
        mem,
        "Tab autocomplete  |  Up/Down history  |  PgUp/PgDn scroll  |  ~ or Esc close",
        260, 6, W - FB_CON_PAD_X, FB_CON_HEADER_H - 4, CON_COL_MUTED, DT_RIGHT | DT_VCENTER);

    /* Scrollback area */
    int log_top = FB_CON_HEADER_H + 6;
    int log_bot = H - FB_CON_INPUT_H - 6;
    int visible = (log_bot - log_top) / FB_CON_LINE_H;
    if (visible < 1) {
        visible = 1;
    }

    if (con->lock) {
        EnterCriticalSection(con->lock);
    }

    SelectObject(mem, f_mono);
    size_t total = con->line_count;
    size_t max_scroll = (total > (size_t)visible) ? (total - (size_t)visible) : 0u;
    if (con->scroll_offset > max_scroll) {
        con->scroll_offset = max_scroll;
    }
    size_t end_idx = total - con->scroll_offset;
    size_t start_idx = (end_idx > (size_t)visible) ? (end_idx - (size_t)visible) : 0u;

    int y = log_top;
    for (size_t i = start_idx; i < end_idx && y + FB_CON_LINE_H <= log_bot; ++i) {
        const fb_console_line_t* ln = fb_console_line_at(con, i);
        if (!ln) {
            continue;
        }
        COLORREF c = CON_COL_TEXT;
        if (ln->level == FB_LOG_ERROR) {
            c = CON_COL_RED;
        } else if (ln->level == FB_LOG_WARN) {
            c = CON_COL_AMBER;
        } else if (ln->level <= FB_LOG_DEBUG) {
            c = (ln->text[0] == '>') ? CON_COL_GREEN : CON_COL_MUTED;
        }
        fb_con_draw_text(mem, ln->text, FB_CON_PAD_X, y, W - FB_CON_PAD_X, y + FB_CON_LINE_H, c, DT_LEFT | DT_VCENTER);
        y += FB_CON_LINE_H;
    }

    /* Input prompt bar */
    int inp_top = H - FB_CON_INPUT_H;
    fb_con_fill_rect(mem, 1, inp_top, W - 1, H - 1, CON_COL_SURFACE);
    fb_con_fill_rect(mem, 1, inp_top, W - 1, inp_top + 1, CON_COL_BORDER);

    char prompt_line[FB_CONSOLE_INPUT_MAX + 8];
    snprintf(prompt_line, sizeof(prompt_line), "> %s", con->input);
    fb_con_draw_text(
        mem,
        prompt_line,
        FB_CON_PAD_X, inp_top + 4, W - FB_CON_PAD_X, H - 4,
        CON_COL_TEXT, DT_LEFT | DT_VCENTER);

    /* Caret bar */
    {
        wchar_t prefix_w[FB_CONSOLE_INPUT_MAX + 8];
        char prefix_u8[FB_CONSOLE_INPUT_MAX + 8];
        size_t clen = con->caret <= con->input_len ? con->caret : con->input_len;
        snprintf(prefix_u8, sizeof(prefix_u8), "> %.*s", (int)clen, con->input);
        int wn = MultiByteToWideChar(CP_UTF8, 0, prefix_u8, -1, prefix_w, (int)(sizeof(prefix_w) / sizeof(prefix_w[0])));
        if (wn > 1) {
            SIZE sz = { 0, 0 };
            GetTextExtentPoint32W(mem, prefix_w, wn - 1, &sz);
            int cx = FB_CON_PAD_X + sz.cx;
            if (cx < W - FB_CON_PAD_X) {
                fb_con_fill_rect(mem, cx, inp_top + 8, cx + 2, H - 8, CON_COL_ACCENT);
            }
        }
    }

    if (con->lock) {
        LeaveCriticalSection(con->lock);
    }

    BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);

    SelectObject(mem, old_font);
    SelectObject(mem, old_bmp);
    DeleteObject(f_hdr);
    DeleteObject(f_mono);
    DeleteObject(f_small);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK fb_console_wndproc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    fb_console_t* con = (fb_console_t*)(uintptr_t)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (msg) {
    case WM_CREATE: {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)(uintptr_t)cs->lpCreateParams);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (con) {
            fb_console_paint(con, hdc);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_FB_CON_SYNC:
        if (con) {
            if (con->is_open) {
                int sx = GetSystemMetrics(SM_CXSCREEN);
                int x = (sx > con->width) ? (sx - con->width) / 2 : 20;
                int y = 24;
                SetWindowPos(hwnd, HWND_TOPMOST, x, y, con->width, con->height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                InvalidateRect(hwnd, NULL, FALSE);
            } else {
                ShowWindow(hwnd, SW_HIDE);
            }
        }
        return 0;
    case WM_TIMER:
        if (con && con->is_open && InterlockedCompareExchange(&con->dirty, 0, 1) == 1) {
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_FB_CON_QUIT:
        KillTimer(hwnd, FB_CON_TIMER_ID);
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

static DWORD WINAPI fb_console_ui_thread_main(LPVOID param) {
    fb_console_t* con = (fb_console_t*)param;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = fb_console_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = FB_CONSOLE_WND_CLASS;
    RegisterClassExW(&wc);

    int sx = GetSystemMetrics(SM_CXSCREEN);
    int x = (sx > con->width) ? (sx - con->width) / 2 : 20;
    int y = 24;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        FB_CONSOLE_WND_CLASS,
        L"Fireball Console",
        WS_POPUP,
        x, y, con->width, con->height,
        NULL, NULL, wc.hInstance, con);

    con->hwnd = hwnd;
    if (hwnd) {
        SetLayeredWindowAttributes(hwnd, 0, 238, LWA_ALPHA);
        SetTimer(hwnd, FB_CON_TIMER_ID, 33, NULL);
        if (con->is_open) {
            ShowWindow(hwnd, SW_SHOWNOACTIVATE);
            InvalidateRect(hwnd, NULL, FALSE);
        }
    }
    InterlockedExchange(&con->ui_ready, 1);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    UnregisterClassW(FB_CONSOLE_WND_CLASS, wc.hInstance);
    con->hwnd = NULL;
    return 0;
}

static void fb_console_sync_window(fb_console_t* con) {
    if (!con || con->headless) {
        return;
    }
    if (!con->ui_thread && con->is_open) {
        con->ui_thread = CreateThread(NULL, 0, fb_console_ui_thread_main, con, 0, &con->ui_thread_id);
        return;
    }
    if (con->hwnd) {
        PostMessageW(con->hwnd, WM_FB_CON_SYNC, 0, 0);
    }
}

/* ---------------------------------------------------------------------------
 * Public lifecycle & input API
 * --------------------------------------------------------------------------- */

void fb_console_init(
    fb_console_t*                   con,
    CRITICAL_SECTION*               lock,
    fb_settings_manager_t*          settings,
    fb_binding_manager_t*           bindings,
    fb_menu_ui_t*                   menu_ui,
    const fb_menu_telemetry_ptrs_t* telemetry)
{
    if (!con) {
        return;
    }
    memset(con, 0, sizeof(*con));
    con->lock = lock;
    con->settings = settings;
    con->bindings = bindings;
    con->menu_ui = menu_ui;
    if (telemetry) {
        con->telemetry = *telemetry;
    }
    con->history_pos = -1;
    con->width = FB_CON_WIDTH;
    con->height = FB_CON_HEIGHT;
    con->headless = fb_con_env_is_headless();
    fb_console_register_builtins(con);
    fb_console_append_line(con, FB_LOG_INFO, "Fireball Console ready. Type 'help' for commands.");
}

void fb_console_set_host_snapshot(
    fb_console_t*              con,
    const fb_host_api_v1_t*    host_api,
    const fb_plugin_manager_t* plugin_mgr)
{
    if (!con) {
        return;
    }
    if (con->lock) {
        EnterCriticalSection(con->lock);
    }
    free(con->loaded_plugins);
    free(con->skipped_plugins);
    con->loaded_plugins = NULL;
    con->loaded_count = 0u;
    con->skipped_plugins = NULL;
    con->skipped_count = 0u;

    if (host_api) {
        snprintf(con->host_name, sizeof(con->host_name), "%s", host_api->host_name ? host_api->host_name : "unknown");
        snprintf(con->host_version, sizeof(con->host_version), "%s", host_api->host_version ? host_api->host_version : "0.0.0");
        fb_format_caps(host_api->capabilities, con->host_caps_str, sizeof(con->host_caps_str));
    }
    if (plugin_mgr && plugin_mgr->plugin_count > 0u) {
        con->loaded_plugins = (fb_menu_plugin_snapshot_t*)calloc(
            plugin_mgr->plugin_count,
            sizeof(fb_menu_plugin_snapshot_t));
        if (con->loaded_plugins) {
            con->loaded_count = plugin_mgr->plugin_count;
            for (size_t i = 0; i < plugin_mgr->plugin_count; ++i) {
                snprintf(con->loaded_plugins[i].mod_id, sizeof(con->loaded_plugins[i].mod_id), "%s", plugin_mgr->plugins[i].mod_id);
                snprintf(con->loaded_plugins[i].plugin_id, sizeof(con->loaded_plugins[i].plugin_id), "%s", plugin_mgr->plugins[i].plugin_id);
                snprintf(con->loaded_plugins[i].version, sizeof(con->loaded_plugins[i].version), "%s", plugin_mgr->plugins[i].version);
                con->loaded_plugins[i].has_menu_api = plugin_mgr->plugins[i].has_menu_api;
                con->loaded_plugins[i].has_console_api = plugin_mgr->plugins[i].has_console_api;
            }
        }
    }
    if (plugin_mgr && plugin_mgr->skipped_count > 0u) {
        con->skipped_plugins = (fb_menu_skipped_snapshot_t*)calloc(
            plugin_mgr->skipped_count,
            sizeof(fb_menu_skipped_snapshot_t));
        if (con->skipped_plugins) {
            con->skipped_count = plugin_mgr->skipped_count;
            for (size_t i = 0; i < plugin_mgr->skipped_count; ++i) {
                snprintf(con->skipped_plugins[i].mod_id, sizeof(con->skipped_plugins[i].mod_id), "%s", plugin_mgr->skipped[i].mod_id);
                snprintf(con->skipped_plugins[i].plugin_id, sizeof(con->skipped_plugins[i].plugin_id), "%s", plugin_mgr->skipped[i].plugin_id);
                snprintf(con->skipped_plugins[i].version, sizeof(con->skipped_plugins[i].version), "%s", plugin_mgr->skipped[i].version);
                snprintf(con->skipped_plugins[i].reason, sizeof(con->skipped_plugins[i].reason), "%s", plugin_mgr->skipped[i].reason);
            }
        }
    }
    if (con->lock) {
        LeaveCriticalSection(con->lock);
    }
}

void fb_console_set_open(fb_console_t* con, int open) {
    if (!con) {
        return;
    }
    con->is_open = open ? 1 : 0;
    if (con->is_open && con->menu_ui && fb_menu_ui_is_open(con->menu_ui)) {
        fb_menu_ui_set_open(con->menu_ui, 0);
    }
    InterlockedExchange(&con->dirty, 1);
    fb_console_sync_window(con);
}

void fb_console_toggle(fb_console_t* con) {
    if (!con) {
        return;
    }
    fb_console_set_open(con, con->is_open ? 0 : 1);
}

int fb_console_is_open(const fb_console_t* con) {
    return con ? con->is_open : 0;
}

int fb_console_handle_input(fb_console_t* con, const fb_input_event_v1_t* ev) {
    if (!con || !ev || !con->is_open) {
        return 0;
    }

    /* Console toggle key closes the console cleanly */
    if (con->bindings && fb_bindings_matches_action_press(con->bindings, "fireball.core", "toggle_console", ev)) {
        fb_console_set_open(con, 0);
        return 1;
    }

    /* Pressing the Mod Menu toggle switches directly from console to Mod Menu */
    if (con->bindings && fb_bindings_matches_action_press(con->bindings, "fireball.core", "toggle_menu", ev)) {
        fb_console_set_open(con, 0);
        if (con->menu_ui) {
            fb_menu_ui_set_open(con->menu_ui, 1);
        }
        return 1;
    }

    /* While console is open, consume ALL keyboard events so typing never leaks into gameplay */
    if (ev->device_kind != FB_DEV_KEYBOARD) {
        return 0;
    }

    int is_press = (ev->action == (uint32_t)FB_INPUT_PRESS);
    int is_press_or_rep = (ev->action == (uint32_t)FB_INPUT_PRESS || ev->action == (uint32_t)FB_INPUT_REPEAT);
    if (!is_press_or_rep) {
        return 1; /* Consume key releases too */
    }

    uint32_t mods = ev->modifiers | (con->bindings ? con->bindings->held_kbd_mods : 0u);
    int shift = ((mods & FB_MOD_SHIFT) != 0u);
    int ctrl  = ((mods & FB_MOD_CTRL) != 0u);
    int alt   = ((mods & FB_MOD_ALT) != 0u);

    if (ev->code == 0x1Bu && is_press) { /* VK_ESCAPE */
        fb_console_set_open(con, 0);
        return 1;
    }
    if (ev->code == 0x0Du && is_press) { /* VK_RETURN */
        char cmd_copy[FB_CONSOLE_INPUT_MAX];
        snprintf(cmd_copy, sizeof(cmd_copy), "%s", con->input);
        con->input[0] = '\0';
        con->input_len = 0u;
        con->caret = 0u;
        con->history_pos = -1;
        con->draft[0] = '\0';
        if (cmd_copy[0] != '\0') {
            (void)fb_console_exec(con, cmd_copy, NULL);
        }
        InterlockedExchange(&con->dirty, 1);
        return 1;
    }
    if (ev->code == 0x08u) { /* VK_BACK */
        if (con->caret > 0u) {
            memmove(con->input + con->caret - 1u, con->input + con->caret, con->input_len - con->caret + 1u);
            con->caret -= 1u;
            con->input_len -= 1u;
            InterlockedExchange(&con->dirty, 1);
        }
        return 1;
    }
    if (ev->code == 0x2Eu) { /* VK_DELETE */
        if (con->caret < con->input_len) {
            memmove(con->input + con->caret, con->input + con->caret + 1u, con->input_len - con->caret);
            con->input_len -= 1u;
            InterlockedExchange(&con->dirty, 1);
        }
        return 1;
    }
    if (ev->code == 0x25u) { /* VK_LEFT */
        if (con->caret > 0u) {
            con->caret -= 1u;
            InterlockedExchange(&con->dirty, 1);
        }
        return 1;
    }
    if (ev->code == 0x27u) { /* VK_RIGHT */
        if (con->caret < con->input_len) {
            con->caret += 1u;
            InterlockedExchange(&con->dirty, 1);
        }
        return 1;
    }
    if (ev->code == 0x24u && is_press) { /* VK_HOME */
        con->caret = 0u;
        InterlockedExchange(&con->dirty, 1);
        return 1;
    }
    if (ev->code == 0x23u && is_press) { /* VK_END */
        con->caret = con->input_len;
        InterlockedExchange(&con->dirty, 1);
        return 1;
    }
    if (ev->code == 0x26u) { /* VK_UP: older history */
        if (con->history_count > 0u) {
            if (con->history_pos < 0) {
                snprintf(con->draft, sizeof(con->draft), "%s", con->input);
                con->history_pos = (int)con->history_count - 1;
            } else if (con->history_pos > 0) {
                con->history_pos -= 1;
            }
            snprintf(con->input, sizeof(con->input), "%s", con->history[con->history_pos]);
            con->input_len = strlen(con->input);
            con->caret = con->input_len;
            InterlockedExchange(&con->dirty, 1);
        }
        return 1;
    }
    if (ev->code == 0x28u) { /* VK_DOWN: newer history */
        if (con->history_pos >= 0) {
            if ((size_t)(con->history_pos + 1) < con->history_count) {
                con->history_pos += 1;
                snprintf(con->input, sizeof(con->input), "%s", con->history[con->history_pos]);
            } else {
                con->history_pos = -1;
                snprintf(con->input, sizeof(con->input), "%s", con->draft);
            }
            con->input_len = strlen(con->input);
            con->caret = con->input_len;
            InterlockedExchange(&con->dirty, 1);
        }
        return 1;
    }
    if (ev->code == 0x09u && is_press) { /* VK_TAB */
        fb_console_autocomplete(con);
        return 1;
    }
    if (ev->code == 0x21u) { /* VK_PRIOR (PgUp) */
        con->scroll_offset += 5u;
        if (con->scroll_offset > con->line_count) {
            con->scroll_offset = con->line_count;
        }
        InterlockedExchange(&con->dirty, 1);
        return 1;
    }
    if (ev->code == 0x22u) { /* VK_NEXT (PgDn) */
        con->scroll_offset = (con->scroll_offset > 5u) ? (con->scroll_offset - 5u) : 0u;
        InterlockedExchange(&con->dirty, 1);
        return 1;
    }

    if (ctrl && !alt) {
        if (ev->code == 'V' && is_press) {
            fb_console_paste_clipboard(con);
            return 1;
        }
        if (ev->code == 'L' && is_press) {
            fb_console_clear(con);
            return 1;
        }
        if (ev->code == 'C' && is_press) {
            con->input[0] = '\0';
            con->input_len = 0u;
            con->caret = 0u;
            con->history_pos = -1;
            InterlockedExchange(&con->dirty, 1);
            return 1;
        }
        return 1;
    }

    if (!alt) {
        char ch = fb_vk_to_ascii(ev->code, shift);
        if (ch != '\0') {
            fb_console_insert_char(con, ch);
        }
    }
    return 1;
}

void fb_console_tick(fb_console_t* con, double dt_sec) {
    (void)dt_sec;
    if (!con) {
        return;
    }
    if (con->is_open && con->hwnd && InterlockedCompareExchange(&con->dirty, 0, 1) == 1) {
        InvalidateRect(con->hwnd, NULL, FALSE);
    }
}

/* ---------------------------------------------------------------------------
 * Optional Local Named-Pipe Transport (`\\.\pipe\fireball_console`)
 * --------------------------------------------------------------------------- */

typedef struct fb_pipe_reply_buf {
    char*  data;
    size_t len;
    size_t cap;
} fb_pipe_reply_buf_t;

static void fb_pipe_buf_append(fb_pipe_reply_buf_t* buf, const char* s, size_t n) {
    if (!buf || !s || n == 0u) {
        return;
    }
    if (buf->len + n + 1u > buf->cap) {
        size_t new_cap = (buf->cap == 0u) ? 512u : (buf->cap * 2u);
        while (new_cap < buf->len + n + 1u) {
            new_cap *= 2u;
        }
        char* grown = (char*)realloc(buf->data, new_cap);
        if (!grown) {
            return;
        }
        buf->data = grown;
        buf->cap = new_cap;
    }
    memcpy(buf->data + buf->len, s, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
}

static void fb_pipe_writer_print(void* user_ctx, int level, const char* text) {
    fb_pipe_reply_buf_t* buf = (fb_pipe_reply_buf_t*)user_ctx;
    if (!buf || !text) {
        return;
    }
    const char* pfx = "";
    if (level == FB_LOG_ERROR) {
        pfx = "[ERROR] ";
    } else if (level == FB_LOG_WARN) {
        pfx = "[WARN] ";
    }
    if (pfx[0]) {
        fb_pipe_buf_append(buf, pfx, strlen(pfx));
    }
    fb_pipe_buf_append(buf, text, strlen(text));
    fb_pipe_buf_append(buf, "\n", 1u);
}

static void fb_pipe_writer_print_fmt(void* user_ctx, int level, const char* fmt, ...) {
    if (!fmt) {
        return;
    }
    char tmp[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    fb_pipe_writer_print(user_ctx, level, tmp);
}

static int fb_pipe_wait_ov(HANDLE pipe, OVERLAPPED* ov, HANDLE stop_ev, DWORD* out_bytes) {
    HANDLE waits[2] = { stop_ev, ov->hEvent };
    DWORD w = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    if (w == WAIT_OBJECT_0) {
        CancelIo(pipe);
        return 0; /* Stopping */
    }
    if (w != WAIT_OBJECT_0 + 1) {
        CancelIo(pipe);
        return -1;
    }
    DWORD transferred = 0;
    if (!GetOverlappedResult(pipe, ov, &transferred, FALSE)) {
        return -1;
    }
    if (out_bytes) {
        *out_bytes = transferred;
    }
    return 1;
}

static int fb_pipe_write_all(HANDLE pipe, HANDLE io_ev, HANDLE stop_ev, const char* data, size_t len) {
    size_t sent = 0u;
    while (sent < len) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = io_ev;
        ResetEvent(io_ev);
        DWORD chunk = (len - sent > 4096u) ? 4096u : (DWORD)(len - sent);
        DWORD written = 0;
        BOOL ok = WriteFile(pipe, data + sent, chunk, &written, &ov);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                int wr = fb_pipe_wait_ov(pipe, &ov, stop_ev, &written);
                if (wr <= 0) {
                    return wr;
                }
            } else {
                return -1;
            }
        }
        sent += (size_t)written;
    }
    return 1;
}

static DWORD WINAPI fb_console_pipe_thread_main(LPVOID param) {
    fb_console_t* con = (fb_console_t*)param;
    HANDLE io_ev = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!io_ev) {
        return 0;
    }

    while (WaitForSingleObject(con->pipe_stop_event, 0) == WAIT_TIMEOUT) {
        HANDLE pipe = CreateNamedPipeW(
            con->pipe_name,
            PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1,
            8192,
            4096,
            0,
            NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            /* Pipe name may already be in use by another instance; back off and check stop */
            if (WaitForSingleObject(con->pipe_stop_event, 250) == WAIT_OBJECT_0) {
                break;
            }
            continue;
        }

        OVERLAPPED ov;
        memset(&ov, 0, sizeof(ov));
        ov.hEvent = io_ev;
        ResetEvent(io_ev);

        BOOL connected = ConnectNamedPipe(pipe, &ov);
        if (!connected) {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                int wr = fb_pipe_wait_ov(pipe, &ov, con->pipe_stop_event, NULL);
                if (wr <= 0) {
                    CloseHandle(pipe);
                    if (wr == 0) {
                        break;
                    }
                    continue;
                }
            } else if (err != ERROR_PIPE_CONNECTED) {
                CloseHandle(pipe);
                continue;
            }
        }

        /* Read newline-delimited commands from client until disconnect or stop */
        char line_buf[1024];
        size_t line_len = 0u;
        char chunk[256];
        int client_alive = 1;

        while (client_alive && WaitForSingleObject(con->pipe_stop_event, 0) == WAIT_TIMEOUT) {
            memset(&ov, 0, sizeof(ov));
            ov.hEvent = io_ev;
            ResetEvent(io_ev);
            DWORD got = 0;
            BOOL rok = ReadFile(pipe, chunk, (DWORD)sizeof(chunk), &got, &ov);
            if (!rok) {
                DWORD err = GetLastError();
                if (err == ERROR_IO_PENDING) {
                    int wr = fb_pipe_wait_ov(pipe, &ov, con->pipe_stop_event, &got);
                    if (wr <= 0) {
                        client_alive = 0;
                        break;
                    }
                } else {
                    client_alive = 0;
                    break;
                }
            }
            if (got == 0) {
                client_alive = 0;
                break;
            }

            for (DWORD i = 0; i < got && client_alive; ++i) {
                char c = chunk[i];
                if (c == '\r') {
                    continue;
                }
                if (c == '\n') {
                    line_buf[line_len] = '\0';
                    fb_pipe_reply_buf_t reply;
                    memset(&reply, 0, sizeof(reply));

                    fb_console_writer_v1_t pw;
                    memset(&pw, 0, sizeof(pw));
                    pw.struct_size = (uint32_t)sizeof(pw);
                    pw.user_ctx = &reply;
                    pw.print = fb_pipe_writer_print;
                    pw.print_fmt = fb_pipe_writer_print_fmt;

                    int rc = FB_OK;
                    if (con->pipe_exec_fn) {
                        rc = con->pipe_exec_fn(con->pipe_exec_ctx, line_buf, &pw);
                    }
                    char eot[32];
                    int en = snprintf(eot, sizeof(eot), "\x04%d\n", rc);
                    if (en > 0) {
                        fb_pipe_buf_append(&reply, eot, (size_t)en);
                    }
                    if (reply.data && reply.len > 0u) {
                        if (fb_pipe_write_all(pipe, io_ev, con->pipe_stop_event, reply.data, reply.len) <= 0) {
                            client_alive = 0;
                        }
                    }
                    free(reply.data);
                    line_len = 0u;
                } else if (line_len + 1u < sizeof(line_buf)) {
                    line_buf[line_len++] = c;
                }
            }
        }

        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }

    CloseHandle(io_ev);
    return 0;
}

void fb_console_start_pipe(fb_console_t* con, fb_console_pipe_exec_fn_t exec_fn, void* exec_ctx) {
    if (!con || con->pipe_thread) {
        return;
    }
    if (!fb_con_resolve_pipe_name(con->headless, con->pipe_name, sizeof(con->pipe_name) / sizeof(con->pipe_name[0]))) {
        return;
    }
    con->pipe_exec_fn = exec_fn;
    con->pipe_exec_ctx = exec_ctx;
    con->pipe_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!con->pipe_stop_event) {
        return;
    }
    con->pipe_thread = CreateThread(NULL, 0, fb_console_pipe_thread_main, con, 0, NULL);
    if (!con->pipe_thread) {
        CloseHandle(con->pipe_stop_event);
        con->pipe_stop_event = NULL;
    }
}

void fb_console_stop_pipe(fb_console_t* con) {
    if (!con) {
        return;
    }
    if (con->pipe_stop_event) {
        SetEvent(con->pipe_stop_event);
    }
    if (con->pipe_thread) {
        WaitForSingleObject(con->pipe_thread, 2000);
        CloseHandle(con->pipe_thread);
        con->pipe_thread = NULL;
    }
    if (con->pipe_stop_event) {
        CloseHandle(con->pipe_stop_event);
        con->pipe_stop_event = NULL;
    }
}

void fb_console_shutdown(fb_console_t* con) {
    if (!con) {
        return;
    }
    fb_console_stop_pipe(con);
    con->is_open = 0;
    if (con->ui_thread) {
        for (int i = 0; i < 100 && !InterlockedCompareExchange(&con->ui_ready, 0, 0); ++i) {
            Sleep(10);
        }
        if (con->hwnd) {
            PostMessageW(con->hwnd, WM_FB_CON_QUIT, 0, 0);
        }
        WaitForSingleObject(con->ui_thread, 2000);
        CloseHandle(con->ui_thread);
        con->ui_thread = NULL;
        con->hwnd = NULL;
        InterlockedExchange(&con->ui_ready, 0);
    }
    free(con->commands);
    free(con->loaded_plugins);
    free(con->skipped_plugins);
    con->commands = NULL;
    con->command_count = 0u;
    con->command_cap = 0u;
    con->loaded_plugins = NULL;
    con->loaded_count = 0u;
    con->skipped_plugins = NULL;
    con->skipped_count = 0u;
}
