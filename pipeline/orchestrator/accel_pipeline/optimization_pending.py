"""Queued gate feedback for optimization worker (no dedicated model calls)."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class PendingFeedback:
    _delta: str = ""

    def push(self, delta: str) -> None:
        text = (delta or "").strip()
        if text:
            self._delta = text

    def has_pending(self) -> bool:
        return bool(self._delta.strip())

    def peek(self) -> str:
        return self._delta

    def clear(self) -> None:
        self._delta = ""

    def to_dict(self) -> dict[str, str]:
        return {"pending_feedback": self._delta}

    @classmethod
    def from_dict(cls, data: dict[str, Any] | None) -> PendingFeedback:
        if not data:
            return cls()
        return cls(_delta=str(data.get("pending_feedback", "") or ""))

    def flush_into_attempt_prompt(
        self,
        *,
        optimization_iter: int,
        attempt: int,
        experimental_summary: str = "",
        max_chars: int = 4000,
    ) -> str:
        parts = [
            f"Optimization iter {optimization_iter}, attempt {attempt}.",
            "Use read_file/rip_grep to inspect, patch, build, test, and quick-benchmark "
            "before signaling CANDIDATE_READY.",
        ]
        if self._delta.strip():
            parts.append("")
            parts.append("## Prior gate feedback")
            parts.append(self._delta.strip())
            self.clear()
        if experimental_summary.strip():
            parts.append("")
            parts.append("## Experimental branch")
            parts.append(experimental_summary.strip())
        text = "\n".join(parts)
        if max_chars > 0 and len(text) > max_chars:
            return text[: max_chars - 40] + "\n... [truncated attempt prompt]"
        return text


def format_gate_delta(
    *,
    verdict: str,
    reason: str = "",
    hypothesis: dict[str, Any] | None = None,
    improvement_pct: float | None = None,
    reverted: bool = False,
    max_chars: int = 4000,
) -> str:
    bottleneck = (hypothesis or {}).get("bottleneck", "")
    lines = [f"Previous candidate **{verdict}**."]
    if improvement_pct is not None:
        lines.append(f"- improvement_pct: {improvement_pct:+.2f}%")
    if bottleneck:
        lines.append(f"- hypothesis: {bottleneck}")
    if reason:
        lines.append(f"- reason: {reason}")
    if reverted:
        lines.append("- Code reverted to best-valid baseline. Continue with a new approach.")
    text = "\n".join(lines)
    if max_chars > 0 and len(text) > max_chars:
        return text[: max_chars - 40] + "\n... [truncated gate delta]"
    return text
