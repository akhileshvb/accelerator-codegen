#!/usr/bin/env python3
"""PostgreSQL SEC-EDGAR Q3 differential: accel_sec_edgar_q3_aux() vs plain SQL.

Every comparison checks the defaults accel_sec_edgar_q3_aux() against q3.sql
and a few random accel_sec_edgar_q3_aux(fy, uom) drawn from the domain
fy in {2021..2024}, uom in {USD, shares, pure, CAD, CNY, EUR} plus edge values
(2019, 2025, 0, NULL arguments, unknown / case-variant / empty uom) against
q3.sql with :fy / :uom substituted.

Modes
  random : randomized I/D/U on num and sub (cohort fy moves, adsh moves, NULLs,
           multi-statement transactions, rollbacks, savepoints) on a private
           schema copy (q3diff) of a sample of the data; the accelerator is
           compared with plain SQL after every step, from the writing backend
           and from a second backend.
  full   : full-data read check on the public tables (no writes).

Env: PGHOST PGPORT PGUSER PGPASSWORD PGDATABASE, Q3_SEED, Q3_STEPS, Q3_SAMPLE.
"""
import math
import os
import random
import sys

import psycopg2

_Q3 = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                        "../../benchmarks/sec_edgar/queries/q3.sql")).read().strip().rstrip(";")
for _lit, _par in (("s.fy = 2022", "s.fy = %(fy)s"), ("s2.fy = 2022", "s2.fy = %(fy)s"),
                   ("n.uom = 'USD'", "n.uom = %(uom)s"), ("n2.uom = 'USD'", "n2.uom = %(uom)s")):
    assert _Q3.count(_lit) == 1, _lit
    _Q3 = _Q3.replace(_lit, _par)
REF_SQL = _Q3                      # parameters %(fy)s, %(uom)s
DEFAULT_REF = {"fy": 2022, "uom": "USD"}
ACC_SQL = "SELECT * FROM accel_sec_edgar_q3_aux()"
ACC_PARAM_SQL = "SELECT * FROM accel_sec_edgar_q3_aux(%(fy)s::bigint, %(uom)s::text)"
REL = 1e-9

FYS = [2021, 2022, 2022, 2023, 2024, 2019, 2025, 0, -1, 2147483647, None]
UOMS = ["USD", "USD", "shares", "pure", "CAD", "CNY", "EUR", "nonexistent", "usd", "", "USD ", None]


def connect(search_path=None):
    c = psycopg2.connect(host=os.environ.get("PGHOST", "127.0.0.1"),
                         port=int(os.environ.get("PGPORT", "55403")),
                         user=os.environ.get("PGUSER", "postgres"),
                         password=os.environ.get("PGPASSWORD", "postgres"),
                         dbname=os.environ.get("PGDATABASE", "sec_edgar"))
    c.autocommit = True
    if search_path:
        with c.cursor() as cur:
            cur.execute("SET search_path = %s" % search_path)
    return c


def fetch(conn, sql, params=None):
    with conn.cursor() as cur:
        cur.execute(sql, params)
        return cur.fetchall()


def close(a, b):
    if a == b:
        return True
    if a is None or b is None:
        return False
    if math.isnan(a) and math.isnan(b):
        return True
    return abs(a - b) <= REL * max(abs(a), abs(b), 1.0) + 1e-6


def compare(conn, label, params=None):
    """params None: defaults (no-argument overload); else {'fy':..., 'uom':...}."""
    if params is None:
        ref = fetch(conn, REF_SQL, DEFAULT_REF)
        acc = fetch(conn, ACC_SQL)
    else:
        ref = fetch(conn, REF_SQL, params)
        acc = fetch(conn, ACC_PARAM_SQL, params)
        label = "%s fy=%r uom=%r" % (label, params["fy"], params["uom"])
    ok = len(ref) == len(acc)
    msg = ""
    if not ok:
        msg = "row count %d vs %d" % (len(ref), len(acc))
    else:
        # boundary of the LIMIT / HAVING may flip on sums equal up to rounding
        edge = ref[-1][2] if ref else None
        rk = {}
        for n, c, t in ref:
            rk[(n, c)] = t
        for i, ((rn, rc, rt), (an, ac, at)) in enumerate(zip(ref, acc)):
            if not close(rt, at):
                ok, msg = False, "row %d total %r vs %r" % (i, rt, at)
                break
            if (rn, rc) != (an, ac):
                tie = (i > 0 and close(ref[i - 1][2], rt)) or \
                      (i + 1 < len(ref) and close(ref[i + 1][2], rt)) or \
                      (edge is not None and close(edge, rt))
                if not tie or (an, ac) not in rk and not (close(edge, at) if edge else False):
                    ok, msg = False, "row %d key %r vs %r" % (i, (rn, rc), (an, ac))
                    break
    if not ok:
        print("MISMATCH [%s]: %s" % (label, msg))
        print(" ref:", ref[:5])
        print(" acc:", acc[:5])
        sys.exit(1)
    return len(ref)


def compare_all(conn, rnd, label, draws=2):
    n = compare(conn, label)
    for _ in range(draws):
        n += compare(conn, label, {"fy": rnd.choice(FYS), "uom": rnd.choice(UOMS)})
    return n


def full():
    conn = connect("public")
    n = compare(conn, "full#1")
    n2 = compare(conn, "full#2")
    for uom in ("USD", "shares", "pure", "CAD", "CNY", "EUR", "nonexistent"):
        for fy in (2021, 2022, 2023, 2024, 2019, 2025, 0, None):
            compare(conn, "full", {"fy": fy, "uom": uom})
    print("PASS full-data differential (%d rows)" % n)
    return n


class Rand:
    def __init__(self, conn, rnd, filings):
        self.conn, self.rnd, self.filings = conn, rnd, filings
        self.next_id = 0

    def exec(self, sql, args=None):
        with self.conn.cursor() as cur:
            cur.execute(sql, args)
            if os.environ.get("Q3_TRACE"):
                print("  SQL", cur.query.decode()[:200], "rows", cur.rowcount)
        if os.environ.get("Q3_FINE") and self.conn.autocommit:
            compare(self.conn, "after: " + sql[:80] + str(args)[:80])

    def adsh(self):
        return self.rnd.choice(self.filings)

    def new_adsh(self):
        self.next_id += 1
        if self.rnd.random() < 0.15:
            return "odd-adsh-%d" % self.next_id  # non-conforming key (overflow dict)
        return "9%09d-22-%06d" % (self.next_id, self.next_id)

    def value(self):
        r = self.rnd.random()
        if r < 0.1:
            return None
        if r < 0.15:
            return 0.0
        return round(self.rnd.uniform(-1e6, 1e9), self.rnd.choice([0, 2, 6]))

    def uom(self):
        return self.rnd.choice(["USD", "USD", "USD", "EUR", "shares", "pure", "CAD", "CNY", None, "usd"])

    def op(self):
        k = self.rnd.randrange(14)
        a = self.adsh()
        if k == 0:
            for _ in range(self.rnd.randint(1, 5)):
                self.exec("INSERT INTO num(adsh,tag,version,ddate,qtrs,uom,coreg,value)"
                          " VALUES (%s,'t','v',20220101,4,%s,'',%s)", (a, self.uom(), self.value()))
        elif k == 1:
            self.exec("INSERT INTO num SELECT * FROM num WHERE adsh=%s LIMIT %s", (a, self.rnd.randint(1, 20)))
        elif k == 2:
            self.exec("DELETE FROM num WHERE ctid IN (SELECT ctid FROM num WHERE adsh=%s LIMIT %s)",
                      (a, self.rnd.randint(1, 10)))
        elif k == 3:
            self.exec("UPDATE num SET value=%s WHERE ctid IN (SELECT ctid FROM num WHERE adsh=%s LIMIT %s)",
                      (self.value(), a, self.rnd.randint(1, 10)))
        elif k == 4:
            self.exec("UPDATE num SET uom=%s WHERE ctid IN (SELECT ctid FROM num WHERE adsh=%s LIMIT %s)",
                      (self.uom(), a, self.rnd.randint(1, 10)))
        elif k == 5:  # num row moves to another filing
            self.exec("UPDATE num SET adsh=%s WHERE ctid IN (SELECT ctid FROM num WHERE adsh=%s LIMIT %s)",
                      (self.adsh(), a, self.rnd.randint(1, 5)))
        elif k == 6:  # unrelated column: must be a no-op
            self.exec("UPDATE num SET footnote='x' WHERE ctid IN (SELECT ctid FROM num WHERE adsh=%s LIMIT 3)", (a,))
        elif k == 7:  # cohort move
            self.exec("UPDATE sub SET fy=%s WHERE adsh=%s",
                      (self.rnd.choice([2022, 2022, 2021, 2023, 2024, None]), a))
        elif k == 8:
            self.exec("UPDATE sub SET cik=%s WHERE adsh=%s",
                      (self.rnd.choice([None, 1, 2, 3, self.rnd.randint(1, 10**6)]), a))
        elif k == 9:
            self.exec("UPDATE sub SET name=%s WHERE adsh=%s",
                      (self.rnd.choice([None, "ACME", "Beta Corp", "name%d" % self.rnd.randint(0, 50)]), a))
        elif k == 10:  # new filing, possibly with num rows
            n = self.new_adsh()
            self.exec("INSERT INTO sub(adsh,cik,name,fy) VALUES (%s,%s,%s,%s)",
                      (n, self.rnd.choice([None, 1, 2, 5, 77]), self.rnd.choice([None, "ACME", "NEWCO"]),
                       self.rnd.choice([2022, 2022, 2021, 2023, 2024, None])))
            for _ in range(self.rnd.randint(0, 6)):
                self.exec("INSERT INTO num(adsh,tag,version,ddate,qtrs,uom,coreg,value)"
                          " VALUES (%s,'t','v',20220101,4,%s,'',%s)", (n, self.rnd.choice(["USD", "USD", "EUR", "shares", "pure"]), self.value()))
            self.filings.append(n)
        elif k == 11:  # delete a filing with its facts
            if len(self.filings) > 20:
                self.exec("DELETE FROM num WHERE adsh=%s", (a,))
                self.exec("DELETE FROM sub WHERE adsh=%s", (a,))
                self.filings.remove(a)
        elif k == 12:  # sub key change (num follows)
            n = self.new_adsh()
            self.exec("INSERT INTO sub SELECT %s, cik, name, sic, countryba, stprba, cityba, countryinc, form,"
                      " period, fy, fp, filed, accepted, prevrpt, nciks, afs, wksi, fye, instance FROM sub"
                      " WHERE adsh=%s", (n, a))
            self.exec("UPDATE num SET adsh=%s WHERE adsh=%s", (n, a))
            self.exec("DELETE FROM sub WHERE adsh=%s", (a,))
            self.filings.remove(a)
            self.filings.append(n)
        else:  # duplicate cohort name/cik across filings
            b = self.adsh()
            self.exec("UPDATE sub SET (name,cik,fy) = (SELECT name,cik,fy FROM sub WHERE adsh=%s) WHERE adsh=%s", (b, a))


def random_mode():
    seed = int(os.environ.get("Q3_SEED", "1"))
    steps = int(os.environ.get("Q3_STEPS", "300"))
    sample = int(os.environ.get("Q3_SAMPLE", "400"))
    rnd = random.Random(seed)
    setup = connect("public")
    with setup.cursor() as cur:
        cur.execute("DROP SCHEMA IF EXISTS q3diff CASCADE")
        cur.execute("CREATE SCHEMA q3diff")
        cur.execute("CREATE TEMP TABLE pick AS SELECT adsh FROM public.sub WHERE fy=2022 "
                    "ORDER BY adsh LIMIT %s", (sample,))
        cur.execute("INSERT INTO pick SELECT adsh FROM public.sub WHERE fy IS DISTINCT FROM 2022 "
                    "ORDER BY adsh LIMIT %s", (sample // 2,))
        cur.execute("CREATE TABLE q3diff.sub AS SELECT s.* FROM public.sub s JOIN pick USING (adsh)")
        cur.execute("CREATE TABLE q3diff.num AS SELECT n.* FROM public.num n JOIN pick USING (adsh)")
        cur.execute("CREATE INDEX ON q3diff.num(adsh)")
        cur.execute("CREATE INDEX ON q3diff.sub(adsh)")
        cur.execute("SET search_path = q3diff, public")
    install_triggers(setup)
    writer = connect("q3diff, public")
    reader = connect("q3diff, public")
    filings = [r[0] for r in fetch(writer, "SELECT adsh FROM sub")]
    R = Rand(writer, rnd, filings)
    compare_all(writer, rnd, "initial/writer", 6)
    compare_all(reader, rnd, "initial/reader", 6)
    stats = dict(commit=0, rollback=0, savepoint=0)
    for step in range(steps):
        mode = rnd.random()
        label = "seed=%d step=%d" % (seed, step)
        if os.environ.get("Q3_TRACE"):
            print("STEP", step, "txn" if mode >= 0.55 else "auto")
        if mode < 0.55:  # autocommit statement(s)
            for _ in range(rnd.randint(1, 3)):
                R.op()
            stats["commit"] += 1
        else:
            writer.autocommit = False
            try:
                for _ in range(rnd.randint(1, 5)):
                    R.op()
                    if os.environ.get("Q3_SPLIT"):
                        writer.commit(); writer.autocommit = True
                        compare_all(writer, rnd, label + " split"); writer.autocommit = False
                    if rnd.random() < 0.3:
                        compare_all(writer, rnd, label + " in-txn", 1)  # own uncommitted writes
                if rnd.random() < 0.2:
                    with writer.cursor() as cur:
                        cur.execute("SAVEPOINT sp")
                        if os.environ.get("Q3_TRACE"): print("  SAVEPOINT")
                    for _ in range(rnd.randint(1, 3)):
                        R.op()
                    if rnd.random() < 0.6:
                        with writer.cursor() as cur:
                            cur.execute("ROLLBACK TO SAVEPOINT sp")
                            if os.environ.get("Q3_TRACE"): print("  ROLLBACK TO SP")
                        stats["savepoint"] += 1
                if rnd.random() < 0.3:
                    if os.environ.get("Q3_TRACE"): print("  ROLLBACK")
                    writer.rollback()
                    stats["rollback"] += 1
                    # the generator's filing list may have drifted; resync
                    R.filings[:] = [r[0] for r in fetch_txn(writer)]
                else:
                    if os.environ.get("Q3_TRACE"): print("  COMMIT")
                    writer.commit()
                    stats["commit"] += 1
            except Exception:
                writer.rollback()
                raise
            finally:
                writer.autocommit = True
        if rnd.random() < 0.5:
            R.filings[:] = [r[0] for r in fetch(writer, "SELECT adsh FROM sub")]
        compare_all(writer, rnd, label + " writer")
        if step % 3 == 0:
            compare_all(reader, rnd, label + " reader")
    print("PASS randomized differential seed=%d steps=%d %s" % (seed, steps, stats))
    with setup.cursor() as cur:
        cur.execute("DROP SCHEMA q3diff CASCADE")


def fetch_txn(conn):
    conn.autocommit = True
    try:
        return fetch(conn, "SELECT adsh FROM sub")
    finally:
        pass


def install_triggers(conn):
    # create_accel_mutation_triggers() takes an unqualified relation name; the
    # session search_path (q3diff first) resolves it to the schema copy.
    with conn.cursor() as cur:
        cur.execute("SET search_path = q3diff, public")
        cur.execute("SELECT create_accel_mutation_triggers('num', '2', 'accel_codegen_sec_edgar_t')")
        cur.execute("SELECT create_accel_mutation_triggers('sub', '1', 'accel_codegen_sec_edgar_t')")


if __name__ == "__main__":
    mode = sys.argv[1] if len(sys.argv) > 1 else "random"
    {"random": random_mode, "full": full}[mode]()
