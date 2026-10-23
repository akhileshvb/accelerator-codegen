"""A failure any stage can hand to the repair loop (accel_pipeline.repair)."""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Literal

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
