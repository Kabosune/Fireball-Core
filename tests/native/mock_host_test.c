/*
 * mock_host_test.c — Native C11 VIctory-style Mock HLE Host test executable (`fireball_mock_host.exe`).
 *
 * Dynamically loads `fireball_core.dll` via `LoadLibraryW` (mirroring how Marvisak/VIctory loads it),
 * validates ABI version/struct_size enforcement, tests `on_dstorage_read` (full-asset, per-64KiB-page
 * slices, path normalization, pass-through, buffer-too-small safety), tests `on_file_open` loose-file
 * overlay virtualization, and exercises `fpv_toggle.fbplugin.dll` under BOTH:
 *   - Scenario A (Full Tier 0+1+2 host): toggle 0->2->0, hold-guard re-assert, clean release,
 *     shutdown restore to 0, and guarded unmapped/read-only memory faults.
 *   - Scenario B (Tier 0-only host): clean plugin skip with explicit missing FB_CAP_* log diagnostics,
 *     0 crashes, while Tier 0 DirectStorage overrides continue functioning.
 *
 * Modes:
 *   1. Synthetic self-contained CTest mode (no args): builds a synthetic `fireball_bundle.fbin` in a
 *      temporary directory next to the executable, copies `fpv_toggle.fbplugin.dll`, and runs all checks.
 *   2. Real-bundle verification mode (`--mods-dir <dir> --expect-dir <out_dir>`): verifies a bundle built
 *      by `fireball_cli.py build-bundle` against `D:\Games\GTA6`, asserting byte-for-byte parity with the
 *      offline Python verifier's `.patched.xml`, `.patched.dat`, and `.patched.pso` outputs.
 */

#include "fireball_host.h"
#include "fb_bundle.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*fb_attach_host_fn)(const fb_host_api_v1_t*, fb_modcore_callbacks_v1_t*);

/* ---------------------------------------------------------------------------
 * Minimal assertion & logging harness
 * --------------------------------------------------------------------------- */

static int g_failures = 0;

static void check_cond(int cond, const char* expr, int line) {
    if (!cond) {
        fprintf(stderr, "  [FAIL] line %d: %s\n", line, expr);
        g_failures += 1;
    }
}

#define CHECK(expr) check_cond((expr) ? 1 : 0, #expr, __LINE__)

typedef struct mock_log_entry {
    int  level;
    char subsystem[64];
    char msg[512];
} mock_log_entry_t;

#define MAX_LOG_ENTRIES 128

static mock_log_entry_t g_logs[MAX_LOG_ENTRIES];
static size_t           g_log_count = 0;

static void reset_logs(void) {
    g_log_count = 0;
}

static void host_log_cb(int level, const char* subsystem, const char* msg) {
    if (g_log_count < MAX_LOG_ENTRIES) {
        g_logs[g_log_count].level = level;
        snprintf(g_logs[g_log_count].subsystem, sizeof(g_logs[g_log_count].subsystem), "%s", subsystem ? subsystem : "");
        snprintf(g_logs[g_log_count].msg, sizeof(g_logs[g_log_count].msg), "%s", msg ? msg : "");
        g_log_count += 1;
    }
    const char* lvl_str = "INFO";
    if (level == FB_LOG_WARN) {
        lvl_str = "WARN";
    } else if (level == FB_LOG_ERROR) {
        lvl_str = "ERROR";
    }
    printf("    [host-log][%s][%s] %s\n", lvl_str, subsystem ? subsystem : "?", msg ? msg : "");
}

static int logs_contain(int min_level, const char* needle1, const char* needle2) {
    for (size_t i = 0; i < g_log_count; ++i) {
        if (g_logs[i].level >= min_level &&
            strstr(g_logs[i].msg, needle1) != NULL &&
            (!needle2 || strstr(g_logs[i].msg, needle2) != NULL))
        {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * Guarded Mock Title Memory (mirrors Marvisak/VIctory HLE memory maps)
 * --------------------------------------------------------------------------- */

#define DIRECTOR_VA  0x00007FF712340000ull
#define READONLY_VA  0x00007FF712350000ull
#define UNMAPPED_VA  0x00007FF7DEADBEEFull
#define REGION_SIZE  0x1000u

static uint8_t g_rw_region[REGION_SIZE];
static uint8_t g_ro_region[REGION_SIZE];

static void* host_resolve_symbol(const char* symbol_name) {
    if (symbol_name && strcmp(symbol_name, "rage::camGameplayDirector::s_Instance") == 0) {
        return (void*)(uintptr_t)DIRECTOR_VA;
    }
    return NULL;
}

static int host_poll_key_state(uint32_t device_kind, uint32_t code) {
    (void)device_kind;
    (void)code;
    return 0;
}

static int host_title_mem_read_u32(uint64_t title_va, uint32_t* out_value) {
    if (!out_value) {
        return FB_ERR_INVALID_ARG;
    }
    if (title_va >= DIRECTOR_VA && title_va + 4ull <= DIRECTOR_VA + REGION_SIZE) {
        memcpy(out_value, g_rw_region + (size_t)(title_va - DIRECTOR_VA), 4);
        return FB_OK;
    }
    if (title_va >= READONLY_VA && title_va + 4ull <= READONLY_VA + REGION_SIZE) {
        memcpy(out_value, g_ro_region + (size_t)(title_va - READONLY_VA), 4);
        return FB_OK;
    }
    return FB_ERR_MEM_FAULT;
}

static int host_title_mem_write_u32(uint64_t title_va, uint32_t value) {
    if (title_va >= DIRECTOR_VA && title_va + 4ull <= DIRECTOR_VA + REGION_SIZE) {
        memcpy(g_rw_region + (size_t)(title_va - DIRECTOR_VA), &value, 4);
        return FB_OK;
    }
    if (title_va >= READONLY_VA && title_va + 4ull <= READONLY_VA + REGION_SIZE) {
        return FB_ERR_PERMISSION;
    }
    return FB_ERR_MEM_FAULT;
}

static uint32_t engine_read_u32(uint64_t title_va) {
    uint32_t v = 0;
    CHECK(title_va >= DIRECTOR_VA && title_va + 4ull <= DIRECTOR_VA + REGION_SIZE);
    memcpy(&v, g_rw_region + (size_t)(title_va - DIRECTOR_VA), 4);
    return v;
}

static void engine_write_u32(uint64_t title_va, uint32_t value) {
    CHECK(title_va >= DIRECTOR_VA && title_va + 4ull <= DIRECTOR_VA + REGION_SIZE);
    memcpy(g_rw_region + (size_t)(title_va - DIRECTOR_VA), &value, 4);
}

/* ---------------------------------------------------------------------------
 * File & synthetic bundle helpers (independent of fireball_core internals)
 * --------------------------------------------------------------------------- */

static uint64_t local_fnv1a_64(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)p[i];
        h *= 0x00000100000001B3ull;
    }
    return h;
}

static uint64_t local_ds_key_hash(const char* path, uint64_t file_offset, uint32_t orig_uncomp) {
    size_t plen = strlen(path);
    uint8_t buf[512];
    CHECK(plen + 1 + 12 <= sizeof(buf));
    memcpy(buf, path, plen);
    buf[plen] = 0;
    memcpy(buf + plen + 1, &file_offset, 8);
    memcpy(buf + plen + 9, &orig_uncomp, 4);
    return local_fnv1a_64(buf, plen + 13);
}

static uint32_t local_crc32(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint32_t)p[i];
        for (int b = 0; b < 8; ++b) {
            uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static uint8_t* read_entire_file(const wchar_t* path, size_t* out_size) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return NULL;
    }
    LARGE_INTEGER li;
    if (!GetFileSizeEx(h, &li) || li.QuadPart < 0) {
        CloseHandle(h);
        return NULL;
    }
    size_t sz = (size_t)li.QuadPart;
    uint8_t* buf = (uint8_t*)malloc(sz > 0 ? sz : 1);
    if (!buf) {
        CloseHandle(h);
        return NULL;
    }
    size_t got_total = 0;
    while (got_total < sz) {
        DWORD got = 0;
        if (!ReadFile(h, buf + got_total, (DWORD)(sz - got_total), &got, NULL) || got == 0) {
            free(buf);
            CloseHandle(h);
            return NULL;
        }
        got_total += (size_t)got;
    }
    CloseHandle(h);
    *out_size = sz;
    return buf;
}

static int write_entire_file(const wchar_t* path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)size, &written, NULL);
    CloseHandle(h);
    return ok && (size_t)written == size;
}

static size_t align16_sz(size_t v) {
    return (v + 15u) & ~15u;
}

static const char SYNTH_XML[] = "<CLassoDataManager><UseFirstPersonCameraMatrix value=\"true\" /></CLassoDataManager>\n";
static const char SYNTH_KV[] = "defaultmotionblur.fpv.strength 0.05\nmotionblur.fpv.MBLowCameraRotSpeedScale 0.375\n";
static const uint8_t SYNTH_PSO[16] = {
    'P','S','I','N', 0x00, 0x00, 0x00, 0x10,
    0x02, 0x00, 0x00, 0x00, 0x59, 0xED, 0xD3, 0x71
};
static const char SYNTH_OVERLAY[] = "fireball-core-virtual-overlay-payload-v1\n";
static const char SYNTH_CFG_JSON[] =
    "{\"director_symbol\":\"rage::camGameplayDirector::s_Instance\","
    "\"first_person_mode\":2,\"hold_guard_enabled\":true,\"hotkey_vkey\":86,"
    "\"third_person_mode\":0,\"view_mode_field_offset\":0}";

static int write_synthetic_bundle(const wchar_t* bundle_path) {
    /*
     * 4 DirectStorage entries:
     *   0: ("common.rpf", 0x0003B000, len(XML)+7) -> SYNTH_XML (simulates shrunk XML)
     *   1: ("common.rpf", 0x0003BA30, len(XML)+7) -> SYNTH_XML (page_offset slice)
     *   2: ("common.rpf", 0x00643650, len(KV)-2)  -> SYNTH_KV  (simulates grown KV page)
     *   3: ("data.rpf",   0x006C6000, 16)         -> SYNTH_PSO (exact-size PSO)
     * 1 Overlay entry:
     *   0: "data/fireball_overlay_test.txt" -> SYNTH_OVERLAY
     * 1 Plugin config entry:
     *   0: "community.fpv_unlocker" -> SYNTH_CFG_JSON
     */
    const char* s_common = "common.rpf";
    const char* s_data = "data.rpf";
    const char* s_ov = "data/fireball_overlay_test.txt";
    const char* s_mod = "community.fpv_unlocker";

    size_t ds_off = sizeof(fb_bundle_header_t);
    size_t ov_off = ds_off + 4u * sizeof(fb_bundle_dstorage_entry_t);
    size_t cfg_off = ov_off + 1u * sizeof(fb_bundle_overlay_entry_t);
    size_t str_off = cfg_off + 1u * sizeof(fb_bundle_plugin_cfg_entry_t);

    size_t off_s_common = str_off;
    size_t off_s_data = off_s_common + strlen(s_common) + 1u;
    size_t off_s_ov = off_s_data + strlen(s_data) + 1u;
    size_t off_s_mod = off_s_ov + strlen(s_ov) + 1u;
    size_t off_s_cfg = off_s_mod + strlen(s_mod) + 1u;
    size_t pay_start = align16_sz(off_s_cfg + strlen(SYNTH_CFG_JSON) + 1u);

    size_t xml_len = strlen(SYNTH_XML);
    size_t kv_len = strlen(SYNTH_KV);
    size_t pso_len = sizeof(SYNTH_PSO);
    size_t ov_len = strlen(SYNTH_OVERLAY);

    size_t off_p_xml0 = pay_start;
    size_t off_p_xml1 = off_p_xml0 + align16_sz(xml_len);
    size_t off_p_kv = off_p_xml1 + align16_sz(xml_len);
    size_t off_p_pso = off_p_kv + align16_sz(kv_len);
    size_t off_p_ov = off_p_pso + align16_sz(pso_len);
    size_t total_size = off_p_ov + align16_sz(ov_len);

    uint8_t* buf = (uint8_t*)calloc(1, total_size);
    if (!buf) {
        return 0;
    }

    fb_bundle_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = FB_BUNDLE_MAGIC;
    hdr.version = FB_BUNDLE_VERSION;
    hdr.header_size = FB_BUNDLE_HEADER_SIZE;
    hdr.dstorage_count = 4u;
    hdr.overlay_count = 1u;
    hdr.plugin_config_count = 1u;
    hdr.dstorage_table_off = (uint64_t)ds_off;
    hdr.overlay_table_off = (uint64_t)ov_off;
    hdr.plugin_cfg_table_off = (uint64_t)cfg_off;
    hdr.total_file_size = (uint64_t)total_size;
    memcpy(buf, &hdr, sizeof(hdr));

    fb_bundle_dstorage_entry_t ds[4];
    memset(ds, 0, sizeof(ds));

    ds[0].key_hash = local_ds_key_hash(s_common, 0x0003B000ull, (uint32_t)(xml_len + 7u));
    ds[0].file_offset = 0x0003B000ull;
    ds[0].orig_uncomp_size = (uint32_t)(xml_len + 7u);
    ds[0].payload_size = (uint32_t)xml_len;
    ds[0].payload_offset = (uint64_t)off_p_xml0;
    ds[0].path_offset = (uint32_t)off_s_common;
    ds[0].path_len = (uint32_t)strlen(s_common);
    ds[0].flags = FB_DS_FLAG_FULL_ASSET | FB_DS_FLAG_TEXT_PATCH;
    ds[0].payload_crc32 = local_crc32(SYNTH_XML, xml_len);

    ds[1].key_hash = local_ds_key_hash(s_common, 0x0003BA30ull, (uint32_t)(xml_len + 7u));
    ds[1].file_offset = 0x0003BA30ull;
    ds[1].orig_uncomp_size = (uint32_t)(xml_len + 7u);
    ds[1].payload_size = (uint32_t)xml_len;
    ds[1].payload_offset = (uint64_t)off_p_xml1;
    ds[1].path_offset = (uint32_t)off_s_common;
    ds[1].path_len = (uint32_t)strlen(s_common);
    ds[1].flags = FB_DS_FLAG_PAGE_SLICE | FB_DS_FLAG_TEXT_PATCH;
    ds[1].payload_crc32 = local_crc32(SYNTH_XML, xml_len);

    ds[2].key_hash = local_ds_key_hash(s_common, 0x00643650ull, (uint32_t)(kv_len - 2u));
    ds[2].file_offset = 0x00643650ull;
    ds[2].orig_uncomp_size = (uint32_t)(kv_len - 2u);
    ds[2].payload_size = (uint32_t)kv_len;
    ds[2].payload_offset = (uint64_t)off_p_kv;
    ds[2].path_offset = (uint32_t)off_s_common;
    ds[2].path_len = (uint32_t)strlen(s_common);
    ds[2].flags = FB_DS_FLAG_PAGE_SLICE | FB_DS_FLAG_TEXT_PATCH;
    ds[2].payload_crc32 = local_crc32(SYNTH_KV, kv_len);

    ds[3].key_hash = local_ds_key_hash(s_data, 0x006C6000ull, (uint32_t)pso_len);
    ds[3].file_offset = 0x006C6000ull;
    ds[3].orig_uncomp_size = (uint32_t)pso_len;
    ds[3].payload_size = (uint32_t)pso_len;
    ds[3].payload_offset = (uint64_t)off_p_pso;
    ds[3].path_offset = (uint32_t)off_s_data;
    ds[3].path_len = (uint32_t)strlen(s_data);
    ds[3].flags = FB_DS_FLAG_FULL_ASSET | FB_DS_FLAG_PAGE_SLICE | FB_DS_FLAG_PSO_PATCH;
    ds[3].payload_crc32 = local_crc32(SYNTH_PSO, pso_len);

    memcpy(buf + ds_off, ds, sizeof(ds));

    fb_bundle_overlay_entry_t ov;
    memset(&ov, 0, sizeof(ov));
    ov.path_hash = local_fnv1a_64(s_ov, strlen(s_ov));
    ov.payload_offset = (uint64_t)off_p_ov;
    ov.payload_size = (uint64_t)ov_len;
    ov.path_offset = (uint32_t)off_s_ov;
    ov.path_len = (uint32_t)strlen(s_ov);
    ov.payload_crc32 = local_crc32(SYNTH_OVERLAY, ov_len);
    memcpy(buf + ov_off, &ov, sizeof(ov));

    fb_bundle_plugin_cfg_entry_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mod_id_hash = local_fnv1a_64(s_mod, strlen(s_mod));
    cfg.mod_id_offset = (uint32_t)off_s_mod;
    cfg.mod_id_len = (uint32_t)strlen(s_mod);
    cfg.json_offset = (uint32_t)off_s_cfg;
    cfg.json_len = (uint32_t)strlen(SYNTH_CFG_JSON);
    memcpy(buf + cfg_off, &cfg, sizeof(cfg));

    memcpy(buf + off_s_common, s_common, strlen(s_common) + 1u);
    memcpy(buf + off_s_data, s_data, strlen(s_data) + 1u);
    memcpy(buf + off_s_ov, s_ov, strlen(s_ov) + 1u);
    memcpy(buf + off_s_mod, s_mod, strlen(s_mod) + 1u);
    memcpy(buf + off_s_cfg, SYNTH_CFG_JSON, strlen(SYNTH_CFG_JSON) + 1u);

    memcpy(buf + off_p_xml0, SYNTH_XML, xml_len);
    memcpy(buf + off_p_xml1, SYNTH_XML, xml_len);
    memcpy(buf + off_p_kv, SYNTH_KV, kv_len);
    memcpy(buf + off_p_pso, SYNTH_PSO, pso_len);
    memcpy(buf + off_p_ov, SYNTH_OVERLAY, ov_len);

    int ok = write_entire_file(bundle_path, buf, total_size);
    free(buf);
    return ok;
}

/* ---------------------------------------------------------------------------
 * Verification routines
 * --------------------------------------------------------------------------- */

static void test_abi_validation(fb_attach_host_fn attach_fn) {
    printf("[1] Testing fb_attach_host ABI version & struct_size validation...\n");
    fb_host_api_v1_t api;
    fb_modcore_callbacks_v1_t cb;
    memset(&api, 0, sizeof(api));
    memset(&cb, 0, sizeof(cb));

    CHECK(attach_fn(NULL, &cb) == FB_ERR_INVALID_ARG);
    CHECK(attach_fn(&api, NULL) == FB_ERR_INVALID_ARG);

    api.struct_size = (uint32_t)sizeof(api);
    api.abi_version = 999u;
    CHECK(attach_fn(&api, &cb) == FB_ERR_ABI_MISMATCH);

    api.abi_version = FB_HOST_ABI_VERSION;
    api.struct_size = (uint32_t)(sizeof(api) - 8u);
    CHECK(attach_fn(&api, &cb) == FB_ERR_ABI_MISMATCH);

    api.struct_size = (uint32_t)sizeof(api);
    cb.struct_size = 12u;
    CHECK(attach_fn(&api, &cb) == FB_ERR_ABI_MISMATCH);
}

static void verify_ds_hit_bytes(
    const fb_modcore_callbacks_v1_t* cb,
    const wchar_t*                   file_path,
    uint64_t                         file_offset,
    uint32_t                         orig_uncomp_size,
    const uint8_t*                   expected,
    size_t                           expected_len,
    uint32_t                         dst_size,
    int                              check_zero_tail)
{
    uint8_t* dst = (uint8_t*)malloc(dst_size > 0 ? dst_size : 1);
    CHECK(dst != NULL);
    memset(dst, 0xCC, dst_size);

    fb_dstorage_req_view_t req;
    memset(&req, 0, sizeof(req));
    req.file_path = file_path;
    req.file_offset = file_offset;
    req.uncomp_size = orig_uncomp_size;
    req.codec_hint = 1u; /* zlib */
    req.dst_ptr = dst;
    req.dst_size = dst_size;

    int hit = cb->on_dstorage_read(&req);
    CHECK(hit == 1);
    if (hit == 1) {
        CHECK(memcmp(dst, expected, expected_len) == 0);
        if (check_zero_tail && orig_uncomp_size > expected_len && orig_uncomp_size <= dst_size) {
            for (size_t i = expected_len; i < (size_t)orig_uncomp_size; ++i) {
                if (dst[i] != 0) {
                    CHECK(0 && "expected zero-padded tail for shrunk payload");
                    break;
                }
            }
        }
    }
    free(dst);
}

static void verify_ds_passthrough(
    const fb_modcore_callbacks_v1_t* cb,
    const wchar_t*                   file_path,
    uint64_t                         file_offset,
    uint32_t                         uncomp_size,
    uint32_t                         dst_size)
{
    uint8_t* dst = (uint8_t*)malloc(dst_size > 0 ? dst_size : 1);
    CHECK(dst != NULL);
    memset(dst, 0xA5, dst_size);

    fb_dstorage_req_view_t req;
    memset(&req, 0, sizeof(req));
    req.file_path = file_path;
    req.file_offset = file_offset;
    req.uncomp_size = uncomp_size;
    req.dst_ptr = dst;
    req.dst_size = dst_size;

    int hit = cb->on_dstorage_read(&req);
    CHECK(hit == 0);
    for (uint32_t i = 0; i < dst_size; ++i) {
        if (dst[i] != 0xA5) {
            CHECK(0 && "pass-through or buffer-too-small modified dst_ptr!");
            break;
        }
    }
    free(dst);
}

static void verify_synthetic_bundle_io(const fb_modcore_callbacks_v1_t* cb) {
    printf("[2] Verifying synthetic bundle DirectStorage + loose-file overlay interception...\n");
    size_t xml_len = strlen(SYNTH_XML);
    size_t kv_len = strlen(SYNTH_KV);

    /* Full XML hit + zero tail check */
    verify_ds_hit_bytes(cb, L"common.rpf", 0x0003B000ull, (uint32_t)(xml_len + 7u),
                        (const uint8_t*)SYNTH_XML, xml_len, (uint32_t)(xml_len + 7u), 1);
    /* Page-slice XML hit with uppercase/backslash path normalization */
    verify_ds_hit_bytes(cb, L".\\COMMON.RPF", 0x0003BA30ull, (uint32_t)(xml_len + 7u),
                        (const uint8_t*)SYNTH_XML, xml_len, (uint32_t)(xml_len + 7u), 1);
    /* KV page-slice hit (grown payload into 64 KiB staging buffer) */
    verify_ds_hit_bytes(cb, L"common.rpf", 0x00643650ull, (uint32_t)(kv_len - 2u),
                        (const uint8_t*)SYNTH_KV, kv_len, 65536u, 0);
    /* Buffer-too-small safety on grown KV page */
    verify_ds_passthrough(cb, L"common.rpf", 0x00643650ull, (uint32_t)(kv_len - 2u), (uint32_t)(kv_len - 2u));

    /* Exact PSO hit + fallback (uncomp_size == 0) + buffer-too-small */
    verify_ds_hit_bytes(cb, L"data.rpf", 0x006C6000ull, (uint32_t)sizeof(SYNTH_PSO),
                        SYNTH_PSO, sizeof(SYNTH_PSO), (uint32_t)sizeof(SYNTH_PSO), 0);
    verify_ds_hit_bytes(cb, L"data.rpf", 0x006C6000ull, 0u,
                        SYNTH_PSO, sizeof(SYNTH_PSO), (uint32_t)sizeof(SYNTH_PSO), 0);
    verify_ds_passthrough(cb, L"data.rpf", 0x006C6000ull, (uint32_t)sizeof(SYNTH_PSO), (uint32_t)(sizeof(SYNTH_PSO) - 1u));

    /* Pass-through on unpatched offset */
    verify_ds_passthrough(cb, L"common.rpf", 0x00001000ull, 4096u, 4096u);

    /* Loose-file overlay virtualization (`on_file_open`) */
    fb_io_override_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    CHECK(cb->on_file_open(L"data\\nonexistent_file.txt", &desc) == 0);
    CHECK(cb->on_file_open(L"DATA\\fireball_overlay_test.txt", &desc) == 1);
    CHECK(desc.virtual_size == strlen(SYNTH_OVERLAY));
    CHECK(desc.read_at != NULL && desc.close != NULL);
    if (desc.read_at && desc.close) {
        char part1[16];
        char part2[64];
        memset(part1, 0, sizeof(part1));
        memset(part2, 0, sizeof(part2));
        int64_t r1 = desc.read_at(desc.user_ctx, 0, part1, 13);
        int64_t r2 = desc.read_at(desc.user_ctx, 13, part2, sizeof(part2));
        int64_t r3 = desc.read_at(desc.user_ctx, desc.virtual_size + 10, part2, 8);
        CHECK(r1 == 13);
        CHECK(r2 == (int64_t)(strlen(SYNTH_OVERLAY) - 13));
        CHECK(r3 == 0);
        CHECK(memcmp(part1, SYNTH_OVERLAY, 13) == 0);
        CHECK(memcmp(part2, SYNTH_OVERLAY + 13, (size_t)r2) == 0);
        desc.close(desc.user_ctx);
    }
}

static void verify_real_bundle_against_expect_dir(const fb_modcore_callbacks_v1_t* cb, const wchar_t* expect_dir) {
    printf("[2] Verifying compiled GTA VI bundle byte-for-byte against offline Python verifier outputs...\n");

    wchar_t fpath[MAX_PATH * 2];
    size_t sz = 0;
    uint8_t* expected = NULL;

    /* 1. XML: lasso_fpv_camera_matrix (full @ 0x0003B000 orig=126639, page1 @ 0x0003BA30 orig=61103) */
    swprintf(fpath, sizeof(fpath) / sizeof(fpath[0]), L"%ls\\lasso_fpv_camera_matrix.patched.xml", expect_dir);
    expected = read_entire_file(fpath, &sz);
    CHECK(expected != NULL && sz == 126632u);
    if (expected) {
        verify_ds_hit_bytes(cb, L"common.rpf", 0x0003B000ull, 126639u, expected, sz, 126639u, 1);
        verify_ds_hit_bytes(cb, L"COMMON.RPF", 0x0003BA30ull, 61103u, expected + 65536u, sz - 65536u, 61103u, 1);
        verify_ds_passthrough(cb, L"common.rpf", 0x0003BA30ull, 61103u, (uint32_t)(sz - 65536u - 1u));
        free(expected);
    }

    /* 2. KV: visualsettings_fpv_motionblur (full @ 0x00640000 orig=117176, page1 @ 0x00643650 orig=51640) */
    swprintf(fpath, sizeof(fpath) / sizeof(fpath[0]), L"%ls\\visualsettings_fpv_motionblur.patched.dat", expect_dir);
    expected = read_entire_file(fpath, &sz);
    CHECK(expected != NULL && sz == 117179u);
    if (expected) {
        verify_ds_hit_bytes(cb, L"common.rpf", 0x00640000ull, 117176u, expected, sz, (uint32_t)sz, 0);
        verify_ds_hit_bytes(cb, L".\\common.rpf", 0x00643650ull, 51640u, expected + 65536u, sz - 65536u, 65536u, 0);
        /* Buffer-too-small safety: 51640 < 51643 patched page */
        verify_ds_passthrough(cb, L"common.rpf", 0x00643650ull, 51640u, 51640u);
        free(expected);
    }

    /* 3. PSO #1: camera_view_mode_firstperson (data.rpf @ 0x006C6000, 54804 B) */
    swprintf(fpath, sizeof(fpath) / sizeof(fpath[0]), L"%ls\\camera_view_mode_firstperson.patched.pso", expect_dir);
    expected = read_entire_file(fpath, &sz);
    CHECK(expected != NULL && sz == 54804u);
    if (expected) {
        verify_ds_hit_bytes(cb, L"data.rpf", 0x006C6000ull, 54804u, expected, sz, (uint32_t)sz, 0);
        verify_ds_hit_bytes(cb, L"DATA.RPF", 0x006C6000ull, 0u, expected, sz, (uint32_t)sz, 0);
        verify_ds_passthrough(cb, L"data.rpf", 0x006C6000ull, 54804u, (uint32_t)(sz - 1u));
        free(expected);
    }

    /* 4. PSO #2: camera_freecam_nearclip_fov (data.rpf @ 0x0063F000, 16856 B) */
    swprintf(fpath, sizeof(fpath) / sizeof(fpath[0]), L"%ls\\camera_freecam_nearclip_fov.patched.pso", expect_dir);
    expected = read_entire_file(fpath, &sz);
    CHECK(expected != NULL && sz == 16856u);
    if (expected) {
        verify_ds_hit_bytes(cb, L"data.rpf", 0x0063F000ull, 16856u, expected, sz, (uint32_t)sz, 0);
        free(expected);
    }

    /* 5. PSO #3: camera_ads_optic_free_toggle (data.rpf @ 0x006A2000 full=69132 B & page0=65536 B) */
    swprintf(fpath, sizeof(fpath) / sizeof(fpath[0]), L"%ls\\camera_ads_optic_free_toggle.patched.pso", expect_dir);
    expected = read_entire_file(fpath, &sz);
    CHECK(expected != NULL && sz == 69132u);
    if (expected) {
        verify_ds_hit_bytes(cb, L"data.rpf", 0x006A2000ull, 69132u, expected, sz, (uint32_t)sz, 0);
        verify_ds_hit_bytes(cb, L"data.rpf", 0x006A2000ull, 65536u, expected, 65536u, 65536u, 0);
        /* Unchanged page 1 @ 0x006A7950 must pass through untouched */
        verify_ds_passthrough(cb, L"data.rpf", 0x006A7950ull, 3596u, 3596u);
        free(expected);
    }

    /* 6. Unpatched offset pass-through */
    verify_ds_passthrough(cb, L"common.rpf", 0x00001000ull, 4096u, 4096u);
}

static void verify_size_preserve_visualsettings(const fb_modcore_callbacks_v1_t* cb) {
    printf("[2b] Verifying --size-preserve visualsettings.dat fits exact 51,640 B fixed DirectStorage buffer...\n");
    uint8_t* dst = (uint8_t*)malloc(51640u + 1u);
    CHECK(dst != NULL);
    if (!dst) {
        return;
    }
    memset(dst, 0xCC, 51640u);
    dst[51640u] = '\0';

    fb_dstorage_req_view_t req;
    memset(&req, 0, sizeof(req));
    req.file_path = L"common.rpf";
    req.file_offset = 0x00643650ull;
    req.uncomp_size = 51640u;
    req.codec_hint = 1u;
    req.dst_ptr = dst;
    req.dst_size = 51640u;

    int hit = cb->on_dstorage_read(&req);
    CHECK(hit == 1);
    if (hit == 1) {
        const char* txt = (const char*)dst;
        CHECK(strstr(txt, "defaultmotionblur.fpv.strength") != NULL);
        CHECK(strstr(txt, "0.05") != NULL);
        CHECK(strstr(txt, "motionblur.fpv.MBLowCameraRotSpeedScale 0.375") != NULL);
        CHECK(strstr(txt, "motionblur.fpv.MBHighCameraRotSpeedScale 0.15") != NULL);
    }
    free(dst);

    /* Also verify full asset fits exact 117,176 B buffer */
    uint8_t* full_dst = (uint8_t*)malloc(117176u);
    CHECK(full_dst != NULL);
    if (full_dst) {
        memset(full_dst, 0xCC, 117176u);
        req.file_offset = 0x00640000ull;
        req.uncomp_size = 117176u;
        req.dst_ptr = full_dst;
        req.dst_size = 117176u;
        CHECK(cb->on_dstorage_read(&req) == 1);
        free(full_dst);
    }

    /* And verify PSO 0x006C6000 fits exact 54,804 B buffer */
    uint8_t* pso_dst = (uint8_t*)malloc(54804u);
    CHECK(pso_dst != NULL);
    if (pso_dst) {
        memset(pso_dst, 0xCC, 54804u);
        req.file_path = L"data.rpf";
        req.file_offset = 0x006C6000ull;
        req.uncomp_size = 54804u;
        req.dst_ptr = pso_dst;
        req.dst_size = 54804u;
        CHECK(cb->on_dstorage_read(&req) == 1);
        free(pso_dst);
    }
}

static void test_scenario_a_full_host(
    fb_attach_host_fn attach_fn,
    const wchar_t*    mods_dir,
    const wchar_t*    expect_dir,
    int               size_preserve_check)
{
    printf("\n[Scenario A] Full Tier 0+1+2 Host + Native fpv_toggle.fbplugin.dll\n");
    reset_logs();
    memset(g_rw_region, 0, sizeof(g_rw_region));
    memset(g_ro_region, 0, sizeof(g_ro_region));
    engine_write_u32(DIRECTOR_VA, 0u); /* Start in ThirdPerson (0) */

    fb_host_api_v1_t api;
    memset(&api, 0, sizeof(api));
    api.struct_size = (uint32_t)sizeof(api);
    api.abi_version = FB_HOST_ABI_VERSION;
    api.host_name = "VIctory-HLE-Mock-Full";
    api.host_version = "0.1.0-native";
    api.game_root_dir = L"D:\\Games\\GTA6";
    api.mods_root_dir = mods_dir;
    api.capabilities =
        FB_CAP_IO_VIRTUAL |
        FB_CAP_DSTORAGE_INTERCEPT |
        FB_CAP_D3D12_SHADERS |
        FB_CAP_FRAME_LIFECYCLE |
        FB_CAP_INPUT_EVENTS |
        FB_CAP_RAGE_FIDEVICE |
        FB_CAP_RAGE_DATAFILEMGR |
        FB_CAP_RAGE_CRC_BYPASS |
        FB_CAP_RAGE_SYMBOL_REG |
        FB_CAP_TITLE_MEM_RW;
    api.log = host_log_cb;
    api.resolve_symbol = host_resolve_symbol;
    api.poll_key_state = host_poll_key_state;
    api.title_mem_read_u32 = host_title_mem_read_u32;
    api.title_mem_write_u32 = host_title_mem_write_u32;

    fb_modcore_callbacks_v1_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.struct_size = (uint32_t)sizeof(cb);

    CHECK(attach_fn(&api, &cb) == FB_OK);
    CHECK(cb.abi_version == FB_HOST_ABI_VERSION);
    CHECK(cb.on_dstorage_read != NULL && cb.on_file_open != NULL);
    CHECK(cb.on_frame_begin != NULL && cb.on_frame_present != NULL);
    CHECK(cb.on_input_event != NULL && cb.on_shutdown != NULL);

    if (size_preserve_check) {
        verify_size_preserve_visualsettings(&cb);
    } else if (expect_dir) {
        verify_real_bundle_against_expect_dir(&cb, expect_dir);
    } else {
        verify_synthetic_bundle_io(&cb);
    }

    printf("[3] Verifying fpv_toggle.fbplugin.dll state machine (toggle, hold-guard, release, shutdown)...\n");

    /* Frame 1: idle in ThirdPerson (0) */
    cb.on_frame_begin(1u, 1.0 / 60.0);
    CHECK(engine_read_u32(DIRECTOR_VA) == 0u);

    /* Unrelated key 'W' (0x57) must NOT toggle or consume */
    fb_input_event_v1_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = (uint32_t)sizeof(ev);
    ev.device_kind = 0u;
    ev.code = 0x57u;
    ev.action = (uint32_t)FB_INPUT_PRESS;
    CHECK(cb.on_input_event(&ev) == 0);
    CHECK(engine_read_u32(DIRECTOR_VA) == 0u);

    /* Hotkey 'V' (0x56) press #1 -> flips 0 -> 2 (FirstPerson), consumed=1 */
    ev.code = 0x56u;
    ev.action = (uint32_t)FB_INPUT_PRESS;
    CHECK(cb.on_input_event(&ev) == 1);
    CHECK(engine_read_u32(DIRECTOR_VA) == 2u);

    /* Key release must NOT toggle */
    ev.action = (uint32_t)FB_INPUT_RELEASE;
    CHECK(cb.on_input_event(&ev) == 0);
    CHECK(engine_read_u32(DIRECTOR_VA) == 2u);

    /* Simulated context reset #1 (game resets to 0) -> hold-guard re-asserts 2 */
    engine_write_u32(DIRECTOR_VA, 0u);
    cb.on_frame_begin(2u, 1.0 / 60.0);
    CHECK(engine_read_u32(DIRECTOR_VA) == 2u);

    /* Simulated context reset #2 -> hold-guard re-asserts 2 */
    engine_write_u32(DIRECTOR_VA, 0u);
    cb.on_frame_begin(3u, 1.0 / 60.0);
    CHECK(engine_read_u32(DIRECTOR_VA) == 2u);

    /* Hotkey 'V' press #2 -> flips 2 -> 0 (ThirdPerson) and releases latch */
    ev.action = (uint32_t)FB_INPUT_PRESS;
    CHECK(cb.on_input_event(&ev) == 1);
    CHECK(engine_read_u32(DIRECTOR_VA) == 0u);

    /* After toggle OFF, game sets 1 (Cinematic) -> plugin must NOT overwrite */
    engine_write_u32(DIRECTOR_VA, 1u);
    cb.on_frame_begin(4u, 1.0 / 60.0);
    CHECK(engine_read_u32(DIRECTOR_VA) == 1u);

    /* Re-latch ON -> 2, exercise on_frame_present, then verify on_shutdown restores 0 */
    ev.action = (uint32_t)FB_INPUT_PRESS;
    CHECK(cb.on_input_event(&ev) == 1);
    CHECK(engine_read_u32(DIRECTOR_VA) == 2u);
    cb.on_frame_present(NULL);

    /* Guarded memory safety checks: unmapped VA & read-only VA */
    uint32_t dummy = 0;
    CHECK(host_title_mem_read_u32(UNMAPPED_VA, &dummy) == FB_ERR_MEM_FAULT);
    CHECK(host_title_mem_write_u32(READONLY_VA, 2u) == FB_ERR_PERMISSION);

    cb.on_shutdown();
    CHECK(engine_read_u32(DIRECTOR_VA) == 0u);
    CHECK(logs_contain(FB_LOG_INFO, "Telemetry summary:", "dstorage_hits="));
    CHECK(logs_contain(FB_LOG_INFO, "First DirectStorage hit:", NULL));
}

static void test_scenario_b_tier0_only(fb_attach_host_fn attach_fn, const wchar_t* mods_dir) {
    printf("\n[Scenario B] Tier 0-Only Host (Graceful Degradation)\n");
    reset_logs();
    engine_write_u32(DIRECTOR_VA, 0u);

    fb_host_api_v1_t api;
    memset(&api, 0, sizeof(api));
    api.struct_size = (uint32_t)sizeof(api);
    api.abi_version = FB_HOST_ABI_VERSION;
    api.host_name = "VIctory-HLE-Mock-Tier0Only";
    api.host_version = "0.1.0-native";
    api.game_root_dir = L"D:\\Games\\GTA6";
    api.mods_root_dir = mods_dir;
    api.capabilities =
        FB_CAP_IO_VIRTUAL |
        FB_CAP_DSTORAGE_INTERCEPT |
        FB_CAP_D3D12_SHADERS |
        FB_CAP_FRAME_LIFECYCLE;
    api.log = host_log_cb;
    /* Optional Tier 1/2 pointers intentionally NULL */

    fb_modcore_callbacks_v1_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.struct_size = (uint32_t)sizeof(cb);

    CHECK(attach_fn(&api, &cb) == FB_OK);
    CHECK(logs_contain(FB_LOG_WARN, "fpv_toggle", "FB_CAP_TITLE_MEM_RW"));
    CHECK(logs_contain(FB_LOG_WARN, "fpv_toggle", "FB_CAP_RAGE_SYMBOL_REG"));
    CHECK(logs_contain(FB_LOG_WARN, "fpv_toggle", "FB_CAP_INPUT_EVENTS"));

    /* Lifecycle & input calls must succeed with 0 crashes and without mutating title memory */
    cb.on_frame_begin(1u, 1.0 / 60.0);
    fb_input_event_v1_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.struct_size = (uint32_t)sizeof(ev);
    ev.device_kind = 0u;
    ev.code = 0x56u;
    ev.action = (uint32_t)FB_INPUT_PRESS;
    CHECK(cb.on_input_event(&ev) == 0);
    CHECK(engine_read_u32(DIRECTOR_VA) == 0u);
    cb.on_frame_present(NULL);
    cb.on_shutdown();
}

static void get_exe_dir(wchar_t* out_dir, size_t cap) {
    DWORD len = GetModuleFileNameW(NULL, out_dir, (DWORD)cap);
    if (len == 0 || len >= cap) {
        out_dir[0] = L'\0';
        return;
    }
    wchar_t* slash = wcsrchr(out_dir, L'\\');
    if (slash) {
        *slash = L'\0';
    }
}

int wmain(int argc, wchar_t** argv) {
    wchar_t exe_dir[MAX_PATH * 2];
    get_exe_dir(exe_dir, sizeof(exe_dir) / sizeof(exe_dir[0]));

    wchar_t core_path[MAX_PATH * 2];
    wchar_t plugin_path[MAX_PATH * 2];
    const wchar_t* mods_dir_arg = NULL;
    const wchar_t* expect_dir_arg = NULL;
    int size_preserve_check = 0;

    swprintf(core_path, sizeof(core_path) / sizeof(core_path[0]), L"%ls\\fireball_core.dll", exe_dir);
    swprintf(plugin_path, sizeof(plugin_path) / sizeof(plugin_path[0]), L"%ls\\fpv_toggle.fbplugin.dll", exe_dir);

    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--core") == 0 && i + 1 < argc) {
            swprintf(core_path, sizeof(core_path) / sizeof(core_path[0]), L"%ls", argv[++i]);
        } else if (wcscmp(argv[i], L"--plugin") == 0 && i + 1 < argc) {
            swprintf(plugin_path, sizeof(plugin_path) / sizeof(plugin_path[0]), L"%ls", argv[++i]);
        } else if (wcscmp(argv[i], L"--mods-dir") == 0 && i + 1 < argc) {
            mods_dir_arg = argv[++i];
        } else if (wcscmp(argv[i], L"--expect-dir") == 0 && i + 1 < argc) {
            expect_dir_arg = argv[++i];
        } else if (wcscmp(argv[i], L"--size-preserve-check") == 0) {
            size_preserve_check = 1;
        }
    }

    wchar_t mods_dir[MAX_PATH * 2];
    if (mods_dir_arg) {
        swprintf(mods_dir, sizeof(mods_dir) / sizeof(mods_dir[0]), L"%ls", mods_dir_arg);
        /* Ensure fpv_toggle.fbplugin.dll is staged in mods_dir if built plugin exists */
        wchar_t dst_plugin[MAX_PATH * 2];
        swprintf(dst_plugin, sizeof(dst_plugin) / sizeof(dst_plugin[0]), L"%ls\\fpv_toggle.fbplugin.dll", mods_dir);
        if (GetFileAttributesW(dst_plugin) == INVALID_FILE_ATTRIBUTES) {
            CHECK(CopyFileW(plugin_path, dst_plugin, FALSE) != 0);
        }
    } else {
        swprintf(mods_dir, sizeof(mods_dir) / sizeof(mods_dir[0]), L"%ls\\mock_mods_synthetic", exe_dir);
        CreateDirectoryW(mods_dir, NULL);
        wchar_t bundle_path[MAX_PATH * 2];
        wchar_t dst_plugin[MAX_PATH * 2];
        swprintf(bundle_path, sizeof(bundle_path) / sizeof(bundle_path[0]), L"%ls\\fireball_bundle.fbin", mods_dir);
        swprintf(dst_plugin, sizeof(dst_plugin) / sizeof(dst_plugin[0]), L"%ls\\fpv_toggle.fbplugin.dll", mods_dir);
        CHECK(write_synthetic_bundle(bundle_path) != 0);
        CHECK(CopyFileW(plugin_path, dst_plugin, FALSE) != 0);
    }

    printf("Loading fireball_core.dll dynamically via LoadLibraryW...\n");
    HMODULE hCore = LoadLibraryW(core_path);
    CHECK(hCore != NULL);
    if (!hCore) {
        fprintf(stderr, "LoadLibraryW failed (Win32 error %lu)\n", (unsigned long)GetLastError());
        return 1;
    }

    fb_attach_host_fn attach_fn = (fb_attach_host_fn)GetProcAddress(hCore, "fb_attach_host");
    CHECK(attach_fn != NULL);
    if (attach_fn) {
        test_abi_validation(attach_fn);
        test_scenario_a_full_host(attach_fn, mods_dir, expect_dir_arg, size_preserve_check);
        test_scenario_b_tier0_only(attach_fn, mods_dir);
    }

    FreeLibrary(hCore);

    if (g_failures == 0) {
        printf("\nALL NATIVE MOCK HOST CHECKS PASSED (0 failures)\n");
        return 0;
    }
    fprintf(stderr, "\nNATIVE MOCK HOST FAILED (%d failures)\n", g_failures);
    return 1;
}
