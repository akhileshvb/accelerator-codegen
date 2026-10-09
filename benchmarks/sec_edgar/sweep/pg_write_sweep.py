#!/usr/bin/env python3
"""Write-frequency sweep for one SEC-EDGAR query on PostgreSQL.

Mirrors benchmark_shared_duckdb_write_interval's virtual-time model so the two
backends are comparable: a reader issues READS queries at READER_QPS virtual
reads/s; before each read, every write whose virtual time has come due is
applied (one write per `interval` seconds of virtual time, i.e. 1/interval
writes/s). Workload time = sum of read latencies + sum of write latencies.

Modes, each in a fresh backend, single-threaded (max_parallel_workers_per_gather=0):
  base  - plain query SQL, capture triggers disabled (session_replication_role=replica)
  aux   - SELECT * FROM accel_sec_edgar_q<N>_aux(); first call (state build) timed
          separately, then one warm-up, then the timed loop.

Writes: one-row UPDATE that moves a value the query aggregates, located by ctid
(the analogue of DuckDB's rowid probe) and tracked across updates. The value
only grows during a mode (+delta per write) and is restored afterwards, so the
data is unchanged between modes.

Parameters: --params default|random (env ACCEL_SWEEP_PARAMS), --seed (env
ACCEL_SWEEP_PARAM_SEED). In random mode read i draws the i-th parameter set of
the seeded sequence from benchmarks/sec_edgar/params/q<N>.json (identical for
base and aux, and identical to the DuckDB benchmark's draws); base runs the
substituted template, aux runs accel_sec_edgar_q<N>_aux(args). At the heaviest
write rate, --check-sets extra drawn sets are compared aux vs base.

usage: pg_write_sweep.py --port P --query N --intervals 0,0.001,... --out CSV
"""
import argparse
import csv
import math
import os
import statistics
import sys
import time

import psycopg2

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import sweep_params  # noqa: E402

REPO = os.environ.get("ACCEL_REPO", os.path.abspath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..")))

# (table, column, delta, selector returning one ctid that feeds the query result)
PROBES = {
    1: ("pre", "line", 1000,
        "SELECT ctid FROM pre WHERE stmt IS NOT NULL AND line IS NOT NULL LIMIT 1"),
    2: ("num", "value", 0.5,
        "SELECT n.ctid FROM num n JOIN sub s ON s.adsh = n.adsh "
        "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
        "ORDER BY n.value DESC LIMIT 1"),
    3: ("num", "value", 0.5,
        "SELECT n.ctid FROM num n JOIN sub s ON s.adsh = n.adsh "
        "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL LIMIT 1"),
    4: ("num", "value", 0.5,
        "SELECT n.ctid FROM num n JOIN sub s ON s.adsh = n.adsh "
        "JOIN tag t ON t.tag = n.tag AND t.version = n.version "
        "JOIN pre p ON p.adsh = n.adsh AND p.tag = n.tag AND p.version = n.version "
        "WHERE n.uom = 'USD' AND p.stmt = 'EQ' AND s.sic BETWEEN 4000 AND 4999 "
        "AND n.value IS NOT NULL AND t.abstract = 0 LIMIT 1"),
    6: ("num", "value", 0.5,
        "SELECT n.ctid FROM num n JOIN sub s ON s.adsh = n.adsh "
        "JOIN pre p ON p.adsh = n.adsh AND p.tag = n.tag AND p.version = n.version "
        "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 "
        "AND n.value IS NOT NULL LIMIT 1"),
    24: ("num", "value", 0.5,
         "SELECT n.ctid FROM num n WHERE n.uom = 'USD' "
         "AND n.ddate BETWEEN 20230101 AND 20231231 AND n.value IS NOT NULL "
         "AND NOT EXISTS (SELECT 1 FROM pre p WHERE p.adsh = n.adsh "
         "AND p.tag = n.tag AND p.version = n.version) LIMIT 1"),
}


def connect(port, base_mode):
    conn = psycopg2.connect(host="127.0.0.1", port=port, user="postgres",
                            password="postgres", dbname="sec_edgar")
    conn.autocommit = True
    cur = conn.cursor()
    cur.execute("SET max_parallel_workers_per_gather = 0")
    if base_mode:
        cur.execute("SET session_replication_role = replica")  # no capture triggers
    return conn, cur


def timed(cur, sql, args=None):
    start = time.perf_counter()
    cur.execute(sql, args)
    rows = cur.fetchall()
    return (time.perf_counter() - start) * 1e6, rows


class Probe:
    def __init__(self, cur, query):
        self.table, self.column, self.delta, selector = PROBES[query]
        cur.execute(selector)
        row = cur.fetchone()
        if row is None:
            raise SystemExit(f"q{query}: no probe row found")
        self.ctid = row[0]
        self.pending = 0  # number of +delta applications not yet restored
        self.sql = (f"UPDATE {self.table} SET {self.column} = {self.column} + %s "
                    f"WHERE ctid = %s::tid RETURNING ctid")

    def write(self, cur):
        start = time.perf_counter()
        cur.execute(self.sql, (self.delta, self.ctid))
        self.ctid = cur.fetchone()[0]
        self.pending += 1
        return (time.perf_counter() - start) * 1e6

    def restore(self, cur):
        if self.pending:
            cur.execute(self.sql, (-self.delta * self.pending, self.ctid))
            self.ctid = cur.fetchone()[0]
            self.pending = 0


def canon(rows):
    out = []
    for r in rows:
        out.append(tuple(
            ("%.9g" % float(v)) if isinstance(v, float) or type(v).__name__ == "Decimal" else v
            for v in r))
    return sorted(out, key=repr)


def run_mode(port, query, interval, base_mode, probe, reads, qps, check,
             read_draws=None, check_draws=None):
    conn, cur = connect(port, base_mode)
    base_sql = open(os.path.join(REPO, f"benchmarks/sec_edgar/queries/q{query}.sql")).read()
    read_sql = base_sql if base_mode else f"SELECT * FROM accel_sec_edgar_q{query}_aux()"
    build_us = 0.0
    if not base_mode:
        build_us, _ = timed(cur, read_sql)  # first call builds the backend-local state
    timed(cur, read_sql)  # warm-up
    read_t, write_t = [], []
    next_write = interval if interval > 0 else math.inf
    rows = []
    for i in range(reads):
        vt = i / qps
        while vt + 1e-12 >= next_write:
            write_t.append(probe.write(cur))
            next_write += interval
        sql = read_sql if read_draws is None else read_draws[i % len(read_draws)][base_mode]
        us, rows = timed(cur, sql)
        read_t.append(us)
    ok = None
    if check and not base_mode:
        # Correctness: accel answer vs plain SQL on the same (written) data;
        # in random mode also several drawn parameter sets.
        _, plain = timed(cur, base_sql)
        ok = canon(rows if read_draws is None else timed(cur, read_sql)[1]) == canon(plain)
        for base_q, call_q in (check_draws or []):
            _, a = timed(cur, call_q)
            _, b = timed(cur, base_q)
            ok = ok and canon(a) == canon(b)
            if canon(a) != canon(b):
                print(f"q{query} MISMATCH for {call_q}", flush=True)
    probe.restore(cur)
    conn.close()
    return {
        "reads": len(read_t), "writes": len(write_t), "rows": len(rows),
        "read_p50_us": statistics.median(read_t) if read_t else 0.0,
        "read_sum_us": sum(read_t),
        "write_p50_us": statistics.median(write_t) if write_t else 0.0,
        "write_sum_us": sum(write_t),
        "build_us": build_us, "correct": ok,
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--query", type=int, required=True)
    ap.add_argument("--intervals", required=True)
    ap.add_argument("--reads", type=int, default=int(os.environ.get("SWEEP_READS", "4")))
    ap.add_argument("--qps", type=float, default=float(os.environ.get("SWEEP_QPS", "2")))
    ap.add_argument("--out", required=True)
    ap.add_argument("--params", choices=["default", "random"],
                    default=os.environ.get("ACCEL_SWEEP_PARAMS", "default"))
    ap.add_argument("--seed", type=int, default=int(os.environ.get("ACCEL_SWEEP_PARAM_SEED", "1")))
    ap.add_argument("--check-sets", type=int, default=int(os.environ.get("ACCEL_SWEEP_CHECK_SETS", "4")))
    a = ap.parse_args()
    intervals = [float(x) for x in a.intervals.split(",")]
    conn, cur = connect(a.port, True)
    probe = Probe(cur, a.query)
    conn.close()
    read_draws = check_draws = None
    if a.params == "random":
        spec = sweep_params.load_spec(a.query)
        sets = sweep_params.draw_sets(spec, a.reads + a.check_sets, a.seed) if sweep_params.has_params(spec) \
            else [{}] * (a.reads + a.check_sets)
        pair = lambda v: (sweep_params.render_base(spec, v),
                          sweep_params.render_call(spec, v) if sweep_params.has_params(spec)
                          else f"SELECT * FROM {spec['function']}()")
        pairs = [pair(v) for v in sets]
        # indexed [base_mode]: True -> base SQL, False -> aux call
        read_draws = [{True: b, False: c} for b, c in pairs[:a.reads]]
        check_draws = pairs[a.reads:]
    fields = ["engine", "workload", "query", "write_interval_sec", "writes_per_sec", "mode",
              "reads", "writes", "rows", "read_p50_us", "read_sum_us", "write_p50_us",
              "write_sum_us", "build_us", "workload_sum_us", "workload_speedup_vs_base",
              "workload_with_build_us", "workload_with_build_speedup_vs_base", "correct", "params"]
    new = not os.path.exists(a.out)
    with open(a.out, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        if new:
            w.writeheader()
        for iv in intervals:
            check = iv == max(intervals)  # verify against plain SQL at the heaviest write rate
            res = {}
            for mode in ("base", "aux"):
                t0 = time.time()
                res[mode] = run_mode(a.port, a.query, iv, mode == "base", probe,
                                     a.reads, a.qps, check, read_draws, check_draws)
                print(f"q{a.query} interval={iv} {mode}: {res[mode]} ({time.time()-t0:.0f}s)",
                      flush=True)
            base_wl = res["base"]["read_sum_us"] + res["base"]["write_sum_us"]
            for mode in ("base", "aux"):
                r = res[mode]
                wl = r["read_sum_us"] + r["write_sum_us"]
                wlb = wl + r["build_us"]
                w.writerow({
                    "engine": "postgres", "workload": "sec_edgar", "query": a.query,
                    "write_interval_sec": iv, "writes_per_sec": (1 / iv) if iv > 0 else 0,
                    "mode": "base" if mode == "base" else "shared",
                    **{k: r[k] for k in ("reads", "writes", "rows", "read_p50_us",
                                          "read_sum_us", "write_p50_us", "write_sum_us",
                                          "build_us")},
                    "workload_sum_us": wl,
                    "workload_speedup_vs_base": base_wl / wl if wl else 0,
                    "workload_with_build_us": wlb,
                    "workload_with_build_speedup_vs_base": base_wl / wlb if wlb else 0,
                    "correct": "" if r["correct"] is None else r["correct"],
                    "params": a.params,
                })
                f.flush()


if __name__ == "__main__":
    main()
