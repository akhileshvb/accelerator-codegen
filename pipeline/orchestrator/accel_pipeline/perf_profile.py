"""A real profile of the accelerator's hit path: where the samples fall, what the hardware did, how much state a read visits.

`run_profile` used to return only the bench's phase summary. This module runs the aux bench's read-only
hit loop under `perf` and reports

* a sampling profile (self time, top functions), folded into the module's own code versus the rest of the
  accelerator library (substrate, backend) versus the engine and system libraries;
* hardware counters per query (instructions, IPC, cache and branch misses);
* whether the hottest own function is vectorized (wide-vector instructions in its disassembly);
* the visited-state count the read path reports (`RecordPartitionsRead`), against the analysis's estimate.

Counters and samples cover only the bench's measured read loop: perf starts disabled and the bench opens the
window through the `ACCEL_PROFILE_CONTROL` fifos (`eval/bench/driver/profile_window.hpp`).

An engine whose hit path does not run inside the bench process (a server engine) is `not_applicable`, with the
reason. A machine where `perf` cannot run, or a bench that never opens the window, is an `error` the caller fails
on: a broken profiler is never read as "nothing to profile".
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Callable

PERF_BIN_ENV = "ACCEL_PERF_BIN"
SAMPLE_HZ = 10000
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
    from accel_pipeline.pack import FairLock, host_lock_dir

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
        measured = [perf, "record", "-F", str(SAMPLE_HZ), "-o", str(data), *window,
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
            run = _run(runner, [perf, "record", "-F", str(SAMPLE_HZ), "-o", str(data), "--", *bench_cmd],
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


