from __future__ import annotations

from typing import Any

from accel_pipeline.backends.base import AgentBackend, StageRunRequest, StageRunResult


def _wrap_tools(tools: list) -> list:
    from agents import function_tool

    return [function_tool(fn) for fn in tools]


class OpenAIAgentsBackend:
    name = "openai_agents"

    def __init__(self, tools: list[Any]) -> None:
        self._tools = tools

    def run_stage(self, request: StageRunRequest) -> StageRunResult:
        from agents import Agent, Runner

        agent = Agent(
            name=request.agent_name,
            model=request.model_id,
            instructions=request.instructions,
            tools=_wrap_tools(self._tools),
        )
        result = Runner.run_sync(agent, request.user_message)
        final_text = _extract_final_text(result)
        return StageRunResult(
            backend=self.name,
            stage=request.stage,
            status="finished",
            final_text=final_text,
            raw=result,
        )


def _extract_final_text(result: Any) -> str:
    if result is None:
        return ""
    for attr in ("final_output", "output", "result"):
        value = getattr(result, attr, None)
        if isinstance(value, str) and value.strip():
            return value
    return str(result)
