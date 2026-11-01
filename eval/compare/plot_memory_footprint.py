#!/usr/bin/env python3
"""TPC-H SF20 memory footprint: JetStream vs Bespoke OLAP vs GenDB.

GenDB and Bespoke keep their own encoded copy of the data, so a deployment that
still serves ad-hoc SQL must also keep the base engine's tables around; that
base cost is stacked on top of their bars in a separate color.

Usage: plot_memory_footprint.py VERIFIED_GB [OUT_PREFIX]
"""

from __future__ import annotations

import sys
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parents[2]

BASE_TABLE_GB = 5.4  # tpch_sf20.duckdb is 5,416,693,760 bytes
STATELESS_GB = 5.4
STATEFUL_GB = 28.4
BESPOKE_GB = 16.12
GENDB_GB = 18.1

STATELESS_COLOR = "#1F3A8A"
BESPOKE_COLOR = "#16A34A"
GENDB_COLOR = "#F59E0B"
STATEFUL_COLOR = "#1A2654"
VERIFIED_COLOR = "#3B1F6B"
BASE_COLOR = "#D9577A"

LABEL_SIZE = 7
TICK_SIZE = 7
AXIS_LABEL_SIZE = 8
TITLE_SIZE = 9
COLUMN_WIDTH_IN = 3.33


def main() -> None:
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    verified_gb = float(sys.argv[1])
    out_prefix = Path(sys.argv[2]) if len(sys.argv) > 2 else ROOT / "eval/compare/results/memory_footprint_sf20"

    # (label, own footprint, base tables stacked on top, color)
    systems = [
        ("JetStream\n(stateless)", STATELESS_GB, 0.0, STATELESS_COLOR),
        ("Bespoke\nOLAP", BESPOKE_GB, BASE_TABLE_GB, BESPOKE_COLOR),
        ("GenDB", GENDB_GB, BASE_TABLE_GB, GENDB_COLOR),
        ("JetStream\n(stateful)", STATEFUL_GB, 0.0, STATEFUL_COLOR),
        ("JetStream\n(stateful,\nverified)", verified_gb, 0.0, VERIFIED_COLOR),
    ]

    fig, ax = plt.subplots(figsize=(COLUMN_WIDTH_IN, 2.2), facecolor="white", layout="constrained")
    fig.get_layout_engine().set(w_pad=0.01, h_pad=0.01, rect=(0, 0, 0.98, 1))
    x = np.arange(len(systems))
    width = 0.58
    own = [s[1] for s in systems]
    base = [s[2] for s in systems]
    ax.bar(x, own, width, color=[s[3] for s in systems], zorder=3)
    base_bars = ax.bar(x, base, width, bottom=own, color=BASE_COLOR, zorder=3,
                       label=f"Base tables for ad-hoc SQL ({BASE_TABLE_GB:g} GB)")

    totals = [o + b for o, b in zip(own, base)]
    for xi, total in zip(x, totals):
        ax.text(xi, total + max(totals) * 0.015, f"{total:.1f} GB", ha="center", va="bottom",
                fontsize=LABEL_SIZE, fontweight="bold")
    for xi, o, b in zip(x, own, base):
        if b:
            ax.text(xi, o + b / 2, f"+{b:g}", ha="center", va="center", fontsize=LABEL_SIZE - 1,
                    color="white", fontweight="bold", zorder=4)

    ax.set_xticks(x)
    ax.set_xticklabels([s[0] for s in systems], fontsize=TICK_SIZE)
    ax.tick_params(axis="y", labelsize=TICK_SIZE)
    ax.set_ylabel("Memory footprint (GB)", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylim(0, max(totals) * 1.15)
    ax.set_title("TPC-H (SF=20): Persistent Memory Footprint", fontsize=TITLE_SIZE)
    ax.legend(handles=[base_bars], loc="upper left", frameon=False, fontsize=LABEL_SIZE - 1,
              handlelength=1.2, borderaxespad=0.3)

    out_prefix.parent.mkdir(parents=True, exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(out_prefix.with_suffix(f".{ext}"), dpi=600, facecolor="white")
        print(f"wrote {out_prefix.with_suffix('.' + ext)}")


if __name__ == "__main__":
    main()
