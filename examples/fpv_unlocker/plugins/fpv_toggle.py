"""
fpv_toggle — Reference Fireball-Core Tier 2 Plugin State Machine for On-Foot FPV Toggle & Hold-Guard.

Mirrors the exact lifecycle and service calls defined in include/fireball_plugin.h:
  - fb_plugin_query()
  - fb_plugin_init(core_services)
  - fb_plugin_shutdown()

Behavior:
  1. Resolves `rage::camGameplayDirector::s_Instance` via `resolve_engine_symbol`.
  2. Listens for `FB_EVENT_INPUT_EVENT`: pressing hotkey (`VK_V` / 0x56 by default)
     flips `camGameplayDirector.view_mode` between `0 (ThirdPerson)` and `2 (FirstPerson)`
     via guarded `title_mem_read_u32` / `title_mem_write_u32`.
  3. Listens for `FB_EVENT_FRAME_BEGIN`: while FPV toggle is latched ON (`fpv_latched == True`)
     and `hold_guard_enabled == True`, detects if the game engine resets `view_mode` back to `0`
     (e.g., on-foot transition / vehicle exit / cover exit) and immediately re-asserts `2 (FirstPerson)`.
  4. When toggled OFF (`fpv_latched == False`), releases control cleanly so game-driven mode
     transitions (e.g., `0=ThirdPerson`, `1=Cinematic`) are never overwritten.
"""

from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Any, Dict

from fireball.abi import (
    FB_CAP_FRAME_LIFECYCLE,
    FB_CAP_INPUT_EVENTS,
    FB_CAP_RAGE_SYMBOL_REG,
    FB_CAP_TITLE_MEM_RW,
    FB_DEV_KEYBOARD,
    FB_ERR_NOT_FOUND,
    FB_EVENT_FRAME_BEGIN,
    FB_EVENT_INPUT_EVENT,
    FB_EVENT_SHUTDOWN,
    FB_INPUT_PRESS,
    FB_LOG_INFO,
    FB_LOG_WARN,
    FB_OK,
    FB_PLUGIN_ABI_VERSION,
    FrameEvent,
    InputEvent,
)


@dataclass
class PluginInfo:
    abi_version: int
    mod_id: str
    plugin_id: str
    version: str
    required_caps: int


class FpvTogglePlugin:
    def __init__(self) -> None:
        self._core: Any = None
        self.mod_id: str = "community.fpv_unlocker"
        self.plugin_id: str = "fpv_toggle"
        self.hotkey_vkey: int = 0x56  # 'V'
        self.third_person_mode: int = 0
        self.first_person_mode: int = 2
        self.director_symbol: str = "rage::camGameplayDirector::s_Instance"
        self.view_mode_field_offset: int = 0
        self.hold_guard_enabled: bool = True

        self.director_va: int = 0
        self.view_mode_va: int = 0
        self.fpv_latched: bool = False
        self.toggle_count: int = 0
        self.reassert_count: int = 0
        self.frames_seen: int = 0
        self.initialized: bool = False

    def fb_plugin_query(self) -> PluginInfo:
        return PluginInfo(
            abi_version=FB_PLUGIN_ABI_VERSION,
            mod_id=self.mod_id,
            plugin_id=self.plugin_id,
            version="0.1.0",
            required_caps=(
                FB_CAP_FRAME_LIFECYCLE
                | FB_CAP_INPUT_EVENTS
                | FB_CAP_RAGE_SYMBOL_REG
                | FB_CAP_TITLE_MEM_RW
            ),
        )

    def fb_plugin_init(self, core: Any) -> int:
        self._core = core
        cfg_raw = core.config_get_json(self.mod_id)
        if cfg_raw:
            cfg: Dict[str, Any] = json.loads(cfg_raw)
            self.hotkey_vkey = int(cfg.get("hotkey_vkey", self.hotkey_vkey))
            self.third_person_mode = int(cfg.get("third_person_mode", self.third_person_mode))
            self.first_person_mode = int(cfg.get("first_person_mode", self.first_person_mode))
            self.director_symbol = str(cfg.get("director_symbol", self.director_symbol))
            self.view_mode_field_offset = int(cfg.get("view_mode_field_offset", self.view_mode_field_offset))
            self.hold_guard_enabled = bool(cfg.get("hold_guard_enabled", self.hold_guard_enabled))

        self.director_va = core.resolve_engine_symbol(self.director_symbol)
        if not self.director_va:
            core.log(
                self.mod_id,
                FB_LOG_WARN,
                f"[fpv_toggle] Symbol '{self.director_symbol}' unavailable; plugin standing by.",
            )
            return FB_ERR_NOT_FOUND

        self.view_mode_va = self.director_va + self.view_mode_field_offset
        core.register_event_listener(FB_EVENT_INPUT_EVENT, self._on_event, self)
        core.register_event_listener(FB_EVENT_FRAME_BEGIN, self._on_event, self)
        core.register_event_listener(FB_EVENT_SHUTDOWN, self._on_event, self)
        self.initialized = True
        core.log(
            self.mod_id,
            FB_LOG_INFO,
            f"[fpv_toggle] Initialized: symbol={self.director_symbol} @ 0x{self.director_va:016X}, "
            f"view_mode_va=0x{self.view_mode_va:016X}, hotkey=0x{self.hotkey_vkey:02X}, hold_guard={self.hold_guard_enabled}",
        )
        return FB_OK

    def fb_plugin_shutdown(self) -> None:
        if self.initialized and self.fpv_latched and self._core is not None:
            self._core.title_mem_write_u32(self.view_mode_va, self.third_person_mode)
            self.fpv_latched = False
        self.initialized = False

    def _on_event(self, event_id: int, payload: Any, _user_ctx: Any) -> int:
        if event_id == FB_EVENT_INPUT_EVENT and isinstance(payload, InputEvent):
            return self._handle_input(payload)
        if event_id == FB_EVENT_FRAME_BEGIN and isinstance(payload, FrameEvent):
            self._handle_frame_begin(payload)
            return 0
        if event_id == FB_EVENT_SHUTDOWN:
            self.fb_plugin_shutdown()
            return 0
        return 0

    def _handle_input(self, ev: InputEvent) -> int:
        if ev.device_kind != FB_DEV_KEYBOARD:
            return 0
        if ev.code != self.hotkey_vkey or ev.action != FB_INPUT_PRESS:
            return 0

        rc, cur_mode = self._core.title_mem_read_u32(self.view_mode_va)
        if rc != FB_OK:
            self._core.log(
                self.mod_id,
                FB_LOG_WARN,
                f"[fpv_toggle] Guarded read failed on 0x{self.view_mode_va:016X} (rc={rc})",
            )
            return 0

        if not self.fpv_latched:
            target_mode = self.first_person_mode
            self.fpv_latched = True
        else:
            target_mode = self.third_person_mode
            self.fpv_latched = False

        wrc = self._core.title_mem_write_u32(self.view_mode_va, target_mode)
        if wrc == FB_OK:
            self.toggle_count += 1
            self._core.log(
                self.mod_id,
                FB_LOG_INFO,
                f"[fpv_toggle] Hotkey toggled view_mode: {cur_mode} -> {target_mode} (latched={self.fpv_latched})",
            )
            return 1
        return 0

    def _handle_frame_begin(self, frame: FrameEvent) -> None:
        self.frames_seen += 1
        if not (self.fpv_latched and self.hold_guard_enabled):
            return
        rc, cur_mode = self._core.title_mem_read_u32(self.view_mode_va)
        if rc == FB_OK and cur_mode != self.first_person_mode:
            wrc = self._core.title_mem_write_u32(self.view_mode_va, self.first_person_mode)
            if wrc == FB_OK:
                self.reassert_count += 1
                self._core.log(
                    self.mod_id,
                    FB_LOG_INFO,
                    f"[fpv_toggle] Frame #{frame.frame_index}: hold-guard intercepted context reset "
                    f"({cur_mode} -> re-asserted {self.first_person_mode})",
                )


def create_plugin() -> FpvTogglePlugin:
    return FpvTogglePlugin()
