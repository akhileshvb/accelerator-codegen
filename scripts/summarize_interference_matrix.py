#!/usr/bin/env python3
"""Summarize interference matrix CSV."""

from __future__ import annotations

import csv
import sys
from pathlib import Path


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "cross_accel_interference_matrix.csv")
    rows = list(csv.DictReader(path.open()))
    if not rows:
        print(f"No data in {path}", file=sys.stderr)
        return 1

    writers = []
    for row in rows:
        if row["writer_label"] not in writers:
            writers.append(row["writer_label"])

    queries = sorted({int(row["reader_query"]) for row in rows})

    lines = [
        "Cross-accelerator interference matrix",
        f"Source: {path}",
        "Retention = idle_accel_p50 / accel_p50 under load (1.0 = no slowdown vs no-writer baseline)",
        "",
        "writer_label\tcommits\teff_write%\t" + "\t".join(f"Q{q} ret" for q in queries),
    ]

    for writer in writers:
        subset = [r for r in rows if r["writer_label"] == writer]
        if not subset:
            continue
        meta = subset[0]
        cells = [
            writer,
            meta["writer_commits"],
            f"{float(meta['effective_write_pct']):.2f}",
        ]
        by_q = {int(r["reader_query"]): float(r["retention_vs_idle"]) for r in subset}
        for q in queries:
            val = by_q.get(q)
            cells.append(f"{val:.3f}" if val is not None else "-")
        lines.append("\t".join(cells))

    lines.extend(["", "Diagonal isolation checks (writer target vs same reader):"])
    for q in queries:
        key = f"target-Q{q}"
        if key in writers:
            subset = [r for r in rows if r["writer_label"] == key]
            by_q = {int(r["reader_query"]): float(r["retention_vs_idle"]) for r in subset}
            own = by_q.get(q, 0.0)
            others = [by_q.get(oq, 0.0) for oq in queries if oq != q]
            avg_other = sum(others) / len(others) if others else 0.0
            lines.append(f"  Writer Q{q}: Q{q} retention={own:.3f}, other readers avg={avg_other:.3f}")

    text = "\n".join(lines) + "\n"
    summary = path.with_suffix(".summary.txt")
    summary.write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
