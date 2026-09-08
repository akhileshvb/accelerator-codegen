"""Parse aux-state write-frequency benchmark sweeps for optimization feedback."""

from __future__ import annotations

import csv
import json
from pathlib import Path
from typing import Any

from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.optimization_signals import STORE_DIAGNOSTIC_FIELDS, coerce_diagnostic_field


def _artifacts_dir(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> Path:
    return query_artifacts_dir(repo_root, query, workload=workload)


def bench_summary_path(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> Path:
    return _artifacts_dir(repo_root, query, workload) / "aux_bench_summary.json"


def optimization_state_path(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> Path:
    return _artifacts_dir(repo_root, query, workload) / "optimization_state.json"


def sweep_results_path(
    repo_root: Path,
    query: int,
    build_dir: Path | None = None,
    *,
    workload: dict[str, Any] | None = None,
) -> Path:
    suffix = "q{query}_aux_bench_sf1"
    if workload:
        suffix = str(workload.get("aux_bench_work_dir_suffix", suffix.format(query=query)))
    else:
        suffix = suffix.format(query=query)
    if build_dir is not None:
        return build_dir / suffix / "results.csv"
    return repo_root / "build-release" / suffix / "results.csv"


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
                if coerced is not None:
                    parsed[key] = coerced
                else:
                    try:
                        parsed[key] = float(value)
                    except ValueError:
                        parsed[key] = value
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
            old_rate = float(existing.get("accel_hit_rate") or 0.0)
            new_rate = float(point.get("accel_hit_rate") or 0.0)
            if old_rate > 0 and new_rate > 0:
                merged["accel_hit_rate"] = min(old_rate, new_rate)
            elif new_rate > 0:
                merged["accel_hit_rate"] = new_rate
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
    }
    state.setdefault("history", []).append(entry)
    if score >= float(state.get("best_composite_score", 0.0) or 0.0):
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
    return (
        f"- iter {iteration}: bench_sweep composite={summary.get('composite_score', 0):.3f} "
        f"(wl_min={summary.get('composite_workload_speedup', 0):.2f}x, "
        f"read={summary.get('read_only_query_speedup', 0):.2f}x, "
        f"cold_start={summary.get('read_only_cold_start_workload_speedup', 0):.2f}x) "
        f"candidate={state.get('active_candidate_id', '?')}{improved}; "
        f"{' | '.join(freq_bits)}"
    )


def list_viable_candidate_ids(analysis: dict[str, Any]) -> list[str]:
    search = analysis.get("state_candidate_search", {})
    candidates = search.get("candidates", [])
    return [
        str(c.get("id"))
        for c in candidates
        if isinstance(c, dict) and c.get("viability") == "viable" and c.get("id")
    ]


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
            if read_only > 0:
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
