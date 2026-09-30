#!/usr/bin/env python3
"""Stateless TPC-H Q1 on PostgreSQL and MySQL: base SQL vs the Verus-verified
kernel (accel_tpch_q1_verus / mysql_accel_tpch_q1_verus).

For each Q1 manifest parameter (day offsets 103, 105, 90, 98, 104):
  1. the accelerator's rows are compared with the SQL's rows (exact text);
  2. each system is timed: `--warmup` runs, then the median / mean / min of
     `--iters` runs.
PostgreSQL: SQL with the server's parallel workers (default) and with
max_parallel_workers_per_gather = 0 (single core, like the kernel), via
psycopg2 (client-observed, includes fetching the rows).
MySQL: one `mysql` CLI process per run inside the container (client-observed;
CLI start-up is ~10 ms against ~10 s queries).  MySQL runs Q1 on one thread.
Every accelerator call is also checked to have been answered by the verified
kernel, not by the SQL fallback (PostgreSQL: the entry point's DEBUG1 notice;
MySQL: the fallback line in the server log); `kernel_path` reports it.
Output: CSV on stdout.
"""
import argparse
import statistics
import subprocess
import time

OFFSETS = [103, 105, 90, 98, 104]

PG_SQL = """SELECT l_returnflag, l_linestatus, sum(l_quantity), sum(l_extendedprice),
sum(l_extendedprice * (1 - l_discount)), sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)),
avg(l_quantity), avg(l_extendedprice), avg(l_discount), count(*)
FROM lineitem WHERE l_shipdate <= DATE '1998-12-01' - %s::integer
GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus"""

MY_SQL = """SELECT l_returnflag, l_linestatus, sum(l_quantity), sum(l_extendedprice),
sum(l_extendedprice * (1 - l_discount)), sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)),
avg(l_quantity), avg(l_extendedprice), avg(l_discount), count(*)
FROM lineitem WHERE l_shipdate <= DATE_SUB(DATE '1998-12-01', INTERVAL {off} DAY)
GROUP BY l_returnflag, l_linestatus ORDER BY l_returnflag, l_linestatus"""


def stats(xs):
    return statistics.median(xs), statistics.mean(xs), min(xs)


def emit(engine, off, system, s, match, path="n/a"):
    print(f"{engine},{off},{system},{s[0]:.1f},{s[1]:.1f},{s[2]:.1f},{match},{path}", flush=True)


def run_pg(args):
    import psycopg2
    con = psycopg2.connect(args.pg_dsn)
    con.autocommit = True
    cur = con.cursor()
    cur.execute("SET client_min_messages = debug1")

    def q(sql, params):
        t = time.perf_counter()
        cur.execute(sql, params)
        rows = cur.fetchall()
        return (time.perf_counter() - t) * 1e3, rows

    def timed(sql, params):
        for _ in range(args.warmup):
            q(sql, params)
        return stats([q(sql, params)[0] for _ in range(args.iters)])

    verus = "SELECT * FROM accel_tpch_q1_verus(%s)"

    def kernel_calls():
        n = sum("answered by the verified kernel" in m for m in con.notices)
        f = sum("answering with SQL" in m for m in con.notices)
        con.notices.clear()
        return n, f

    for off in OFFSETS:
        cur.execute("RESET max_parallel_workers_per_gather")
        ref = q(PG_SQL, (off,))[1]
        got = q(verus, (off,))[1]
        match = [tuple(str(x) for x in r) for r in ref] == [tuple(str(x) for x in r) for r in got]
        emit("postgres", off, "sql_parallel_default", timed(PG_SQL, (off,)), "ref")
        cur.execute("SET max_parallel_workers_per_gather = 0")
        emit("postgres", off, "sql_serial", timed(PG_SQL, (off,)), "ref")
        kernel_calls()
        st = timed(verus, (off,))
        n, f = kernel_calls()
        emit("postgres", off, "accel_verus", st, str(match).lower(), f"kernel={n};fallback={f}")
    con.close()


def run_mysql(args):
    cli = ["docker", "exec", args.my_container, "mysql", "-uroot", "-pmysql", "-D", args.my_db,
           "--batch", "--raw", "--skip-column-names", "-e"]

    def q(sql):
        t = time.perf_counter()
        out = subprocess.run(cli + [sql], capture_output=True, text=True, check=True).stdout
        return (time.perf_counter() - t) * 1e3, out

    def timed(sql):
        for _ in range(args.warmup):
            q(sql)
        return stats([q(sql)[0] for _ in range(args.iters)])

    def fallbacks():
        log = subprocess.run(["docker", "logs", args.my_container], capture_output=True, text=True)
        return (log.stdout + log.stderr).count("mysql_accel_tpch_q1_verus(")

    for off in OFFSETS:
        sql = MY_SQL.format(off=off).replace("\n", " ")
        verus = f"SELECT mysql_accel_tpch_q1_verus({off})"
        ref = q(sql)[1].replace("\t", "|")
        got = q(verus)[1]
        match = ref.strip() == got.strip()
        emit("mysql", off, "sql", timed(sql), "ref")
        f0 = fallbacks()
        st = timed(verus)
        f = fallbacks() - f0
        emit("mysql", off, "accel_verus", st, str(match).lower(),
             f"kernel={args.warmup + args.iters - f};fallback={f}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engines", default="postgres,mysql")
    ap.add_argument("--warmup", type=int, default=1)
    ap.add_argument("--iters", type=int, default=5)
    ap.add_argument("--pg-dsn", default="host=127.0.0.1 port=5434 user=postgres password=postgres dbname=accel_q21_bench")
    ap.add_argument("--my-container", default="accel_mysql_sf1")
    ap.add_argument("--my-db", default="accel_mysql_test")
    args = ap.parse_args()
    print("engine,day_offset,system,median_ms,mean_ms,min_ms,matches_sql,kernel_path", flush=True)
    engines = args.engines.split(",")
    if "postgres" in engines:
        run_pg(args)
    if "mysql" in engines:
        run_mysql(args)


if __name__ == "__main__":
    main()
