from __future__ import annotations

import os
from pathlib import Path

from accel_pipeline.backends.base import StageRunRequest, StageRunResult
from accel_pipeline.tools.cursor_adapter import build_cursor_custom_tools
from accel_pipeline.tools.tool_policy import TOOL_USAGE_INSTRUCTIONS


class CursorSDKBackend:
    name = "cursor"

    def __init__(
        self,
        *,
        repo_root: Path,
        api_key_env: str,
        tool_registry: dict,
    ) -> None:
        self._repo_root = repo_root
        self._api_key_env = api_key_env
        self._tool_registry = tool_registry

    def run_stage(self, request: StageRunRequest) -> StageRunResult:
        from cursor_sdk import Agent, AgentOptions, LocalAgentOptions

        api_key = os.environ.get(self._api_key_env, "")
        if not api_key and os.environ.get("ACCEL_PIPELINE_DRY_RUN") != "1":
            raise EnvironmentError(f"set {self._api_key_env} or use --dry-run")

        custom_tools = build_cursor_custom_tools(self._tool_registry)
        instructions = f"{request.instructions.rstrip()}\n\n{TOOL_USAGE_INSTRUCTIONS}"
        message = (
            f"# Instructions\n\n{instructions.strip()}\n\n"
            f"# Task\n\n{request.user_message.strip()}"
        )

        with Agent.create(
            AgentOptions(
                name=request.agent_name,
                model=request.model_id,
                api_key=api_key,
                local=LocalAgentOptions(
                    cwd=str(self._repo_root),
                    custom_tools=custom_tools,
                    setting_sources=[],
                ),
            ),
        ) as agent:
            run = agent.send(message)
            result = run.wait()

        status = result.status if result.status in {"finished", "error", "cancelled"} else "error"
        final_text = result.result or ""
        error = ""
        if status != "finished":
            error = final_text or f"run ended with status={status}"

        usage = getattr(result, "usage", None)
        usage_dict: dict[str, int] | None = None
        if usage is not None:
            usage_dict = {
                "input_tokens": int(getattr(usage, "input_tokens", 0) or 0),
                "output_tokens": int(getattr(usage, "output_tokens", 0) or 0),
                "total_tokens": int(getattr(usage, "total_tokens", 0) or 0),
                "cache_read_tokens": int(getattr(usage, "cache_read_tokens", 0) or 0),
                "cache_write_tokens": int(getattr(usage, "cache_write_tokens", 0) or 0),
            }

        return StageRunResult(
            backend=self.name,
            stage=request.stage,
            status=status,
            final_text=final_text,
            raw=result,
            run_id=getattr(result, "id", "") or getattr(run, "id", ""),
            error=error,
            metadata={
                "duration_ms": getattr(result, "duration_ms", None),
                "model": getattr(getattr(result, "model", None), "id", request.model_id),
                "usage": usage_dict,
            },
        )
