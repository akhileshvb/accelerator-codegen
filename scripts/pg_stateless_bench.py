#!/usr/bin/env python3
"""Stateless TPC-H accelerators on PostgreSQL: correctness check and read-only sweep.

  scripts/pg_stateless_bench.py --mode check --queries 1,6 --threads-list 1,4 [--schema public]
  scripts/pg_stateless_bench.py --mode sweep --threads-list 16,32 --seeds 20 --out results.csv

Instances (query parametrizations + reference SQL) come from
eval/bench/tpch/stateless_instances_sf1.jsonl (written by
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
       That is only a cap for PostgreSQL: its planner sizes the worker count from each table's
       size (or its parallel_workers reloption).  --force-workers sets parallel_workers = T-1 on
       the eight TPC-H tables while PostgreSQL runs at T participants and restores the original
       reloptions on exit.  The pg_workers_planned column records the largest "Workers Planned"
       in the PostgreSQL plan (EXPLAIN, untimed) for every PostgreSQL row.
       --pg-best-plan: PostgreSQL's cost model can keep a query serial even when a parallel
       plan is much faster (Q13 at SF20: 27.8 s serial vs 9.5 s with 31 workers).  At each
       T, the warm-up times PostgreSQL on the reference SQL under default costs, then each
       alternative capped at the best time so far: parallel_setup_cost = parallel_tuple_cost = 0
       (T > 1) and, for Q17/Q20, the official TPC-H text with its correlated subquery (used only
       if it returns the accelerator's rows).  Every timed PostgreSQL run of that query at T uses
       the fastest (column pg_plan: ref|official / default|zero).

Connection: libpq environment (PGHOST, PGPORT, PGUSER, PGPASSWORD) and --db.
"""
import argparse
import datetime
import decimal
import hashlib
import json
import math
import os
import signal
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


TPCH_TABLES = ("lineitem", "orders", "customer", "part", "partsupp", "supplier", "nation", "region")


def table_parallel_workers(cur):
    """Current parallel_workers reloption per TPC-H table (None when unset)."""
    cur.execute("SELECT c.relname, (SELECT split_part(o, '=', 2) FROM unnest(c.reloptions) o "
                "WHERE o LIKE 'parallel_workers=%%') FROM pg_class c "
                "WHERE c.relname = ANY(%s) AND c.relnamespace = 'public'::regnamespace", (list(TPCH_TABLES),))
    return {name: (int(v) if v is not None else None) for name, v in cur.fetchall()}


def set_table_workers(cur, workers):
    """workers: {table: n or None}; None resets the reloption."""
    for name, n in workers.items():
        if n is None:
            cur.execute("ALTER TABLE public.%s RESET (parallel_workers)" % name)
        else:
            cur.execute("ALTER TABLE public.%s SET (parallel_workers = %d)" % (name, n))


def set_pg_costs(cur, costs):
    if costs == "zero":
        cur.execute("SET parallel_setup_cost = 0")
        cur.execute("SET parallel_tuple_cost = 0")
    else:
        cur.execute("RESET parallel_setup_cost")
        cur.execute("RESET parallel_tuple_cost")


def workers_planned(cur, sql):
    cur.execute("EXPLAIN (FORMAT JSON) " + sql)
    plan = cur.fetchone()[0]
    best, stack = 0, [plan[0]["Plan"]]
    while stack:
        node = stack.pop()
        best = max(best, node.get("Workers Planned", 0))
        stack.extend(node.get("Plans", []))
    return best


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


def split_sql_args(a):
    """'(DATE '1997-01-01', 'x%', 'RUSSIA')' -> ["DATE '1997-01-01'", "'x%'", "'RUSSIA'"]."""
    a = a.strip()[1:-1]
    parts, cur_part, quoted = [], "", False
    for ch in a:
        if ch == "'":
            quoted = not quoted
        if ch == "," and not quoted:
            parts.append(cur_part.strip())
            cur_part = ""
        else:
            cur_part += ch
    parts.append(cur_part.strip())
    return parts


# The instance file's ref_sql for Q17 and Q20 pre-aggregates lineitem in a derived table, which
# PostgreSQL cannot restrict to the qualifying parts (it aggregates all of lineitem).  The official
# TPC-H text keeps the correlated subquery, which PostgreSQL runs as per-part index lookups.
OFFICIAL_SQL = {
    17: lambda a: ("SELECT SUM(l_extendedprice) / 7.0 AS avg_yearly FROM lineitem, part "
                   "WHERE p_partkey = l_partkey AND p_brand = %s AND p_container = %s "
                   "AND l_quantity < (SELECT 0.2 * AVG(l_quantity) FROM lineitem WHERE l_partkey = p_partkey)"
                   % tuple(split_sql_args(a))),
    20: lambda a: ("SELECT s_name, s_address FROM supplier, nation WHERE s_suppkey IN ("
                   "SELECT ps_suppkey FROM partsupp WHERE ps_partkey IN "
                   "(SELECT p_partkey FROM part WHERE p_name LIKE {1}) "
                   "AND ps_availqty > (SELECT 0.5 * SUM(l_quantity) FROM lineitem "
                   "WHERE l_partkey = ps_partkey AND l_suppkey = ps_suppkey "
                   "AND l_shipdate >= {0} AND l_shipdate < {0} + interval '1' year)) "
                   "AND s_nationkey = n_nationkey AND n_name = {2} ORDER BY s_name"
                   .format(*split_sql_args(a))),
}


def pg_sql(q, inst, kind):
    return inst["ref_sql"] if kind == "ref" else OFFICIAL_SQL[q](inst["args"])


def sweep(args, conn, instances):
    cur = conn.cursor()
    out = open(args.out, "w") if args.out else sys.stdout
    out.write("engine,query,seed,variant,threads,ms,pg_workers_planned,pg_plan\n")
    results = {}
    planned = {}
    original = table_parallel_workers(cur) if args.force_workers else None
    if original is not None:
        print("# --force-workers: original parallel_workers %s (restored on exit)" % original,
              file=sys.stderr, flush=True)

    def participants(t):
        set_participants(cur, t)
        if original is not None and t > 1:
            set_table_workers(cur, {name: t - 1 for name in original})

    try:
        return sweep_body(args, cur, out, instances, results, planned, participants)
    finally:
        if original is not None:
            set_table_workers(cur, original)


def sweep_body(args, cur, out, instances, results, planned, participants):
    # A statement that hits --statement-timeout is recorded as inf ("inf" in the CSV) and that
    # (query, variant, threads[, costs]) is not run again, so one pathological plan cannot stall
    # or abort the sweep.
    timed_out = set()

    def timed(sql, key):
        if key in timed_out:
            return math.inf
        start = time.perf_counter()
        try:
            run(cur, sql)
        except psycopg2.Error as e:
            # The accelerators re-raise PostgreSQL's cancel as their own error, so match the message too.
            if e.pgcode != "57014" and "statement timeout" not in str(e):
                raise
            timed_out.add(key)
            set_timeout(args.statement_timeout)
            print("# timeout Q%d %s (statement_timeout %s); skipping its remaining runs"
                  % (key[0], key[1:], args.statement_timeout), file=sys.stderr, flush=True)
            return math.inf
        return (time.perf_counter() - start) * 1e3

    def set_timeout(value):
        # A statement timeout that fires as a statement ends can be delivered to the next
        # statement instead, so a SET right after a capped run may itself be cancelled; that
        # consumes the stale cancel, and the retry goes through.
        for attempt in range(3):
            try:
                cur.execute("SET statement_timeout = %s", (value,))
                return
            except psycopg2.errors.QueryCanceled:
                if attempt == 2:
                    raise

    def capped(sql, cap_ms):
        """(ms, rows); ms = inf if the statement runs longer than cap_ms (inf: the sweep timeout)."""
        if cap_ms != math.inf:
            set_timeout(max(1, int(cap_ms) + 1))
        start = time.perf_counter()
        try:
            rows = run(cur, sql)
            return (time.perf_counter() - start) * 1e3, rows
        except psycopg2.Error as e:
            if e.pgcode != "57014" and "statement timeout" not in str(e):
                raise
            return math.inf, None
        finally:
            set_timeout(args.statement_timeout)

    # PostgreSQL plan per (query, T): (sql kind, costs).  With --pg-best-plan the warm-up tries
    # the reference SQL under default costs, then each alternative (zero parallel costs at T > 1;
    # the official TPC-H text for Q17/Q20, accepted only if it returns the accelerator's rows),
    # each capped at the best time so far, and keeps the fastest.
    def choose_plan(q, t, inst):
        best, best_ms = ("ref", "default"), timed(inst["ref_sql"], (q, "postgres", t, "ref", "default"))
        if not args.pg_best_plan:
            return best
        if best_ms != math.inf:
            best_ms = timed(inst["ref_sql"], (q, "postgres", t, "ref", "default"))
        kinds = ["ref"] + (["official"] if q in OFFICIAL_SQL else [])
        cfgs = ["default"] + (["zero"] if t > 1 else [])
        expected = run(cur, accel_sql(args.schema, q, t > 1, inst["args"])) if q in OFFICIAL_SQL else None
        for kind in kinds:
            for cfg in cfgs:
                if (kind, cfg) == ("ref", "default"):
                    continue
                set_pg_costs(cur, cfg)
                ms, rows = capped(pg_sql(q, inst, kind), best_ms)
                set_pg_costs(cur, "default")
                if ms == math.inf:
                    continue
                if kind == "official":
                    d = diff(expected, rows)
                    if d:
                        print("# Q%d official SQL disagrees with the accelerator (%s); not used" % (q, d),
                              file=sys.stderr, flush=True)
                        continue
                if ms < best_ms:
                    best, best_ms = (kind, cfg), ms
        print("# Q%d T=%d PostgreSQL plan: %s/%s" % (q, t, best[0], best[1]), file=sys.stderr, flush=True)
        return best

    costs = {}
    for q, insts in sorted(instances.items()):
        first = insts[0]
        # Untimed warm-up of every variant on the first parametrization (plus plan choice).
        participants(1)
        costs[(q, 1)] = choose_plan(q, 1, first)
        timed(accel_sql(args.schema, q, False, first["args"]), (q, "accel_st", 1))
        for t in args.threads_list:
            participants(t)
            costs[(q, t)] = choose_plan(q, t, first)
            timed(accel_sql(args.schema, q, True, first["args"]), (q, "accel_par", t))

        def pg_row(t, inst):
            kind, cfg = costs[(q, t)]
            sql = pg_sql(q, inst, kind)
            set_pg_costs(cur, cfg)
            try:
                return ("postgres", t, timed(sql, (q, "postgres", t, kind, cfg)), workers_planned(cur, sql),
                        kind + "/" + cfg)
            finally:
                set_pg_costs(cur, "default")

        for inst in insts:
            rows = []
            participants(1)
            rows.append(pg_row(1, inst))
            rows.append(("accel_st", 1, timed(accel_sql(args.schema, q, False, inst["args"]), (q, "accel_st", 1)),
                         None, ""))
            for t in args.threads_list:
                participants(t)
                rows.append(pg_row(t, inst))
                rows.append(("accel_par", t, timed(accel_sql(args.schema, q, True, inst["args"]), (q, "accel_par", t)),
                             None, ""))
            for variant, t, ms, wp, cfg in rows:
                results.setdefault((q, variant, t), []).append(ms)
                if wp is not None:
                    planned.setdefault((q, t), []).append(wp)
                out.write("postgres,Q%d,%s,%s,%d,%.3f,%s,%s\n" % (q, inst["label"], variant, t, ms,
                                                                 "" if wp is None else wp, cfg))
            out.flush()
    med = lambda k: statistics.median(results[k])  # noqa: E731
    hdr = "# medians over parametrizations (ms): query,postgres@1,accel_st@1,st_speedup,pg_plan@1"
    for t in args.threads_list:
        hdr += ",postgres@%d,accel_par@%d,par_speedup@%d,pg_workers_planned@%d,pg_plan@%d" % (t, t, t, t, t)
    out.write(hdr + "\n")
    for q in sorted(instances):
        p1, s1 = med((q, "postgres", 1)), med((q, "accel_st", 1))
        line = "#Q%d,%.3f,%.3f,%.3f,%s" % (q, p1, s1, p1 / s1, "/".join(costs[(q, 1)]))
        for t in args.threads_list:
            pt, at = med((q, "postgres", t)), med((q, "accel_par", t))
            wps = sorted(set(planned[(q, t)]))
            line += ",%.3f,%.3f,%.3f,%s,%s" % (pt, at, pt / at, "/".join(map(str, wps)), "/".join(costs[(q, t)]))
        out.write(line + "\n")
    return 0


def main():
    # SIGTERM/SIGHUP (kill, timeout, closed tmux pane) unwind like Ctrl-C, so the sweep's finally
    # block restores the tables' parallel_workers reloptions.
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda signum, frame: sys.exit(128 + signum))
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", choices=["check", "sweep"], default="check")
    ap.add_argument("--queries", default=",".join(str(q) for q in range(1, 23)))
    ap.add_argument("--seeds", type=int, default=20, help="parametrizations per query")
    ap.add_argument("--threads-list", default="4", help="participant counts for the parallel variants")
    ap.add_argument("--schema", default="public")
    ap.add_argument("--db", default=os.environ.get("TPCH_PG_DB", "accel_tpch_test"))
    ap.add_argument("--instances", default=os.path.join(ROOT, "eval/bench/tpch/stateless_instances_sf1.jsonl"))
    ap.add_argument("--ref-cache", default=os.path.expanduser("~/.cache/accel_pg_refcache"))
    ap.add_argument("--ref-threads", type=int, default=8, help="participants used to compute uncached references")
    ap.add_argument("--statement-timeout", default="30min")
    ap.add_argument("--out", default="")
    ap.add_argument("--pg-best-plan", action="store_true",
                    help="sweep: per query and T > 1, time PostgreSQL with default and zero parallel costs, keep the faster")
    ap.add_argument("--force-workers", action="store_true",
                    help="sweep: set parallel_workers = T-1 on the TPC-H tables at T participants")
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
