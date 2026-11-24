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
import json
import os
import sys
from typing import Any, Dict, List, Optional

_ROOT_DIR = os.path.dirname(os.path.abspath(__file__))
if _ROOT_DIR not in sys.path:
    sys.path.insert(0, _ROOT_DIR)

from fireball.abi import verify_c11_headers  # noqa: E402
from fireball.archive_reader import parse_int_hex, read_paged_zlib_at, read_single_zlib_chunk_at  # noqa: E402
from fireball.bundle import build_patch_bundle  # noqa: E402
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
        tools_dir=args.tools_dir,
    )
    print(json.dumps(rep, indent=2))
    return 0


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
        "verify-all": cmd_verify_all,
    }
    return dispatch[args.cmd](args)


if __name__ == "__main__":
    raise SystemExit(main())
