#!/usr/bin/env python3
"""Workload speedup over DuckDB per query and write interval, from a `run_compare.py cycle` run.

  python3 eval/compare/plot_cycle.py <out_dir> [--intervals 300 120 60] [--demand saturated | rate:<queries per minute>]

Inputs: <out_dir>/cycle_q<N>_{base,accel}_r<k>.json (one accelerator alone per process; the TPC-H refresh functions applied
one commit at a time, a burst of reads after each).  Per query (medians over repetitions; means over the pairs of a run for commit costs, so periodic compactions count):
  m_k  maintenance of one refresh slot with the accelerator (k = RF1 or RF2): the RF commit and the point writes after it
       (supplier / part / partsupp: what maintains Q2, Q11, Q16, which RF never touches), each commit's time (capture,
       maintenance, install) plus what the first read after it costs beyond a steady read (a lazily rebuilt state shows there)
  w_k  the same slot as plain SQL on DuckDB
  Ra, Rd  a read with the accelerator / on DuckDB, after the refresh
The writer commits RF1, RF2, RF1, ... one commit per interval T (T_k = max(T, m_k): a commit that takes longer than the
interval delays the next one; such points are hatched: the accelerator cannot keep up).  Demand between commits:
  saturated    queries back to back: the accelerator serves n = (T_k - m_k) / Ra of them per interval, DuckDB is charged for
               the same n queries plus its commit:  speedup = (sum w_k + n * Rd) / sum T_k   (the old chart's definition)
  rate:Q       Q queries per minute of that query, the same for both systems (n = Q * T_k / 60):
               speedup = (w_k + n * Rd) / (m_k + n * Ra)
Writes <out_dir>/cycle_speedup.png and cycle_speedup.csv.
"""
import argparse
import csv
import glob
import json
import math
import os
import statistics
import sys


def load(out):
    runs = {}
    for f in glob.glob(os.path.join(out, "cycle_q*_*_r*.json")):
        r = json.load(open(f))
        if "error" in r:
            print(f"skipping {os.path.basename(f)}: {r['error']}", file=sys.stderr)
            continue
        runs.setdefault((r["query"], r["system"]), []).append(r)
    return runs


def slots(writes):
    """[(rf kind, [the slot's commits])]: a slot is an RF commit and the point writes after it, up to the next RF commit."""
    out = []
    for w in writes:
        if w["kind"] in ("rf1", "rf2"):
            out.append((w["kind"], [w]))
        elif out:
            out[-1][1].append(w)
    return out


def steady_read(writes):
    """A read after a refresh, from the bursts after the RF commits (median of their medians), seconds."""
    return statistics.median(w["read_p50_us"] for w in writes if w["kind"] in ("rf1", "rf2")) / 1e6


def slot_costs(writes, penalty_over=None):
    """Mean cost per slot kind (rf1 / rf2) over the pairs of one run: every commit of the slot, plus (accelerator runs) what
    the first read after each commit costs beyond a steady read (a lazily rebuilt state shows there).  MEANS, not medians:
    some accelerators compact their state every few commits (Q20: a bulk build once its overlay passes 400,000 rows), a real
    cost that a median over a few pairs would hide.  Also the share of the point writes."""
    by = {"rf1": [], "rf2": []}
    pts = {"rf1": [], "rf2": []}
    for kind, commits in slots(writes):
        total = point = 0.0
        for c in commits:
            t = c["commit_us"] / 1e6
            if penalty_over is not None and c.get("reads", 0) > 0:
                t += max(0.0, c["first_read_us"] / 1e6 - penalty_over)
            total += t
            if c["kind"] not in ("rf1", "rf2"):
                point += t
        by[kind].append(total)
        pts[kind].append(point)
    return ({k: statistics.mean(v) for k, v in by.items() if v}, {k: statistics.mean(v) for k, v in pts.items() if v})


def components(runs, q):
    acc, base = runs.get((q, "accel")), runs.get((q, "base"))
    if not acc or not base:
        return None
    m, w, pt = {"rf1": [], "rf2": []}, {"rf1": [], "rf2": []}, {"rf1": [], "rf2": []}
    ra, rd, mism, actions = [], [], 0, {}
    for r in acc:
        r_a = steady_read(r["writes"])
        ra.append(r_a)
        costs, points = slot_costs(r["writes"], penalty_over=r_a)
        for k in costs:
            m[k].append(costs[k])
            pt[k].append(points[k])
        mism += r["mismatches"]
        for x in r["writes"]:
            if x["kind"] in ("rf1", "rf2"):
                actions.setdefault(x["kind"], {}).setdefault(x["action"], 0)
                actions[x["kind"]][x["action"]] += 1
    for r in base:
        rd.append(steady_read(r["writes"]))
        for k, v in slot_costs(r["writes"])[0].items():
            w[k].append(v)
    med = statistics.median
    return {"m": {k: med(v) for k, v in m.items() if v}, "w": {k: med(v) for k, v in w.items() if v},
            "points": {k: med(v) for k, v in pt.items() if v},
            "Ra": med(ra), "Rd": med(rd), "mismatches": mism, "actions": actions,
            "build_s": med([r["build_ms"] / 1e3 for r in acc]), "reps": len(acc)}


def speedup(c, T, demand):
    """-> (speedup, keeps_up) at write interval T seconds."""
    ks = ("rf1", "rf2")
    Tk = {k: max(T, c["m"][k]) for k in ks}
    keeps_up = all(c["m"][k] <= T for k in ks)
    if demand == "saturated":
        n = sum((Tk[k] - c["m"][k]) / c["Ra"] for k in ks)
        return (sum(c["w"][k] for k in ks) + n * c["Rd"]) / sum(Tk.values()), keeps_up
    rate = float(demand.split(":")[1])
    num = den = 0.0
    for k in ks:
        n = rate * Tk[k] / 60.0
        num += c["w"][k] + n * c["Rd"]
        den += c["m"][k] + n * c["Ra"]
    return num / den, keeps_up


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--intervals", type=float, nargs="+", default=[300, 120, 60])
    ap.add_argument("--demand", default="saturated")
    ap.add_argument("--title", default=None)
    ap.add_argument("--png", default=None)
    a = ap.parse_args()
    runs = load(a.out)
    queries = sorted({q for q, _ in runs})
    comp = {q: components(runs, q) for q in queries}
    comp = {q: c for q, c in comp.items() if c and "rf1" in c["m"] and "rf2" in c["m"] and "rf1" in c["w"] and "rf2" in c["w"]}
    if not comp:
        raise SystemExit("no complete (base + accel) cycle results in " + a.out)
    rows = []
    print(f"{'Q':>3} {'build s':>8} {'RF1 slot s':>10} {'RF2 slot s':>10} {'(points)':>8} {'DuckDB slot s':>14} {'Ra ms':>8} {'Rd ms':>9} {'read x':>8} | "
          + " ".join(f"{('T=%gs' % t):>10}" for t in a.intervals) + " | actions (RF1; RF2) mism")
    for q, c in sorted(comp.items()):
        sp = [speedup(c, t, a.demand) for t in a.intervals]
        acts = "; ".join("/".join(sorted(c["actions"].get(k, {}))) for k in ("rf1", "rf2"))
        print(f"{q:>3} {c['build_s']:8.1f} {c['m']['rf1']:10.2f} {c['m']['rf2']:10.2f} {c['points']['rf1']:8.2f} "
              f"{c['w']['rf1']:6.2f}/{c['w']['rf2']:<7.2f} {c['Ra'] * 1e3:8.2f} {c['Rd'] * 1e3:9.1f} {c['Rd'] / c['Ra']:8.0f} | "
              + " ".join(f"{s:9.2f}{'*' if not ok else ' '}" for s, ok in sp) + f" | {acts} {c['mismatches']}")
        rows.append({"query": q, "build_s": c["build_s"], "maint_rf1_slot_s": c["m"]["rf1"], "maint_rf2_slot_s": c["m"]["rf2"],
                     "points_rf1_slot_s": c["points"]["rf1"], "points_rf2_slot_s": c["points"]["rf2"],
                     "duckdb_rf1_s": c["w"]["rf1"], "duckdb_rf2_s": c["w"]["rf2"], "accel_read_s": c["Ra"],
                     "duckdb_read_s": c["Rd"], "actions": json.dumps(c["actions"]), "mismatches": c["mismatches"],
                     **{f"speedup_T{t:g}": s for t, (s, ok) in zip(a.intervals, sp)},
                     **{f"keeps_up_T{t:g}": ok for t, (s, ok) in zip(a.intervals, sp)}})
    print("(* = a refresh commit takes longer than the interval: the accelerator cannot keep up; the interval stretches)")
    with open(os.path.join(a.out, "cycle_speedup.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0]))
        w.writeheader()
        w.writerows(rows)

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
    qs = [r["query"] for r in rows]
    x = np.arange(len(qs))
    width = 0.8 / len(a.intervals)
    colors = ["#2b4c6f", "#4f9d92", "#e3c76d", "#c0583c", "#7a5c99"]
    fig, ax = plt.subplots(figsize=(13.5, 4.8))
    for j, t in enumerate(a.intervals):
        ys = [r[f"speedup_T{t:g}"] for r in rows]
        gm = math.exp(sum(math.log(max(y, 1e-9)) for y in ys) / len(ys))
        bars = ax.bar(x + (j - (len(a.intervals) - 1) / 2) * width, ys, width * 0.92, color=colors[j % len(colors)],
                      label=f"{t:g}s interval (geo {gm:,.0f}×)")
        for b, r in zip(bars, rows):
            if not r[f"keeps_up_T{t:g}"]:
                b.set_hatch("///")
                b.set_edgecolor("white")
    ax.set_yscale("log")
    ys_all = [r[f"speedup_T{t:g}"] for r in rows for t in a.intervals]
    ax.set_ylim(min(0.7, min(ys_all) * 0.6), max(ys_all) * 6)  # headroom for the legend
    ax.axhline(1.0, color="#444", lw=0.8, alpha=0.6)
    ax.set_xticks(x, [f"Q{q}" for q in qs])
    ax.set_xlabel("Query")
    ax.set_ylabel("Workload speedup (DuckDB / accel)")
    demand = "queries back to back" if a.demand == "saturated" else f"{a.demand.split(':')[1]} queries/min of each query"
    ax.set_title(a.title or f"TPC-H (SF=20): Workload Speedup against DuckDB under RF Write Interval Sweep\n"
                            f"(real TPC-H update sets, {demand}; hatched = refresh takes longer than the interval)", fontsize=11)
    ax.grid(axis="y", ls="--", alpha=0.35)
    ax.legend(frameon=False, loc="upper left", ncol=len(a.intervals), fontsize=8)
    fig.tight_layout()
    png = a.png or os.path.join(a.out, "cycle_speedup.png")
    fig.savefig(png, dpi=160)
    print("wrote", png, "and", os.path.join(a.out, "cycle_speedup.csv"))


if __name__ == "__main__":
    main()
