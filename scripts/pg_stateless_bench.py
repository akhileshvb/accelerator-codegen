#!/usr/bin/env python3
"""Stateless TPC-H accelerators on PostgreSQL: correctness check and read-only sweep.

  scripts/pg_stateless_bench.py --mode check --queries 1,6 --threads-list 1,4 [--schema public]
  scripts/pg_stateless_bench.py --mode sweep --threads-list 16,32 --seeds 20 --out results.csv

Instances (query parametrizations + reference SQL) come from
benchmarks/tpch/stateless_instances_sf1.jsonl (written by
`stateless_matrix --emit-instances`).

check: for every instance, the reference SQL (plain PostgreSQL), <schema>.accel_tpch_qN(args)
       and, for each T in --threads-list, <schema>.accel_tpch_qN_parallel(args) at T participants
       (twice, for determinism) must return the same rows in the same order.  Numeric values
       compare exactly unless they are non-terminating results (scale > 6, e.g. AVG), which
       compare to 1e-12 relative; floats compare to 1e-9 relative.  Reference results are cached
       in --ref-cache (keyed by database, row counts and SQL).

sweep: every timed repetition is a different parametrization (one seed per rep).  Per query,
       one untimed warm-up of each variant on the first seed, then for each seed: PostgreSQL and
       accel_tpch_qN at 1 participant, and for each T: PostgreSQL and accel_tpch_qN_parallel at T
       participants.  T participants = max_parallel_workers_per_gather = T-1 for both sides
       (PostgreSQL's leader + T-1 workers; the accelerator's leader + T-1 parallel workers).

Connection: libpq environment (PGHOST, PGPORT, PGUSER, PGPASSWORD) and --db.
"""
import argparse
import datetime
import decimal
import hashlib
import json
import os
import statistics
import sys
import time

import psycopg2

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_list(s):
    return [int(x) for x in s.split(",") if x]


def load_instances(path, queries, seeds):
    out = {}
    with open(path) as f:
        for line in f:
            inst = json.loads(line)
            q = inst["query"]
            if q in queries and len(out.setdefault(q, [])) < seeds:
                out[q].append(inst)
    return out


def set_participants(cur, t):
    cur.execute("SET max_parallel_workers_per_gather = %s", (max(0, t - 1),))


def run(cur, sql):
    cur.execute(sql)
    return cur.fetchall()


def norm(v):
    if isinstance(v, (datetime.date, datetime.datetime)):
        return v.isoformat()
    return v


def cell_equal(a, b):
    if a is None or b is None:
        return a is None and b is None
    if isinstance(a, float) or isinstance(b, float):
        fa, fb = float(a), float(b)
        return abs(fa - fb) <= 1e-9 * max(1.0, abs(fa), abs(fb))
    if isinstance(a, decimal.Decimal) or isinstance(b, decimal.Decimal):
        da, db = decimal.Decimal(a), decimal.Decimal(b)
        if da == db:
            return True
        scale = max(-da.as_tuple().exponent, -db.as_tuple().exponent)
        return scale > 6 and abs(da - db) <= decimal.Decimal("1e-12") * max(1, abs(da), abs(db))
    return norm(a) == norm(b)


def diff(expected, actual):
    if len(expected) != len(actual):
        return "row count %d vs %d" % (len(expected), len(actual))
    for r, (er, ar) in enumerate(zip(expected, actual)):
        if len(er) != len(ar):
            return "row %d column count %d vs %d" % (r, len(er), len(ar))
        for c, (ev, av) in enumerate(zip(er, ar)):
            if not cell_equal(ev, av):
                return "row %d col %d: %r vs %r" % (r, c, ev, av)
    return ""


class RefCache:
    def __init__(self, path, cur, db):
        self.path = path
        os.makedirs(path, exist_ok=True)
        cur.execute("SELECT string_agg(relname || ':' || n_live_tup, ',' ORDER BY relname) FROM pg_stat_user_tables "
                    "WHERE relname IN ('lineitem','orders','customer','part','partsupp','supplier','nation','region')")
        self.tag = db + "|" + (cur.fetchone()[0] or "")

    def _file(self, sql):
        return os.path.join(self.path, hashlib.sha1((self.tag + "\n" + sql).encode()).hexdigest() + ".json")

    def get(self, sql):
        f = self._file(sql)
        if not os.path.exists(f):
            return None
        with open(f) as fh:
            rows = json.load(fh)
        return [tuple(decode(v) for v in row) for row in rows]

    def put(self, sql, rows):
        with open(self._file(sql) + ".tmp", "w") as fh:
            json.dump([[encode(v) for v in row] for row in rows], fh)
        os.replace(self._file(sql) + ".tmp", self._file(sql))


def encode(v):
    if isinstance(v, decimal.Decimal):
        return {"d": str(v)}
    if isinstance(v, (datetime.date, datetime.datetime)):
        return {"t": v.isoformat()}
    return v


def decode(v):
    if isinstance(v, dict):
        if "d" in v:
            return decimal.Decimal(v["d"])
        return v["t"]
    return v


def accel_sql(schema, q, parallel, args):
    return "SELECT * FROM %s.accel_tpch_q%d%s%s" % (schema, q, "_parallel" if parallel else "", args)


def reference(cur, cache, sql, ref_threads):
    rows = cache.get(sql)
    if rows is None:
        set_participants(cur, ref_threads)
        rows = run(cur, sql)
        cache.put(sql, rows)
    return rows


def check(args, conn, instances):
    cur = conn.cursor()
    cache = RefCache(args.ref_cache, cur, args.db)
    failures = 0
    for q, insts in sorted(instances.items()):
        for inst in insts:
            label = "Q%d %s" % (q, inst["label"])
            try:
                ref = reference(cur, cache, inst["ref_sql"], args.ref_threads)
                set_participants(cur, 1)
                d = diff(ref, run(cur, accel_sql(args.schema, q, False, inst["args"])))
                if d:
                    raise AssertionError("st-vs-ref: " + d)
                for t in args.threads_list:
                    set_participants(cur, t)
                    p1 = run(cur, accel_sql(args.schema, q, True, inst["args"]))
                    d = diff(ref, p1)
                    if d:
                        raise AssertionError("par@%d-vs-ref: %s" % (t, d))
                    p2 = run(cur, accel_sql(args.schema, q, True, inst["args"]))
                    if [tuple(map(norm, r)) for r in p1] != [tuple(map(norm, r)) for r in p2]:
                        raise AssertionError("par@%d nondeterministic" % t)
                print("%s ok" % label, flush=True)
            except Exception as e:  # noqa: BLE001
                conn.rollback()
                failures += 1
                print("%s FAIL %s" % (label, str(e).strip().replace("\n", " ")[:400]), flush=True)
    print("# failures=%d" % failures)
    return 1 if failures else 0


def sweep(args, conn, instances):
    cur = conn.cursor()
    out = open(args.out, "w") if args.out else sys.stdout
    out.write("engine,query,seed,variant,threads,ms\n")
    results = {}

    def timed(sql):
        start = time.perf_counter()
        run(cur, sql)
        return (time.perf_counter() - start) * 1e3

    for q, insts in sorted(instances.items()):
        first = insts[0]
        # Untimed warm-up of every variant on the first parametrization.
        set_participants(cur, 1)
        run(cur, first["ref_sql"])
        run(cur, accel_sql(args.schema, q, False, first["args"]))
        for t in args.threads_list:
            set_participants(cur, t)
            run(cur, first["ref_sql"])
            run(cur, accel_sql(args.schema, q, True, first["args"]))
        for inst in insts:
            rows = []
            set_participants(cur, 1)
            rows.append(("postgres", 1, timed(inst["ref_sql"])))
            rows.append(("accel_st", 1, timed(accel_sql(args.schema, q, False, inst["args"]))))
            for t in args.threads_list:
                set_participants(cur, t)
                rows.append(("postgres", t, timed(inst["ref_sql"])))
                rows.append(("accel_par", t, timed(accel_sql(args.schema, q, True, inst["args"]))))
            for variant, t, ms in rows:
                results.setdefault((q, variant, t), []).append(ms)
                out.write("postgres,Q%d,%s,%s,%d,%.3f\n" % (q, inst["label"], variant, t, ms))
            out.flush()
    med = lambda k: statistics.median(results[k])  # noqa: E731
    hdr = "# medians over parametrizations (ms): query,postgres@1,accel_st@1,st_speedup"
    for t in args.threads_list:
        hdr += ",postgres@%d,accel_par@%d,par_speedup@%d" % (t, t, t)
    out.write(hdr + "\n")
    for q in sorted(instances):
        p1, s1 = med((q, "postgres", 1)), med((q, "accel_st", 1))
        line = "#Q%d,%.3f,%.3f,%.3f" % (q, p1, s1, p1 / s1)
        for t in args.threads_list:
            pt, at = med((q, "postgres", t)), med((q, "accel_par", t))
            line += ",%.3f,%.3f,%.3f" % (pt, at, pt / at)
        out.write(line + "\n")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["check", "sweep"], default="check")
    ap.add_argument("--queries", default=",".join(str(q) for q in range(1, 23)))
    ap.add_argument("--seeds", type=int, default=20, help="parametrizations per query")
    ap.add_argument("--threads-list", default="4", help="participant counts for the parallel variants")
    ap.add_argument("--schema", default="public")
    ap.add_argument("--db", default=os.environ.get("TPCH_PG_DB", "accel_tpch_test"))
    ap.add_argument("--instances", default=os.path.join(ROOT, "benchmarks/tpch/stateless_instances_sf1.jsonl"))
    ap.add_argument("--ref-cache", default=os.path.expanduser("~/.cache/accel_pg_refcache"))
    ap.add_argument("--ref-threads", type=int, default=8, help="participants used to compute uncached references")
    ap.add_argument("--statement-timeout", default="30min")
    ap.add_argument("--out", default="")
    args = ap.parse_args()
    args.threads_list = parse_list(args.threads_list)
    instances = load_instances(args.instances, set(parse_list(args.queries)), args.seeds)
    conn = psycopg2.connect(dbname=args.db, host=os.environ.get("PGHOST", "127.0.0.1"),
                            port=os.environ.get("PGPORT", "5434"), user=os.environ.get("PGUSER", "postgres"),
                            password=os.environ.get("PGPASSWORD", "postgres"))
    conn.autocommit = True
    with conn.cursor() as cur:
        cur.execute("SET statement_timeout = %s", (args.statement_timeout,))
        cur.execute("SET search_path = public")
    return check(args, conn, instances) if args.mode == "check" else sweep(args, conn, instances)


if __name__ == "__main__":
    sys.exit(main())
