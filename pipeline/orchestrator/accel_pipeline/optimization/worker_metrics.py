"""Aggregate metrics for optimization worker episodes."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any


@dataclass
class WorkerMetrics:
    runner_invocations: int = 0
    episode_tool_calls: int = 0
    input_tokens: int = 0
    cache_read_tokens: int = 0

    def record_run(
        self,
        *,
        tool_calls: int,
        usage: dict[str, int] | None,
    ) -> None:
        self.runner_invocations += 1
        self.episode_tool_calls += tool_calls
        if usage:
            self.input_tokens += int(usage.get("input_tokens", 0) or 0)
            self.cache_read_tokens += int(usage.get("cache_read_tokens", 0) or 0)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict[str, Any] | None) -> WorkerMetrics:
        if not data:
            return cls()
        return cls(
            runner_invocations=int(data.get("runner_invocations", 0) or 0),
            episode_tool_calls=int(data.get("episode_tool_calls", 0) or 0),
            input_tokens=int(data.get("input_tokens", 0) or 0),
            cache_read_tokens=int(data.get("cache_read_tokens", 0) or 0),
        )


def save_worker_metrics(art_dir: Path, metrics: WorkerMetrics) -> Path:
    path = art_dir / "optimization_worker_metrics.json"
    path.write_text(json.dumps(metrics.to_dict(), indent=2), encoding="utf-8")
    return path
