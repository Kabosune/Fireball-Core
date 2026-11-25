# AGENTS.md — shared rules for every AI agent in this repo

This file is the single source of truth for **all** agents (Claude, Antigravity/Gemini, or any other). Read it fully before doing anything, then read `ROADMAP.md`. `CLAUDE.md` just points here.

## Project

Fireball-Core: the mod core and mod loader for *GTA VI* (Project Americas / RAGE 9), designed to be loaded by [Marvisak/VIctory](https://github.com/marvisak/victory). It consists of:
- `fireball_core.dll`, a pure C11 + Win32 runtime (`src/core/`), behind frozen C11 ABIs (`include/`).
- An offline Python 3.13 compiler, CLI and reference core (`fireball/`, `fireball_cli.py`).
- A Rust bridge crate for VIctory (`bridges/victory-fireball/`).
- Example mods (`examples/`).

`README.md` describes the layout in detail.

### Related repos

| Repo | Path | Relationship |
| :--- | :--- | :--- |
| gta6-modding | `C:\dev\gta6-modding` | Format research and read-only tools. `fireball/pso_patcher.py` imports its `tools/` (`--tools-dir` / env `GTA6_MODDING_TOOLS`). Don't copy its parsers here. |
| fireball-mods | `C:\dev\fireball-mods` | Standalone mods built **only** against this repo's `include/` headers and `build/Release/fireball_core.dll`. Any change to `include/` or to the build output layout can break it. |

## Hard rules
- Strictly offline / single-player. No online/multiplayer, anti-cheat, or DRM-bypass work.
- `D:\Games\GTA6` is **read-only**. Never write, rename, or delete anything there.
- Never commit DRM/encryption keys, key material, decrypted executables, disassembly, game assets, dumps, or build outputs (see `.gitignore`).
- **Zero third-party dependencies** in C11, Python (stdlib only), and Rust (no crates).
- **Frozen ABIs.** Keep these backward-compatible. Never change existing struct layouts, function signatures, or enum values. Additions must be additive and optional (like the existing `fb_plugin_register_menu` / `fb_plugin_register_console` exports):
  - `include/fireball_host.h`: `FB_HOST_ABI_VERSION = 1`
  - `include/fireball_plugin.h`: `FB_PLUGIN_ABI_VERSION = 1`
  - `include/fireball_menu.h`: `FB_MENU_ABI_VERSION = 1`
  - `include/fireball_console.h`: `FB_CONSOLE_ABI_VERSION = 1`
- Compiled bundles (`fireball_bundle.fbin`) must stay loadable by older readers, or the format must be versioned so v1 bundles still load.
- **Do not commit or push unless the user explicitly asks.** Stop with a summary and `git status` for review.

## Paths

| What | Path | Rule |
| :--- | :--- | :--- |
| Game install | `D:\Games\GTA6` | Read-only. Tools take `--game-dir` (env `GTA6_DIR`, default this). |
| Scratch / test output | `D:\GTA6-work\fireball-test\...` | All generated bundles, staged mods, and diffs go here (`--out`). Never inside the repo. |
| Native build | `build\Release`, `build\Debug` | Git-ignored. |

## Code conventions
- C: pure C11, MSVC `/std:c11 /W4 /WX`. The C++ ABI check compiles with `/W4 /WX`. No warnings allowed.
- Python 3.13, stdlib only. Run with `python -B`. Every CLI subcommand must work for `--help` without the game present.
- Rust: zero-dependency crate. The FFI stays in `ffi.rs`, with safe wrappers in `lib.rs`.
- Deterministic outputs: the same inputs must produce byte-identical bundles and JSON.
- Stream or seek large archives. Never read whole RPFs into memory.

## Build & test

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
cmake --build build --config Debug
ctest --test-dir build -C Release --output-on-failure
ctest --test-dir build -C Debug --output-on-failure
cargo test --manifest-path bridges/victory-fireball/Cargo.toml   # needs the Release build
python -B -m unittest discover -s tests -v
python -B fireball_cli.py --help
```

Current native tests: `fireball_mock_host_native`, `fireball_plugin_examples_native`, `fireball_menu_system_native`, `fireball_console_unit_native`.

Report verification honestly: paste real command output, say what failed, and say what you skipped and why. Never claim a number you didn't get from a command.

## Workflow
1. Read `ROADMAP.md`. Work on the phase marked **Next** or **In progress**, unless the user says otherwise.
2. Tick checkboxes in `ROADMAP.md` as deliverables are done and verified.
3. Before ending a session, update `ROADMAP.md`:
   - The phase's status.
   - Its **Handoff** notes: what was done, what's left, gotchas, and the verification results.
   - If the user has agreed on what comes next, a spec for the next phase.

   This replaces writing a prompt for the next session. The user reviews the diff.
4. Only edit `AGENTS.md` / `CLAUDE.md` when the user asks.
