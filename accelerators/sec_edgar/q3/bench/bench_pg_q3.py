#!/usr/bin/env python3
"""PostgreSQL SEC-EDGAR Q3 benchmark: accel read p50 vs plain SQL, build time,
write overhead with capture vs plain write.  Env: PGHOST PGPORT PGUSER ..."""
import os
import statistics
import sys
import time

import psycopg2

HERE = os.path.dirname(os.path.abspath(__file__))
REF = open(os.path.join(HERE, "../../../../workloads/sec_edgar/q3.sql")).read().strip().rstrip(";")
ACC = "SELECT * FROM accel_sec_edgar_q3_aux()"


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
    c = conn()
    out = {}
    # plain SQL
    timed(c, REF)
    plain = [timed(c, REF) for _ in range(reps)]
    out["plain_p50_ms"] = statistics.median(plain)
    # build: first accel call in a fresh backend
    c2 = conn()
    out["build_ms"] = timed(c2, ACC)
    hits = [timed(c2, ACC) for _ in range(200)]
    out["accel_p50_ms"] = statistics.median(hits)
    out["accel_p99_ms"] = p(hits, 0.99)
    # write overhead on the live state: single-row num updates / sub updates
    cur = c2.cursor()
    cur.execute("SELECT adsh FROM sub WHERE fy=2022 ORDER BY adsh LIMIT 1")
    adsh = cur.fetchone()[0]
    cur.execute("SELECT count(*) FROM num WHERE adsh=%s AND uom='USD' AND value IS NOT NULL", (adsh,))
    out["filing_usd_rows"] = cur.fetchone()[0]
    upd_num = "UPDATE num SET value = value + %s WHERE ctid = (SELECT ctid FROM num WHERE adsh=%s AND uom='USD' AND value IS NOT NULL LIMIT 1)"
    upd_sub = "UPDATE sub SET cik = cik WHERE adsh=%s"
    ins_del = None

    def run(label, sql, mk):
        xs = []
        for i in range(60):
            xs.append(timed(c2, sql, mk(i)))
        out[label] = statistics.median(xs)

    for state in ("capture", "plain"):
        for t in ("num", "sub"):
            c2.cursor().execute("ALTER TABLE %s %s TRIGGER USER" % (t, "ENABLE" if state == "capture" else "DISABLE"))
        run("write_num_update_%s_ms" % state, upd_num, lambda i: (1.0 if i % 2 == 0 else -1.0, adsh))
        run("write_sub_update_%s_ms" % state, upd_sub, lambda i: (adsh,))
    for t in ("num", "sub"):
        c2.cursor().execute("ALTER TABLE %s ENABLE TRIGGER USER" % t)
    # first read after a write (incremental maintenance on commit => ready)
    timed(c2, upd_num, (1.0, adsh))
    out["read_after_write_ms"] = timed(c2, ACC)
    timed(c2, upd_num, (-1.0, adsh))
    for k, v in out.items():
        print("%-28s %s" % (k, ("%.3f" % v) if isinstance(v, float) else v))


if __name__ == "__main__":
    main()
