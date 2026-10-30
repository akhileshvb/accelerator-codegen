"""Accelerator read speedup over MVs + pg_ivm, 40 threads each (TPC-H SF20, PostgreSQL 17).

Every point: 8 closed-loop readers (5 processes each = 40 threads) + one writer committing the TPC-H refresh functions
(RF1/RF2 of the dbgen update sets, ~150k rows each, plus the 6 point writes) every 120 / 60 / 30 s; read-only = no
writer.  Speedup per query = median in-window MV read / median in-window accelerator read (run_sql_backend
per_query p50), median over repetitions.  Writes are offered at the target rate; each system's achieved rows/s is
reported next to the chart (the MV writer falls behind).
  plot.py [results dir (default ../data)] [out dir (default: results dir)]   -> speedup_vs_mv_40t.{png,pdf,csv}, summary.json
"""
import glob, gzip, json, math, os, statistics, sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
from matplotlib.ticker import FuncFormatter, LogLocator

R = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'data')
O = sys.argv[2] if len(sys.argv) > 2 else R
os.makedirs(O, exist_ok=True)
Q = range(1, 23)
IVS = [0, 120, 60, 30]
gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
plt.rcParams.update({'font.size': 8, 'axes.titlesize': 9.5, 'axes.labelsize': 8.5, 'xtick.labelsize': 8,
                     'ytick.labelsize': 8, 'legend.fontsize': 8, 'axes.linewidth': 0.6,
                     'axes.spines.top': False, 'axes.spines.right': False})


def runs(system, iv):
    out = {}
    for f in glob.glob(f'{R}/sweep_{system}/postgres/rf-fixed/interval{iv}s/*/rep*/{system}.json*'):  # .json or .json.gz
        r = json.load(gzip.open(f, 'rt') if f.endswith('.gz') else open(f))
        out[r['rep']] = r
    return out


series, summary = [], {}
for iv in IVS:
    mv, ac = runs('mv', iv), runs('accel', iv)
    reps = sorted(set(mv) & set(ac))
    if not reps:
        continue
    sp = {q: [mv[k]['per_query'][str(q)]['p50_us'] / ac[k]['per_query'][str(q)]['p50_us'] for k in reps] for q in Q}
    med = {q: statistics.median(v) for q, v in sp.items()}
    name = 'Read-only' if iv == 0 else {120: '1,250 rows/s', 60: '2,500 rows/s', 30: '5,000 rows/s'}[iv]
    series.append((name, med, sp))
    summary[name] = {
        'reps': reps, 'geomean': gm(med.values()), 'faster_on': sum(v > 1 for v in med.values()),
        'mv_rows_per_s': [round(mv[k]['rows_per_s']) for k in reps], 'accel_rows_per_s': [round(ac[k]['rows_per_s']) for k in reps],
        'mv_rf_commits': [mv[k]['rf_commits'] for k in reps], 'accel_rf_commits': [ac[k]['rf_commits'] for k in reps],
        'mv_valid': [mv[k].get('valid') for k in reps], 'accel_valid': [ac[k].get('valid') for k in reps],
        'mv_qps': [round(mv[k]['qps'], 1) for k in reps], 'accel_qps': [round(ac[k]['qps'], 1) for k in reps],
        'per_query': {q: {'speedup_median': med[q], 'speedups': sp[q],
                          'mv_p50_ms': [mv[k]['per_query'][str(q)]['p50_us'] / 1e3 for k in reps],
                          'accel_p50_ms': [ac[k]['per_query'][str(q)]['p50_us'] / 1e3 for k in reps]} for q in Q}}
    s = summary[name]
    print(f"{name:>12}: geomean {s['geomean']:.1f}x, faster on {s['faster_on']}/22, reps {reps}, "
          f"achieved rows/s mv {s['mv_rows_per_s']} accel {s['accel_rows_per_s']}, valid mv {s['mv_valid']} accel {s['accel_valid']}")
json.dump(summary, open(f'{O}/summary.json', 'w'), indent=1)
with open(f'{O}/speedup_vs_mv_40t.csv', 'w') as f:
    f.write('query,' + ','.join(n for n, _, _ in series) + '\n')
    for q in Q:
        f.write(f'{q},' + ','.join(f'{m[q]:.3f}' for _, m, _ in series) + '\n')

n = len(Q); GM_X, GM_STEP = n + 1.5, 0.46
fig, ax = plt.subplots(figsize=(7.3, 2.75))
colors = ['#4b2a7b'] + list(plt.cm.Greens(np.linspace(0.45, 0.9, max(len(series) - 1, 1))))
w = 0.84 / max(len(series), 1)
for i, ((name, med, sp), c) in enumerate(zip(series, colors)):
    vals = [med[q] for q in Q]; off = i - (len(series) - 1) / 2
    lo = [med[q] - min(sp[q]) for q in Q]; hi = [max(sp[q]) - med[q] for q in Q]
    ax.bar(np.arange(n) + off * w, vals, w, color=c, linewidth=0, label=name,
           yerr=[lo, hi] if len(sp[1]) > 1 else None, error_kw={'lw': 0.4, 'capsize': 0})
    g = gm(vals); gx = GM_X + off * GM_STEP
    ax.bar(gx, g, GM_STEP * 0.92, color=c, linewidth=0)
    ax.text(gx, g * 1.12, f'{g:.1f}', ha='center', va='bottom', fontsize=7, rotation=90)
ax.axhline(1, color='black', lw=0.5)
ax.set_yscale('log')
ax.yaxis.set_major_locator(LogLocator(base=10))
ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f'{v:,.0f}' if v >= 1 else f'{v:g}'))
ax.set_xticks(list(range(n)) + [GM_X]); ax.set_xticklabels([f'Q{q}' for q in Q] + ['GM'])
ax.grid(True, axis='y', which='major', color='#dddddd', lw=0.6); ax.set_axisbelow(True)
ax.set_xlim(-0.6, GM_X + 2 * GM_STEP + 0.35)
ax.set_ylabel('Speedup over MVs + IVM')
ax.set_title('TPC-H (SF=20): Speedup over Materialized Views + IVM (PostgreSQL, 40 vs 40 threads)', pad=16)
ax.legend(ncol=len(series), loc='lower center', bbox_to_anchor=(0.5, 1.0), frameon=False,
          handlelength=1.4, handleheight=0.8, columnspacing=1.2, borderaxespad=0.1)
fig.tight_layout(pad=0.3)
for ext in ('png', 'pdf'):
    fig.savefig(f'{O}/speedup_vs_mv_40t.{ext}', dpi=300)
print('wrote', f'{O}/speedup_vs_mv_40t.png')
