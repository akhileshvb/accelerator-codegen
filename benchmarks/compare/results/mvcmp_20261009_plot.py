"""Accelerators vs materialized views (+ pg_ivm IVM) under concurrent RF writes, Postgres TPC-H SF20.

Reads: per (query, target write rate) speedup = MV median read latency / accelerator median read latency, every
in-window ok read pooled over the runs (bars) with min-max of per-run medians (whiskers).  The MV writer cannot
sustain any target rate (one RF1 commit takes minutes of pg_ivm maintenance), so its reads come from every MV run
with the writer busy: the 1.25k rows/s sweep run and the uncapped throughput run.
Writes: maximum sustained rows/s.  Accelerator: the v3 sweep's runs whose target exceeds its writer (15/10/5 s
intervals: it commits back to back).  MV: the uncapped throughput run, rows committed / writer busy time.

  python mvcmp_20261009_plot.py      (writes mvcmp_20261009_* next to this script)
"""
import csv, glob, json, math, os, statistics
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
V3 = f'{HERE}/backends_20261009_pg_rffixed_v3/postgres/rf-fixed'
# accelerator runs on the per-accelerator-worker build (+ Q9 / Q11 / Q21 read paths), 2026-10-09 22:18
V2 = f'{HERE}/mvcmp_20261009_accel_v2/postgres/rf-fixed'
ACCEL = {120: f'{V2}/interval120s', 60: f'{V2}/interval60s', 30: f'{V2}/interval30s'}
MV_RUNS = sorted(glob.glob(f'{HERE}/mvcmp_20261009_mv/postgres/rf-fixed/interval*s/cps*/rep*/mv.json')) + \
          sorted(glob.glob(f'{HERE}/mvcmp_20261009_mv/postgres/throughput/*/rep*/mv.json'))
Q = range(1, 23)
gm = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
load = lambda f: json.load(open(f))


def reads(r):
    lo, hi, by = r['warmup'], r['warmup'] + r['seconds'], {}
    for q, t, us, st in r['reads_raw']:
        if lo <= t < hi and st == 'ok':
            by.setdefault(q, []).append(us)
    return by


def writer_throughput(r):
    """rows committed / (end of last commit - start of first): back-to-back commits, so the writer's sustained rate."""
    c = [x for x in r['commits_raw'] if x[5] == 'ok']
    span = max(x[1] + x[3] / 1e6 for x in c) - min(x[1] for x in c)
    return sum(x[4] for x in c) / span, span, c


M = [load(f) for f in MV_RUNS]
mv_reads = {}
for r in M:
    for q, v in reads(r).items():
        mv_reads.setdefault(q, []).append(v)
# MV Q20: the runs used a read the planner ran as a nested loop (~30 s); the fixed read (mv_defs.READ[20], a
# materialized key set) measured 610 / 453 / 878 / 581 ms at SF20 with the MV writer running (mv_defs check)
MV_Q20_FIXED_US = [610370, 453450, 877750, 581340]
mv_reads[20] = [MV_Q20_FIXED_US]
mv_pool = {q: [u for v in vs for u in v] for q, vs in mv_reads.items()}

rows, summary = [], []
for iv, d in ACCEL.items():
    A = [load(f) for f in sorted(glob.glob(f'{d}/cps*/rep*/accel.json'))]
    target = statistics.mean(r['rf_rows_per_s'] / r['rf_rate'] for r in A if r['rf_rate']) * A[0]['rate_target']
    acc = {}
    for r in A:
        for q, v in reads(r).items():
            acc.setdefault(q, []).append(v)
    for q in Q:
        a, m = [u for v in acc.get(q, []) for u in v], mv_pool.get(q, [])
        if a and m:
            per = [statistics.median(mv) / statistics.median(av) for av in acc[q] for mv in mv_reads[q]]
            rows.append({'query': q, 'target_rows_s': round(target, -1), 'accel_p50_ms': statistics.median(a) / 1e3,
                         'mv_p50_ms': statistics.median(m) / 1e3, 'speedup': statistics.median(m) / statistics.median(a),
                         'accel_reads': len(a), 'mv_reads': len(m), 'run_min': min(per), 'run_max': max(per)})
    sel = [x['speedup'] for x in rows if x['target_rows_s'] == round(target, -1)]
    summary.append({'target_rows_s': round(target, -1), 'interval_s': iv, 'accel_runs': len(A),
                    'accel_rows_s_achieved': round(statistics.mean(r['rows_per_s'] for r in A)),
                    'accel_valid': all(r['valid'] for r in A), 'queries': len(sel), 'geomean_read_speedup': gm(sel)})

# write throughput
acc_sat = [load(f) for iv in (15, 10, 5) for f in glob.glob(f'{V3}/interval{iv}s/cps*/rep*/accel.json')]
acc_rf = [x[3] / 1e6 for r in acc_sat for x in r['commits_raw'] if x[0] in ('rf1', 'rf2') and x[5] == 'ok']
writes = {'accel_max_rows_s': round(statistics.mean(r['rows_per_s'] for r in acc_sat)),
          'accel_rf_commit_s_p50': statistics.median(acc_rf), 'accel_runs': len(acc_sat)}
thr = [r for r, f in zip(M, MV_RUNS) if '/throughput/' in f]
if thr:
    rate, span, c = writer_throughput(thr[0])
    writes.update({'mv_max_rows_s': round(rate), 'mv_writer_busy_s': round(span),
                   'mv_commit_s_by_kind': {k: round(statistics.median([x[3] / 1e6 for x in c if x[0] == k]), 1)
                                           for k in sorted({x[0] for x in c})},
                   'mv_refresh_s_by_view': {k: round(v['p50'] / 1e6, 1) for k, v in (thr[0].get('mv_refresh_us_by_view') or {}).items()},
                   'mv_valid': thr[0]['valid'], 'mv_final_mismatches': thr[0].get('final_mismatches', [])})
mv_rf1 = [x[3] / 1e6 for r in M for x in r['commits_raw'] if x[0] == 'rf1' and x[5] == 'ok']
writes['mv_rf1_commit_s'] = [round(x) for x in mv_rf1]

with open(f'{HERE}/mvcmp_20261009_speedups.csv', 'w', newline='') as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
with open(f'{HERE}/mvcmp_20261009_summary.json', 'w') as f:
    json.dump({'reads': summary, 'writes': writes, 'mv_runs': [os.path.relpath(x, HERE) for x in MV_RUNS]}, f, indent=1)
for s in summary:
    print(s)
print(writes)

# read speedup bars
groups = [f'Q{q}' for q in Q] + ['Geomean']
fig, ax = plt.subplots(figsize=(18, 6.5))
colors = plt.cm.Blues(np.linspace(0.45, 0.9, len(summary)))
w = 0.84 / len(summary)
for i, (s, c) in enumerate(zip(summary, colors)):
    by = {r['query']: r for r in rows if r['target_rows_s'] == s['target_rows_s']}
    med = np.array([by[q]['speedup'] if q in by else np.nan for q in Q] + [s['geomean_read_speedup']])
    lo = np.array([by[q]['run_min'] if q in by else np.nan for q in Q] + [s['geomean_read_speedup']])
    hi = np.array([by[q]['run_max'] if q in by else np.nan for q in Q] + [s['geomean_read_speedup']])
    x = np.arange(len(groups)) + (i - (len(summary) - 1) / 2) * w
    ax.bar(x, med, w, color=c, edgecolor='white', linewidth=0.5, label=f"{s['target_rows_s']:,.0f} rows/s")
    ax.errorbar(x, med, yerr=[np.maximum(med - lo, 0), np.maximum(hi - med, 0)], fmt='none', ecolor='black', elinewidth=0.8, capsize=2)
    ax.text(x[-1], max(s['geomean_read_speedup'], 1) * 1.3, f"{s['geomean_read_speedup']:,.1f}×", ha='center', va='bottom', fontsize=8, rotation=90)
ax.axhline(1, color='black', lw=0.8)
ax.axvline(len(groups) - 1.5, color='gray', lw=0.8, ls='--')
ax.set_yscale('log')
ax.set_xticks(range(len(groups))); ax.set_xticklabels(groups)
ax.set_xlim(-0.6, len(groups) - 0.4)
ax.set_ylabel('Read speedup over materialized views + IVM (×, log)')
ax.set_title('Per-accelerator read speedup vs. materialized views (pg_ivm) under concurrent writes (Postgres, TPC-H SF20, 8 readers)')
ax.grid(True, axis='y', alpha=0.3)
ax.legend(title='Accelerator target write rate', ncol=len(summary), loc='upper left')
fig.tight_layout()
fig.savefig(f'{HERE}/mvcmp_20261009_read_speedup_vs_mv.png', dpi=150)

# write throughput bars
if 'mv_max_rows_s' in writes:
    fig, ax = plt.subplots(figsize=(5, 4.5))
    vals = [writes['accel_max_rows_s'], writes['mv_max_rows_s']]
    ax.bar(['Accelerators', 'MVs + IVM'], vals, color=[plt.cm.Blues(0.8), plt.cm.Greys(0.5)])
    for i, v in enumerate(vals):
        ax.text(i, v * 1.02, f'{v:,}', ha='center', va='bottom')
    ax.set_ylabel('Max sustained write rate (rows touched / s)')
    ax.set_title(f'Max write throughput (TPC-H RF, SF20)\naccelerators {vals[0] / vals[1]:.0f}× higher')
    ax.grid(True, axis='y', alpha=0.3)
    fig.tight_layout()
    fig.savefig(f'{HERE}/mvcmp_20261009_write_throughput.png', dpi=150)
print('wrote plots')
