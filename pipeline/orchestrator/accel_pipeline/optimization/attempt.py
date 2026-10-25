"""Measurement-driven optimization attempt loop."""

from __future__ import annotations

import json
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline.backends import StageRunRequest, create_agent_backend
from accel_pipeline.optimization.base_bench import build_optimization_diagnosis, load_base_optimization_state
from accel_pipeline.config import (
    ModelConfig,
    PipelineConfig,
    apply_workload_overrides,
    load_run_workload,
    resolve_optimization_edit_paths,
    resolve_workload_paths,
)
from accel_pipeline.failure import Failure, StageFailure, is_transient_provider_failure
from accel_pipeline.jsonio import read_json
from accel_pipeline.optimization.kernel_hot_path_review import analyze_kernel_source
from accel_pipeline.models import make_model_id
from accel_pipeline.optimization.bootstrap import build_optimization_bootstrap, save_optimization_bootstrap
from accel_pipeline.optimization.checkpoint import (
    advance_best_valid_baseline,
    append_attempt_log,
    begin_attempt_checkpoint,
    clear_attempt_checkpoint,
    load_best_valid_baseline,
    load_experimental_branch,
    revert_attempt_checkpoint,
    save_experimental_branch,
    snapshot_best_valid_baseline_files,
)
from accel_pipeline.optimization.packet import (
    build_context_packet,
    build_initial_worker_seed,
    build_optimization_user_message,
    reconstruct_seed_from_artifacts,
    save_context_packet,
)
from accel_pipeline.optimization.evidence import extract_bottleneck_evidence
from accel_pipeline.layout import query_dir
from accel_pipeline.optimization.measurement import check_measurement
from accel_pipeline.optimization.eval import (
    AttemptOutcome,
    MultiSampleBenchmark,
    eval_config_from_pipeline,
    evaluate_attempt_outcome,
    is_aux_workload,
    run_multi_sample_benchmark,
    score_metrics,
)
from accel_pipeline.optimization.probe import run_optimization_probe
from accel_pipeline.optimization.fix import (
    format_build_failure_diagnosis,
    format_smoke_failure_diagnosis,
    log_fix_round,
    optimization_fix_limit,
)
from accel_pipeline.optimization.runtime import refresh_optimization_runtime
from accel_pipeline.optimization.negative_memory import maybe_record_reject_trap
from accel_pipeline.optimization.saturation import (
    REANALYSIS_MARKER_FILE,
    Verdict,
    judge_saturation_claim,
    parse_optimization_saturated,
    profile_guidance_error,
    record_trial_protocol,
)
from accel_pipeline.optimization.smoke import SmokeResult, run_build, run_smoke_correctness_repeated
from accel_pipeline.paths import analysis_artifact_path, load_query_analysis, query_artifacts_dir
from accel_pipeline.optimization.structural import (
    design_revision_error,
    is_structural_attempt,
    record_design_revision,
    run_full_tier_gates,
)
from accel_pipeline.structural_reanalysis import write_request as write_structural_request
from accel_pipeline.optimization.pending import format_gate_delta
from accel_pipeline.optimization.worker import OptimizationWorker
from accel_pipeline.optimization.worker_checkpoint import load_worker_checkpoint
from accel_pipeline.pipeline_log import log_event, log_stage_complete, log_stage_start
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.prompts import render_prompt, save_rendered_prompt, with_codex_tooling
from accel_pipeline.session import MAIN, run_in_session
from accel_pipeline.stages.base import resolve_optimization_max_turns


def save_attempt_state(art: Path, state: dict[str, Any]) -> None:
    (art / "attempt_state.json").write_text(json.dumps(state, indent=2), encoding="utf-8")


HYPOTHESIS_COMPLETE = "HYPOTHESIS_COMPLETE"
PATCH_COMPLETE = "HYPOTHESIS_PATCH_COMPLETE"
CANNOT_TEST = "CANNOT_TEST"
EXPERIMENTAL_CHAIN_CAP = 3


@dataclass
class UnifiedAttemptResult:
    status: str  # "complete" | "cannot_test" | "failed" | "saturated"
    hypothesis: dict[str, Any] | None = None
    reason: str = ""
    final_text: str = ""
    saturation: dict[str, Any] | None = None


def _request_read_path_reanalysis(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    art: Path,
    optimization_iter: int,
    detail: str,
) -> None:
    """Saturated below the read floor: hand the design back to analysis once instead of stopping."""
    (art / REANALYSIS_MARKER_FILE).write_text(detail, encoding="utf-8")
    try:
        analysis = json.loads(analysis_artifact_path(pipeline_config.repo_root, query, workload=workload).read_text(encoding="utf-8"))
        candidate = str(analysis["correctness_facts"]["state_design"]["selected_candidate_id"])
    except (OSError, KeyError, TypeError, json.JSONDecodeError):
        candidate = "current"
    write_structural_request(
        pipeline_config.repo_root,
        query,
        workload,
        source_stage="optimization",
        source_iteration=optimization_iter,
        bottleneck_type="residual_read_path",
        evidence=f"{detail}. The profile-guided structural and low-level attempts recorded in trial_protocol.jsonl did not close the gap.",
        proposed_direction="Choose a design whose typical read visits at least 100x less state than the base path: "
        "finer partitioning by the predicate's columns, coarser pre-aggregation with output-sensitive refinement, "
        "or a data-derived bound that tightens with the parameter.",
        current_candidate_id=candidate,
    )


def _structural_failure(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    art: Path,
    hypothesis: dict[str, Any] | None,
    optimization_iter: int,
) -> tuple[bool, str]:
    """(is_structural, why it fails): a redesign carries its design record and passes the full-tier gates."""
    if workload.get("workload_kind") != "aux_state" or not is_structural_attempt(pipeline_config, art, hypothesis):
        return False, ""
    print("[pipeline] structural attempt: full-tier gates before the benchmark", file=sys.stderr, flush=True)
    error = design_revision_error(hypothesis) or run_full_tier_gates(
        pipeline_config, workload, query, iteration=optimization_iter
    )
    return True, error


def _try_honor_saturation(
    *,
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    art: Path,
    optimization_iter: int,
    attempt: int,
    payload: dict[str, Any] | None,
    worker: OptimizationWorker | None = None,
) -> tuple[bool, str]:
    """Return (honored, detail). On reject, push feedback / log failure."""
    verdict, detail, cfg = judge_saturation_claim(pipeline_config, art, payload)
    ok = verdict is not Verdict.REJECT
    if verdict is Verdict.REANALYZE:
        _request_read_path_reanalysis(pipeline_config, workload, query, art, optimization_iter, detail)
    log_event(
        pipeline_config.repo_root,
        query,
        workload,
        "optimization_saturation_claim",
        honored=ok,
        detail=detail[:500],
        floors=cfg,
        payload=payload or {},
        optimization_iter=optimization_iter,
        attempt=attempt,
        workload_name=workload.get("name"),
    )
    if ok:
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- iter {optimization_iter} a{attempt}: OPTIMIZATION_SATURATED — {detail}",
            workload=workload,
        )
        append_attempt_log(
            art,
            {
                "attempt": attempt,
                "accepted": False,
                "kept": True,
                "verdict": "optimization_saturated",
                "reason": detail,
                "saturation": payload or {},
            },
        )
        clear_attempt_checkpoint(art)
        return True, detail

    reject_msg = (
        f"OPTIMIZATION_SATURATED not honored: {detail}. Continue optimizing."
    )
    if worker is not None:
        worker.pending.push(reject_msg)
        worker.save_checkpoint()
    _record_failure(
        art,
        attempt=attempt,
        hypothesis=None,
        reason=reject_msg,
        verdict="saturation_floors_not_met",
    )
    return False, detail


def _parse_hypothesis_json(text: str) -> dict[str, Any] | None:
    if not text:
        return None
    fence = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL)
    if fence:
        try:
            return json.loads(fence.group(1))
        except json.JSONDecodeError:
            pass
    for match in re.finditer(r"\{[^{}]*(?:\{[^{}]*\}[^{}]*)*\}", text, re.DOTALL):
        try:
            payload = json.loads(match.group(0))
            if isinstance(payload, dict) and payload.get("bottleneck"):
                return payload
        except json.JSONDecodeError:
            continue
    return None


def _parse_cannot_test(text: str) -> str | None:
    if CANNOT_TEST not in text:
        return None
    fence = re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL)
    if fence:
        try:
            payload = json.loads(fence.group(1))
            if payload.get("status") == "cannot_test":
                return str(payload.get("reason", "cannot test hypothesis"))
        except json.JSONDecodeError:
            pass
    return "patch agent returned CANNOT_TEST"


def _effective_consecutive_fail_stop(pipeline_config: PipelineConfig, max_attempts: int) -> int:
    raw = int(getattr(pipeline_config, "optimization_consecutive_fail_stop", 0) or 0)
    if raw <= 0:
        return max_attempts + 1
    return raw


def _short_failure_reason(reason: str | None, *, max_chars: int = 240) -> str:
    text = " ".join((reason or "unknown").split())
    if len(text) <= max_chars:
        return text
    return text[: max_chars - 3] + "..."


def _measure_baseline(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    eval_config,
) -> MultiSampleBenchmark:
    errors: list[str] = []
    baseline = run_multi_sample_benchmark(
        pipeline_config,
        workload,
        query,
        samples=eval_config.quick_benchmark_samples,
        include_profile=True,
        errors=errors,
    )
    if baseline is None:
        raise StageFailure(
            Failure(
                kind="measurement",
                stage="optimization",
                source="bench_baseline",
                summary="the baseline benchmark did not produce a measurement",
                evidence="\n".join(errors),
                diagnosis=["the benchmark failed to build or run; nothing measured the accelerator"],
            )
        )
    _sanity_gate_baseline(pipeline_config, workload, query, baseline)
    return baseline


def _sanity_gate_baseline(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    baseline: MultiSampleBenchmark,
) -> None:
    """A baseline that does not measure the accelerator would make every attempt's verdict meaningless."""
    paths = resolve_workload_paths(pipeline_config, workload, query)
    target = paths["aux_bench_target" if is_aux_workload(workload) else "bench_target"]
    benchmark = str(workload.get("benchmark_name", "tpch"))
    failures = check_measurement(
        baseline,
        workload=workload,
        phase="baseline",
        config=pipeline_config.measurement,
        bench_binary=pipeline_config.build_dir / target,
        query_dir=pipeline_config.repo_root / query_dir(benchmark, int(workload.get("query_number", query))),
    )
    if failures:
        raise StageFailure(failures[0])


def _raise_if_invalid_measurement(outcome: AttemptOutcome) -> None:
    """An attempt whose benchmark measured nothing is neither accepted nor reverted."""
    if outcome.verdict == "reject_invalid_measurement":
        raise StageFailure(outcome.measurement_failures[0])


def _revert_and_rebuild(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> None:
    """Restore checkpointed sources and rebuild so a reverted patch cannot leave a stale binary."""
    revert_attempt_checkpoint(pipeline_config, workload, query)
    built, build_out = run_build(pipeline_config, workload, query)
    if not built:
        infra = _fail_fast_infra_if_needed(
            pipeline_config, workload, query, text=build_out or ""
        )
        if infra:
            return
        print(
            f"[pipeline] warning: rebuild after revert failed: {(build_out or '')[-400:]}",
            file=sys.stderr,
            flush=True,
        )


def _fail_fast_infra_if_needed(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    text: str,
) -> dict[str, Any] | None:
    """On shared-tree infra failures, repair + release lease and stop burning agent turns."""
    from accel_pipeline.failure import handle_infra_failure

    info = handle_infra_failure(pipeline_config, workload, query, text=text or "")
    if not info:
        return None
    print(
        f"[pipeline] fail-fast infra ({info.get('infra_reason')}): "
        f"lease_released={info.get('lease_released')} "
        f"restored={list((info.get('repair') or {}).get('restored') or {})}",
        file=sys.stderr,
        flush=True,
    )
    return info


class InfraFailFastError(RuntimeError):
    """Raised to abort fix loops without launching more agent turns."""

    def __init__(self, info: dict[str, Any], *, detail: str = ""):
        self.info = info
        reason = info.get("infra_reason") or "infra_failure"
        super().__init__(f"infra_fail_fast:{reason}: {detail}"[:2000])


def _build_evidence(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    baseline: MultiSampleBenchmark,
) -> dict[str, Any]:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    opt_state = load_base_optimization_state(art)
    baseline_summary = read_json(art / "baseline_summary.json")
    profile = read_json(art / "profile_summary.json")
    _, analysis = load_query_analysis(pipeline_config.repo_root, query, workload=workload)
    edit_paths = resolve_optimization_edit_paths(pipeline_config, workload, query)
    kernel_path = pipeline_config.repo_root / edit_paths["primary_cpp"]
    kernel_review = analyze_kernel_source(kernel_path)
    diagnosis = build_optimization_diagnosis(
        baseline_summary,
        profile,
        best_speedup_median=float(opt_state.get("best_speedup_median") or 0.0),
        analysis=analysis or None,
        kernel_review=kernel_review,
    )
    if baseline.median_metrics.get("speedup_median") is not None:
        diagnosis["speedup_median"] = baseline.median_metrics.get("speedup_median")
    return extract_bottleneck_evidence(
        diagnosis, profile, kernel_review, analysis=analysis or None
    )


def _run_agent_substage(
    *,
    stage: str,
    prompt_file: str,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    user_message: str,
    model_overrides: dict[str, str] | None,
    optimization_iter: int,
    extra_context: dict[str, str] | None = None,
    session_role: str | None = None,
) -> Any:
    workload = load_run_workload(pipeline_config, workload_name)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    prompt = render_prompt(
        pipeline_config,
        prompt_file,
        workload,
        query,
        stage,
        optimization_iter=optimization_iter,
        extra_context=extra_context,
    )
    save_rendered_prompt(pipeline_config, workload, query, stage, prompt, optimization_iter=optimization_iter)

    model_id = make_model_id(stage, pipeline_config, model_config, model_overrides)
    agent_model = model_id
    if stage == "optimization_unified":
        max_turns = resolve_optimization_max_turns(pipeline_config, "unified")
    elif stage in ("optimization_unified_hypothesis", "optimization_unified_patch"):
        max_turns = resolve_optimization_max_turns(pipeline_config, "unified")
    elif "analysis" in stage:
        max_turns = resolve_optimization_max_turns(pipeline_config, "analysis")
    else:
        max_turns = resolve_optimization_max_turns(pipeline_config, "patch")

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

    instructions = with_codex_tooling(pipeline_config, prompt)

    request = StageRunRequest(
        stage=stage,
        instructions=instructions,
        user_message=user_message,
        agent_name=f"accel_{stage}",
        model_id=model_id,
        agent_model=agent_model,
        max_turns=max_turns,
    )
    if session_role:
        art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
        return run_in_session(backend, request, pipeline_config=pipeline_config, art=art, query=query,
                              role=session_role)
    return backend.run_stage(request)


def _two_phase_optimization_enabled(workload: dict[str, Any]) -> bool:
    opt = workload.get("optimization") or {}
    if opt.get("two_phase_hypothesis_patch") is False:
        return False
    return True


def _saturation_from_text(final_text: str, hypothesis: dict[str, Any] | None = None) -> UnifiedAttemptResult | None:
    payload = parse_optimization_saturated(final_text)
    if payload is None:
        return None
    return UnifiedAttemptResult(
        status="saturated",
        hypothesis=hypothesis or _parse_hypothesis_json(final_text),
        reason=str(payload.get("reason") or "optimization_saturated"),
        final_text=final_text,
        saturation=payload,
    )


def _run_hypothesis_phase(
    *,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    optimization_iter: int,
    attempt: int,
    user_message: str,
    model_overrides: dict[str, str] | None,
) -> UnifiedAttemptResult:
    result = _run_agent_substage(
        stage="optimization_unified_hypothesis",
        prompt_file="03_optimization_unified.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=user_message,
        model_overrides=model_overrides,
        optimization_iter=optimization_iter,
    )
    final_text = result.final_text or ""
    if result.status != "finished":
        return UnifiedAttemptResult(status="failed", reason=result.error or result.status, final_text=final_text)
    saturated = _saturation_from_text(final_text)
    if saturated is not None:
        return saturated
    hypothesis = _parse_hypothesis_json(final_text)
    if HYPOTHESIS_COMPLETE not in final_text:
        return UnifiedAttemptResult(
            status="failed",
            hypothesis=hypothesis,
            reason="missing HYPOTHESIS_COMPLETE in hypothesis phase",
            final_text=final_text,
        )
    if not hypothesis:
        return UnifiedAttemptResult(status="failed", reason="missing hypothesis JSON", final_text=final_text)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=load_run_workload(pipeline_config, workload_name))
    fields = {key: str(hypothesis.get(key) or "") for key in ("hot_spot", "expected_gain", "strategy")}
    guidance_error = profile_guidance_error(art, **fields)
    if guidance_error:
        return UnifiedAttemptResult(
            status="failed",
            hypothesis=hypothesis,
            reason=f"the hypothesis must start from the profile: {guidance_error}",
            final_text=final_text,
        )
    record_trial_protocol(art, trial_id=f"iter{optimization_iter}_a{attempt}", **fields)
    return UnifiedAttemptResult(status="complete", hypothesis=hypothesis, final_text=final_text)


def _run_patch_phase(
    *,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    optimization_iter: int,
    attempt: int,
    hypothesis: dict[str, Any],
    user_message: str,
    model_overrides: dict[str, str] | None,
) -> UnifiedAttemptResult:
    patch_message = (
        f"{user_message}\n\n## Locked hypothesis\n```json\n"
        f"{json.dumps(hypothesis, indent=2)}\n```\n"
        "Implement this hypothesis with a single coherent patch, then end with "
        f"`{PATCH_COMPLETE}`.\n"
    )
    result = _run_agent_substage(
        stage="optimization_unified_patch",
        prompt_file="03_optimization_unified.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=patch_message,
        model_overrides=model_overrides,
        optimization_iter=optimization_iter,
    )
    final_text = result.final_text or ""
    if result.status != "finished":
        return UnifiedAttemptResult(status="failed", reason=result.error or result.status, final_text=final_text)
    saturated = _saturation_from_text(final_text, hypothesis=hypothesis)
    if saturated is not None:
        return saturated
    cannot = _parse_cannot_test(final_text)
    if cannot:
        return UnifiedAttemptResult(status="cannot_test", reason=cannot, final_text=final_text)
    if PATCH_COMPLETE not in final_text:
        return UnifiedAttemptResult(
            status="failed",
            hypothesis=hypothesis,
            reason="missing HYPOTHESIS_PATCH_COMPLETE",
            final_text=final_text,
        )
    return UnifiedAttemptResult(status="complete", hypothesis=hypothesis, final_text=final_text)


def _run_unified_optimization_attempt(
    *,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    optimization_iter: int,
    attempt: int,
    context_markdown: str,
    bootstrap_markdown: str,
    model_overrides: dict[str, str] | None,
    experimental_summary: str = "",
) -> UnifiedAttemptResult:
    workload = load_run_workload(pipeline_config, workload_name)
    guidance = str(workload.get("optimization_guidance", "") or "").strip()
    if _two_phase_optimization_enabled(workload):
        hypo_message = build_optimization_user_message(
            optimization_iter=optimization_iter,
            attempt=attempt,
            context_markdown=context_markdown,
            bootstrap_markdown=bootstrap_markdown,
            workload_guidance=guidance,
            experimental_summary=experimental_summary,
            hypothesis_phase=True,
        )
        hypo = _run_hypothesis_phase(
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            optimization_iter=optimization_iter,
            attempt=attempt,
            user_message=hypo_message,
            model_overrides=model_overrides,
        )
        if hypo.status != "complete":
            return hypo
        patch_message = build_optimization_user_message(
            optimization_iter=optimization_iter,
            attempt=attempt,
            context_markdown=context_markdown,
            bootstrap_markdown=bootstrap_markdown,
            workload_guidance=guidance,
            experimental_summary=experimental_summary,
            hypothesis_phase=False,
        )
        return _run_patch_phase(
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            optimization_iter=optimization_iter,
            attempt=attempt,
            hypothesis=hypo.hypothesis or {},
            user_message=patch_message,
            model_overrides=model_overrides,
        )

    user_message = build_optimization_user_message(
        optimization_iter=optimization_iter,
        attempt=attempt,
        context_markdown=context_markdown,
        bootstrap_markdown=bootstrap_markdown,
        workload_guidance=guidance,
        experimental_summary=experimental_summary,
        hypothesis_phase=False,
    )
    user_message = (
        user_message.replace(
            "Hypothesis is locked — patch phase only. Edit the editable sources; "
            "no repository exploration.\n",
            "Hypothesize, implement, then end with "
            f"`{PATCH_COMPLETE}` in one short session.\n",
        )
    )
    result = _run_agent_substage(
        stage="optimization_unified",
        prompt_file="03_optimization_unified.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=user_message,
        model_overrides=model_overrides,
        optimization_iter=optimization_iter,
    )
    final_text = result.final_text or ""
    if result.status == "error":
        if "max_turns" in str(result.error or "").lower():
            return UnifiedAttemptResult(
                status="failed",
                reason="agent ended before HYPOTHESIS_PATCH_COMPLETE — finish implementation then end with that marker",
                final_text=final_text,
            )
        return UnifiedAttemptResult(
            status="failed",
            reason=result.error or result.status,
            final_text=final_text,
        )
    if result.status != "finished":
        return UnifiedAttemptResult(status="failed", reason=result.error or result.status, final_text=final_text)

    saturated = _saturation_from_text(final_text)
    if saturated is not None:
        return saturated

    cannot = _parse_cannot_test(final_text)
    if cannot:
        return UnifiedAttemptResult(status="cannot_test", reason=cannot, final_text=final_text)

    hypothesis = _parse_hypothesis_json(final_text)
    if PATCH_COMPLETE not in final_text:
        return UnifiedAttemptResult(
            status="failed",
            hypothesis=hypothesis,
            reason="missing HYPOTHESIS_PATCH_COMPLETE",
            final_text=final_text,
        )
    if not hypothesis:
        hypothesis = {"bottleneck": "unified_optimization_patch", "why": "no hypothesis JSON in agent output"}
    return UnifiedAttemptResult(status="complete", hypothesis=hypothesis, final_text=final_text)


def _build_reproduce_cmd(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> str:
    paths = resolve_workload_paths(pipeline_config, workload, query)
    if workload.get("workload_kind") == "aux_state":
        targets = [paths.get("aux_bench_target", ""), paths.get("test_target", "")]
    else:
        targets = [paths["bench_target"], paths["test_target"]]
    target_str = " ".join(t for t in targets if t)
    return f"cmake --build {pipeline_config.build_dir.name} --target {target_str} -j4"


def _smoke_reproduce_cmd(smoke: SmokeResult, *, build_dir_name: str = "build-release") -> str:
    return f"ctest --test-dir {build_dir_name} -R {smoke.regex} --output-on-failure"


def _run_optimization_fix_agent(
    *,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    workload: dict[str, Any],
    query: int,
    optimization_iter: int,
    attempt: int,
    diagnosis: str,
    model_overrides: dict[str, str] | None,
) -> None:
    _run_agent_substage(
        stage="optimization_fix",
        prompt_file="03_optimization_fix.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=diagnosis,
        model_overrides=model_overrides,
        optimization_iter=optimization_iter,
        session_role=MAIN,
    )


def _run_build_with_fix_loop(
    *,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    workload: dict[str, Any],
    query: int,
    optimization_iter: int,
    attempt: int,
    model_overrides: dict[str, str] | None,
) -> tuple[bool, str]:
    log_chars = int(getattr(pipeline_config, "optimization_fix_log_chars", 6000) or 6000)
    reproduce = _build_reproduce_cmd(pipeline_config, workload, query)
    build_out = ""
    fix_round = 0
    while True:
        built, build_out = run_build(pipeline_config, workload, query)
        if built:
            if fix_round > 0:
                print(
                    f"[pipeline] build recovered after {fix_round} fix round(s)",
                    file=sys.stderr,
                    flush=True,
                )
            return True, build_out
        infra = _fail_fast_infra_if_needed(
            pipeline_config, workload, query, text=build_out or ""
        )
        if infra:
            raise InfraFailFastError(infra, detail=(build_out or "")[-1500:])
        fix_round += 1
        optimization_fix_limit(pipeline_config, fix_round, kind="build")
        diagnosis = format_build_failure_diagnosis(
            build_out,
            optimization_iter=optimization_iter,
            attempt=attempt,
            fix_round=fix_round,
            reproduce_cmd=reproduce,
            max_log_chars=log_chars,
        )
        log_fix_round(
            pipeline_config,
            workload,
            query,
            optimization_iter=optimization_iter,
            attempt=attempt,
            kind="build",
            fix_round=fix_round,
            diagnosis=diagnosis,
        )
        _run_optimization_fix_agent(
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            workload=workload,
            query=query,
            optimization_iter=optimization_iter,
            attempt=attempt,
            diagnosis=diagnosis,
            model_overrides=model_overrides,
        )


def _smoke_runs(pipeline_config: PipelineConfig) -> int:
    return max(1, int(getattr(pipeline_config, "optimization_smoke_runs", 2) or 2))


def _run_smoke_with_fix_loop(
    *,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    workload: dict[str, Any],
    query: int,
    optimization_iter: int,
    attempt: int,
    model_overrides: dict[str, str] | None,
) -> SmokeResult:
    log_chars = int(getattr(pipeline_config, "optimization_fix_log_chars", 6000) or 6000)
    fix_round = 0
    smoke_runs = _smoke_runs(pipeline_config)
    while True:
        smoke = run_smoke_correctness_repeated(
            pipeline_config, workload, query, runs=smoke_runs
        )
        if smoke.passed:
            if fix_round > 0:
                print(
                    f"[pipeline] smoke correctness recovered after {fix_round} fix round(s)",
                    file=sys.stderr,
                    flush=True,
                )
            return smoke
        infra = _fail_fast_infra_if_needed(
            pipeline_config, workload, query, text=smoke.output or ""
        )
        if infra:
            raise InfraFailFastError(infra, detail=(smoke.output or "")[-1500:])
        fix_round += 1
        optimization_fix_limit(pipeline_config, fix_round, kind="correctness")
        reproduce = _smoke_reproduce_cmd(
            smoke, build_dir_name=pipeline_config.build_dir.name
        )
        diagnosis = format_smoke_failure_diagnosis(
            smoke,
            optimization_iter=optimization_iter,
            attempt=attempt,
            fix_round=fix_round,
            reproduce_cmd=reproduce,
            max_log_chars=log_chars,
        )
        log_fix_round(
            pipeline_config,
            workload,
            query,
            optimization_iter=optimization_iter,
            attempt=attempt,
            kind="correctness",
            fix_round=fix_round,
            diagnosis=diagnosis,
        )
        _run_optimization_fix_agent(
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            workload=workload,
            query=query,
            optimization_iter=optimization_iter,
            attempt=attempt,
            diagnosis=diagnosis,
            model_overrides=model_overrides,
        )


def _record_failure(
    art,
    *,
    attempt: int,
    hypothesis: dict[str, Any] | None,
    reason: str,
    improvement_pct: float | None = None,
    candidate_metrics: dict[str, Any] | None = None,
    repo_root: Path | None = None,
    query: int | None = None,
    workload: dict[str, Any] | None = None,
    verdict: str | None = None,
) -> None:
    entry: dict[str, Any] = {
        "attempt": attempt,
        "hypothesis": (hypothesis or {}).get("bottleneck", ""),
        "hypothesis_json": hypothesis,
        "accepted": False,
        "kept": False,
        "failure_reason": reason,
        "improvement_pct": improvement_pct,
    }
    if candidate_metrics:
        entry["candidate_metrics"] = candidate_metrics
    if verdict:
        entry["verdict"] = verdict
    append_attempt_log(art, entry)
    if repo_root is not None and query is not None and verdict:
        maybe_record_reject_trap(
            repo_root,
            query=query,
            workload=workload,
            hypothesis=hypothesis,
            verdict=verdict,
            reason=reason,
            improvement_pct=improvement_pct,
        )


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
    worker: OptimizationWorker | None = None,
    deadline: float | None = None,
) -> Any:
    """One optimization iteration; no new attempt starts after `deadline` (time.time())."""
    workload = load_run_workload(pipeline_config, workload_name)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    eval_config = eval_config_from_pipeline(pipeline_config)
    max_attempts = int(getattr(pipeline_config, "optimization_max_attempts_per_iteration", 4) or 4)
    consecutive_fail_stop = _effective_consecutive_fail_stop(pipeline_config, max_attempts)

    if dry_run:
        return {
            "dry_run": True,
            "stage": "optimization",
            "max_attempts": max_attempts,
        }

    log_stage_start(
        pipeline_config.repo_root,
        query,
        workload,
        stage="optimization",
        model_id=make_model_id("optimization", pipeline_config, model_config, model_overrides),
        optimization_iter=optimization_iter,
    )

    print(
        "[pipeline] preflight build before optimization iteration",
        file=sys.stderr,
        flush=True,
    )
    preflight_ok, preflight_out = run_build(pipeline_config, workload, query)
    if not preflight_ok:
        infra = _fail_fast_infra_if_needed(
            pipeline_config, workload, query, text=preflight_out or ""
        )
        raise StageFailure(Failure(
            kind="infra" if infra else "code",
            stage="optimization",
            source="preflight_build",
            summary="preflight build failed before the optimization iteration; fix the tree or restore the checkpoint",
            evidence=(preflight_out or "")[-2000:],
            infra_reason=infra.get("infra_reason") if infra else None,
        ))

    from accel_pipeline.optimization.aux_write_freqs import ensure_attempt_write_frequencies

    locked_freqs = ensure_attempt_write_frequencies(
        art,
        workload,
        optimization_iter=optimization_iter,
        attempt=0,
    )
    print(
        f"[pipeline] optimization write frequencies (baseline attempt 0, iter {optimization_iter}): "
        f"{locked_freqs}",
        file=sys.stderr,
        flush=True,
    )

    print(
        "[pipeline] measuring optimization baseline (aux: 0% + sampled write mixes, "
        "score=dual_axis_weighted read^0.75*write_mix^0.25)",
        file=sys.stderr,
        flush=True,
    )
    baseline = _measure_baseline(pipeline_config, workload, query, eval_config)
    if not load_best_valid_baseline(art):
        snapshot_best_valid_baseline_files(pipeline_config, workload, query)
        advance_best_valid_baseline(
            pipeline_config,
            workload,
            query,
            attempt_id=f"iter{optimization_iter}_baseline",
            score=baseline.median_score,
            metrics=baseline.median_metrics,
        )
    evidence = _build_evidence(pipeline_config, workload, query, baseline)
    consecutive_fails = 0
    experimental_depth = 0
    last_result: Any = None

    if worker is not None:
        worker.episode_meta.optimization_iter = optimization_iter
        if not worker.seeded:
            ckpt = load_worker_checkpoint(
                art,
                getattr(pipeline_config, "optimization_checkpoint_file", "optimization_worker_checkpoint.json"),
            )
            if ckpt:
                seed = reconstruct_seed_from_artifacts(
                    pipeline_config, workload, query, checkpoint=ckpt
                )
            else:
                seed = build_initial_worker_seed(
                    pipeline_config,
                    workload,
                    query,
                    evidence=evidence,
                    baseline_metrics=baseline.median_metrics,
                    baseline_score=baseline.median_score,
                )
            worker.seed_episode(seed)

    for attempt in range(1, max_attempts + 1):
        if consecutive_fails >= consecutive_fail_stop:
            break
        if deadline is not None and time.time() >= deadline:
            break

        experimental = load_experimental_branch(art)
        experimental_summary = ""
        if experimental:
            experimental_depth = int(experimental.get("chain_depth") or 0)
            experimental_summary = json.dumps(
                experimental.get("diagnostic_summary") or {},
                indent=2,
            )

        attempt_id = f"iter{optimization_iter}_a{attempt}"
        locked_freqs = ensure_attempt_write_frequencies(
            art,
            workload,
            optimization_iter=optimization_iter,
            attempt=attempt,
        )
        print(
            f"[pipeline] optimization write frequencies (iter {optimization_iter} attempt {attempt}): "
            f"{locked_freqs}",
            file=sys.stderr,
            flush=True,
        )
        begin_attempt_checkpoint(
            pipeline_config,
            workload,
            query,
            attempt_id=attempt_id,
            hypothesis="pending",
        )

        if worker is not None:
            worker.episode_meta.attempt = attempt
            user_msg = worker.pending.flush_into_attempt_prompt(
                optimization_iter=optimization_iter,
                attempt=attempt,
                experimental_summary=experimental_summary,
                max_chars=int(
                    getattr(pipeline_config, "optimization_turn_delta_max_chars", 4000) or 4000
                ),
            )
            # Soft-retry provider flakes on the same attempt slot so network
            # disconnects do not burn the whole attempt budget without any work.
            provider_retries = 0
            max_provider_retries = pipeline_config.repair.provider_retries
            while True:
                cycle = worker.run_candidate_cycle(user_msg)
                save_attempt_state(
                    art, {"attempt": attempt, "phase": "worker", "status": cycle.status}
                )
                last_result = cycle
                hypothesis = cycle.hypothesis
                if (
                    cycle.status != "ready"
                    and cycle.status != "saturated"
                    and cycle.status != "cannot_test"
                    and is_transient_provider_failure(cycle.reason)
                    and provider_retries < max_provider_retries
                ):
                    provider_retries += 1
                    short = _short_failure_reason(cycle.reason)
                    print(
                        f"[pipeline] transient provider failure on attempt {attempt} "
                        f"(retry {provider_retries}/{max_provider_retries}): {short}",
                        file=sys.stderr,
                        flush=True,
                    )
                    time.sleep(pipeline_config.repair.provider_backoff(provider_retries))
                    continue
                break

            if cycle.status == "saturated":
                honored, detail = _try_honor_saturation(
                    pipeline_config=pipeline_config,
                    workload=workload,
                    query=query,
                    art=art,
                    optimization_iter=optimization_iter,
                    attempt=attempt,
                    payload=cycle.saturation,
                    worker=worker,
                )
                if honored:
                    print(
                        f"[pipeline] OPTIMIZATION_SATURATED honored — {detail}",
                        file=sys.stderr,
                        flush=True,
                    )
                    break
                # Rejected claim must not look like iteration-level stop.
                cycle.status = "failed"
                cycle.reason = detail
                last_result = cycle
                consecutive_fails += 1
                if consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            if cycle.status == "cannot_test":
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                _record_failure(
                    art,
                    attempt=attempt,
                    hypothesis=hypothesis,
                    reason=f"cannot_test: {cycle.reason}",
                )
                worker.save_checkpoint()
                if consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            if cycle.status != "ready":
                short = _short_failure_reason(cycle.reason or "worker_candidate_failed")
                print(
                    f"[pipeline] candidate failed (no CANDIDATE_READY): {short}",
                    file=sys.stderr,
                    flush=True,
                )
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                _record_failure(
                    art,
                    attempt=attempt,
                    hypothesis=hypothesis,
                    reason=cycle.reason or "worker_candidate_failed",
                )
                worker.save_checkpoint()
                if consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            print(
                "[pipeline] CANDIDATE_READY; running authoritative build → smoke (x"
                f"{_smoke_runs(pipeline_config)}) → probe → quick bench",
                file=sys.stderr,
                flush=True,
            )
            built, build_out = run_build(pipeline_config, workload, query)
            if not built:
                infra = _fail_fast_infra_if_needed(
                    pipeline_config, workload, query, text=build_out or ""
                )
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                reason = (
                    f"infra_fail_fast:{infra.get('infra_reason')}"
                    if infra
                    else "auth_build_failed"
                )
                worker.pending.push(
                    format_gate_delta(
                        verdict=reason,
                        reason=(build_out or "")[-2000:],
                        hypothesis=hypothesis,
                        reverted=True,
                    )
                )
                worker.save_checkpoint()
                _record_failure(art, attempt=attempt, hypothesis=hypothesis, reason=reason)
                if infra or consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            smoke = run_smoke_correctness_repeated(
                pipeline_config, workload, query, runs=_smoke_runs(pipeline_config)
            )
            if not smoke.passed:
                infra = _fail_fast_infra_if_needed(
                    pipeline_config, workload, query, text=smoke.output or ""
                )
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                reason = (
                    f"infra_fail_fast:{infra.get('infra_reason')}"
                    if infra
                    else f"auth_smoke_failed: {(smoke.output or '')[-500:]}"
                )
                worker.pending.push(
                    format_gate_delta(
                        verdict="auth_smoke_failed" if not infra else reason,
                        reason=(smoke.output or "")[-2000:],
                        hypothesis=hypothesis,
                        reverted=True,
                    )
                )
                worker.save_checkpoint()
                _record_failure(
                    art,
                    attempt=attempt,
                    hypothesis=hypothesis,
                    reason=reason,
                )
                if infra or consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            probe = run_optimization_probe(pipeline_config, workload, query)
            if not probe.passed:
                probe_text = f"{probe.diff_summary or ''}\n{probe.output or ''}"
                infra = _fail_fast_infra_if_needed(
                    pipeline_config, workload, query, text=probe_text
                )
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                worker.pending.push(
                    format_gate_delta(
                        verdict=(
                            f"infra_fail_fast:{infra.get('infra_reason')}"
                            if infra
                            else "auth_probe_failed"
                        ),
                        reason=probe.diff_summary or (probe.output or "")[-2000:],
                        hypothesis=hypothesis,
                        reverted=True,
                    )
                )
                worker.save_checkpoint()
                _record_failure(
                    art,
                    attempt=attempt,
                    hypothesis=hypothesis,
                    reason=(
                        f"infra_fail_fast:{infra.get('infra_reason')}"
                        if infra
                        else f"auth_probe_failed: {probe.diff_summary or ''}"
                    ),
                )
                if infra or consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            structural, structural_error = _structural_failure(
                pipeline_config, workload, query, art, hypothesis, optimization_iter
            )
            if structural_error:
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                worker.pending.push(
                    format_gate_delta(
                        verdict="structural_gates_failed",
                        reason=structural_error,
                        hypothesis=hypothesis,
                        reverted=True,
                    )
                )
                worker.save_checkpoint()
                _record_failure(art, attempt=attempt, hypothesis=hypothesis, reason=structural_error)
                if consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            candidate = run_multi_sample_benchmark(
                pipeline_config,
                workload,
                query,
                samples=eval_config.quick_benchmark_samples,
                include_profile=False,
            )
            if not candidate:
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                worker.pending.push(
                    format_gate_delta(
                        verdict="auth_benchmark_failed",
                        hypothesis=hypothesis,
                        reverted=True,
                    )
                )
                worker.save_checkpoint()
                _record_failure(art, attempt=attempt, hypothesis=hypothesis, reason="auth_benchmark_failed")
                if consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            outcome = evaluate_attempt_outcome(
                baseline_score=baseline.median_score,
                candidate_score=candidate.median_score,
                baseline_metrics=baseline.median_metrics,
                candidate_metrics=candidate.median_metrics,
                config=eval_config,
                correctness_ok=True,
                measurement=pipeline_config.measurement,
                structural=structural,
            )
            _raise_if_invalid_measurement(outcome)
            log_event(
                pipeline_config.repo_root,
                query,
                workload,
                "optimization_worker_gate_verdict",
                verdict=outcome.verdict,
                improvement_pct=outcome.improvement_pct,
                workload_name=workload.get("name", workload_name),
            )

            if outcome.verdict in ("reject_broken", "reject_regress"):
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                worker.pending.push(
                    format_gate_delta(
                        verdict=outcome.verdict,
                        reason=outcome.reason,
                        hypothesis=hypothesis,
                        improvement_pct=outcome.improvement_pct,
                        reverted=True,
                    )
                )
                _record_failure(
                    art,
                    attempt=attempt,
                    hypothesis=hypothesis,
                    reason=outcome.reason,
                    improvement_pct=outcome.improvement_pct,
                    candidate_metrics=candidate.median_metrics,
                    repo_root=pipeline_config.repo_root,
                    query=query,
                    workload=workload,
                    verdict=outcome.verdict,
                )
                worker.save_checkpoint()
                if consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            if outcome.verdict == "keep_explore":
                if experimental_depth >= EXPERIMENTAL_CHAIN_CAP:
                    _revert_and_rebuild(pipeline_config, workload, query)
                    consecutive_fails += 1
                    _record_failure(
                        art,
                        attempt=attempt,
                        hypothesis=hypothesis,
                        reason="keep_explore_chain_cap_reached",
                        improvement_pct=outcome.improvement_pct,
                        candidate_metrics=candidate.median_metrics,
                    )
                    worker.save_checkpoint()
                    if consecutive_fails >= consecutive_fail_stop:
                        break
                    continue
                experimental_depth += 1
                consecutive_fails = 0
                save_experimental_branch(
                    art,
                    attempt_id=attempt_id,
                    chain_depth=experimental_depth,
                    diagnostic_summary=outcome.diagnostic_vector,
                )
                worker.pending.push(
                    format_gate_delta(
                        verdict="keep_explore",
                        reason=outcome.reason,
                        hypothesis=hypothesis,
                        improvement_pct=outcome.improvement_pct,
                    )
                )
                append_attempt_log(
                    art,
                    {
                        "attempt": attempt,
                        "hypothesis": (hypothesis or {}).get("bottleneck", ""),
                        "hypothesis_json": hypothesis,
                        "accepted": False,
                        "kept": True,
                        "verdict": "keep_explore",
                        "improvement_pct": outcome.improvement_pct,
                        "metrics": candidate.median_metrics,
                        "diagnostic_vector": outcome.diagnostic_vector,
                        "reason": outcome.reason,
                    },
                )
                clear_attempt_checkpoint(art)
                worker.save_checkpoint()
                continue

            existing_best = load_best_valid_baseline(art)
            existing_metrics = dict(existing_best.get("metrics") or {})
            existing_score = score_metrics(
                existing_metrics, is_aux=True, workload=workload
            ) if existing_metrics else float(existing_best.get("score") or 0.0)
            if existing_score <= 0:
                existing_score = float(existing_best.get("score") or 0.0)
            if existing_score > 0 and candidate.median_score + 1e-9 < existing_score:
                # Local baseline can be noisy; never ratchet best_valid downward.
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                worker.pending.push(
                    format_gate_delta(
                        verdict="reject_regress",
                        reason=(
                            f"candidate_score={candidate.median_score:.4f} below "
                            f"best_valid={existing_score:.4f}"
                        ),
                        hypothesis=hypothesis,
                        improvement_pct=outcome.improvement_pct,
                    )
                )
                append_attempt_log(
                    art,
                    {
                        "attempt": attempt,
                        "hypothesis": (hypothesis or {}).get("bottleneck", ""),
                        "hypothesis_json": hypothesis,
                        "accepted": False,
                        "kept": False,
                        "verdict": "reject_regress",
                        "improvement_pct": outcome.improvement_pct,
                        "metrics": candidate.median_metrics,
                        "reason": "below_best_valid_baseline",
                    },
                )
                clear_attempt_checkpoint(art)
                worker.save_checkpoint()
                if consecutive_fails >= consecutive_fail_stop:
                    break
                continue

            advance_best_valid_baseline(
                pipeline_config,
                workload,
                query,
                attempt_id=attempt_id,
                score=candidate.median_score,
                metrics=candidate.median_metrics,
            )
            if structural:
                record_design_revision(
                    art, attempt_id=attempt_id, hypothesis=hypothesis,
                    before=baseline.median_metrics, after=candidate.median_metrics,
                )
            experimental_depth = 0
            clear_attempt_checkpoint(art)
            consecutive_fails = 0
            baseline = candidate
            evidence = _build_evidence(pipeline_config, workload, query, baseline)
            worker.pending.push(
                format_gate_delta(
                    verdict="accept",
                    reason=outcome.reason,
                    hypothesis=hypothesis,
                    improvement_pct=outcome.improvement_pct,
                )
            )
            append_attempt_log(
                art,
                {
                    "attempt": attempt,
                    "hypothesis": (hypothesis or {}).get("bottleneck", ""),
                    "hypothesis_json": hypothesis,
                    "accepted": True,
                    "kept": True,
                    "verdict": "accept",
                    "improvement_pct": outcome.improvement_pct,
                    "metrics": candidate.median_metrics,
                    "diagnostic_vector": outcome.diagnostic_vector,
                    "reason": outcome.reason,
                },
            )
            worker.save_checkpoint()
            continue

        packet = build_context_packet(
            pipeline_config,
            workload,
            query,
            evidence=evidence,
            baseline_metrics=baseline.median_metrics,
            baseline_score=baseline.median_score,
        )
        context_md_path = save_context_packet(pipeline_config, workload, query, packet)
        context_markdown = context_md_path.read_text(encoding="utf-8")
        bootstrap_markdown, bootstrap_sources = build_optimization_bootstrap(
            pipeline_config, workload, query
        )
        save_optimization_bootstrap(art, bootstrap_markdown, bootstrap_sources)
        print(
            f"[pipeline] optimization bootstrap: {len(bootstrap_sources)} files, "
            f"{len(bootstrap_markdown)} chars (sources inlined in the agent message)",
            file=sys.stderr,
            flush=True,
        )
        refresh_optimization_runtime(
            pipeline_config,
            workload,
            query,
            optimization_iter=optimization_iter,
            episode=attempt,
        )

        unified = _run_unified_optimization_attempt(
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            optimization_iter=optimization_iter,
            attempt=attempt,
            context_markdown=context_markdown,
            bootstrap_markdown=bootstrap_markdown,
            model_overrides=model_overrides,
            experimental_summary=experimental_summary,
        )
        save_attempt_state(art, {"attempt": attempt, "phase": "unified", "status": unified.status})
        last_result = unified
        hypothesis = unified.hypothesis

        if unified.status == "saturated":
            honored, detail = _try_honor_saturation(
                pipeline_config=pipeline_config,
                workload=workload,
                query=query,
                art=art,
                optimization_iter=optimization_iter,
                attempt=attempt,
                payload=unified.saturation,
                worker=None,
            )
            if honored:
                print(
                    f"[pipeline] OPTIMIZATION_SATURATED honored — {detail}",
                    file=sys.stderr,
                    flush=True,
                )
                break
            unified.status = "failed"
            unified.reason = detail
            last_result = unified
            consecutive_fails += 1
            if consecutive_fails >= consecutive_fail_stop:
                break
            continue

        if unified.status == "cannot_test":
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=f"cannot_test: {unified.reason}",
            )
            if consecutive_fails >= consecutive_fail_stop:
                break
            continue

        if unified.status != "complete":
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=unified.reason or "unified_attempt_failed",
            )
            if consecutive_fails >= consecutive_fail_stop:
                break
            continue

        print(
            "[pipeline] patch complete; running build → smoke (x"
            f"{_smoke_runs(pipeline_config)}) → correctness probe → quick bench "
            "(attempt gates only; full suite runs end of iteration)",
            file=sys.stderr,
            flush=True,
        )
        try:
            built, build_out = _run_build_with_fix_loop(
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name=workload_name,
                workload=workload,
                query=query,
                optimization_iter=optimization_iter,
                attempt=attempt,
                model_overrides=model_overrides,
            )
        except InfraFailFastError as exc:
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=str(exc),
            )
            break
        except RuntimeError as exc:
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=f"build_fix_exhausted: {exc}",
            )
            if consecutive_fails >= consecutive_fail_stop:
                break
            continue
        if not built:
            infra = _fail_fast_infra_if_needed(
                pipeline_config, workload, query, text=build_out or ""
            )
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=(
                    f"infra_fail_fast:{infra.get('infra_reason')}"
                    if infra
                    else f"build_failed: {build_out[-2000:]}"
                ),
            )
            if infra or consecutive_fails >= consecutive_fail_stop:
                break
            continue

        try:
            smoke = _run_smoke_with_fix_loop(
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name=workload_name,
                workload=workload,
                query=query,
                optimization_iter=optimization_iter,
                attempt=attempt,
                model_overrides=model_overrides,
            )
        except InfraFailFastError as exc:
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=str(exc),
            )
            break
        except RuntimeError as exc:
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=f"correctness_fix_exhausted: {exc}",
            )
            if consecutive_fails >= consecutive_fail_stop:
                break
            continue
        if not smoke.passed:
            infra = _fail_fast_infra_if_needed(
                pipeline_config, workload, query, text=smoke.output or ""
            )
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=(
                    f"infra_fail_fast:{infra.get('infra_reason')}"
                    if infra
                    else f"quick_correctness_failed (ctest -R {smoke.regex}): {(smoke.output or '')[-2000:]}"
                ),
            )
            if infra or consecutive_fails >= consecutive_fail_stop:
                break
            continue

        probe = run_optimization_probe(pipeline_config, workload, query)
        if not probe.passed:
            probe_text = f"{probe.diff_summary or ''}\n{probe.output or ''}"
            infra = _fail_fast_infra_if_needed(
                pipeline_config, workload, query, text=probe_text
            )
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=(
                    f"infra_fail_fast:{infra.get('infra_reason')}"
                    if infra
                    else f"correctness_probe_failed: {probe.diff_summary or probe.output[-2000:]}"
                ),
            )
            if infra or consecutive_fails >= consecutive_fail_stop:
                break
            continue

        structural, structural_error = _structural_failure(
            pipeline_config, workload, query, art, hypothesis, optimization_iter
        )
        if structural_error:
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(art, attempt=attempt, hypothesis=hypothesis, reason=structural_error)
            if consecutive_fails >= consecutive_fail_stop:
                break
            continue

        candidate = run_multi_sample_benchmark(
            pipeline_config,
            workload,
            query,
            samples=eval_config.quick_benchmark_samples,
            include_profile=False,
        )
        if not candidate:
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(art, attempt=attempt, hypothesis=hypothesis, reason="benchmark_failed")
            if consecutive_fails >= consecutive_fail_stop:
                break
            continue

        outcome = evaluate_attempt_outcome(
            baseline_score=baseline.median_score,
            candidate_score=candidate.median_score,
            baseline_metrics=baseline.median_metrics,
            candidate_metrics=candidate.median_metrics,
            config=eval_config,
            correctness_ok=True,
            measurement=pipeline_config.measurement,
            structural=structural,
        )
        _raise_if_invalid_measurement(outcome)

        if outcome.verdict in ("reject_broken", "reject_regress"):
            _revert_and_rebuild(pipeline_config, workload, query)
            consecutive_fails += 1
            _record_failure(
                art,
                attempt=attempt,
                hypothesis=hypothesis,
                reason=outcome.reason,
                improvement_pct=outcome.improvement_pct,
                candidate_metrics=candidate.median_metrics,
                repo_root=pipeline_config.repo_root,
                query=query,
                workload=workload,
                verdict=outcome.verdict,
            )
            if consecutive_fails >= consecutive_fail_stop:
                break
            continue

        if outcome.verdict == "keep_explore":
            if experimental_depth >= EXPERIMENTAL_CHAIN_CAP:
                _revert_and_rebuild(pipeline_config, workload, query)
                consecutive_fails += 1
                _record_failure(
                    art,
                    attempt=attempt,
                    hypothesis=hypothesis,
                    reason="keep_explore_chain_cap_reached",
                    improvement_pct=outcome.improvement_pct,
                    candidate_metrics=candidate.median_metrics,
                )
                if consecutive_fails >= consecutive_fail_stop:
                    break
                continue
            experimental_depth += 1
            consecutive_fails = 0
            save_experimental_branch(
                art,
                attempt_id=attempt_id,
                chain_depth=experimental_depth,
                diagnostic_summary=outcome.diagnostic_vector,
            )
            append_attempt_log(
                art,
                {
                    "attempt": attempt,
                    "hypothesis": (hypothesis or {}).get("bottleneck", ""),
                    "hypothesis_json": hypothesis,
                    "accepted": False,
                    "kept": True,
                    "verdict": "keep_explore",
                    "improvement_pct": outcome.improvement_pct,
                    "metrics": candidate.median_metrics,
                    "diagnostic_vector": outcome.diagnostic_vector,
                    "reason": outcome.reason,
                },
            )
            clear_attempt_checkpoint(art)
            continue

        advance_best_valid_baseline(
            pipeline_config,
            workload,
            query,
            attempt_id=attempt_id,
            score=candidate.median_score,
            metrics=candidate.median_metrics,
        )
        if structural:
            record_design_revision(
                art, attempt_id=attempt_id, hypothesis=hypothesis,
                before=baseline.median_metrics, after=candidate.median_metrics,
            )
        experimental_depth = 0
        clear_attempt_checkpoint(art)
        consecutive_fails = 0
        baseline = candidate
        evidence = _build_evidence(pipeline_config, workload, query, baseline)
        append_attempt_log(
            art,
            {
                "attempt": attempt,
                "hypothesis": (hypothesis or {}).get("bottleneck", ""),
                "hypothesis_json": hypothesis,
                "accepted": True,
                "kept": True,
                "verdict": "accept",
                "improvement_pct": outcome.improvement_pct,
                "metrics": candidate.median_metrics,
                "diagnostic_vector": outcome.diagnostic_vector,
                "reason": outcome.reason,
            },
        )

    log_stage_complete(
        pipeline_config.repo_root,
        query,
        workload,
        stage="optimization",
        model_id=make_model_id("optimization", pipeline_config, model_config, model_overrides),
        backend="codex_exec",
        duration_ms=None,
        usage=None,
        optimization_iter=optimization_iter,
    )
    return last_result
