from __future__ import annotations

import os
import sys
from pathlib import Path

from accel_pipeline.agent_progress import log_agent_message
from accel_pipeline.backends.base import StageRunRequest, StageRunResult
from accel_pipeline.backends.cursor_timeouts import (
    bridge_retry_count,
    close_agent_safely,
    is_bridge_failure,
    pipeline_cursor_session,
    reset_cursor_bridge,
)
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
        art_dir: Path | None = None,
        compress_outputs: bool = False,
    ) -> None:
        self._repo_root = repo_root
        self._api_key_env = api_key_env
        self._tool_registry = tool_registry
        self._art_dir = art_dir
        self._compress_outputs = compress_outputs

    def run_stage(self, request: StageRunRequest) -> StageRunResult:
        retries = bridge_retry_count()
        last_error: Exception | None = None
        for attempt in range(retries + 1):
            try:
                return self._run_stage_once(request)
            except Exception as exc:
                if not is_bridge_failure(exc) or attempt >= retries:
                    raise
                last_error = exc
                reset_cursor_bridge()
                print(
                    f"[pipeline] cursor bridge failed ({exc}); retrying stage "
                    f"({attempt + 2}/{retries + 1})",
                    file=sys.stderr,
                    flush=True,
                )
        if last_error is not None:
            raise last_error
        raise RuntimeError("cursor stage failed without an error")

    def _run_stage_once(self, request: StageRunRequest) -> StageRunResult:
        from cursor_sdk import Agent, AgentOptions, LocalAgentOptions

        api_key = os.environ.get(self._api_key_env, "")
        if not api_key and os.environ.get("ACCEL_PIPELINE_DRY_RUN") != "1":
            raise EnvironmentError(f"set {self._api_key_env} or use --dry-run")

        custom_tools = build_cursor_custom_tools(
            self._tool_registry,
            art_dir=self._art_dir,
            compress_outputs=self._compress_outputs,
        )
        instructions = f"{request.instructions.rstrip()}\n\n{TOOL_USAGE_INSTRUCTIONS}"
        message = (
            f"# Instructions\n\n{instructions.strip()}\n\n"
            f"# Task\n\n{request.user_message.strip()}"
        )

        with pipeline_cursor_session(self._repo_root) as client:
            agent = Agent.create(
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
                client=client,
            )
            try:
                run = agent.send(message)
                self._stream_progress(run, request.stage)
                result = run.wait()
            finally:
                close_agent_safely(agent)

        status = result.status if result.status in {"finished", "error", "cancelled"} else "error"
        final_text = result.result or ""
        error = ""
        if status != "finished":
            error = final_text or f"run ended with status={status}"
            snippet = (final_text or error).strip()
            if snippet:
                preview = snippet if len(snippet) <= 2000 else snippet[:2000] + "\n... [truncated]"
                print(
                    f"[pipeline] cursor stage {request.stage} ended with status={status}:\n{preview}",
                    file=sys.stderr,
                    flush=True,
                )

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

    @staticmethod
    def _stream_progress(run: object, stage: str) -> None:
        messages = getattr(run, "messages", None)
        if not callable(messages):
            return
        try:
            for message in messages():
                log_agent_message(stage, message)
        except Exception:
            return
