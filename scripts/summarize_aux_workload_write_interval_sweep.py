#!/usr/bin/env python3
"""Summarize time-based write interval workload sweep."""

from __future__ import annotations

import sys
from pathlib import Path

QUERIES = (1, 3, 6, 10, 12, 14)


def fmt_speedup(v: float) -> str:
    if v <= 0:
        return "—"
    if v >= 100:
        return f"{v:.0f}×"
    if v >= 10:
        return f"{v:.1f}×"
    return f"{v:.2f}×"


def load_rows(path: Path) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    for line in path.read_text().splitlines():
        if line.startswith("experiment,"):
            continue
        if line.startswith("write_interval,"):
            payload = line.split(",", 2)[2]
            fields = payload.split(",")
            if len(fields) < 23:
                continue
            header = [
                "query_num",
                "write_frequency_pct",
                "write_interval_sec",
                "effective_write_pct",
                "writes_per_sec",
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


def fmt_hit_rate(v: float) -> str:
    if v < 0:
        return "—"
    return f"{100 * v:.0f}%"


def lookup_rec(
    by_key: dict[tuple[str, str], dict[str, str]],
    rows: list[dict[str, str]],
    query: str,
    interval: float,
) -> dict[str, str] | None:
    rec = by_key.get((query, str(interval)))
    if rec is None:
        rec = by_key.get((query, f"{interval:g}"))
    if rec is None and interval == int(interval):
        rec = by_key.get((query, str(int(interval))))
    if rec is None:
        rec = next(
            (r for r in rows if r["query_num"] == query and float(r["write_interval_sec"]) == interval),
            None,
        )
    return rec


def measured_writes_label(
    rows: list[dict[str, str]],
    by_key: dict[tuple[str, str], dict[str, str]],
    queries: list[str],
    interval: float,
) -> str:
    if interval <= 0:
        return "no writes"
    rates: list[float] = []
    for query in queries:
        rec = lookup_rec(by_key, rows, query, interval)
        if rec is not None:
            rates.append(float(rec["writes_per_sec"]))
    if not rates:
        return "—"
    low = min(rates)
    high = max(rates)
    if abs(high - low) < 0.05:
        return f"{high:.2f} w/s"
    return f"{low:.2f}–{high:.2f} w/s"


def pivot_table(
    rows: list[dict[str, str]],
    intervals: list[float],
    queries: list[str],
    value_key: str,
    formatter,
) -> list[str]:
    by_key = {(r["query_num"], r["write_interval_sec"]): r for r in rows}
    header = "| Writes/sec | " + " | ".join(f"Q{q}" for q in queries) + " |"
    sep = "| ---------- | " + " | ".join("-----" for _ in queries) + " |"
    table = [header, sep]
    for iv in intervals:
        label = measured_writes_label(rows, by_key, queries, iv)
        cells: list[str] = []
        for q in queries:
            rec = lookup_rec(by_key, rows, q, iv)
            if rec is None:
                cells.append("—")
            else:
                cells.append(formatter(float(rec[value_key])))
        table.append("| " + label + " | " + " | ".join(cells) + " |")
    return table


def main() -> int:
    path = Path(sys.argv[1] if len(sys.argv) > 1 else "aux_workload_write_interval_sweep.csv")
    rows = load_rows(path)
    if not rows:
        print(f"No rows in {path}", file=sys.stderr)
        return 1

    by_key = {(r["query_num"], r["write_interval_sec"]): r for r in rows}
    intervals = sorted({float(r["write_interval_sec"]) for r in rows})

    lines = [
        "# Time-based write interval sweep",
        "",
        "Invalidating `UPDATE` on a wall-clock schedule while readers run at calibrated aggregate QPS.",
        "Pivot rows use **measured writes/sec** (actual writer throughput), not the configured sleep interval.",
        f"Source: `{path}`",
        "",
        "## Workload speedup by query and measured write rate",
        "",
        "| Query | Writes/sec | Workload | Read p50 | Hit rate |",
        "| ----- | ---------- | -------- | -------- | -------- |",
    ]
    for q in sorted({r["query_num"] for r in rows}, key=int):
        for iv in intervals:
            rec = lookup_rec(by_key, rows, str(q), iv)
            if not rec:
                continue
            wps = float(rec["writes_per_sec"])
            label = "no writes" if iv <= 0 else f"{wps:.2f} w/s"
            lines.append(
                f"| Q{rec['query_num']} | {label} | "
                f"{fmt_speedup(float(rec['workload_speedup']))} | "
                f"{fmt_speedup(float(rec['read_speedup_p50']))} | "
                f"{fmt_hit_rate(float(rec['accel_hit_rate']))} |"
            )

    queries = [str(q) for q in sorted({int(r["query_num"]) for r in rows})]
    lines.extend(
        [
            "",
            "## Workload speedup (measured writes/sec)",
            "",
            *pivot_table(rows, intervals, queries, "workload_speedup", fmt_speedup),
            "",
            "## Aux hit rate (measured writes/sec)",
            "",
            *pivot_table(rows, intervals, queries, "accel_hit_rate", fmt_hit_rate),
        ]
    )
    lines.extend(
        [
            "",
            "## Crossover (workload speedup < 1×)",
            "",
        ]
    )
    crossovers: list[str] = []
    for q in sorted({r["query_num"] for r in rows}, key=int):
        qrows = sorted(
            [r for r in rows if r["query_num"] == str(q)],
            key=lambda r: float(r["write_interval_sec"]),
            reverse=True,
        )
        aggressive = [r for r in qrows if float(r["write_interval_sec"]) > 0]
        below = next((r for r in aggressive if float(r["workload_speedup"]) < 1.0), None)
        if below:
            crossovers.append(
                f"- Q{q}: below 1× at **{float(below['writes_per_sec']):.2f} w/s** "
                f"(configured {float(below['write_interval_sec']):g}s, "
                f"workload {float(below['workload_speedup']):.2f}×)"
            )
        else:
            fastest = min(
                (float(r["write_interval_sec"]) for r in aggressive),
                default=0.0,
            )
            if fastest > 0:
                rec = next(r for r in aggressive if float(r["write_interval_sec"]) == fastest)
                crossovers.append(
                    f"- Q{q}: still above 1× at fastest tested **{float(rec['writes_per_sec']):.2f} w/s** "
                    f"(configured {fastest:g}s, workload {float(rec['workload_speedup']):.2f}×)"
                )
    lines.extend(crossovers if crossovers else ["- (none yet)"])
    lines.extend(
        [
            "",
            "## Interpretation",
            "",
            "- Compare **measured writes/sec**; configured sleep intervals can exceed what the writer sustains.",
            "- Use fresh DB copies per point when `ACCEL_WORKLOAD_FRESH_DB=1` so write load does not accumulate.",
            "- Look for the interval where workload speedup crosses ~1× (aux no longer worth it).",
            "",
        ]
    )
    md_path = path.with_suffix(".summary.md")
    md_path.write_text("\n".join(lines))
    print("\n".join(lines))
    print(f"wrote {md_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
