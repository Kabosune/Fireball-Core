"""
Compiled Patch Bundle (`fireball_bundle.fbin`) compiler, serializer, and reader.

Compiles one or more mods (loose folders or `.gta6mod` ZIPs) in load order into a
compact, deterministic binary bundle consumed at runtime by native `fireball_core.dll`
with O(1) lookup and zero runtime XML/KV/PSO parsing.

Binary layout (all integers little-endian):
  - Header (64 bytes): `fb_bundle_header_t` (magic "FBIN", version 1)
  - DirectStorage table (`dstorage_count` x 64 bytes): indexed by normalized
    `(archive_path, file_offset, orig_uncomp_size)` covering both full-asset
    offsets and per-64KiB-page `page_offset` slices.
  - Loose-file overlay table (`overlay_count` x 40 bytes): indexed by normalized
    game-relative path for `on_file_open`.
  - Plugin config table (`plugin_config_count` x 32 bytes): maps `mod_id` ->
    UTF-8 JSON config string for `config_get_json`.
  - Deduplicated NUL-terminated UTF-8 string pool.
  - 16-byte aligned uncompressed payload data section.
"""

from __future__ import annotations

import binascii
import json
import os
import struct
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .abi import caps_from_names
from .archive_reader import ZlibPageInfo, parse_int_hex, read_paged_zlib_pages_at
from .mergers import compact_kv_bytes, compact_xml_slice, merge_kv_bytes, merge_xml_bytes
from .mod_source import ModManifest, VirtualModSource, load_manifest, open_mod_source
from .pso_patcher import patch_pso_bytes

FB_BUNDLE_MAGIC = 0x4E494246  # "FBIN" little-endian
FB_BUNDLE_VERSION = 1
FB_BUNDLE_HEADER_SIZE = 64

FB_DS_ENTRY_SIZE = 64
FB_OVERLAY_ENTRY_SIZE = 40
FB_PLUGIN_CFG_ENTRY_SIZE = 32

FB_DS_FLAG_FULL_ASSET = 1 << 0
FB_DS_FLAG_PAGE_SLICE = 1 << 1
FB_DS_FLAG_TEXT_PATCH = 1 << 2
FB_DS_FLAG_PSO_PATCH = 1 << 3

_HEADER_STRUCT = struct.Struct("<IIIIIIIIQQQQ")
_DS_ENTRY_STRUCT = struct.Struct("<QQIIQIIIIQII")
_OVERLAY_ENTRY_STRUCT = struct.Struct("<QQQIIII")
_PLUGIN_CFG_STRUCT = struct.Struct("<QIIIIQ")

assert _HEADER_STRUCT.size == FB_BUNDLE_HEADER_SIZE
assert _DS_ENTRY_STRUCT.size == FB_DS_ENTRY_SIZE
assert _OVERLAY_ENTRY_STRUCT.size == FB_OVERLAY_ENTRY_SIZE
assert _PLUGIN_CFG_STRUCT.size == FB_PLUGIN_CFG_ENTRY_SIZE

_FNV1A_64_OFFSET = 0xCBF29CE484222325
_FNV1A_64_PRIME = 0x00000100000001B3
_U64_MASK = 0xFFFFFFFFFFFFFFFF


def normalize_rel_path(path: str) -> str:
    """Normalize a game-relative path: forward slashes, lowercase, no leading './' or '/'."""
    p = path.replace("\\", "/").strip()
    while p.startswith("./"):
        p = p[2:]
    p = p.lstrip("/")
    parts = [seg for seg in p.split("/") if seg and seg != "."]
    if any(seg == ".." for seg in parts):
        raise ValueError(f"Path traversal not allowed in bundle path: {path!r}")
    return "/".join(parts).lower()


def fnv1a_64(data: bytes) -> int:
    h = _FNV1A_64_OFFSET
    for b in data:
        h ^= b
        h = (h * _FNV1A_64_PRIME) & _U64_MASK
    return h


def dstorage_key_hash(norm_path: str, file_offset: int, orig_uncomp_size: int) -> int:
    key = norm_path.encode("utf-8") + b"\x00" + struct.pack("<QI", file_offset, orig_uncomp_size)
    return fnv1a_64(key)


def _align16(val: int) -> int:
    return (val + 15) & ~15


@dataclass
class BundleDStorageEntry:
    archive_path: str
    file_offset: int
    orig_uncomp_size: int
    payload: bytes
    flags: int
    required_tier: int = 0
    required_caps: int = 0

    def __post_init__(self) -> None:
        self.archive_path = normalize_rel_path(self.archive_path)

    @property
    def key_hash(self) -> int:
        return dstorage_key_hash(self.archive_path, self.file_offset, self.orig_uncomp_size)

    @property
    def payload_crc32(self) -> int:
        return binascii.crc32(self.payload) & 0xFFFFFFFF


@dataclass
class BundleOverlayEntry:
    rel_path: str
    payload: bytes
    flags: int = 0

    def __post_init__(self) -> None:
        self.rel_path = normalize_rel_path(self.rel_path)

    @property
    def path_hash(self) -> int:
        return fnv1a_64(self.rel_path.encode("utf-8"))

    @property
    def payload_crc32(self) -> int:
        return binascii.crc32(self.payload) & 0xFFFFFFFF


@dataclass
class BundlePluginConfigEntry:
    mod_id: str
    config_json: str

    @property
    def mod_id_hash(self) -> int:
        return fnv1a_64(self.mod_id.encode("utf-8"))


@dataclass
class CompiledBundle:
    dstorage_entries: List[BundleDStorageEntry] = field(default_factory=list)
    overlay_entries: List[BundleOverlayEntry] = field(default_factory=list)
    plugin_configs: List[BundlePluginConfigEntry] = field(default_factory=list)


def serialize_bundle(bundle: CompiledBundle) -> bytes:
    """Serialize a CompiledBundle into deterministic `fireball_bundle.fbin` bytes."""
    ds_sorted = sorted(
        bundle.dstorage_entries,
        key=lambda e: (e.archive_path, e.file_offset, e.orig_uncomp_size),
    )
    ov_sorted = sorted(bundle.overlay_entries, key=lambda e: e.rel_path)
    cfg_sorted = sorted(bundle.plugin_configs, key=lambda e: e.mod_id)

    # Check for duplicate keys before writing
    seen_ds: set[Tuple[str, int, int]] = set()
    for e in ds_sorted:
        k = (e.archive_path, e.file_offset, e.orig_uncomp_size)
        if k in seen_ds:
            raise ValueError(f"Duplicate DirectStorage bundle key: {k}")
        seen_ds.add(k)

    seen_ov: set[str] = set()
    for e in ov_sorted:
        if e.rel_path in seen_ov:
            raise ValueError(f"Duplicate overlay bundle key: {e.rel_path}")
        seen_ov.add(e.rel_path)

    seen_cfg: set[str] = set()
    for e in cfg_sorted:
        if e.mod_id in seen_cfg:
            raise ValueError(f"Duplicate plugin config key: {e.mod_id}")
        seen_cfg.add(e.mod_id)

    dstorage_table_off = FB_BUNDLE_HEADER_SIZE
    overlay_table_off = dstorage_table_off + len(ds_sorted) * FB_DS_ENTRY_SIZE
    plugin_cfg_table_off = overlay_table_off + len(ov_sorted) * FB_OVERLAY_ENTRY_SIZE
    string_pool_start = plugin_cfg_table_off + len(cfg_sorted) * FB_PLUGIN_CFG_ENTRY_SIZE

    # Deterministic deduplicated UTF-8 NUL-terminated string pool
    all_strings: set[str] = set()
    for e in ds_sorted:
        all_strings.add(e.archive_path)
    for e in ov_sorted:
        all_strings.add(e.rel_path)
    for e in cfg_sorted:
        all_strings.add(e.mod_id)
        all_strings.add(e.config_json)

    string_pool = bytearray()
    str_offsets: Dict[str, Tuple[int, int]] = {}
    for s in sorted(all_strings):
        b = s.encode("utf-8")
        str_offsets[s] = (string_pool_start + len(string_pool), len(b))
        string_pool.extend(b)
        string_pool.append(0)

    payload_start = _align16(string_pool_start + len(string_pool))
    string_pad = b"\x00" * (payload_start - (string_pool_start + len(string_pool)))

    payload_blob = bytearray()
    ds_payload_offs: List[int] = []
    for e in ds_sorted:
        cur_off = payload_start + len(payload_blob)
        ds_payload_offs.append(cur_off)
        payload_blob.extend(e.payload)
        pad = _align16(len(e.payload)) - len(e.payload)
        if pad:
            payload_blob.extend(b"\x00" * pad)

    ov_payload_offs: List[int] = []
    for e in ov_sorted:
        cur_off = payload_start + len(payload_blob)
        ov_payload_offs.append(cur_off)
        payload_blob.extend(e.payload)
        pad = _align16(len(e.payload)) - len(e.payload)
        if pad:
            payload_blob.extend(b"\x00" * pad)

    total_size = payload_start + len(payload_blob)

    out = bytearray()
    out.extend(
        _HEADER_STRUCT.pack(
            FB_BUNDLE_MAGIC,
            FB_BUNDLE_VERSION,
            0,  # flags
            FB_BUNDLE_HEADER_SIZE,
            len(ds_sorted),
            len(ov_sorted),
            len(cfg_sorted),
            0,  # reserved0
            dstorage_table_off,
            overlay_table_off,
            plugin_cfg_table_off,
            total_size,
        )
    )

    for e, p_off in zip(ds_sorted, ds_payload_offs):
        s_off, s_len = str_offsets[e.archive_path]
        out.extend(
            _DS_ENTRY_STRUCT.pack(
                e.key_hash,
                e.file_offset,
                e.orig_uncomp_size,
                len(e.payload),
                p_off,
                s_off,
                s_len,
                e.flags,
                e.required_tier,
                e.required_caps,
                e.payload_crc32,
                0,
            )
        )

    for e, p_off in zip(ov_sorted, ov_payload_offs):
        s_off, s_len = str_offsets[e.rel_path]
        out.extend(
            _OVERLAY_ENTRY_STRUCT.pack(
                e.path_hash,
                p_off,
                len(e.payload),
                s_off,
                s_len,
                e.payload_crc32,
                e.flags,
            )
        )

    for e in cfg_sorted:
        id_off, id_len = str_offsets[e.mod_id]
        js_off, js_len = str_offsets[e.config_json]
        out.extend(
            _PLUGIN_CFG_STRUCT.pack(
                e.mod_id_hash,
                id_off,
                id_len,
                js_off,
                js_len,
                0,
            )
        )

    out.extend(string_pool)
    out.extend(string_pad)
    out.extend(payload_blob)
    assert len(out) == total_size
    return bytes(out)


def parse_bundle(raw: bytes) -> CompiledBundle:
    """Parse and validate `fireball_bundle.fbin` bytes back into a CompiledBundle."""
    if len(raw) < FB_BUNDLE_HEADER_SIZE:
        raise ValueError(f"Bundle too small: {len(raw)} bytes")

    (
        magic,
        version,
        _flags,
        header_size,
        ds_count,
        ov_count,
        cfg_count,
        _reserved0,
        ds_off,
        ov_off,
        cfg_off,
        total_size,
    ) = _HEADER_STRUCT.unpack_from(raw, 0)

    if magic != FB_BUNDLE_MAGIC:
        raise ValueError(f"Invalid bundle magic: 0x{magic:08X}")
    if version != FB_BUNDLE_VERSION:
        raise ValueError(f"Unsupported bundle version: {version}")
    if header_size != FB_BUNDLE_HEADER_SIZE:
        raise ValueError(f"Unexpected bundle header size: {header_size}")
    if total_size != len(raw):
        raise ValueError(f"Bundle size mismatch: header={total_size}, actual={len(raw)}")

    def read_str(off: int, length: int) -> str:
        if off + length >= len(raw) or raw[off + length] != 0:
            raise ValueError(f"Unterminated or out-of-bounds string at 0x{off:X} len={length}")
        return raw[off : off + length].decode("utf-8")

    ds_entries: List[BundleDStorageEntry] = []
    for i in range(ds_count):
        pos = ds_off + i * FB_DS_ENTRY_SIZE
        (
            khash,
            foff,
            orig_sz,
            pay_sz,
            pay_off,
            s_off,
            s_len,
            eflags,
            req_tier,
            req_caps,
            crc,
            _res,
        ) = _DS_ENTRY_STRUCT.unpack_from(raw, pos)
        path = read_str(s_off, s_len)
        if pay_off + pay_sz > len(raw):
            raise ValueError(f"DirectStorage payload out of bounds for {path} @ 0x{foff:X}")
        payload = raw[pay_off : pay_off + pay_sz]
        entry = BundleDStorageEntry(
            archive_path=path,
            file_offset=foff,
            orig_uncomp_size=orig_sz,
            payload=payload,
            flags=eflags,
            required_tier=req_tier,
            required_caps=req_caps,
        )
        if entry.key_hash != khash:
            raise ValueError(f"Key hash mismatch for {path} @ 0x{foff:X}")
        if entry.payload_crc32 != crc:
            raise ValueError(f"CRC32 mismatch for {path} @ 0x{foff:X}")
        ds_entries.append(entry)

    ov_entries: List[BundleOverlayEntry] = []
    for i in range(ov_count):
        pos = ov_off + i * FB_OVERLAY_ENTRY_SIZE
        phash, pay_off, pay_sz, s_off, s_len, crc, eflags = _OVERLAY_ENTRY_STRUCT.unpack_from(raw, pos)
        path = read_str(s_off, s_len)
        if pay_off + pay_sz > len(raw):
            raise ValueError(f"Overlay payload out of bounds for {path}")
        payload = raw[pay_off : pay_off + pay_sz]
        entry = BundleOverlayEntry(rel_path=path, payload=payload, flags=eflags)
        if entry.path_hash != phash:
            raise ValueError(f"Overlay path hash mismatch for {path}")
        if entry.payload_crc32 != crc:
            raise ValueError(f"Overlay CRC32 mismatch for {path}")
        ov_entries.append(entry)

    cfg_entries: List[BundlePluginConfigEntry] = []
    for i in range(cfg_count):
        pos = cfg_off + i * FB_PLUGIN_CFG_ENTRY_SIZE
        mhash, id_off, id_len, js_off, js_len, _res = _PLUGIN_CFG_STRUCT.unpack_from(raw, pos)
        mod_id = read_str(id_off, id_len)
        cfg_json = read_str(js_off, js_len)
        entry = BundlePluginConfigEntry(mod_id=mod_id, config_json=cfg_json)
        if entry.mod_id_hash != mhash:
            raise ValueError(f"Plugin config hash mismatch for {mod_id}")
        cfg_entries.append(entry)

    return CompiledBundle(
        dstorage_entries=ds_entries,
        overlay_entries=ov_entries,
        plugin_configs=cfg_entries,
    )


@dataclass
class _AssetWorkState:
    archive_path: str
    archive_offset: int
    orig_bytes: bytes
    pages: List[ZlibPageInfo]
    current_bytes: bytes
    kind_flags: int = 0
    text_format: str = ""  # "xml" or "kv"
    required_caps: int = 0
    explicit_page_offsets: set[int] = field(default_factory=set)
    contributing_mods: List[str] = field(default_factory=list)
    contributing_patches: List[str] = field(default_factory=list)


def _check_mod_dependencies(manifests: List[ModManifest]) -> None:
    loaded_ids = {m.mod_id for m in manifests}
    for m in manifests:
        for req in m.requires:
            if req not in loaded_ids:
                raise ValueError(f"Mod '{m.mod_id}' requires missing mod '{req}'")
        for conf in m.conflicts:
            if conf in loaded_ids:
                raise ValueError(f"Mod '{m.mod_id}' conflicts with loaded mod '{conf}'")


def _apply_size_preserve(st: _AssetWorkState) -> int:
    """
    When `--size-preserve` is enabled, ensure every modified page slice and the full
    text asset satisfy `payload_size <= orig_uncomp_size` by reclaiming non-semantic
    whitespace/comments only where growth occurred.
    Returns total bytes reclaimed.
    """
    if not (st.kind_flags & FB_DS_FLAG_TEXT_PATCH) or st.text_format not in ("kv", "xml"):
        return 0

    reclaimed_total = 0
    num_pages = len(st.pages)
    if num_pages == 1:
        max_b = len(st.orig_bytes)
        if len(st.current_bytes) > max_b:
            if st.text_format == "kv":
                st.current_bytes, rec = compact_kv_bytes(st.current_bytes, max_b)
            else:
                st.current_bytes, rec = compact_xml_slice(st.current_bytes, max_b)
            reclaimed_total += rec
        return reclaimed_total

    # Multi-page text asset: compact any page slice whose byte length exceeds its original page length
    rebuilt_pages: List[bytes] = []
    raw_slices: List[bytes] = []
    for pg in st.pages:
        if pg.page_index < num_pages - 1:
            raw_slices.append(st.current_bytes[pg.uncompressed_offset : pg.uncompressed_offset + 65536])
        else:
            raw_slices.append(st.current_bytes[pg.uncompressed_offset :])

    for i, pg in enumerate(st.pages):
        sl = raw_slices[i]
        if len(sl) > pg.uncompressed_size:
            if st.text_format == "kv":
                sl, rec = compact_kv_bytes(sl, pg.uncompressed_size)
            else:
                prefix = b"".join(rebuilt_pages)
                suffix = b"".join(raw_slices[i + 1 :])
                sl, rec = compact_xml_slice(sl, pg.uncompressed_size, full_prefix=prefix, full_suffix=suffix)
            reclaimed_total += rec
        rebuilt_pages.append(sl)

    if reclaimed_total > 0:
        st.current_bytes = b"".join(rebuilt_pages)
    return reclaimed_total


def build_patch_bundle(
    mod_paths: Sequence[str],
    game_dir: str,
    out_dir: str,
    *,
    bundle_filename: str = "fireball_bundle.fbin",
    extra_overlays: Optional[Dict[str, bytes]] = None,
    size_preserve: bool = False,
    tools_dir: Optional[str] = None,
) -> Dict[str, Any]:
    """
    Compile one or more mods (loose folders or `.gta6mod` ZIPs) in load order into
    a deterministic `fireball_bundle.fbin` plus `fireball_bundle_provenance.json`.
    """
    os.makedirs(out_dir, exist_ok=True)
    sources: List[VirtualModSource] = []
    manifests: List[ModManifest] = []
    try:
        for mp in mod_paths:
            src = open_mod_source(mp)
            sources.append(src)
            manifests.append(load_manifest(src))

        _check_mod_dependencies(manifests)

        assets: Dict[Tuple[str, int], _AssetWorkState] = {}
        overlays: Dict[str, Tuple[bytes, str]] = {}  # norm_rel_path -> (bytes, provenance_source)
        plugin_configs: Dict[str, Dict[str, Any]] = {}
        provenance_entries: List[Dict[str, Any]] = []
        mod_summaries: List[Dict[str, Any]] = []

        def get_or_load_asset(archive_rel: str, off: int) -> _AssetWorkState:
            norm_arch = normalize_rel_path(archive_rel)
            key = (norm_arch, off)
            if key not in assets:
                disk_path = os.path.join(game_dir, archive_rel)
                raw, pages, _ = read_paged_zlib_pages_at(disk_path, off)
                assets[key] = _AssetWorkState(
                    archive_path=norm_arch,
                    archive_offset=off,
                    orig_bytes=raw,
                    pages=pages,
                    current_bytes=raw,
                )
            return assets[key]

        for src, manifest in zip(sources, manifests):
            mod_summaries.append(
                {
                    "mod_id": manifest.mod_id,
                    "version": manifest.version,
                    "source_kind": src.kind,
                    "source_path": src.source_path,
                }
            )

            # 1. XML patches (merged in load order)
            for p in manifest.xml_patches:
                if not p.get("enabled", True):
                    continue
                off = parse_int_hex(p["archive_offset"])
                st = get_or_load_asset(str(p["archive"]), off)
                ops = src.read_toml(str(p["patch"])).get("ops", [])
                res = merge_xml_bytes(
                    st.current_bytes,
                    ops,
                    mod_id=manifest.mod_id,
                    patch_id=str(p["id"]),
                    target=str(p["target"]),
                )
                st.current_bytes = res.patched_bytes
                st.kind_flags |= FB_DS_FLAG_TEXT_PATCH
                st.text_format = "xml"
                st.required_caps |= caps_from_names(list(p.get("required_caps", [])))
                if "page_offset" in p:
                    st.explicit_page_offsets.add(parse_int_hex(p["page_offset"]))
                if manifest.mod_id not in st.contributing_mods:
                    st.contributing_mods.append(manifest.mod_id)
                st.contributing_patches.append(str(p["id"]))
                provenance_entries.extend(pr.to_dict() for pr in res.provenance)

            # 2. KV patches (merged in load order)
            for p in manifest.kv_patches:
                if not p.get("enabled", True):
                    continue
                off = parse_int_hex(p["archive_offset"])
                st = get_or_load_asset(str(p["archive"]), off)
                ops = src.read_toml(str(p["patch"])).get("ops", [])
                res = merge_kv_bytes(
                    st.current_bytes,
                    ops,
                    mod_id=manifest.mod_id,
                    patch_id=str(p["id"]),
                    target=str(p["target"]),
                )
                st.current_bytes = res.patched_bytes
                st.kind_flags |= FB_DS_FLAG_TEXT_PATCH
                st.text_format = "kv"
                st.required_caps |= caps_from_names(list(p.get("required_caps", [])))
                if "page_offset" in p:
                    st.explicit_page_offsets.add(parse_int_hex(p["page_offset"]))
                if manifest.mod_id not in st.contributing_mods:
                    st.contributing_mods.append(manifest.mod_id)
                st.contributing_patches.append(str(p["id"]))
                provenance_entries.extend(pr.to_dict() for pr in res.provenance)

            # 3. Binary PSO patches (merged in load order)
            for p in manifest.pso_patches:
                if not p.get("enabled", True):
                    continue
                off = parse_int_hex(p["archive_offset"])
                st = get_or_load_asset(str(p["archive"]), off)
                ops = src.read_toml(str(p["patch"])).get("ops", [])
                root_h = parse_int_hex(p["root_struct_hash"]) if "root_struct_hash" in p else None
                res = patch_pso_bytes(
                    st.current_bytes,
                    ops,
                    patch_id=str(p["id"]),
                    target=str(p["target"]),
                    archive_offset=off,
                    expected_root_hash=root_h,
                    tools_dir=tools_dir,
                )
                st.current_bytes = res.patched_bytes
                st.kind_flags |= FB_DS_FLAG_PSO_PATCH
                st.required_caps |= caps_from_names(list(p.get("required_caps", [])))
                if "page_offset" in p:
                    st.explicit_page_offsets.add(parse_int_hex(p["page_offset"]))
                if manifest.mod_id not in st.contributing_mods:
                    st.contributing_mods.append(manifest.mod_id)
                st.contributing_patches.append(str(p["id"]))
                for d in res.diffs:
                    provenance_entries.append(
                        {
                            "mod_id": manifest.mod_id,
                            "patch_id": str(p["id"]),
                            "target": str(p["target"]),
                            "location": f"{d.struct_hash}[{d.instance_index}]{d.member_offset} ({d.member_name})",
                            "op": f"pso.{d.field_type}",
                            "old_value": d.old_value,
                            "new_value": d.new_value,
                            "reason": d.reason,
                        }
                    )

            # 4. Loose-file overlays from `overlays/` directory or `[[overlays]]` in mod.toml
            for rel_file in src.list_files():
                if rel_file.startswith("overlays/"):
                    target_rel = normalize_rel_path(rel_file[len("overlays/") :])
                    overlays[target_rel] = (src.read_bytes(rel_file), f"{manifest.mod_id}:{rel_file}")
            for ov_spec in manifest.raw.get("overlays", []):
                target_rel = normalize_rel_path(str(ov_spec["target"]))
                src_rel = str(ov_spec["source"])
                overlays[target_rel] = (src.read_bytes(src_rel), f"{manifest.mod_id}:{src_rel}")

            # 5. Plugin config JSONs
            for plg in manifest.plugins:
                cfg = plg.get("config")
                if isinstance(cfg, dict) and cfg:
                    merged_cfg = plugin_configs.setdefault(manifest.mod_id, {})
                    merged_cfg.update(cfg)

        if extra_overlays:
            for target_rel, raw_data in extra_overlays.items():
                norm_rel = normalize_rel_path(target_rel)
                overlays[norm_rel] = (raw_data, "cli:--overlay")

        # Build DirectStorage entries: full-asset + changed/explicit per-64KiB-page slices
        ds_entries: List[BundleDStorageEntry] = []
        ds_index_report: List[Dict[str, Any]] = []
        total_reclaimed_bytes = 0

        for (_norm_arch, _off), st in sorted(assets.items()):
            reclaimed = _apply_size_preserve(st) if size_preserve else 0
            total_reclaimed_bytes += reclaimed
            orig_total = len(st.orig_bytes)
            patched_total = len(st.current_bytes)
            single_page = len(st.pages) == 1

            full_flags = FB_DS_FLAG_FULL_ASSET | st.kind_flags
            if single_page:
                full_flags |= FB_DS_FLAG_PAGE_SLICE

            full_entry = BundleDStorageEntry(
                archive_path=st.archive_path,
                file_offset=st.archive_offset,
                orig_uncomp_size=orig_total,
                payload=st.current_bytes,
                flags=full_flags,
                required_tier=1 if st.required_caps else 0,
                required_caps=st.required_caps,
            )
            ds_entries.append(full_entry)
            ds_index_report.append(
                {
                    "kind": "full_asset" if not single_page else "full_asset_and_single_page",
                    "archive_path": full_entry.archive_path,
                    "file_offset": f"0x{full_entry.file_offset:08X}",
                    "orig_uncomp_size": full_entry.orig_uncomp_size,
                    "payload_size": len(full_entry.payload),
                    "delta": len(full_entry.payload) - full_entry.orig_uncomp_size,
                    "fits_fixed_dst": len(full_entry.payload) <= full_entry.orig_uncomp_size,
                    "size_preserve_reclaimed": reclaimed,
                    "payload_crc32": f"0x{full_entry.payload_crc32:08X}",
                    "mods": st.contributing_mods,
                    "patches": st.contributing_patches,
                }
            )

            # Multi-page asset: emit per-64KiB-page slice entries for every modified or explicit page
            if not single_page:
                num_pages = len(st.pages)
                for pg in st.pages:
                    if pg.page_index < num_pages - 1:
                        slice_bytes = st.current_bytes[pg.uncompressed_offset : pg.uncompressed_offset + 65536]
                    else:
                        slice_bytes = st.current_bytes[pg.uncompressed_offset :]

                    is_changed = slice_bytes != pg.data
                    is_explicit = pg.file_offset in st.explicit_page_offsets
                    if not (is_changed or is_explicit):
                        continue

                    # Guard against key collision with full-asset entry (only possible if orig_uncomp_size == orig_total)
                    if pg.file_offset == st.archive_offset and pg.uncompressed_size == orig_total:
                        continue

                    pg_entry = BundleDStorageEntry(
                        archive_path=st.archive_path,
                        file_offset=pg.file_offset,
                        orig_uncomp_size=pg.uncompressed_size,
                        payload=slice_bytes,
                        flags=FB_DS_FLAG_PAGE_SLICE | st.kind_flags,
                        required_tier=1 if st.required_caps else 0,
                        required_caps=st.required_caps,
                    )
                    ds_entries.append(pg_entry)
                    ds_index_report.append(
                        {
                            "kind": f"page_slice[{pg.page_index}]",
                            "archive_path": pg_entry.archive_path,
                            "file_offset": f"0x{pg_entry.file_offset:08X}",
                            "orig_uncomp_size": pg_entry.orig_uncomp_size,
                            "payload_size": len(pg_entry.payload),
                            "delta": len(pg_entry.payload) - pg_entry.orig_uncomp_size,
                            "fits_fixed_dst": len(pg_entry.payload) <= pg_entry.orig_uncomp_size,
                            "payload_crc32": f"0x{pg_entry.payload_crc32:08X}",
                            "mods": st.contributing_mods,
                            "patches": st.contributing_patches,
                        }
                    )

        ov_entries: List[BundleOverlayEntry] = []
        ov_index_report: List[Dict[str, Any]] = []
        for rel_path, (raw_data, source_tag) in sorted(overlays.items()):
            ov_e = BundleOverlayEntry(rel_path=rel_path, payload=raw_data)
            ov_entries.append(ov_e)
            ov_index_report.append(
                {
                    "rel_path": ov_e.rel_path,
                    "size_bytes": len(ov_e.payload),
                    "crc32": f"0x{ov_e.payload_crc32:08X}",
                    "source": source_tag,
                }
            )

        cfg_entries: List[BundlePluginConfigEntry] = [
            BundlePluginConfigEntry(
                mod_id=mid,
                config_json=json.dumps(cfg_dict, sort_keys=True, separators=(",", ":")),
            )
            for mid, cfg_dict in sorted(plugin_configs.items())
        ]

        compiled = CompiledBundle(
            dstorage_entries=ds_entries,
            overlay_entries=ov_entries,
            plugin_configs=cfg_entries,
        )
        fbin_bytes = serialize_bundle(compiled)
        # Round-trip self-check before writing to disk
        _parsed_check = parse_bundle(fbin_bytes)
        assert len(_parsed_check.dstorage_entries) == len(ds_entries)
        assert len(_parsed_check.overlay_entries) == len(ov_entries)
        assert len(_parsed_check.plugin_configs) == len(cfg_entries)

        bundle_path = os.path.join(out_dir, bundle_filename)
        with open(bundle_path, "wb") as wf:
            wf.write(fbin_bytes)

        prov_report = {
            "bundle_file": os.path.abspath(bundle_path),
            "bundle_version": FB_BUNDLE_VERSION,
            "bundle_size_bytes": len(fbin_bytes),
            "size_preserve": size_preserve,
            "size_preserve_reclaimed_bytes": total_reclaimed_bytes,
            "all_fit_fixed_dst": all(len(e.payload) <= e.orig_uncomp_size for e in ds_entries),
            "mods": mod_summaries,
            "dstorage_entry_count": len(ds_entries),
            "overlay_entry_count": len(ov_entries),
            "plugin_config_count": len(cfg_entries),
            "dstorage_entries": ds_index_report,
            "overlay_entries": ov_index_report,
            "plugin_configs": [c.mod_id for c in cfg_entries],
            "total_field_changes": len(provenance_entries),
            "provenance": provenance_entries,
        }
        prov_path = os.path.join(out_dir, "fireball_bundle_provenance.json")
        with open(prov_path, "w", encoding="utf-8") as wf:
            json.dump(prov_report, wf, indent=2)

        prov_report["provenance_file"] = os.path.abspath(prov_path)
        return prov_report
    finally:
        for s in sources:
            s.close()


def _format_ds_flags(flags: int) -> List[str]:
    names: List[str] = []
    if flags & FB_DS_FLAG_FULL_ASSET:
        names.append("FULL")
    if flags & FB_DS_FLAG_PAGE_SLICE:
        names.append("PAGE")
    if flags & FB_DS_FLAG_TEXT_PATCH:
        names.append("TEXT")
    if flags & FB_DS_FLAG_PSO_PATCH:
        names.append("PSO")
    return names


def inspect_bundle_file(bundle_path: str) -> Dict[str, Any]:
    """Parse and inspect a `fireball_bundle.fbin` file, returning structured metadata."""
    with open(bundle_path, "rb") as rf:
        raw = rf.read()
    parsed = parse_bundle(raw)

    ds_list: List[Dict[str, Any]] = []
    for e in parsed.dstorage_entries:
        delta = len(e.payload) - e.orig_uncomp_size
        ds_list.append(
            {
                "archive_path": e.archive_path,
                "file_offset": f"0x{e.file_offset:08X}",
                "orig_uncomp_size": e.orig_uncomp_size,
                "payload_size": len(e.payload),
                "delta": delta,
                "fits_fixed_dst": len(e.payload) <= e.orig_uncomp_size,
                "flags": _format_ds_flags(e.flags),
                "payload_crc32": f"0x{e.payload_crc32:08X}",
            }
        )

    ov_list = [
        {
            "rel_path": e.rel_path,
            "size_bytes": len(e.payload),
            "crc32": f"0x{e.payload_crc32:08X}",
        }
        for e in parsed.overlay_entries
    ]
    cfg_list = [
        {
            "mod_id": e.mod_id,
            "json_len": len(e.config_json.encode("utf-8")),
            "config_json": e.config_json,
        }
        for e in parsed.plugin_configs
    ]
    return {
        "bundle_file": os.path.abspath(bundle_path),
        "bundle_version": FB_BUNDLE_VERSION,
        "bundle_size_bytes": len(raw),
        "dstorage_entry_count": len(ds_list),
        "overlay_entry_count": len(ov_list),
        "plugin_config_count": len(cfg_list),
        "all_fit_fixed_dst": all(d["fits_fixed_dst"] for d in ds_list),
        "dstorage_entries": ds_list,
        "overlay_entries": ov_list,
        "plugin_configs": cfg_list,
    }
