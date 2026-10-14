"""Bar chart: per-accelerator read-only speedup over Postgres (no concurrent writes).

Per rep: speedup = geomean over the 4 parameter sets of (median base latency / median accel latency),
as written by run_sql_backend.py read.  Bar = median of the reps, whiskers = min-max.  Geomean group:
geomean over the queries of the bars; whiskers = min-max of the per-rep geomeans (over the queries
that rep has).  Later directories override earlier ones per query (e.g. a rerun of Q17/Q18).
"""
import glob, json, math, os, re, statistics, sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

OUT, DIRS = sys.argv[1], sys.argv[2:]
gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))

sp = {}   # q -> {rep: speedup}
for d in DIRS:
    found = {}
    for f in glob.glob(f'{d}/q*_rep*.json'):
        q, rep = map(int, re.match(r'q(\d+)_rep(\d+)\.json', os.path.basename(f)).groups())
        r = json.load(open(f))
        if r['speedup_geomean']: found.setdefault(q, {})[rep] = r['speedup_geomean']
    sp.update(found)
Q = sorted(sp)
for q in Q: print(f'Q{q:<3} reps {sorted(sp[q])}  median {statistics.median(sp[q].values()):,.0f}x')

med = [statistics.median(sp[q].values()) for q in Q]
vals = [list(sp[q].values()) for q in Q]
reps = sorted({k for q in Q for k in sp[q]})
per_rep_gm = [gm([sp[q][k] for q in Q if k in sp[q]]) for k in reps]
g = gm(med)
print(f'geomean over {len(Q)} queries: {g:,.0f}x  (per-rep {", ".join(f"{x:,.0f}" for x in per_rep_gm)})')
med.append(g); vals.append(per_rep_gm)
groups = [f'Q{q}' for q in Q] + ['Geomean']

fig, ax = plt.subplots(figsize=(15, 6.5))
x = np.arange(len(groups))
err = [[max(m - min(v), 0) for m, v in zip(med, vals)], [max(max(v) - m, 0) for m, v in zip(med, vals)]]
ax.bar(x, med, 0.7, color=plt.cm.Blues(0.7), edgecolor='white', linewidth=0.5)
ax.errorbar(x, med, yerr=err, fmt='none', ecolor='black', elinewidth=0.8, capsize=2)
ax.text(x[-1], max(*per_rep_gm, g) * 1.3, f'{g:,.0f}×', ha='center', va='bottom', fontsize=9)
ax.axvline(len(groups) - 1.5, color='gray', lw=0.8, ls='--')
ax.set_yscale('log'); ax.set_ylim(1, 1e6)
ax.set_xticks(x); ax.set_xticklabels(groups)
ax.set_xlim(-0.6, len(groups) - 0.4)
ax.set_ylabel('Read-only speedup over Postgres (×, log)')
ax.set_title('Per-accelerator read-only speedup (Postgres, TPC-H SF20, no concurrent writes)')
ax.grid(True, axis='y', alpha=0.3)
fig.tight_layout()
fig.savefig(OUT, dpi=150)
print('wrote', OUT)
