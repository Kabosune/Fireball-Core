/*
 * fb_plugins.c — Dynamic `*.fbplugin.dll` discovery, capability enforcement, and event bus.
 *
 * Pure C11 + Win32 API.
 */

#include "fb_plugins.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct fb_cap_desc {
    fb_cap_flags_t bit;
    const char*    name;
} fb_cap_desc_t;

static const fb_cap_desc_t g_cap_table[] = {
    { FB_CAP_IO_VIRTUAL,         "FB_CAP_IO_VIRTUAL" },
    { FB_CAP_DSTORAGE_INTERCEPT, "FB_CAP_DSTORAGE_INTERCEPT" },
    { FB_CAP_D3D12_SHADERS,      "FB_CAP_D3D12_SHADERS" },
    { FB_CAP_D3D12_TEXTURES,     "FB_CAP_D3D12_TEXTURES" },
    { FB_CAP_BVH_DXR_FALLBACK,   "FB_CAP_BVH_DXR_FALLBACK" },
    { FB_CAP_XMEM_HOOKS,         "FB_CAP_XMEM_HOOKS" },
    { FB_CAP_MEDIA_BINK_AUDIO,   "FB_CAP_MEDIA_BINK_AUDIO" },
    { FB_CAP_FRAME_LIFECYCLE,    "FB_CAP_FRAME_LIFECYCLE" },
    { FB_CAP_INPUT_EVENTS,       "FB_CAP_INPUT_EVENTS" },
    { FB_CAP_RAGE_FIDEVICE,      "FB_CAP_RAGE_FIDEVICE" },
    { FB_CAP_RAGE_DATAFILEMGR,   "FB_CAP_RAGE_DATAFILEMGR" },
    { FB_CAP_RAGE_CRC_BYPASS,    "FB_CAP_RAGE_CRC_BYPASS" },
    { FB_CAP_RAGE_STORES,        "FB_CAP_RAGE_STORES" },
    { FB_CAP_RAGE_SYMBOL_REG,    "FB_CAP_RAGE_SYMBOL_REG" },
    { FB_CAP_TITLE_MEM_RW,       "FB_CAP_TITLE_MEM_RW" }
};

void fb_format_caps(fb_cap_flags_t mask, char* out_buf, size_t out_cap) {
    if (!out_buf || out_cap == 0) {
        return;
    }
    if (mask == FB_CAP_NONE) {
        snprintf(out_buf, out_cap, "FB_CAP_NONE");
        return;
    }
    size_t pos = 0;
    out_buf[0] = '\0';
    fb_cap_flags_t remaining = mask;
    for (size_t i = 0; i < sizeof(g_cap_table) / sizeof(g_cap_table[0]); ++i) {
        if ((mask & g_cap_table[i].bit) != 0) {
            int written = snprintf(
                out_buf + pos,
                (pos < out_cap) ? (out_cap - pos) : 0,
                "%s%s",
                (pos > 0) ? " | " : "",
                g_cap_table[i].name);
            if (written > 0 && pos + (size_t)written < out_cap) {
                pos += (size_t)written;
            }
            remaining &= ~g_cap_table[i].bit;
        }
    }
    if (remaining != 0) {
        snprintf(
            out_buf + pos,
            (pos < out_cap) ? (out_cap - pos) : 0,
            "%s0x%llX",
            (pos > 0) ? " | " : "",
            (unsigned long long)remaining);
    }
}

static void fb_host_log(const fb_host_api_v1_t* host_api, int level, const char* msg) {
    if (host_api && host_api->log) {
        host_api->log(level, "fireball-core", msg);
    }
}

typedef struct fb_wpath_list {
    wchar_t** items;
    size_t    count;
    size_t    cap;
} fb_wpath_list_t;

static void fb_wpath_list_push(fb_wpath_list_t* list, const wchar_t* path) {
    if (list->count == list->cap) {
        size_t new_cap = (list->cap == 0) ? 16 : (list->cap * 2);
        wchar_t** grown = (wchar_t**)realloc(list->items, new_cap * sizeof(wchar_t*));
        if (!grown) {
            return;
        }
        list->items = grown;
        list->cap = new_cap;
    }
    size_t len = wcslen(path);
    wchar_t* copy = (wchar_t*)malloc((len + 1) * sizeof(wchar_t));
    if (!copy) {
        return;
    }
    memcpy(copy, path, (len + 1) * sizeof(wchar_t));
    list->items[list->count++] = copy;
}

static void fb_wpath_list_free(fb_wpath_list_t* list) {
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i]);
    }
    free(list->items);
    memset(list, 0, sizeof(*list));
}

static int fb_wstr_ends_with_ci(const wchar_t* s, const wchar_t* suffix) {
    size_t slen = wcslen(s);
    size_t suflen = wcslen(suffix);
    if (slen < suflen) {
        return 0;
    }
    return _wcsicmp(s + (slen - suflen), suffix) == 0;
}

static int fb_wpath_cmp(const void* a, const void* b) {
    const wchar_t* pa = *(const wchar_t* const*)a;
    const wchar_t* pb = *(const wchar_t* const*)b;
    return _wcsicmp(pa, pb);
}

static void fb_scan_dir_recursive(const wchar_t* dir, int depth, fb_wpath_list_t* out_list) {
    if (!dir || depth > 3) {
        return;
    }
    wchar_t pattern[MAX_PATH * 2];
    int n = swprintf(pattern, sizeof(pattern) / sizeof(pattern[0]), L"%ls\\*", dir);
    if (n <= 0) {
        return;
    }

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        return;
    }

    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
            continue;
        }
        wchar_t child[MAX_PATH * 2];
        int cn = swprintf(child, sizeof(child) / sizeof(child[0]), L"%ls\\%ls", dir, fd.cFileName);
        if (cn <= 0) {
            continue;
        }
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            fb_scan_dir_recursive(child, depth + 1, out_list);
        } else if (fb_wstr_ends_with_ci(fd.cFileName, L".fbplugin.dll")) {
            fb_wpath_list_push(out_list, child);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void fb_plugin_mgr_init(fb_plugin_manager_t* mgr) {
    memset(mgr, 0, sizeof(*mgr));
    mgr->loading_plugin_index = -1;
}

static void fb_remove_listeners_for_plugin(fb_plugin_manager_t* mgr, int plugin_idx) {
    size_t dst = 0;
    for (size_t i = 0; i < mgr->listener_count; ++i) {
        if (mgr->listeners[i].owner_plugin_index != plugin_idx) {
            if (dst != i) {
                mgr->listeners[dst] = mgr->listeners[i];
            }
            ++dst;
        }
    }
    mgr->listener_count = dst;
}

int fb_plugin_mgr_add_listener(fb_plugin_manager_t* mgr, uint32_t event_id, fb_event_fn_t fn, void* user_ctx) {
    if (!mgr || !fn) {
        return FB_ERR_INVALID_ARG;
    }
    if (mgr->listener_count == mgr->listener_cap) {
        size_t new_cap = (mgr->listener_cap == 0) ? 16 : (mgr->listener_cap * 2);
        fb_event_listener_t* grown = (fb_event_listener_t*)realloc(
            mgr->listeners,
            new_cap * sizeof(fb_event_listener_t));
        if (!grown) {
            return FB_ERR_INTERNAL;
        }
        mgr->listeners = grown;
        mgr->listener_cap = new_cap;
    }
    fb_event_listener_t* slot = &mgr->listeners[mgr->listener_count++];
    slot->event_id = event_id;
    slot->fn = fn;
    slot->user_ctx = user_ctx;
    slot->owner_plugin_index = mgr->loading_plugin_index;
    return FB_OK;
}

void fb_plugin_mgr_discover_and_load(
    fb_plugin_manager_t*         mgr,
    const wchar_t*               mods_root_dir,
    const fb_host_api_v1_t*      host_api,
    const fb_core_services_v1_t* services)
{
    if (!mgr || !mods_root_dir || !host_api || !services) {
        return;
    }

    fb_wpath_list_t found;
    memset(&found, 0, sizeof(found));
    fb_scan_dir_recursive(mods_root_dir, 0, &found);
    if (found.count > 1) {
        qsort(found.items, found.count, sizeof(wchar_t*), fb_wpath_cmp);
    }

    for (size_t i = 0; i < found.count; ++i) {
        const wchar_t* dll_path = found.items[i];
        char utf8_path[512];
        int pn = WideCharToMultiByte(CP_UTF8, 0, dll_path, -1, utf8_path, (int)sizeof(utf8_path), NULL, NULL);
        if (pn <= 0) {
            snprintf(utf8_path, sizeof(utf8_path), "<plugin#%zu>", i);
        }

        HMODULE mod = LoadLibraryW(dll_path);
        if (!mod) {
            char msg[768];
            snprintf(msg, sizeof(msg), "Failed to LoadLibraryW('%s'): Win32 error %lu", utf8_path, (unsigned long)GetLastError());
            fb_host_log(host_api, FB_LOG_WARN, msg);
            continue;
        }

        fb_plugin_query_fn query_fn = (fb_plugin_query_fn)GetProcAddress(mod, "fb_plugin_query");
        fb_plugin_init_fn init_fn = (fb_plugin_init_fn)GetProcAddress(mod, "fb_plugin_init");
        fb_plugin_shutdown_fn shutdown_fn = (fb_plugin_shutdown_fn)GetProcAddress(mod, "fb_plugin_shutdown");
        if (!query_fn || !init_fn || !shutdown_fn) {
            char msg[768];
            snprintf(msg, sizeof(msg), "Skipping '%s': missing required fb_plugin_* exports", utf8_path);
            fb_host_log(host_api, FB_LOG_WARN, msg);
            FreeLibrary(mod);
            continue;
        }

        fb_plugin_info_v1_t info;
        memset(&info, 0, sizeof(info));
        info.struct_size = (uint32_t)sizeof(info);
        int qrc = query_fn(&info);
        if (qrc != FB_OK || info.abi_version != FB_PLUGIN_ABI_VERSION || info.struct_size != sizeof(fb_plugin_info_v1_t)) {
            char msg[768];
            snprintf(
                msg,
                sizeof(msg),
                "Skipping '%s': fb_plugin_query failed or ABI mismatch (rc=%d, abi=%u, size=%u)",
                utf8_path,
                qrc,
                info.abi_version,
                info.struct_size);
            fb_host_log(host_api, FB_LOG_WARN, msg);
            FreeLibrary(mod);
            continue;
        }

        const char* plugin_id = info.plugin_id ? info.plugin_id : "<unnamed>";
        const char* mod_id = info.mod_id ? info.mod_id : "<unknown>";
        const char* version = info.version ? info.version : "0.0.0";

        /* Strict capability enforcement: required_caps must be a subset of host_api->capabilities */
        fb_cap_flags_t missing = info.required_caps & ~host_api->capabilities;
        if (missing != FB_CAP_NONE) {
            char missing_str[512];
            fb_format_caps(missing, missing_str, sizeof(missing_str));
            char msg[1024];
            snprintf(
                msg,
                sizeof(msg),
                "Skipped plugin '%s' (mod '%s' v%s): host '%s' lacks required capabilities [%s]",
                plugin_id,
                mod_id,
                version,
                host_api->host_name ? host_api->host_name : "unknown",
                missing_str);
            fb_host_log(host_api, FB_LOG_WARN, msg);
            FreeLibrary(mod);
            continue;
        }

        if (mgr->plugin_count == mgr->plugin_cap) {
            size_t new_cap = (mgr->plugin_cap == 0) ? 8 : (mgr->plugin_cap * 2);
            fb_loaded_plugin_t* grown = (fb_loaded_plugin_t*)realloc(
                mgr->plugins,
                new_cap * sizeof(fb_loaded_plugin_t));
            if (!grown) {
                fb_host_log(host_api, FB_LOG_ERROR, "Out of memory tracking loaded plugins");
                FreeLibrary(mod);
                continue;
            }
            mgr->plugins = grown;
            mgr->plugin_cap = new_cap;
        }

        int idx = (int)mgr->plugin_count;
        mgr->loading_plugin_index = idx;
        int irc = init_fn(services);
        mgr->loading_plugin_index = -1;

        if (irc != FB_OK) {
            fb_remove_listeners_for_plugin(mgr, idx);
            char msg[768];
            snprintf(msg, sizeof(msg), "Plugin '%s' (mod '%s') fb_plugin_init returned error %d; unloading", plugin_id, mod_id, irc);
            fb_host_log(host_api, FB_LOG_WARN, msg);
            FreeLibrary(mod);
            continue;
        }

        fb_loaded_plugin_t* slot = &mgr->plugins[mgr->plugin_count++];
        memset(slot, 0, sizeof(*slot));
        slot->module = mod;
        slot->shutdown_fn = shutdown_fn;
        snprintf(slot->mod_id, sizeof(slot->mod_id), "%s", mod_id);
        snprintf(slot->plugin_id, sizeof(slot->plugin_id), "%s", plugin_id);
        snprintf(slot->version, sizeof(slot->version), "%s", version);
        slot->required_caps = info.required_caps;

        char msg[512];
        snprintf(msg, sizeof(msg), "Loaded native plugin '%s' (mod '%s' v%s)", slot->plugin_id, slot->mod_id, slot->version);
        fb_host_log(host_api, FB_LOG_INFO, msg);
    }

    fb_wpath_list_free(&found);
}

int fb_plugin_mgr_dispatch(const fb_plugin_manager_t* mgr, uint32_t event_id, const void* payload) {
    if (!mgr) {
        return 0;
    }
    int consumed = 0;
    for (size_t i = 0; i < mgr->listener_count; ++i) {
        if (mgr->listeners[i].event_id == event_id) {
            int r = mgr->listeners[i].fn(event_id, payload, mgr->listeners[i].user_ctx);
            if (event_id == FB_EVENT_INPUT_EVENT && r != 0) {
                consumed = 1;
            }
        }
    }
    return consumed;
}

void fb_plugin_mgr_shutdown_all(fb_plugin_manager_t* mgr, const fb_host_api_v1_t* host_api) {
    if (!mgr) {
        return;
    }
    /* Notify FB_EVENT_SHUTDOWN listeners first */
    fb_plugin_mgr_dispatch(mgr, FB_EVENT_SHUTDOWN, NULL);

    /* Call fb_plugin_shutdown() and FreeLibrary() in reverse load order */
    for (size_t i = mgr->plugin_count; i > 0; --i) {
        fb_loaded_plugin_t* p = &mgr->plugins[i - 1];
        if (p->shutdown_fn) {
            p->shutdown_fn();
        }
        if (host_api && host_api->log) {
            char msg[384];
            snprintf(msg, sizeof(msg), "Unloaded native plugin '%s' (mod '%s')", p->plugin_id, p->mod_id);
            host_api->log(FB_LOG_INFO, "fireball-core", msg);
        }
        if (p->module) {
            FreeLibrary(p->module);
            p->module = NULL;
        }
    }

    free(mgr->plugins);
    free(mgr->listeners);
    memset(mgr, 0, sizeof(*mgr));
    mgr->loading_plugin_index = -1;
}
