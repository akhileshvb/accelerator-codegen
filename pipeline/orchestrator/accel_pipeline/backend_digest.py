"""A deterministic digest of an engine backend in progress, so an agent starting a layer (or a fresh
session after the cap) is told what exists instead of re-reading it: the file map, the public api
symbols per contract role, the decisions and open findings so far, the files the next layer will
probably touch. Built from backends/<engine>/ and the run's artifacts; no model is involved.
Also the per-layer file snapshots the critic's diff is taken against."""

from __future__ import annotations

import difflib
import importlib.util
import json
import re
import shutil
from pathlib import Path
from typing import Any

_SOURCE_SUFFIXES = {".hpp", ".cpp", ".h", ".c", ".cc", ".cmake", ".sh", ".py", ".sql", ".md", ".tpl"}
_COMMENT = re.compile(r"^\s*(?://+|#+|/\*+|\*)\s*(.*?)\s*(?:\*/)?$")
_DIR_ROLES = {"api": "accelerator-facing api", "engine": "engine internals", "state": "backend state",
              "tests": "layer harness or test", "bench": "benchmark driver", "host": "host helper"}
_LOG_TAIL_LINES = 25
_MAX_FILES = 120


def backend_dir(repo_root: Path, engine: str) -> Path:
    return repo_root / "backends" / engine


def _sources(root: Path) -> list[Path]:
    return sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in _SOURCE_SUFFIXES)


def _role(path: Path, root: Path) -> str:
    """The first comment line of the file; else what its directory is for."""
    try:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[:12]:
            match = _COMMENT.match(line)
            if match and len(match.group(1)) > 8 and not match.group(1).startswith(("GENERATED", "SCAFFOLDING", "!/")):
                return match.group(1)[:110]
    except OSError:
        pass
    parts = path.relative_to(root).parts
    return _DIR_ROLES.get(parts[0], "") if len(parts) > 1 else ""


def _file_map(repo_root: Path, engine: str) -> list[str]:
    root = backend_dir(repo_root, engine)
    files = _sources(root) if root.is_dir() else []
    lines = [f"- `{p.relative_to(repo_root)}` ({p.stat().st_size // 1024 + 1} KB): {_role(p, root) or '-'}"
             for p in files[:_MAX_FILES]]
    if len(files) > _MAX_FILES:
        lines.append(f"- ... and {len(files) - _MAX_FILES} more under `backends/{engine}/`")
    return lines or ["- (the backend tree is empty)"]


def _api_symbols(repo_root: Path, engine: str) -> list[str]:
    script = Path(__file__).resolve().parents[2] / "gates" / "check_api_manifest.py"
    manifest = backend_dir(repo_root, engine) / "api" / "README.md"
    if not manifest.is_file() or not script.is_file():
        return []
    spec = importlib.util.spec_from_file_location("_check_api_manifest", script)
    module = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
    spec.loader.exec_module(module)  # type: ignore[union-attr]
    rows = module.parse_roles(manifest.read_text(encoding="utf-8")) or {}
    return [f"- {role}: " + ", ".join(f"`{s}`" for s in symbols[:14]) + (" ..." if len(symbols) > 14 else "")
            for role, (_headers, symbols) in rows.items() if symbols]


def _decisions(art: Path) -> list[str]:
    lines: list[str] = []
    log = art / "experiment_log.md"
    if log.is_file():
        tail = [ln for ln in log.read_text(encoding="utf-8").splitlines() if ln.strip()][-_LOG_TAIL_LINES:]
        lines += ["Experiment log (tail):", *tail]
    for path in sorted(art.glob("layer_*.open_findings.md")):
        lines += ["", f"Open finding `{path.name}`:", path.read_text(encoding="utf-8").strip()[:1500]]
    return lines


def _next_layer_files(art: Path, layer_id: str) -> list[str]:
    try:
        analysis = json.loads((art / "backend_analysis.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []
    for layer in analysis.get("layers") or []:
        if isinstance(layer, dict) and layer.get("id") == layer_id:
            return [str(f) for f in layer.get("files") or []]
    return []


def backend_digest(repo_root: Path, engine: str, art: Path, *, next_layer: str = "") -> str:
    parts = [f"## Backend digest (`backends/{engine}/`, generated from the tree and the run's artifacts)", "",
             "What exists now. Read a file only when you are about to change it or must match its exact "
             "signatures; this digest replaces a survey of the tree.", "", "### Files", *_file_map(repo_root, engine)]
    symbols = _api_symbols(repo_root, engine)
    if symbols:
        parts += ["", "### Public api symbols per contract role (api/README.md)", *symbols]
    decisions = _decisions(art)
    if decisions:
        parts += ["", "### Decisions and open findings so far", *decisions]
    likely = _next_layer_files(art, next_layer) if next_layer else []
    if likely:
        parts += ["", f"### Files layer `{next_layer}` will most likely touch (analysis sketch)",
                  *(f"- `{f}`" for f in likely)]
    return "\n".join(parts) + "\n\n"


def snapshot_backend(repo_root: Path, engine: str, dest: Path) -> None:
    """Copy the backend's sources to `dest` (replacing it): the baseline of a later layer diff."""
    shutil.rmtree(dest, ignore_errors=True)
    root = backend_dir(repo_root, engine)
    for path in _sources(root) if root.is_dir() else []:
        target = dest / path.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    dest.mkdir(parents=True, exist_ok=True)


def backend_diff(repo_root: Path, engine: str, baseline: Path, *, max_chars: int = 60000) -> str:
    """Unified diff of the backend's sources against a snapshot_backend() baseline."""
    root = backend_dir(repo_root, engine)
    now = {p.relative_to(root): p for p in _sources(root)} if root.is_dir() else {}
    before = {p.relative_to(baseline): p for p in _sources(baseline)} if baseline.is_dir() else {}
    chunks: list[str] = []
    for rel in sorted(set(now) | set(before)):
        old = before[rel].read_text(encoding="utf-8", errors="replace").splitlines(keepends=True) if rel in before else []
        new = now[rel].read_text(encoding="utf-8", errors="replace").splitlines(keepends=True) if rel in now else []
        if old != new:
            chunks.append("".join(difflib.unified_diff(
                old, new, f"a/backends/{engine}/{rel}", f"b/backends/{engine}/{rel}", n=3)))
    text = "".join(chunks)
    return text if len(text) <= max_chars else text[:max_chars] + f"\n... [diff truncated at {max_chars} chars]\n"


_BULLET = re.compile(r"^- \*\*", re.MULTILINE)
_LAYER_IDS = re.compile(r"`(\w+)`")


def layer_contracts_digest(contracts: str, layer_id: str, path: str) -> str:
    """layer_contracts.md cut to one layer: the intro, the cross-layer requirements that name this
    layer (or all layers), and the layer's own section. The rest stays on disk at `path`."""
    sections = re.split(r"^(?=## )", contracts, flags=re.MULTILINE)
    intro = sections[0].split("\n---")[0].strip()
    requirements = next((s for s in sections if s.startswith("## Requirements every backend meets")), "")
    own = next((s for s in sections if re.match(rf"## `{re.escape(layer_id)}`", s)), "")
    bullets = _BULLET.split(requirements)[1:]
    kept = []
    for bullet in bullets:
        header = bullet.split("\n", 1)[0]
        scope = header[header.find("(") : header.find(")") + 1] if "(" in header else ""
        if "all layers" in scope or layer_id in _LAYER_IDS.findall(scope):
            kept.append("- **" + bullet.rstrip())
    parts = [intro, "## Requirements that name this layer", *(kept or ["(none beyond the layer section)"]), own.rstrip(),
             f"Other layers' sections, and the requirements above that do not name this layer: `{path}` (read on demand)."]
    return "\n\n".join(p for p in parts if p)
