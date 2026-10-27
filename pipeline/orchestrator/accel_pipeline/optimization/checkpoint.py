"""Kernel checkpoint/restore for optimization attempts."""

from __future__ import annotations

import json
import shutil
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from accel_pipeline.config import PipelineConfig
from accel_pipeline.workload import query_artifacts_dir
from accel_pipeline.optimization.kernel_snapshot import resolve_snapshot_paths

CHECKPOINT_DIRNAME = "attempt_checkpoint"
ACTIVE_ATTEMPT_FILE = "active_attempt.json"
BEST_VALID_BASELINE_FILE = "best_valid_baseline.json"
EXPERIMENTAL_BRANCH_FILE = "experimental_branch.json"


def _checkpoint_dir(art: Path) -> Path:
    return art / CHECKPOINT_DIRNAME


def _checkpoint_files(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> list[Path]:
    repo = pipeline_config.repo_root
    return [p for p in resolve_snapshot_paths(pipeline_config, workload, query) if p.is_file()]


def begin_attempt_checkpoint(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    attempt_id: str,
    hypothesis: str,
) -> list[str]:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    ckpt = _checkpoint_dir(art)
    if ckpt.exists():
        shutil.rmtree(ckpt)
    ckpt.mkdir(parents=True)
    copied: list[str] = []
    for src in _checkpoint_files(pipeline_config, workload, query):
        rel = src.relative_to(pipeline_config.repo_root)
        dest = ckpt / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        copied.append(str(rel))
    (art / ACTIVE_ATTEMPT_FILE).write_text(
        json.dumps(
            {
                "attempt_id": attempt_id,
                "hypothesis": hypothesis,
                "files": copied,
                "ts": datetime.now(timezone.utc).isoformat(),
            },
            indent=2,
        ),
        encoding="utf-8",
    )
    return copied


def revert_attempt_checkpoint(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> list[str]:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    active_path = art / ACTIVE_ATTEMPT_FILE
    if not active_path.is_file():
        return []
    active = json.loads(active_path.read_text(encoding="utf-8"))
    ckpt = _checkpoint_dir(art)
    restored: list[str] = []
    for rel in active.get("files") or []:
        src = ckpt / rel
        if not src.is_file():
            continue
        dest = pipeline_config.repo_root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)
        restored.append(rel)
    clear_attempt_checkpoint(art)
    return restored


def clear_attempt_checkpoint(art: Path) -> None:
    ckpt = _checkpoint_dir(art)
    if ckpt.exists():
        shutil.rmtree(ckpt)
    active = art / ACTIVE_ATTEMPT_FILE
    if active.is_file():
        active.unlink()


def append_attempt_log(art: Path, entry: dict[str, Any]) -> None:
    path = art / "attempt_log.jsonl"
    entry = {**entry, "ts": datetime.now(timezone.utc).isoformat()}
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(entry, ensure_ascii=False) + "\n")
    legacy_path = art / "ab_trials.json"
    trials: list[dict[str, Any]] = []
    if legacy_path.is_file():
        try:
            trials = list(json.loads(legacy_path.read_text(encoding="utf-8")).get("trials") or [])
        except json.JSONDecodeError:
            trials = []
    trials.append(entry)
    legacy_path.write_text(json.dumps({"trials": trials}, indent=2), encoding="utf-8")


def load_best_valid_baseline(art: Path) -> dict[str, Any]:
    path = art / BEST_VALID_BASELINE_FILE
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def save_best_valid_baseline(
    art: Path,
    *,
    attempt_id: str,
    score: float,
    metrics: dict[str, Any],
    checkpoint_files: list[str],
) -> None:
    payload = {
        "attempt_id": attempt_id,
        "score": score,
        "metrics": metrics,
        "checkpoint_files": checkpoint_files,
        "ts": datetime.now(timezone.utc).isoformat(),
    }
    (art / BEST_VALID_BASELINE_FILE).write_text(json.dumps(payload, indent=2), encoding="utf-8")


def advance_best_valid_baseline(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    attempt_id: str,
    score: float,
    metrics: dict[str, Any],
) -> list[str]:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    snapshot_best_valid_baseline_files(pipeline_config, workload, query)
    copied = [
        str(p.relative_to(pipeline_config.repo_root))
        for p in _checkpoint_files(pipeline_config, workload, query)
    ]
    save_best_valid_baseline(
        art,
        attempt_id=attempt_id,
        score=score,
        metrics=metrics,
        checkpoint_files=copied,
    )
    clear_experimental_branch(art)
    # Worktree isolation: mirror accepted owned sources back to the primary tree
    # so the main checkout stays current for non-pack use / next pack reset.
    try:
        from accel_pipeline.pack.worker_worktree import (
            primary_repo_root,
            sync_owned_to_primary,
            using_worktree_isolation,
        )

        if using_worktree_isolation():
            primary = primary_repo_root(pipeline_config.repo_root)
            bench = str(
                workload.get("artifact_namespace")
                or workload.get("benchmark_name")
                or "tpch"
            )
            synced = sync_owned_to_primary(
                primary=primary,
                worktree=Path(pipeline_config.repo_root),
                query=query,
                benchmark=bench,
            )
            if synced:
                import sys

                print(
                    f"[pipeline] synced {len(synced)} owned files to primary after accept",
                    file=sys.stderr,
                    flush=True,
                )
    except Exception as exc:  # noqa: BLE001 — never fail accept on sync
        import sys

        print(f"[pipeline] primary sync after accept failed: {exc}", file=sys.stderr)
    return copied


def snapshot_best_valid_baseline_files(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> None:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    dest_root = art / "best_valid_baseline_snapshot"
    if dest_root.exists():
        shutil.rmtree(dest_root)
    for src in _checkpoint_files(pipeline_config, workload, query):
        rel = src.relative_to(pipeline_config.repo_root)
        dest = dest_root / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest)


def save_experimental_branch(
    art: Path,
    *,
    attempt_id: str,
    chain_depth: int,
    diagnostic_summary: dict[str, Any],
) -> None:
    (art / EXPERIMENTAL_BRANCH_FILE).write_text(
        json.dumps(
            {
                "attempt_id": attempt_id,
                "chain_depth": chain_depth,
                "diagnostic_summary": diagnostic_summary,
                "ts": datetime.now(timezone.utc).isoformat(),
            },
            indent=2,
        ),
        encoding="utf-8",
    )


def load_experimental_branch(art: Path) -> dict[str, Any]:
    path = art / EXPERIMENTAL_BRANCH_FILE
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def clear_experimental_branch(art: Path) -> None:
    path = art / EXPERIMENTAL_BRANCH_FILE
    if path.is_file():
        path.unlink()
