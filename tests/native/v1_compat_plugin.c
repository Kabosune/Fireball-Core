/*
 * v1_compat_plugin.c — Minimal FB_PLUGIN_ABI_VERSION=1 plugin that does NOT export
 * `fb_plugin_register_menu`. Used to prove 100% backward compatibility with legacy v1 plugins.
 */

#include "fireball_plugin.h"

#include <string.h>

static fb_core_services_v1_t g_core;
static int g_init = 0;

static int v1_on_event(uint32_t event_id, const void* payload, void* user_ctx) {
    (void)user_ctx;
    if (!g_init || event_id != (uint32_t)FB_EVENT_INPUT_EVENT || !payload) {
        return 0;
    }
    const fb_input_event_v1_t* ev = (const fb_input_event_v1_t*)payload;
    /* Consumes F6 (0x75) presses and writes a marker to log */
    if (ev->device_kind == 0u && ev->code == 0x75u && ev->action == (uint32_t)FB_INPUT_PRESS) {
        if (g_core.log) {
            g_core.log("test.v1_compat", FB_LOG_INFO, "v1_compat handled F6");
        }
        return 1;
    }
    return 0;
}

FB_API int fb_plugin_query(fb_plugin_info_v1_t* out_info) {
    if (!out_info) {
        return FB_ERR_INVALID_ARG;
    }
    out_info->struct_size = (uint32_t)sizeof(fb_plugin_info_v1_t);
    out_info->abi_version = FB_PLUGIN_ABI_VERSION;
    out_info->mod_id = "test.v1_compat";
    out_info->plugin_id = "v1_compat";
    out_info->version = "1.0.0";
    out_info->required_caps = FB_CAP_INPUT_EVENTS;
    return FB_OK;
}

FB_API int fb_plugin_init(const fb_core_services_v1_t* core) {
    if (!core || core->abi_version != FB_PLUGIN_ABI_VERSION || core->struct_size != sizeof(fb_core_services_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }
    g_core = *core;
    g_init = 1;
    return core->register_event_listener((uint32_t)FB_EVENT_INPUT_EVENT, v1_on_event, NULL);
}

FB_API void fb_plugin_shutdown(void) {
    g_init = 0;
}
