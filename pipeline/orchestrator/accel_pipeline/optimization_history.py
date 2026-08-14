"""In-memory compact history for OpenRouter optimization worker episodes."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


def _user_item(content: str) -> dict[str, Any]:
    return {"role": "user", "content": content}


def _assistant_item(content: str) -> dict[str, Any]:
    return {"role": "assistant", "content": content}


@dataclass
class _CycleRecord:
    user_msg: str
    assistant_output: str
    tool_summary: str


@dataclass
class OpenRouterHistoryManager:
    seed_message: str
    max_chars: int = 120_000
    preserve_recent_cycles: int = 2
    tool_output_summary_chars: int = 80
    _cycles: list[_CycleRecord] = field(default_factory=list)
    _summaries: list[str] = field(default_factory=list)

    def record_cycle(self, user_msg: str, assistant_output: str, tool_summary: str = "") -> None:
        summary = (tool_summary or "").strip()
        if len(summary) > self.tool_output_summary_chars:
            summary = summary[: self.tool_output_summary_chars] + "..."
        self._cycles.append(
            _CycleRecord(
                user_msg=user_msg.strip(),
                assistant_output=(assistant_output or "").strip(),
                tool_summary=summary,
            )
        )

    def char_count(self) -> int:
        return len(self.build_input("").__repr__())  # approximate via serialization

    def _cycle_to_lines(self, cycle: _CycleRecord, *, include_user: bool = True) -> list[str]:
        lines: list[str] = []
        if include_user and cycle.user_msg:
            lines.append(f"User: {cycle.user_msg}")
        if cycle.tool_summary:
            lines.append(f"Tools: {cycle.tool_summary}")
        if cycle.assistant_output:
            lines.append(f"Assistant: {cycle.assistant_output}")
        return lines

    def compact(self) -> tuple[int, int]:
        """Compact between Runner invocations. Returns (chars_before, chars_after)."""
        before = self._estimate_chars()
        while len(self._cycles) > self.preserve_recent_cycles:
            old = self._cycles.pop(0)
            hypo_line = ""
            for line in old.assistant_output.splitlines():
                if "bottleneck" in line.lower():
                    hypo_line = line.strip()[:120]
                    break
            summary = hypo_line or (old.assistant_output[:80] + "..." if old.assistant_output else "cycle")
            self._summaries.append(f"- {summary}")
        while self._estimate_chars() > self.max_chars and self._summaries:
            self._summaries.pop(0)
        while self._estimate_chars() > self.max_chars and len(self._cycles) > 1:
            old = self._cycles.pop(0)
            self._summaries.append(f"- {(old.assistant_output or 'cycle')[:80]}")
        after = self._estimate_chars()
        return before, after

    def _estimate_chars(self) -> int:
        total = len(self.seed_message)
        total += sum(len(s) for s in self._summaries)
        for cycle in self._cycles:
            total += len(cycle.user_msg) + len(cycle.assistant_output) + len(cycle.tool_summary)
        return total

    def build_input(self, new_user_message: str) -> list[dict[str, Any]]:
        items: list[dict[str, Any]] = []
        if self.seed_message.strip():
            items.append(_user_item(self.seed_message.strip()))
        if self._summaries:
            items.append(
                _user_item(
                    "## Prior cycle summaries\n" + "\n".join(self._summaries[-20:])
                )
            )
        start = max(0, len(self._cycles) - self.preserve_recent_cycles)
        for cycle in self._cycles[start:]:
            if cycle.user_msg:
                items.append(_user_item(cycle.user_msg))
            if cycle.tool_summary:
                items.append(_user_item(f"[tool summary] {cycle.tool_summary}"))
            if cycle.assistant_output:
                items.append(_assistant_item(cycle.assistant_output))
        if new_user_message.strip():
            items.append(_user_item(new_user_message.strip()))
        return items

    def drop_last_cycle(self) -> None:
        if self._cycles:
            self._cycles.pop()
