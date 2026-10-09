#!/usr/bin/env python3
"""Plot a MODE=multi run_rf_interval_sweep.sh directory (the 22 accelerators in one process, full TPC-H refresh sets).

  python3 benchmarks/compare/plot_rf_multi_sweep.py <out_dir> [--intervals "90 60 30"] [--rf-dir DIR]

<out_dir>/T<interval>s/rep<k>/multi_<system>.json.  Points are medians over the repetitions, bars their min..max.
Each interval is labelled by the rows its refresh stream touches per second (RF1 inserts the update set's orders and
lineitems, RF2 deletes as many orders with theirs; counted from the RF1 files, the point writes are not counted).
Writes <out_dir>/rf_multi_sweep.png (the panels of plot_wps_sweep.py against the refresh interval) and
<out_dir>/rf_multi_per_query.png (per query, workload speedup over DuckDB under the refresh stream,
workload.py's workload_speedup_per_query, one bar per interval) and prints both tables.
"""
import argparse
import glob
import json
import math
import os
import re
import statistics

import matplotlib.pyplot as plt

from plot_wps_sweep import errplot, stat
from workload import key, workload_speedup, workload_speedup_per_query

SYSTEMS = [("base", "DuckDB", "#6b7280"), ("stateless", "Stateless", "#0891b2"), ("cpp", "C++", "#2563eb"),
           ("verus_cpp", "Verus per-accelerator store", "#d97706"), ("verus", "Verus global store", "#dc2626")]


def rows_per_slot(rf_dir):
    """Mean rows of one refresh commit: orders + lineitems of an RF1 update set."""
    n = []
    for f in sorted(glob.glob(os.path.join(rf_dir, "orders.tbl.u*")))[:5]:
        k = f.rsplit(".u", 1)[1]
        n.append(sum(1 for _ in open(f)) + sum(1 for _ in open(os.path.join(rf_dir, f"lineitem.tbl.u{k}"))))
    return statistics.mean(n) if n else None


def load(out, keep):
    runs = {}  # T -> system -> [result per repetition]
    for d in sorted(glob.glob(os.path.join(out, "T*s"))):
        m = re.match(r"T([0-9.]+)s$", os.path.basename(d))
        if not m or not os.path.isdir(d) or (keep and float(m.group(1)) not in keep):
            continue
        for f in sorted(glob.glob(os.path.join(d, "rep*", "multi_*.json"))):
            r = json.load(open(f))
            if "error" not in r:
                runs.setdefault(float(m.group(1)), {}).setdefault(r.get("system") or
                                                                   os.path.basename(f)[6:-5], []).append(r)
    return runs


def rep_of(rs, rep):
    return next((x for x in rs if x.get("rep") == rep), None)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--intervals", default=None, help="only these intervals (seconds), e.g. \"90 60 30\"")
    ap.add_argument("--rf-dir", default="/flash1/akhilvb/tpch/refresh_sf20")
    a = ap.parse_args()
    keep = {float(x) for x in a.intervals.split()} if a.intervals else None
    runs = load(a.out, keep)
    if not runs:
        raise SystemExit(f"no T*s/rep*/multi_*.json under {a.out}")
    rows = rows_per_slot(a.rf_dir)
    Ts = sorted(runs, reverse=True)  # lightest write load first
    xs = {T: i for i, T in enumerate(Ts)}
    ticks = [f"{T:g}s\n{rows / T / 1e3:,.1f}k rows/s" if rows else f"{T:g}s" for T in Ts]

    fig, ax = plt.subplots(1, 5, figsize=(32, 7))
    print(f"{'T':>5} {'system':>28} {'RF/min':>7} {'commit p50':>11} {'p99':>10} {'read p99*':>10} {'wait/read':>10} "
          f"{'qps':>7} {'speedup':>8}")
    for s, label, color in SYSTEMS:
        pts = [(xs[T], T, runs[T][s]) for T in Ts if s in runs[T]]
        if not pts:
            continue
        x = [p[0] for p in pts]

        def per(f):
            return [stat([f(r) for r in rs]) for _, _, rs in pts]

        rf = per(lambda r: 60 * sum(v for k, v in r["write_kinds"].items() if k.startswith("rf")) / r["seconds"])
        c50 = per(lambda r: r["commit_p50_us"] / 1000)
        c99 = per(lambda r: r["commit_p99_us"] / 1000)
        r99 = per(lambda r: statistics.median(q["read_p99_us"] for q in r["per_accel"] if q["reads"]) / 1000
                  if any(q["reads"] for q in r["per_accel"]) else None)
        wait = per(lambda r: sum(q.get("store_wait_mean_us", 0) * q["reads"] for q in r["per_accel"]) /
                   max(1, sum(q["reads"] for q in r["per_accel"])) / 1000)
        qps = per(lambda r: r["qps"])
        sp = []
        for _, T, rs in pts:
            vals = []
            for r in rs:  # paired with the same repetition's C++ and DuckDB runs
                base = rep_of(runs[T].get("base", []), r.get("rep"))
                if base and s != "base":
                    vals.append(workload_speedup(base, r))
            sp.append(stat(vals))
        errplot(ax[0], x, rf, fmt="o-", color=color, label=label)
        errplot(ax[1], x, c50, fmt="o-", color=color, label=label)
        errplot(ax[2], x, r99, fmt="o-", color=color, label=label)
        if s != "base":
            errplot(ax[3], x, wait, fmt="o-", color=color, label=label)
            errplot(ax[4], x, sp, fmt="o-", color=color, label=label)
            dy = {"stateless": -20, "cpp": 10, "verus_cpp": 10, "verus": -20}[s]
            for xv, t in zip(x, sp):
                if t:
                    ax[4].annotate("%.0fx" % t[0], (xv, t[0]), textcoords="offset points", xytext=(0, dy),
                                   ha="center", fontsize=12, color=color)
        f = lambda t, fmt: (fmt % t[0]) if t else "-"
        for (_, T, rs), a_, p50, p99, rp, w, q, v in zip(pts, rf, c50, c99, r99, wait, qps, sp):
            print(f"{T:>4g}s {label:>28} {f(a_, '%7.2f')} {f(p50, '%9.1f')}ms {f(p99, '%8.1f')}ms {f(rp, '%8.2f')}ms "
                  f"{f(w, '%8.3f')}ms {f(q, '%7.0f')} {f(v, '%.0fx'):>8}  ({len(rs)} rep)")
    ax[0].plot([xs[T] for T in Ts], [60 / T for T in Ts], "x:", color="#9ca3af", label="Target")
    up, down = "\n↑ Higher is better", "\n↓ Lower is better"
    titles = ["Refresh commits per minute" + up, "Commit latency p50 (ms)" + down,
              "Read p99 (ms), median over accelerators" + down, "Store wait per read (ms)" + down,
              "Workload speedup over DuckDB" + up]
    for a_, t in zip(ax, titles):
        a_.set_title(t)
        a_.set_xlabel("Refresh interval")
        a_.set_xticks(range(len(Ts)), ticks)
        a_.grid(alpha=0.3)
        a_.legend()
    for a_ in ax[1:5]:
        a_.set_yscale("log")
    ax[4].margins(y=0.15)
    fig.tight_layout()
    png = os.path.join(a.out, "rf_multi_sweep.png")
    fig.savefig(png, dpi=120)
    print("wrote " + png)
    per_query(a.out, runs, Ts, ticks)


def per_query(out, runs, Ts, ticks):
    """Per query and interval: workload_speedup_per_query (workload.py), median over repetitions."""
    systems = [s for s in SYSTEMS if s[0] != "base"]
    qs = sorted({int(key(p)) for T in Ts for rs in runs[T].values() for r in rs for p in r["per_accel"]})
    fig, axs = plt.subplots(len(systems), 1, figsize=(26, 5.5 * len(systems)), sharex=True)
    shades = ["#93c5fd", "#3b82f6", "#1d4ed8", "#1e3a8a", "#0b1d4d"]
    width = 0.84 / len(Ts)
    labels = [f"Q{q}" for q in qs] + ["Geomean"]
    print(f"\nper-query workload speedup over DuckDB (workload.py, median over repetitions)")
    for ax, (s, label, _) in zip(axs, systems):
        print(f"{label}: " + "  ".join(f"T={T:g}s" for T in Ts))
        for j, T in enumerate(Ts):
            ys = []
            for q in qs:
                vals = []
                for r in runs[T].get(s, []):
                    base = rep_of(runs[T].get("base", []), r.get("rep"))
                    v = workload_speedup_per_query(base, r).get(str(q)) if base else None
                    if v:
                        vals.append(v)
                ys.append(statistics.median(vals) if vals else None)
            got = [y for y in ys if y]
            gm = math.exp(sum(map(math.log, got)) / len(got)) if got else None
            off = (j - (len(Ts) - 1) / 2) * width
            ax.bar([i + off for i in range(len(labels))], [y or 0 for y in ys] + [gm or 0], width * 0.92,
                   color=shades[j % len(shades)], label=ticks[j].replace("\n", ", "))
            if gm:
                ax.text(len(qs) + off, gm * 1.15, f"{gm:.0f}x", ha="center", va="bottom", rotation=90, fontsize=11)
            print(f"  T={T:g}s: " + " ".join(f"q{q}={y:.0f}x" if y else f"q{q}=-" for q, y in zip(qs, ys))
                  + (f"  gm={gm:.0f}x ({len(got)}/{len(qs)} queries read)" if gm else "  gm=-"))
        ax.set_yscale("log")
        ax.axhline(1, color="#6b7280", ls="--", lw=1)
        ax.set_ylabel("Workload speedup over DuckDB")
        ax.set_title(f"{label}: per-query workload speedup over DuckDB under the refresh stream (missing bar = no reads)")
        ax.grid(axis="y", alpha=0.3)
        ax.legend(ncol=len(Ts), loc="upper left")
    axs[-1].set_xticks(range(len(labels)), labels)
    fig.tight_layout()
    png = os.path.join(out, "rf_multi_per_query.png")
    fig.savefig(png, dpi=110)
    print("wrote " + png)


if __name__ == "__main__":
    main()
