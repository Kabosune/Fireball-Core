"""
Multi-mod load-order resolver and `load_order.toml` profile loader for Fireball-Core.

Performs deterministic topological sorting over loose mod folders and `.gta6mod`
ZIP archives, honoring `requires`, `load_after`, and `conflicts` from `mod.toml`,
plus explicit ordering, enable/disable flags, and profile-level overrides from
`load_order.toml`.
"""

from __future__ import annotations

import heapq
import os
import tomllib
from dataclasses import dataclass, field
from typing import Any, Dict, List, Optional, Sequence, Tuple

from .mod_source import ModManifest, load_manifest, open_mod_source


class LoadOrderError(ValueError):
    """Base error for load-order resolution and profile validation."""


class DuplicateModError(LoadOrderError):
    """Raised when two enabled mod sources declare the same `[mod].id`."""


class MissingDependencyError(LoadOrderError):
    """Raised when an enabled mod lists a `requires` mod ID that is not enabled."""


class ModConflictError(LoadOrderError):
    """Raised when two enabled mods declare a conflict via `[dependencies].conflicts`."""


class DependencyCycleError(LoadOrderError):
    """Raised when `requires` / `load_after` constraints form a cycle."""


@dataclass
class ModOverrides:
    patches: Dict[str, bool] = field(default_factory=dict)
    plugin_config: Dict[str, Any] = field(default_factory=dict)

    def merge_from(self, other: "ModOverrides") -> None:
        self.patches.update(other.patches)
        self.plugin_config.update(other.plugin_config)

    def to_dict(self) -> Dict[str, Any]:
        return {
            "patches": dict(sorted(self.patches.items())),
            "plugin_config": dict(sorted(self.plugin_config.items())),
        }


@dataclass
class ProfileModEntry:
    path: str
    enabled: bool = True
    expected_id: Optional[str] = None
    overrides: ModOverrides = field(default_factory=ModOverrides)


@dataclass
class LoadOrderProfile:
    source_path: str
    name: str = ""
    size_preserve: Optional[bool] = None
    bundle_name: Optional[str] = None
    explicit_order: List[str] = field(default_factory=list)
    disabled_ids: set[str] = field(default_factory=set)
    mod_entries: List[ProfileModEntry] = field(default_factory=list)
    overrides_by_id: Dict[str, ModOverrides] = field(default_factory=dict)


@dataclass
class ResolvedMod:
    mod_id: str
    name: str
    version: str
    source_path: str
    source_kind: str
    manifest: ModManifest
    overrides: ModOverrides = field(default_factory=ModOverrides)
    load_index: int = 0

    def to_summary_dict(self) -> Dict[str, Any]:
        return {
            "load_index": self.load_index,
            "mod_id": self.mod_id,
            "name": self.name,
            "version": self.version,
            "source_kind": self.source_kind,
            "source_path": self.source_path,
            "requires": list(self.manifest.requires),
            "load_after": list(self.manifest.load_after),
            "conflicts": list(self.manifest.conflicts),
            "overrides": self.overrides.to_dict(),
        }


@dataclass
class LoadOrderResolution:
    ordered: List[ResolvedMod]
    disabled: List[Dict[str, Any]] = field(default_factory=list)
    profile_path: Optional[str] = None
    profile_name: Optional[str] = None
    size_preserve: Optional[bool] = None
    bundle_name: Optional[str] = None

    @property
    def mod_paths(self) -> List[str]:
        return [m.source_path for m in self.ordered]

    @property
    def mod_overrides(self) -> Dict[str, Dict[str, Any]]:
        out: Dict[str, Dict[str, Any]] = {}
        for m in self.ordered:
            if m.overrides.patches or m.overrides.plugin_config:
                out[m.mod_id] = m.overrides.to_dict()
        return out


def _parse_overrides_dict(raw: Any) -> ModOverrides:
    if not isinstance(raw, dict):
        return ModOverrides()
    patches_raw = raw.get("patches", {})
    cfg_raw = raw.get("plugin_config", {})
    if not isinstance(patches_raw, dict):
        raise LoadOrderError(f"Expected table for overrides.patches, got {type(patches_raw).__name__}")
    if not isinstance(cfg_raw, dict):
        raise LoadOrderError(f"Expected table for overrides.plugin_config, got {type(cfg_raw).__name__}")
    return ModOverrides(
        patches={str(k): bool(v) for k, v in patches_raw.items()},
        plugin_config=dict(cfg_raw),
    )


def _resolve_profile_mod_path(raw_path: str, profile_dir: str) -> str:
    if os.path.isabs(raw_path):
        return os.path.abspath(raw_path)
    rel_to_profile = os.path.abspath(os.path.join(profile_dir, raw_path))
    if os.path.exists(rel_to_profile):
        return rel_to_profile
    rel_to_cwd = os.path.abspath(raw_path)
    if os.path.exists(rel_to_cwd):
        return rel_to_cwd
    return rel_to_profile


def load_profile(profile_path: str) -> LoadOrderProfile:
    """Parse a `load_order.toml` profile file."""
    abs_prof = os.path.abspath(profile_path)
    if not os.path.isfile(abs_prof):
        raise FileNotFoundError(f"Load-order profile not found: {abs_prof}")
    with open(abs_prof, "rb") as rf:
        data = tomllib.load(rf)

    prof_dir = os.path.dirname(abs_prof)
    prof_sec = data.get("profile", {})
    if not isinstance(prof_sec, dict):
        raise LoadOrderError("[profile] must be a TOML table")

    explicit_order = [str(mid) for mid in prof_sec.get("order", [])]
    disabled_ids = {str(mid) for mid in prof_sec.get("disabled", [])}
    size_preserve = bool(prof_sec["size_preserve"]) if "size_preserve" in prof_sec else None
    bundle_name = str(prof_sec["bundle_name"]) if "bundle_name" in prof_sec else None

    entries: List[ProfileModEntry] = []
    for idx, item in enumerate(data.get("mods", [])):
        if not isinstance(item, dict) or "path" not in item:
            raise LoadOrderError(f"[[mods]] entry #{idx} must be a table with a 'path' field")
        resolved_p = _resolve_profile_mod_path(str(item["path"]), prof_dir)
        entries.append(
            ProfileModEntry(
                path=resolved_p,
                enabled=bool(item.get("enabled", True)),
                expected_id=str(item["id"]) if "id" in item else None,
                overrides=_parse_overrides_dict(item.get("overrides")),
            )
        )

    overrides_by_id: Dict[str, ModOverrides] = {}
    for mid, raw_ov in data.get("overrides", {}).items():
        overrides_by_id[str(mid)] = _parse_overrides_dict(raw_ov)

    return LoadOrderProfile(
        source_path=abs_prof,
        name=str(prof_sec.get("name", "")),
        size_preserve=size_preserve,
        bundle_name=bundle_name,
        explicit_order=explicit_order,
        disabled_ids=disabled_ids,
        mod_entries=entries,
        overrides_by_id=overrides_by_id,
    )


def _find_cycle_path(nodes: List[str], adj: Dict[str, List[str]]) -> List[str]:
    """Find a deterministic cycle path among `nodes` using sorted DFS."""
    node_set = set(nodes)
    state: Dict[str, int] = {n: 0 for n in nodes}  # 0=unvisited, 1=visiting, 2=done
    stack: List[str] = []
    pos_in_stack: Dict[str, int] = {}

    def dfs(u: str) -> Optional[List[str]]:
        state[u] = 1
        pos_in_stack[u] = len(stack)
        stack.append(u)
        for v in sorted(adj.get(u, [])):
            if v not in node_set:
                continue
            if state[v] == 1:
                return stack[pos_in_stack[v] :] + [v]
            if state[v] == 0:
                found = dfs(v)
                if found is not None:
                    return found
        stack.pop()
        del pos_in_stack[u]
        state[u] = 2
        return None

    for n in sorted(nodes):
        if state[n] == 0:
            cyc = dfs(n)
            if cyc is not None:
                return cyc
    return sorted(nodes)


def resolve_load_order(
    mod_paths: Sequence[str] = (),
    *,
    profile_path: Optional[str] = None,
) -> LoadOrderResolution:
    """
    Inspect all candidate mods from `profile_path` and/or `mod_paths`, validate
    uniqueness, dependencies, and declared conflicts, and return a deterministic
    topological load order.
    """
    profile: Optional[LoadOrderProfile] = load_profile(profile_path) if profile_path else None

    # Gather candidate entries: (abs_path, enabled_flag, expected_id, entry_overrides, input_rank)
    candidates: List[Tuple[str, bool, Optional[str], ModOverrides, int]] = []
    seen_norm_paths: set[str] = set()
    rank_counter = 0

    if profile is not None:
        for entry in profile.mod_entries:
            norm_p = os.path.normcase(os.path.abspath(entry.path))
            seen_norm_paths.add(norm_p)
            candidates.append((entry.path, entry.enabled, entry.expected_id, entry.overrides, rank_counter))
            rank_counter += 1

    for raw_mp in mod_paths:
        abs_mp = os.path.abspath(raw_mp)
        norm_p = os.path.normcase(abs_mp)
        if norm_p in seen_norm_paths:
            continue
        seen_norm_paths.add(norm_p)
        candidates.append((abs_mp, True, None, ModOverrides(), rank_counter))
        rank_counter += 1

    if not candidates:
        raise LoadOrderError("No mods specified (pass --mod and/or --profile).")

    explicit_rank_by_id: Dict[str, int] = {}
    if profile is not None:
        for i, mid in enumerate(profile.explicit_order):
            explicit_rank_by_id.setdefault(mid, i)

    disabled_records: List[Dict[str, Any]] = []
    disabled_ids_seen: set[str] = set(profile.disabled_ids) if profile else set()
    enabled_by_id: Dict[str, Tuple[ResolvedMod, int]] = {}

    for path, entry_enabled, expected_id, entry_ov, input_rank in candidates:
        if not entry_enabled:
            rec_id = expected_id
            if rec_id is None and os.path.exists(path):
                with open_mod_source(path) as src:
                    rec_id = load_manifest(src).mod_id
            if rec_id:
                disabled_ids_seen.add(rec_id)
            disabled_records.append(
                {
                    "mod_id": rec_id or "<unknown>",
                    "source_path": os.path.abspath(path),
                    "reason": "disabled in profile [[mods]]",
                }
            )
            continue

        with open_mod_source(path) as src:
            manifest = load_manifest(src)
            kind = src.kind
            src_abs = src.source_path

        if expected_id is not None and manifest.mod_id != expected_id:
            raise LoadOrderError(
                f"Profile expected mod id '{expected_id}' at '{src_abs}', found '{manifest.mod_id}'"
            )

        if profile is not None and manifest.mod_id in profile.disabled_ids:
            disabled_ids_seen.add(manifest.mod_id)
            disabled_records.append(
                {
                    "mod_id": manifest.mod_id,
                    "source_path": src_abs,
                    "reason": "disabled in [profile].disabled",
                }
            )
            continue

        if manifest.mod_id in enabled_by_id:
            prev_mod, _ = enabled_by_id[manifest.mod_id]
            raise DuplicateModError(
                f"Duplicate mod id '{manifest.mod_id}': '{prev_mod.source_path}' and '{src_abs}'"
            )

        merged_ov = ModOverrides()
        merged_ov.merge_from(entry_ov)
        if profile is not None and manifest.mod_id in profile.overrides_by_id:
            merged_ov.merge_from(profile.overrides_by_id[manifest.mod_id])

        # Compute deterministic priority rank: explicit [profile].order first, then candidate order
        if manifest.mod_id in explicit_rank_by_id:
            eff_rank = explicit_rank_by_id[manifest.mod_id]
        else:
            eff_rank = len(explicit_rank_by_id) + input_rank

        resolved = ResolvedMod(
            mod_id=manifest.mod_id,
            name=manifest.name,
            version=manifest.version,
            source_path=src_abs,
            source_kind=kind,
            manifest=manifest,
            overrides=merged_ov,
        )
        enabled_by_id[manifest.mod_id] = (resolved, eff_rank)

    # Validate dependencies & conflicts on all enabled mods (in deterministic mod_id order)
    enabled_ids = sorted(enabled_by_id.keys())
    adj: Dict[str, List[str]] = {mid: [] for mid in enabled_ids}
    edge_set: set[Tuple[str, str]] = set()
    in_degree: Dict[str, int] = {mid: 0 for mid in enabled_ids}

    for mid in enabled_ids:
        mod, _ = enabled_by_id[mid]
        m = mod.manifest

        for req in m.requires:
            if req == mid:
                raise DependencyCycleError(f"Cyclic mod dependency detected: {mid} -> {mid}")
            if req not in enabled_by_id:
                extra = " (explicitly disabled in profile)" if req in disabled_ids_seen else ""
                raise MissingDependencyError(
                    f"Mod '{mid}' requires missing mod '{req}'{extra}"
                )
            if (req, mid) not in edge_set:
                edge_set.add((req, mid))
                adj[req].append(mid)
                in_degree[mid] += 1

        for conf in m.conflicts:
            if conf in enabled_by_id:
                raise ModConflictError(
                    f"Mod '{mid}' declares conflict with enabled mod '{conf}'"
                )

        for after in m.load_after:
            if after == mid:
                raise DependencyCycleError(f"Cyclic load_after constraint detected: {mid} -> {mid}")
            if after in enabled_by_id and (after, mid) not in edge_set:
                edge_set.add((after, mid))
                adj[after].append(mid)
                in_degree[mid] += 1

    # Deterministic topological sort (Kahn's algorithm + min-heap keyed by (eff_rank, mod_id))
    heap: List[Tuple[int, str]] = []
    for mid in enabled_ids:
        if in_degree[mid] == 0:
            _, eff_rank = enabled_by_id[mid]
            heapq.heappush(heap, (eff_rank, mid))

    ordered_ids: List[str] = []
    while heap:
        _, cur = heapq.heappop(heap)
        ordered_ids.append(cur)
        for nxt in sorted(adj[cur]):
            in_degree[nxt] -= 1
            if in_degree[nxt] == 0:
                _, nxt_rank = enabled_by_id[nxt]
                heapq.heappush(heap, (nxt_rank, nxt))

    if len(ordered_ids) != len(enabled_ids):
        remaining = [mid for mid in enabled_ids if in_degree[mid] > 0]
        cycle = _find_cycle_path(remaining, adj)
        raise DependencyCycleError(
            f"Cyclic mod dependency detected: {' -> '.join(cycle)}"
        )

    ordered: List[ResolvedMod] = []
    for idx, mid in enumerate(ordered_ids):
        rm, _ = enabled_by_id[mid]
        rm.load_index = idx
        ordered.append(rm)

    return LoadOrderResolution(
        ordered=ordered,
        disabled=disabled_records,
        profile_path=profile.source_path if profile else None,
        profile_name=profile.name if profile else None,
        size_preserve=profile.size_preserve if profile else None,
        bundle_name=profile.bundle_name if profile else None,
    )
