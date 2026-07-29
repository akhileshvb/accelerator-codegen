#!/usr/bin/env python3
"""Summarize targeted-write sweep as speedup matrix (query × writer mode)."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "cross_accel_targeted_write_sweep.csv")
    rows = list(csv.DictReader(path.open()))
    if not rows:
        print(f"No data in {path}", file=sys.stderr)
        return 1

    labels = []
    for row in rows:
        if row["sweep_label"] not in labels:
            labels.append(row["sweep_label"])

    queries = sorted({row["query_num"] for row in rows}, key=int)

    matrix: dict[tuple[str, str], float] = {}
    meta: dict[str, dict[str, str]] = {}
    for row in rows:
        label = row["sweep_label"]
        q = row["query_num"]
        matrix[(label, q)] = float(row["speedup_p50"])
        meta[label] = {
            "writer_target": row["writer_target"],
            "writer_prob_pct": row["writer_prob_pct"],
            "effective_write_pct": row["effective_write_pct"],
            "accel_writer_commits": row["accel_writer_commits"],
        }

    lines: list[str] = []
    lines.append("Cross-accelerator targeted-write isolation sweep")
    lines.append("Speedup = concurrent DuckDB p50 / accel p50 (SQL invalidating UPDATEs)")
    lines.append(f"Source: {path}")
    lines.append("")

    header = ["sweep_label", "writer_target", "eff_write_%"] + [f"Q{q}×" for q in queries]
    lines.append("\t".join(header))
    for label in labels:
        m = meta[label]
        cells = [
            label,
            m["writer_target"],
            f"{float(m['effective_write_pct']):.4f}",
        ]
        for q in queries:
            val = matrix.get((label, q))
            cells.append(f"{val:.2f}" if val is not None else "-")
        lines.append("\t".join(cells))

    lines.append("")
    lines.append("Isolation check (targeted writes should mostly hurt the targeted query):")
    if "C-target-Q14" in labels and "C-target-Q1" in labels:
        q14_on_q14 = matrix.get(("C-target-Q14", "14"), 0.0)
        q14_on_q1 = matrix.get(("C-target-Q14", "1"), 0.0)
        q1_on_q1 = matrix.get(("C-target-Q1", "1"), 0.0)
        q1_on_q14 = matrix.get(("C-target-Q1", "14"), 0.0)
        lines.append(f"  Q14-targeted write → Q14 speedup={q14_on_q14:.1f}×, Q1 speedup={q14_on_q1:.1f}×")
        lines.append(f"  Q1-targeted write  → Q1 speedup={q1_on_q1:.1f}×, Q14 speedup={q1_on_q14:.1f}×")

    text = "\n".join(lines) + "\n"
    summary_path = path.with_suffix(".summary.txt")
    summary_path.write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
