"""Offline unit tests for Fireball-Core (no game dump or research repo required)."""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

import fireball_cli  # noqa: E402
from fireball.abi import verify_c11_headers  # noqa: E402
from fireball.mergers import merge_kv_bytes, merge_xml_bytes  # noqa: E402
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
            self.assertEqual(info["file_count"], 7)

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


if __name__ == "__main__":
    unittest.main()
