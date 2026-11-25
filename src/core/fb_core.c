/*
 * fb_core.c — Native Fireball-Core runtime (`fireball_core.dll`).
 *
 * Pure C11 + Win32 API (zero third-party dependencies).
 * Exports `fb_attach_host()` per `include/fireball_host.h` (FB_HOST_ABI_VERSION = 1).
 * Provides built-in Mod Menu, Typed Settings Registry, and Keyboard + Gamepad Action
 * Binding Manager to native plugins via `include/fireball_menu.h`.
 */

#include "fireball_host.h"
#include "fireball_plugin.h"
#include "fireball_menu.h"
#include "fireball_console.h"
#include "fb_bundle.h"
#include "fb_plugins.h"
#include "fb_bindings.h"
#include "fb_settings.h"
#include "fb_menu_ui.h"
#include "fb_console.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fb_overlay_handle {
    const uint8_t* data;
    uint64_t       size;
} fb_overlay_handle_t;

typedef struct fb_core_state {
    int                   attached;
    SRWLOCK               lock;
    CRITICAL_SECTION      menu_lock;
    int                   menu_lock_init;
    fb_host_api_v1_t      host_api;
    char                  host_name_copy[128];
    char                  host_version_copy[64];
    char                  game_root_utf8[512];
    size_t                game_root_utf8_len;
    fb_bundle_t           bundle;
    int                   has_bundle;
    fb_plugin_manager_t   plugin_mgr;
    fb_binding_manager_t  bindings;
    fb_settings_manager_t settings;
    fb_menu_ui_t          menu_ui;
    fb_console_t          console;

    /* Per-entry first-hit flags (log once per target without spamming hot loops) */
    volatile LONG*        ds_first_hit;
    volatile LONG*        ov_first_hit;

    /* Lock-free atomic live boot telemetry counters */
    volatile LONG64       dstorage_calls;
    volatile LONG64       dstorage_hits;
    volatile LONG64       dstorage_buf_too_small;
    volatile LONG64       file_open_calls;
    volatile LONG64       file_open_hits;
    volatile LONG64       input_events;
    volatile LONG64       input_consumed;
    volatile LONG64       frames_seen;
} fb_core_state_t;

static fb_core_state_t g_core = {
    .attached = 0,
    .lock = SRWLOCK_INIT
};

static void core_log(int level, const char* subsystem, const char* msg) {
    if (g_core.host_api.log) {
        g_core.host_api.log(level, subsystem ? subsystem : "fireball-core", msg ? msg : "");
    }
}

static void fb_core_toggle_menu_action(const char* action_id, int active, void* user_ctx) {
    (void)action_id;
    (void)user_ctx;
    if (active) {
        if (!fb_menu_ui_is_open(&g_core.menu_ui) && fb_console_is_open(&g_core.console)) {
            fb_console_set_open(&g_core.console, 0);
        }
        fb_menu_ui_toggle(&g_core.menu_ui);
    }
}

static void fb_core_toggle_console_action(const char* action_id, int active, void* user_ctx) {
    (void)action_id;
    (void)user_ctx;
    if (active) {
        fb_console_toggle(&g_core.console);
    }
}

/* ---------------------------------------------------------------------------
 * Virtualized loose-file overlay handle callbacks (`on_file_open`)
 * --------------------------------------------------------------------------- */

static int64_t fb_overlay_read_at(void* user_ctx, uint64_t offset, void* dst, uint64_t len) {
    fb_overlay_handle_t* h = (fb_overlay_handle_t*)user_ctx;
    if (!h) {
        return (int64_t)FB_ERR_INVALID_ARG;
    }
    if (len == 0) {
        return 0;
    }
    if (!dst) {
        return (int64_t)FB_ERR_INVALID_ARG;
    }
    if (offset >= h->size) {
        return 0;
    }
    uint64_t avail = h->size - offset;
    uint64_t to_copy = (len < avail) ? len : avail;
    memcpy(dst, h->data + offset, (size_t)to_copy);
    return (int64_t)to_copy;
}

static void fb_overlay_close(void* user_ctx) {
    free(user_ctx);
}

/* ---------------------------------------------------------------------------
 * Host callbacks exposed via `fb_modcore_callbacks_v1_t`
 * --------------------------------------------------------------------------- */

static int fb_cb_on_file_open(const wchar_t* rel_path, fb_io_override_desc_t* out_desc) {
    InterlockedIncrement64(&g_core.file_open_calls);
    if (!rel_path || !out_desc) {
        return 0;
    }

    AcquireSRWLockShared(&g_core.lock);
    if (!g_core.attached || !g_core.has_bundle || g_core.bundle.ov_count == 0) {
        ReleaseSRWLockShared(&g_core.lock);
        return 0;
    }

    char norm_path[1024];
    int norm_len = fb_normalize_wide_path(
        rel_path,
        g_core.game_root_utf8,
        g_core.game_root_utf8_len,
        norm_path,
        sizeof(norm_path));
    if (norm_len <= 0) {
        ReleaseSRWLockShared(&g_core.lock);
        return 0;
    }

    const fb_bundle_overlay_entry_t* entry = fb_bundle_find_overlay(&g_core.bundle, norm_path, (size_t)norm_len);
    if (!entry) {
        ReleaseSRWLockShared(&g_core.lock);
        return 0;
    }

    fb_overlay_handle_t* handle = (fb_overlay_handle_t*)malloc(sizeof(fb_overlay_handle_t));
    if (!handle) {
        ReleaseSRWLockShared(&g_core.lock);
        core_log(FB_LOG_ERROR, "fireball-core", "Out of memory allocating overlay handle in on_file_open");
        return 0;
    }
    handle->data = g_core.bundle.raw_data + entry->payload_offset;
    handle->size = entry->payload_size;

    out_desc->virtual_size = entry->payload_size;
    out_desc->user_ctx = handle;
    out_desc->read_at = fb_overlay_read_at;
    out_desc->close = fb_overlay_close;

    InterlockedIncrement64(&g_core.file_open_hits);
    ptrdiff_t idx = entry - g_core.bundle.ov_entries;
    int log_first = 0;
    uint64_t virtual_size = entry->payload_size;
    if (g_core.ov_first_hit && idx >= 0 && (uint32_t)idx < g_core.bundle.ov_count) {
        if (InterlockedCompareExchange(&g_core.ov_first_hit[idx], 1, 0) == 0) {
            log_first = 1;
        }
    }

    ReleaseSRWLockShared(&g_core.lock);
    if (log_first) {
        char msg[512];
        snprintf(
            msg,
            sizeof(msg),
            "First file_open overlay hit: '%s' (%llu B)",
            norm_path,
            (unsigned long long)virtual_size);
        core_log(FB_LOG_INFO, "fireball-core", msg);
    }
    return 1;
}

static int fb_cb_on_dstorage_read(const fb_dstorage_req_view_t* req) {
    InterlockedIncrement64(&g_core.dstorage_calls);
    if (!req || !req->file_path) {
        return 0;
    }

    AcquireSRWLockShared(&g_core.lock);
    if (!g_core.attached || !g_core.has_bundle || g_core.bundle.ds_count == 0) {
        ReleaseSRWLockShared(&g_core.lock);
        return 0;
    }

    char norm_path[1024];
    int norm_len = fb_normalize_wide_path(
        req->file_path,
        g_core.game_root_utf8,
        g_core.game_root_utf8_len,
        norm_path,
        sizeof(norm_path));
    if (norm_len <= 0) {
        ReleaseSRWLockShared(&g_core.lock);
        return 0;
    }

    const fb_bundle_dstorage_entry_t* entry = fb_bundle_find_dstorage(
        &g_core.bundle,
        norm_path,
        (size_t)norm_len,
        req->file_offset,
        req->uncomp_size);
    if (!entry) {
        ReleaseSRWLockShared(&g_core.lock);
        return 0;
    }

    /* Enforce destination buffer bounds strictly before touching dst_ptr */
    if (!req->dst_ptr || req->dst_size < entry->payload_size) {
        InterlockedIncrement64(&g_core.dstorage_buf_too_small);
        char msg[512];
        snprintf(
            msg,
            sizeof(msg),
            "on_dstorage_read('%s' @ 0x%llX): destination buffer too small (%u B < %u B patched payload); passing through safely",
            norm_path,
            (unsigned long long)req->file_offset,
            req->dst_size,
            entry->payload_size);
        ReleaseSRWLockShared(&g_core.lock);
        core_log(FB_LOG_WARN, "fireball-core", msg);
        return 0;
    }

    memcpy(req->dst_ptr, g_core.bundle.raw_data + entry->payload_offset, entry->payload_size);

    /* If patched payload shrank relative to requested uncomp_size, zero-pad the remainder cleanly */
    if (req->uncomp_size > entry->payload_size && req->uncomp_size <= req->dst_size) {
        memset(
            (uint8_t*)req->dst_ptr + entry->payload_size,
            0,
            (size_t)(req->uncomp_size - entry->payload_size));
    }

    InterlockedIncrement64(&g_core.dstorage_hits);
    ptrdiff_t idx = entry - g_core.bundle.ds_entries;
    int log_first = 0;
    uint32_t orig_sz = entry->orig_uncomp_size;
    uint32_t pay_sz = entry->payload_size;
    if (g_core.ds_first_hit && idx >= 0 && (uint32_t)idx < g_core.bundle.ds_count) {
        if (InterlockedCompareExchange(&g_core.ds_first_hit[idx], 1, 0) == 0) {
            log_first = 1;
        }
    }

    ReleaseSRWLockShared(&g_core.lock);
    if (log_first) {
        char msg[512];
        snprintf(
            msg,
            sizeof(msg),
            "First DirectStorage hit: '%s' @ 0x%llX (orig=%u B, patched=%u B, dst=%u B)",
            norm_path,
            (unsigned long long)req->file_offset,
            orig_sz,
            pay_sz,
            req->dst_size);
        core_log(FB_LOG_INFO, "fireball-core", msg);
    }
    return 1;
}

static int fb_cb_on_shader_load(
    const uint8_t container_blake3[32],
    const uint8_t dxbc_digest[16],
    const char*   effect_name_or_null,
    const void**  out_dxil,
    size_t*       out_size)
{
    (void)container_blake3;
    (void)dxbc_digest;
    (void)effect_name_or_null;
    (void)out_dxil;
    (void)out_size;
    return 0;
}

static int fb_cb_on_bvh_rehydrate(uint64_t archetype_h64, uint64_t drawable_h64) {
    (void)archetype_h64;
    (void)drawable_h64;
    return 0;
}

static void fb_cb_on_frame_begin(uint64_t frame_index, double dt_sec) {
    InterlockedIncrement64(&g_core.frames_seen);
    fb_frame_event_v1_t ev;
    ev.frame_index = frame_index;
    ev.dt_sec = dt_sec;

    AcquireSRWLockShared(&g_core.lock);
    if (g_core.attached) {
        EnterCriticalSection(&g_core.menu_lock);
        fb_menu_ui_tick(&g_core.menu_ui, dt_sec);
        fb_console_tick(&g_core.console, dt_sec);
        LeaveCriticalSection(&g_core.menu_lock);

        fb_plugin_mgr_dispatch(&g_core.plugin_mgr, FB_EVENT_FRAME_BEGIN, &ev);
    }
    ReleaseSRWLockShared(&g_core.lock);
}

static void fb_cb_on_frame_present(void* d3d12_swapchain_or_null) {
    AcquireSRWLockShared(&g_core.lock);
    if (g_core.attached) {
        fb_plugin_mgr_dispatch(&g_core.plugin_mgr, FB_EVENT_FRAME_PRESENT, d3d12_swapchain_or_null);
    }
    ReleaseSRWLockShared(&g_core.lock);
}

static int fb_cb_on_input_event(const fb_input_event_v1_t* ev) {
    InterlockedIncrement64(&g_core.input_events);
    if (!ev || ev->struct_size < sizeof(fb_input_event_v1_t)) {
        return 0;
    }
    int consumed = 0;
    AcquireSRWLockShared(&g_core.lock);
    if (g_core.attached) {
        EnterCriticalSection(&g_core.menu_lock);
        fb_bindings_track_input(&g_core.bindings, ev);
        int console_open = fb_console_is_open(&g_core.console);
        int menu_open = fb_menu_ui_is_open(&g_core.menu_ui);
        if (console_open) {
            consumed = fb_console_handle_input(&g_core.console, ev);
        } else if (menu_open) {
            if (!g_core.bindings.capture_active &&
                fb_bindings_matches_action_press(&g_core.bindings, "fireball.core", "toggle_console", ev))
            {
                fb_menu_ui_set_open(&g_core.menu_ui, 0);
                fb_console_set_open(&g_core.console, 1);
                consumed = 1;
            } else {
                consumed = fb_menu_ui_handle_input(&g_core.menu_ui, ev);
            }
        } else {
            consumed = fb_bindings_dispatch_input(&g_core.bindings, ev, 0);
        }
        LeaveCriticalSection(&g_core.menu_lock);

        if (!consumed && !menu_open && !console_open) {
            consumed = fb_plugin_mgr_dispatch(&g_core.plugin_mgr, FB_EVENT_INPUT_EVENT, ev);
        }
    }
    ReleaseSRWLockShared(&g_core.lock);
    if (consumed) {
        InterlockedIncrement64(&g_core.input_consumed);
    }
    return consumed;
}

static void fb_free_first_hit_arrays(void) {
    free((void*)g_core.ds_first_hit);
    free((void*)g_core.ov_first_hit);
    g_core.ds_first_hit = NULL;
    g_core.ov_first_hit = NULL;
}

static void fb_reset_telemetry(void) {
    g_core.dstorage_calls = 0;
    g_core.dstorage_hits = 0;
    g_core.dstorage_buf_too_small = 0;
    g_core.file_open_calls = 0;
    g_core.file_open_hits = 0;
    g_core.input_events = 0;
    g_core.input_consumed = 0;
    g_core.frames_seen = 0;
}

static void fb_teardown_unlocked(void) {
    fb_console_shutdown(&g_core.console);
    fb_menu_ui_shutdown(&g_core.menu_ui);
    fb_plugin_mgr_shutdown_all(&g_core.plugin_mgr, &g_core.host_api);
    fb_bindings_free(&g_core.bindings);
    fb_settings_free(&g_core.settings);
    fb_free_first_hit_arrays();
    if (g_core.has_bundle) {
        fb_bundle_free(&g_core.bundle);
        g_core.has_bundle = 0;
    }
    fb_reset_telemetry();
}

static void fb_cb_on_shutdown(void) {
    fb_console_stop_pipe(&g_core.console);
    AcquireSRWLockExclusive(&g_core.lock);
    if (!g_core.attached) {
        ReleaseSRWLockExclusive(&g_core.lock);
        return;
    }
    {
        char msg[512];
        snprintf(
            msg,
            sizeof(msg),
            "Telemetry summary: dstorage_calls=%lld dstorage_hits=%lld dstorage_buf_too_small=%lld | "
            "file_open_calls=%lld file_open_hits=%lld | frames_seen=%lld | input_events=%lld input_consumed=%lld",
            (long long)g_core.dstorage_calls,
            (long long)g_core.dstorage_hits,
            (long long)g_core.dstorage_buf_too_small,
            (long long)g_core.file_open_calls,
            (long long)g_core.file_open_hits,
            (long long)g_core.frames_seen,
            (long long)g_core.input_events,
            (long long)g_core.input_consumed);
        core_log(FB_LOG_INFO, "fireball-core", msg);
    }
    core_log(FB_LOG_INFO, "fireball-core", "Shutting down Fireball-Core runtime...");
    fb_teardown_unlocked();
    memset(&g_core.host_api, 0, sizeof(g_core.host_api));
    g_core.game_root_utf8[0] = '\0';
    g_core.game_root_utf8_len = 0;
    g_core.attached = 0;
    ReleaseSRWLockExclusive(&g_core.lock);
}

static int fb_cb_on_rage_vfs_open(const char* vfs_uri, fb_io_override_desc_t* out_desc) {
    (void)vfs_uri;
    (void)out_desc;
    return 0;
}

static int fb_cb_on_rage_datafile_register(const char* file_type, const char* vfs_uri) {
    (void)file_type;
    (void)vfs_uri;
    return 0;
}

/* ---------------------------------------------------------------------------
 * Core services exposed to native plugins via `fb_core_services_v1_t`
 * --------------------------------------------------------------------------- */

static void fb_svc_log(const char* mod_id, int level, const char* msg) {
    core_log(level, mod_id ? mod_id : "plugin", msg ? msg : "");
}

static int fb_svc_config_get_json(const char* mod_id, char* buf, size_t cap) {
    if (!mod_id || !buf || cap == 0) {
        return FB_ERR_INVALID_ARG;
    }
    if (!g_core.has_bundle) {
        return FB_ERR_NOT_FOUND;
    }
    const fb_bundle_plugin_cfg_entry_t* e = fb_bundle_find_plugin_cfg(&g_core.bundle, mod_id, strlen(mod_id));
    if (!e) {
        return FB_ERR_NOT_FOUND;
    }
    if ((size_t)e->json_len + 1u > cap) {
        return FB_ERR_INVALID_ARG;
    }
    memcpy(buf, g_core.bundle.raw_data + e->json_offset, (size_t)e->json_len + 1u);
    return FB_OK;
}

static int fb_svc_register_event_listener(uint32_t event_id, fb_event_fn_t fn, void* user_ctx) {
    return fb_plugin_mgr_add_listener(&g_core.plugin_mgr, event_id, fn, user_ctx);
}

static int fb_svc_poll_key_state(uint32_t device_kind, uint32_t code) {
    if ((g_core.host_api.capabilities & FB_CAP_INPUT_EVENTS) == 0 || !g_core.host_api.poll_key_state) {
        return FB_ERR_UNSUPPORTED;
    }
    return g_core.host_api.poll_key_state(device_kind, code);
}

static int fb_svc_vfs_read_file(const char* vfs_uri, void** out_buf, size_t* out_len) {
    (void)vfs_uri;
    (void)out_buf;
    (void)out_len;
    return FB_ERR_UNSUPPORTED;
}

static void fb_svc_vfs_free_buffer(void* buf) {
    free(buf);
}

static void* fb_svc_resolve_engine_symbol(const char* symbol_name) {
    if (!symbol_name) {
        return NULL;
    }
    if ((g_core.host_api.capabilities & FB_CAP_RAGE_SYMBOL_REG) == 0 || !g_core.host_api.resolve_symbol) {
        return NULL;
    }
    return g_core.host_api.resolve_symbol(symbol_name);
}

static void* fb_svc_get_store_singleton(const char* store_name) {
    (void)store_name;
    return NULL;
}

static int fb_svc_title_mem_read_u32(uint64_t title_va, uint32_t* out_value) {
    if (!out_value) {
        return FB_ERR_INVALID_ARG;
    }
    if ((g_core.host_api.capabilities & FB_CAP_TITLE_MEM_RW) == 0 || !g_core.host_api.title_mem_read_u32) {
        return FB_ERR_UNSUPPORTED;
    }
    return g_core.host_api.title_mem_read_u32(title_va, out_value);
}

static int fb_svc_title_mem_write_u32(uint64_t title_va, uint32_t value) {
    if ((g_core.host_api.capabilities & FB_CAP_TITLE_MEM_RW) == 0 || !g_core.host_api.title_mem_write_u32) {
        return FB_ERR_UNSUPPORTED;
    }
    return g_core.host_api.title_mem_write_u32(title_va, value);
}

/* ---------------------------------------------------------------------------
 * Menu / Settings / Action Binding API exposed via `fb_menu_api_v1_t`
 * --------------------------------------------------------------------------- */

static int fb_menu_svc_register_mod(const char* mod_id, const char* display_name, const char* description) {
    EnterCriticalSection(&g_core.menu_lock);
    int rc = fb_settings_register_mod(&g_core.settings, mod_id, display_name, description);
    if (rc == FB_OK) {
        fb_menu_ui_rebuild(&g_core.menu_ui);
    }
    LeaveCriticalSection(&g_core.menu_lock);
    return rc;
}

static int fb_menu_svc_register_action(const fb_action_desc_v1_t* desc) {
    EnterCriticalSection(&g_core.menu_lock);
    if (desc && desc->mod_id) {
        fb_settings_register_mod(&g_core.settings, desc->mod_id, NULL, NULL);
    }
    int rc = fb_bindings_register(&g_core.bindings, desc);
    if (rc == FB_OK) {
        fb_menu_ui_rebuild(&g_core.menu_ui);
    }
    LeaveCriticalSection(&g_core.menu_lock);
    return rc;
}

static int fb_menu_svc_register_setting(const fb_setting_desc_v1_t* desc) {
    EnterCriticalSection(&g_core.menu_lock);
    int rc = fb_settings_register(&g_core.settings, desc);
    if (rc == FB_OK) {
        fb_menu_ui_rebuild(&g_core.menu_ui);
    }
    LeaveCriticalSection(&g_core.menu_lock);
    return rc;
}

static int fb_menu_svc_get_setting(const char* mod_id, const char* setting_id, fb_setting_value_t* out_val) {
    EnterCriticalSection(&g_core.menu_lock);
    int rc = fb_settings_get(&g_core.settings, mod_id, setting_id, out_val);
    LeaveCriticalSection(&g_core.menu_lock);
    return rc;
}

static int fb_menu_svc_set_setting(const char* mod_id, const char* setting_id, const fb_setting_value_t* val, int notify) {
    EnterCriticalSection(&g_core.menu_lock);
    int rc = fb_settings_set(&g_core.settings, mod_id, setting_id, val, notify);
    InterlockedExchange(&g_core.menu_ui.dirty, 1);
    LeaveCriticalSection(&g_core.menu_lock);
    return rc;
}

static int fb_menu_svc_save_user_config(void) {
    EnterCriticalSection(&g_core.menu_lock);
    int rc = fb_user_config_save(&g_core.settings, &g_core.bindings, NULL);
    LeaveCriticalSection(&g_core.menu_lock);
    return rc;
}

static int fb_menu_svc_is_menu_open(void) {
    EnterCriticalSection(&g_core.menu_lock);
    int open = fb_menu_ui_is_open(&g_core.menu_ui);
    LeaveCriticalSection(&g_core.menu_lock);
    return open;
}

/* ---------------------------------------------------------------------------
 * Command Console API exposed via `fb_console_api_v1_t`
 * --------------------------------------------------------------------------- */

static int fb_console_svc_register_command(const fb_command_desc_v1_t* desc) {
    EnterCriticalSection(&g_core.menu_lock);
    int rc = fb_console_register_command(&g_core.console, desc);
    LeaveCriticalSection(&g_core.menu_lock);
    return rc;
}

static int fb_console_svc_exec(const char* line) {
    EnterCriticalSection(&g_core.menu_lock);
    int rc = fb_console_exec(&g_core.console, line, NULL);
    LeaveCriticalSection(&g_core.menu_lock);
    return rc;
}

static int fb_console_svc_is_open(void) {
    EnterCriticalSection(&g_core.menu_lock);
    int open = fb_console_is_open(&g_core.console);
    LeaveCriticalSection(&g_core.menu_lock);
    return open;
}

static int fb_console_pipe_exec_cb(void* user_ctx, const char* line, const fb_console_writer_v1_t* out) {
    (void)user_ctx;
    int rc = FB_ERR_UNSUPPORTED;
    AcquireSRWLockShared(&g_core.lock);
    if (g_core.attached) {
        EnterCriticalSection(&g_core.menu_lock);
        rc = fb_console_exec(&g_core.console, line, out);
        LeaveCriticalSection(&g_core.menu_lock);
    }
    ReleaseSRWLockShared(&g_core.lock);
    return rc;
}

/* ---------------------------------------------------------------------------
 * Primary DLL export: `fb_attach_host`
 * --------------------------------------------------------------------------- */

FB_API int fb_attach_host(
    const fb_host_api_v1_t*    host_api,
    fb_modcore_callbacks_v1_t* out_callbacks)
{
    if (!host_api || !out_callbacks) {
        return FB_ERR_INVALID_ARG;
    }
    if (host_api->abi_version != FB_HOST_ABI_VERSION || host_api->struct_size != sizeof(fb_host_api_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    if (out_callbacks->struct_size != 0 && out_callbacks->struct_size != sizeof(fb_modcore_callbacks_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }

    fb_console_stop_pipe(&g_core.console);
    AcquireSRWLockExclusive(&g_core.lock);

    if (!g_core.menu_lock_init) {
        InitializeCriticalSection(&g_core.menu_lock);
        g_core.menu_lock_init = 1;
    }

    if (g_core.attached) {
        /* Cleanly tear down any previous attachment before re-attaching */
        fb_teardown_unlocked();
        g_core.attached = 0;
    }

    fb_reset_telemetry();

    g_core.host_api = *host_api;
    snprintf(g_core.host_name_copy, sizeof(g_core.host_name_copy), "%s", host_api->host_name ? host_api->host_name : "unknown");
    snprintf(g_core.host_version_copy, sizeof(g_core.host_version_copy), "%s", host_api->host_version ? host_api->host_version : "0.0.0");
    g_core.host_api.host_name = g_core.host_name_copy;
    g_core.host_api.host_version = g_core.host_version_copy;

    /* Normalize game_root_dir for stripping absolute paths in on_file_open / on_dstorage_read */
    g_core.game_root_utf8[0] = '\0';
    g_core.game_root_utf8_len = 0;
    if (host_api->game_root_dir && host_api->game_root_dir[0] != L'\0') {
        int gl = fb_normalize_wide_path(host_api->game_root_dir, NULL, 0, g_core.game_root_utf8, sizeof(g_core.game_root_utf8));
        if (gl > 0) {
            g_core.game_root_utf8_len = (size_t)gl;
        }
    }

    fb_plugin_mgr_init(&g_core.plugin_mgr);
    fb_bindings_init(&g_core.bindings);
    fb_settings_init(&g_core.settings);
    fb_settings_set_config_path(&g_core.settings, host_api->mods_root_dir);

    {
        fb_menu_telemetry_ptrs_t tptrs;
        tptrs.dstorage_calls = &g_core.dstorage_calls;
        tptrs.dstorage_hits = &g_core.dstorage_hits;
        tptrs.dstorage_buf_too_small = &g_core.dstorage_buf_too_small;
        tptrs.file_open_calls = &g_core.file_open_calls;
        tptrs.file_open_hits = &g_core.file_open_hits;
        tptrs.input_events = &g_core.input_events;
        tptrs.input_consumed = &g_core.input_consumed;
        tptrs.frames_seen = &g_core.frames_seen;
        fb_menu_ui_init(&g_core.menu_ui, &g_core.menu_lock, &g_core.settings, &g_core.bindings, &tptrs);
        fb_console_init(&g_core.console, &g_core.menu_lock, &g_core.settings, &g_core.bindings, &g_core.menu_ui, &tptrs);
    }

    /* Load persisted user settings & bindings before plugins register */
    {
        int urc = fb_user_config_load(&g_core.settings, &g_core.bindings, NULL);
        if (urc == FB_OK) {
            char msg[256];
            snprintf(
                msg,
                sizeof(msg),
                "Loaded fireball_user_config.json (%zu setting overrides, %zu binding overrides)",
                g_core.settings.override_count,
                g_core.bindings.override_count);
            core_log(FB_LOG_INFO, "fireball-core", msg);
        }
    }

    /* Register Fireball Core's own Toggle Mod Menu action (F1 / LB + View) */
    {
        fb_action_desc_v1_t toggle_act;
        memset(&toggle_act, 0, sizeof(toggle_act));
        toggle_act.struct_size = (uint32_t)sizeof(toggle_act);
        toggle_act.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
        toggle_act.mod_id = "fireball.core";
        toggle_act.action_id = "toggle_menu";
        toggle_act.label = "Toggle Mod Menu";
        toggle_act.description = "Open or close the Fireball in-game mod menu overlay";
        toggle_act.consume_input = 1;
        toggle_act.default_key.code = 0x70u; /* VK_F1 */
        toggle_act.default_key.modifiers = FB_MOD_NONE;
        toggle_act.default_pad.code = FB_PAD_BACK; /* View/Back */
        toggle_act.default_pad.modifiers = FB_PAD_LEFT_SHOULDER; /* LB + View */
        toggle_act.callback = fb_core_toggle_menu_action;
        toggle_act.user_ctx = NULL;
        fb_bindings_register(&g_core.bindings, &toggle_act);

        fb_action_desc_v1_t con_act;
        memset(&con_act, 0, sizeof(con_act));
        con_act.struct_size = (uint32_t)sizeof(con_act);
        con_act.trigger_mode = (uint32_t)FB_TRIGGER_PRESS;
        con_act.mod_id = "fireball.core";
        con_act.action_id = "toggle_console";
        con_act.label = "Toggle Command Console";
        con_act.description = "Open or close the Fireball in-game command console overlay";
        con_act.consume_input = 1;
        con_act.default_key.code = 0xC0u; /* VK_OEM_3 ('`' / '~') */
        con_act.default_key.modifiers = FB_MOD_NONE;
        con_act.default_pad.code = FB_PAD_NONE;
        con_act.default_pad.modifiers = FB_PAD_NONE;
        con_act.callback = fb_core_toggle_console_action;
        con_act.user_ctx = NULL;
        fb_bindings_register(&g_core.bindings, &con_act);
    }

    {
        char caps_str[512];
        fb_format_caps(g_core.host_api.capabilities, caps_str, sizeof(caps_str));
        char msg[768];
        snprintf(
            msg,
            sizeof(msg),
            "Attached to host '%s' v%s (ABI v%u, caps=[%s])",
            g_core.host_api.host_name,
            g_core.host_api.host_version,
            g_core.host_api.abi_version,
            caps_str);
        core_log(FB_LOG_INFO, "fireball-core", msg);
    }

    /* Load compiled patch bundle (`<mods_root_dir>\fireball_bundle.fbin`) if present */
    if (host_api->mods_root_dir && host_api->mods_root_dir[0] != L'\0') {
        wchar_t bundle_path[MAX_PATH * 2];
        int n = swprintf(
            bundle_path,
            sizeof(bundle_path) / sizeof(bundle_path[0]),
            L"%ls\\fireball_bundle.fbin",
            host_api->mods_root_dir);
        if (n > 0) {
            char err_buf[256];
            err_buf[0] = '\0';
            int brc = fb_bundle_load_file(bundle_path, &g_core.bundle, err_buf, sizeof(err_buf));
            if (brc == FB_OK) {
                g_core.has_bundle = 1;
                if (g_core.bundle.ds_count > 0) {
                    g_core.ds_first_hit = (volatile LONG*)calloc(g_core.bundle.ds_count, sizeof(LONG));
                }
                if (g_core.bundle.ov_count > 0) {
                    g_core.ov_first_hit = (volatile LONG*)calloc(g_core.bundle.ov_count, sizeof(LONG));
                }
                char msg[512];
                snprintf(
                    msg,
                    sizeof(msg),
                    "Loaded fireball_bundle.fbin (%zu B): %u DirectStorage entries, %u loose overlays, %u plugin configs",
                    g_core.bundle.raw_size,
                    g_core.bundle.ds_count,
                    g_core.bundle.ov_count,
                    g_core.bundle.cfg_count);
                core_log(FB_LOG_INFO, "fireball-core", msg);
            } else if (brc == FB_ERR_NOT_FOUND) {
                core_log(FB_LOG_INFO, "fireball-core", "No fireball_bundle.fbin found in mods_root_dir; continuing without compiled patch bundle");
            } else {
                char msg[512];
                snprintf(msg, sizeof(msg), "Failed to load fireball_bundle.fbin: %s (rc=%d)", err_buf, brc);
                core_log(FB_LOG_ERROR, "fireball-core", msg);
            }
        }
    }

    /* Discover and initialize native plugins (`*.fbplugin.dll`) */
    fb_core_services_v1_t services;
    memset(&services, 0, sizeof(services));
    services.struct_size = (uint32_t)sizeof(services);
    services.abi_version = FB_PLUGIN_ABI_VERSION;
    services.active_host_caps = g_core.host_api.capabilities;
    services.log = fb_svc_log;
    services.config_get_json = fb_svc_config_get_json;
    services.register_event_listener = fb_svc_register_event_listener;
    services.poll_key_state = fb_svc_poll_key_state;
    services.vfs_read_file = fb_svc_vfs_read_file;
    services.vfs_free_buffer = fb_svc_vfs_free_buffer;
    services.resolve_engine_symbol = fb_svc_resolve_engine_symbol;
    services.get_store_singleton = fb_svc_get_store_singleton;
    services.title_mem_read_u32 = fb_svc_title_mem_read_u32;
    services.title_mem_write_u32 = fb_svc_title_mem_write_u32;

    fb_menu_api_v1_t menu_api;
    memset(&menu_api, 0, sizeof(menu_api));
    menu_api.struct_size = (uint32_t)sizeof(menu_api);
    menu_api.abi_version = FB_MENU_ABI_VERSION;
    menu_api.register_mod = fb_menu_svc_register_mod;
    menu_api.register_action = fb_menu_svc_register_action;
    menu_api.register_setting = fb_menu_svc_register_setting;
    menu_api.get_setting = fb_menu_svc_get_setting;
    menu_api.set_setting = fb_menu_svc_set_setting;
    menu_api.save_user_config = fb_menu_svc_save_user_config;
    menu_api.is_menu_open = fb_menu_svc_is_menu_open;

    fb_console_api_v1_t console_api;
    memset(&console_api, 0, sizeof(console_api));
    console_api.struct_size = (uint32_t)sizeof(console_api);
    console_api.abi_version = FB_CONSOLE_ABI_VERSION;
    console_api.register_command = fb_console_svc_register_command;
    console_api.exec = fb_console_svc_exec;
    console_api.is_console_open = fb_console_svc_is_open;

    if (host_api->mods_root_dir && host_api->mods_root_dir[0] != L'\0') {
        fb_plugin_mgr_discover_and_load(
            &g_core.plugin_mgr,
            host_api->mods_root_dir,
            &g_core.host_api,
            &services,
            &menu_api,
            &console_api);
    }

    fb_menu_ui_set_host_snapshot(&g_core.menu_ui, &g_core.host_api, &g_core.plugin_mgr);
    fb_console_set_host_snapshot(&g_core.console, &g_core.host_api, &g_core.plugin_mgr);

    /* Populate host callbacks */
    memset(out_callbacks, 0, sizeof(*out_callbacks));
    out_callbacks->struct_size = (uint32_t)sizeof(*out_callbacks);
    out_callbacks->abi_version = FB_HOST_ABI_VERSION;
    out_callbacks->on_file_open = fb_cb_on_file_open;
    out_callbacks->on_dstorage_read = fb_cb_on_dstorage_read;
    out_callbacks->on_shader_load = fb_cb_on_shader_load;
    out_callbacks->on_bvh_rehydrate = fb_cb_on_bvh_rehydrate;
    out_callbacks->on_frame_begin = fb_cb_on_frame_begin;
    out_callbacks->on_frame_present = fb_cb_on_frame_present;
    out_callbacks->on_input_event = fb_cb_on_input_event;
    out_callbacks->on_shutdown = fb_cb_on_shutdown;
    out_callbacks->on_rage_vfs_open = fb_cb_on_rage_vfs_open;
    out_callbacks->on_rage_datafile_register = fb_cb_on_rage_datafile_register;

    g_core.attached = 1;
    ReleaseSRWLockExclusive(&g_core.lock);
    fb_console_start_pipe(&g_core.console, fb_console_pipe_exec_cb, NULL);
    return FB_OK;
}
