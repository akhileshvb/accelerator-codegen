#!/usr/bin/env python3
"""Find the fastest PostgreSQL configuration for each SEC-EDGAR default-parameter query.

usage: pg_tune_base.py PORT QUERY_DIR GROUND_TRUTH_DIR OUT_CSV

For every query, each config (work_mem, table-level parallel_workers, jit) is
timed once with EXPLAIN ANALYZE (statement_timeout 180 s). The fastest config is
then run 1 warmup + 3 hot (GenDB's protocol), and its result is compared to the
ground-truth CSV. Writes one row per query with every config's time.
"""
import csv
import statistics
import sys
import time

import psycopg2

port, qdir, gtdir, out = sys.argv[1:5]
TABLES = ["num", "pre", "sub", "tag"]
CONFIGS = [  # name, work_mem, table parallel_workers (None = Postgres picks), jit
    ("wm64MB_auto", "64MB", None, "on"),
    ("wm1GB_auto", "1GB", None, "on"),
    ("wm1GB_auto_nojit", "1GB", None, "off"),
    ("wm4GB_auto", "4GB", None, "on"),
    ("wm4GB_auto_nojit", "4GB", None, "off"),
    ("wm256MB_w32", "256MB", 32, "on"),
    ("wm1GB_w32", "1GB", 32, "on"),
    ("wm1GB_w32_nojit", "1GB", 32, "off"),
]

conn = psycopg2.connect(host="127.0.0.1", port=port, user="postgres",
                        password="postgres", dbname="sec_edgar")
conn.autocommit = True
cur = conn.cursor()
cur.execute("SET max_parallel_workers_per_gather = 32")
cur.execute("SET statement_timeout = '180s'")


def apply(cfg):
    _, wm, workers, jit = cfg
    cur.execute(f"SET work_mem = '{wm}'")
    cur.execute(f"SET jit = {jit}")
    for t in TABLES:
        if workers is None:
            cur.execute(f"ALTER TABLE {t} RESET (parallel_workers)")
        else:
            cur.execute(f"ALTER TABLE {t} SET (parallel_workers = {workers})")


def canon(rows):
    out = []
    for r in rows:
        out.append(tuple(f"{float(v):.4g}" if isinstance(v, float) or type(v).__name__ == "Decimal"
                         else ("" if v is None else str(v)) for v in r))
    return sorted(out)


def ground_truth(q):
    with open(f"{gtdir}/q{q}.csv", newline="") as f:
        rows = list(csv.reader(f))[1:]

    def norm(v):
        try:
            if "." in v or "e" in v.lower():
                return f"{float(v):.4g}"
        except ValueError:
            pass
        return v
    return sorted(tuple(norm(v) for v in r) for r in rows)


results = []
for q in [1, 2, 3, 4, 6, 24]:
    sql = open(f"{qdir}/q{q}.sql").read().strip().rstrip(";")
    per_cfg = {}
    for cfg in CONFIGS:
        apply(cfg)
        try:
            cur.execute("EXPLAIN (ANALYZE, FORMAT JSON) " + sql)
            per_cfg[cfg[0]] = cur.fetchone()[0][0]["Execution Time"]
        except psycopg2.errors.QueryCanceled:
            per_cfg[cfg[0]] = float("inf")
        print(f"Q{q} {cfg[0]}: {per_cfg[cfg[0]]:.1f} ms", flush=True)
    best = min(CONFIGS, key=lambda c: per_cfg[c[0]])
    apply(best)
    times, rows = [], None
    for _ in range(4):
        t = time.perf_counter()
        cur.execute(sql)
        rows = cur.fetchall()
        times.append((time.perf_counter() - t) * 1e3)
    ok = canon(rows) == ground_truth(q)
    row = {"query": f"Q{q}", "best_config": best[0], "hot_avg_ms": round(statistics.mean(times[1:]), 3),
           "hot_ms": ";".join(f"{x:.3f}" for x in times[1:]), "rows": len(rows), "matches_ground_truth": ok}
    row.update({f"explain_ms_{k}": round(v, 1) for k, v in per_cfg.items()})
    results.append(row)
    print(row, flush=True)
for t in TABLES:
    cur.execute(f"ALTER TABLE {t} RESET (parallel_workers)")
with open(out, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(results[0]))
    w.writeheader()
    w.writerows(results)
