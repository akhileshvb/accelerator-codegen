"""Accelerators vs materialized views + pg_ivm on Postgres SF20 (accel_pg_sf20r, rf-fixed sweep of 2026-10-19/20), in the
format of eval/compare/results/mvcmp_20261009_plot_combined.py.

Figure 1 (read speedup): per query, MV median read latency / accelerator median read latency, reads pooled over the
repetitions of a rate (in-window, ok reads; 8 readers, the RF writer at the rate's commit interval).
Figure 2 (workload speedup): per query, the offered-workload formula of mvcmp/analysis/offered_workload.py (the MV run's
reads at each system's mean latency + the offered write stream at each system's measured commit cost, writes split
evenly over the 22 queries), median over the repetitions.  Read-only has no writes, so its workload bars are the read
speedup of Figure 1 (median latency ratio).  Both figures' data go to one CSV, mvcmp_speedup_sf20.csv: the workload
figure's series (read-only = read speedup).
Runs left out: read-only rep 0 (first MV run after the build, cold views), accelerator runs marked invalid (a worker
rebuilt: 1,000 rows/s rep 1, every uncapped run) -- except 5,000 rows/s, where every run is invalid: plotted, starred."""
import json, glob, os, math, statistics, collections, csv, sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FuncFormatter, LogLocator
sys.path.insert(0, "/disk3/akhilvb_tpch/pg_sf20r/mvcmp/analysis")
import offered_workload as OW  # noqa: E402  (runs its summary on import: harmless)

plt.rcParams.update({'font.size': 8, 'axes.titlesize': 9.5, 'axes.labelsize': 8.5, 'xtick.labelsize': 8,
                     'ytick.labelsize': 8, 'legend.fontsize': 8, 'axes.linewidth': 0.6,
                     'axes.spines.top': False, 'axes.spines.right': False,
                     'xtick.major.width': 0.6, 'ytick.major.width': 0.6, 'ytick.minor.width': 0.4,
                     'xtick.major.size': 2.5, 'ytick.major.size': 3, 'ytick.minor.size': 1.5})
D = "/disk3/akhilvb_tpch/pg_sf20r/mvcmp/postgres/rf-fixed"; OUT = os.path.dirname(os.path.abspath(__file__))
Q = range(1, 23); gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
RATES = [("interval0s", "Read-only"), ("interval268.6s", "10 rows/s"), ("interval53.7s", "50 rows/s"),
         ("interval13.43s", "200 rows/s"), ("interval2.686s", "1,000 rows/s"), ("interval0.537s", "5,000 rows/s")]
# every 5,000 rows/s accelerator run is marked invalid (a shard worker rebuilt mid-run); they are plotted anyway, starred
KEEP_INVALID = {"interval0.537s"}
RO_RERUN = "/disk3/akhilvb_tpch/pg_sf20r/q10dev/ro_rerun"

def runs(iv):
    for d in sorted(glob.glob(f"{D}/{iv}/*/rep*")):
        if not (os.path.exists(f"{d}/mv.json") and os.path.exists(f"{d}/accel.json")): continue
        if iv == "interval0s" and d.endswith("rep0"): continue
        m = OW.load(f"{d}/mv.json")
        # read-only: the accelerator side is the rerun with the Q10-windows build (same flags / seed / reps)
        acc = f"{RO_RERUN}/{d.rsplit('/', 1)[1]}/cps0/{d.rsplit('/', 1)[1]}/accel.json" if iv == "interval0s" else f"{d}/accel.json"
        if not os.path.exists(acc): continue
        a = OW.load(acc)
        if not a["valid"] and iv not in KEEP_INVALID: continue
        yield d, a, m

def inwin(r):
    out = collections.defaultdict(list)
    for q, t, us, st in r["reads_raw"]:
        if st == "ok" and t >= r.get("warmup", 30): out[int(q)].append(us)
    return out

read_series, work_series, used = [], [], {}
detail = []  # joint CSV rows
for iv, name in RATES:
    pa, pm = collections.defaultdict(list), collections.defaultdict(list); wl = collections.defaultdict(list); n = 0
    tot, acc_rps, mv_rps, invalid = [], [], [], 0
    for d, a, m in runs(iv):
        n += 1
        for q, v in inwin(a).items(): pa[q] += v
        for q, v in inwin(m).items(): pm[q] += v
        ma, mm = OW.mean_reads(a), OW.mean_reads(m); N = {q: m["per_query"][q]["reads"] for q in m["per_query"]}
        ac, mc = OW.ok_commits(a), OW.ok_commits(m); longer = ac if len(ac) >= len(mc) else mc
        rate = int(name.split()[0].replace(",", "").rstrip("*")) if name != "Read-only" else 0
        stream, rows = [], 0
        for c in longer:
            if rows >= rate * m["seconds"]: break
            stream.append((c[0], c[4])); rows += c[4]
        i = 0  # stream shorter than the offered rows (5,000 rows/s): repeat its own commit mix (offered_workload.py)
        while rows < rate * m["seconds"] and longer:
            c = longer[i % len(longer)]; stream.append((c[0], c[4])); rows += c[4]; i += 1
        wa, wm = OW.charge(stream, ac), OW.charge(stream, mc)
        for q in N: wl[int(q)].append((N[q] * mm[q] + wm / 22) / (N[q] * ma[q] + wa / 22))
        ra = sum(N[q] * ma[q] for q in N); rm = sum(N[q] * mm[q] for q in N)
        tot.append((rm + wm) / (ra + wa)); acc_rps.append(a.get("rows_per_s", 0)); mv_rps.append(m.get("rows_per_s", 0))
        invalid += not a["valid"]
    used[name] = n
    for q in Q:
        detail.append({"rate": name, "query": f"Q{q}",
                       "read_speedup": statistics.median(pm[q]) / statistics.median(pa[q]),
                       "workload_speedup": statistics.median(wl[q]),
                       "accel_p50_ms": statistics.median(pa[q]) / 1e3, "mv_p50_ms": statistics.median(pm[q]) / 1e3,
                       "accel_mean_ms": statistics.mean(pa[q]) / 1e3, "mv_mean_ms": statistics.mean(pm[q]) / 1e3,
                       "accel_reads": len(pa[q]), "mv_reads": len(pm[q])})
    detail.append({"rate": name, "query": "GM",
                   "read_speedup": gm([statistics.median(pm[q]) / statistics.median(pa[q]) for q in Q]),
                   "workload_speedup": gm([statistics.median(wl[q]) for q in Q]),
                   "workload_speedup_total": statistics.median(tot), "runs": n, "accel_runs_invalid": invalid,
                   "accel_rows_per_s": statistics.median(acc_rps), "mv_rows_per_s": statistics.median(mv_rps),
                   "accel_build": "Q10 windows + malloc trim (rerun)" if name == "Read-only" else "sweep build (2026-10-19)"})
    read_series.append((name, {q: statistics.median(pm[q]) / statistics.median(pa[q]) for q in Q}))
    work_series.append((name, {q: statistics.median(wl[q]) for q in Q}))

# read-only: no writes, so the workload speedup is the read speedup (median latency ratio, as in Figure 1)
work_series[0] = (work_series[0][0], read_series[0][1])

def plot(series, fname, title, ylabel, ylim, csv_name=None):
    labels = [f'Q{q}' for q in Q] + ['GM']; n = len(Q); GM_X, GM_STEP = n + 1.5, 0.46
    fig, ax = plt.subplots(figsize=(7.3, 2.75))
    colors = ['#4b2a7b'] + list(plt.cm.Greens(np.linspace(0.35, 0.9, len(series) - 1)))
    w = 0.84 / len(series)
    for i, ((name, s), c) in enumerate(zip(series, colors)):
        vals = [s[q] for q in Q]; g = gm(vals); off = i - (len(series) - 1) / 2
        ax.bar(np.arange(n) + off * w, vals, w, color=c, linewidth=0, label=name)
        gx = GM_X + off * GM_STEP
        ax.bar(gx, g, GM_STEP * 0.92, color=c, linewidth=0)
        ax.text(gx, g * 1.12, f'{g:.1f}', ha='center', va='bottom', fontsize=7, rotation=90)
        print(f'{fname}: {name:>13}: geomean {g:.2f}x, faster on {sum(v > 1 for v in vals)}/22 ({used[name]} runs)')
    ax.axhline(1, color='black', lw=0.5)
    ax.set_yscale('log'); ax.set_ylim(*ylim)
    ax.yaxis.set_major_locator(LogLocator(base=10))
    ax.yaxis.set_minor_locator(LogLocator(base=10, subs=np.arange(2, 10), numticks=20))
    ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f'{v:,.0f}' if v >= 1 else f'{v:g}'))
    ax.set_xticks(list(range(n)) + [GM_X]); ax.set_xticklabels(labels)
    ax.grid(True, axis='y', which='major', color='#dddddd', lw=0.6); ax.set_axisbelow(True)
    ax.set_xlim(-0.6, GM_X + 2.5 * GM_STEP + 0.35)
    ax.set_ylabel(ylabel); ax.set_title(title, pad=16)
    ax.legend(ncol=len(series), loc='lower center', bbox_to_anchor=(0.5, 1.0), frameon=False,
              handlelength=1.4, handleheight=0.8, columnspacing=1.2, borderaxespad=0.1)
    fig.tight_layout(pad=0.3)
    for ext in ('png', 'pdf'): fig.savefig(f'{OUT}/{fname}.{ext}', dpi=300)
    if csv_name is None: return
    with open(f'{OUT}/{csv_name}.csv', 'w', newline='') as f:
        wr = csv.writer(f); wr.writerow(['query'] + [nm for nm, _ in series])
        for q in Q: wr.writerow([f'Q{q}'] + [round(s[q], 4) for _, s in series])
        wr.writerow(['GM'] + [round(gm([s[q] for q in Q]), 4) for _, s in series])

lo = min(v for _, s in read_series for v in s.values())
plot(read_series, 'mvcmp_read_speedup_sf20', 'TPC-H (SF=20): Read Speedup over Materialized Views + IVM (PostgreSQL)',
     'Speedup over MVs + IVM', (min(0.5, lo / 2), 4000))
lo = min(v for _, s in work_series for v in s.values())
plot(work_series, 'mvcmp_workload_speedup_sf20', 'TPC-H (SF=20): Workload Speedup over Materialized Views + IVM (PostgreSQL)',
     'Workload speedup over MVs + IVM', (min(0.5, lo / 2), 4000), csv_name='mvcmp_speedup_sf20')

# read-only: the figure uses the read speedup as the workload speedup (no writes; see work_series[0]): the CSV too
for r in detail:
    if r["rate"] == "Read-only":
        r["workload_speedup"] = r["read_speedup"]
        if r["query"] == "GM":
            r.pop("workload_speedup_total", None)
JOINT = os.path.expanduser("~/mvcmp_sf20_speedups.csv")
cols = ["rate", "query", "read_speedup", "workload_speedup", "accel_p50_ms", "mv_p50_ms", "accel_mean_ms", "mv_mean_ms",
        "accel_reads", "mv_reads", "workload_speedup_total", "runs", "accel_runs_invalid", "accel_rows_per_s",
        "mv_rows_per_s", "accel_build"]
with open(JOINT, "w", newline="") as f:
    wr = csv.DictWriter(f, fieldnames=cols); wr.writeheader()
    for r in detail:
        wr.writerow({k: (round(v, 4) if isinstance(v, float) else v) for k, v in r.items()})
import shutil
for fn in ("mvcmp_read_speedup_sf20", "mvcmp_workload_speedup_sf20"):
    shutil.copy(f"{OUT}/{fn}.pdf", os.path.expanduser(f"~/{fn}.pdf"))
print("wrote", JOINT)
