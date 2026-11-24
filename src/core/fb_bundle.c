/*
 * fb_bundle.c — Binary `fireball_bundle.fbin` loader, validator, and O(1) hash index.
 *
 * Pure C11 + Win32 file I/O (zero third-party dependencies).
 */

#include "fb_bundle.h"
#include "fireball_host.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FB_FNV1A_OFFSET 0xCBF29CE484222325ull
#define FB_FNV1A_PRIME  0x00000100000001B3ull

uint64_t fb_fnv1a_64(const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = FB_FNV1A_OFFSET;
    for (size_t i = 0; i < len; ++i) {
        h ^= (uint64_t)p[i];
        h *= FB_FNV1A_PRIME;
    }
    return h;
}

uint64_t fb_dstorage_key_hash(const char* norm_path, size_t path_len, uint64_t file_offset, uint32_t orig_uncomp_size) {
    uint64_t h = FB_FNV1A_OFFSET;
    for (size_t i = 0; i < path_len; ++i) {
        h ^= (uint64_t)(uint8_t)norm_path[i];
        h *= FB_FNV1A_PRIME;
    }
    /* NUL separator */
    h ^= 0ull;
    h *= FB_FNV1A_PRIME;

    uint8_t tail[12];
    memcpy(tail, &file_offset, 8);
    memcpy(tail + 8, &orig_uncomp_size, 4);
    for (size_t i = 0; i < sizeof(tail); ++i) {
        h ^= (uint64_t)tail[i];
        h *= FB_FNV1A_PRIME;
    }
    return h;
}

uint64_t fb_dstorage_fallback_hash(const char* norm_path, size_t path_len, uint64_t file_offset) {
    return fb_dstorage_key_hash(norm_path, path_len, file_offset, 0u);
}

uint32_t fb_crc32_ieee(const void* data, size_t len) {
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

static uint32_t fb_next_pow2(uint32_t n) {
    uint32_t p = 8u;
    while (p < n && p < 0x80000000u) {
        p <<= 1;
    }
    return p;
}

static char fb_ascii_lower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return (char)(c - 'A' + 'a');
    }
    return c;
}

int fb_normalize_wide_path(
    const wchar_t* wide_path,
    const char*    game_root_utf8,
    size_t         game_root_len,
    char*          out_buf,
    size_t         out_cap)
{
    if (!wide_path || !out_buf || out_cap < 2) {
        return -1;
    }

    char utf8_tmp[1024];
    int n = WideCharToMultiByte(CP_UTF8, 0, wide_path, -1, utf8_tmp, (int)sizeof(utf8_tmp), NULL, NULL);
    if (n <= 1) {
        return -1;
    }
    size_t raw_len = (size_t)(n - 1);

    /* Normalize slashes and lowercase ASCII in place */
    for (size_t i = 0; i < raw_len; ++i) {
        if (utf8_tmp[i] == '\\') {
            utf8_tmp[i] = '/';
        } else {
            utf8_tmp[i] = fb_ascii_lower(utf8_tmp[i]);
        }
    }

    const char* p = utf8_tmp;
    size_t len = raw_len;

    /* Strip Win32 extended-length prefix //?/ if present */
    if (len >= 4 && p[0] == '/' && p[1] == '/' && p[2] == '?' && p[3] == '/') {
        p += 4;
        len -= 4;
    }

    /* Strip game_root_dir prefix if caller passed an absolute path under game_root_dir */
    if (game_root_utf8 && game_root_len > 0 && len > game_root_len) {
        if (memcmp(p, game_root_utf8, game_root_len) == 0 && p[game_root_len] == '/') {
            p += game_root_len + 1;
            len -= game_root_len + 1;
        }
    }

    /* Trim leading whitespace, './', and '/' */
    while (len > 0 && (*p == ' ' || *p == '\t')) {
        ++p;
        --len;
    }
    while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t' || p[len - 1] == '/')) {
        --len;
    }
    while (len >= 2 && p[0] == '.' && p[1] == '/') {
        p += 2;
        len -= 2;
    }
    while (len > 0 && p[0] == '/') {
        ++p;
        --len;
    }

    /* Collapse segments, remove empty/'.' segments, reject '..' */
    size_t out_len = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && p[i] == '/') {
            ++i;
        }
        if (i >= len) {
            break;
        }
        size_t seg_start = i;
        while (i < len && p[i] != '/') {
            ++i;
        }
        size_t seg_len = i - seg_start;
        if (seg_len == 1 && p[seg_start] == '.') {
            continue;
        }
        if (seg_len == 2 && p[seg_start] == '.' && p[seg_start + 1] == '.') {
            return -1; /* Path traversal rejected */
        }
        if (out_len > 0) {
            if (out_len + 1 >= out_cap) {
                return -1;
            }
            out_buf[out_len++] = '/';
        }
        if (out_len + seg_len >= out_cap) {
            return -1;
        }
        memcpy(out_buf + out_len, p + seg_start, seg_len);
        out_len += seg_len;
    }

    out_buf[out_len] = '\0';
    return (int)out_len;
}

static void fb_set_err(char* err_buf, size_t err_cap, const char* msg) {
    if (err_buf && err_cap > 0) {
        snprintf(err_buf, err_cap, "%s", msg);
    }
}

static int fb_validate_string(const uint8_t* raw, size_t raw_size, uint32_t off, uint32_t len) {
    if ((size_t)off >= raw_size) {
        return 0;
    }
    if ((size_t)len >= raw_size - (size_t)off) {
        return 0;
    }
    return raw[(size_t)off + (size_t)len] == 0;
}

static int fb_validate_range(size_t raw_size, uint64_t off, uint64_t len) {
    if (off > (uint64_t)raw_size) {
        return 0;
    }
    if (len > (uint64_t)raw_size - off) {
        return 0;
    }
    return 1;
}

static int fb_build_hash_tables(fb_bundle_t* b, char* err_buf, size_t err_cap) {
    if (b->ds_count > 0) {
        uint32_t cap = fb_next_pow2(b->ds_count * 2u + 1u);
        b->ds_exact_slots = (fb_hash_slot_t*)calloc(cap, sizeof(fb_hash_slot_t));
        b->ds_fallback_slots = (fb_hash_slot_t*)calloc(cap, sizeof(fb_hash_slot_t));
        if (!b->ds_exact_slots || !b->ds_fallback_slots) {
            fb_set_err(err_buf, err_cap, "Out of memory allocating DirectStorage hash tables");
            return FB_ERR_INTERNAL;
        }
        b->ds_exact_mask = cap - 1u;
        b->ds_fallback_mask = cap - 1u;

        for (uint32_t i = 0; i < b->ds_count; ++i) {
            const fb_bundle_dstorage_entry_t* e = &b->ds_entries[i];
            const char* path = (const char*)(b->raw_data + e->path_offset);

            /* Insert into exact table */
            uint32_t slot = (uint32_t)(e->key_hash & b->ds_exact_mask);
            while (b->ds_exact_slots[slot].occupied) {
                slot = (slot + 1u) & b->ds_exact_mask;
            }
            b->ds_exact_slots[slot].hash = e->key_hash;
            b->ds_exact_slots[slot].index = i;
            b->ds_exact_slots[slot].occupied = 1u;

            /* Insert into fallback (path, file_offset) table; prefer FULL_ASSET if colliding */
            uint64_t fh = fb_dstorage_fallback_hash(path, e->path_len, e->file_offset);
            uint32_t fslot = (uint32_t)(fh & b->ds_fallback_mask);
            while (b->ds_fallback_slots[fslot].occupied) {
                const fb_bundle_dstorage_entry_t* prev = &b->ds_entries[b->ds_fallback_slots[fslot].index];
                const char* prev_path = (const char*)(b->raw_data + prev->path_offset);
                if (b->ds_fallback_slots[fslot].hash == fh &&
                    prev->file_offset == e->file_offset &&
                    prev->path_len == e->path_len &&
                    memcmp(prev_path, path, e->path_len) == 0)
                {
                    if ((e->flags & FB_DS_FLAG_FULL_ASSET) != 0 && (prev->flags & FB_DS_FLAG_FULL_ASSET) == 0) {
                        b->ds_fallback_slots[fslot].index = i;
                    }
                    goto next_ds_entry;
                }
                fslot = (fslot + 1u) & b->ds_fallback_mask;
            }
            b->ds_fallback_slots[fslot].hash = fh;
            b->ds_fallback_slots[fslot].index = i;
            b->ds_fallback_slots[fslot].occupied = 1u;
        next_ds_entry:
            ;
        }
    }

    if (b->ov_count > 0) {
        uint32_t cap = fb_next_pow2(b->ov_count * 2u + 1u);
        b->ov_slots = (fb_hash_slot_t*)calloc(cap, sizeof(fb_hash_slot_t));
        if (!b->ov_slots) {
            fb_set_err(err_buf, err_cap, "Out of memory allocating overlay hash table");
            return FB_ERR_INTERNAL;
        }
        b->ov_mask = cap - 1u;
        for (uint32_t i = 0; i < b->ov_count; ++i) {
            const fb_bundle_overlay_entry_t* e = &b->ov_entries[i];
            uint32_t slot = (uint32_t)(e->path_hash & b->ov_mask);
            while (b->ov_slots[slot].occupied) {
                slot = (slot + 1u) & b->ov_mask;
            }
            b->ov_slots[slot].hash = e->path_hash;
            b->ov_slots[slot].index = i;
            b->ov_slots[slot].occupied = 1u;
        }
    }

    if (b->cfg_count > 0) {
        uint32_t cap = fb_next_pow2(b->cfg_count * 2u + 1u);
        b->cfg_slots = (fb_hash_slot_t*)calloc(cap, sizeof(fb_hash_slot_t));
        if (!b->cfg_slots) {
            fb_set_err(err_buf, err_cap, "Out of memory allocating plugin config hash table");
            return FB_ERR_INTERNAL;
        }
        b->cfg_mask = cap - 1u;
        for (uint32_t i = 0; i < b->cfg_count; ++i) {
            const fb_bundle_plugin_cfg_entry_t* e = &b->cfg_entries[i];
            uint32_t slot = (uint32_t)(e->mod_id_hash & b->cfg_mask);
            while (b->cfg_slots[slot].occupied) {
                slot = (slot + 1u) & b->cfg_mask;
            }
            b->cfg_slots[slot].hash = e->mod_id_hash;
            b->cfg_slots[slot].index = i;
            b->cfg_slots[slot].occupied = 1u;
        }
    }

    return FB_OK;
}

int fb_bundle_load_file(const wchar_t* file_path, fb_bundle_t* out_bundle, char* err_buf, size_t err_cap) {
    if (!file_path || !out_bundle) {
        fb_set_err(err_buf, err_cap, "Invalid arguments to fb_bundle_load_file");
        return FB_ERR_INVALID_ARG;
    }
    memset(out_bundle, 0, sizeof(*out_bundle));

    HANDLE hFile = CreateFileW(
        file_path,
        GENERIC_READ,
        FILE_SHARE_READ,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            fb_set_err(err_buf, err_cap, "Bundle file not found");
            return FB_ERR_NOT_FOUND;
        }
        if (err_buf && err_cap > 0) {
            snprintf(err_buf, err_cap, "Failed to open bundle file (Win32 error %lu)", (unsigned long)err);
        }
        return FB_ERR_INTERNAL;
    }

    LARGE_INTEGER li_size;
    if (!GetFileSizeEx(hFile, &li_size) || li_size.QuadPart < (LONGLONG)FB_BUNDLE_HEADER_SIZE) {
        CloseHandle(hFile);
        fb_set_err(err_buf, err_cap, "Bundle file is too small or unreadable");
        return FB_ERR_INVALID_ARG;
    }
    if (li_size.QuadPart > (LONGLONG)(512ull * 1024ull * 1024ull)) {
        CloseHandle(hFile);
        fb_set_err(err_buf, err_cap, "Bundle file exceeds 512 MiB safety cap");
        return FB_ERR_INVALID_ARG;
    }

    size_t file_size = (size_t)li_size.QuadPart;
    uint8_t* raw = (uint8_t*)malloc(file_size);
    if (!raw) {
        CloseHandle(hFile);
        fb_set_err(err_buf, err_cap, "Out of memory reading bundle");
        return FB_ERR_INTERNAL;
    }

    size_t total_read = 0;
    while (total_read < file_size) {
        size_t rem = file_size - total_read;
        DWORD to_read = (rem > 0x10000000u) ? 0x10000000u : (DWORD)rem;
        DWORD got = 0;
        if (!ReadFile(hFile, raw + total_read, to_read, &got, NULL) || got == 0) {
            free(raw);
            CloseHandle(hFile);
            fb_set_err(err_buf, err_cap, "Short read while loading bundle");
            return FB_ERR_INTERNAL;
        }
        total_read += (size_t)got;
    }
    CloseHandle(hFile);

    fb_bundle_header_t hdr;
    memcpy(&hdr, raw, sizeof(hdr));
    if (hdr.magic != FB_BUNDLE_MAGIC || hdr.version != FB_BUNDLE_VERSION || hdr.header_size != FB_BUNDLE_HEADER_SIZE) {
        free(raw);
        fb_set_err(err_buf, err_cap, "Invalid fireball_bundle.fbin header magic/version");
        return FB_ERR_ABI_MISMATCH;
    }
    if (hdr.total_file_size != (uint64_t)file_size) {
        free(raw);
        fb_set_err(err_buf, err_cap, "Bundle total_file_size does not match file length on disk");
        return FB_ERR_INVALID_ARG;
    }

    uint64_t ds_bytes = (uint64_t)hdr.dstorage_count * sizeof(fb_bundle_dstorage_entry_t);
    uint64_t ov_bytes = (uint64_t)hdr.overlay_count * sizeof(fb_bundle_overlay_entry_t);
    uint64_t cfg_bytes = (uint64_t)hdr.plugin_config_count * sizeof(fb_bundle_plugin_cfg_entry_t);

    if (!fb_validate_range(file_size, hdr.dstorage_table_off, ds_bytes) ||
        !fb_validate_range(file_size, hdr.overlay_table_off, ov_bytes) ||
        !fb_validate_range(file_size, hdr.plugin_cfg_table_off, cfg_bytes))
    {
        free(raw);
        fb_set_err(err_buf, err_cap, "Bundle section table offsets out of bounds");
        return FB_ERR_INVALID_ARG;
    }

    out_bundle->raw_data = raw;
    out_bundle->raw_size = file_size;
    out_bundle->ds_entries = (const fb_bundle_dstorage_entry_t*)(raw + hdr.dstorage_table_off);
    out_bundle->ds_count = hdr.dstorage_count;
    out_bundle->ov_entries = (const fb_bundle_overlay_entry_t*)(raw + hdr.overlay_table_off);
    out_bundle->ov_count = hdr.overlay_count;
    out_bundle->cfg_entries = (const fb_bundle_plugin_cfg_entry_t*)(raw + hdr.plugin_cfg_table_off);
    out_bundle->cfg_count = hdr.plugin_config_count;

    /* Validate every entry's string bounds, payload bounds, key hash, and CRC32 */
    for (uint32_t i = 0; i < out_bundle->ds_count; ++i) {
        const fb_bundle_dstorage_entry_t* e = &out_bundle->ds_entries[i];
        if (!fb_validate_string(raw, file_size, e->path_offset, e->path_len) ||
            !fb_validate_range(file_size, e->payload_offset, e->payload_size))
        {
            fb_bundle_free(out_bundle);
            fb_set_err(err_buf, err_cap, "DirectStorage entry string or payload out of bounds");
            return FB_ERR_INVALID_ARG;
        }
        const char* p = (const char*)(raw + e->path_offset);
        if (fb_dstorage_key_hash(p, e->path_len, e->file_offset, e->orig_uncomp_size) != e->key_hash) {
            fb_bundle_free(out_bundle);
            fb_set_err(err_buf, err_cap, "DirectStorage entry key_hash mismatch");
            return FB_ERR_INVALID_ARG;
        }
        if (fb_crc32_ieee(raw + e->payload_offset, e->payload_size) != e->payload_crc32) {
            fb_bundle_free(out_bundle);
            fb_set_err(err_buf, err_cap, "DirectStorage entry payload CRC32 mismatch");
            return FB_ERR_INVALID_ARG;
        }
    }

    for (uint32_t i = 0; i < out_bundle->ov_count; ++i) {
        const fb_bundle_overlay_entry_t* e = &out_bundle->ov_entries[i];
        if (!fb_validate_string(raw, file_size, e->path_offset, e->path_len) ||
            !fb_validate_range(file_size, e->payload_offset, e->payload_size))
        {
            fb_bundle_free(out_bundle);
            fb_set_err(err_buf, err_cap, "Overlay entry string or payload out of bounds");
            return FB_ERR_INVALID_ARG;
        }
        const char* p = (const char*)(raw + e->path_offset);
        if (fb_fnv1a_64(p, e->path_len) != e->path_hash) {
            fb_bundle_free(out_bundle);
            fb_set_err(err_buf, err_cap, "Overlay entry path_hash mismatch");
            return FB_ERR_INVALID_ARG;
        }
        if (fb_crc32_ieee(raw + e->payload_offset, (size_t)e->payload_size) != e->payload_crc32) {
            fb_bundle_free(out_bundle);
            fb_set_err(err_buf, err_cap, "Overlay entry payload CRC32 mismatch");
            return FB_ERR_INVALID_ARG;
        }
    }

    for (uint32_t i = 0; i < out_bundle->cfg_count; ++i) {
        const fb_bundle_plugin_cfg_entry_t* e = &out_bundle->cfg_entries[i];
        if (!fb_validate_string(raw, file_size, e->mod_id_offset, e->mod_id_len) ||
            !fb_validate_string(raw, file_size, e->json_offset, e->json_len))
        {
            fb_bundle_free(out_bundle);
            fb_set_err(err_buf, err_cap, "Plugin config entry string out of bounds");
            return FB_ERR_INVALID_ARG;
        }
        const char* mid = (const char*)(raw + e->mod_id_offset);
        if (fb_fnv1a_64(mid, e->mod_id_len) != e->mod_id_hash) {
            fb_bundle_free(out_bundle);
            fb_set_err(err_buf, err_cap, "Plugin config mod_id_hash mismatch");
            return FB_ERR_INVALID_ARG;
        }
    }

    int rc = fb_build_hash_tables(out_bundle, err_buf, err_cap);
    if (rc != FB_OK) {
        fb_bundle_free(out_bundle);
        return rc;
    }
    return FB_OK;
}

void fb_bundle_free(fb_bundle_t* bundle) {
    if (!bundle) {
        return;
    }
    free(bundle->ds_exact_slots);
    free(bundle->ds_fallback_slots);
    free(bundle->ov_slots);
    free(bundle->cfg_slots);
    free(bundle->raw_data);
    memset(bundle, 0, sizeof(*bundle));
}

const fb_bundle_dstorage_entry_t* fb_bundle_find_dstorage(
    const fb_bundle_t* bundle,
    const char*        norm_path,
    size_t             path_len,
    uint64_t           file_offset,
    uint32_t           uncomp_size)
{
    if (!bundle || !norm_path || bundle->ds_count == 0) {
        return NULL;
    }

    /* 1. Exact O(1) lookup on (norm_path, file_offset, uncomp_size) */
    if (uncomp_size != 0 && bundle->ds_exact_slots) {
        uint64_t h = fb_dstorage_key_hash(norm_path, path_len, file_offset, uncomp_size);
        uint32_t slot = (uint32_t)(h & bundle->ds_exact_mask);
        for (uint32_t probe = 0; probe <= bundle->ds_exact_mask; ++probe) {
            const fb_hash_slot_t* s = &bundle->ds_exact_slots[slot];
            if (!s->occupied) {
                break;
            }
            if (s->hash == h) {
                const fb_bundle_dstorage_entry_t* e = &bundle->ds_entries[s->index];
                const char* epath = (const char*)(bundle->raw_data + e->path_offset);
                if (e->file_offset == file_offset &&
                    e->orig_uncomp_size == uncomp_size &&
                    e->path_len == path_len &&
                    memcmp(epath, norm_path, path_len) == 0)
                {
                    return e;
                }
            }
            slot = (slot + 1u) & bundle->ds_exact_mask;
        }
    }

    /* 2. Fallback O(1) lookup on (norm_path, file_offset) when caller leaves uncomp_size == 0 */
    if (uncomp_size == 0 && bundle->ds_fallback_slots) {
        uint64_t fh = fb_dstorage_fallback_hash(norm_path, path_len, file_offset);
        uint32_t slot = (uint32_t)(fh & bundle->ds_fallback_mask);
        for (uint32_t probe = 0; probe <= bundle->ds_fallback_mask; ++probe) {
            const fb_hash_slot_t* s = &bundle->ds_fallback_slots[slot];
            if (!s->occupied) {
                break;
            }
            if (s->hash == fh) {
                const fb_bundle_dstorage_entry_t* e = &bundle->ds_entries[s->index];
                const char* epath = (const char*)(bundle->raw_data + e->path_offset);
                if (e->file_offset == file_offset &&
                    e->path_len == path_len &&
                    memcmp(epath, norm_path, path_len) == 0)
                {
                    return e;
                }
            }
            slot = (slot + 1u) & bundle->ds_fallback_mask;
        }
    }

    return NULL;
}

const fb_bundle_overlay_entry_t* fb_bundle_find_overlay(
    const fb_bundle_t* bundle,
    const char*        norm_path,
    size_t             path_len)
{
    if (!bundle || !norm_path || bundle->ov_count == 0 || !bundle->ov_slots) {
        return NULL;
    }
    uint64_t h = fb_fnv1a_64(norm_path, path_len);
    uint32_t slot = (uint32_t)(h & bundle->ov_mask);
    for (uint32_t probe = 0; probe <= bundle->ov_mask; ++probe) {
        const fb_hash_slot_t* s = &bundle->ov_slots[slot];
        if (!s->occupied) {
            break;
        }
        if (s->hash == h) {
            const fb_bundle_overlay_entry_t* e = &bundle->ov_entries[s->index];
            const char* epath = (const char*)(bundle->raw_data + e->path_offset);
            if (e->path_len == path_len && memcmp(epath, norm_path, path_len) == 0) {
                return e;
            }
        }
        slot = (slot + 1u) & bundle->ov_mask;
    }
    return NULL;
}

const fb_bundle_plugin_cfg_entry_t* fb_bundle_find_plugin_cfg(
    const fb_bundle_t* bundle,
    const char*        mod_id,
    size_t             mod_id_len)
{
    if (!bundle || !mod_id || bundle->cfg_count == 0 || !bundle->cfg_slots) {
        return NULL;
    }
    uint64_t h = fb_fnv1a_64(mod_id, mod_id_len);
    uint32_t slot = (uint32_t)(h & bundle->cfg_mask);
    for (uint32_t probe = 0; probe <= bundle->cfg_mask; ++probe) {
        const fb_hash_slot_t* s = &bundle->cfg_slots[slot];
        if (!s->occupied) {
            break;
        }
        if (s->hash == h) {
            const fb_bundle_plugin_cfg_entry_t* e = &bundle->cfg_entries[s->index];
            const char* eid = (const char*)(bundle->raw_data + e->mod_id_offset);
            if (e->mod_id_len == mod_id_len && memcmp(eid, mod_id, mod_id_len) == 0) {
                return e;
            }
        }
        slot = (slot + 1u) & bundle->cfg_mask;
    }
    return NULL;
}
