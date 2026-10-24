"""Parse aux-state write-frequency benchmark sweeps for optimization feedback."""

from __future__ import annotations

import copy
import csv
import json
import statistics
from pathlib import Path
from typing import Any

from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.optimization_signals import STORE_DIAGNOSTIC_FIELDS, coerce_diagnostic_field

# What a bench reports so measurement.check_measurement can tell a hit from a fallback.
MEASUREMENT_COUNTERS = ("num_queries", "accel_fallback_reads", "state_builds")
MEASUREMENT_TIMES = ("fallback_median_us",)


def bench_summary_path(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> Path:
    return query_artifacts_dir(repo_root, query, workload=workload) / "aux_bench_summary.json"


def optimization_state_path(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> Path:
    return query_artifacts_dir(repo_root, query, workload=workload) / "optimization_state.json"


def _normalize_sweep_row_keys(row: dict[str, str]) -> dict[str, str]:
    """Accept legacy benchmark CSV headers (e.g. write_freq_pct)."""
    normalized = dict(row)
    aliases = {
        "write_freq_pct": "write_frequency_pct",
        "policy": "write_policy",
        "queries": "num_queries",
        "writes": "writes_executed",
        "query_speedup": "query_only_speedup",
        "cold_start_speedup": "cold_start_workload_speedup",
        "blended_speedup": "blended_query_speedup",
        "cold_start_workload_us": "cold_start_accel_workload_us",
    }
    for legacy, canonical in aliases.items():
        if canonical not in normalized and legacy in normalized:
            normalized[canonical] = normalized[legacy]
    return normalized


def _is_data_row(row: dict[str, str]) -> bool:
    row = _normalize_sweep_row_keys(row)
    if not row:
        return False
    first = next(iter(row.keys()), "")
    if first.startswith("#"):
        return False
    if "write_frequency_pct" not in row or "workload_speedup" not in row:
        return False
    try:
        float(row["workload_speedup"])
        int(float(row["write_frequency_pct"]))
    except (TypeError, ValueError):
        return False
    return True


def _parse_sweep_reader(reader: csv.DictReader) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for row in reader:
        row = _normalize_sweep_row_keys(row)
        if not _is_data_row(row):
            continue
        parsed: dict[str, Any] = {}
        for key, value in row.items():
            if value is None or value == "":
                continue
            if key in {
                "write_frequency_pct",
                "num_queries",
                "writes_scheduled",
                "writes_executed",
                "rows_touched_per_write",
                "aux_state_bytes",
                "valid_partitions",
                "total_slots",
                "state_bytes_read_median",
                "store_execute_calls",
                *MEASUREMENT_COUNTERS,
            }:
                try:
                    parsed[key] = int(float(value))
                except ValueError:
                    parsed[key] = value
            elif key in STORE_DIAGNOSTIC_FIELDS or key.endswith("_rate") or key in {
                "workload_speedup",
                "cold_start_workload_speedup",
                "query_only_speedup",
                "query_only_speedup_profiler",
                "blended_query_speedup",
                "oracle_regret_pct",
                "validity_slots_scanned_median",
                "validity_scan_us_median",
                "partitions_read_per_query_median",
                "accel_hit_median_us",
                "accel_profiler_query_median_us",
            }:
                coerced = coerce_diagnostic_field(key, value)
                if coerced is None or isinstance(coerced, str):
                    try:
                        parsed[key] = float(value)
                    except ValueError:
                        parsed[key] = value
                else:
                    parsed[key] = coerced
            else:
                parsed[key] = value
        rows.append(parsed)
    return rows


def parse_sweep_csv(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(line for line in handle if not line.startswith("#"))
        return _parse_sweep_reader(reader)


def parse_aux_benchmark_output(text: str) -> list[dict[str, Any]]:
    """Parse aux benchmark stdout (single-point or sweep CSV)."""
    reader = csv.DictReader(
        line for line in text.splitlines() if line.strip() and not line.startswith("#")
    )
    return _parse_sweep_reader(reader)


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
                "query_only_speedup_profiler": float(
                    row.get("query_only_speedup_profiler", 0.0) or 0.0
                ),
                "blended_query_speedup": float(row.get("blended_query_speedup", 0.0) or 0.0),
                "duckdb_workload_us": float(row.get("duckdb_workload_us", 0.0) or 0.0),
                "accel_workload_us": float(row.get("accel_workload_us", 0.0) or 0.0),
                "cold_start_accel_workload_us": float(row.get("cold_start_accel_workload_us", 0.0) or 0.0),
                "setup_full_build_ms": float(row.get("setup_full_build_ms", 0.0) or 0.0),
                "aux_state_bytes": int(float(row.get("aux_state_bytes", 0) or 0)),
                "accel_hit_rate": float(row.get("accel_hit_rate", 0.0) or 0.0),
                "accel_hit_median_us": float(row.get("accel_hit_median_us", 0.0) or 0.0),
                "accel_profiler_query_median_us": float(
                    row.get("accel_profiler_query_median_us", 0.0) or 0.0
                ),
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
                "query_only_speedup_profiler",
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
            old_prof = float(existing.get("accel_profiler_query_median_us") or 0.0)
            new_prof = float(point.get("accel_profiler_query_median_us") or 0.0)
            if old_prof > 0 and new_prof > 0:
                merged["accel_profiler_query_median_us"] = max(old_prof, new_prof)
            elif new_prof > 0:
                merged["accel_profiler_query_median_us"] = new_prof
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
    for metric in ("accel_hit_median_us", "accel_profiler_query_median_us", "accel_workload_us"):
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


def load_bench_summary(
    repo_root: Path, query: int, workload: dict[str, Any] | None = None
) -> dict[str, Any]:
    path = bench_summary_path(repo_root, query, workload)
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def load_optimization_state(
    repo_root: Path, query: int, workload: dict[str, Any] | None = None
) -> dict[str, Any]:
    path = optimization_state_path(repo_root, query, workload)
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save_optimization_state(
    repo_root: Path,
    query: int,
    state: dict[str, Any],
    workload: dict[str, Any] | None = None,
) -> Path:
    path = optimization_state_path(repo_root, query, workload)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2), encoding="utf-8")
    return path


def initialize_optimization_state(
    repo_root: Path,
    query: int,
    analysis: dict[str, Any],
    workload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    existing = load_optimization_state(repo_root, query, workload)
    if existing:
        return existing
    selected = (
        analysis.get("correctness_facts", {})
        .get("state_design", {})
        .get("selected_candidate_id", "")
    )
    state = {
        "active_candidate_id": selected,
        "best_composite_score": 0.0,
        "best_iteration": 0,
        "history": [],
    }
    save_optimization_state(repo_root, query, state, workload)
    return state


def record_sweep_in_optimization_state(
    repo_root: Path,
    query: int,
    *,
    iteration: int,
    summary: dict[str, Any],
    change_kind: str = "baseline",
    active_candidate_id: str | None = None,
    workload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    state = load_optimization_state(repo_root, query, workload) or {
        "active_candidate_id": "",
        "best_composite_score": 0.0,
        "best_iteration": 0,
        "history": [],
    }
    if active_candidate_id:
        state["active_candidate_id"] = active_candidate_id
    score = float(summary.get("composite_score", 0.0) or 0.0)
    entry = {
        "iteration": iteration,
        "change_kind": change_kind,
        "active_candidate_id": state.get("active_candidate_id", ""),
        "composite_score": score,
        "composite_workload_speedup": summary.get("composite_workload_speedup"),
        "read_only_query_speedup": summary.get("read_only_query_speedup"),
        "read_only_cold_start_workload_speedup": summary.get("read_only_cold_start_workload_speedup"),
        "by_write_frequency_pct": summary.get("by_write_frequency_pct", {}),
        "measurement_ok": summary.get("measurement_ok", True),
    }
    state.setdefault("history", []).append(entry)
    if summary.get("measurement_ok", True) and score >= float(state.get("best_composite_score", 0.0) or 0.0):
        state["best_composite_score"] = score
        state["best_iteration"] = iteration
        state["best_summary"] = summary
    save_optimization_state(repo_root, query, state, workload)
    return state


def format_bench_experiment_log_line(
    iteration: int,
    summary: dict[str, Any],
    state: dict[str, Any],
) -> str:
    by_freq = summary.get("by_write_frequency_pct", {})
    freq_bits = []
    for key in sorted(by_freq.keys(), key=lambda x: int(x)):
        row = by_freq[key]
        freq_bits.append(
            f"{key}%→wl={row.get('workload_speedup', 0):.2f}x"
            f"/cold={row.get('cold_start_workload_speedup', 0):.2f}x"
            f"/q={row.get('query_only_speedup', 0):.2f}x"
        )
    improved = ""
    if state.get("best_iteration") == iteration:
        improved = " **new best**"
    if not summary.get("measurement_ok", True):
        improved = f" **INVALID MEASUREMENT: {summary['measurement_diagnosis'][0]}**"
    return (
        f"- iter {iteration}: bench_sweep composite={summary.get('composite_score', 0):.3f} "
        f"(wl_min={summary.get('composite_workload_speedup', 0):.2f}x, "
        f"read={summary.get('read_only_query_speedup', 0):.2f}x, "
        f"cold_start={summary.get('read_only_cold_start_workload_speedup', 0):.2f}x) "
        f"candidate={state.get('active_candidate_id', '?')}{improved}; "
        f"{' | '.join(freq_bits)}"
    )


def _normalize_bench_change_kind(change_kind: str) -> str:
    if change_kind == "implementation":
        return "baseline"
    return change_kind or "bench_sweep"


def _sweep_already_recorded(
    state: dict[str, Any], iteration: int, change_kind: str, score: float
) -> bool:
    history = state.get("history") or []
    if not history:
        return False
    last = history[-1]
    return (
        int(last.get("iteration", -1)) == iteration
        and str(last.get("change_kind", "")) == change_kind
        and float(last.get("composite_score") or 0.0) == score
    )


def record_aux_bench_sweep_outcome(
    repo_root: Path,
    query: int,
    summary: dict[str, Any],
    *,
    iteration: int,
    change_kind: str,
    active_candidate_id: str | None = None,
    workload: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Update optimization_state and append experiment_log.md after a bench sweep."""
    from accel_pipeline.playbook import append_experiment_log

    normalized_kind = _normalize_bench_change_kind(change_kind)
    state = load_optimization_state(repo_root, query, workload) or {
        "active_candidate_id": "",
        "best_composite_score": 0.0,
        "best_iteration": 0,
        "history": [],
    }
    score = float(summary.get("composite_score", 0.0) or 0.0)
    if not _sweep_already_recorded(state, iteration, normalized_kind, score):
        state = record_sweep_in_optimization_state(
            repo_root,
            query,
            iteration=iteration,
            summary=summary,
            change_kind=normalized_kind,
            active_candidate_id=active_candidate_id,
            workload=workload,
        )
        append_experiment_log(
            repo_root,
            query,
            format_bench_experiment_log_line(iteration, summary, state),
            workload=workload,
        )
        if workload and normalized_kind in ("implementation", "baseline", "bench_sweep"):
            from accel_pipeline.config import load_pipeline_config
            from accel_pipeline.kernel_snapshot import (
                has_best_kernel_snapshot,
                snapshot_best_kernel,
            )

            read_only = float(summary.get("read_only_query_speedup") or 0.0)
            if read_only > 0 and summary.get("measurement_ok", True):
                pipeline_config = load_pipeline_config(repo_root=repo_root)
                art = query_artifacts_dir(repo_root, query, workload=workload)
                if normalized_kind == "implementation" or not has_best_kernel_snapshot(art):
                    snapshot_best_kernel(
                        pipeline_config,
                        workload,
                        query,
                        speedup_median=read_only,
                        iteration=iteration,
                        change_kind=normalized_kind,
                    )
    return state


def _measurement_verdict(
    repo_root: Path, summary: dict[str, Any], workload: dict[str, Any] | None
) -> dict[str, Any]:
    from accel_pipeline.config import load_pipeline_config
    from accel_pipeline.measurement import check_measurement

    failures = check_measurement(
        summary,
        workload=workload or {},
        phase="gate",
        config=load_pipeline_config(repo_root=repo_root).measurement,
    )
    return {
        "measurement_ok": not failures,
        "measurement_diagnosis": [d for f in failures for d in f.diagnosis],
    }


def process_aux_bench_after_gates(
    repo_root: Path,
    query: int,
    *,
    iteration: int,
    change_kind: str,
    active_candidate_id: str | None = None,
    workload: dict[str, Any] | None = None,
) -> dict[str, Any] | None:
    """Read aux_bench_summary.json written by aux_bench_sweep gate and update optimization state."""
    summary_path = bench_summary_path(repo_root, query, workload)
    if not summary_path.is_file():
        return None
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    summary.update(_measurement_verdict(repo_root, summary, workload))
    state = record_aux_bench_sweep_outcome(
        repo_root,
        query,
        summary,
        iteration=iteration,
        change_kind=change_kind,
        active_candidate_id=active_candidate_id,
        workload=workload,
    )
    return {"summary": summary, "state": state}
