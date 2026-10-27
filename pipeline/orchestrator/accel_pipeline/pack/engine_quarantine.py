"""Quarantine foreign engine trees from the agent's view.

Codex runs with full sandbox bypass for MCP, so prompt denylists are not enough.
Moving trees out of the workspace is the reliable hide: other engines' backends
during an engine_backend run, and a query's other engines (its accelerator
and artifact trees) during an accelerator run.
"""

from __future__ import annotations

import json
import os
import shutil
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.config import workload_engines
from accel_pipeline.layout import query_dir


@dataclass
class QuarantineSession:
    repo_root: Path
    quarantine_root: Path
    moved: list[tuple[str, str]]  # (rel_src, rel_dst under quarantine)
    manifest_path: Path

    def restore(self) -> None:
        for rel_src, rel_dst in reversed(self.moved):
            src = self.repo_root / rel_src
            dst = self.quarantine_root / rel_dst
            if src.exists():
                continue
            if not dst.exists():
                continue
            src.parent.mkdir(parents=True, exist_ok=True)
            shutil.move(str(dst), str(src))
        if self.manifest_path.is_file():
            self.manifest_path.unlink()
        # Clean empty quarantine dirs
        if self.quarantine_root.is_dir() and not any(self.quarantine_root.rglob("*")):
            shutil.rmtree(self.quarantine_root, ignore_errors=True)


def quarantine_root_for(repo_root: Path) -> Path:
    return Path(repo_root) / "pipeline" / ".quarantine" / "foreign_engines"


def sibling_engine_trees(workload: dict[str, Any]) -> list[str]:
    """The query's trees of the other engines its workload supports (independent runs)."""
    engine = str(workload.get("engine_name") or "")
    bench = str(workload.get("benchmark_name") or "tpch")
    query = workload.get("query_number")
    return [
        tree
        for other in workload_engines(workload)
        if other != engine
        for tree in (
            f"{query_dir(bench, query)}/{other}",
            f"pipeline/artifacts/{bench}/q{query}/{other}",
        )
    ]


def foreign_trees_for_workload(workload: dict[str, Any]) -> list[str]:
    """Return repo-relative trees to hide (never the target engine)."""
    if workload.get("workload_kind") != "engine_backend":
        return sibling_engine_trees(workload)
    engine = str(workload.get("engine_name") or "").strip()
    configured = workload.get("agent_hidden_trees")
    if isinstance(configured, list) and configured:
        trees = [str(p).strip().rstrip("/") for p in configured if str(p).strip()]
    else:
        trees = []
    # Never quarantine the engine being generated.
    if engine:
        trees = [
            t
            for t in trees
            if f"/{engine}" not in f"/{t}" and not t.endswith(f"/{engine}") and t != engine
        ]
    return trees


def apply_foreign_engine_quarantine(
    repo_root: Path,
    workload: dict[str, Any],
    *,
    enabled: bool | None = None,
) -> QuarantineSession | None:
    """Move foreign engine trees out of the repo. Returns session or None if disabled/noop."""
    if enabled is None:
        enabled = bool(workload.get("hide_foreign_engines", True))
        env = os.environ.get("ACCEL_HIDE_FOREIGN_ENGINES", "").strip().lower()
        if env in {"0", "false", "no", "off"}:
            enabled = False
        elif env in {"1", "true", "yes", "on"}:
            enabled = True
    if not enabled:
        return None

    root = Path(repo_root).resolve()
    qroot = quarantine_root_for(root)
    qroot.mkdir(parents=True, exist_ok=True)
    moved: list[tuple[str, str]] = []
    for rel in foreign_trees_for_workload(workload):
        src = root / rel
        if not src.exists():
            continue
        dst_rel = rel.replace("/", "__")
        dst = qroot / dst_rel
        if dst.exists():
            shutil.rmtree(dst)
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(src), str(dst))
        moved.append((rel, dst_rel))

    manifest = qroot / "manifest.json"
    session = QuarantineSession(
        repo_root=root,
        quarantine_root=qroot,
        moved=moved,
        manifest_path=manifest,
    )
    manifest.write_text(
        json.dumps({"moved": [{"src": a, "dst": b} for a, b in moved]}, indent=2) + "\n",
        encoding="utf-8",
    )
    if moved:
        print(
            f"[pipeline] quarantined {len(moved)} foreign engine tree(s) under {qroot}",
            flush=True,
        )
    return session


def restore_quarantine(session: QuarantineSession | None) -> None:
    if session is None:
        return
    session.restore()
    if session.moved:
        print(
            f"[pipeline] restored {len(session.moved)} quarantined engine tree(s)",
            flush=True,
        )
