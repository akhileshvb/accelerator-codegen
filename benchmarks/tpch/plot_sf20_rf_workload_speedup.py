#!/usr/bin/env python3
"""Plot SF20 RF aux workload speedups and aux-state memory (read-only + write-interval sweep)."""

from __future__ import annotations

import csv
import re
from io import StringIO
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

ROOT = Path(__file__).resolve().parent
LOG_DIR = ROOT / "results_rf_full_tf"
CSV_DIR = LOG_DIR / "csv"
OUT_DIR = LOG_DIR
QUERIES = [1, 3, 6, 9, 10, 12, 14, 17, 18, 21]
WRITE_INTERVALS = [300.0, 120.0, 60.0]


def pick_row(cands: list[dict], interval: float) -> dict | None:
    if not cands:
        return None
    target = 600.0 if interval == 0.0 else max(600.0, 5.0 * interval)

    def score(r: dict) -> tuple:
        return (abs(float(r["measured_duration_sec"]) - target),
                -float(r["workload_speedup"]))

    return sorted(cands, key=score)[0]


def parse_log(path: Path) -> list[dict]:
    text = path.read_text()
    header = next((line for line in text.splitlines() if line.startswith("suite,")), None)
    if header is None:
        return []
    rows: list[dict] = []
    for line in text.splitlines():
        if not line.startswith("lineitem_only,"):
            continue
        rows.append(next(csv.DictReader(StringIO(header + "\n" + line))))
    return rows


def export_csvs() -> None:
    CSV_DIR.mkdir(parents=True, exist_ok=True)
    for path in sorted(LOG_DIR.glob("tpch_aux_sf20_q*.log")):
        q = int(re.search(r"q(\d+)", path.name).group(1))
        rows = parse_log(path)
        if not rows:
            continue
        out = CSV_DIR / f"q{q}_results.csv"
        with out.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)


def load_rows() -> dict[int, list[dict]]:
    export_csvs()
    by_q: dict[int, list[dict]] = {}
    for path in sorted(CSV_DIR.glob("q*_results.csv")):
        q = int(path.name.split("_")[0][1:])
        with path.open() as f:
            by_q[q] = list(csv.DictReader(f))
    return by_q


def load_speedups(rows_by_q: dict[int, list[dict]]) -> dict[int, dict[float, float]]:
    by_q: dict[int, dict[float, float]] = {}
    for q, rows in rows_by_q.items():
        buckets: dict[float, list[dict]] = {}
        for r in rows:
            if r.get("write_mode") != "rf":
                continue
            # write_frequency_pct is a measured writes/queries ratio, not a sweep axis.
            iv = float(r["write_interval_sec"])
            buckets.setdefault(iv, []).append(r)
        by_q[q] = {}
        for iv, cands in buckets.items():
            row = pick_row(cands, iv)
            if row:
                by_q[q][iv] = float(row["workload_speedup"])
    return by_q


def load_aux_bytes(rows_by_q: dict[int, list[dict]]) -> dict[int, int]:
    aux: dict[int, int] = {}
    for q, rows in rows_by_q.items():
        for r in rows:
            if r.get("write_mode") != "rf":
                continue
            if float(r["write_interval_sec"]) != 0.0:
                continue
            aux[q] = int(float(r["aux_state_bytes"]))
            break
    return aux


def fmt_bytes(n: int) -> str:
    if n >= 1_000_000_000:
        return f"{n / 1_000_000_000:.2g} GB"
    if n >= 1_000_000:
        return f"{n / 1_000_000:.2g} MB"
    if n >= 1_000:
        return f"{n / 1_000:.2g} KB"
    return f"{n} B"


def style() -> None:
    from matplotlib import font_manager

    bundled = (
        Path(__file__).resolve().parents[2]
        / ".mpl-venv"
        / "lib"
        / "python3.14"
        / "site-packages"
        / "matplotlib"
        / "mpl-data"
        / "fonts"
        / "ttf"
        / "DejaVuSans.ttf"
    )
    if bundled.exists():
        font_manager.fontManager.addfont(str(bundled))
        plt.rcParams["font.family"] = font_manager.FontProperties(fname=str(bundled)).get_name()
    else:
        plt.rcParams["font.family"] = "DejaVu Sans"
    plt.rcParams.update({
        "font.size": 11,
        "axes.titlesize": 13,
        "axes.labelsize": 11,
        "figure.facecolor": "white",
        "axes.facecolor": "#fafafa",
        "axes.grid": True,
        "grid.alpha": 0.35,
        "grid.linestyle": "--",
        "axes.axisbelow": True,
        "text.antialiased": True,
        "savefig.dpi": 180,
    })


def plot_readonly(data: dict[int, dict[float, float]]) -> Path:
    style()
    labels = [f"Q{q}" for q in QUERIES]
    vals = [data.get(q, {}).get(0.0) for q in QUERIES]
    x = np.arange(len(QUERIES))

    fig, ax = plt.subplots(figsize=(10, 5.2))
    bars = ax.bar(
        x,
        [v if v is not None else 0 for v in vals],
        color="#2a6f7a",
        width=0.72,
        edgecolor="white",
        linewidth=0.5,
    )
    for i, v in enumerate(vals):
        if v is None:
            bars[i].set_alpha(0.15)
            ax.text(i, 1.5, "n/a", ha="center", va="bottom", fontsize=8, color="#666")
        else:
            ax.text(i, v, f"{v:.3g}", ha="center", va="bottom", fontsize=8)

    ax.set_yscale("log")
    ax.set_xticks(x, labels)
    ax.set_ylabel("Workload speedup (DuckDB / accel)")
    ax.set_xlabel("Query")
    ax.set_title("SF20 RF aux — read-only workload speedup (write_interval = 0)")
    ax.axhline(1.0, color="#444", lw=0.8, alpha=0.6)
    fig.tight_layout()
    out = OUT_DIR / "sf20_rf_workload_speedup_readonly.png"
    fig.savefig(out, dpi=160)
    plt.close(fig)
    return out


def plot_write_sweep(data: dict[int, dict[float, float]]) -> Path:
    style()
    labels = [f"Q{q}" for q in QUERIES]
    x = np.arange(len(QUERIES))
    n_series = len(WRITE_INTERVALS)
    group = 0.82
    width = group / n_series
    colors = ["#1b4f72", "#2a9d8f", "#e9c46a"]

    fig, ax = plt.subplots(figsize=(11, 5.5))
    for j, iv in enumerate(WRITE_INTERVALS):
        offsets = x - group / 2 + width / 2 + j * width
        ys = [data.get(q, {}).get(iv) for q in QUERIES]
        bars = ax.bar(
            offsets,
            [v if v is not None else 0 for v in ys],
            width=width * 0.92,
            color=colors[j],
            edgecolor="white",
            linewidth=0.5,
            label=f"{int(iv)}s interval",
        )
        for i, v in enumerate(ys):
            if v is None:
                bars[i].set_alpha(0.12)
                ax.text(offsets[i], 1.4, "—", ha="center", va="bottom",
                        fontsize=7, color="#888")

    ax.set_yscale("log")
    ax.set_xticks(x, labels)
    ax.set_ylabel("Workload speedup (DuckDB / accel)")
    ax.set_xlabel("Query")
    ax.set_title(
        "SF20 RF aux — workload speedup under write-interval sweep\n"
        "(rf_chunk_orders=30000, DuckDB JSON profile ref, TF SQL accel path)"
    )
    ax.axhline(1.0, color="#444", lw=0.8, alpha=0.6)
    ax.legend(frameon=False, loc="upper right")
    fig.tight_layout()
    out = OUT_DIR / "sf20_rf_workload_speedup_write_sweep.png"
    fig.savefig(out, dpi=160)
    plt.close(fig)
    return out


def plot_aux_memory(aux_bytes: dict[int, int]) -> Path:
    style()
    labels = [f"Q{q}" for q in QUERIES]
    vals_mb = [(aux_bytes.get(q, 0) / 1_000_000) if q in aux_bytes else None for q in QUERIES]
    x = np.arange(len(QUERIES))

    fig, ax = plt.subplots(figsize=(10, 5.2))
    heights = [v if v is not None else 0 for v in vals_mb]
    bars = ax.bar(
        x,
        heights,
        color="#6a4c93",
        width=0.72,
        edgecolor="white",
        linewidth=0.5,
    )
    for i, (q, v) in enumerate(zip(QUERIES, vals_mb)):
        if v is None:
            bars[i].set_alpha(0.15)
            ax.text(i, 0.05, "n/a", ha="center", va="bottom", fontsize=8, color="#666")
        else:
            ax.text(i, v, fmt_bytes(aux_bytes[q]), ha="center", va="bottom", fontsize=8)

    ax.set_yscale("log")
    ax.set_xticks(x, labels)
    ax.set_ylabel("Aux state size")
    ax.set_xlabel("Query")
    ax.set_title("SF20 RF aux — in-memory aux state size (read-only, after full build)")
    fig.tight_layout()
    out = OUT_DIR / "sf20_rf_aux_state_memory.png"
    fig.savefig(out, dpi=160)
    plt.close(fig)
    return out


def main() -> None:
    rows_by_q = load_rows()
    speedups = load_speedups(rows_by_q)
    aux_bytes = load_aux_bytes(rows_by_q)
    out1 = plot_readonly(speedups)
    out2 = plot_write_sweep(speedups)
    out3 = plot_aux_memory(aux_bytes)
    print(f"wrote {out1}")
    print(f"wrote {out2}")
    print(f"wrote {out3}")


if __name__ == "__main__":
    main()
