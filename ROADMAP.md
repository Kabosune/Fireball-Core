# ROADMAP

The lasting plan and status for Fireball-Core. Rules and constraints live in `AGENTS.md` and aren't repeated here.

**How to use:** start a session with "read ROADMAP.md and do the next phase". Agents tick boxes as they go and fill in the phase's **Handoff** notes before stopping (see `AGENTS.md` → Workflow).

Status values: `Done` · `In progress` · `Next` · `Planned` · `Idea`

---

## Done

| Phase | Status | Commit | Summary |
| :--- | :--- | :--- | :--- |
| 0 | Done | `3d620ef` | C11 ABI, Python prototype core, `fpv_unlocker` vertical slice |
| 1 | Done | `cf2d18f` | Native C11 `fireball_core.dll`, compiled patch bundle, `fpv_toggle` plugin, VIctory bridge spec |
| 2 | Done | `e97fc28` | Rust `victory-fireball` bridge crate, `--size-preserve` bundles, `inspect-bundle`, live telemetry |
| 2.1 | Done | `78c1420` | `time_weather_control` Tier 2 example mod + `state_scan` research helper |
| 2.2 | Done | `0261017` | In-game mod menu, typed settings registry, keyboard/gamepad chord bindings (`FB_MENU_ABI_VERSION = 1`) |
| 2.3 | Done | `8831023` | Command console ABI, `~` overlay, local named-pipe REPL (`FB_CONSOLE_ABI_VERSION = 1`) |

---

## Phase 3 — Multi-mod load order, `on_shader_load` DXIL table, `stage` command, CI

**Status:** Done · **Base:** `dcef1e2`

### 1. Multi-mod load-order resolver, conflict report, `stage` CLI
Files: `fireball/load_order.py` (new), `fireball/bundle.py`, `fireball_cli.py`
- [x] Deterministic topological sort of multiple loose/`.gta6mod` mods, honoring `requires`, `load_after`, and `conflicts` from `mod.toml`.
- [x] Clear error reporting for: cycles, missing dependencies, duplicate mod ids, declared conflicts.
- [x] `load_order.toml` profile support: explicit ordering, enable/disable flags, profile-level overrides.
- [x] `fireball_cli.py stage` subcommand:
  - [x] Resolves load order from `--mod` arguments and/or `--profile <load_order.toml>`.
  - [x] Builds `fireball_bundle.fbin` (supports `--size-preserve`) plus provenance JSON into `--out`.
  - [x] Copies native `*.fbplugin.dll` binaries (found in mod folders and/or `--plugin-dir`, e.g. `build/Release`) into `--out`.
  - [x] Optionally copies `fireball_core.dll` when `--core-dll` is given.
  - [x] Prints and writes to JSON a field-level conflict/override summary: every XML/KV/PSO field, overlay, or shader entry touched by more than one mod, and which mod won in load order.

### 2. Native `on_shader_load` DXIL override table
Files: `fireball/bundle.py`, `src/core/fb_bundle.h/.c`, `src/core/fb_core.c`, `bridges/victory-fireball/src/lib.rs` (+ `ffi.rs` if needed)
- [x] Deterministic shader DXIL override section in `fireball_bundle.fbin`, keyed by `(container_blake3[32], dxbc_digest[16])`. Either a v1-compatible addition or a clean v2 extension. **v1 bundles must still load.**
- [x] Wire `on_shader_load(container_blake3[32], dxbc_digest[16], effect_name_or_null, out_dxil, out_size)` in `fb_core.c` with:
  - [x] O(1) hash lookup.
  - [x] First-hit logging.
  - [x] Lock-free atomic counters `shader_calls`, `shader_hits`, reported in shutdown telemetry and in the existing menu/console status views.
- [x] Safe Rust wrapper: `try_shader_load` on `FireballBridge` plus a global helper.
- [x] End-to-end round-trip test (bundle compile → inspect → native lookup hit/miss + telemetry) in:
  - [x] `tests/native/mock_host_test.c`
  - [x] `bridges/victory-fireball/tests/integration_test.rs`
  - [x] `tests/test_offline.py`

### 3. GitHub Actions CI
File: `.github/workflows/ci.yml`. Runs on `windows-latest`, triggered by `push` and `pull_request`.
- [x] CMake configure, then Release and Debug builds (MSVC C `/std:c11 /W4 /WX`, C++ `/W4 /WX`), then `ctest --output-on-failure` for both configs. This covers all four native tests.
- [x] `cargo test --manifest-path bridges/victory-fireball/Cargo.toml` against the built `fireball_core.dll` and plugins.
- [x] `python -B -m unittest discover -s tests -v`, `python -B fireball_cli.py --help`, `python -B fireball_cli.py stage --help`.

### Verification (run and report the real output)
1. [x] CMake Release and Debug builds, plus `ctest --output-on-failure` for both.
2. [x] `cargo test --manifest-path bridges/victory-fireball/Cargo.toml`
3. [x] `python -B -m unittest discover -s tests -v`
4. [x] `fireball_cli.py stage --size-preserve`, staging `examples/fpv_unlocker`, `examples/time_weather_control`, and `examples/state_scan` with `--plugin-dir build/Release --core-dll build/Release/fireball_core.dll`, against `D:\Games\GTA6` into `D:\GTA6-work\fireball-test\staged`. Then `fireball_cli.py inspect-bundle` on the result.

Stop with a clean summary and `git status`. Don't commit or push.

### Handoff
- **Done:**
  - Added `fireball/load_order.py` (deterministic Kahn topological sort honoring `requires`, `load_after`, `conflicts`; dedicated errors for cycles, missing deps, duplicate IDs, and conflicts; `load_order.toml` profile support with explicit ordering, enable/disable, and per-mod patch/plugin_config overrides).
  - Extended `fireball/bundle.py` and `fireball/mod_source.py` with field-level/overlay/shader conflict tracking, profile override application, and clean v2 `fireball_bundle.fbin` shader DXIL override section (80-byte v2 header + 88-byte `fb_bundle_shader_entry_t` keyed by `(container_blake3[32], dxbc_digest[16])`; emits v1 64-byte bundles when no shaders are present, and readers load both v1 and v2).
  - Added `fireball_cli.py stage` subcommand (resolves load order, builds bundle + provenance + `fireball_conflicts.json` + `fireball_stage_report.json`, copies matching `*.fbplugin.dll` binaries from mod sources or `--plugin-dir`, optionally copies `--core-dll`, prints conflict summary) and updated `inspect-bundle` to display shader entries.
  - Wired native v1/v2 bundle loading and O(1) `on_shader_load` lookup in `src/core/fb_bundle.h/.c` and `src/core/fb_core.c` with first-hit logging, atomic `shader_calls`/`shader_hits` telemetry in shutdown logs, Mod Menu (`src/core/fb_menu_ui.h/.c`), and Console `stats` (`src/core/fb_console.c`).
  - Added safe Rust wrappers `FireballBridge::try_shader_load` and global `try_shader_load` in `bridges/victory-fireball/src/lib.rs`.
  - Added `.github/workflows/ci.yml` covering Release/Debug CMake + CTest, Cargo bridge tests, Python `unittest`, and CLI `--help` checks on `windows-latest`.
  - Added round-trip, v1-compat, load-order, and `stage` tests in `tests/native/mock_host_test.c`, `tests/native/console_unit_test.c`, `bridges/victory-fireball/tests/integration_test.rs`, and `tests/test_offline.py`.
- **Left:** User review and commit/push; decide next phase from Open questions / backlog below.
- **Gotchas:**
  - On this machine `cargo` lives in `%USERPROFILE%\.cargo\bin` (may not be on non-login PowerShell `PATH`).
  - All frozen C11 ABI headers in `include/` remain untouched (`FB_HOST_ABI_VERSION = 1`, etc.).
- **Verification:**
  - CMake Release & Debug builds (`/std:c11 /W4 /WX`, C++ `/W4 /WX`): passed cleanly.
  - `ctest --test-dir build -C Release --output-on-failure`: 4/4 passed.
  - `ctest --test-dir build -C Debug --output-on-failure`: 4/4 passed.
  - `cargo test --manifest-path bridges/victory-fireball/Cargo.toml`: 2/2 integration tests passed.
  - `python -B -m unittest discover -s tests -v`: 18/18 passed.
  - `fireball_cli.py stage --size-preserve` + `inspect-bundle` on `D:\GTA6-work\fireball-test\staged\fireball_bundle.fbin`: passed (v1, 564,832 B, 8 DirectStorage entries all `FitsDst=True`, 3 plugin configs, 3 native plugins + `fireball_core.dll` copied, 0 conflicts).

---

## Open questions / backlog
- `examples/time_weather_control`: clock/weather addresses and layouts in `mod.toml` are placeholders until a live scan (via `state_scan` / the research repo) confirms them.
- `C:\dev\fireball-mods` holds "upgraded" copies of `fpv_unlocker` and `time_weather_control`. Decide which copy is canonical, and whether the examples here should stay minimal SDK samples.
- After Phase 3: should fireball-mods' `scripts/stage_mods.py` call `fireball_cli.py stage` instead of keeping its own staging logic?
