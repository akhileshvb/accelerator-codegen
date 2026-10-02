#!/usr/bin/env python3
"""Randomized differential stress test of the Verus Q21 accelerator.

Like tests/verification/q21/runtime/q21_runtime_stress.py, but for
accel_tpch_q21_verus and with data outside the C++ kernel's assumptions,
because the Verus kernel's reference specification is exact SQL bag
semantics with no key or foreign-key assumptions:

  * duplicate orders rows for one o_orderkey (several 'F' rows),
  * lineitems whose order does not exist (inserted before the order, or left
    behind when the order is deleted),
  * o_orderstatus changes to 'F' for orders that already have lineitems
    (exercises the kernel's targeted-scan path),
  * suppliers sharing an s_name, lineitems of an unknown supplier,
  * two nations with the same n_name, nation renames, supplier moves.

Updates cover every column Q21 reads on all four tables, alone and in
combination (l_commitdate, l_receiptdate, l_suppkey, l_orderkey; o_orderkey,
o_orderstatus; s_suppkey, s_name, s_nationkey; n_nationkey, n_name), key changes
included, one row / all rows of an order / bulk statements over many orders.

W worker sessions run random READ COMMITTED / REPEATABLE READ transactions
(savepoints, rollbacks, TRUNCATE) with Q21 queries interleaved.  Every query
evaluates accel_tpch_q21_verus(nation) and the catalog SQL in ONE statement
(same snapshot) and compares the ordered results.

Usage: q21_verus_stress.py [seconds=60] [workers=4] [seed=1]
Exit 1 on any mismatch or if no query was served from the Aux store.
"""
import os
import random
import sys
import threading
import time

import psycopg2
from psycopg2 import errors

DSN = os.environ.get(
    "Q21_RT_DSN", "host=127.0.0.1 port=5434 user=postgres password=postgres dbname=accel_q21_rt")
FN = os.environ.get("Q21_FN", "accel_tpch_q21_verus")
NATIONS = ["SAUDI ARABIA", "PERU", "KENYA", "ATLANTIS"]
SUPPLIERS = list(range(1, 9))
DATES = ["1995-01-01", "1995-01-02", "1995-01-05", "1995-01-09"]
# Q21_FORCE=ApplyDelta|RebuildPartition|RebuildAll|InvalidateAndRebuildOnRead|NoOp forces the
# substrate maintenance action in every session (empty: the cost-model policy).
FORCE = os.environ.get("Q21_FORCE", "")
Q21 = """SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F'
AND l1.l_receiptdate > l1.l_commitdate
AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
AND s_nationkey = n_nationkey AND n_name = %(n)s GROUP BY s_name
ORDER BY numwait DESC, s_name LIMIT 100"""
COMPARE = f"""SELECT
 (SELECT coalesce(array_agg(t.s_name || ':' || t.numwait ORDER BY t.o), '{{}}')
    FROM {FN}(%(n)s) WITH ORDINALITY AS t(s_name, numwait, o)),
 (SELECT coalesce(array_agg(q.s_name || ':' || q.numwait ORDER BY q.numwait DESC, q.s_name), '{{}}')
    FROM ({Q21}) q)"""


def setup(rng):
    c = psycopg2.connect(DSN)
    c.autocommit = True
    cur = c.cursor()
    cur.execute("DELETE FROM lineitem; DELETE FROM orders; DELETE FROM supplier; DELETE FROM nation;")
    # nation 3 duplicates the name PERU
    cur.execute("INSERT INTO nation VALUES (0,'SAUDI ARABIA',0,'c'),(1,'PERU',0,'c'),(2,'KENYA',0,'c'),(3,'PERU',0,'c')")
    for s in SUPPLIERS:
        # suppliers 1 and 5 share a name
        name = "Supp#01" if s == 5 else f"Supp#{s:02d}"
        cur.execute("INSERT INTO supplier VALUES (%s,%s,'a',%s,'p',0,'c')", (s, name, s % 4))
    for ok in range(1, 41):
        insert_order(cur, rng, ok)
    c.close()


def insert_order(cur, rng, ok, with_lines=True):
    cur.execute("INSERT INTO orders VALUES (%s,1,%s,0,'1995-01-01','p','c',0,'c')",
                (ok, rng.choice("FFFOP")))
    if with_lines:
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
        self.actions = {}
        self.errors = []

    def own_keys(self, cur):
        # orderkeys of this worker: from orders and from (possibly orphan) lineitems
        cur.execute("SELECT o_orderkey FROM orders WHERE o_orderkey %% %s = %s UNION "
                    "SELECT l_orderkey FROM lineitem WHERE l_orderkey %% %s = %s",
                    (self.nw, self.wid, self.nw, self.wid))
        return [r[0] for r in cur.fetchall()]

    def query(self, c, cur, where):
        n = self.rng.choice(NATIONS)
        c.notices.clear()
        cur.execute(COMPARE, {"n": n})
        accel, sql = cur.fetchone()
        for note in c.notices:
            if "tpch_q21_verus" in note:
                for key, tag in (("hit", "aux hit"), ("fallback", "fallback"), ("rebuild", "rebuild")):
                    if tag in note:
                        self.stats[key] += 1
        c.notices.clear()
        self.stats["queries"] += 1
        if accel != sql:
            self.stats["mismatch"] += 1
            self.errors.append(f"worker {self.wid} {where} nation={n}: accel={accel} sql={sql}")

    def note_action(self, cur):
        """Tally the substrate's last maintenance action in this backend (after a commit)."""
        cur.execute("SELECT accel_postgres_get_last_maintenance_action()")
        a = cur.fetchone()[0]
        cur.connection.commit()
        self.actions[a] = self.actions.get(a, 0) + 1

    def new_key(self, mine):
        base = max(mine + [self.wid]) + self.nw
        return base - (base % self.nw) + self.wid

    def other_key(self, mine, ok):
        """An orderkey in this worker's partition (existing, or fresh), usually not `ok`."""
        rng = self.rng
        if rng.random() < 0.3:
            return self.new_key(mine)
        return rng.choice(mine)

    def lineitem_update(self, cur, mine):
        """UPDATE of lineitem: every column Q21 reads, alone and in combination."""
        rng = self.rng
        ok = rng.choice(mine)
        one = "WHERE ctid IN (SELECT ctid FROM lineitem WHERE l_orderkey = %s LIMIT 1)"
        d = lambda: rng.choice(DATES)
        variants = [
            ("l_receiptdate = %s", lambda: (d(),), one),
            ("l_commitdate = %s", lambda: (d(),), one),
            ("l_commitdate = %s, l_receiptdate = %s", lambda: (d(), d()), one),
            ("l_suppkey = %s", lambda: (rng.choice(SUPPLIERS + [9]),), one),
            ("l_suppkey = %s, l_receiptdate = %s", lambda: (rng.choice(SUPPLIERS + [9]), d()), one),
            ("l_orderkey = %s", lambda: (self.other_key(mine, ok),), one),
            ("l_orderkey = %s, l_suppkey = %s", lambda: (self.other_key(mine, ok), rng.choice(SUPPLIERS + [9])), one),
            ("l_orderkey = %s, l_suppkey = %s, l_commitdate = %s, l_receiptdate = %s",
             lambda: (self.other_key(mine, ok), rng.choice(SUPPLIERS + [9]), d(), d()), one),
            # every row of the order in one statement
            ("l_commitdate = %s", lambda: (d(),), "WHERE l_orderkey = %s"),
            ("l_suppkey = %s, l_commitdate = %s", lambda: (rng.choice(SUPPLIERS + [9]), d()), "WHERE l_orderkey = %s"),
        ]
        sets, args, where = rng.choice(variants)
        cur.execute(f"UPDATE lineitem SET {sets} {where}", args() + (ok,))

    def orders_update(self, cur, mine):
        rng = self.rng
        ok = rng.choice(mine)
        r = rng.random()
        if r < 0.5:
            # status change, possibly O/P -> F on an order with lineitems (targeted scan)
            cur.execute("UPDATE orders SET o_orderstatus = %s WHERE o_orderkey = %s", (rng.choice("FFOP"), ok))
        elif r < 0.75:
            cur.execute("UPDATE orders SET o_orderkey = %s WHERE o_orderkey = %s", (self.other_key(mine, ok), ok))
        else:
            cur.execute("UPDATE orders SET o_orderkey = %s, o_orderstatus = %s WHERE o_orderkey = %s",
                        (self.other_key(mine, ok), rng.choice("FFOP"), ok))

    def supplier_update(self, cur):
        rng = self.rng
        sk = rng.choice(SUPPLIERS)
        name = lambda: rng.choice(["Supp#01", "Supp#02", "Supp#07", "Supp#%02d" % sk])
        variants = [
            ("s_nationkey = %s", lambda: (rng.randint(0, 4),)),
            ("s_name = %s", lambda: (name(),)),
            ("s_suppkey = %s", lambda: (rng.choice(SUPPLIERS + [9]),)),
            ("s_nationkey = %s, s_name = %s", lambda: (rng.randint(0, 4), name())),
            ("s_suppkey = %s, s_nationkey = %s", lambda: (rng.choice(SUPPLIERS + [9]), rng.randint(0, 4))),
            ("s_suppkey = %s, s_name = %s, s_nationkey = %s",
             lambda: (rng.choice(SUPPLIERS + [9]), name(), rng.randint(0, 4))),
        ]
        sets, args = rng.choice(variants)
        cur.execute(f"UPDATE supplier SET {sets} WHERE s_suppkey = %s", args() + (sk,))

    def nation_update(self, cur):
        rng = self.rng
        nk = rng.randint(0, 4)
        variants = [
            ("n_name = %s", lambda: (rng.choice(NATIONS),)),
            ("n_nationkey = %s", lambda: (rng.randint(0, 4),)),
            ("n_name = %s, n_nationkey = %s", lambda: (rng.choice(NATIONS), rng.randint(0, 4))),
        ]
        sets, args = rng.choice(variants)
        cur.execute(f"UPDATE nation SET {sets} WHERE n_nationkey = %s", args() + (nk,))

    def bulk_update(self, cur):
        """One statement over many of this worker's rows (several orders, several rows each)."""
        rng = self.rng
        own = "WHERE l_orderkey %% %s = %s"
        pk = (self.nw, self.wid)
        r = rng.random()
        if r < 0.25:
            cur.execute(f"UPDATE lineitem SET l_receiptdate = %s {own}", (rng.choice(DATES),) + pk)
        elif r < 0.5:
            cur.execute(f"UPDATE lineitem SET l_commitdate = %s {own}", (rng.choice(DATES),) + pk)
        elif r < 0.75:
            cur.execute(f"UPDATE lineitem SET l_suppkey = 1 + (l_suppkey %% 8) {own}", pk)
        else:
            cur.execute(f"UPDATE lineitem SET l_commitdate = l_receiptdate, l_receiptdate = l_commitdate {own}", pk)

    def write_op(self, cur):
        rng = self.rng
        mine = self.own_keys(cur)
        r = rng.random()
        if r < 0.22 and mine:
            ok = rng.choice(mine)
            cur.execute("SELECT coalesce(max(l_linenumber), 0) FROM lineitem WHERE l_orderkey = %s", (ok,))
            insert_lineitem(cur, rng, ok, cur.fetchone()[0] + 1)
        elif r < 0.32 and mine:
            cur.execute("DELETE FROM lineitem WHERE ctid IN (SELECT ctid FROM lineitem WHERE l_orderkey = %s LIMIT 1)",
                        (rng.choice(mine),))
        elif r < 0.50 and mine:
            self.lineitem_update(cur, mine)
        elif r < 0.60 and mine:
            self.orders_update(cur, mine)
        elif r < 0.66:
            insert_order(cur, rng, self.new_key(mine))
        elif r < 0.70 and mine:
            # duplicate orders row for an existing key
            cur.execute("INSERT INTO orders VALUES (%s,1,%s,0,'1995-01-01','p','c',0,'c')",
                        (rng.choice(mine), rng.choice("FFO")))
        elif r < 0.74:
            # orphan lineitems first; the order may arrive later
            ok = self.new_key(mine)
            for ln in range(1, rng.randint(1, 3) + 1):
                insert_lineitem(cur, rng, ok, ln)
            if rng.random() < 0.5:
                cur.execute("INSERT INTO orders VALUES (%s,1,'F',0,'1995-01-01','p','c',0,'c')", (ok,))
        elif r < 0.80 and mine:
            ok = rng.choice(mine)
            if rng.random() < 0.5:
                cur.execute("DELETE FROM lineitem WHERE l_orderkey = %s", (ok,))
            cur.execute("DELETE FROM orders WHERE o_orderkey = %s", (ok,))
        elif r < 0.803:
            cur.execute("TRUNCATE lineitem")
        elif r < 0.83:
            self.supplier_update(cur)
        elif r < 0.84:
            # supplier 9 is referenced by lineitems; it comes and goes
            if rng.random() < 0.5:
                cur.execute("INSERT INTO supplier VALUES (9,'Supp#09','a',%s,'p',0,'c') "
                            "ON CONFLICT DO NOTHING", (rng.randint(0, 4),))
            else:
                cur.execute("DELETE FROM supplier WHERE s_suppkey = 9")
        elif r < 0.87:
            self.nation_update(cur)
        elif r < 0.88:
            if rng.random() < 0.5:
                cur.execute("INSERT INTO nation VALUES (4,%s,0,'c') ON CONFLICT DO NOTHING",
                            (rng.choice(NATIONS),))
            else:
                cur.execute("DELETE FROM nation WHERE n_nationkey = 4")
        elif r < 0.90:
            self.bulk_update(cur)
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
        c.cursor().execute("SELECT accel_postgres_set_force_maintenance_action(%s)", (FORCE,))
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
                    self.note_action(cur)
            except (errors.SerializationFailure, errors.DeadlockDetected):
                c.rollback()
                self.stats["serialization"] += 1
            except Exception as ex:  # noqa: BLE001
                c.rollback()
                self.errors.append(f"worker {self.wid}: {type(ex).__name__}: {ex}")
                self.stats["mismatch"] += 1
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
    print("q21 verus stress:", total)
    actions = {}
    for w in workers:
        for a, n in w.actions.items():
            actions[a] = actions.get(a, 0) + n
    print("q21 verus stress: last maintenance action after commit (force=%r):" % FORCE, actions)
    ok = total["mismatch"] == 0 and total["hit"] > 0
    if FORCE and FORCE != "policy":
        # a forced action must actually have run (anything else is the kernel declining)
        # (the Q21 kernel has no partitions: RebuildPartition is a full rebuild, module.cpp)
        want = "RebuildAll" if FORCE == "RebuildPartition" else FORCE
        ok = ok and actions.get(want, 0) > 0
    print("q21 verus stress:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
