#!/usr/bin/env python3
"""Aux accelerators vs the base DBMS on TPC-H, for the client/server backends (Postgres, MySQL).

The DuckDB backend has its own harness (run_compare.py + aux_compare.cpp: the accelerator lives in
the DuckDB process).  Postgres and MySQL run the accelerator inside the server (an extension / a UDF
plugin, writes captured by triggers), so this driver talks SQL: the accelerator is
`SELECT * FROM accel_tpch_qN_aux(args)` (Postgres) / `SELECT mysql_accel_tpch_qN_aux(args)` (MySQL),
the baseline is the same TPC-H text with the same parameters as plain SQL.

  read       Single-threaded, read-only.  One connection, one query at a time: per query and TPC-H
             parameter draw, the accelerator's first call (it builds its state), then the median
             latency of the accelerator and of the base DBMS, and whether the answers agree.

  writerate  Concurrent readers + one paced writer, per (write rate, repetition), two systems on the
             same database: base (the DBMS alone: capture triggers OFF, plain SQL writes) and accel
             (triggers on, readers call the accelerator).  Readers are closed-loop over every
             (query, parameter set), each reader on its own connection; the writer commits one
             transaction at a time at a target rate of commits/s (--rate -1: back to back).

             Write stream (the same shape as aux_compare's): 4 of every 5 commits are TPC-H refresh,
             alternating RF2 (delete B orders and their lineitems) and RF1 (insert B new orders with
             their lineitems, cloned from existing ones, every fourth a large-volume order); the
             fifth is a point write rotating over supplier/customer/part/partsupp/lineitem/orders
             updates, part and supplier delete + insert (with partsupp), and nation/region updates,
             so every table a TPC-H query reads changes during a run.  B = --batch-orders (the TPC-H
             refresh functions move 1500 x SF orders each).

             The stream is net-neutral: the writer logs an undo for everything it did and restores
             the data after each run (unmeasured), so every run starts from the same content.
             After the window: the accelerator's answers are compared with the DBMS's (the
             "final check") and with those before the first write ("answer changed", i.e. which
             queries the stream actually exercised).

  --shared-state (Postgres): the accelerators live once, in the extension's per-database background
             worker (accel.shared_state), instead of once per connection.  Each writer commit ships its
             captured rows to the worker, which applies them as a delta before answering the next read;
             per connection, every write by another connection forces a full rebuild in each reader.
             The worker is restarted at the start of every run: base runs write with the capture
             triggers off, which it cannot see.

  summary    median [min-max] over repetitions of a writerate output directory.

Needs: pip install psycopg2-binary pymysql.
  python3 benchmarks/compare/run_sql_backend.py read --backend postgres --db accel_tpch_sf20 --sf 20
  python3 benchmarks/compare/run_sql_backend.py writerate --backend mysql --db tpch_sf20 --sf 20 \\
      --readers 8 --batch-orders 100 --rates "0.2 1 5" --seconds 120
"""
import argparse
import glob
import json
import math
import os
import random
import re
import statistics
import sys
import threading
import time
from datetime import date, datetime
from decimal import Decimal

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
import run_compare  # noqa: E402  (parameter draws: gen_tpch_params.py)

TABLES = ["region", "nation", "customer", "supplier", "part", "partsupp", "orders", "lineitem"]
MYSQL_TRIGGER_SQL = os.path.join(ROOT, "scripts", "mysql_accel_mutation_triggers.sql")
CHUNK = 5000  # rows per multi-row INSERT / keys per IN list


# ---------------------------------------------------------------------------------------------
# SQL texts
# ---------------------------------------------------------------------------------------------
def kit_sql(q):
    """The parameterized ($n) TPC-H text of query q: the one the Verus kits and the DuckDB harness
    use as their baseline (src/generated/tpch/qN/verus_kit/*/glue.cpp, kQuerySql)."""
    f = glob.glob(os.path.join(ROOT, "src", "generated", "tpch", f"q{q}", "verus_kit", "*", "glue.cpp"))[0]
    t = open(f).read()
    pos = t.index("=", t.index("kQuerySql")) + 1
    tok = re.compile(r'\s*(?:R"(\w*)\((.*?)\)\1"|"((?:[^"\\]|\\.)*)")', re.S)
    parts = []
    while True:
        m = tok.match(t, pos)
        if not m:
            break
        parts.append(m.group(2) if m.group(2) is not None else m.group(3).encode().decode("unicode_escape"))
        pos = m.end()
    sql = "".join(parts)
    assert sql.strip(), f"q{q}: no kQuerySql in {f}"
    return "\n".join(l for l in sql.splitlines() if not l.lstrip().startswith("--")).strip().rstrip(";")


def split_args(s):
    """Split an argument list at its top-level commas (quotes and parentheses respected)."""
    out, depth, quote, cur = [], 0, False, ""
    for ch in s:
        if ch == "'":
            quote = not quote
        if not quote:
            depth += ch == "("
            depth -= ch == ")"
            if ch == "," and depth == 0:
                out.append(cur.strip())
                cur = ""
                continue
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


def base_sql(q, verus_args, backend, _cache={}):
    if q not in _cache:
        _cache[q] = kit_sql(q)
    args = split_args(verus_args)
    sql = re.sub(r"\$(\d+)", lambda m: args[int(m.group(1)) - 1], _cache[q])
    if backend == "mysql":  # `n * INTERVAL '1' DAY` is not an expression in MySQL
        sql = re.sub(r"(\d+(?:\.\d+)?)\s*\*\s*INTERVAL\s*'1'\s*DAY", r"INTERVAL \1 DAY", sql, flags=re.I)
    return sql


def accel_sql(q, cpp_args, backend):
    if backend == "postgres":
        return f"SELECT * FROM accel_tpch_q{q}_aux({cpp_args})"
    return f"SELECT mysql_accel_tpch_q{q}_aux({cpp_args})"


# ---------------------------------------------------------------------------------------------
# connections
# ---------------------------------------------------------------------------------------------
class Db:
    """One connection, either backend: execute / fetch with timing, transactions, statement timeout."""

    def __init__(self, a, accel):
        self.backend = a.backend
        self.timeout_s = a.query_timeout
        if a.backend == "postgres":
            import psycopg2
            self.mod = psycopg2
            self.c = psycopg2.connect(host=a.host, port=a.port, user=a.user, password=a.password, dbname=a.db)
            self.c.autocommit = True
            self.cur = self.c.cursor()
            if getattr(a, "pg_workers", None) is not None:
                # the same per-session parallel budget for every connection (base and accel, readers and writer)
                self.cur.execute(f"SET max_parallel_workers_per_gather = {int(a.pg_workers)}")
            if accel:
                self.cur.execute(f"LOAD '{a.pg_so}'")
                if a.shared_state:
                    self.cur.execute("SET accel.shared_state = on")
                    self.cur.execute(f"SET accel.shared_apply_timeout_ms = {int(a.apply_timeout * 1000)}")
                    # wait for the worker (e.g. while it builds another query's state) instead of answering
                    # from SQL after the 60 s default; the statement timeout still bounds the call
                    self.cur.execute(f"SET accel.shared_timeout_ms = {min(3600000, int(a.query_timeout * 1000))}")
        else:
            import pymysql
            self.mod = pymysql
            self.c = pymysql.connect(host=a.host, port=a.port, user=a.user, password=a.password, database=a.db,
                                     autocommit=True, local_infile=False, read_timeout=int(a.query_timeout) + 600)
            self.cur = self.c.cursor()
            self.cur.execute("SET SESSION sql_mode = CONCAT(@@sql_mode, ',PIPES_AS_CONCAT')")
            if accel:  # the plugin scans this database to build / rebuild state (a process-wide setting)
                try:
                    self.cur.execute("SELECT mysql_accel_set_scan_database(%s)", (a.db,))
                    self.cur.fetchall()
                except Exception:  # noqa: BLE001  (a plugin without the UDF reads MYSQL_DATABASE)
                    pass
        self.set_timeout(a.query_timeout)

    def set_timeout(self, s):
        if self.backend == "postgres":
            self.cur.execute(f"SET statement_timeout = {int(s * 1000)}")
        else:
            self.cur.execute(f"SET SESSION max_execution_time = {int(s * 1000)}")

    def is_timeout(self, e):
        if self.backend == "postgres":
            return isinstance(e, self.mod.errors.QueryCanceled)
        return isinstance(e, self.mod.err.OperationalError) and e.args and e.args[0] in (3024, 1317)

    def query(self, sql):
        """-> (rows, microseconds); raises on error (see is_timeout)."""
        t0 = time.perf_counter_ns()
        self.cur.execute(sql)
        rows = self.cur.fetchall()
        return rows, (time.perf_counter_ns() - t0) / 1000

    def begin(self):
        if self.backend == "postgres":
            self.c.autocommit = False
        else:
            self.c.autocommit(False)

    def commit(self):
        self.c.commit()
        if self.backend == "postgres":
            self.c.autocommit = True
        else:
            self.c.autocommit(True)

    def rollback(self):
        try:
            self.c.rollback()
        finally:
            if self.backend == "postgres":
                self.c.autocommit = True
            else:
                self.c.autocommit(True)

    def execute(self, sql, params=None):
        self.cur.execute(sql, params)
        return self.cur.rowcount

    def insert_rows(self, table, rows):
        for i in range(0, len(rows), CHUNK):
            chunk = rows[i:i + CHUNK]
            if self.backend == "postgres":
                from psycopg2.extras import execute_values
                execute_values(self.cur, f"INSERT INTO {table} VALUES %s", chunk, page_size=CHUNK)
            else:
                self.cur.executemany(f"INSERT INTO {table} VALUES ({','.join(['%s'] * len(chunk[0]))})", chunk)

    def delete_in(self, table, col, keys):
        for i in range(0, len(keys), CHUNK):
            ks = ",".join(str(int(k)) for k in keys[i:i + CHUNK])
            self.cur.execute(f"DELETE FROM {table} WHERE {col} IN ({ks})")

    def select_in(self, table, col, keys):
        out = []
        for i in range(0, len(keys), CHUNK):
            ks = ",".join(str(int(k)) for k in keys[i:i + CHUNK])
            self.cur.execute(f"SELECT * FROM {table} WHERE {col} IN ({ks})")
            out += list(self.cur.fetchall())
        return out

    def close(self):
        try:
            self.c.close()
        except Exception:
            pass


# ---------------------------------------------------------------------------------------------
# triggers: the base system must not pay for the accelerator's write capture
# ---------------------------------------------------------------------------------------------
def mysql_trigger_statements():
    text = open(MYSQL_TRIGGER_SQL).read()
    out = []
    for stmt in text.split("//"):
        stmt = "\n".join(l for l in stmt.splitlines()
                         if not l.strip().upper().startswith("DELIMITER") and not l.lstrip().startswith("--")).strip()
        if stmt:
            out.append(stmt)
    return out


def set_triggers(a, on):
    db = Db(a, accel=False)
    try:
        if a.backend == "postgres":
            for t in TABLES:
                db.execute(f"ALTER TABLE {t} {'ENABLE' if on else 'DISABLE'} TRIGGER USER")
        elif on:
            for s in mysql_trigger_statements():
                db.execute(s)
        else:
            db.execute("SELECT trigger_name FROM information_schema.triggers WHERE trigger_schema = DATABASE() "
                       "AND trigger_name LIKE 'accel_codegen_%'")
            for (name,) in db.cur.fetchall():
                db.execute(f"DROP TRIGGER IF EXISTS `{name}`")
        if a.backend == "mysql":
            db.execute("SELECT COUNT(*) FROM information_schema.triggers WHERE trigger_schema = DATABASE() "
                       "AND trigger_name LIKE 'accel_codegen_%'")
            n = db.cur.fetchone()[0]
        else:
            db.execute("SELECT COUNT(*) FROM pg_trigger WHERE NOT tgisinternal AND tgenabled <> 'D'")
            n = db.cur.fetchone()[0]
        return n
    finally:
        db.close()


def restart_shared_worker(a):
    """Stop the shared-state worker (if any): the next accelerator call starts a fresh one, which builds from
    the tables as they are now.  A worker that lived through writes with the capture triggers off would not
    know it is stale."""
    if a.backend != "postgres" or not a.shared_state:
        return
    db = Db(a, accel=True)
    try:
        db.execute("SELECT accel_postgres_stop_shared_worker()")
        for _ in range(100):  # wait until it has exited (it restarts on demand)
            db.execute("SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'accel aux server' AND datname = current_database()")
            if db.cur.fetchone()[0] == 0:
                break
            time.sleep(0.1)
    finally:
        db.close()


def shared_stats(a, db):
    """The shared-state worker's counters (cumulative per database): requests, hits, rebuilds, declined,
    errors, applies (commits shipped to it), synced (applied as a delta) / unsynced (not: the next read
    rebuilds)."""
    if a.backend != "postgres" or not a.shared_state:
        return None
    try:
        rows, _ = db.query("SELECT accel_postgres_shared_stats()")
        return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", str(rows[0][0]))}
    except Exception:  # noqa: BLE001
        return None


# ---------------------------------------------------------------------------------------------
# answers
# ---------------------------------------------------------------------------------------------
def norm(c):
    if c is None:
        return None
    if isinstance(c, (int, float, Decimal)):
        return float(c)
    if isinstance(c, (date, datetime)):
        return c.isoformat()[:10]
    c = str(c).strip()
    if c == "NULL":  # the MySQL accelerator prints NULL as text
        return None
    try:
        return float(c)
    except ValueError:
        return c


def canon(backend, rows, accel):
    """Rows as lists of normalised cells, sorted.  The MySQL accelerator answers with one string,
    rows separated by a newline, columns by '|'."""
    if backend == "mysql" and accel:
        s = rows[0][0] if rows and rows[0] and rows[0][0] is not None else ""
        if isinstance(s, bytes):
            s = s.decode()
        rows = [r.split("|") for r in re.split(r"\\n|\n", s) if r != ""]
    out = [[norm(c) for c in r] for r in rows]
    return sorted(out, key=lambda r: tuple((0, round(c)) if isinstance(c, float) else (1, str(c)) for c in r))


def same(x, y):
    if len(x) != len(y):
        return False
    for r, s in zip(x, y):
        if len(r) != len(s):
            return False
        for c, d in zip(r, s):
            c, d = (0.0 if c is None and isinstance(d, float) else c), (0.0 if d is None and isinstance(c, float) else d)
            # (an aggregate over no rows: SUM is NULL in the DBMS, 0 in the accelerators)
            if isinstance(c, float) and isinstance(d, float):
                if not math.isclose(c, d, rel_tol=1e-6, abs_tol=1e-3):
                    return False
            elif c != d:
                return False
    return True


def pct(xs, p):
    if not xs:
        return 0.0
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p * len(xs)))]


# ---------------------------------------------------------------------------------------------
# the write stream
# ---------------------------------------------------------------------------------------------
SEGMENTS = ["BUILDING", "AUTOMOBILE", "MACHINERY", "HOUSEHOLD", "FURNITURE"]


class Stream:
    """Deterministic, reversible TPC-H writes.  next() -> (kind, tables, prepare) where prepare()
    builds the rows outside the timed commit and returns apply(db) (run inside the transaction)."""

    def __init__(self, db, commits, batch, seed, rf_only=False, dim_after_rf=False, rf_dir=None, set_orders=0):
        self.rng = random.Random(seed)
        self.rf_dir = rf_dir          # TPC-H update sets (dbgen -U): RF1 / RF2 from them instead of cloning orders
        self.set_orders = set_orders  # orders used from each set (0: all)
        self.rf_orders_used = 0
        self._set = (None, None, None, None)
        self.rf_only = rf_only
        self.dim_after_rf = dim_after_rf
        self.batch = batch
        rf_commits = commits * 4 // 5 + 4
        n_order = (rf_commits // 2 + 4) * batch
        dims = (commits if dim_after_rf else commits // 5) + 4
        k = lambda sql: [r[0] for r in db.query(sql)[0]]
        self.low_orders = k(f"SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT {n_order}")
        self.mid_orders = k(f"SELECT o_orderkey FROM orders ORDER BY o_orderkey LIMIT {dims * 2} "
                            f"OFFSET {(db.query('SELECT COUNT(*) FROM orders')[0][0][0]) // 2}")
        self.template_orders = k("SELECT o_orderkey FROM orders ORDER BY o_orderkey DESC LIMIT 1000")
        # large-volume orders (Q18): every fourth RF1 order clones one (a full lineitem scan; cached per process)
        self.large_orders = [] if rf_dir else _cached(
            db, "large_orders", "SELECT l_orderkey FROM lineitem GROUP BY l_orderkey HAVING SUM(l_quantity) > 300 "
                                "ORDER BY l_orderkey DESC LIMIT 200", k)
        self.low_parts = k(f"SELECT p_partkey FROM part ORDER BY p_partkey LIMIT {dims}")
        self.mid_parts = k(f"SELECT p_partkey FROM part ORDER BY p_partkey LIMIT {dims} "
                           f"OFFSET {(db.query('SELECT COUNT(*) FROM part')[0][0][0]) // 2}")
        self.template_parts = k("SELECT p_partkey FROM part ORDER BY p_partkey DESC LIMIT 500")
        self.low_supps = k(f"SELECT s_suppkey FROM supplier ORDER BY s_suppkey LIMIT {dims}")
        self.mid_supps = k(f"SELECT s_suppkey FROM supplier ORDER BY s_suppkey LIMIT {dims} "
                           f"OFFSET {(db.query('SELECT COUNT(*) FROM supplier')[0][0][0]) // 2}")
        self.template_supps = k("SELECT s_suppkey FROM supplier ORDER BY s_suppkey DESC LIMIT 200")
        self.custs = k(f"SELECT c_custkey FROM customer ORDER BY c_custkey LIMIT {dims} "
                       f"OFFSET {(db.query('SELECT COUNT(*) FROM customer')[0][0][0]) // 3}")
        checks = (("orders", self.low_orders, self.mid_orders), ("part", self.low_parts, self.mid_parts),
                  ("supplier", self.low_supps, self.mid_supps))
        for name, lo, mid in (() if rf_only else checks[1:] if dim_after_rf else checks):
            assert max(lo) < min(mid), (f"{name}: the keys the stream deletes ({len(lo)}) overlap the ones it updates; "
                                        f"the table is too small for this many commits (lower the rate / --seconds)")
        self.nations = k("SELECT n_nationkey FROM nation ORDER BY n_nationkey")
        self.regions = k("SELECT r_regionkey FROM region ORDER BY r_regionkey")
        self.next_order = k("SELECT MAX(o_orderkey) FROM orders")[0] + 1
        self.next_part = k("SELECT MAX(p_partkey) FROM part")[0] + 1
        self.next_supp = k("SELECT MAX(s_suppkey) FROM supplier")[0] + 1
        # new keys are max + 1 and the accelerators pack TPC-H keys into 32 bits: a database holding keys of a
        # different magnitude (left by other tests) would make every insert fail inside an accelerator
        assert max(self.next_order, self.next_part, self.next_supp) < 2_000_000_000, \
            "a key above 2e9: the database is not plain TPC-H (polluted by earlier tests?); use a clean one"
        self.at = {}
        self.tmpl = {}  # template key -> cached rows
        self.seq = self.rf = self.rf1 = self.dim = 0
        # undo log
        self.reinsert = []   # (table, rows): rows the stream deleted
        self.redelete = []   # (table, col, keys): rows the stream inserted
        self.reupdate = []   # (table, where clause, {col: old})
        self.written = {t: 0 for t in TABLES}

    def take(self, name, pool):
        i = self.at.get(name, 0)
        assert i < len(pool), f"write stream: out of {name} keys (more commits than planned; raise --max-commits)"
        self.at[name] = i + 1
        return pool[i]

    def cached(self, db, table, col, key):
        if (table, col, key) not in self.tmpl:
            self.tmpl[(table, col, key)] = db.select_in(table, col, [key])
        return self.tmpl[(table, col, key)]

    def next_is_paced(self):
        """A refresh starts a slot (paced by the writer's rate); with dim_after_rf its point writes follow unpaced."""
        return not self.dim_after_rf or self.seq % (1 + len(self.rf_dims())) == 0

    def rf_dims(self):
        """The point writes that follow each refresh (dim_after_rf): the kinds that change the tables RF leaves
        alone (part, partsupp, supplier: all Q2, Q11, Q16 read them).  Nation and region writes are left out: they
        rebuild many accelerators and the tables are static in practice."""
        return [self.supplier_update, self.part_delete, self.part_insert, self.supplier_delete,
                self.supplier_insert, self.partsupp_update]

    def next(self, db):
        i = self.seq
        self.seq += 1
        if self.dim_after_rf:
            pos = i % (1 + len(self.rf_dims()))
            if pos:
                return self.rf_dims()[pos - 1](db)
        elif not (self.rf_only or i % 5 != 4):
            d = self.dim
            self.dim += 1
            return [self.supplier_update, self.customer_update, self.part_delete, self.part_insert, self.supplier_delete,
                    self.supplier_insert, self.partsupp_update, self.lineitem_update, self.orders_update,
                    self.nation_update, self.region_update][d % 11](db)
        if self.rf_dir:  # the specification's order: RF1 of set K, then RF2 of set K
            idx = self.rf
            self.rf += 1
            return self.rf1_set(db, idx // 2 + 1) if idx % 2 == 0 else self.rf2_set(db, idx // 2 + 1)
        if self.rf % 2 == 0:
            self.rf += 1
            return self.rf2(db)
        self.rf += 1
        return self.rf1_(db)

    # --- refresh -------------------------------------------------------------------------
    def rf2(self, db, keys=None):
        keys = keys or [self.take("low_orders", self.low_orders) for _ in range(self.batch)]
        orders, lineitems = db.select_in("orders", "o_orderkey", keys), db.select_in("lineitem", "l_orderkey", keys)

        def apply(d):
            d.delete_in("lineitem", "l_orderkey", keys)
            d.delete_in("orders", "o_orderkey", keys)
            return len(orders) + len(lineitems)
        self.reinsert += [("orders", orders), ("lineitem", lineitems)]
        self.written["orders"] += 1
        self.written["lineitem"] += 1
        return "rf2", apply

    # --- refresh from the TPC-H update sets (official dbgen -U output) --------------------------
    def load_set(self, n):
        """(orders rows, lineitem rows, delete keys) of update set n, as read from the files (cells stay strings:
        the databases cast them to the column types); only the last set is kept in memory."""
        if self._set[0] != n:
            def tbl(name):
                path = os.path.join(self.rf_dir, name)
                assert os.path.exists(path), f"TPC-H update set {n} missing: {path} (scripts/gen_tpch_refresh_sets.sh)"
                with open(path) as f:
                    return [tuple(line.rstrip("\n").split("|")[:-1]) for line in f]  # every line ends with '|'
            orders, lineitems = tbl(f"orders.tbl.u{n}"), tbl(f"lineitem.tbl.u{n}")
            deletes = [int(r[0]) for r in tbl(f"delete.{n}")]
            self._set = (n, orders, lineitems, deletes)
        return self._set[1:]

    def rf1_set(self, db, n):
        orders, lineitems, _ = self.load_set(n)
        if self.set_orders:
            orders = orders[:self.set_orders]
            keep = {r[0] for r in orders}
            lineitems = [r for r in lineitems if r[0] in keep]
        keys = [int(r[0]) for r in orders]
        self.rf_orders_used = len(orders)

        def apply(d):
            d.insert_rows("orders", orders)
            d.insert_rows("lineitem", lineitems)
            return len(orders) + len(lineitems)
        self.redelete += [("lineitem", "l_orderkey", keys), ("orders", "o_orderkey", keys)]
        self.written["orders"] += 1
        self.written["lineitem"] += 1
        return "rf1", apply

    def rf2_set(self, db, n):
        deletes = self.load_set(n)[2]
        keys = deletes[:self.set_orders] if self.set_orders else deletes
        self.rf_orders_used = len(keys)
        return self.rf2(db, keys)

    def rf1_(self, db):
        orders, lineitems, keys = [], [], []
        for _ in range(self.batch):
            large = self.rf1 % 4 == 3 and self.large_orders
            self.rf1 += 1
            pool = self.large_orders if large else self.template_orders
            src = pool[self.rng.randrange(len(pool))]
            new = self.next_order
            self.next_order += 1
            keys.append(new)
            orders += [(new,) + tuple(r[1:]) for r in self.cached(db, "orders", "o_orderkey", src)]
            lineitems += [(new,) + tuple(r[1:]) for r in self.cached(db, "lineitem", "l_orderkey", src)]

        def apply(d):
            d.insert_rows("orders", orders)
            d.insert_rows("lineitem", lineitems)
            return len(orders) + len(lineitems)
        self.redelete += [("lineitem", "l_orderkey", keys), ("orders", "o_orderkey", keys)]
        self.written["orders"] += 1
        self.written["lineitem"] += 1
        return "rf1", apply

    # --- point writes (each one row, or one part / supplier with its partsupp rows) -----------
    def update(self, db, kind, table, where, sets):
        """One row (where: {key column: value}, the full primary key); sets: {column: SQL expression}.
        The old values are saved for the undo."""
        cols, cond = list(sets), " AND ".join(f"{c} = {int(v)}" for c, v in where.items())
        rows = db.query(f"SELECT {','.join(cols)} FROM {table} WHERE {cond}")[0]
        assert rows, f"{table}: no row where {cond}"
        old = rows[0]
        self.reupdate.append((table, cond, dict(zip(cols, old))))
        self.written[table] += 1
        sql = f"UPDATE {table} SET " + ", ".join(f"{c} = {e}" for c, e in sets.items()) + f" WHERE {cond}"
        return kind, lambda d: (d.execute(sql), 1)[1]

    def supplier_update(self, db):
        return self.update(db, "supplier update", "supplier", {"s_suppkey": self.take("mid_supps", self.mid_supps)},
                           {"s_acctbal": "s_acctbal + 1", "s_comment": "REVERSE(s_comment)"})

    def customer_update(self, db):
        return self.update(db, "customer update", "customer", {"c_custkey": self.take("custs", self.custs)},
                           {"c_acctbal": "c_acctbal + 1", "c_mktsegment": f"'{self.rng.choice(SEGMENTS)}'"})

    def partsupp_update(self, db):
        pk = self.take("mid_parts", self.mid_parts)
        sk = db.query(f"SELECT ps_suppkey FROM partsupp WHERE ps_partkey = {int(pk)} ORDER BY ps_suppkey LIMIT 1")[0][0][0]
        return self.update(db, "partsupp update", "partsupp", {"ps_partkey": pk, "ps_suppkey": sk},
                           {"ps_supplycost": "ps_supplycost + 1", "ps_availqty": "ps_availqty + 1"})

    def lineitem_update(self, db):
        while True:  # (an order without a first line: take the next)
            k = self.take("mid_orders", self.mid_orders)
            if db.query(f"SELECT 1 FROM lineitem WHERE l_orderkey = {int(k)} AND l_linenumber = 1")[0]:
                break
        return self.update(db, "lineitem update", "lineitem", {"l_orderkey": k, "l_linenumber": 1},
                           {"l_quantity": "l_quantity + 1", "l_discount": "0.04"})

    def orders_update(self, db):
        return self.update(db, "orders update", "orders", {"o_orderkey": self.take("mid_orders", self.mid_orders)},
                           {"o_comment": "REVERSE(o_comment)",
                            "o_orderpriority": f"'{self.rng.choice(['1-URGENT', '2-HIGH', '3-MEDIUM', '5-LOW'])}'"})

    def nation_update(self, db):
        return self.update(db, "nation update", "nation", {"n_nationkey": self.nations[self.written["nation"] % len(self.nations)]},
                           {"n_comment": "REVERSE(n_comment)"})

    def region_update(self, db):
        return self.update(db, "region update", "region", {"r_regionkey": self.regions[self.written["region"] % len(self.regions)]},
                           {"r_comment": "REVERSE(r_comment)"})

    def delete_with_partsupp(self, db, kind, parent, pcol, child_col, key):
        prow, crows = db.select_in(parent, pcol, [key]), db.select_in("partsupp", child_col, [key])
        self.reinsert += [(parent, prow), ("partsupp", crows)]
        self.written[parent] += 1
        self.written["partsupp"] += 1

        def apply(d):
            d.delete_in("partsupp", child_col, [key])
            d.delete_in(parent, pcol, [key])
            return len(prow) + len(crows)
        return kind, apply

    def insert_with_partsupp(self, db, kind, parent, pcol, child_col, src, new):
        prow = [(new,) + tuple(r[1:]) for r in self.cached(db, parent, pcol, src)]
        crow = [((new,) + tuple(r[1:]) if child_col == "ps_partkey" else (r[0], new) + tuple(r[2:]))
                for r in self.cached(db, "partsupp", child_col, src)]
        self.redelete += [("partsupp", child_col, [new]), (parent, pcol, [new])]
        self.written[parent] += 1
        self.written["partsupp"] += 1

        def apply(d):
            d.insert_rows(parent, prow)
            d.insert_rows("partsupp", crow)
            return len(prow) + len(crow)
        return kind, apply

    def part_delete(self, db):
        return self.delete_with_partsupp(db, "part delete", "part", "p_partkey", "ps_partkey",
                                         self.take("low_parts", self.low_parts))

    def part_insert(self, db):
        new, self.next_part = self.next_part, self.next_part + 1
        return self.insert_with_partsupp(db, "part insert", "part", "p_partkey", "ps_partkey",
                                         self.template_parts[self.rng.randrange(len(self.template_parts))], new)

    def supplier_delete(self, db):
        return self.delete_with_partsupp(db, "supplier delete", "supplier", "s_suppkey", "ps_suppkey",
                                         self.take("low_supps", self.low_supps))

    def supplier_insert(self, db):
        new, self.next_supp = self.next_supp, self.next_supp + 1
        return self.insert_with_partsupp(db, "supplier insert", "supplier", "s_suppkey", "ps_suppkey",
                                         self.template_supps[self.rng.randrange(len(self.template_supps))], new)

    # --- undo ----------------------------------------------------------------------------
    def restore(self, db):
        """Put the data back as it was before the stream (triggers, if on, maintain the accelerators).
        Updates are undone last: a row the stream updated and later deleted (a partsupp row of a deleted supplier)
        comes back from reinsert with the updated values, and only then gets its old values."""
        for table, col, keys in reversed(self.redelete):
            db.delete_in(table, col, keys)
        for table, rows in reversed(self.reinsert):
            if rows:
                db.insert_rows(table, rows)
        for table, cond, old in reversed(self.reupdate):
            cols = list(old)
            db.execute(f"UPDATE {table} SET " + ", ".join(f"{c} = %s" for c in cols) + f" WHERE {cond}",
                       [old[c] for c in cols])


_pool_cache = {}


def _cached(db, name, sql, k):
    if name not in _pool_cache:
        _pool_cache[name] = k(sql)
    return _pool_cache[name]


FINGERPRINT = {
    "region": "COUNT(*)", "nation": "COUNT(*)", "customer": "COUNT(*), SUM(c_acctbal), SUM(c_custkey)",
    "supplier": "COUNT(*), SUM(s_acctbal), SUM(s_suppkey)", "part": "COUNT(*), SUM(p_retailprice), SUM(p_partkey)",
    "partsupp": "COUNT(*), SUM(ps_supplycost), SUM(ps_availqty)",
    "orders": "COUNT(*), SUM(o_totalprice), SUM(o_orderkey)",
    "lineitem": "COUNT(*), SUM(l_quantity), SUM(l_discount), SUM(l_extendedprice)"}


def fingerprint(db):
    """Cheap per-table checksums: the stream must leave the data exactly as it found it."""
    return {t: [str(norm(x)) for x in db.query(f"SELECT {e} FROM {t}")[0][0]] for t, e in FINGERPRINT.items()}


# ---------------------------------------------------------------------------------------------
# experiments
# ---------------------------------------------------------------------------------------------
def draws(a):
    return run_compare.draws(a)


def case_list(a, spec, system):
    """[(q, accel_sql, base_sql)] over every query and parameter set of one draw."""
    out = []
    for q in a.queries:
        for s in spec[str(q)]["sets"]:
            out.append((q, accel_sql(q, s["cpp"], a.backend), base_sql(q, s["verus"], a.backend)))
    return out


def timed(db, sql):
    """-> (rows or None, us, status)  status: ok | timeout | error:<msg>"""
    try:
        rows, us = db.query(sql)
        return rows, us, "ok"
    except Exception as e:  # noqa: BLE001
        if db.is_timeout(e):
            return None, db.timeout_s * 1e6, "timeout"
        return None, 0.0, "error: " + str(e)[:200]


def cmd_read(a):
    os.makedirs(a.out, exist_ok=True)
    reps = draws(a)
    set_triggers(a, True)
    restart_shared_worker(a)
    db = Db(a, accel=True)
    for k, seed, spec in reps:
        for q in a.queries:
            results = []
            for i, s in enumerate(spec[str(q)]["sets"]):
                asql, bsql = accel_sql(q, s["cpp"], a.backend), base_sql(q, s["verus"], a.backend)
                brows, _, bst = timed(db, bsql)  # warm, and the reference answer
                arows, first_us, ast = timed(db, asql)  # the accelerator's first call: builds its state
                row = {"params": s["params"], "base_status": bst, "accel_status": ast, "accel_first_us": first_us}
                if bst == "ok" and ast == "ok":
                    row["match"] = same(canon(a.backend, brows, False), canon(a.backend, arows, True))
                    row["rows"] = [len(brows), len(canon(a.backend, arows, True))]
                tb, ta = [], []
                for _ in range(a.base_reps if bst == "ok" else 0):
                    tb.append(timed(db, bsql)[1])
                for _ in range(a.reps if ast == "ok" else 0):
                    ta.append(timed(db, asql)[1])
                row["base_us"] = statistics.median(tb) if tb else None
                row["accel_us"] = statistics.median(ta) if ta else None
                if tb and ta:
                    row["speedup"] = row["base_us"] / max(row["accel_us"], 1.0)
                results.append(row)
            sp = [r["speedup"] for r in results if "speedup" in r]
            out = {"mode": "read", "backend": a.backend, "query": q, "rep": k, "seed": seed, "sf": a.sf, "shared_state": a.shared_state,
                   "speedup_geomean": math.exp(sum(map(math.log, sp)) / len(sp)) if sp else None,
                   "mismatches": sum(1 for r in results if r.get("match") is False), "cases": results}
            with open(os.path.join(a.out, f"q{q}_rep{k}.json"), "w") as f:
                json.dump(out, f, indent=1, default=str)
            print(f"rep {k} q{q:>2}: " + (f"speedup {out['speedup_geomean']:.1f}x " if sp else "no timing ") +
                  f"mismatches {out['mismatches']}/{len(results)}  states "
                  f"{sorted({r['base_status'] + '/' + r['accel_status'] for r in results})}", flush=True)
    db.close()


class Counters:
    def __init__(self):
        self.lock = threading.Lock()
        self.reads = []      # (q, start offset s, us, status)
        self.commits = []    # (kind, start offset s, lag s, us, rows, status)


def reader(a, system, cases, idx, ctr, t0, stop, ready):
    try:
        db = Db(a, accel=system == "accel")
        prime = []
        for q, asql, bsql in cases:  # prime: the accelerator builds its state (per backend process on Postgres)
            if system == "accel":
                _, us, st = timed(db, asql)
                prime.append(us)
    except Exception as e:  # noqa: BLE001
        print(f"  reader {idx} failed to start: {e}", flush=True)
        ready.append(0.0)
        return
    ready.append(sum(prime) / 1e6)
    while not t0[0]:
        time.sleep(0.01)
    n = len(cases)
    i = idx * max(1, n // max(1, a.readers))
    while not stop.is_set():
        q, asql, bsql = cases[i % n]
        i += 1
        s = time.perf_counter()
        rows, us, st = timed(db, asql if system == "accel" else bsql)
        with ctr.lock:
            ctr.reads.append((q, s - t0[0], us, st))
        if st.startswith("error"):
            print(f"  reader {idx} q{q}: {st}", flush=True)
            time.sleep(1)
    db.close()


def writer(a, system, rate, stream, ctr, t0, stop):
    db = Db(a, accel=system == "accel")
    interval = 0 if rate < 0 else 1.0 / rate
    nxt = t0[0]
    # (a refresh slot's point writes, --dim-after-rf, run unpaced and complete after the stop)
    while not stop.is_set() or not stream.next_is_paced():
        if stream.next_is_paced():
            wait = nxt - time.perf_counter()
            if wait > 0 and stop.wait(wait):
                break
            sched = nxt
            nxt += interval
        else:
            sched = time.perf_counter()
        try:
            kind, apply = stream.next(db)
        except Exception as e:  # noqa: BLE001  (a pool ran out, a query failed: the run is not valid)
            print(f"  WRITER STOPPED: {type(e).__name__}: {e}", flush=True)
            with ctr.lock:
                ctr.commits.append(("stream error", time.perf_counter() - t0[0], 0.0, 0.0, 0, "error: " + str(e)[:200]))
            break
        s = time.perf_counter()
        try:
            db.begin()
            rows = apply(db)
            db.commit()
            st = "ok"
        except Exception as e:  # noqa: BLE001
            db.rollback()
            rows, st = 0, "error: " + str(e)[:200]
            print(f"  writer {kind}: {st}", flush=True)
        us = (time.perf_counter() - s) * 1e6
        with ctr.lock:
            ctr.commits.append((kind, s - t0[0], max(0.0, s - sched), us, rows, st))
    db.close()


def final_check(a, db, cases_one, before):
    """Accelerator answers after the stream vs the DBMS's (and vs before it)."""
    out = {}
    db.set_timeout(a.check_timeout)
    for q, asql, bsql in cases_one:
        arows, _, ast = timed(db, asql)
        brows, _, bst = timed(db, bsql)
        e = {"accel": ast, "base": bst}
        if ast == "ok":
            ca = canon(a.backend, arows, True)
            e["changed"] = None if before.get(q) is None else not same(before[q], ca)
            if bst == "ok":
                e["match"] = same(canon(a.backend, brows, False), ca)
        out[q] = e
    db.set_timeout(a.query_timeout)
    return out


def run_one(a, system, rate, rep, seed, spec):
    cases = case_list(a, spec, system)
    triggers = set_triggers(a, system == "accel")
    if (system == "accel") != (triggers > 0):
        raise SystemExit(f"{system}: {triggers} enabled capture triggers (base needs none, accel needs them)")
    restart_shared_worker(a)
    ctr, stop, ready, t0 = Counters(), threading.Event(), [], [0.0]
    ctl = Db(a, accel=system == "accel")
    fp0 = fingerprint(ctl)
    commits = int((a.warmup + a.seconds) * (200 if rate < 0 else rate) * 1.3) + 8
    commits = min(commits, a.max_commits, a.max_write_commits or a.max_commits)
    stream = Stream(ctl, commits, 1 if a.rf_dir else a.batch_orders, seed, a.rf_only, a.dim_after_rf,
                    a.rf_dir, a.batch_orders or 0 if a.rf_dir else 0)
    one = [next(c for c in cases if c[0] == q) for q in a.queries]  # one case per query, for the checks
    before = {}
    if system == "accel":
        for q, asql, _ in one:
            rows, _, st = timed(ctl, asql)
            before[q] = canon(a.backend, rows, True) if st == "ok" else None
    ts = [threading.Thread(target=reader, args=(a, system, cases, i, ctr, t0, stop, ready)) for i in range(a.readers)]
    wt = threading.Thread(target=writer, args=(a, system, rate, stream, ctr, t0, stop)) if rate != 0 else None
    for t in ts:
        t.start()
    while len(ready) < a.readers:
        time.sleep(0.2)
    t_prime = max(ready or [0.0])
    stats0 = shared_stats(a, ctl) if system == "accel" else None
    t0[0] = time.perf_counter()
    if wt:
        wt.start()
    end = t0[0] + a.warmup + a.seconds
    while time.perf_counter() < end:
        time.sleep(0.2)
        if a.max_write_commits and wt and len(ctr.commits) >= a.max_write_commits:
            break
        if wt and not wt.is_alive():  # the writer died (its error is printed): do not wait out the window
            print("  WRITER THREAD EXITED before the window ended", flush=True)
            break
    stop.set()
    for t in ts + ([wt] if wt else []):
        t.join()
    lo, hi = a.warmup, a.warmup + a.seconds
    reads = [r for r in ctr.reads if lo <= r[1] < hi]
    commits_m = [c for c in ctr.commits if lo <= c[1] < hi]
    per_q = {}
    for q in a.queries:
        us = [r[2] for r in reads if r[0] == q and r[3] == "ok"]
        per_q[q] = {"reads": len(us), "p50_us": pct(us, .5), "p95_us": pct(us, .95), "p99_us": pct(us, .99),
                    "timeouts": sum(1 for r in reads if r[0] == q and r[3] == "timeout"),
                    "errors": sum(1 for r in reads if r[0] == q and r[3].startswith("error"))}
    cus = [c[3] for c in commits_m if c[5] == "ok"]
    by_kind = {}
    for c in ctr.commits:
        by_kind.setdefault(c[0], []).append(c[3])
    stats1 = shared_stats(a, ctl) if system == "accel" else None
    check = final_check(a, ctl, one, before) if system == "accel" or a.check_base else {}
    t_r = time.perf_counter()
    stream.restore(ctl)
    restore_s = time.perf_counter() - t_r
    fp1 = fingerprint(ctl)
    restore_ok = fp0 == fp1
    if not restore_ok:
        print("  !!! RESTORE FAILED: table contents differ from before the run: " +
              ", ".join(t for t in fp0 if fp0[t] != fp1[t]), flush=True)
    ctl.close()
    ok = [r for r in reads if r[3] == "ok"]
    expected_tables = (set(TABLES) if not (a.rf_only or a.dim_after_rf) else
                       {"orders", "lineitem", "part", "partsupp", "supplier"} if a.dim_after_rf
                       else {"orders", "lineitem"})
    res = {"mode": "writerate", "backend": a.backend, "system": system, "rate_target": rate, "rep": rep, "seed": seed,
           "sf": a.sf, "batch_orders": a.batch_orders,
           "rf_source": "tpch_update_sets" if a.rf_dir else "clone", "rf_dir": a.rf_dir,
           "rf_orders_used": stream.rf_orders_used or a.batch_orders, "readers": a.readers, "shared_state": a.shared_state, "seconds": a.seconds, "warmup": a.warmup,
           "queries": a.queries, "sets_per_query": a.sets, "prime_s": t_prime, "triggers_on": triggers,
           "qps": len(ok) / a.seconds, "reads": len(reads), "timeouts": sum(1 for r in reads if r[3] == "timeout"),
           "read_errors": sum(1 for r in reads if r[3].startswith("error")),
           "read_p50_us": pct([r[2] for r in ok], .5), "read_p99_us": pct([r[2] for r in ok], .99), "per_query": per_q,
           "commits": len(commits_m), "commits_total": len(ctr.commits),
           "commit_rate": len(commits_m) / a.seconds,
           "rf_commits": sum(1 for c in commits_m if c[0] in ("rf1", "rf2")),
           "rf_rate": sum(1 for c in commits_m if c[0] in ("rf1", "rf2")) / a.seconds,
           "write_errors": sum(1 for c in ctr.commits if c[5] != "ok"),
           "commit_p50_us": pct(cus, .5), "commit_p99_us": pct(cus, .99), "commit_max_us": max(cus or [0]),
           "writer_lag_p50_s": pct([c[2] for c in commits_m], .5), "writer_lag_max_s": max([c[2] for c in commits_m] or [0]),
           "rows_per_commit": statistics.mean([c[4] for c in commits_m]) if commits_m else 0,
           "rows_per_s": sum(c[4] for c in commits_m if c[5] == "ok") / a.seconds,
           "rf_rows_per_s": sum(c[4] for c in commits_m if c[5] == "ok" and c[0] in ("rf1", "rf2")) / a.seconds,
           "pg_workers": getattr(a, "pg_workers", None),
           "commit_us_by_kind": {k: {"n": len(v), "p50": pct(v, .5), "max": max(v)} for k, v in by_kind.items()},
           "tables_written": stream.written, "tables_not_written": [t for t, n in stream.written.items()
                                  if n == 0 and t in expected_tables],
           "rf_only": a.rf_only, "dim_after_rf": a.dim_after_rf,
           "final_check": check, "restore_s": restore_s, "restore_ok": restore_ok,
           "shared_worker": {k: stats1[k] - stats0.get(k, 0) for k in stats1 if k not in ("generation", "pid")}
                            if stats0 and stats1 else None}
    if system == "accel":
        res["answers_changed"] = sorted(q for q, e in check.items() if e.get("changed"))
        res["answers_unchanged"] = sorted(q for q, e in check.items() if e.get("changed") is False)
        res["final_mismatches"] = sorted(q for q, e in check.items() if e.get("match") is False)
    return res


def rate_name(r):
    return "uncapped" if r < 0 else f"cps{r:g}"


def table_parallel_workers(a, workers=None):
    """Read the TPC-H tables' parallel_workers reloptions; with `workers` (dict or int), set them first."""
    import psycopg2
    c = psycopg2.connect(host=a.host, port=a.port, user=a.user, password=a.password, dbname=a.db)
    c.autocommit = True
    cur = c.cursor()
    if workers is not None:
        for t in TABLES:
            w = workers.get(t) if isinstance(workers, dict) else workers
            cur.execute(f"ALTER TABLE {t} RESET (parallel_workers)" if w is None
                        else f"ALTER TABLE {t} SET (parallel_workers = {int(w)})")
    out = {}
    for t in TABLES:
        cur.execute("SELECT option_value FROM pg_options_to_table((SELECT reloptions FROM pg_class "
                    "WHERE oid = %s::regclass)) WHERE option_name = 'parallel_workers'", (t,))
        r = cur.fetchone()
        out[t] = int(r[0]) if r else None
    c.close()
    return out


def cmd_writerate(a):
    if a.backend == "postgres" and a.pg_workers is not None:
        # PostgreSQL sizes a scan's workers from the table's parallel_workers reloption (capped by
        # max_parallel_workers_per_gather): set it to the per-session budget for the run, restore it after.
        original = table_parallel_workers(a)
        print(f"# --pg-workers {a.pg_workers}: max_parallel_workers_per_gather and table parallel_workers = "
              f"{a.pg_workers} for every session (tables' original reloptions {original}, restored on exit)", flush=True)
        table_parallel_workers(a, a.pg_workers)
        try:
            return cmd_writerate_body(a)
        finally:
            table_parallel_workers(a, original)
    return cmd_writerate_body(a)


def cmd_writerate_body(a):
    os.makedirs(a.out, exist_ok=True)
    rates = [float(x) for x in str(a.rates).replace(",", " ").split()]
    reps = draws(a)
    for rate in rates:
        for k, seed, spec in reps:
            order = ["base", "accel"] if k % 2 == 0 else ["accel", "base"]  # alternate, so drift cancels
            for system in order:
                print(f"{rate_name(rate)} rep {k} {system} (RF: {'update sets, ' + (str(a.batch_orders) if a.batch_orders else 'whole sets') if a.rf_dir else str(a.batch_orders) + ' cloned orders'}, {a.readers} readers) "
                      f"{time.strftime('%T')}", flush=True)
                r = run_one(a, system, rate, k, seed, spec)
                d = os.path.join(a.out, rate_name(rate), f"rep{k}")
                os.makedirs(d, exist_ok=True)
                with open(os.path.join(d, f"{system}.json"), "w") as f:
                    json.dump(r, f, indent=1, default=str)
                print("   " + line(r), flush=True)
                if a.vacuum and a.backend == "postgres":
                    vacuum(a)
    set_triggers(a, True)


def vacuum(a):
    import psycopg2
    c = psycopg2.connect(host=a.host, port=a.port, user=a.user, password=a.password, dbname=a.db)
    c.autocommit = True
    cur = c.cursor()
    for t in TABLES:
        cur.execute(f"VACUUM (ANALYZE) {t}")
    c.close()


def line(r):
    s = (f"qps {r['qps']:.1f}  read p50 {r['read_p50_us'] / 1000:.1f}ms p99 {r['read_p99_us'] / 1000:.1f}ms  timeouts "
         f"{r['timeouts']}  RF commits {r['rf_commits']} ({r['rf_rate']:.3g}/s of {r['rate_target']:g}) of {r['commits']} commits  commit p50 "
         f"{r['commit_p50_us'] / 1000:.1f}ms max {r['commit_max_us'] / 1000:.0f}ms  lag max {r['writer_lag_max_s']:.1f}s  "
         f"rows/s {r.get('rows_per_s', 0):.0f} (RF {r.get('rf_rows_per_s', 0):.0f})  "
         f"errors read {r['read_errors']} write {r['write_errors']}")
    if r["system"] == "accel":
        s += (f"  final-check mismatches {r['final_mismatches'] or 'none'}  answers unchanged by writes "
              f"{r['answers_unchanged'] or 'none'}")
    if r.get("shared_worker"):
        w = r["shared_worker"]
        s += (f"  worker: applies {w.get('applies', 0)} as delta {w.get('synced', 0)} not {w.get('unsynced', 0)} "
              f"rebuilds {w.get('rebuilds', 0)} declined {w.get('declined', 0)} errors {w.get('errors', 0)}")
    if not r["restore_ok"]:
        s += "  RESTORE FAILED"
    if r["reads"] == 0:
        s += "  NO READ COMPLETED IN THE WINDOW (reads take longer than it: lengthen the window)"
    if r["tables_not_written"]:
        s += f"  TABLES NOT WRITTEN {r['tables_not_written']}"
    return s


def cmd_summary(a):
    runs = {}
    for f in sorted(glob.glob(os.path.join(a.dir, "*", "rep*", "*.json"))):
        r = json.load(open(f))
        runs.setdefault((r["rate_target"], r["system"]), []).append(r)

    def ms(rs, key, scale=1000.0):
        xs = [r[key] / scale for r in rs]
        return f"{statistics.median(xs):.1f} [{min(xs):.1f}-{max(xs):.1f}]"
    print(f"{'rate':>9} {'system':>6} {'qps':>20} {'read p50 ms':>20} {'read p99 ms':>20} {'commit p50 ms':>20} {'lag max s':>14}")
    for (rate, system), rs in sorted(runs.items()):
        print(f"{rate_name(rate):>9} {system:>6} {ms(rs, 'qps', 1):>20} {ms(rs, 'read_p50_us'):>20} "
              f"{ms(rs, 'read_p99_us'):>20} {ms(rs, 'commit_p50_us'):>20} {ms(rs, 'writer_lag_max_s', 1):>14}")
        if system == "accel":
            bad = sorted({q for r in rs for q in r.get("final_mismatches", [])})
            unch = sorted(set.intersection(*[set(r.get("answers_unchanged", [])) for r in rs]))
            if bad:
                print(f"{'':>16} FINAL-CHECK MISMATCH queries: {bad}")
            if unch:
                print(f"{'':>16} never changed by the writes (not exercised): {unch}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    for mode in ("read", "writerate"):
        p = sub.add_parser(mode)
        p.add_argument("--backend", choices=("postgres", "mysql"), required=True)
        p.add_argument("--host", default="127.0.0.1")
        p.add_argument("--port", type=int, default=None, help="default: PG_PORT (5434) / MYSQL_TCP_PORT (3307)")
        p.add_argument("--user", default=None)
        p.add_argument("--password", default=None)
        p.add_argument("--db", required=True, help="database holding TPC-H (and the accelerator functions / triggers)")
        p.add_argument("--pg-so", default="/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so")
        p.add_argument("--out", default=os.path.join(HERE, "results", f"{mode}_sql_{time.strftime('%Y%m%d_%H%M%S')}"))
        p.add_argument("--queries", default=" ".join(map(str, range(1, 23))))
        p.add_argument("--sets", type=int, default=4, help="TPC-H parameter sets per query")
        p.add_argument("--seed", type=int, default=None)
        p.add_argument("--repeats", type=int, default=3, help="repetitions, each with its own parameter draw")
        p.add_argument("--sf", type=float, default=20)
        p.add_argument("--query-timeout", type=float, default=1800, help="seconds, per baseline query (timeouts are reported)")
        p.add_argument("--shared-state", action=argparse.BooleanOptionalAction, default=False,
                       help="postgres: one shared copy of the accelerators in a background worker, maintained by "
                            "deltas (accel.shared_state); default: a copy per connection, rebuilt after others' writes")
        p.add_argument("--apply-timeout", type=float, default=600,
                       help="seconds a committing writer waits for the shared worker's apply (--shared-state)")
    r = sub.choices["read"]
    r.add_argument("--reps", type=int, default=5, help="timed accelerator runs per parameter set")
    r.add_argument("--base-reps", type=int, default=2, help="timed baseline runs per parameter set (after one warm run)")
    w = sub.choices["writerate"]
    w.add_argument("--readers", type=int, default=8)
    w.add_argument("--pg-workers", type=int, default=None,
                   help="Postgres: max_parallel_workers_per_gather (and the tables' parallel_workers) for every "
                        "session of the run, e.g. floor(cores / readers) - 1; default: the server's settings")
    w.add_argument("--rates", default="1 5 20", help="writer commits/s (list; -1 uncapped; 0 no writes)")
    w.add_argument("--batch-orders", type=int, default=None,
                   help="orders per RF1 / RF2 commit.  With --rf-dir: only the first B orders of each update set (default: "
                        "the whole set, 1500 x SF orders).  Without it: cloned orders per commit (default 1)")
    w.add_argument("--rf-dir", default=os.environ.get("RF_DIR") or None,
                   help="directory of TPC-H update sets written by `dbgen -s SF -U N` (orders.tbl.uK, lineitem.tbl.uK, "
                        "delete.K): RF1 of set K inserts its orders, RF2 of set K deletes the listed ones, the "
                        "specification's refresh functions (default: $RF_DIR)")
    w.add_argument("--seconds", type=float, default=60)
    w.add_argument("--warmup", type=float, default=5)
    w.add_argument("--max-commits", type=int, default=3000)
    w.add_argument("--rf-only", action="store_true", help="the writer commits RF1 / RF2 only (no point writes)")
    w.add_argument("--dim-after-rf", action="store_true",
                   help="every RF commit is followed by one point write of each kind on part / partsupp / supplier "
                        "(supplier update, part and supplier delete + insert, partsupp update), unpaced")
    w.add_argument("--max-write-commits", type=int, default=0, help="end the run after this many commits (calibration)")
    w.add_argument("--check-timeout", type=float, default=120, help="seconds, per baseline query of the final check")
    w.add_argument("--check-base", action="store_true", help="also run the final check in base runs (answers only)")
    w.add_argument("--vacuum", action=argparse.BooleanOptionalAction, default=True,
                   help="Postgres: VACUUM ANALYZE after every run (autovacuum is off in the test container)")
    s = sub.add_parser("summary")
    s.add_argument("dir")
    a = ap.parse_args()
    if a.mode == "summary":
        return cmd_summary(a)
    a.queries = [int(x) for x in a.queries.replace(",", " ").split()]
    if a.mode == "writerate" and not a.rf_dir and a.batch_orders is None:
        a.batch_orders = 1  # clone mode: the old one-order mini-chunk
    pg = a.backend == "postgres"
    a.port = a.port or int(os.environ.get("PG_PORT" if pg else "MYSQL_TCP_PORT", 5434 if pg else 3307))
    a.user = a.user or os.environ.get("PGUSER" if pg else "MYSQL_USER", "postgres" if pg else "root")
    a.password = a.password or os.environ.get("PGPASSWORD" if pg else "MYSQL_PWD", "postgres" if pg else "mysql")
    {"read": cmd_read, "writerate": cmd_writerate}[a.mode](a)


if __name__ == "__main__":
    main()
