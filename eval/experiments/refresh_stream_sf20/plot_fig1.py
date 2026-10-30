#!/usr/bin/env python3
"""Figure 1: TPC-H SF20 geomean speedup over DuckDB, read-only (top) and under a refresh stream (bottom); 40 vs 40.

Read-only: geomeans of the Figure 5 data (../readonly_sf20), recomputed here by running its plot script.
Refresh stream (T=15 s, ~10k rows/s, 8 readers, one writer): per repetition the geomean over Q1-Q22 of
workload.workload_speedup against the DuckDB run of the same query, median with min-max whiskers.
Stateless and verified come from data/headline_T15 (Q21 stateless is the rerun after the LIMIT 100 fix);
stateful from data/writerate/T15s, a separate run; each is paired with its own run's DuckDB base.
GenDB and Bespoke have no maintenance path, so a refresh sends them back to DuckDB (1x, fallback).

Usage: plot_fig1.py OUT_DIR   (writes OUT_DIR/headline_sf20_combined_40v40_T15s.{png,pdf})
"""
import csv
import glob
import json
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1] / "compare"))
sys.path.insert(0, str(HERE.parent))
from figstyle import geomean  # noqa: E402
from workload import workload_speedup  # noqa: E402

DATA = HERE / "data"
HEADLINE_DIR, STATEFUL_DIR = DATA / "headline_T15", DATA / "writerate"
READONLY_SF20 = HERE.parent / "readonly_sf20"
T = 15.0

STATELESS_COLOR, GENDB_COLOR, BESPOKE_COLOR, VERIFIED_COLOR, STATEFUL_COLOR = (
    "#1F3A8A", "#F59E0B", "#16A34A", "#3B1F6B", "#1A2654")
# ascending read-only order: GenDB below Bespoke
SYSTEMS = [("stateless", "JetStream\n(stateless)", STATELESS_COLOR),
           ("gendb", "GenDB", GENDB_COLOR),
           ("bespoke", "Bespoke\nOLAP", BESPOKE_COLOR),
           ("verus", "JetStream\n(stateful,\nverified)", VERIFIED_COLOR),
           ("cpp", "JetStream\n(stateful)", STATEFUL_COLOR)]


def fmt(v):
    return f"{v:.1f}×" if v >= 10 else f"{v:.2f}×"


def read_only_geomeans() -> dict[str, float]:
    """Figure 5 geomeans per system key, from the geomean row of readonly_sf20's per-query CSV."""
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([sys.executable, str(READONLY_SF20 / "plot_fig5.py"), tmp,
                        str(READONLY_SF20 / "data" / "warm_sweep.log")], check=True, stdout=subprocess.DEVNULL)
        gm = next(r for r in csv.DictReader(open(f"{tmp}/readonly_speedup_sf20_40t_samebase.csv"))
                  if r["query"] == "geomean")
    return {"stateless": float(gm["stateless_x"]), "gendb": float(gm["gendb_x"]), "bespoke": float(gm["bespoke_x"]),
            "cpp": float(gm["stateful_cpp_x"]), "verus": float(gm["verified_x"])}


def write_speedups(out: Path):
    """system -> (median, min, max) over repetitions of the 22-query geomean workload speedup; median rows/s touched."""
    runs = {}
    for f in glob.glob(f"{out}/T{T:g}s/conc_q*_*_r*.json"):
        r = json.load(open(f))
        if "error" not in r:
            runs.setdefault(r["system"], {})[(r["query"], r["rep"])] = r
    base = runs["base"]
    res = {}
    for s in ("stateless", "cpp", "verus"):
        vals = []
        for k in sorted({k for _, k in runs.get(s, {})}):
            pq = [workload_speedup(base[(q, k)], runs[s][(q, k)]) if (q, k) in base and (q, k) in runs[s] else None
                  for q in range(1, 23)]
            if all(pq):
                vals.append(geomean(pq))
        if vals:
            res[s] = (statistics.median(vals), min(vals), max(vals))
            print(f"T={T:g}s {s}: {res[s][0]:.1f}x [{res[s][1]:.1f}-{res[s][2]:.1f}] n={len(vals)}")
    return res, statistics.median(r.get("rows_touched_per_sec", 0) for r in base.values())


def panel(ax, values, title, ylabel, ytop, errs=None, fallback=()):
    xs = range(len(SYSTEMS))
    ax.bar(xs, [values[k] for k, _, _ in SYSTEMS], 0.72, color=[c for _, _, c in SYSTEMS])
    for x, (k, _, _) in zip(xs, SYSTEMS):
        top = values[k]
        if errs and k in errs and errs[k][1] < errs[k][2]:
            _, lo, hi = errs[k]
            ax.errorbar([x], [top], yerr=[[top - lo], [hi - top]], fmt="none", ecolor="black", capsize=2, lw=0.8)
            top = hi
        label = fmt(values[k]) + ("\n(fallback)" if k in fallback else "")
        ax.text(x, top * 1.12, label, ha="center", va="bottom", linespacing=0.95)
    ax.set_yscale("log")
    ax.axhline(1.0, color="gray", ls="--", lw=0.7)
    ax.set_ylim(0.6, ytop)
    ax.set_xlim(-0.5, len(SYSTEMS) - 0.5)
    ax.set_xticks(list(xs), [l for _, l, _ in SYSTEMS])
    ax.set_ylabel(ylabel)
    ax.set_title(title, pad=3)


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    read_only = read_only_geomeans()
    new, rate = write_speedups(HEADLINE_DIR)
    stateful, _ = write_speedups(STATEFUL_DIR)
    w = {"stateless": new["stateless"], "verus": new["verus"], "cpp": stateful["cpp"]}
    refresh = {"gendb": 1.0, "bespoke": 1.0, **{k: v[0] for k, v in w.items()}}
    print(f"read-only geomean vs DuckDB: { {k: round(v, 2) for k, v in read_only.items()} }")
    print(f"refresh stream ({rate / 1e3:.1f}k rows/s, T={T:g}s): { {k: round(v, 2) for k, v in refresh.items()} }")

    plt.rcParams.update({"font.size": 7, "axes.titlesize": 7.5, "axes.labelsize": 7, "xtick.labelsize": 6.5,
                         "ytick.labelsize": 6.5, "axes.linewidth": 0.6, "xtick.major.width": 0.6,
                         "ytick.major.width": 0.6, "xtick.major.pad": 2, "ytick.major.pad": 1.5,
                         "axes.labelpad": 2, "pdf.fonttype": 42})
    fig, (top, bot) = plt.subplots(2, 1, figsize=(3.33, 3.3), sharex=True)
    panel(top, read_only, "TPC-H (SF=20): Read-only", "Geomean speedup\nvs DuckDB", 5000)
    panel(bot, refresh, f"TPC-H (SF=20): Refresh stream, ≈{rate / 1e3:.1f}k rows/s",
          "Geomean workload\nspeedup vs DuckDB", 500, errs=w, fallback={"gendb", "bespoke"})
    top.tick_params(axis="x", length=0)
    fig.align_ylabels()
    fig.tight_layout(pad=0.2, h_pad=0.8)
    for ext in ("png", "pdf"):
        fig.savefig(out / f"headline_sf20_combined_40v40_T15s.{ext}", dpi=600, bbox_inches="tight", pad_inches=0.02)


if __name__ == "__main__":
    main()
