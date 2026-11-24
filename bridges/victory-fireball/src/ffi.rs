//! Raw `#[repr(C)]` FFI bindings mirroring `include/fireball_host.h` (`FB_HOST_ABI_VERSION = 1`)
//! plus zero-dependency Win32 `LoadLibraryW` / `GetProcAddress` / `FreeLibrary` imports.

use std::ffi::{c_char, c_int, c_void};
use std::os::windows::ffi::OsStrExt;
use std::path::Path;

pub const FB_HOST_ABI_VERSION: u32 = 1;

/* Standard status codes (fb_status_t) */
pub const FB_OK: c_int = 0;
pub const FB_ERR_INVALID_ARG: c_int = -1;
pub const FB_ERR_ABI_MISMATCH: c_int = -2;
pub const FB_ERR_UNSUPPORTED: c_int = -3;
pub const FB_ERR_NOT_FOUND: c_int = -4;
pub const FB_ERR_MEM_FAULT: c_int = -5;
pub const FB_ERR_PERMISSION: c_int = -6;
pub const FB_ERR_INTERNAL: c_int = -7;

/* Log severity levels (fb_log_level_t) */
pub const FB_LOG_TRACE: c_int = 0;
pub const FB_LOG_DEBUG: c_int = 1;
pub const FB_LOG_INFO: c_int = 2;
pub const FB_LOG_WARN: c_int = 3;
pub const FB_LOG_ERROR: c_int = 4;

/* Capability flags (fb_cap_flags_t) */
pub const FB_CAP_NONE: u64 = 0;
pub const FB_CAP_IO_VIRTUAL: u64 = 1 << 0;
pub const FB_CAP_DSTORAGE_INTERCEPT: u64 = 1 << 1;
pub const FB_CAP_D3D12_SHADERS: u64 = 1 << 2;
pub const FB_CAP_D3D12_TEXTURES: u64 = 1 << 3;
pub const FB_CAP_BVH_DXR_FALLBACK: u64 = 1 << 4;
pub const FB_CAP_XMEM_HOOKS: u64 = 1 << 5;
pub const FB_CAP_MEDIA_BINK_AUDIO: u64 = 1 << 6;
pub const FB_CAP_FRAME_LIFECYCLE: u64 = 1 << 7;
pub const FB_CAP_INPUT_EVENTS: u64 = 1 << 8;

pub const FB_CAP_RAGE_FIDEVICE: u64 = 1 << 16;
pub const FB_CAP_RAGE_DATAFILEMGR: u64 = 1 << 17;
pub const FB_CAP_RAGE_CRC_BYPASS: u64 = 1 << 18;

pub const FB_CAP_RAGE_STORES: u64 = 1 << 24;
pub const FB_CAP_RAGE_SYMBOL_REG: u64 = 1 << 25;
pub const FB_CAP_TITLE_MEM_RW: u64 = 1 << 26;

pub const FB_TIER0_MASK: u64 = 0x0000_0000_0000_FFFF;
pub const FB_TIER1_MASK: u64 = 0x0000_0000_00FF_0000;
pub const FB_TIER2_MASK: u64 = 0x0000_0000_FF00_0000;

/* Input action constants (fb_input_action_t) */
pub const FB_INPUT_RELEASE: u32 = 0;
pub const FB_INPUT_PRESS: u32 = 1;
pub const FB_INPUT_REPEAT: u32 = 2;

/* Device kinds */
pub const FB_DEV_KEYBOARD: u32 = 0;
pub const FB_DEV_GAMEPAD: u32 = 1;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct FbIoOverrideDesc {
    pub virtual_size: u64,
    pub user_ctx: *mut c_void,
    pub read_at: Option<unsafe extern "C" fn(user_ctx: *mut c_void, offset: u64, dst: *mut c_void, len: u64) -> i64>,
    pub close: Option<unsafe extern "C" fn(user_ctx: *mut c_void)>,
}

impl Default for FbIoOverrideDesc {
    fn default() -> Self {
        Self {
            virtual_size: 0,
            user_ctx: std::ptr::null_mut(),
            read_at: None,
            close: None,
        }
    }
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct FbDStorageReqView {
    pub file_path: *const u16,
    pub file_offset: u64,
    pub src_size: u32,
    pub uncomp_size: u32,
    pub codec_hint: u32,
    pub dst_ptr: *mut c_void,
    pub dst_size: u32,
}

#[repr(C)]
#[derive(Copy, Clone, Debug, Default)]
pub struct FbInputEventV1 {
    pub struct_size: u32,
    pub device_kind: u32,
    pub code: u32,
    pub action: u32,
    pub modifiers: u32,
    pub reserved: u32,
    pub timestamp_us: u64,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct FbHostApiV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub host_name: *const c_char,
    pub host_version: *const c_char,
    pub game_root_dir: *const u16,
    pub mods_root_dir: *const u16,
    pub capabilities: u64,
    pub log: Option<unsafe extern "C" fn(level: c_int, subsystem: *const c_char, msg: *const c_char)>,
    pub resolve_symbol: Option<unsafe extern "C" fn(symbol_name: *const c_char) -> *mut c_void>,
    pub request_dxr_blas_build: Option<unsafe extern "C" fn(asset_h64: u64, mesh_desc: *const c_void) -> c_int>,
    pub register_linear_tex_bypass: Option<unsafe extern "C" fn(tex_h64: u64, enable_linear: c_int) -> c_int>,
    pub poll_key_state: Option<unsafe extern "C" fn(device_kind: u32, code: u32) -> c_int>,
    pub title_mem_read_u32: Option<unsafe extern "C" fn(title_va: u64, out_value: *mut u32) -> c_int>,
    pub title_mem_write_u32: Option<unsafe extern "C" fn(title_va: u64, value: u32) -> c_int>,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct FbModcoreCallbacksV1 {
    pub struct_size: u32,
    pub abi_version: u32,
    pub on_file_open: Option<unsafe extern "C" fn(rel_path: *const u16, out_desc: *mut FbIoOverrideDesc) -> c_int>,
    pub on_dstorage_read: Option<unsafe extern "C" fn(req: *const FbDStorageReqView) -> c_int>,
    pub on_shader_load: Option<
        unsafe extern "C" fn(
            container_blake3: *const u8,
            dxbc_digest: *const u8,
            effect_name_or_null: *const c_char,
            out_dxil: *mut *const c_void,
            out_size: *mut usize,
        ) -> c_int,
    >,
    pub on_bvh_rehydrate: Option<unsafe extern "C" fn(archetype_h64: u64, drawable_h64: u64) -> c_int>,
    pub on_frame_begin: Option<unsafe extern "C" fn(frame_index: u64, dt_sec: f64)>,
    pub on_frame_present: Option<unsafe extern "C" fn(d3d12_swapchain_or_null: *mut c_void)>,
    pub on_input_event: Option<unsafe extern "C" fn(ev: *const FbInputEventV1) -> c_int>,
    pub on_shutdown: Option<unsafe extern "C" fn()>,
    pub on_rage_vfs_open: Option<unsafe extern "C" fn(vfs_uri: *const c_char, out_desc: *mut FbIoOverrideDesc) -> c_int>,
    pub on_rage_datafile_register: Option<unsafe extern "C" fn(file_type: *const c_char, vfs_uri: *const c_char) -> c_int>,
}

impl Default for FbModcoreCallbacksV1 {
    fn default() -> Self {
        Self {
            struct_size: std::mem::size_of::<Self>() as u32,
            abi_version: 0,
            on_file_open: None,
            on_dstorage_read: None,
            on_shader_load: None,
            on_bvh_rehydrate: None,
            on_frame_begin: None,
            on_frame_present: None,
            on_input_event: None,
            on_shutdown: None,
            on_rage_vfs_open: None,
            on_rage_datafile_register: None,
        }
    }
}

pub type FbAttachHostFn =
    unsafe extern "C" fn(host_api: *const FbHostApiV1, out_callbacks: *mut FbModcoreCallbacksV1) -> c_int;

pub type HModule = *mut c_void;

#[cfg(windows)]
#[link(name = "kernel32")]
extern "system" {
    pub fn LoadLibraryW(lp_lib_file_name: *const u16) -> HModule;
    pub fn GetProcAddress(h_module: HModule, lp_proc_name: *const c_char) -> *const c_void;
    pub fn FreeLibrary(h_lib_module: HModule) -> c_int;
    pub fn GetLastError() -> u32;
}

pub fn to_wide_nul_str(s: &str) -> Vec<u16> {
    s.encode_utf16().chain(std::iter::once(0)).collect()
}

pub fn to_wide_nul_path(p: &Path) -> Vec<u16> {
    p.as_os_str().encode_wide().chain(std::iter::once(0)).collect()
}
