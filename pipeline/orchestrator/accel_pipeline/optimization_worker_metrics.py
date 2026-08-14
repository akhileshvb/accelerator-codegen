"""Aggregate metrics for optimization worker episodes."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any


@dataclass
class WorkerMetrics:
    runner_invocations: int = 0
    episode_tool_calls: int = 0
    input_tokens: int = 0
    output_tokens: int = 0
    cache_read_tokens: int = 0
    total_wall_clock_ms: int = 0
    candidates_ready: int = 0
    candidates_cannot_test: int = 0
    candidates_failed: int = 0
    auth_accepts: int = 0
    auth_rejects: int = 0
    phase_timings_ms: dict[str, int] = field(default_factory=dict)

    def record_run(
        self,
        *,
        tool_calls: int,
        usage: dict[str, int] | None,
        wall_clock_ms: int,
    ) -> None:
        self.runner_invocations += 1
        self.episode_tool_calls += tool_calls
        self.total_wall_clock_ms += wall_clock_ms
        if usage:
            self.input_tokens += int(usage.get("input_tokens", 0) or 0)
            self.output_tokens += int(usage.get("output_tokens", 0) or 0)
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
            output_tokens=int(data.get("output_tokens", 0) or 0),
            cache_read_tokens=int(data.get("cache_read_tokens", 0) or 0),
            total_wall_clock_ms=int(data.get("total_wall_clock_ms", 0) or 0),
            candidates_ready=int(data.get("candidates_ready", 0) or 0),
            candidates_cannot_test=int(data.get("candidates_cannot_test", 0) or 0),
            candidates_failed=int(data.get("candidates_failed", 0) or 0),
            auth_accepts=int(data.get("auth_accepts", 0) or 0),
            auth_rejects=int(data.get("auth_rejects", 0) or 0),
            phase_timings_ms=dict(data.get("phase_timings_ms") or {}),
        )


def save_worker_metrics(art_dir: Path, metrics: WorkerMetrics) -> Path:
    path = art_dir / "optimization_worker_metrics.json"
    path.write_text(json.dumps(metrics.to_dict(), indent=2), encoding="utf-8")
    return path
