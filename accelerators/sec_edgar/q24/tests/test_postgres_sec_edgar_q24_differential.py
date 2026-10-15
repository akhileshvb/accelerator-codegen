#!/usr/bin/env python3
"""SEC-EDGAR Q24 aux (PostgreSQL) vs plain PostgreSQL SQL, end to end.

Drives real psql backends (the aux state is per backend), so every step goes
through the loaded accelerator_codegen_pg module: capture triggers, the
transaction coordinator, PRE_COMMIT maintenance (incremental lookups through
SPI), rebuilds, rollbacks, savepoints and cross-backend sync.

Phases
  edges    scripted HAVING (> 10) / date-range / uom / NULL-key / LIMIT (100)
           boundary scenarios, compared after every step
  random   randomized INSERT / DELETE / UPDATE on num and pre (relevant and
           irrelevant columns, join-key moves, duplicates, NULL keys), single
           statements and multi-statement transactions (COMMIT, ROLLBACK,
           SAVEPOINT + ROLLBACK TO), a second backend reading and writing
           (cross-backend sync), compared after every step and inside open
           transactions
  sf       (--existing-db) read-only check on an already loaded database,
           then real writes: a rolled-back transaction, a committed value
           update and a committed pre delete of the top group (restored)

Every comparison also draws random (year, uom) parameters (the benchmark domain
plus edges: NULLs, unknown uom, years 0 / 2017 / 2025 / huge) and compares
accel_sec_edgar_q24_aux(year, uom) with the plain query bound to them.
--sweep runs a parameter sweep on an existing (SF3) database.

The reference is the plain query (workloads/sec_edgar/q24.sql) with a
deterministic tie-break appended to ORDER BY (cnt DESC leaves ties unordered;
the aux result breaks them by tag, version in byte order, NULLs first).

Env: PGHOST PGPORT PGUSER PGPASSWORD (libpq), Q24_PG_DB (default q24_diff),
     Q24_SEED, Q24_STEPS. The accelerator_codegen_postgres extension must be
     installed in the server (build_pg_ext.sh).
Usage: test_postgres_sec_edgar_q24_differential.py [--existing-db DB] [--steps N]
       [--seed S] [--create-only]
"""
import argparse
import math
import os
import random
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
SCRIPTS = os.path.join(ROOT, "scripts")

ACCEL_SQL = "SELECT tag, version, cnt, total FROM accel_sec_edgar_q24_aux()"
PLAIN_SQL = (
    "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
    "FROM num n LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND "
    "n.adsh = p.adsh "
    "WHERE n.uom = 'USD' AND n.ddate BETWEEN 20230101 AND 20231231 "
    "AND n.value IS NOT NULL AND p.adsh IS NULL "
    "GROUP BY n.tag, n.version HAVING COUNT(*) > 10 "
    "ORDER BY cnt DESC, n.tag COLLATE \"C\" NULLS FIRST, "
    "n.version COLLATE \"C\" NULLS FIRST LIMIT 100"
)
NULLTXT = "<NULL>"
YEARS = [2019, 2020, 2021, 2022, 2023, 2024]
UOMS = ["USD", "shares", "pure", "CAD", "CNY", "EUR"]
# Buckets that hold HAVING > 10 groups in the synthetic data.
HOT = [(2023, "USD"), (2024, "USD"), (2023, "EUR"), (2022, "shares"), (2024, "EUR"),
       (2019, "pure")]


def plain_sql(year, uom):
    """q24 with :year / :uom bound; a huge year (BIGINT overflow in the
    reference) is clamped: no INTEGER ddate lies there, both are empty."""
    if year is not None:
        year = max(-10 ** 9, min(10 ** 9, year))
    ys = "NULL" if year is None else str(year)
    return (
        "SELECT n.tag, n.version, COUNT(*) AS cnt, SUM(n.value) AS total "
        "FROM num n LEFT JOIN pre p ON n.tag = p.tag AND n.version = p.version AND "
        "n.adsh = p.adsh "
        "WHERE n.uom = %s AND n.ddate BETWEEN %s::bigint*10000+101 AND %s::bigint*10000+1231 "
        "AND n.value IS NOT NULL AND p.adsh IS NULL "
        "GROUP BY n.tag, n.version HAVING COUNT(*) > 10 "
        "ORDER BY cnt DESC, n.tag COLLATE \"C\" NULLS FIRST, "
        "n.version COLLATE \"C\" NULLS FIRST LIMIT 100"
    ) % (lit(uom), ys, ys)


def accel_sql(year, uom):
    ys = "NULL::bigint" if year is None else "%d::bigint" % year
    us = "NULL::text" if uom is None else lit(uom) + "::text"
    return "SELECT tag, version, cnt, total FROM accel_sec_edgar_q24_aux(%s, %s)" % (ys, us)


class Fail(Exception):
    pass


class Session:
    """One long-lived psql backend."""

    def __init__(self, db, name):
        self.name = name
        self.db = db
        env = dict(os.environ)
        self.p = subprocess.Popen(
            ["psql", "-X", "-q", "-A", "-t", "-F", "|", "-d", db,
             "-v", "ON_ERROR_STOP=0"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
        self.n = 0
        self.run("\\pset null '%s'\n" % NULLTXT)

    def run(self, sql, allow_error=False):
        self.n += 1
        mark = "@@END%d" % self.n
        sql = sql.rstrip()
        if not sql.startswith("\\") and not sql.endswith(";"):
            sql += ";"
        self.p.stdin.write(sql + "\n\\echo " + mark + "\n")
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if line == "":
                raise Fail("[%s] psql exited; output so far:\n%s" %
                           (self.name, "\n".join(out)))
            line = line.rstrip("\n")
            if line == mark:
                break
            out.append(line)
        if not allow_error:
            for l in out:
                if "ERROR:" in l or "FATAL:" in l or "server closed" in l:
                    raise Fail("[%s] SQL error: %s\n  in: %s" %
                               (self.name, l, sql[:500]))
        return out

    def rows(self, sql):
        return [tuple(l.split("|")) for l in self.run(sql) if l != ""]

    def close(self):
        try:
            self.p.stdin.write("\\q\n")
            self.p.stdin.flush()
            self.p.wait(timeout=20)
        except Exception:
            self.p.kill()


def close_total(a, b):
    if a == b:
        return True
    if a == NULLTXT or b == NULLTXT:
        return False
    x, y = float(a), float(b)
    if math.isnan(x) and math.isnan(y):
        return True
    return abs(x - y) <= max(1.0, abs(x), abs(y)) * 1e-9


class Checker:
    def __init__(self):
        self.compares = 0
        self.max_rows = 0
        self.limit_hits = 0
        self.empty = 0
        self.param_compares = 0
        self.param_nonempty = 0

    gen = None  # set by main(): enables random parameter comparisons

    def compare(self, sess, ctx, exp=None):
        self.compare_params(sess, ctx, None, None, exp, default=True)
        if self.gen is not None and exp is None:
            for year, uom in self.gen.params(2):
                self.compare_params(sess, ctx, year, uom)

    def compare_params(self, sess, ctx, year, uom, exp=None, default=False):
        got = sess.rows(ACCEL_SQL if default else accel_sql(year, uom))
        if exp is None:
            exp = sess.rows(PLAIN_SQL if default else plain_sql(year, uom))
        if not default:
            ctx = "%s [year=%s uom=%s]" % (ctx, year, uom)
            self.param_compares += 1
            self.param_nonempty += len(exp) > 0
        self.compares += 1
        self.max_rows = max(self.max_rows, len(exp))
        self.limit_hits += len(exp) == 100
        self.empty += len(exp) == 0
        ok = len(got) == len(exp)
        if ok:
            for g, e in zip(got, exp):
                if (len(g) != 4 or g[0] != e[0] or g[1] != e[1] or g[2] != e[2]
                        or not close_total(g[3], e[3])):
                    ok = False
                    break
        if not ok:
            raise Fail("MISMATCH after %s [%s]\n  accel(%d rows): %s\n  plain(%d rows): %s" %
                       (ctx, sess.name, len(got), got[:6], len(exp), exp[:6]))


# ── data model ───────────────────────────────────────────────────────────────

ADSH = ["0001-23-%06d" % i for i in range(12)]
TAGS = ["Tag%02d" % i for i in range(36)] + ["O'Brien", "Back\\slash", "Ünï|x"[:3]]
VERS = ["us-gaap/2022", "us-gaap/2023", "ifrs/2023"]
DATES = [20221231, 20230101, 20230102, 20230615, 20231230, 20231231, 20240101,
         20240615, 20241231, 20220630, 20190315, 20200101, 20210930, 20250102,
         # month/day outside every year window, year-0 and negative-year buckets
         20230100, 20231232, 20230000, 20221300, 201, -9000, 0]
DATE_WEIGHTS = [3, 2, 1, 3, 1, 2, 1, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]


def lit(s):
    if s is None:
        return "NULL"
    return "E'" + s.replace("\\", "\\\\").replace("'", "''") + "'"


def ilit(v):
    return "NULL" if v is None else str(v)


class Gen:
    def __init__(self, seed):
        self.r = random.Random(seed)

    def pick(self, seq, null_p=0.0):
        return None if self.r.random() < null_p else self.r.choice(seq)

    def date(self, null_p=0.03):
        if self.r.random() < null_p:
            return None
        return self.r.choices(DATES, DATE_WEIGHTS)[0]

    def uom(self):
        return self.r.choices(
            ["USD", "EUR", "shares", "pure", "CAD", "CNY", "usd", "GBP", None],
            [55, 12, 8, 6, 4, 3, 4, 4, 2])[0]

    def params(self, k):
        """k random (year, uom) pairs: hot buckets, the domain and the edges."""
        out = []
        for _ in range(k):
            x = self.r.random()
            if x < 0.55:
                out.append(self.r.choice(HOT))
                continue
            y = self.r.choice(YEARS)
            e = self.r.random()
            if e < 0.06:
                y = None
            elif e < 0.12:
                y = self.r.choice([2017, 2025, 0, -1, 201 // 10000, 4 * 10 ** 18, -(4 * 10 ** 18)])
            u = self.r.choice(UOMS)
            e = self.r.random()
            if e < 0.04:
                u = None
            elif e < 0.1:
                u = "nosuchuom"
            out.append((y, u))
        return out

    def value(self):
        k = self.r.random()
        if k < 0.06:
            return None
        if k < 0.35:
            return float(self.r.randrange(100000))
        if k < 0.6:
            return self.r.randrange(1000) / 7.0
        if k < 0.7:
            return -float(self.r.randrange(5000)) / 3.0
        if k < 0.78:
            return 1e9 + self.r.randrange(1000) / 8.0
        return self.r.randrange(10 ** 6) * 1e-3

    def num_row(self, tags=TAGS, uom_usd_p=0.8):
        uom = self.uom()
        return (self.pick(ADSH), self.pick(tags, 0.02), self.pick(VERS, 0.01),
                self.date(), self.r.randrange(4), uom,
                self.pick(["", "x"]), self.value())

    def pre_row(self):
        return (self.pick(ADSH), self.pick(TAGS, 0.02), self.pick(VERS, 0.01))


def num_insert(rows):
    vals = ",".join("(%s,%s,%s,%s,%d,%s,%s,%s,NULL)" % (
        lit(a), lit(t), lit(v), ilit(d), q, lit(u), lit(c),
        "NULL" if val is None else repr(val)) for a, t, v, d, q, u, c, val in rows)
    return "INSERT INTO num(adsh,tag,version,ddate,qtrs,uom,coreg,value,footnote) VALUES " + vals


def pre_insert(rows):
    vals = ",".join("(%s,1,1,'BS',0,'H',%s,%s,'lbl',0)" % (lit(a), lit(t), lit(v))
                    for a, t, v in rows)
    return "INSERT INTO pre(adsh,report,line,stmt,inpth,rfile,tag,version,plabel,negating) VALUES " + vals


def load_schema_and_data(admin, db, gen):
    admin.run("DROP DATABASE IF EXISTS %s WITH (FORCE)" % db)
    admin.run("CREATE DATABASE %s" % db)
    s = Session(db, "setup")
    with open(os.path.join(ROOT, "workloads", "sec_edgar", "schema.sql")) as f:
        schema = f.read().replace("DOUBLE,", "DOUBLE PRECISION,")
    s.run(schema)
    s.run("INSERT INTO sub(adsh) VALUES " + ",".join("(%s)" % lit(a) for a in ADSH))
    for _ in range(6):
        s.run(num_insert([gen.num_row() for _ in range(500)]))
    for _ in range(3):
        s.run(pre_insert([gen.pre_row() for _ in range(250)]))
    s.run(open(os.path.join(SCRIPTS, "postgres_sec_edgar_q24_indexes.sql")).read())
    return s


def install_module(s):
    s.run("DROP EXTENSION IF EXISTS accelerator_codegen_postgres CASCADE")
    s.run("CREATE EXTENSION accelerator_codegen_postgres")
    # Fixed SEC-EDGAR capture ids (same as the instance template): num=2 pre=3.
    s.run("\\i %s" % os.path.join(SCRIPTS, "postgres_accel_mutation_triggers.sql"))
    s.run("SELECT create_accel_mutation_triggers('num','accel_codegen_sec_edgar')")
    s.run("SELECT create_accel_mutation_triggers('pre','accel_codegen_sec_edgar')")


# ── random writes ────────────────────────────────────────────────────────────

def ctid_sub(table, where, k):
    return "ctid = ANY(ARRAY(SELECT ctid FROM %s WHERE %s LIMIT %d))" % (table, where, k)


def rand_where_num(g):
    k = g.r.randrange(6)
    if k == 0:
        return "tag = %s" % lit(g.pick(TAGS))
    if k == 1:
        return "adsh = %s AND version = %s" % (lit(g.pick(ADSH)), lit(g.pick(VERS)))
    if k == 2:
        return "adsh = %s AND tag = %s" % (lit(g.pick(ADSH)), lit(g.pick(TAGS)))
    if k == 3:
        return "uom = 'USD' AND ddate = %d" % g.pick([d for d in DATES if d is not None])
    if k == 4:
        return "value IS NULL"
    return "random() < 0.01"


def rand_where_pre(g):
    k = g.r.randrange(5)
    if k == 0:
        return "tag = %s" % lit(g.pick(TAGS))
    if k == 1:
        return "adsh = %s" % lit(g.pick(ADSH))
    if k == 2:
        return "adsh = %s AND tag = %s" % (lit(g.pick(ADSH)), lit(g.pick(TAGS)))
    if k == 3:
        return "version = %s AND random() < 0.2" % lit(g.pick(VERS))
    return "random() < 0.01"


def random_write(g):
    """One statement, returned with a label."""
    kind = g.r.randrange(17)
    kk = 1 + g.r.randrange(8)
    if kind <= 1:
        return "num_ins", num_insert([g.num_row() for _ in range(1 + g.r.randrange(30))])
    if kind == 2:
        return "num_del", "DELETE FROM num WHERE " + ctid_sub("num", rand_where_num(g), kk)
    if kind == 3:
        v = g.r.choice(["value + 0.1", "value * 2", "value / 3", "NULL", "-value",
                        "%r" % (g.r.randrange(10 ** 5) / 3.0)])
        return "num_upd_value", ("UPDATE num SET value = %s WHERE " % v) + ctid_sub(
            "num", rand_where_num(g), kk)
    if kind == 4:
        col, val = g.r.choice([("tag", lit(g.pick(TAGS))), ("version", lit(g.pick(VERS))),
                               ("adsh", lit(g.pick(ADSH))), ("tag", "NULL")])
        return "num_upd_key", ("UPDATE num SET %s = %s WHERE " % (col, val)) + ctid_sub(
            "num", rand_where_num(g), kk)
    if kind == 5:
        col, val = g.r.choice([("ddate", ilit(g.date())), ("uom", lit(g.uom())),
                               ("uom", "'GBP'"), ("ddate", "NULL"), ("uom", "NULL")])
        return "num_upd_filter", ("UPDATE num SET %s = %s WHERE " % (col, val)) + ctid_sub(
            "num", rand_where_num(g), kk)
    if kind == 6:
        return "num_upd_irrelevant", ("UPDATE num SET qtrs = qtrs + 1, footnote = 'f' WHERE "
                                      + ctid_sub("num", rand_where_num(g), kk))
    if kind <= 8:
        rows = [g.pre_row() for _ in range(1 + g.r.randrange(6))]
        if g.r.random() < 0.3:  # duplicate an existing triple
            return "pre_ins_dup", ("INSERT INTO pre(adsh,report,line,stmt,inpth,rfile,tag,version,plabel,negating) "
                                   "SELECT adsh,report,line,stmt,inpth,rfile,tag,version,plabel,negating "
                                   "FROM pre WHERE %s LIMIT %d" % (rand_where_pre(g), kk))
        return "pre_ins", pre_insert(rows)
    if kind <= 10:
        return "pre_del", "DELETE FROM pre WHERE " + ctid_sub("pre", rand_where_pre(g), kk)
    if kind == 11:
        col, val = g.r.choice([("tag", lit(g.pick(TAGS))), ("version", lit(g.pick(VERS))),
                               ("adsh", lit(g.pick(ADSH)))])
        return "pre_upd_key", ("UPDATE pre SET %s = %s WHERE " % (col, val)) + ctid_sub(
            "pre", rand_where_pre(g), kk)
    if kind == 12:
        return "pre_upd_irrelevant", ("UPDATE pre SET plabel = 'p', line = line + 1 WHERE "
                                      + ctid_sub("pre", rand_where_pre(g), kk))
    if kind == 13:  # big pre delete: more than the incremental triple budget
        return "pre_del_bulk", "DELETE FROM pre WHERE ctid = ANY(ARRAY(SELECT ctid FROM pre WHERE random() < 0.5 LIMIT 400))"
    if kind == 14:  # bulk num re-key
        return "num_upd_bulk", ("UPDATE num SET value = value + 1 WHERE random() < 0.05")
    if kind == 15:  # insert num rows into an existing group as USD / in range
        r = g.num_row()
        year, uom = g.r.choice(HOT)
        r = (r[0], r[1], r[2], year * 10000 + 601, r[4], uom, r[6], float(g.r.randrange(100)))
        return "num_ins_group", num_insert([r] * (1 + g.r.randrange(12)))
    return "mixed_stmt", ("WITH d AS (DELETE FROM pre WHERE %s RETURNING adsh,tag,version) "
                          "INSERT INTO num(adsh,tag,version,ddate,qtrs,uom,coreg,value) "
                          "SELECT adsh,tag,version,20230301,0,'USD','',1.5 FROM d" %
                          ctid_sub("pre", rand_where_pre(g), kk))


def edges_phase(sa, sb, chk, g):
    """Scripted boundary scenarios."""
    def step(sql, ctx, sess=None):
        (sess or sa).run(sql)
        chk.compare(sess or sa, ctx)
        chk.compare(sb, ctx + " (other backend)")

    chk.compare(sa, "initial build")
    for y, u in HOT:
        chk.compare_params(sa, "initial build", y, u)
    chk.compare(sb, "initial build (other backend)")
    ed = "EdgeTag"
    base = lambda n, d=20230601, u="USD", v=3.25: num_insert(
        [(ADSH[0], ed, "v1", d, 0, u, "", v + i) for i in range(n)])
    # HAVING COUNT(*) > 10: 10 rows absent, 11 present, back to 10 absent.
    step(base(10), "edge: 10 rows")
    step(base(1), "edge: 11 rows")
    step("DELETE FROM num WHERE ctid = ANY(ARRAY(SELECT ctid FROM num WHERE tag='EdgeTag' LIMIT 1))",
         "edge: back to 10")
    step(base(1), "edge: 11 again")
    # Date range boundaries: in-range rows only count.
    for d in (20221231, 20230101, 20231231, 20240101):
        step(base(1, d=d), "edge: ddate %d" % d)
    step("UPDATE num SET ddate = 20231231 WHERE tag='EdgeTag' AND ddate = 20240101", "edge: ddate move in")
    step("UPDATE num SET ddate = 20230101 WHERE tag='EdgeTag' AND ddate = 20230101 AND random() < 2",
         "edge: ddate same")
    step("UPDATE num SET uom = 'usd' WHERE tag='EdgeTag' AND ddate = 20231231", "edge: uom case")
    step("UPDATE num SET value = NULL WHERE tag='EdgeTag' AND ddate = 20230101", "edge: value NULL")
    # Other (year, uom) buckets: HAVING edge per bucket, bucket moves, odd dates.
    for y, u in ((2024, "EUR"), (2019, "CNY"), (0, "USD"), (-1, "USD")):
        d = y * 10000 + 601
        step(num_insert([(ADSH[3], "ParamTag", "v1", d, 0, u, "", 2.0 + i) for i in range(10)]),
             "param: 10 rows %s/%d" % (u, y))
        chk.compare_params(sa, "param edge", y, u)
        step(num_insert([(ADSH[3], "ParamTag", "v1", d, 0, u, "", 9.0)]), "param: 11 rows %s/%d" % (u, y))
        chk.compare_params(sa, "param edge", y, u)
        chk.compare_params(sb, "param edge (other backend)", y, u)
    step("UPDATE num SET ddate = 20240100 WHERE tag='ParamTag' AND uom='EUR'", "param: ddate to no bucket")
    chk.compare_params(sa, "param edge", 2024, "EUR")
    step("UPDATE num SET ddate = 20240631 WHERE tag='ParamTag' AND uom='CNY'", "param: odd but in-window ddate")
    chk.compare_params(sa, "param edge", 2024, "CNY")
    step("UPDATE num SET uom = 'USD' WHERE tag='ParamTag' AND uom='CNY'", "param: uom move")
    chk.compare_params(sa, "param edge", 2019, "CNY")
    chk.compare_params(sa, "param edge", 2019, "USD")
    step(pre_insert([(ADSH[3], "ParamTag", "v1")]), "param: pre insert hides all buckets")
    for y, u in ((2024, "EUR"), (0, "USD"), (-1, "USD")):
        chk.compare_params(sa, "param edge", y, u)
    step("DELETE FROM pre WHERE tag='ParamTag'", "param: pre delete restores all buckets")
    for y, u in ((2024, "EUR"), (0, "USD"), (-1, "USD"), (2019, "USD")):
        chk.compare_params(sa, "param edge", y, u)
    step("DELETE FROM num WHERE tag='ParamTag'", "param cleanup")
    for y, u in ((None, "USD"), (2023, None), (None, None), (4 * 10 ** 18, "USD"),
                 (-(4 * 10 ** 18), "USD"), (2023, "nosuchuom"), (2025, "USD"), (2017, "USD")):
        chk.compare_params(sa, "param null/edge", y, u)
    # pre match removes the whole triple from the anti-join; deleting restores it.
    step(pre_insert([(ADSH[0], ed, "v1")]), "edge: pre insert matches")
    step("DELETE FROM pre WHERE tag='EdgeTag'", "edge: pre delete restores")
    step(pre_insert([(ADSH[0], ed, "v1"), (ADSH[0], ed, "v1")]), "edge: duplicate pre")
    step("DELETE FROM pre WHERE ctid = ANY(ARRAY(SELECT ctid FROM pre WHERE tag='EdgeTag' LIMIT 1))",
         "edge: one of two duplicate pre removed")
    step("UPDATE pre SET tag = 'EdgeTag2' WHERE tag='EdgeTag'", "edge: pre key update")
    step("DELETE FROM pre WHERE tag='EdgeTag2'", "edge: pre removed")
    # NULL keys never match a pre row.
    step(num_insert([(ADSH[1], None, "v1", 20230601, 0, "USD", "", 2.0)] * 12), "edge: NULL tag group")
    step(pre_insert([(ADSH[1], None, "v1")]), "edge: pre with NULL tag")
    # Quote / backslash / non-ASCII keys.
    step(num_insert([(ADSH[2], "O'Brien", "v'1", 20230601, 0, "USD", "", 1.0)] * 11), "edge: quote group")
    step(pre_insert([(ADSH[2], "O'Brien", "v'1")]), "edge: quote pre")
    step("DELETE FROM pre WHERE tag = 'O''Brien'", "edge: quote pre delete")
    # LIMIT 100: 130 groups with cnt 11..15, ties cut inside a class.
    rows = []
    for i in range(130):
        rows += [(ADSH[i % 12], "LimTag%03d" % i, "v1", 20230601, 0, "USD", "", 1.0 + j)
                 for j in range(11 + i % 5)]
    step(num_insert(rows), "limit: 130 groups")
    step("DELETE FROM num WHERE ctid = ANY(ARRAY(SELECT ctid FROM num WHERE tag='LimTag005' LIMIT 3))",
         "limit: group shrinks")
    step(pre_insert([(ADSH[0], "LimTag000", "v1")]) , "limit: pre hides a group")
    step(pre_insert([(ADSH[(i % 12)], "LimTag%03d" % i, "v1") for i in range(1, 40)]),
         "limit: pre hides 39 groups")
    step("DELETE FROM pre WHERE tag LIKE 'LimTag%'", "limit: pre delete restores")
    # Rolled back and committed explicit transactions.
    sa.run("BEGIN")
    sa.run(base(5))
    chk.compare(sa, "txn: dirty read")
    sa.run("ROLLBACK")
    chk.compare(sa, "txn: after rollback")
    chk.compare(sb, "txn: after rollback (other backend)")
    sa.run("BEGIN")
    sa.run(base(5))
    sa.run(pre_insert([(ADSH[0], ed, "v1")]))
    sa.run("DELETE FROM pre WHERE tag='EdgeTag'")
    chk.compare(sa, "txn: insert+delete pre in one txn (dirty)")
    sa.run("COMMIT")
    chk.compare(sa, "txn: committed")
    chk.compare(sb, "txn: committed (other backend)")
    # TRUNCATE-free reset of the edge rows.
    step("DELETE FROM num WHERE tag IN ('EdgeTag','LimTag000') OR tag LIKE 'LimTag%'", "edge cleanup")


def random_phase(sa, sb, chk, g, steps):
    for i in range(steps):
        kind = g.r.random()
        writer = sa if g.r.random() < 0.8 else sb
        reader_other = sb if writer is sa else sa
        if kind < 0.45:  # single statement
            label, sql = random_write(g)
            writer.run(sql)
            chk.compare(writer, "step %d %s" % (i, label))
        elif kind < 0.85:  # explicit transaction
            writer.run("BEGIN")
            labels = []
            sp = False
            for j in range(1 + g.r.randrange(4)):
                if g.r.random() < 0.2:
                    writer.run("SAVEPOINT s%d" % j)
                    label, sql = random_write(g)
                    writer.run(sql)
                    labels.append(label + "+sp")
                    if g.r.random() < 0.5:
                        writer.run("ROLLBACK TO s%d" % j)
                        labels.append("rollback_to")
                else:
                    label, sql = random_write(g)
                    writer.run(sql)
                    labels.append(label)
                if g.r.random() < 0.3:
                    chk.compare(writer, "step %d dirty %s" % (i, labels))
            end = "COMMIT" if g.r.random() < 0.6 else "ROLLBACK"
            writer.run(end)
            chk.compare(writer, "step %d %s %s" % (i, end, labels))
        else:  # the other backend writes, this one reads (sync protocol)
            label, sql = random_write(g)
            writer.run(sql)
            chk.compare(reader_other, "step %d cross-backend after %s" % (i, label))
            chk.compare(writer, "step %d writer after %s" % (i, label))
        if g.r.random() < 0.25:
            chk.compare(sb if writer is sa else sa, "step %d other backend" % i)
        if i % 50 == 49:
            print("  random: %d/%d steps ok (compares=%d max_rows=%d limit_hits=%d empty=%d)" %
                  (i + 1, steps, chk.compares, chk.max_rows, chk.limit_hits, chk.empty),
                  flush=True)


def existing_db_phase(sa, sb, chk, g):
    """Read check + real writes on a loaded database, undone afterwards.
    The plain query takes minutes at SF3, so each database state is
    evaluated by plain SQL once and the aux result is compared with it from
    both backends (and again after state-restoring rollbacks)."""
    def timed(label, fn):
        t0 = time.time()
        r = fn()
        print("  existing db: %s %.1fs" % (label, time.time() - t0), flush=True)
        return r

    p0 = timed("plain (initial)", lambda: sa.rows(PLAIN_SQL))
    timed("A first read (build) + compare", lambda: chk.compare(sa, "existing db: build + read", p0))
    timed("A second read + compare", lambda: chk.compare(sa, "existing db: second read", p0))
    timed("B read (own build) + compare", lambda: chk.compare(sb, "existing db: other backend read", p0))
    # A rolled-back transaction: its own dirty state vs plain, then back to p0.
    sa.run("BEGIN")
    sa.run("UPDATE num SET value = value + 1 WHERE ctid = ANY(ARRAY(SELECT ctid FROM num "
           "WHERE uom='USD' AND ddate BETWEEN 20230101 AND 20231231 LIMIT 50))")
    sa.run("DELETE FROM pre WHERE ctid = ANY(ARRAY(SELECT ctid FROM pre LIMIT 20 OFFSET 5000))")
    timed("dirty txn compare", lambda: chk.compare(sa, "existing db: dirty txn"))
    sa.run("ROLLBACK")
    chk.compare(sa, "existing db: after rollback", p0)
    chk.compare(sb, "existing db: B after rollback", p0)
    # Committed value update on result rows (maintained incrementally).
    top = p0[0] if p0 else None
    where = ("tag = %s AND version = %s" % (lit(top[0]), lit(top[1]))) if top else "TRUE"
    sa.run("UPDATE num SET value = value + 0.5 WHERE ctid = ANY(ARRAY(SELECT ctid FROM num "
           "WHERE uom='USD' AND ddate BETWEEN 20230101 AND 20231231 AND %s LIMIT 5))" % where)
    p1 = timed("plain after value update", lambda: sa.rows(PLAIN_SQL))
    chk.compare(sa, "existing db: committed value update", p1)
    chk.compare(sb, "existing db: B after value update", p1)
    # Committed pre delete of the top group's matched triples (turns matched
    # num rows into orphans: the targeted triple lookups), then restore.
    sa.run("CREATE TEMP TABLE pre_bak AS SELECT * FROM pre WHERE %s" % where)
    n = sa.rows("SELECT count(*) FROM pre_bak")[0][0]
    sa.run("DELETE FROM pre WHERE %s" % where)
    p2 = timed("plain after pre delete (%s rows)" % n, lambda: sa.rows(PLAIN_SQL))
    chk.compare(sa, "existing db: committed pre delete", p2)
    chk.compare(sb, "existing db: B after pre delete", p2)
    sa.run("INSERT INTO pre SELECT * FROM pre_bak")
    chk.compare(sa, "existing db: pre restored", p1)
    # (The +0.5 on five top-group rows is left in place: no row identity to undo it.)


def sweep_phase(sa, chk, g, n):
    """Read-only parameter sweep on a loaded (SF3) database. The accelerator
    answers every (year, uom) from one state; the plain query (about 100 s at
    SF3) is evaluated on parallel backends for the sampled pairs."""
    import threading
    combos = [(2023, "USD"), (2024, "USD"), (2022, "USD"), (2023, "shares"),
              (2019, "pure"), (2021, "CAD"), (2020, "CNY"), (2024, "EUR"),
              (2025, "USD"), (None, "USD"), (2023, None), (2023, "nosuchuom")]
    rnd = random.Random(7)
    pool = [(y, u) for y in YEARS for u in UOMS if (y, u) not in combos]
    rnd.shuffle(pool)
    combos = (combos + pool)[:n]
    t0 = time.time()
    reads = {}
    for y, u in combos:  # first call builds the state; the rest are reads
        t1 = time.time()
        reads[(y, u)] = sa.rows(accel_sql(y, u))
        print("  sweep: accel (%s, %s) -> %d rows in %.1f ms" %
              (y, u, len(reads[(y, u)]), (time.time() - t1) * 1000), flush=True)
    refs = {}
    lock = threading.Lock()
    work = list(combos)

    def worker(idx):
        s = Session(os.environ.get("Q24_SWEEP_DB", sa.db), "plain%d" % idx)
        try:
            s.run("SET max_parallel_workers_per_gather = 2")
            while True:
                with lock:
                    if not work:
                        return
                    y, u = work.pop()
                t1 = time.time()
                rows = s.rows(plain_sql(y, u))
                with lock:
                    refs[(y, u)] = rows
                print("  sweep: plain (%s, %s) %.1fs" % (y, u, time.time() - t1), flush=True)
        finally:
            s.close()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(4)]
    for th in threads:
        th.start()
    for th in threads:
        th.join()
    bad = 0
    for (y, u) in combos:
        got, exp = reads[(y, u)], refs[(y, u)]
        ok = len(got) == len(exp) and all(
            len(a) == 4 and a[:3] == b[:3] and close_total(a[3], b[3])
            for a, b in zip(got, exp))
        # re-read after the plain query: same answer
        chk.compares += 1
        chk.param_compares += 1
        chk.param_nonempty += len(exp) > 0
        if not ok:
            bad += 1
            print("  SWEEP MISMATCH (%s, %s): accel %d rows, plain %d rows\n  %s\n  %s" %
                  (y, u, len(got), len(exp), got[:4], exp[:4]))
    if bad:
        raise Fail("%d sweep mismatches" % bad)
    print("sweep ok: %d combinations in %.0fs" % (len(combos), time.time() - t0))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--existing-db")
    ap.add_argument("--steps", type=int, default=int(os.environ.get("Q24_STEPS", "300")))
    ap.add_argument("--seed", type=int, default=int(os.environ.get("Q24_SEED", "20240101")))
    ap.add_argument("--create-only", action="store_true")
    ap.add_argument("--sweep", type=int, default=0,
                    help="with --existing-db: compare N sampled (year, uom) "
                         "parameter pairs against plain SQL (read-only)")
    args = ap.parse_args()
    g = Gen(args.seed)
    chk = Checker()
    chk.gen = None if args.existing_db else g
    db = args.existing_db or os.environ.get("Q24_PG_DB", "q24_diff")
    admin = Session("postgres", "admin")
    sessions = []
    try:
        if not args.existing_db:
            print("creating %s (seed %d)" % (db, args.seed))
            s = load_schema_and_data(admin, db, g)
            install_module(s)
            s.close()
            if args.create_only:
                return 0
        sa, sb = Session(db, "A"), Session(db, "B")
        sessions += [sa, sb]
        if args.existing_db and args.sweep:
            sweep_phase(sa, chk, g, args.sweep)
        elif args.existing_db:
            existing_db_phase(sa, sb, chk, g)
        else:
            edges_phase(sa, sb, chk, g)
            print("edges phase ok (compares=%d)" % chk.compares, flush=True)
            random_phase(sa, sb, chk, g, args.steps)
        counts = sa.run("SELECT accel_postgres_get_execute_counts()")
        print("execute counts [A]: %s" % counts)
        print("PASS: %d compares (%d parameterised, %d non-empty), max result rows %d, "
              "%d at LIMIT, %d empty" %
              (chk.compares, chk.param_compares, chk.param_nonempty, chk.max_rows,
               chk.limit_hits, chk.empty))
        return 0
    except Fail as e:
        print("FAIL:", e)
        return 1
    finally:
        for s in sessions:
            s.close()
        admin.close()


if __name__ == "__main__":
    sys.exit(main())
