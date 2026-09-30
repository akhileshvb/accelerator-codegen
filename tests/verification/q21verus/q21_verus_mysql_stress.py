#!/usr/bin/env python3
"""Randomized differential test of the Verus Q21 accelerator on MySQL.

Single session, autocommit (the MySQL runtime maintains the store after each
statement; its change capture is sound for autocommit writes only).  Data is
outside the C++ kernel's assumptions where MySQL allows it (primary keys are
enforced by the MySQL schema): lineitems without an order (inserted before
the order, or left behind when the order is deleted), o_orderstatus changes
to 'F' for orders that already have lineitems (the kernel's targeted-scan
path), suppliers sharing an s_name, lineitems of an unknown supplier, nations
sharing an n_name, and supplier / nation inserts, updates, and deletes.

After every write, mysql_accel_tpch_q21_verus(nation) is compared with the
catalog SQL (ordered rows).  The database is overwritten: back it up first
(the driver script does).

Usage: q21_verus_mysql_stress.py [writes=2000] [seed=1]  (env Q21_MYSQL_PORT)
"""
import os
import random
import sys

import pymysql

PORT = int(os.environ.get("Q21_MYSQL_PORT", "3307"))
NATIONS = ["SAUDI ARABIA", "PERU", "KENYA", "ATLANTIS"]
SUPPLIERS = list(range(1, 9))
Q21 = """SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F'
AND l1.l_receiptdate > l1.l_commitdate
AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
AND s_nationkey = n_nationkey AND n_name = %s GROUP BY s_name ORDER BY numwait DESC, s_name"""


def connect():
    return pymysql.connect(host="127.0.0.1", port=PORT, user="root", password="mysql",
                           database="accel_mysql_test", autocommit=True, charset="utf8mb4")


def accel(cur, nation):
    cur.execute("SELECT mysql_accel_tpch_q21_verus(%s)", (nation,))
    text = cur.fetchone()[0]
    if isinstance(text, bytes):
        text = text.decode()
    rows = []
    for line in (text or "").split("\n"):
        if line:
            name, n = line.rsplit("|", 1)
            rows.append((name, int(n)))
    return rows


def ref(cur, nation):
    cur.execute(Q21, (nation,))
    return [(r[0], int(r[1])) for r in cur.fetchall()]


def insert_lineitem(cur, rng, ok, ln):
    late = rng.random() < 0.5
    cur.execute("INSERT INTO lineitem VALUES (%s,1,%s,%s,1,1,0,0,'N','O','1995-01-01',%s,%s,'x','x','c')",
                (ok, rng.choice(SUPPLIERS + [9]), ln,
                 "1995-01-02" if late else "1995-01-05", "1995-01-05" if late else "1995-01-02"))


def setup(cur, rng):
    cur.execute("DELETE FROM lineitem")
    cur.execute("DELETE FROM orders")
    cur.execute("DELETE FROM supplier")
    cur.execute("DELETE FROM nation")
    cur.execute("INSERT INTO nation VALUES (0,'SAUDI ARABIA',0,'c'),(1,'PERU',0,'c'),(2,'KENYA',0,'c'),(3,'PERU',0,'c')")
    for s in SUPPLIERS:
        name = "Supp#01" if s == 5 else f"Supp#{s:02d}"
        cur.execute("INSERT INTO supplier VALUES (%s,%s,'a',%s,'p',0,'c')", (s, name, s % 4))
    for ok in range(1, 41):
        cur.execute("INSERT INTO orders VALUES (%s,1,%s,0,'1995-01-01','p','c',0,'c')", (ok, rng.choice("FFFOP")))
        for ln in range(1, rng.randint(1, 4) + 1):
            insert_lineitem(cur, rng, ok, ln)


def keys(cur):
    cur.execute("SELECT o_orderkey FROM orders UNION SELECT l_orderkey FROM lineitem")
    return [r[0] for r in cur.fetchall()]


def write(cur, rng, nxt):
    mine = keys(cur)
    r = rng.random()
    if r < 0.22 and mine:
        ok = rng.choice(mine)
        cur.execute("SELECT coalesce(max(l_linenumber), 0) FROM lineitem WHERE l_orderkey = %s", (ok,))
        insert_lineitem(cur, rng, ok, cur.fetchone()[0] + 1)
    elif r < 0.32 and mine:
        cur.execute("DELETE FROM lineitem WHERE l_orderkey = %s ORDER BY l_linenumber LIMIT 1", (rng.choice(mine),))
    elif r < 0.44 and mine:
        late = rng.random() < 0.5
        cur.execute("UPDATE lineitem SET l_receiptdate = %s WHERE l_orderkey = %s AND l_linenumber = 1",
                    ("1995-01-09" if late else "1995-01-01", rng.choice(mine)))
    elif r < 0.56 and mine:
        cur.execute("UPDATE orders SET o_orderstatus = %s WHERE o_orderkey = %s", (rng.choice("FFOP"), rng.choice(mine)))
    elif r < 0.64:
        ok = nxt[0]
        nxt[0] += 1
        cur.execute("INSERT INTO orders VALUES (%s,1,%s,0,'1995-01-01','p','c',0,'c')", (ok, rng.choice("FFFOP")))
        for ln in range(1, rng.randint(1, 4) + 1):
            insert_lineitem(cur, rng, ok, ln)
    elif r < 0.72:
        ok = nxt[0]
        nxt[0] += 1
        for ln in range(1, rng.randint(1, 3) + 1):
            insert_lineitem(cur, rng, ok, ln)
        if rng.random() < 0.5:
            cur.execute("INSERT INTO orders VALUES (%s,1,'F',0,'1995-01-01','p','c',0,'c')", (ok,))
    elif r < 0.80 and mine:
        ok = rng.choice(mine)
        if rng.random() < 0.5:
            cur.execute("DELETE FROM lineitem WHERE l_orderkey = %s", (ok,))
        cur.execute("DELETE FROM orders WHERE o_orderkey = %s", (ok,))
    elif r < 0.84:
        cur.execute("UPDATE supplier SET s_nationkey = %s WHERE s_suppkey = %s", (rng.randint(0, 4), rng.choice(SUPPLIERS)))
    elif r < 0.87:
        cur.execute("UPDATE supplier SET s_name = %s WHERE s_suppkey = %s",
                    (rng.choice(["Supp#01", "Supp#02", "Supp#07"]), rng.choice(SUPPLIERS)))
    elif r < 0.90:
        if rng.random() < 0.5:
            cur.execute("INSERT IGNORE INTO supplier VALUES (9,'Supp#09','a',%s,'p',0,'c')", (rng.randint(0, 4),))
        else:
            cur.execute("DELETE FROM supplier WHERE s_suppkey = 9")
    elif r < 0.93:
        cur.execute("UPDATE nation SET n_name = %s WHERE n_nationkey = %s", (rng.choice(NATIONS), rng.randint(0, 3)))
    elif r < 0.96:
        if rng.random() < 0.5:
            cur.execute("INSERT IGNORE INTO nation VALUES (4,%s,0,'c')", (rng.choice(NATIONS),))
        else:
            cur.execute("DELETE FROM nation WHERE n_nationkey = 4")
    else:
        cur.execute("UPDATE lineitem SET l_comment = 'z' WHERE l_orderkey = %s", (rng.choice(mine) if mine else 0,))


def main():
    writes = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    rng = random.Random(seed)
    c = connect()
    cur = c.cursor()
    # Q21_FORCE=ApplyDelta forces the substrate policy onto the kernel's
    # incremental path (the calibrated MySQL cost model otherwise rebuilds).
    force = os.environ.get("Q21_FORCE", "")
    cur.execute("SELECT mysql_accel_set_force_maintenance_action(%s)", (force,))
    cur.fetchall()
    setup(cur, rng)
    nxt = [1000]
    stats = {"writes": 0, "queries": 0, "mismatch": 0, "nonempty": 0}
    errors = []
    for i in range(writes):
        write(cur, rng, nxt)
        stats["writes"] += 1
        n = rng.choice(NATIONS)
        a, b = accel(cur, n), ref(cur, n)
        stats["queries"] += 1
        stats["nonempty"] += bool(b)
        if a != b:
            stats["mismatch"] += 1
            if len(errors) < 10:
                errors.append(f"write {i} nation={n}: accel={a} sql={b}")
    cur.execute("SELECT mysql_accel_set_force_maintenance_action('')")
    cur.fetchall()
    for e in errors:
        print(e)
    print("q21 verus mysql stress:", stats)
    ok = stats["mismatch"] == 0
    print("q21 verus mysql stress:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
