"""Semantic checkpoint for optimization worker resume (no raw conversation DB)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.jsonio import read_json
from accel_pipeline.optimization.checkpoint import load_experimental_branch
from accel_pipeline.optimization.pending import PendingFeedback


WORKER_SEED_FILE = "optimization_worker_seed.md"
DEFAULT_CHECKPOINT_FILE = "optimization_worker_checkpoint.json"


def checkpoint_path(art_dir: Path, filename: str) -> Path:
    return art_dir / filename


def load_worker_checkpoint(art_dir: Path, filename: str = DEFAULT_CHECKPOINT_FILE) -> dict[str, Any]:
    path = checkpoint_path(art_dir, filename)
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return {}


def save_worker_checkpoint(art_dir: Path, payload: dict[str, Any], filename: str = DEFAULT_CHECKPOINT_FILE) -> Path:
    path = checkpoint_path(art_dir, filename)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return path


def _recent_hypotheses_from_attempt_log(art: Path, limit: int = 8) -> list[dict[str, Any]]:
    path = art / "attempt_log.jsonl"
    if not path.is_file():
        return []
    rows: list[dict[str, Any]] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except json.JSONDecodeError:
            continue
    out: list[dict[str, Any]] = []
    for row in rows[-limit:]:
        hypo = row.get("hypothesis_json") or {}
        out.append(
            {
                "attempt": row.get("attempt"),
                "bottleneck": row.get("hypothesis") or hypo.get("bottleneck", ""),
                "verdict": row.get("verdict", row.get("failure_reason", "")),
                "improvement_pct": row.get("improvement_pct"),
                "accepted": row.get("accepted"),
            }
        )
    return out


def build_semantic_worker_checkpoint(
    art_dir: Path,
    *,
    provider: str,
    optimization_iter: int,
    attempt: int,
    seeded: bool,
    pending: PendingFeedback,
    runner_invocations: int,
    episode_tool_calls: int,
    checkpoint_file: str = DEFAULT_CHECKPOINT_FILE,
) -> dict[str, Any]:
    best_valid = read_json(art_dir / "best_valid_baseline.json")
    baseline_summary = read_json(art_dir / "baseline_summary.json")
    profile_summary = read_json(art_dir / "profile_summary.json")
    experimental = load_experimental_branch(art_dir)

    best_metrics = dict(best_valid.get("metrics") or {})
    if not best_metrics and baseline_summary:
        best_metrics = dict(baseline_summary)

    payload: dict[str, Any] = {
        "provider": provider,
        "optimization_iter": optimization_iter,
        "attempt": attempt,
        "seeded": seeded,
        "best_metrics": best_metrics,
        "hot_path_summary_ref": WORKER_SEED_FILE if (art_dir / WORKER_SEED_FILE).is_file() else "",
        "recent_hypotheses": _recent_hypotheses_from_attempt_log(art_dir),
        "experimental_branch_summary": experimental.get("diagnostic_summary") or {},
        "pending_feedback": pending.peek(),
        "runner_invocations": runner_invocations,
        "episode_tool_calls": episode_tool_calls,
        "profile_summary_ref": "profile_summary.json" if profile_summary else "",
        "baseline_summary_ref": "baseline_summary.json" if baseline_summary else "",
    }
    save_worker_checkpoint(art_dir, payload, checkpoint_file)
    return payload
