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

Updates cover every column Q21 reads on all four tables, alone and in
combination (l_commitdate, l_receiptdate, l_suppkey, l_orderkey; o_orderkey,
o_orderstatus; s_suppkey, s_name, s_nationkey; n_nationkey, n_name); key changes
respect the primary keys (UPDATE IGNORE), and rows move between orders.
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
DATES = ["1995-01-01", "1995-01-02", "1995-01-05", "1995-01-09"]
Q21 = """SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F'
AND l1.l_receiptdate > l1.l_commitdate
AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
AND s_nationkey = n_nationkey AND n_name = %s GROUP BY s_name ORDER BY numwait DESC, s_name LIMIT 100"""


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


def next_line(cur, ok):
    cur.execute("SELECT coalesce(max(l_linenumber), 0) + 1 FROM lineitem WHERE l_orderkey = %s", (ok,))
    return cur.fetchone()[0]


def lineitem_update(cur, rng, mine, nxt):
    """UPDATE of lineitem: every column Q21 reads, alone and in combination.
    Key changes respect the (l_orderkey, l_linenumber) primary key."""
    ok = rng.choice(mine)
    one = "WHERE l_orderkey = %s ORDER BY l_linenumber LIMIT 1"
    d = lambda: rng.choice(DATES)
    sk = lambda: rng.choice(SUPPLIERS + [9])
    v = rng.randrange(10)
    if v == 0:
        cur.execute(f"UPDATE lineitem SET l_receiptdate = %s {one}", (d(), ok))
    elif v == 1:
        cur.execute(f"UPDATE lineitem SET l_commitdate = %s {one}", (d(), ok))
    elif v == 2:
        cur.execute(f"UPDATE lineitem SET l_commitdate = %s, l_receiptdate = %s {one}", (d(), d(), ok))
    elif v == 3:
        cur.execute(f"UPDATE lineitem SET l_suppkey = %s {one}", (sk(), ok))
    elif v == 4:
        cur.execute(f"UPDATE lineitem SET l_suppkey = %s, l_receiptdate = %s {one}", (sk(), d(), ok))
    elif v in (5, 6, 7):
        # move the row to another order (existing, or a fresh orderkey), fresh line number
        ok2 = rng.choice(mine) if rng.random() < 0.7 else nxt[0]
        if ok2 == nxt[0]:
            nxt[0] += 1
        ln2 = next_line(cur, ok2)
        if v == 5:
            cur.execute(f"UPDATE IGNORE lineitem SET l_orderkey = %s, l_linenumber = %s {one}", (ok2, ln2, ok))
        elif v == 6:
            cur.execute(f"UPDATE IGNORE lineitem SET l_orderkey = %s, l_linenumber = %s, l_suppkey = %s {one}",
                        (ok2, ln2, sk(), ok))
        else:
            cur.execute(f"UPDATE IGNORE lineitem SET l_orderkey = %s, l_linenumber = %s, l_suppkey = %s, "
                        f"l_commitdate = %s, l_receiptdate = %s {one}", (ok2, ln2, sk(), d(), d(), ok))
    elif v == 8:
        # every row of the order in one statement
        cur.execute("UPDATE lineitem SET l_commitdate = %s WHERE l_orderkey = %s", (d(), ok))
    else:
        cur.execute("UPDATE lineitem SET l_suppkey = %s, l_commitdate = %s WHERE l_orderkey = %s", (sk(), d(), ok))


def orders_update(cur, rng, mine, nxt):
    ok = rng.choice(mine)
    r = rng.random()
    ok2 = rng.choice(mine) if rng.random() < 0.5 else nxt[0]
    if ok2 == nxt[0]:
        nxt[0] += 1
    if r < 0.5:
        cur.execute("UPDATE orders SET o_orderstatus = %s WHERE o_orderkey = %s", (rng.choice("FFOP"), ok))
    elif r < 0.75:
        cur.execute("UPDATE IGNORE orders SET o_orderkey = %s WHERE o_orderkey = %s", (ok2, ok))
    else:
        cur.execute("UPDATE IGNORE orders SET o_orderkey = %s, o_orderstatus = %s WHERE o_orderkey = %s",
                    (ok2, rng.choice("FFOP"), ok))


def supplier_update(cur, rng):
    cur.execute("SELECT s_suppkey FROM supplier")
    have = [r[0] for r in cur.fetchall()] or SUPPLIERS
    sk = rng.choice(have)
    name = rng.choice(["Supp#01", "Supp#02", "Supp#07", "Supp#%02d" % sk])
    nk = rng.randint(0, 4)
    # a free key (another supplier's old key, or a fresh one) so the move takes effect
    free = [k for k in SUPPLIERS + [9] if k not in have]
    sk2 = rng.choice(free) if free and rng.random() < 0.6 else 20 + rng.randint(0, 15)
    v = rng.randrange(6)
    if v == 0:
        cur.execute("UPDATE supplier SET s_nationkey = %s WHERE s_suppkey = %s", (nk, sk))
    elif v == 1:
        cur.execute("UPDATE supplier SET s_name = %s WHERE s_suppkey = %s", (name, sk))
    elif v == 2:
        cur.execute("UPDATE IGNORE supplier SET s_suppkey = %s WHERE s_suppkey = %s", (sk2, sk))
    elif v == 3:
        cur.execute("UPDATE supplier SET s_nationkey = %s, s_name = %s WHERE s_suppkey = %s", (nk, name, sk))
    elif v == 4:
        cur.execute("UPDATE IGNORE supplier SET s_suppkey = %s, s_nationkey = %s WHERE s_suppkey = %s", (sk2, nk, sk))
    else:
        cur.execute("UPDATE IGNORE supplier SET s_suppkey = %s, s_name = %s, s_nationkey = %s WHERE s_suppkey = %s",
                    (sk2, name, nk, sk))


def nation_update(cur, rng):
    cur.execute("SELECT n_nationkey FROM nation")
    have = [r[0] for r in cur.fetchall()] or [0, 1, 2, 3]
    nk = rng.choice(have)
    free = [k for k in range(0, 5) if k not in have]
    nk2 = rng.choice(free) if free and rng.random() < 0.6 else 5 + rng.randint(0, 9)
    v = rng.randrange(3)
    if v == 0:
        cur.execute("UPDATE nation SET n_name = %s WHERE n_nationkey = %s", (rng.choice(NATIONS), nk))
    elif v == 1:
        cur.execute("UPDATE IGNORE nation SET n_nationkey = %s WHERE n_nationkey = %s", (nk2, nk))
    else:
        cur.execute("UPDATE IGNORE nation SET n_name = %s, n_nationkey = %s WHERE n_nationkey = %s",
                    (rng.choice(NATIONS), nk2, nk))


def bulk_update(cur, rng):
    """One statement over many orders (several rows each)."""
    k = rng.randint(0, 2)
    r = rng.random()
    if r < 0.25:
        cur.execute("UPDATE lineitem SET l_receiptdate = %s WHERE l_orderkey %% 3 = %s", (rng.choice(DATES), k))
    elif r < 0.5:
        cur.execute("UPDATE lineitem SET l_commitdate = %s WHERE l_orderkey %% 3 = %s", (rng.choice(DATES), k))
    elif r < 0.75:
        cur.execute("UPDATE lineitem SET l_suppkey = 1 + (l_suppkey %% 8) WHERE l_orderkey %% 3 = %s", (k,))
    else:
        cur.execute("UPDATE lineitem SET l_commitdate = l_receiptdate WHERE l_orderkey %% 3 = %s", (k,))


def write(cur, rng, nxt):
    mine = keys(cur)
    r = rng.random()
    if r < 0.22 and mine:
        ok = rng.choice(mine)
        insert_lineitem(cur, rng, ok, next_line(cur, ok))
    elif r < 0.32 and mine:
        cur.execute("DELETE FROM lineitem WHERE l_orderkey = %s ORDER BY l_linenumber LIMIT 1", (rng.choice(mine),))
    elif r < 0.50 and mine:
        lineitem_update(cur, rng, mine, nxt)
    elif r < 0.60 and mine:
        orders_update(cur, rng, mine, nxt)
    elif r < 0.66:
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
    elif r < 0.78 and mine:
        ok = rng.choice(mine)
        if rng.random() < 0.5:
            cur.execute("DELETE FROM lineitem WHERE l_orderkey = %s", (ok,))
        cur.execute("DELETE FROM orders WHERE o_orderkey = %s", (ok,))
    elif r < 0.83:
        supplier_update(cur, rng)
    elif r < 0.86:
        if rng.random() < 0.5:
            cur.execute("INSERT IGNORE INTO supplier VALUES (9,'Supp#09','a',%s,'p',0,'c')", (rng.randint(0, 4),))
        else:
            cur.execute("DELETE FROM supplier WHERE s_suppkey = 9")
    elif r < 0.90:
        nation_update(cur, rng)
    elif r < 0.92:
        if rng.random() < 0.5:
            cur.execute("INSERT IGNORE INTO nation VALUES (4,%s,0,'c')", (rng.choice(NATIONS),))
        else:
            cur.execute("DELETE FROM nation WHERE n_nationkey = 4")
    elif r < 0.95:
        bulk_update(cur, rng)
    else:
        cur.execute("UPDATE lineitem SET l_comment = 'z' WHERE l_orderkey = %s", (rng.choice(mine) if mine else 0,))


def main():
    writes = int(sys.argv[1]) if len(sys.argv) > 1 else 2000
    seed = int(sys.argv[2]) if len(sys.argv) > 2 else 1
    rng = random.Random(seed)
    c = connect()
    cur = c.cursor()
    # coverage: for each UPDATE, the columns it SETs and whether it changed a row
    cover = {}
    raw_execute = cur.execute

    def execute(q, args=None):
        n = raw_execute(q, args)
        if q.startswith("UPDATE"):
            sets = q.split(" SET ")[1].split(" WHERE ")[0].split(" ORDER BY")[0]
            cols = "+".join(sorted(a.split("=")[0].strip() for a in sets.split(", ")))
            tab = q.split()[2 if q.startswith("UPDATE IGNORE") else 1]
            hit = cover.setdefault((tab, cols), [0, 0])
            hit[0] += 1
            hit[1] += bool(n)
        return n

    cur.execute = execute
    # Q21_FORCE=ApplyDelta forces the substrate policy onto the kernel's
    # incremental path (the calibrated MySQL cost model otherwise rebuilds).
    force = os.environ.get("Q21_FORCE", "")
    cur.execute("SELECT mysql_accel_set_force_maintenance_action(%s)", (force,))
    cur.fetchall()
    setup(cur, rng)
    nxt = [1000]
    stats = {"writes": 0, "queries": 0, "mismatch": 0, "nonempty": 0}
    actions = {}
    errors = []
    for i in range(writes):
        write(cur, rng, nxt)
        stats["writes"] += 1
        cur.execute("SELECT mysql_accel_get_last_maintenance_action()")
        act = cur.fetchone()[0]
        act = act.decode() if isinstance(act, bytes) else act
        actions[act] = actions.get(act, 0) + 1
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
    for (tab, cols), (n, eff) in sorted(cover.items()):
        print(f"  update {tab:9s} {cols:70s} {n:4d} statements, {eff:4d} changed rows")
    print("q21 verus mysql stress:", stats)
    print("q21 verus mysql stress: last maintenance action after each write (force=%r):" % force, actions)
    ok = stats["mismatch"] == 0
    if force and force != "policy":
        # (the Q21 kernel has no partitions: RebuildPartition is a full rebuild, module.cpp)
        ok = ok and actions.get("RebuildAll" if force == "RebuildPartition" else force, 0) > 0
    print("q21 verus mysql stress:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
