# Fireball-Core Command Console API (`include/fireball_console.h`)

## Overview

`fireball_core.dll` includes a built-in **Command Console** exposed to native plugins through [`include/fireball_console.h`](../include/fireball_console.h) (`FB_CONSOLE_ABI_VERSION = 1`). It shares one command registry across:

1. **In-Game `~` Overlay Console:** Focus-safe Win32 GDI overlay (`WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW`) matching the `F1` Mod Menu’s dark slate style. Consumes keyboard events via `on_input_event` while open so typing never leaks into `VIctory` / gameplay. Respects `FIREBALL_HEADLESS=1` (state machine and command execution stay active with no Win32 window).
2. **Optional Local Named-Pipe Terminal REPL:** Local-only Win32 named pipe (`\\.\pipe\fireball_console`, created with `PIPE_REJECT_REMOTE_CLIENTS`) accessible from another terminal or script via `python -B fireball_cli.py console`.

- **Zero host ABI changes:** `FB_HOST_ABI_VERSION = 1` ([`include/fireball_host.h`](../include/fireball_host.h)), `FB_PLUGIN_ABI_VERSION = 1`, and `FB_MENU_ABI_VERSION = 1` are untouched.
- **100% backward compatible:** Existing v1 plugins that do not export `fb_plugin_register_console` continue to load unchanged.
- **Strictly in-process / offline:** Commands run inside `fireball_core.dll` and loaded plugins only — never an OS shell.

---

## Default In-Game Controls

| Control | Keyboard | Notes |
| :--- | :--- | :--- |
| **Open / Close Console** (remappable in `F1` menu) | `` ` `` / `~` (`VK_OEM_3`) | Opening the console closes the `F1` Mod Menu (and vice versa) |
| **Execute Command** | `Enter` | Appends to history and prints output to scrollback |
| **Autocomplete** | `Tab` | Completes command names, plus `mod_id` and `setting_id`/`action_id` for `help`/`get`/`set`/`trigger` |
| **Command History** | `Up` / `Down` | Preserves in-progress draft when browsing back to the bottom |
| **Caret Editing** | `Left` / `Right` / `Home` / `End` / `Backspace` / `Delete` | Standard single-line prompt editing |
| **Scroll Output** | `PgUp` / `PgDn` | Scroll through the 256-line scrollback ring buffer |
| **Paste / Clear** | `Ctrl+V` / `Ctrl+L` / `Ctrl+C` | ASCII-sanitized clipboard paste / clear scrollback / clear prompt |
| **Close Console** | `Esc` or `` ` `` | Releases keyboard input back to gameplay |

---

## Built-In Core Commands

| Command | Description |
| :--- | :--- |
| `help [command]` | List all registered commands, or show detailed usage for one command |
| `clear` | Clear console scrollback |
| `mods` | Show active host capabilities, loaded plugins (`[menu]`/`[console]`/`[v1]`), and skipped plugins |
| `stats` | Print live atomic telemetry (`dstorage`, `file_open`, `frames_seen`, `input_events/consumed`) |
| `get <mod_id> [setting_id]` | Read one or all registered `fireball_menu.h` settings for a mod |
| `set <mod_id> <setting_id> <value>` | Update any registered `BOOL`, `INT`, `FLOAT`, or `ENUM` setting (clamped to bounds, triggers `on_change`) |
| `trigger <mod_id> <action_or_button_id>` | Invoke a registered menu button (`FB_SETTING_BUTTON`) or bound action |
| `save` | Atomically save `fireball_user_config.json` |

### Example Plugin Commands
- **`fpv_toggle.fbplugin.dll`:** `fpv [on|off|toggle|status]`
- **`time_weather.fbplugin.dll`:** `time <HH[:MM[:SS]]|pause|resume|scale <f>|+N|-N|status>`, `weather <name|index|lock|unlock|list|status> [blend]`
- **`state_scan.fbplugin.dll`:** `scan.snap <run|paused>`, `scan.read <hex_rva>`, `scan.write <hex_rva> <u32|f<float>>`

---

## Plugin Integration (`include/fireball_console.h`)

Export the optional entry point `fb_plugin_register_console`:

```c
#include "fireball_plugin.h"
#include "fireball_console.h"

static int my_cmd(
    int                           argc,
    const char* const*            argv,
    const fb_console_writer_v1_t* out,
    void*                         user_ctx)
{
    (void)user_ctx;
    if (out && out->print_fmt) {
        out->print_fmt(out->user_ctx, FB_LOG_INFO, "Hello from %s (argc=%d)", argv[0], argc);
    }
    return FB_OK;
}

FB_API int fb_plugin_register_console(const fb_console_api_v1_t* console) {
    if (!console || console->abi_version != FB_CONSOLE_ABI_VERSION ||
        console->struct_size < sizeof(fb_console_api_v1_t))
    {
        return FB_ERR_ABI_MISMATCH;
    }

    fb_command_desc_v1_t cmd = { 0 };
    cmd.struct_size = (uint32_t)sizeof(cmd);
    cmd.mod_id      = "author.my_mod";
    cmd.name        = "mycmd";
    cmd.usage       = "mycmd [arg]";
    cmd.description = "Example plugin console command";
    cmd.callback    = my_cmd;
    return console->register_command(&cmd);
}
```

---

## Local Named-Pipe Terminal REPL (`fireball_cli.py console`)

When `fireball_core.dll` is attached (and not headless), it listens on `\\.\pipe\fireball_console` (local machine only via `PIPE_REJECT_REMOTE_CLIENTS`).

- Override or enable in headless mode via `FIREBALL_CONSOLE_PIPE` (set to `0`/`off` to disable, `1`/`on` for the default pipe, or a custom pipe name).
- Connect interactively or run one-shot commands from another terminal:

```powershell
# Interactive REPL
python -B fireball_cli.py console

# One-shot command(s)
python -B fireball_cli.py console -c "stats" -c "fpv status"

# Custom pipe name
python -B fireball_cli.py console --pipe my_test_pipe -c "help"
```
