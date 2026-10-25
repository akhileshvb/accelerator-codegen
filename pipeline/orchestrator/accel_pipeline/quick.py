"""The seconds-level inner loop: incremental build, the query's tests, a subset differential.

run_quick_check runs the quick_check gate (gates/run_aux_quick_check.sh) on the test databases (test_db.py) and
turns its result into a QuickResult: correctness, and on a plain pass the compact report of check_report.py
(build time and scaling, read latency, write cost per kind, perf counters).
"""

from __future__ import annotations

import json
import os
import subprocess
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from accel_pipeline import check_report, write_cost
from accel_pipeline import test_suite as ts
from accel_pipeline.build_stamp import code_stamp
from accel_pipeline.check_measure import Bench
from accel_pipeline.config import PipelineConfig
from accel_pipeline.failure import Failure, render_failure
from accel_pipeline.gate_stamps import current_stamp
from accel_pipeline.gates import run_gate
from accel_pipeline.layout import query_dir
from accel_pipeline.perf_profile import render_for_agent
from accel_pipeline.paths import gates_dir_for_query, load_query_analysis, query_artifacts_dir
from accel_pipeline.test_db import CutDatasets, ensure_cut_datasets
from accel_pipeline.test_suite import suite_path

QUICK_GATE = "quick_check"
# What kills one differential run inside a gate (aux_gate_common.sh engine_run): the budget the projection must fit.
GATE_RUN_TIMEOUT_S = float(os.environ.get("ACCEL_GATE_RUN_TIMEOUT", 900))
ADMISSION_FILE = f"{QUICK_GATE}.admitted"
PROJECTION_LINES = 5  # the costliest write kinds a passing check reports
INSTANCE_KEYS = ("engine_instance", "engine_state_dir", "dataset")  # what the gate result names of the instance it ran in
REPORT_FILE = f"{QUICK_GATE}.report.json"  # what plain passes left: their reports and the engine instance


@dataclass
class QuickResult:
    status: str  # "pass" | "fail" | "skip"
    phase_timings: dict[str, float] = field(default_factory=dict)
    failure: Failure | None = None
    summary: str = ""
    report: dict[str, Any] = field(default_factory=dict)  # build / read / writes / counters, see check_report.py


def _result_stamp(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int,
                  targets: list[str], differential: bool, sanitizer: str = "", threads: int = 0) -> str:
    bench = str(workload.get("benchmark_name") or "")
    return code_stamp(
        pipeline_config.repo_root,
        query_dir=query_dir(bench, query) if bench else "",
        extra=[QUICK_GATE, *targets, f"differential={differential}", f"sanitizer={sanitizer}:{threads}",
               str(pipeline_config.build_dir)],
    )


def _passed_stamp_path(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> Path:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    return gates_dir_for_query(art) / f"{QUICK_GATE}.stamp"


def record_admission(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> None:
    """quick_check passed on the code as it is now: the heavy gates may run."""
    stamp = current_stamp(pipeline_config, workload, query)
    if stamp:
        path = _passed_stamp_path(pipeline_config, workload, query).with_name(ADMISSION_FILE)
        path.write_text(stamp, encoding="utf-8")


def admission_refusal(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, gate: str) -> str:
    """Why `gate` may not run yet ("" when it may): a heavy aux gate needs a passing quick_check on the current code."""
    quick = pipeline_config.quick_tier
    if not quick.enabled or gate not in quick.admission_gates or workload.get("workload_kind") != "aux_state":
        return ""
    path = _passed_stamp_path(pipeline_config, workload, query).with_name(ADMISSION_FILE)
    stamp = current_stamp(pipeline_config, workload, query)
    if stamp and path.is_file() and path.read_text(encoding="utf-8") == stamp:
        return ""
    return f"{gate} refused: run quick_check first and iterate until it passes on the current code (it takes seconds), then request {gate} once."


def _project_write_cost(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, cuts: CutDatasets | None) -> write_cost.Projection | None:
    """The full gate's write time, projected from the timings of the quick differential (None: no timings)."""
    bench = str(workload.get("benchmark_name") or "")
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    reports = write_cost.load_reports(gates_dir_for_query(art))
    suite = suite_path(pipeline_config.repo_root, bench, query)
    if not reports or not (suite / ts.MANIFEST).is_file():
        return None
    probes = int(((ts._yaml(suite / ts.MANIFEST).get("point_writes") or {}).get("max_probes")) or 5)
    return write_cost.project(
        reports, profiles=ts.txn_profiles(suite), dependent=ts.dependent_tables(art),
        scale=cuts.scale if cuts else (lambda tables: 1.0), probes_full=probes,
        budget_s=GATE_RUN_TIMEOUT_S * pipeline_config.quick_tier.write_cost_budget_share)


def _unfaithful_rebuilds(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> list[str]:
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    analysis = art / "analysis.json"
    reports = write_cost.load_reports(gates_dir_for_query(art))
    if not reports or not analysis.is_file():
        return []
    coverage = (json.loads(analysis.read_text(encoding="utf-8")).get("correctness_facts") or {}).get("mutation_coverage") or []
    suite = suite_path(pipeline_config.repo_root, str(workload.get("benchmark_name", "")), query)
    return write_cost.unfaithful_rebuilds(reports, ts.txn_files(suite), coverage, ts.statements)


def _plain_report(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, cuts: CutDatasets | None,
                  instance: dict[str, str]) -> tuple[dict[str, Any], list[str]]:
    """(the compact report of a passing plain check, the defects it shows): rebuild-on-write, writes and a build that
    would not fit the gates, a build that does not scale with threads; then the reads and perf counters."""
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    defects = _unfaithful_rebuilds(pipeline_config, workload, query)
    defects = [f"{len(defects)} write transaction(s) rebuild the state where the analysis maintains them incrementally "
               f"(at full scale each is a full scan). First: {defects[0]}"] if defects else []
    report: dict[str, Any] = {}
    reports = write_cost.load_reports(gates_dir_for_query(art))
    dependent = ts.dependent_tables(art)
    line, defect = check_report.build_report(
        reports, scale=cuts.scale(dependent) if cuts else 1.0, threads=check_report.gate_threads(),
        budget_s=pipeline_config.quick_tier.build_budget_sec,
        expected_scaling=check_report.expects_scaling(load_query_analysis(pipeline_config.repo_root, query, workload=workload)[1]))
    report.update({"build": line} if line else {})
    defects += [defect] if defect else []
    projection = _project_write_cost(pipeline_config, workload, query, cuts)
    if projection:
        report["writes"] = projection.lines[:PROJECTION_LINES]
        report["writes_total"] = projection.summary()
        if projection.over:
            defects.append(f"{projection.summary()}: the full gate would time out. Costliest: {projection.worst}")
    report.update(_measure(pipeline_config, workload, query, instance))
    return report, defects


def _measure(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, instance: dict[str, str]) -> dict[str, str]:
    """Read latency against the engine's SQL, and perf counters, from the aux bench on the subset."""
    if not instance:
        return {}
    bench = Bench(pipeline_config, workload, query, instance)
    try:
        row = bench.row()
    except (RuntimeError, OSError, subprocess.SubprocessError) as exc:
        return {"read": f"unavailable ({str(exc)[-300:]})"}
    return {"read": check_report.read_line(row), "counters": check_report.counters_line(bench.counters(row))}


def _sanitizer_evidence(data: dict[str, Any]) -> str:
    found = data.get("sanitizer_findings") or {}
    lines = []
    for item in found.get("yours") or []:
        lines += [item["summary"], *[f"    {f}" for f in item["frames"]]]
    infra = found.get("infra") or {}
    if infra.get("count"):
        lines.append(f"infrastructure reports (not failures): {infra['count']}, e.g. " + "; ".join(infra.get("summaries") or []))
    return "\n".join(lines)


def _failure(data: dict[str, Any], timings: dict[str, float]) -> Failure:
    reason = str(data.get("reason") or "")
    step = str(data.get("step") or "")
    return Failure(
        kind="timeout" if "did not finish within" in reason else "code",
        stage=QUICK_GATE,
        source=QUICK_GATE,
        summary=f"quick_check: {reason or data.get('status')}" + (f" (step {step})" if step else ""),
        evidence=_sanitizer_evidence(data) or str(data.get("log_tail") or ""),
        reproduce=str(data.get("reproduce") or ""),
        diagnosis=[f"{name}: {seconds:.1f}s" for name, seconds in timings.items()],
    )


SANITIZERS = ("thread", "address")
PROFILE_SECONDS = 8.0  # of each sampled loop in run_profile (read, write)
SANITIZER_TIMEOUT_SEC = 3600  # the first instrumented build of a worker; later checks are incremental


def prepare_inner_loop(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> threading.Thread | None:
    """Once per run, before analysis: make the test databases, and start building the sanitizer variant of the
    substrate and harness in the background, so the agent's first sanitizer check compiles only the query's files."""
    if not pipeline_config.quick_tier.enabled or workload.get("workload_kind") != "aux_state":
        return None
    cuts = ensure_cut_datasets(pipeline_config, workload, query)
    gates_dir = gates_dir_for_query(query_artifacts_dir(pipeline_config.repo_root, query, workload=workload))

    def build() -> None:
        run_gate(pipeline_config, QUICK_GATE, query, workload, out_dir=gates_dir / "prebuild", timeout_s=SANITIZER_TIMEOUT_SEC,
                 env={"ACCEL_QUICK_SANITIZER": "thread", "ACCEL_QUICK_PREBUILD": "1",
                      **({"ACCEL_SUBSET_DATASET_DIR": str(cuts.tiny)} if cuts else {})})
    thread = threading.Thread(target=build, name="sanitizer-prebuild", daemon=True)
    thread.start()
    return thread


def run_quick_check(pipeline_config: PipelineConfig, workload: dict, query: int, *, targets: list[str] | None = None,
                    differential: bool = True, sanitizer: str = "", threads: int = 4) -> QuickResult:
    """Build the query's targets (skipped when unchanged), run its tests, and, when `differential`,
    the SQL-vs-accelerator differential on the subset dataset. `targets` are extra build targets
    (the agent's own test binaries) that are built and run. A tree unchanged since the last passing
    check passes without running anything.

    `sanitizer` ("thread" or "address") builds in an instrumented build dir of the worker's own (incremental),
    runs the same steps with every phase of the thread budget at `threads`, and reports the sanitizer findings:
    only reports with a frame in the query's own directory fail the check, the rest is counted as
    infrastructure. A host that cannot build the variant reports "not applicable"."""
    if sanitizer and sanitizer not in SANITIZERS:
        return QuickResult(status="fail", summary=f"sanitizer must be one of {SANITIZERS}, not {sanitizer!r}")
    quick = pipeline_config.quick_tier
    if not quick.enabled:
        return QuickResult(status="skip", summary="quick tier disabled (quick_tier.enabled)")
    if workload.get("workload_kind") != "aux_state":
        return QuickResult(status="skip", summary="quick_check covers aux-state workloads")
    extra_targets = list(targets or [])
    stamp = _result_stamp(pipeline_config, workload, query, extra_targets, differential, sanitizer, threads)
    passed_path = _passed_stamp_path(pipeline_config, workload, query)
    admits = differential and not sanitizer  # only the plain run with the differential opens the heavy gates
    if stamp and passed_path.is_file() and passed_path.read_text(encoding="utf-8") == stamp:
        if admits:
            record_admission(pipeline_config, workload, query)
        return QuickResult(status="pass", summary="unchanged since the last passing quick_check",
                           report=saved_report(pipeline_config, workload, query).get("reports", {}).get(stamp, {}))

    started = time.monotonic()
    cuts = ensure_cut_datasets(pipeline_config, workload, query)
    gates_dir = gates_dir_for_query(query_artifacts_dir(pipeline_config.repo_root, query, workload=workload))
    for stale in write_cost.report_paths(gates_dir):
        stale.unlink(missing_ok=True)
    timings = {"test_databases": round(time.monotonic() - started, 3)}
    result = run_gate(
        pipeline_config, QUICK_GATE, query, workload,
        env={
            "ACCEL_QUICK_TARGETS": " ".join(extra_targets),
            "ACCEL_QUICK_DIFFERENTIAL": "1" if differential else "0",
            "ACCEL_QUICK_PROBES": str(quick.differential_probes),
            "ACCEL_BUILD_STAMP_CACHE": "1" if quick.build_stamp_cache else "0",
            **({"ACCEL_SUBSET_DATASET_DIR": str(cuts.tiny if sanitizer else cuts.subset)} if cuts else {}),
            **({"ACCEL_QUICK_SANITIZER": sanitizer, "ACCEL_QUICK_THREADS": str(threads)} if sanitizer else {}),
        },
        timeout_s=max(quick.timeout_sec, SANITIZER_TIMEOUT_SEC) if sanitizer else quick.timeout_sec,
    )
    timings.update(result.data.get("phase_timings") or {})
    if result.status == "skip":
        return QuickResult(status="skip", phase_timings=timings, summary=str(result.data.get("reason") or "skipped"))
    if result.status != "pass":
        passed_path.unlink(missing_ok=True)
        failure = _failure(result.data, timings)
        return QuickResult(status="fail", phase_timings=timings, failure=failure, summary=failure.summary)
    instance = {} if sanitizer else {k: str(result.data[k]) for k in INSTANCE_KEYS if result.data.get(k)}
    report, defects = _plain_report(pipeline_config, workload, query, cuts, instance) if admits else ({}, [])
    if defects:
        passed_path.unlink(missing_ok=True)
        failure = Failure(kind="code", stage=QUICK_GATE, source=QUICK_GATE, summary=f"quick_check: {defects[0]}",
                          diagnosis=defects[1:])
        return QuickResult(status="fail", phase_timings=timings, failure=failure, summary=failure.summary, report=report)
    if stamp:
        passed_path.parent.mkdir(parents=True, exist_ok=True)
        passed_path.write_text(stamp, encoding="utf-8")
        if not sanitizer:
            _save_report(pipeline_config, workload, query, stamp, report, instance)
    if admits:
        record_admission(pipeline_config, workload, query)
    return QuickResult(status="pass", phase_timings=timings, summary="quick_check passed", report=report)


def profile_hot_spots(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> str:
    """run_profile: samples of the read, write and build paths on the subset, as JSON. A tree that does not pass the
    plain check, or a profiler that cannot run, is a failure to fix or report; a server engine is not applicable."""
    checked = run_quick_check(pipeline_config, workload, query, differential=False)
    instance = saved_report(pipeline_config, workload, query).get("instance")
    if checked.status != "pass" or not instance:
        return render_quick_result(checked) if checked.status != "pass" else json.dumps(
            {"status": "fail", "summary": "no quick_check instance to profile in: run quick_check first"})
    profiles = Bench(pipeline_config, workload, query, instance).profile(PROFILE_SECONDS)
    broken = {path: p["reason"] for path, p in profiles.items() if p["status"] == "error"}
    if broken:
        failure = Failure(kind="infra", stage="run_profile", source="perf", summary="run_profile could not sample: " + "; ".join(
            f"{path}: {reason}" for path, reason in broken.items()),
            diagnosis=["profiling failed because of the host or the harness, not because there is nothing to profile: fix what "
                       "the message names, or report it (report_blocker) if it is outside the query's files"])
        return json.dumps({"status": "fail", "failure": render_failure(failure)}, indent=2)
    return json.dumps({"status": "pass", **{f"{path}_path": render_for_agent(p).splitlines() for path, p in profiles.items()}}, indent=2)


def _report_path(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> Path:
    return _passed_stamp_path(pipeline_config, workload, query).with_name(REPORT_FILE)


def saved_report(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """What plain passes left: {"instance": the engine instance and dataset the quick gate loaded (the profile runs
    in it), "reports": {result stamp: report}}; {} before the first."""
    try:
        return json.loads(_report_path(pipeline_config, workload, query).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {}


def _save_report(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, stamp: str,
                 report: dict[str, Any], instance: dict[str, str]) -> None:
    saved = saved_report(pipeline_config, workload, query)
    saved = {"instance": instance, "reports": {**saved.get("reports", {}), stamp: report}}
    _report_path(pipeline_config, workload, query).write_text(json.dumps(saved), encoding="utf-8")


def render_quick_result(result: QuickResult) -> str:
    """Compact JSON for the agent: status, the report, seconds per phase, and on failure the rendered failure."""
    payload: dict[str, Any] = {"status": result.status}
    if not result.failure:  # the failure text carries the summary
        payload["summary"] = result.summary
    payload.update(result.report)
    if result.phase_timings:
        payload["phase_seconds"] = result.phase_timings
    if result.failure:
        payload["failure"] = render_failure(result.failure)
    return json.dumps(payload, indent=2)
