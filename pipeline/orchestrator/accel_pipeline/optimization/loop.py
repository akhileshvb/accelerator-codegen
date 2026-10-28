"""The optimization stage: attempts at making the accelerator faster, each measured and kept or reverted.

An iteration measures a baseline, then runs attempts. An attempt asks the agent for one hypothesis and its patch
(through the stage's persistent worker, or two-phase hypothesis/patch turns), then judges the patch itself rather
than trusting the agent: build, smoke ctest, a correctness probe, a structural redesign's full-tier gates, the
benchmark, and the verdict (`bench.evaluate_attempt_outcome`). A kept patch becomes the best valid baseline, a
rejected one is reverted from the attempt's checkpoint. The stage runs iterations, then the full gate batch.

Everything the loop keeps on disk is here too: the attempt checkpoint and the best valid baseline, the best
kernel snapshot, the optimization state the bench sweeps feed, and the A/B trial tools the agent calls.
What attempts teach is `memory`; how the numbers are made is `bench`.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from collections import Counter
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

from accel_pipeline.agent import MAIN, StageRunRequest, create_agent_backend, run_in_session
from accel_pipeline.config import (
    ModelConfig,
    PipelineConfig,
    analysis_artifact_path,
    apply_workload_overrides,
    load_pipeline_config,
    load_query_analysis,
    load_run_workload,
    make_model_id,
    query_artifacts_dir,
    read_json,
    resolve_optimization_edit_paths,
    resolve_stage_provider,
    resolve_workload_paths,
)
from accel_pipeline.gates import HostRun, ensure_build_dir, run_gate
from accel_pipeline.layout import code_stamp, query_dir
from accel_pipeline.mcp import ToolContext, none_default
from accel_pipeline.optimization import bench, memory
from accel_pipeline.optimization.bench import (
    AttemptOutcome,
    MultiSampleBenchmark,
    aux_optimization_score,
    build_win,
    check_measurement,
    eval_config_from_pipeline,
    evaluate_attempt_outcome,
    improvement_ratio,
    is_aux_workload,
    run_multi_sample_benchmark,
    score_metrics,
)
from accel_pipeline.pack import format_seed_pointer, lease_note, noise_warning
from accel_pipeline.prompts import (
    analysis_for_optimization,
    compact_base_analysis_for_optimization,
    render_prompt,
    save_rendered_prompt,
    with_codex_tooling,
)
from accel_pipeline.repair import (
    Failure,
    RepairCtx,
    StageFailure,
    extract_crash_highlights,
    handle_infra_failure,
    is_transient_provider_failure,
    run_gates_with_repair,
    run_with_repair,
)
from accel_pipeline.runlog import (
    append_experiment_log,
    log_base_bench_summary,
    log_event,
    log_stage_complete,
    log_stage_start,
)
from accel_pipeline.stages import (
    resolve_optimization_max_turns,
    workload_post_optimization_gates,
    write_request as write_structural_request,
)

HYPOTHESIS_COMPLETE = "HYPOTHESIS_COMPLETE"
PATCH_COMPLETE = "HYPOTHESIS_PATCH_COMPLETE"
CANDIDATE_READY = "CANDIDATE_READY"
CANNOT_TEST = "CANNOT_TEST"
EXPERIMENTAL_CHAIN_CAP = 3
_WORKER_STAGE = "optimization_worker"
_UNLIMITED_TURNS = 10_000


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _log(message: str) -> None:
    print(f"[pipeline] {message}", file=sys.stderr, flush=True)


# The files an attempt may change, and where they are kept: the attempt's checkpoint, the best valid baseline,
# the best kernel snapshot

ATTEMPT_CHECKPOINT = "attempt_checkpoint"
AB_CHECKPOINT = "ab_checkpoint"
ACTIVE_ATTEMPT_FILE = "active_attempt.json"
ACTIVE_TRIAL_FILE = "ab_active_trial.json"
BEST_VALID_BASELINE_FILE = "best_valid_baseline.json"
EXPERIMENTAL_BRANCH_FILE = "experimental_branch.json"
SNAPSHOT_DIRNAME = "best_kernel_snapshot"
MANIFEST_NAME = "manifest.json"


def resolve_snapshot_paths(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> list[Path]:
    """The repo files an attempt may change; optional files only when present."""
    repo_root = pipeline_config.repo_root
    if configured := list(workload.get("snapshot_paths", [])):
        paths = [repo_root / str(template).replace("{N}", str(query)) for template in configured]
        return [p for p in paths if p.is_file()]
    if workload.get("workload_kind") == "aux_state":
        edit = resolve_optimization_edit_paths(pipeline_config, workload, query)
        return [repo_root / rel for rel in edit["all_paths"] if (repo_root / rel).is_file()]
    paths = resolve_workload_paths(pipeline_config, workload, query)
    return [repo_root / paths[key] for key in ("header_path", "kernel_path", "package_path")]


def _tracked_files(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> list[Path]:
    return [p for p in resolve_snapshot_paths(pipeline_config, workload, query) if p.is_file()]


def _save_files(repo_root: Path, files: list[Path], dest_root: Path) -> list[str]:
    """Copy `files` under `dest_root` (replacing it) at their repo-relative paths."""
    shutil.rmtree(dest_root, ignore_errors=True)
    dest_root.mkdir(parents=True)
    saved = []
    for src in files:
        rel = src.relative_to(repo_root)
        (dest_root / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dest_root / rel)
        saved.append(str(rel))
    return saved


def _restore_files(repo_root: Path, rels: list[str], src_root: Path) -> list[str]:
    restored = []
    for rel in rels:
        if (src_root / rel).is_file():
            (repo_root / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src_root / rel, repo_root / rel)
            restored.append(rel)
    return restored


def _art(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> Path:
    return query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)


def begin_attempt_checkpoint(pipeline_config, workload, query, *, attempt_id: str, hypothesis: str) -> list[str]:
    art = _art(pipeline_config, workload, query)
    copied = _save_files(pipeline_config.repo_root, _tracked_files(pipeline_config, workload, query), art / ATTEMPT_CHECKPOINT)
    (art / ACTIVE_ATTEMPT_FILE).write_text(
        json.dumps({"attempt_id": attempt_id, "hypothesis": hypothesis, "files": copied, "ts": _now()}, indent=2), encoding="utf-8")
    return copied


def revert_attempt_checkpoint(pipeline_config, workload, query) -> list[str]:
    art = _art(pipeline_config, workload, query)
    if not (art / ACTIVE_ATTEMPT_FILE).is_file():
        return []
    active = json.loads((art / ACTIVE_ATTEMPT_FILE).read_text(encoding="utf-8"))
    restored = _restore_files(pipeline_config.repo_root, active.get("files") or [], art / ATTEMPT_CHECKPOINT)
    clear_attempt_checkpoint(art)
    return restored


def clear_attempt_checkpoint(art: Path) -> None:
    shutil.rmtree(art / ATTEMPT_CHECKPOINT, ignore_errors=True)
    (art / ACTIVE_ATTEMPT_FILE).unlink(missing_ok=True)


def _read_json_or_empty(path: Path) -> dict[str, Any]:
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return {}


def load_best_valid_baseline(art: Path) -> dict[str, Any]:
    return _read_json_or_empty(art / BEST_VALID_BASELINE_FILE)


def advance_best_valid_baseline(pipeline_config, workload, query, *, attempt_id: str, score: float, metrics: dict[str, Any]) -> list[str]:
    """The attempt's files become the best valid baseline (and, under worktree isolation, reach the primary tree)."""
    art = _art(pipeline_config, workload, query)
    snapshot_best_valid_baseline_files(pipeline_config, workload, query)
    files = [str(p.relative_to(pipeline_config.repo_root)) for p in _tracked_files(pipeline_config, workload, query)]
    (art / BEST_VALID_BASELINE_FILE).write_text(json.dumps(
        {"attempt_id": attempt_id, "score": score, "metrics": metrics, "checkpoint_files": files, "ts": _now()}, indent=2), encoding="utf-8")
    (art / EXPERIMENTAL_BRANCH_FILE).unlink(missing_ok=True)
    try:
        from accel_pipeline.pack import primary_repo_root, sync_owned_to_primary, using_worktree_isolation

        if using_worktree_isolation():
            bench_name = str(workload.get("artifact_namespace") or workload.get("benchmark_name") or "tpch")
            synced = sync_owned_to_primary(primary=primary_repo_root(pipeline_config.repo_root), worktree=Path(pipeline_config.repo_root),
                                           query=query, benchmark=bench_name)
            if synced:
                _log(f"synced {len(synced)} owned files to primary after accept")
    except Exception as exc:  # noqa: BLE001 — never fail accept on sync
        _log(f"primary sync after accept failed: {exc}")
    return files


def snapshot_best_valid_baseline_files(pipeline_config, workload, query) -> None:
    art = _art(pipeline_config, workload, query)
    _save_files(pipeline_config.repo_root, _tracked_files(pipeline_config, workload, query), art / "best_valid_baseline_snapshot")


def save_experimental_branch(art: Path, *, attempt_id: str, chain_depth: int, diagnostic_summary: dict[str, Any]) -> None:
    (art / EXPERIMENTAL_BRANCH_FILE).write_text(json.dumps(
        {"attempt_id": attempt_id, "chain_depth": chain_depth, "diagnostic_summary": diagnostic_summary, "ts": _now()}, indent=2), encoding="utf-8")


def load_experimental_branch(art: Path) -> dict[str, Any]:
    return _read_json_or_empty(art / EXPERIMENTAL_BRANCH_FILE)


def snapshot_dir(art: Path) -> Path:
    return art / SNAPSHOT_DIRNAME


def has_best_kernel_snapshot(art: Path) -> bool:
    return (snapshot_dir(art) / MANIFEST_NAME).is_file()


def snapshot_best_kernel(pipeline_config, workload, query, *, speedup_median: float, iteration: int, change_kind: str) -> Path:
    art = _art(pipeline_config, workload, query)
    dest = snapshot_dir(art)
    files = _save_files(pipeline_config.repo_root, _tracked_files(pipeline_config, workload, query), dest)
    manifest = {"speedup_median": speedup_median, "iteration": iteration, "change_kind": change_kind,
                "files": [{"repo_rel": rel, "snapshot_rel": rel} for rel in files]}
    (dest / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    return dest


def restore_best_kernel(pipeline_config, workload, query) -> dict[str, Any] | None:
    dest = snapshot_dir(_art(pipeline_config, workload, query))
    if not (dest / MANIFEST_NAME).is_file():
        return None
    manifest = json.loads((dest / MANIFEST_NAME).read_text(encoding="utf-8"))
    restored = _restore_files(pipeline_config.repo_root, [e["repo_rel"] for e in manifest.get("files", []) if e.get("repo_rel")], dest)
    return {**manifest, "restored_files": restored} if restored else None


def rebuild_workload_targets(pipeline_config, workload, query) -> bool:
    paths = resolve_workload_paths(pipeline_config, workload, query)
    bench_target = paths.get("aux_bench_target", "") if is_aux_workload(workload) else paths["bench_target"]
    targets = [t for t in (paths.get("test_target", ""), bench_target, "accelerator_codegen") if t]
    return HostRun.worker(pipeline_config, workload).build(*targets) is None


def maybe_restore_best_kernel(
    pipeline_config, workload, query, *, best_speedup_median: float, last_speedup_median: float, reason: str,
) -> bool:
    """Restore the on-disk kernel to the best-known snapshot when regressed."""
    if best_speedup_median <= 0 or last_speedup_median + 0.005 >= best_speedup_median:
        return False
    note = lambda text: append_experiment_log(pipeline_config.repo_root, query, f"- {reason}: {text}", workload=workload)  # noqa: E731
    if not has_best_kernel_snapshot(_art(pipeline_config, workload, query)):
        note(f"regressed ({last_speedup_median:.3f}x vs best {best_speedup_median:.3f}x) but no kernel snapshot exists — "
             "re-run implementation to seed best baseline")
        return False
    manifest = restore_best_kernel(pipeline_config, workload, query)
    if manifest is None:
        return False
    rebuild_workload_targets(pipeline_config, workload, query)
    note(f"auto-restored best kernel snapshot ({manifest.get('speedup_median', best_speedup_median):.3f}x from "
         f"{manifest.get('change_kind', 'unknown')} iter {manifest.get('iteration', '?')})")
    return True


# The optimization state: what each measured gate batch or sweep added, and the best so far

def bench_summary_path(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> Path:
    return query_artifacts_dir(repo_root, query, workload=workload) / "aux_bench_summary.json"


def load_optimization_state(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> dict[str, Any]:
    path = query_artifacts_dir(repo_root, query, workload=workload) / "optimization_state.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}


def save_optimization_state(repo_root: Path, query: int, state: dict[str, Any], workload: dict[str, Any] | None = None) -> Path:
    path = query_artifacts_dir(repo_root, query, workload=workload) / "optimization_state.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2), encoding="utf-8")
    return path


def _empty_state(active_candidate_id: str = "") -> dict[str, Any]:
    return {"active_candidate_id": active_candidate_id, "best_composite_score": 0.0, "best_iteration": 0, "history": []}


def initialize_optimization_state(repo_root: Path, query: int, analysis: dict[str, Any], workload: dict[str, Any] | None = None) -> dict[str, Any]:
    if existing := load_optimization_state(repo_root, query, workload):
        return existing
    selected = analysis.get("correctness_facts", {}).get("state_design", {}).get("selected_candidate_id", "")
    state = _empty_state(selected)
    save_optimization_state(repo_root, query, state, workload)
    return state


def record_sweep_in_optimization_state(
    repo_root: Path, query: int, *, iteration: int, summary: dict[str, Any], change_kind: str = "baseline",
    active_candidate_id: str | None = None, workload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    state = load_optimization_state(repo_root, query, workload) or _empty_state()
    if active_candidate_id:
        state["active_candidate_id"] = active_candidate_id
    score = float(summary.get("composite_score", 0.0) or 0.0)
    state.setdefault("history", []).append({
        "iteration": iteration, "change_kind": change_kind, "active_candidate_id": state.get("active_candidate_id", ""),
        "composite_score": score, "composite_workload_speedup": summary.get("composite_workload_speedup"),
        "read_only_query_speedup": summary.get("read_only_query_speedup"),
        "read_only_cold_start_workload_speedup": summary.get("read_only_cold_start_workload_speedup"),
        "by_write_frequency_pct": summary.get("by_write_frequency_pct", {}), "measurement_ok": summary.get("measurement_ok", True),
    })
    if summary.get("measurement_ok", True) and score >= float(state.get("best_composite_score", 0.0) or 0.0):
        state.update(best_composite_score=score, best_iteration=iteration, best_summary=summary)
    save_optimization_state(repo_root, query, state, workload)
    return state


def format_bench_experiment_log_line(iteration: int, summary: dict[str, Any], state: dict[str, Any]) -> str:
    by_freq = summary.get("by_write_frequency_pct", {})
    freq_bits = [f"{key}%→wl={by_freq[key].get('workload_speedup', 0):.2f}x/cold={by_freq[key].get('cold_start_workload_speedup', 0):.2f}x"
                 f"/q={by_freq[key].get('query_only_speedup', 0):.2f}x" for key in sorted(by_freq, key=int)]
    flag = " **new best**" if state.get("best_iteration") == iteration else ""
    if not summary.get("measurement_ok", True):
        flag = f" **INVALID MEASUREMENT: {summary['measurement_diagnosis'][0]}**"
    return (f"- iter {iteration}: bench_sweep composite={summary.get('composite_score', 0):.3f} "
            f"(wl_min={summary.get('composite_workload_speedup', 0):.2f}x, read={summary.get('read_only_query_speedup', 0):.2f}x, "
            f"cold_start={summary.get('read_only_cold_start_workload_speedup', 0):.2f}x) "
            f"candidate={state.get('active_candidate_id', '?')}{flag}; {' | '.join(freq_bits)}")


def record_aux_bench_sweep_outcome(
    repo_root: Path, query: int, summary: dict[str, Any], *, iteration: int, change_kind: str,
    active_candidate_id: str | None = None, workload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Update optimization_state and append experiment_log.md after a bench sweep (once per sweep)."""
    kind = "baseline" if change_kind == "implementation" else change_kind or "bench_sweep"
    state = load_optimization_state(repo_root, query, workload) or _empty_state()
    score = float(summary.get("composite_score", 0.0) or 0.0)
    last = (state.get("history") or [{}])[-1]
    if last and int(last.get("iteration", -1)) == iteration and str(last.get("change_kind", "")) == kind \
            and float(last.get("composite_score") or 0.0) == score:
        return state
    state = record_sweep_in_optimization_state(repo_root, query, iteration=iteration, summary=summary, change_kind=kind,
                                               active_candidate_id=active_candidate_id, workload=workload)
    append_experiment_log(repo_root, query, format_bench_experiment_log_line(iteration, summary, state), workload=workload)
    read_only = float(summary.get("read_only_query_speedup") or 0.0)
    if workload and kind in ("implementation", "baseline", "bench_sweep") and read_only > 0 and summary.get("measurement_ok", True):
        art = query_artifacts_dir(repo_root, query, workload=workload)
        if kind == "implementation" or not has_best_kernel_snapshot(art):
            snapshot_best_kernel(load_pipeline_config(repo_root=repo_root), workload, query, speedup_median=read_only,
                                 iteration=iteration, change_kind=kind)
    return state


def process_aux_bench_after_gates(
    repo_root: Path, query: int, *, iteration: int, change_kind: str, active_candidate_id: str | None = None,
    workload: dict[str, Any] | None = None,
) -> dict[str, Any] | None:
    """Read aux_bench_summary.json written by the sweep gate and update the optimization state."""
    summary_path = bench_summary_path(repo_root, query, workload)
    if not summary_path.is_file():
        return None
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    failures = check_measurement(summary, workload=workload or {}, phase="gate", config=load_pipeline_config(repo_root=repo_root).measurement)
    summary.update(measurement_ok=not failures, measurement_diagnosis=[d for f in failures for d in f.diagnosis])
    state = record_aux_bench_sweep_outcome(repo_root, query, summary, iteration=iteration, change_kind=change_kind,
                                           active_candidate_id=active_candidate_id, workload=workload)
    return {"summary": summary, "state": state}


def load_base_optimization_state(art: Path) -> dict[str, Any]:
    return _read_json_or_empty(art / "optimization_state.json") or {"best_speedup_median": 0.0, "iterations": []}


def process_base_bench_after_gates(
    repo_root: Path, query: int, workload: dict[str, Any], *, iteration: int = 0, change_kind: str = "implementation",
) -> dict[str, Any]:
    """Update the baseline/profile summaries and the optimization state after a base kernel's gates; the best
    kernel is snapshotted on an improvement and restored on a regression."""
    art = query_artifacts_dir(repo_root, query, workload=workload)
    baseline, profile = bench.write_baseline_summary(art), bench.write_profile_summary(art)
    _, analysis = load_query_analysis(repo_root, query, workload=workload)
    speedup = float(baseline.get("speedup_median") or 0.0)
    state = load_base_optimization_state(art)
    prev_best = float(state.get("best_speedup_median") or 0.0)
    pipeline_config = load_pipeline_config(repo_root=repo_root)
    paths = resolve_workload_paths(pipeline_config, workload, query)
    diagnosis = memory.diagnose(baseline, profile, best_speedup=prev_best, analysis=analysis or None,
                                kernel_review=memory.review_kernel(repo_root / paths["kernel_path"]))
    improved = speedup > prev_best
    state.setdefault("iterations", []).append({
        "iteration": iteration, "change_kind": change_kind, "speedup_median": speedup,
        "reference_median_us": baseline.get("reference_median_us"), "accelerator_median_us": baseline.get("accelerator_median_us"),
        "improved": improved, "dominant_phase": diagnosis.get("dominant_phase"),
    })
    if improved:
        state["best_speedup_median"] = speedup
    state["last_speedup_median"] = speedup
    (art / "optimization_state.json").write_text(json.dumps(state, indent=2), encoding="utf-8")
    best_now = float(state.get("best_speedup_median") or 0.0)
    if improved or (not has_best_kernel_snapshot(art) and speedup >= 1.0 and change_kind in ("implementation", "optimization")):
        snapshot_best_kernel(pipeline_config, workload, query, speedup_median=speedup, iteration=iteration, change_kind=change_kind)
    elif best_now > 0 and speedup + 0.005 < best_now:
        maybe_restore_best_kernel(pipeline_config, workload, query, best_speedup_median=best_now, last_speedup_median=speedup,
                                  reason=f"after {change_kind} iter {iteration}")
    append_experiment_log(
        repo_root, query,
        f"- {change_kind} iter {iteration}: speedup_median={speedup:.3f}x (ref={baseline.get('reference_median_us')}us "
        f"accel={baseline.get('accelerator_median_us')}us) phase={diagnosis.get('dominant_phase') or 'n/a'} "
        f"{'IMPROVED' if improved else 'no improvement'}", workload=workload)
    log_base_bench_summary(repo_root, query, diagnosis, change_kind=change_kind, iteration=iteration, improved=improved, workload=workload)
    return {"speedup_median": speedup, "improved": improved, "prev_best": prev_best, "baseline": baseline, "profile": profile,
            "diagnosis": diagnosis}


# Building and checking a patch

@dataclass
class SmokeResult:
    passed: bool
    output: str = ""
    regex: str = ""

    @property
    def diff_summary(self) -> str:
        markers = ("FAILED", "error:", "Assertion", "NULL", "mismatch", "expected")
        return "\n".join([line.strip() for line in self.output.splitlines() if any(m in line for m in markers)][-12:])


def _smoke_regex(workload: dict, paths: dict[str, str]) -> str:
    if workload.get("workload_kind") == "aux_state":
        explicit = paths.get("aux_ctest_smoke_regex") or workload.get("aux_ctest_smoke_regex")
        if explicit:
            return str(explicit)
        return f"{workload.get('benchmark_name', 'tpch')}_q{int(workload.get('query_number', 0) or 0)}_aux"
    return str(paths.get("ctest_smoke_regex") or paths.get("test_target", ""))


def run_smoke_correctness(pipeline_config: PipelineConfig, workload: dict, query: int, *, timeout_sec: int = 180) -> SmokeResult:
    """The workload's ctest smoke (after every optimization patch; also the fast correctness probe before the bench)."""
    paths = resolve_workload_paths(pipeline_config, workload, query)
    if not (regex := _smoke_regex(workload, paths)):
        return SmokeResult(passed=False, output="no smoke test regex configured")
    host = HostRun.worker(pipeline_config, workload)
    if problem := host.load():
        return SmokeResult(passed=False, output=problem, regex=regex)
    proc = host.run(["ctest", "--test-dir", str(pipeline_config.build_dir), "-R", regex, "--output-on-failure"], timeout=timeout_sec)
    return SmokeResult(passed=proc.returncode == 0, output=((proc.stdout or "") + (proc.stderr or ""))[-6000:], regex=regex)


def run_smoke_correctness_repeated(pipeline_config: PipelineConfig, workload: dict, query: int, *, runs: int = 2, timeout_sec: int = 180) -> SmokeResult:
    """Run the smoke ctest several times; all runs must pass."""
    outputs: list[str] = []
    for idx in range(1, max(runs, 1) + 1):
        result = run_smoke_correctness(pipeline_config, workload, query, timeout_sec=timeout_sec)
        if runs <= 1:
            return result
        outputs.append(f"--- smoke run {idx}/{runs} ---\n{result.output}")
        if not result.passed:
            break
    return SmokeResult(passed=result.passed, output="\n".join(outputs)[-6000:], regex=result.regex)


def run_build(pipeline_config: PipelineConfig, workload: dict, query: int) -> tuple[bool, str]:
    try:  # pack workers use build-release-{worker_id}; dirs may be missing after disk cleanup
        ensure_build_dir(pipeline_config, configure=True)
    except Exception as exc:  # noqa: BLE001 — surface as preflight failure
        return False, str(exc)
    paths = resolve_workload_paths(pipeline_config, workload, query)
    targets = [paths.get("aux_bench_target", ""), paths.get("aux_test_target", "")] if is_aux_workload(workload) \
        else [paths["bench_target"], paths["test_target"]]
    targets = [t for t in targets if t]
    build_dir = Path(pipeline_config.build_dir)
    stamp = ""
    if pipeline_config.quick_tier.build_stamp_cache:
        bench_name = str(workload.get("benchmark_name") or "")
        stamp = code_stamp(pipeline_config.repo_root, query_dir=query_dir(bench_name, query) if bench_name else "",
                           extra=["run_build", *targets, str(pipeline_config.build_dir)])
    stamp_file = build_dir / ".build_stamps" / f"run_build_q{query}"
    if stamp and (build_dir / "CMakeCache.txt").is_file() and _read_text(stamp_file) == stamp:
        return True, "build skipped: the code is unchanged since the last successful build"
    if problem := HostRun.worker(pipeline_config, workload).build(*targets):
        return False, problem
    if stamp:
        stamp_file.parent.mkdir(parents=True, exist_ok=True)
        stamp_file.write_text(stamp, encoding="utf-8")
    return True, "built"


def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError:
        return ""


class InfraFailFastError(RuntimeError):
    """Raised to abort fix loops without launching more agent turns."""

    def __init__(self, info: dict[str, Any], *, detail: str = ""):
        self.info = info
        super().__init__(f"infra_fail_fast:{info.get('infra_reason') or 'infra_failure'}: {detail}"[:2000])


def _fail_fast_infra_if_needed(pipeline_config, workload, query, *, text: str) -> dict[str, Any] | None:
    """On shared-tree infra failures, repair + release lease and stop burning agent turns."""
    info = handle_infra_failure(pipeline_config, workload, query, text=text or "")
    if info:
        _log(f"fail-fast infra ({info.get('infra_reason')}): lease_released={info.get('lease_released')} "
             f"restored={list((info.get('repair') or {}).get('restored') or {})}")
    return info or None


def _revert_and_rebuild(pipeline_config, workload, query) -> None:
    """Restore checkpointed sources and rebuild so a reverted patch cannot leave a stale binary."""
    revert_attempt_checkpoint(pipeline_config, workload, query)
    built, out = run_build(pipeline_config, workload, query)
    if not built and not _fail_fast_infra_if_needed(pipeline_config, workload, query, text=out or ""):
        _log(f"warning: rebuild after revert failed: {(out or '')[-400:]}")


def _fix_diagnosis(kind: str, output: str, *, iteration: int, attempt: int, fix_round: int, reproduce_cmd: str,
                   max_log_chars: int, regex: str = "") -> str:
    """What the fix agent is told when the patch does not build (`kind="build"`) or fails the smoke test."""
    log_tail = (output or "").strip()[-max_log_chars:] if (output or "").strip() else ""
    build = kind == "build"
    lines = [
        f"## {'Build failure' if build else 'Quick correctness failure'} (fix before benchmark)", "",
        f"- context: optimization iter {iteration}, attempt {attempt}, fix round {fix_round}",
        *([] if build else [f"- failed smoke: `ctest -R {regex}`"]),
        f"- **Your hypothesis patch is still active** — fix {'the compile/build error' if build else 'correctness'}; do not start a new redesign.",
        f"- reproduce: `{reproduce_cmd}`",
    ]
    if highlights := extract_crash_highlights(log_tail):
        lines += ["", "### Likely failure lines", *(f"- `{item}`" for item in highlights)]
    if log_tail:
        lines += ["", "### Build log tail" if build else "### Test output tail", "```text", log_tail[-4000:], "```"]
    lines += ["", "Diagnose root cause, patch the minimum files, then rebuild in this worker's build dir. "
                  "The orchestrator will re-run build automatically after you finish." if build else
              "Find why the smoke test failed (assertion, wrong result, segfault), patch, "
              "then rebuild and re-run the smoke test in this worker's build dir."]
    return "\n".join(lines)


def _fix_limit(pipeline_config: PipelineConfig, round_idx: int, *, kind: str) -> None:
    prefix = "build" if kind == "build" else "correctness"
    max_rounds = max(0, int(getattr(pipeline_config, f"optimization_{prefix}_fix_max_rounds", 0) or 0))
    safety_cap = max(1, int(getattr(pipeline_config, f"optimization_{prefix}_fix_safety_cap", 12) or 12))
    if max_rounds > 0 and round_idx >= max_rounds:
        raise RuntimeError(f"optimization {kind} fix exhausted max_rounds={max_rounds} (attempt still failing after {round_idx} fix rounds)")
    if max_rounds == 0 and round_idx >= safety_cap:
        raise RuntimeError(f"optimization {kind} fix safety_cap={safety_cap} reached (attempt still failing after {round_idx} fix rounds)")


# What the agent is handed for an attempt: the editable sources, and the context packet

BOOTSTRAP_MAX_CHARS_DEFAULT = 300_000
CONTEXT_MAX_CHARS = 80_000
CONTEXT_MAX_LINES = 1200
_LINE_CAPS = {"summary": 120, "hot_path": 600, "signatures": 80, "measurements": 100, "evidence": 100, "failed_hypotheses": 40}


def _truncate_lines(text: str, max_lines: int, *, label: str = "") -> str:
    lines = text.splitlines()
    if len(lines) <= max_lines:
        return text
    return "\n".join(lines[:max_lines]) + (f"\n... [truncated {len(lines) - max_lines} lines from {label}]" if label else "")


def _truncate_chars(text: str, max_chars: int) -> str:
    return text if max_chars <= 0 or len(text) <= max_chars else text[: max_chars - 48] + f"\n... [truncated at {max_chars} chars]"


def build_optimization_bootstrap(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> tuple[str, list[dict[str, Any]]]:
    """All editable sources, numbered and inlined, so the agent does not re-read files."""
    repo_root = pipeline_config.repo_root
    edit_paths = resolve_optimization_edit_paths(pipeline_config, workload, query)
    max_lines = int(getattr(pipeline_config, "optimization_bootstrap_max_lines_per_file", 0) or 0)
    max_chars = int(getattr(pipeline_config, "optimization_bootstrap_max_chars", BOOTSTRAP_MAX_CHARS_DEFAULT) or BOOTSTRAP_MAX_CHARS_DEFAULT)
    sources = []
    for rel in edit_paths.get("all_paths") or []:
        if not (repo_root / str(rel)).is_file():
            continue
        raw = (repo_root / str(rel)).read_text(encoding="utf-8", errors="replace")
        lines = raw.splitlines()[:max_lines] if max_lines > 0 else raw.splitlines()
        sources.append({"path": str(rel), "lines": len(lines), "bytes": len(raw.encode("utf-8")),
                        "content": "\n".join(f"{idx + 1}: {line}" for idx, line in enumerate(lines))})
    if sources:
        parts = ["## Editable sources (bootstrap — full contents, do not re-read)", "",
                 "All editable files are inlined below. Edit them in place using these line numbers. "
                 "**Do not re-read files** — the orchestrator refreshes this block each attempt.", ""]
        for entry in sources:
            parts += [f"### `{entry['path']}` ({entry['lines']} lines)", "```cpp", entry["content"], "```", ""]
        markdown = _truncate_chars("\n".join(parts), max_chars)
    else:
        markdown = "## Editable sources (bootstrap)\n\n(no editable files found on disk)\n"
    # a pointer to the shared pack rather than re-inlining the peer aux index every attempt
    return markdown + "\n\n" + format_seed_pointer(repo_root, max_preview_lines=24), sources


def _signatures(path: Path, max_lines: int = 50) -> str:
    if not path.is_file():
        return ""
    lines: list[str] = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        stripped = line.strip()
        if stripped.startswith(("//", "/*", "*")):
            if lines and lines[-1].startswith("//"):
                lines.append(line)
        elif (re.match(r"^(class|struct|enum|using|namespace|#)", stripped) or stripped in ("};", "}")
              or re.match(r"^(inline\s+)?(static\s+)?[\w:<>,\s*&]+\s+\w+\s*\([^;]*\)\s*(const)?\s*;", stripped)
              or (re.match(r"^(inline\s+)?(static\s+)?[\w:<>,\s*&]+\s+\w+\s*\(", stripped) and "{" not in stripped)):
            lines.append(line)
    return _truncate_lines("\n".join(lines[: max_lines * 2]), max_lines, label=path.name)


def _hot_path_excerpt(kernel_path: Path, review: dict[str, Any], max_lines: int) -> str:
    if not kernel_path.is_file():
        return ""
    text = kernel_path.read_text(encoding="utf-8", errors="replace")
    hot = {line + offset for smell in review.get("smells") or [] if isinstance(line := smell.get("line"), int) and line > 0
           for offset in range(-5, 15)}
    selected = [f"{idx:4d}| {line}" for idx, line in enumerate(text.splitlines(), start=1) if idx in hot][:max_lines]
    return "\n".join(selected) if selected else _truncate_lines(text, max_lines, label=kernel_path.name)


def build_context_packet(
    pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, *, evidence: dict[str, Any],
    baseline_metrics: dict[str, Any], baseline_score: float,
) -> dict[str, Any]:
    repo = pipeline_config.repo_root
    art = _art(pipeline_config, workload, query)
    _, analysis = load_query_analysis(repo, query, workload=workload)
    paths = resolve_workload_paths(pipeline_config, workload, query)
    edit = resolve_optimization_edit_paths(pipeline_config, workload, query)
    kernel_path = repo / edit["primary_cpp"]
    review = memory.review_kernel(kernel_path)
    is_aux = workload.get("workload_kind") == "aux_state"
    summary = json.dumps(analysis_for_optimization(analysis or {}), indent=2) if is_aux else compact_base_analysis_for_optimization(analysis or {})
    fused = paths.get("fused_scan_path")
    sources = [(edit["header"], repo / edit["header"]), (edit["primary_cpp"], kernel_path), (edit["secondary_cpp"], repo / edit["secondary_cpp"])]
    if fused and (repo / fused).is_file():
        sources.append((fused, repo / fused))
    signatures = "\n\n".join(f"// {rel}\n{_signatures(path, _LINE_CAPS['signatures'])}" for rel, path in sources)
    failed = [a for a in memory.recall(art, "attempt", limit=5) if not a.get("accepted") and not a.get("kept")][-_LINE_CAPS["failed_hypotheses"]:]
    diag = bench.extract_store_diagnostics(baseline_metrics)
    cards = memory.precedents(repo, memory.evidence_signals(art, baseline_metrics, analysis)) if is_aux else []
    compact = lambda name: bench.compact_summary_for_prompt(read_json(art / name))  # noqa: E731
    return {
        "query": query, "baseline_score": baseline_score, "baseline_quick_metrics": baseline_metrics, "store_diagnostics": diag,
        "retrieved_precedents": cards,
        "summary": _truncate_lines(summary, _LINE_CAPS["summary"], label="summary"),
        "hot_path_code": _hot_path_excerpt(kernel_path, review, _LINE_CAPS["hot_path"]),
        "api_signatures": _truncate_lines(signatures, _LINE_CAPS["signatures"] * 2, label="signatures"),
        "measurements": _truncate_lines(json.dumps({"gate_baseline": compact("baseline_summary.json"), "gate_profile": compact("profile_summary.json"),
                                                    "quick_baseline": baseline_metrics}, indent=2), _LINE_CAPS["measurements"], label="measurements"),
        "evidence": _truncate_lines(json.dumps({**evidence, "store_diagnostic_tiers": bench.organize_diagnostics_evidence(diag)}, indent=2),
                                    _LINE_CAPS["evidence"], label="evidence"),
        "failed_hypotheses": [{"hypothesis": h.get("hypothesis", ""), "reason": h.get("reason", "")} for h in failed],
        "kernel_paths": {"header": edit["header"], "kernel": edit["primary_cpp"], "package": edit["secondary_cpp"],
                         "fused_scan": fused if not edit.get("is_aux") else None,
                         "cost_model": paths.get("cost_model_config") if edit.get("is_aux") else None},
    }


def save_context_packet(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, packet: dict[str, Any]) -> Path:
    """Write the packet as markdown (the agent reads it) and json, within the configured budget."""
    art = _art(pipeline_config, workload, query)
    diag = packet.get("store_diagnostics") or {}
    lines = [
        "# Optimization context packet", "",
        f"- baseline_score (`read_only_query_speedup`): **{float(packet.get('baseline_score') or 0):.4f}**", "",
        "## Store diagnostics (baseline)", "", *(["```json", json.dumps({"measurements": diag}, indent=2), "```"] if diag else ["(no measurements)"]), "",
        memory.precedents_markdown(packet.get("retrieved_precedents") or []), "",
        "## Query / plan summary", packet.get("summary", ""), "",
        "## Hot-path code (excerpt)", "```cpp", packet.get("hot_path_code", ""), "```", "",
        "## API signatures", "```cpp", packet.get("api_signatures", ""), "```", "",
        "## Measurements", "```json", packet.get("measurements", "{}"), "```", "",
        "## Bottleneck evidence (tier-organized)", "```json", packet.get("evidence", "{}"), "```",
    ]
    if failed := packet.get("failed_hypotheses"):
        lines += ["", "## Recent failed hypotheses", *(f"- {row.get('hypothesis', '')} ({row.get('reason', '')})" for row in failed)]
    lines += [f"- {key}: `{value}`" for key, value in (packet.get("kernel_paths") or {}).items() if value]
    max_lines = int(getattr(pipeline_config, "optimization_context_max_lines", CONTEXT_MAX_LINES))
    max_chars = int(getattr(pipeline_config, "optimization_context_max_chars", CONTEXT_MAX_CHARS))
    (art / "optimization_context.md").write_text(_truncate_chars(_truncate_lines("\n".join(lines), max_lines), max_chars), encoding="utf-8")
    (art / "optimization_context.json").write_text(json.dumps(packet, indent=2), encoding="utf-8")
    return art / "optimization_context.md"


def attempt_message(
    *, optimization_iter: int, attempt: int, feedback: str = "", experimental_summary: str = "", context_markdown: str = "",
    bootstrap_markdown: str = "", workload_guidance: str = "", phase: str = "attempt",
) -> str:
    """The user message of an attempt turn. The persistent worker has its context in its session and gets the
    previous candidate's verdict; a hypothesis/patch turn gets the sources and the context packet inline."""
    if phase == "worker":
        parts = [f"Optimization iter {optimization_iter}, attempt {attempt}.",
                 "Inspect, patch, build, test, and quick-benchmark before signaling CANDIDATE_READY."]
        if feedback.strip():
            parts += ["", "## Prior gate feedback", feedback.strip()]
        if experimental_summary.strip():
            parts += ["", "## Experimental branch", experimental_summary.strip()]
        text = "\n".join(parts)
        return text if len(text) <= 4000 else text[:3960] + "\n... [truncated attempt prompt]"
    note = {
        "hypothesis": "Phase 1 (hypothesis only): output structured hypothesis JSON citing measured diagnostics. No file edits in this phase.\n",
        "patch": "Hypothesis is locked — patch phase only. Edit the editable sources; no repository exploration.\n",
        "attempt": f"Hypothesize, implement, then end with `{PATCH_COMPLETE}` in one short session.\n",
    }[phase]
    guidance = f"\n\n## Workload guidance\n{workload_guidance}\n" if workload_guidance else ""
    experimental = f"\n\n## Experimental branch diagnostic summary\n{experimental_summary}\n" if experimental_summary else ""
    return (f"Optimization iter {optimization_iter}, attempt {attempt}.\n{note}"
            "Sources are bootstrapped below — do not try to read files during patch phase.\n"
            f"{guidance}{experimental}\n{context_markdown}\n\n{bootstrap_markdown}")


def format_gate_delta(*, verdict: str, reason: str = "", hypothesis: dict[str, Any] | None = None,
                      improvement_pct: float | None = None, reverted: bool = False) -> str:
    """The persistent worker's note on the previous candidate's fate."""
    lines = [f"Previous candidate **{verdict}**."]
    if improvement_pct is not None:
        lines.append(f"- improvement_pct: {improvement_pct:+.2f}%")
    if bottleneck := (hypothesis or {}).get("bottleneck", ""):
        lines.append(f"- hypothesis: {bottleneck}")
    if reason:
        lines.append(f"- reason: {reason}")
    if reverted:
        lines.append("- Code reverted to best-valid baseline. Continue with a new approach.")
    text = "\n".join(lines)
    return text if len(text) <= 4000 else text[:3960] + "\n... [truncated gate delta]"


# Agent turns

@dataclass
class Candidate:
    """What an agent turn came back with."""

    status: str  # ready | cannot_test | failed | saturated
    hypothesis: dict[str, Any] | None = None
    reason: str = ""
    final_text: str = ""
    saturation: dict[str, Any] | None = None


def parse_hypothesis_json(text: str) -> dict[str, Any] | None:
    if not text:
        return None
    if fence := re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL):
        try:
            return json.loads(fence.group(1))
        except json.JSONDecodeError:
            pass
    for match in re.finditer(r"\{[^{}]*(?:\{[^{}]*\}[^{}]*)*\}", text, re.DOTALL):
        try:
            payload = json.loads(match.group(0))
        except json.JSONDecodeError:
            continue
        if isinstance(payload, dict) and payload.get("bottleneck"):
            return payload
    return None


def parse_cannot_test(text: str) -> str | None:
    if CANNOT_TEST not in text:
        return None
    if fence := re.search(r"```(?:json)?\s*(\{.*?\})\s*```", text, re.DOTALL):
        try:
            payload = json.loads(fence.group(1))
            if payload.get("status") == "cannot_test":
                return str(payload.get("reason", "cannot test hypothesis"))
        except json.JSONDecodeError:
            pass
    return "agent returned CANNOT_TEST"


def _agent_turn(
    stage: str, prompt_file: str, pipeline_config: PipelineConfig, model_config: ModelConfig, workload_name: str, query: int,
    user_message: str, model_overrides: dict[str, str] | None, optimization_iter: int, *, session_role: str | None = None,
) -> Any:
    workload = load_run_workload(pipeline_config, workload_name)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    prompt = render_prompt(pipeline_config, prompt_file, workload, query, stage, optimization_iter=optimization_iter)
    save_rendered_prompt(pipeline_config, workload, query, stage, prompt, optimization_iter=optimization_iter)
    model_id = make_model_id(stage, pipeline_config, model_config, model_overrides)
    backend = create_agent_backend(provider=resolve_stage_provider(stage, model_config), pipeline_config=pipeline_config, query=query,
                                   stage=stage, model_config=model_config, workload_name=workload_name, optimization_iter=optimization_iter)
    request = StageRunRequest(
        stage=stage, instructions=with_codex_tooling(pipeline_config, prompt), user_message=user_message, agent_name=f"accel_{stage}",
        model_id=model_id, agent_model=model_id,
        max_turns=resolve_optimization_max_turns(pipeline_config, "unified" if "unified" in stage else "patch"))
    if session_role:
        return run_in_session(backend, request, pipeline_config=pipeline_config, art=_art(pipeline_config, workload, query),
                              query=query, role=session_role)
    return backend.run_stage(request)


def _saturation_candidate(final_text: str, hypothesis: dict[str, Any] | None = None) -> Candidate | None:
    if (payload := memory.parse_optimization_saturated(final_text)) is None:
        return None
    return Candidate("saturated", hypothesis or parse_hypothesis_json(final_text),
                     str(payload.get("reason") or "optimization_saturated"), final_text, payload)


class OptimizationWorker:
    """The persistent agent session of the stage: its instructions are sent once, each attempt continues it."""

    def __init__(self, pipeline_config, model_config, workload_name, query, workload, art_dir, backend, instructions, provider, model_overrides):
        self.pipeline_config, self.model_config, self.workload_name, self.query = pipeline_config, model_config, workload_name, query
        self.workload, self.art_dir, self.backend, self.instructions = workload, art_dir, backend, instructions
        self.provider, self.model_overrides = provider, model_overrides
        self.optimization_iter = self.attempt = self.cycles = self.tool_calls = 0

    @classmethod
    def create(cls, *, pipeline_config: PipelineConfig, model_config: ModelConfig, workload_name: str, query: int,
               model_overrides: dict[str, str] | None = None) -> OptimizationWorker:
        workload = load_run_workload(pipeline_config, workload_name)
        pipeline_config = apply_workload_overrides(pipeline_config, workload)
        provider = resolve_stage_provider(_WORKER_STAGE, model_config)
        prompt = render_prompt(pipeline_config, "03_optimization_worker.md", workload, query, _WORKER_STAGE, optimization_iter=1)
        save_rendered_prompt(pipeline_config, workload, query, _WORKER_STAGE, prompt, optimization_iter=1)
        backend = create_agent_backend(provider=provider, pipeline_config=pipeline_config, query=query, stage=_WORKER_STAGE,
                                       model_config=model_config, workload_name=workload_name, optimization_iter=1)
        worker = cls(pipeline_config, model_config, workload_name, query, workload, _art(pipeline_config, workload, query), backend,
                     with_codex_tooling(pipeline_config, prompt), provider, model_overrides)
        log_event(pipeline_config.repo_root, query, workload, "optimization_worker_episode_start", provider=provider)
        return worker

    def _max_turns(self) -> int:
        raw = int(getattr(self.pipeline_config, "optimization_worker_max_turns_per_candidate", 0) or 0)
        if raw > 0:
            return raw
        cap = int(getattr(self.pipeline_config, "optimization_worker_safety_cap_turns", 80) or 80)
        return cap if cap > 0 else _UNLIMITED_TURNS

    def _run(self, user_message: str) -> Any:
        model_id = make_model_id(_WORKER_STAGE, self.pipeline_config, self.model_config, self.model_overrides)
        request = StageRunRequest(stage=_WORKER_STAGE, instructions=self.instructions, user_message=user_message,
                                  agent_name=f"accel_{_WORKER_STAGE}", model_id=model_id, agent_model=model_id, max_turns=self._max_turns())
        return run_in_session(self.backend, request, pipeline_config=self.pipeline_config, art=self.art_dir, query=self.query, role=MAIN)

    @staticmethod
    def _yielded(text: str) -> bool:
        return CANDIDATE_READY in text or memory.OPTIMIZATION_SATURATED in text or bool(parse_cannot_test(text))

    def run_candidate_cycle(self, user_message: str) -> Candidate:
        events = (self.pipeline_config.repo_root, self.query, self.workload)
        log_event(*events, "optimization_worker_candidate_start", iter=self.optimization_iter, attempt=self.attempt)
        started = time.monotonic()
        result = self._run(user_message)
        if result.status == "finished" and not self._yielded(result.final_text or ""):
            # Codex often exits after edits/tests without the marker: one short follow-up lets the cycle complete.
            log_event(*events, "optimization_worker_yield_nudge", attempt=self.attempt)
            nudge = self._run(
                "Your previous optimization attempt ended without a required yield marker.\n"
                "Do not start a new redesign. Based on the current tree state:\n"
                "1. Briefly confirm build/smoke already done (or run MCP `run_correctness` / shell ctest if unsure).\n"
                "2. Call MCP `get_profile_digest` then, if you edited, `begin_ab_trial`, `run_quick_benchmark(acceptance=True)` and\n"
                "   `end_ab_trial` (`keep=true` needs that acceptance run).\n"
                "   Do not yield after `ab_signal=regress` / `forced_discard`.\n"
                "3. Print hypothesis JSON then end with exactly `CANDIDATE_READY`,\n"
                "   or `CANNOT_TEST` with JSON reason,\n"
                "   or `OPTIMIZATION_SATURATED` with JSON if you already pushed hard and "
                "further attempts are stuck (no target speedup number).\n"
                "Exiting without that marker fails this attempt.")
            if nudge.status == "finished" and self._yielded(nudge.final_text or ""):
                result = nudge
            elif nudge.final_text:  # a marker in either segment counts
                result.final_text = f"{result.final_text or ''}\n{nudge.final_text}"
        text = result.final_text or ""
        if result.status != "finished":
            cycle, marker = Candidate("failed", reason=result.error or result.status, final_text=text), "failed"
        elif cycle := _saturation_candidate(text):  # saturation wins over CANDIDATE_READY when both appear
            marker = "saturated"
        elif cannot := parse_cannot_test(text):
            cycle, marker = Candidate("cannot_test", reason=cannot, final_text=text), "cannot_test"
        elif CANDIDATE_READY not in text:
            cycle, marker = Candidate("failed", parse_hypothesis_json(text), "missing CANDIDATE_READY", text), "failed"
        else:
            hypothesis = parse_hypothesis_json(text) or {"bottleneck": "optimization_worker_patch", "why": "no hypothesis JSON"}
            cycle, marker = Candidate("ready", hypothesis, final_text=text), "ready"
        usage = (result.metadata or {}).get("usage") or {}
        tool_calls = int((result.metadata or {}).get("tool_calls", 0) or 0)
        self.cycles, self.tool_calls = self.cycles + 1, self.tool_calls + tool_calls
        snippet = " ".join((cycle.reason or result.error or "")[:500].split())[:240] if marker == "failed" else ""
        if snippet:
            _log(f"optimization_worker_candidate_failed: {snippet}")
        log_event(*events, "optimization_worker_candidate_end", runner_invocation_idx=self.cycles, tool_calls=tool_calls,
                  wall_clock_ms=int((time.monotonic() - started) * 1000), input_tokens=usage.get("input_tokens"),
                  output_tokens=usage.get("output_tokens"), cache_read_tokens=usage.get("cache_read_tokens"), marker=marker,
                  failure_snip=snippet or None)
        return cycle

    def close(self) -> None:
        log_event(self.pipeline_config.repo_root, self.query, self.workload, "optimization_worker_episode_end",
                  runner_invocations=self.cycles, tool_calls=self.tool_calls)


def _hypothesis_turn(it: _Iteration, attempt: int, message: str) -> Candidate:
    result = it.turn("optimization_unified_hypothesis", "03_optimization_unified.md", message)
    text = result.final_text or ""
    if result.status != "finished":
        return Candidate("failed", reason=result.error or result.status, final_text=text)
    if saturated := _saturation_candidate(text):
        return saturated
    hypothesis = parse_hypothesis_json(text)
    if HYPOTHESIS_COMPLETE not in text:
        return Candidate("failed", hypothesis, "missing HYPOTHESIS_COMPLETE in hypothesis phase", text)
    if not hypothesis:
        return Candidate("failed", reason="missing hypothesis JSON", final_text=text)
    fields = {key: str(hypothesis.get(key) or "") for key in ("hot_spot", "expected_gain", "strategy")}
    if error := memory.profile_guidance_error(it.art, **fields):
        return Candidate("failed", hypothesis, f"the hypothesis must start from the profile: {error}", text)
    memory.record_trial(it.art, trial_id=f"iter{it.iteration}_a{attempt}", **fields)
    return Candidate("ready", hypothesis, final_text=text)


def _patch_turn(it: _Iteration, message: str, hypothesis: dict[str, Any]) -> Candidate:
    result = it.turn("optimization_unified_patch", "03_optimization_unified.md",
                     f"{message}\n\n## Locked hypothesis\n```json\n{json.dumps(hypothesis, indent=2)}\n```\n"
                     f"Implement this hypothesis with a single coherent patch, then end with `{PATCH_COMPLETE}`.\n")
    text = result.final_text or ""
    if result.status != "finished":
        return Candidate("failed", reason=result.error or result.status, final_text=text)
    if saturated := _saturation_candidate(text, hypothesis):
        return saturated
    if cannot := parse_cannot_test(text):
        return Candidate("cannot_test", reason=cannot, final_text=text)
    if PATCH_COMPLETE not in text:
        return Candidate("failed", hypothesis, f"missing {PATCH_COMPLETE}", text)
    return Candidate("ready", hypothesis, final_text=text)


def _single_turn(it: _Iteration, message: str) -> Candidate:
    result = it.turn("optimization_unified", "03_optimization_unified.md", message)
    text = result.final_text or ""
    if result.status == "error" and "max_turns" in str(result.error or "").lower():
        return Candidate("failed", reason=f"agent ended before {PATCH_COMPLETE} — finish implementation then end with that marker", final_text=text)
    if result.status != "finished":
        return Candidate("failed", reason=result.error or result.status, final_text=text)
    if saturated := _saturation_candidate(text):
        return saturated
    if cannot := parse_cannot_test(text):
        return Candidate("cannot_test", reason=cannot, final_text=text)
    hypothesis = parse_hypothesis_json(text)
    if PATCH_COMPLETE not in text:
        return Candidate("failed", hypothesis, f"missing {PATCH_COMPLETE}", text)
    return Candidate("ready", hypothesis or {"bottleneck": "unified_optimization_patch", "why": "no hypothesis JSON in agent output"}, final_text=text)


# Structural redesigns: when the profile shows the state itself is the bottleneck, an attempt may redesign it in
# place. It is kept only if it passes the full tier of the correctness, unit, race and multi-connection gates
# and measurably improves reads without regressing writes; the redesign is then written to the design record.

DEFAULT_STRUCTURAL_GATES = ("aux_correctness", "aux_unit", "aux_tsan", "aux_multi_connection")
DEFAULT_INFERRED_DIFF_FRACTION = 0.35


def structural_config(pipeline_config: Any) -> dict[str, Any]:
    raw = getattr(pipeline_config, "optimization_structural", None)
    cfg = dict(raw) if isinstance(raw, dict) else {}
    return {"gates": list(cfg.get("gates") or DEFAULT_STRUCTURAL_GATES),
            "inferred_diff_fraction": float(cfg.get("inferred_diff_fraction", DEFAULT_INFERRED_DIFF_FRACTION))}


def changed_line_fraction(art: Path, repo_root: Path) -> float:
    """Share of the attempt-start lines of the checkpointed sources that the attempt removed or replaced."""
    ckpt = art / ATTEMPT_CHECKPOINT
    before_total = removed = 0
    for saved in ckpt.rglob("*") if ckpt.is_dir() else ():
        if not saved.is_file():
            continue
        current = repo_root / saved.relative_to(ckpt)
        before = Counter(saved.read_text(encoding="utf-8", errors="replace").splitlines())
        after = Counter(current.read_text(encoding="utf-8", errors="replace").splitlines()) if current.is_file() else Counter()
        before_total += sum(before.values())
        removed += sum((before - after).values())
    return removed / before_total if before_total else 0.0


def is_structural_attempt(pipeline_config: Any, art: Path, hypothesis: dict[str, Any] | None) -> bool:
    """Declared by the attempt (strategy=structural), or inferred from how much of the sources it rewrote."""
    hypothesis = hypothesis or {}
    declared = hypothesis.get("strategy") == memory.STRATEGY_STRUCTURAL or hypothesis.get("tier") == "structural_redesign"
    return declared or changed_line_fraction(art, pipeline_config.repo_root) >= structural_config(pipeline_config)["inferred_diff_fraction"]


def run_full_tier_gates(pipeline_config: Any, workload: dict[str, Any], query: int, *, iteration: int) -> str:
    """Run the structural gates in order; "" when all pass, else which one failed and why."""
    for name in structural_config(pipeline_config)["gates"]:
        result = run_gate(pipeline_config, name, query, workload, change_kind="optimization", iteration=iteration)
        if result.status != "pass":
            detail = str(result.data.get("detail") or result.data.get("message") or "")[-1500:]
            return f"structural attempt failed gate {name} ({result.status}): {detail}"
    return ""


# One iteration

@dataclass
class Gated:
    """Where a candidate patch fell among the gates ("" label: it passed them all and was measured)."""

    label: str = ""
    detail: str = ""
    infra: bool = False
    structural: bool = False
    candidate: MultiSampleBenchmark | None = None
    outcome: AttemptOutcome | None = None


def _short(text: str | None, limit: int = 240) -> str:
    cleaned = " ".join((text or "unknown").split())
    return cleaned if len(cleaned) <= limit else cleaned[: limit - 3] + "..."


class _Iteration:
    """One optimization iteration: a measured baseline, then attempts until one stops it."""

    def __init__(self, pipeline_config, model_config, workload_name, query, optimization_iter, model_overrides, worker, deadline):
        self.workload_name, self.query, self.iteration = workload_name, query, optimization_iter
        self.model_config, self.model_overrides, self.worker, self.deadline = model_config, model_overrides, worker, deadline
        self.workload = load_run_workload(pipeline_config, workload_name)
        self.cfg = apply_workload_overrides(pipeline_config, self.workload)
        self.art = _art(self.cfg, self.workload, query)
        self.eval_config = eval_config_from_pipeline(self.cfg)
        self.max_attempts = int(getattr(self.cfg, "optimization_max_attempts_per_iteration", 4) or 4)
        stop = int(getattr(self.cfg, "optimization_consecutive_fail_stop", 0) or 0)
        self.fail_stop = stop if stop > 0 else self.max_attempts + 1
        self.fails = self.depth = 0
        self.feedback = ""  # the previous candidate's fate, for the persistent worker's next attempt
        self.baseline: MultiSampleBenchmark | None = None
        self.evidence: dict[str, Any] = {}
        self.last_result: Candidate | None = None

    def turn(self, stage: str, prompt_file: str, message: str, *, session_role: str | None = None) -> Any:
        return _agent_turn(stage, prompt_file, self.cfg, self.model_config, self.workload_name, self.query, message,
                           self.model_overrides, self.iteration, session_role=session_role)

    # baseline

    def measure_baseline(self) -> None:
        errors: list[str] = []
        baseline = run_multi_sample_benchmark(self.cfg, self.workload, self.query, samples=self.eval_config.quick_benchmark_samples,
                                              include_profile=True, errors=errors)
        if baseline is None:
            raise StageFailure(Failure(
                kind="measurement", stage="optimization", source="bench_baseline",
                summary="the baseline benchmark did not produce a measurement", evidence="\n".join(errors),
                diagnosis=["the benchmark failed to build or run; nothing measured the accelerator"]))
        # a baseline that does not measure the accelerator would make every attempt's verdict meaningless
        paths = resolve_workload_paths(self.cfg, self.workload, self.query)
        target = paths["aux_bench_target" if is_aux_workload(self.workload) else "bench_target"]
        failures = check_measurement(
            baseline, workload=self.workload, phase="baseline", config=self.cfg.measurement,
            bench_binary=self.cfg.build_dir / target,
            query_dir=self.cfg.repo_root / query_dir(str(self.workload.get("benchmark_name", "tpch")), int(self.workload.get("query_number", self.query))))
        if failures:
            raise StageFailure(failures[0])
        self.baseline = baseline

    def build_evidence(self) -> None:
        """The bottleneck evidence of the baseline, which the context packet carries (a persistent worker pulls it by tool)."""
        repo = self.cfg.repo_root
        _, analysis = load_query_analysis(repo, self.query, workload=self.workload)
        profile = read_json(self.art / "profile_summary.json")
        review = memory.review_kernel(repo / resolve_optimization_edit_paths(self.cfg, self.workload, self.query)["primary_cpp"])
        diagnosis = memory.diagnose(read_json(self.art / "baseline_summary.json"), profile,
                                    best_speedup=float(load_base_optimization_state(self.art).get("best_speedup_median") or 0.0),
                                    analysis=analysis or None, kernel_review=review)
        if self.baseline.median_metrics.get("speedup_median") is not None:
            diagnosis["speedup_median"] = self.baseline.median_metrics["speedup_median"]
        self.evidence = memory.bottleneck_evidence(diagnosis, profile, review, analysis=analysis or None)

    # recording

    def note(self, attempt: int, hypothesis: Any, verdict: str, **fields: Any) -> None:
        memory.record_attempt(self.art, repo_root=self.cfg.repo_root, workload=self.workload, query=self.query, attempt=attempt,
                              hypothesis=hypothesis, verdict=verdict, **fields)

    def reject(self, attempt: int, hypothesis: Any, verdict: str, reason: str, *, tell: str | bool = True, detail: str = "",
               improvement_pct: float | None = None, metrics: dict[str, Any] | None = None) -> bool:
        """Revert the patch and count the failure; whether the iteration stops on it. `tell` is whether (and
        under which verdict) the persistent worker hears of it."""
        _revert_and_rebuild(self.cfg, self.workload, self.query)
        self.fails += 1
        if tell:
            self.feedback = format_gate_delta(verdict=tell if isinstance(tell, str) else verdict, reason=detail or reason, hypothesis=hypothesis if isinstance(hypothesis, dict) else None,
                                              improvement_pct=improvement_pct, reverted=True)
        self.note(attempt, hypothesis, verdict, reason=reason, improvement_pct=improvement_pct, metrics=metrics)
        return self.fails >= self.fail_stop

    # saturation

    def judge_saturation(self, attempt: int, payload: dict[str, Any] | None) -> tuple[bool, str]:
        """(honored, detail). A claim below the read floor, after the required work, asks analysis for one redesign."""
        verdict, detail, cfg = memory.judge_saturation_claim(self.cfg, self.art, payload)
        honored = verdict != memory.REJECT
        if verdict == memory.REANALYZE:
            self._request_reanalysis(detail)
        log_event(self.cfg.repo_root, self.query, self.workload, "optimization_saturation_claim", honored=honored, detail=detail[:500],
                  floors=cfg, payload=payload or {}, optimization_iter=self.iteration, attempt=attempt, workload_name=self.workload.get("name"))
        if honored:
            append_experiment_log(self.cfg.repo_root, self.query, f"- iter {self.iteration} a{attempt}: OPTIMIZATION_SATURATED — {detail}",
                                  workload=self.workload)
            self.note(attempt, None, "optimization_saturated", reason=detail, kept=True, saturation=payload or {})
            clear_attempt_checkpoint(self.art)
            return True, detail
        self.feedback = f"OPTIMIZATION_SATURATED not honored: {detail}. Continue optimizing."
        self.note(attempt, None, "saturation_floors_not_met", reason=self.feedback)
        return False, detail

    def _request_reanalysis(self, detail: str) -> None:
        (self.art / memory.REANALYSIS_MARKER_FILE).write_text(detail, encoding="utf-8")
        try:
            analysis = json.loads(analysis_artifact_path(self.cfg.repo_root, self.query, workload=self.workload).read_text(encoding="utf-8"))
            candidate = str(analysis["correctness_facts"]["state_design"]["selected_candidate_id"])
        except (OSError, KeyError, TypeError, json.JSONDecodeError):
            candidate = "current"
        write_structural_request(
            self.cfg.repo_root, self.query, self.workload, source_stage="optimization", source_iteration=self.iteration,
            bottleneck_type="residual_read_path",
            evidence=f"{detail}. The profile-guided structural and low-level attempts recorded in the trial records did not close the gap.",
            proposed_direction="Choose a design whose typical read visits at least 100x less state than the base path: "
            "finer partitioning by the predicate's columns, coarser pre-aggregation with output-sensitive refinement, "
            "or a data-derived bound that tightens with the parameter.",
            current_candidate_id=candidate)

    # the patch

    def candidate(self, attempt: int, experimental_summary: str) -> Candidate:
        """Ask the agent for the attempt's hypothesis and patch."""
        if self.worker is not None:
            return self._worker_candidate(attempt, experimental_summary)
        bootstrap, sources = build_optimization_bootstrap(self.cfg, self.workload, self.query)
        (self.art / "optimization_bootstrap.md").write_text(bootstrap, encoding="utf-8")
        (self.art / "optimization_bootstrap_meta.json").write_text(
            json.dumps([{k: s[k] for k in ("path", "lines", "bytes")} for s in sources], indent=2), encoding="utf-8")
        _log(f"optimization bootstrap: {len(sources)} files, {len(bootstrap)} chars (sources inlined in the agent message)")
        packet = build_context_packet(self.cfg, self.workload, self.query, evidence=self.evidence,
                                      baseline_metrics=self.baseline.median_metrics, baseline_score=self.baseline.median_score)
        context = save_context_packet(self.cfg, self.workload, self.query, packet).read_text(encoding="utf-8")
        message = lambda phase: attempt_message(  # noqa: E731
            optimization_iter=self.iteration, attempt=attempt, context_markdown=context, bootstrap_markdown=bootstrap,
            workload_guidance=str(self.workload.get("optimization_guidance", "") or "").strip(),
            experimental_summary=experimental_summary, phase=phase)
        if (self.workload.get("optimization") or {}).get("two_phase_hypothesis_patch") is False:
            return _single_turn(self, message("attempt"))
        hypo = _hypothesis_turn(self, attempt, message("hypothesis"))
        return hypo if hypo.status != "ready" else _patch_turn(self, message("patch"), hypo.hypothesis or {})

    def _worker_candidate(self, attempt: int, experimental_summary: str) -> Candidate:
        worker = self.worker
        worker.optimization_iter, worker.attempt = self.iteration, attempt
        message = attempt_message(optimization_iter=self.iteration, attempt=attempt, feedback=self.feedback,
                                  experimental_summary=experimental_summary, phase="worker")
        self.feedback = ""
        retries, max_retries = 0, self.cfg.repair.provider_retries
        while True:  # soft-retry provider flakes on the same attempt slot, so a disconnect does not burn the attempt budget
            cycle = worker.run_candidate_cycle(message)
            if cycle.status in ("ready", "saturated", "cannot_test") or not is_transient_provider_failure(cycle.reason) or retries >= max_retries:
                return cycle
            retries += 1
            _log(f"transient provider failure on attempt {attempt} (retry {retries}/{max_retries}): {_short(cycle.reason)}")
            time.sleep(self.cfg.repair.provider_backoff(retries))

    # the gates

    def _fix_loop(self, kind: str, attempt: int, check: Callable[[], tuple[bool, str, str]], reproduce: str) -> tuple[bool, str]:
        """Run `check` (built?, output, ctest regex); until it passes, hand the failure to the fix agent."""
        log_chars = int(getattr(self.cfg, "optimization_fix_log_chars", 6000) or 6000)
        fix_round = 0
        while True:
            ok, output, regex = check()
            if ok:
                if fix_round:
                    _log(f"{kind} recovered after {fix_round} fix round(s)")
                return True, output
            if infra := _fail_fast_infra_if_needed(self.cfg, self.workload, self.query, text=output or ""):
                raise InfraFailFastError(infra, detail=(output or "")[-1500:])
            fix_round += 1
            _fix_limit(self.cfg, fix_round, kind=kind)
            diagnosis = _fix_diagnosis(kind, output, iteration=self.iteration, attempt=attempt, fix_round=fix_round,
                                       reproduce_cmd=reproduce if kind == "build" else f"ctest --test-dir {self.cfg.build_dir.name} -R {regex} --output-on-failure",
                                       max_log_chars=log_chars, regex=regex)
            artifact = self.art / f"optimization_fix_{kind}_iter{self.iteration}_a{attempt}_r{fix_round}.md"
            artifact.write_text(diagnosis, encoding="utf-8")
            append_experiment_log(self.cfg.repo_root, self.query, f"- optimization {kind} fix round {fix_round} "
                                  f"(iter {self.iteration} attempt {attempt}): see {artifact.name}", workload=self.workload)
            _log(f"optimization {kind} fix round {fix_round} (iter {self.iteration} attempt {attempt})")
            self.turn("optimization_fix", "03_optimization_fix.md", diagnosis, session_role=MAIN)

    def gate(self, attempt: int, hypothesis: dict[str, Any] | None) -> Gated:
        """Build, smoke, probe and (for a redesign) the full-tier gates, then the benchmark and the verdict."""
        cfg, workload, query = self.cfg, self.workload, self.query
        runs = max(1, int(getattr(cfg, "optimization_smoke_runs", 2) or 2))
        fixing = self.worker is None
        paths = resolve_workload_paths(cfg, workload, query)
        targets = [paths.get("aux_bench_target", ""), paths.get("test_target", "")] if is_aux_workload(workload) else [paths["bench_target"], paths["test_target"]]
        reproduce = f"cmake --build {cfg.build_dir.name} --target {' '.join(t for t in targets if t)} -j4"
        _log(f"patch complete; running build → smoke (x{runs}) → correctness probe → quick bench (attempt gates only; full suite runs end of iteration)")

        def fell(label: str, detail: str = "", *, infra_text: str | None = None) -> Gated:
            infra = _fail_fast_infra_if_needed(cfg, workload, query, text=infra_text) if infra_text is not None else None
            return Gated(f"infra_fail_fast:{infra.get('infra_reason')}" if infra else label, detail, infra=bool(infra))

        try:
            if fixing:
                built, build_out = self._fix_loop("build", attempt, lambda: (*run_build(cfg, workload, query), ""), reproduce)
            else:
                built, build_out = run_build(cfg, workload, query)
        except InfraFailFastError as exc:
            return Gated("infra_fail_fast", str(exc), infra=True)
        except RuntimeError as exc:
            return Gated("build_fix_exhausted", str(exc))
        if not built:
            return fell("auth_build_failed", (build_out or "")[-2000:], infra_text=build_out)

        def smoke_check() -> tuple[bool, str, str]:
            result = run_smoke_correctness_repeated(cfg, workload, query, runs=runs)
            return result.passed, result.output, result.regex

        try:
            if fixing:
                passed, smoke_out = self._fix_loop("correctness", attempt, smoke_check, "")
            else:
                passed, smoke_out, _ = smoke_check()
        except InfraFailFastError as exc:
            return Gated("infra_fail_fast", str(exc), infra=True)
        except RuntimeError as exc:
            return Gated("correctness_fix_exhausted", str(exc))
        if not passed:
            return fell("auth_smoke_failed", (smoke_out or "")[-2000:], infra_text=smoke_out)

        probe = run_smoke_correctness(cfg, workload, query, timeout_sec=120)
        if not probe.passed:
            return fell("auth_probe_failed", probe.diff_summary or (probe.output or "")[-2000:],
                        infra_text=f"{probe.diff_summary or ''}\n{probe.output or ''}")

        structural = workload.get("workload_kind") == "aux_state" and is_structural_attempt(cfg, self.art, hypothesis)
        if structural:
            _log("structural attempt: full-tier gates before the benchmark")
            if error := memory.design_revision_error(hypothesis) or run_full_tier_gates(cfg, workload, query, iteration=self.iteration):
                return Gated("structural_gates_failed", error)

        candidate = run_multi_sample_benchmark(cfg, workload, query, samples=self.eval_config.quick_benchmark_samples, include_profile=False)
        if not candidate:
            return Gated("auth_benchmark_failed")
        outcome = evaluate_attempt_outcome(
            baseline_score=self.baseline.median_score, candidate_score=candidate.median_score,
            baseline_metrics=self.baseline.median_metrics, candidate_metrics=candidate.median_metrics, config=self.eval_config,
            correctness_ok=True, measurement=cfg.measurement, structural=structural)
        if outcome.verdict == "reject_invalid_measurement":  # a bench that measured nothing neither accepts nor reverts
            raise StageFailure(outcome.measurement_failures[0])
        log_event(cfg.repo_root, query, workload, "optimization_worker_gate_verdict", verdict=outcome.verdict,
                  improvement_pct=outcome.improvement_pct, workload_name=workload.get("name", self.workload_name))
        return Gated(structural=structural, candidate=candidate, outcome=outcome)

    def settle(self, attempt: int, hypothesis: dict[str, Any] | None, gated: Gated) -> bool:
        """The measured candidate is kept, explored further, or reverted; whether the iteration stops."""
        cfg, workload = self.cfg, self.workload
        cand, outcome = gated.candidate, gated.outcome
        pct, metrics = outcome.improvement_pct, cand.median_metrics
        if outcome.verdict in ("reject_broken", "reject_regress"):
            return self.reject(attempt, hypothesis, outcome.verdict, outcome.reason, improvement_pct=pct, metrics=metrics)
        if outcome.verdict == "keep_explore":
            if self.depth >= EXPERIMENTAL_CHAIN_CAP:
                return self.reject(attempt, hypothesis, "keep_explore_chain_cap_reached", "keep_explore_chain_cap_reached", tell=False,
                                   improvement_pct=pct, metrics=metrics)
            self.depth += 1
            self.fails = 0
            save_experimental_branch(self.art, attempt_id=f"iter{self.iteration}_a{attempt}", chain_depth=self.depth,
                                     diagnostic_summary=outcome.diagnostic_vector)
            self.feedback = format_gate_delta(verdict="keep_explore", reason=outcome.reason, hypothesis=hypothesis, improvement_pct=pct)
            self.note(attempt, hypothesis, "keep_explore", kept=True, reason=outcome.reason, improvement_pct=pct, metrics=metrics,
                      diagnostic_vector=outcome.diagnostic_vector)
            clear_attempt_checkpoint(self.art)
            return False
        existing = load_best_valid_baseline(self.art)
        existing_score = score_metrics(existing["metrics"], is_aux=is_aux_workload(workload), workload=workload) if existing.get("metrics") else 0.0
        existing_score = existing_score if existing_score > 0 else float(existing.get("score") or 0.0)
        if existing_score > 0 and cand.median_score + 1e-9 < existing_score:  # a noisy local baseline never ratchets best_valid down
            return self.reject(attempt, hypothesis, "below_best_valid_baseline", "below_best_valid_baseline", tell="reject_regress",
                               detail=f"candidate_score={cand.median_score:.4f} below best_valid={existing_score:.4f}",
                               improvement_pct=pct, metrics=metrics)
        attempt_id = f"iter{self.iteration}_a{attempt}"
        advance_best_valid_baseline(cfg, workload, self.query, attempt_id=attempt_id, score=cand.median_score, metrics=metrics)
        if gated.structural:
            memory.record_design_revision(self.art, attempt_id=attempt_id, hypothesis=hypothesis, before=self.baseline.median_metrics, after=metrics)
        self.depth = self.fails = 0
        clear_attempt_checkpoint(self.art)
        self.baseline = cand
        if self.worker is None:
            self.build_evidence()
        self.feedback = format_gate_delta(verdict="accept", reason=outcome.reason, hypothesis=hypothesis, improvement_pct=pct)
        self.note(attempt, hypothesis, "accept", accepted=True, kept=True, reason=outcome.reason, improvement_pct=pct, metrics=metrics,
                  diagnostic_vector=outcome.diagnostic_vector)
        return False

    def run(self) -> Candidate | None:
        cfg, workload, query = self.cfg, self.workload, self.query
        _log("preflight build before optimization iteration")
        ok, out = run_build(cfg, workload, query)
        if not ok:
            infra = _fail_fast_infra_if_needed(cfg, workload, query, text=out or "")
            raise StageFailure(Failure(
                kind="infra" if infra else "code", stage="optimization", source="preflight_build",
                summary="preflight build failed before the optimization iteration; fix the tree or restore the checkpoint",
                evidence=(out or "")[-2000:], infra_reason=infra.get("infra_reason") if infra else None))
        _log(f"optimization write frequencies (baseline attempt 0, iter {self.iteration}): "
             f"{bench.ensure_attempt_write_frequencies(self.art, workload, optimization_iter=self.iteration, attempt=0)}")
        _log("measuring optimization baseline (aux: 0% + sampled write mixes, score=dual_axis_weighted read^0.75*write_mix^0.25)")
        self.measure_baseline()
        if not load_best_valid_baseline(self.art):
            snapshot_best_valid_baseline_files(cfg, workload, query)
            advance_best_valid_baseline(cfg, workload, query, attempt_id=f"iter{self.iteration}_baseline",
                                        score=self.baseline.median_score, metrics=self.baseline.median_metrics)
        if self.worker is None:
            self.build_evidence()
        for attempt in range(1, self.max_attempts + 1):
            if self.fails >= self.fail_stop or (self.deadline is not None and time.time() >= self.deadline):
                break
            experimental = load_experimental_branch(self.art)
            if experimental:
                self.depth = int(experimental.get("chain_depth") or 0)
            _log(f"optimization write frequencies (iter {self.iteration} attempt {attempt}): "
                 f"{bench.ensure_attempt_write_frequencies(self.art, workload, optimization_iter=self.iteration, attempt=attempt)}")
            begin_attempt_checkpoint(cfg, workload, query, attempt_id=f"iter{self.iteration}_a{attempt}", hypothesis="pending")
            memory.refresh_optimization_runtime(cfg, workload, query, optimization_iter=self.iteration, episode=attempt)
            cand = self.last_result = self.candidate(attempt, json.dumps(experimental.get("diagnostic_summary") or {}, indent=2) if experimental else "")
            (self.art / "attempt_state.json").write_text(json.dumps({"attempt": attempt, "status": cand.status}, indent=2), encoding="utf-8")
            if cand.status == "saturated":
                honored, detail = self.judge_saturation(attempt, cand.saturation)
                if honored:
                    _log(f"OPTIMIZATION_SATURATED honored — {detail}")
                    break
                cand.status, cand.reason = "failed", detail  # a rejected claim is no iteration-level stop
                self.fails += 1
                continue
            if cand.status != "ready":
                label = "cannot_test" if cand.status == "cannot_test" else "candidate_failed"
                if label == "candidate_failed":
                    _log(f"candidate failed: {_short(cand.reason or label)}")
                stop = self.reject(attempt, cand.hypothesis, label, f"{label}: {cand.reason}" if label == "cannot_test" else cand.reason or label, tell=False)
            else:
                gated = self.gate(attempt, cand.hypothesis)
                if gated.label:
                    reason = f"{gated.label}: {gated.detail[-500:]}" if gated.detail and not gated.infra else gated.label
                    stop = self.reject(attempt, cand.hypothesis, gated.label, reason, detail=gated.detail) or gated.infra
                else:
                    stop = self.settle(attempt, cand.hypothesis, gated)
            if stop:
                break
        return self.last_result


def run_iteration(
    *, pipeline_config: PipelineConfig, model_config: ModelConfig, workload_name: str, query: int, optimization_iter: int,
    model_overrides: dict[str, str] | None = None, dry_run: bool = False, worker: OptimizationWorker | None = None,
    deadline: float | None = None,
) -> Any:
    """One optimization iteration; no new attempt starts after `deadline` (time.time())."""
    it = _Iteration(pipeline_config, model_config, workload_name, query, optimization_iter, model_overrides, worker, deadline)
    if dry_run:
        return {"dry_run": True, "stage": "optimization", "max_attempts": it.max_attempts}
    model_id = make_model_id("optimization", it.cfg, model_config, model_overrides)
    log_stage_start(it.cfg.repo_root, query, it.workload, stage="optimization", model_id=model_id, optimization_iter=optimization_iter)
    last = it.run()
    log_stage_complete(it.cfg.repo_root, query, it.workload, stage="optimization", model_id=model_id, backend="codex_exec",
                       duration_ms=None, usage=None, optimization_iter=optimization_iter)
    return last


# The stage

def _restore_best_kernel_at_start(cfg: PipelineConfig, workload: dict[str, Any], query: int, art: Path, iteration: int) -> None:
    if workload.get("workload_kind") != "aux_state":
        state = load_base_optimization_state(art)
        maybe_restore_best_kernel(cfg, workload, query, best_speedup_median=float(state.get("best_speedup_median") or 0.0),
                                  last_speedup_median=float(state.get("last_speedup_median") or 0.0),
                                  reason=f"optimization iter {iteration} start")
        return
    # An aux stage starts from the fair-sweep best kernel when the tree (or best_valid) drifted below it.
    if not has_best_kernel_snapshot(art):
        return
    if iteration == 1:
        fair_best = float(_read_json_or_empty(snapshot_dir(art) / MANIFEST_NAME).get("speedup_median") or 0.0)
        quick_best = float(load_best_valid_baseline(art).get("score") or 0.0)
        if not (fair_best > 0 and (quick_best <= 0 or quick_best + 0.05 < fair_best)):
            return
    manifest = restore_best_kernel(cfg, workload, query)
    if manifest is None:
        return
    rebuild_workload_targets(cfg, workload, query)
    append_experiment_log(cfg.repo_root, query, f"- optimization iter {iteration} start: restored best aux kernel "
                          f"({manifest.get('speedup_median', 0):.2f}x read path from {manifest.get('change_kind', 'unknown')} iter "
                          f"{manifest.get('iteration', '?')})", workload=workload)
    (art / BEST_VALID_BASELINE_FILE).unlink(missing_ok=True)  # the next measure re-seeds it from the restored tree


def run(
    pipeline_config: PipelineConfig, model_config: ModelConfig, workload_name: str, query: int,
    model_overrides: dict[str, str] | None = None, dry_run: bool = False, *, skip_gates: bool = False,
):
    """The optimization stage: iterations, each ending in the full gate batch, until the agent declares saturation,
    iterations stop improving, or the stage's time budget is spent."""
    workload = load_run_workload(pipeline_config, workload_name)
    opt_gates = workload_post_optimization_gates(workload)
    budget = float(getattr(pipeline_config, "optimization_stage_budget_sec", 0) or 0)
    deadline = time.time() + budget if budget > 0 else None
    art = _art(pipeline_config, workload, query)
    note = lambda text: append_experiment_log(pipeline_config.repo_root, query, text, workload=workload)  # noqa: E731
    worker = None
    if not dry_run and bool(getattr(pipeline_config, "optimization_persistent_worker", True)):
        worker = OptimizationWorker.create(pipeline_config=pipeline_config, model_config=model_config, workload_name=workload_name,
                                           query=query, model_overrides=model_overrides)
    last_result, no_improve = None, 0
    try:
        for iteration in range(1, pipeline_config.optimization_max_iterations + 1):
            if deadline is not None and time.time() >= deadline and not dry_run:
                note(f"- iter {iteration}: optimization stage budget ({budget:.0f}s) spent; going to audit")
                break
            if not dry_run:
                _restore_best_kernel_at_start(pipeline_config, workload, query, art, iteration)

            def iterate():
                return run_iteration(pipeline_config=pipeline_config, model_config=model_config, workload_name=workload_name, query=query,
                                     optimization_iter=iteration, model_overrides=model_overrides, dry_run=dry_run, worker=worker, deadline=deadline)

            if dry_run:
                last_result = iterate()
                continue
            repair_ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="optimization", change_kind="optimization",
                                   iteration=iteration, model_overrides=model_overrides)
            last_result = run_with_repair(repair_ctx, iterate)
            saturated = getattr(last_result, "status", None) == "saturated"
            if saturated and skip_gates:
                note(f"- iter {iteration}: agent declared OPTIMIZATION_SATURATED; skipping remaining iters")
                break
            if skip_gates:
                continue
            _log(f"optimization iter {iteration}: running full gate batch ({len(opt_gates)} gates, end-of-iteration only)")
            run_gates_with_repair(pipeline_config, model_config, workload_name, query, opt_gates, change_kind="optimization",
                                  iteration=iteration, model_overrides=model_overrides)
            if workload.get("workload_kind") == "aux_state":
                best = lambda: float(load_optimization_state(pipeline_config.repo_root, query, workload).get("best_composite_score", 0.0) or 0.0)  # noqa: E731
                before = best()
                process_aux_bench_after_gates(pipeline_config.repo_root, query, iteration=iteration, change_kind="optimization", workload=workload)
                improved = best() > before
            else:
                improved = bool(process_base_bench_after_gates(pipeline_config.repo_root, query, workload, iteration=iteration,
                                                               change_kind="optimization").get("improved"))
            no_improve = 0 if improved else no_improve + 1
            memory.merge_playbook_proposal(pipeline_config.repo_root, query, workload=workload)
            if saturated:
                note(f"- iter {iteration}: OPTIMIZATION_SATURATED — ran end-of-iter gates, skipping remaining iterations")
                break
            if no_improve >= pipeline_config.optimization_no_improvement_stop:
                note(f"- iter {iteration}: stopping after {no_improve} iterations without improvement")
                break
    finally:
        if worker is not None:
            worker.close()
    return last_result


# The A/B trial tools: the agent's exploratory trials inside an attempt

@dataclass
class AbToolContext:
    pipeline_config: PipelineConfig
    query: int
    workload: dict[str, Any]
    optimization_iter: int = 0
    attempt: int | None = None

    @property
    def art(self) -> Path:
        return _art(self.pipeline_config, self.workload, self.query)

    @property
    def is_aux(self) -> bool:
        return is_aux_workload(self.workload)


def _reference_score(ctx: AbToolContext) -> float:
    """The score a trial must beat: best valid only (the stable scoreboard)."""
    board = memory.build_scoreboard(ctx.art, is_aux=ctx.is_aux)
    memory.write_scoreboard(ctx.art, board)
    if board.get("best_valid_score"):
        return float(board["best_valid_score"])
    best = load_best_valid_baseline(ctx.art)
    try:
        if best.get("score") is not None:
            return float(best["score"])
    except (TypeError, ValueError):
        pass
    if best.get("metrics"):
        return bench.primary_score_from_metrics(dict(best["metrics"]), is_aux=ctx.is_aux)
    # last resort: the fair sweep / kernel snapshot, still better than a noisy local baseline
    if ctx.is_aux and (fair := read_json(ctx.art / "aux_bench_summary.json")):
        return bench.primary_score_from_metrics(fair, is_aux=True)
    return float(read_json(ctx.art / SNAPSHOT_DIRNAME / MANIFEST_NAME).get("speedup_median") or 0.0)


def _build_bench_target(ctx: AbToolContext) -> str | None:
    """Rebuild the bench target through the host; why not, or None."""
    cfg = ctx.pipeline_config
    paths = resolve_workload_paths(cfg, ctx.workload, ctx.query)
    return HostRun.worker(cfg, ctx.workload).build(paths["aux_bench_target" if ctx.is_aux else "bench_target"])


def make_ab_tools(ctx: AbToolContext):
    cfg, art = ctx.pipeline_config, ctx.art
    trial_file, checkpoint = art / ACTIVE_TRIAL_FILE, art / AB_CHECKPOINT
    candidate_score = lambda metrics: aux_optimization_score(metrics, ctx.workload) if ctx.is_aux else float(metrics.get("speedup_median") or 0.0)  # noqa: E731

    def active_trial() -> dict[str, Any]:
        return json.loads(trial_file.read_text(encoding="utf-8")) if trial_file.is_file() else {}

    def board_with(metrics: dict[str, Any] | None = None) -> dict[str, Any]:
        board = memory.build_scoreboard(art, is_aux=ctx.is_aux, last_metrics=metrics)
        memory.write_scoreboard(art, board)
        return board

    def begin_ab_trial(trial_id: str, hypothesis: str, hot_spot: str = "", expected_gain: str = "", strategy: str = "") -> str:
        """Snapshot current kernel files before an A/B trial that targets a profiled hot spot."""
        if active_trial():
            return "ERROR: an A/B trial is already active; call end_ab_trial first."
        if error := memory.profile_guidance_error(art, hot_spot=hot_spot, expected_gain=expected_gain, strategy=strategy):
            return f"ERROR: a trial starts from the profile: {error}"
        files = _tracked_files(cfg, ctx.workload, ctx.query)
        if not files:
            return "ERROR: no kernel files found to checkpoint."
        copied = _save_files(cfg.repo_root, files, checkpoint)
        reference = _reference_score(ctx)
        board = board_with()
        memory.record_trial(art, trial_id=trial_id, hot_spot=hot_spot, expected_gain=expected_gain, strategy=strategy)
        trial_file.write_text(json.dumps({"trial_id": trial_id, "hypothesis": hypothesis, "files": copied, "reference_score": reference,
                                          "compare_against": "best_valid_score", "scoreboard": board}, indent=2), encoding="utf-8")
        return json.dumps({"status": "checkpointed", "trial_id": trial_id, "files": copied, "reference_score": reference,
                           "compare_against": "best_valid_score", "scoreboard": board,
                           "guidance": "A/B keep/regress vs scoreboard.best_valid_score only. "
                                       "After edits: quick_check, then run_quick_benchmark(acceptance=True), then end_ab_trial."}, indent=2)

    def run_quick_benchmark(include_profile: bool | None = True, acceptance: bool | None = False) -> str:
        """Run a benchmark and return compact metrics. Default: the quick tier (a few queries, read
        axis only: direction, not a measurement). acceptance=True: the full optimization tier
        the accept/reject decision uses."""
        include_profile, acceptance = bool(none_default(include_profile, True)), bool(none_default(acceptance, False))
        measured = bench.measure_quick_benchmark(cfg, ctx.workload, ctx.query, art=art, optimization_iter=ctx.optimization_iter,
                                                 attempt=ctx.attempt, include_profile=include_profile, acceptance=acceptance)
        if isinstance(measured, str):
            return f"ERROR: {measured}"
        metrics, invalid = measured.metrics, measured.invalid
        active = active_trial() if acceptance else {}
        if active and invalid:  # a bench that did not measure the accelerator voids the trial's metrics: end_ab_trial then discards
            for key in ("last_metrics", "last_score", "last_improvement_pct"):
                active.pop(key, None)
            trial_file.write_text(json.dumps(active, indent=2), encoding="utf-8")
            active = {}
        if active:
            score, reference = candidate_score(metrics), float(active.get("reference_score") or 0.0)
            active.update(last_metrics=metrics, last_score=score, last_improvement_pct=improvement_ratio(reference, score) if reference > 0 else None)
            trial_file.write_text(json.dumps(active, indent=2), encoding="utf-8")
        memory.refresh_optimization_runtime(cfg, ctx.workload, ctx.query, optimization_iter=ctx.optimization_iter)
        board = board_with(metrics)
        payload = json.loads(bench.format_quick_benchmark_result(metrics, aux=ctx.is_aux))
        payload["scoreboard"] = board
        payload["tier"] = "acceptance" if acceptance else (
            "quick: direction only, never kept or discarded on this; end_ab_trial(keep=True) needs an acceptance=True run")
        if lease := lease_note(measured.output):
            payload["measurement_lease"] = {**lease, "note": f"waited {lease['waited_sec']} s for the measurement lease"}
        elif not acceptance and (noise := noise_warning(cfg.repair.load_factor)):
            payload["measurement_noise"] = noise
        if invalid:
            payload["measurement_invalid"] = invalid
            payload["ab_signal"] = "invalid_measurement"
        elif active and active.get("last_improvement_pct") is not None:
            pct = float(active["last_improvement_pct"])  # surfaced explicitly so an agent cannot ignore a regression
            payload.update(vs_reference_score=active.get("reference_score"), vs_best_valid_score=board.get("best_valid_score"),
                           improvement_pct_vs_reference=pct, ab_signal="improve" if pct > 0.5 else "regress" if pct < -0.5 else "flat",
                           ab_compare_against="best_valid_score")
        return json.dumps(payload, indent=2)

    def end_ab_trial(trial_id: str, keep: bool) -> str:
        """End an A/B trial: keep changes or restore the checkpoint."""
        active = active_trial()
        if not active:
            return "ERROR: no active A/B trial; call begin_ab_trial first."
        if active.get("trial_id") != trial_id:
            return f"ERROR: active trial is {active.get('trial_id')!r}, not {trial_id!r}."
        metrics = dict(active.get("last_metrics") or {})
        forced = ""
        if keep and not metrics:
            keep, forced = False, "keep_requested_without_metrics"
        elif keep:  # never keep a measured regression against the reference
            score, reference = candidate_score(metrics), float(active.get("reference_score") or _reference_score(ctx) or 0.0)
            faster_build = reference > 0 and build_win(dict(load_best_valid_baseline(art).get("metrics") or {}), metrics,
                                                       improvement_ratio(reference, score), eval_config_from_pipeline(cfg))
            if reference > 0 and score + 1e-9 < reference and not faster_build:
                keep, forced = False, f"exploratory_regress cand={score:.4f} < reference={reference:.4f}"
        restored: list[str] = []
        if not keep:
            restored = _restore_files(cfg.repo_root, active.get("files") or [], checkpoint)
            if problem := _build_bench_target(ctx):
                return f"ERROR: rebuild after restore failed\n{problem}"
        memory.record_attempt(art, repo_root=cfg.repo_root, workload=ctx.workload, query=ctx.query, attempt=trial_id,
                              hypothesis=str(active.get("hypothesis") or ""), verdict="trial_kept" if keep else "trial_discarded",
                              metrics=metrics, kept=keep, source="ab_trial")
        trial_file.unlink(missing_ok=True)
        shutil.rmtree(checkpoint, ignore_errors=True)
        memory.refresh_optimization_runtime(cfg, ctx.workload, ctx.query, optimization_iter=ctx.optimization_iter)
        payload: dict[str, Any] = {"trial_id": trial_id, "kept": keep, "restored_files": restored, "metrics": metrics,
                                   "scoreboard": board_with(metrics or None), "compare_against": "best_valid_score"}
        if forced:
            payload.update(forced_discard=True, discard_reason=forced, guidance=(
                "Exploratory A/B showed a regression vs best_valid_score (or missing metrics). Do not yield CANDIDATE_READY for this "
                "change; form a new hypothesis. After restore, call run_profile — prior phase breakdown is stale."))
        elif keep:
            payload["guidance"] = ("Exploratory A/B improved vs best_valid_score — you may yield CANDIDATE_READY for authoritative "
                                   "gates. Remeasure with run_profile before the next hypothesis.")
        else:
            payload["guidance"] = ("Changes restored. Do not yield the discarded candidate; try another hypothesis. "
                                   "Call run_profile on the restored tree before hypothesizing again.")
        return json.dumps(payload, indent=2)

    return begin_ab_trial, run_quick_benchmark, end_ab_trial


# The MCP tools of the stage

def callables(ctx: ToolContext) -> dict[str, Callable[..., str]]:
    begin_ab_trial, run_quick_benchmark, end_ab_trial = make_ab_tools(
        AbToolContext(pipeline_config=ctx.pipeline_config, query=ctx.query, workload=ctx.workload, optimization_iter=ctx.optimization_iter))
    return {**memory.digest_tools(ctx.pipeline_config, ctx.workload, ctx.query), "begin_ab_trial": begin_ab_trial,
            "run_quick_benchmark": run_quick_benchmark, "end_ab_trial": end_ab_trial}


def register(tool, call) -> None:
    @tool(read_only=True)
    def get_profile_digest() -> str:
        """Compact profile/baseline digest from artifacts."""
        return call("get_profile_digest")

    @tool(read_only=True)
    def get_optimization_evidence() -> str:
        """Optimization evidence / runtime packet."""
        return call("get_optimization_evidence")

    @tool(read_only=True)
    def get_store_diagnostics() -> str:
        """Aux store diagnostics if present."""
        return call("get_store_diagnostics")

    @tool(read_only=False)
    def begin_ab_trial(trial_id: str, hypothesis: str, hot_spot: str = "", expected_gain: str = "", strategy: str = "") -> str:
        """Begin exploratory A/B trial (non-binding). Start from run_profile output: hot_spot names the
        hot function or loop it targets, expected_gain the gain you expect there, strategy is
        structural | low_level | other."""
        return call("begin_ab_trial", trial_id, hypothesis, hot_spot, expected_gain, strategy)

    @tool(read_only=False)
    def run_quick_benchmark(include_profile: bool = True, acceptance: bool = False) -> str:
        """Run the benchmark for the active A/B trial. Default: the quick tier, seconds, read axis only,
        direction not measurement. acceptance=True: the full optimization tier; end_ab_trial(keep=True)
        needs one."""
        return call("run_quick_benchmark", include_profile, acceptance)

    @tool(read_only=False)
    def end_ab_trial(trial_id: str, keep: bool = False) -> str:
        """End exploratory A/B trial."""
        return call("end_ab_trial", trial_id, keep)
