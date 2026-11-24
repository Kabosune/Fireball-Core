/*
 * fireball_host.h — Fireball-Core Recomp/HLE Host <-> Mod Core C11 ABI
 *
 * Pure C11, stable ABI, zero C++ STL across the boundary.
 * Clean-room MIT / Apache-2.0 interface designed around Marvisak/VIctory
 * (Rust HLE runner) as well as C/C++/Zig hosts and standalone loaders.
 *
 * Strictly offline / single-player research interface.
 */

#ifndef FIREBALL_HOST_H
#define FIREBALL_HOST_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_HOST_ABI_VERSION 1u

#if defined(_WIN32)
#  if defined(FB_BUILD_DLL)
#    define FB_API __declspec(dllexport)
#  elif defined(FB_USE_DLL)
#    define FB_API __declspec(dllimport)
#  else
#    define FB_API
#  endif
#else
#  define FB_API __attribute__((visibility("default")))
#endif

/* Standard status / error codes returned across the C11 ABI */
typedef enum fb_status {
    FB_OK                =  0,
    FB_ERR_INVALID_ARG   = -1,
    FB_ERR_ABI_MISMATCH  = -2,
    FB_ERR_UNSUPPORTED   = -3,
    FB_ERR_NOT_FOUND     = -4,
    FB_ERR_MEM_FAULT     = -5,
    FB_ERR_PERMISSION    = -6,
    FB_ERR_INTERNAL      = -7
} fb_status_t;

/* Log severity levels */
typedef enum fb_log_level {
    FB_LOG_TRACE = 0,
    FB_LOG_DEBUG = 1,
    FB_LOG_INFO  = 2,
    FB_LOG_WARN  = 3,
    FB_LOG_ERROR = 4
} fb_log_level_t;

/*
 * Capability flags negotiated between host (e.g. VIctory) and Fireball-Core.
 * Typed as uint64_t for strict C11 portability (no C23 fixed-underlying enum).
 */
typedef uint64_t fb_cap_flags_t;

#define FB_CAP_NONE               (0ull)

/* Tier 0: Host / Scarlett GDK Shim hooks (zero game RVAs required) */
#define FB_CAP_IO_VIRTUAL         (1ull << 0)   /* Win32/GDK file open/stat/read virtualization */
#define FB_CAP_DSTORAGE_INTERCEPT (1ull << 1)   /* XCRDAPI DirectStorage request interception */
#define FB_CAP_D3D12_SHADERS      (1ull << 2)   /* d3d12_xs PSO / SGD5 shader override */
#define FB_CAP_D3D12_TEXTURES     (1ull << 3)   /* d3d12_xs / xg_xs texture upload & linear bypass */
#define FB_CAP_BVH_DXR_FALLBACK   (1ull << 4)   /* XboxBvhBuilder -> PC DXR BLAS build hook */
#define FB_CAP_XMEM_HOOKS         (1ull << 5)   /* xmem / __XMemAllocationHooks__ headroom */
#define FB_CAP_MEDIA_BINK_AUDIO   (1ull << 6)   /* Bink2Scarlett / Opus/XMA2 stream hook */
#define FB_CAP_FRAME_LIFECYCLE    (1ull << 7)   /* Frame begin / present / shutdown callbacks */
#define FB_CAP_INPUT_EVENTS       (1ull << 8)   /* Host keyboard/gamepad event & hotkey polling */

/* Tier 1: RAGE 9 VFS & Data Mount hooks (minimal host-published symbols) */
#define FB_CAP_RAGE_FIDEVICE      (1ull << 16)  /* common:/ platform:/ %s_*:/ extra:/ hooks */
#define FB_CAP_RAGE_DATAFILEMGR   (1ull << 17)  /* CDataFileMgr changeset / dlclist injection */
#define FB_CAP_RAGE_CRC_BYPASS    (1ull << 18)  /* commoncrc:/ & PSO CHKS offline override */

/* Tier 2: Engine Stores, Symbols & Guarded Title Memory (mature host maps) */
#define FB_CAP_RAGE_STORES        (1ull << 24)  /* 43 *Store registries (36 run starts + 7 tail-merged) */
#define FB_CAP_RAGE_SYMBOL_REG    (1ull << 25)  /* Named engine symbol / RVA lookup table */
#define FB_CAP_TITLE_MEM_RW       (1ull << 26)  /* Guarded title_mem_read_u32 / write_u32 */

/* Tier masks for quick capability diagnostics */
#define FB_TIER0_MASK (0x000000000000FFFFull)
#define FB_TIER1_MASK (0x0000000000FF0000ull)
#define FB_TIER2_MASK (0x00000000FF000000ull)

/* Virtualized file descriptor returned by on_file_open / on_rage_vfs_open */
typedef struct fb_io_override_desc {
    uint64_t virtual_size;
    void*    user_ctx;
    /* Returns bytes read (>= 0), or negative fb_status_t error code */
    int64_t (*read_at)(void* user_ctx, uint64_t offset, void* dst, uint64_t len);
    void    (*close)(void* user_ctx);
} fb_io_override_desc_t;

/* DirectStorage request view presented by XCRDAPI shim (e.g. victory-hle::modules::dstorage) */
typedef struct fb_dstorage_req_view {
    const wchar_t* file_path;      /* Normalized game-relative path */
    uint64_t       file_offset;    /* Byte offset in real or virtual file */
    uint32_t       src_size;
    uint32_t       uncomp_size;
    uint32_t       codec_hint;     /* 0=raw, 1=zlib, 2=bcpack (RDS5 codec_flags) */
    void*          dst_ptr;        /* Destination CPU/staging buffer in host shim */
    uint32_t       dst_size;
} fb_dstorage_req_view_t;

/* Input event kinds for Tier 0/2 hotkey and controller handling */
typedef enum fb_input_action {
    FB_INPUT_RELEASE = 0,
    FB_INPUT_PRESS   = 1,
    FB_INPUT_REPEAT  = 2
} fb_input_action_t;

typedef struct fb_input_event_v1 {
    uint32_t struct_size;
    uint32_t device_kind;          /* 0=keyboard (Win32 VK_*), 1=gamepad (XINPUT_GAMEPAD_*) */
    uint32_t code;                 /* Virtual key code or gamepad button mask */
    uint32_t action;               /* fb_input_action_t */
    uint32_t modifiers;            /* Bit0=Shift, Bit1=Ctrl, Bit2=Alt */
    uint32_t reserved;
    uint64_t timestamp_us;
} fb_input_event_v1_t;

/*
 * Host API passed into fb_attach_host() by the host (e.g. VIctory).
 * Optional function pointers MUST be NULL if their corresponding
 * FB_CAP_* bit is not set in `capabilities`.
 */
typedef struct fb_host_api_v1 {
    uint32_t       struct_size;
    uint32_t       abi_version;    /* Must equal FB_HOST_ABI_VERSION */
    const char*    host_name;      /* e.g. "VIctory-HLE" */
    const char*    host_version;
    const wchar_t* game_root_dir;  /* Read-only base dump root (e.g. D:\Games\GTA6) */
    const wchar_t* mods_root_dir;  /* User mods directory */
    fb_cap_flags_t capabilities;   /* Bitmask of FB_CAP_* */

    /* Logging & diagnostics provided by host */
    void  (*log)(int level, const char* subsystem, const char* msg);

    /* Optional Tier 1/2 symbol discovery (FB_CAP_RAGE_SYMBOL_REG)
     * Returns title virtual address (uint64_t cast as void* or 0/NULL if unknown). */
    void* (*resolve_symbol)(const char* symbol_name);

    /* Optional Tier 0 GPU/BVH helpers exposed by host graphics shim */
    int   (*request_dxr_blas_build)(uint64_t asset_h64, const void* mesh_desc);
    int   (*register_linear_tex_bypass)(uint64_t tex_h64, int enable_linear);

    /* Optional Tier 0/2 hotkey polling (FB_CAP_INPUT_EVENTS)
     * Returns 1 if key/button is currently held, 0 if not, <0 on error. */
    int   (*poll_key_state)(uint32_t device_kind, uint32_t code);

    /* Optional Tier 2 guarded title memory access (FB_CAP_TITLE_MEM_RW)
     * Compatible with VIctory-style HLE memory maps: validates mapped ranges
     * and page permissions before reading/writing, returning FB_OK or <0. */
    int   (*title_mem_read_u32)(uint64_t title_va, uint32_t* out_value);
    int   (*title_mem_write_u32)(uint64_t title_va, uint32_t value);
} fb_host_api_v1_t;

/*
 * Callbacks filled by Fireball-Core during fb_attach_host() and invoked by
 * the host at GDK shim / VFS / frame / input boundaries.
 */
typedef struct fb_modcore_callbacks_v1 {
    uint32_t struct_size;
    uint32_t abi_version;

    /* Tier 0: Host calls these at GDK shim boundaries */
    /* Return 1 if Fireball virtualizes this file, 0 to pass through to real OS file */
    int  (*on_file_open)(const wchar_t* rel_path, fb_io_override_desc_t* out_desc);
    /* Return 1 if Fireball fulfilled the DirectStorage read into req->dst_ptr */
    int  (*on_dstorage_read)(const fb_dstorage_req_view_t* req);
    /* Return 1 if Fireball supplies replacement DXIL bytecode (valid against PSV0) */
    int  (*on_shader_load)(const uint8_t container_blake3[32],
                           const uint8_t dxbc_digest[16],
                           const char*   effect_name_or_null,
                           const void**  out_dxil,
                           size_t*       out_size);
    /* Return 1 if Fireball instructs XboxBvhBuilder shim to build PC DXR BLAS */
    int  (*on_bvh_rehydrate)(uint64_t archetype_h64, uint64_t drawable_h64);
    /* Frame, input & shutdown lifecycle */
    void (*on_frame_begin)(uint64_t frame_index, double dt_sec);
    void (*on_frame_present)(void* d3d12_swapchain_or_null);
    /* Return 1 if a mod/plugin consumed the input event, 0 to pass to game */
    int  (*on_input_event)(const fb_input_event_v1_t* ev);
    void (*on_shutdown)(void);

    /* Tier 1: Host calls when RAGE 9 VFS / CDataFileMgr hooks are wired */
    int  (*on_rage_vfs_open)(const char* vfs_uri, fb_io_override_desc_t* out_desc);
    int  (*on_rage_datafile_register)(const char* file_type, const char* vfs_uri);
} fb_modcore_callbacks_v1_t;

/* Primary entry point exported by fireball_core.dll */
FB_API int fb_attach_host(
    const fb_host_api_v1_t*    host_api,
    fb_modcore_callbacks_v1_t* out_callbacks);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_HOST_H */
