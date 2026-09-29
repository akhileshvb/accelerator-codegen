#!/usr/bin/env python3
"""Concurrent randomized stress test of the Q21 Postgres runtime protocol.

W worker sessions run random READ COMMITTED / REPEATABLE READ transactions on
the accel_q21_rt database: lineitem/orders/supplier/nation writes, TRUNCATE, SAVEPOINT +
ROLLBACK TO / RELEASE, COMMIT or ROLLBACK, with Q21 queries interleaved
(inside write transactions too).  Every query evaluates
accel_tpch_q21_aux(nation) and the catalog SQL in ONE statement, so both see
the same snapshot, and compares the ordered results (the reference orders
s_name by the column's own collation, which must be C: the runtime refuses
other collations).  Hit / fallback / rebuild
counts come from the runtime's DEBUG1 trace, so a run where nothing ever hits
is reported rather than silently passing.

Database invariants kept by construction (Q21 KeysOk / DomOk): order keys are
unique (each worker owns the keys congruent to its id), every lineitem's order
exists (a worker only touches its own orders, and deletes an order together
with its lineitems), supplier and nation keys are fixed.

Usage: q21_runtime_stress.py [seconds=60] [workers=4] [seed=1]
Exit 1 on any mismatch or if no query was served from the Aux store.
"""
import os
import random
import sys
import threading
import time

import psycopg2
from psycopg2 import errors

DSN = os.environ.get("Q21_RT_DSN", "host=127.0.0.1 port=5434 user=postgres password=postgres dbname=accel_q21_rt")
NATIONS = ["SAUDI ARABIA", "PERU", "KENYA", "ATLANTIS"]
SUPPLIERS = list(range(1, 9))
Q21 = """SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F'
AND l1.l_receiptdate > l1.l_commitdate
AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
AND s_nationkey = n_nationkey AND n_name = %(n)s GROUP BY s_name"""
COMPARE = f"""SELECT
 (SELECT coalesce(array_agg(t.s_name || ':' || t.numwait ORDER BY t.o), '{{}}')
    FROM accel_tpch_q21_aux(%(n)s) WITH ORDINALITY AS t(s_name, numwait, o)),
 (SELECT coalesce(array_agg(q.s_name || ':' || q.numwait ORDER BY q.numwait DESC, q.s_name), '{{}}')
    FROM ({Q21}) q)"""


def setup(rng):
    c = psycopg2.connect(DSN)
    c.autocommit = True
    cur = c.cursor()
    cur.execute("DELETE FROM lineitem; DELETE FROM orders; DELETE FROM supplier; DELETE FROM nation;")
    cur.execute("INSERT INTO nation VALUES (0,'SAUDI ARABIA',0,'c'),(1,'PERU',0,'c'),(2,'KENYA',0,'c')")
    for s in SUPPLIERS:
        cur.execute("INSERT INTO supplier VALUES (%s,%s,'a',%s,'p',0,'c')", (s, f"Supp#{s:02d}", s % 3))
    for ok in range(1, 41):
        insert_order(cur, rng, ok)
    c.close()


def insert_order(cur, rng, ok):
    cur.execute("INSERT INTO orders VALUES (%s,1,%s,0,'1995-01-01','p','c',0,'c')",
                (ok, rng.choice("FFFOP")))
    for ln in range(1, rng.randint(1, 4) + 1):
        insert_lineitem(cur, rng, ok, ln)


def insert_lineitem(cur, rng, ok, ln):
    late = rng.random() < 0.5
    cur.execute("INSERT INTO lineitem VALUES (%s,1,%s,%s,1,1,0,0,'N','O','1995-01-01',%s,%s,'x','x','c')",
                (ok, rng.choice(SUPPLIERS + [9]), ln,
                 "1995-01-02" if late else "1995-01-05", "1995-01-05" if late else "1995-01-02"))


class Worker(threading.Thread):
    def __init__(self, wid, nworkers, deadline, seed):
        super().__init__()
        self.wid, self.nw, self.deadline = wid, nworkers, deadline
        self.rng = random.Random(seed * 1000 + wid)
        self.stats = {"queries": 0, "hit": 0, "fallback": 0, "rebuild": 0, "commits": 0,
                      "rollbacks": 0, "serialization": 0, "mismatch": 0}
        self.errors = []

    def own_orders(self, cur):
        cur.execute("SELECT o_orderkey FROM orders WHERE o_orderkey %% %s = %s", (self.nw, self.wid))
        return [r[0] for r in cur.fetchall()]

    def query(self, c, cur, where):
        n = self.rng.choice(NATIONS)
        c.notices.clear()
        cur.execute(COMPARE, {"n": n})
        accel, sql = cur.fetchone()
        for note in c.notices:
            if "tpch_q21" in note:
                for key, tag in (("hit", "aux hit"), ("fallback", "fallback"), ("rebuild", "rebuild")):
                    if tag in note:
                        self.stats[key] += 1
        c.notices.clear()
        self.stats["queries"] += 1
        if accel != sql:
            self.stats["mismatch"] += 1
            self.errors.append(f"worker {self.wid} {where} nation={n}: accel={accel} sql={sql}")

    def write_op(self, cur):
        rng = self.rng
        mine = self.own_orders(cur)
        r = rng.random()
        if r < 0.30 and mine:
            ok = rng.choice(mine)
            cur.execute("SELECT coalesce(max(l_linenumber), 0) FROM lineitem WHERE l_orderkey = %s", (ok,))
            insert_lineitem(cur, rng, ok, cur.fetchone()[0] + 1)
        elif r < 0.45 and mine:
            cur.execute("DELETE FROM lineitem WHERE ctid IN (SELECT ctid FROM lineitem WHERE l_orderkey = %s LIMIT 1)",
                        (rng.choice(mine),))
        elif r < 0.60 and mine:
            late = rng.random() < 0.5
            cur.execute("UPDATE lineitem SET l_receiptdate = %s WHERE l_orderkey = %s AND l_linenumber = 1",
                        ("1995-01-09" if late else "1995-01-01", rng.choice(mine)))
        elif r < 0.72 and mine:
            cur.execute("UPDATE orders SET o_orderstatus = %s WHERE o_orderkey = %s",
                        (rng.choice("FOP"), rng.choice(mine)))
        elif r < 0.82:
            base = max(mine + [self.wid]) + self.nw
            insert_order(cur, rng, base - (base % self.nw) + self.wid)
        elif r < 0.90 and mine:
            ok = rng.choice(mine)
            cur.execute("DELETE FROM lineitem WHERE l_orderkey = %s", (ok,))
            cur.execute("DELETE FROM orders WHERE o_orderkey = %s", (ok,))
        elif r < 0.905:
            cur.execute("TRUNCATE lineitem")
        elif r < 0.95:
            cur.execute("UPDATE supplier SET s_nationkey = %s WHERE s_suppkey = %s",
                        (rng.randint(0, 2), rng.choice(SUPPLIERS)))
        else:
            cur.execute("UPDATE lineitem SET l_comment = 'z' WHERE l_orderkey = %s",
                        (rng.choice(mine) if mine else 0,))

    def run(self):
        try:
            self.run_session()
        except Exception as ex:  # noqa: BLE001  (a dead session is a failure)
            self.errors.append(f"worker {self.wid}: session died: {type(ex).__name__}: {ex}")
            self.stats["mismatch"] += 1

    def run_session(self):
        c = psycopg2.connect(DSN)
        c.autocommit = True
        c.cursor().execute("SET client_min_messages = debug1")
        c.autocommit = False
        cur = c.cursor()
        rng = self.rng
        while time.time() < self.deadline:
            iso = rng.choice(["READ COMMITTED", "READ COMMITTED", "REPEATABLE READ"])
            try:
                cur.execute(f"BEGIN ISOLATION LEVEL {iso}")
                if rng.random() < 0.5:
                    for _ in range(rng.randint(1, 3)):
                        self.query(c, cur, f"read-only {iso}")
                    c.commit()
                    continue
                if rng.random() < 0.3:
                    self.query(c, cur, f"before-write {iso}")
                for _ in range(rng.randint(1, 4)):
                    sp = rng.random() < 0.3
                    if sp:
                        cur.execute("SAVEPOINT s")
                    self.write_op(cur)
                    if rng.random() < 0.3:
                        self.query(c, cur, f"in-write {iso}")
                    if sp:
                        cur.execute("ROLLBACK TO SAVEPOINT s" if rng.random() < 0.5 else "RELEASE SAVEPOINT s")
                if rng.random() < 0.2:
                    c.rollback()
                    self.stats["rollbacks"] += 1
                else:
                    c.commit()
                    self.stats["commits"] += 1
            except (errors.SerializationFailure, errors.DeadlockDetected):
                c.rollback()
                self.stats["serialization"] += 1
            except Exception as ex:  # noqa: BLE001
                c.rollback()
                self.errors.append(f"worker {self.wid}: {type(ex).__name__}: {ex}")
                self.stats["mismatch"] += 1
        # Final quiescent check in a fresh transaction.
        self.query(c, cur, "final")
        c.commit()
        c.close()


def main():
    secs = float(sys.argv[1]) if len(sys.argv) > 1 else 60
    nw = int(sys.argv[2]) if len(sys.argv) > 2 else 4
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    setup(random.Random(seed))
    deadline = time.time() + secs
    workers = [Worker(w, nw, deadline, seed) for w in range(nw)]
    for w in workers:
        w.start()
    for w in workers:
        w.join()
    total = {k: sum(w.stats[k] for w in workers) for k in workers[0].stats}
    for w in workers:
        for e in w.errors[:10]:
            print(e)
    print("q21 runtime stress:", total)
    ok = total["mismatch"] == 0 and total["hit"] > 0
    print("q21 runtime stress:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
