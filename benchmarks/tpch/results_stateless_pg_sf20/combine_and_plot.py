"""Combine the stateless PG SF20 sweeps into one 22-query table and plot geomean speedups.

Each sweep CSV ends with '#Qn,...' median rows (see pg_stateless_bench.py).  Later sources override
earlier ones per query.  Speedup = PostgreSQL median / accelerator median (PostgreSQL with
--force-workers --pg-best-plan).
"""
import math
import os
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "../../.."))
BASELINE = [os.path.join(HERE, "sweep_20261008_011107.csv"),             # Q1-16,19,21,22 (merged build)
            os.path.join(HERE, "sweep_20261008_091205_q17_18_20.csv")]   # Q17,18,20 (merged build)
UPDATES = [os.path.join(ROOT, ".worktrees/q14q22/bench/sweep_q14_q22.csv"),  # opt/pg-stateless-q14q22
           os.path.join(ROOT, ".worktrees/q5q19/bench/sweep_q5_q19.csv")]    # opt/pg-stateless-q5q19
THREADS = (1, 16, 32)
# summary columns: query, pg@1, accel@1, sp@1, plan@1, pg@16, accel@16, sp@16, workers@16, plan@16, pg@32, ...
COLS = {1: (1, 2), 16: (5, 6), 32: (10, 11)}


def load(path):
    out = {}
    for line in open(path):
        if line.startswith("#Q"):
            f = line[1:].strip().split(",")
            out[int(f[0][1:])] = {t: (float(f[COLS[t][0]]), float(f[COLS[t][1]]), os.path.basename(path))
                                  for t in THREADS}
    return out


def merge(paths):
    rows = {}
    for p in paths:
        rows.update(load(p))
    return rows


def geomean(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


before = merge(BASELINE)
after = merge(BASELINE + UPDATES)
missing = sorted(set(range(1, 23)) - set(after))
if missing:
    sys.exit("missing queries: %s" % missing)

with open(os.path.join(HERE, "combined_speedups.csv"), "w") as out:
    out.write("query,threads,postgres_ms,accel_ms,speedup,source\n")
    for q in range(1, 23):
        for t in THREADS:
            pg, ac, src = after[q][t]
            out.write("Q%d,%d,%.3f,%.3f,%.3f,%s\n" % (q, t, pg, ac, pg / ac, src))

gm = {}
for name, rows in (("before", before), ("after", after)):
    qs = sorted(rows)
    gm[name] = [geomean([rows[q][t][0] / rows[q][t][1] for q in qs]) for t in THREADS]
    losses = {t: ["Q%d" % q for q in qs if rows[q][t][0] < rows[q][t][1]] for t in THREADS}
    print("%-6s (%d queries) geomean speedup T=1 %.2fx  T=16 %.2fx  T=32 %.2fx   losses %s"
          % (name, len(qs), *gm[name], losses))

# --- plot (same format as the DuckDB stateless figure) --------------------------------------------
vals = gm["after"]
colors = ["#85b6ef", "#2a78d6", "#0f3f80"]  # one blue, light -> dark with thread count
fig, ax = plt.subplots(figsize=(6.8, 4.6), dpi=200)
bars = ax.bar(["1 thread", "16 threads", "32 threads"], vals, width=0.6, color=colors, zorder=3)
for b, v in zip(bars, vals):
    ax.text(b.get_x() + b.get_width() / 2, v + 0.04, "%.2f\u00d7" % v, ha="center", va="bottom", fontsize=13)
ax.axhline(1.0, color="#777777", linewidth=1.3, linestyle="--", zorder=4)
ax.set_title("TPC-H (SF=20): Stateless Speedup over PostgreSQL", fontsize=14)
ax.set_ylabel("Geomean speedup", fontsize=13)
ax.set_ylim(0, math.ceil((max(vals) + 0.3) * 2) / 2)
ax.yaxis.set_major_locator(matplotlib.ticker.MultipleLocator(0.5))
ax.yaxis.grid(True, color="#dddddd", linewidth=0.8, zorder=0)
ax.tick_params(labelsize=13)
for side in ("top", "right"):
    ax.spines[side].set_visible(False)
fig.tight_layout()
png = os.path.join(HERE, "stateless_geomean_speedup_sf20.png")
fig.savefig(png)
print("wrote", png)
