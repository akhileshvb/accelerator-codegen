"""A failure any stage can hand to the repair loop (accel_pipeline.repair)."""

from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any, Literal

if TYPE_CHECKING:
    from accel_pipeline.gates import GateResult

FailureKind = Literal["code", "infra", "timeout", "measurement", "orchestrator"]


@dataclass
class Failure:
    kind: FailureKind
    stage: str
    source: str
    summary: str
    evidence: str = ""
    reproduce: str = ""
    infra_reason: str | None = None
    diagnosis: list[str] = field(default_factory=list)


class StageFailure(Exception):
    def __init__(self, failure: Failure):
        super().__init__(failure.summary)
        self.failure = failure


def render_failure(f: Failure) -> str:
    """The markdown report fed to the agent."""
    lines = [f"## {f.kind} failure in {f.stage} ({f.source})", "", f.summary]
    if f.diagnosis:
        lines += ["", *(f"- {d}" for d in f.diagnosis)]
    if f.reproduce:
        lines += ["", f"Reproduce: `{f.reproduce}`"]
    if f.evidence:
        lines += ["", "```", f.evidence[-6000:], "```"]
    return "\n".join(lines)


_CRASH_SIGNALS = re.compile(
    r"(?i)(segmentation fault|sigsegv|sigabrt|internalexception|"
    r"assertion failed|stack trace|sanitizer|core dumped|"
    r"undefined reference|error:|fatal error:|terminate called|"
    r"tests passed.*failed|test.*failed)"
)


def extract_crash_highlights(log_tail: str, *, max_lines: int = 40) -> list[str]:
    if not log_tail:
        return []
    lines = log_tail.splitlines()
    highlighted = [line.strip() for line in lines if _CRASH_SIGNALS.search(line)]
    if highlighted:
        return highlighted[-max_lines:]
    return [line.strip() for line in lines[-max_lines:] if line.strip()]


def gate_log_tail(gate_data: dict[str, Any], *, max_chars: int = 6000) -> str:
    for key in ("log_tail", "stderr", "stdout", "notes", "reason"):
        text = str(gate_data.get(key) or "").strip()
        if text:
            return text[-max_chars:]
    return ""


def failure_from_gate(
    result: GateResult,
    *,
    stage: str,
    workload: dict[str, Any] | None = None,
    repo_root: Path | None = None,
    load_factor: float = 1.5,
) -> Failure:
    """A failed gate as a Failure, triaged as infra or code."""
    from accel_pipeline.gates import gate_failure_message
    from accel_pipeline.triage import classify

    workload = workload or {}
    kind, reason = classify(result.name, result.data, workload, repo_root, load_factor=load_factor)
    tail = gate_log_tail(result.data)
    reproduce = ""
    if workload.get("test_target"):
        reproduce = (f"cmake --build build-release --target {workload['test_target']} -j$(nproc) && "
                     f"ctest -R {workload.get('ctest_smoke_regex', '')}")
    return Failure(
        kind=kind,  # type: ignore[arg-type]
        stage=stage,
        source=f"gate:{result.name}",
        summary=gate_failure_message(result),
        evidence=tail,
        reproduce=reproduce,
        infra_reason=reason,
        diagnosis=[f"`{line}`" for line in extract_crash_highlights(tail, max_lines=15)],
    )
