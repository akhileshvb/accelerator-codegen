from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any


@dataclass
class StageRunRequest:
    stage: str
    instructions: str
    user_message: str
    agent_name: str
    model_id: str
    agent_model: str | Any | None = None
    max_turns: int = 15
    # resume_session_id / phase_change: continue a codex session (the stage's instructions only on a phase change)
    metadata: dict[str, Any] = field(default_factory=dict)


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

