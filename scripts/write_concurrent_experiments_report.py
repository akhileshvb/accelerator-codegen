#!/usr/bin/env python3
"""Write a combined explainable report from concurrent experiment artifacts."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


def fmt(v: float) -> str:
    if v >= 100:
        return f"{v:.0f}×"
    if v >= 10:
        return f"{v:.1f}×"
    return f"{v:.2f}×"


def load_workload_sweep(path: Path) -> list[dict]:
    rows: list[dict] = []
    if not path.exists():
        return rows
    for line in path.read_text().splitlines()[1:]:
        if not line.startswith("per_query_workload,"):
            continue
        payload = line.split(",", 2)[2]
        fields = payload.split(",")
        if len(fields) < 21:
            continue
        header = [
            "query_num",
            "write_frequency_pct",
            "effective_write_pct",
            "readers",
            "duration_sec",
            "aggregate_reader_qps",
            "reader_queries",
            "baseline_writer_commits",
            "accel_writer_commits",
            "baseline_read_sum_us",
            "accel_read_sum_us",
            "baseline_write_us",
            "accel_write_us",
            "workload_speedup",
            "read_speedup_p50",
            "read_speedup_p95",
            "baseline_read_p50_us",
            "accel_read_p50_us",
            "baseline_read_p95_us",
            "accel_read_p95_us",
            "accel_hit_rate",
        ]
        rows.append(dict(zip(header, fields)))
    return rows


def main() -> int:
    results_dir = Path(sys.argv[1] if len(sys.argv) > 1 else "build-release/concurrent_experiments")
    workload_rows = load_workload_sweep(results_dir / "aux_workload_multi_connection_sweep.csv")

    lines = [
        "# Concurrent accelerator experiments — combined report",
        "",
        f"Artifacts: `{results_dir}`",
        "",
        "## Experiment 1: Per-query multi-connection workload (primary)",
        "",
        "Setup: N reader connections + 1 writer per query; calibrated aggregate reader QPS;",
        "invalidating `UPDATE` schedule 0/1/10/50%; **workload_speedup** includes read + write time.",
        "",
    ]

    if workload_rows:
        by_q: dict[str, list[dict]] = {}
        for row in workload_rows:
            by_q.setdefault(row["query_num"], []).append(row)
        lines.append("| Query | 0% wl | 1% wl | 10% wl | 50% wl | Min wl | Hit@0% |")
        lines.append("| ----- | ----- | ----- | ------ | ------ | ------ | ------ |")
        for q in sorted(by_q, key=int):
            recs = {r["write_frequency_pct"]: r for r in by_q[q]}
            wls = [float(recs[str(f)]["workload_speedup"]) for f in (0, 1, 10, 50) if str(f) in recs]
            cells = [f"Q{q}"]
            for f in (0, 1, 10, 50):
                cells.append(fmt(float(recs[str(f)]["workload_speedup"])) if str(f) in recs else "—")
            cells.append(fmt(min(wls)) if wls else "—")
            cells.append(f"{100*float(recs['0']['accel_hit_rate']):.0f}%" if "0" in recs else "—")
            lines.append("| " + " | ".join(cells) + " |")
        lines.append("")
    else:
        lines.append("_Workload sweep CSV missing._")
        lines.append("")

    for name in (
        "aux_workload_multi_connection_sweep.summary.md",
        "cross_accel_writer_scheduled_sweep.summary.md",
    ):
        p = results_dir / name
        if p.exists():
            lines.append(f"## See also: `{name}`")
            lines.append("")
            lines.append(p.read_text())
            lines.append("")

    lines.extend(
        [
            "## How to read counterintuitive results",
            "",
            "- **Flat read p50, falling workload speedup:** writes are timed on the writer connection;",
            "  micro-read queries stay on the aux fast path (high hit_rate) while total write cost grows.",
            "- **Q3/Q10 low speedup:** join-heavy DuckDB baselines and higher maintenance sensitivity at 50% writes.",
            "- **Cross-accel isolation:** interference matrix shows unrelated writers should not collapse unrelated readers.",
            "",
        ]
    )

    out = results_dir / "CONCURRENT_EXPERIMENTS_REPORT.md"
    out.write_text("\n".join(lines))
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
