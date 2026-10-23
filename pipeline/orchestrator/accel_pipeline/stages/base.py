from __future__ import annotations

from dataclasses import replace
from pathlib import Path
from typing import Any

from accel_pipeline.backends import StageRunRequest, create_agent_backend
from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload, apply_workload_overrides
from accel_pipeline.models import make_model_id
from accel_pipeline.agent_progress import begin_agent_run
from accel_pipeline.pipeline_log import (
    log_stage_complete,
    log_stage_start,
    usage_from_metadata,
)
from accel_pipeline.prompts import render_prompt, save_rendered_prompt, with_codex_tooling
from accel_pipeline.frozen_paths import restore as restore_frozen, snapshot as snapshot_frozen
from accel_pipeline.playbook import append_experiment_log

_UNLIMITED_STAGE_TURNS = 10_000


def resolve_stage_max_turns(stage_max_turns: dict[str, int], stage: str, default: int = 15) -> int:
    if stage not in stage_max_turns and stage.startswith("impl_"):
        raw = int(stage_max_turns.get("implementation", default))
    elif stage not in stage_max_turns and (
        stage == "layer_critique" or stage.startswith("critique_")
    ):
        raw = int(stage_max_turns.get("layer_critique", default))
    else:
        raw = int(stage_max_turns.get(stage, default))
    if raw <= 0:
        return _UNLIMITED_STAGE_TURNS
    return raw


def resolve_optimization_max_turns(pipeline_config: Any, phase: str) -> int:
    """Resolve optimization sub-stage turns; 0 or unset means unlimited."""
    key = f"optimization_max_turns_{phase}"
    raw = int(getattr(pipeline_config, key, 0) or 0)
    if raw <= 0:
        return _UNLIMITED_STAGE_TURNS
    return raw


def run_agent_stage(
    *,
    stage: str,
    prompt_file: str,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    user_message: str,
    model_overrides: dict[str, str] | None = None,
    optimization_iter: int = 0,
    dry_run: bool = False,
    analysis_revision_request: str = "",
    extra_context: dict[str, str] | None = None,
    must_change: Path | None = None,
) -> Any:
    """Run one agent stage. With `must_change`, a run that leaves every file under that
    directory as it was is re-invoked (twice at most): agents run unattended, so a turn that
    ends with a question or a plan instead of edits did nothing."""
    workload = load_run_workload(pipeline_config, workload_name)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    if not dry_run:
        # pick up primitives other workers published (no-op outside worker worktrees)
        from accel_pipeline.prims_library import refresh_for_stage

        refresh_for_stage(pipeline_config.repo_root)
    merged_extra = dict(extra_context or {})
    if analysis_revision_request:
        merged_extra["analysis_revision_request"] = analysis_revision_request
    prompt = render_prompt(
        pipeline_config,
        prompt_file,
        workload,
        query,
        stage,
        optimization_iter=optimization_iter,
        extra_context=merged_extra or None,
    )
    save_rendered_prompt(
        pipeline_config,
        workload,
        query,
        stage,
        prompt,
        optimization_iter=optimization_iter,
    )

    if dry_run:
        log_stage_complete(
            pipeline_config.repo_root,
            query,
            workload,
            stage=stage,
            model_id="(dry-run)",
            backend="dry-run",
            duration_ms=None,
            usage=None,
            optimization_iter=optimization_iter,
            dry_run=True,
            prompt_chars=len(prompt),
        )
        return {"dry_run": True, "stage": stage, "prompt_chars": len(prompt)}

    model_id = make_model_id(stage, pipeline_config, model_config, model_overrides)
    agent_model = model_id
    log_stage_start(
        pipeline_config.repo_root,
        query,
        workload,
        stage=stage,
        model_id=model_id,
        optimization_iter=optimization_iter,
        revision=bool(analysis_revision_request),
    )
    from accel_pipeline.models import resolve_stage_provider

    stage_provider = resolve_stage_provider(stage, model_config)
    backend = create_agent_backend(
        provider=stage_provider,
        pipeline_config=pipeline_config,
        query=query,
        stage=stage,
        model_config=model_config,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
    )
    max_turns = resolve_stage_max_turns(pipeline_config.stage_max_turns, stage)
    instructions = with_codex_tooling(pipeline_config, prompt)

    begin_agent_run(stage, max_turns=max_turns, prompt_chars=len(instructions))

    request = StageRunRequest(
        stage=stage,
        instructions=instructions,
        user_message=user_message,
        agent_name=f"accel_{stage}",
        model_id=model_id,
        agent_model=agent_model,
        max_turns=max_turns,
    )
    frozen = snapshot_frozen(pipeline_config.repo_root)
    before = _tree_state(must_change) if must_change else None
    result = backend.run_stage(request)
    for _ in range(2):
        if before is None or result.status != "finished" or _tree_state(must_change) != before:
            break
        print(f"[pipeline] {stage}: no file under {must_change} changed; re-invoking", flush=True)
        request = replace(request, user_message=request.user_message + "\n\n" + _no_change_nudge(must_change))
        result = backend.run_stage(request)
    reverted = restore_frozen(pipeline_config.repo_root, frozen)
    if reverted:
        print(f"[pipeline] {stage}: reverted agent edits to frozen paths: {', '.join(reverted)}", flush=True)
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- {stage}: reverted agent edits to frozen pipeline/substrate paths ({', '.join(reverted)})",
            workload=workload,
        )
    if result.status != "finished" and result.metadata.get("timed_out"):
        # The agent ran out of time, not into an error: what it wrote is judged by the gates like
        # any other round, and the round limits bound the stage.
        print(f"[pipeline] {stage}: {result.error}; the gates judge what it produced", flush=True)
        append_experiment_log(
            pipeline_config.repo_root, query, f"- {stage}: {result.error} (gates judge the work)", workload=workload
        )
    elif result.status != "finished":
        detail = (result.error or result.status or "").strip()
        final = (result.final_text or "").strip()
        if final and final not in detail:
            clipped = final if len(final) <= 3000 else final[:3000] + "\n... [truncated]"
            detail = f"{detail}\n\n--- agent output ---\n{clipped}"
        raise RuntimeError(f"{backend.name} stage {stage} failed: {detail}")
    log_stage_complete(
        pipeline_config.repo_root,
        query,
        workload,
        stage=stage,
        model_id=model_id,
        backend=result.backend,
        duration_ms=result.metadata.get("duration_ms"),
        usage=usage_from_metadata(result.metadata),
        optimization_iter=optimization_iter,
        run_id=result.run_id,
    )
    return result


def _tree_state(root: Path) -> dict[str, tuple[int, int]]:
    if not root.is_dir():
        return {}
    return {
        str(f): (f.stat().st_mtime_ns, f.stat().st_size)
        for f in root.rglob("*") if f.is_file()
    }


def _no_change_nudge(root: Path) -> str:
    return (
        f"Your last run ended without changing any file under `{root}`. Nobody reads a question "
        "or approves a next step: this run is the work. Write the code now (create the files "
        "that are missing), run the gate, and finish only when it passes."
    )
