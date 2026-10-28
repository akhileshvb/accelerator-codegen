"""Measuring and scoring the accelerator: the benchmark runs, their parsing, the score, the verdict.

An attempt is measured by running the bench (aux write-frequency sweeps, or the base query bench) several times,
reducing the CSV rows to metrics, and scoring them: the dual-axis score `read^w * write_mix^(1-w)` for stateful
accelerators, the median speedup for base kernels. `check_measurement` says whether the numbers describe the
accelerator at all, `evaluate_attempt_outcome` decides accept / keep_explore / reject. The perf profile of the
hit path and the agent's quick A/B benchmark call live here too.
"""

from __future__ import annotations

import copy
import json
import math
import os
import random
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable

from accel_pipeline import layout
from accel_pipeline.config import (
    MeasurementConfig,
    PipelineConfig,
    find_repo_root,
    load_aux_state_defaults,
    query_artifacts_dir,
    resolve_workload_paths,
)
from accel_pipeline.data import (
    MEASUREMENT_COUNTERS,
    MEASUREMENT_TIMES,
    STORE_DIAGNOSTIC_FIELDS,
    coerce_diagnostic_field,
    draw_write_frequencies,
    parse_aux_benchmark_output,
    parse_sweep_csv,
    read_write_mix,
    suite_path,
)
from accel_pipeline.gates import build_jobs, build_lock
from accel_pipeline.pack import FairLock, ensure_private_dataset_db, host_lock_dir, run_measured
from accel_pipeline.repair import Failure


# The perf profile of the hit path
#
# `perf` runs the aux bench's read-only hit loop and reports a sampling profile (self time by function, folded into
# the module's own code, the rest of the accelerator library, and the engine and system libraries), hardware
# counters per query, whether the hottest own function is vectorized, and the visited-state count the read path
# reports. Counters and samples cover only the measured read loop: perf starts disabled and the bench opens the
# window through the `ACCEL_PROFILE_CONTROL` fifos. An engine whose hit path does not run inside the bench
# process is `not_applicable`; a machine where `perf` cannot run, or a bench that never opens the window, is an
# `error` the caller fails on: a broken profiler is never read as "nothing to profile".
PERF_BIN_ENV = "ACCEL_PERF_BIN"
SAMPLE_HZ = 10000
# perf record ring pages per CPU: every session of a user shares one locked-memory budget (perf_event_mlock_kb +
# ulimit -l) that the default size spends across every CPU; concurrent profiling runs then fail to map their pages
MMAP_PAGES = 8
TOP_N = 15
STAT_EVENTS = (
    "task-clock",
    "cycles",
    "instructions",
    "branches",
    "branch-misses",
    "L1-dcache-load-misses",
    "cache-references",
    "cache-misses",
)

PROFILE_CONTROL_ENV = "ACCEL_PROFILE_CONTROL"
MIN_QUERIES = 100
MAX_QUERIES = 100_000
TARGET_LOOP_SEC = 6.0
ACCELERATOR_DSO_MARKER = "accelerator_codegen"
MODULE_NAMESPACE = re.compile(r"::(query|q\d+)::")  # accelerators/<bench>/q<N> code, and templates instantiated over its types
PERF_TIMEOUT_SEC = 300
WIDE_VECTOR_RE = re.compile(r"\b[yz]mm\d+")
SAMPLE_LINE_RE = re.compile(r"^\s*(?P<pct>\d+(?:\.\d+)?)%\s+(?P<rest>.+?)\s*$")

Runner = Callable[..., subprocess.CompletedProcess]


def not_applicable(reason: str) -> dict[str, Any]:
    return {"status": "not_applicable", "reason": reason}


def failed(reason: str) -> dict[str, Any]:
    return {"status": "error", "reason": reason}


def perf_binary() -> str | None:
    return shutil.which(os.environ.get(PERF_BIN_ENV) or "perf")


def engine_profile_gap(host_info: dict[str, Any] | None) -> str:
    """Why the engine's hit path cannot be profiled from the bench process, or "" when it can.

    The host contract's `info` says so: `profile` (bool) wins; otherwise an embedded engine runs the hit path
    in the bench process and a server engine does not."""
    if host_info is None:
        return "the engine host reports no info, so the hit path's process is unknown"
    declared = host_info.get("profile")
    if declared is False or (declared is None and not host_info.get("embedded")):
        return "the engine runs the hit path in a server process, outside the bench process the profiler attaches to"
    return ""


def parse_stat(text: str) -> dict[str, float | None]:
    """`perf stat -x,` lines (value,unit,event,...) to {event: value}; unsupported events are None."""
    counters: dict[str, float | None] = {}
    for line in text.splitlines():
        fields = line.split(",")
        if len(fields) < 3 or not fields[2].strip():
            continue
        event = fields[2].strip().split(":")[0]
        try:
            counters[event] = float(fields[0])
        except ValueError:
            counters[event] = None
    return counters


def per_query_counters(counters: dict[str, float | None], queries: int) -> dict[str, float | None]:
    """The window's counters per measured query, with the usual ratios."""
    out: dict[str, float | None] = {
        event: (counters[event] / queries if counters.get(event) is not None and queries > 0 else None)
        for event in STAT_EVENTS
    }
    cycles, instructions = out.get("cycles"), out.get("instructions")
    out["cpu_ms"] = out.pop("task-clock")  # task-clock counts milliseconds, kernel time included
    out["ipc"] = instructions / cycles if cycles and instructions else None
    branches, branch_misses = out.get("branches"), out.get("branch-misses")
    out["branch_miss_rate"] = branch_misses / branches if branches and branch_misses is not None else None
    refs, misses = out.get("cache-references"), out.get("cache-misses")
    out["cache_miss_rate"] = misses / refs if refs and misses is not None else None
    return {k: (round(v, 4) if v is not None else None) for k, v in out.items()}


def bucket_of(dso: str, symbol: str) -> str:
    """own_code: the module's functions (and templates instantiated over its types); substrate_or_backend: the
    rest of the accelerator library or binary; engine_or_system: the engine, libc, libstdc++."""
    if ACCELERATOR_DSO_MARKER not in dso and not dso.startswith("benchmark"):
        return "engine_or_system"
    return "own_code" if MODULE_NAMESPACE.search(symbol) else "substrate_or_backend"


def shorten(symbol: str, limit: int = 120) -> str:
    symbol = symbol.strip()
    return symbol if len(symbol) <= limit else symbol[: limit - 3] + "..."


def parse_report(text: str) -> dict[str, Any]:
    """`perf report --stdio --no-children --sort dso,sym` to the top rows and per-bucket totals.

    Row format: `  12.34%  dso  [.] symbol`."""
    rows: list[dict[str, Any]] = []
    buckets: dict[str, float] = {}
    for line in text.splitlines():
        match = SAMPLE_LINE_RE.match(line)
        if not match:
            continue
        dso, _, symbol = match["rest"].partition("  [")
        full_symbol = re.sub(r"^[.k]\]\s+", "", symbol).strip()
        symbol = shorten(full_symbol)
        pct = float(match["pct"])
        bucket = bucket_of(dso, symbol)
        buckets[bucket] = buckets.get(bucket, 0.0) + pct
        rows.append({"self_pct": pct, "dso": dso.strip(), "symbol": symbol, "full_symbol": full_symbol, "bucket": bucket})
    rows.sort(key=lambda r: -r["self_pct"])
    return {
        "top": rows,
        "self_pct_by_bucket": {k: round(v, 2) for k, v in sorted(buckets.items())},
    }


def function_totals(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    """Self time per function across its source lines."""
    totals: dict[str, dict[str, Any]] = {}
    for row in rows:
        entry = totals.setdefault(
            row["symbol"], {"symbol": row["symbol"], "self_pct": 0.0, "bucket": row["bucket"], "full_symbol": row["full_symbol"]})
        entry["self_pct"] = round(entry["self_pct"] + row["self_pct"], 2)
    return sorted(totals.values(), key=lambda e: -e["self_pct"])


def vectorization_of(annotate_text: str) -> dict[str, Any]:
    instructions = [ln for ln in annotate_text.splitlines() if re.search(r":\s+[a-z][a-z0-9]+\b", ln)]
    wide = [ln for ln in instructions if WIDE_VECTOR_RE.search(ln)]
    return {
        "instructions": len(instructions),
        "wide_vector_instructions": len(wide),
        "uses_wide_vectors": bool(wide),
    }


def window_query_count(hit_us: float | None, loop_seconds: float = TARGET_LOOP_SEC) -> int:
    """Measured queries in the profile window: about `loop_seconds` of the hit loop."""
    if not hit_us or hit_us <= 0:
        return 2 * MIN_QUERIES
    return max(MIN_QUERIES, min(MAX_QUERIES, int(loop_seconds * 1e6 / hit_us)))


def work_counter_report(diagnostics: dict[str, Any], estimate: dict[str, Any]) -> dict[str, Any]:
    """The visited-state count the read path reports, next to what the analysis estimated."""
    units = float(diagnostics.get("partitions_read_per_query_median") or 0.0)
    report: dict[str, Any] = {
        "state_units_visited_per_query": units,
        "state_bytes_read_per_query": float(diagnostics.get("state_bytes_read_median") or 0.0),
        "analysis_estimate": estimate,
    }
    if units <= 0:
        report["note"] = (
            "the read path reports no visited-state count: call RecordPartitionsRead(units, bytes) from "
            "the read path (accelerator_codegen::aux_state::core) so this profile can show whether the "
            "read visits too much state"
        )
    return report


def _run(runner: Runner, cmd: list[str], env: dict[str, str], cwd: Path | None) -> subprocess.CompletedProcess:
    """One perf command; a sampling one waits for the host's perf turn: perf's ring buffers come from one per-user
    locked-memory budget, and two concurrent sessions fail with "Permission error mapping pages"."""
    if "record" not in cmd:
        return runner(cmd, env=env, cwd=cwd, capture_output=True, text=True, check=False, timeout=PERF_TIMEOUT_SEC)
    turn = FairLock(host_lock_dir(), "perf").acquire(timeout_sec=PERF_TIMEOUT_SEC)
    try:
        return runner(cmd, env=env, cwd=cwd, capture_output=True, text=True, check=False, timeout=PERF_TIMEOUT_SEC)
    finally:
        turn.close()


def _make_fifo(path: Path) -> int:
    """A perf control fifo plus a held read end on its ack fifo (perf blocks opening it for writing otherwise)."""
    os.mkfifo(path)
    os.mkfifo(path.with_suffix(".ack"))
    return os.open(path.with_suffix(".ack"), os.O_RDONLY | os.O_NONBLOCK)


def profile_hit_path(
    bench_cmd: list[str],
    env: dict[str, str],
    *,
    cwd: Path | None = None,
    hit_us: float | None = None,
    diagnostics: dict[str, Any] | None = None,
    estimate: dict[str, Any] | None = None,
    runner: Runner = subprocess.run,
    loop_seconds: float = TARGET_LOOP_SEC,
    record: bool = True,
) -> dict[str, Any]:
    """Profile `bench_cmd` (the aux bench at one write-frequency point); `env` carries its parameters. The window
    is about `loop_seconds` of the measured loop (`hit_us` per operation). With record=False only the counters
    are taken (no samples, no disassembly)."""
    perf = perf_binary()
    if perf is None:
        return failed("perf is not installed on this host")
    queries = window_query_count(hit_us, loop_seconds)
    events = ",".join(STAT_EVENTS)
    with tempfile.TemporaryDirectory(prefix="accel-perf-") as tmp:
        tmpdir = Path(tmp)
        controls = [_make_fifo(tmpdir / "stat.ctl")] + ([_make_fifo(tmpdir / "record.ctl")] if record else [])
        data, stat_file = tmpdir / "perf.data", tmpdir / "window.stat"
        window = ["-D", "-1", "--control"]
        measured = [perf, "record", "-F", str(SAMPLE_HZ), "-m", str(MMAP_PAGES), "-o", str(data), *window,
                    f"fifo:{tmpdir / 'record.ctl'},{tmpdir / 'record.ack'}", "--", *bench_cmd] if record else bench_cmd
        cmd = [perf, "stat", "-x,", "-e", events, "-o", str(stat_file), *window,
               f"fifo:{tmpdir / 'stat.ctl'},{tmpdir / 'stat.ack'}", "--", *measured]
        window_env = {
            **env,
            "ACCEL_BENCH_QUERIES": str(queries),
            PROFILE_CONTROL_ENV: ",".join(str(tmpdir / f"{n}.ctl") for n in (("stat", "record") if record else ("stat",))),
        }
        try:
            run = _run(runner, cmd, window_env, cwd)
            window_stat = parse_stat(stat_file.read_text(encoding="utf-8")) if stat_file.is_file() else {}
            if not window_stat or (record and not data.is_file()):
                detail = (run.stderr or run.stdout or "").strip()[-300:]
                return failed(f"perf could not profile the bench (exit {run.returncode}): {detail}")
            sampled = _sample_report(runner, perf, data, env, cwd) if record else None
            vector = _vectorization(runner, perf, data, sampled["top"], env, cwd) if record and sampled["top"] else None
        except subprocess.TimeoutExpired:
            return failed(f"perf did not finish within {PERF_TIMEOUT_SEC}s")
        finally:
            for fd in controls:
                os.close(fd)
    if record and not sampled["top"]:
        return failed(f"no samples fell in the profile window: the bench did not open it through {PROFILE_CONTROL_ENV}")
    counters = per_query_counters(window_stat, queries)
    if not record:
        return {"status": "ok", "counters_per_query": counters}
    notes = []
    cpu_ms = counters.get("cpu_ms")
    if hit_us and cpu_ms is not None and cpu_ms * 1000.0 < 0.5 * hit_us:
        notes.append(
            f"CPU time per query ({cpu_ms * 1000.0:.0f} us) is under half the measured hit latency ({hit_us:.0f} us): "
            "the rest is waiting (locks, sleeps, thread hand-off) or kernel time, which user-space sampling does not see"
        )
    return {
        "status": "ok",
        "notes": notes,
        "sampling": {
            "frequency_hz": SAMPLE_HZ,
            "window_queries": queries,
            "note": "self time over the bench's measured loop only; rows are functions",
            "self_pct_by_bucket": sampled["self_pct_by_bucket"],
            "top_functions": [
                {k: v for k, v in row.items() if k != "full_symbol"} for row in function_totals(sampled["top"])[:TOP_N]
            ],
        },
        "counters_per_query": counters,
        "vectorization_of_hottest_own_function": vector,
        "work_counters": work_counter_report(diagnostics or {}, estimate or {}),
    }


def profile_build(bench_cmd: list[str], env: dict[str, str], *, cwd: Path | None = None,
                  runner: Runner = subprocess.run) -> dict[str, Any]:
    """Samples of a one-query run of the bench, which the state build dominates: the hot spots of the build."""
    perf = perf_binary()
    if perf is None:
        return failed("perf is not installed on this host")
    with tempfile.TemporaryDirectory(prefix="accel-perf-") as tmp:
        data = Path(tmp) / "perf.data"
        try:
            run = _run(runner, [perf, "record", "-F", str(SAMPLE_HZ), "-m", str(MMAP_PAGES), "-o", str(data), "--", *bench_cmd],
                       {**env, "ACCEL_BENCH_QUERIES": "1"}, cwd)
            sampled = _sample_report(runner, perf, data, env, cwd) if data.is_file() else {"top": []}
        except subprocess.TimeoutExpired:
            return failed(f"perf did not finish within {PERF_TIMEOUT_SEC}s")
    if not sampled["top"]:
        return failed(f"perf recorded no samples of the build (exit {run.returncode}): {(run.stderr or run.stdout or '').strip()[-300:]}")
    top = [{k: v for k, v in row.items() if k != "full_symbol"} for row in function_totals(sampled["top"])[:TOP_N]]
    return {"status": "ok", "sampling": {"self_pct_by_bucket": sampled["self_pct_by_bucket"], "top_functions": top}}


def _sample_report(runner: Runner, perf: str, data: Path, env: dict[str, str], cwd: Path | None) -> dict[str, Any]:
    report = _run(runner, [perf, "report", "-i", str(data), "--stdio", "--no-children", "--sort", "dso,sym",
                           "-F", "overhead,dso,sym"], env, cwd)
    return parse_report(report.stdout or "")


def _vectorization(
    runner: Runner, perf: str, data: Path, rows: list[dict[str, Any]], env: dict[str, str], cwd: Path | None
) -> dict[str, Any] | None:
    own = [f for f in function_totals([r for r in rows if r["bucket"] == "own_code"])]
    if not own:
        return None
    symbol = own[0]["symbol"]
    done = _run(runner, [perf, "annotate", "-i", str(data), "--stdio", "-s", own[0]["full_symbol"]], env, cwd)
    if done.returncode != 0 or not done.stdout:
        return {"symbol": symbol, "note": "no disassembly available"}
    return {"symbol": symbol, **vectorization_of(done.stdout)}


def render_for_agent(profile: dict[str, Any]) -> str:
    """The profile as the compact text the agent reads first."""
    if profile.get("status") == "error":
        return f"profile FAILED: {profile.get('reason')}"
    if profile.get("status") != "ok":
        return f"profile: not applicable ({profile.get('reason')})"
    lines = ["self time (share of samples):"]
    buckets = profile["sampling"]["self_pct_by_bucket"]
    lines.append("  by code: " + ", ".join(f"{k} {v}%" for k, v in buckets.items()))
    for row in profile["sampling"]["top_functions"]:
        lines.append(f"  {row['self_pct']:6.2f}%  [{row['bucket']}]  {row['symbol']}")
    if counters := profile.get("counters_per_query"):
        lines.append("counters per operation: " + ", ".join(f"{k}={v}" for k, v in counters.items() if v is not None))
    if vec := profile.get("vectorization_of_hottest_own_function"):
        lines.append(f"vectorization: {vec}")
    lines.extend(f"note: {note}" for note in profile.get("notes") or [])
    if work := profile.get("work_counters"):
        lines.append(f"work counters: {work}")
    return "\n".join(lines)


# Reading a sweep: the rows of an aux bench, reduced per write frequency
def bench_summary_path(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> Path:
    return query_artifacts_dir(repo_root, query, workload=workload) / "aux_bench_summary.json"


def _parse_interval_sec(row: dict[str, Any]) -> float | None:
    raw = row.get("write_interval_sec")
    if raw in (None, ""):
        return None
    try:
        return float(raw)
    except (TypeError, ValueError):
        return None


def _should_key_by_write_interval(rows: list[dict[str, Any]]) -> bool:
    """Use interval keys only when intervals actually discriminate write rates.

    Many aux benches emit ``write_interval_sec=0`` (or ``0.00``) on every row while
    still varying ``write_frequency_pct``. Keying on interval then collapses the
    whole sweep into one bucket and corrupts ``read_only_query_speedup``.
    """
    values: set[float] = set()
    for row in rows:
        interval = _parse_interval_sec(row)
        if interval is None:
            continue
        values.add(round(interval, 6))
    if len(values) <= 1:
        return False
    return any(v > 0.0 for v in values)


def summarize_sweep_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    if not rows:
        return {
            "status": "empty",
            "row_count": 0,
            "composite_score": 0.0,
            "composite_workload_speedup": 0.0,
            "read_only_query_speedup": 0.0,
            "read_only_cold_start_workload_speedup": 0.0,
            "by_write_frequency_pct": {},
        }

    by_freq: dict[str, dict[str, Any]] = {}
    workload_speedups: list[float] = []
    query_speedups: list[float] = []
    read_only_query_speedups: list[float] = []
    read_only_cold_starts: list[float] = []
    use_interval_key = _should_key_by_write_interval(rows)

    for row in rows:
        if use_interval_key:
            interval = float(_parse_interval_sec(row) or 0.0)
            key = f"{interval:g}"
            point = {
                "write_interval_sec": interval,
                "write_frequency_pct": int(float(row.get("write_frequency_pct", 0) or 0)),
            }
            read_only = interval <= 0.0
        else:
            raw_freq = row.get("write_frequency_pct", -1)
            if raw_freq in (None, ""):
                freq = -1
            else:
                try:
                    freq = int(float(raw_freq))
                except (TypeError, ValueError):
                    freq = -1
            key = str(freq)
            point = {"write_frequency_pct": freq}
            interval = _parse_interval_sec(row)
            if interval is not None:
                point["write_interval_sec"] = interval
            read_only = freq == 0
        point.update(
            {
                "suite": row.get("suite"),
                "workload_speedup": float(row.get("workload_speedup", 0.0) or 0.0),
                "cold_start_workload_speedup": float(row.get("cold_start_workload_speedup", 0.0) or 0.0),
                "query_only_speedup": float(row.get("query_only_speedup", 0.0) or 0.0),
                "blended_query_speedup": float(row.get("blended_query_speedup", 0.0) or 0.0),
                "duckdb_workload_us": float(row.get("duckdb_workload_us", 0.0) or 0.0),
                "accel_workload_us": float(row.get("accel_workload_us", 0.0) or 0.0),
                "cold_start_accel_workload_us": float(row.get("cold_start_accel_workload_us", 0.0) or 0.0),
                "setup_full_build_ms": float(row.get("setup_full_build_ms", 0.0) or 0.0),
                "aux_state_bytes": int(float(row.get("aux_state_bytes", 0) or 0)),
                "accel_hit_rate": float(row.get("accel_hit_rate", 0.0) or 0.0),
                "accel_hit_median_us": float(row.get("accel_hit_median_us", 0.0) or 0.0),
                "writes_executed": int(row.get("writes_executed", 0) or 0),
            }
        )
        for name in (*MEASUREMENT_COUNTERS, *MEASUREMENT_TIMES):
            if row.get(name) not in (None, ""):
                point[name] = float(row[name])
        for name, value in row.items():  # the reference engine's column, whichever engine it is
            if name.endswith("ref_query_median_us") and value not in (None, ""):
                point["ref_query_median_us"] = float(value)
        if row.get("measured_duration_sec") not in (None, ""):
            point["measured_duration_sec"] = float(row.get("measured_duration_sec", 0.0) or 0.0)
        if row.get("reader_qps") not in (None, ""):
            point["reader_qps"] = float(row.get("reader_qps", 0.0) or 0.0)
        for diag_key in STORE_DIAGNOSTIC_FIELDS:
            if diag_key in row and row[diag_key] not in (None, ""):
                point[diag_key] = coerce_diagnostic_field(diag_key, row[diag_key])

        # Multiple suite/locality replicates at the same write rate: keep the
        # worst-case speedups so headlines cannot overclaim.
        existing = by_freq.get(key)
        if existing is None:
            by_freq[key] = point
        else:
            merged = dict(existing)
            for speedup_key in (
                "workload_speedup",
                "query_only_speedup",
                "blended_query_speedup",
                "cold_start_workload_speedup",
            ):
                old = float(existing.get(speedup_key) or 0.0)
                new = float(point.get(speedup_key) or 0.0)
                if old > 0 and new > 0:
                    merged[speedup_key] = min(old, new)
                elif new > 0:
                    merged[speedup_key] = new
            old_hit = float(existing.get("accel_hit_median_us") or 0.0)
            new_hit = float(point.get("accel_hit_median_us") or 0.0)
            if old_hit > 0 and new_hit > 0:
                merged["accel_hit_median_us"] = max(old_hit, new_hit)
            elif new_hit > 0:
                merged["accel_hit_median_us"] = new_hit
            merged["accel_hit_rate"] = min(
                float(existing.get("accel_hit_rate") or 0.0), float(point.get("accel_hit_rate") or 0.0)
            )
            for name, worst in (("accel_fallback_reads", max), ("state_builds", min)):
                if name in existing and name in point:
                    merged[name] = worst(existing[name], point[name])
            for extra_key, value in point.items():
                if extra_key not in merged or merged[extra_key] in (None, "", 0, 0.0):
                    merged[extra_key] = value
            by_freq[key] = merged

        ws = point["workload_speedup"]
        qs = point["query_only_speedup"]
        if ws > 0:
            workload_speedups.append(ws)
        if qs > 0:
            query_speedups.append(qs)
        if read_only and qs > 0:
            read_only_query_speedups.append(qs)
        if read_only:
            cold = point["cold_start_workload_speedup"]
            if cold > 0:
                read_only_cold_starts.append(cold)

    read_only_query_speedup = min(read_only_query_speedups) if read_only_query_speedups else 0.0
    read_only_cold_start_workload_speedup = (
        min(read_only_cold_starts) if read_only_cold_starts else 0.0
    )
    composite_workload = min(workload_speedups) if workload_speedups else 0.0
    composite_query = min(query_speedups) if query_speedups else 0.0
    # Primary optimization signal: worst-case end-to-end speedup across write rates,
    # with a small bonus for strong read-only performance.
    composite_score = composite_workload + 0.1 * read_only_query_speedup

    return {
        "status": "ok",
        "row_count": len(rows),
        "composite_score": round(composite_score, 4),
        "composite_workload_speedup": round(composite_workload, 4),
        "composite_query_speedup": round(composite_query, 4),
        "read_only_query_speedup": round(read_only_query_speedup, 4),
        "read_only_cold_start_workload_speedup": round(read_only_cold_start_workload_speedup, 4),
        "by_write_frequency_pct": by_freq,
    }


def _spread(values: list[float]) -> dict[str, float]:
    return {"median": round(statistics.median(values), 4), "min": round(min(values), 4), "max": round(max(values), 4)}


def aggregate_sweep_repeats(summaries: list[dict[str, Any]], draws: list[dict[str, Any]]) -> dict[str, Any]:
    """One summary of repeated sweeps, each at its own random write frequencies (`draws`: {seed, frequencies}
    per repeat): every score is the median over the repeats, with [min, max]; a write point reports the
    median over the repeats that drew it."""
    runs = [s for s in summaries if s.get("status") == "ok"]
    if not runs:
        return summaries[0]
    out = copy.deepcopy(sorted(runs, key=lambda s: s["composite_score"])[(len(runs) - 1) // 2])  # the median repeat: its points, as measured
    for key in ("composite_score", "composite_workload_speedup", "composite_query_speedup", "read_only_query_speedup",
                "read_only_cold_start_workload_speedup"):
        values = [float(s.get(key) or 0.0) for s in runs]
        out[key] = _spread(values)["median"]
        out.setdefault("score_spread", {})[key] = _spread(values)
    points: dict[str, list[dict[str, Any]]] = {}
    for run in runs:
        for key, point in run["by_write_frequency_pct"].items():
            points.setdefault(key, []).append(point)
    for key, seen in points.items():
        point = out["by_write_frequency_pct"].setdefault(key, copy.deepcopy(seen[0]))
        speedups = [float(p["workload_speedup"]) for p in seen if float(p.get("workload_speedup") or 0) > 0]
        if speedups:
            point["workload_speedup"] = _spread(speedups)["median"]
            point["repeats"] = {"n": len(seen), "workload_speedup": _spread(speedups)}
    out["row_count"] = sum(int(s.get("row_count", 0)) for s in runs)
    out["repeats"] = [{**d, "composite_score": s["composite_score"]} for d, s in zip(draws, summaries) if s.get("status") == "ok"]
    return out


def thread_scaling(at_n_csv: Path, at_1_csv: Path, threads: int, suite: str | None = None) -> dict[str, Any]:
    """Read (and build) time of the read-only point measured at the default thread budget (`threads`) and at
    one thread, as the speedup of N threads over 1. Report-only: an output-bound read legitimately shows ~1x."""

    def point(path: Path) -> dict[str, Any]:
        rows = [r for r in parse_sweep_csv(path) if int(float(r.get("write_frequency_pct", -1) or 0)) == 0]
        if suite is not None:
            rows = [r for r in rows if r.get("suite") == suite] or rows
        return (summarize_sweep_rows(rows[:1]).get("by_write_frequency_pct") or {}).get("0", {})

    at_n, at_1 = point(at_n_csv), point(at_1_csv)
    out: dict[str, Any] = {"threads": threads}
    for metric in ("accel_hit_median_us", "accel_workload_us"):
        if at_n.get(metric, 0) > 0 and at_1.get(metric, 0) > 0:
            out.update({"read_metric": metric, "read_us_at_1": at_1[metric], "read_us_at_N": at_n[metric],
                        "read_speedup_at_N_vs_1": round(at_1[metric] / at_n[metric], 3)})
            break
    if at_n.get("setup_full_build_ms", 0) > 0 and at_1.get("setup_full_build_ms", 0) > 0:
        out.update({"build_ms_at_1": at_1["setup_full_build_ms"], "build_ms_at_N": at_n["setup_full_build_ms"],
                    "build_speedup_at_N_vs_1": round(at_1["setup_full_build_ms"] / at_n["setup_full_build_ms"], 3)})
    return out


def summarize_sweep_csv(path: Path) -> dict[str, Any]:
    summary = summarize_sweep_rows(parse_sweep_csv(path))
    summary["results_csv"] = str(path)
    return summary


# A base kernel's benchmark output
def _parse_csv_row(header: list[str], values: list[str]) -> dict[str, Any]:
    row: dict[str, Any] = {}
    for key, raw in zip(header, values):
        key = key.strip()
        if not key:
            continue
        try:
            if "." in raw:
                row[key] = float(raw)
            else:
                row[key] = int(raw)
        except ValueError:
            row[key] = raw.strip()
    return row


def _normalize_benchmark_row(row: dict[str, Any]) -> dict[str, Any]:
    if "speedup_median" not in row and "speedup" in row:
        row["speedup_median"] = row["speedup"]
    if "reference_median_us" not in row and "reference_ms" in row:
        row["reference_median_us"] = float(row["reference_ms"]) * 1000.0
    if "accelerator_median_us" not in row and "accelerator_ms" in row:
        row["accelerator_median_us"] = float(row["accelerator_ms"]) * 1000.0
    if "reference_median_us" not in row and "reference_median_ms" in row:
        row["reference_median_us"] = float(row["reference_median_ms"]) * 1000.0
    if "accelerator_median_us" not in row and "accelerator_median_ms" in row:
        row["accelerator_median_us"] = float(row["accelerator_median_ms"]) * 1000.0
    return row


def parse_benchmark_log(log_tail: str) -> dict[str, Any]:
    """Parse benchmark CSV output (baseline or profile gate log_tail)."""
    lines = [line.strip() for line in log_tail.strip().splitlines() if line.strip()]
    header: list[str] | None = None
    data_row: dict[str, Any] | None = None
    summary_row: dict[str, Any] | None = None
    profile_row: dict[str, Any] | None = None

    for line in lines:
        if line.startswith("label,"):
            header = [part.strip() for part in line.split(",")]
            continue
        if header is None:
            continue
        row = _normalize_benchmark_row(_parse_csv_row(header, line.split(",")))
        label = str(row.get("label", ""))
        if label.startswith("summary_"):
            summary_row = row
        elif "profile" in label.lower() or row.get("pre_scan_us") is not None:
            profile_row = row
        elif data_row is None:
            data_row = row

    if profile_row:
        return profile_row
    result = summary_row or data_row or {}
    return _normalize_benchmark_row(result)


def _read_gate_log_tail(art: Path, gate_name: str) -> str:
    gate_path = art / "gates" / f"{gate_name}.json"
    if not gate_path.is_file():
        return ""
    data = json.loads(gate_path.read_text(encoding="utf-8"))
    return str(data.get("log_tail", "") or "")


def write_baseline_summary(art: Path) -> dict[str, Any]:
    parsed = parse_benchmark_log(_read_gate_log_tail(art, "baseline"))
    summary = {
        "source": "baseline_gate",
        "reference_median_us": parsed.get("reference_median_us"),
        "accelerator_median_us": parsed.get("accelerator_median_us"),
        "reference_median_ms": parsed.get("reference_ms") or (
            parsed.get("reference_median_us", 0) / 1000.0 if parsed.get("reference_median_us") else None
        ),
        "accelerator_median_ms": parsed.get("accelerator_ms") or (
            parsed.get("accelerator_median_us", 0) / 1000.0 if parsed.get("accelerator_median_us") else None
        ),
        "speedup_median": parsed.get("speedup_median"),
        "speedup_mean": parsed.get("speedup_mean"),
        "raw": parsed,
    }
    (art / "baseline_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


def write_profile_summary(art: Path) -> dict[str, Any]:
    profile_path = art / "profile_summary.json"
    log_tail = ""
    if profile_path.is_file():
        data = json.loads(profile_path.read_text(encoding="utf-8"))
        log_tail = str(data.get("log_tail", "") or "")
    if not log_tail:
        log_tail = _read_gate_log_tail(art, "profile")

    parsed = parse_benchmark_log(log_tail)
    raw_phases = {
        "pre_scan_us": parsed.get("pre_scan_us"),
        "pre_next_us": parsed.get("pre_next_us"),
        "pre_consume_us": parsed.get("pre_consume_us"),
        "finalize_us": parsed.get("finalize_us"),
        "sub_scan_us": parsed.get("sub_scan_us"),
        "num_scan_us": parsed.get("num_scan_us"),
        "num_aggregate_us": parsed.get("num_aggregate_us"),
    }
    summary = {
        "source": "profile_gate",
        "speedup_median": parsed.get("speedup_median"),
        "phases_us": {k: v for k, v in raw_phases.items() if v is not None},
        "counters": {
            "qualifying_rows": parsed.get("qualifying_rows"),
            "output_groups": parsed.get("output_groups") or parsed.get("active_groups"),
            "pre_chunks": parsed.get("pre_chunks"),
            "pre_rows": parsed.get("pre_rows") or parsed.get("pre_rows_scanned"),
            "generic_rows": parsed.get("generic_rows") or parsed.get("generic_fallback_rows"),
            "flat_chunks": parsed.get("flat_chunks") or parsed.get("pre_flat"),
            "dict_chunks": parsed.get("dict_chunks") or parsed.get("pre_dict"),
            "generic_chunks": parsed.get("generic_chunks") or parsed.get("pre_generic"),
            "distinct_inserts": parsed.get("distinct_inserts"),
        },
        "raw": parsed,
    }
    (art / "profile_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    return summary


# write frequencies
# Static `write_frequencies: [0, 10]` lets an agent overfit maintenance to one mix, so the optimization bench
# also draws `write_mix_sample` points from the query's own write mix. 0% is always measured. The draw is
# locked per attempt (the samples of one attempt share a load) and re-drawn for the next.
LOCK_FILENAME = "optimization_write_frequencies.json"
_DEFAULT_WRITE_MIX_CHOICES = (1, 5, 10, 15, 20, 25, 33, 40, 50)


def _parse_freq_list(raw: Any) -> list[int]:
    if isinstance(raw, list):
        return [int(x) for x in raw]
    if isinstance(raw, str) and raw.strip():
        return [int(part.strip()) for part in raw.split(",") if part.strip()]
    return []


def _opt_sweep(workload: dict[str, Any]) -> dict[str, Any]:
    return dict(workload.get("aux_bench_sweep_optimization") or {})


def _sample_cfg(workload: dict[str, Any]) -> dict[str, Any]:
    return dict(_opt_sweep(workload).get("write_mix_sample") or {})


def base_write_frequencies(workload: dict[str, Any]) -> list[int]:
    """Configured static frequencies (optimization override, else fair sweep, else default)."""
    raw = _opt_sweep(workload).get("write_frequencies")
    if raw is None:
        raw = (workload.get("aux_bench_sweep") or {}).get("write_frequencies")
    return list(dict.fromkeys(_parse_freq_list(raw) or [0, 1, 10, 50]))


def sample_write_mix_enabled(workload: dict[str, Any]) -> bool:
    cfg = _sample_cfg(workload)
    return bool(cfg["enabled"]) if "enabled" in cfg else bool(cfg.get("choices") or cfg.get("count"))


def lock_per_attempt_enabled(workload: dict[str, Any]) -> bool:
    return bool(_sample_cfg(workload).get("lock_per_attempt", True))


def _suite_write_mix(workload: dict[str, Any]) -> dict[str, Any] | None:
    """The query's own write-frequency distribution (its test suite's write_mix.yaml), capped at the
    optimization bench's `max_pct`; None when the query has no suite."""
    try:
        path = suite_path(find_repo_root(), str(workload.get("benchmark_name", "tpch")), int(workload["query_number"]))
        mix = read_write_mix(path)
    except (FileNotFoundError, KeyError, ValueError):
        return None
    spec = dict(mix["frequency_pct"])
    cap = _sample_cfg(workload).get("max_pct")
    if cap is not None:
        spec["max"] = max(int(spec["min"]), min(int(spec["max"]), int(cap)))
    return {**mix, "frequency_pct": spec}


def _sample_write_mix_freqs(workload: dict[str, Any], *, already: list[int], rng: random.Random) -> list[int]:
    cfg = _sample_cfg(workload)
    count = max(0, int(cfg.get("count", 1)))
    if count <= 0:
        return []
    if mix := _suite_write_mix(workload):
        return [f for f in draw_write_frequencies(mix, count, rng.getrandbits(31))[1:] if f not in already]
    choices = list(_DEFAULT_WRITE_MIX_CHOICES) if cfg.get("choices") is None else _parse_freq_list(cfg["choices"])
    cap = int(cfg.get("max_pct") or max(choices))
    pool = [c for c in choices if 0 < c <= cap and c not in already]
    return sorted(rng.sample(pool, k=min(count, len(pool)))) if pool else []


def _merge_freqs(base: list[int], extra: list[int]) -> list[int]:
    merged = list(dict.fromkeys(int(f) for f in [*base, *extra]))
    return merged if 0 in merged else [0, *merged]


def _lock_path(art_dir: Path) -> Path:
    return art_dir / LOCK_FILENAME


def _rng_for_sample(
    workload: dict[str, Any], *, optimization_iter: int | None, attempt: int | None, rng: random.Random | None
) -> random.Random:
    """A configured `seed` (or, with none, the attempt's identity) gives distinct draws per attempt."""
    seed = _sample_cfg(workload).get("seed")
    if seed is None:
        if rng is not None:
            return rng
        if optimization_iter is None and attempt is None:
            return random.Random()
        seed = 0xA11CE
    mix = int(seed)
    if optimization_iter is not None:
        mix ^= int(optimization_iter) * 0x9E3779B1
    if attempt is not None:
        mix ^= int(attempt) * 0x85EBCA6B
    return random.Random(mix & 0xFFFFFFFF)


def read_locked_write_frequencies(
    art_dir: Path | None, *, optimization_iter: int | None = None, attempt: int | None = None
) -> list[int] | None:
    """The attempt's locked draw, None when there is none or it belongs to another iteration/attempt."""
    if art_dir is None or not _lock_path(art_dir).is_file():
        return None
    try:
        payload = json.loads(_lock_path(art_dir).read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError, TypeError):
        return None
    for key, wanted in (("optimization_iter", optimization_iter), ("attempt", attempt)):
        if wanted is None or payload.get(key) is None:
            continue
        try:
            if int(payload[key]) != int(wanted):
                return None
        except (TypeError, ValueError):
            return None
    return _parse_freq_list(payload.get("write_frequencies")) or None


def write_locked_write_frequencies(
    art_dir: Path, freqs: list[int], *, optimization_iter: int | None = None, attempt: int | None = None,
    source: str = "sampled",
) -> Path:
    art_dir.mkdir(parents=True, exist_ok=True)
    payload = {"write_frequencies": [int(f) for f in freqs], "optimization_iter": optimization_iter,
               "attempt": attempt, "source": source}
    _lock_path(art_dir).write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    return _lock_path(art_dir)


def _sample_freqs(
    workload: dict[str, Any], *, optimization_iter: int | None, attempt: int | None, rng: random.Random | None
) -> list[int]:
    base = base_write_frequencies(workload)
    if not sample_write_mix_enabled(workload):
        return _merge_freqs(base, [])
    local_rng = _rng_for_sample(workload, optimization_iter=optimization_iter, attempt=attempt, rng=rng)
    extra = _sample_write_mix_freqs(workload, already=base, rng=local_rng)
    if not extra and not any(f > 0 for f in base):  # a mix is sampled even when `count` is 0
        cfg = _sample_cfg(workload)
        forced = {**_opt_sweep(workload), "write_mix_sample": {**cfg, "count": max(1, int(cfg.get("count", 1)))}}
        extra = _sample_write_mix_freqs({**workload, "aux_bench_sweep_optimization": forced}, already=base, rng=local_rng)
    return _merge_freqs(base, extra)


def _env_int(name: str, default: int | None) -> int | None:
    try:
        return int(os.environ[name])
    except (KeyError, ValueError):
        return default


def resolve_optimization_write_frequencies(
    workload: dict[str, Any],
    *,
    art_dir: Path | None = None,
    optimization_iter: int | None = None,
    attempt: int | None = None,
    rng: random.Random | None = None,
    persist: bool = True,
) -> list[int]:
    """Frequencies of the optimization bench: 0% plus, with `write_mix_sample.enabled`, a write-mix draw
    locked per attempt under `art_dir`. The iteration and attempt default to the environment's
    (`ensure_attempt_write_frequencies` sets it), so a tool called mid-attempt reads the attempt's lock."""
    opt_iter = _env_int("ACCEL_PIPELINE_OPT_ITER", optimization_iter)
    opt_attempt = _env_int("ACCEL_PIPELINE_OPT_ATTEMPT", attempt)
    if not sample_write_mix_enabled(workload):
        return _merge_freqs(base_write_frequencies(workload), [])
    locking = lock_per_attempt_enabled(workload) and art_dir is not None
    if locking and (locked := read_locked_write_frequencies(art_dir, optimization_iter=opt_iter, attempt=opt_attempt)):
        return locked
    freqs = _sample_freqs(workload, optimization_iter=opt_iter, attempt=opt_attempt, rng=rng)
    if persist and locking:
        write_locked_write_frequencies(art_dir, freqs, optimization_iter=opt_iter, attempt=opt_attempt,
                                       source="write_mix_sample")
    return freqs


def ensure_attempt_write_frequencies(
    art_dir: Path, workload: dict[str, Any], *, optimization_iter: int, attempt: int, rng: random.Random | None = None
) -> list[int]:
    """Force (re)sample+lock for a specific optimization attempt (0 = iter baseline)."""
    os.environ["ACCEL_PIPELINE_OPT_ITER"] = str(optimization_iter)
    os.environ["ACCEL_PIPELINE_OPT_ATTEMPT"] = str(attempt)
    if sample_write_mix_enabled(workload):
        _lock_path(art_dir).unlink(missing_ok=True)
        freqs, source = _sample_freqs(workload, optimization_iter=optimization_iter, attempt=attempt, rng=rng), "write_mix_sample"
    else:
        freqs, source = _merge_freqs(base_write_frequencies(workload), []), "static"
    write_locked_write_frequencies(art_dir, freqs, optimization_iter=optimization_iter, attempt=attempt, source=source)
    return freqs


def geometric_mean(values: list[float]) -> float:
    positives = [float(v) for v in values if v is not None and float(v) > 0]
    return math.exp(sum(math.log(v) for v in positives) / len(positives)) if positives else 0.0


def _positive_float(value: Any) -> float:
    try:
        return max(float(value), 0.0)
    except (TypeError, ValueError):
        return 0.0


def write_mix_speedups_from_by_freq(by_freq: dict[str, Any] | None) -> list[tuple[int, float]]:
    """Non-zero write-frequency workload speedups as (pct, speedup)."""
    pairs = []
    for key, row in (by_freq if isinstance(by_freq, dict) else {}).items():
        try:
            pct = int(float(row.get("write_frequency_pct", key)))
        except (AttributeError, TypeError, ValueError):
            continue
        if pct > 0 and (speedup := _positive_float(row.get("workload_speedup"))) > 0:
            pairs.append((pct, speedup))
    return sorted(pairs)


def aggregate_write_mix_workload_speedup(metrics: dict[str, Any], *, prefer_direct: bool = True) -> tuple[float, list[int]]:
    """Write-mix axis of the dual-axis score: the geo-mean of the non-zero write points, as
    `(speedup, write_mix_pcts)`. A reported `write_mix_workload_speedup` stands in when there are no points."""
    by_freq = metrics.get("by_write_frequency_pct") or {}
    direct = _positive_float(metrics.get("write_mix_workload_speedup"))
    pct = metrics.get("write_mix_pct")
    if direct and prefer_direct and not by_freq:
        pcts = metrics.get("write_mix_pcts")
        return direct, [int(x) for x in pcts] if isinstance(pcts, list) and pcts else ([int(pct)] if pct else [])
    pairs = write_mix_speedups_from_by_freq(by_freq)
    if pairs:
        return geometric_mean([s for _, s in pairs]), [p for p, _ in pairs]
    if direct:
        return direct, [int(pct)] if pct else []
    for key in ("10", 10, "1", 1):
        row = by_freq.get(key) if isinstance(by_freq, dict) else None
        if isinstance(row, dict) and (value := _positive_float(row.get("workload_speedup"))) > 0:
            return value, [int(key)]
    return 0.0, []


# Can the numbers be trusted? Is the bench measuring the accelerator, and are its numbers plausible?
_SPEEDUP_KEYS = (
    "workload_speedup",
    "query_only_speedup",
    "cold_start_workload_speedup",
    "blended_query_speedup",
    "speedup_median",
)

_SLOWER_THAN_FALLBACK_FACTOR = 10.0
_READ_AXIS_TOLERANCE = (0.9, 1.1)  # the bench rounds its microsecond columns
_SOURCE_SUFFIXES = {".cpp", ".hpp", ".h", ".inc"}
_OTHER_TARGET_DIRS = {"tests", "bench", "config"}  # edits there do not relink the bench binary
_EVIDENCE_KEYS = (
    "write_frequency_pct", "num_queries", "accel_hit_rate", "accel_fallback_reads", "state_builds",
    "accel_hit_median_us", "fallback_median_us", "ref_query_median_us", "query_only_speedup",
)


def check_measurement(
    rows_or_metrics: Any,
    *,
    workload: dict,
    phase: str,
    baseline: dict | None = None,
    config: MeasurementConfig | None = None,
    bench_binary: Path | None = None,
    query_dir: Path | None = None,
) -> list[Failure]:
    """Failures (kind="measurement", non-empty diagnosis) when the signal is untrustworthy; [] when it is.

    rows_or_metrics: aux_bench.parse_sweep_csv rows, a metrics dict (by_write_frequency_pct or flat),
    or a MultiSampleBenchmark. phase: "baseline" | "attempt" | "gate".
    """
    cfg = config or MeasurementConfig()
    if not cfg.enabled:
        return []
    samples = getattr(rows_or_metrics, "samples", None) or []
    points = _points(getattr(rows_or_metrics, "median_metrics", rows_or_metrics))
    diagnosis = [
        *_hit_diagnosis(points, cfg, _points(baseline)),
        *_fallback_diagnosis(points, cfg),
        *_built_diagnosis(points, cfg),
        *_timing_diagnosis(points, cfg),
        *_spread_diagnosis([s.score for s in samples], cfg),
        *_stale_diagnosis(bench_binary, query_dir),
    ]
    if not diagnosis:
        return []
    return [
        Failure(
            kind="measurement",
            stage="optimization" if phase in ("baseline", "attempt") else phase,
            source=f"bench_{phase}",
            summary=f"{workload.get('name') or 'workload'}: the benchmark signal is not trustworthy",
            evidence="\n".join(_describe(p) for p in points),
            diagnosis=diagnosis,
        )
    ]


def _points(data: Any) -> list[dict[str, Any]]:
    if isinstance(data, list):
        data = summarize_sweep_rows(data)
    if not isinstance(data, dict) or not data:
        return []
    by_freq = data.get("by_write_frequency_pct")
    if isinstance(by_freq, dict) and by_freq:
        return [{"write_frequency_pct": key, **point} for key, point in by_freq.items()]
    return [data]


def _num(point: dict[str, Any], key: str) -> float | None:
    try:
        return float(point[key])
    except (KeyError, TypeError, ValueError):
        return None


def _read_only(point: dict[str, Any]) -> bool:
    interval = _num(point, "write_interval_sec")
    if interval is not None and interval > 0:
        return False
    return not _num(point, "write_frequency_pct")


def _read_only_points(points: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return [p for p in points if _read_only(p)]


def _label(point: dict[str, Any]) -> str:
    return f"write frequency {int(_num(point, 'write_frequency_pct') or 0)}%"


def _hit_diagnosis(
    points: list[dict[str, Any]], cfg: MeasurementConfig, baseline_points: list[dict[str, Any]]
) -> list[str]:
    out = []
    for point in _read_only_points(points):
        rate = _num(point, "accel_hit_rate")
        if rate is None or rate > cfg.min_hit_rate:
            continue
        if rate == 0:
            reads = int(_num(point, "num_queries") or 0) or "the measured"
            out.append(
                f"bench measured the fallback path: 0 of {reads} reads hit the accelerator. "
                "Likely causes: query not registered / first-read build not triggered / "
                "harness uses a different SQL entry"
            )
        else:
            out.append(
                f"accel_hit_rate {rate:.3f} at write frequency 0 is not above {cfg.min_hit_rate}: "
                "most reads were answered by the fallback, so the speedup does not describe the accelerator"
            )
        was = [r for p in _read_only_points(baseline_points) if (r := _num(p, "accel_hit_rate")) is not None]
        if was and min(was) > cfg.min_hit_rate:
            out.append(f"the baseline hit rate was {min(was):.3f}: this attempt broke the accelerated path")
    return out


def _fallback_diagnosis(points: list[dict[str, Any]], cfg: MeasurementConfig) -> list[str]:
    return [
        f"{int(fallbacks)} fallback reads at write frequency 0 (allowed {cfg.max_fallbacks_on_hit}): "
        "the reads were not all served from the state"
        for point in _read_only_points(points)
        if (fallbacks := _num(point, "accel_fallback_reads")) is not None
        and fallbacks > cfg.max_fallbacks_on_hit
    ]


def _built_diagnosis(points: list[dict[str, Any]], cfg: MeasurementConfig) -> list[str]:
    if not cfg.require_state_built:
        return []
    return [
        "no state was built (state_builds=0): reads cannot have come from an accelerator state"
        for point in _read_only_points(points)
        if _num(point, "state_builds") == 0
    ]


def _is_engine_read_axis(reported: float, ref_us: float, hit_us: float) -> bool:
    low, high = _READ_AXIS_TOLERANCE
    return low <= reported * hit_us / ref_us <= high


def _timing_diagnosis(points: list[dict[str, Any]], cfg: MeasurementConfig) -> list[str]:
    out = []
    for point in points:
        label = _label(point)
        for key in _SPEEDUP_KEYS:
            value = _num(point, key)
            if value is not None and not math.isfinite(value):
                out.append(f"{label}: {key} is {value}")
        hit_us = _num(point, "accel_hit_median_us")
        rate = _num(point, "accel_hit_rate")
        if hit_us is not None and hit_us <= 0 and (rate is None or rate > 0):
            out.append(f"{label}: accelerator hit median is {hit_us:g} us")
        ref_us = _num(point, "ref_query_median_us")
        if ref_us and not cfg.min_ref_us <= ref_us <= cfg.max_ref_s * 1e6:
            out.append(
                f"{label}: reference query median {ref_us:g} us is outside "
                f"[{cfg.min_ref_us:g} us, {cfg.max_ref_s:g} s]"
            )
        reported = _num(point, "query_only_speedup")
        if ref_us and hit_us and reported and not _is_engine_read_axis(reported, ref_us, hit_us):
            out.append(
                f"{label}: query_only_speedup {reported:g} is not the engine reference {ref_us:g} us over "
                f"the accelerator hit {hit_us:g} us: the read axis must be measured against the engine's own SQL"
            )
        fallback_us = _num(point, "fallback_median_us") or ref_us
        if hit_us and fallback_us and hit_us > _SLOWER_THAN_FALLBACK_FACTOR * fallback_us:
            out.append(
                f"{label}: accelerated read {hit_us:g} us is more than "
                f"{_SLOWER_THAN_FALLBACK_FACTOR:g}x the fallback {fallback_us:g} us"
            )
    return out


def _spread_diagnosis(scores: list[float], cfg: MeasurementConfig) -> list[str]:
    mean = statistics.fmean(scores) if scores else 0.0
    if len(scores) < 2 or mean <= 0:
        return []
    cv = statistics.pstdev(scores) / mean
    if cv < cfg.max_cv:
        return []
    return [f"sample scores vary too much (CV {cv:.2f} >= {cfg.max_cv}): {', '.join(f'{s:.3g}' for s in scores)}"]


def _stale_diagnosis(bench_binary: Path | None, query_dir: Path | None) -> list[str]:
    if bench_binary is None or query_dir is None:
        return []
    if not bench_binary.is_file():
        return [f"bench binary {bench_binary} does not exist"]
    sources = [
        p
        for p in query_dir.rglob("*")
        if p.suffix in _SOURCE_SUFFIXES and p.is_file() and not _OTHER_TARGET_DIRS.intersection(p.parts)
    ]
    newest = max(sources, key=lambda p: p.stat().st_mtime, default=None)
    if newest and newest.stat().st_mtime > bench_binary.stat().st_mtime:
        return [f"bench binary is older than {newest}: it measures a stale build"]
    return []


def _describe(point: dict[str, Any]) -> str:
    shown = {k: _num(point, k) for k in _EVIDENCE_KEYS}
    return ", ".join(f"{k}={v:g}" for k, v in shown.items() if v is not None)


# Scores and the verdict
def extract_store_diagnostics(metrics: dict[str, Any] | None) -> dict[str, Any]:
    """Pull factual store/execution measurements from benchmark metrics."""
    if not metrics:
        return {}
    nested = metrics.get("store_diagnostics")
    if isinstance(nested, dict) and nested:
        return {k: nested.get(k) for k in STORE_DIAGNOSTIC_FIELDS if nested.get(k) is not None}

    out: dict[str, Any] = {}
    for key in STORE_DIAGNOSTIC_FIELDS:
        if key in metrics and metrics[key] is not None:
            coerced = coerce_diagnostic_field(key, metrics[key])
            if coerced is not None:
                out[key] = coerced
    return out


def attach_store_diagnostics(metrics: dict[str, Any]) -> dict[str, Any]:
    """Ensure metrics dict carries a normalized store_diagnostics block."""
    block = extract_store_diagnostics(metrics)
    if not block:
        return metrics
    merged = dict(metrics)
    merged["store_diagnostics"] = block
    for key, value in block.items():
        merged.setdefault(key, value)
    return merged


# Suggested phase names for agent instrumentation (not enforced).
RECOMMENDED_PHASES: tuple[str, ...] = ("setup", "load", "scan", "filter", "aggregate", "count", "sort", "materialize", "emit", "finalize")

# Work-volume keys commonly useful across queries.
WORK_COUNT_KEYS: tuple[str, ...] = ("rows", "emitted_rows", "scanned_rows", "groups", "blocks", "partitions", "keys", "items",
                                    "bytes", "candidates", "hits", "misses")


def organize_diagnostics_evidence(diagnostics: dict[str, Any]) -> dict[str, Any]:
    """Bucket measured facts for context organization — no fix recommendations."""
    diag = extract_store_diagnostics(diagnostics)
    if not diag:
        return {}
    buckets = {
        "partition_store_cost": ("aux_state_bytes", "valid_partitions", "total_slots", "validity_slots_scanned_median",
                                 "validity_scan_us_median"),
        "read_path_cost": ("partitions_read_per_query_median", "accel_hit_median_us", "accel_hit_rate",
                           "state_bytes_read_median"),
        "execution_volume": ("store_execute_calls",),
    }
    return {name: {k: diag[k] for k in keys if k in diag} for name, keys in buckets.items()}


def as_float(value: Any) -> float | None:
    if value is None or value == "":
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None


def primary_score_from_metrics(
    metrics: dict[str, Any] | None,
    *,
    is_aux: bool,
    workload: dict[str, Any] | None = None,
) -> float:
    if not metrics:
        return 0.0
    if is_aux:
        # Prefer dual-axis formula (read + write-mix). Fall back through
        # aux_optimization_score which handles missing write-mix / hit-time.

        scored = aux_optimization_score(metrics, workload)
        if scored > 0:
            return scored
        for key in (
            "optimization_score",
            "read_only_query_speedup",
            "speedup_median",
            "composite_query_speedup",
        ):
            val = as_float(metrics.get(key))
            if val is not None and val > 0:
                return val
        return 0.0
    for key in ("speedup_median", "optimization_score"):
        val = as_float(metrics.get(key))
        if val is not None and val > 0:
            return val
    return 0.0


def latency_from_metrics(metrics: dict[str, Any] | None, *, is_aux: bool) -> float | None:
    if not metrics:
        return None
    if is_aux:
        for key in ("accel_hit_median_us", "accelerator_median_us"):
            val = as_float(metrics.get(key))
            if val is not None and val > 0:
                return val
        return None
    for key in ("accelerator_median_us", "accel_hit_median_us"):
        val = as_float(metrics.get(key))
        if val is not None and val > 0:
            return val
    return None


def read_axes(metrics: dict[str, Any] | None) -> dict[str, float | None]:
    """The read axis on its own (the write axis never masks it): the engine's own SQL for the same
    binding over the accelerator hit, wall clock, as the bench reports it."""
    value = as_float((metrics or {}).get("read_only_query_speedup"))
    return {"vs_engine": value if value and value > 0 else None}


def aggregate_custom_profile_events(events: list[dict[str, Any]]) -> dict[str, Any]:
    """Aggregate ACCEL_CUSTOM_PROFILE events into phase timers + work counters.

    Accepted event shapes (generic):
      {"phase":"sort","us":12.3,"emitted_rows":180}
      {"phase":"aggregate","aggregate_us":40.0,"groups":12}
      {"sort_us":12.3,"rows":100}   # phase inferred from *_us key
    """
    phase_samples: dict[str, list[float]] = {}
    phase_counts: dict[str, dict[str, list[float]]] = {}
    global_counts: dict[str, list[float]] = {}

    for event in events:
        if not isinstance(event, dict):
            continue
        phase = str(event.get("phase") or event.get("name") or "").strip()
        us = as_float(event.get("us"))
        if us is None:
            # Infer from first *_us key.
            for key, value in event.items():
                if str(key).endswith("_us"):
                    us = as_float(value)
                    if not phase:
                        phase = str(key)[: -len("_us")]
                    break
        if not phase:
            phase = "unnamed"
        if us is not None and us >= 0:
            phase_samples.setdefault(phase, []).append(us)

        for key in WORK_COUNT_KEYS:
            if key not in event:
                continue
            val = as_float(event[key])
            if val is None:
                continue
            phase_counts.setdefault(phase, {}).setdefault(key, []).append(val)
            global_counts.setdefault(key, []).append(val)

    phases_out: list[dict[str, Any]] = []
    for phase, samples in phase_samples.items():
        samples_sorted = sorted(samples)
        mid = samples_sorted[len(samples_sorted) // 2]
        entry: dict[str, Any] = {
            "phase": phase,
            "n": len(samples),
            "mean_us": round(sum(samples) / len(samples), 3),
            "p50_us": round(mid, 3),
            "total_us": round(sum(samples), 3),
            "max_us": round(max(samples), 3),
        }
        counts = phase_counts.get(phase) or {}
        if counts:
            entry["counts_mean"] = {
                k: round(sum(vs) / len(vs), 3) for k, vs in counts.items() if vs
            }
        phases_out.append(entry)

    phases_out.sort(key=lambda row: float(row.get("mean_us") or 0.0), reverse=True)
    work_means = {
        k: round(sum(vs) / len(vs), 3) for k, vs in global_counts.items() if vs
    }
    dominant = phases_out[0] if phases_out else {}
    timed_mean_sum = sum(float(p.get("mean_us") or 0.0) for p in phases_out)

    return {
        "event_count": len(events),
        "phases": phases_out,
        "dominant_phase": dominant.get("phase") or "",
        "dominant_phase_mean_us": dominant.get("mean_us"),
        "timed_phases_mean_sum_us": round(timed_mean_sum, 3),
        "work_counters_mean": work_means,
        "recommended_phases": list(RECOMMENDED_PHASES),
        "recommended_count_keys": list(WORK_COUNT_KEYS),
    }
@dataclass(frozen=True)
class EvalConfig:
    improvement_threshold_pct: float = 3.0
    reject_threshold_pct: float = 2.0
    quick_benchmark_samples: int = 3
    aux_quick_benchmark_samples: int = 1


@dataclass
class BenchmarkSample:
    metrics: dict[str, Any]
    score: float


@dataclass
class MultiSampleBenchmark:
    samples: list[BenchmarkSample]
    median_score: float
    median_metrics: dict[str, Any]


@dataclass
class AttemptOutcome:
    verdict: str  # accept | keep_explore | reject_broken | reject_regress | reject_invalid_measurement
    improvement_pct: float
    baseline_score: float
    candidate_score: float
    diagnostic_vector: dict[str, Any]
    reason: str = ""
    measurement_failures: list[Failure] = field(default_factory=list)

STRUCTURAL_DIAGNOSTIC_AXES: tuple[str, ...] = (
    "validity_slots_scanned_median",
    "validity_scan_us_median",
    "partitions_read_per_query_median",
    "state_bytes_read_median",
    "valid_partitions",
    "total_slots",
)


def _structural_progress_pct(
    baseline_diag: dict[str, Any],
    candidate_diag: dict[str, Any],
) -> float:
    """Positive when candidate reduces measured structural work on any axis."""
    improvements: list[float] = []
    lower_is_better = {
        "validity_slots_scanned_median",
        "validity_scan_us_median",
        "partitions_read_per_query_median",
        "state_bytes_read_median",
        "valid_partitions",
        "total_slots",
    }
    for key in STRUCTURAL_DIAGNOSTIC_AXES:
        base_val = baseline_diag.get(key)
        cand_val = candidate_diag.get(key)
        if not isinstance(base_val, (int, float)) or not isinstance(cand_val, (int, float)):
            continue
        if base_val <= 0:
            continue
        if key in lower_is_better:
            delta_pct = 100.0 * (base_val - cand_val) / base_val
        else:
            delta_pct = 100.0 * (cand_val - base_val) / base_val
        if delta_pct > 0:
            improvements.append(delta_pct)
    return max(improvements) if improvements else 0.0


def _structural_rejection(
    baseline_metrics: dict[str, Any], candidate_metrics: dict[str, Any], config: EvalConfig
) -> str:
    """A redesign stays only if reads improve and the write axis holds (within the reject tolerance)."""
    read_before = float(baseline_metrics.get("read_only_query_speedup") or 0.0)
    read_after = float(candidate_metrics.get("read_only_query_speedup") or 0.0)
    if read_after < read_before * (1.0 + config.improvement_threshold_pct / 100.0):
        return f"structural_read_not_improved: read speedup {read_before:.2f}x -> {read_after:.2f}x"
    write_before = write_mix_from_metrics(baseline_metrics)
    write_after = write_mix_from_metrics(candidate_metrics)
    if write_before > 0 and write_after < write_before * (1.0 - config.reject_threshold_pct / 100.0):
        return f"structural_write_regressed: write mix {write_before:.2f}x -> {write_after:.2f}x"
    return ""


def evaluate_attempt_outcome(
    *,
    baseline_score: float,
    candidate_score: float,
    baseline_metrics: dict[str, Any],
    candidate_metrics: dict[str, Any],
    config: EvalConfig,
    correctness_ok: bool,
    structural_progress_threshold_pct: float = 5.0,
    severe_regress_threshold_pct: float = 10.0,
    measurement: MeasurementConfig | None = None,
    structural: bool = False,
) -> AttemptOutcome:
    """Multi-axis verdict: accept / keep_explore / reject_broken / reject_regress, or
    reject_invalid_measurement when the benchmark did not measure the accelerator (the kernel is not judged)."""
    baseline_diag = extract_store_diagnostics(baseline_metrics)
    candidate_diag = extract_store_diagnostics(candidate_metrics)
    diagnostic_vector = {
        "baseline": baseline_diag,
        "candidate": candidate_diag,
        "read_only_query_speedup_baseline": baseline_metrics.get("read_only_query_speedup"),
        "read_only_query_speedup_candidate": candidate_metrics.get("read_only_query_speedup"),
        "write_mix_workload_speedup_baseline": baseline_metrics.get(
            "write_mix_workload_speedup"
        ),
        "write_mix_workload_speedup_candidate": candidate_metrics.get(
            "write_mix_workload_speedup"
        ),
        "accel_hit_median_us_baseline": baseline_metrics.get("accel_hit_median_us"),
        "accel_hit_median_us_candidate": candidate_metrics.get("accel_hit_median_us"),
        "accel_hit_rate_candidate": candidate_metrics.get("accel_hit_rate"),
    }
    improvement_pct = improvement_ratio(baseline_score, candidate_score)

    if not correctness_ok:
        return AttemptOutcome(
            verdict="reject_broken",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason="correctness_probe_failed",
        )

    invalid = check_measurement(
        candidate_metrics, workload={}, phase="attempt", baseline=baseline_metrics, config=measurement
    )
    if invalid:
        return AttemptOutcome(
            verdict="reject_invalid_measurement",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason="; ".join(invalid[0].diagnosis),
            measurement_failures=invalid,
        )

    if faster_build := build_win(baseline_metrics, candidate_metrics, improvement_pct, config):
        return AttemptOutcome(
            verdict="accept",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason=faster_build,
        )

    if structural:
        rejection = _structural_rejection(baseline_metrics, candidate_metrics, config)
        if rejection:
            return AttemptOutcome(
                verdict="reject_regress",
                improvement_pct=improvement_pct,
                baseline_score=baseline_score,
                candidate_score=candidate_score,
                diagnostic_vector=diagnostic_vector,
                reason=rejection,
            )

    if improvement_pct >= config.improvement_threshold_pct:
        return AttemptOutcome(
            verdict="accept",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason="score_improvement_above_accept_threshold",
        )

    if improvement_pct < -severe_regress_threshold_pct:
        return AttemptOutcome(
            verdict="reject_regress",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason="dual_axis_score_severely_regressed",
        )

    structural_pct = _structural_progress_pct(baseline_diag, candidate_diag)
    if not structural and structural_pct >= structural_progress_threshold_pct and improvement_pct >= -config.reject_threshold_pct:
        return AttemptOutcome(
            verdict="keep_explore",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason=f"structural_progress {structural_pct:.1f}% with correctness held",
        )

    if improvement_pct < config.reject_threshold_pct:
        return AttemptOutcome(
            verdict="reject_regress",
            improvement_pct=improvement_pct,
            baseline_score=baseline_score,
            candidate_score=candidate_score,
            diagnostic_vector=diagnostic_vector,
            reason="below_reject_threshold_without_structural_progress",
        )

    return AttemptOutcome(
        verdict="reject_regress",
        improvement_pct=improvement_pct,
        baseline_score=baseline_score,
        candidate_score=candidate_score,
        diagnostic_vector=diagnostic_vector,
        reason="no_accept_or_keep_explore_criteria_met",
    )


def is_aux_workload(workload: dict[str, Any]) -> bool:
    return workload.get("workload_kind") == "aux_state"

BUILD_WIN_PCT = 10.0  # a state build this much faster is kept at an equal score: at SF3000 it is hours of build


def _build_ms(metrics: dict[str, Any]) -> float:
    point = (metrics.get("by_write_frequency_pct") or {}).get("0") or {}
    return float(point.get("setup_full_build_ms") or metrics.get("setup_full_build_ms") or 0.0)


def build_win(baseline_metrics: dict[str, Any], candidate_metrics: dict[str, Any], improvement_pct: float,
              config: EvalConfig) -> str:
    """Why the candidate is kept for a faster state build ("" if it is not): the build is BUILD_WIN_PCT faster
    while the score and the write axis stay within the reject threshold (the score has no build term)."""
    before, after = _build_ms(baseline_metrics), _build_ms(candidate_metrics)
    if not before or not after or (before - after) / before * 100.0 < BUILD_WIN_PCT:
        return ""
    if improvement_pct < -config.reject_threshold_pct:
        return ""
    write_before, write_after = write_mix_from_metrics(baseline_metrics), write_mix_from_metrics(candidate_metrics)
    if write_before > 0 and write_after < write_before * (1.0 - config.reject_threshold_pct / 100.0):
        return ""
    return f"state build {before / 1e3:.1f} s -> {after / 1e3:.1f} s ({(before - after) / before:.0%} faster) at an equal score"


def write_mix_from_metrics(metrics: dict[str, Any]) -> float:
    """Write-mix axis: geo-mean of all non-zero write-frequency workload speedups."""

    value, _pcts = aggregate_write_mix_workload_speedup(metrics)
    return value


def aux_optimization_score(
    metrics: dict[str, Any],
    workload: dict[str, Any] | None = None,
) -> float:
    """Aux accept/reject score: dual-axis blend of read-only and write-mix speedups.

    Default is a weighted geometric mean:
      score = read_only ** read_w * write_mix ** write_w
    with read_w=0.75, write_w=0.25 (read weighs more). When write-mix is missing
    (read-only-only sweep), falls back to read_only alone. Hit-time inverse is
    used only when no positive read_only is available.
    """
    opt = (workload or {}).get("aux_bench_sweep_optimization") or {}
    metric = str(
        opt.get("optimization_score_metric")
        or (workload or {}).get("optimization_score_metric")
        or "dual_axis_weighted"
    )

    read_only = 0.0
    raw_read = metrics.get("read_only_query_speedup")
    if raw_read is not None:
        try:
            read_only = float(raw_read)
        except (TypeError, ValueError):
            read_only = 0.0

    if metric == "read_only_query_speedup":
        if read_only > 0:
            return read_only
        for key in ("composite_query_speedup", "composite_score"):
            raw = metrics.get(key)
            if raw is not None and float(raw) > 0:
                return float(raw)
        hit_us = float(metrics.get("accel_hit_median_us") or 0.0)
        if hit_us > 0:
            return 1.0e9 / hit_us
        return float(metrics.get("speedup_median") or 0.0)

    write_mix = write_mix_from_metrics(metrics)
    read_w = float(opt.get("read_score_weight", 0.75))
    write_w = float(opt.get("write_score_weight", 0.25))
    weight_sum = read_w + write_w
    if weight_sum <= 0:
        read_w, write_w = 0.75, 0.25
        weight_sum = 1.0
    read_w /= weight_sum
    write_w /= weight_sum

    if read_only <= 0:
        hit_us = float(metrics.get("accel_hit_median_us") or 0.0)
        if hit_us > 0:
            read_only = 1.0e9 / hit_us
        else:
            read_only = float(metrics.get("speedup_median") or 0.0)
    if read_only <= 0:
        return 0.0
    if write_mix <= 0:
        # No write-mix point in this measurement — score on read axis only.
        return read_only

    # Floor tiny write mixes so a near-zero cliff still participates without
    # hard-zeroing the geometric mean on floating noise.
    write_floor = float(opt.get("write_score_floor", 1.0e-3))
    write_mix = max(write_mix, write_floor)
    return float(read_only**read_w) * float(write_mix**write_w)


def score_metrics(
    metrics: dict[str, Any],
    *,
    is_aux: bool,
    workload: dict[str, Any] | None = None,
) -> float:
    if is_aux:
        return aux_optimization_score(metrics, workload)
    return float(metrics.get("speedup_median") or 0.0)


def improvement_ratio(baseline_score: float, candidate_score: float) -> float:
    if baseline_score <= 0:
        return 100.0 if candidate_score > 0 else 0.0
    return 100.0 * (candidate_score - baseline_score) / baseline_score


# Running the bench
QUICK_TIER_DEFAULTS = {
    "write_frequencies": [0],
    "bench_warmup": 1,
    "bench_queries": 5,
    "bench_ref_samples": 1,
    "bench_base_write_samples": 1,
    "bench_timeout_sec": 120,
}
_OPTIMIZATION_TIER_DEFAULTS = {
    "bench_warmup": 1, "bench_queries": 3, "bench_ref_samples": 2, "bench_base_write_samples": 1,
}


def _quick_tier_settings(workload: dict[str, Any]) -> dict[str, Any]:
    """aux_bench_sweep_quick: the repetitions of an agent's direction-only bench call."""
    return {**QUICK_TIER_DEFAULTS, **(workload.get("aux_bench_sweep_quick") or {})}


def _aux_write_frequencies(
    workload: dict[str, Any],
    *,
    art_dir: Path | None = None,
    optimization_iter: int | None = None,
    attempt: int | None = None,
    acceptance: bool = True,
) -> list[int]:
    """The quick tier measures the read axis alone; the optimization tier adds the attempt's write-mix draw."""
    if not acceptance:
        return [int(f) for f in _quick_tier_settings(workload)["write_frequencies"]]
    return resolve_optimization_write_frequencies(
        workload, art_dir=art_dir, optimization_iter=optimization_iter, attempt=attempt, persist=art_dir is not None
    )


def database_env(paths: dict[str, str]) -> dict[str, str]:
    """The worker's private copy of the dataset, under the host contract's ACCEL_DB (what the workload harnesses
    read when the bench runs outside host.sh) and the workload's own variable."""
    return {"ACCEL_DB": str(paths["dataset_db"]), paths["db_env_var"]: str(paths["dataset_db"])}


def _aux_bench_env(
    paths: dict[str, str], *, query: int, workload: dict[str, Any], freqs: list[int], acceptance: bool = True,
    include_profile: bool = False,
) -> dict[str, str]:
    tier = (
        {**_OPTIMIZATION_TIER_DEFAULTS, **(workload.get("aux_bench_sweep_optimization") or {})}
        if acceptance else _quick_tier_settings(workload)
    )
    env = {
        **database_env(paths),
        f"ACCEL_Q{query}_BENCH_SUITE": paths["aux_bench_suites"],
        "ACCEL_AUX_WRITE_FREQUENCIES": ",".join(str(f) for f in freqs),
        "ACCEL_BENCH_WARMUP": str(int(tier["bench_warmup"])),
        "ACCEL_BENCH_QUERIES": str(int(tier.get("quick_bench_queries", tier["bench_queries"]))),
        "ACCEL_BENCH_REF_SAMPLES": str(int(tier["bench_ref_samples"])),
        "ACCEL_BENCH_BASE_WRITE_SAMPLES": str(int(tier["bench_base_write_samples"])),
        "ACCEL_BENCH_SKIP_PARTITION_REBUILD_CALIB": "1",
    }
    if tier.get("skip_duckdb_ref"):
        env["ACCEL_BENCH_SKIP_DUCKDB_REF"] = "1"
    if include_profile:
        env["ACCEL_AUX_CUSTOM_PROFILE"] = "1"  # enables the agent's optional instrumentation
    return env


def _base_benchmark_env(paths: dict[str, str], *, query: int, include_profile: bool) -> dict[str, str]:
    env = {
        **database_env(paths),
        "ACCEL_BENCH_WARMUP": "1",
        "ACCEL_BENCH_ITERS": "3",
        "ACCEL_BENCH_FILTER": f"seed_1_q{query}",
    }
    if include_profile:
        env["ACCEL_BENCH_PROFILE"] = "1"
    return env


def _benchmark_target(paths: dict[str, str], workload: dict[str, Any]) -> str:
    return paths["aux_bench_target" if is_aux_workload(workload) else "bench_target"]


def _log_benchmark(msg: str) -> None:
    print(f"[pipeline] {msg}", file=sys.stderr, flush=True)


def _build_benchmark(pipeline_config: PipelineConfig, target: str) -> str | None:
    """Build the bench target; the build output when it fails."""
    _log_benchmark(f"building benchmark target {target}")
    with build_lock(pipeline_config.build_dir):
        build = subprocess.run(
            ["cmake", "--build", str(pipeline_config.build_dir), "--target", target, "-j", str(build_jobs())],
            cwd=pipeline_config.repo_root, capture_output=True, text=True, check=False,
        )
    if build.returncode != 0:
        return f"build failed\n{(build.stdout or '') + (build.stderr or '')[-2000:]}"
    return None


def _run_bench(cmd: list[str], *, acceptance: bool, reason: str = "optimization benchmark", **kwargs):
    """Acceptance runs are measurements: they queue for the measurement lease and run on its CPUs. The quick
    tier (direction only) runs at once, on the worker's own CPUs."""
    if acceptance:
        return run_measured(cmd, reason=reason, **kwargs)
    return subprocess.run(cmd, **kwargs)


def _sweep_csv(outputs: list[str]) -> str:
    """One CSV from per-point bench outputs: the header once, then each run's data rows (as the sweep gate keeps them)."""
    lines: list[str] = []
    for out in outputs:
        for line in out.splitlines():
            if line.startswith("suite,"):
                if not lines:
                    lines.append(line)
            elif line.count(",") >= 9 and not line.startswith(("speedup_gate", "#")):
                lines.append(line)
    return "\n".join(lines)


def _run_aux_sweep(
    bin_path: Path, *, repo: Path, query: int, env: dict[str, str], freqs: list[int], acceptance: bool,
    reason: str, timeout: float | None,
) -> tuple[list[dict[str, Any]] | None, str]:
    """The aux bench at each write frequency, one bench run per point (bench_sweep_contract.md); the CSV rows,
    or None with what went wrong. A pathological write mix (or lock contention under pack fan-out) must not
    stall the worker, hence the timeout."""
    env = {k: v for k, v in env.items() if k != f"ACCEL_Q{query}_WRITE_FREQUENCY_PCT"}
    outputs: list[str] = []
    for pct in freqs:
        try:
            proc = _run_bench([str(bin_path), "--query", str(query), str(pct)], acceptance=acceptance, reason=reason,
                              cwd=repo, capture_output=True, text=True, env=env, check=False, timeout=timeout)
        except subprocess.TimeoutExpired as exc:
            partial = "".join(x for x in (exc.stdout, exc.stderr) if isinstance(x, str))
            return None, f"benchmark timed out after {timeout:.0f}s (write {pct}%)\n{partial[-2000:]}"
        point = (proc.stdout or "") + (proc.stderr or "")
        if proc.returncode in (-9, 137):
            hint = "benchmark subprocess killed (SIGKILL); likely OOM from a bad patch or stale binary after a reverted patch"
            return None, hint + (f"\n{point[-2000:]}" if point.strip() else "")
        if proc.returncode != 0:
            return None, point or f"benchmark exited with code {proc.returncode} (write {pct}%)"
        outputs.append(point)
    output = "\n".join(outputs)
    rows = parse_aux_benchmark_output(_sweep_csv(outputs))
    if len(rows) < len(freqs):
        return None, f"expected {len(freqs)} sweep rows, got {len(rows)}\n{output[-4000:]}"
    return rows, output


def aux_metrics(rows: list[dict[str, Any]]) -> dict[str, Any]:
    """The metrics of one aux sweep: the read axis, the write-mix axis, the dual-axis score, the store diagnostics."""
    summary = summarize_sweep_rows(rows)
    by_freq = summary.get("by_write_frequency_pct") or {}
    zero_row = by_freq.get("0") or (rows[0] if rows else {})
    composite_wl = float(summary.get("composite_workload_speedup") or 0.0)
    read_only = float(summary.get("read_only_query_speedup") or 0.0)
    hit_us = float(zero_row.get("accel_hit_median_us") or rows[0].get("accel_hit_median_us", 0.0) if rows else 0.0)
    write_mix, write_mix_pcts = aggregate_write_mix_workload_speedup({"by_write_frequency_pct": by_freq})
    metrics = {
        "mode": "aux_quick_sweep",
        "write_frequency_points": len(rows),
        "composite_score": float(summary.get("composite_score") or 0.0),
        "composite_workload_speedup": composite_wl,
        "composite_query_speedup": float(summary.get("composite_query_speedup") or read_only),
        "read_only_query_speedup": read_only,
        "read_only_cold_start_workload_speedup": float(summary.get("read_only_cold_start_workload_speedup") or 0.0),
        "write_mix_pct": write_mix_pcts[0] if len(write_mix_pcts) == 1 else 0,
        "write_mix_pcts": write_mix_pcts,
        "write_mix_workload_speedup": write_mix,
        "by_write_frequency_pct": by_freq,
        "workload_speedup": composite_wl,
        "query_only_speedup": read_only,
        "cold_start_workload_speedup": float(zero_row.get("cold_start_workload_speedup") or 0.0),
        "accel_hit_median_us": hit_us,
        "accel_hit_rate": float(zero_row.get("accel_hit_rate") or 0.0),
        "setup_full_build_ms": float(zero_row.get("setup_full_build_ms") or 0.0),
        "reference_median_us": float(zero_row.get("ref_query_median_us") or 0.0),
        "accelerator_median_us": hit_us,
        "aux_summary": summary,
    }
    dual = aux_optimization_score(metrics)
    metrics["optimization_score"] = metrics["speedup_median"] = dual if dual > 0 else read_only
    metrics.update({k: zero_row[k] for k in STORE_DIAGNOSTIC_FIELDS if k in zero_row})
    return attach_store_diagnostics(metrics)


def _optimization_signal(metrics: dict[str, Any]) -> str | None:
    """What the aux metrics say to attack next (the agent's A/B call hands this back)."""
    read_only, write_mix = metrics["read_only_query_speedup"], metrics["write_mix_workload_speedup"]
    pcts = metrics["write_mix_pcts"]
    label = f"{pcts[0]}%" if len(pcts) == 1 else ("+".join(f"{p}%" for p in pcts) if pcts else "write-mix")
    if pcts and 0 < write_mix < 1.0:
        return (f"{label} write-mix workload_speedup={write_mix:.2f}x (below 1x vs DuckDB) — incremental maintenance "
                "is too slow; prefer delta/partition-local apply over full rebuilds on point writes. Dual-axis "
                "accept score penalizes this cliff.")
    if pcts and write_mix > 0 and read_only > 10.0 and write_mix < max(1.0, read_only * 0.02):
        return (f"Read-only is strong ({read_only:.1f}x) but {label} write-mix workload_speedup={write_mix:.2f}x is a "
                "large maintenance cliff — optimize bulk/point incremental maintenance; dual-axis score weights "
                "write_mix^0.25.")
    if read_only < 5.0:
        return ("read_only_query_speedup is weak; aggressively attack aux hit path (residual scans, map/hash chasing, "
                "decode/materialization, EmitResult hot loop).")
    if metrics["composite_workload_speedup"] < 1.0:
        return ("Keep pushing dual-axis score (read^0.75 * write_mix^0.25); watch write-mix health across sampled "
                f"frequencies ({label}).")
    return None


def _run_base_once(
    bin_path: str, *, repo: Path, paths: dict[str, str], query: int, include_profile: bool, acceptance: bool = True,
) -> tuple[dict[str, Any] | None, str]:
    proc = _run_bench(
        [bin_path], acceptance=acceptance, cwd=repo, capture_output=True, text=True, check=False,
        env={**os.environ, **_base_benchmark_env(paths, query=query, include_profile=include_profile)},
    )
    output = (proc.stdout or "") + (proc.stderr or "")
    if proc.returncode != 0:
        return None, output
    return (parse_benchmark_log(output) or None), output


def _private_paths(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> dict[str, str]:
    """The workload's paths, with the dataset db this worker alone writes."""
    paths = resolve_workload_paths(pipeline_config, workload, query)
    return {**paths, "dataset_db": str(ensure_private_dataset_db(pipeline_config, paths["dataset_db"], query=query))}


def run_single_benchmark(
    pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, *, include_profile: bool = False
) -> tuple[dict[str, Any] | None, str]:
    """Run one benchmark invocation; return (metrics, output_or_error)."""
    try:
        paths = _private_paths(pipeline_config, workload, query)
    except Exception as exc:  # noqa: BLE001
        return None, f"private dataset db setup failed: {exc}"
    target = _benchmark_target(paths, workload)
    if build_err := _build_benchmark(pipeline_config, target):
        return None, build_err
    bin_path = pipeline_config.build_dir / target
    repo = pipeline_config.repo_root
    if not is_aux_workload(workload):
        return _run_base_once(str(bin_path), repo=repo, paths=paths, query=query, include_profile=include_profile)
    freqs = _aux_write_frequencies(workload, art_dir=query_artifacts_dir(repo, query, workload=workload))
    env = {**os.environ, **_aux_bench_env(paths, query=query, workload=workload, freqs=freqs)}
    _log_benchmark(f"aux quick sweep ({len(freqs)} runs): freqs={freqs} queries={env.get('ACCEL_BENCH_QUERIES')} "
                   f"warmup={env.get('ACCEL_BENCH_WARMUP')}")
    timeout = int(_opt_sweep(workload).get("bench_timeout_sec") or os.environ.get("ACCEL_BENCH_TIMEOUT_SEC") or 300)
    rows, output = _run_aux_sweep(bin_path, repo=repo, query=query, env=env, freqs=freqs, acceptance=True,
                                  reason="optimization benchmark", timeout=timeout)
    return (aux_metrics(rows) if rows else None), output


def _aux_sample_count(
    workload: dict[str, Any],
    config: EvalConfig,
    *,
    confirmation: bool,
) -> int:
    if confirmation:
        return 1
    opt = workload.get("aux_bench_sweep_optimization") or {}
    if opt.get("quick_benchmark_samples") is not None:
        return max(1, int(opt["quick_benchmark_samples"]))
    return max(1, config.aux_quick_benchmark_samples)


def run_multi_sample_benchmark(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    samples: int = 3,
    include_profile: bool = False,
    confirmation: bool = False,
    errors: list[str] | None = None,
) -> MultiSampleBenchmark | None:
    """`errors` collects the first failing sample's output (build or run), for the caller's diagnosis."""
    aux = is_aux_workload(workload)
    if aux:
        sample_count = _aux_sample_count(
            workload,
            eval_config_from_pipeline(pipeline_config),
            confirmation=confirmation,
        )
    else:
        sample_count = samples if not confirmation else max(1, samples)
    collected: list[BenchmarkSample] = []
    for sample_idx in range(sample_count):
        if aux:
            _log_benchmark(f"aux benchmark sample {sample_idx + 1}/{sample_count}")
        metrics, err = run_single_benchmark(
            pipeline_config,
            workload,
            query,
            include_profile=include_profile and not collected,
        )
        if not metrics:
            if not collected:
                _log_benchmark(f"benchmark failed: {(err or '')[-500:]}")
                if errors is not None:
                    errors.append(err or "")
                return None
            continue
        collected.append(
            BenchmarkSample(
                metrics=metrics,
                score=score_metrics(metrics, is_aux=aux, workload=workload),
            )
        )
    if not collected:
        return None
    scores = [s.score for s in collected]
    median_score = float(statistics.median(scores))
    median_idx = min(range(len(scores)), key=lambda i: abs(scores[i] - median_score))
    return MultiSampleBenchmark(
        samples=collected,
        median_score=median_score,
        median_metrics=collected[median_idx].metrics,
    )


def eval_config_from_pipeline(pipeline_config: PipelineConfig) -> EvalConfig:
    aux_samples = 1
    try:
        raw_opt = load_aux_state_defaults(pipeline_config.repo_root).get(
            "aux_bench_sweep_optimization", {}
        )
        if isinstance(raw_opt, dict) and raw_opt.get("quick_benchmark_samples") is not None:
            aux_samples = int(raw_opt["quick_benchmark_samples"])
    except Exception:  # noqa: BLE001
        pass
    return EvalConfig(
        improvement_threshold_pct=float(
            getattr(pipeline_config, "optimization_improvement_threshold_pct", 3.0)
        ),
        reject_threshold_pct=float(getattr(pipeline_config, "optimization_reject_threshold_pct", 2.0)),
        quick_benchmark_samples=int(
            getattr(pipeline_config, "optimization_quick_benchmark_samples", 3)
        ),
        aux_quick_benchmark_samples=aux_samples,
    )


# The aux profile summary: the metrics as a profile the digest and the tools read
CUSTOM_PROFILE_PREFIX = "ACCEL_CUSTOM_PROFILE"
_CUSTOM_LINE_RE = re.compile(
    rf"{re.escape(CUSTOM_PROFILE_PREFIX)}\s*[:=]?\s*(\{{.*\}}|\S.+)\s*$"
)


def parse_custom_profile_events(output: str) -> list[dict[str, Any]]:
    """Parse agent-emitted ``ACCEL_CUSTOM_PROFILE {...}`` lines from bench output."""
    events: list[dict[str, Any]] = []
    for line in (output or "").splitlines():
        match = _CUSTOM_LINE_RE.search(line.strip())
        if not match:
            continue
        raw = match.group(1).strip()
        try:
            payload = json.loads(raw)
            if isinstance(payload, dict):
                events.append(payload)
            else:
                events.append({"value": payload})
        except json.JSONDecodeError:
            events.append({"raw": raw})
    return events


def best_valid_metrics(best_valid: dict[str, Any]) -> dict[str, Any]:
    metrics = dict(best_valid.get("metrics") or {})
    if best_valid.get("score") is not None and "optimization_score" not in metrics:
        metrics["optimization_score"] = best_valid["score"]
    return metrics


def build_aux_profile_summary(
    metrics: dict[str, Any],
    *,
    source: str,
    custom_events: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Shape aux bench metrics into a profile_summary-compatible digest."""
    diag = extract_store_diagnostics(metrics)
    read_only = float(
        metrics.get("read_only_query_speedup")
        or metrics.get("optimization_score")
        or metrics.get("speedup_median")
        or 0.0
    )
    hit_us = float(diag.get("accel_hit_median_us") or metrics.get("accel_hit_median_us") or 0.0)
    hit_rate = float(diag.get("accel_hit_rate") or metrics.get("accel_hit_rate") or 0.0)
    validity_us = float(diag.get("validity_scan_us_median") or 0.0)
    parts_read = float(diag.get("partitions_read_per_query_median") or 0.0)
    state_bytes = float(diag.get("state_bytes_read_median") or 0.0)

    custom_agg = aggregate_custom_profile_events(list(custom_events or []))
    phases: dict[str, float] = {}
    if hit_us > 0:
        phases["accel_hit_us"] = hit_us
    if validity_us > 0:
        phases["validity_scan_us"] = validity_us
    for phase_row in custom_agg.get("phases") or []:
        name = str(phase_row.get("phase") or "")
        mean_us = phase_row.get("mean_us")
        if name and mean_us is not None:
            phases[name] = float(mean_us)

    dominant_phase = str(custom_agg.get("dominant_phase") or "")
    dominant_us = float(custom_agg.get("dominant_phase_mean_us") or 0.0)
    if not dominant_phase and phases:
        # Prefer non-hit aggregate phases when present.
        candidates = {k: v for k, v in phases.items() if k != "accel_hit_us"}
        if candidates:
            dominant_phase, dominant_us = max(candidates.items(), key=lambda kv: kv[1])
        else:
            dominant_phase, dominant_us = max(phases.items(), key=lambda kv: kv[1])

    timed_sum = float(custom_agg.get("timed_phases_mean_sum_us") or 0.0)
    residual = None
    if hit_us > 0 and timed_sum > 0:
        residual = round(hit_us - timed_sum, 3)

    signals: list[str] = []
    if read_only > 0:
        signals.append(f"read_only_query_speedup={read_only:.3f}x")
    if hit_us > 0:
        signals.append(f"accel_hit_median_us={hit_us:.1f}")
    if hit_rate > 0:
        signals.append(f"accel_hit_rate={hit_rate:.3f}")
    if parts_read > 0:
        signals.append(f"partitions_read_per_query_median={parts_read:.1f}")
    if state_bytes > 0:
        signals.append(f"state_bytes_read_median={state_bytes:.0f}")
    if dominant_phase:
        signals.append(f"dominant_custom_phase={dominant_phase}@{dominant_us:.1f}us")
    if custom_agg.get("work_counters_mean"):
        for key, value in list(custom_agg["work_counters_mean"].items())[:4]:
            signals.append(f"{key}={value}")
    if custom_events:
        signals.append(f"custom_profile_events={len(custom_events)}")
    if residual is not None:
        signals.append(f"residual_vs_hit_us={residual}")

    return {
        "workload_kind": "aux_state",
        "source": source,
        "measured_at": datetime.now(timezone.utc).isoformat(),
        "score_metric": "read_only_query_speedup",
        "read_only_query_speedup": read_only,
        "composite_score": float(metrics.get("composite_score") or 0.0),
        "accel_hit_median_us": hit_us,
        "accel_hit_rate": hit_rate,
        "setup_full_build_ms": float(metrics.get("setup_full_build_ms") or 0.0),
        "store_diagnostics": diag,
        "diagnostics_evidence": organize_diagnostics_evidence(diag),
        "profile_phases_us": phases,
        "phase_breakdown": custom_agg.get("phases") or [],
        "work_counters_mean": custom_agg.get("work_counters_mean") or {},
        "residual_vs_hit_us": residual,
        "dominant_phase": dominant_phase,
        "dominant_phase_us": dominant_us,
        "profile_signals": signals,
        "custom_profile_events": list(custom_events or [])[-40:],
        "custom_profile_aggregate": custom_agg,
        "counters": {
            "valid_partitions": diag.get("valid_partitions"),
            "total_slots": diag.get("total_slots"),
            "store_execute_calls": diag.get("store_execute_calls"),
            "partitions_read_per_query_median": diag.get("partitions_read_per_query_median"),
            "state_bytes_read_median": diag.get("state_bytes_read_median"),
            **(custom_agg.get("work_counters_mean") or {}),
        },
    }


def write_aux_profile_artifacts(
    art: Path,
    metrics: dict[str, Any],
    *,
    source: str,
    bench_output: str = "",
) -> dict[str, Any]:
    """Write profile_summary.json (+ optional custom_profile.json) for aux digests."""
    art.mkdir(parents=True, exist_ok=True)
    custom_events = parse_custom_profile_events(bench_output)
    summary = build_aux_profile_summary(metrics, source=source, custom_events=custom_events)
    (art / "profile_summary.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    if custom_events:
        (art / "custom_profile.json").write_text(
            json.dumps(
                {
                    "events": custom_events,
                    "aggregate": summary.get("custom_profile_aggregate") or {},
                    "measured_at": summary.get("measured_at"),
                    "source": source,
                },
                indent=2,
            ),
            encoding="utf-8",
        )
    baseline = {
        "workload_kind": "aux_state",
        "speedup_median": summary.get("read_only_query_speedup"),
        "read_only_query_speedup": summary.get("read_only_query_speedup"),
        "accelerator_median_us": summary.get("accel_hit_median_us"),
        "store_diagnostics": summary.get("store_diagnostics") or {},
        "source": source,
        "measured_at": summary.get("measured_at"),
    }
    (art / "baseline_summary.json").write_text(json.dumps(baseline, indent=2), encoding="utf-8")
    return summary


# the agent's A/B benchmark call
def format_quick_benchmark_result(metrics: dict[str, Any], *, aux: bool) -> str:
    if aux:
        payload: dict[str, Any] = {
            "mode": metrics.get("mode"),
            "write_frequency_points": metrics.get("write_frequency_points"),
            "composite_score": metrics.get("composite_score"),
            "composite_workload_speedup": metrics.get("composite_workload_speedup"),
            "read_only_query_speedup": metrics.get("read_only_query_speedup"),
            "read_speedup_vs_engine": metrics.get("read_only_query_speedup"),
            "read_only_cold_start_workload_speedup": metrics.get(
                "read_only_cold_start_workload_speedup"
            ),
            "setup_full_build_ms": metrics.get("setup_full_build_ms"),
            "write_mix_pct": metrics.get("write_mix_pct"),
            "write_mix_pcts": metrics.get("write_mix_pcts"),
            "write_mix_workload_speedup": metrics.get("write_mix_workload_speedup"),
            "by_write_frequency_pct": metrics.get("by_write_frequency_pct"),
            "optimization_score": metrics.get("optimization_score"),
            "speedup_median": metrics.get("speedup_median"),
            "optimization_signal": metrics.get("optimization_signal"),
        }
        # Drop null-only guidance field for cleaner JSON.
        if payload["optimization_signal"] is None:
            payload.pop("optimization_signal")
        if payload.get("optimization_score") is None:
            payload.pop("optimization_score")
        if not payload.get("write_mix_pcts"):
            payload.pop("write_mix_pcts", None)
    else:
        payload = {
            "speedup_median": metrics.get("speedup_median"),
            "reference_median_us": metrics.get("reference_median_us"),
            "accelerator_median_us": metrics.get("accelerator_median_us"),
            "pre_scan_us": metrics.get("pre_scan_us"),
            "pre_consume_us": metrics.get("pre_consume_us"),
            "finalize_us": metrics.get("finalize_us"),
            "generic_rows": metrics.get("generic_rows") or metrics.get("generic_fallback_rows"),
            "pre_rows": metrics.get("pre_rows"),
            "dict_chunks": metrics.get("dict_chunks"),
            "flat_chunks": metrics.get("flat_chunks"),
            "distinct_inserts": metrics.get("distinct_inserts"),
        }
    return json.dumps(payload, indent=2)


@dataclass
class QuickBenchmark:
    metrics: dict[str, Any]
    output: str
    invalid: list[str]  # why the numbers do not describe the accelerator ([] when they do)


def measure_quick_benchmark(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    art: Path,
    optimization_iter: int,
    attempt: int | None,
    include_profile: bool,
    acceptance: bool,
) -> QuickBenchmark | str:
    """The agent's A/B benchmark call: build, run the quick tier (or, with `acceptance`, the optimization
    tier), reduce to metrics. A string is the error to hand back to the agent."""
    try:
        paths = _private_paths(pipeline_config, workload, query)
    except Exception as exc:  # noqa: BLE001
        return f"private dataset db setup failed: {exc}"
    repo = pipeline_config.repo_root
    target = _benchmark_target(paths, workload)
    bin_path = pipeline_config.build_dir / target
    if build_error := _build_benchmark(pipeline_config, target):
        return build_error
    if not is_aux_workload(workload):
        metrics, output = _run_base_once(str(bin_path), repo=repo, paths=paths, query=query,
                                         include_profile=include_profile, acceptance=acceptance)
        return QuickBenchmark(metrics, output, []) if metrics else f"benchmark failed\n{output[-2000:]}"
    freqs = _aux_write_frequencies(workload, art_dir=art, optimization_iter=optimization_iter, attempt=attempt,
                                   acceptance=acceptance)
    env = {**os.environ, **_aux_bench_env(paths, query=query, workload=workload, freqs=freqs, acceptance=acceptance,
                                          include_profile=include_profile)}
    timeout = None if acceptance else float(_quick_tier_settings(workload)["bench_timeout_sec"])
    rows, output = _run_aux_sweep(bin_path, repo=repo, query=query, env=env, freqs=freqs, acceptance=acceptance,
                                  reason="run_quick_benchmark acceptance", timeout=timeout)
    if not rows:
        lift = "" if acceptance or "timed out" not in output else " (quick tier; acceptance=True lifts the limit)"
        return f"aux quick sweep produced no CSV rows{lift}\n{output[-4000:]}"
    metrics = aux_metrics(rows)
    metrics["optimization_signal"] = _optimization_signal(metrics)
    invalid = [
        line
        for failure in check_measurement(
            rows, workload=workload, phase="attempt", config=pipeline_config.measurement, bench_binary=bin_path,
            query_dir=repo / layout.query_dir(str(workload.get("benchmark_name", "tpch")), query),
        )
        for line in failure.diagnosis
    ]
    write_aux_profile_artifacts(art, metrics, source="run_quick_benchmark", bench_output=output)
    return QuickBenchmark(metrics, output, invalid)


def compact_summary_for_prompt(summary: dict[str, Any]) -> str:
    """A summary without its nested raw/log blob, for the optimization prompts."""
    return json.dumps({k: v for k, v in (summary or {}).items() if k != "raw"}, separators=(",", ":"), ensure_ascii=False)
