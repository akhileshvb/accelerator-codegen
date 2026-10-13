"""Accelerator read speedup over materialized views + pg_ivm, read-only and at three RF write rates (SF20).

Read-only: single client, no concurrent writes, one parameter set per query (seed 35489017).  MV: the mv_defs check
right after the views were built (one call per query, mvcmp_20261009_mv/check.log); accelerators: median of 20 calls
on the installed build (mvcmp_20261009_accel_readonly.json).
Write rates: 8 readers + RF writer, pooled in-window medians (mvcmp_20261009_speedups.csv from mvcmp_20261009_plot.py).
"""
import csv, json, math, os, re
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

from matplotlib.ticker import FuncFormatter, LogLocator

# paper style: default sans font at small sizes, open (left/bottom) frame, light y grid
plt.rcParams.update({'font.size': 8, 'axes.titlesize': 9.5, 'axes.labelsize': 8.5, 'xtick.labelsize': 8,
                     'ytick.labelsize': 8, 'legend.fontsize': 8, 'axes.linewidth': 0.6,
                     'axes.spines.top': False, 'axes.spines.right': False,
                     'xtick.major.width': 0.6, 'ytick.major.width': 0.6, 'ytick.minor.width': 0.4,
                     'xtick.major.size': 2.5, 'ytick.major.size': 3, 'ytick.minor.size': 1.5})

HERE = os.path.dirname(os.path.abspath(__file__))
Q = range(1, 23)
gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))

mv_ro = {}
for line in open(f'{HERE}/mvcmp_20261009_mv/check.log'):
    m = re.match(r'Q(\d+)\s+ok\s.*?mv\s+([\d.]+) ms', line)
    if m:
        mv_ro[int(m.group(1))] = float(m.group(2))
acc_ro = {int(k): v['p50_ms'] for k, v in json.load(open(f'{HERE}/mvcmp_20261009_accel_readonly.json'))['reads'].items()}
series = [('Read-only', {q: mv_ro[q] / acc_ro[q] for q in Q})]
rows = list(csv.DictReader(open(f'{HERE}/mvcmp_20261009_speedups.csv')))
for t in sorted({float(r['target_rows_s']) for r in rows}):
    series.append((f'{t:,.0f} rows/s', {int(r['query']): float(r['speedup']) for r in rows if float(r['target_rows_s']) == t}))

with open(f'{HERE}/mvcmp_20261009_speedups_combined.csv', 'w', newline='') as f:
    w = csv.writer(f)
    w.writerow(['query'] + [name for name, _ in series] + ['accel_readonly_ms', 'mv_readonly_ms'])
    for q in Q:
        w.writerow([q] + [round(s[q], 3) for _, s in series] + [round(acc_ro[q], 3), mv_ro[q]])

labels = [f'Q{q}' for q in Q] + ['GM']
n = len(Q)
# the GM group is spread wider than a query group so its value labels fit
GM_X, GM_STEP = n + 1.5, 0.46
fig, ax = plt.subplots(figsize=(7.3, 2.75))
colors = ['#4b2a7b'] + list(plt.cm.Greens(np.linspace(0.45, 0.9, len(series) - 1)))
w = 0.84 / len(series)
for i, ((name, s), c) in enumerate(zip(series, colors)):
    vals = [s[q] for q in Q]
    g = gm(vals)
    off = i - (len(series) - 1) / 2
    ax.bar(np.arange(n) + off * w, vals, w, color=c, linewidth=0, label=name)
    gx = GM_X + off * GM_STEP
    ax.bar(gx, g, GM_STEP * 0.92, color=c, linewidth=0)
    ax.text(gx, g * 1.12, f'{g:.1f}', ha='center', va='bottom', fontsize=7, rotation=90)
    print(f'{name:>14}: geomean {g:.1f}x, faster on {sum(v > 1 for v in vals)}/22')
ax.axhline(1, color='black', lw=0.5)
ax.set_yscale('log'); ax.set_ylim(0.5, 4000)
ax.yaxis.set_major_locator(LogLocator(base=10))
ax.yaxis.set_minor_locator(LogLocator(base=10, subs=np.arange(2, 10), numticks=20))
ax.yaxis.set_major_formatter(FuncFormatter(lambda v, _: f'{v:,.0f}' if v >= 1 else f'{v:g}'))
ax.set_xticks(list(range(n)) + [GM_X]); ax.set_xticklabels(labels)
ax.grid(True, axis='y', which='major', color='#dddddd', lw=0.6); ax.set_axisbelow(True)
ax.set_xlim(-0.6, GM_X + 2 * GM_STEP + 0.35)
ax.set_ylabel('Speedup over MVs + IVM')
ax.set_title('TPC-H (SF=20): Speedup over Materialized Views + IVM (PostgreSQL)', pad=16)
ax.legend(ncol=len(series), loc='lower center', bbox_to_anchor=(0.5, 1.0), frameon=False,
          handlelength=1.4, handleheight=0.8, columnspacing=1.2, borderaxespad=0.1)
fig.tight_layout(pad=0.3)
fig.savefig(f'{HERE}/mvcmp_20261009_speedup_vs_mv_combined.png', dpi=300)
fig.savefig(f'{HERE}/mvcmp_20261009_speedup_vs_mv_combined.pdf')
print('wrote', f'{HERE}/mvcmp_20261009_speedup_vs_mv_combined.png')
