"""TPC-H SF20 read-only speedup at 40 threads, every system on the same footing: one DuckDB
reference per query (our 40T warm median on the PK database, i.e. the base_us of the C++ read runs)
divided by each system's own latency. Bottom panel (vs Postgres, 40v40) unchanged.
All systems run on socket 0 (CPUs 0-19,40-59, NUMA node 0) at 40 threads. Inputs in data/ and the scripts/ that made them:
  cpp_read/ (stateful C++ vs DuckDB, commit 29ed49f7)  run_cpp_read_40t_pk.sh   (PK database: make_pk_db.sh)
  warm_sweep.log (warm back-to-back C++ latency)       warm_sweep.sh
  stateless_duckdb_sweep_t40only_sf20_*.csv            run_stateless_40t_pk.sh  (run_both_pk.sh also runs Verus read)
  sf20_40t_socket0_bespoke_gendb.csv                   run_socket0_40t.sh
  stateful_verified_40t.csv, pg_40v40_readonly.csv     merged by hand from the runs' CSVs
Usage: plot_fig5.py [OUT_DIR=data] [WARM_LOG=data/warm_sweep.log]   -> readonly_speedup_sf20_40t_samebase.pdf in OUT_DIR"""
import csv, sys, glob, json, statistics, os
HERE = os.path.dirname(os.path.abspath(__file__)); DATA = f"{HERE}/data"
sys.path.insert(0, os.path.dirname(HERE))
import matplotlib; matplotlib.use("Agg")
import matplotlib.pyplot as plt, numpy as np
import matplotlib.transforms as mtransforms
from figstyle import (fmt, label_bars, place_labels, style_axis, geomean, LABEL_SIZE, PG_LABEL_SIZE,
    TICK_SIZE, AXIS_LABEL_SIZE, STATELESS_COLOR, GENDB_COLOR, BESPOKE_COLOR, STATEFUL_COLOR, VERIFIED_COLOR)
S, PG_CSV = (sys.argv[1] if len(sys.argv) > 1 else DATA), f"{DATA}/pg_40v40_readonly.csv"; os.makedirs(S, exist_ok=True); Q = [f"Q{q}" for q in range(1, 23)]
TITLE_SIZE, LEGEND_SIZE = 33, 29
TOP_LABEL_SIZE = 19  # ~4.4 pt at 7 in wide: the largest whose rotated digits clear a neighbouring bar's label

# Per-query latencies in ms. DuckDB reference and C++ stateful: medians over all cases of all reps.
CPP = f"{DATA}/cpp_read"
ref, cpp_ms = {}, {}
for q in Q:
    cases = [c for f in glob.glob(f"{CPP}/read_q{q[1:]}_cpp_r*.json") for c in json.load(open(f))["cases"]]
    ref[q] = statistics.median(c["base_us"] for c in cases) / 1e3
    cpp_ms[q] = statistics.median(c["accel_us"] for c in cases) / 1e3
WARM_LOG = sys.argv[2] if len(sys.argv) > 2 else f"{DATA}/warm_sweep.log"
if os.path.exists(WARM_LOG):  # warm back-to-back C++ latency (warm_sweep.log: "Q<n> profile loop: <k> calls, median <us> us")
    for l in open(WARM_LOG):
        if l.startswith("Q") and "median" in l:
            cpp_ms[l.split()[0]] = float(l.split("median ")[1].split()[0]) / 1e3
sl_ms = {l.split(",")[0][1:]: float(l.split(",")[5]) for l in open(  # median accel_par ms over 5 seeds
    f"{DATA}/stateless_duckdb_sweep_t40only_sf20_20261019_211957.csv")
    if l.startswith("#Q")}
ext_ms = {}  # GenDB / Bespoke own latency (their DuckDB numbers are not used)
for r in csv.DictReader(open(f"{DATA}/sf20_40t_socket0_bespoke_gendb.csv")):
    if r["duckdb_version"] == "1.5.6": ext_ms[(r["system"], r["query"])] = float(r["system_ms"])
hy = {r["query"]: r for r in csv.DictReader(open(f"{DATA}/stateful_verified_40t.csv"))}
vf_ms = {q: float(hy[q]["verified_ms"]) for q in Q}
pg = {r["query"]: r for r in csv.DictReader(open(PG_CSV)) if r["query"].startswith("Q")}

def gl(vals):
    g = geomean(vals); return f"{g:.2f}×" if g < 10 else f"{g:.1f}×" if g < 100 else f"{g:.0f}×"

def vs_ref(ms): return [ref[q] / ms[q] for q in Q]
sl_v, cpp_v, vf_v = vs_ref(sl_ms), vs_ref(cpp_ms), vs_ref(vf_ms)
gd_v = vs_ref({q: ext_ms[("GenDB", q)] for q in Q}); bo_v = vs_ref({q: ext_ms[("BespokeOLAP", q)] for q in Q})
pg_sl = [float(pg[q]["stateless_speedup"]) for q in Q]; pg_sf = [float(pg[q]["stateful_speedup_median"]) for q in Q]
with open(f"{S}/readonly_speedup_sf20_40t_samebase.csv", "w", newline="") as fh:
    w = csv.writer(fh)
    w.writerow(["query", "duckdb_ref_ms", "stateless_ms", "gendb_ms", "bespoke_ms", "stateful_cpp_ms", "verified_ms",
                "stateless_x", "gendb_x", "bespoke_x", "stateful_cpp_x", "verified_x"])
    for i, q in enumerate(Q):
        w.writerow([q, f"{ref[q]:.3f}", sl_ms[q], ext_ms[("GenDB", q)], ext_ms[("BespokeOLAP", q)], f"{cpp_ms[q]:.3f}",
                    f"{vf_ms[q]:.3f}"] + [f"{v[i]:.2f}" for v in (sl_v, gd_v, bo_v, cpp_v, vf_v)])
    w.writerow(["geomean", "", "", "", "", "", ""] + [f"{geomean(v):.2f}" for v in (sl_v, gd_v, bo_v, cpp_v, vf_v)])
# Every system runs at 40T (stated in the title), so the legend drops it to fit two rows.
top = [(f"JetStream (stateless) (DuckDB: {gl(sl_v)}, Postgres: {gl(pg_sl)})", sl_v, STATELESS_COLOR),
       (f"GenDB (MT) (DuckDB: {gl(gd_v)})", gd_v, GENDB_COLOR),
       (f"Bespoke OLAP (MT) (DuckDB: {gl(bo_v)})", bo_v, BESPOKE_COLOR),
       (f"JetStream (stateful) (DuckDB: {gl(cpp_v)}, Postgres: {gl(pg_sf)})", cpp_v, STATEFUL_COLOR),
       (f"JetStream (stateful, verified) (DuckDB: {gl(vf_v)})", vf_v, VERIFIED_COLOR)]
bottom = [("_stateless", pg_sl, STATELESS_COLOR), ("_stateful", pg_sf, STATEFUL_COLOR)]

fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(30, 12), sharex=True, facecolor="white",
                               gridspec_kw={"height_ratios": [1.1, 1]})
x = np.arange(len(Q))
def grouped(ax, series, size):
    w = 0.94 / len(series); labels = []
    for i, (name, vals, c) in enumerate(series):
        bars = ax.bar(x + (i - (len(series) - 1) / 2) * w, vals, w * 0.94, color=c, label=name, zorder=3)
        labels.append(list(zip(label_bars(ax, bars, vals, size), bars)))
    return [list(g) for g in zip(*labels)]
def grouped_top(ax, series, size):
    """Top panel: a query's bars touch, with a gap between queries; each speedup sits on its bar,
    shifted right by half the font's descent so the digits, not the text box, are centred on the bar."""
    w = 0.92 / len(series)
    nudge = mtransforms.offset_copy(ax.transData, fig=fig, x=0.11 * size, y=0, units="points")
    for i, (name, vals, c) in enumerate(series):
        bars = ax.bar(x + (i - (len(series) - 1) / 2) * w, vals, w, color=c, label=name, zorder=3, linewidth=0)
        for bar, v in zip(bars, vals):
            ax.text(bar.get_x() + bar.get_width() / 2, bar.get_height() * 1.12, fmt(v), transform=nudge,
                    ha="center", va="bottom", rotation=90, fontsize=size, color="#333333")
grouped_top(ax1, top, TOP_LABEL_SIZE); style_axis(ax1, len(Q))
allv = [v for _, vs, _ in top for v in vs]
ax1.set_ylim(min(0.8, min(allv) * 0.7), max(allv) * 12)
ax1.set_ylabel("vs DuckDB", fontsize=AXIS_LABEL_SIZE)
ax1.set_title("TPC-H (SF=20, 40 threads): Read-only Speedup", fontsize=TITLE_SIZE)
bg = grouped(ax2, bottom, PG_LABEL_SIZE); style_axis(ax2, len(Q))
ax2.set_ylim(0.8, max(pg_sf + pg_sl) * 40)
ax2.set_ylabel("vs Postgres", fontsize=AXIS_LABEL_SIZE)
ax2.set_xticks(x); ax2.set_xticklabels(Q)  # Q1..Q22 need no axis title
h = ax1.containers
# column-major, 3 columns x 2 rows: the two long JetStream entries share the first column
fig.legend(handles=[h[0], h[3], h[4], h[2], h[1]], loc="lower center", ncol=3, frameon=False, fontsize=LEGEND_SIZE,
           bbox_to_anchor=(0.5, -0.02), handlelength=1.2, handletextpad=0.4, columnspacing=1.2)
fig.tight_layout(rect=(0, 0.09, 1, 1), h_pad=0.4)
left = place_labels(fig, ax2, bg)
if left: print(f"warning: {left} label collisions")
for ext in ("png", "pdf"):
    fig.savefig(f"{S}/readonly_speedup_sf20_40t_samebase.{ext}", dpi=200, bbox_inches="tight", facecolor="white")
for name, vals, _ in top + bottom: print(f"{name:90} {geomean(vals):9.2f}")
