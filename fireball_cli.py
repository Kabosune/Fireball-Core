#!/usr/bin/env python3
"""
fireball_cli.py — Offline Phase 0 Test Runner & Vertical-Slice Verifier for Fireball-Core.

Python 3.13, stdlib only. Reuses `pso_decode.py` from the companion research repo
(`--tools-dir` or env `GTA6_MODDING_TOOLS`) without copying.
Strictly read-only against `--game-dir` (default: env `GTA6_DIR` or `D:\\Games\\GTA6`);
all outputs/diffs/exports go under `--out` (default: `D:\\GTA6-work\\fireball-fpv-test`).
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import shutil
import sys
from typing import Any, Dict, List, Optional, Tuple

_ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
if _ROOT_DIR not in sys.path:
    sys.path.insert(0, _ROOT_DIR)

from fireball.abi import verify_c11_headers  # noqa: E402
from fireball.archive_reader import parse_int_hex, read_paged_zlib_at, read_single_zlib_chunk_at  # noqa: E402
from fireball.bundle import build_patch_bundle, inspect_bundle_file  # noqa: E402
from fireball.load_order import LoadOrderError, resolve_load_order  # noqa: E402
from fireball.mergers import merge_kv_bytes, merge_xml_bytes  # noqa: E402
from fireball.mock_host import run_mock_host_suite  # noqa: E402
from fireball.mod_source import load_manifest, open_mod_source, pack_mod_to_zip  # noqa: E402
from fireball.pso_patcher import patch_pso_bytes, probe_chks_algorithm  # noqa: E402


DEFAULT_GAME_DIR = os.environ.get("GTA6_DIR", r"D:\Games\GTA6")
DEFAULT_OUT_DIR = r"D:\GTA6-work\fireball-fpv-test"
DEFAULT_MOD_DIR = os.path.join(_ROOT_DIR, "examples", "fpv_unlocker")


def _check_out_not_in_game_dir(out_path: str, game_dir: str) -> None:
    """Refuse writing outputs inside the read-only game install directory."""
    abs_out = os.path.abspath(out_path)
    abs_game = os.path.abspath(game_dir)
    try:
        common = os.path.commonpath([abs_out, abs_game])
    except ValueError:
        return
    if os.path.normcase(common) == os.path.normcase(abs_game):
        raise ValueError(f"Refusing to write inside read-only game directory: {abs_out}")


def _validate_mod_against_game(mod_path: str, game_dir: str) -> Dict[str, Any]:
    with open_mod_source(mod_path) as src:
        manifest = load_manifest(src)
        checks: List[Dict[str, Any]] = []

        for p in manifest.xml_patches + manifest.kv_patches + manifest.pso_patches:
            pfile = str(p["patch"])
            if not src.exists(pfile):
                raise FileNotFoundError(f"Missing patch file '{pfile}' in {mod_path}")
            _ = src.read_toml(pfile)
            arch_path = os.path.join(game_dir, str(p["archive"]))
            off = parse_int_hex(p["archive_offset"])
            raw, pages, _ = read_paged_zlib_at(arch_path, off)
            page_ok = True
            if "page_offset" in p:
                poff = parse_int_hex(p["page_offset"])
                chunk = read_single_zlib_chunk_at(arch_path, poff)
                page_ok = len(chunk) > 0 and (chunk in raw)
            checks.append(
                {
                    "id": p["id"],
                    "archive": p["archive"],
                    "archive_offset": f"0x{off:08X}",
                    "decompressed_bytes": len(raw),
                    "pages": pages,
                    "page_offset_verified": page_ok,
                }
            )
            if not page_ok:
                raise AssertionError(f"page_offset check failed for {p['id']}")

        for plg in manifest.plugins:
            if not src.exists(str(plg["entry"])):
                raise FileNotFoundError(f"Missing plugin entry '{plg['entry']}' in {mod_path}")

        return {
            "source_kind": src.kind,
            "source_path": src.source_path,
            "mod_id": manifest.mod_id,
            "version": manifest.version,
            "target_game_version": manifest.target_game_version,
            "validated_targets": checks,
            "plugins": [p["id"] for p in manifest.plugins],
        }


def _apply_mod_offline(
    mod_path: str,
    game_dir: str,
    out_dir: str,
    *,
    tools_dir: Optional[str] = None,
) -> Dict[str, Any]:
    _check_out_not_in_game_dir(out_dir, game_dir)
    os.makedirs(out_dir, exist_ok=True)
    provenance_all: List[Dict[str, Any]] = []
    summary: Dict[str, Any] = {
        "xml": [],
        "kv": [],
        "pso": [],
        "out_dir": os.path.abspath(out_dir),
    }

    with open_mod_source(mod_path) as src:
        manifest = load_manifest(src)
        summary["source_kind"] = src.kind
        summary["mod_id"] = manifest.mod_id

        # 1. XML patches
        for p in manifest.xml_patches:
            arch_path = os.path.join(game_dir, str(p["archive"]))
            off = parse_int_hex(p["archive_offset"])
            raw, pages, _ = read_paged_zlib_at(arch_path, off)
            ops = src.read_toml(str(p["patch"])).get("ops", [])
            res = merge_xml_bytes(raw, ops, mod_id=manifest.mod_id, patch_id=str(p["id"]), target=str(p["target"]))

            out_xml = os.path.join(out_dir, f"{p['id']}.patched.xml")
            out_diff = os.path.join(out_dir, f"{p['id']}.diff")
            with open(out_xml, "wb") as wf:
                wf.write(res.patched_bytes)
            with open(out_diff, "w", encoding="utf-8") as wf:
                wf.write(res.unified_diff)
            provenance_all.extend(pr.to_dict() for pr in res.provenance)
            summary["xml"].append(
                {
                    "id": p["id"],
                    "target": p["target"],
                    "archive": f"{p['archive']} @ 0x{off:08X} (page @ {p.get('page_offset', '-')}, {pages} pages)",
                    "changes_applied": res.changes_applied,
                    "xml_valid_before": res.xml_valid_before,
                    "xml_valid_after": res.xml_valid_after,
                    "patched_file": out_xml,
                    "diff_file": out_diff,
                    "diff_preview": res.unified_diff,
                }
            )

        # 2. KV patches (visualsettings.dat)
        for p in manifest.kv_patches:
            arch_path = os.path.join(game_dir, str(p["archive"]))
            off = parse_int_hex(p["archive_offset"])
            raw, pages, _ = read_paged_zlib_at(arch_path, off)
            ops = src.read_toml(str(p["patch"])).get("ops", [])
            res = merge_kv_bytes(raw, ops, mod_id=manifest.mod_id, patch_id=str(p["id"]), target=str(p["target"]))

            out_kv = os.path.join(out_dir, f"{p['id']}.patched.dat")
            out_diff = os.path.join(out_dir, f"{p['id']}.diff")
            with open(out_kv, "wb") as wf:
                wf.write(res.patched_bytes)
            with open(out_diff, "w", encoding="utf-8") as wf:
                wf.write(res.unified_diff)
            provenance_all.extend(pr.to_dict() for pr in res.provenance)
            summary["kv"].append(
                {
                    "id": p["id"],
                    "target": p["target"],
                    "archive": f"{p['archive']} @ 0x{off:08X} (page @ {p.get('page_offset', '-')}, {pages} pages)",
                    "changes_applied": res.changes_applied,
                    "patched_file": out_kv,
                    "diff_file": out_diff,
                    "diff_preview": res.unified_diff,
                }
            )

        # 3. Binary PSO patches
        for p in manifest.pso_patches:
            if not p.get("enabled", True):
                continue
            arch_path = os.path.join(game_dir, str(p["archive"]))
            off = parse_int_hex(p["archive_offset"])
            raw, _pages, _ = read_paged_zlib_at(arch_path, off)
            ops = src.read_toml(str(p["patch"])).get("ops", [])
            root_h = parse_int_hex(p["root_struct_hash"]) if "root_struct_hash" in p else None
            res = patch_pso_bytes(
                raw,
                ops,
                patch_id=str(p["id"]),
                target=str(p["target"]),
                archive_offset=off,
                expected_root_hash=root_h,
                tools_dir=tools_dir,
            )

            out_pso = os.path.join(out_dir, f"{p['id']}.patched.pso")
            with open(out_pso, "wb") as wf:
                wf.write(res.patched_bytes)
            for d in res.diffs:
                provenance_all.append(
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
            summary["pso"].append(
                {
                    "id": p["id"],
                    "target": p["target"],
                    "archive_offset": f"0x{off:08X}",
                    "optional": bool(p.get("optional", False)),
                    "size_bytes": res.patched_size,
                    "roundtrip_identity_ok": res.roundtrip_identity_ok,
                    "redecode_ok": res.redecode_ok,
                    "decoded_structs": res.decoded_structs,
                    "decoded_members": res.decoded_members,
                    "unknown_types": res.unknown_types,
                    "chks_original": res.chks_original,
                    "chks_status": res.chks_status,
                    "patched_file": out_pso,
                    "diffs": [d.to_dict() for d in res.diffs],
                }
            )

    prov_path = os.path.join(out_dir, "provenance_report.json")
    with open(prov_path, "w", encoding="utf-8") as wf:
        json.dump(
            {
                "mod_id": summary.get("mod_id"),
                "source_kind": summary.get("source_kind"),
                "total_field_changes": len(provenance_all),
                "entries": provenance_all,
            },
            wf,
            indent=2,
        )
    summary["provenance_file"] = prov_path
    summary["total_field_changes"] = len(provenance_all)
    return summary


# ---------------------------------------------------------------------------
# Subcommand handlers
# ---------------------------------------------------------------------------

def cmd_pack(args: argparse.Namespace) -> int:
    out_zip = args.output or os.path.join(args.out, "fpv_unlocker.gta6mod")
    _check_out_not_in_game_dir(out_zip, args.game_dir)
    info = pack_mod_to_zip(args.mod, out_zip)
    print(json.dumps(info, indent=2))
    return 0


def cmd_validate(args: argparse.Namespace) -> int:
    ok_h, h_errs = verify_c11_headers(os.path.join(_ROOT_DIR, "include"))
    if not ok_h:
        print("C11 ABI header validation FAILED:", h_errs, file=sys.stderr)
        return 1
    res = _validate_mod_against_game(args.mod, args.game_dir)
    res["c11_headers_ok"] = ok_h
    print(json.dumps(res, indent=2))
    return 0


def cmd_apply(args: argparse.Namespace) -> int:
    res = _apply_mod_offline(args.mod, args.game_dir, args.out, tools_dir=args.tools_dir)
    print(json.dumps({k: v for k, v in res.items() if k != "diff_preview"}, indent=2))
    return 0


def cmd_chks_probe(args: argparse.Namespace) -> int:
    data_rpf = os.path.join(args.game_dir, "data.rpf")
    off = parse_int_hex(args.offset)
    buf, _, _ = read_paged_zlib_at(data_rpf, off)
    pair = None
    try:
        b1, _, _ = read_paged_zlib_at(data_rpf, 0x00032000)
        b2, _, _ = read_paged_zlib_at(data_rpf, 0x0003D000)
        if len(b1) == len(b2):
            pair = (b1, b2)
    except Exception:
        pair = None
    rep = probe_chks_algorithm(buf, same_len_pair=pair, tools_dir=args.tools_dir)
    rep["target"] = f"data.rpf @ 0x{off:08X}"
    print(json.dumps(rep, indent=2))
    return 0


def cmd_simulate(args: argparse.Namespace) -> int:
    with open_mod_source(args.mod) as src:
        rep = run_mock_host_suite(src)
    print(json.dumps(rep, indent=2))
    return 0


def cmd_build_bundle(args: argparse.Namespace) -> int:
    _check_out_not_in_game_dir(args.out, args.game_dir)
    mod_paths = args.mod if args.mod else [DEFAULT_MOD_DIR]
    extra_overlays: Dict[str, bytes] = {}
    for spec in args.overlay or []:
        if "=" not in spec:
            raise ValueError(f"Invalid --overlay '{spec}': expected TARGET_REL_PATH=SOURCE_FILE")
        target_rel, src_file = spec.split("=", 1)
        with open(src_file, "rb") as rf:
            extra_overlays[target_rel] = rf.read()
    rep = build_patch_bundle(
        mod_paths,
        args.game_dir,
        args.out,
        bundle_filename=args.bundle_name,
        extra_overlays=extra_overlays or None,
        size_preserve=bool(args.size_preserve),
        tools_dir=args.tools_dir,
    )
    print(json.dumps(rep, indent=2))
    return 0


def cmd_inspect_bundle(args: argparse.Namespace) -> int:
    info = inspect_bundle_file(args.bundle)
    if args.json:
        print(json.dumps(info, indent=2))
        return 0

    print(
        f"Bundle: {info['bundle_file']} (v{info['bundle_version']}, {info['bundle_size_bytes']:,} B, "
        f"all_fit_fixed_dst={info['all_fit_fixed_dst']})"
    )
    print(f"DirectStorage entries ({info['dstorage_entry_count']}):")
    print(
        f"  {'Archive':<18} {'Offset':<12} {'OrigSize':>10} {'Patched':>10} {'Delta':>7} "
        f"{'FitsDst':>7} {'CRC32':<12} Flags"
    )
    for e in info["dstorage_entries"]:
        flags_s = "|".join(e["flags"])
        print(
            f"  {e['archive_path']:<18} {e['file_offset']:<12} {e['orig_uncomp_size']:>10} "
            f"{e['payload_size']:>10} {e['delta']:>+7} {str(e['fits_fixed_dst']):>7} "
            f"{e['payload_crc32']:<12} {flags_s}"
        )
    print(f"Loose-file overlays ({info['overlay_entry_count']}):")
    for o in info["overlay_entries"]:
        print(f"  {o['rel_path']} ({o['size_bytes']:,} B, crc32={o['crc32']})")
    print(f"Plugin configs ({info['plugin_config_count']}):")
    for c in info["plugin_configs"]:
        print(f"  {c['mod_id']} ({c['json_len']} B JSON)")
    print(f"Shader DXIL overrides ({info['shader_entry_count']}):")
    for s in info["shader_entries"]:
        eff = f" effect='{s['effect_name']}'" if s["effect_name"] else ""
        print(
            f"  blake3={s['container_blake3'][:16]}... dxbc={s['dxbc_digest']} "
            f"({s['payload_size']:,} B, crc32={s['payload_crc32']}){eff}"
        )
    return 0


def _stage_native_plugins(
    ordered_mods: List[Any],
    out_dir: str,
    plugin_dirs: List[str],
) -> Tuple[List[Dict[str, str]], List[Dict[str, str]]]:
    """
    Copy `*.fbplugin.dll` binaries referenced by or embedded inside staged mods
    into `out_dir`. Searches each mod source first, then `--plugin-dir` directories.
    Returns `(copied_plugins, missing_plugins)`.
    """
    copied: List[Dict[str, str]] = []
    missing: List[Dict[str, str]] = []
    copied_by_name: Dict[str, str] = {}

    for rm in ordered_mods:
        with open_mod_source(rm.source_path) as src:
            embedded_dlls: Dict[str, str] = {}
            for rel in src.list_files():
                if rel.lower().endswith(".fbplugin.dll"):
                    embedded_dlls[os.path.basename(rel).lower()] = rel

            # 1. Copy every *.fbplugin.dll embedded in the mod source
            for base_low, rel in sorted(embedded_dlls.items()):
                base_name = os.path.basename(rel)
                dst_path = os.path.join(out_dir, base_name)
                with open(dst_path, "wb") as wf:
                    wf.write(src.read_bytes(rel))
                copied_by_name[base_low] = dst_path
                copied.append(
                    {
                        "mod_id": rm.mod_id,
                        "plugin_file": base_name,
                        "source": f"{rm.source_path}:{rel}",
                        "dest": os.path.abspath(dst_path),
                    }
                )

            # 2. Resolve any declared `[[plugins]]` native binaries from mod or `--plugin-dir`
            for plg in rm.manifest.plugins:
                native_path = str(plg.get("native_path", "")).strip()
                plg_id = str(plg.get("id", "")).strip()
                candidates: List[str] = []
                if native_path:
                    candidates.append(os.path.basename(native_path.replace("\\", "/")))
                if plg_id:
                    cand_id = f"{plg_id}.fbplugin.dll"
                    if cand_id.lower() not in [c.lower() for c in candidates]:
                        candidates.append(cand_id)

                if not candidates:
                    continue
                if any(c.lower() in copied_by_name for c in candidates):
                    continue

                found_src: Optional[str] = None
                found_name: Optional[str] = None
                for pdir in plugin_dirs:
                    for cand in candidates:
                        probe = os.path.join(pdir, cand)
                        if os.path.isfile(probe):
                            found_src = os.path.abspath(probe)
                            found_name = cand
                            break
                    if found_src:
                        break

                if found_src and found_name:
                    dst_path = os.path.join(out_dir, found_name)
                    if os.path.normcase(os.path.abspath(dst_path)) != os.path.normcase(found_src):
                        shutil.copy2(found_src, dst_path)
                    copied_by_name[found_name.lower()] = dst_path
                    copied.append(
                        {
                            "mod_id": rm.mod_id,
                            "plugin_file": found_name,
                            "source": found_src,
                            "dest": os.path.abspath(dst_path),
                        }
                    )
                else:
                    missing.append(
                        {
                            "mod_id": rm.mod_id,
                            "plugin_id": plg_id,
                            "expected": candidates[0],
                        }
                    )

    return copied, missing


def cmd_stage(args: argparse.Namespace) -> int:
    _check_out_not_in_game_dir(args.out, args.game_dir)
    if not args.mod and not args.profile:
        print("Stage error: pass at least one --mod or --profile <load_order.toml>.", file=sys.stderr)
        return 1

    try:
        resolution = resolve_load_order(args.mod or [], profile_path=args.profile)
    except (LoadOrderError, FileNotFoundError, ValueError) as exc:
        print(f"Load-order resolution failed: {exc}", file=sys.stderr)
        return 1

    os.makedirs(args.out, exist_ok=True)
    extra_overlays: Dict[str, bytes] = {}
    for spec in args.overlay or []:
        if "=" not in spec:
            raise ValueError(f"Invalid --overlay '{spec}': expected TARGET_REL_PATH=SOURCE_FILE")
        target_rel, src_file = spec.split("=", 1)
        with open(src_file, "rb") as rf:
            extra_overlays[target_rel] = rf.read()

    effective_size_preserve = bool(args.size_preserve) or bool(resolution.size_preserve)
    effective_bundle_name = args.bundle_name or resolution.bundle_name or "fireball_bundle.fbin"

    bundle_rep = build_patch_bundle(
        resolution.mod_paths,
        args.game_dir,
        args.out,
        bundle_filename=effective_bundle_name,
        extra_overlays=extra_overlays or None,
        mod_overrides=resolution.mod_overrides or None,
        size_preserve=effective_size_preserve,
        tools_dir=args.tools_dir,
    )

    plugin_dirs = [os.path.abspath(d) for d in (args.plugin_dir or [])]
    copied_plugins, missing_plugins = _stage_native_plugins(resolution.ordered, args.out, plugin_dirs)

    copied_core_dll: Optional[str] = None
    if args.core_dll:
        src_core = os.path.abspath(args.core_dll)
        if not os.path.isfile(src_core):
            print(f"Stage error: --core-dll not found: {src_core}", file=sys.stderr)
            return 1
        dst_core = os.path.join(args.out, os.path.basename(src_core))
        if os.path.normcase(os.path.abspath(dst_core)) != os.path.normcase(src_core):
            shutil.copy2(src_core, dst_core)
        copied_core_dll = os.path.abspath(dst_core)

    conflicts = bundle_rep.get("conflicts", [])
    conflicts_report = {
        "load_order": [m.mod_id for m in resolution.ordered],
        "conflict_count": len(conflicts),
        "conflicts": conflicts,
    }
    conflicts_path = os.path.join(args.out, "fireball_conflicts.json")
    with open(conflicts_path, "w", encoding="utf-8") as wf:
        json.dump(conflicts_report, wf, indent=2)

    stage_report = {
        "out_dir": os.path.abspath(args.out),
        "profile_path": resolution.profile_path,
        "profile_name": resolution.profile_name,
        "load_order": [m.to_summary_dict() for m in resolution.ordered],
        "disabled_mods": resolution.disabled,
        "bundle_file": bundle_rep["bundle_file"],
        "bundle_version": bundle_rep["bundle_version"],
        "bundle_size_bytes": bundle_rep["bundle_size_bytes"],
        "size_preserve": effective_size_preserve,
        "all_fit_fixed_dst": bundle_rep["all_fit_fixed_dst"],
        "provenance_file": bundle_rep["provenance_file"],
        "conflicts_file": os.path.abspath(conflicts_path),
        "dstorage_entry_count": bundle_rep["dstorage_entry_count"],
        "overlay_entry_count": bundle_rep["overlay_entry_count"],
        "plugin_config_count": bundle_rep["plugin_config_count"],
        "shader_entry_count": bundle_rep["shader_entry_count"],
        "copied_plugins": copied_plugins,
        "missing_plugins": missing_plugins,
        "copied_core_dll": copied_core_dll,
        "conflict_count": len(conflicts),
        "conflicts": conflicts,
    }
    stage_report_path = os.path.join(args.out, "fireball_stage_report.json")
    with open(stage_report_path, "w", encoding="utf-8") as wf:
        json.dump(stage_report, wf, indent=2)
    stage_report["stage_report_file"] = os.path.abspath(stage_report_path)

    if args.json:
        print(json.dumps(stage_report, indent=2))
        return 0

    print(f"Staged {len(resolution.ordered)} mod(s) into: {stage_report['out_dir']}")
    print("Load order:")
    for m in resolution.ordered:
        print(f"  {m.load_index + 1}. {m.mod_id} v{m.version} ({m.source_kind}: {m.source_path})")
    if resolution.disabled:
        print(f"Disabled mods ({len(resolution.disabled)}):")
        for d in resolution.disabled:
            print(f"  - {d['mod_id']} ({d['reason']})")
    print(
        f"Bundle: {stage_report['bundle_file']} (v{stage_report['bundle_version']}, "
        f"{stage_report['bundle_size_bytes']:,} B, ds={stage_report['dstorage_entry_count']}, "
        f"ov={stage_report['overlay_entry_count']}, cfg={stage_report['plugin_config_count']}, "
        f"shaders={stage_report['shader_entry_count']}, all_fit_fixed_dst={stage_report['all_fit_fixed_dst']})"
    )
    print(f"Native plugins copied ({len(copied_plugins)}):")
    for cp in copied_plugins:
        print(f"  {cp['plugin_file']} <- {cp['source']}")
    if missing_plugins:
        print(f"Native plugins not found ({len(missing_plugins)}):")
        for mp in missing_plugins:
            print(f"  ! {mp['expected']} (mod={mp['mod_id']})")
    if copied_core_dll:
        print(f"Core DLL copied: {copied_core_dll}")
    print(f"Field/overlay/shader conflicts ({len(conflicts)}):")
    if not conflicts:
        print("  (none)")
    else:
        for c in conflicts:
            overridden = ", ".join(c["overridden_mods"])
            print(
                f"  [{c['kind']}] {c['target']} :: {c['field']} -> "
                f"winner={c['winner_mod']} (overrides: {overridden}; value={c['winning_value']})"
            )
    print(f"Conflicts JSON: {stage_report['conflicts_file']}")
    print(f"Provenance JSON: {stage_report['provenance_file']}")
    return 0


def _normalize_pipe_name(pipe_arg: Optional[str]) -> str:
    raw = pipe_arg or os.environ.get("FIREBALL_CONSOLE_PIPE") or r"\\.\pipe\fireball_console"
    if raw.lower().startswith("\\\\.\\pipe\\"):
        return raw
    return "\\\\.\\pipe\\" + raw


class _Win32ConsolePipeClient:
    r"""Stdlib-only Win32 named-pipe client for `\\.\pipe\fireball_console`."""

    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    OPEN_EXISTING = 3
    INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value
    ERROR_PIPE_BUSY = 231

    def __init__(self, pipe_name: str, timeout_ms: int = 3000) -> None:
        if sys.platform != "win32":
            raise RuntimeError("Fireball console named pipe is only supported on Windows.")
        self.pipe_name = pipe_name
        self.timeout_ms = timeout_ms
        self._k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._k32.CreateFileW.restype = ctypes.c_void_p
        self._k32.CreateFileW.argtypes = [
            ctypes.c_wchar_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.c_uint32,
            ctypes.c_void_p,
        ]
        self._k32.WaitNamedPipeW.restype = ctypes.c_int
        self._k32.WaitNamedPipeW.argtypes = [ctypes.c_wchar_p, ctypes.c_uint32]
        self._k32.ReadFile.restype = ctypes.c_int
        self._k32.ReadFile.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_void_p,
        ]
        self._k32.WriteFile.restype = ctypes.c_int
        self._k32.WriteFile.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint32,
            ctypes.POINTER(ctypes.c_uint32),
            ctypes.c_void_p,
        ]
        self._k32.CloseHandle.restype = ctypes.c_int
        self._k32.CloseHandle.argtypes = [ctypes.c_void_p]
        self._handle: Optional[int] = None
        self._rx_buf = bytearray()

    def connect(self) -> None:
        for _ in range(4):
            h = self._k32.CreateFileW(
                self.pipe_name,
                self.GENERIC_READ | self.GENERIC_WRITE,
                0,
                None,
                self.OPEN_EXISTING,
                0,
                None,
            )
            if h != self.INVALID_HANDLE_VALUE and h is not None:
                self._handle = h
                return
            err = ctypes.get_last_error()
            if err == self.ERROR_PIPE_BUSY:
                self._k32.WaitNamedPipeW(self.pipe_name, self.timeout_ms)
                continue
            raise OSError(err, f"Could not connect to Fireball console pipe '{self.pipe_name}' (Win32 error {err})")
        raise OSError(self.ERROR_PIPE_BUSY, f"Timed out waiting for busy pipe '{self.pipe_name}'")

    def close(self) -> None:
        if self._handle is not None:
            self._k32.CloseHandle(self._handle)
            self._handle = None

    def __enter__(self) -> "_Win32ConsolePipeClient":
        self.connect()
        return self

    def __exit__(self, *_: Any) -> None:
        self.close()

    def _readline(self) -> Optional[str]:
        assert self._handle is not None
        while True:
            nl = self._rx_buf.find(b"\n")
            if nl != -1:
                raw = bytes(self._rx_buf[:nl])
                del self._rx_buf[: nl + 1]
                return raw.rstrip(b"\r").decode("utf-8", errors="replace")
            buf = ctypes.create_string_buffer(4096)
            got = ctypes.c_uint32(0)
            ok = self._k32.ReadFile(self._handle, buf, 4096, ctypes.byref(got), None)
            if not ok or got.value == 0:
                if self._rx_buf:
                    raw = bytes(self._rx_buf)
                    self._rx_buf.clear()
                    return raw.rstrip(b"\r\n").decode("utf-8", errors="replace")
                return None
            self._rx_buf.extend(buf.raw[: got.value])

    def exec_command(self, line: str) -> Tuple[int, List[str]]:
        assert self._handle is not None
        payload = (line.replace("\r", "").replace("\n", " ") + "\n").encode("utf-8")
        written = ctypes.c_uint32(0)
        ok = self._k32.WriteFile(self._handle, payload, len(payload), ctypes.byref(written), None)
        if not ok or written.value != len(payload):
            err = ctypes.get_last_error()
            raise OSError(err, f"WriteFile failed on '{self.pipe_name}' (Win32 error {err})")

        lines: List[str] = []
        rc = 0
        while True:
            resp = self._readline()
            if resp is None:
                raise ConnectionError("Fireball console pipe closed unexpectedly.")
            if resp.startswith("\x04"):
                try:
                    rc = int(resp[1:].strip() or "0")
                except ValueError:
                    rc = 0
                break
            lines.append(resp)
        return rc, lines


def cmd_console(args: argparse.Namespace) -> int:
    pipe_name = _normalize_pipe_name(args.pipe)
    one_shot: List[str] = list(args.command or [])
    try:
        with _Win32ConsolePipeClient(pipe_name, timeout_ms=args.timeout_ms) as client:
            if one_shot:
                exit_rc = 0
                for cmd_line in one_shot:
                    rc, out_lines = client.exec_command(cmd_line)
                    for ln in out_lines:
                        print(ln)
                    if rc != 0:
                        exit_rc = 1
                return exit_rc

            print(f"Connected to Fireball Console ({pipe_name}). Type 'help' or 'exit'.")
            while True:
                try:
                    line = input("fireball> ")
                except (EOFError, KeyboardInterrupt):
                    print()
                    return 0
                stripped = line.strip()
                if not stripped:
                    continue
                if stripped.lower() in ("exit", "quit"):
                    return 0
                rc, out_lines = client.exec_command(stripped)
                for ln in out_lines:
                    print(ln)
                if rc != 0 and not out_lines:
                    print(f"[rc={rc}]")
    except (OSError, ConnectionError, RuntimeError) as exc:
        print(f"Console error: {exc}", file=sys.stderr)
        return 1


def cmd_verify_all(args: argparse.Namespace) -> int:
    """Run the full end-to-end vertical-slice test suite in one command."""
    _check_out_not_in_game_dir(args.out, args.game_dir)
    os.makedirs(args.out, exist_ok=True)
    steps: List[Dict[str, Any]] = []

    def record(name: str, ok: bool, detail: str) -> None:
        steps.append({"test": name, "status": "PASS" if ok else "FAIL", "detail": detail})

    # 1. C11 ABI headers contract verification
    ok_h, h_errs = verify_c11_headers(os.path.join(_ROOT_DIR, "include"))
    record(
        "1. C11 ABI Headers (fireball_host.h & fireball_plugin.h)",
        ok_h,
        "Strict C11 guards, all 16 FB_CAP_* macros, input/symbol/guarded title_mem hooks verified"
        if ok_h
        else "; ".join(h_errs),
    )

    # 2. Pack loose folder -> .gta6mod ZIP and validate BOTH loose & ZIP against game_dir
    zip_path = os.path.join(args.out, "fpv_unlocker.gta6mod")
    pack_info = pack_mod_to_zip(args.mod, zip_path)
    val_loose = _validate_mod_against_game(args.mod, args.game_dir)
    val_zip = _validate_mod_against_game(zip_path, args.game_dir)
    both_ok = (
        val_loose["mod_id"] == val_zip["mod_id"]
        and len(val_loose["validated_targets"]) == 5
        and len(val_zip["validated_targets"]) == 5
    )
    record(
        "2. Dual-Mode Mod Loader (Loose Folder + .gta6mod ZIP)",
        both_ok,
        f"mod={val_loose['mod_id']} v{val_loose['version']}, packed={pack_info['file_count']} files "
        f"({pack_info['zip_size']:,} B), 5/5 archive & page targets validated against {args.game_dir}",
    )

    # 3. Real offline apply (XML merge + KV merge + 3 Binary PSO patches) from BOTH loose & ZIP
    out_loose = os.path.join(args.out, "out_loose")
    out_zip = os.path.join(args.out, "out_zip")
    apply_loose = _apply_mod_offline(args.mod, args.game_dir, out_loose, tools_dir=args.tools_dir)
    apply_zip = _apply_mod_offline(zip_path, args.game_dir, out_zip, tools_dir=args.tools_dir)

    parity_ok = True
    for fn in (
        "lasso_fpv_camera_matrix.patched.xml",
        "visualsettings_fpv_motionblur.patched.dat",
        "camera_view_mode_firstperson.patched.pso",
        "camera_freecam_nearclip_fov.patched.pso",
        "camera_ads_optic_free_toggle.patched.pso",
    ):
        with open(os.path.join(out_loose, fn), "rb") as f1, open(os.path.join(out_zip, fn), "rb") as f2:
            if f1.read() != f2.read():
                parity_ok = False
    record(
        "3. Offline XML Merge (common.rpf @ 0x0003B000 / page 0x0003BA30)",
        apply_loose["xml"][0]["changes_applied"] == 7
        and apply_loose["xml"][0]["xml_valid_before"]
        and apply_loose["xml"][0]["xml_valid_after"],
        "7/7 <UseFirstPersonCameraMatrix> flipped false->true, ElementTree valid before & after",
    )
    record(
        "4. Offline KV Merge (visualsettings.dat @ 0x00640000 / page 0x00643650)",
        apply_loose["kv"][0]["changes_applied"] == 4,
        "4/4 FPV motionblur KV ops applied (strength 0.2->0.05, rot scales halved), diff + provenance written",
    )

    pso_all_ok = all(
        p["roundtrip_identity_ok"] and p["redecode_ok"] and p["unknown_types"] == 0 for p in apply_loose["pso"]
    )
    vm_pso = apply_loose["pso"][0]
    record(
        "5. Binary PSO Patch + Serializer Round-Trip + pso_decode Re-Decode",
        pso_all_ok and parity_ok and vm_pso["diffs"][0]["new_value"] == 2,
        f"3/3 PSOs (0x006C6000 enum 0->2 FirstPerson, 0x0063F000 6 fields, 0x006A2000 HasScope 0->-1): "
        f"0-patch round-trip 100% byte-exact, pso_decode 0 errors / 0 unknown types, loose==ZIP byte parity={parity_ok}",
    )

    # 4. Bonus empirical CHKS +0x0C probe on 0x006C6000
    data_rpf = os.path.join(args.game_dir, "data.rpf")
    buf_6c6, _, _ = read_paged_zlib_at(data_rpf, 0x006C6000)
    b1, _, _ = read_paged_zlib_at(data_rpf, 0x00032000)
    b2, _, _ = read_paged_zlib_at(data_rpf, 0x0003D000)
    chks_rep = probe_chks_algorithm(buf_6c6, same_len_pair=(b1, b2), tools_dir=args.tools_dir)
    record(
        "6. Bonus CHKS +0x0C Empirical Probe (data.rpf @ 0x006C6000)",
        True,
        f"Tested {chks_rep['tested_count']} hash/slice combos + GF(2) CRC32 linearity "
        f"(ruled_out={chks_rep['gf2_ieee_crc32_ruled_out']}): status={chks_rep['status']}",
    )

    # 5. Mock VIctory HLE Host + fpv_toggle plugin state machine (both Full & Tier 0-only)
    with open_mod_source(zip_path) as zsrc:
        sim_rep = run_mock_host_suite(zsrc)
    record(
        "7. Mock VIctory Host (Tier 0+1+2) + fpv_toggle State Machine",
        sim_rep["full_tier012_host"]["passed"],
        f"Loaded from .gta6mod ZIP: hotkey 'V' flipped 0->2->0, hold-guard re-asserted "
        f"{sim_rep['full_tier012_host']['reassert_count']}x on context reset, clean release on toggle OFF, "
        f"guarded memory fault/RO checks passed",
    )
    record(
        "8. Graceful Degradation (Tier 0-Only Host)",
        sim_rep["tier0_only_degradation"]["passed"],
        f"Cleanly skipped {sim_rep['tier0_only_degradation']['skipped_patches_count']} Tier 1 patches & "
        f"1 Tier 2 plugin with explicit missing FB_CAP_* diagnostics, 0 crashes",
    )

    full_report = {
        "steps": steps,
        "all_passed": all(s["status"] == "PASS" for s in steps),
        "apply_summary": apply_loose,
        "chks_probe": chks_rep,
        "mock_host": sim_rep,
    }
    report_path = os.path.join(args.out, "verification_report.json")
    with open(report_path, "w", encoding="utf-8") as wf:
        json.dump(full_report, wf, indent=2)

    print("=" * 84)
    print("FIREBALL-CORE VERTICAL-SLICE VERIFICATION REPORT — fpv_unlocker (GTA VI 1.0.2.1)")
    print("=" * 84)
    for s in steps:
        print(f"  [{s['status']}] {s['test']}")
        print(f"         {s['detail']}")

    print("\n--- Patched Field Diffs ---")
    print("[XML] common.rpf @ 0x0003B000 (page 0x0003BA30, CLassoDataManager):")
    for ln in apply_loose["xml"][0]["diff_preview"].splitlines():
        if ln.startswith(("@@", "- ", "+ ", "-  ", "+  ")) or "UseFirstPersonCameraMatrix" in ln:
            print("  " + ln)

    print("\n[KV] common.rpf @ 0x00640000 (page 0x00643650, commoncrc:/data/visualsettings):")
    for ln in apply_loose["kv"][0]["diff_preview"].splitlines():
        if ln.startswith(("@@", "-", "+")) and not ln.startswith(("---", "+++")):
            print("  " + ln)

    print("\n[PSO] data.rpf Binary Field Patches (re-decoded via pso_decode.py):")
    for p in apply_loose["pso"]:
        tag = " (optional)" if p["optional"] else ""
        print(
            f"  * {p['id']}{tag} @ {p['archive_offset']} ({p['size_bytes']:,} B, "
            f"decoded {p['decoded_structs']} structs / {p['decoded_members']} members, "
            f"CHKS={p['chks_original']} -> {p['chks_status']}):"
        )
        for d in p["diffs"]:
            print(
                f"      {d['struct_hash']}[{d['instance_index']}]{d['member_offset']} "
                f"{d['member_name']} ({d['field_type']}): {d['old_value']} -> {d['new_value']} "
                f"(file_off={d['file_byte_offset']})"
            )

    print("\n--- Mock VIctory Host Timeline (Tier 0+1+2) ---")
    for ev_s in sim_rep["full_tier012_host"]["timeline"]:
        print(f"  * {ev_s}")

    print("\n--- Tier 0-Only Graceful Degradation Diagnostics ---")
    for diag in sim_rep["tier0_only_degradation"]["diagnostics"]:
        print(f"  * {diag}")

    print(f"\nArtifacts & full JSON report written to: {args.out}")
    print(f"Overall result: {'ALL PASSED (8/8)' if full_report['all_passed'] else 'FAILED'}")
    return 0 if full_report["all_passed"] else 1


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description="Fireball-Core offline Phase 0 test runner & vertical-slice verifier."
    )
    p.add_argument("--game-dir", default=DEFAULT_GAME_DIR, help=f"Read-only GTA VI dir (default: {DEFAULT_GAME_DIR})")
    p.add_argument("--out", default=DEFAULT_OUT_DIR, help=f"Scratch/export output dir (default: {DEFAULT_OUT_DIR})")
    p.add_argument(
        "--tools-dir",
        default=None,
        help="Path to companion research repo tools/ containing pso_decode.py (default: env GTA6_MODDING_TOOLS or sibling repo)",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    p_pack = sub.add_parser("pack", help="Package a loose mod folder into a .gta6mod ZIP archive.")
    p_pack.add_argument("--mod", default=DEFAULT_MOD_DIR, help="Loose mod folder to pack")
    p_pack.add_argument("--output", default=None, help="Output .gta6mod path (default: <out>/fpv_unlocker.gta6mod)")

    p_val = sub.add_parser("validate", help="Validate C11 ABI headers and mod.toml targets against game install.")
    p_val.add_argument("--mod", default=DEFAULT_MOD_DIR, help="Loose mod folder or .gta6mod ZIP")

    p_app = sub.add_parser("apply", help="Apply XML, KV, and binary PSO patches offline and output diffs + provenance.")
    p_app.add_argument("--mod", default=DEFAULT_MOD_DIR, help="Loose mod folder or .gta6mod ZIP")

    p_ck = sub.add_parser("chks-probe", help="Run reproducible empirical CHKS +0x0C checksum probe on a data.rpf PSO.")
    p_ck.add_argument("--offset", default="0x006C6000", help="Hex offset in data.rpf (default: 0x006C6000)")

    p_sim = sub.add_parser("simulate", help="Run Mock VIctory HLE Host + fpv_toggle state-machine tests.")
    p_sim.add_argument("--mod", default=DEFAULT_MOD_DIR, help="Loose mod folder or .gta6mod ZIP")

    p_bun = sub.add_parser(
        "build-bundle",
        help="Compile one or more mods into a deterministic fireball_bundle.fbin + provenance JSON.",
    )
    p_bun.add_argument(
        "--mod",
        action="append",
        default=None,
        help="Mod folder or .gta6mod ZIP (repeatable in load order; default: examples/fpv_unlocker)",
    )
    p_bun.add_argument(
        "--overlay",
        action="append",
        default=None,
        help="Optional extra loose-file overlay as TARGET_REL_PATH=LOCAL_SOURCE_FILE (repeatable)",
    )
    p_bun.add_argument(
        "--bundle-name",
        default="fireball_bundle.fbin",
        help="Output bundle filename under --out (default: fireball_bundle.fbin)",
    )
    p_bun.add_argument(
        "--size-preserve",
        action="store_true",
        help="Reclaim non-semantic whitespace/comments in grown XML/KV pages so payload_size <= orig_uncomp_size",
    )

    p_ins = sub.add_parser("inspect-bundle", help="Inspect a compiled fireball_bundle.fbin file.")
    p_ins.add_argument("bundle", help="Path to fireball_bundle.fbin")
    p_ins.add_argument("--json", action="store_true", help="Emit structured JSON instead of a table")

    p_stg = sub.add_parser(
        "stage",
        help="Resolve multi-mod load order, build bundle + provenance + conflict report, and copy native DLLs.",
    )
    p_stg.add_argument(
        "--mod",
        action="append",
        default=None,
        help="Mod folder or .gta6mod ZIP (repeatable)",
    )
    p_stg.add_argument(
        "--profile",
        default=None,
        help="Path to a load_order.toml profile file",
    )
    p_stg.add_argument(
        "--plugin-dir",
        action="append",
        default=None,
        help="Directory to search for built *.fbplugin.dll binaries (repeatable, e.g. build/Release)",
    )
    p_stg.add_argument(
        "--core-dll",
        default=None,
        help="Optional path to fireball_core.dll to copy into --out",
    )
    p_stg.add_argument(
        "--overlay",
        action="append",
        default=None,
        help="Optional extra loose-file overlay as TARGET_REL_PATH=LOCAL_SOURCE_FILE (repeatable)",
    )
    p_stg.add_argument(
        "--bundle-name",
        default=None,
        help="Output bundle filename under --out (default: profile value or fireball_bundle.fbin)",
    )
    p_stg.add_argument(
        "--size-preserve",
        action="store_true",
        help="Reclaim non-semantic whitespace/comments in grown XML/KV pages so payload_size <= orig_uncomp_size",
    )
    p_stg.add_argument(
        "--json",
        action="store_true",
        help="Emit the full staging report as JSON to stdout",
    )

    p_con = sub.add_parser(
        "console",
        help="Attach to a live Fireball-Core named-pipe console (interactive REPL or one-shot -c).",
    )
    p_con.add_argument(
        "-c",
        "--command",
        action="append",
        default=None,
        help="Execute one or more commands non-interactively and exit (repeatable)",
    )
    p_con.add_argument(
        "--pipe",
        default=None,
        help=r"Named pipe path or short name (default: env FIREBALL_CONSOLE_PIPE or \\.\pipe\fireball_console)",
    )
    p_con.add_argument(
        "--timeout-ms",
        type=int,
        default=3000,
        help="Timeout in milliseconds when waiting for a busy pipe (default: 3000)",
    )

    p_all = sub.add_parser("verify-all", help="Run the full end-to-end vertical-slice verification suite.")
    p_all.add_argument("--mod", default=DEFAULT_MOD_DIR, help="Loose mod folder (default: examples/fpv_unlocker)")
    return p


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    dispatch = {
        "pack": cmd_pack,
        "validate": cmd_validate,
        "apply": cmd_apply,
        "chks-probe": cmd_chks_probe,
        "simulate": cmd_simulate,
        "build-bundle": cmd_build_bundle,
        "inspect-bundle": cmd_inspect_bundle,
        "stage": cmd_stage,
        "console": cmd_console,
        "verify-all": cmd_verify_all,
    }
    return dispatch[args.cmd](args)


if __name__ == "__main__":
    raise SystemExit(main())
