"""Episodic optimization: fresh agent sessions + file-backed runtime state."""

from __future__ import annotations

from typing import Any

from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.backends import StageRunRequest, create_agent_backend
from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload, apply_workload_overrides
from accel_pipeline.models import make_model_id
from accel_pipeline.optimization_runtime import (
    append_episode_summary,
    refresh_optimization_runtime,
    runtime_path_for_prompt,
)
from accel_pipeline.pipeline_log import (
    log_stage_complete,
    log_stage_start,
    usage_from_metadata,
)
from accel_pipeline.prompts import render_prompt, save_rendered_prompt
from accel_pipeline.tools.tool_policy import TOOL_USAGE_INSTRUCTIONS

_EPISODE_CONTINUE_INSTRUCTIONS = """\
## Optimization continuation (episode {episode}/{episodes}, iter {optimization_iter})

Read `{runtime_rel}` — sole source of gate metrics, profile signals, A/B trials, episode history, technique memory.

Workflow: profile → hot-path hypothesis → `grep_context` verify → strategy palette → A/B bundle.
Do **not** re-read `03_optimization.md`, gate JSON, or `base_kernel_optimization.md`.

Reply `EPISODE_COMPLETE` when this episode's bundle is done."""


def _episode_instructions(
    *,
    full_prompt: str,
    episode: int,
    episodes: int,
    optimization_iter: int,
    runtime_rel: str,
    provider: str,
) -> str:
    if episode == 1:
        if provider != "cursor":
            return f"{full_prompt.rstrip()}\n\n{TOOL_USAGE_INSTRUCTIONS}"
        return full_prompt
    return _EPISODE_CONTINUE_INSTRUCTIONS.format(
        episode=episode,
        episodes=episodes,
        optimization_iter=optimization_iter,
        runtime_rel=runtime_rel,
    )


def _episode_user_message(
    *,
    episode: int,
    episodes: int,
    optimization_iter: int,
    runtime_rel: str,
    restored: bool,
) -> str:
    if episode == 1:
        msg = (
            f"Optimization iter {optimization_iter}, episode 1/{episodes}.\n"
            f"Read `{runtime_rel}` first, then follow the stage prompt.\n"
            "Prefer `grep_context` over full `read_file` on large kernels.\n"
            "Reply `EPISODE_COMPLETE` when this episode's bundle is done."
        )
        if restored:
            msg = "Pipeline auto-restored the best kernel snapshot. Do not repeat regressed techniques.\n\n" + msg
        return msg
    return (
        f"Continue optimization iter {optimization_iter}, episode {episode}/{episodes}. "
        f"Read `{runtime_rel}` and proceed."
    )


def _merge_usage(total: dict[str, int], metadata: dict[str, Any]) -> None:
    usage = metadata.get("usage") or {}
    for key in ("input_tokens", "output_tokens", "total_tokens", "cache_read_tokens", "cache_write_tokens"):
        total[key] = int(total.get(key, 0) or 0) + int(usage.get(key, 0) or 0)


def run_optimization_iteration(
    *,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    optimization_iter: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    restored: bool = False,
) -> Any:
    workload = load_workload(workload_name, pipeline_config.repo_root)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    episodes = int(getattr(pipeline_config, "optimization_episodes_per_iteration", 4) or 4)
    turns_per_episode = int(getattr(pipeline_config, "optimization_max_turns_per_episode", 8) or 8)

    prompt = render_prompt(
        pipeline_config,
        "03_optimization.md",
        workload,
        query,
        "optimization",
        optimization_iter=optimization_iter,
    )
    save_rendered_prompt(
        pipeline_config,
        query,
        "optimization",
        prompt,
        optimization_iter=optimization_iter,
    )

    if dry_run:
        log_stage_complete(
            pipeline_config.repo_root,
            query,
            stage="optimization",
            model_id="(dry-run)",
            backend="dry-run",
            duration_ms=None,
            usage=None,
            optimization_iter=optimization_iter,
            dry_run=True,
            prompt_chars=len(prompt),
        )
        return {"dry_run": True, "stage": "optimization", "episodes": episodes}

    model_id = make_model_id("optimization", pipeline_config, model_config, model_overrides)
    log_stage_start(
        pipeline_config.repo_root,
        query,
        stage="optimization",
        model_id=model_id,
        optimization_iter=optimization_iter,
    )

    runtime_rel = runtime_path_for_prompt(pipeline_config, workload, query)
    last_result = None
    total_usage: dict[str, int] = {}
    total_duration_ms = 0

    for episode in range(1, episodes + 1):
        refresh_optimization_runtime(
            pipeline_config,
            workload,
            query,
            optimization_iter=optimization_iter,
            episode=episode,
        )

        user_message = _episode_user_message(
            episode=episode,
            episodes=episodes,
            optimization_iter=optimization_iter,
            runtime_rel=runtime_rel,
            restored=restored,
        )

        backend = create_agent_backend(
            provider=model_config.provider,
            pipeline_config=pipeline_config,
            query=query,
            stage="optimization",
            model_config=model_config,
            workload_name=workload_name,
            optimization_iter=optimization_iter,
        )
        instructions = _episode_instructions(
            full_prompt=prompt,
            episode=episode,
            episodes=episodes,
            optimization_iter=optimization_iter,
            runtime_rel=runtime_rel,
            provider=model_config.provider,
        )

        request = StageRunRequest(
            stage="optimization",
            instructions=instructions,
            user_message=user_message,
            agent_name=f"accel_optimization_ep{episode}",
            model_id=model_id,
            max_turns=turns_per_episode,
        )
        result = backend.run_stage(request)
        if result.status != "finished":
            raise RuntimeError(
                f"{backend.name} optimization episode {episode} failed: {result.error or result.status}"
            )
        last_result = result
        append_episode_summary(
            query_artifacts_dir(pipeline_config.repo_root, query, workload=workload),
            episode,
            result.final_text,
        )
        _merge_usage(total_usage, result.metadata)
        duration = result.metadata.get("duration_ms")
        if isinstance(duration, int):
            total_duration_ms += duration

        if "EPISODE_COMPLETE" in (result.final_text or ""):
            break

    log_stage_complete(
        pipeline_config.repo_root,
        query,
        stage="optimization",
        model_id=model_id,
        backend=last_result.backend if last_result else model_config.provider,
        duration_ms=total_duration_ms or None,
        usage=total_usage or None,
        optimization_iter=optimization_iter,
        run_id=getattr(last_result, "run_id", "") if last_result else "",
    )
    return last_result
