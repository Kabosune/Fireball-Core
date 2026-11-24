/*
 * fb_bundle.h — Compiled Fireball Patch Bundle (`fireball_bundle.fbin`) format & O(1) index.
 *
 * Pure C11, zero third-party dependencies. Layout matches `fireball/bundle.py` byte-for-byte.
 */

#ifndef FIREBALL_CORE_FB_BUNDLE_H
#define FIREBALL_CORE_FB_BUNDLE_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_BUNDLE_MAGIC         0x4E494246u /* "FBIN" little-endian */
#define FB_BUNDLE_VERSION       1u
#define FB_BUNDLE_HEADER_SIZE   64u

#define FB_DS_FLAG_FULL_ASSET   (1u << 0)
#define FB_DS_FLAG_PAGE_SLICE   (1u << 1)
#define FB_DS_FLAG_TEXT_PATCH   (1u << 2)
#define FB_DS_FLAG_PSO_PATCH    (1u << 3)

typedef struct fb_bundle_header {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t header_size;
    uint32_t dstorage_count;
    uint32_t overlay_count;
    uint32_t plugin_config_count;
    uint32_t reserved0;
    uint64_t dstorage_table_off;
    uint64_t overlay_table_off;
    uint64_t plugin_cfg_table_off;
    uint64_t total_file_size;
} fb_bundle_header_t;

typedef struct fb_bundle_dstorage_entry {
    uint64_t key_hash;
    uint64_t file_offset;
    uint32_t orig_uncomp_size;
    uint32_t payload_size;
    uint64_t payload_offset;
    uint32_t path_offset;
    uint32_t path_len;
    uint32_t flags;
    uint32_t required_tier;
    uint64_t required_caps;
    uint32_t payload_crc32;
    uint32_t reserved;
} fb_bundle_dstorage_entry_t;

typedef struct fb_bundle_overlay_entry {
    uint64_t path_hash;
    uint64_t payload_offset;
    uint64_t payload_size;
    uint32_t path_offset;
    uint32_t path_len;
    uint32_t payload_crc32;
    uint32_t flags;
} fb_bundle_overlay_entry_t;

typedef struct fb_bundle_plugin_cfg_entry {
    uint64_t mod_id_hash;
    uint32_t mod_id_offset;
    uint32_t mod_id_len;
    uint32_t json_offset;
    uint32_t json_len;
    uint64_t reserved;
} fb_bundle_plugin_cfg_entry_t;

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(fb_bundle_header_t) == 64, "fb_bundle_header_t must be 64 bytes");
_Static_assert(sizeof(fb_bundle_dstorage_entry_t) == 64, "fb_bundle_dstorage_entry_t must be 64 bytes");
_Static_assert(sizeof(fb_bundle_overlay_entry_t) == 40, "fb_bundle_overlay_entry_t must be 40 bytes");
_Static_assert(sizeof(fb_bundle_plugin_cfg_entry_t) == 32, "fb_bundle_plugin_cfg_entry_t must be 32 bytes");
#endif

typedef struct fb_hash_slot {
    uint64_t hash;
    uint32_t index;   /* index into entries array */
    uint32_t occupied;
} fb_hash_slot_t;

typedef struct fb_bundle {
    uint8_t*                          raw_data;
    size_t                            raw_size;
    const fb_bundle_dstorage_entry_t* ds_entries;
    uint32_t                          ds_count;
    const fb_bundle_overlay_entry_t*  ov_entries;
    uint32_t                          ov_count;
    const fb_bundle_plugin_cfg_entry_t* cfg_entries;
    uint32_t                          cfg_count;

    /* Open-addressing hash tables for O(1) runtime lookup */
    fb_hash_slot_t* ds_exact_slots;
    uint32_t        ds_exact_mask;

    fb_hash_slot_t* ds_fallback_slots;
    uint32_t        ds_fallback_mask;

    fb_hash_slot_t* ov_slots;
    uint32_t        ov_mask;

    fb_hash_slot_t* cfg_slots;
    uint32_t        cfg_mask;
} fb_bundle_t;

/* Hash & normalization utilities */
uint64_t fb_fnv1a_64(const void* data, size_t len);
uint64_t fb_dstorage_key_hash(const char* norm_path, size_t path_len, uint64_t file_offset, uint32_t orig_uncomp_size);
uint64_t fb_dstorage_fallback_hash(const char* norm_path, size_t path_len, uint64_t file_offset);
uint32_t fb_crc32_ieee(const void* data, size_t len);

/*
 * Normalize a UTF-16 path to lowercase UTF-8 with '/' separators, optionally
 * stripping `game_root_utf8` prefix and leading './' or '/'.
 * Returns length written (excluding NUL) on success, or -1 on error / traversal.
 */
int fb_normalize_wide_path(
    const wchar_t* wide_path,
    const char*    game_root_utf8,
    size_t         game_root_len,
    char*          out_buf,
    size_t         out_cap);

/* Load, parse, validate, and index `fireball_bundle.fbin` from disk. */
int  fb_bundle_load_file(const wchar_t* file_path, fb_bundle_t* out_bundle, char* err_buf, size_t err_cap);
void fb_bundle_free(fb_bundle_t* bundle);

/* O(1) lookups */
const fb_bundle_dstorage_entry_t* fb_bundle_find_dstorage(
    const fb_bundle_t* bundle,
    const char*        norm_path,
    size_t             path_len,
    uint64_t           file_offset,
    uint32_t           uncomp_size);

const fb_bundle_overlay_entry_t* fb_bundle_find_overlay(
    const fb_bundle_t* bundle,
    const char*        norm_path,
    size_t             path_len);

const fb_bundle_plugin_cfg_entry_t* fb_bundle_find_plugin_cfg(
    const fb_bundle_t* bundle,
    const char*        mod_id,
    size_t             mod_id_len);

#ifdef __cplusplus
}
#endif

#endif /* FIREBALL_CORE_FB_BUNDLE_H */
