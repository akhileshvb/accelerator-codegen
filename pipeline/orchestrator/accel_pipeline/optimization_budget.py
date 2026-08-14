"""Per-attempt tool and turn budgets for optimization sub-stages."""

from __future__ import annotations

import functools
import inspect
import json
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

from accel_pipeline.paths import query_artifacts_dir


@dataclass
class AttemptBudget:
    max_file_reads: int = 3
    max_grep_calls: int = 6
    max_tool_calls: int = 12

    def to_dict(self) -> dict[str, int]:
        return {
            "max_file_reads": self.max_file_reads,
            "max_grep_calls": self.max_grep_calls,
            "max_tool_calls": self.max_tool_calls,
        }


@dataclass
class BudgetTracker:
    budget: AttemptBudget
    file_reads: int = 0
    grep_calls: int = 0
    tool_calls: int = 0

    def record(self, tool_name: str) -> str | None:
        unlimited = (
            self.budget.max_tool_calls <= 0
            and self.budget.max_file_reads <= 0
            and self.budget.max_grep_calls <= 0
        )
        self.tool_calls += 1
        if not unlimited and self.tool_calls > self.budget.max_tool_calls:
            return f"ERROR: budget exhausted (max_tool_calls={self.budget.max_tool_calls})"
        if tool_name in {"read_file"}:
            self.file_reads += 1
            if not unlimited and self.file_reads > self.budget.max_file_reads:
                return f"ERROR: budget exhausted (max_file_reads={self.budget.max_file_reads})"
        if tool_name in {"grep_context", "rip_grep", "ast_grep", "search_repo"}:
            self.grep_calls += 1
            if not unlimited and self.grep_calls > self.budget.max_grep_calls:
                return f"ERROR: budget exhausted (max_grep_calls={self.budget.max_grep_calls})"
        return None

    def remaining(self) -> dict[str, int]:
        return {
            "file_reads": max(0, self.budget.max_file_reads - self.file_reads),
            "grep_calls": max(0, self.budget.max_grep_calls - self.grep_calls),
            "tool_calls": max(0, self.budget.max_tool_calls - self.tool_calls),
        }


def budget_from_pipeline(pipeline_config: Any, *, phase: str = "analysis") -> AttemptBudget:
    raw = dict(getattr(pipeline_config, "optimization_attempt_budgets", None) or {})
    if phase == "patch":
        patch_raw = dict(getattr(pipeline_config, "optimization_patch_budgets", None) or {})
        raw = {**raw, **patch_raw}
    return AttemptBudget(
        max_file_reads=int(raw.get("max_file_reads", 3)),
        max_grep_calls=int(raw.get("max_grep_calls", 6)),
        max_tool_calls=int(raw.get("max_tool_calls", 12)),
    )


def wrap_with_budget(
    registry: dict[str, Callable[..., Any]],
    tracker: BudgetTracker,
) -> dict[str, Callable[..., str]]:
    wrapped: dict[str, Callable[..., str]] = {}

    def _wrap(name: str, fn: Callable[..., Any]) -> Callable[..., str]:
        signature = inspect.signature(fn)

        @functools.wraps(fn)
        def inner(*args: Any, **kwargs: Any) -> str:
            err = tracker.record(name)
            if err:
                return err
            result = fn(*args, **kwargs)
            return result if isinstance(result, str) else json.dumps(result, indent=2)

        inner.__signature__ = signature
        return inner

    for name, fn in registry.items():
        wrapped[name] = _wrap(name, fn)
    return wrapped


def save_attempt_state(art: Path, state: dict[str, Any]) -> None:
    (art / "attempt_state.json").write_text(json.dumps(state, indent=2), encoding="utf-8")


def load_attempt_state(art: Path) -> dict[str, Any]:
    path = art / "attempt_state.json"
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


def art_dir(pipeline_config: Any, workload: dict, query: int) -> Path:
    return query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
