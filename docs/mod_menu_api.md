# Fireball-Core In-Game Mod Menu, Settings & Action Binding API

## Overview

`fireball_core.dll` includes a built-in **Mod Menu**, **Typed Settings Registry**, and **Keyboard + Gamepad Action Binding Manager** exposed to native plugins through [`include/fireball_menu.h`](../include/fireball_menu.h) (`FB_MENU_ABI_VERSION = 1`).

- **Zero host ABI changes:** `FB_HOST_ABI_VERSION = 1` ([`include/fireball_host.h`](../include/fireball_host.h)) is completely untouched. Works immediately with `Marvisak/VIctory` via existing `on_input_event`, `poll_key_state`, `on_frame_begin`, `mods_root_dir`, and `on_shutdown` callbacks.
- **100% backward compatible with v1 plugins:** Existing `*.fbplugin.dll` plugins that only export `fb_plugin_query` / `fb_plugin_init` / `fb_plugin_shutdown` continue to load unchanged.
- **Focus-safe Win32 overlay:** Rendered on a dedicated UI thread using `WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW` so VIctory's `GameInput` keeps receiving keyboard and gamepad events cleanly without focus theft. Set `FIREBALL_HEADLESS=1` in automated/headless test environments to keep the state machine active without creating a Win32 window.

---

## Default Controls

| Control | Keyboard | Gamepad (XInput) |
| :--- | :--- | :--- |
| **Open / Close Mod Menu** (remappable) | `F1` | `LB + View` |
| **Switch Mod Tabs** | `Tab` / `Shift+Tab` / `PgUp` / `PgDn` | `LB` / `RB` |
| **Navigate Items** | `Up` / `Down` | `DPad Up` / `DPad Down` |
| **Adjust Slider / Enum / Action Column** | `Left` / `Right` | `DPad Left` / `DPad Right` |
| **Activate / Toggle / Start Rebind** | `Enter` / `Space` | `A` |
| **Reset Highlighted Item to Default** | `Delete` | `Y` |
| **Close Menu** | `Esc` | `B` |
| **Cancel Rebind Capture** | `Esc` | Hold `B` (0.5 s) |
| **Unbind Slot During Capture** | `Backspace` | `X` (solo) |

While the menu is open, navigation and capture inputs are consumed (`on_input_event` returns `1`) so they do not leak into gameplay.

---

## Plugin Integration (`include/fireball_menu.h`)

Export the optional entry point `fb_plugin_register_menu`:

```c
#include "fireball_plugin.h"
#include "fireball_menu.h"

static void my_action_cb(const char* action_id, int active, void* user_ctx) {
    if (active) { /* trigger action */ }
}

static void my_setting_cb(const char* setting_id, const fb_setting_value_t* val, void* user_ctx) {
    /* apply updated setting live */
}

FB_API int fb_plugin_register_menu(const fb_menu_api_v1_t* menu) {
    if (!menu || menu->abi_version != FB_MENU_ABI_VERSION || menu->struct_size < sizeof(fb_menu_api_v1_t)) {
        return FB_ERR_ABI_MISMATCH;
    }

    menu->register_mod("author.my_mod", "My Mod", "Short description");

    /* Register a typed setting (BOOL, INT, FLOAT, ENUM, BUTTON, or STATUS) */
    fb_setting_desc_v1_t s = { 0 };
    s.struct_size = (uint32_t)sizeof(s);
    s.mod_id = "author.my_mod";
    s.section = "General";
    s.type = FB_SETTING_BOOL;
    s.setting_id = "enabled";
    s.label = "Enable Feature";
    s.default_bool = 1;
    s.on_change = my_setting_cb;
    menu->register_setting(&s);

    /* Register a remappable Keyboard + Gamepad Chord action */
    fb_action_desc_v1_t a = { 0 };
    a.struct_size = (uint32_t)sizeof(a);
    a.mod_id = "author.my_mod";
    a.action_id = "do_thing";
    a.label = "Do Thing";
    a.trigger_mode = FB_TRIGGER_PRESS; /* or FB_TRIGGER_HOLD */
    a.consume_input = 1;
    a.default_key.code = 'V';
    a.default_key.modifiers = FB_MOD_NONE;
    a.default_pad.code = FB_PAD_RIGHT_THUMB;
    a.default_pad.modifiers = FB_PAD_LEFT_SHOULDER; /* LB + R3 */
    a.callback = my_action_cb;
    menu->register_action(&a);

    return FB_OK;
}
```

### Lifecycle & Persistence
1. During `fb_attach_host()`, `fireball_core.dll` loads `<mods_root_dir>\fireball_user_config.json` before discovering plugins.
2. For each loaded plugin, `fireball_core.dll` calls `fb_plugin_init(core)` and then, if exported, `fb_plugin_register_menu(menu_api)`.
3. Any persisted user override for a registered setting is applied immediately (clamped to valid bounds) and delivered to the plugin's `on_change` callback.
4. Any persisted user keyboard/gamepad binding or `consume_input` override is applied immediately to the registered action.
5. Changes made in the Mod Menu (or via `menu->save_user_config()`) are written atomically to `<mods_root_dir>\fireball_user_config.json` via a `.tmp` file + `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`. Overrides for temporarily unloaded/skipped plugins are preserved across saves.

### Chord Matching Priority
- **Keyboard:** Modifier masks (`FB_MOD_SHIFT`, `FB_MOD_CTRL`, `FB_MOD_ALT`) must match exactly, so `Ctrl+Shift+V` never accidentally fires plain `V`.
- **Gamepad:** Chord bindings (`modifiers != 0`, e.g., `LB + DPad Right`) take priority over single-button bindings (`modifiers == 0`, e.g., `DPad Right`) when the modifier button is held.
- **Hold Actions (`FB_TRIGGER_HOLD`):** Fire `active = 1` on press and `active = 0` when either the trigger key/button or a chord modifier is released.
