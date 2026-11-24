/*
 * fireball_plugin.h — Fireball-Core Native Plugin C11 ABI
 *
 * Pure C11, stable ABI, zero C++ STL across the boundary.
 * Allows native plugins (*.fbplugin.dll) to be authored in C, C++, Rust, or Zig.
 */

#ifndef FIREBALL_PLUGIN_H
#define FIREBALL_PLUGIN_H

#include "fireball_host.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FB_PLUGIN_ABI_VERSION 1u

/* Event IDs for register_event_listener() */
typedef enum fb_event_id {
    FB_EVENT_FRAME_BEGIN   = 1, /* payload: const fb_frame_event_v1_t* */
    FB_EVENT_FRAME_PRESENT = 2, /* payload: void* (d3d12_swapchain_or_null) */
    FB_EVENT_INPUT_EVENT   = 3, /* payload: const fb_input_event_v1_t* */
    FB_EVENT_SHUTDOWN      = 4  /* payload: NULL */
} fb_event_id_t;

typedef struct fb_frame_event_v1 {
    uint64_t frame_index;
    double   dt_sec;
} fb_frame_event_v1_t;

/*
 * Plugin event listener signature.
 * For FB_EVENT_INPUT_EVENT, returning 1 consumes the event; returning 0 passes it through.
 * For lifecycle events, return value is ignored (return 0).
 */
typedef int (*fb_event_fn_t)(uint32_t event_id, const void* payload, void* user_ctx);

/* Metadata returned by a plugin from fb_plugin_query() */
typedef struct fb_plugin_info_v1 {
    uint32_t       struct_size;
    uint32_t       abi_version;    /* Must equal FB_PLUGIN_ABI_VERSION */
    const char*    mod_id;         /* Must match mod.toml [mod].id */
    const char*    plugin_id;      /* Plugin identifier within the mod */
    const char*    version;
    fb_cap_flags_t required_caps;  /* Subset of FB_CAP_* required to activate */
} fb_plugin_info_v1_t;

/*
 * Core services table passed to fb_plugin_init().
 * Plugins MUST inspect `active_host_caps` before relying on Tier 1/2 hooks.
 * Fireball-Core guarantees that if `required_caps` is not a subset of `active_host_caps`,
 * `fb_plugin_init()` is never called (plugin is skipped cleanly with diagnostics).
 */
typedef struct fb_core_services_v1 {
    uint32_t       struct_size;
    uint32_t       abi_version;
    fb_cap_flags_t active_host_caps;

    /* Core logging & config */
    void  (*log)(const char* mod_id, int level, const char* msg);
    int   (*config_get_json)(const char* mod_id, char* buf, size_t cap);

    /* Event bus (Tier 0+: FB_CAP_FRAME_LIFECYCLE / FB_CAP_INPUT_EVENTS) */
    int   (*register_event_listener)(uint32_t event_id, fb_event_fn_t fn, void* user_ctx);

    /* Input polling helper (FB_CAP_INPUT_EVENTS) */
    int   (*poll_key_state)(uint32_t device_kind, uint32_t code);

    /* VFS access (Tier 1: FB_CAP_RAGE_FIDEVICE) */
    int   (*vfs_read_file)(const char* vfs_uri, void** out_buf, size_t* out_len);
    void  (*vfs_free_buffer)(void* buf);

    /* Tier 2: Engine symbols, 43 *Store singletons (36 run starts + 7 tail-merged) & guarded title memory */
    void* (*resolve_engine_symbol)(const char* symbol_name);          /* FB_CAP_RAGE_SYMBOL_REG */
    void* (*get_store_singleton)(const char* store_name);             /* FB_CAP_RAGE_STORES */
    int   (*title_mem_read_u32)(uint64_t title_va, uint32_t* out_value); /* FB_CAP_TITLE_MEM_RW */
    int   (*title_mem_write_u32)(uint64_t title_va, uint32_t value);     /* FB_CAP_TITLE_MEM_RW */
} fb_core_services_v1_t;

/*
 * Entry points exported by each native plugin DLL (*.fbplugin.dll).
 */
FB_API int  fb_plugin_query(fb_plugin_info_v1_t* out_info);
FB_API int  fb_plugin_init(const fb_core_services_v1_t* core);
FB_API void fb_plugin_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_PLUGIN_H */
