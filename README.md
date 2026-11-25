# Fireball-Core

Mod core and mod loader for *Grand Theft Auto VI* (Project Americas / RAGE 9), designed around **[Marvisak/VIctory](https://github.com/marvisak/victory)** (Rust x86-64 HLE runner) with a frozen pure-C11 host & plugin ABI.

## Companion Research Repository

Format specifications, VFS mount maps, and standalone read-only inspection/extraction tools live in (it's a private repo, ask for access):
- **[Project-Americas-modding-and-documentation](https://github.com/Kabosune/Project-Americas-modding-and-documentation)**

## Scope
- Single-player / offline modding core and mod loader architecture.
- Strictly read-only against the user's local game install (`--game-dir` / `GTA6_DIR`, default `D:\Games\GTA6`). All scratch/patched outputs go under `--out` (default `D:\GTA6-work\fireball-fpv-test`).
- No DRM/encryption keys, decrypted game executables, copyrighted game assets, or online/multiplayer/anti-cheat functionality.

## Repository Layout

- `CMakeLists.txt` — MSVC (VS 2022) + CMake build with strict C11 (`/std:c11 /W4 /WX`) & C++ (`/W4 /WX`) ABI compile checks, `fireball_core.dll`, `fpv_toggle.fbplugin.dll`, `fireball_mock_host.exe`, and CTest integration.
- `include/fireball_host.h` — Frozen pure-C11 Host <-> Fireball-Core ABI (`FB_HOST_ABI_VERSION = 1`), covering Tier 0/1/2 capabilities (`FB_CAP_*`), `on_file_open`, `on_dstorage_read`, `on_shader_load`, `on_frame_begin`/`on_frame_present`, `on_input_event`/`poll_key_state`, `resolve_symbol`, and guarded `title_mem_read_u32`/`title_mem_write_u32`.
- `include/fireball_plugin.h` — Frozen pure-C11 Native Plugin ABI (`FB_PLUGIN_ABI_VERSION = 1`) for `*.fbplugin.dll` plugins authored in C, C++, Rust, or Zig.
- `include/fireball_menu.h` — Additive pure-C11 Mod Menu, Typed Settings Registry, and Keyboard + Gamepad Chord Action Binding ABI (`FB_MENU_ABI_VERSION = 1`, optional `fb_plugin_register_menu` export; 100% backward compatible with v1 plugins).
- `include/fireball_console.h` — Additive pure-C11 Command Console ABI (`FB_CONSOLE_ABI_VERSION = 1`, optional `fb_plugin_register_console` export) driving both the in-game `` ` ``/`~` overlay console and optional local named-pipe terminal REPL (`fireball_cli.py console`).
- `src/core/` — Pure-C11 + Win32 native `fireball_core.dll` runtime (`fb_bundle.h/.c` O(1) `fireball_bundle.fbin` loader, `fb_plugins.h/.c` dynamic `*.fbplugin.dll` loader & capability gate, `fb_bindings.h/.c` keyboard/gamepad chord binding & rebind capture manager, `fb_settings.h/.c` typed mod settings & atomic `fireball_user_config.json` persistence, `fb_menu_ui.h/.c` focus-safe `WS_EX_NOACTIVATE` Win32 GDI overlay menu toggled with `F1` / `LB+View`, `fb_console.h/.c` command registry, `` ` ``/`~` overlay console, and local named-pipe server, and `fb_core.c` `fb_attach_host`, callbacks, and lock-free live boot telemetry).
- `bridges/victory-fireball/` — Zero-dependency drop-in Rust crate (`victory_fireball`) for optional dynamic loading of `fireball_core.dll` inside `Marvisak/VIctory` with end-to-end Rust integration tests.
- `examples/fpv_unlocker/` — End-to-end vertical-slice sample mod (`mod.toml`, XML/KV/PSO patch manifests, `plugins/fpv_toggle.py` reference state machine, and `native/fpv_toggle.c` native C11 `fpv_toggle.fbplugin.dll`).
- `examples/time_weather_control/` — Tier 2 single-player time & weather mod: `native/time_weather.c` + `time_weather_ui.c` → `time_weather.fbplugin.dll` (hotkeys `]` `[` `\` `;` `'`, F8 control window: time slider, ±1h, pause, time scale, weather list with set/blend/lock), `plugins/time_weather.py` reference state machine. Clock/weather addresses and layouts in `mod.toml` are placeholders until the research repo's live scan confirms them.
- `examples/state_scan/` — research helper `state_scan.fbplugin.dll`: F9/F10 request title-memory snapshots from VIctory's `VICTORY_SNAPSHOT` diagnostics hook, watched-RVA change log, F11 file-driven write tests with read-back.
- `fireball/` — Python 3.13 stdlib-only compiler & Phase 0 reference core (reuses `pso_decode.py` from the companion research repo via `--tools-dir` / `GTA6_MODDING_TOOLS`):
  - `abi.py` — Capability constants + static C11 header contract checker.
  - `mod_source.py` — Dual-mode `VirtualModSource` (loose folder + `.gta6mod` ZIP packer/reader).
  - `archive_reader.py` — Read-only streaming/seeking paged zlib reader (with per-64KiB-page offset metadata).
  - `mergers.py` — Additive XML & `visualsettings.dat` KV mergers with diffs, provenance, and `--size-preserve` compaction.
  - `pso_patcher.py` — RAGE 9 binary PSO field patcher, 7-section serializer, re-decoder verification, and empirical `CHKS +0x0C` probe.
  - `bundle.py` — Deterministic compiled patch bundle (`fireball_bundle.fbin`) compiler, inspector, serializer, and parser.
  - `mock_host.py` — `VIctory`-style Mock HLE Host + Tier 0-only graceful degradation suite.
- `fireball_cli.py` — Offline CLI & live console REPL (`pack`, `validate`, `apply`, `build-bundle`, `inspect-bundle`, `chks-probe`, `simulate`, `console`, `verify-all`).
- `tests/` — Offline Python `unittest` suite (`test_offline.py`), strict C11/C++ ABI checks (`abi_check_c11.c`, `abi_check_cpp.cpp`), and native `VIctory`-style mock host (`native/mock_host_test.c`).
- `docs/victory_bridge.md` — Minimal-diff `Marvisak/VIctory` dynamic-loading bridge spec and live smoke-test checklist.
- `docs/mod_menu_api.md` — In-game Mod Menu (`F1` / `LB+View`), typed settings, keyboard + gamepad chord rebinding, and `fireball_user_config.json` persistence spec.
- `docs/console_api.md` — In-game Command Console (`` ` `` / `~`), plugin command registration ABI, built-in commands, and `fireball_cli.py console` named-pipe REPL spec.

## Quick Start

```powershell
# Offline unit tests (no game dump required)
python -B -m unittest discover -s tests -v

# Native C11 build + CTest (MSVC VS 2022 + CMake)
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure

# Rust VIctory bridge crate tests (loads built fireball_core.dll + fpv_toggle.fbplugin.dll)
cargo test --manifest-path bridges/victory-fireball/Cargo.toml

# Full vertical-slice verification + fixed-buffer-safe compiled bundle against D:\Games\GTA6
python -B fireball_cli.py verify-all
python -B fireball_cli.py --game-dir D:\Games\GTA6 --out D:\GTA6-work\fireball-test\mods_sp build-bundle --size-preserve
python -B fireball_cli.py inspect-bundle D:\GTA6-work\fireball-test\mods_sp\fireball_bundle.fbin
```

## License

Licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE) or <https://www.apache.org/licenses/LICENSE-2.0>)
- MIT license ([LICENSE-MIT](LICENSE-MIT) or <https://opensource.org/licenses/MIT>)

at your option. This includes the C11 ABI headers in `include/`, so native plugins built against them may use any license.

### Contribution

Unless you explicitly state otherwise, any contribution intentionally submitted for inclusion in this work, as defined in the Apache-2.0 license, shall be dual licensed as above, without any additional terms or conditions.

This license covers Fireball-Core's own source code only. It grants no rights to any game, its assets, or other third-party material.
