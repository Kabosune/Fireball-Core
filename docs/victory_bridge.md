# Marvisak/VIctory Host Integration Spec (`fireball_core.dll`)

## Overview

This document specifies the minimal, optional dynamic-loading bridge between **[Marvisak/VIctory](https://github.com/marvisak/victory)** (Rust Xbox Series X|S HLE runner for *Grand Theft Auto VI*) and **Fireball-Core** (`fireball_core.dll`, frozen pure-C11 ABI v1 in [`include/fireball_host.h`](../include/fireball_host.h)).

### Design Goals
1. **Tiny upstream rebase footprint**: all FFI bindings and state live in one self-contained Rust module (`victory-hle::fireball_bridge`), wired into `VIctory` via ~5 single-line hook calls.
2. **Zero hard dependency**: `VIctory` loads `fireball_core.dll` dynamically at startup via `LoadLibraryW` only when present (or configured via `VICTORY_FIREBALL_DLL` / `victory.toml`). When absent, `VIctory` executes identically with zero overhead.
3. **Zero runtime XML/KV/PSO parsing**: mods are pre-compiled offline into a deterministic binary bundle (`fireball_bundle.fbin`) via `fireball_cli.py build-bundle`. `fireball_core.dll` serves pre-merged uncompressed payloads in O(1) time from memory.
4. **Strictly offline / read-only**: `VICTORY_GAME_DIR` (`D:\Games\GTA6`) is never modified. All bundles, plugins, and logs stay under the user's external mods directory (`mods_root_dir`).

---

## 1. Frozen Host ABI (`FB_HOST_ABI_VERSION = 1`)

No ABI extension or version bump was required for Phase 1: `include/fireball_host.h` (`FB_HOST_ABI_VERSION = 1`) already carries every field `VIctory`'s `dstorage.rs` request structure needs (`file_path`, `file_offset`, `src_size`, `uncomp_size`, `codec_hint`, `dst_ptr`, `dst_size`).

### Startup & Teardown Lifecycle
1. During `victory-hle` runtime initialization (after `VICTORY_GAME_DIR` and the mods directory are resolved, before the title entry point executes):
   - Resolve `fireball_core.dll` (next to `victory.exe`, in `mods_root_dir`, or via `VICTORY_FIREBALL_DLL`).
   - Call `LoadLibraryW` and `GetProcAddress("fb_attach_host")`.
   - Populate `fb_host_api_v1_t` with:
     - `struct_size = size_of::<FbHostApiV1>() as u32`
     - `abi_version = 1`
     - `host_name = c"VIctory-HLE".as_ptr()`
     - `game_root_dir` = UTF-16 NUL-terminated path to `VICTORY_GAME_DIR`
     - `mods_root_dir` = UTF-16 NUL-terminated path to external mods dir (containing `fireball_bundle.fbin` and optional `*.fbplugin.dll`)
     - `capabilities` = bitmask of **actually wired** `FB_CAP_*` hooks (optional function pointers MUST be `None`/`NULL` if their capability bit is unset)
   - Call `fb_attach_host(&host_api, &mut callbacks)`. On `FB_OK (0)`, store `callbacks` in a global `OnceLock<FireballBridge>`.
2. On clean shutdown (`victory-hle` teardown / `ExitProcess` path):
   - Invoke `(callbacks.on_shutdown)()`, which notifies `FB_EVENT_SHUTDOWN`, calls `fb_plugin_shutdown()` in reverse load order, frees `fireball_bundle.fbin`, and unloads plugin DLLs.

---

## 2. Exact Hook Contract Against `Marvisak/VIctory`

| VIctory Crate / Module | Fireball Callback / Host API | Capability Bit | Tier | Purpose & Return Contract |
| :--- | :--- | :--- | :-: | :--- |
| `victory-hle::modules::files` | `callbacks.on_file_open(rel_path, &mut out_desc)` | `FB_CAP_IO_VIRTUAL` (`1 << 0`) | 0 | Intercept loose file opens (`CreateFileW` / `NtCreateFile` shim). Pass normalized path relative to `VICTORY_GAME_DIR` (or full path; Fireball strips `game_root_dir` automatically). Returns `1` -> register virtual handle backed by `out_desc.{virtual_size, read_at, close}`; returns `0` -> normal OS file open. |
| `victory-hle::modules::dstorage` | `callbacks.on_dstorage_read(&req_view)` | `FB_CAP_DSTORAGE_INTERCEPT` (`1 << 1`) | 0 | Intercept `IDStorageQueueX::EnqueueRequest` / worker completion **before** host zlib/BCPack decompression. Returns `1` -> Fireball has written uncompressed patched bytes directly into `req_view.dst_ptr`, skip disk read & decompression; returns `0` -> normal `VIctory` read/decompress path. |
| `victory-hle::modules::gameinput` | `callbacks.on_input_event(&ev)` + `host_api.poll_key_state` | `FB_CAP_INPUT_EVENTS` (`1 << 8`) | 0 | Forward Win32 keyboard (`device_kind = 0`, `VK_*`) and XInput/GameInput button (`device_kind = 1`) transitions. If `on_input_event` returns `1`, suppress forwarding that keystroke to the title's `GameInput` queue so hotkeys (e.g. `'V'`) do not collide with bound game controls. |
| `victory-gpu` (frame/present boundary) | `callbacks.on_frame_begin(frame_idx, dt_sec)` & `callbacks.on_frame_present(swapchain_ptr)` | `FB_CAP_FRAME_LIFECYCLE` (`1 << 7`) | 0 | Call `on_frame_begin` once per title frame tick (or command-queue submission boundary) and `on_frame_present` right before/after `IDXGISwapChain::Present`. Drives per-frame plugin logic (e.g. `fpv_toggle` hold-guard). |
| `victory-loader` (optional Tier 2) | `host_api.resolve_symbol`, `host_api.title_mem_read_u32`, `host_api.title_mem_write_u32` | `FB_CAP_RAGE_SYMBOL_REG` (`1 << 25`) + `FB_CAP_TITLE_MEM_RW` (`1 << 26`) | 2 | Expose confirmed title RVAs by name and guarded 32-bit reads/writes validated against `victory-loader`'s mapped section table and page protections (`FB_ERR_MEM_FAULT` on unmapped VA, `FB_ERR_PERMISSION` on non-writable page). Omit these bits until RVAs are confirmed; Tier 2 plugins degrade and skip cleanly with host log warnings. |

---

## 3. DirectStorage Request Mapping (`victory-hle::modules::dstorage`)

In `victory-hle::modules::dstorage`, each queued `DSTORAGE_REQUEST` targets an opened archive file handle (e.g., `common.rpf`, `data.rpf`, `pack/story000/story000.rpf`) at a byte offset with a compressed source size, expected uncompressed size, codec flag, and destination buffer.

Map each file-source request into `fb_dstorage_req_view_t`:

| `fb_dstorage_req_view_t` field | Source in `VIctory` `dstorage.rs` | Notes |
| :--- | :--- | :--- |
| `file_path` | Game-relative UTF-16 path stored on the `IDStorageFileX` handle (e.g. `L"common.rpf"`) | Fireball normalizes case, slashes, leading `./`, and strips `game_root_dir` if full path is passed. |
| `file_offset` | `req.Source.File.Offset` (`u64`) | Matches **either** a full-asset `archive_offset` (e.g. `0x0003B000`) **or** an individual 64 KiB compressed chunk's `page_offset` (e.g. `0x0003BA30`). |
| `src_size` | `req.Source.File.Size` (`u32`) | Compressed byte count on disk (informational). |
| `uncomp_size` | `req.UncompressedSize` (`u32`) | **Primary disambiguation key** together with `(file_path, file_offset)`. Critical for multi-page assets where `page[0]` starts at the same offset as the full asset (e.g. `data.rpf @ 0x006A2000`: full asset = `69,132` B vs `page[0]` = `65,536` B). Pass `0` only for raw/unknown-size reads (falls back to `(file_path, file_offset)`). |
| `codec_hint` | Compression format (`0` = raw, `1` = zlib, `2` = BCPack) | Informational; bundle payloads are always uncompressed. |
| `dst_ptr` | Resolved destination CPU / staging buffer pointer | Must be writable for `dst_size` bytes. |
| `dst_size` | Available destination buffer capacity in bytes (`req.Destination.Memory.Size`) | Fireball strictly verifies `dst_size >= payload_size` before copying. If smaller, Fireball logs a warning, leaves `dst_ptr` untouched, and returns `0` (safe pass-through). |

### Important Buffer Sizing Note for Text Patches
- **Binary PSO patches** (e.g. `data.rpf @ 0x006C6000`, `0x0063F000`, `0x006A2000`) preserve exact byte length (`delta == 0`), so `payload_size == orig_uncomp_size` always fits `dst_size = uncomp_size`.
- **Shrunk text patches** (e.g. `lasso_data.meta` @ `common.rpf:0x0003BA30`: `61,103 -> 61,096` B, `-7` B) always fit `dst_size = uncomp_size`; Fireball zero-pads the remaining `uncomp_size - payload_size` bytes cleanly.
- **Grown text patches** (e.g. `visualsettings.dat` @ `common.rpf:0x00643650`: `51,640 -> 51,643` B, `+3` B): pass `--size-preserve` to `fireball_cli.py build-bundle`! `--size-preserve` reclaims non-semantic whitespace/comments (for `visualsettings.dat`, collapsing 3 repeated `\t\t` separators into `\t` with zero deleted comments and 100% verified KV semantic parity) so `payload_size <= orig_uncomp_size` (`51,640 <= 51,640`) fits exact-size DirectStorage buffers natively. Verify fit anytime via `fireball_cli.py inspect-bundle <path.fbin>`.

---

## 4. Drop-In Rust Crate (`bridges/victory-fireball/`) & Exact `VIctory` Call Sites

Fireball-Core ships a ready-to-use, zero-dependency Rust crate in [`bridges/victory-fireball/`](../bridges/victory-fireball/) (`victory_fireball`) that implements the full `#[repr(C)]` ABI v1 bindings, `LoadLibraryW` optional loading (`VICTORY_FIREBALL_DLL` -> exe dir -> `mods_root_dir`), RAII `VirtualOverlayFile`, and thread-safe global one-liner helpers.

### 1) `Cargo.toml` (optional path dependency in `victory-hle/Cargo.toml`)
```toml
[dependencies]
victory-fireball = { path = "../../fireball-core/bridges/victory-fireball" }
```
*(Zero external crates are pulled in — `victory-fireball` uses pure `std` + raw `kernel32` Win32 externs.)*

### 2) Startup & Teardown (`victory-hle` runtime init / exit)
```rust
let cfg = victory_fireball::HostConfig::tier0_default(game_root_dir.clone(), mods_root_dir.clone());
let _ = victory_fireball::init_global(cfg, Arc::new(VictoryFireballHooks::new()));
// On clean runtime exit (flushes live boot telemetry summary + unloads plugins):
victory_fireball::shutdown_global();
```

### 3) The 4 One-Line Hook Call Sites in `Marvisak/VIctory`
- **`victory-hle::modules::files` (`files.rs`)** — loose-file overlay virtualization:
  ```rust
  if let Some(vfile) = victory_fireball::try_file_open(&rel_path) {
      return Ok(register_virtual_handle(vfile));
  }
  ```
- **`victory-hle::modules::dstorage` (`dstorage.rs`)** — pre-decompression DirectStorage interception:
  ```rust
  let mut fb_req = victory_fireball::DStorageReq {
      file_path: &archive_rel_path,
      file_offset: req.file_offset,
      src_size: req.src_size,
      uncomp_size: req.uncomp_size,
      codec_hint: req.codec_hint,
      dst: dst_slice,
  };
  if victory_fireball::try_dstorage_read(&mut fb_req) {
      return Ok(());
  }
  ```
- **`victory-hle::modules::gameinput` (`gameinput.rs`)** — hotkey interception:
  ```rust
  if victory_fireball::dispatch_input_event(&fb_input_ev) {
      return; // consumed by active Fireball plugin
  }
  ```
- **`victory-gpu` (frame/present boundary)**:
  ```rust
  victory_fireball::on_frame_begin(frame_idx, dt_sec);
  victory_fireball::on_frame_present(swapchain_ptr);
  ```

---

## 5. First Live `VIctory` Smoke-Test Checklist

Execute these phases in order so each layer is isolated before testing the next:

### Phase A — Build & Stage Artifacts (Read-Only Against `D:\Games\GTA6`)
- [ ] Build `fireball_core.dll` and `fpv_toggle.fbplugin.dll` in Release (`cmake --build build --config Release`).
- [ ] Compile a fixed-buffer-safe bundle into a scratch mods directory outside `D:\Games\GTA6` and verify all entries report `FitsDst=True`:
  ```powershell
  python -B fireball_cli.py --game-dir D:\Games\GTA6 --out D:\GTA6-work\fireball-test\mods build-bundle --size-preserve
  python -B fireball_cli.py inspect-bundle D:\GTA6-work\fireball-test\mods\fireball_bundle.fbin
  ```
- [ ] Copy `build/Release/fireball_core.dll` next to `victory.exe` (or point `VICTORY_FIREBALL_DLL` to it) and copy `build/Release/fpv_toggle.fbplugin.dll` into `D:\GTA6-work\fireball-test\mods\`.
- [ ] Boot `VIctory` with Tier 0 capabilities only (`FB_CAP_IO_VIRTUAL | FB_CAP_DSTORAGE_INTERCEPT | FB_CAP_FRAME_LIFECYCLE`) and verify logs show:
  - `Attached to host 'VIctory-HLE' ...`
  - `Loaded fireball_bundle.fbin (...): 8 DirectStorage entries, ...`
  - `Skipped plugin 'fpv_toggle' ... lacks required capabilities [FB_CAP_INPUT_EVENTS | FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW]` (clean Tier 0 degradation, zero crashes).
  - On shutdown: `Telemetry summary: dstorage_calls=... dstorage_hits=... dstorage_buf_too_small=0 | ...` plus `First DirectStorage hit: ...` lines showing which archive offsets actually triggered!

### Phase B — Text Override Smoke Test (`visualsettings.dat` + `lasso_data.meta`)
- [ ] Watch `First DirectStorage hit:` logs emitted by `fireball_core.dll` for `common.rpf`.
- [ ] Confirm whether `game_scarlett_master.exe` requests `common.rpf` by full asset offset (`0x0003B000` / `0x00640000`) or per-64KiB-page offset (`0x0003BA30` / `0x00643650`), and record the exact `orig`, `patched`, and `dst` sizes logged.
- [ ] Verify `lasso_data.meta` (`common.rpf @ 0x0003BA30`, `61,103 -> 61,096` B) returns `1` from `on_dstorage_read` and parses without error in RAGE's XML loader.
- [ ] Verify `visualsettings.dat` (`common.rpf @ 0x00643650`, compiled with `--size-preserve` to exact `51,640` B, mounted as `commoncrc:/data/visualsettings`):
  - Confirm `on_dstorage_read` hits with `dstorage_buf_too_small=0`.
  - Record empirically whether `commoncrc:/` rejects modified `visualsettings.dat` or accepts it without a CRC bypass.

### Phase C — Binary Camera PSO Smoke Test (`CHKS +0x0C` Verification)
- [ ] Observe `First DirectStorage hit:` logs for `data.rpf` (`0x006C6000` `54,804` B `view_mode` enum `0 -> 2`, `0x0063F000` `16,856` B `camFreeCameraMetadata`, and `0x006A2000` `69,132` B / `page[0]` `65,536` B `CAIConditionEquippedWeaponHasScope`).
- [ ] **Key empirical question**: observe whether the title's PSO deserializer accepts the patched `0x006C6000` PSO with the original `CHKS +0x0C` word intact, or rejects/asserts on checksum mismatch:
  - **If accepted**: Tier 0 DirectStorage interception alone is sufficient for binary PSO mods — no `CHKS` bypass needed!
  - **If rejected**: confirms Tier 1 `FB_CAP_RAGE_CRC_BYPASS` (or solving `CHKS +0x0C`) is required for modified PSOs.

### Phase D — Tier 2 Runtime Plugin Smoke Test (`fpv_toggle.fbplugin.dll`)
- [ ] Wire `victory-hle::modules::gameinput` (`FB_CAP_INPUT_EVENTS`) and `victory-loader` guarded memory (`FB_CAP_RAGE_SYMBOL_REG` + `FB_CAP_TITLE_MEM_RW`) once `rage::camGameplayDirector::s_Instance` RVA and `view_mode_field_offset` are confirmed.
- [ ] Confirm `fpv_toggle.fbplugin.dll` initializes cleanly, `'V'` (`0x56`) toggles `0 <-> 2`, hold-guard re-asserts `2` on context resets, and shutdown restores `0`.
