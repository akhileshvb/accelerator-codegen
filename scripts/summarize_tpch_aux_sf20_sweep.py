#!/usr/bin/env python3
"""Summarize TPC-H SF20 aux write-frequency sweep CSVs into one report."""

from __future__ import annotations

import argparse
import csv
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "pipeline" / "orchestrator"))

from accel_pipeline.aux_bench import summarize_sweep_csv  # noqa: E402


def load_config(path: Path) -> dict:
    if not path.is_file():
        return {}
    try:
        import yaml  # type: ignore
    except ImportError:
        return {}
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        default=str(ROOT / "build-release"),
        help="CMake build directory",
    )
    parser.add_argument(
        "--label",
        default="tpch_sf20",
        help="ACCEL_AUX_BENCH_DB_LABEL / work dir suffix label",
    )
    parser.add_argument(
        "--queries",
        default="",
        help="Comma-separated query list (default: from config or built-in)",
    )
    parser.add_argument(
        "--config",
        default=str(ROOT / "pipeline/config/benchmark_packs/tpch_sf20_aux_sweep.yaml"),
    )
    parser.add_argument(
        "--out",
        default="",
        help="Summary CSV path (default: <build-dir>/tpch_aux_sf20_sweep_summary.csv)",
    )
    args = parser.parse_args()

    cfg = load_config(Path(args.config))
    if args.queries.strip():
        queries = [int(q) for q in args.queries.split(",") if q.strip()]
    else:
        queries = [int(q) for q in cfg.get("queries", [])]

    build_dir = Path(args.build_dir)
    out_path = Path(args.out) if args.out else build_dir / "tpch_aux_sf20_sweep_summary.csv"
    detail_path = out_path.with_suffix(".json")

    rows_out: list[dict] = []
    detail: dict[str, object] = {"label": args.label, "queries": {}}

    for query in queries:
        results_csv = build_dir / f"q{query}_aux_bench_{args.label}" / "results.csv"
        summary = summarize_sweep_csv(results_csv)
        detail["queries"][str(query)] = {
            "results_csv": str(results_csv),
            "summary": summary,
        }
        if summary.get("status") != "ok":
            rows_out.append(
                {
                    "query": query,
                    "write_frequency_pct": "",
                    "status": summary.get("status", "missing"),
                    "workload_speedup": "",
                    "query_only_speedup": "",
                    "accel_hit_median_us": "",
                    "duckdb_workload_us": "",
                    "accel_workload_us": "",
                    "composite_score": summary.get("composite_score", 0.0),
                }
            )
            continue

        by_freq = summary.get("by_write_frequency_pct", {})
        for freq_key in sorted(by_freq, key=lambda k: float(k)):
            row = by_freq[freq_key]
            rows_out.append(
                {
                    "query": query,
                    "write_interval_sec": row.get("write_interval_sec", ""),
                    "write_frequency_pct": row.get("write_frequency_pct"),
                    "status": "ok",
                    "workload_speedup": row.get("workload_speedup"),
                    "query_only_speedup": row.get("query_only_speedup"),
                    "accel_hit_median_us": row.get("accel_hit_median_us"),
                    "duckdb_workload_us": row.get("duckdb_workload_us"),
                    "accel_workload_us": row.get("accel_workload_us"),
                    "composite_score": summary.get("composite_score", 0.0),
                }
            )

    fieldnames = [
        "query",
        "write_interval_sec",
        "write_frequency_pct",
        "status",
        "workload_speedup",
        "query_only_speedup",
        "accel_hit_median_us",
        "duckdb_workload_us",
        "accel_workload_us",
        "composite_score",
    ]
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows_out)

    detail_path.write_text(json.dumps(detail, indent=2), encoding="utf-8")

    print(f"wrote {out_path}")
    print(f"wrote {detail_path}")
    print()
    print(f"{'Q':>3}  {'interval':>8}  {'workload':>10}  {'query':>10}  {'accel_us':>10}")
    for row in rows_out:
        if row["status"] != "ok":
            print(f"{row['query']:>3}  {'—':>8}  {row['status']}")
            continue
        interval = row.get("write_interval_sec", "")
        interval_label = "read-only" if interval in ("", "0", "0.0") else f"{float(interval):g}s"
        print(
            f"{row['query']:>3}  {interval_label:>8}  "
            f"{float(row['workload_speedup']):>10.2f}  "
            f"{float(row['query_only_speedup']):>10.2f}  "
            f"{float(row['accel_hit_median_us']):>10.1f}"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
