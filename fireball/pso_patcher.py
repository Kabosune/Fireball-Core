"""
RAGE 9 Binary PSO Field Patcher & 7-Section Serializer for Fireball-Core.

Reuses `pso_decode.py` from the companion research repo
(`Project-Americas-modding-and-documentation`, configured via `--tools-dir` or
env `GTA6_MODDING_TOOLS`) without copy-pasting the parser. Preserves untouched
LSCH/LENM/LSNS/LGID/LRSC sections byte-for-byte, validates 0-patch round-trip
identity, and re-decodes every patched PSO through `pso_decode`.
"""

from __future__ import annotations

import binascii
import hashlib
import importlib
import math
import os
import struct
import sys
import zlib
from dataclasses import dataclass, field
from types import ModuleType
from typing import Any, Dict, List, Optional, Tuple

from .archive_reader import parse_int_hex

_PSO_DECODE_MOD: Optional[ModuleType] = None


def resolve_research_tools_dir(explicit_dir: Optional[str] = None) -> Optional[str]:
    """Locate the companion research repo's `tools/` directory containing `pso_decode.py`."""
    candidates: List[str] = []
    if explicit_dir:
        candidates.append(explicit_dir)
    env_dir = os.environ.get("GTA6_MODDING_TOOLS")
    if env_dir:
        candidates.append(env_dir)
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidates.append(os.path.join(os.path.dirname(repo_root), "gta6-modding", "tools"))
    candidates.append(os.path.join(os.path.dirname(repo_root), "Project-Americas-modding-and-documentation", "tools"))
    candidates.append(r"C:\dev\gta6-modding\tools")

    for cand in candidates:
        if cand and os.path.isfile(os.path.join(cand, "pso_decode.py")):
            return os.path.abspath(cand)
    return None


def get_pso_decode(tools_dir: Optional[str] = None) -> ModuleType:
    """Lazily import `pso_decode.py` from the configured research tools directory."""
    global _PSO_DECODE_MOD
    if _PSO_DECODE_MOD is not None and tools_dir is None:
        return _PSO_DECODE_MOD

    resolved = resolve_research_tools_dir(tools_dir)
    if resolved is None:
        raise ImportError(
            "Could not locate `pso_decode.py`. Pass `--tools-dir <path>` or set "
            "`GTA6_MODDING_TOOLS` to the `tools/` folder of "
            "Project-Americas-modding-and-documentation."
        )
    if resolved not in sys.path:
        sys.path.insert(0, resolved)
    mod = importlib.import_module("pso_decode")
    _PSO_DECODE_MOD = mod
    return mod


def _make_decoder(pso: Any, pso_decode: ModuleType) -> Any:
    """
    Thin wrapper around `pso_decode.PsoDecoder` fixing empirically verified
    RAGE 9 `type_id = 0x33` (16-byte SIMD vector/quaternion `<4f`, observed
    with exact 16 B stride across 19/19 members in camera PSOs 0x006C6000,
    0x0063F000, and 0x006A2000; `pso_decode.py` provisionally mapped 0x33 to 48 B).
    """

    class FireballPsoDecoder(pso_decode.PsoDecoder):
        def member_size(self, sd: Any, mem: Any) -> int:
            if mem.type_id == 0x33:
                return 16
            return super().member_size(sd, mem)

        def decode_member(self, sd: Any, mem: Any, buf: bytes, off: int, depth: int) -> Any:
            if mem.type_id == 0x33:
                return [round(v, 6) for v in struct.unpack_from("<4f", buf, off)]
            return super().decode_member(sd, mem, buf, off, depth)

    return FireballPsoDecoder(pso)


@dataclass
class PsoFieldDiff:
    struct_hash: str
    instance_index: int
    block_id: int
    member_offset: str
    member_hash: str
    member_name: str
    field_type: str
    old_value: Any
    new_value: Any
    file_byte_offset: int
    reason: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return {
            "struct_hash": self.struct_hash,
            "instance_index": self.instance_index,
            "block_id": self.block_id,
            "member_offset": self.member_offset,
            "member_hash": self.member_hash,
            "member_name": self.member_name,
            "field_type": self.field_type,
            "old_value": self.old_value,
            "new_value": self.new_value,
            "file_byte_offset": f"0x{self.file_byte_offset:08X}",
            "reason": self.reason,
        }


@dataclass
class PsoPatchResult:
    patch_id: str
    target: str
    archive_offset: int
    original_size: int
    patched_size: int
    roundtrip_identity_ok: bool
    redecode_ok: bool
    decoded_structs: int
    decoded_members: int
    unknown_types: int
    chks_original: str
    chks_status: str
    patched_bytes: bytes
    diffs: List[PsoFieldDiff] = field(default_factory=list)


def _split_sections(buf: bytes) -> List[Tuple[str, bytes]]:
    pos = 0
    out: List[Tuple[str, bytes]] = []
    while pos + 8 <= len(buf):
        magic = buf[pos : pos + 4].decode("ascii")
        sz = struct.unpack_from(">I", buf, pos + 4)[0]
        out.append((magic, buf[pos + 8 : pos + sz]))
        pos += sz
    if pos != len(buf):
        raise ValueError(f"Trailing bytes in PSO: {pos} != {len(buf)}")
    return out


def _rebuild_psin_payload(
    orig_psin: bytes,
    num_blocks: int,
    modified_blocks: Dict[int, bytes],
    pso_decode: ModuleType,
) -> Tuple[bytes, Dict[int, int]]:
    """
    Rebuild the PSIN payload while preserving exact 16-byte block directory
    headers (including 0xAA runtime_ptr slots), the 8-byte 'pppppppp' pad,
    and exact inter-block padding bytes.

    Returns (new_psin_payload, block_id_to_payload_offset).
    """
    hdr_end = num_blocks * 16
    pad8 = orig_psin[hdr_end : hdr_end + 8]
    out = bytearray()
    out.extend(orig_psin[:hdr_end])
    out.extend(pad8)

    block_offsets: Dict[int, int] = {}
    in_off = hdr_end + 8
    for i in range(num_blocks):
        bid = i + 1
        bh, bsz, bptr = struct.unpack_from("<IIQ", orig_psin, i * 16)
        orig_aligned = pso_decode.align16(bsz)
        orig_raw = orig_psin[in_off : in_off + bsz]
        orig_pad = orig_psin[in_off + bsz : in_off + orig_aligned]

        new_raw = modified_blocks.get(bid, orig_raw)
        if len(new_raw) != bsz:
            struct.pack_into("<IIQ", out, i * 16, bh, len(new_raw), bptr)
            pad_len = pso_decode.align16(len(new_raw)) - len(new_raw)
            pad_bytes = b"\x00" * pad_len
        else:
            pad_bytes = orig_pad

        block_offsets[bid] = len(out)
        out.extend(new_raw)
        out.extend(pad_bytes)
        in_off += orig_aligned

    return bytes(out), block_offsets


def serialize_pso_with_blocks(
    orig_buf: bytes,
    modified_blocks: Dict[int, bytes],
    *,
    new_chks_checksum: Optional[int] = None,
    tools_dir: Optional[str] = None,
) -> Tuple[bytes, Dict[int, int]]:
    """
    Re-serialize a 7-section RAGE 9 PSO buffer with updated PSIN block payloads.
    When `modified_blocks` is empty and `new_chks_checksum` is None, output is
    guaranteed byte-for-byte identical to `orig_buf`.
    """
    pso_decode = get_pso_decode(tools_dir)
    pso = pso_decode.parse_pso_bytes(orig_buf)
    sections = _split_sections(orig_buf)
    rebuilt_sections: List[Tuple[str, bytes]] = []
    block_file_offsets: Dict[int, int] = {}

    for magic, payload in sections:
        if magic == "PSIN":
            new_psin, rel_offsets = _rebuild_psin_payload(payload, pso.num_blocks, modified_blocks, pso_decode)
            for bid, roff in rel_offsets.items():
                block_file_offsets[bid] = 8 + roff  # 8-byte PSIN section header at file offset 0
            rebuilt_sections.append((magic, new_psin))
        else:
            rebuilt_sections.append((magic, payload))

    total_len = sum(8 + len(p) for _, p in rebuilt_sections)
    out = bytearray()
    for magic, payload in rebuilt_sections:
        if magic == "CHKS":
            orig_sz, orig_ck, orig_tag = struct.unpack(">II4s", payload)
            ck_val = orig_ck if new_chks_checksum is None else (new_chks_checksum & 0xFFFFFFFF)
            payload = struct.pack(">II4s", total_len, ck_val, orig_tag)
        out.extend(magic.encode("ascii"))
        out.extend(struct.pack(">I", 8 + len(payload)))
        out.extend(payload)

    return bytes(out), block_file_offsets


def _read_typed_field(buf: bytes, off: int, field_type: str) -> Any:
    if field_type == "BOOL":
        return bool(buf[off])
    if field_type in ("S32", "ENUM32"):
        return struct.unpack_from("<i", buf, off)[0]
    if field_type in ("U32", "ATFINALHASHSTRING"):
        return struct.unpack_from("<I", buf, off)[0]
    if field_type == "FLOAT":
        return struct.unpack_from("<f", buf, off)[0]
    raise ValueError(f"Unsupported PSO patch field_type: {field_type}")


def _write_typed_field(buf: bytearray, off: int, field_type: str, value: Any) -> None:
    if field_type == "BOOL":
        buf[off] = 1 if bool(value) else 0
    elif field_type in ("S32", "ENUM32"):
        struct.pack_into("<i", buf, off, int(value))
    elif field_type in ("U32", "ATFINALHASHSTRING"):
        struct.pack_into("<I", buf, off, parse_int_hex(value))
    elif field_type == "FLOAT":
        struct.pack_into("<f", buf, off, float(value))
    else:
        raise ValueError(f"Unsupported PSO patch field_type: {field_type}")


def _values_equal(field_type: str, a: Any, b: Any) -> bool:
    if field_type == "FLOAT":
        return math.isclose(float(a), float(b), rel_tol=1e-5, abs_tol=1e-5)
    return a == b


def patch_pso_bytes(
    orig_buf: bytes,
    ops: List[Dict[str, Any]],
    *,
    patch_id: str,
    target: str,
    archive_offset: int,
    expected_root_hash: Optional[int] = None,
    tools_dir: Optional[str] = None,
) -> PsoPatchResult:
    """
    Parse a real RAGE 9 PSO, verify 0-patch round-trip byte identity, apply all
    field ops, serialize back to binary, and re-decode via `pso_decode`.
    """
    pso_decode = get_pso_decode(tools_dir)

    # 1. Verify 0-patch serializer round-trip byte-for-byte identity
    rt_bytes, orig_block_offsets = serialize_pso_with_blocks(orig_buf, {}, tools_dir=tools_dir)
    roundtrip_ok = rt_bytes == orig_buf
    if not roundtrip_ok:
        raise AssertionError(f"PSO serializer 0-patch round-trip mismatch on {target}")

    pso = pso_decode.parse_pso_bytes(orig_buf)
    rbid, _ = pso_decode.decode_struct_id(pso.root_id)
    actual_root = pso.blocks[rbid].struct_hash
    if expected_root_hash is not None and actual_root != expected_root_hash:
        raise AssertionError(
            f"Root struct hash mismatch for {target}: got 0x{actual_root:08X}, expected 0x{expected_root_hash:08X}"
        )

    mutable_blocks: Dict[int, bytearray] = {}
    diffs: List[PsoFieldDiff] = []

    for op in ops:
        sh = parse_int_hex(op["struct_hash"])
        inst_idx = int(op.get("instance_index", 0))
        moff = parse_int_hex(op["member_offset"])
        mhash = parse_int_hex(op["member_hash"])
        ftype = str(op["field_type"]).upper()
        mname = str(op.get("member_name") or f"0x{mhash:08X}")
        reason = str(op.get("reason", ""))

        sd = pso.structs.get(sh)
        if sd is None:
            raise KeyError(f"Struct 0x{sh:08X} not found in LSCH of {target}")

        mem = next((m for m in sd.members if m.offset == moff and m.name_hash == mhash), None)
        if mem is None:
            raise KeyError(
                f"Member hash=0x{mhash:08X} @ +0x{moff:03X} not found in struct 0x{sh:08X} of {target}"
            )
        if mem.type_name != ftype:
            raise TypeError(
                f"Member type mismatch in 0x{sh:08X}+0x{moff:03X}: schema={mem.type_name}, patch={ftype}"
            )
        if "enum_hash" in op:
            eh = parse_int_hex(op["enum_hash"])
            if mem.referent_hash != eh:
                raise AssertionError(
                    f"Enum referent mismatch: schema=0x{mem.referent_hash:08X}, patch=0x{eh:08X}"
                )
            ed = pso.enums.get(eh)
            if ed is None or int(op["value"]) not in ed.value_to_hash:
                raise ValueError(f"Value {op['value']} is not a valid enumerator of enum 0x{eh:08X}")

        matching_blocks = [b for b in pso.blocks.values() if b.struct_hash == sh]
        if not matching_blocks:
            raise KeyError(f"No PSIN block for struct 0x{sh:08X} in {target}")

        rem_idx = inst_idx
        target_blk = None
        local_idx = 0
        for blk in matching_blocks:
            cnt = len(blk.data) // sd.size
            if rem_idx < cnt:
                target_blk = blk
                local_idx = rem_idx
                break
            rem_idx -= cnt
        if target_blk is None:
            raise IndexError(f"Instance index {inst_idx} out of range for struct 0x{sh:08X}")

        blk_buf = mutable_blocks.setdefault(target_blk.block_id, bytearray(target_blk.data))
        field_off = local_idx * sd.size + moff
        old_val = _read_typed_field(blk_buf, field_off, ftype)
        if "expected_old" in op:
            exp_old = op["expected_old"]
            if not _values_equal(ftype, old_val, exp_old):
                raise AssertionError(
                    f"Precondition mismatch on 0x{sh:08X}+0x{moff:03X}: got {old_val!r}, expected {exp_old!r}"
                )

        new_val = op["value"]
        _write_typed_field(blk_buf, field_off, ftype, new_val)
        written_val = _read_typed_field(blk_buf, field_off, ftype)

        diffs.append(
            PsoFieldDiff(
                struct_hash=f"0x{sh:08X}",
                instance_index=inst_idx,
                block_id=target_blk.block_id,
                member_offset=f"+0x{moff:03X}",
                member_hash=f"0x{mhash:08X}",
                member_name=mname,
                field_type=ftype,
                old_value=round(old_val, 5) if ftype == "FLOAT" else old_val,
                new_value=round(written_val, 5) if ftype == "FLOAT" else written_val,
                file_byte_offset=orig_block_offsets[target_blk.block_id] + field_off,
                reason=reason,
            )
        )

    modified_frozen = {bid: bytes(ba) for bid, ba in mutable_blocks.items()}
    patched_bytes, _ = serialize_pso_with_blocks(orig_buf, modified_frozen, tools_dir=tools_dir)

    # 2. Re-parse and recursively decode the patched binary through pso_decode
    re_pso = pso_decode.parse_pso_bytes(patched_bytes)
    decoder = _make_decoder(re_pso, pso_decode)
    _decoded_tree = decoder.decode_root()
    if re_pso.warnings:
        raise AssertionError(f"Re-parsed PSO produced warnings: {re_pso.warnings}")
    if sum(decoder.unknown_types.values()) != 0:
        raise AssertionError(f"Re-decoded PSO had unknown types: {dict(decoder.unknown_types)}")

    # 3. Verify every patched field reads back cleanly from the re-parsed PsoFile
    for op, diff in zip(ops, diffs):
        sh = parse_int_hex(op["struct_hash"])
        moff = parse_int_hex(op["member_offset"])
        ftype = str(op["field_type"]).upper()
        sd = re_pso.structs[sh]
        blk = re_pso.blocks[diff.block_id]
        matching = [b for b in re_pso.blocks.values() if b.struct_hash == sh]
        rem = diff.instance_index
        local_idx = 0
        for b in matching:
            cnt = len(b.data) // sd.size
            if b.block_id == diff.block_id:
                local_idx = rem
                break
            rem -= cnt
        val_after = _read_typed_field(blk.data, local_idx * sd.size + moff, ftype)
        if not _values_equal(ftype, val_after, op["value"]):
            raise AssertionError(
                f"Post-patch verification failed on 0x{sh:08X}+0x{moff:03X}: got {val_after!r}, expected {op['value']!r}"
            )

    return PsoPatchResult(
        patch_id=patch_id,
        target=target,
        archive_offset=archive_offset,
        original_size=len(orig_buf),
        patched_size=len(patched_bytes),
        roundtrip_identity_ok=roundtrip_ok,
        redecode_ok=True,
        decoded_structs=decoder.decoded_structs,
        decoded_members=decoder.decoded_members,
        unknown_types=sum(decoder.unknown_types.values()),
        chks_original=f"0x{pso.chks_checksum:08X}",
        chks_status="NEEDS_TIER1_CRC_BYPASS (CHKS +0x0C algorithm unconfirmed; see chks-probe)",
        patched_bytes=patched_bytes,
        diffs=diffs,
    )


def probe_chks_algorithm(
    buf: bytes,
    same_len_pair: Optional[Tuple[bytes, bytes]] = None,
    *,
    tools_dir: Optional[str] = None,
) -> Dict[str, Any]:
    """
    Reproducible empirical check of CHKS +0x0C (32-bit checksum in RAGE 9 PSO v122 'zppp').
    Tests standard CRC32/Adler32/JOAAT/FNV1a/SHA256/MD5 candidates plus GF(2)
    linearity on a same-length PSO pair when provided.
    """
    pso_decode = get_pso_decode(tools_dir)
    pso = pso_decode.parse_pso_bytes(buf)
    target_be = pso.chks_checksum
    target_le = struct.unpack(">I", struct.pack("<I", target_be))[0]
    tested_algos: List[str] = []
    matches: List[str] = []

    def joaat_u8(data: bytes, seed: int = 0) -> int:
        h = seed & 0xFFFFFFFF
        for b in data:
            h = (h + b) & 0xFFFFFFFF
            h = (h + (h << 10)) & 0xFFFFFFFF
            h ^= h >> 6
        h = (h + (h << 3)) & 0xFFFFFFFF
        h ^= h >> 11
        h = (h + (h << 15)) & 0xFFFFFFFF
        return h

    def joaat_u32(data: bytes, seed: int = 0) -> int:
        h = seed & 0xFFFFFFFF
        for i in range(len(data) // 4):
            h = (h + struct.unpack_from("<I", data, i * 4)[0]) & 0xFFFFFFFF
            h = (h + (h << 10)) & 0xFFFFFFFF
            h ^= h >> 6
        h = (h + (h << 3)) & 0xFFFFFFFF
        h ^= h >> 11
        h = (h + (h << 15)) & 0xFFFFFFFF
        return h

    def fnv1a_32(data: bytes) -> int:
        h = 0x811C9DC5
        for b in data:
            h ^= b
            h = (h * 0x01000193) & 0xFFFFFFFF
        return h

    slices = {
        "before_CHKS": buf[:-20],
        "before_CHKS_plus_size": buf[:-8],
        "whole_zeroed_chks": buf[:-8] + b"\x00\x00\x00\x00" + buf[-4:],
        "PSIN_only": buf[: pso.sections[0][2]],
    }
    algos = [
        ("crc32_ieee", lambda b: binascii.crc32(b) & 0xFFFFFFFF),
        ("crc32_ieee_inv", lambda b: (~binascii.crc32(b)) & 0xFFFFFFFF),
        ("crc32_seed_0x3FAC7125", lambda b: binascii.crc32(b, 0x3FAC7125) & 0xFFFFFFFF),
        ("adler32", lambda b: zlib.adler32(b) & 0xFFFFFFFF),
        ("joaat_u8_0", lambda b: joaat_u8(b, 0)),
        ("joaat_u8_0x3FAC7125", lambda b: joaat_u8(b, 0x3FAC7125)),
        ("joaat_u32_0", lambda b: joaat_u32(b, 0)),
        ("joaat_u32_0x3FAC7125", lambda b: joaat_u32(b, 0x3FAC7125)),
        ("fnv1a_32", fnv1a_32),
        ("sha256_lo32", lambda b: struct.unpack_from("<I", hashlib.sha256(b).digest(), 0)[0]),
        ("md5_lo32", lambda b: struct.unpack_from("<I", hashlib.md5(b).digest(), 0)[0]),
    ]

    for sname, sb in slices.items():
        for aname, fn in algos:
            tested_algos.append(f"{aname}({sname})")
            v = fn(sb)
            if v == target_be or v == target_le:
                matches.append(f"{aname}({sname}) -> 0x{v:08X}")

    gf2_crc_ruled_out = False
    if same_len_pair is not None:
        b1, b2 = same_len_pair
        p1, p2 = pso_decode.parse_pso_bytes(b1), pso_decode.parse_pso_bytes(b2)
        xor_be = p1.chks_checksum ^ p2.chks_checksum
        xor_le = struct.unpack(">I", struct.pack("<I", xor_be))[0]
        diff = bytes(x ^ y for x, y in zip(b1[:-20], b2[:-20]))
        crc_zero = binascii.crc32(diff, 0xFFFFFFFF) ^ 0xFFFFFFFF
        gf2_crc_ruled_out = crc_zero != xor_be and crc_zero != xor_le

    return {
        "pso_size": len(buf),
        "chks_be": f"0x{target_be:08X}",
        "chks_tag": pso.chks_tag,
        "tested_count": len(tested_algos),
        "matches": matches,
        "gf2_ieee_crc32_ruled_out": gf2_crc_ruled_out,
        "status": "SOLVED" if matches else "UNSOLVED_REQUIRES_TIER1_CRC_BYPASS",
        "remaining_hypotheses": [
            "Keyed/salted RAGE 9 v122 ('zppp') hash (e.g. non-public seed or SipHash/keyed lookup3/Murmur3).",
            "Section-filtered or normalized hash that skips/zeroes disk placeholder pointers (0xAA/0xBB/0xEE) before hashing.",
            "External asset path/name hash mixed into CHKS at pack time.",
        ],
    }
