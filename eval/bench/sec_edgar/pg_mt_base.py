#!/usr/bin/env python3
"""Multithreaded PostgreSQL base timings for the SEC-EDGAR default-parameter queries.

usage: pg_mt_base.py PORT QUERY_DIR OUT_CSV WORKERS
1 warmup + 3 hot runs per query (same protocol as GenDB's baselines), with
max_parallel_workers_per_gather=WORKERS. Records rows and the workers launched.
"""
import csv
import re
import statistics
import sys
import time

import psycopg2

port, qdir, out, workers = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
conn = psycopg2.connect(host="127.0.0.1", port=port, user="postgres",
                        password="postgres", dbname="sec_edgar")
conn.autocommit = True
cur = conn.cursor()
cur.execute(f"SET max_parallel_workers_per_gather = {workers}")
rows_out = []
for q in [1, 2, 3, 4, 6, 24]:
    sql = open(f"{qdir}/q{q}.sql").read().strip().rstrip(";")
    cur.execute("EXPLAIN (ANALYZE, FORMAT TEXT) " + sql)
    plan = "\n".join(r[0] for r in cur.fetchall())
    launched = sum(int(m) for m in re.findall(r"Workers Launched: (\d+)", plan))
    times = []
    for i in range(4):
        t = time.perf_counter()
        cur.execute(sql)
        n = len(cur.fetchall())
        times.append((time.perf_counter() - t) * 1e3)
    hot = times[1:]
    row = {"query": f"Q{q}", "workers_per_gather": workers, "workers_launched": launched,
           "rows": n, "warmup_ms": round(times[0], 3),
           "hot_ms": ";".join(f"{x:.3f}" for x in hot),
           "hot_avg_ms": round(statistics.mean(hot), 3)}
    rows_out.append(row)
    print(row, flush=True)
with open(out, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows_out[0]))
    w.writeheader()
    w.writerows(rows_out)
