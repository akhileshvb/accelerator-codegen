#!/usr/bin/env python3
"""Read speedup vs the host engine: JetStream (aux state) and GenDB, on DuckDB and Postgres.

usage: plot_read_speedup_vs_gendb.py TIMES_CSV OUT_PREFIX

TIMES_CSV columns (milliseconds, default-parameter queries, read-only):
  query, duckdb_host_ms, postgres_host_ms, jetstream_duckdb_ms, jetstream_postgres_ms, gendb_ms
Speedup = host time / system time, with the same host time for JetStream and GenDB.
Writes OUT_PREFIX.pdf and OUT_PREFIX.png.
"""

import csv
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

BLUE = "#4C72B0"    # JetStream
ORANGE = "#DD8452"  # GenDB
QUERIES = ["Q1", "Q2", "Q3", "Q4", "Q6", "Q24"]


def fmt(v):
    return f"{v:.3g}×" if v < 1000 else f"{v:.0f}×"


def main():
    times_csv, out_prefix = sys.argv[1], sys.argv[2]
    rows = {r["query"]: {k: float(v) for k, v in r.items() if k != "query"}
            for r in csv.DictReader(open(times_csv))}
    series = [  # (label, color, hatch, host column, system column)
        ("JetStream / DuckDB", BLUE, None, "duckdb_host_ms", "jetstream_duckdb_ms"),
        ("GenDB / DuckDB", ORANGE, None, "duckdb_host_ms", "gendb_ms"),
        ("JetStream / Postgres", BLUE, "////", "postgres_host_ms", "jetstream_postgres_ms"),
        ("GenDB / Postgres", ORANGE, "////", "postgres_host_ms", "gendb_ms"),
    ]
    plt.rcParams.update({"font.size": 7, "hatch.linewidth": 0.6, "hatch.color": "#333333"})
    fig, ax = plt.subplots(figsize=(3.4, 2.5))
    x = np.arange(len(QUERIES))
    width = 0.2
    for i, (label, color, hatch, host, system) in enumerate(series):
        vals = [rows[q][host] / rows[q][system] for q in QUERIES]
        pos = x + (i - 1.5) * width
        ax.bar(pos, vals, width * 0.92, color=color, hatch=hatch, edgecolor="#333333" if hatch else color,
               linewidth=0.4, label=label, zorder=2)
        for p, v in zip(pos, vals):
            ax.text(p, v * 1.12, fmt(v), rotation=90, ha="center", va="bottom", fontsize=5.5, color="#222222")
    ax.set_yscale("log")
    # Start below 1x so a slowdown still shows as a bar; dashed line marks parity.
    ax.set_ylim(0.5, ax.get_ylim()[1] * 12)
    ax.axhline(1, color="#666666", linestyle="--", linewidth=0.6, zorder=1)
    ax.set_xticks(x, QUERIES)
    ax.set_xlabel("Query")
    ax.set_ylabel("Read speedup vs host (×)")
    ax.set_title("SEC-EDGAR (SF=3)", fontsize=8)
    ax.grid(axis="y", which="major", color="#dddddd", linewidth=0.5, zorder=0)
    for s in ("top", "right"):
        ax.spines[s].set_visible(False)
    # Legend fills column-first: DuckDB pair on the left, Postgres pair on the right.
    ax.legend(ncol=2, frameon=False, loc="upper center", bbox_to_anchor=(0.5, -0.2),
              fontsize=6.5, handlelength=2.2)
    fig.tight_layout()
    for ext in ("pdf", "png"):
        fig.savefig(f"{out_prefix}.{ext}", dpi=300, bbox_inches="tight")
    print(f"wrote {out_prefix}.pdf / .png")


if __name__ == "__main__":
    main()
