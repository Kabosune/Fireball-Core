"""Read-only streaming/seeking helpers for GTA VI RPF9 paged zlib streams (never modifies game files)."""

from __future__ import annotations

import os
import zlib
from typing import Tuple


def parse_int_hex(val: object) -> int:
    if isinstance(val, int):
        return val
    return int(str(val), 0)


def read_paged_zlib_at(
    archive_path: str,
    offset: int,
    max_read_bytes: int = 4 * 1024 * 1024,
) -> Tuple[bytes, int, int]:
    """
    Seek to `offset` inside a read-only RPF9 archive and reassemble a single or
    multi-page (64 KiB) unencrypted `78 DA` zlib asset without reading the full RPF.

    Returns (decompressed_bytes, page_count, end_file_offset).
    """
    if not os.path.isfile(archive_path):
        raise FileNotFoundError(f"Game archive not found: {archive_path}")

    file_size = os.path.getsize(archive_path)
    if offset < 0 or offset >= file_size:
        raise ValueError(f"Offset 0x{offset:X} out of bounds for {archive_path} ({file_size} B)")

    with open(archive_path, "rb") as f:
        f.seek(offset)
        window = f.read(min(max_read_bytes, file_size - offset))

    if window[:2] != b"\x78\xda":
        raise ValueError(
            f"Expected zlib 78 DA stream at {os.path.basename(archive_path)} @ 0x{offset:08X}, "
            f"got {window[:4].hex()}"
        )

    out = bytearray()
    pos = 0
    pages = 0
    n = len(window)
    while pos + 2 <= n and window[pos : pos + 2] == b"\x78\xda":
        d = zlib.decompressobj()
        chunk = d.decompress(window[pos:])
        if not d.eof:
            raise ValueError(
                f"Incomplete zlib stream at {os.path.basename(archive_path)} @ 0x{offset + pos:08X}"
            )
        consumed = (n - pos) - len(d.unused_data)
        out.extend(chunk)
        pages += 1
        pos += consumed
        # RPF9 paged assets split into 65,536-byte uncompressed pages aligned to 16-byte boundaries.
        if len(chunk) < 65536:
            break
        # Advance across 16-byte zero/pad boundary to next page's 78 DA
        aligned = (pos + 15) & ~15
        if aligned + 2 <= n and window[aligned : aligned + 2] == b"\x78\xda":
            pos = aligned
        elif pos + 2 <= n and window[pos : pos + 2] == b"\x78\xda":
            continue
        else:
            break

    return bytes(out), pages, offset + pos


def read_single_zlib_chunk_at(archive_path: str, offset: int, max_read_bytes: int = 512 * 1024) -> bytes:
    """Decompress exactly one zlib stream at `offset` (useful for verifying page offsets)."""
    with open(archive_path, "rb") as f:
        f.seek(offset)
        window = f.read(max_read_bytes)
    if window[:2] != b"\x78\xda":
        raise ValueError(f"Expected zlib 78 DA at 0x{offset:08X}")
    d = zlib.decompressobj()
    chunk = d.decompress(window)
    if not d.eof:
        raise ValueError(f"Truncated zlib chunk at 0x{offset:08X}")
    return chunk
