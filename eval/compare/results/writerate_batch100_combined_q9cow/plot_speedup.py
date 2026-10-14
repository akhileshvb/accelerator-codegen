#!/usr/bin/env python3
"""Workload speedup over DuckDB vs offered write load, one panel (data from plot_writerate.py's loader).

  python3 plot_speedup.py <out_dir>
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot_writerate import ROWS, load, stat, errplot, workload_speedup, plt  # noqa: E402

SYSTEMS = [("cpp", "C++ accelerators", "#2563eb", "o-"), ("verus_cpp", "Verus, C++ store protocol", "#d97706", "s--"),
           ("verus", "Verus, Verus store protocol", "#dc2626", "^-")]


def main():
    out = sys.argv[1]
    runs = load(out)
    order = sorted(runs, key=lambda n: float(n[3:]))
    xs = {n: i for i, n in enumerate(order)}
    plt.rcParams.update({"font.size": 18, "axes.titlesize": 21, "axes.labelsize": 19, "xtick.labelsize": 17,
                         "ytick.labelsize": 17, "legend.fontsize": 15, "lines.markersize": 10, "lines.linewidth": 2.6})
    fig, ax = plt.subplots(figsize=(9, 6.5))
    for s, label, color, fmt in SYSTEMS:
        pts = [(xs[n], n, runs[n][s]) for n in order if s in runs[n]]
        sp = []
        for _, n, rs in pts:
            vals = [workload_speedup(b, r) for r in rs
                    for b in runs[n].get("base", []) if b.get("rep") == r.get("rep")]
            sp.append(stat(vals))
        errplot(ax, [p[0] for p in pts], sp, fmt=fmt, color=color, label=label)
        # cpp above-left, Verus/C++ protocol above-right, Verus/Verus protocol below: the three never share a spot
        (dx, dy, ha) = {"cpp": (-7, 9, "right"), "verus_cpp": (7, 9, "left"), "verus": (0, -22, "center")}[s]
        for (xv, n, _), t in zip(pts, sp):
            print(f"{n:>8} {label:>28} {t[0]:6.2f}x [{t[1]:.2f}-{t[2]:.2f}]")
            # the first cpp point stands alone far above the rest: centre it there, clear of the y axis
            lx, lh = (0, "center") if (s == "cpp" and xv == 0) else (dx, ha)
            ax.annotate("%.1fx" % t[0], (xv, t[0]), textcoords="offset points", xytext=(lx, dy), ha=lh,
                        fontsize=15, color=color, fontweight="bold", zorder=10,
                        bbox=dict(boxstyle="round,pad=0.15", fc="white", ec="none", alpha=0.85))
    ax.set_title("Workload speedup over DuckDB\n↑ Higher is better")
    ax.set_xlabel("Rows updated per second")
    ax.set_xticks([xs[n] for n in order], [f"{float(n[3:]) * ROWS:,.0f}" for n in order])
    ax.set_yscale("log")
    ax.margins(x=0.13, y=0.2)
    ax.grid(alpha=0.3)
    ax.legend()
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(os.path.join(out, f"writerate_speedup.{ext}"), dpi=150)
    print("wrote " + os.path.join(out, "writerate_speedup.{png,pdf}"))


if __name__ == "__main__":
    main()
