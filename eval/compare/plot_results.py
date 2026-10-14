#!/usr/bin/env python3
"""Bar charts for the C++ vs Verus comparison (run_compare.py / run_all.sh results).

  python3 eval/compare/plot_results.py <out_dir>            # a run_all.sh dir (read/, concurrent/, multi/)
  python3 eval/compare/plot_results.py --read R --concurrent C --multi M --to DIR

Writes read.png, concurrent.png, multi.png (whichever results exist), and
multi_commit.png (multi commits vs each accelerator's commit alone).  Every
panel is a speedup over the DuckDB baseline run in the same experiment (>1 =
faster); log scales, since speedups span several orders of magnitude.
"""
import argparse
import json
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

COLORS = {"base": "#9e9e9e", "cpp": "#1f77b4", "verus": "#d62728"}
LABELS = {"base": "DuckDB", "cpp": "C++ accelerator", "verus": "Verus accelerator (global store)"}


def load(path):
    try:
        with open(path) as f:
            r = json.load(f)
        return None if "error" in r else r
    except (OSError, ValueError):
        return None


def bars(ax, queries, series, ylabel, title, log=True, fmt=None):
    """series: [(system, [value or None per query])]"""
    n = len(series)
    width = 0.8 / n
    for i, (system, values) in enumerate(series):
        xs = [q + (i - (n - 1) / 2) * width for q in range(len(queries))]
        vs = [v if v is not None and v > 0 else 0 for v in values]
        rects = ax.bar(xs, vs, width, color=COLORS[system], label=LABELS[system])
        if fmt:
            for rect, v in zip(rects, values):
                if v:
                    ax.annotate(fmt(v), (rect.get_x() + rect.get_width() / 2, rect.get_height()),
                                ha="center", va="bottom", fontsize=6, rotation=90)
        for x, v in zip(xs, values):
            if v is None:
                ax.annotate("n/a", (x, 1 if log else 0), ha="center", va="bottom", fontsize=6, color=COLORS[system])
    ax.set_xticks(range(len(queries)))
    ax.set_xticklabels([f"Q{q}" for q in queries])
    if log:
        ax.set_yscale("log")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.grid(axis="y", which="major", alpha=0.3)
    ax.legend(fontsize=8, loc="upper left", bbox_to_anchor=(1.0, 1.0))


def plot_read(d, to):
    queries = [q for q in range(1, 23) if load(f"{d}/read_q{q}_cpp.json") or load(f"{d}/read_q{q}_verus.json")]
    if not queries:
        return None
    sp = lambda s: [(load(f"{d}/read_q{q}_{s}.json") or {}).get("speedup_geomean") for q in queries]
    bm = lambda s: [((load(f"{d}/read_q{q}_{s}.json") or {}).get("build_ms") or 0) / 1000 or None for q in queries]
    fig, (a1, a2) = plt.subplots(2, 1, figsize=(18, 9))
    bars(a1, queries, [("cpp", sp("cpp")), ("verus", sp("verus"))], "speedup over DuckDB (x, log)",
         "Read-only: accelerator latency speedup over DuckDB (median per parameter set, geomean of 8 sets), SF20",
         fmt=lambda v: f"{v:.0f}x")
    a1.axhline(1, color="black", lw=0.8)
    bars(a2, queries, [("cpp", bm("cpp")), ("verus", bm("verus"))], "build time (s, log)",
         "Accelerator build time (s)", fmt=lambda v: f"{v:.0f}")
    fig.tight_layout()
    out = os.path.join(to, "read.png")
    fig.savefig(out, dpi=130)
    plt.close(fig)
    return out


def ratio(num, den):
    return num / den if num and den else None


def speedup_axis(ax):
    ax.axhline(1, color="black", lw=0.8)


def plot_concurrent(d, to):
    queries = [q for q in range(1, 23) if any(load(f"{d}/conc_q{q}_{s}.json") for s in ("cpp", "verus"))]
    if not queries:
        return None
    get = lambda s, k: [(load(f"{d}/conc_q{q}_{s}.json") or {}).get(k) for q in queries]
    # Throughput: accel / DuckDB.  Latency: DuckDB / accel.  >1 = faster than DuckDB.
    up = lambda s, k: [ratio(a, b) for a, b in zip(get(s, k), get("base", k))]
    down = lambda s, k: [ratio(b, a) for a, b in zip(get(s, k), get("base", k))]
    systems = ("cpp", "verus")
    fig, (a1, a2, a3) = plt.subplots(3, 1, figsize=(18, 13))
    bars(a1, queries, [(s, up(s, "qps")) for s in systems], "speedup over DuckDB (x, log)",
         "Concurrent (one accelerator per run): reader throughput speedup over DuckDB, "
         "8 readers + 1 writer at 5 writes/s, SF20", fmt=lambda v: f"{v:.0f}x")
    speedup_axis(a1)
    bars(a2, queries, [(s, down(s, "read_p99_us")) for s in systems], "speedup over DuckDB (x, log)",
         "Reader p99 latency speedup over DuckDB", fmt=lambda v: f"{v:.0f}x")
    speedup_axis(a2)
    bars(a3, queries, [(s, down(s, "max_commit_us")) for s in systems], "speedup over DuckDB (x, log)",
         "Writer: slowest-commit speedup over DuckDB (<1 = slower; the accelerator also maintains its aux state; "
         "far below 1 = maintenance stall)", fmt=lambda v: f"{v:.2g}x")
    speedup_axis(a3)
    fig.tight_layout()
    out = os.path.join(to, "concurrent.png")
    fig.savefig(out, dpi=130)
    plt.close(fig)
    return out


def plot_multi(d, to):
    runs = {s: load(f"{d}/multi_{s}.json") for s in ("base", "cpp", "verus")}
    runs = {s: r for s, r in runs.items() if r}
    if not runs:
        return None
    key = lambda a: int(a.replace("_vgen", "").split(":")[-1].replace("tpch_q", ""))
    per = {s: {key(p["accel"]): p for p in r["per_accel"]} for s, r in runs.items()}
    queries = sorted({q for d_ in per.values() for q in d_})
    lat = lambda s, k: [per.get(s, {}).get(q, {}).get(k) for q in queries]
    down = lambda s, k: [ratio(b, a) for a, b in zip(lat(s, k), lat("base", k))]
    fig = plt.figure(figsize=(18, 11))
    a1 = fig.add_subplot(2, 1, 1)
    systems = [s for s in ("cpp", "verus") if s in runs]
    bars(a1, queries, [(s, down(s, "read_p50_us")) for s in systems], "speedup over DuckDB (x, log)",
         f"Multi-accelerator ({len(queries)} live in one process; each commit maintains all of them): "
         "per-query read p50 latency speedup over DuckDB, SF20", fmt=lambda v: f"{v:.0f}x")
    speedup_axis(a1)
    base = runs.get("base", {})
    a2 = fig.add_subplot(2, 2, 3)
    bars(a2, list(range(1)), [(s, [ratio(runs[s]["qps"], base.get("qps"))]) for s in systems],
         "speedup over DuckDB (x, log)", "Total reader throughput speedup over DuckDB", fmt=lambda v: f"{v:.0f}x")
    speedup_axis(a2)
    a2.set_xticklabels(["all queries"])
    a3 = fig.add_subplot(2, 2, 4)
    labels = ["commit p50", "commit p99", "slowest commit"]
    keys = ["commit_p50_us", "commit_p99_us", "max_commit_us"]
    n = len(systems)
    for i, s in enumerate(systems):
        xs = [k + (i - (n - 1) / 2) * 0.8 / n for k in range(3)]
        vs = [ratio(base.get(k), runs[s][k]) or 0 for k in keys]
        rects = a3.bar(xs, vs, 0.8 / n, color=COLORS[s], label=LABELS[s])
        for rect, v in zip(rects, vs):
            if v:
                a3.annotate(f"{v:.2g}x", (rect.get_x() + rect.get_width() / 2, rect.get_height()),
                            ha="center", va="bottom", fontsize=7)
    a3.set_xticks(range(3))
    a3.set_xticklabels(labels)
    a3.set_yscale("log")
    a3.set_ylabel("speedup over DuckDB (x, log)")
    a3.set_title("Writer commit speedup over DuckDB (<1 = slower; one commit maintains every accelerator)")
    speedup_axis(a3)
    a3.legend(fontsize=8, loc="upper left", bbox_to_anchor=(1.0, 1.0))
    a3.grid(axis="y", alpha=0.3)
    fig.tight_layout()
    out = os.path.join(to, "multi.png")
    fig.savefig(out, dpi=130)
    plt.close(fig)
    return out


def plot_multi_commit(multi, conc, to):
    """Where the Verus global store costs: commit latency when every commit
    maintains all accelerators, against each accelerator's commit alone (the
    concurrent runs), and the write rate that leaves the writer saturated."""
    runs = {s: load(f"{multi}/multi_{s}.json") for s in ("cpp", "verus")}
    if not all(runs.values()):
        return None
    key = lambda a: int(a.replace("_vgen", "").split(":")[-1].replace("tpch_q", ""))
    per = {s: {key(p["accel"]): p for p in r["per_accel"]} for s, r in runs.items()}
    queries = sorted(per["cpp"])
    alone = {s: [(load(f"{conc}/conc_q{q}_{s}.json") or {}).get("commit_p50_us") for q in queries] for s in runs}
    systems = ("cpp", "verus")
    fig = plt.figure(figsize=(18, 11))
    a1 = fig.add_subplot(2, 1, 1)
    bars(a1, queries, [(s, [v / 1000 if v else None for v in alone[s]]) for s in systems], "commit p50 (ms, log)",
         f"Commit p50: each accelerator alone (bars, concurrent runs) vs all {len(queries)} live in one process "
         "(dashed lines, multi run), SF20, 5 writes/s", fmt=lambda v: f"{v:.0f}")
    for s in systems:
        m = runs[s]["commit_p50_us"] / 1000
        total = sum(v for v in alone[s] if v) / 1000
        a1.axhline(m, color=COLORS[s], ls="--", lw=1.5)
        a1.annotate(f"{LABELS[s].split(' (')[0]} multi: {m:.0f} ms (sum alone: {total:.0f} ms)",
                    (len(queries) - 0.5, m), ha="right", va="bottom", fontsize=9, color=COLORS[s])
    a2 = fig.add_subplot(2, 2, 3)
    labels = ["from commit p50", "from commit p99"]
    keys = ["commit_p50_us", "commit_p99_us"]
    for i, s in enumerate(systems):
        xs = [k + (i - 0.5) * 0.4 for k in range(2)]
        vs = [1e6 / runs[s][k] for k in keys]
        rects = a2.bar(xs, vs, 0.4, color=COLORS[s], label=LABELS[s])
        for rect, v in zip(rects, vs):
            a2.annotate(f"{v:.1f}/s", (rect.get_x() + rect.get_width() / 2, rect.get_height()),
                        ha="center", va="bottom", fontsize=8)
    a2.axhline(runs["cpp"]["writes_per_sec_target"], color="black", ls=":", lw=1)
    a2.annotate("rate we ran", (1.45, runs["cpp"]["writes_per_sec_target"]), ha="right", va="bottom", fontsize=8)
    a2.set_xticks(range(2))
    a2.set_xticklabels(labels)
    a2.set_ylabel("writes / s")
    a2.set_title("Multi: max write rate before the writer saturates (1 / commit latency, estimate)")
    a2.legend(fontsize=8, loc="upper right")
    a2.grid(axis="y", alpha=0.3)
    a3 = fig.add_subplot(2, 2, 4)
    bars(a3, queries, [(s, [per[s][q]["read_max_us"] / 1000 for q in queries]) for s in systems],
         "slowest read (ms, log)", "Multi: slowest read per query (one sample each; Verus is flat at ~120-135 ms)",
         fmt=lambda v: f"{v:.0f}")
    a3.legend(fontsize=8, loc="upper left")
    fig.tight_layout()
    out = os.path.join(to, "multi_commit.png")
    fig.savefig(out, dpi=130)
    plt.close(fig)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_dir", nargs="?")
    ap.add_argument("--read")
    ap.add_argument("--concurrent")
    ap.add_argument("--multi")
    ap.add_argument("--to")
    a = ap.parse_args()
    if a.out_dir:
        a.read = a.read or os.path.join(a.out_dir, "read")
        a.concurrent = a.concurrent or os.path.join(a.out_dir, "concurrent")
        a.multi = a.multi or os.path.join(a.out_dir, "multi")
        a.to = a.to or a.out_dir
    os.makedirs(a.to or ".", exist_ok=True)
    for f, d in ((plot_read, a.read), (plot_concurrent, a.concurrent), (plot_multi, a.multi)):
        if d and os.path.isdir(d):
            out = f(d, a.to or ".")
            print(out or f"{f.__name__}: no results in {d}")
    if a.multi and a.concurrent and os.path.isdir(a.multi) and os.path.isdir(a.concurrent):
        print(plot_multi_commit(a.multi, a.concurrent, a.to or ".") or "plot_multi_commit: needs cpp and verus runs")


if __name__ == "__main__":
    main()
