#!/usr/bin/env python3
"""Plot writer→reader interference matrix as a retention heatmap."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path

import matplotlib.pyplot as plt
import numpy as np

QUERY_ORDER = [1, 3, 6, 10, 12, 14]
WRITER_ORDER = [
    "idle",
    "round-robin",
    "target-Q1",
    "target-Q3",
    "target-Q6",
    "target-Q10",
    "target-Q12",
    "target-Q14",
]

WRITER_LABELS = {
    "idle": "No writer",
    "round-robin": "Round-robin",
    "target-Q1": "Writer → Q1",
    "target-Q3": "Writer → Q3",
    "target-Q6": "Writer → Q6",
    "target-Q10": "Writer → Q10",
    "target-Q12": "Writer → Q12",
    "target-Q14": "Writer → Q14",
}


def load_rows(csv_path: Path) -> list[dict[str, str]]:
    with csv_path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def compute_retention(rows: list[dict[str, str]]) -> list[dict[str, str]]:
    idle_p50: dict[int, float] = {}
    for row in rows:
        if row["writer_label"] != "idle":
            continue
        idle_p50[int(row["reader_query"])] = float(row["accel_p50_us"])

    out: list[dict[str, str]] = []
    for row in rows:
        q = int(row["reader_query"])
        accel = float(row["accel_p50_us"])
        idle = idle_p50.get(q, accel)
        retention = idle / accel if accel > 0 else 1.0
        row = dict(row)
        row["retention_vs_idle"] = f"{retention:.4f}"
        out.append(row)
    return out


def plot_heatmap(rows: list[dict[str, str]], out_path: Path, title: str) -> None:
    matrix: dict[tuple[str, int], float] = {}
    meta: dict[str, dict[str, float]] = {}
    for row in rows:
        label = row["writer_label"]
        q = int(row["reader_query"])
        matrix[(label, q)] = float(row["retention_vs_idle"])
        meta[label] = {
            "commits": float(row["writer_commits"]),
            "eff_write_pct": float(row["effective_write_pct"]),
        }

    load_writers = [w for w in WRITER_ORDER if w != "idle" and any(k[0] == w for k in matrix)]
    data = np.array([[matrix.get((w, q), np.nan) for q in QUERY_ORDER] for w in load_writers])

    vmin = max(0.94, float(np.nanmin(data)) - 0.005)
    vmax = min(1.06, float(np.nanmax(data)) + 0.005)

    fig, ax = plt.subplots(figsize=(10, 6.5), dpi=150)
    fig.patch.set_facecolor("#fafafa")
    ax.set_facecolor("#fafafa")

    im = ax.imshow(data, cmap="RdYlGn", vmin=vmin, vmax=vmax, aspect="auto")
    cbar = fig.colorbar(im, ax=ax, fraction=0.046, pad=0.04)
    cbar.set_label("Latency retention vs idle (1.0 = no slowdown)", fontsize=11)

    ax.set_xticks(range(len(QUERY_ORDER)))
    ax.set_xticklabels([f"Reader Q{q}" for q in QUERY_ORDER], fontsize=12, fontweight="bold")
    ax.set_yticks(range(len(load_writers)))
    ylabels = []
    for w in load_writers:
        m = meta.get(w, {})
        ylabels.append(
            f"{WRITER_LABELS.get(w, w)}\n"
            f"({m.get('commits', 0):.0f} commits, {m.get('eff_write_pct', 0):.2f}% writes)"
        )
    ax.set_yticklabels(ylabels, fontsize=10)
    ax.set_xlabel("Reader accelerator (concurrent)", fontsize=11)
    ax.set_ylabel("Background SQL writer", fontsize=11)
    ax.set_title(title, fontsize=13, fontweight="bold", pad=12)

    for i, w in enumerate(load_writers):
        for j, q in enumerate(QUERY_ORDER):
            val = data[i, j]
            if np.isnan(val):
                continue
            writer_q = int(w.replace("target-Q", "")) if w.startswith("target-") else None
            on_diag = writer_q == q
            color = "white" if val < (vmin + 0.02) else "black"
            weight = "bold" if on_diag else "normal"
            ax.text(j, i, f"{val:.3f}", ha="center", va="center", color=color, fontsize=10, fontweight=weight)
            if on_diag:
                ax.add_patch(
                    plt.Rectangle((j - 0.48, i - 0.48), 0.96, 0.96, fill=False, edgecolor="#111", linewidth=2.2)
                )

    fig.text(
        0.5,
        0.01,
        "SQL invalidating UPDATEs · writer_sleep_ms=200 · ~0.02% write rate · "
        "boxed cells = writer targets same query as reader",
        ha="center",
        fontsize=9,
        color="#555",
    )
    fig.subplots_adjust(bottom=0.12)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(out_path, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {out_path}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv", nargs="?", default="build-release/cross_accel_interference_matrix.csv")
    parser.add_argument("png", nargs="?", default="build-release/cross_accel_interference_matrix.png")
    parser.add_argument(
        "--title",
        default="Cross-accelerator isolation (SQL writers, concurrent readers)",
    )
    args = parser.parse_args()

    csv_path = Path(args.csv)
    rows = compute_retention(load_rows(csv_path))

    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)

    plot_heatmap(rows, Path(args.png), args.title)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
