"""Offline unit tests for Fireball-Core (no game dump or research repo required)."""

from __future__ import annotations

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

import fireball_cli  # noqa: E402
from fireball.abi import verify_c11_headers  # noqa: E402
from fireball.bundle import (  # noqa: E402
    FB_BUNDLE_MAGIC,
    FB_BUNDLE_VERSION,
    FB_BUNDLE_VERSION_V2,
    FB_DS_FLAG_FULL_ASSET,
    FB_DS_FLAG_PAGE_SLICE,
    FB_DS_FLAG_TEXT_PATCH,
    BundleDStorageEntry,
    BundleOverlayEntry,
    BundlePluginConfigEntry,
    BundleShaderEntry,
    CompiledBundle,
    build_patch_bundle,
    dstorage_key_hash,
    inspect_bundle_file,
    normalize_rel_path,
    parse_bundle,
    serialize_bundle,
    shader_key_hash,
)
from fireball.load_order import (  # noqa: E402
    DependencyCycleError,
    DuplicateModError,
    MissingDependencyError,
    ModConflictError,
    resolve_load_order,
)
from fireball.mergers import (  # noqa: E402
    compact_kv_bytes,
    compact_xml_slice,
    merge_kv_bytes,
    merge_xml_bytes,
    parse_kv_pairs,
)
from fireball.mock_host import run_mock_host_suite  # noqa: E402
from fireball.mod_source import load_manifest, open_mod_source, pack_mod_to_zip  # noqa: E402


class TestFireballOffline(unittest.TestCase):
    def test_c11_headers_contract(self) -> None:
        ok, errs = verify_c11_headers(os.path.join(ROOT, "include"))
        self.assertTrue(ok, f"Header check failed: {errs}")

    def test_cli_help_without_game_or_research_repo(self) -> None:
        env = dict(os.environ)
        env["GTA6_DIR"] = os.path.join(ROOT, "__nonexistent_game__")
        env["GTA6_MODDING_TOOLS"] = os.path.join(ROOT, "__nonexistent_tools__")
        proc = subprocess.run(
            [sys.executable, "-B", os.path.join(ROOT, "fireball_cli.py"), "--help"],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Fireball-Core", proc.stdout)

    def test_refuses_output_inside_game_dir(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            game_dir = os.path.join(td, "GTA6")
            nested = os.path.join(game_dir, "out")
            with self.assertRaises(ValueError):
                fireball_cli._check_out_not_in_game_dir(nested, game_dir)
            # Sibling dir is allowed
            fireball_cli._check_out_not_in_game_dir(os.path.join(td, "GTA6-work"), game_dir)

    def test_loose_and_zip_mod_parity_and_mock_host(self) -> None:
        mod_dir = os.path.join(ROOT, "examples", "fpv_unlocker")
        with tempfile.TemporaryDirectory() as td:
            zip_path = os.path.join(td, "fpv_unlocker.gta6mod")
            info = pack_mod_to_zip(mod_dir, zip_path)
            self.assertEqual(info["file_count"], 8)

            with open_mod_source(mod_dir) as s_loose, open_mod_source(zip_path) as s_zip:
                m_loose = load_manifest(s_loose)
                m_zip = load_manifest(s_zip)
                self.assertEqual(m_loose.mod_id, m_zip.mod_id)
                self.assertEqual(m_loose.min_fireball_abi, 1)
                self.assertEqual(s_loose.list_files(), s_zip.list_files())

                rep = run_mock_host_suite(s_zip)
                self.assertTrue(rep["full_tier012_host"]["passed"])
                self.assertTrue(rep["tier0_only_degradation"]["passed"])
                self.assertEqual(rep["tier0_only_degradation"]["skipped_patches_count"], 5)

    def test_xml_and_kv_mergers_preserve_bom_and_format(self) -> None:
        xml_in = b'\xef\xbb\xbf<Root>\n  <UseFirstPersonCameraMatrix value="false" />\n</Root>\n'
        xml_res = merge_xml_bytes(
            xml_in,
            [
                {
                    "op": "set_attr",
                    "tag": "UseFirstPersonCameraMatrix",
                    "attr": "value",
                    "expected_old": "false",
                    "value": "true",
                    "expected_matches": 1,
                }
            ],
            mod_id="test.mod",
            patch_id="p_xml",
            target="common:/test.meta",
        )
        self.assertTrue(xml_res.had_bom)
        self.assertEqual(xml_res.changes_applied, 1)
        self.assertTrue(xml_res.patched_bytes.startswith(b"\xef\xbb\xbf"))
        self.assertIn(b'value="true"', xml_res.patched_bytes)

        kv_in = b"# comment\nmotionblur.fpv.MBLowCameraRotSpeedScale 0.75\ndefaultmotionblur.fpv.strength\t\t0.2\n"
        kv_res = merge_kv_bytes(
            kv_in,
            [
                {"op": "scale", "key": "motionblur.fpv.MBLowCameraRotSpeedScale", "factor": 0.5},
                {"op": "set", "key": "defaultmotionblur.fpv.strength", "value": "0.05"},
            ],
            mod_id="test.mod",
            patch_id="p_kv",
            target="commoncrc:/data/visualsettings",
        )
        self.assertEqual(kv_res.changes_applied, 2)
        self.assertIn(b"motionblur.fpv.MBLowCameraRotSpeedScale 0.375", kv_res.patched_bytes)
        self.assertIn(b"defaultmotionblur.fpv.strength\t\t0.05", kv_res.patched_bytes)

    def test_bundle_serialize_parse_determinism_and_normalization(self) -> None:
        self.assertEqual(normalize_rel_path(r".\Pack\Story000\Story000.RPF"), "pack/story000/story000.rpf")
        with self.assertRaises(ValueError):
            normalize_rel_path("../escape.rpf")

        e1 = BundleDStorageEntry(
            archive_path="DATA.RPF",
            file_offset=0x006C6000,
            orig_uncomp_size=16,
            payload=b"PSIN0123456789ab",
            flags=FB_DS_FLAG_FULL_ASSET | FB_DS_FLAG_PAGE_SLICE,
        )
        e2 = BundleDStorageEntry(
            archive_path="common.rpf",
            file_offset=0x0003BA30,
            orig_uncomp_size=100,
            payload=b"<Root/>",
            flags=FB_DS_FLAG_PAGE_SLICE | FB_DS_FLAG_TEXT_PATCH,
        )
        ov = BundleOverlayEntry(rel_path=r"Data\Loose\Test.txt", payload=b"hello overlay")
        cfg = BundlePluginConfigEntry(mod_id="community.fpv_unlocker", config_json='{"hotkey_vkey":86}')

        # Order of input lists must not affect binary output (deterministic sorting)
        b_a = serialize_bundle(CompiledBundle([e1, e2], [ov], [cfg]))
        b_b = serialize_bundle(CompiledBundle([e2, e1], [ov], [cfg]))
        self.assertEqual(b_a, b_b)
        magic, ver = struct.unpack_from("<II", b_a, 0)
        self.assertEqual(magic, FB_BUNDLE_MAGIC)
        self.assertEqual(ver, FB_BUNDLE_VERSION)

        parsed = parse_bundle(b_a)
        self.assertEqual(len(parsed.dstorage_entries), 2)
        self.assertEqual(parsed.dstorage_entries[0].archive_path, "common.rpf")
        self.assertEqual(parsed.dstorage_entries[1].archive_path, "data.rpf")
        self.assertEqual(parsed.dstorage_entries[1].key_hash, dstorage_key_hash("data.rpf", 0x006C6000, 16))
        self.assertEqual(parsed.overlay_entries[0].rel_path, "data/loose/test.txt")
        self.assertEqual(parsed.overlay_entries[0].payload, b"hello overlay")
        self.assertEqual(parsed.plugin_configs[0].mod_id, "community.fpv_unlocker")

        # Corrupted payload must fail CRC32 check
        corrupted = bytearray(b_a)
        corrupted[-16] ^= 0xFF
        with self.assertRaises(ValueError):
            parse_bundle(bytes(corrupted))

    def test_build_bundle_multimod_load_order_and_multipage_slices(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            game_dir = os.path.join(td, "game")
            out_dir = os.path.join(td, "out")
            os.makedirs(game_dir, exist_ok=True)

            # Build a synthetic 2-page (64 KiB + tail) 78 DA zlib stream in fake common.rpf
            page0 = b"<!-- " + (b"A" * (65536 - 10)) + b" -->\n"
            self.assertEqual(len(page0), 65536)
            page1 = (
                b'<Root>\n'
                b'  <UseFirstPersonCameraMatrix value="false" />\n'
                b'  <ExtraFlag value="off" />\n'
                b'</Root>\n'
            )
            c0 = zlib.compress(page0, level=9)
            c1 = zlib.compress(page1, level=9)
            self.assertEqual(c0[:2], b"\x78\xda")
            self.assertEqual(c1[:2], b"\x78\xda")

            # Align page1 to 16-byte boundary after page0 at offset 0x100
            arch = bytearray(0x100)
            arch.extend(c0)
            while len(arch) % 16 != 0:
                arch.append(0)
            page1_off = len(arch)
            arch.extend(c1)
            with open(os.path.join(game_dir, "common.rpf"), "wb") as wf:
                wf.write(arch)

            # Mod A patches UseFirstPersonCameraMatrix and provides an overlay + plugin config
            mod_a = os.path.join(td, "mod_a")
            os.makedirs(os.path.join(mod_a, "patches"), exist_ok=True)
            os.makedirs(os.path.join(mod_a, "overlays", "data"), exist_ok=True)
            with open(os.path.join(mod_a, "mod.toml"), "w", encoding="utf-8") as wf:
                wf.write(
                    f'[mod]\nid = "test.mod_a"\nname = "Mod A"\nversion = "1.0.0"\n'
                    f'target_game_version = "1.0.2.1"\nmin_fireball_abi = 1\n'
                    f'[[patches.xml]]\nid = "xml_a"\ntarget = "common:/test.meta"\narchive = "common.rpf"\n'
                    f'archive_offset = "0x100"\npage_offset = "0x{page1_off:X}"\npatch = "patches/a.toml"\n'
                    f'[[plugins]]\nid = "demo"\nentry = "none.py"\n[plugins.config]\nhotkey_vkey = 86\n'
                )
            with open(os.path.join(mod_a, "patches", "a.toml"), "w", encoding="utf-8") as wf:
                wf.write(
                    '[[ops]]\nop = "set_attr"\ntag = "UseFirstPersonCameraMatrix"\nattr = "value"\n'
                    'expected_old = "false"\nvalue = "true"\nexpected_matches = 1\n'
                )
            with open(os.path.join(mod_a, "overlays", "data", "hello.txt"), "wb") as wf:
                wf.write(b"from mod A")

            # Mod B loads after Mod A, patches ExtraFlag in the same XML, and overrides the overlay
            mod_b_dir = os.path.join(td, "mod_b_src")
            os.makedirs(os.path.join(mod_b_dir, "patches"), exist_ok=True)
            os.makedirs(os.path.join(mod_b_dir, "overlays", "data"), exist_ok=True)
            with open(os.path.join(mod_b_dir, "mod.toml"), "w", encoding="utf-8") as wf:
                wf.write(
                    f'[mod]\nid = "test.mod_b"\nname = "Mod B"\nversion = "1.0.0"\n'
                    f'target_game_version = "1.0.2.1"\nmin_fireball_abi = 1\n'
                    f'[[patches.xml]]\nid = "xml_b"\ntarget = "common:/test.meta"\narchive = "common.rpf"\n'
                    f'archive_offset = "0x100"\npage_offset = "0x{page1_off:X}"\npatch = "patches/b.toml"\n'
                )
            with open(os.path.join(mod_b_dir, "patches", "b.toml"), "w", encoding="utf-8") as wf:
                wf.write(
                    '[[ops]]\nop = "set_attr"\ntag = "ExtraFlag"\nattr = "value"\n'
                    'expected_old = "off"\nvalue = "on"\nexpected_matches = 1\n'
                )
            with open(os.path.join(mod_b_dir, "overlays", "data", "hello.txt"), "wb") as wf:
                wf.write(b"from mod B")
            mod_b_zip = os.path.join(td, "mod_b.gta6mod")
            pack_mod_to_zip(mod_b_dir, mod_b_zip)

            rep = build_patch_bundle([mod_a, mod_b_zip], game_dir, out_dir)
            self.assertEqual(rep["dstorage_entry_count"], 2)  # full asset + modified page[1]
            self.assertEqual(rep["overlay_entry_count"], 1)
            self.assertEqual(rep["plugin_config_count"], 1)
            self.assertTrue(os.path.isfile(rep["bundle_file"]))
            self.assertTrue(os.path.isfile(rep["provenance_file"]))

            with open(rep["bundle_file"], "rb") as rf:
                parsed = parse_bundle(rf.read())
            by_off = {e.file_offset: e for e in parsed.dstorage_entries}
            self.assertIn(0x100, by_off)
            self.assertIn(page1_off, by_off)
            # Both Mod A and Mod B edits must appear in both full asset and page[1] slice
            for e in (by_off[0x100], by_off[page1_off]):
                self.assertIn(b'UseFirstPersonCameraMatrix value="true"', e.payload)
                self.assertIn(b'ExtraFlag value="on"', e.payload)
            self.assertEqual(by_off[page1_off].orig_uncomp_size, len(page1))
            self.assertEqual(parsed.overlay_entries[0].payload, b"from mod B")

            with open(rep["provenance_file"], "r", encoding="utf-8") as rf:
                prov = json.load(rf)
            self.assertEqual(prov["total_field_changes"], 2)

    def test_size_preserve_kv_xml_and_inspect_bundle(self) -> None:
        # 1. KV compaction reclaims repeated tabs while preserving 100% (key, value) parity
        kv_orig = (
            b"// comment header\r\n"
            b"defaultmotionblur.fpv.strength\t\t0.2\r\n"
            b"motionblur.fpv.MBLowCameraRotSpeedScale\t\t0.75\r\n"
            b"motionblur.fpv.MBHighCameraRotSpeedScale\t\t0.3\r\n"
        )
        kv_res = merge_kv_bytes(
            kv_orig,
            [
                {"op": "set", "key": "defaultmotionblur.fpv.strength", "value": "0.05"},
                {"op": "scale", "key": "motionblur.fpv.MBLowCameraRotSpeedScale", "factor": 0.5},
                {"op": "scale", "key": "motionblur.fpv.MBHighCameraRotSpeedScale", "factor": 0.5},
            ],
            mod_id="test.sp",
            patch_id="kv_sp",
            target="commoncrc:/data/visualsettings",
        )
        self.assertGreater(len(kv_res.patched_bytes), len(kv_orig))
        compacted_kv, reclaimed = compact_kv_bytes(kv_res.patched_bytes, len(kv_orig))
        self.assertLessEqual(len(compacted_kv), len(kv_orig))
        self.assertEqual(reclaimed, len(kv_res.patched_bytes) - len(compacted_kv))
        self.assertEqual(parse_kv_pairs(compacted_kv), parse_kv_pairs(kv_res.patched_bytes))

        # 2. XML compaction reclaims indentation & comments while preserving exact DOM
        xml_orig = b"<Root>\n  <!-- optional comment -->\n        <Flag value=\"no\" />\n        <Other value=\"1\" />\n</Root>\n"
        xml_grown = b"<Root>\n  <!-- optional comment -->\n        <Flag value=\"yes_longer_value\" />\n        <Other value=\"1\" />\n</Root>\n"
        compacted_xml, xml_rec = compact_xml_slice(xml_grown, len(xml_orig))
        self.assertLessEqual(len(compacted_xml), len(xml_orig))
        self.assertGreater(xml_rec, 0)
        self.assertIn(b'value="yes_longer_value"', compacted_xml)
        # Impossible compaction target raises cleanly instead of silently overflowing
        with self.assertRaises(ValueError):
            compact_xml_slice(xml_grown, 10)

        # 3. End-to-end build-bundle --size-preserve + inspect-bundle CLI on synthetic KV archive
        with tempfile.TemporaryDirectory() as td:
            game_dir = os.path.join(td, "game")
            out_dir = os.path.join(td, "out")
            mod_dir = os.path.join(td, "mod")
            os.makedirs(game_dir, exist_ok=True)
            os.makedirs(os.path.join(mod_dir, "patches"), exist_ok=True)

            comp = zlib.compress(kv_orig, level=9)
            self.assertEqual(comp[:2], b"\x78\xda")
            with open(os.path.join(game_dir, "common.rpf"), "wb") as wf:
                wf.write(b"\x00" * 0x100 + comp)

            with open(os.path.join(mod_dir, "mod.toml"), "w", encoding="utf-8") as wf:
                wf.write(
                    '[mod]\nid = "test.sp"\nname = "SP"\nversion = "1.0.0"\n'
                    'target_game_version = "1.0.2.1"\nmin_fireball_abi = 1\n'
                    '[[patches.kv]]\nid = "kv_sp"\ntarget = "commoncrc:/data/visualsettings"\n'
                    'archive = "common.rpf"\narchive_offset = "0x100"\npatch = "patches/kv.toml"\n'
                )
            with open(os.path.join(mod_dir, "patches", "kv.toml"), "w", encoding="utf-8") as wf:
                wf.write(
                    '[[ops]]\nop = "set"\nkey = "defaultmotionblur.fpv.strength"\nvalue = "0.05"\n'
                    '[[ops]]\nop = "scale"\nkey = "motionblur.fpv.MBLowCameraRotSpeedScale"\nfactor = 0.5\n'
                    '[[ops]]\nop = "scale"\nkey = "motionblur.fpv.MBHighCameraRotSpeedScale"\nfactor = 0.5\n'
                )

            rep = build_patch_bundle([mod_dir], game_dir, out_dir, size_preserve=True)
            self.assertTrue(rep["all_fit_fixed_dst"])
            self.assertGreater(rep["size_preserve_reclaimed_bytes"], 0)

            insp = inspect_bundle_file(rep["bundle_file"])
            self.assertTrue(insp["all_fit_fixed_dst"])
            self.assertEqual(insp["dstorage_entry_count"], 1)
            self.assertLessEqual(insp["dstorage_entries"][0]["delta"], 0)

            proc = subprocess.run(
                [sys.executable, "-B", os.path.join(ROOT, "fireball_cli.py"), "inspect-bundle", rep["bundle_file"]],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("all_fit_fixed_dst=True", proc.stdout)
            self.assertIn("common.rpf", proc.stdout)

    def test_console_cli_help_and_pipe_normalization(self) -> None:
        self.assertEqual(fireball_cli._normalize_pipe_name("my_pipe"), r"\\.\pipe\my_pipe")
        self.assertEqual(
            fireball_cli._normalize_pipe_name(r"\\.\pipe\custom_pipe"),
            r"\\.\pipe\custom_pipe",
        )
        proc = subprocess.run(
            [sys.executable, "-B", os.path.join(ROOT, "fireball_cli.py"), "console", "--help"],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("--pipe", proc.stdout)
        self.assertIn("--command", proc.stdout)

    def test_v2_shader_bundle_roundtrip_determinism_and_v1_compat(self) -> None:
        b3_a = bytes(range(32))
        dxbc_a = bytes(range(0xA0, 0xB0))
        b3_b = bytes(range(32, 64))
        dxbc_b = bytes(range(0x10, 0x20))

        sh_a = BundleShaderEntry(
            container_blake3=b3_a,
            dxbc_digest=dxbc_a,
            payload=b"DXBC\x01\x02\x03\x04DXIL_PAYLOAD_A",
            effect_name="fx_visor_ps",
        )
        sh_b = BundleShaderEntry(
            container_blake3=b3_b,
            dxbc_digest=dxbc_b,
            payload=b"DXBC\x05\x06\x07\x08DXIL_PAYLOAD_B",
            effect_name="",
        )

        raw1 = serialize_bundle(CompiledBundle(shader_entries=[sh_a, sh_b]))
        raw2 = serialize_bundle(CompiledBundle(shader_entries=[sh_b, sh_a]))
        self.assertEqual(raw1, raw2)

        magic, ver = struct.unpack_from("<II", raw1, 0)
        self.assertEqual(magic, FB_BUNDLE_MAGIC)
        self.assertEqual(ver, FB_BUNDLE_VERSION_V2)

        parsed = parse_bundle(raw1)
        self.assertEqual(parsed.bundle_version, FB_BUNDLE_VERSION_V2)
        self.assertEqual(len(parsed.shader_entries), 2)
        self.assertEqual(parsed.shader_entries[0].container_blake3, b3_a)
        self.assertEqual(parsed.shader_entries[0].dxbc_digest, dxbc_a)
        self.assertEqual(parsed.shader_entries[0].effect_name, "fx_visor_ps")
        self.assertEqual(parsed.shader_entries[0].payload, sh_a.payload)
        self.assertEqual(parsed.shader_entries[0].key_hash, shader_key_hash(b3_a, dxbc_a))
        self.assertEqual(parsed.shader_entries[1].effect_name, "")

        # Corrupting shader DXIL payload must fail CRC32 validation
        corrupted = bytearray(raw1)
        corrupted[-16] ^= 0xFF
        with self.assertRaises(ValueError):
            parse_bundle(bytes(corrupted))

        # Inspect CLI reports v2 and shader entries cleanly
        with tempfile.TemporaryDirectory() as td:
            bpath = os.path.join(td, "v2.fbin")
            with open(bpath, "wb") as wf:
                wf.write(raw1)
            insp = inspect_bundle_file(bpath)
            self.assertEqual(insp["bundle_version"], FB_BUNDLE_VERSION_V2)
            self.assertEqual(insp["shader_entry_count"], 2)
            self.assertEqual(insp["shader_entries"][0]["effect_name"], "fx_visor_ps")

            proc = subprocess.run(
                [sys.executable, "-B", os.path.join(ROOT, "fireball_cli.py"), "inspect-bundle", bpath],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("(v2,", proc.stdout)
            self.assertIn("Shader DXIL overrides (2):", proc.stdout)
            self.assertIn("fx_visor_ps", proc.stdout)

    def _write_minimal_mod(
        self,
        parent: str,
        dirname: str,
        mod_id: str,
        *,
        requires: list[str] | None = None,
        load_after: list[str] | None = None,
        conflicts: list[str] | None = None,
        extra_toml: str = "",
    ) -> str:
        mdir = os.path.join(parent, dirname)
        os.makedirs(mdir, exist_ok=True)
        req_s = json.dumps(requires or [])
        aft_s = json.dumps(load_after or [])
        cnf_s = json.dumps(conflicts or [])
        with open(os.path.join(mdir, "mod.toml"), "w", encoding="utf-8") as wf:
            wf.write(
                f'[mod]\nid = "{mod_id}"\nname = "{mod_id}"\nversion = "1.0.0"\n'
                f'target_game_version = "1.0.2.1"\nmin_fireball_abi = 1\n\n'
                f"[dependencies]\nrequires = {req_s}\nload_after = {aft_s}\nconflicts = {cnf_s}\n\n"
                f"{extra_toml}"
            )
        return mdir

    def test_load_order_resolver_topo_sort_errors_and_profiles(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            m_base = self._write_minimal_mod(td, "base", "test.base")
            m_mid = self._write_minimal_mod(
                td, "mid", "test.mid", requires=["test.base"], load_after=["test.missing_soft"]
            )
            m_top = self._write_minimal_mod(
                td, "top", "test.top", requires=["test.mid"], load_after=["test.base"]
            )
            m_top_zip = os.path.join(td, "top.gta6mod")
            pack_mod_to_zip(m_top, m_top_zip)

            # Deterministic topological sort regardless of scrambled input order (loose + .gta6mod)
            res1 = resolve_load_order([m_top_zip, m_mid, m_base])
            res2 = resolve_load_order([m_mid, m_base, m_top_zip])
            self.assertEqual([m.mod_id for m in res1.ordered], ["test.base", "test.mid", "test.top"])
            self.assertEqual([m.mod_id for m in res2.ordered], ["test.base", "test.mid", "test.top"])

            # Missing dependency error
            with self.assertRaises(MissingDependencyError) as ctx_miss:
                resolve_load_order([m_mid])
            self.assertIn("test.base", str(ctx_miss.exception))

            # Duplicate mod id error
            m_base_dup = self._write_minimal_mod(td, "base_dup", "test.base")
            with self.assertRaises(DuplicateModError) as ctx_dup:
                resolve_load_order([m_base, m_base_dup])
            self.assertIn("test.base", str(ctx_dup.exception))

            # Declared conflict error
            m_conf = self._write_minimal_mod(td, "conf", "test.conf", conflicts=["test.base"])
            with self.assertRaises(ModConflictError) as ctx_conf:
                resolve_load_order([m_base, m_conf])
            self.assertIn("test.conf", str(ctx_conf.exception))
            self.assertIn("test.base", str(ctx_conf.exception))

            # Cycle error (a -> b -> a)
            m_c1 = self._write_minimal_mod(td, "cyc1", "test.cyc1", requires=["test.cyc2"])
            m_c2 = self._write_minimal_mod(td, "cyc2", "test.cyc2", load_after=["test.cyc1"])
            with self.assertRaises(DependencyCycleError) as ctx_cyc:
                resolve_load_order([m_c1, m_c2])
            self.assertIn("test.cyc1", str(ctx_cyc.exception))
            self.assertIn("test.cyc2", str(ctx_cyc.exception))

            # load_order.toml profile: explicit ordering, enable/disable, and overrides
            m_x = self._write_minimal_mod(td, "mod_x", "test.x")
            m_y = self._write_minimal_mod(td, "mod_y", "test.y")
            m_z = self._write_minimal_mod(td, "mod_z", "test.z")
            prof_path = os.path.join(td, "load_order.toml")
            with open(prof_path, "w", encoding="utf-8") as wf:
                wf.write(
                    '[profile]\n'
                    'name = "custom"\n'
                    'size_preserve = true\n'
                    'order = ["test.y", "test.x"]\n\n'
                    '[[mods]]\npath = "mod_x"\nenabled = true\n'
                    '[mods.overrides]\npatches = { p1 = false }\nplugin_config = { k = 1 }\n\n'
                    '[[mods]]\npath = "mod_y"\nenabled = true\n\n'
                    '[[mods]]\npath = "mod_z"\nenabled = false\n\n'
                    '[overrides."test.x".plugin_config]\n'
                    'k = 2\nextra = true\n'
                )
            pres = resolve_load_order(profile_path=prof_path)
            self.assertEqual([m.mod_id for m in pres.ordered], ["test.y", "test.x"])
            self.assertEqual(len(pres.disabled), 1)
            self.assertEqual(pres.disabled[0]["mod_id"], "test.z")
            self.assertTrue(pres.size_preserve)
            self.assertEqual(pres.mod_overrides["test.x"]["patches"], {"p1": False})
            self.assertEqual(pres.mod_overrides["test.x"]["plugin_config"], {"extra": True, "k": 2})

    def test_stage_cli_conflicts_plugins_shaders_and_profile_overrides(self) -> None:
        # 1. `stage --help` works without game dir
        env = dict(os.environ)
        env["GTA6_DIR"] = os.path.join(ROOT, "__nonexistent_game__")
        hproc = subprocess.run(
            [sys.executable, "-B", os.path.join(ROOT, "fireball_cli.py"), "stage", "--help"],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
        self.assertEqual(hproc.returncode, 0, hproc.stderr)
        self.assertIn("--profile", hproc.stdout)
        self.assertIn("--plugin-dir", hproc.stdout)
        self.assertIn("--core-dll", hproc.stdout)

        # 2. Full end-to-end stage with XML + KV + overlay + shader conflicts, plugin copy, core DLL copy
        with tempfile.TemporaryDirectory() as td:
            game_dir = os.path.join(td, "game")
            out_dir = os.path.join(td, "staged")
            pdir = os.path.join(td, "bin_release")
            os.makedirs(game_dir, exist_ok=True)
            os.makedirs(pdir, exist_ok=True)

            # Synthetic game archive containing both an XML at 0x100 and a KV at 0x800
            xml_raw = b'<Root>\n  <Flag value="0" />\n  <Other value="orig" />\n</Root>\n'
            kv_raw = b"visual.alpha\t\t1.0\nvisual.beta\t\t2.0\n"
            c_xml = zlib.compress(xml_raw, level=9)
            c_kv = zlib.compress(kv_raw, level=9)
            arch = bytearray(0x100)
            arch.extend(c_xml)
            while len(arch) < 0x800:
                arch.append(0)
            arch.extend(c_kv)
            with open(os.path.join(game_dir, "common.rpf"), "wb") as wf:
                wf.write(arch)

            # Fake built plugin DLL and fake fireball_core.dll in pdir
            with open(os.path.join(pdir, "mod_b.fbplugin.dll"), "wb") as wf:
                wf.write(b"MZ_FAKE_PLUGIN_B")
            with open(os.path.join(pdir, "unused_other.fbplugin.dll"), "wb") as wf:
                wf.write(b"MZ_UNUSED")
            core_src = os.path.join(pdir, "fireball_core.dll")
            with open(core_src, "wb") as wf:
                wf.write(b"MZ_FAKE_CORE")

            b3_hex = bytes(range(32)).hex()
            dxbc_hex = bytes(range(16)).hex()

            # Mod A (loads first): patches Flag=1, visual.alpha=0.5, overlay, shader, embedded plugin
            mod_a = self._write_minimal_mod(
                td,
                "mod_a",
                "test.mod_a",
                extra_toml=(
                    '[[patches.xml]]\nid = "xml_a"\ntarget = "common:/t.meta"\narchive = "common.rpf"\n'
                    'archive_offset = "0x100"\npatch = "patches/x.toml"\n'
                    '[[patches.xml]]\nid = "xml_disabled_by_profile"\ntarget = "common:/t.meta"\narchive = "common.rpf"\n'
                    'archive_offset = "0x100"\npatch = "patches/x_off.toml"\n'
                    '[[patches.kv]]\nid = "kv_a"\ntarget = "commoncrc:/vs"\narchive = "common.rpf"\n'
                    'archive_offset = "0x800"\npatch = "patches/k.toml"\n'
                    '[[patches.shaders]]\nid = "sh_a"\n'
                    f'container_blake3 = "{b3_hex}"\ndxbc_digest = "{dxbc_hex}"\n'
                    'effect_name = "fx_test"\ndxil = "shaders/a.dxil"\n'
                    '[[plugins]]\nid = "mod_a"\nentry = "none.py"\nnative_path = "bin/mod_a.fbplugin.dll"\n'
                    '[plugins.config]\nspeed = 1\n'
                ),
            )
            os.makedirs(os.path.join(mod_a, "patches"), exist_ok=True)
            os.makedirs(os.path.join(mod_a, "overlays", "data"), exist_ok=True)
            os.makedirs(os.path.join(mod_a, "shaders"), exist_ok=True)
            os.makedirs(os.path.join(mod_a, "bin"), exist_ok=True)
            with open(os.path.join(mod_a, "patches", "x.toml"), "w", encoding="utf-8") as wf:
                wf.write('[[ops]]\nop = "set_attr"\ntag = "Flag"\nattr = "value"\nvalue = "1"\n')
            with open(os.path.join(mod_a, "patches", "x_off.toml"), "w", encoding="utf-8") as wf:
                wf.write('[[ops]]\nop = "set_attr"\ntag = "Other"\nattr = "value"\nvalue = "bad"\n')
            with open(os.path.join(mod_a, "patches", "k.toml"), "w", encoding="utf-8") as wf:
                wf.write('[[ops]]\nop = "set"\nkey = "visual.alpha"\nvalue = "0.5"\n')
            with open(os.path.join(mod_a, "overlays", "data", "shared.txt"), "wb") as wf:
                wf.write(b"overlay-A")
            with open(os.path.join(mod_a, "shaders", "a.dxil"), "wb") as wf:
                wf.write(b"DXIL_MOD_A_BYTES")
            with open(os.path.join(mod_a, "bin", "mod_a.fbplugin.dll"), "wb") as wf:
                wf.write(b"MZ_FAKE_PLUGIN_A")

            # Mod B (requires Mod A, overrides Flag=2, visual.alpha=0.25, overlay, shader, plugin from --plugin-dir)
            mod_b = self._write_minimal_mod(
                td,
                "mod_b",
                "test.mod_b",
                requires=["test.mod_a"],
                extra_toml=(
                    '[[patches.xml]]\nid = "xml_b"\ntarget = "common:/t.meta"\narchive = "common.rpf"\n'
                    'archive_offset = "0x100"\npatch = "patches/x.toml"\n'
                    '[[patches.kv]]\nid = "kv_b"\ntarget = "commoncrc:/vs"\narchive = "common.rpf"\n'
                    'archive_offset = "0x800"\npatch = "patches/k.toml"\n'
                    '[[patches.shaders]]\nid = "sh_b"\n'
                    f'container_blake3 = "{b3_hex}"\ndxbc_digest = "{dxbc_hex}"\n'
                    'effect_name = "fx_test"\ndxil = "shaders/b.dxil"\n'
                    '[[plugins]]\nid = "mod_b"\nentry = "none.py"\nnative_path = "bin/mod_b.fbplugin.dll"\n'
                ),
            )
            os.makedirs(os.path.join(mod_b, "patches"), exist_ok=True)
            os.makedirs(os.path.join(mod_b, "overlays", "data"), exist_ok=True)
            os.makedirs(os.path.join(mod_b, "shaders"), exist_ok=True)
            with open(os.path.join(mod_b, "patches", "x.toml"), "w", encoding="utf-8") as wf:
                wf.write('[[ops]]\nop = "set_attr"\ntag = "Flag"\nattr = "value"\nvalue = "2"\n')
            with open(os.path.join(mod_b, "patches", "k.toml"), "w", encoding="utf-8") as wf:
                wf.write('[[ops]]\nop = "set"\nkey = "visual.alpha"\nvalue = "0.25"\n')
            with open(os.path.join(mod_b, "overlays", "data", "shared.txt"), "wb") as wf:
                wf.write(b"overlay-B-winner")
            with open(os.path.join(mod_b, "shaders", "b.dxil"), "wb") as wf:
                wf.write(b"DXIL_MOD_B_WINNER")

            # Profile passes Mod B before Mod A, disables xml_disabled_by_profile, and overrides mod_a plugin config
            prof_path = os.path.join(td, "load_order.toml")
            with open(prof_path, "w", encoding="utf-8") as wf:
                wf.write(
                    '[profile]\nname = "stage_test"\nsize_preserve = true\n\n'
                    '[[mods]]\npath = "mod_b"\n'
                    '[[mods]]\npath = "mod_a"\n\n'
                    '[overrides."test.mod_a".patches]\nxml_disabled_by_profile = false\n'
                    '[overrides."test.mod_a".plugin_config]\nspeed = 9\n'
                )

            proc = subprocess.run(
                [
                    sys.executable,
                    "-B",
                    os.path.join(ROOT, "fireball_cli.py"),
                    "--game-dir",
                    game_dir,
                    "--out",
                    out_dir,
                    "stage",
                    "--profile",
                    prof_path,
                    "--plugin-dir",
                    pdir,
                    "--core-dll",
                    core_src,
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertIn("winner=test.mod_b", proc.stdout)

            # Verify copied binaries (only staged mods' plugins + core DLL, NOT unused_other)
            self.assertTrue(os.path.isfile(os.path.join(out_dir, "fireball_core.dll")))
            self.assertTrue(os.path.isfile(os.path.join(out_dir, "mod_a.fbplugin.dll")))
            self.assertTrue(os.path.isfile(os.path.join(out_dir, "mod_b.fbplugin.dll")))
            self.assertFalse(os.path.isfile(os.path.join(out_dir, "unused_other.fbplugin.dll")))

            # Verify conflicts JSON records all 4 conflicts (xml, kv, overlay, shader) won by test.mod_b
            with open(os.path.join(out_dir, "fireball_conflicts.json"), "r", encoding="utf-8") as rf:
                crep = json.load(rf)
            self.assertEqual(crep["load_order"], ["test.mod_a", "test.mod_b"])
            self.assertEqual(crep["conflict_count"], 4)
            by_kind = {c["kind"]: c for c in crep["conflicts"]}
            self.assertEqual(set(by_kind.keys()), {"xml", "kv", "overlay", "shader"})
            for kind, entry in by_kind.items():
                self.assertEqual(entry["winner_mod"], "test.mod_b", kind)
                self.assertEqual(entry["overridden_mods"], ["test.mod_a"], kind)

            # Verify compiled bundle reflects profile overrides & load-order winners
            with open(os.path.join(out_dir, "fireball_bundle.fbin"), "rb") as rf:
                parsed = parse_bundle(rf.read())
            self.assertEqual(parsed.bundle_version, FB_BUNDLE_VERSION_V2)
            ds_by_off = {e.file_offset: e for e in parsed.dstorage_entries}
            self.assertIn(b'Flag value="2"', ds_by_off[0x100].payload)
            self.assertIn(b'Other value="orig"', ds_by_off[0x100].payload)  # disabled by profile!
            self.assertIn(b"0.25", ds_by_off[0x800].payload)
            self.assertEqual(parsed.overlay_entries[0].payload, b"overlay-B-winner")
            self.assertEqual(parsed.shader_entries[0].payload, b"DXIL_MOD_B_WINNER")
            self.assertEqual(json.loads(parsed.plugin_configs[0].config_json), {"speed": 9})


if __name__ == "__main__":
    unittest.main()
