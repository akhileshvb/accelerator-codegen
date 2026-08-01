#!/usr/bin/env python3
"""Summarize cross_accel_scheduled_write_sweep.csv (speedup vs DuckDB at 0/1/10/50%)."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "cross_accel_scheduled_write_sweep.csv")
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
            points[label] = {
                "readers": row["readers"],
                "scheduled_write_pct": row["scheduled_write_pct"],
                "effective_write_pct": float(row["effective_write_pct"]),
            }

    ordered = sorted(points.items(), key=lambda item: float(item[1]["scheduled_write_pct"]))
    queries = sorted({row["query_num"] for row in rows}, key=int)

    lines: list[str] = []
    lines.append("Cross-accelerator scheduled write-frequency sweep")
    lines.append("Writer: per-reader invalidating UPDATEs on aux_bench schedule (0/1/10/50%)")
    lines.append(f"Source: {path}")
    lines.append("")
    lines.append("Effective write % = 100 * writer_commits / reader_queries (should match scheduled %).")
    lines.append("Speedup = concurrent DuckDB p50 / accel p50 under the same load.")
    lines.append("")

    header = ["sweep_label", "sched_%", "eff_write_%", "readers"] + [f"Q{q}×" for q in queries]
    lines.append("\t".join(header))

    for label, meta in ordered:
        row_cells = [
            label,
            meta["scheduled_write_pct"],
            f"{meta['effective_write_pct']:.2f}",
            meta["readers"],
        ]
        for q in queries:
            r = by_label_query.get((label, q))
            row_cells.append(f"{float(r['speedup_p50']):.2f}" if r else "-")
        lines.append("\t".join(row_cells))

    text = "\n".join(lines) + "\n"
    summary_path = path.with_suffix(".summary.txt")
    summary_path.write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
