"""One optimization iteration: the persistent worker proposes candidates; each is gated, measured, then kept or reverted."""

from __future__ import annotations

import json
import sys
import time
from pathlib import Path
from typing import Any

from accel_pipeline.aux_write_freqs import ensure_attempt_write_frequencies
from accel_pipeline.base_bench import build_optimization_diagnosis, load_base_optimization_state
from accel_pipeline.config import (
    ModelConfig,
    PipelineConfig,
    apply_workload_overrides,
    load_run_workload,
    resolve_optimization_edit_paths,
    resolve_workload_paths,
)
from accel_pipeline.failure import Failure, StageFailure
from accel_pipeline.infra_fail_fast import handle_infra_failure
from accel_pipeline.jsonio import read_json
from accel_pipeline.kernel_hot_path_review import analyze_kernel_source
from accel_pipeline.layout import query_dir
from accel_pipeline.measurement import check_measurement
from accel_pipeline.models import make_model_id
from accel_pipeline.negative_memory import maybe_record_reject_trap
from accel_pipeline.optimization_checkpoint import (
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
from accel_pipeline.optimization_context import build_initial_worker_seed, reconstruct_seed_from_artifacts
from accel_pipeline.optimization_eval import (
    AttemptOutcome,
    MultiSampleBenchmark,
    eval_config_from_pipeline,
    evaluate_attempt_outcome,
    is_aux_workload,
    run_multi_sample_benchmark,
    score_metrics,
)
from accel_pipeline.optimization_evidence import extract_bottleneck_evidence
from accel_pipeline.optimization_pending import format_gate_delta
from accel_pipeline.optimization_probe import run_optimization_probe
from accel_pipeline.optimization_saturation import REANALYSIS_MARKER_FILE, Verdict, judge_saturation_claim
from accel_pipeline.optimization_smoke import run_build, run_smoke_correctness_repeated
from accel_pipeline.optimization_structural import (
    design_revision_error,
    is_structural_attempt,
    record_design_revision,
    run_full_tier_gates,
)
from accel_pipeline.optimization_worker import CandidateCycleResult, OptimizationWorker
from accel_pipeline.optimization_worker_checkpoint import load_worker_checkpoint
from accel_pipeline.paths import analysis_artifact_path, load_query_analysis, query_artifacts_dir
from accel_pipeline.pipeline_log import log_event, log_stage_complete, log_stage_start
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.structural_reanalysis import write_request as write_structural_request
from accel_pipeline.triage import is_transient_provider_failure

EXPERIMENTAL_CHAIN_CAP = 3


def _log(message: str) -> None:
    print(f"[pipeline] {message}", file=sys.stderr, flush=True)


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
    _log("structural attempt: full-tier gates before the benchmark")
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
) -> tuple[bool, str]:
    """(honored, detail) for an OPTIMIZATION_SATURATED claim; a rejected claim is logged as a failed attempt."""
    verdict, detail, floors = judge_saturation_claim(pipeline_config, art, payload)
    honored = verdict is not Verdict.REJECT
    if verdict is Verdict.REANALYZE:
        _request_read_path_reanalysis(pipeline_config, workload, query, art, optimization_iter, detail)
    log_event(
        pipeline_config.repo_root,
        query,
        workload,
        "optimization_saturation_claim",
        honored=honored,
        detail=detail[:500],
        floors=floors,
        payload=payload or {},
        optimization_iter=optimization_iter,
        attempt=attempt,
        workload_name=workload.get("name"),
    )
    if not honored:
        _record_failure(art, attempt=attempt, hypothesis=None, verdict="saturation_floors_not_met",
                        reason=f"OPTIMIZATION_SATURATED not honored: {detail}")
        return False, detail
    append_experiment_log(
        pipeline_config.repo_root,
        query,
        f"- iter {optimization_iter} a{attempt}: OPTIMIZATION_SATURATED — {detail}",
        workload=workload,
    )
    append_attempt_log(art, {"attempt": attempt, "accepted": False, "kept": True, "verdict": "optimization_saturated",
                             "reason": detail, "saturation": payload or {}})
    clear_attempt_checkpoint(art)
    return True, detail


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


def _infra_failure(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    text: str,
) -> str:
    """The infra reason of a shared-tree failure (repaired, lease released), or "" for a failure of the code."""
    info = handle_infra_failure(pipeline_config, workload, query, text=text)
    if not info:
        return ""
    _log(
        f"fail-fast infra ({info.get('infra_reason')}): lease_released={info.get('lease_released')} "
        f"restored={list((info.get('repair') or {}).get('restored') or {})}"
    )
    return str(info.get("infra_reason") or "infra_failure")


def _revert_and_rebuild(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> None:
    """Restore checkpointed sources and rebuild so a reverted patch cannot leave a stale binary."""
    revert_attempt_checkpoint(pipeline_config, workload, query)
    built, build_out = run_build(pipeline_config, workload, query)
    if not built and not _infra_failure(pipeline_config, workload, query, build_out or ""):
        _log(f"warning: rebuild after revert failed: {(build_out or '')[-400:]}")


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
    kernel_review = analyze_kernel_source(pipeline_config.repo_root / edit_paths["primary_cpp"])
    diagnosis = build_optimization_diagnosis(
        baseline_summary,
        profile,
        best_speedup_median=float(opt_state.get("best_speedup_median") or 0.0),
        analysis=analysis or None,
        kernel_review=kernel_review,
    )
    if baseline.median_metrics.get("speedup_median") is not None:
        diagnosis["speedup_median"] = baseline.median_metrics.get("speedup_median")
    return extract_bottleneck_evidence(diagnosis, profile, kernel_review, analysis=analysis or None)


def _record_failure(
    art: Path,
    *,
    attempt: int,
    hypothesis: dict[str, Any] | None,
    verdict: str,
    reason: str,
    improvement_pct: float | None = None,
    candidate_metrics: dict[str, Any] | None = None,
) -> None:
    entry: dict[str, Any] = {
        "attempt": attempt,
        "hypothesis": (hypothesis or {}).get("bottleneck", ""),
        "hypothesis_json": hypothesis,
        "accepted": False,
        "kept": False,
        "verdict": verdict,
        "failure_reason": reason,
        "improvement_pct": improvement_pct,
    }
    if candidate_metrics:
        entry["candidate_metrics"] = candidate_metrics
    append_attempt_log(art, entry)


def _kept_entry(attempt: int, hypothesis: dict[str, Any] | None, verdict: str, outcome: AttemptOutcome,
                metrics: dict[str, Any]) -> dict[str, Any]:
    return {
        "attempt": attempt,
        "hypothesis": (hypothesis or {}).get("bottleneck", ""),
        "hypothesis_json": hypothesis,
        "accepted": verdict == "accept",
        "kept": True,
        "verdict": verdict,
        "improvement_pct": outcome.improvement_pct,
        "metrics": metrics,
        "diagnostic_vector": outcome.diagnostic_vector,
        "reason": outcome.reason,
    }


class _Iteration:
    """The attempts of one optimization iteration, each measured against the best candidate kept so far."""

    def __init__(
        self,
        pipeline_config: PipelineConfig,
        workload: dict[str, Any],
        query: int,
        optimization_iter: int,
        worker: OptimizationWorker,
        baseline: MultiSampleBenchmark,
    ):
        self.config = pipeline_config
        self.workload = workload
        self.query = query
        self.optimization_iter = optimization_iter
        self.worker = worker
        self.baseline = baseline
        self.art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
        self.eval_config = eval_config_from_pipeline(pipeline_config)
        self.max_attempts = pipeline_config.optimization_max_attempts_per_iteration
        configured_fail_stop = pipeline_config.optimization_consecutive_fail_stop
        self.fail_stop = configured_fail_stop if configured_fail_stop > 0 else self.max_attempts + 1
        self.consecutive_fails = 0
        self.experimental_depth = 0
        self.done = False
        self.last_result: CandidateCycleResult | None = None

    def run(self, deadline: float | None) -> CandidateCycleResult | None:
        for attempt in range(1, self.max_attempts + 1):
            if self.done or self.consecutive_fails >= self.fail_stop:
                break
            if deadline is not None and time.time() >= deadline:
                break
            self.attempt(attempt)
        return self.last_result

    def attempt(self, attempt: int) -> None:
        attempt_id = f"iter{self.optimization_iter}_a{attempt}"
        freqs = ensure_attempt_write_frequencies(self.art, self.workload, optimization_iter=self.optimization_iter, attempt=attempt)
        _log(f"optimization write frequencies (iter {self.optimization_iter} attempt {attempt}): {freqs}")
        begin_attempt_checkpoint(self.config, self.workload, self.query, attempt_id=attempt_id, hypothesis="pending")
        experimental = load_experimental_branch(self.art)
        if experimental:
            self.experimental_depth = int(experimental.get("chain_depth") or 0)

        cycle = self._run_cycle(attempt, experimental)
        self.last_result = cycle
        hypothesis = cycle.hypothesis
        if cycle.status == "saturated":
            self._claim_saturation(attempt, cycle)
            return
        if cycle.status == "cannot_test":
            self.reject(attempt, hypothesis, "cannot_test", logged=f"cannot_test: {cycle.reason}", tell_worker=False)
            return
        if cycle.status != "ready":
            reason = cycle.reason or "worker_candidate_failed"
            _log(f"candidate failed (no CANDIDATE_READY): {' '.join(reason.split())[:240]}")
            self.reject(attempt, hypothesis, "candidate_failed", logged=reason, tell_worker=False)
            return

        _log(f"CANDIDATE_READY; running authoritative build → smoke (x{self.config.optimization_smoke_runs}) "
             "→ probe → quick bench")
        structural = self._gate(attempt, hypothesis)
        if structural is None:
            return
        candidate = run_multi_sample_benchmark(
            self.config, self.workload, self.query,
            samples=self.eval_config.quick_benchmark_samples, include_profile=False,
        )
        if not candidate:
            self.reject(attempt, hypothesis, "auth_benchmark_failed")
            return
        outcome = evaluate_attempt_outcome(
            baseline_score=self.baseline.median_score,
            candidate_score=candidate.median_score,
            baseline_metrics=self.baseline.median_metrics,
            candidate_metrics=candidate.median_metrics,
            config=self.eval_config,
            correctness_ok=True,
            measurement=self.config.measurement,
            structural=structural,
        )
        _raise_if_invalid_measurement(outcome)
        log_event(self.config.repo_root, self.query, self.workload, "optimization_worker_gate_verdict",
                  verdict=outcome.verdict, improvement_pct=outcome.improvement_pct,
                  workload_name=self.workload.get("name"))
        self._decide(attempt_id, attempt, hypothesis, outcome, candidate, structural)

    def _run_cycle(self, attempt: int, experimental: dict[str, Any]) -> CandidateCycleResult:
        """One candidate cycle of the worker, retried on transient provider failures."""
        self.worker.episode_meta.attempt = attempt
        message = self.worker.pending.flush_into_attempt_prompt(
            optimization_iter=self.optimization_iter,
            attempt=attempt,
            experimental_summary=json.dumps(experimental.get("diagnostic_summary") or {}, indent=2) if experimental else "",
            max_chars=self.config.optimization_turn_delta_max_chars,
        )
        cycle = self.worker.run_candidate_cycle(message)
        retries = self.config.repair.provider_retries
        for retry in range(1, retries + 1):
            if cycle.status in ("ready", "saturated", "cannot_test") or not is_transient_provider_failure(cycle.reason):
                break
            _log(f"transient provider failure on attempt {attempt} (retry {retry}/{retries}): "
                 f"{' '.join(cycle.reason.split())[:240]}")
            time.sleep(self.config.repair.provider_backoff(retry))
            cycle = self.worker.run_candidate_cycle(message)
        return cycle

    def _claim_saturation(self, attempt: int, cycle: CandidateCycleResult) -> None:
        honored, detail = _try_honor_saturation(
            pipeline_config=self.config, workload=self.workload, query=self.query, art=self.art,
            optimization_iter=self.optimization_iter, attempt=attempt, payload=cycle.saturation,
        )
        if honored:
            _log(f"OPTIMIZATION_SATURATED honored — {detail}")
            self.done = True
            return
        # A rejected claim must not read as an iteration-level stop.
        cycle.status, cycle.reason = "failed", detail
        self.worker.pending.push(f"OPTIMIZATION_SATURATED not honored: {detail}. Continue optimizing.")
        self.worker.save_checkpoint()
        self.consecutive_fails += 1

    def _gate(self, attempt: int, hypothesis: dict[str, Any] | None) -> bool | None:
        """Build, smoke, probe and (for a redesign) the full-tier gates; whether the candidate is structural, None if rejected."""
        built, build_out = run_build(self.config, self.workload, self.query)
        if not built:
            output = build_out or ""
            self._reject_gate(attempt, hypothesis, "auth_build_failed", output, feedback=output[-2000:],
                              logged="auth_build_failed")
            return None
        smoke = run_smoke_correctness_repeated(self.config, self.workload, self.query,
                                               runs=self.config.optimization_smoke_runs)
        if not smoke.passed:
            output = smoke.output or ""
            self._reject_gate(attempt, hypothesis, "auth_smoke_failed", output, feedback=output[-2000:],
                              logged=f"auth_smoke_failed: {output[-500:]}")
            return None
        probe = run_optimization_probe(self.config, self.workload, self.query)
        if not probe.passed:
            self._reject_gate(attempt, hypothesis, "auth_probe_failed", f"{probe.diff_summary or ''}\n{probe.output or ''}",
                              feedback=probe.diff_summary or (probe.output or "")[-2000:],
                              logged=f"auth_probe_failed: {probe.diff_summary or ''}")
            return None
        structural, error = _structural_failure(self.config, self.workload, self.query, self.art, hypothesis, self.optimization_iter)
        if error:
            self.reject(attempt, hypothesis, "structural_gates_failed", feedback=error)
            return None
        return structural

    def _reject_gate(self, attempt: int, hypothesis: dict[str, Any] | None, verdict: str, output: str, *,
                     feedback: str, logged: str) -> None:
        """A failed gate (its `output`): an infra failure of the shared tree ends the iteration, a code failure is
        one more failed attempt."""
        infra = _infra_failure(self.config, self.workload, self.query, output)
        if infra:
            verdict = logged = f"infra_fail_fast:{infra}"
        self.reject(attempt, hypothesis, verdict, feedback=feedback, logged=logged, stop=bool(infra))

    def _decide(self, attempt_id: str, attempt: int, hypothesis: dict[str, Any] | None, outcome: AttemptOutcome,
                candidate: MultiSampleBenchmark, structural: bool) -> None:
        metrics = candidate.median_metrics
        if outcome.verdict in ("reject_broken", "reject_regress"):
            self.reject(attempt, hypothesis, outcome.verdict, feedback=outcome.reason,
                        improvement_pct=outcome.improvement_pct, metrics=metrics)
            maybe_record_reject_trap(self.config.repo_root, query=self.query, workload=self.workload,
                                     hypothesis=hypothesis, verdict=outcome.verdict, reason=outcome.reason,
                                     improvement_pct=outcome.improvement_pct)
            return
        if outcome.verdict == "keep_explore":
            if self.experimental_depth >= EXPERIMENTAL_CHAIN_CAP:
                self.reject(attempt, hypothesis, "keep_explore_chain_cap_reached",
                            improvement_pct=outcome.improvement_pct, metrics=metrics)
                return
            self.experimental_depth += 1
            save_experimental_branch(self.art, attempt_id=attempt_id, chain_depth=self.experimental_depth,
                                     diagnostic_summary=outcome.diagnostic_vector)
            self._keep(attempt, hypothesis, "keep_explore", outcome, metrics)
            return
        best_score = self._best_valid_score()
        if best_score > 0 and candidate.median_score + 1e-9 < best_score:
            # The quick baseline is noisy; best_valid never ratchets down.
            self.reject(attempt, hypothesis, "reject_regress",
                        feedback=f"candidate_score={candidate.median_score:.4f} below best_valid={best_score:.4f}",
                        logged="below_best_valid_baseline",
                        improvement_pct=outcome.improvement_pct, metrics=metrics)
            return
        advance_best_valid_baseline(self.config, self.workload, self.query, attempt_id=attempt_id,
                                    score=candidate.median_score, metrics=metrics)
        if structural:
            record_design_revision(self.art, attempt_id=attempt_id, hypothesis=hypothesis,
                                   before=self.baseline.median_metrics, after=metrics)
        self.experimental_depth = 0
        self.baseline = candidate
        self._keep(attempt, hypothesis, "accept", outcome, metrics)

    def _best_valid_score(self) -> float:
        best = load_best_valid_baseline(self.art)
        metrics = dict(best.get("metrics") or {})
        score = score_metrics(metrics, is_aux=True, workload=self.workload) if metrics else 0.0
        return score if score > 0 else float(best.get("score") or 0.0)

    def _keep(self, attempt: int, hypothesis: dict[str, Any] | None, verdict: str, outcome: AttemptOutcome,
              metrics: dict[str, Any]) -> None:
        self.consecutive_fails = 0
        clear_attempt_checkpoint(self.art)
        self.worker.pending.push(format_gate_delta(verdict=verdict, reason=outcome.reason, hypothesis=hypothesis,
                                                   improvement_pct=outcome.improvement_pct))
        append_attempt_log(self.art, _kept_entry(attempt, hypothesis, verdict, outcome, metrics))
        self.worker.save_checkpoint()

    def reject(
        self,
        attempt: int,
        hypothesis: dict[str, Any] | None,
        verdict: str,
        *,
        feedback: str = "",
        logged: str = "",
        tell_worker: bool = True,
        stop: bool = False,
        improvement_pct: float | None = None,
        metrics: dict[str, Any] | None = None,
    ) -> None:
        """Revert the candidate, tell the worker why (`feedback`), and log the failed attempt (`logged`)."""
        _revert_and_rebuild(self.config, self.workload, self.query)
        self.consecutive_fails += 1
        if tell_worker:
            self.worker.pending.push(format_gate_delta(verdict=verdict, reason=feedback, hypothesis=hypothesis,
                                                       improvement_pct=improvement_pct, reverted=True))
        _record_failure(self.art, attempt=attempt, hypothesis=hypothesis, verdict=verdict,
                        reason=logged or feedback or verdict, improvement_pct=improvement_pct,
                        candidate_metrics=metrics)
        self.worker.save_checkpoint()
        self.done = self.done or stop


def _preflight_build(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> None:
    _log("preflight build before optimization iteration")
    built, output = run_build(pipeline_config, workload, query)
    if built:
        return
    infra = _infra_failure(pipeline_config, workload, query, output or "")
    raise StageFailure(Failure(
        kind="infra" if infra else "code",
        stage="optimization",
        source="preflight_build",
        summary="preflight build failed before the optimization iteration; fix the tree or restore the checkpoint",
        evidence=(output or "")[-2000:],
        infra_reason=infra or None,
    ))


def _seed_worker(
    worker: OptimizationWorker,
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    baseline: MultiSampleBenchmark,
) -> None:
    """The worker's first message: rebuilt from the artifacts on resume, else from the baseline's evidence."""
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    checkpoint = load_worker_checkpoint(art, pipeline_config.optimization_checkpoint_file)
    if checkpoint:
        seed = reconstruct_seed_from_artifacts(pipeline_config, workload, query, checkpoint=checkpoint)
    else:
        seed = build_initial_worker_seed(
            pipeline_config,
            workload,
            query,
            evidence=_build_evidence(pipeline_config, workload, query, baseline),
            baseline_metrics=baseline.median_metrics,
            baseline_score=baseline.median_score,
        )
    worker.seed_episode(seed)


def run_optimization_iteration(
    *,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    optimization_iter: int,
    worker: OptimizationWorker,
    model_overrides: dict[str, str] | None = None,
    deadline: float | None = None,
) -> CandidateCycleResult | None:
    """One optimization iteration; no new attempt starts after `deadline` (time.time())."""
    workload = load_run_workload(pipeline_config, workload_name)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    model_id = make_model_id("optimization", pipeline_config, model_config, model_overrides)
    log_stage_start(pipeline_config.repo_root, query, workload, stage="optimization", model_id=model_id,
                    optimization_iter=optimization_iter)

    _preflight_build(pipeline_config, workload, query)
    freqs = ensure_attempt_write_frequencies(art, workload, optimization_iter=optimization_iter, attempt=0)
    _log(f"optimization write frequencies (baseline attempt 0, iter {optimization_iter}): {freqs}")
    _log("measuring optimization baseline (aux: 0% + sampled write mixes, "
         "score=dual_axis_weighted read^0.75*write_mix^0.25)")
    baseline = _measure_baseline(pipeline_config, workload, query, eval_config_from_pipeline(pipeline_config))
    if not load_best_valid_baseline(art):
        snapshot_best_valid_baseline_files(pipeline_config, workload, query)
        advance_best_valid_baseline(pipeline_config, workload, query, attempt_id=f"iter{optimization_iter}_baseline",
                                    score=baseline.median_score, metrics=baseline.median_metrics)

    worker.episode_meta.optimization_iter = optimization_iter
    if not worker.seeded:
        _seed_worker(worker, pipeline_config, workload, query, baseline)
    result = _Iteration(pipeline_config, workload, query, optimization_iter, worker, baseline).run(deadline)

    log_stage_complete(pipeline_config.repo_root, query, workload, stage="optimization", model_id=model_id,
                       backend="codex_exec", duration_ms=None, usage=None, optimization_iter=optimization_iter)
    return result
