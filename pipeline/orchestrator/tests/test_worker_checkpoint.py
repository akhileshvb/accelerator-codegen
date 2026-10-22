"""Tests for semantic worker checkpoint."""

from __future__ import annotations

import json
from pathlib import Path

from accel_pipeline.optimization_pending import PendingFeedback
from accel_pipeline.optimization_worker_checkpoint import (
    build_semantic_worker_checkpoint,
    load_worker_checkpoint,
    save_worker_checkpoint,
)


def test_checkpoint_save_load(tmp_path: Path):
    art = tmp_path / "art"
    art.mkdir()
    (art / "attempt_log.jsonl").write_text(
        json.dumps(
            {
                "attempt": 1,
                "hypothesis": "decode",
                "verdict": "reject_regress",
                "accepted": False,
            }
        )
        + "\n",
        encoding="utf-8",
    )
    pending = PendingFeedback()
    pending.push("rejected")
    payload = build_semantic_worker_checkpoint(
        art,
        provider="openrouter",
        optimization_iter=1,
        attempt=2,
        seeded=True,
        pending=pending,
        runner_invocations=1,
        episode_tool_calls=5,
    )
    assert payload["provider"] == "openrouter"
    loaded = load_worker_checkpoint(art)
    assert loaded["optimization_iter"] == 1
    assert loaded["pending_feedback"] == "rejected"


def test_no_sqlite_db_created(tmp_path: Path):
    art = tmp_path / "art"
    art.mkdir()
    save_worker_checkpoint(art, {"provider": "openai", "seeded": False})
    dbs = list(art.glob("*.db"))
    assert dbs == []
