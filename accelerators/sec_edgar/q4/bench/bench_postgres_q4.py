#!/usr/bin/env python3
"""SEC-EDGAR Q4 on PostgreSQL: accel read vs plain SQL, build time, write overhead.

Env: PGHOST PGPORT PGUSER PGPASSWORD PGDATABASE (the database with the SF data,
the accelerator_codegen_pg extension and the SEC-EDGAR capture triggers); ACCEL_PARAM_SEED,
ACCEL_PARAM_SETS, ACCEL_PARAM_FILE (eval/harness/param_sets.py): the parameter sets read from the
plain query and the accelerator; the write overhead is measured on the first set with an answer.

  bench_postgres_q4.py [--reads 200] [--plain-runs 5] [--writes 40]

Write overhead compares a write with capture + incremental maintenance (the
default) against the same write with the capture triggers off
(session_replication_role = replica): the plain write.  The state is built and
synced in the writing connection before every measured series.
"""
import argparse
import os
import statistics
import sys
import time

import psycopg2

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "..", "eval", "harness"))
import param_sets  # noqa: E402

TEMPLATE = open(os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "workloads", "sec_edgar", "q4.sql")).read().rstrip().rstrip(";")


def connect():
    c = psycopg2.connect(
        host=os.environ.get("PGHOST", "127.0.0.1"), port=os.environ.get("PGPORT", "5432"),
        user=os.environ.get("PGUSER", "postgres"), password=os.environ.get("PGPASSWORD", "postgres"),
        dbname=os.environ.get("PGDATABASE", "postgres"))
    c.autocommit = True
    return c


def timed(cur, sql, params=None):
    t = time.perf_counter()
    cur.execute(sql, params)
    rows = cur.fetchall() if cur.description else None
    return (time.perf_counter() - t) * 1000.0, rows


def p50(xs):
    return statistics.median(xs)


def series(conn, fn, n):
    out = []
    for i in range(n):
        t = time.perf_counter()
        fn(i)
        out.append((time.perf_counter() - t) * 1000.0)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--reads", type=int, default=200)
    ap.add_argument("--plain-runs", type=int, default=5)
    ap.add_argument("--writes", type=int, default=40)
    ap.add_argument("--builds", type=int, default=3)
    a = ap.parse_args()

    res = {}
    sets = param_sets.draw_sets("sec_edgar", 4)
    conn = connect()
    cur = conn.cursor()
    cur.execute("SELECT count(*) FROM num")
    print("num rows:", cur.fetchone()[0])
    cur.execute("PREPARE q4_plain AS " + TEMPLATE)

    answered = None  # the first set with an answer: the write target's cell
    for k, drawn in enumerate(sets):
        lits = drawn["cpp"]
        plain_sql = "EXECUTE q4_plain(%s)" % lits
        accel_sql = "SELECT * FROM accel_sec_edgar_q4_aux(%s)" % lits
        plain = []
        for _ in range(a.plain_runs):
            ms, ref_rows = timed(cur, plain_sql)
            plain.append(ms)
        # build time: first accel call of a fresh backend
        builds = []
        for _ in range(a.builds):
            c = connect()
            cu = c.cursor()
            ms, rows = timed(cu, accel_sql)
            builds.append(ms)
            assert len(rows) == len(ref_rows), (lits, len(rows), len(ref_rows))
            c.close()
        ms, rows = timed(cur, accel_sql)
        reads = [timed(cur, accel_sql)[0] for _ in range(a.reads)]
        tag = "set %d (%s) rows=%d: " % (k, lits, len(ref_rows))
        res[tag + "plain read p50 (ms)"] = p50(plain)
        res[tag + "build (first call) p50 (ms)"] = p50(builds)
        res[tag + "accel read p50 (ms)"] = p50(reads)
        res[tag + "accel read p99 (ms)"] = sorted(reads)[int(len(reads) * 0.99) - 1]
        res[tag + "speedup (plain p50 / accel p50)"] = p50(plain) / p50(reads)
        if answered is None and ref_rows:
            answered = drawn
    assert answered is not None, "none of the drawn parameter sets has an answer"
    cell = answered["params"]
    ACCEL = "SELECT * FROM accel_sec_edgar_q4_aux(%s)" % answered["cpp"]
    cur.execute("EXECUTE q4_plain(%s)" % answered["cpp"])
    ref_rows = cur.fetchall()

    # ---- write overhead
    cur.execute("SELECT s.adsh FROM sub s JOIN pre p ON p.adsh=s.adsh AND p.stmt=%s "
                "WHERE s.sic BETWEEN %s AND %s GROUP BY s.adsh ORDER BY s.adsh OFFSET 0 LIMIT 1",
                (cell["stmt"], cell["sic_lo"], cell["sic_hi"]))
    adsh = cur.fetchone()[0]
    cur.execute("SELECT n.ctid::text FROM num n JOIN pre p ON p.adsh=n.adsh AND p.tag=n.tag AND "
                "p.version=n.version AND p.stmt=%s WHERE n.adsh=%s AND n.uom=%s AND n.value IS NOT NULL LIMIT 1",
                (cell["stmt"], adsh, cell["uom"]))
    num_key = cur.fetchone()[0]
    cur.execute("SELECT p.tag, p.version FROM pre p JOIN tag t ON t.tag=p.tag AND t.version=p.version AND t.abstract=0 "
                "WHERE p.adsh=%s AND p.stmt=%s LIMIT 1", (adsh, cell["stmt"]))
    tag, version = cur.fetchone()
    cur.execute("SELECT count(*) FROM num WHERE adsh=%s", (adsh,))
    print("write target:", adsh, "num rows of filing:", cur.fetchone()[0])

    writes = {
        "num UPDATE value (1 row)": lambda w, i: w.execute(
            "UPDATE num SET value = value + %s WHERE ctid = (SELECT ctid FROM num WHERE adsh=%s AND tag=%s "
            "AND version=%s AND uom=%s AND value IS NOT NULL LIMIT 1)", (1 if i % 2 == 0 else -1, adsh, tag, version, cell["uom"])),
        "num INSERT (1 row)": lambda w, i: w.execute(
            "INSERT INTO num (adsh,tag,version,ddate,qtrs,uom,coreg,value) VALUES (%s,%s,%s,20220101,4,%s,'',%s)",
            (adsh, tag, version, cell["uom"], 1.0 + i)),
        "sub UPDATE cik (structural, 1 filing)": lambda w, i: w.execute(
            "UPDATE sub SET cik = cik + %s WHERE adsh=%s", (1000000 if i % 2 == 0 else -1000000, adsh)),
        "tag UPDATE tlabel (structural, 1 tag)": lambda w, i: w.execute(
            "UPDATE tag SET tlabel = tlabel || %s WHERE tag=%s AND version=%s", ("x", tag, version)),
        "pre UPDATE stmt toggle (structural, 1 key)": lambda w, i: w.execute(
            "UPDATE pre SET stmt = %s WHERE ctid = (SELECT ctid FROM pre WHERE adsh=%s AND tag=%s AND version=%s "
            "AND stmt IN (%s,'XX') LIMIT 1)", ("XX" if i % 2 == 0 else cell["stmt"], adsh, tag, version, cell["stmt"])),
    }
    cleanup = {
        "num INSERT (1 row)": ("DELETE FROM num WHERE adsh=%s AND tag=%s AND version=%s AND ddate=20220101 AND coreg='' "
                               "AND value >= 1.0 AND value < 100000", (adsh, tag, version)),
        "tag UPDATE tlabel (structural, 1 tag)": ("UPDATE tag SET tlabel = regexp_replace(tlabel, 'x+$', '') "
                                                  "WHERE tag=%s AND version=%s", (tag, version)),
    }
    wr = connect()
    wr.autocommit = False
    wc = wr.cursor()
    for name, fn in writes.items():
        out = {}
        for mode in ("plain", "accel"):
            # plain: capture triggers off for this transaction series; accel: captured + maintained.
            wc.execute("SET session_replication_role = %s" % ("replica" if mode == "plain" else "origin"))
            wr.commit()
            if mode == "accel":
                wc.execute(ACCEL)
                wc.fetchall()
                wr.commit()

            def one(i, fn=fn):
                fn(wc, i)
                wr.commit()
            out[mode] = p50(series(wr, one, a.writes))
            if name in cleanup:
                wc.execute("SET session_replication_role = replica")
                wc.execute(*cleanup[name])
                wr.commit()
                wc.execute("SET session_replication_role = origin")
                wr.commit()
        res["write: " + name] = "plain %.3f ms | with accel %.3f ms | +%.3f ms" % (
            out["plain"], out["accel"], out["accel"] - out["plain"])
        # state must still equal the plain query after the series (read after writes)
        wc.execute("SET session_replication_role = origin")
        wr.commit()
        wc.execute(ACCEL)
        got = wc.fetchall()
        wr.commit()
        assert len(got) == len(ref_rows)
    # first read after a write (incremental state is served, no rescan)
    def write_then_read(i):
        wc.execute("UPDATE num SET value = value + %s WHERE ctid = (SELECT ctid FROM num WHERE adsh=%s AND tag=%s "
                   "AND version=%s AND uom=%s AND value IS NOT NULL LIMIT 1)",
                   (1 if i % 2 == 0 else -1, adsh, tag, version, cell["uom"]))
        wr.commit()
        t = time.perf_counter()
        wc.execute(ACCEL)
        wc.fetchall()
        wr.commit()
        return (time.perf_counter() - t) * 1000.0
    after = [write_then_read(i) for i in range(a.writes)]
    res["accel read right after a num write p50 (ms)"] = p50(after)

    print()
    for k, v in res.items():
        print("%-48s %s" % (k, ("%.3f" % v) if isinstance(v, float) else v))


if __name__ == "__main__":
    main()
