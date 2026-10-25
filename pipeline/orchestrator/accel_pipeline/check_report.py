"""What a passing quick_check reports besides correctness, as short lines, and which of it fails the check.

The suite reports of the two differential runs (N threads, then 1) carry the state build's seconds on the subset;
the bench carries the read latency and the perf counters (check_measure.py).
"""

from __future__ import annotations

import os
import re
from typing import Any

MIN_TIMED_BUILD_S = 1.0  # a subset build shorter than this is mostly fixed cost (per-partition scans, thread start):
                         # no verdict on its scaling or its full-scale projection
MIN_THREADS = 4
SCALING_FLOOR = 1.5  # the N-thread build must be at least this much faster than the one-thread build
_SERIAL_PLAN = re.compile(r"^\s*(n/a|none|serial|single)\b", re.IGNORECASE)


def gate_threads() -> int:
    """The thread cap of the gate's N-thread run: ACCEL_AUX_THREADS, else the CPUs of this process (aux_gate_common.sh)."""
    return int(os.environ.get("ACCEL_AUX_THREADS") or len(os.sched_getaffinity(0)))


def expects_scaling(analysis: dict[str, Any]) -> bool:
    """The analysis says the build splits over the thread cap (scale_plan.build_parallelism)."""
    plan = str(((analysis.get("correctness_facts") or {}).get("scale_plan") or {}).get("build_parallelism") or "")
    return bool(plan.strip()) and not _SERIAL_PLAN.match(plan)


def _not_scaling(at_n: float, at_1: float, threads: int, where: str) -> str:
    return (
        f"the state build does not scale with threads: {at_n:.2f} s at {threads} threads vs {at_1:.2f} s at 1 "
        f"({at_1 / at_n:.1f}x) {where}, but analysis.json's scale_plan.build_parallelism says it splits over the thread cap. "
        "Something in it is serial, usually one thread inserting rows one at a time into a shared structure. "
        "Build per-partition partials in core::PartitionedBuild (or the scan role's partitioned scan) and merge "
        "them; sort or bulk-load instead of inserting per row.")


def full_scale_defect(sweep_scaling: dict[str, Any]) -> str:
    """The thread_scaling of the last bench sweep (SF1 builds at N and at 1 thread) as a defect, for a subset build too
    short to judge."""
    at_n, at_1 = float(sweep_scaling.get("build_ms_at_N") or 0.0) / 1e3, float(sweep_scaling.get("build_ms_at_1") or 0.0) / 1e3
    threads = int(sweep_scaling.get("threads") or 0)
    if threads < MIN_THREADS or not at_n or at_1 / at_n >= SCALING_FLOOR:
        return ""
    return _not_scaling(at_n, at_1, threads, "in the last bench sweep on the full SF1 database")


def build_report(reports: list[dict], *, scale: float, threads: int, budget_s: float, expected_scaling: bool,
                 sweep_scaling: dict[str, Any] | None = None) -> tuple[str, str]:
    """(the build line, the defect or ""): the build's seconds at N and at 1 thread on the subset, its speedup, and the
    N-thread build projected to the full dataset (rows scale). `reports`: the suite reports of the N-thread and the
    one-thread run, the second missing when the cap is one thread. A subset build too short to time is judged on
    `sweep_scaling`, the last bench sweep's builds at full scale, and not projected."""
    if not reports or not float(reports[0].get("build_seconds") or 0.0):
        return "", ""
    at_n = float(reports[0]["build_seconds"])
    at_1 = float(reports[1]["build_seconds"]) if len(reports) > 1 else 0.0
    speedup = at_1 / at_n if at_1 else 0.0
    full_s = at_n * scale
    line = f"{at_n:.2f} s at {threads} threads" + (f", {at_1:.2f} s at 1 thread ({speedup:.1f}x)" if at_1 else "")
    if max(at_n, at_1) < MIN_TIMED_BUILD_S:
        line += " on the subset, too short to time (the exit gate times the full build)"
        return line, full_scale_defect(sweep_scaling or {}) if expected_scaling else ""
    line += f" on the subset; projected {full_s:.0f} s on the full dataset (budget {budget_s:.0f} s)"
    if expected_scaling and threads >= MIN_THREADS and at_1 >= MIN_TIMED_BUILD_S and speedup < SCALING_FLOOR:
        return line, _not_scaling(at_n, at_1, threads, "on the subset")
    if full_s > budget_s:
        return line, (
            f"the state build is too slow for the full dataset: {at_n:.2f} s on the subset at {threads} threads, "
            f"projected ~{full_s:.0f} s at full scale; the gates build it several times (budget {budget_s:.0f} s). "
            "Cut the work per row (bulk-load a sorted run instead of one insert per row, size containers up front, "
            "avoid per-row allocation) and split it over threads; `run_profile` shows the build's hot spots.")
    return line, ""


def read_line(row: dict[str, Any]) -> str:
    """The engine's SQL against the accelerator for the same bindings, medians on the subset."""
    ref, accel = float(row["engine_ref_query_median_us"]), float(row["accel_hit_median_us"])
    hits = float(row["accel_hit_rate"])
    line = f"engine SQL {ref:.0f} us, accelerator {accel:.0f} us ({ref / accel:.1f}x faster)" if accel else "no accelerated reads"
    return line + ("" if hits >= 0.999 else f"; only {hits:.0%} of the reads were answered by the accelerator, the rest fell back to the SQL")


def _si(value: float | None) -> str:
    if value is None:
        return "?"
    for unit, size in (("G", 1e9), ("M", 1e6), ("k", 1e3)):
        if value >= size:
            return f"{value / size:.1f}{unit}"
    return f"{value:.0f}"


def counters_line(counters: dict[str, Any]) -> str:
    """perf stat counters: per read, and for the whole one-query run that builds the state."""
    if "unavailable" in counters:
        return f"unavailable ({counters['unavailable']})"

    def fmt(c: dict[str, float | None]) -> str:
        return ", ".join(f"{_si(c.get(k))} {name}" for k, name in (("instructions", "instructions"), ("cycles", "cycles"),
                                                                     ("cache-misses", "cache misses")))
    return f"per read {fmt(counters['read'])}; build run {fmt(counters['build'])}"
