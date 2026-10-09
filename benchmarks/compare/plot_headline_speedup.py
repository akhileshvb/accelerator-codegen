#!/usr/bin/env python3
"""Headline figures: TPC-H SF20 geomean speedup over DuckDB, read-only and under a refresh stream.

Read-only (one bar per system, geomean over Q1-Q22 of the per-query speedup):
  JetStream stateless / stateful / stateful-verified and the GenDB / Bespoke baselines, single-threaded
  (--readonly-threads st), against multi-threaded DuckDB (mt) or each at its best (best, the default); sources in
  READONLY below.
Under writes (geomean over Q1-Q22 of workload.py's workload speedup, median over repetitions with min..max
whiskers), from a run_rf_interval_sweep.sh concurrent directory (--rf-conc): per query, ONE accelerator in the process,
8 single-threaded readers on it and one writer applying the TPC-H refresh stream at the chosen interval, DuckDB
threads=1; each system paired with that directory's DuckDB (base) run of the same query, repetition and draws.
--writes-mode multi uses a MODE=multi directory instead (--rf-out: every accelerator of a system live in ONE process,
one writer maintaining all of them). GenDB and Bespoke have no maintenance path, so a refresh sends them back to
DuckDB (1x). A system with no runs is marked "not measured".

  plot_headline_speedup.py [--interval 60] [--writes-mode concurrent|multi] [--rf-conc DIR] [--rf-out DIR]
                           [--out-prefix PREFIX]
"""
import argparse
import csv
import glob
import json
import os
import statistics
import sys

import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from plot_readonly_speedup import BESPOKE_ST, STATEFUL_DIR, STATELESS_CSV, geomean  # noqa: E402
from plot_rf_multi_sweep import load, rep_of, rows_per_slot  # noqa: E402
from workload import workload_speedup, workload_speedup_per_query  # noqa: E402

RF_OUT = os.path.join(HERE, "results/rf_interval_multi_sf20_20261008_112014")
# DuckDB and C++ from the sweep; Verus and stateless from run_headline_T15.sh (same draw and stream, --no-base)
RF_CONC = [os.path.join(HERE, "results/rf_interval_sf20_20261007_222743"),
           os.path.join(HERE, "results/rf_interval_sf20_T15_headline")]
RF_DIR = "/flash1/akhilvb/tpch/refresh_sf20"

# Read-only panel per thread setting.  GenDB: its reported TPC-H geomean speedups over DuckDB, single-threaded and
# multi-threaded.  Stateless: benchmarks/tpch/results/stateless_duckdb_sf20_summary.csv, kernel and DuckDB at the
# same thread count.  Stateful: run_compare.py read JSONs; the accelerators' read kernels are single-threaded, DuckDB
# runs single-threaded (ACCEL_COMPARE_DUCKDB_THREADS=1) for "st" and with all cores for "mt".  Bespoke OLAP reports
# single-threaded numbers only.
READONLY = {"st": {"label": "single-threaded", "gendb": 5.4, "stateless_col": ["st_speedup"],
                   "stateful_dir": [os.path.join(HERE, "results/read_duckdb1t_sf20/read")], "bespoke": True},
            "mt": {"label": "DuckDB multi-threaded", "gendb": 34.4, "stateless_col": ["par_speedup_32t"],
                   "stateful_dir": [str(STATEFUL_DIR)], "bespoke": False},
            # each system's best measured configuration: GenDB multi-threaded, stateless at its best thread count,
            # stateful / verified from the complete read run with the highest geomean
            "best": {"label": "best configuration", "gendb": 34.4,
                     "stateless_col": ["st_speedup", "par_speedup_16t", "par_speedup_32t"],
                     "stateful_dir": [os.path.join(HERE, "results", d) for d in
                                      ("backends_20261007_v2/duckdb/read", "cpp_verus_writerate-read/read",
                                       "rf_interval_multi_sf20_20261008_112014/read")],
                     "bespoke": True}}

# (key, label, color); same order in both panels.
SYSTEMS = [("stateless", "JetStream\n(stateless)", "C0"),
           ("bespoke", "Bespoke\nOLAP", "C1"),
           ("gendb", "GenDB", "C2"),
           ("verus", "JetStream\n(stateful,\nverified)", "C3"),
           ("cpp", "JetStream\n(stateful)", "C4")]


def readonly_speedups(cfg):
    """system -> read-only geomean speedup over Q1-Q22 (stateful: median over repetitions per query); a system with
    no or incomplete results is left out."""
    ro = {"gendb": cfg["gendb"]}
    if cfg["bespoke"]:
        ro["bespoke"] = geomean(BESPOKE_ST)
    with open(STATELESS_CSV, newline="") as f:
        rows = [r for r in csv.DictReader(f) if r["query"].startswith("Q")]
    for col in cfg["stateless_col"]:
        st = {int(r["query"][1:]): float(r[col]) for r in rows}
        g = geomean([st[q] for q in range(1, 23)])
        print(f"  stateless {col}: {g:.2f}x")
        ro["stateless"] = max(ro.get("stateless", 0), g)
    for s in ("cpp", "verus"):
        for d in cfg["stateful_dir"]:
            pq = []
            for q in range(1, 23):
                reps = [json.load(open(f)) for f in glob.glob(os.path.join(d, f"read_q{q}_{s}_r*.json"))]
                reps = [r for r in reps if "error" not in r and r.get("speedup_geomean")]
                if any(r["mismatches"] for r in reps):
                    raise SystemExit(f"Q{q} {s} read run in {d} has result mismatches")
                pq.append(statistics.median(r["speedup_geomean"] for r in reps) if reps else None)
            if all(pq):
                print(f"  {s} {os.path.relpath(d, HERE)}: {geomean(pq):.2f}x")
                ro[s] = max(ro.get(s, 0), geomean(pq))
            elif any(pq):
                print(f"warning: read-only {s}: {sum(map(bool, pq))} of 22 queries in {d}, left out")
    return ro


def write_speedups(out, T):
    """system -> (median, min, max) over repetitions of the geomean of the per-query workload speedups, and the median
    refresh rows/s touched (offered: rows per refresh slot / T)."""
    runs = load(out, [T]).get(T, {})
    res = {}
    for s in ("stateless", "cpp", "verus"):
        vals = []
        for r in runs.get(s, []):
            base = rep_of(runs.get("base", []), r.get("rep"))
            pq = workload_speedup_per_query(base, r) if base else {}
            if len(pq) == 22 and all(pq.values()):
                vals.append(geomean(list(pq.values())))
            else:
                print(f"warning: T={T:g}s {s} rep {r.get('rep')}: {len(pq)} of 22 queries, left out")
        if vals:
            res[s] = (statistics.median(vals), min(vals), max(vals))
            print(f"T={T:g}s {s}: {res[s][0]:.1f}x [{res[s][1]:.1f}-{res[s][2]:.1f}] n={len(vals)}")
    return res, rows_per_slot(RF_DIR) / T


def write_speedups_conc(outs, T):
    """As write_speedups, from a concurrent sweep: per repetition the geomean over the 22 queries of each query's
    workload speedup against its own DuckDB run; rows/s is the median touched by the DuckDB runs."""
    runs, rows = {}, []
    for f in (f for out in outs for f in glob.glob(os.path.join(out, f"T{T:g}s", "conc_q*_*_r*.json"))):
        r = json.load(open(f))
        if "error" not in r:
            runs.setdefault(r["system"], {})[(r["query"], r["rep"])] = r
    base = runs.get("base", {})
    rows = [r.get("rows_touched_per_sec", 0) for r in base.values()]
    res = {}
    for s in ("stateless", "cpp", "verus"):
        vals = []
        for k in sorted({k for _, k in runs.get(s, {})}):
            pq = [workload_speedup(base[(q, k)], runs[s][(q, k)]) if (q, k) in base and (q, k) in runs[s] else None
                  for q in range(1, 23)]
            if all(pq):
                vals.append(geomean(pq))
            else:
                print(f"warning: T={T:g}s {s} rep {k}: {sum(map(bool, pq))} of 22 queries, left out")
        if vals:
            res[s] = (statistics.median(vals), min(vals), max(vals))
            print(f"T={T:g}s {s}: {res[s][0]:.1f}x [{res[s][1]:.1f}-{res[s][2]:.1f}] n={len(vals)}")
    return res, statistics.median(rows) if rows else rows_per_slot(RF_DIR) / T


def fmt(v):
    return f"{v:,.0f}×" if v >= 100 else f"{v:.1f}×" if v >= 10 else f"{v:.2f}×"


def panel(ax, values, title, errs=None, missing=(), fallback=()):
    xs = range(len(SYSTEMS))
    hs = [values.get(k, 0) for k, _, _ in SYSTEMS]
    ax.bar(xs, hs, 0.6, color=[c for _, _, c in SYSTEMS])
    for x, (k, _, _) in zip(xs, SYSTEMS):
        if k in missing:
            ax.text(x, 1.1, "n/a", ha="center", va="bottom")
            continue
        top = values[k]
        if errs and k in errs and errs[k][1] < errs[k][2]:
            _, lo, hi = errs[k]
            ax.errorbar([x], [top], yerr=[[top - lo], [hi - top]], fmt="none", ecolor="black", capsize=3)
            top = hi
        label = fmt(values[k]) + ("\n(DuckDB\nfallback)" if k in fallback else "")
        ax.text(x, top * 1.15, label, ha="center", va="bottom")
    ax.set_yscale("log")
    ax.axhline(1.0, color="gray", ls="--", lw=1)
    ax.set_ylim(0.6, max(hs) * 8)
    ax.set_xticks(list(xs), [l for _, l, _ in SYSTEMS])
    ax.set_ylabel("Geomean speedup vs DuckDB" if not fallback else "Geomean workload speedup vs DuckDB")
    ax.set_title(title)


def save(fig, path):
    fig.tight_layout()
    for ext in ("png", "pdf"):
        fig.savefig(f"{path}.{ext}", dpi=200, bbox_inches="tight")
        print(f"wrote {path}.{ext}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--interval", type=float, default=15.0, help="refresh interval (s) to plot")
    ap.add_argument("--writes-mode", choices=("concurrent", "multi"), default="concurrent")
    ap.add_argument("--rf-conc", nargs="+", default=RF_CONC,
                    help="run_rf_interval_sweep.sh concurrent directories, merged (a system's runs pair with the "
                         "base run of the same query and repetition in any of them)")
    ap.add_argument("--rf-out", default=RF_OUT, help="run_rf_interval_sweep.sh MODE=multi directory")
    ap.add_argument("--readonly-threads", choices=("best", "st", "mt"), default="best",
                    help="read-only panel: best = each system's best measured configuration; st = everything single-threaded (as the refresh-stream panel); "
                         "mt = DuckDB multi-threaded")
    ap.add_argument("--out-prefix", default=os.path.join(HERE, "results/headline_sf20"))
    a = ap.parse_args()

    cfg = READONLY[a.readonly_threads]
    ro = readonly_speedups(cfg)
    for k, v in ro.items():
        print(f"read-only ({a.readonly_threads}) {k}: {v:.2f}x")
    ro_title = "TPC-H (SF=20): Read-only" + ("" if a.readonly_threads == "best" else f", {cfg['label']}")
    ro_missing = {k for k, _, _ in SYSTEMS if k not in ro}
    fig, ax = plt.subplots(figsize=(7, 4.6))
    panel(ax, ro, ro_title, missing=ro_missing)
    save(fig, a.out_prefix + f"_readonly_{a.readonly_threads}")

    w, rate = (write_speedups_conc(a.rf_conc, a.interval) if a.writes_mode == "concurrent"
               else write_speedups(a.rf_out, a.interval))
    wv = {"gendb": 1.0, "bespoke": 1.0, **{k: v[0] for k, v in w.items()}}
    fig, ax = plt.subplots(figsize=(7, 4.6))
    panel(ax, wv, f"TPC-H (SF=20): Refresh stream, ≈{rate / 1e3:.1f}k rows/s", errs=w,
          missing={k for k in ("stateless", "cpp", "verus") if k not in w}, fallback={"gendb", "bespoke"})
    save(fig, a.out_prefix + f"_writes_T{a.interval:g}s")

    # both panels stacked in one figure (the paper's headline)
    fig, (top, bot) = plt.subplots(2, 1, figsize=(7, 9.2))
    panel(top, ro, ro_title, missing=ro_missing)
    panel(bot, wv, f"TPC-H (SF=20): Refresh stream, ≈{rate / 1e3:.1f}k rows/s", errs=w,
          missing={k for k in ("stateless", "cpp", "verus") if k not in w}, fallback={"gendb", "bespoke"})
    save(fig, a.out_prefix + f"_combined_{a.readonly_threads}_T{a.interval:g}s")


if __name__ == "__main__":
    main()
