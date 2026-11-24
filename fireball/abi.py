"""Python mirror of include/fireball_host.h and include/fireball_plugin.h plus C11 ABI contract checks."""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from typing import Dict, List, Tuple

FB_HOST_ABI_VERSION = 1
FB_PLUGIN_ABI_VERSION = 1

# Status codes (fb_status_t)
FB_OK = 0
FB_ERR_INVALID_ARG = -1
FB_ERR_ABI_MISMATCH = -2
FB_ERR_UNSUPPORTED = -3
FB_ERR_NOT_FOUND = -4
FB_ERR_MEM_FAULT = -5
FB_ERR_PERMISSION = -6
FB_ERR_INTERNAL = -7

# Log levels (fb_log_level_t)
FB_LOG_TRACE = 0
FB_LOG_DEBUG = 1
FB_LOG_INFO = 2
FB_LOG_WARN = 3
FB_LOG_ERROR = 4

# Capability flags (fb_cap_flags_t)
FB_CAP_NONE = 0
# Tier 0
FB_CAP_IO_VIRTUAL = 1 << 0
FB_CAP_DSTORAGE_INTERCEPT = 1 << 1
FB_CAP_D3D12_SHADERS = 1 << 2
FB_CAP_D3D12_TEXTURES = 1 << 3
FB_CAP_BVH_DXR_FALLBACK = 1 << 4
FB_CAP_XMEM_HOOKS = 1 << 5
FB_CAP_MEDIA_BINK_AUDIO = 1 << 6
FB_CAP_FRAME_LIFECYCLE = 1 << 7
FB_CAP_INPUT_EVENTS = 1 << 8
# Tier 1
FB_CAP_RAGE_FIDEVICE = 1 << 16
FB_CAP_RAGE_DATAFILEMGR = 1 << 17
FB_CAP_RAGE_CRC_BYPASS = 1 << 18
# Tier 2
FB_CAP_RAGE_STORES = 1 << 24
FB_CAP_RAGE_SYMBOL_REG = 1 << 25
FB_CAP_TITLE_MEM_RW = 1 << 26

CAP_NAME_TO_BIT: Dict[str, int] = {
    "FB_CAP_NONE": FB_CAP_NONE,
    "FB_CAP_IO_VIRTUAL": FB_CAP_IO_VIRTUAL,
    "FB_CAP_DSTORAGE_INTERCEPT": FB_CAP_DSTORAGE_INTERCEPT,
    "FB_CAP_D3D12_SHADERS": FB_CAP_D3D12_SHADERS,
    "FB_CAP_D3D12_TEXTURES": FB_CAP_D3D12_TEXTURES,
    "FB_CAP_BVH_DXR_FALLBACK": FB_CAP_BVH_DXR_FALLBACK,
    "FB_CAP_XMEM_HOOKS": FB_CAP_XMEM_HOOKS,
    "FB_CAP_MEDIA_BINK_AUDIO": FB_CAP_MEDIA_BINK_AUDIO,
    "FB_CAP_FRAME_LIFECYCLE": FB_CAP_FRAME_LIFECYCLE,
    "FB_CAP_INPUT_EVENTS": FB_CAP_INPUT_EVENTS,
    "FB_CAP_RAGE_FIDEVICE": FB_CAP_RAGE_FIDEVICE,
    "FB_CAP_RAGE_DATAFILEMGR": FB_CAP_RAGE_DATAFILEMGR,
    "FB_CAP_RAGE_CRC_BYPASS": FB_CAP_RAGE_CRC_BYPASS,
    "FB_CAP_RAGE_STORES": FB_CAP_RAGE_STORES,
    "FB_CAP_RAGE_SYMBOL_REG": FB_CAP_RAGE_SYMBOL_REG,
    "FB_CAP_TITLE_MEM_RW": FB_CAP_TITLE_MEM_RW,
}

BIT_TO_CAP_NAME: Dict[int, str] = {v: k for k, v in CAP_NAME_TO_BIT.items() if v != 0}

# Event IDs (fb_event_id_t)
FB_EVENT_FRAME_BEGIN = 1
FB_EVENT_FRAME_PRESENT = 2
FB_EVENT_INPUT_EVENT = 3
FB_EVENT_SHUTDOWN = 4

# Input actions (fb_input_action_t)
FB_INPUT_RELEASE = 0
FB_INPUT_PRESS = 1
FB_INPUT_REPEAT = 2

# Device kinds
FB_DEV_KEYBOARD = 0
FB_DEV_GAMEPAD = 1


@dataclass(frozen=True)
class InputEvent:
    device_kind: int
    code: int
    action: int
    modifiers: int = 0
    timestamp_us: int = 0


@dataclass(frozen=True)
class FrameEvent:
    frame_index: int
    dt_sec: float


def caps_from_names(names: List[str]) -> int:
    mask = 0
    for name in names:
        if name not in CAP_NAME_TO_BIT:
            raise ValueError(f"Unknown Fireball capability flag: {name}")
        mask |= CAP_NAME_TO_BIT[name]
    return mask


def cap_names_from_mask(mask: int) -> List[str]:
    return [name for bit, name in sorted(BIT_TO_CAP_NAME.items()) if mask & bit]


def missing_cap_names(required_mask: int, active_mask: int) -> List[str]:
    missing = required_mask & ~active_mask
    return cap_names_from_mask(missing)


def verify_c11_headers(include_dir: str) -> Tuple[bool, List[str]]:
    """Static verification of include/fireball_host.h and include/fireball_plugin.h."""
    errors: List[str] = []
    host_h = os.path.join(include_dir, "fireball_host.h")
    plugin_h = os.path.join(include_dir, "fireball_plugin.h")
    for p in (host_h, plugin_h):
        if not os.path.isfile(p):
            errors.append(f"Missing header: {p}")
    if errors:
        return False, errors

    with open(host_h, "r", encoding="utf-8") as f:
        host_txt = f.read()
    with open(plugin_h, "r", encoding="utf-8") as f:
        plugin_txt = f.read()

    # Check pure-C11 compatibility (no C23 fixed-underlying enum syntax)
    for label, txt in (("fireball_host.h", host_txt), ("fireball_plugin.h", plugin_txt)):
        if re.search(r"enum\s+\w+\s*:\s*uint\d+_t", txt):
            errors.append(f"{label}: uses C23 fixed-underlying enum syntax (not strict C11)")
        if 'extern "C"' not in txt:
            errors.append(f"{label}: missing extern \"C\" guard")

    # Verify every capability macro matches abi.py bit positions
    for name, bit in CAP_NAME_TO_BIT.items():
        if name == "FB_CAP_NONE":
            if not re.search(r"#define\s+FB_CAP_NONE\s+\(0ull\)", host_txt):
                errors.append("fireball_host.h: FB_CAP_NONE macro mismatch")
        else:
            shift = bit.bit_length() - 1
            pat = rf"#define\s+{re.escape(name)}\s+\(1ull\s*<<\s*{shift}\)"
            if not re.search(pat, host_txt):
                errors.append(f"fireball_host.h: missing or mismatched macro {name} (expected 1ull << {shift})")

    # Verify required structs & function pointers in host header
    for sym in (
        "fb_host_api_v1_t",
        "fb_modcore_callbacks_v1_t",
        "fb_input_event_v1_t",
        "fb_io_override_desc_t",
        "fb_dstorage_req_view_t",
        "on_input_event",
        "poll_key_state",
        "resolve_symbol",
        "title_mem_read_u32",
        "title_mem_write_u32",
        "fb_attach_host",
    ):
        if sym not in host_txt:
            errors.append(f"fireball_host.h: missing required symbol '{sym}'")

    # Verify required structs & function pointers in plugin header
    for sym in (
        "fb_plugin_info_v1_t",
        "fb_core_services_v1_t",
        "fb_frame_event_v1_t",
        "resolve_engine_symbol",
        "register_event_listener",
        "poll_key_state",
        "title_mem_read_u32",
        "title_mem_write_u32",
        "fb_plugin_query",
        "fb_plugin_init",
        "fb_plugin_shutdown",
    ):
        if sym not in plugin_txt:
            errors.append(f"fireball_plugin.h: missing required symbol '{sym}'")

    return (len(errors) == 0), errors
