#!/usr/bin/env python3
"""PostgreSQL SEC-EDGAR Q3 benchmark: accel read p50 vs plain SQL, build time,
write overhead with capture vs plain write.  Env: PGHOST PGPORT PGUSER ...;
ACCEL_PARAM_SEED, ACCEL_PARAM_SETS, ACCEL_PARAM_FILE (eval/harness/param_sets.py): the parameter
sets the plain query and the accelerator are read with; the write overhead is measured on the
first set with an answer."""
import os
import statistics
import sys
import time

import psycopg2

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "..", "eval", "harness"))
import param_sets  # noqa: E402

TEMPLATE = open(os.path.join(HERE, "../../../../workloads/sec_edgar/q3.sql")).read().strip().rstrip(";")


def conn():
    c = psycopg2.connect(host=os.environ.get("PGHOST", "127.0.0.1"), port=int(os.environ.get("PGPORT", "55403")),
                         user=os.environ.get("PGUSER", "postgres"), password=os.environ.get("PGPASSWORD", "postgres"),
                         dbname=os.environ.get("PGDATABASE", "sec_edgar"))
    c.autocommit = True
    return c


def timed(c, sql, args=None):
    cur = c.cursor()
    t = time.perf_counter()
    cur.execute(sql, args)
    if cur.description:
        cur.fetchall()
    return (time.perf_counter() - t) * 1e3


def p(xs, q):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(q * len(xs)))]


def main():
    reps = int(os.environ.get("REPS", "15"))
    sets = param_sets.draw_sets("sec_edgar", 3)
    c = conn()
    out = {}
    c.cursor().execute("PREPARE q3_plain AS " + TEMPLATE)
    answered = None
    for k, drawn in enumerate(sets):
        lits = drawn["cpp"]
        ref = "EXECUTE q3_plain(%s)" % lits
        acc = "SELECT * FROM accel_sec_edgar_q3_aux(%s)" % lits
        timed(c, ref)
        plain = [timed(c, ref) for _ in range(reps)]
        # build: first accel call in a fresh backend
        c2 = conn()
        build = timed(c2, acc)
        hits = [timed(c2, acc) for _ in range(200)]
        tag = "set %d (%s): " % (k, lits)
        out[tag + "plain_p50_ms"] = statistics.median(plain)
        out[tag + "build_ms"] = build
        out[tag + "accel_p50_ms"] = statistics.median(hits)
        out[tag + "accel_p99_ms"] = p(hits, 0.99)
        cur = c.cursor()
        cur.execute(ref)
        if answered is None and cur.fetchall():
            answered = drawn
    assert answered is not None, "none of the drawn parameter sets has an answer"
    cell = answered["params"]
    ACC = "SELECT * FROM accel_sec_edgar_q3_aux(%s)" % answered["cpp"]
    c2 = conn()
    timed(c2, ACC)
    # write overhead on the live state: single-row num updates / sub updates
    cur = c2.cursor()
    cur.execute("SELECT adsh FROM sub WHERE fy=%s ORDER BY adsh LIMIT 1", (cell["fy"],))
    adsh = cur.fetchone()[0]
    cur.execute("SELECT count(*) FROM num WHERE adsh=%s AND uom=%s AND value IS NOT NULL", (adsh, cell["uom"]))
    out["filing_cohort_rows"] = cur.fetchone()[0]
    upd_num = "UPDATE num SET value = value + %s WHERE ctid = (SELECT ctid FROM num WHERE adsh=%s AND uom=%s AND value IS NOT NULL LIMIT 1)"
    upd_sub = "UPDATE sub SET cik = cik WHERE adsh=%s"

    def run(label, sql, mk):
        xs = []
        for i in range(60):
            xs.append(timed(c2, sql, mk(i)))
        out[label] = statistics.median(xs)

    for state in ("capture", "plain"):
        for t in ("num", "sub"):
            c2.cursor().execute("ALTER TABLE %s %s TRIGGER USER" % (t, "ENABLE" if state == "capture" else "DISABLE"))
        run("write_num_update_%s_ms" % state, upd_num, lambda i: (1.0 if i % 2 == 0 else -1.0, adsh, cell["uom"]))
        run("write_sub_update_%s_ms" % state, upd_sub, lambda i: (adsh,))
    for t in ("num", "sub"):
        c2.cursor().execute("ALTER TABLE %s ENABLE TRIGGER USER" % t)
    # first read after a write (incremental maintenance on commit => ready)
    timed(c2, upd_num, (1.0, adsh, cell["uom"]))
    out["read_after_write_ms"] = timed(c2, ACC)
    timed(c2, upd_num, (-1.0, adsh, cell["uom"]))
    for k, v in out.items():
        print("%-28s %s" % (k, ("%.3f" % v) if isinstance(v, float) else v))


if __name__ == "__main__":
    main()
