#!/usr/bin/env python3
"""Summarize aux_workload_multi_connection_sweep.csv."""

from __future__ import annotations

import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from merge_workload_sweep_csv import load_rows

QUERIES = (1, 3, 6, 10, 12, 14)
FREQS = (0, 1, 10, 50)


def fmt_speedup(v: float) -> str:
    if v <= 0:
        return "—"
    if v >= 100:
        return f"{v:.0f}×"
    if v >= 10:
        return f"{v:.1f}×"
    return f"{v:.2f}×"


def fmt_pct(v: float) -> str:
    return f"{v:.1f}%"


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(cell))

    def fmt_row(cells: list[str]) -> str:
        return "| " + " | ".join(c.ljust(widths[i]) for i, c in enumerate(cells)) + " |"

    sep = "| " + " | ".join("-" * w for w in widths) + " |"
    return "\n".join([fmt_row(headers), sep] + [fmt_row(r) for r in rows])


def load_all_rows(path: Path) -> dict[tuple[str, str], dict]:
    return load_rows(path)


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "aux_workload_multi_connection_sweep.csv")
    by_key = load_all_rows(path)
    if not by_key:
        print(f"No rows in {path}", file=sys.stderr)
        return 1

    workload_headers = ["Query", "Writes (sched)", "Writes (eff)", "Workload", "Read p50", "Hit rate"]
    workload_rows: list[list[str]] = []
    composite: dict[str, list[float]] = {}

    for q in QUERIES:
        wls: list[float] = []
        for freq in FREQS:
            rec = by_key.get((str(q), str(freq)))
            if not rec:
                continue
            wl = float(rec["workload_speedup"])
            wls.append(wl)
            workload_rows.append(
                [
                    f"Q{q}",
                    f"{freq}%",
                    fmt_pct(float(rec["effective_write_pct"])),
                    fmt_speedup(wl),
                    fmt_speedup(float(rec["read_speedup_p50"])),
                    fmt_pct(100.0 * float(rec["accel_hit_rate"])),
                ]
            )
        if wls:
            composite[str(q)] = wls

    composite_rows = [
        ["Q" + q, fmt_speedup(min(v)), fmt_speedup(sum(v) / len(v))]
        for q, v in sorted(composite.items(), key=lambda x: int(x[0]))
    ]

    intro = [
        "# Per-query multi-connection workload sweep",
        "",
        "Concurrent **N reader connections + 1 writer** per query; same aggregate reader QPS at every write level.",
        "Primary metric: **workload_speedup** = (baseline read+write sum) / (accel read+write sum).",
        f"Source: `{path}`",
        "",
        "## Workload speedup by query and write %",
        "",
        markdown_table(workload_headers, workload_rows),
        "",
        "## Composite (min / mean workload speedup across write %)",
        "",
        markdown_table(["Query", "Min workload", "Mean workload"], composite_rows),
        "",
        "**Interpretation:**",
        "- Micro-read queries (Q1/Q12/Q14) should show high workload speedup when hit_rate ≈ 100%.",
        "- Workload speedup should fall as write % rises (writer time dominates).",
        "- Q3/Q10 may show low speedup: join-heavy baselines and higher maintenance sensitivity.",
        "",
    ]
    md_path = path.with_suffix(".summary.md")
    md_path.write_text("\n".join(intro))
    print("\n".join(intro))
    print(f"wrote {md_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
