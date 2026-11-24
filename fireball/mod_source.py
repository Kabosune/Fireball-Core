"""Dual-mode mod loader supporting both loose dev folders and packaged .gta6mod ZIP archives."""

from __future__ import annotations

import os
import tomllib
import zipfile
from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any, Dict, List


class VirtualModSource(ABC):
    """Unified read-only interface over a loose mod folder or .gta6mod ZIP archive."""

    def __init__(self, source_path: str, kind: str):
        self.source_path = os.path.abspath(source_path)
        self.kind = kind  # "loose" or "zip"

    @abstractmethod
    def read_bytes(self, rel_path: str) -> bytes:
        raise NotImplementedError

    @abstractmethod
    def exists(self, rel_path: str) -> bool:
        raise NotImplementedError

    @abstractmethod
    def list_files(self) -> List[str]:
        raise NotImplementedError

    def read_text(self, rel_path: str) -> str:
        return self.read_bytes(rel_path).decode("utf-8")

    def read_toml(self, rel_path: str) -> Dict[str, Any]:
        return tomllib.loads(self.read_text(rel_path))

    def close(self) -> None:
        pass

    def __enter__(self) -> "VirtualModSource":
        return self

    def __exit__(self, *_exc: object) -> None:
        self.close()


class LooseModSource(VirtualModSource):
    def __init__(self, folder_path: str):
        super().__init__(folder_path, "loose")
        if not os.path.isdir(self.source_path):
            raise FileNotFoundError(f"Loose mod folder not found: {self.source_path}")

    def _resolve(self, rel_path: str) -> str:
        norm = rel_path.replace("\\", "/").lstrip("/")
        full = os.path.abspath(os.path.join(self.source_path, norm))
        if os.path.commonpath([self.source_path, full]) != self.source_path:
            raise ValueError(f"Path traversal blocked: {rel_path}")
        return full

    def read_bytes(self, rel_path: str) -> bytes:
        with open(self._resolve(rel_path), "rb") as f:
            return f.read()

    def exists(self, rel_path: str) -> bool:
        return os.path.isfile(self._resolve(rel_path))

    def list_files(self) -> List[str]:
        out: List[str] = []
        for root, _, files in os.walk(self.source_path):
            for fn in sorted(files):
                abs_p = os.path.join(root, fn)
                rel = os.path.relpath(abs_p, self.source_path).replace("\\", "/")
                out.append(rel)
        return sorted(out)


class ZipModSource(VirtualModSource):
    def __init__(self, zip_path: str):
        super().__init__(zip_path, "zip")
        if not os.path.isfile(self.source_path):
            raise FileNotFoundError(f".gta6mod archive not found: {self.source_path}")
        self._zf = zipfile.ZipFile(self.source_path, "r")
        self._names = {
            info.filename.replace("\\", "/").lstrip("/"): info.filename
            for info in self._zf.infolist()
            if not info.is_dir()
        }

    def read_bytes(self, rel_path: str) -> bytes:
        key = rel_path.replace("\\", "/").lstrip("/")
        if key not in self._names:
            raise FileNotFoundError(f"{rel_path} not found in {self.source_path}")
        return self._zf.read(self._names[key])

    def exists(self, rel_path: str) -> bool:
        key = rel_path.replace("\\", "/").lstrip("/")
        return key in self._names

    def list_files(self) -> List[str]:
        return sorted(self._names.keys())

    def close(self) -> None:
        self._zf.close()


def open_mod_source(path: str) -> VirtualModSource:
    if os.path.isdir(path):
        return LooseModSource(path)
    if os.path.isfile(path) and (path.lower().endswith(".gta6mod") or zipfile.is_zipfile(path)):
        return ZipModSource(path)
    raise ValueError(f"Unsupported mod source (expected directory or .gta6mod ZIP): {path}")


def pack_mod_to_zip(mod_dir: str, out_zip_path: str) -> Dict[str, Any]:
    """Pack a loose mod directory into a reproducible .gta6mod ZIP archive."""
    src = LooseModSource(mod_dir)
    if not src.exists("mod.toml"):
        raise ValueError(f"Missing mod.toml in {mod_dir}")
    os.makedirs(os.path.dirname(os.path.abspath(out_zip_path)), exist_ok=True)
    files = src.list_files()
    packed_count = 0
    with zipfile.ZipFile(out_zip_path, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
        for rel in files:
            if "__pycache__" in rel.split("/") or rel.endswith(".pyc"):
                continue
            info = zipfile.ZipInfo(filename=rel, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            zf.writestr(info, src.read_bytes(rel))
            packed_count += 1
    return {
        "out_path": os.path.abspath(out_zip_path),
        "file_count": packed_count,
        "zip_size": os.path.getsize(out_zip_path),
    }


@dataclass
class ModManifest:
    mod_id: str
    name: str
    version: str
    authors: List[str]
    license: str
    target_game_version: str
    min_fireball_abi: int
    required_tiers: List[int]
    description: str = ""
    requires: List[str] = field(default_factory=list)
    load_after: List[str] = field(default_factory=list)
    conflicts: List[str] = field(default_factory=list)
    xml_patches: List[Dict[str, Any]] = field(default_factory=list)
    kv_patches: List[Dict[str, Any]] = field(default_factory=list)
    pso_patches: List[Dict[str, Any]] = field(default_factory=list)
    plugins: List[Dict[str, Any]] = field(default_factory=list)
    raw: Dict[str, Any] = field(default_factory=dict)


def load_manifest(source: VirtualModSource) -> ModManifest:
    if not source.exists("mod.toml"):
        raise FileNotFoundError(f"mod.toml missing in {source.source_path}")
    data = source.read_toml("mod.toml")
    mod_sec = data.get("mod", {})
    for req in ("id", "name", "version", "target_game_version", "min_fireball_abi"):
        if req not in mod_sec:
            raise ValueError(f"mod.toml missing required field [mod].{req}")

    deps = data.get("dependencies", {})
    patches = data.get("patches", {})
    return ModManifest(
        mod_id=str(mod_sec["id"]),
        name=str(mod_sec["name"]),
        version=str(mod_sec["version"]),
        authors=list(mod_sec.get("authors", [])),
        license=str(mod_sec.get("license", "")),
        target_game_version=str(mod_sec["target_game_version"]),
        min_fireball_abi=int(mod_sec["min_fireball_abi"]),
        required_tiers=[int(t) for t in mod_sec.get("required_tiers", [0])],
        description=str(mod_sec.get("description", "")),
        requires=list(deps.get("requires", [])),
        load_after=list(deps.get("load_after", [])),
        conflicts=list(deps.get("conflicts", [])),
        xml_patches=list(patches.get("xml", [])),
        kv_patches=list(patches.get("kv", [])),
        pso_patches=list(patches.get("pso", [])),
        plugins=list(data.get("plugins", [])),
        raw=data,
    )
