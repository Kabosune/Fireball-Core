/*
 * abi_check_c11.c — Strict C11 compile check for include/fireball_host.h and include/fireball_plugin.h.
 * Compiled under MSVC with `/std:c11 /W4 /WX`.
 */

#include "fireball_host.h"
#include "fireball_plugin.h"

_Static_assert(FB_HOST_ABI_VERSION == 1u, "FB_HOST_ABI_VERSION must be 1");
_Static_assert(FB_PLUGIN_ABI_VERSION == 1u, "FB_PLUGIN_ABI_VERSION must be 1");
_Static_assert(sizeof(fb_cap_flags_t) == 8, "fb_cap_flags_t must be 64-bit");
_Static_assert(FB_CAP_IO_VIRTUAL == (1ull << 0), "FB_CAP_IO_VIRTUAL bit check");
_Static_assert(FB_CAP_DSTORAGE_INTERCEPT == (1ull << 1), "FB_CAP_DSTORAGE_INTERCEPT bit check");
_Static_assert(FB_CAP_INPUT_EVENTS == (1ull << 8), "FB_CAP_INPUT_EVENTS bit check");
_Static_assert(FB_CAP_RAGE_FIDEVICE == (1ull << 16), "FB_CAP_RAGE_FIDEVICE bit check");
_Static_assert(FB_CAP_TITLE_MEM_RW == (1ull << 26), "FB_CAP_TITLE_MEM_RW bit check");
_Static_assert(sizeof(fb_host_api_v1_t) > 0, "fb_host_api_v1_t must be complete");
_Static_assert(sizeof(fb_modcore_callbacks_v1_t) > 0, "fb_modcore_callbacks_v1_t must be complete");
_Static_assert(sizeof(fb_dstorage_req_view_t) > 0, "fb_dstorage_req_view_t must be complete");
_Static_assert(sizeof(fb_io_override_desc_t) > 0, "fb_io_override_desc_t must be complete");
_Static_assert(sizeof(fb_input_event_v1_t) == 32, "fb_input_event_v1_t must be 32 bytes");
_Static_assert(sizeof(fb_plugin_info_v1_t) > 0, "fb_plugin_info_v1_t must be complete");
_Static_assert(sizeof(fb_core_services_v1_t) > 0, "fb_core_services_v1_t must be complete");

int fb_abi_check_c11_anchor(void) {
    fb_host_api_v1_t api = { 0 };
    fb_modcore_callbacks_v1_t cb = { 0 };
    fb_plugin_info_v1_t pinfo = { 0 };
    fb_core_services_v1_t svc = { 0 };
    return (int)(sizeof(api) + sizeof(cb) + sizeof(pinfo) + sizeof(svc));
}
