from __future__ import annotations

import os
import sys
import time
from collections.abc import Callable
from pathlib import Path
from typing import Any

from accel_pipeline.agent_run_hooks import PipelineLoggingHooks, log_agent_run_summary, summarize_run_items
from accel_pipeline.backends.base import AgentBackend, StageRunRequest, StageRunResult
from accel_pipeline.tools.pipeline_tool_wrapper import wrap_tool_registry

_TRANSIENT_API_ERROR_MARKERS = (
    "unable to get json response",
    "expecting value",
    "connection reset",
    "connection error",
    "timeout",
    "rate limit",
    "overloaded",
    "bad gateway",
    "service unavailable",
    "internal server error",
    "temporarily unavailable",
)


def _is_transient_api_error(exc: BaseException) -> bool:
    message = str(exc).lower()
    if any(marker in message for marker in _TRANSIENT_API_ERROR_MARKERS):
        return True
    name = type(exc).__name__.lower()
    return name in {"apierror", "timeouterror", "connecterror", "ratelimiterror", "serviceunavailableerror"}


def _wrap_tools(tools: list[Callable[..., Any]]) -> list[Any]:
    from agents import function_tool

    return [function_tool(fn) for fn in tools]


def _extract_final_text(result: Any) -> str:
    if result is None:
        return ""
    for attr in ("final_output", "output", "result"):
        value = getattr(result, attr, None)
        if isinstance(value, str) and value.strip():
            return value
    return str(result)


def _extract_usage(result: Any) -> dict[str, int] | None:
    wrapper = getattr(result, "context_wrapper", None)
    usage = getattr(wrapper, "usage", None) if wrapper is not None else None
    if usage is None:
        return None
    cached = 0
    cache_write = 0
    details = getattr(usage, "input_tokens_details", None)
    if details is not None:
        cached = int(getattr(details, "cached_tokens", 0) or 0)
        cache_write = int(getattr(details, "cache_write_tokens", 0) or 0)
    input_tokens = int(getattr(usage, "input_tokens", 0) or 0)
    output_tokens = int(getattr(usage, "output_tokens", 0) or 0)
    total_tokens = int(getattr(usage, "total_tokens", 0) or 0) or (input_tokens + output_tokens)
    return {
        "input_tokens": input_tokens,
        "output_tokens": output_tokens,
        "total_tokens": total_tokens,
        "cache_read_tokens": cached,
        "cache_write_tokens": cache_write,
    }


def _resolve_agent_model(request: StageRunRequest) -> str | Any:
    if request.agent_model is not None:
        return request.agent_model
    return request.model_id


class OpenAIAgentsBackend:
    name = "openai_agents"

    def __init__(
        self,
        *,
        provider: str,
        tools: list[Any],
        art_dir: Path | None = None,
        compress_outputs: bool = False,
    ) -> None:
        self._provider = provider
        self._tools = tools
        self._art_dir = art_dir
        self._compress_outputs = compress_outputs
        self._cached_agent: Any | None = None
        self._cached_agent_key: tuple[str, str, str] | None = None

    def _get_or_create_agent(self, request: StageRunRequest) -> Any:
        from agents import Agent

        model = _resolve_agent_model(request)
        model_key = getattr(model, "model", None) or str(model)
        key = (request.agent_name, request.instructions, model_key)
        if self._cached_agent is not None and self._cached_agent_key == key:
            return self._cached_agent
        agent = Agent(
            name=request.agent_name,
            model=model,
            instructions=request.instructions,
            tools=_wrap_tools(self._tools),
        )
        self._cached_agent = agent
        self._cached_agent_key = key
        return agent

    def run_stage(self, request: StageRunRequest) -> StageRunResult:
        from agents import RunConfig, Runner
        from agents.exceptions import AgentsException, MaxTurnsExceeded

        agent = self._get_or_create_agent(request)
        if request.run_input is not None:
            message = request.run_input
        else:
            message = request.user_message.strip()
            if not message:
                message = "Follow the instructions above and complete the stage task."

        hooks = PipelineLoggingHooks(request.stage)
        max_attempts = max(1, int(os.environ.get("ACCEL_PIPELINE_API_RETRIES", "3")))
        last_error: BaseException | None = None
        run_kwargs: dict[str, Any] = {
            "max_turns": request.max_turns,
            "run_config": RunConfig(tracing_disabled=True),
            "hooks": hooks,
        }
        if request.previous_response_id:
            run_kwargs["previous_response_id"] = request.previous_response_id
        if request.auto_previous_response_id:
            run_kwargs["auto_previous_response_id"] = True

        for attempt in range(1, max_attempts + 1):
            try:
                result = Runner.run_sync(
                    agent,
                    message,
                    **run_kwargs,
                )
                break
            except MaxTurnsExceeded as exc:
                run_data = getattr(exc, "run_data", None)
                new_items = list(getattr(run_data, "new_items", None) or [])
                log_agent_run_summary(
                    request.stage,
                    status="max_turns_exceeded",
                    max_turns=request.max_turns,
                    turns_used=hooks._turn,
                    new_items=new_items,
                )
                return StageRunResult(
                    backend=self.name,
                    stage=request.stage,
                    status="error",
                    final_text=str(exc),
                    error=f"max_turns ({request.max_turns}) exceeded",
                    metadata={"provider": self._provider},
                )
            except AgentsException as exc:
                return StageRunResult(
                    backend=self.name,
                    stage=request.stage,
                    status="error",
                    final_text=str(exc),
                    error=str(exc),
                    metadata={"provider": self._provider},
                )
            except Exception as exc:
                last_error = exc
                if attempt >= max_attempts or not _is_transient_api_error(exc):
                    raise
                delay_s = min(60, 5 * (2 ** (attempt - 1)))
                print(
                    f"[pipeline] transient API error on attempt {attempt}/{max_attempts}; "
                    f"retrying in {delay_s}s: {exc}",
                    file=sys.stderr,
                )
                time.sleep(delay_s)
        else:
            raise last_error or RuntimeError("agent run failed without result")

        final_text = _extract_final_text(result)
        run_summary = summarize_run_items(list(getattr(result, "new_items", None) or []))
        log_agent_run_summary(
            request.stage,
            status="finished",
            max_turns=request.max_turns,
            turns_used=hooks._turn,
            new_items=list(getattr(result, "new_items", None) or []),
        )
        interruptions = list(getattr(result, "interruptions", None) or [])
        if interruptions:
            pending = ", ".join(getattr(item, "name", "tool") for item in interruptions[:5])
            return StageRunResult(
                backend=self.name,
                stage=request.stage,
                status="error",
                final_text=final_text,
                raw=result,
                error=f"run paused for tool approval: {pending}",
                metadata={"provider": self._provider, "usage": _extract_usage(result)},
            )

        return StageRunResult(
            backend=self.name,
            stage=request.stage,
            status="finished",
            final_text=final_text,
            raw=result,
            metadata={
                "provider": self._provider,
                "usage": _extract_usage(result),
                "turns": getattr(result, "_current_turn", None),
                "tool_calls": run_summary.get("tool_call_count", 0),
                "last_response_id": _extract_last_response_id(result),
            },
        )


def _extract_last_response_id(result: Any) -> str | None:
    from accel_pipeline.backends.continuation import extract_continuation_from_result

    return extract_continuation_from_result(result)


def build_openai_agents_backend(
    *,
    provider: str,
    tool_registry: dict[str, Any],
    stage_tool_names: list[str],
    art_dir: Path | None = None,
    compress_outputs: bool = False,
) -> OpenAIAgentsBackend:
    wrapped = wrap_tool_registry(
        {name: tool_registry[name] for name in stage_tool_names if name in tool_registry},
        art_dir=art_dir,
        compress_outputs=compress_outputs,
    )
    tools = [wrapped[name] for name in stage_tool_names if name in wrapped]
    return OpenAIAgentsBackend(
        provider=provider,
        tools=tools,
        art_dir=art_dir,
        compress_outputs=compress_outputs,
    )


def require_litellm() -> None:
    try:
        import litellm  # noqa: F401
    except ImportError as exc:
        raise ImportError(
            "litellm is required for provider 'openrouter'; "
            "install with: pip install -e '.[openrouter]'"
        ) from exc


def build_openrouter_model(
    preset: str,
    *,
    api_key_env: str,
    base_url: str = "https://openrouter.ai/api/v1",
) -> Any:
    require_litellm()
    import litellm
    from agents.extensions.models.litellm_model import LitellmModel

    timeout_s = float(os.environ.get("ACCEL_LLM_REQUEST_TIMEOUT", "180"))
    litellm.request_timeout = timeout_s

    api_key = os.environ.get(api_key_env, "")
    if not api_key and os.environ.get("ACCEL_PIPELINE_DRY_RUN") != "1":
        raise EnvironmentError(f"set {api_key_env} or use --dry-run")

    model_name = preset if preset.startswith("openrouter/") else f"openrouter/{preset}"
    return LitellmModel(model=model_name, api_key=api_key or None, base_url=base_url)
