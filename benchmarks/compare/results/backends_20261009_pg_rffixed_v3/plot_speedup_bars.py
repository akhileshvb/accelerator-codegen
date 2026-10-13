"""Grouped bar chart: per-accelerator workload speedup at each target write rate (rows touched/s).

Per rep: speedup = median base in-window read latency / median accel in-window read latency.
Bar = median of the reps, whiskers = min-max.  Geomean group: geomean over the 22 queries of
the bars; whiskers = min-max of the per-rep geomeans.  Target rows/s = rows per RF slot x
scheduled slot rate.
"""
import glob, json, math, statistics, sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

R, OUT = sys.argv[1], sys.argv[2]   # optional argv[3]: minimum target rows/s to plot
ivs = sorted({int(p.split('interval')[1].split('s')[0]) for p in glob.glob(f'{R}/interval*s')}, reverse=True)
Q = range(1, 23)
gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))

def medians(r):
    lo, hi, by = r['warmup'], r['warmup'] + r['seconds'], {}
    for q, t, us, st in r['reads_raw']:
        if lo <= t < hi and st == 'ok': by.setdefault(q, []).append(us)
    return {q: statistics.median(v) for q, v in by.items()}

sp, target = {}, {}
for iv in ivs:
    runs = {s: {json.load(open(f))['rep']: json.load(open(f)) for f in glob.glob(f'{R}/interval{iv}s/cps*/rep*/{s}.json')} for s in ('accel', 'base')}
    assert sorted(runs['accel']) == sorted(runs['base']) == [0, 1, 2], iv
    assert all(r['valid'] for rs in runs.values() for r in rs.values()), iv
    allr = [*runs['accel'].values(), *runs['base'].values()]
    target[iv] = statistics.mean(r['rf_rows_per_s'] / r['rf_rate'] for r in allr) * allr[0]['rate_target']
    for rep in runs['accel']:
        a, b = medians(runs['accel'][rep]), medians(runs['base'][rep])
        for q in Q:
            sp.setdefault((q, iv), []).append(b[q] / a[q])   # KeyError = a hole in the grid

MIN_TARGET = float(sys.argv[3]) if len(sys.argv) > 3 else 0   # only plot target rates >= this (rows/s)
ivs = [iv for iv in ivs if target[iv] >= MIN_TARGET * 0.99]
label = lambda iv: f'{round(target[iv], -2):,.0f} rows/s'
groups = [f'Q{q}' for q in Q] + ['Geomean']
fig, ax = plt.subplots(figsize=(18 if len(ivs) > 3 else 15, 6.5))
colors = plt.cm.Blues(np.linspace(0.45, 0.9, len(ivs)))
w = 0.84 / len(ivs)
print('geomean speedup by target write rate:')
for i, (iv, c) in enumerate(zip(ivs, colors)):
    vals = [sp[q, iv] for q in Q]
    med = [statistics.median(v) for v in vals]
    per_rep_gm = [gm([v[r] for v in vals]) for r in range(3)]
    g = gm(med)
    med.append(g); vals.append(per_rep_gm)
    print(f'  {label(iv):>14}: {g:,.0f}x  (per-rep {", ".join(f"{x:,.0f}" for x in per_rep_gm)})')
    x = np.arange(len(groups)) + (i - (len(ivs) - 1) / 2) * w
    err = [[m - min(v) for m, v in zip(med, vals)], [max(v) - m for m, v in zip(med, vals)]]
    ax.bar(x, med, w, color=c, label=label(iv), edgecolor='white', linewidth=0.5)
    ax.errorbar(x, med, yerr=err, fmt='none', ecolor='black', elinewidth=0.8, capsize=2)
    ax.text(x[-1], max(per_rep_gm) * 1.3, f'{g:,.0f}×', ha='center', va='bottom', fontsize=8, rotation=90)
ax.axvline(len(groups) - 1.5, color='gray', lw=0.8, ls='--')
ax.set_yscale('log'); ax.set_ylim(1, 1e6)
ax.set_xticks(range(len(groups))); ax.set_xticklabels(groups)
ax.set_xlim(-0.6, len(groups) - 0.4)
ax.set_ylabel('Workload speedup over Postgres (×, log)')
ax.set_title('Per-accelerator workload speedup vs. target write rate (Postgres, TPC-H SF20, 8 readers)')
ax.grid(True, axis='y', alpha=0.3)
ax.legend(title='Target write rate', ncol=len(ivs), loc='upper left')
fig.tight_layout()
fig.savefig(OUT, dpi=150)
print('wrote', OUT)
