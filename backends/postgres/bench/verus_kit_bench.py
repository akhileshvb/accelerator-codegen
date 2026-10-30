#!/usr/bin/env python3
"""Verus kit reads vs the plain SQL query on Postgres, and apply latency per write batch.

  verus_kit_bench.py reads <bench> <queries,..> [sets=5] [reps=5] [--parallel N]
  verus_kit_bench.py apply <bench> <kit_id> <probe_args> <n_batches>

reads: params from eval/bench/<bench>/gen_params.py, the query from workloads/<bench>/q<N>.sql ($n -> literals),
the kit as accel_<bench>_q<N>_vgen(args).  Per param set one untimed warm-up of each, then `reps` timed reps.
Prints one JSON line per (query, set) and a summary line per query (ms, median [min, max]).
apply: a batch = one submission's rows in every table keyed by adsh, copied under a fresh adsh in one transaction;
times the transaction and the first kit read after it (which waits for the apply; probe_args '-' skips it), with the capture triggers
disabled (plain) and enabled (accelerated).
Connection: libpq PG* environment, database TPCH_PG_DB.
"""
import json
import os
import re
import statistics
import subprocess
import sys
import time

import psycopg2

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


def connect():
    conn = psycopg2.connect(dbname=os.environ["TPCH_PG_DB"], options=os.environ.get("PGOPTIONS", ""))
    conn.autocommit = True
    return conn


def bind(sql, args):
    values = [a.strip() for a in re.findall(r"'(?:[^']|'')*'|[^,]+", args)]
    return re.sub(r"\$(\d+)", lambda m: values[int(m.group(1)) - 1], sql)


def timed(cur, sql):
    start = time.perf_counter()
    cur.execute(sql)
    rows = cur.fetchall()
    return time.perf_counter() - start, rows


def same(a, b):
    def eq(p, q):
        if isinstance(p, float) or isinstance(q, float) or hasattr(p, "as_tuple"):
            return abs(float(p) - float(q)) <= 1e-9 * max(1.0, abs(float(p)))
        return p == q
    return len(a) == len(b) and all(len(x) == len(y) and all(map(eq, x, y)) for x, y in zip(a, b))


def stat(values, scale=1e3, digits=2):
    return [round(scale * statistics.median(values), digits), round(scale * min(values), digits),
            round(scale * max(values), digits)]


def counts(cur):
    cur.execute("SELECT accel_get_execute_counts()")
    return cur.fetchone()[0]


def reads(bench, queries, sets, reps, parallel):
    params = json.loads(subprocess.check_output(
        [sys.executable, f"{ROOT}/eval/bench/{bench}/gen_params.py", str(sets), "1"]))["queries"]
    cur = connect().cursor()
    cur.execute(f"SET max_parallel_workers_per_gather = {parallel}")
    for q in queries:
        sql = "\n".join(l for l in open(f"{ROOT}/workloads/{bench}/q{q}.sql").read().splitlines()
                        if not l.lstrip().startswith("--"))
        kit = f"accel_{bench}_q{q}_vgen"
        plains, accels, speedups = [], [], []
        for s in params[str(q)]["sets"]:
            args = s["verus"]
            query = bind(sql, args)
            before = counts(cur)
            _, arows = timed(cur, f"SELECT * FROM {kit}({args})")  # the first call may build
            _, prows = timed(cur, query)
            a = [timed(cur, f"SELECT * FROM {kit}({args})")[0] for _ in range(reps)]
            p = [timed(cur, query)[0] for _ in range(reps)]
            print(json.dumps({"q": q, "args": args, "rows": len(prows), "match": same(arows, prows),
                              "plain_ms": stat(p), "verus_ms": stat(a, digits=3), "counts_changed": counts(cur) != before}),
                  flush=True)
            plains.append(statistics.median(p))
            accels.append(statistics.median(a))
            speedups.append(plains[-1] / accels[-1])
        print(json.dumps({"q": q, "summary": True, "parallel": parallel, "plain_ms": stat(plains),
                          "verus_ms": stat(accels, digits=3), "speedup": stat(speedups, scale=1, digits=1)}), flush=True)


def apply(bench, kit, probe_args, batches):
    cur = connect().cursor()
    cur.execute("SELECT adsh FROM sub TABLESAMPLE SYSTEM (0.2) LIMIT %s", (batches,))
    adshes = [r[0] for r in cur.fetchall()]
    cur.execute("SELECT table_name FROM information_schema.columns WHERE column_name='adsh' AND table_schema='public' "
                "ORDER BY (table_name='sub') DESC, table_name")
    tables = [r[0] for r in cur.fetchall()]
    columns = {}
    for t in tables:
        cur.execute("SELECT column_name FROM information_schema.columns WHERE table_name=%s AND table_schema='public' "
                    "ORDER BY ordinal_position", (t,))
        columns[t] = [r[0] for r in cur.fetchall()]
    try:
        for label, action in (("plain", "DISABLE"), ("accelerated", "ENABLE")) * 2:  # the second pair runs on warm pages
            for t in tables:
                cur.execute(f"ALTER TABLE {t} {action} TRIGGER USER")
            txn, after, sizes = [], [], []
            for adsh in adshes:
                new = adsh[:-1] + ("z" if adsh[-1] != "z" else "y")
                start = time.perf_counter()
                cur.execute("BEGIN")
                rows = 0
                for t in tables:
                    select = ", ".join("%s" if c == "adsh" else c for c in columns[t])
                    cur.execute(f"INSERT INTO {t} SELECT {select} FROM {t} WHERE adsh = %s", (new, adsh))
                    rows += cur.rowcount
                cur.execute("COMMIT")
                txn.append(time.perf_counter() - start)
                sizes.append(rows)
                if label == "accelerated" and probe_args != "-":
                    after.append(timed(cur, f"SELECT * FROM {kit}({probe_args})")[0])
                for t in reversed(tables):
                    cur.execute(f"DELETE FROM {t} WHERE adsh = %s", (new,))
            print(json.dumps({"mode": label, "batches": len(adshes), "rows_per_batch_median": statistics.median(sizes),
                              "txn_ms": stat(txn), "first_read_after_ms": stat(after) if after else None}), flush=True)
    finally:
        for t in tables:
            cur.execute(f"ALTER TABLE {t} ENABLE TRIGGER USER")


if __name__ == "__main__":
    mode, bench = sys.argv[1], sys.argv[2]
    rest = [a for a in sys.argv[3:]]
    parallel = int(rest.pop(rest.index("--parallel") + 1)) if "--parallel" in rest else 0
    if parallel or "--parallel" in rest:
        rest.remove("--parallel")
    if mode == "reads":
        reads(bench, rest[0].split(","), int(rest[1]) if len(rest) > 1 else 5, int(rest[2]) if len(rest) > 2 else 5, parallel)
    else:
        apply(bench, rest[0], rest[1], int(rest[2]))
