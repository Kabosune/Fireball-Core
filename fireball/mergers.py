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
