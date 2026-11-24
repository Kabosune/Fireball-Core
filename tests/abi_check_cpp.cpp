/*
 * abi_check_cpp.cpp — Strict C++ compile check for include/fireball_host.h and include/fireball_plugin.h.
 * Compiled under MSVC with `/W4 /WX` to guarantee clean C++ interoperability with zero C++ STL across the ABI.
 */

#include "fireball_host.h"
#include "fireball_plugin.h"

static_assert(FB_HOST_ABI_VERSION == 1u, "FB_HOST_ABI_VERSION must be 1");
static_assert(FB_PLUGIN_ABI_VERSION == 1u, "FB_PLUGIN_ABI_VERSION must be 1");
static_assert(sizeof(fb_cap_flags_t) == 8, "fb_cap_flags_t must be 64-bit");
static_assert(sizeof(fb_input_event_v1_t) == 32, "fb_input_event_v1_t must be 32 bytes");
static_assert(sizeof(fb_host_api_v1_t) > 0, "fb_host_api_v1_t must be complete");
static_assert(sizeof(fb_modcore_callbacks_v1_t) > 0, "fb_modcore_callbacks_v1_t must be complete");
static_assert(sizeof(fb_plugin_info_v1_t) > 0, "fb_plugin_info_v1_t must be complete");
static_assert(sizeof(fb_core_services_v1_t) > 0, "fb_core_services_v1_t must be complete");

extern "C" int fb_abi_check_cpp_anchor(void) {
    fb_host_api_v1_t api{};
    fb_modcore_callbacks_v1_t cb{};
    fb_plugin_info_v1_t pinfo{};
    fb_core_services_v1_t svc{};
    return static_cast<int>(sizeof(api) + sizeof(cb) + sizeof(pinfo) + sizeof(svc));
}
