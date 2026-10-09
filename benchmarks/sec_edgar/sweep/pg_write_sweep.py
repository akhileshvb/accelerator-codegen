#!/usr/bin/env python3
"""Write-frequency sweep for one SEC-EDGAR query on PostgreSQL.

Mirrors benchmark_shared_duckdb_write_interval's virtual-time model so the two
backends are comparable: a reader issues READS queries at READER_QPS virtual
reads/s; the write rate is counted in rows touched per second (1/interval).
Before read i, every row-write due by virtual time i/qps that is not yet
applied -- floor(i * rows_per_sec / qps) in all -- is applied as ONE set-based
UPDATE of that many distinct rows in ONE transaction, timed as one write
sample. Workload time = sum of read latencies + sum of write latencies.

Modes, each in a fresh backend, single-threaded (max_parallel_workers_per_gather=0):
  base  - plain query SQL, capture triggers disabled (session_replication_role=replica)
  aux   - SELECT * FROM accel_sec_edgar_q<N>_aux(); first call (state build) timed
          separately, then one warm-up, then the timed loop.

Writes: a pool of distinct rows of the table/column each query aggregates
(POOLS): the query's invalidating candidates first (the rows feeding its
result, the same selection as the DuckDB catalog's probe_select_sql), then
filler rows of the same table/column passing the query's predicate on it.
Each row has a fixed delta (num.value: max(1, |value| * 0.01); pre.line:
1000). A batch is the next n pool slots (each row at most once per mode when
the pool covers the run) applied with

  UPDATE <table> t SET <col> = t.<col> + p.delta
  FROM pg_temp.accel_batch_pool p WHERE t.ctid = p.tid AND <slots>
  RETURNING p.k, t.ctid

in one transaction (a Tid Scan per row; the returned new ctids are written to
the session's temp pool after the timer, for later batches and the restore).
The capture triggers (AFTER UPDATE FOR EACH STATEMENT, transition tables) see
every row of the batch. Base and aux apply
the same batches; every touched value is restored after each mode (triggers
off), so the data is unchanged between modes.

Parameters: --params default|random (env ACCEL_SWEEP_PARAMS), --seed (env
ACCEL_SWEEP_PARAM_SEED). In random mode read i draws the i-th parameter set of
the seeded sequence from benchmarks/sec_edgar/params/q<N>.json (identical for
base and aux, and identical to the DuckDB benchmark's draws); base runs the
substituted template, aux runs accel_sec_edgar_q<N>_aux(args). At every write
rate (--check-at all, the default; "lightest" for the lightest rate only) the
aux answer and --check-sets extra drawn sets are compared aux vs base after
the mode's writes.

usage: pg_write_sweep.py --port P --query N --intervals 0,0.001,... --out CSV
(intervals: 1 / rows touched per second)
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

NUM_DELTA = "GREATEST(1.0, abs(n.value) * 0.01)"
NUM_COLS = f"n.ctid, {NUM_DELTA}"

# query -> (table, column, invalidating selector, filler selector); each
# selector returns (ctid, delta) rows. The invalidating selectors are the
# DuckDB catalog's probe_select_sql (rows feeding the query's result) without
# its LIMIT 1; the fillers are rows passing the query's predicate on the table
# (src/generated/sec_edgar/duckdb_probes.hpp, BatchFillerSelectSql).
POOLS = {
    1: ("pre", "line",
        "SELECT ctid, 1000 FROM pre WHERE stmt IS NOT NULL AND line IS NOT NULL",
        None),
    2: ("num", "value",
        f"SELECT {NUM_COLS} FROM num n JOIN sub s ON n.adsh = s.adsh "
        "JOIN (SELECT adsh, tag, MAX(value) AS max_value FROM num "
        "      WHERE uom = 'pure' AND value IS NOT NULL GROUP BY adsh, tag) m "
        "ON n.adsh = m.adsh AND n.tag = m.tag AND n.value = m.max_value "
        "WHERE n.uom = 'pure' AND s.fy = 2022 AND n.value IS NOT NULL "
        "ORDER BY n.value DESC, s.name, n.tag",
        f"SELECT {NUM_COLS} FROM num n WHERE n.uom = 'pure' AND n.value IS NOT NULL"),
    3: ("num", "value",
        f"SELECT {NUM_COLS} FROM num n JOIN sub s ON n.adsh = s.adsh "
        "JOIN (SELECT s2.cik FROM num n2 JOIN sub s2 ON n2.adsh = s2.adsh "
        "      WHERE n2.uom = 'USD' AND s2.fy = 2022 AND n2.value IS NOT NULL "
        "      GROUP BY s2.name, s2.cik "
        "      HAVING SUM(n2.value) > (SELECT AVG(sub_total) FROM ("
        "          SELECT SUM(n3.value) AS sub_total FROM num n3 "
        "          JOIN sub s3 ON n3.adsh = s3.adsh "
        "          WHERE n3.uom = 'USD' AND s3.fy = 2022 AND n3.value IS NOT NULL "
        "          GROUP BY s3.cik) avg_sub) "
        "      ORDER BY SUM(n2.value) DESC LIMIT 100) top ON s.cik = top.cik "
        "WHERE n.uom = 'USD' AND s.fy = 2022 AND n.value IS NOT NULL ORDER BY n.adsh",
        f"SELECT {NUM_COLS} FROM num n WHERE n.uom = 'USD' AND n.value IS NOT NULL"),
    4: ("num", "value",
        f"SELECT {NUM_COLS} FROM num n JOIN sub s ON n.adsh = s.adsh "
        "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
        "JOIN tag t ON n.tag = t.tag AND n.version = t.version "
        "JOIN (SELECT s2.sic, t2.tlabel, p2.stmt FROM num n2 "
        "      JOIN sub s2 ON n2.adsh = s2.adsh "
        "      JOIN tag t2 ON n2.tag = t2.tag AND n2.version = t2.version "
        "      JOIN pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND n2.version = p2.version "
        "      WHERE n2.uom = 'USD' AND p2.stmt = 'EQ' AND s2.sic BETWEEN 4000 AND 4999 "
        "      AND n2.value IS NOT NULL AND t2.abstract = 0 "
        "      GROUP BY s2.sic, t2.tlabel, p2.stmt HAVING COUNT(DISTINCT s2.cik) >= 2 "
        "      ORDER BY SUM(n2.value) DESC LIMIT 500) top "
        "ON s.sic = top.sic AND t.tlabel = top.tlabel AND p.stmt = top.stmt "
        "WHERE n.uom = 'USD' AND p.stmt = 'EQ' AND s.sic BETWEEN 4000 AND 4999 "
        "AND n.value IS NOT NULL AND t.abstract = 0 ORDER BY n.adsh, n.tag, n.version",
        f"SELECT {NUM_COLS} FROM num n WHERE n.uom = 'USD' AND n.value IS NOT NULL"),
    6: ("num", "value",
        f"SELECT {NUM_COLS} FROM num n JOIN sub s ON n.adsh = s.adsh "
        "JOIN pre p ON n.adsh = p.adsh AND n.tag = p.tag AND n.version = p.version "
        "JOIN (SELECT s2.name, p2.stmt, n2.tag, p2.plabel FROM num n2 "
        "      JOIN sub s2 ON n2.adsh = s2.adsh "
        "      JOIN pre p2 ON n2.adsh = p2.adsh AND n2.tag = p2.tag AND n2.version = p2.version "
        "      WHERE n2.uom = 'USD' AND p2.stmt = 'IS' AND s2.fy = 2023 AND n2.value IS NOT NULL "
        "      GROUP BY s2.name, p2.stmt, n2.tag, p2.plabel "
        "      ORDER BY SUM(n2.value) DESC, s2.name, n2.tag, p2.plabel LIMIT 200) top "
        "ON s.name = top.name AND p.stmt = top.stmt AND n.tag = top.tag "
        "AND p.plabel IS NOT DISTINCT FROM top.plabel "
        "WHERE n.uom = 'USD' AND p.stmt = 'IS' AND s.fy = 2023 AND n.value IS NOT NULL "
        "ORDER BY n.adsh, n.tag, n.version",
        f"SELECT {NUM_COLS} FROM num n WHERE n.uom = 'USD' AND n.value IS NOT NULL"),
    24: ("num", "value",
         f"SELECT {NUM_COLS} FROM num n LEFT JOIN pre p ON n.tag = p.tag "
         "AND n.version = p.version AND n.adsh = p.adsh "
         "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
         "AND n.value IS NOT NULL AND p.adsh IS NULL AND (n.tag, n.version) IN ("
         "  SELECT n2.tag, n2.version FROM num n2 LEFT JOIN pre p2 ON n2.tag = p2.tag "
         "  AND n2.version = p2.version AND n2.adsh = p2.adsh "
         "  WHERE n2.uom = 'USD' AND n2.ddate BETWEEN 20230101 AND 20231231 "
         "  AND n2.value IS NOT NULL AND p2.adsh IS NULL GROUP BY n2.tag, n2.version "
         "  HAVING COUNT(*) > 10 ORDER BY COUNT(*) DESC LIMIT 100) "
         "ORDER BY n.value DESC",
         f"SELECT {NUM_COLS} FROM num n WHERE n.uom = 'USD' "
         "AND n.ddate BETWEEN 20230101 AND 20231231 AND n.value IS NOT NULL"),
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


def schedule(rows_per_sec, reads, qps):
    """Rows due before each read: floor(i * rows_per_sec / qps) in all by read i."""
    out, applied = [], 0
    for i in range(reads):
        due = int(math.floor(i * rows_per_sec / qps + 1e-9)) if rows_per_sec else 0
        out.append(due - applied)
        applied = due
    return out


class BatchPool:
    """Distinct (ctid, delta) rows; ctids are kept current across updates."""

    def __init__(self, cur, query, want, min_rows):
        self.table, self.column, inval_sql, filler_sql = POOLS[query]
        self.tids, self.deltas, seen = [], [], set()
        self.cursor = 0

        def take(sql, limit):
            # over-fetch: joins (num x pre) repeat rows
            cur.execute(f"{sql} LIMIT {int(limit)}")
            for tid, delta in cur.fetchall():
                if len(self.tids) >= want:
                    break
                if tid in seen:
                    continue
                seen.add(tid)
                self.tids.append(tid)
                self.deltas.append(float(delta))

        take(inval_sql, 2 * want + 16)
        self.invalidating = len(self.tids)
        if len(self.tids) < want and filler_sql:
            take(filler_sql, want + len(self.tids) + 16)
        if len(self.tids) < min_rows:
            raise SystemExit(f"q{query}: only {len(self.tids)} distinct rows for a batch of {min_rows}")
        print(f"q{query} batch pool: table={self.table} column={self.column} rows={len(self.tids)} "
              f"invalidating_candidates={self.invalidating} filler={len(self.tids) - self.invalidating}",
              flush=True)

    def __len__(self):
        return len(self.tids)

    def stage(self, cur):
        """The pool in this session's temp table (setup, not timed)."""
        cur.execute("DROP TABLE IF EXISTS pg_temp.accel_batch_pool")
        cur.execute(
            f"CREATE TEMP TABLE accel_batch_pool AS "
            f"SELECT u.k, u.tid, u.delta, t.{self.column} AS orig "
            f"FROM unnest(%s::int[], %s::tid[], %s::float8[]) AS u(k, tid, delta) "
            f"JOIN {self.table} t ON t.ctid = u.tid",
            (list(range(len(self.tids))), self.tids, self.deltas))
        if cur.rowcount != len(self.tids):
            raise SystemExit(f"pool staging matched {cur.rowcount} of {len(self.tids)} rows")
        cur.execute("ALTER TABLE pg_temp.accel_batch_pool ADD PRIMARY KEY (k)")
        cur.execute("ANALYZE pg_temp.accel_batch_pool")
        self.cursor = 0

    def batch_sql(self, n):
        lo, size = self.cursor, len(self.tids)
        rng = lambda a, b: f"(p.k >= {a} AND p.k < {b})"
        pred = rng(lo, lo + n) if lo + n <= size else f"({rng(lo, size)} OR {rng(0, lo + n - size)})"
        return (f"UPDATE {self.table} t SET {self.column} = t.{self.column} + p.delta "
                f"FROM pg_temp.accel_batch_pool p WHERE t.ctid = p.tid AND {pred} "
                f"RETURNING p.k, t.ctid")

    def write(self, conn, cur, n):
        """One transaction updating the next n pool rows; returns its latency (us).
        The rows' new ctids (RETURNING) go to the temp pool after the timer."""
        sql = self.batch_sql(n)
        conn.autocommit = False
        start = time.perf_counter()
        cur.execute(sql)
        moved = cur.fetchall()
        conn.commit()
        elapsed = (time.perf_counter() - start) * 1e6
        conn.autocommit = True
        if len(moved) != n:
            raise SystemExit(f"batch touched {len(moved)} rows, expected {n}")
        for k, tid in moved:
            self.tids[k] = tid
        cur.execute("UPDATE pg_temp.accel_batch_pool p SET tid = m.tid "
                    "FROM unnest(%s::int[], %s::tid[]) AS m(k, tid) WHERE p.k = m.k",
                    ([k for k, _ in moved], [t for _, t in moved]))
        self.cursor = (self.cursor + n) % len(self.tids)
        return elapsed

    def restore(self, cur):
        """Every pool row back to its value before the mode (triggers off); the
        rows' new ctids become the pool's for the next mode."""
        cur.execute("SET session_replication_role = replica")
        cur.execute(f"UPDATE {self.table} t SET {self.column} = p.orig "
                    f"FROM pg_temp.accel_batch_pool p WHERE t.ctid = p.tid RETURNING p.k, t.ctid")
        moved = cur.fetchall()
        if len(moved) != len(self.tids):
            raise SystemExit(f"restore matched {len(moved)} of {len(self.tids)} rows")
        for k, tid in moved:
            self.tids[k] = tid
        cur.execute("SET session_replication_role = DEFAULT")

    def first_batch_changes(self, cur, query_sql, n):
        """Base result before vs after one n-row batch (rolled back)."""
        cur.execute("SET session_replication_role = replica")
        cur.execute(query_sql)
        before = canon(cur.fetchall())
        self.stage(cur)
        conn = cur.connection
        conn.autocommit = False
        cur.execute(self.batch_sql(n))
        touched = len(cur.fetchall())
        cur.execute(query_sql)
        after = canon(cur.fetchall())
        conn.rollback()
        conn.autocommit = True
        cur.execute("DROP TABLE IF EXISTS pg_temp.accel_batch_pool")
        self.cursor = 0
        return touched == n and before != after


def canon(rows):
    out = []
    for r in rows:
        out.append(tuple(
            ("%.9g" % float(v)) if isinstance(v, float) or type(v).__name__ == "Decimal" else v
            for v in r))
    return sorted(out, key=repr)


def run_mode(port, query, batches, base_mode, pool, reads, qps, check,
             read_draws=None, check_draws=None):
    conn, cur = connect(port, base_mode)
    base_sql = open(os.path.join(REPO, f"benchmarks/sec_edgar/queries/q{query}.sql")).read()
    read_sql = base_sql if base_mode else f"SELECT * FROM accel_sec_edgar_q{query}_aux()"
    build_us = 0.0
    if not base_mode:
        build_us, _ = timed(cur, read_sql)  # first call builds the backend-local state
    timed(cur, read_sql)  # warm-up
    do_writes = any(n > 0 for n in batches)
    if do_writes:
        pool.stage(cur)
    read_t, write_t = [], []
    rows, touched = [], 0
    for i in range(reads):
        n = batches[i] if i < len(batches) else 0
        if n > 0:
            write_t.append(pool.write(conn, cur, n))
            touched += n
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
    if do_writes:
        pool.restore(cur)
    conn.close()
    return {
        "reads": len(read_t), "writes": len(write_t), "rows": len(rows),
        "rows_touched": touched,
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
    ap.add_argument("--check-at", choices=["all", "lightest"],
                    default=os.environ.get("ACCEL_SWEEP_CHECK_AT", "all"))
    a = ap.parse_args()
    intervals = [float(x) for x in a.intervals.split(",")]
    # interval = 1 / (rows touched per second); 0 = read-only
    rates = {iv: (int(round(1 / iv)) if iv > 0 else 0) for iv in intervals}
    plans = {iv: schedule(rates[iv], a.reads, a.qps) for iv in intervals}
    total = max(sum(p) for p in plans.values())
    max_batch = max(max(p) for p in plans.values())
    pool = None
    if total > 0:
        conn, cur = connect(a.port, True)
        pool = BatchPool(cur, a.query, total, max_batch)
        base_sql = open(os.path.join(REPO, f"benchmarks/sec_edgar/queries/q{a.query}.sql")).read()
        if not pool.first_batch_changes(cur, base_sql, max_batch):
            raise SystemExit(f"q{a.query}: a {max_batch}-row batch does not change the base result")
        print(f"q{a.query} batch pool: first {max_batch}-row batch changes the base result", flush=True)
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
              "workload_with_build_us", "workload_with_build_speedup_vs_base", "correct", "params",
              "rows_per_sec", "rows_touched", "batches", "rows_per_batch", "write_sum_per_row_us",
              "invalidating_rows"]
    new = not os.path.exists(a.out)
    with open(a.out, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        if new:
            w.writeheader()
        for iv in intervals:
            check = a.check_at == "all" or iv == max(intervals)
            plan = plans[iv]
            # pool slots 0.. are written in order: these many row-writes hit
            # invalidating candidates
            inval = sum(1 for k in range(sum(plan)) if pool and k % len(pool) < pool.invalidating)
            res = {}
            for mode in ("base", "aux"):
                t0 = time.time()
                res[mode] = run_mode(a.port, a.query, plan, mode == "base", pool,
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
                    "rows_per_sec": rates[iv], "rows_touched": r["rows_touched"],
                    "batches": r["writes"],
                    "rows_per_batch": r["rows_touched"] / r["writes"] if r["writes"] else 0,
                    "write_sum_per_row_us": r["write_sum_us"] / r["rows_touched"] if r["rows_touched"] else 0,
                    "invalidating_rows": inval,
                })
                f.flush()


if __name__ == "__main__":
    main()
