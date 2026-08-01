#!/usr/bin/env python3
"""Summarize cross_accel_writer_scheduled_sweep.csv as formatted tables."""

from __future__ import annotations

import csv
import sys
from pathlib import Path

QUERIES_DEFAULT = (1, 3, 6, 10, 12, 14)


def format_speedup(value: float) -> str:
    if value <= 0:
        return "—"
    if value >= 100:
        return f"{value:.0f}×"
    if value >= 10:
        return f"{value:.1f}×"
    return f"{value:.2f}×"


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))

    def fmt_row(cells: list[str]) -> str:
        return "| " + " | ".join(cell.ljust(widths[i]) for i, cell in enumerate(cells)) + " |"

    sep = "| " + " | ".join("-" * widths[i] for i in range(len(headers))) + " |"
    lines = [fmt_row(headers), sep]
    lines.extend(fmt_row(row) for row in rows)
    return "\n".join(lines)


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "cross_accel_writer_scheduled_sweep.csv")
    rows = list(csv.DictReader(path.open()))
    if not rows:
        print(f"No data in {path}", file=sys.stderr)
        return 1

    points: dict[str, dict] = {}
    by_label_query: dict[tuple[str, str], dict] = {}
    for row in rows:
        label = row["write_freq_sweep_label"]
        q = row["query_num"]
        by_label_query[(label, q)] = row
        if label not in points:
            workload = row.get("workload_speedup", "")
            points[label] = {
                "writer_target": row["writer_target"],
                "readers": row["readers"],
                "scheduled_write_pct": row["scheduled_write_pct"],
                "effective_write_pct": float(row["effective_write_pct"]),
                "workload_speedup": float(workload) if workload else 0.0,
                "aggregate_reader_qps": row.get("aggregate_reader_qps", ""),
            }

    ordered = sorted(points.items(), key=lambda item: float(item[1]["scheduled_write_pct"]))
    queries = sorted({int(row["query_num"]) for row in rows})
    if not queries:
        queries = list(QUERIES_DEFAULT)

    workload_headers = ["Writes (sched)", "Writes (eff)", "Workload"]
    workload_rows: list[list[str]] = []
    read_headers = ["Writes (sched)", "Writes (eff)"] + [f"Q{q}" for q in queries]
    read_rows: list[list[str]] = []

    for _label, meta in ordered:
        workload_rows.append(
            [
                f"{meta['scheduled_write_pct']}%",
                f"{meta['effective_write_pct']:.1f}%",
                format_speedup(meta["workload_speedup"]),
            ]
        )
        read_cells = [
            f"{meta['scheduled_write_pct']}%",
            f"{meta['effective_write_pct']:.1f}%",
        ]
        for q in queries:
            r = by_label_query.get((_label, str(q)))
            speedup = float(r["speedup_p50"]) if r and r.get("speedup_p50") else 0.0
            read_cells.append(format_speedup(speedup))
        read_rows.append(read_cells)

    writer_target = ordered[0][1]["writer_target"] if ordered else "?"
    readers = ordered[0][1]["readers"] if ordered else "?"
    aggregate_qps = ordered[0][1].get("aggregate_reader_qps", "")

    intro = [
        "# Cross-accelerator speedup vs DuckDB (fair writer-scheduled sweep)",
        "",
        f"- **Setup:** {readers} concurrent readers (one per query), single writer (`{writer_target}`), scheduled invalidating `UPDATE`s",
        "- **Fairness:** readers are not throttled at write boundaries; aggregate reader QPS is capped equally at every write level",
        f"- **Reader cap:** `{aggregate_qps}` aggregate queries/sec (calibrated so 50% schedule can land near target)",
        "- **Primary metric:** `workload_speedup` = geometric mean of per-query read p50 speedups, adjusted for write time share",
        "- **Per-query metric:** `concurrent DuckDB p50 / accel p50` (read latency only)",
        f"- **Source:** `{path}`",
        "",
        "## Workload speedup (read + write)",
        "",
    ]
    workload_table = markdown_table(workload_headers, workload_rows)
    read_section = [
        "",
        "## Per-query read p50 speedup",
        "",
        markdown_table(read_headers, read_rows),
    ]
    notes = [
        "",
        "**Notes:**",
        "- Scheduled write % = one invalidating write per N aggregate reader queries (N = 100 / 10 / 2 for 1% / 10% / 50%).",
        "- Each benchmark process builds aux state locally (`SKIP_BUILD=0`); earlier idle-vs-write read speedup gaps were mostly unbuilt aux at 0% writes.",
        "- Workload speedup should decrease as write % rises (more invalidation/maintenance); flat or rising read p50 speedups across 0/1/10% indicate a fair read path.",
        "- Q12 and Q14 often drop under writes (lineitem invalidation dominates maintenance cost).",
        "",
    ]

    md_text = "\n".join(intro + [workload_table] + read_section + notes)
    md_path = path.with_suffix(".summary.md")
    md_path.write_text(md_text)

    plain_lines = [
        "Cross-accelerator single-writer scheduled sweep (fair)",
        f"Writer: {writer_target} | Readers: {readers}",
        f"Source: {path}",
        "",
        workload_table.replace("|", "\t").replace("-", ""),
        "",
        read_section[3].replace("|", "\t").replace("-", ""),
        "",
    ]
    summary_path = path.with_suffix(".summary.txt")
    summary_path.write_text("\n".join(plain_lines))
    print(md_text)
    print(f"wrote {md_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
