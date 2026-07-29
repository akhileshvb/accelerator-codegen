"""Parse aux-state write-frequency benchmark sweeps for optimization feedback."""

from __future__ import annotations

import csv
import json
from pathlib import Path
from typing import Any


def bench_summary_path(repo_root: Path, query: int) -> Path:
    return repo_root / "pipeline" / "artifacts" / f"q{query}" / "aux_bench_summary.json"


def optimization_state_path(repo_root: Path, query: int) -> Path:
    return repo_root / "pipeline" / "artifacts" / f"q{query}" / "optimization_state.json"


def sweep_results_path(repo_root: Path, query: int, build_dir: Path | None = None) -> Path:
    if build_dir is not None:
        return build_dir / f"q{query}_aux_bench_sf1" / "results.csv"
    return repo_root / "build-release" / f"q{query}_aux_bench_sf1" / "results.csv"


def _is_data_row(row: dict[str, str]) -> bool:
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


def parse_sweep_csv(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    rows: list[dict[str, Any]] = []
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(line for line in handle if not line.startswith("#"))
        for row in reader:
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
                }:
                    try:
                        parsed[key] = int(float(value))
                    except ValueError:
                        parsed[key] = value
                elif key.endswith("_rate") or key in {
                    "workload_speedup",
                    "query_only_speedup",
                    "blended_query_speedup",
                    "oracle_regret_pct",
                }:
                    try:
                        parsed[key] = float(value)
                    except ValueError:
                        parsed[key] = value
                else:
                    parsed[key] = value
            rows.append(parsed)
    return rows


def summarize_sweep_rows(rows: list[dict[str, Any]]) -> dict[str, Any]:
    if not rows:
        return {
            "status": "empty",
            "row_count": 0,
            "composite_score": 0.0,
            "composite_workload_speedup": 0.0,
            "read_only_query_speedup": 0.0,
            "by_write_frequency_pct": {},
        }

    by_freq: dict[str, dict[str, Any]] = {}
    workload_speedups: list[float] = []
    query_speedups: list[float] = []
    read_only_query_speedup = 0.0

    for row in rows:
        freq = int(row.get("write_frequency_pct", -1))
        key = str(freq)
        by_freq[key] = {
            "write_frequency_pct": freq,
            "suite": row.get("suite"),
            "workload_speedup": float(row.get("workload_speedup", 0.0) or 0.0),
            "query_only_speedup": float(row.get("query_only_speedup", 0.0) or 0.0),
            "blended_query_speedup": float(row.get("blended_query_speedup", 0.0) or 0.0),
            "duckdb_workload_us": float(row.get("duckdb_workload_us", 0.0) or 0.0),
            "accel_workload_us": float(row.get("accel_workload_us", 0.0) or 0.0),
            "accel_hit_rate": float(row.get("accel_hit_rate", 0.0) or 0.0),
            "writes_executed": int(row.get("writes_executed", 0) or 0),
        }
        ws = by_freq[key]["workload_speedup"]
        qs = by_freq[key]["query_only_speedup"]
        if ws > 0:
            workload_speedups.append(ws)
        if qs > 0:
            query_speedups.append(qs)
        if freq == 0 and qs > 0:
            read_only_query_speedup = qs

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
        "by_write_frequency_pct": by_freq,
    }


def summarize_sweep_csv(path: Path) -> dict[str, Any]:
    summary = summarize_sweep_rows(parse_sweep_csv(path))
    summary["results_csv"] = str(path)
    return summary


def load_bench_summary(repo_root: Path, query: int) -> dict[str, Any]:
    path = bench_summary_path(repo_root, query)
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def load_optimization_state(repo_root: Path, query: int) -> dict[str, Any]:
    path = optimization_state_path(repo_root, query)
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save_optimization_state(repo_root: Path, query: int, state: dict[str, Any]) -> Path:
    path = optimization_state_path(repo_root, query)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(state, indent=2), encoding="utf-8")
    return path


def initialize_optimization_state(repo_root: Path, query: int, analysis: dict[str, Any]) -> dict[str, Any]:
    existing = load_optimization_state(repo_root, query)
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
    save_optimization_state(repo_root, query, state)
    return state


def record_sweep_in_optimization_state(
    repo_root: Path,
    query: int,
    *,
    iteration: int,
    summary: dict[str, Any],
    change_kind: str = "baseline",
    active_candidate_id: str | None = None,
) -> dict[str, Any]:
    state = load_optimization_state(repo_root, query) or {
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
        "by_write_frequency_pct": summary.get("by_write_frequency_pct", {}),
    }
    state.setdefault("history", []).append(entry)
    if score >= float(state.get("best_composite_score", 0.0) or 0.0):
        state["best_composite_score"] = score
        state["best_iteration"] = iteration
        state["best_summary"] = summary
    save_optimization_state(repo_root, query, state)
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
            f"{key}%→wl={row.get('workload_speedup', 0):.2f}x/q={row.get('query_only_speedup', 0):.2f}x"
        )
    improved = ""
    if state.get("best_iteration") == iteration:
        improved = " **new best**"
    return (
        f"- iter {iteration}: bench_sweep composite={summary.get('composite_score', 0):.3f} "
        f"(wl_min={summary.get('composite_workload_speedup', 0):.2f}x, "
        f"read={summary.get('read_only_query_speedup', 0):.2f}x) "
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


def process_aux_bench_after_gates(
    repo_root: Path,
    query: int,
    *,
    iteration: int,
    change_kind: str,
    active_candidate_id: str | None = None,
) -> dict[str, Any] | None:
    """Read aux_bench_summary.json written by aux_bench_sweep gate and update optimization state."""
    summary_path = bench_summary_path(repo_root, query)
    if not summary_path.is_file():
        return None
    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    state = record_sweep_in_optimization_state(
        repo_root,
        query,
        iteration=iteration,
        summary=summary,
        change_kind=change_kind,
        active_candidate_id=active_candidate_id,
    )
    from accel_pipeline.playbook import append_experiment_log

    append_experiment_log(
        repo_root,
        query,
        format_bench_experiment_log_line(iteration, summary, state),
    )
    return {"summary": summary, "state": state}
