#!/usr/bin/env python3
"""Plot a multi run (the 22 accelerators in one process) swept over the write load: run_rf_interval_sweep.sh MODE=multi
(full TPC-H refresh sets) or a `run_compare.py multi --wps "5 10 ..."` write-rate sweep.

  python3 eval/compare/plot_rf_multi_sweep.py <out_dir> [--intervals "90 60 30"] [--rf-dir DIR]

Interval sweep: <out_dir>/T<interval>s/rep<k>/multi_<system>.json.  Each interval is labelled by the rows its refresh
stream touches per second (RF1 inserts the update set's orders and lineitems, RF2 deletes as many orders with theirs;
counted from the RF1 files, the point writes are not counted).  Writes <out_dir>/rf_multi_sweep.png and
<out_dir>/rf_multi_per_query.png (per query, workload speedup over DuckDB under the refresh stream,
workload.py's workload_speedup_per_query, one bar per interval) and prints both tables.
Write-rate sweep (a <out_dir> with wps<rate>/ subdirectories, [rep<k>/]multi_<system>.json inside; an uncapped/ run is
ignored): writes <out_dir>/wps_sweep.png and prints a table.
Points are medians over the repetitions (each its own parameter draw), bars their min..max.  Panels, against the
target write load:
  1. achieved write rate (refresh commits per minute / writes per second; below the target = the writer is saturated)
  2. commit latency p50
  3. read latency p99, median over the accelerators
  4. time readers spent in the store waiting for the state (lock, pending commit), mean per read over all accelerators
  5. workload speedup over DuckDB (workload.py: the reads the DuckDB run completed + each system's commit time)
"""
import argparse
import glob
import json
import math
import os
import re
import statistics

import matplotlib.pyplot as plt

from plotlib import errplot, stat
from workload import key, workload_speedup, workload_speedup_per_query

plt.rcParams.update({"font.size": 15, "axes.titlesize": 17, "axes.labelsize": 15, "xtick.labelsize": 14,
                     "ytick.labelsize": 14, "legend.fontsize": 12, "lines.markersize": 8, "lines.linewidth": 2.2})

# verus_kit (rate sweeps) and verus_cpp (interval sweeps) are the same system under two names
SYSTEMS = [("base", "DuckDB", "#6b7280"), ("stateless", "Stateless", "#0891b2"), ("cpp", "C++", "#2563eb"),
           ("verus_kit", "Verus per-accelerator store", "#d97706"),
           ("verus_cpp", "Verus per-accelerator store", "#d97706"), ("verus", "Verus global store", "#dc2626")]
LABEL_DY = {"stateless": -20, "cpp": 10, "verus_kit": 10, "verus_cpp": 10, "verus": -20}  # keeps the Verus labels apart


def rows_per_slot(rf_dir):
    """Mean rows of one refresh commit: orders + lineitems of an RF1 update set."""
    n = []
    for f in sorted(glob.glob(os.path.join(rf_dir, "orders.tbl.u*")))[:5]:
        k = f.rsplit(".u", 1)[1]
        n.append(sum(1 for _ in open(f)) + sum(1 for _ in open(os.path.join(rf_dir, f"lineitem.tbl.u{k}"))))
    return statistics.mean(n) if n else None


def load(out, keep):
    """Interval sweep: T (seconds) -> system -> [result per repetition]."""
    runs = {}
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


def load_rates(out):
    """Write-rate sweep: rate dir name (wps5, ...) -> system -> [result per repetition]; uncapped runs are not plotted."""
    runs = {}
    for d in sorted(os.listdir(out)):
        rd = os.path.join(out, d)
        if not d.startswith("wps") or not os.path.isdir(rd):
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


def rep_of(rs, rep):
    return next((x for x in rs if x.get("rep") == rep), None)


def sweep_panels(runs, order, ticks, xlabel, achieved, ach_title, target, png, row_fmt, header, with_qps):
    """The five panels over `order` (the sweep's x values, lightest write load first), written to png."""
    xs = {n: i for i, n in enumerate(order)}  # evenly spaced, so the low loads do not crowd
    fig, ax = plt.subplots(1, 5, figsize=(32, 7))
    print(header)
    for s, label, color in SYSTEMS:
        pts = [(xs[n], n, runs[n][s]) for n in order if s in runs[n]]
        if not pts:
            continue
        x = [p[0] for p in pts]

        def per(f):
            return [stat([f(r) for r in rs]) for _, _, rs in pts]

        ach = per(achieved)
        c50 = per(lambda r: r["commit_p50_us"] / 1000)
        c99 = per(lambda r: r["commit_p99_us"] / 1000)
        r99 = per(lambda r: statistics.median(q["read_p99_us"] for q in r["per_accel"] if q["reads"]) / 1000
                  if any(q["reads"] for q in r["per_accel"]) else None)
        wait = per(lambda r: sum(q.get("store_wait_mean_us", 0) * q["reads"] for q in r["per_accel"]) /
                   max(1, sum(q["reads"] for q in r["per_accel"])) / 1000)
        qps = per(lambda r: r["qps"]) if with_qps else [None] * len(pts)
        sp = []
        for _, n, rs in pts:
            vals = []
            for r in rs:  # paired with the same repetition's DuckDB run
                base = rep_of(runs[n].get("base", []), r.get("rep"))
                if base and s != "base":
                    vals.append(workload_speedup(base, r))
            sp.append(stat(vals))
        if s == "verus_kit":  # dotted, over the global store's line, which it coincides with
            errplot(ax[0], x, ach, fmt="o:", color=color, label=label, lw=3, ms=5, zorder=5)
        else:
            errplot(ax[0], x, ach, fmt="o-", color=color, label=label)
        errplot(ax[1], x, c50, fmt="o-", color=color, label=label)
        errplot(ax[2], x, r99, fmt="o-", color=color, label=label)
        if s != "base":
            errplot(ax[3], x, wait, fmt="o-", color=color, label=label)
            errplot(ax[4], x, sp, fmt="o-", color=color, label=label)
            for xv, t in zip(x, sp):
                if t:
                    ax[4].annotate("%.0fx" % t[0], (xv, t[0]), textcoords="offset points", xytext=(0, LABEL_DY[s]),
                                   ha="center", fontsize=12, color=color)
        f = lambda t, fmt: (fmt % t[0]) if t else "-"
        for (_, n, rs), a_, p50, p99, rp, w, q, v in zip(pts, ach, c50, c99, r99, wait, qps, sp):
            print(row_fmt(n, label, f, a_, p50, p99, rp, w, q, v, len(rs)))
    ax[0].plot([xs[n] for n in order], [target(n) for n in order], "x:", color="#9ca3af", label="Target")
    up, down = "\n↑ Higher is better", "\n↓ Lower is better"
    titles = [ach_title + up, "Commit latency p50 (ms)" + down, "Read p99 (ms), median over accelerators" + down,
              "Store wait per read (ms)" + down, "Workload speedup over DuckDB" + up]
    for a_, t in zip(ax, titles):
        a_.set_title(t)
        a_.set_xlabel(xlabel)
        a_.set_xticks(range(len(order)), ticks)
        a_.grid(alpha=0.3)
        a_.legend()
    for a_ in ax[1:5]:
        a_.set_yscale("log")
    ax[4].margins(y=0.15)
    fig.tight_layout()
    fig.savefig(png, dpi=120)
    print("wrote " + png)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--intervals", default=None, help="only these intervals (seconds), e.g. \"90 60 30\"")
    ap.add_argument("--rf-dir", default="/flash1/akhilvb/tpch/refresh_sf20")
    a = ap.parse_args()
    keep = {float(x) for x in a.intervals.split()} if a.intervals else None
    runs = load(a.out, keep)
    if not runs:
        rates = load_rates(a.out)
        if not rates:
            raise SystemExit(f"no T*s/rep*/multi_*.json or wps*/multi_*.json under {a.out}")
        order = [n for _, n in sorted((float(n[3:]), n) for n in rates)]
        sweep_panels(
            rates, order, [n.replace("wps", "") for n in order], "Target writes/s",
            lambda r: r["writes_per_sec"], "Achieved writes/s", lambda n: float(n[3:]),
            os.path.join(a.out, "wps_sweep.png"),
            lambda n, label, f, a_, p50, p99, rp, w, q, v, nrep:
                f"{n:>9} {label:>20} {f(a_, '%7.1f')} {f(p50, '%9.1f')}ms {f(p99, '%7.1f')}ms {f(rp, '%8.2f')}ms "
                f"{f(w, '%8.3f')}ms {f(v, '%.0fx'):>8}  ({nrep} rep)",
            f"{'rate':>9} {'system':>20} {'wps':>7} {'commit p50':>11} {'p99':>9} {'read p99*':>10} "
            f"{'wait/read':>10} {'speedup':>8}", False)
        return
    rows = rows_per_slot(a.rf_dir)
    Ts = sorted(runs, reverse=True)  # lightest write load first
    ticks = [f"{T:g}s\n{rows / T / 1e3:,.1f}k rows/s" if rows else f"{T:g}s" for T in Ts]
    sweep_panels(
        runs, Ts, ticks, "Refresh interval",
        lambda r: 60 * sum(v for k, v in r["write_kinds"].items() if k.startswith("rf")) / r["seconds"],
        "Refresh commits per minute", lambda T: 60 / T, os.path.join(a.out, "rf_multi_sweep.png"),
        lambda T, label, f, a_, p50, p99, rp, w, q, v, nrep:
            f"{T:>4g}s {label:>28} {f(a_, '%7.2f')} {f(p50, '%9.1f')}ms {f(p99, '%8.1f')}ms {f(rp, '%8.2f')}ms "
            f"{f(w, '%8.3f')}ms {f(q, '%7.0f')} {f(v, '%.0fx'):>8}  ({nrep} rep)",
        f"{'T':>5} {'system':>28} {'RF/min':>7} {'commit p50':>11} {'p99':>10} {'read p99*':>10} {'wait/read':>10} "
        f"{'qps':>7} {'speedup':>8}", True)
    per_query(a.out, runs, Ts, ticks)


def per_query(out, runs, Ts, ticks):
    """Per query and interval: workload_speedup_per_query (workload.py), median over repetitions."""
    systems = [s for s in SYSTEMS if s[0] not in ("base", "verus_kit")]
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
