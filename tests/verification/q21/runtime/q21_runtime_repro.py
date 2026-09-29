#!/usr/bin/env python3
"""Live Postgres repros for the Q21 runtime protocol (E1 multi-backend,
E2 savepoints, E3 build inside a dirty / aborted transaction, E4 TRUNCATE,
E5 the C-collation guard).

Every check compares accel_tpch_q21_aux(nation) against the plain catalog SQL
evaluated in the same session/snapshot, and (where given) asserts whether the
call was served from the Aux store ("hit") or by the fallback scan, from the
runtime's DEBUG1 trace.  Exit 1 on any mismatch.
Needs the accel_q21_rt database (tests/verification/q21/runtime/setup_q21_rt_db.sh).
"""
import os
import sys
import psycopg2

DSN = os.environ.get("Q21_RT_DSN", "host=127.0.0.1 port=5434 user=postgres password=postgres dbname=accel_q21_rt")
SQL = """SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F'
AND l1.l_receiptdate > l1.l_commitdate
AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
AND s_nationkey = n_nationkey AND n_name = %s GROUP BY s_name ORDER BY numwait DESC, s_name"""
NATION = "SAUDI ARABIA"
fails = 0
next_ok = [1000]


def conn(autocommit=True):
    c = psycopg2.connect(DSN)
    c.autocommit = True
    c.cursor().execute("SET client_min_messages = debug1")
    c.autocommit = autocommit
    return c


def mode_of(c):
    modes = [n for n in c.notices if "tpch_q21" in n and ("aux hit" in n or "fallback" in n)]
    c.notices.clear()
    if not modes:
        return None
    return "hit" if "aux hit" in modes[-1] else "fallback"


def check(c, label, want_mode=None):
    global fails
    cur = c.cursor()
    c.notices.clear()
    cur.execute("SELECT s_name, numwait FROM accel_tpch_q21_aux(%s)", (NATION,))
    got = [tuple(r) for r in cur.fetchall()]
    mode = mode_of(c)
    cur.execute(SQL, (NATION,))
    want = [tuple(r) for r in cur.fetchall()]
    ok = got == want and (want_mode is None or mode == want_mode)
    fails += 0 if ok else 1
    print(f"  [{'ok' if ok else 'MISMATCH'}] {label} ({mode}): accel={got} sql={want}")


def contributing_order(c, supp_late=1, supp_other=2):
    """Insert an 'F' order whose only late supplier is supp_late (contributes 1)."""
    ok = next_ok[0]; next_ok[0] += 1
    cur = c.cursor()
    cur.execute("INSERT INTO orders VALUES (%s,1,'F',0,'1995-01-01','p','c',0,'c')", (ok,))
    cur.execute("INSERT INTO lineitem VALUES (%s,1,%s,1,1,1,0,0,'N','O','1995-01-01','1995-01-02','1995-01-05','x','x','c'),"
                "(%s,1,%s,2,1,1,0,0,'N','O','1995-01-01','1995-01-05','1995-01-02','x','x','c')",
                (ok, supp_late, ok, supp_other))
    return ok


def reset():
    c = conn()
    c.cursor().execute("DELETE FROM lineitem; DELETE FROM orders;")
    c.close()
    b = conn(); contributing_order(b); b.close()


def e1():
    print("E1 multi-backend: A builds, B commits a contributing order, A queries again")
    reset()
    a, b = conn(), conn()
    check(a, "A first call (build)", "hit")
    contributing_order(b)
    check(a, "A after B's commit", "hit")
    check(b, "B after its own commit", "hit")
    c = conn(); check(c, "fresh backend C", "hit"); c.close()
    a.close(); b.close()


def e1_inflight():
    print("E1b: B has an uncommitted write while A queries; then B commits")
    reset()
    a, b = conn(), conn(False)
    check(a, "A build", "hit")
    contributing_order(b)
    check(a, "A while B in flight", "fallback")
    b.commit()
    check(a, "A after B commit", "hit")
    a.close(); b.close()


def e2():
    print("E2 savepoints: rolled-back subtransaction write must not reach the Aux")
    reset()
    a = conn(False)
    check(a, "A build", "hit"); a.commit()
    cur = a.cursor()
    cur.execute("SAVEPOINT s1")
    contributing_order(a)
    cur.execute("ROLLBACK TO SAVEPOINT s1")
    a.commit()
    check(a, "A after commit (savepoint rolled back)", "hit"); a.commit()
    cur.execute("SAVEPOINT s2")
    contributing_order(a)
    cur.execute("RELEASE SAVEPOINT s2")
    a.commit()
    check(a, "A after commit (savepoint released)", "hit"); a.commit()
    b = conn(); check(b, "fresh backend", "hit"); b.close()
    a.close()


def e3():
    print("E3 first use inside a dirty transaction, then COMMIT / ROLLBACK")
    reset()
    a = conn(False)
    contributing_order(a)
    check(a, "A in dirty txn (first use)", "fallback")
    a.commit()
    check(a, "A after commit", "hit"); a.commit()
    b = conn(); check(b, "fresh backend"); b.close()
    a.close()
    reset()
    a = conn(False)
    contributing_order(a)
    check(a, "A in dirty txn (first use)", "fallback")
    a.rollback()
    check(a, "A after rollback", "hit"); a.commit()
    a.close()


def e4():
    print("E4 TRUNCATE in another backend, then in this backend")
    reset()
    a, b = conn(), conn()
    check(a, "A build", "hit")
    b.cursor().execute("TRUNCATE lineitem")
    check(a, "A after B's TRUNCATE", "hit")
    reset()
    check(a, "A after reload", "hit")
    a.autocommit = False
    a.cursor().execute("TRUNCATE lineitem")
    check(a, "A in own TRUNCATE txn", "fallback")
    a.commit()
    check(a, "A after own TRUNCATE commit", "hit")
    a.commit()
    a.close(); b.close()


def e5():
    global fails
    print("E5 non-C collation on supplier.s_name must fail closed")
    reset()
    a = conn()
    cur = a.cursor()
    cur.execute("SELECT datcollate FROM pg_database WHERE datname = current_database()")
    if cur.fetchone()[0] in ("C", "POSIX"):
        print("  [skip] database default collation is C; no non-C default to test")
        a.close()
        return
    cur.execute('ALTER TABLE supplier ALTER COLUMN s_name TYPE character varying COLLATE "default"')
    try:
        cur.execute("SELECT * FROM accel_tpch_q21_aux(%s)", (NATION,))
        fails += 1
        print("  [MISMATCH] accelerator ran on a non-C s_name collation")
    except psycopg2.errors.FeatureNotSupported as ex:
        print(f"  [ok] refused: {str(ex).splitlines()[0]}")
    finally:
        cur.execute('ALTER TABLE supplier ALTER COLUMN s_name TYPE character varying COLLATE "C"')
    check(a, "A after restoring C collation", "hit")
    a.close()


for t in (e1, e1_inflight, e2, e3, e4, e5):
    t()
print(f"q21 runtime repro: {'FAIL' if fails else 'PASS'} ({fails} mismatches)")
sys.exit(1 if fails else 0)
