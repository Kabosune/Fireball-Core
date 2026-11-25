"""Offline tests: time_weather_control reference plugin under fireball.mock_host (no game needed)."""

from __future__ import annotations

import os
import re
import shutil
import struct
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if ROOT not in sys.path:
    sys.path.insert(0, ROOT)

from fireball.abi import (  # noqa: E402
    FB_CAP_FRAME_LIFECYCLE,
    FB_CAP_INPUT_EVENTS,
    FB_CAP_IO_VIRTUAL,
    FB_CAP_RAGE_SYMBOL_REG,
    FB_CAP_TITLE_MEM_RW,
    FB_DEV_KEYBOARD,
    FB_INPUT_PRESS,
    InputEvent,
)
from fireball.mock_host import FireballCore, MockVictoryHost  # noqa: E402
from fireball.mod_source import open_mod_source  # noqa: E402

MOD_DIR = os.path.join(ROOT, "examples", "time_weather_control")
BASE = 0x7FF7_0000_0000
CLOCK = BASE + 0x100
WEATHER = BASE + 0x200
H = [0xF5A87B65, 0x614A1F91, 0x5974E8E5]
TIER2 = FB_CAP_FRAME_LIFECYCLE | FB_CAP_INPUT_EVENTS | FB_CAP_RAGE_SYMBOL_REG | FB_CAP_TITLE_MEM_RW


def f32(x: float) -> int:
    return struct.unpack("<I", struct.pack("<f", x))[0]


class TimeWeatherTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.mod = os.path.join(self.tmp.name, "time_weather_control")
        shutil.copytree(MOD_DIR, self.mod)
        lst = os.path.join(self.tmp.name, "weather_types.txt")
        with open(lst, "w", encoding="ascii") as f:
            f.write("# index joaat_lower joaat name\n")
            for i, (h, n) in enumerate(zip(H, ["HIGHPRESSURE", "SUNNY", "MISTY"])):
                f.write(f"{i} 0x{h:08X} 0x{i + 1:08X} {n}\n")
        self.log = os.path.join(self.tmp.name, "tw_log.csv")
        toml_path = os.path.join(self.mod, "mod.toml")
        with open(toml_path, encoding="utf-8") as f:
            text = f.read()
        text = re.sub(r"^weather_list_path = .*$", lambda _: f"weather_list_path = '{lst}'", text, flags=re.M)
        text = re.sub(r"^log_path = .*$", lambda _: f"log_path = '{self.log}'", text, flags=re.M)
        text = re.sub(r"^verify_frames = .*$", "verify_frames = 3", text, flags=re.M)
        text = re.sub(r"^weather_blend_ms = .*$", "weather_blend_ms = 1000", text, flags=re.M)
        with open(toml_path, "w", encoding="utf-8") as f:
            f.write(text)
        self.frame_no = 0

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def start(self, caps: int = TIER2, symbols: bool = True):
        host = MockVictoryHost("mock", caps)
        host.map_region(BASE, 0x1000)
        if symbols:
            host.register_symbol("fb.clock", CLOCK)
            host.register_symbol("fb.weather", WEATHER)
        core = FireballCore()
        core.attach_host(host)
        with open_mod_source(self.mod) as src:
            report = core.load_mod(src)
        return host, core, report

    def frame(self, core: FireballCore, n: int = 1) -> None:
        for _ in range(n):
            self.frame_no += 1
            core.on_frame_begin(self.frame_no, 1.0 / 30.0)

    def press(self, core: FireballCore, vk: int) -> int:
        return core.on_input_event(InputEvent(device_kind=FB_DEV_KEYBOARD, code=vk, action=FB_INPUT_PRESS))

    def clock(self, host: MockVictoryHost):
        return tuple(host.engine_read_u32(CLOCK + 4 * i) for i in range(3))

    def set_clock(self, host: MockVictoryHost, h: int, m: int, s: int = 0) -> None:
        for i, v in enumerate((h, m, s)):
            host.engine_write_u32(CLOCK + 4 * i, v)

    def test_hour_steps_and_midnight(self) -> None:
        host, core, rep = self.start()
        self.assertEqual(rep.activated_plugins, ["time_weather"])
        self.set_clock(host, 10, 15)
        self.frame(core)
        self.assertEqual(self.press(core, 0xDD), 1)
        self.assertEqual(self.clock(host), (10, 15, 0), "nothing is written before the frame")
        self.frame(core)
        self.assertEqual(self.clock(host), (11, 15, 0))
        self.press(core, 0xDB)
        self.press(core, 0xDB)
        self.frame(core)
        self.assertEqual(self.clock(host), (9, 15, 0))
        self.set_clock(host, 0, 30)
        self.press(core, 0xDB)
        self.frame(core)
        self.assertEqual(self.clock(host), (23, 30, 0))
        self.assertEqual(self.press(core, 0x41), 0, "unrelated keys pass through")

    def test_pause_holds_and_resume_releases(self) -> None:
        host, core, _ = self.start()
        self.set_clock(host, 12, 0)
        self.frame(core)
        self.press(core, 0xDC)
        self.frame(core)
        host.engine_write_u32(CLOCK + 4, 1)  # the game ticks a minute
        self.frame(core)
        self.assertEqual(self.clock(host), (12, 0, 0))
        self.press(core, 0xDD)
        self.frame(core)
        host.engine_write_u32(CLOCK + 4, 9)
        self.frame(core)
        self.assertEqual(self.clock(host), (13, 0, 0), "+1h moves the held time")
        self.press(core, 0xDC)
        self.frame(core)
        host.engine_write_u32(CLOCK + 4, 7)
        self.frame(core)
        self.assertEqual(self.clock(host)[1], 7)

    def test_time_scale_and_slider_set(self) -> None:
        host, core, _ = self.start()
        plugin = core.loaded_plugins["time_weather"]
        self.set_clock(host, 6, 0)
        self.frame(core)
        plugin.cmd_set_scale(10.0)  # 10 x 30 game s per wall s at 2,000 ms per game minute
        self.frame(core)
        self.frame(core, 30)  # one wall second
        h, m, s = self.clock(host)
        self.assertEqual((h, m), (6, 5))
        plugin.cmd_set_tod(18 * 3600 + 45 * 60)  # the slider
        self.frame(core)
        self.assertEqual(self.clock(host)[:2], (18, 45))
        plugin.cmd_set_scale(1.0)
        self.frame(core)
        host.engine_write_u32(CLOCK + 4, 50)
        self.frame(core)
        self.assertEqual(self.clock(host)[1], 50, "scale 1 releases the clock")

    def test_weather_set_lock_prev_and_blend(self) -> None:
        host, core, _ = self.start()
        plugin = core.loaded_plugins["time_weather"]
        host.engine_write_u32(WEATHER, H[0])
        host.engine_write_u32(WEATHER + 4, H[0])
        self.frame(core)
        self.assertEqual(self.press(core, 0xDE), 1)
        self.frame(core)
        self.assertEqual([host.engine_read_u32(WEATHER + o) for o in (0, 4, 8)], [H[1], H[1], 0])
        host.engine_write_u32(WEATHER + 4, H[2])  # the game's cycle moves on
        self.frame(core)
        self.assertEqual(host.engine_read_u32(WEATHER + 4), H[1], "lock re-asserts")
        self.press(core, 0xBA)
        self.frame(core)
        self.assertEqual(host.engine_read_u32(WEATHER + 4), H[0])

        plugin.cmd_set_weather(2, blend=True)
        self.frame(core)
        self.assertEqual([host.engine_read_u32(WEATHER + o) for o in (0, 4, 8)], [H[0], H[2], f32(0.0)])
        self.frame(core, 15)  # half of 1,000 ms
        interp = struct.unpack("<f", struct.pack("<I", host.engine_read_u32(WEATHER + 8)))[0]
        self.assertAlmostEqual(interp, 0.5, delta=0.05)
        self.frame(core, 20)
        self.assertEqual([host.engine_read_u32(WEATHER + o) for o in (0, 4, 8)], [H[2], H[2], 0])
        plugin.cmd_set_lock(False)
        self.frame(core)
        host.engine_write_u32(WEATHER + 4, H[1])
        self.frame(core)
        self.assertEqual(host.engine_read_u32(WEATHER + 4), H[1], "unlocked: the game may change it")

        self.frame(core, 5)
        events = {(r[1], r[2]) for r in plugin.log_rows}
        self.assertIn(("weather_set", "weather.next"), events)
        self.assertIn(("hold_lock", "weather.next"), events)
        self.assertIn(("verify", "weather.next"), events)
        with open(self.log, encoding="utf-8") as f:
            self.assertIn(",weather_blend_done,weather.prev,", f.read())

    def test_degrades_without_tier2_or_symbols(self) -> None:
        _, core, rep = self.start(caps=FB_CAP_IO_VIRTUAL | FB_CAP_FRAME_LIFECYCLE)
        self.assertEqual(rep.activated_plugins, [])
        self.assertTrue(any("FB_CAP_TITLE_MEM_RW" in d for d in rep.diagnostics))
        _, core, rep = self.start(symbols=False)
        self.assertEqual(rep.activated_plugins, [])
        self.assertTrue(any("init returned error" in d for d in rep.diagnostics))

    def test_layout_round_trip(self) -> None:
        import importlib.util

        spec = importlib.util.spec_from_file_location("tw_ref", os.path.join(MOD_DIR, "plugins", "time_weather.py"))
        mod = importlib.util.module_from_spec(spec)
        assert spec.loader is not None
        sys.modules["tw_ref"] = mod
        spec.loader.exec_module(mod)
        for layout in mod.LAYOUT_FIELDS:
            for tod in (0.0, 3599.0, 12 * 3600 + 34 * 60, 86340.0):
                vals = [v for _, v in mod.encode_tod(layout, tod)]
                back = mod.decode_tod(layout, vals)
                want = tod - tod % 60 if layout in ("h,m", "m,h", "minutes_of_day") else tod
                self.assertAlmostEqual(back, want, delta=0.5, msg=layout)


if __name__ == "__main__":
    unittest.main()
