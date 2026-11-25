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

**Status:** Next · **Base:** `8831023`

### 1. Multi-mod load-order resolver, conflict report, `stage` CLI
Files: `fireball/load_order.py` (new), `fireball/bundle.py`, `fireball_cli.py`
- [ ] Deterministic topological sort of multiple loose/`.gta6mod` mods, honoring `requires`, `load_after`, and `conflicts` from `mod.toml`.
- [ ] Clear error reporting for: cycles, missing dependencies, duplicate mod ids, declared conflicts.
- [ ] `load_order.toml` profile support: explicit ordering, enable/disable flags, profile-level overrides.
- [ ] `fireball_cli.py stage` subcommand:
  - [ ] Resolves load order from `--mod` arguments and/or `--profile <load_order.toml>`.
  - [ ] Builds `fireball_bundle.fbin` (supports `--size-preserve`) plus provenance JSON into `--out`.
  - [ ] Copies native `*.fbplugin.dll` binaries (found in mod folders and/or `--plugin-dir`, e.g. `build/Release`) into `--out`.
  - [ ] Optionally copies `fireball_core.dll` when `--core-dll` is given.
  - [ ] Prints and writes to JSON a field-level conflict/override summary: every XML/KV/PSO field, overlay, or shader entry touched by more than one mod, and which mod won in load order.

### 2. Native `on_shader_load` DXIL override table
Files: `fireball/bundle.py`, `src/core/fb_bundle.h/.c`, `src/core/fb_core.c`, `bridges/victory-fireball/src/lib.rs` (+ `ffi.rs` if needed)
- [ ] Deterministic shader DXIL override section in `fireball_bundle.fbin`, keyed by `(container_blake3[32], dxbc_digest[16])`. Either a v1-compatible addition or a clean v2 extension. **v1 bundles must still load.**
- [ ] Wire `on_shader_load(container_blake3[32], dxbc_digest[16], effect_name_or_null, out_dxil, out_size)` in `fb_core.c` with:
  - [ ] O(1) hash lookup.
  - [ ] First-hit logging.
  - [ ] Lock-free atomic counters `shader_calls`, `shader_hits`, reported in shutdown telemetry and in the existing menu/console status views.
- [ ] Safe Rust wrapper: `try_shader_load` on `FireballBridge` plus a global helper.
- [ ] End-to-end round-trip test (bundle compile → inspect → native lookup hit/miss + telemetry) in:
  - [ ] `tests/native/mock_host_test.c`
  - [ ] `bridges/victory-fireball/tests/integration_test.rs`
  - [ ] `tests/test_offline.py`

### 3. GitHub Actions CI
File: `.github/workflows/ci.yml`. Runs on `windows-latest`, triggered by `push` and `pull_request`.
- [ ] CMake configure, then Release and Debug builds (MSVC C `/std:c11 /W4 /WX`, C++ `/W4 /WX`), then `ctest --output-on-failure` for both configs. This covers all four native tests.
- [ ] `cargo test --manifest-path bridges/victory-fireball/Cargo.toml` against the built `fireball_core.dll` and plugins.
- [ ] `python -B -m unittest discover -s tests -v`, `python -B fireball_cli.py --help`, `python -B fireball_cli.py stage --help`.

### Verification (run and report the real output)
1. [ ] CMake Release and Debug builds, plus `ctest --output-on-failure` for both.
2. [ ] `cargo test --manifest-path bridges/victory-fireball/Cargo.toml`
3. [ ] `python -B -m unittest discover -s tests -v`
4. [ ] `fireball_cli.py stage --size-preserve`, staging `examples/fpv_unlocker`, `examples/time_weather_control`, and `examples/state_scan` with `--plugin-dir build/Release --core-dll build/Release/fireball_core.dll`, against `D:\Games\GTA6` into `D:\GTA6-work\fireball-test\staged`. Then `fireball_cli.py inspect-bundle` on the result.

Stop with a clean summary and `git status`. Don't commit or push.

### Handoff
_(Filled in by the agent at the end of each session working on this phase.)_

---

## Open questions / backlog
- `examples/time_weather_control`: clock/weather addresses and layouts in `mod.toml` are placeholders until a live scan (via `state_scan` / the research repo) confirms them.
- `C:\dev\fireball-mods` holds "upgraded" copies of `fpv_unlocker` and `time_weather_control`. Decide which copy is canonical, and whether the examples here should stay minimal SDK samples.
- After Phase 3: should fireball-mods' `scripts/stage_mods.py` call `fireball_cli.py stage` instead of keeping its own staging logic?
