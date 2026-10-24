"""The seconds-level inner loop: incremental build, the query's tests, a subset differential.

run_quick_check runs the quick_check gate (gates/run_aux_quick_check.sh) on the subset dataset
and turns its result into a QuickResult.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import os
import re
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from accel_pipeline.build_stamp import code_stamp
from accel_pipeline import test_suite as ts
from accel_pipeline import write_cost
from accel_pipeline.config import PipelineConfig
from accel_pipeline.failure import Failure, render_failure
from accel_pipeline.gate_stamps import current_stamp
from accel_pipeline.gates import GATE_TIMEOUT_S, run_gate
from accel_pipeline.layout import query_dir, workload_schema_sql
from accel_pipeline.paths import gates_dir_for_query, query_artifacts_dir
from accel_pipeline.test_suite import suite_path
from accel_pipeline.workload_facts import scratch_connection

QUICK_GATE = "quick_check"
ADMISSION_FILE = f"{QUICK_GATE}.admitted"
SUBSET_MAX_SOURCE_GB = float(os.environ.get("ACCEL_SUBSET_MAX_SOURCE_GB", 30))


@dataclass
class QuickResult:
    status: str  # "pass" | "fail" | "skip"
    phase_timings: dict[str, float] = field(default_factory=dict)
    failure: Failure | None = None
    summary: str = ""
    projection: list[str] = field(default_factory=list)  # the projected full-gate write cost per transaction kind


def _sql_statements(path: Path) -> list[str]:
    text = re.sub(r"--[^\n]*", "", path.read_text(encoding="utf-8"))
    return [stmt.strip() for stmt in text.split(";") if stmt.strip()]


def _subset_key(source: Path, subset_sql: Path, schema_sql: Path) -> str:
    digest = hashlib.sha256(str(source.resolve()).encode())
    digest.update(subset_sql.read_bytes() + schema_sql.read_bytes())
    for tbl in sorted(source.glob("*.tbl")):
        digest.update(f"{tbl.name}{tbl.stat().st_size}{tbl.stat().st_mtime_ns}".encode())
    return digest.hexdigest()[:16]


def _write_subset(source: Path, subset_sql: Path, schema_sql: Path, dest: Path) -> None:
    """Load `source` into a scratch database, cut the subset out of it with `subset_sql` (written
    against the source attached as `src`) and export its tables as <table>.tbl files into `dest`."""
    with tempfile.TemporaryDirectory() as scratch:
        full = scratch_connection(f"{scratch}/full.db")
        for stmt in _sql_statements(schema_sql):
            full.execute(stmt)
        for tbl in sorted(source.glob("*.tbl")):
            full.execute(f"COPY {tbl.stem} FROM '{tbl}' (DELIMITER '|', HEADER false)")
        full.close()
        subset = scratch_connection(f"{scratch}/subset.db")
        subset.execute(f"ATTACH '{scratch}/full.db' AS src (READ_ONLY)")
        for stmt in _sql_statements(subset_sql):
            subset.execute(stmt)
        tables = subset.execute(
            "SELECT table_name FROM information_schema.tables "
            "WHERE table_schema = 'main' AND table_catalog = current_database()"
        ).fetchall()
        for (table,) in tables:
            subset.execute(f"COPY {table} TO '{dest}/{table}.tbl' (DELIMITER '|', HEADER false)")
        subset.close()


def ensure_subset_dataset(pipeline_config: PipelineConfig, workload: dict[str, Any]) -> Path | None:
    """Directory of <table>.tbl files holding the workload's test subset (workloads/<bench>/test_db_subset.sql
    cut out of the full dataset), built once per dataset. None when the workload has no subset, the
    dataset is missing, or it is too large to load here (ACCEL_SUBSET_MAX_SOURCE_GB): the quick tier
    then runs on the full dataset."""
    if os.environ.get("ACCEL_SUBSET_DATASET_DIR"):
        return Path(os.environ["ACCEL_SUBSET_DATASET_DIR"])
    bench = str(workload.get("benchmark_name") or "")
    repo = pipeline_config.repo_root
    subset_sql = repo / "workloads" / bench / "test_db_subset.sql"
    schema_sql = repo / workload_schema_sql(str(workload.get("dataset_benchmark") or bench))
    source = Path(str(workload.get("dataset_dir") or os.environ.get("ACCEL_DATASET_DIR") or ""))
    if not (subset_sql.is_file() and schema_sql.is_file() and source.is_dir() and any(source.glob("*.tbl"))):
        return None
    if sum(t.stat().st_size for t in source.glob("*.tbl")) > SUBSET_MAX_SOURCE_GB * 2**30:
        return None
    dest = Path(f"{pipeline_config.build_dir}.subset") / f"{bench}-{_subset_key(source, subset_sql, schema_sql)}"
    dest.parent.mkdir(parents=True, exist_ok=True)
    with (dest.parent / f"{dest.name}.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not (dest / ".done").is_file():
            dest.mkdir(exist_ok=True)
            _write_subset(source, subset_sql, schema_sql, dest)
            (dest / ".done").touch()
    return dest


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


def _project_write_cost(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, subset: Path | None) -> write_cost.Projection | None:
    """The full gate's write time, projected from the timings of the quick differential (None: no timings)."""
    bench = str(workload.get("benchmark_name") or "")
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    reports = write_cost.load_reports(gates_dir_for_query(art))
    suite = suite_path(pipeline_config.repo_root, bench, query)
    if not reports or not (suite / ts.MANIFEST).is_file():
        return None
    full = Path(str(workload.get("dataset_dir") or os.environ.get("ACCEL_DATASET_DIR") or ""))
    probes = int(((ts._yaml(suite / ts.MANIFEST).get("point_writes") or {}).get("max_probes")) or 5)
    return write_cost.project(
        reports, profiles=ts.txn_profiles(suite), dependent=ts.dependent_tables(art),
        scale=write_cost.table_scale(full if full.is_dir() else None, subset), probes_full=probes,
        budget_s=GATE_TIMEOUT_S * pipeline_config.quick_tier.write_cost_budget_share)


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
SANITIZER_TIMEOUT_SEC = 3600  # the first instrumented build of a worker; later checks are incremental


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
        return QuickResult(status="pass", summary="unchanged since the last passing quick_check")

    started = time.monotonic()
    subset = ensure_subset_dataset(pipeline_config, workload)
    gates_dir = gates_dir_for_query(query_artifacts_dir(pipeline_config.repo_root, query, workload=workload))
    for stale in write_cost.report_paths(gates_dir):
        stale.unlink(missing_ok=True)
    timings = {"subset_dataset": round(time.monotonic() - started, 3)}
    result = run_gate(
        pipeline_config, QUICK_GATE, query, workload,
        env={
            "ACCEL_QUICK_TARGETS": " ".join(extra_targets),
            "ACCEL_QUICK_DIFFERENTIAL": "1" if differential else "0",
            "ACCEL_QUICK_PROBES": str(quick.differential_probes),
            "ACCEL_BUILD_STAMP_CACHE": "1" if quick.build_stamp_cache else "0",
            **({"ACCEL_QUICK_SANITIZER": sanitizer, "ACCEL_QUICK_THREADS": str(threads)} if sanitizer else {}),
        },
        timeout_s=max(quick.timeout_sec, SANITIZER_TIMEOUT_SEC) if sanitizer else quick.timeout_sec,
    )
    timings.update(result.data.get("phase_timings") or {})
    if result.status == "skip":
        return QuickResult(status="skip", phase_timings=timings, summary=str(result.data.get("reason") or "skipped"))
    if result.status == "pass":
        projection = _project_write_cost(pipeline_config, workload, query, subset) if admits else None
        if projection and projection.over:
            passed_path.unlink(missing_ok=True)
            failure = Failure(
                kind="code", stage=QUICK_GATE, source=QUICK_GATE,
                summary=f"quick_check: {projection.summary()}: the full gate would time out. Costliest: {projection.worst}",
                evidence="\n".join(projection.lines),
                diagnosis=[f"{name}: {seconds:.1f}s" for name, seconds in timings.items()])
            return QuickResult(status="fail", phase_timings=timings, failure=failure, summary=failure.summary,
                               projection=projection.lines)
        if stamp:
            passed_path.parent.mkdir(parents=True, exist_ok=True)
            passed_path.write_text(stamp, encoding="utf-8")
        if admits:
            record_admission(pipeline_config, workload, query)
        return QuickResult(status="pass", phase_timings=timings, summary="quick_check passed" + (f"; {projection.summary()}" if projection else ""),
                           projection=projection.lines if projection else [])
    passed_path.unlink(missing_ok=True)
    failure = _failure(result.data, timings)
    return QuickResult(status="fail", phase_timings=timings, failure=failure, summary=failure.summary)


def render_quick_result(result: QuickResult) -> str:
    """Compact JSON for the agent: status, seconds per phase, and on failure the rendered failure."""
    payload: dict[str, Any] = {"status": result.status, "summary": result.summary}
    if result.projection:
        payload["projected_full_gate_write_cost"] = result.projection
    if result.phase_timings:
        payload["phase_seconds"] = result.phase_timings
    if result.failure:
        payload["failure"] = render_failure(result.failure)
    return json.dumps(payload, indent=2)
