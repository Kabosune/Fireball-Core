use std::fs;
use std::mem::size_of;
use std::path::{Path, PathBuf};
use std::sync::{Arc, Mutex};

use victory_fireball::ffi::{
    FbDStorageReqView, FbHostApiV1, FbInputEventV1, FbIoOverrideDesc, FbModcoreCallbacksV1,
    FB_CAP_D3D12_SHADERS, FB_CAP_DSTORAGE_INTERCEPT, FB_CAP_FRAME_LIFECYCLE, FB_CAP_INPUT_EVENTS,
    FB_CAP_IO_VIRTUAL, FB_CAP_RAGE_CRC_BYPASS, FB_CAP_RAGE_DATAFILEMGR, FB_CAP_RAGE_FIDEVICE,
    FB_CAP_RAGE_SYMBOL_REG, FB_CAP_TITLE_MEM_RW, FB_DEV_KEYBOARD, FB_ERR_MEM_FAULT,
    FB_ERR_PERMISSION, FB_INPUT_PRESS, FB_INPUT_RELEASE, FB_LOG_WARN,
};
use victory_fireball::{
    dispatch_input_event, init_global, on_frame_begin, on_frame_present, shutdown_global,
    try_dstorage_read, try_file_open, try_shader_load, DStorageReq, FireballBridge, HostConfig,
    HostHooks, InputEvent,
};

const DIRECTOR_VA: u64 = 0x0000_7FF7_1234_0000;
const READONLY_VA: u64 = 0x0000_7FF7_1235_0000;
const UNMAPPED_VA: u64 = 0x0000_7FF7_DEAD_BEEF;

struct MockVictoryHooks {
    view_mode: Mutex<u32>,
    logs: Mutex<Vec<(i32, String, String)>>,
}

impl MockVictoryHooks {
    fn new(initial_view_mode: u32) -> Self {
        Self {
            view_mode: Mutex::new(initial_view_mode),
            logs: Mutex::new(Vec::new()),
        }
    }

    fn engine_read_u32(&self) -> u32 {
        *self.view_mode.lock().unwrap()
    }

    fn engine_write_u32(&self, v: u32) {
        *self.view_mode.lock().unwrap() = v;
    }

    fn has_log(&self, min_level: i32, needle1: &str, needle2: Option<&str>) -> bool {
        let logs = self.logs.lock().unwrap();
        logs.iter().any(|(lvl, _sub, msg)| {
            *lvl >= min_level && msg.contains(needle1) && needle2.map_or(true, |n2| msg.contains(n2))
        })
    }
}

impl HostHooks for MockVictoryHooks {
    fn log(&self, level: i32, subsystem: &str, msg: &str) {
        self.logs
            .lock()
            .unwrap()
            .push((level, subsystem.to_string(), msg.to_string()));
    }

    fn resolve_symbol(&self, symbol_name: &str) -> Option<u64> {
        (symbol_name == "rage::camGameplayDirector::s_Instance").then_some(DIRECTOR_VA)
    }

    fn poll_key_state(&self, _device_kind: u32, _code: u32) -> i32 {
        0
    }

    fn title_mem_read_u32(&self, title_va: u64) -> Result<u32, i32> {
        if title_va == DIRECTOR_VA {
            Ok(*self.view_mode.lock().unwrap())
        } else if title_va == READONLY_VA {
            Ok(0)
        } else {
            Err(FB_ERR_MEM_FAULT)
        }
    }

    fn title_mem_write_u32(&self, title_va: u64, value: u32) -> Result<(), i32> {
        if title_va == DIRECTOR_VA {
            *self.view_mode.lock().unwrap() = value;
            Ok(())
        } else if title_va == READONLY_VA {
            Err(FB_ERR_PERMISSION)
        } else {
            Err(FB_ERR_MEM_FAULT)
        }
    }
}

fn fnv1a_64(data: &[u8]) -> u64 {
    let mut h: u64 = 0xCBF2_9CE4_8422_2325;
    for &b in data {
        h ^= b as u64;
        h = h.wrapping_mul(0x0000_0100_0000_01B3);
    }
    h
}

fn ds_key_hash(path: &str, file_offset: u64, orig_uncomp: u32) -> u64 {
    let mut buf = Vec::with_capacity(path.len() + 13);
    buf.extend_from_slice(path.as_bytes());
    buf.push(0);
    buf.extend_from_slice(&file_offset.to_le_bytes());
    buf.extend_from_slice(&orig_uncomp.to_le_bytes());
    fnv1a_64(&buf)
}

fn crc32_ieee(data: &[u8]) -> u32 {
    let mut crc: u32 = 0xFFFF_FFFF;
    for &b in data {
        crc ^= b as u32;
        for _ in 0..8 {
            let mask = (crc & 1).wrapping_neg();
            crc = (crc >> 1) ^ (0xEDB8_8320 & mask);
        }
    }
    !crc
}

fn align16(v: usize) -> usize {
    (v + 15) & !15
}

fn shader_key_hash(b3: &[u8; 32], dxbc: &[u8; 16]) -> u64 {
    let mut buf = [0u8; 48];
    buf[..32].copy_from_slice(b3);
    buf[32..48].copy_from_slice(dxbc);
    fnv1a_64(&buf)
}

const SYNTH_SHADER_B3: [u8; 32] = [
    0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
    0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
];
const SYNTH_SHADER_DXBC: [u8; 16] = [
    0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0x07, 0x18,
    0x29, 0x3A, 0x4B, 0x5C, 0x6D, 0x7E, 0x8F, 0x90,
];
const SYNTH_SHADER_DXIL: &[u8] = b"DXBC\x01\x02\x03\x04DXIL\xDE\xAD\xBE\xEF\xCA\xFE\xBA\xBE";

fn write_synthetic_bundle(path: &Path, include_v2_shader: bool) {
    let ds_path = "data.rpf";
    let ov_path = "data/rust_overlay.txt";
    let mod_id = "community.fpv_unlocker";
    let cfg_json = r#"{"director_symbol":"rage::camGameplayDirector::s_Instance","first_person_mode":2,"hold_guard_enabled":true,"hotkey_vkey":86,"third_person_mode":0,"view_mode_field_offset":0}"#;
    let shader_name = "fx_rust_shader_ps";
    let pso_payload: &[u8] = b"PSIN\x00\x00\x00\x10\x02\x00\x00\x00\x59\xED\xD3\x71";
    let ov_payload: &[u8] = b"hello-from-rust-victory-fireball-overlay\n";

    let hdr_size = if include_v2_shader { 80usize } else { 64usize };
    let ds_off = hdr_size;
    let ov_off = ds_off + 64;
    let cfg_off = ov_off + 40;
    let sh_off = cfg_off + 32;
    let str_off = sh_off + if include_v2_shader { 88 } else { 0 };

    let off_ds_path = str_off;
    let off_ov_path = off_ds_path + ds_path.len() + 1;
    let off_mod_id = off_ov_path + ov_path.len() + 1;
    let off_cfg_json = off_mod_id + mod_id.len() + 1;
    let off_sh_name = off_cfg_json + cfg_json.len() + 1;
    let str_end = if include_v2_shader {
        off_sh_name + shader_name.len() + 1
    } else {
        off_sh_name
    };
    let pay_start = align16(str_end);

    let off_pso = pay_start;
    let off_ov = off_pso + align16(pso_payload.len());
    let off_sh = off_ov + align16(ov_payload.len());
    let total_size = if include_v2_shader {
        off_sh + align16(SYNTH_SHADER_DXIL.len())
    } else {
        off_sh
    };

    let mut out = vec![0u8; total_size];
    // Header (64 bytes for v1, 80 bytes for v2)
    out[0..4].copy_from_slice(&0x4E49_4246u32.to_le_bytes()); // "FBIN"
    out[4..8].copy_from_slice(&(if include_v2_shader { 2u32 } else { 1u32 }).to_le_bytes()); // version
    out[12..16].copy_from_slice(&(hdr_size as u32).to_le_bytes()); // header_size
    out[16..20].copy_from_slice(&1u32.to_le_bytes()); // dstorage_count
    out[20..24].copy_from_slice(&1u32.to_le_bytes()); // overlay_count
    out[24..28].copy_from_slice(&1u32.to_le_bytes()); // plugin_config_count
    out[32..40].copy_from_slice(&(ds_off as u64).to_le_bytes());
    out[40..48].copy_from_slice(&(ov_off as u64).to_le_bytes());
    out[48..56].copy_from_slice(&(cfg_off as u64).to_le_bytes());
    out[56..64].copy_from_slice(&(total_size as u64).to_le_bytes());
    if include_v2_shader {
        out[64..68].copy_from_slice(&1u32.to_le_bytes()); // shader_count
        out[72..80].copy_from_slice(&(sh_off as u64).to_le_bytes()); // shader_table_off
    }

    // DirectStorage entry (64 bytes)
    let kh = ds_key_hash(ds_path, 0x006C_6000, pso_payload.len() as u32);
    out[ds_off..ds_off + 8].copy_from_slice(&kh.to_le_bytes());
    out[ds_off + 8..ds_off + 16].copy_from_slice(&0x006C_6000u64.to_le_bytes());
    out[ds_off + 16..ds_off + 20].copy_from_slice(&(pso_payload.len() as u32).to_le_bytes());
    out[ds_off + 20..ds_off + 24].copy_from_slice(&(pso_payload.len() as u32).to_le_bytes());
    out[ds_off + 24..ds_off + 32].copy_from_slice(&(off_pso as u64).to_le_bytes());
    out[ds_off + 32..ds_off + 36].copy_from_slice(&(off_ds_path as u32).to_le_bytes());
    out[ds_off + 36..ds_off + 40].copy_from_slice(&(ds_path.len() as u32).to_le_bytes());
    out[ds_off + 40..ds_off + 44].copy_from_slice(&((1u32 << 0) | (1u32 << 1) | (1u32 << 3)).to_le_bytes());
    out[ds_off + 56..ds_off + 60].copy_from_slice(&crc32_ieee(pso_payload).to_le_bytes());

    // Overlay entry (40 bytes)
    let oh = fnv1a_64(ov_path.as_bytes());
    out[ov_off..ov_off + 8].copy_from_slice(&oh.to_le_bytes());
    out[ov_off + 8..ov_off + 16].copy_from_slice(&(off_ov as u64).to_le_bytes());
    out[ov_off + 16..ov_off + 24].copy_from_slice(&(ov_payload.len() as u64).to_le_bytes());
    out[ov_off + 24..ov_off + 28].copy_from_slice(&(off_ov_path as u32).to_le_bytes());
    out[ov_off + 28..ov_off + 32].copy_from_slice(&(ov_path.len() as u32).to_le_bytes());
    out[ov_off + 32..ov_off + 36].copy_from_slice(&crc32_ieee(ov_payload).to_le_bytes());

    // Plugin config entry (32 bytes)
    let mh = fnv1a_64(mod_id.as_bytes());
    out[cfg_off..cfg_off + 8].copy_from_slice(&mh.to_le_bytes());
    out[cfg_off + 8..cfg_off + 12].copy_from_slice(&(off_mod_id as u32).to_le_bytes());
    out[cfg_off + 12..cfg_off + 16].copy_from_slice(&(mod_id.len() as u32).to_le_bytes());
    out[cfg_off + 16..cfg_off + 20].copy_from_slice(&(off_cfg_json as u32).to_le_bytes());
    out[cfg_off + 20..cfg_off + 24].copy_from_slice(&(cfg_json.len() as u32).to_le_bytes());

    // Shader override entry (88 bytes, v2 only)
    if include_v2_shader {
        let sh_hash = shader_key_hash(&SYNTH_SHADER_B3, &SYNTH_SHADER_DXBC);
        out[sh_off..sh_off + 8].copy_from_slice(&sh_hash.to_le_bytes());
        out[sh_off + 8..sh_off + 40].copy_from_slice(&SYNTH_SHADER_B3);
        out[sh_off + 40..sh_off + 56].copy_from_slice(&SYNTH_SHADER_DXBC);
        out[sh_off + 56..sh_off + 64].copy_from_slice(&(off_sh as u64).to_le_bytes());
        out[sh_off + 64..sh_off + 68].copy_from_slice(&(SYNTH_SHADER_DXIL.len() as u32).to_le_bytes());
        out[sh_off + 68..sh_off + 72].copy_from_slice(&crc32_ieee(SYNTH_SHADER_DXIL).to_le_bytes());
        out[sh_off + 72..sh_off + 76].copy_from_slice(&(off_sh_name as u32).to_le_bytes());
        out[sh_off + 76..sh_off + 80].copy_from_slice(&(shader_name.len() as u32).to_le_bytes());
    }

    // Strings & payloads
    out[off_ds_path..off_ds_path + ds_path.len()].copy_from_slice(ds_path.as_bytes());
    out[off_ov_path..off_ov_path + ov_path.len()].copy_from_slice(ov_path.as_bytes());
    out[off_mod_id..off_mod_id + mod_id.len()].copy_from_slice(mod_id.as_bytes());
    out[off_cfg_json..off_cfg_json + cfg_json.len()].copy_from_slice(cfg_json.as_bytes());
    if include_v2_shader {
        out[off_sh_name..off_sh_name + shader_name.len()].copy_from_slice(shader_name.as_bytes());
    }
    out[off_pso..off_pso + pso_payload.len()].copy_from_slice(pso_payload);
    out[off_ov..off_ov + ov_payload.len()].copy_from_slice(ov_payload);
    if include_v2_shader {
        out[off_sh..off_sh + SYNTH_SHADER_DXIL.len()].copy_from_slice(SYNTH_SHADER_DXIL);
    }

    fs::write(path, &out).unwrap();
}

fn find_built_artifact(name: &str) -> PathBuf {
    let manifest_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo_root = manifest_dir.parent().unwrap().parent().unwrap();
    for cfg in ["Release", "Debug"] {
        let p = repo_root.join("build").join(cfg).join(name);
        if p.is_file() {
            return p;
        }
    }
    panic!("Could not locate built artifact '{name}' under build/Release or build/Debug — run CMake build first.");
}

#[test]
fn ffi_struct_sizes_match_c11_abi_x64() {
    assert_eq!(size_of::<FbInputEventV1>(), 32);
    assert_eq!(size_of::<FbIoOverrideDesc>(), 32);
    assert_eq!(size_of::<FbDStorageReqView>(), 48);
    assert_eq!(size_of::<FbHostApiV1>(), 104);
    assert_eq!(size_of::<FbModcoreCallbacksV1>(), 88);
}

#[test]
fn end_to_end_rust_bridge_against_native_dlls() {
    let core_dll = find_built_artifact("fireball_core.dll");
    let plugin_dll = find_built_artifact("fpv_toggle.fbplugin.dll");

    // 1. Absent DLL degrades silently to Ok(None) + safe global no-ops
    let missing_cfg = HostConfig {
        host_name: "VIctory-NoOp".to_string(),
        host_version: "0.1.0".to_string(),
        game_root_dir: PathBuf::from(r"D:\Games\GTA6"),
        mods_root_dir: PathBuf::from(r"C:\nonexistent_fireball_mods_dir"),
        dll_path: Some(PathBuf::from(r"C:\nonexistent_fireball_core.dll")),
        capabilities: FB_CAP_IO_VIRTUAL | FB_CAP_DSTORAGE_INTERCEPT,
    };
    let noop_hooks = Arc::new(MockVictoryHooks::new(0));
    assert!(!init_global(missing_cfg, noop_hooks).unwrap());
    let mut dummy_buf = [0xAAu8; 16];
    assert!(!try_dstorage_read(&mut DStorageReq {
        file_path: "data.rpf",
        file_offset: 0x006C_6000,
        src_size: 16,
        uncomp_size: 16,
        codec_hint: 1,
        dst: &mut dummy_buf,
    }));
    assert!(try_file_open("data/rust_overlay.txt").is_none());
    assert!(try_shader_load(&SYNTH_SHADER_B3, &SYNTH_SHADER_DXBC, Some("fx_rust_shader_ps")).is_none());
    assert!(!dispatch_input_event(&InputEvent {
        device_kind: FB_DEV_KEYBOARD,
        code: 0x56,
        action: FB_INPUT_PRESS,
        modifiers: 0,
        timestamp_us: 0,
    }));
    on_frame_begin(1, 1.0 / 60.0);
    on_frame_present(std::ptr::null_mut());
    shutdown_global();

    // Stage temporary mods dir inside Cargo target dir (never inside game dir)
    let mods_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("target")
        .join("rust_bridge_test_mods");
    fs::create_dir_all(&mods_dir).unwrap();
    write_synthetic_bundle(&mods_dir.join("fireball_bundle.fbin"), true);
    fs::copy(&plugin_dll, mods_dir.join("fpv_toggle.fbplugin.dll")).unwrap();

    // 2. Scenario A: Full Tier 0+1+2 Host via global one-liner API
    {
        let hooks = Arc::new(MockVictoryHooks::new(0));
        let cfg = HostConfig {
            host_name: "VIctory-Rust-Full".to_string(),
            host_version: "0.1.0".to_string(),
            game_root_dir: PathBuf::from(r"D:\Games\GTA6"),
            mods_root_dir: mods_dir.clone(),
            dll_path: Some(core_dll.clone()),
            capabilities: FB_CAP_IO_VIRTUAL
                | FB_CAP_DSTORAGE_INTERCEPT
                | FB_CAP_D3D12_SHADERS
                | FB_CAP_FRAME_LIFECYCLE
                | FB_CAP_INPUT_EVENTS
                | FB_CAP_RAGE_FIDEVICE
                | FB_CAP_RAGE_DATAFILEMGR
                | FB_CAP_RAGE_CRC_BYPASS
                | FB_CAP_RAGE_SYMBOL_REG
                | FB_CAP_TITLE_MEM_RW,
        };
        assert!(init_global(cfg, hooks.clone()).unwrap());

        // DirectStorage hit + pass-through + buffer-too-small
        let mut dst = [0u8; 16];
        assert!(try_dstorage_read(&mut DStorageReq {
            file_path: r".\DATA.RPF",
            file_offset: 0x006C_6000,
            src_size: 16,
            uncomp_size: 16,
            codec_hint: 1,
            dst: &mut dst,
        }));
        assert_eq!(&dst, b"PSIN\x00\x00\x00\x10\x02\x00\x00\x00\x59\xED\xD3\x71");

        let mut tiny = [0xA5u8; 15];
        assert!(!try_dstorage_read(&mut DStorageReq {
            file_path: "data.rpf",
            file_offset: 0x006C_6000,
            src_size: 16,
            uncomp_size: 16,
            codec_hint: 1,
            dst: &mut tiny,
        }));
        assert!(tiny.iter().all(|&b| b == 0xA5));

        let mut miss = [0x5Au8; 16];
        assert!(!try_dstorage_read(&mut DStorageReq {
            file_path: "common.rpf",
            file_offset: 0x1000,
            src_size: 16,
            uncomp_size: 16,
            codec_hint: 1,
            dst: &mut miss,
        }));
        assert!(miss.iter().all(|&b| b == 0x5A));

        // Loose-file overlay virtualization (RAII Drop calls close)
        {
            let ov = try_file_open(r"DATA\rust_overlay.txt").expect("expected virtual overlay hit");
            assert_eq!(ov.virtual_size(), 41);
            let mut p1 = [0u8; 10];
            let mut p2 = [0u8; 64];
            assert_eq!(ov.read_at(0, &mut p1).unwrap(), 10);
            assert_eq!(ov.read_at(10, &mut p2).unwrap(), 31);
            assert_eq!(ov.read_at(100, &mut p2).unwrap(), 0);
            assert_eq!(&p1, b"hello-from");
            assert_eq!(&p2[..31], b"-rust-victory-fireball-overlay\n");
        }
        assert!(try_file_open("data/does_not_exist.txt").is_none());

        // Shader DXIL override lookup (`try_shader_load`) hit & miss
        let dxil_hit = try_shader_load(&SYNTH_SHADER_B3, &SYNTH_SHADER_DXBC, Some("fx_rust_shader_ps"))
            .expect("expected shader override hit");
        assert_eq!(dxil_hit.as_slice(), SYNTH_SHADER_DXIL);
        let mut wrong_dxbc = SYNTH_SHADER_DXBC;
        wrong_dxbc[0] ^= 0xFF;
        assert!(try_shader_load(&SYNTH_SHADER_B3, &wrong_dxbc, Some("miss")).is_none());

        // Tier 2 fpv_toggle state machine: toggle 0->2->0, hold-guard, release, shutdown restore
        on_frame_begin(1, 1.0 / 60.0);
        assert_eq!(hooks.engine_read_u32(), 0);

        // Unrelated key 'W'
        assert!(!dispatch_input_event(&InputEvent {
            device_kind: FB_DEV_KEYBOARD,
            code: 0x57,
            action: FB_INPUT_PRESS,
            modifiers: 0,
            timestamp_us: 100,
        }));
        assert_eq!(hooks.engine_read_u32(), 0);

        // Hotkey 'V' -> 2 (FirstPerson)
        assert!(dispatch_input_event(&InputEvent {
            device_kind: FB_DEV_KEYBOARD,
            code: 0x56,
            action: FB_INPUT_PRESS,
            modifiers: 0,
            timestamp_us: 200,
        }));
        assert_eq!(hooks.engine_read_u32(), 2);

        // Release 'V' does not toggle
        assert!(!dispatch_input_event(&InputEvent {
            device_kind: FB_DEV_KEYBOARD,
            code: 0x56,
            action: FB_INPUT_RELEASE,
            modifiers: 0,
            timestamp_us: 250,
        }));
        assert_eq!(hooks.engine_read_u32(), 2);

        // Context reset -> hold-guard re-asserts 2
        hooks.engine_write_u32(0);
        on_frame_begin(2, 1.0 / 60.0);
        assert_eq!(hooks.engine_read_u32(), 2);

        // Toggle OFF -> 0, game sets 1 (Cinematic) -> preserved
        assert!(dispatch_input_event(&InputEvent {
            device_kind: FB_DEV_KEYBOARD,
            code: 0x56,
            action: FB_INPUT_PRESS,
            modifiers: 0,
            timestamp_us: 300,
        }));
        assert_eq!(hooks.engine_read_u32(), 0);
        hooks.engine_write_u32(1);
        on_frame_begin(3, 1.0 / 60.0);
        assert_eq!(hooks.engine_read_u32(), 1);

        // Re-latch ON -> 2, present, shutdown restores 0 + logs telemetry
        assert!(dispatch_input_event(&InputEvent {
            device_kind: FB_DEV_KEYBOARD,
            code: 0x56,
            action: FB_INPUT_PRESS,
            modifiers: 0,
            timestamp_us: 400,
        }));
        assert_eq!(hooks.engine_read_u32(), 2);
        on_frame_present(std::ptr::null_mut());

        // Guarded memory fault & read-only permission checks
        assert_eq!(hooks.title_mem_read_u32(UNMAPPED_VA), Err(FB_ERR_MEM_FAULT));
        assert_eq!(hooks.title_mem_write_u32(READONLY_VA, 2), Err(FB_ERR_PERMISSION));

        shutdown_global();
        assert_eq!(hooks.engine_read_u32(), 0);
        assert!(hooks.has_log(0, "Telemetry summary:", Some("dstorage_hits=1")));
        assert!(hooks.has_log(0, "Telemetry summary:", Some("shader_calls=2 shader_hits=1")));
        assert!(hooks.has_log(0, "First DirectStorage hit:", Some("data.rpf")));
        assert!(hooks.has_log(0, "First shader override hit:", Some("fx_rust_shader_ps")));
    }

    // 3. Scenario B: Tier 0-Only Host with legacy v1 bundle (clean plugin skip + v1 compat)
    {
        let mods_v1_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("target")
            .join("rust_bridge_test_mods_v1");
        fs::create_dir_all(&mods_v1_dir).unwrap();
        write_synthetic_bundle(&mods_v1_dir.join("fireball_bundle.fbin"), false);
        fs::copy(&plugin_dll, mods_v1_dir.join("fpv_toggle.fbplugin.dll")).unwrap();

        let hooks = Arc::new(MockVictoryHooks::new(0));
        let cfg = HostConfig {
            host_name: "VIctory-Rust-Tier0Only".to_string(),
            host_version: "0.1.0".to_string(),
            game_root_dir: PathBuf::from(r"D:\Games\GTA6"),
            mods_root_dir: mods_v1_dir,
            dll_path: Some(core_dll),
            capabilities: FB_CAP_IO_VIRTUAL | FB_CAP_DSTORAGE_INTERCEPT | FB_CAP_FRAME_LIFECYCLE,
        };
        let mut bridge = FireballBridge::try_load(cfg, hooks.clone())
            .unwrap()
            .expect("expected fireball_core.dll to load");

        assert!(hooks.has_log(FB_LOG_WARN, "fpv_toggle", Some("FB_CAP_TITLE_MEM_RW")));
        assert!(hooks.has_log(FB_LOG_WARN, "fpv_toggle", Some("FB_CAP_RAGE_SYMBOL_REG")));
        assert!(hooks.has_log(FB_LOG_WARN, "fpv_toggle", Some("FB_CAP_INPUT_EVENTS")));

        let mut dst = [0u8; 16];
        assert!(bridge.try_dstorage_read(&mut DStorageReq {
            file_path: "data.rpf",
            file_offset: 0x006C_6000,
            src_size: 16,
            uncomp_size: 16,
            codec_hint: 1,
            dst: &mut dst,
        }));
        assert!(bridge
            .try_shader_load(&SYNTH_SHADER_B3, &SYNTH_SHADER_DXBC, None)
            .is_none());
        bridge.on_frame_begin(1, 1.0 / 60.0);
        assert!(!bridge.dispatch_input_event(&InputEvent {
            device_kind: FB_DEV_KEYBOARD,
            code: 0x56,
            action: FB_INPUT_PRESS,
            modifiers: 0,
            timestamp_us: 0,
        }));
        assert_eq!(hooks.engine_read_u32(), 0);
        bridge.on_frame_present(std::ptr::null_mut());
        bridge.shutdown();
    }
}
