#!/usr/bin/env python3
"""SEC-EDGAR Q24 aux vs plain SQL on a loaded PostgreSQL database (SF3).

Reports p50/p90 of: plain query, aux build (first call of a fresh backend),
aux hit read, and write latency (autocommit statement incl. the commit-time
maintenance) for a num value UPDATE and a pre triple DELETE in three modes:
  plain     capture triggers disabled (a stock write)
  triggers  triggers enabled, no accelerator in the session
  aux       accelerator state built in the session (maintained at commit)
Env: PGHOST PGPORT PGUSER PGPASSWORD, Q24_DB (default sec_edgar).
Usage: bench_postgres_q24.py [--plain-reps 3] [--build-reps 3] [--write-reps 30]
Needs the triple indexes (scripts/postgres_sec_edgar_q24_indexes.sql).
"""
import argparse
import os
import re
import statistics
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tests", "aux_state"))
import test_postgres_sec_edgar_q24_differential as T  # noqa: E402

TIME = re.compile(r"^Time: ([0-9.]+) ms")


class Timed(T.Session):
    def __init__(self, db, name):
        super().__init__(db, name)
        self.run("\\timing on")

    def rows(self, sql):
        return [r for r in super().rows(sql) if not r[0].startswith("Time:")]

    def ms(self, sql):
        t = None
        for l in self.run(sql):
            m = TIME.match(l)
            if m:
                t = float(m.group(1))
        return t


def pct(v, p):
    v = sorted(v)
    return v[min(len(v) - 1, int(round(p * (len(v) - 1))))]


def line(label, v):
    print("%-34s p50 %10.3f ms  p90 %10.3f ms  (n=%d)" %
          (label, statistics.median(v), pct(v, 0.9), len(v)), flush=True)


def lit(s):
    return T.lit(s)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--plain-reps", type=int, default=3)
    ap.add_argument("--build-reps", type=int, default=3)
    ap.add_argument("--write-reps", type=int, default=30)
    ap.add_argument("--read-reps", type=int, default=200)
    a = ap.parse_args()
    db = os.environ.get("Q24_DB", "sec_edgar")

    s = Timed(db, "bench")
    top = s.rows(T.ACCEL_SQL)
    assert top, "no result rows to anchor the write workload"
    tag, ver = top[0][0], top[0][1]
    nums = s.rows("SELECT DISTINCT adsh FROM num WHERE tag=%s AND version=%s AND uom='USD' "
                  "AND ddate BETWEEN 20230101 AND 20231231 AND value IS NOT NULL LIMIT %d"
                  % (lit(tag), lit(ver), a.write_reps))
    pres = s.rows("SELECT DISTINCT p.adsh FROM pre p JOIN num n ON n.adsh=p.adsh AND n.tag=p.tag "
                  "AND n.version=p.version WHERE p.tag=%s AND p.version=%s LIMIT %d"
                  % (lit(tag), lit(ver), a.write_reps))
    nums = [r[0] for r in nums]
    pres = [r[0] for r in pres]
    print("anchor group %s / %s; %d num triples, %d pre triples" % (tag, ver, len(nums), len(pres)))
    s.run("DROP TABLE IF EXISTS pre_bak_bench")
    s.run("CREATE TABLE pre_bak_bench AS SELECT * FROM pre WHERE false")
    s.close()

    # Plain query.
    v = []
    for _ in range(a.plain_reps):
        x = Timed(db, "plain")
        v.append(x.ms(T.PLAIN_SQL.replace(' COLLATE "C" NULLS FIRST', '')))
        x.close()
    line("plain SQL read", v)

    # Build (first call in a fresh backend).
    v = []
    for _ in range(a.build_reps):
        x = Timed(db, "build")
        v.append(x.ms(T.ACCEL_SQL))
        x.close()
    line("aux build (first call)", v)

    # Hit reads.
    x = Timed(db, "read")
    x.ms(T.ACCEL_SQL)
    v = [x.ms(T.ACCEL_SQL) for _ in range(a.read_reps)]
    line("aux read (hit)", v)
    x.close()

    def writes(sess, label):
        vn, vp = [], []
        for i, adsh in enumerate(nums):
            d = "+ 1" if i % 2 == 0 else "- 1"  # net effect cancels pairwise
            vn.append(sess.ms("UPDATE num SET value = value %s WHERE adsh=%s AND tag=%s AND version=%s"
                              % (d, lit(adsh), lit(tag), lit(ver))))
        for adsh in pres:
            trip = "adsh=%s AND tag=%s AND version=%s" % (lit(adsh), lit(tag), lit(ver))
            sess.run("DELETE FROM pre_bak_bench"); sess.run("INSERT INTO pre_bak_bench SELECT * FROM pre WHERE " + trip)
            vp.append(sess.ms("DELETE FROM pre WHERE " + trip))
            sess.run("INSERT INTO pre SELECT * FROM pre_bak_bench")
        line("%s: num UPDATE (value)" % label, vn)
        line("%s: pre DELETE (triple)" % label, vp)

    adm = T.Session(db, "adm")
    adm.run("ALTER TABLE num DISABLE TRIGGER USER; ALTER TABLE pre DISABLE TRIGGER USER")
    x = Timed(db, "w-plain")
    writes(x, "plain write")
    x.close()
    adm.run("ALTER TABLE num ENABLE TRIGGER USER; ALTER TABLE pre ENABLE TRIGGER USER")
    x = Timed(db, "w-trig")
    writes(x, "triggers only")
    x.close()
    x = Timed(db, "w-aux")
    x.ms(T.ACCEL_SQL)  # build
    writes(x, "aux maintained")
    print("execute counts:", x.rows("SELECT accel_get_execute_counts()"))
    print("last maintenance:", x.rows("SELECT accel_get_last_maintenance_action()"))
    # The maintained state must still equal a fresh build.
    fresh = Timed(db, "fresh")
    A = x.rows(T.ACCEL_SQL)
    B = fresh.rows(T.ACCEL_SQL)
    assert A == B, (A, B)
    print("maintained state == fresh build: OK")
    x.close(); fresh.close()
    adm.run("DROP TABLE IF EXISTS pre_bak_bench")
    adm.close()


if __name__ == "__main__":
    main()
