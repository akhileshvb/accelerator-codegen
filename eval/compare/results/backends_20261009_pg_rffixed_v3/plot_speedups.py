"""Per-accelerator workload speedup vs write load (rows touched/s) for an rf-fixed sweep.

Speedup per (query, interval) = median base read latency / median accel read latency,
pooling every in-window ok read over all reps.  x = accel's achieved rows touched per second
offered.  x = offered write load: rows per RF slot x scheduled slot rate (rows touched/s the
workload asks for).  Hollow markers: accel achieved <90% of it (writer saturated).  Achieved
rates for both sides are printed in the caption.
"""
import glob, json, math, statistics, sys
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.ticker import LogLocator, FuncFormatter

R, OUT = sys.argv[1], sys.argv[2]
ivs = sorted({int(p.split('interval')[1].split('s')[0]) for p in glob.glob(f'{R}/interval*s')}, reverse=True)

def window_reads(r):
    lo, hi = r['warmup'], r['warmup'] + r['seconds']
    return [x for x in r['reads_raw'] if lo <= x[1] < hi and x[3] == 'ok']

rate, cell, mism, problems = {}, {}, set(), []
for iv in ivs:
    runs = {s: [json.load(open(f)) for f in sorted(glob.glob(f'{R}/interval{iv}s/cps*/rep*/{s}.json'))] for s in ('accel', 'base')}
    for s, rs in runs.items():
        if len(rs) != 3: problems.append(f'{iv}s {s}: {len(rs)} reps')
        problems += [f'{iv}s {s} rep{r["rep"]}: invalid' for r in rs if not r.get('valid')]
    for r in runs['accel']: mism.update(r.get('final_mismatches', []))
    rate[iv] = {s: statistics.mean(r['rows_per_s'] for r in rs) for s, rs in runs.items()}
    allr = runs['accel'] + runs['base']
    rate[iv]['offered'] = statistics.mean(r['rf_rows_per_s'] / r['rf_rate'] for r in allr) * allr[0]['rate_target']
    pooled = {s: {} for s in runs}
    for s, rs in runs.items():
        for r in rs:
            for q, _, us, _ in window_reads(r):
                pooled[s].setdefault(q, []).append(us)
    for q in range(1, 23):
        a, b = pooled['accel'].get(q, []), pooled['base'].get(q, [])
        if not a or not b:
            problems.append(f'Q{q} {iv}s: accel {len(a)} / base {len(b)} reads'); continue
        cell[q, iv] = (statistics.median(b) / statistics.median(a), len(a), len(b))

print('grid', len(cell), 'of', 22 * len(ivs), 'cells; problems:', problems or 'none')
for iv in ivs: print(f'{iv:>3}s  offered {rate[iv]["offered"]:7.0f}  accel {rate[iv]["accel"]:7.0f} rows/s  base {rate[iv]["base"]:7.0f} rows/s')

SURF, INK, INK2, MUTED, GRID, BLUE, RED = '#fcfcfb', '#0b0b0b', '#52514e', '#898781', '#e6e5e0', '#2a78d6', '#d03b3b'
plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 9, 'axes.edgecolor': MUTED, 'axes.labelcolor': INK2,
                     'xtick.color': MUTED, 'ytick.color': MUTED, 'text.color': INK})
gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
panels = [(f'Q{q}', q) for q in range(1, 23)] + [('Geomean (22 queries)', None)]
fig, axes = plt.subplots(4, 6, figsize=(17, 11), sharex=True, sharey=True, facecolor=SURF)
xs = [rate[iv]['offered'] for iv in ivs]
iso = [rate[iv]['accel'] >= 0.9 * rate[iv]['offered'] for iv in ivs]
fmt = FuncFormatter(lambda v, _: f'{v:,.0f}×' if v >= 1 else f'{v:g}×')
for ax, (name, q) in zip(axes.flat, panels):
    ax.set_facecolor(SURF)
    ys = [cell[q, iv][0] if q else gm([cell[k, iv][0] for k in range(1, 23)]) for iv in ivs]
    ax.plot(xs, ys, color=BLUE, lw=2, zorder=2)
    for x, y, ok in zip(xs, ys, iso):
        ax.plot(x, y, 'o', ms=7, mfc=BLUE if ok else SURF, mec=BLUE, mew=2, zorder=3)
    ax.annotate(f'{ys[0]:,.0f}×', (xs[0], ys[0]), xytext=(-4, 9), textcoords='offset points', ha='left', fontsize=8, color=INK2)
    ax.annotate(f'{ys[-1]:,.0f}×', (xs[-1], ys[-1]), xytext=(4, 9), textcoords='offset points', ha='right', fontsize=8, color=INK2)
    title = name + ('  ⚠ result mismatch' if q in mism else '')
    ax.set_title(title, loc='left', fontsize=10, fontweight='bold', color=RED if q in mism else INK)
    ax.set_yscale('log'); ax.set_ylim(5, 1e6)
    ax.yaxis.set_major_locator(LogLocator(base=10)); ax.yaxis.set_major_formatter(fmt)
    ax.grid(True, which='major', color=GRID, lw=0.8); ax.set_axisbelow(True)
    for sp in ('top', 'right'): ax.spines[sp].set_visible(False)
    ax.tick_params(length=0)
for ax in axes.flat[len(panels):]: ax.set_visible(False)
for ax in axes[-1]: ax.set_xlabel('offered rows touched / s (log)')
for ax in axes[:, 0]: ax.set_ylabel('speedup (log)')
ax0 = axes.flat[-1]
ax0.set_xscale('log'); ax0.set_xlim(min(xs) / 1.4, max(xs) * 1.4); ax0.set_xticks(xs); ax0.minorticks_off()
ax0.xaxis.set_major_formatter(FuncFormatter(lambda v, _: f'{round(v/500)/2:g}k'))
for ax in axes[-1]: ax.tick_params(axis='x', labelsize=8)
fig.suptitle('Postgres TPC-H SF20 — workload speedup per accelerator vs write load', x=0.01, ha='left', fontsize=14, fontweight='bold')
rates_txt = '   '.join(f'{rate[iv]["offered"]/1000:.1f}k → {rate[iv]["accel"]/1000:.1f}k / {rate[iv]["base"]/1000:.1f}k' for iv in ivs)
fig.text(0.01, 0.945, 'Speedup = base median read latency ÷ accel median, in-window reads pooled over 3 reps, 8 readers.  '
         'Hollow marker = accel achieved <90% of the offered rows/s (its writer saturates near 5.8k); base also falls short there, so those points are not iso-write.', fontsize=9, color=INK2)
fig.text(0.01, 0.925, 'Achieved rows touched/s (offered → accel / base) — ' + rates_txt, fontsize=9, color=MUTED)
fig.tight_layout(rect=(0, 0, 1, 0.915))
fig.savefig(OUT, dpi=150, facecolor=SURF)
print('wrote', OUT)
