"""
Mock VIctory-Style Recomp/HLE Host + Fireball-Core Runtime Simulator.

Exercises:
  1. Full Tier 0+1+2 host attachment (`fb_attach_host`), loading `fpv_unlocker`,
     firing `on_frame_begin` & `on_input_event` (hotkey press/release/unrelated key),
     verifying `camGameplayDirector.view_mode` flips `0 -> 2 -> 0`, verifying
     on-foot context-reset hold-guard re-assertion while latched ON and clean release
     when toggled OFF, and verifying guarded memory bounds protection.
  2. Tier 0-only host attachment verifying graceful degradation: Tier 1 patches
     and Tier 2 `fpv_toggle` plugin are skipped cleanly with explicit missing-cap
     diagnostics and zero crashes.
"""

from __future__ import annotations

import importlib.util
import json
import struct
import sys
from dataclasses import dataclass, field
from typing import Any, Callable, Dict, List, Optional, Tuple

from .abi import (
    FB_CAP_D3D12_SHADERS,
    FB_CAP_DSTORAGE_INTERCEPT,
    FB_CAP_FRAME_LIFECYCLE,
    FB_CAP_INPUT_EVENTS,
    FB_CAP_IO_VIRTUAL,
    FB_CAP_RAGE_CRC_BYPASS,
    FB_CAP_RAGE_DATAFILEMGR,
    FB_CAP_RAGE_FIDEVICE,
    FB_CAP_RAGE_SYMBOL_REG,
    FB_CAP_TITLE_MEM_RW,
    FB_DEV_KEYBOARD,
    FB_ERR_ABI_MISMATCH,
    FB_ERR_MEM_FAULT,
    FB_ERR_PERMISSION,
    FB_ERR_UNSUPPORTED,
    FB_EVENT_FRAME_BEGIN,
    FB_EVENT_INPUT_EVENT,
    FB_EVENT_SHUTDOWN,
    FB_HOST_ABI_VERSION,
    FB_INPUT_PRESS,
    FB_INPUT_RELEASE,
    FB_LOG_INFO,
    FB_LOG_WARN,
    FB_OK,
    FB_PLUGIN_ABI_VERSION,
    FrameEvent,
    InputEvent,
    cap_names_from_mask,
    caps_from_names,
    missing_cap_names,
)
from .mod_source import ModManifest, VirtualModSource, load_manifest


@dataclass
class TitleMemoryRegion:
    base_va: int
    data: bytearray
    writable: bool = True

    def contains(self, va: int, size: int) -> bool:
        return self.base_va <= va and (va + size) <= (self.base_va + len(self.data))


class MockVictoryHost:
    """Simulates a Marvisak/VIctory HLE host with guarded title memory & symbol registry."""

    def __init__(self, host_name: str, capabilities: int) -> None:
        self.abi_version = FB_HOST_ABI_VERSION
        self.host_name = host_name
        self.host_version = "0.1.0-mock"
        self.capabilities = capabilities
        self.logs: List[Tuple[int, str, str]] = []
        self._symbols: Dict[str, int] = {}
        self._regions: List[TitleMemoryRegion] = []
        self._key_state: Dict[Tuple[int, int], bool] = {}

    def map_region(self, base_va: int, size: int, writable: bool = True) -> TitleMemoryRegion:
        reg = TitleMemoryRegion(base_va=base_va, data=bytearray(size), writable=writable)
        self._regions.append(reg)
        return reg

    def register_symbol(self, name: str, title_va: int) -> None:
        self._symbols[name] = title_va

    def log(self, level: int, subsystem: str, msg: str) -> None:
        self.logs.append((level, subsystem, msg))

    def resolve_symbol(self, symbol_name: str) -> int:
        if not (self.capabilities & FB_CAP_RAGE_SYMBOL_REG):
            return 0
        return self._symbols.get(symbol_name, 0)

    def poll_key_state(self, device_kind: int, code: int) -> int:
        if not (self.capabilities & FB_CAP_INPUT_EVENTS):
            return FB_ERR_UNSUPPORTED
        return 1 if self._key_state.get((device_kind, code), False) else 0

    def title_mem_read_u32(self, title_va: int) -> Tuple[int, int]:
        if not (self.capabilities & FB_CAP_TITLE_MEM_RW):
            return FB_ERR_UNSUPPORTED, 0
        for reg in self._regions:
            if reg.contains(title_va, 4):
                off = title_va - reg.base_va
                return FB_OK, struct.unpack_from("<I", reg.data, off)[0]
        return FB_ERR_MEM_FAULT, 0

    def title_mem_write_u32(self, title_va: int, value: int) -> int:
        if not (self.capabilities & FB_CAP_TITLE_MEM_RW):
            return FB_ERR_UNSUPPORTED
        for reg in self._regions:
            if reg.contains(title_va, 4):
                if not reg.writable:
                    return FB_ERR_PERMISSION
                off = title_va - reg.base_va
                struct.pack_into("<I", reg.data, off, value & 0xFFFFFFFF)
                return FB_OK
        return FB_ERR_MEM_FAULT

    # Direct engine-side read/write (representing game_scarlett_master.exe mutating its own state)
    def engine_read_u32(self, title_va: int) -> int:
        for reg in self._regions:
            if reg.contains(title_va, 4):
                return struct.unpack_from("<I", reg.data, title_va - reg.base_va)[0]
        raise MemoryError(f"Unmapped engine VA 0x{title_va:016X}")

    def engine_write_u32(self, title_va: int, value: int) -> None:
        for reg in self._regions:
            if reg.contains(title_va, 4):
                struct.pack_into("<I", reg.data, title_va - reg.base_va, value & 0xFFFFFFFF)
                return
        raise MemoryError(f"Unmapped engine VA 0x{title_va:016X}")


@dataclass
class CoreServicesView:
    """Python representation of fb_core_services_v1_t."""

    core: "FireballCore"
    active_host_caps: int

    def log(self, mod_id: str, level: int, msg: str) -> None:
        assert self.core.host is not None
        self.core.host.log(level, mod_id, msg)

    def config_get_json(self, mod_id: str) -> str:
        return json.dumps(self.core.mod_plugin_configs.get(mod_id, {}))

    def register_event_listener(self, event_id: int, fn: Callable[..., int], user_ctx: Any) -> int:
        self.core.event_listeners.setdefault(event_id, []).append((fn, user_ctx))
        return FB_OK

    def poll_key_state(self, device_kind: int, code: int) -> int:
        assert self.core.host is not None
        return self.core.host.poll_key_state(device_kind, code)

    def resolve_engine_symbol(self, symbol_name: str) -> int:
        assert self.core.host is not None
        return self.core.host.resolve_symbol(symbol_name)

    def title_mem_read_u32(self, title_va: int) -> Tuple[int, int]:
        assert self.core.host is not None
        return self.core.host.title_mem_read_u32(title_va)

    def title_mem_write_u32(self, title_va: int, value: int) -> int:
        assert self.core.host is not None
        return self.core.host.title_mem_write_u32(title_va, value)


@dataclass
class ModActivationReport:
    mod_id: str
    activated_patches: List[str] = field(default_factory=list)
    skipped_patches: List[Dict[str, Any]] = field(default_factory=list)
    activated_plugins: List[str] = field(default_factory=list)
    skipped_plugins: List[Dict[str, Any]] = field(default_factory=list)
    diagnostics: List[str] = field(default_factory=list)


class FireballCore:
    """Prototype Fireball-Core runtime supporting capability negotiation and graceful degradation."""

    def __init__(self) -> None:
        self.host: Optional[MockVictoryHost] = None
        self.event_listeners: Dict[int, List[Tuple[Callable[..., int], Any]]] = {}
        self.mod_plugin_configs: Dict[str, Dict[str, Any]] = {}
        self.loaded_plugins: Dict[str, Any] = {}

    def attach_host(self, host: MockVictoryHost) -> int:
        if host.abi_version != FB_HOST_ABI_VERSION:
            return FB_ERR_ABI_MISMATCH
        self.host = host
        host.log(
            FB_LOG_INFO,
            "fireball-core",
            f"Attached to host '{host.host_name}' v{host.host_version} with caps={cap_names_from_mask(host.capabilities)}",
        )
        return FB_OK

    def load_mod(self, source: VirtualModSource) -> ModActivationReport:
        assert self.host is not None, "Call attach_host() before load_mod()"
        manifest: ModManifest = load_manifest(source)
        report = ModActivationReport(mod_id=manifest.mod_id)

        # 1. Evaluate all declarative patches against host capabilities
        all_patches = (
            [("xml", p) for p in manifest.xml_patches]
            + [("kv", p) for p in manifest.kv_patches]
            + [("pso", p) for p in manifest.pso_patches]
        )
        for kind, pspec in all_patches:
            pid = str(pspec.get("id", pspec.get("target", "unnamed")))
            req_caps = caps_from_names(list(pspec.get("required_caps", [])))
            missing = missing_cap_names(req_caps, self.host.capabilities)
            if missing:
                msg = f"Skipped {kind} patch '{pid}': host '{self.host.host_name}' lacks {missing}"
                report.skipped_patches.append({"id": pid, "kind": kind, "missing_caps": missing, "reason": msg})
                report.diagnostics.append(msg)
                self.host.log(FB_LOG_WARN, "fireball-core", msg)
            else:
                report.activated_patches.append(pid)

        # 2. Evaluate and initialize plugins against host capabilities
        for plg_spec in manifest.plugins:
            plg_id = str(plg_spec["id"])
            entry = str(plg_spec["entry"])
            req_caps = caps_from_names(list(plg_spec.get("required_caps", [])))
            missing = missing_cap_names(req_caps, self.host.capabilities)
            if missing:
                msg = f"Skipped plugin '{plg_id}': host '{self.host.host_name}' lacks {missing}"
                report.skipped_plugins.append({"id": plg_id, "missing_caps": missing, "reason": msg})
                report.diagnostics.append(msg)
                self.host.log(FB_LOG_WARN, "fireball-core", msg)
                continue

            src_code = source.read_text(entry)
            mod_name = f"fb_loaded_plugin_{manifest.mod_id.replace('.', '_')}_{plg_id}"
            spec = importlib.util.spec_from_loader(mod_name, loader=None)
            assert spec is not None
            py_mod = importlib.util.module_from_spec(spec)
            sys.modules[mod_name] = py_mod
            exec(compile(src_code, f"{source.source_path}/{entry}", "exec"), py_mod.__dict__)
            plugin = py_mod.create_plugin()

            info = plugin.fb_plugin_query()
            if info.abi_version != FB_PLUGIN_ABI_VERSION:
                msg = f"Skipped plugin '{plg_id}': ABI mismatch ({info.abi_version} != {FB_PLUGIN_ABI_VERSION})"
                report.skipped_plugins.append({"id": plg_id, "missing_caps": [], "reason": msg})
                report.diagnostics.append(msg)
                continue

            missing_q = missing_cap_names(info.required_caps, self.host.capabilities)
            if missing_q:
                msg = f"Skipped plugin '{plg_id}' after query(): host lacks {missing_q}"
                report.skipped_plugins.append({"id": plg_id, "missing_caps": missing_q, "reason": msg})
                report.diagnostics.append(msg)
                self.host.log(FB_LOG_WARN, "fireball-core", msg)
                continue

            self.mod_plugin_configs[manifest.mod_id] = dict(plg_spec.get("config", {}))
            services = CoreServicesView(core=self, active_host_caps=self.host.capabilities)
            rc = plugin.fb_plugin_init(services)
            if rc != FB_OK:
                msg = f"Plugin '{plg_id}' init returned error {rc}"
                report.skipped_plugins.append({"id": plg_id, "missing_caps": [], "reason": msg})
                report.diagnostics.append(msg)
                continue

            self.loaded_plugins[plg_id] = plugin
            report.activated_plugins.append(plg_id)

        return report

    def on_frame_begin(self, frame_index: int, dt_sec: float) -> None:
        ev = FrameEvent(frame_index=frame_index, dt_sec=dt_sec)
        for fn, ctx in self.event_listeners.get(FB_EVENT_FRAME_BEGIN, []):
            fn(FB_EVENT_FRAME_BEGIN, ev, ctx)

    def on_input_event(self, ev: InputEvent) -> int:
        assert self.host is not None
        if ev.action == FB_INPUT_PRESS:
            self.host._key_state[(ev.device_kind, ev.code)] = True
        elif ev.action == FB_INPUT_RELEASE:
            self.host._key_state[(ev.device_kind, ev.code)] = False
        consumed = 0
        for fn, ctx in self.event_listeners.get(FB_EVENT_INPUT_EVENT, []):
            if fn(FB_EVENT_INPUT_EVENT, ev, ctx):
                consumed = 1
        return consumed

    def on_shutdown(self) -> None:
        for fn, ctx in self.event_listeners.get(FB_EVENT_SHUTDOWN, []):
            fn(FB_EVENT_SHUTDOWN, None, ctx)


def run_mock_host_suite(mod_source: VirtualModSource) -> Dict[str, Any]:
    """
    Execute both Mock VIctory Host scenarios:
      A. Full Tier 0+1+2 VIctory-style host (toggle 0->2->0, hold-guard re-assert, clean release, memory fault check)
      B. Tier 0-only host (graceful degradation with explicit diagnostics, zero crashes)
    """
    results: Dict[str, Any] = {}

    # --- Scenario A: Full Tier 0+1+2 Host ---
    full_caps = (
        FB_CAP_IO_VIRTUAL
        | FB_CAP_DSTORAGE_INTERCEPT
        | FB_CAP_D3D12_SHADERS
        | FB_CAP_FRAME_LIFECYCLE
        | FB_CAP_INPUT_EVENTS
        | FB_CAP_RAGE_FIDEVICE
        | FB_CAP_RAGE_DATAFILEMGR
        | FB_CAP_RAGE_CRC_BYPASS
        | FB_CAP_RAGE_SYMBOL_REG
        | FB_CAP_TITLE_MEM_RW
    )
    host_full = MockVictoryHost("VIctory-HLE-Mock-Full", full_caps)
    director_va = 0x00007FF712340000
    host_full.map_region(director_va, 0x1000, writable=True)
    host_full.map_region(0x00007FF712350000, 0x1000, writable=False)
    host_full.register_symbol("rage::camGameplayDirector::s_Instance", director_va)
    host_full.engine_write_u32(director_va, 0)  # Start in 0 = ThirdPerson

    core_full = FireballCore()
    assert core_full.attach_host(host_full) == FB_OK
    act_full = core_full.load_mod(mod_source)
    assert len(act_full.activated_patches) == 5, f"Expected 5 active patches, got {act_full.activated_patches}"
    assert act_full.activated_plugins == ["fpv_toggle"]
    plg = core_full.loaded_plugins["fpv_toggle"]

    timeline: List[str] = []

    # Frame 1: Idle in ThirdPerson (0)
    core_full.on_frame_begin(1, 1 / 60.0)
    assert host_full.engine_read_u32(director_va) == 0
    timeline.append("frame1: initial view_mode=0 (ThirdPerson)")

    # Unrelated key ('W' = 0x57) must NOT toggle
    consumed_w = core_full.on_input_event(InputEvent(FB_DEV_KEYBOARD, 0x57, FB_INPUT_PRESS))
    assert consumed_w == 0 and host_full.engine_read_u32(director_va) == 0

    # Hotkey press ('V' = 0x56): flips 0 -> 2 (FirstPerson)
    consumed_v1 = core_full.on_input_event(InputEvent(FB_DEV_KEYBOARD, 0x56, FB_INPUT_PRESS))
    assert consumed_v1 == 1
    assert host_full.engine_read_u32(director_va) == 2
    assert plg.fpv_latched is True
    timeline.append("hotkey 'V' press #1: view_mode 0 -> 2 (FirstPerson), latched=True")

    # Key release must NOT toggle back
    core_full.on_input_event(InputEvent(FB_DEV_KEYBOARD, 0x56, FB_INPUT_RELEASE))
    assert host_full.engine_read_u32(director_va) == 2

    # Simulate game engine on-foot context reset (game writes 0 = ThirdPerson)
    host_full.engine_write_u32(director_va, 0)
    assert host_full.engine_read_u32(director_va) == 0
    core_full.on_frame_begin(2, 1 / 60.0)
    assert host_full.engine_read_u32(director_va) == 2, "Hold-guard failed to re-assert FirstPerson (2)"
    assert plg.reassert_count == 1
    timeline.append("frame2: simulated on-foot context reset (game wrote 0) -> hold-guard re-asserted 2 (FirstPerson)")

    # Simulate second context reset (e.g., cover exit)
    host_full.engine_write_u32(director_va, 0)
    core_full.on_frame_begin(3, 1 / 60.0)
    assert host_full.engine_read_u32(director_va) == 2
    assert plg.reassert_count == 2
    timeline.append("frame3: second context reset (game wrote 0) -> hold-guard re-asserted 2 (reassert_count=2)")

    # Hotkey press #2 ('V'): flips 2 -> 0 (ThirdPerson) and releases latch
    consumed_v2 = core_full.on_input_event(InputEvent(FB_DEV_KEYBOARD, 0x56, FB_INPUT_PRESS))
    assert consumed_v2 == 1
    assert host_full.engine_read_u32(director_va) == 0
    assert plg.fpv_latched is False
    timeline.append("hotkey 'V' press #2: view_mode 2 -> 0 (ThirdPerson), latched=False")

    # After toggle OFF, game writes 1 (Cinematic) and plugin must NOT overwrite it
    host_full.engine_write_u32(director_va, 1)
    core_full.on_frame_begin(4, 1 / 60.0)
    assert host_full.engine_read_u32(director_va) == 1, "Plugin overwrote view_mode while toggled OFF!"
    assert plg.reassert_count == 2
    timeline.append("frame4: game set 1 (Cinematic) while latched=False -> preserved cleanly (no overwrite)")

    # Guarded memory safety checks (unmapped & read-only VA)
    rc_unmap, _ = host_full.title_mem_read_u32(0x00007FF7DEADBEEF)
    rc_ro = host_full.title_mem_write_u32(0x00007FF712350000, 2)
    assert rc_unmap == FB_ERR_MEM_FAULT
    assert rc_ro == FB_ERR_PERMISSION
    timeline.append("guarded memory check: unmapped->FB_ERR_MEM_FAULT, read-only->FB_ERR_PERMISSION")

    core_full.on_shutdown()

    results["full_tier012_host"] = {
        "passed": True,
        "activated_patches": act_full.activated_patches,
        "activated_plugins": act_full.activated_plugins,
        "toggle_count": plg.toggle_count,
        "reassert_count": plg.reassert_count,
        "timeline": timeline,
    }

    # --- Scenario B: Tier 0-Only Host (Graceful Degradation) ---
    tier0_caps = (
        FB_CAP_IO_VIRTUAL
        | FB_CAP_DSTORAGE_INTERCEPT
        | FB_CAP_D3D12_SHADERS
        | FB_CAP_FRAME_LIFECYCLE
    )
    host_t0 = MockVictoryHost("VIctory-HLE-Mock-Tier0Only", tier0_caps)
    core_t0 = FireballCore()
    assert core_t0.attach_host(host_t0) == FB_OK
    act_t0 = core_t0.load_mod(mod_source)

    assert len(act_t0.activated_patches) == 0
    assert len(act_t0.skipped_patches) == 5
    assert len(act_t0.activated_plugins) == 0
    assert len(act_t0.skipped_plugins) == 1
    assert act_t0.skipped_plugins[0]["id"] == "fpv_toggle"
    assert "FB_CAP_RAGE_SYMBOL_REG" in act_t0.skipped_plugins[0]["missing_caps"]
    assert "FB_CAP_TITLE_MEM_RW" in act_t0.skipped_plugins[0]["missing_caps"]
    assert "FB_CAP_INPUT_EVENTS" in act_t0.skipped_plugins[0]["missing_caps"]

    core_t0.on_frame_begin(1, 1 / 60.0)
    core_t0.on_input_event(InputEvent(FB_DEV_KEYBOARD, 0x56, FB_INPUT_PRESS))
    core_t0.on_shutdown()

    results["tier0_only_degradation"] = {
        "passed": True,
        "activated_patches": act_t0.activated_patches,
        "skipped_patches_count": len(act_t0.skipped_patches),
        "activated_plugins": act_t0.activated_plugins,
        "skipped_plugins": act_t0.skipped_plugins,
        "diagnostics": act_t0.diagnostics,
    }
    return results
