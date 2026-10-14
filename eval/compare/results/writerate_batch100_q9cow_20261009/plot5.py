#!/usr/bin/env python3
"""Plot a write-rate sweep of the multi experiment (run_compare.py multi --wps "5 10 ...").

  python3 benchmarks/compare/plot_wps_sweep.py <out_dir>

<out_dir>/<rate>/[rep<k>/]multi_<system>.json (rate: wps5, wps10, ...; an uncapped/ run is ignored; one rep<k>
per repetition, each with its own parameter draw).  Points are medians over the repetitions,
bars their min..max.  Writes
<out_dir>/wps_sweep.png and prints a table.  Panels, against the target write rate:
  1. achieved write rate (a writer that falls below the diagonal is saturated)
  2. commit latency p50
  3. read latency p99, median over the accelerators
  4. time readers spent in the store waiting for the state (lock, pending commit),
     mean per read over all accelerators
  5. workload speedup over DuckDB (workload.py: the reads the DuckDB run completed + each system's commit time)
"""
import json
# ~rows touched per write at --batch-orders 100: 4/5 refresh commits of 100 orders (RF2 ~500 rows,
# RF1 ~575 with every 4th order a 7-line large one) + 1/5 point writes (~20 rows): 0.8*537 + 0.2*20
ROWS = 434
import os
import statistics
import sys

import matplotlib

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
from workload import workload_speedup  # noqa: E402

matplotlib.use("Agg")
import matplotlib.pyplot as plt

plt.rcParams.update({"font.size": 15, "axes.titlesize": 17, "axes.labelsize": 15, "xtick.labelsize": 14,
                     "ytick.labelsize": 14, "legend.fontsize": 12, "lines.markersize": 8, "lines.linewidth": 2.2})

SYSTEMS = [("base", "DuckDB", "#6b7280"), ("cpp", "C++ accelerators (old Q9)", "#93c5fd"), ("cpp_q9cow", "C++ accelerators (Q9 fixed)", "#2563eb"), ("verus_cpp", "Verus, C++ store protocol", "#d97706"),
           ("verus", "Verus, Verus store protocol", "#dc2626")]


def rate_value(name, achieved):
    return float(name[3:]) if name.startswith("wps") else None


def load(out):
    runs = {}  # rate name -> system -> [result per repetition]
    for d in sorted(os.listdir(out)):
        rd = os.path.join(out, d)
        if not d.startswith("wps") or not os.path.isdir(rd):  # the uncapped runs are not plotted
            continue
        reps = [os.path.join(rd, x) for x in sorted(os.listdir(rd)) if x.startswith("rep")] or [rd]
        for rep in reps:
            for s, _, _ in SYSTEMS:
                p = os.path.join(rep, f"multi_{s}.json")
                if os.path.exists(p):
                    r = json.load(open(p))
                    if "error" not in r:
                        runs.setdefault(d, {}).setdefault(s, []).append(r)
    return runs


def stat(values):
    """(median, low, high) of the repetitions' values."""
    v = sorted(x for x in values if x is not None)
    if not v:
        return None
    return statistics.median(v), v[0], v[-1]


def errplot(ax, xs, stats, **kw):
    pts = [(x, s) for x, s in zip(xs, stats) if s]
    if not pts:
        return
    ax.errorbar([x for x, _ in pts], [s[0] for _, s in pts],
                yerr=[[s[0] - s[1] for _, s in pts], [s[2] - s[0] for _, s in pts]], capsize=3, **kw)


def main():
    out = sys.argv[1]
    runs = load(out)
    if not runs:
        sys.exit(f"no <rate>/multi_*.json under {out}")
    # x: the target rates, evenly spaced
    capped = sorted((rate_value(n, None), n) for n in runs if n != "uncapped")
    order = [n for _, n in capped]
    xs = {n: i for i, n in enumerate(order)}  # evenly spaced, so the low rates do not crowd
    fig, ax = plt.subplots(1, 5, figsize=(32, 7))
    print(f"{'rate':>9} {'system':>20} {'wps':>7} {'commit p50':>11} {'p99':>9} {'read p99*':>10} "
          f"{'wait/read':>10} {'speedup':>8}")
    for s, label, color in SYSTEMS:
        pts = [(xs[n], n, runs[n][s]) for n in order if s in runs[n]]
        if not pts:
            continue
        x = [p[0] for p in pts]

        def per(f):
            return [stat([f(r) for r in rs]) for _, _, rs in pts]

        ach = per(lambda r: r["writes_per_sec"] * ROWS)
        c50 = per(lambda r: r["commit_p50_us"] / 1000)
        c99 = per(lambda r: r["commit_p99_us"] / 1000)
        r99 = per(lambda r: statistics.median(q["read_p99_us"] for q in r["per_accel"]) / 1000)
        wait = per(lambda r: sum(q.get("store_wait_mean_us", 0) * q["reads"] for q in r["per_accel"]) /
                   max(1, sum(q["reads"] for q in r["per_accel"])) / 1000)
        if False:  # dotted, over the global store's line, which it coincides with
            errplot(ax[0], x, ach, fmt="o:", color=color, label=label, lw=3, ms=5, zorder=5)
        else:
            errplot(ax[0], x, ach, fmt="o-", color=color, label=label)
        errplot(ax[1], x, c50, fmt="o-", color=color, label=label)
        errplot(ax[2], x, r99, fmt="o-", color=color, label=label)
        if s != "base":
            errplot(ax[3], x, wait, fmt="o-", color=color, label=label)
        sp = []
        for _, n, rs in pts:
            vals = []
            for r in rs:  # paired with the same repetition's C++ and DuckDB runs
                base = next((b for b in runs[n].get("base", []) if b.get("rep") == r.get("rep")), None)
                if base and s != "base":
                    vals.append(workload_speedup(base, r))
            sp.append(stat(vals))
        if s != "base":
            errplot(ax[4], x, sp, fmt="o-", color=color, label=label)
            dy = {"cpp": -20, "cpp_q9cow": 10, "verus_cpp": 10, "verus": -20}[s]  # keep the two Verus labels apart
            for xv, t in zip(x, sp):
                if t:
                    ax[4].annotate("%.0fx" % t[0], (xv, t[0]), textcoords="offset points", xytext=(0, dy),
                                   ha="center", fontsize=12, color=color)
        for (xv, n, rs), a_, p50, p99, rp, w, v in zip(pts, ach, c50, c99, r99, wait, sp):
            f = lambda t, fmt: (fmt % t[0]) if t else "-"
            print(f"{n:>9} {label:>20} {f(a_, '%7.1f')} {f(p50, '%9.1f')}ms {f(p99, '%7.1f')}ms {f(rp, '%8.2f')}ms "
                  f"{f(w, '%8.3f')}ms {f(v, '%.0fx'):>8}  ({len(rs)} rep)")
    ax[0].plot([xs[n] for _, n in capped], [w * ROWS for w, _ in capped], "x:", color="#9ca3af", label="Target")
    up, down = "\n↑ Higher is better", "\n↓ Lower is better"
    titles = ["Achieved rows/s written" + up, "Commit latency p50 (ms)" + down, "Read p99 (ms), median over accelerators" + down,
              "Store wait per read (ms)" + down, "Workload speedup over DuckDB" + up]
    for a, t in zip(ax, titles):
        a.set_title(t)
        a.set_xlabel("Offered load, rows touched/s (target writes/s)")
        a.set_xticks([xs[n] for n in order], [f"{float(n[3:]) * ROWS:,.0f}\n({n[3:]}/s)" for n in order])
        a.grid(alpha=0.3)
        a.legend()
    ax[0].set_yscale("log")
    for a in ax[1:4]:
        a.set_yscale("log")
    ax[4].set_yscale("log")
    ax[4].margins(y=0.15)
    fig.tight_layout()
    png = os.path.join(out, "batch_sweep_q9cow.png")
    fig.savefig(png, dpi=120)
    print("wrote " + png)


if __name__ == "__main__":
    main()
