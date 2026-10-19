from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Protocol


@dataclass
class StageRunRequest:
    stage: str
    instructions: str
    user_message: str
    agent_name: str
    model_id: str
    agent_model: str | Any | None = None
    max_turns: int = 15


@dataclass
class StageRunResult:
    backend: str
    stage: str
    status: str
    final_text: str = ""
    raw: Any = None
    run_id: str = ""
    error: str = ""
    metadata: dict[str, Any] = field(default_factory=dict)


class AgentBackend(Protocol):
    name: str

    def run_stage(self, request: StageRunRequest) -> StageRunResult: ...
