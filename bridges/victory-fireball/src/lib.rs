//! `victory-fireball` — Zero-dependency optional dynamic-loading bridge between
//! `Marvisak/VIctory` and `fireball_core.dll` (pure C11 ABI v1).
//!
//! When `fireball_core.dll` is absent, `FireballBridge::try_load` returns `Ok(None)` and
//! the global one-line hook helpers degrade silently to no-ops.

pub mod ffi;

use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::fmt;
use std::path::{Path, PathBuf};
use std::sync::{Arc, OnceLock, RwLock};

use ffi::{
    FbAttachHostFn, FbDStorageReqView, FbHostApiV1, FbInputEventV1, FbIoOverrideDesc,
    FbModcoreCallbacksV1, HModule, FB_CAP_INPUT_EVENTS, FB_CAP_RAGE_SYMBOL_REG,
    FB_CAP_TITLE_MEM_RW, FB_ERR_INVALID_ARG, FB_ERR_UNSUPPORTED, FB_HOST_ABI_VERSION, FB_OK,
};

#[derive(Debug, Clone)]
pub enum BridgeError {
    LoadLibraryFailed { path: PathBuf, win32_error: u32 },
    MissingExport { export: &'static str },
    AttachFailed { status: i32 },
    InvalidConfig(&'static str),
}

impl fmt::Display for BridgeError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            BridgeError::LoadLibraryFailed { path, win32_error } => {
                write!(f, "LoadLibraryW failed for '{}': Win32 error {}", path.display(), win32_error)
            }
            BridgeError::MissingExport { export } => {
                write!(f, "fireball_core.dll missing required export '{export}'")
            }
            BridgeError::AttachFailed { status } => {
                write!(f, "fb_attach_host() returned error status {status}")
            }
            BridgeError::InvalidConfig(msg) => write!(f, "invalid Fireball host config: {msg}"),
        }
    }
}

impl std::error::Error for BridgeError {}

/// Configuration passed when attaching `VIctory` to `fireball_core.dll`.
#[derive(Debug, Clone)]
pub struct HostConfig {
    pub host_name: String,
    pub host_version: String,
    pub game_root_dir: PathBuf,
    pub mods_root_dir: PathBuf,
    /// Optional explicit path to `fireball_core.dll`. When `None`, searches:
    ///   1. `VICTORY_FIREBALL_DLL` environment variable
    ///   2. `<current_exe_dir>/fireball_core.dll`
    ///   3. `<mods_root_dir>/fireball_core.dll`
    pub dll_path: Option<PathBuf>,
    /// Bitmask of `ffi::FB_CAP_*` flags actually wired by the host.
    pub capabilities: u64,
}

impl HostConfig {
    pub fn tier0_default(game_root_dir: PathBuf, mods_root_dir: PathBuf) -> Self {
        Self {
            host_name: "VIctory-HLE".to_string(),
            host_version: "0.1.0".to_string(),
            game_root_dir,
            mods_root_dir,
            dll_path: None,
            capabilities: ffi::FB_CAP_IO_VIRTUAL
                | ffi::FB_CAP_DSTORAGE_INTERCEPT
                | ffi::FB_CAP_FRAME_LIFECYCLE
                | ffi::FB_CAP_INPUT_EVENTS,
        }
    }
}

/// Host services exposed to `fireball_core.dll` and Tier 2 native plugins.
pub trait HostHooks: Send + Sync {
    fn log(&self, _level: i32, _subsystem: &str, _msg: &str) {}
    fn resolve_symbol(&self, _symbol_name: &str) -> Option<u64> {
        None
    }
    fn poll_key_state(&self, _device_kind: u32, _code: u32) -> i32 {
        FB_ERR_UNSUPPORTED
    }
    fn title_mem_read_u32(&self, _title_va: u64) -> Result<u32, i32> {
        Err(FB_ERR_UNSUPPORTED)
    }
    fn title_mem_write_u32(&self, _title_va: u64, _value: u32) -> Result<(), i32> {
        Err(FB_ERR_UNSUPPORTED)
    }
}

/// Safe Rust DirectStorage read request view for `try_dstorage_read()`.
pub struct DStorageReq<'a> {
    pub file_path: &'a str,
    pub file_offset: u64,
    pub src_size: u32,
    pub uncomp_size: u32,
    pub codec_hint: u32,
    pub dst: &'a mut [u8],
}

/// Safe Rust input event for `dispatch_input_event()`.
#[derive(Debug, Copy, Clone)]
pub struct InputEvent {
    pub device_kind: u32,
    pub code: u32,
    pub action: u32,
    pub modifiers: u32,
    pub timestamp_us: u64,
}

/// RAII handle wrapping a virtualized loose-file overlay returned by `on_file_open`.
pub struct VirtualOverlayFile {
    desc: FbIoOverrideDesc,
}

// SAFETY: `fb_overlay_handle_t` in fireball_core.dll points to immutable bundle memory
// and `read_at`/`close` are thread-safe.
unsafe impl Send for VirtualOverlayFile {}
unsafe impl Sync for VirtualOverlayFile {}

impl VirtualOverlayFile {
    pub fn virtual_size(&self) -> u64 {
        self.desc.virtual_size
    }

    pub fn read_at(&self, offset: u64, dst: &mut [u8]) -> Result<usize, i32> {
        let Some(read_fn) = self.desc.read_at else {
            return Err(FB_ERR_UNSUPPORTED);
        };
        let rc = unsafe {
            read_fn(
                self.desc.user_ctx,
                offset,
                dst.as_mut_ptr() as *mut c_void,
                dst.len() as u64,
            )
        };
        if rc < 0 {
            Err(rc as i32)
        } else {
            Ok(rc as usize)
        }
    }
}

impl Drop for VirtualOverlayFile {
    fn drop(&mut self) {
        if let Some(close_fn) = self.desc.close.take() {
            unsafe {
                close_fn(self.desc.user_ctx);
            }
            self.desc.user_ctx = std::ptr::null_mut();
        }
    }
}

/* -------------------------------------------------------------------------
 * Static C11 trampolines forwarding into the active HostHooks trait object
 * ------------------------------------------------------------------------- */

static ACTIVE_HOOKS: OnceLock<RwLock<Option<Arc<dyn HostHooks>>>> = OnceLock::new();

fn hooks_slot() -> &'static RwLock<Option<Arc<dyn HostHooks>>> {
    ACTIVE_HOOKS.get_or_init(|| RwLock::new(None))
}

unsafe extern "C" fn trampoline_log(level: c_int, subsystem: *const c_char, msg: *const c_char) {
    let sub = if subsystem.is_null() {
        ""
    } else {
        CStr::from_ptr(subsystem).to_str().unwrap_or("<non-utf8>")
    };
    let text = if msg.is_null() {
        ""
    } else {
        CStr::from_ptr(msg).to_str().unwrap_or("<non-utf8>")
    };
    if let Ok(guard) = hooks_slot().read() {
        if let Some(h) = guard.as_ref() {
            h.log(level, sub, text);
        }
    }
}

unsafe extern "C" fn trampoline_resolve_symbol(symbol_name: *const c_char) -> *mut c_void {
    if symbol_name.is_null() {
        return std::ptr::null_mut();
    }
    let Ok(name) = CStr::from_ptr(symbol_name).to_str() else {
        return std::ptr::null_mut();
    };
    if let Ok(guard) = hooks_slot().read() {
        if let Some(h) = guard.as_ref() {
            if let Some(va) = h.resolve_symbol(name) {
                return va as usize as *mut c_void;
            }
        }
    }
    std::ptr::null_mut()
}

unsafe extern "C" fn trampoline_poll_key_state(device_kind: u32, code: u32) -> c_int {
    if let Ok(guard) = hooks_slot().read() {
        if let Some(h) = guard.as_ref() {
            return h.poll_key_state(device_kind, code);
        }
    }
    FB_ERR_UNSUPPORTED
}

unsafe extern "C" fn trampoline_title_mem_read_u32(title_va: u64, out_value: *mut u32) -> c_int {
    if out_value.is_null() {
        return FB_ERR_INVALID_ARG;
    }
    if let Ok(guard) = hooks_slot().read() {
        if let Some(h) = guard.as_ref() {
            return match h.title_mem_read_u32(title_va) {
                Ok(v) => {
                    *out_value = v;
                    FB_OK
                }
                Err(code) => code,
            };
        }
    }
    FB_ERR_UNSUPPORTED
}

unsafe extern "C" fn trampoline_title_mem_write_u32(title_va: u64, value: u32) -> c_int {
    if let Ok(guard) = hooks_slot().read() {
        if let Some(h) = guard.as_ref() {
            return match h.title_mem_write_u32(title_va, value) {
                Ok(()) => FB_OK,
                Err(code) => code,
            };
        }
    }
    FB_ERR_UNSUPPORTED
}

/* -------------------------------------------------------------------------
 * FireballBridge — dynamic DLL owner & safe wrapper
 * ------------------------------------------------------------------------- */

pub struct FireballBridge {
    module: HModule,
    callbacks: FbModcoreCallbacksV1,
    shutdown_called: bool,
    // Keep strings alive for the lifetime of the bridge (even though fireball_core copies them)
    _host_name: CString,
    _host_version: CString,
    _game_root_w: Vec<u16>,
    _mods_root_w: Vec<u16>,
}

// SAFETY: `fireball_core.dll` guards mutable state with Win32 SRWLOCK and atomics.
unsafe impl Send for FireballBridge {}
unsafe impl Sync for FireballBridge {}

fn discover_dll_path(config: &HostConfig) -> Option<PathBuf> {
    if let Some(explicit) = &config.dll_path {
        return explicit.is_file().then(|| explicit.clone());
    }
    if let Ok(env_p) = std::env::var("VICTORY_FIREBALL_DLL") {
        let p = PathBuf::from(env_p);
        if p.is_file() {
            return Some(p);
        }
    }
    if let Ok(exe) = std::env::current_exe() {
        if let Some(dir) = exe.parent() {
            let cand = dir.join("fireball_core.dll");
            if cand.is_file() {
                return Some(cand);
            }
        }
    }
    let in_mods = config.mods_root_dir.join("fireball_core.dll");
    if in_mods.is_file() {
        return Some(in_mods);
    }
    None
}

impl FireballBridge {
    /// Optionally load and attach `fireball_core.dll`.
    /// Returns `Ok(None)` when `fireball_core.dll` is not present (clean no-op degradation).
    pub fn try_load(config: HostConfig, hooks: Arc<dyn HostHooks>) -> Result<Option<Self>, BridgeError> {
        let Some(dll_path) = discover_dll_path(&config) else {
            return Ok(None);
        };
        Self::load_from_path(&dll_path, config, hooks).map(Some)
    }

    /// Load and attach `fireball_core.dll` from an explicit path.
    pub fn load_from_path(
        dll_path: &Path,
        config: HostConfig,
        hooks: Arc<dyn HostHooks>,
    ) -> Result<Self, BridgeError> {
        let host_name = CString::new(config.host_name.as_str())
            .map_err(|_| BridgeError::InvalidConfig("host_name contains interior NUL"))?;
        let host_version = CString::new(config.host_version.as_str())
            .map_err(|_| BridgeError::InvalidConfig("host_version contains interior NUL"))?;
        let game_root_w = ffi::to_wide_nul_path(&config.game_root_dir);
        let mods_root_w = ffi::to_wide_nul_path(&config.mods_root_dir);
        let dll_w = ffi::to_wide_nul_path(dll_path);

        let module = unsafe { ffi::LoadLibraryW(dll_w.as_ptr()) };
        if module.is_null() {
            let err = unsafe { ffi::GetLastError() };
            return Err(BridgeError::LoadLibraryFailed {
                path: dll_path.to_path_buf(),
                win32_error: err,
            });
        }

        let proc_name = b"fb_attach_host\0";
        let sym = unsafe { ffi::GetProcAddress(module, proc_name.as_ptr() as *const c_char) };
        if sym.is_null() {
            unsafe {
                ffi::FreeLibrary(module);
            }
            return Err(BridgeError::MissingExport {
                export: "fb_attach_host",
            });
        }
        let attach_fn: FbAttachHostFn = unsafe { std::mem::transmute(sym) };

        if let Ok(mut guard) = hooks_slot().write() {
            *guard = Some(hooks);
        }

        let caps = config.capabilities;
        let host_api = FbHostApiV1 {
            struct_size: std::mem::size_of::<FbHostApiV1>() as u32,
            abi_version: FB_HOST_ABI_VERSION,
            host_name: host_name.as_ptr(),
            host_version: host_version.as_ptr(),
            game_root_dir: game_root_w.as_ptr(),
            mods_root_dir: mods_root_w.as_ptr(),
            capabilities: caps,
            log: Some(trampoline_log),
            resolve_symbol: if (caps & FB_CAP_RAGE_SYMBOL_REG) != 0 {
                Some(trampoline_resolve_symbol)
            } else {
                None
            },
            request_dxr_blas_build: None,
            register_linear_tex_bypass: None,
            poll_key_state: if (caps & FB_CAP_INPUT_EVENTS) != 0 {
                Some(trampoline_poll_key_state)
            } else {
                None
            },
            title_mem_read_u32: if (caps & FB_CAP_TITLE_MEM_RW) != 0 {
                Some(trampoline_title_mem_read_u32)
            } else {
                None
            },
            title_mem_write_u32: if (caps & FB_CAP_TITLE_MEM_RW) != 0 {
                Some(trampoline_title_mem_write_u32)
            } else {
                None
            },
        };

        let mut callbacks = FbModcoreCallbacksV1::default();
        let rc = unsafe { attach_fn(&host_api, &mut callbacks) };
        if rc != FB_OK {
            if let Ok(mut guard) = hooks_slot().write() {
                *guard = None;
            }
            unsafe {
                ffi::FreeLibrary(module);
            }
            return Err(BridgeError::AttachFailed { status: rc });
        }

        Ok(Self {
            module,
            callbacks,
            shutdown_called: false,
            _host_name: host_name,
            _host_version: host_version,
            _game_root_w: game_root_w,
            _mods_root_w: mods_root_w,
        })
    }

    pub fn try_dstorage_read(&self, req: &mut DStorageReq<'_>) -> bool {
        let Some(cb) = self.callbacks.on_dstorage_read else {
            return false;
        };
        let path_w = ffi::to_wide_nul_str(req.file_path);
        let dst_size = u32::try_from(req.dst.len()).unwrap_or(u32::MAX);
        let view = FbDStorageReqView {
            file_path: path_w.as_ptr(),
            file_offset: req.file_offset,
            src_size: req.src_size,
            uncomp_size: req.uncomp_size,
            codec_hint: req.codec_hint,
            dst_ptr: req.dst.as_mut_ptr() as *mut c_void,
            dst_size,
        };
        unsafe { cb(&view) == 1 }
    }

    pub fn try_file_open(&self, rel_path: &str) -> Option<VirtualOverlayFile> {
        let cb = self.callbacks.on_file_open?;
        let path_w = ffi::to_wide_nul_str(rel_path);
        let mut desc = FbIoOverrideDesc::default();
        let hit = unsafe { cb(path_w.as_ptr(), &mut desc) };
        if hit == 1 {
            Some(VirtualOverlayFile { desc })
        } else {
            None
        }
    }

    pub fn try_shader_load(
        &self,
        container_blake3: &[u8; 32],
        dxbc_digest: &[u8; 16],
        effect_name: Option<&str>,
    ) -> Option<Vec<u8>> {
        let cb = self.callbacks.on_shader_load?;
        let effect_cstr = effect_name.and_then(|s| CString::new(s).ok());
        let effect_ptr = effect_cstr
            .as_ref()
            .map_or(std::ptr::null(), |c| c.as_ptr());
        let mut out_dxil: *const c_void = std::ptr::null();
        let mut out_size: usize = 0;
        let hit = unsafe {
            cb(
                container_blake3.as_ptr(),
                dxbc_digest.as_ptr(),
                effect_ptr,
                &mut out_dxil,
                &mut out_size,
            )
        };
        if hit == 1 {
            if out_size == 0 {
                return Some(Vec::new());
            }
            if out_dxil.is_null() {
                return None;
            }
            let bytes = unsafe { std::slice::from_raw_parts(out_dxil as *const u8, out_size) };
            Some(bytes.to_vec())
        } else {
            None
        }
    }

    pub fn dispatch_input_event(&self, ev: &InputEvent) -> bool {
        let Some(cb) = self.callbacks.on_input_event else {
            return false;
        };
        let raw = FbInputEventV1 {
            struct_size: std::mem::size_of::<FbInputEventV1>() as u32,
            device_kind: ev.device_kind,
            code: ev.code,
            action: ev.action,
            modifiers: ev.modifiers,
            reserved: 0,
            timestamp_us: ev.timestamp_us,
        };
        unsafe { cb(&raw) != 0 }
    }

    pub fn on_frame_begin(&self, frame_index: u64, dt_sec: f64) {
        if let Some(cb) = self.callbacks.on_frame_begin {
            unsafe {
                cb(frame_index, dt_sec);
            }
        }
    }

    pub fn on_frame_present(&self, swapchain_ptr: *mut c_void) {
        if let Some(cb) = self.callbacks.on_frame_present {
            unsafe {
                cb(swapchain_ptr);
            }
        }
    }

    pub fn shutdown(&mut self) {
        if !self.shutdown_called {
            if let Some(cb) = self.callbacks.on_shutdown {
                unsafe {
                    cb();
                }
            }
            self.shutdown_called = true;
            if let Ok(mut guard) = hooks_slot().write() {
                *guard = None;
            }
        }
    }
}

impl Drop for FireballBridge {
    fn drop(&mut self) {
        self.shutdown();
        if !self.module.is_null() {
            unsafe {
                ffi::FreeLibrary(self.module);
            }
            self.module = std::ptr::null_mut();
        }
    }
}

/* -------------------------------------------------------------------------
 * Global singleton one-liners for tiny VIctory hook call sites
 * ------------------------------------------------------------------------- */

static GLOBAL_BRIDGE: OnceLock<RwLock<Option<FireballBridge>>> = OnceLock::new();

fn global_slot() -> &'static RwLock<Option<FireballBridge>> {
    GLOBAL_BRIDGE.get_or_init(|| RwLock::new(None))
}

/// Initialize the global optional bridge. Returns `Ok(true)` if `fireball_core.dll` loaded,
/// or `Ok(false)` when absent (no-op mode).
pub fn init_global(config: HostConfig, hooks: Arc<dyn HostHooks>) -> Result<bool, BridgeError> {
    let maybe = FireballBridge::try_load(config, hooks)?;
    let loaded = maybe.is_some();
    if let Ok(mut guard) = global_slot().write() {
        *guard = maybe;
    }
    Ok(loaded)
}

pub fn try_dstorage_read(req: &mut DStorageReq<'_>) -> bool {
    if let Ok(guard) = global_slot().read() {
        if let Some(b) = guard.as_ref() {
            return b.try_dstorage_read(req);
        }
    }
    false
}

pub fn try_file_open(rel_path: &str) -> Option<VirtualOverlayFile> {
    if let Ok(guard) = global_slot().read() {
        if let Some(b) = guard.as_ref() {
            return b.try_file_open(rel_path);
        }
    }
    None
}

pub fn try_shader_load(
    container_blake3: &[u8; 32],
    dxbc_digest: &[u8; 16],
    effect_name: Option<&str>,
) -> Option<Vec<u8>> {
    if let Ok(guard) = global_slot().read() {
        if let Some(b) = guard.as_ref() {
            return b.try_shader_load(container_blake3, dxbc_digest, effect_name);
        }
    }
    None
}

pub fn dispatch_input_event(ev: &InputEvent) -> bool {
    if let Ok(guard) = global_slot().read() {
        if let Some(b) = guard.as_ref() {
            return b.dispatch_input_event(ev);
        }
    }
    false
}

pub fn on_frame_begin(frame_index: u64, dt_sec: f64) {
    if let Ok(guard) = global_slot().read() {
        if let Some(b) = guard.as_ref() {
            b.on_frame_begin(frame_index, dt_sec);
        }
    }
}

pub fn on_frame_present(swapchain_ptr: *mut c_void) {
    if let Ok(guard) = global_slot().read() {
        if let Some(b) = guard.as_ref() {
            b.on_frame_present(swapchain_ptr);
        }
    }
}

pub fn shutdown_global() {
    if let Ok(mut guard) = global_slot().write() {
        if let Some(mut b) = guard.take() {
            b.shutdown();
        }
    }
}
