/*
 * abi_check_cpp.cpp — Strict C++ compile check for include/fireball_host.h,
 * include/fireball_plugin.h, and include/fireball_menu.h.
 * Compiled under MSVC with `/W4 /WX` to guarantee clean C++ interoperability with zero C++ STL across the ABI.
 */

#include "fireball_host.h"
#include "fireball_plugin.h"
#include "fireball_menu.h"
#include "fireball_console.h"

static_assert(FB_HOST_ABI_VERSION == 1u, "FB_HOST_ABI_VERSION must be 1");
static_assert(FB_PLUGIN_ABI_VERSION == 1u, "FB_PLUGIN_ABI_VERSION must be 1");
static_assert(FB_MENU_ABI_VERSION == 1u, "FB_MENU_ABI_VERSION must be 1");
static_assert(FB_CONSOLE_ABI_VERSION == 1u, "FB_CONSOLE_ABI_VERSION must be 1");
static_assert(sizeof(fb_cap_flags_t) == 8, "fb_cap_flags_t must be 64-bit");
static_assert(sizeof(fb_input_event_v1_t) == 32, "fb_input_event_v1_t must be 32 bytes");
static_assert(sizeof(fb_host_api_v1_t) > 0, "fb_host_api_v1_t must be complete");
static_assert(sizeof(fb_modcore_callbacks_v1_t) > 0, "fb_modcore_callbacks_v1_t must be complete");
static_assert(sizeof(fb_plugin_info_v1_t) > 0, "fb_plugin_info_v1_t must be complete");
static_assert(sizeof(fb_core_services_v1_t) > 0, "fb_core_services_v1_t must be complete");
static_assert(sizeof(fb_binding_chord_t) == 8, "fb_binding_chord_t must be 8 bytes");
static_assert(sizeof(fb_setting_value_t) == 16, "fb_setting_value_t must be 16 bytes");
static_assert(sizeof(fb_menu_api_v1_t) > 0, "fb_menu_api_v1_t must be complete");
static_assert(sizeof(fb_console_writer_v1_t) > 0, "fb_console_writer_v1_t must be complete");
static_assert(sizeof(fb_command_desc_v1_t) > 0, "fb_command_desc_v1_t must be complete");
static_assert(sizeof(fb_console_api_v1_t) > 0, "fb_console_api_v1_t must be complete");

extern "C" int fb_abi_check_cpp_anchor(void) {
    fb_host_api_v1_t api{};
    fb_modcore_callbacks_v1_t cb{};
    fb_plugin_info_v1_t pinfo{};
    fb_core_services_v1_t svc{};
    fb_menu_api_v1_t menu{};
    fb_action_desc_v1_t act{};
    fb_setting_desc_v1_t set{};
    fb_console_writer_v1_t cw{};
    fb_command_desc_v1_t cmd{};
    fb_console_api_v1_t con{};
    return static_cast<int>(sizeof(api) + sizeof(cb) + sizeof(pinfo) + sizeof(svc) + sizeof(menu) +
                            sizeof(act) + sizeof(set) + sizeof(cw) + sizeof(cmd) + sizeof(con));
}
