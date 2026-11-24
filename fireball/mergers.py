"""Additive, provenance-tracking XML and Key-Value (visualsettings.dat) mergers for Fireball-Core."""

from __future__ import annotations

import difflib
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from typing import Any, Dict, List, Tuple


@dataclass
class ProvenanceEntry:
    mod_id: str
    patch_id: str
    target: str
    location: str
    op: str
    old_value: str
    new_value: str
    reason: str = ""

    def to_dict(self) -> Dict[str, Any]:
        return {
            "mod_id": self.mod_id,
            "patch_id": self.patch_id,
            "target": self.target,
            "location": self.location,
            "op": self.op,
            "old_value": self.old_value,
            "new_value": self.new_value,
            "reason": self.reason,
        }


@dataclass
class TextMergeResult:
    target: str
    patch_id: str
    had_bom: bool
    original_size: int
    patched_size: int
    changes_applied: int
    xml_valid_before: bool | None
    xml_valid_after: bool | None
    patched_bytes: bytes
    unified_diff: str
    provenance: List[ProvenanceEntry] = field(default_factory=list)


def _strip_bom(raw: bytes) -> Tuple[bool, str]:
    had_bom = raw.startswith(b"\xef\xbb\xbf")
    text = (raw[3:] if had_bom else raw).decode("utf-8")
    return had_bom, text


def _restore_bom(had_bom: bool, text: str) -> bytes:
    body = text.encode("utf-8")
    return (b"\xef\xbb\xbf" + body) if had_bom else body


def merge_xml_bytes(
    raw_xml: bytes,
    ops: List[Dict[str, Any]],
    *,
    mod_id: str,
    patch_id: str,
    target: str,
) -> TextMergeResult:
    """
    Apply additive XML attribute patches while preserving formatting, comments,
    line endings, and BOM, validating well-formedness with ElementTree before & after.
    """
    had_bom, orig_text = _strip_bom(raw_xml)
    root_before = ET.fromstring(orig_text)
    xml_valid_before = root_before is not None

    lines = orig_text.splitlines(keepends=True)
    provenance: List[ProvenanceEntry] = []
    changes_applied = 0

    for op_spec in ops:
        op_kind = op_spec.get("op")
        if op_kind != "set_attr":
            raise ValueError(f"Unsupported XML merge op: {op_kind}")
        tag = str(op_spec["tag"])
        attr = str(op_spec["attr"])
        new_val = str(op_spec["value"])
        expected_old = op_spec.get("expected_old")
        expected_matches = op_spec.get("expected_matches")
        reason = str(op_spec.get("reason", ""))

        pat = re.compile(rf"(<{re.escape(tag)}\b[^>]*\b{re.escape(attr)}=\")([^\"]*)(\"[^>]*>)")
        matched = 0
        for idx, ln in enumerate(lines):
            m = pat.search(ln)
            if not m:
                continue
            old_val = m.group(2)
            if expected_old is not None and old_val != str(expected_old):
                continue
            new_ln = pat.sub(rf"\g<1>{new_val}\g<3>", ln, count=1)
            lines[idx] = new_ln
            matched += 1
            changes_applied += 1
            provenance.append(
                ProvenanceEntry(
                    mod_id=mod_id,
                    patch_id=patch_id,
                    target=target,
                    location=f"L{idx + 1}:<{tag} @{attr}>",
                    op="xml.set_attr",
                    old_value=old_val,
                    new_value=new_val,
                    reason=reason,
                )
            )

        if expected_matches is not None and matched != int(expected_matches):
            raise AssertionError(
                f"XML patch {patch_id}: expected {expected_matches} matches for <{tag} {attr}=...>, got {matched}"
            )

    patched_text = "".join(lines)
    root_after = ET.fromstring(patched_text)
    xml_valid_after = root_after is not None

    for op_spec in ops:
        if op_spec.get("op") == "set_attr":
            tag = str(op_spec["tag"])
            attr = str(op_spec["attr"])
            new_val = str(op_spec["value"])
            dom_elems = list(root_after.iter(tag))
            if op_spec.get("expected_matches") is not None:
                assert len(dom_elems) == int(op_spec["expected_matches"]), (
                    f"DOM count mismatch for <{tag}>: {len(dom_elems)}"
                )
            for el in dom_elems:
                assert el.attrib.get(attr) == new_val, (
                    f"DOM verification failed on <{tag} {attr}={el.attrib.get(attr)!r}> (expected {new_val!r})"
                )

    diff = "".join(
        difflib.unified_diff(
            orig_text.splitlines(keepends=True),
            patched_text.splitlines(keepends=True),
            fromfile=f"a/{target}",
            tofile=f"b/{target}",
            n=2,
        )
    )
    patched_bytes = _restore_bom(had_bom, patched_text)
    return TextMergeResult(
        target=target,
        patch_id=patch_id,
        had_bom=had_bom,
        original_size=len(raw_xml),
        patched_size=len(patched_bytes),
        changes_applied=changes_applied,
        xml_valid_before=xml_valid_before,
        xml_valid_after=xml_valid_after,
        patched_bytes=patched_bytes,
        unified_diff=diff,
        provenance=provenance,
    )


def _format_float_clean(val: float) -> str:
    txt = f"{val:.6f}".rstrip("0").rstrip(".")
    return txt if "." in txt else f"{txt}.0"


def merge_kv_bytes(
    raw_kv: bytes,
    ops: List[Dict[str, Any]],
    *,
    mod_id: str,
    patch_id: str,
    target: str,
) -> TextMergeResult:
    """
    Apply key-level `set` / `scale` / `add` merges on RAGE visualsettings.dat-style
    key-value files while preserving comments, whitespace/tabs, and line endings.
    """
    had_bom, orig_text = _strip_bom(raw_kv)
    lines = orig_text.splitlines(keepends=True)
    kv_re = re.compile(r"^([ \t]*)([A-Za-z0-9_.-]+)([ \t]+)([^\r\n#;/]+?)([ \t]*(?:\r?\n)?)$")
    key_to_lines: Dict[str, List[int]] = {}
    for idx, ln in enumerate(lines):
        stripped = ln.lstrip()
        if not stripped or stripped.startswith("#") or stripped.startswith("//") or stripped.startswith(";"):
            continue
        m = kv_re.match(ln)
        if m:
            key_to_lines.setdefault(m.group(2), []).append(idx)

    provenance: List[ProvenanceEntry] = []
    changes_applied = 0

    for op_spec in ops:
        op_kind = str(op_spec["op"])
        key = str(op_spec["key"])
        reason = str(op_spec.get("reason", ""))
        if key not in key_to_lines:
            raise KeyError(f"KV patch {patch_id}: key '{key}' not found in {target}")

        for idx in key_to_lines[key]:
            m = kv_re.match(lines[idx])
            assert m is not None
            lead, kname, sep, old_val, tail = m.groups()
            if op_kind == "set":
                new_val = str(op_spec["value"])
            elif op_kind == "scale":
                factor = float(op_spec["factor"])
                new_val = _format_float_clean(float(old_val) * factor)
            elif op_kind == "add":
                delta = float(op_spec["delta"])
                new_val = _format_float_clean(float(old_val) + delta)
            else:
                raise ValueError(f"Unsupported KV op: {op_kind}")

            lines[idx] = f"{lead}{kname}{sep}{new_val}{tail}"
            changes_applied += 1
            provenance.append(
                ProvenanceEntry(
                    mod_id=mod_id,
                    patch_id=patch_id,
                    target=target,
                    location=f"L{idx + 1}:{key}",
                    op=f"kv.{op_kind}",
                    old_value=old_val,
                    new_value=new_val,
                    reason=reason,
                )
            )

    patched_text = "".join(lines)
    diff = "".join(
        difflib.unified_diff(
            orig_text.splitlines(keepends=True),
            patched_text.splitlines(keepends=True),
            fromfile=f"a/{target}",
            tofile=f"b/{target}",
            n=2,
        )
    )
    patched_bytes = _restore_bom(had_bom, patched_text)
    return TextMergeResult(
        target=target,
        patch_id=patch_id,
        had_bom=had_bom,
        original_size=len(raw_kv),
        patched_size=len(patched_bytes),
        changes_applied=changes_applied,
        xml_valid_before=None,
        xml_valid_after=None,
        patched_bytes=patched_bytes,
        unified_diff=diff,
        provenance=provenance,
    )


_KV_LINE_RE = re.compile(r"^([ \t]*)([A-Za-z0-9_.-]+)([ \t]+)([^\r\n#;/]+?)([ \t]*(?:\r?\n)?)$")


def parse_kv_pairs(raw_kv: bytes) -> List[Tuple[str, str]]:
    """Extract ordered semantic (key, value) pairs from a visualsettings.dat-style KV buffer."""
    _, text = _strip_bom(raw_kv)
    pairs: List[Tuple[str, str]] = []
    for ln in text.splitlines(keepends=True):
        stripped = ln.lstrip()
        if not stripped or stripped.startswith("#") or stripped.startswith("//") or stripped.startswith(";"):
            continue
        m = _KV_LINE_RE.match(ln)
        if m:
            pairs.append((m.group(2), m.group(4).strip()))
    return pairs


def compact_kv_bytes(raw_kv: bytes, max_bytes: int) -> Tuple[bytes, int]:
    """
    Reclaim non-semantic whitespace/comments from a KV buffer (or single 64 KiB KV page)
    so `len(out) <= max_bytes` while preserving 100% semantic (key, value) equivalence.

    Passes run in minimal-diff order and stop immediately once `len <= max_bytes`:
      1. Collapse repeated tab/space separators on KV lines (`\\t\\t` -> `\\t`, `  ` -> ` `)
      2. Trim trailing horizontal whitespace before line endings
      3. Drop blank lines
      4. Drop pure comment lines (`#`, `//`, `;`)

    Returns (compacted_bytes, bytes_reclaimed).
    """
    if len(raw_kv) <= max_bytes:
        return raw_kv, 0

    before_pairs = parse_kv_pairs(raw_kv)
    had_bom, orig_text = _strip_bom(raw_kv)
    bom_len = 3 if had_bom else 0
    lines = orig_text.splitlines(keepends=True)

    def current_len() -> int:
        return bom_len + sum(len(ln.encode("utf-8")) for ln in lines)

    need = current_len() - max_bytes

    # Pass 1: Collapse repeated whitespace separators on KV lines (1 byte at a time)
    for idx, ln in enumerate(lines):
        if need <= 0:
            break
        stripped = ln.lstrip()
        if not stripped or stripped.startswith("#") or stripped.startswith("//") or stripped.startswith(";"):
            continue
        m = _KV_LINE_RE.match(ln)
        if not m:
            continue
        lead, kname, sep, val, tail = m.groups()
        if len(sep) > 1:
            trim = min(len(sep) - 1, need)
            new_sep = sep[: len(sep) - trim]
            lines[idx] = f"{lead}{kname}{new_sep}{val}{tail}"
            need -= trim

    # Pass 2: Trim trailing spaces/tabs before line endings
    if need > 0:
        for idx, ln in enumerate(lines):
            if need <= 0:
                break
            eol = "\r\n" if ln.endswith("\r\n") else ("\n" if ln.endswith("\n") else "")
            body = ln[: len(ln) - len(eol)] if eol else ln
            trimmed = body.rstrip(" \t")
            saved = len(body) - len(trimmed)
            if saved > 0:
                take = min(saved, need)
                lines[idx] = body[: len(body) - take] + eol
                need -= take

    # Pass 3: Drop blank lines
    if need > 0:
        kept: List[str] = []
        for ln in lines:
            if need > 0 and ln.strip() == "":
                need -= len(ln.encode("utf-8"))
                continue
            kept.append(ln)
        lines = kept

    # Pass 4: Drop pure comment lines
    if need > 0:
        kept = []
        for ln in lines:
            s = ln.lstrip()
            if need > 0 and (s.startswith("#") or s.startswith("//") or s.startswith(";")):
                need -= len(ln.encode("utf-8"))
                continue
            kept.append(ln)
        lines = kept

    out_bytes = _restore_bom(had_bom, "".join(lines))
    if len(out_bytes) > max_bytes:
        raise ValueError(
            f"compact_kv_bytes could not fit {len(raw_kv)} B into {max_bytes} B (remaining {len(out_bytes)} B)"
        )

    after_pairs = parse_kv_pairs(out_bytes)
    if after_pairs != before_pairs:
        raise AssertionError("compact_kv_bytes semantic KV parity check failed")

    return out_bytes, len(raw_kv) - len(out_bytes)


def _xml_canonical_events(xml_bytes: bytes) -> List[Tuple[str, Tuple[Tuple[str, str], ...], str]]:
    _, text = _strip_bom(xml_bytes)
    root = ET.fromstring(text)
    events: List[Tuple[str, Tuple[Tuple[str, str], ...], str]] = []
    for el in root.iter():
        events.append((el.tag, tuple(sorted(el.attrib.items())), (el.text or "").strip()))
    return events


def compact_xml_slice(
    slice_bytes: bytes,
    max_bytes: int,
    *,
    full_prefix: bytes = b"",
    full_suffix: bytes = b"",
) -> Tuple[bytes, int]:
    """
    Reclaim non-semantic indentation, blank lines, and comment lines from an XML
    buffer (or single 64 KiB XML page) so `len(out) <= max_bytes`, validating full
    ElementTree DOM equivalence when `full_prefix + slice_bytes + full_suffix` is
    a complete XML document.
    """
    if len(slice_bytes) <= max_bytes:
        return slice_bytes, 0

    full_before = full_prefix + slice_bytes + full_suffix
    before_events = _xml_canonical_events(full_before)

    had_bom, orig_text = _strip_bom(slice_bytes)
    bom_len = 3 if had_bom else 0
    lines = orig_text.splitlines(keepends=True)
    need = (bom_len + sum(len(ln.encode("utf-8")) for ln in lines)) - max_bytes

    # Pass 1: Trim leading indentation whitespace on lines beginning with '<'
    for idx, ln in enumerate(lines):
        if need <= 0:
            break
        stripped = ln.lstrip(" \t")
        if stripped.startswith("<"):
            lead_len = len(ln) - len(stripped)
            if lead_len > 0:
                take = min(lead_len, need)
                lines[idx] = ln[take:]
                need -= take

    # Pass 2: Drop blank lines
    if need > 0:
        kept: List[str] = []
        for ln in lines:
            if need > 0 and ln.strip() == "":
                need -= len(ln.encode("utf-8"))
                continue
            kept.append(ln)
        lines = kept

    # Pass 3: Drop standalone single-line XML comments
    if need > 0:
        kept = []
        for ln in lines:
            s = ln.strip()
            if need > 0 and s.startswith("<!--") and s.endswith("-->"):
                need -= len(ln.encode("utf-8"))
                continue
            kept.append(ln)
        lines = kept

    out_bytes = _restore_bom(had_bom, "".join(lines))
    if len(out_bytes) > max_bytes:
        raise ValueError(
            f"compact_xml_slice could not fit {len(slice_bytes)} B into {max_bytes} B (remaining {len(out_bytes)} B)"
        )

    after_events = _xml_canonical_events(full_prefix + out_bytes + full_suffix)
    if after_events != before_events:
        raise AssertionError("compact_xml_slice semantic XML DOM parity check failed")

    return out_bytes, len(slice_bytes) - len(out_bytes)
