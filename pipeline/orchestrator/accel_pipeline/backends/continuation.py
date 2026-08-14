"""Provider-specific continuation state for optimization worker runs."""

from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from enum import Enum
from pathlib import Path
from typing import Any


class ProviderContinuationProfile(str, Enum):
    OPENAI_RESPONSE_CHAIN = "openai_response_chain"
    OPENROUTER_IN_MEMORY_HISTORY = "openrouter_in_memory_history"
    LEGACY_STATELESS = "legacy_stateless"


@dataclass
class ContinuationState:
    provider: str = ""
    last_response_id: str | None = None
    runner_invocations: int = 0
    episode_tool_calls: int = 0
    optimization_iter: int = 0
    attempt: int = 0
    seeded: bool = False

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)

    @classmethod
    def from_dict(cls, data: dict[str, Any] | None) -> ContinuationState:
        if not data:
            return cls()
        return cls(
            provider=str(data.get("provider", "") or ""),
            last_response_id=data.get("last_response_id"),
            runner_invocations=int(data.get("runner_invocations", 0) or 0),
            episode_tool_calls=int(data.get("episode_tool_calls", 0) or 0),
            optimization_iter=int(data.get("optimization_iter", 0) or 0),
            attempt=int(data.get("attempt", 0) or 0),
            seeded=bool(data.get("seeded", False)),
        )


def continuation_profile(provider: str) -> ProviderContinuationProfile:
    if provider == "openai":
        return ProviderContinuationProfile.OPENAI_RESPONSE_CHAIN
    if provider == "openrouter":
        return ProviderContinuationProfile.OPENROUTER_IN_MEMORY_HISTORY
    return ProviderContinuationProfile.LEGACY_STATELESS


def resolve_run_kwargs(
    *,
    provider: str,
    last_response_id: str | None,
    auto_previous_response_id: bool,
    is_first_invocation: bool,
) -> dict[str, Any]:
    """Build Runner.run_sync kwargs for provider continuation."""
    if provider == "openai":
        kwargs: dict[str, Any] = {}
        if last_response_id:
            kwargs["previous_response_id"] = last_response_id
        elif auto_previous_response_id and not is_first_invocation:
            kwargs["auto_previous_response_id"] = True
        elif auto_previous_response_id and is_first_invocation:
            kwargs["auto_previous_response_id"] = True
        return kwargs
    return {}


def extract_continuation_from_result(result: Any) -> str | None:
    if result is None:
        return None
    last_id = getattr(result, "last_response_id", None)
    if callable(last_id):
        try:
            return last_id()
        except TypeError:
            pass
    if isinstance(last_id, str) and last_id:
        return last_id
    for attr in ("_previous_response_id", "previous_response_id"):
        value = getattr(result, attr, None)
        if isinstance(value, str) and value:
            return value
    return None


def load_continuation_state(path: Path) -> ContinuationState:
    if not path.is_file():
        return ContinuationState()
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return ContinuationState()
    return ContinuationState.from_dict(data)


def save_continuation_state(path: Path, state: ContinuationState) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state.to_dict(), indent=2), encoding="utf-8")
