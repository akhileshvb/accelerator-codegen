from __future__ import annotations

import sys
from dataclasses import replace
from pathlib import Path
from typing import Any, Callable

from accel_pipeline.backends import StageRunRequest, create_agent_backend
from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload, apply_workload_overrides
from accel_pipeline.failure import (
    AGENT_BLOCKER, PROVIDER_CREDITS, PROVIDER_FLAKE, Failure, StageFailure, is_out_of_credits, is_transient_provider_failure,
)
from accel_pipeline.models import make_model_id
from accel_pipeline.no_progress import REDIRECT
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.session import run_in_session
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
    session_role: str | None = None,
    resume: bool = True,
    handoff: Callable[[], str] | None = None,
) -> Any:
    """Run one agent stage. With `must_change`, a run that leaves every file under that
    directory as it was is re-invoked (twice at most): agents run unattended, so a turn that
    ends with a question or a plan instead of edits did nothing.

    With a `session_role`, the stage runs in that role's codex session (`resume`: continue the
    stored one). A run that fails raises StageFailure for the repair loop."""
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
    instructions = with_codex_tooling(pipeline_config, prompt,
                                      engine_backend=workload.get("workload_kind") == "engine_backend")

    print(f"[pipeline] agent run start: stage={stage} max_turns={max_turns} prompt_chars={len(instructions)}",
          file=sys.stderr, flush=True)

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
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)

    def run(req: StageRunRequest) -> Any:
        if session_role and resume:
            return run_in_session(backend, req, pipeline_config=pipeline_config, art=art, query=query, role=session_role,
                                  handoff=handoff)
        return backend.run_stage(req)

    result = run(request)
    for _ in range(2):
        if before is None or result.status != "finished" or _tree_state(must_change) != before:
            break
        print(f"[pipeline] {stage}: no file under {must_change} changed; re-invoking", flush=True)
        request = replace(request, user_message=request.user_message + "\n\n" + _no_change_nudge(must_change))
        result = run(request)
    reverted = restore_frozen(pipeline_config.repo_root, frozen)
    if reverted:
        print(f"[pipeline] {stage}: reverted agent edits to frozen paths: {', '.join(reverted)}", flush=True)
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- {stage}: reverted agent edits to frozen pipeline/substrate paths ({', '.join(reverted)})",
            workload=workload,
        )
    failure = None if result.status == "finished" else stage_failure(stage, backend.name, result)
    if failure is not None and failure.kind != "timeout":
        raise StageFailure(failure)
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
    if failure is not None:
        append_experiment_log(pipeline_config.repo_root, query, f"- {stage}: {failure.summary}", workload=workload)
        raise StageFailure(failure)
    return result


def stage_failure(stage: str, backend_name: str, result: Any) -> Failure:
    """What a codex run that did not finish failed on: the deadline, a write-policy violation the
    agent can fix, a provider flake, or something only the orchestrator can answer for."""
    detail = (result.error or result.status or "").strip()
    stopped, evidence = result.metadata.get("stop_reason"), str(result.metadata.get("stop_detail") or "")
    if stopped == "blocker":
        return Failure(kind="infra", stage=stage, source="agent", infra_reason=AGENT_BLOCKER, evidence=evidence,
                       summary="the agent reported a blocker outside its files (blocker.md)")
    if stopped == "no_progress":
        return Failure(kind="timeout", stage=stage, source="no_progress", summary=detail, evidence=evidence,
                       diagnosis=[REDIRECT])
    if result.metadata.get("timed_out"):
        return Failure(
            kind="timeout", stage=stage, source="agent", summary=detail,
            diagnosis=["You were stopped at the deadline. State your status, checkpoint the tree "
                       "(it must build), and continue from where you were."],
        )
    final = (result.final_text or "").strip()
    if final and final not in detail:
        clipped = final if len(final) <= 3000 else final[:3000] + "\n... [truncated]"
        detail = f"{detail}\n\n--- agent output ---\n{clipped}"
    summary = f"{backend_name} stage {stage} failed"
    if result.metadata.get("write_violations"):
        return Failure(kind="code", stage=stage, source="write_policy", summary=summary, evidence=detail)
    if is_out_of_credits(detail):
        return Failure(kind="infra", stage=stage, source="agent", evidence=detail, infra_reason=PROVIDER_CREDITS,
                       summary="the model provider's account is out of credits (402): add credits, then --resume")
    if is_transient_provider_failure(detail):
        return Failure(kind="infra", stage=stage, source="agent", summary=summary, evidence=detail,
                       infra_reason=PROVIDER_FLAKE)
    return Failure(kind="orchestrator", stage=stage, source="agent", summary=summary, evidence=detail)


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
