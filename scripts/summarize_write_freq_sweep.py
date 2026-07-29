#!/usr/bin/env python3
"""Summarize cross_accel_write_freq_sweep.csv into speedup tables."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "cross_accel_write_freq_sweep.csv")
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
                "writer_style": row["writer_style"],
                "readers": row["readers"],
                "num_writers": row["num_writers"],
                "writer_sleep_ms": row["writer_sleep_ms"],
                "writer_prob_pct": row["writer_prob_pct"],
                "effective_write_pct": float(row["effective_write_pct"]),
                "accel_writer_commits": row["accel_writer_commits"],
            }

    ordered = sorted(points.items(), key=lambda item: item[1]["effective_write_pct"])
    queries = sorted({row["query_num"] for row in rows}, key=int)

    lines: list[str] = []
    lines.append("Cross-accelerator SQL write-frequency sweep")
    lines.append("Writer: invalidating per-query UPDATEs")
    lines.append(f"Source: {path}")
    lines.append("")
    lines.append(
        "Effective write % = 100 * SQL writer_commits / total_reader_queries during accel phase."
    )
    lines.append("Speedup = concurrent DuckDB p50 / accel p50.")
    lines.append("")

    header = [
        "sweep_label",
        "eff_write_%",
        "readers",
        "writers",
        "sleep_ms",
    ] + [f"Q{q}×" for q in queries]
    lines.append("\t".join(header))

    for label, meta in ordered:
        row_cells = [
            label,
            f"{meta['effective_write_pct']:.4f}",
            meta["readers"],
            meta["num_writers"],
            meta["writer_sleep_ms"],
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
