#!/usr/bin/env python3
"""TPC-H Q21 on MySQL 8.4: base SQL vs the Verus-verified accelerator
(mysql_accel_tpch_q21_verus), with the C++ accelerator (mysql_accel_tpch_q21_aux)
as a reference point.  Mirrors benchmarks/postgres/q21_verus_bench.py.

Setting: the accel_mysql_sf1 container, database accel_mysql_test, with
s_name / n_name / o_orderstatus in utf8mb4_0900_bin (the kernel's text
semantics; the accelerator answers with the SQL itself otherwise).

The MySQL accelerators are process-wide and capture writes through triggers,
so systems are isolated by phase: (1) base MySQL with the capture triggers
dropped, (2) the Verus accelerator, (3) mysqld restart, the C++ accelerator.
Accelerated writes run with the substrate policy forced to ApplyDelta (as the
repository's MySQL write-mix sweep does; the calibrated cost model otherwise
rebuilds on every write).  Writes are autocommit statements; a write
transaction below is one logical operation (one or more statements).

Latencies are client-observed (pymysql, localhost).  Results in JSON.
"""
import argparse
import json
import random
import statistics
import subprocess
import time

import pymysql

SQL = """SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F'
AND l1.l_receiptdate > l1.l_commitdate
AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
AND s_nationkey = n_nationkey AND n_name = %s GROUP BY s_name ORDER BY numwait DESC, s_name"""
FUNCS = {"verus": "mysql_accel_tpch_q21_verus", "cpp": "mysql_accel_tpch_q21_aux"}
TABLES = ["lineitem", "orders", "supplier", "nation"]
NEW_KEY_BASE = 900_000_000
CONTAINER = "accel_mysql_sf1"


def connect(args):
    return pymysql.connect(host="127.0.0.1", port=args.port, user="root", password="mysql",
                           database="accel_mysql_test", autocommit=True, charset="utf8mb4")


def query(cur, system, nation):
    t = time.perf_counter()
    if system == "base":
        cur.execute(SQL, (nation,))
        rows = [(r[0], int(r[1])) for r in cur.fetchall()]
    else:
        cur.execute(f"SELECT {FUNCS[system]}(%s)", (nation,))
        text = cur.fetchone()[0]
        text = text.decode() if isinstance(text, bytes) else (text or "")
        rows = []
        for line in text.split("\n"):
            if line:
                name, n = line.rsplit("|", 1)
                rows.append((name, int(n)))
    return (time.perf_counter() - t) * 1e3, rows


def restart(args):
    subprocess.run(["docker", "restart", CONTAINER], check=True, capture_output=True)
    for _ in range(90):
        r = subprocess.run(["docker", "exec", CONTAINER, "mysqladmin", "-uroot", "-pmysql", "ping"],
                           capture_output=True, text=True)
        if "alive" in r.stdout:
            break
        time.sleep(1)
    time.sleep(2)


def drop_triggers(args):
    c = connect(args)
    cur = c.cursor()
    for t in TABLES:
        for k in "idu":
            cur.execute(f"DROP TRIGGER IF EXISTS accel_codegen_tpch_{t}_{k}")
    c.close()


def create_triggers(args):
    subprocess.run(["docker", "exec", CONTAINER, "bash", "-c",
                    "mysql -uroot -pmysql accel_mysql_test < /accel-src/scripts/mysql_accel_mutation_triggers.sql"],
                   check=True, capture_output=True)


# ── writes ──────────────────────────────────────────────────────────────────
class WriteGen:
    def __init__(self, seed, f_orders, n_supp):
        self.rng = random.Random(seed)
        self.f_orders, self.n_supp = f_orders, n_supp
        self.next_key, self.inserted = NEW_KEY_BASE, []

    def op(self, mix):
        r = self.rng
        kind = mix if mix != "mixed" else r.choice(["point", "point", "rf"])
        if kind == "point":
            return ("point", r.choice(self.f_orders), r.choice([-5, 5]))
        if self.inserted and r.random() < 0.5:
            return ("rf2", self.inserted.pop(r.randrange(len(self.inserted))))
        k = self.next_key
        self.next_key += 1
        self.inserted.append(k)
        return ("rf1", k, [(r.randint(1, self.n_supp), r.choice([-3, 3])) for _ in range(r.randint(1, 7))])


def apply_op(cur, op):
    if op[0] == "point":
        cur.execute("UPDATE lineitem SET l_receiptdate = l_commitdate + INTERVAL %s DAY "
                    "WHERE l_orderkey = %s AND l_linenumber = 1", (op[2], op[1]))
    elif op[0] == "rf1":
        _, k, lines = op
        cur.execute("INSERT INTO orders VALUES (%s, 1, 'F', 1000.00, '1995-01-01', '1-URGENT', "
                    "'Clerk#000000001', 0, 'bench')", (k,))
        for ln, (sk, d) in enumerate(lines, 1):
            cur.execute("INSERT INTO lineitem VALUES (%s, 1, %s, %s, 1, 1000.00, 0.05, 0.02, 'R', 'F', "
                        "'1995-01-10', '1995-01-20', '1995-01-20' + INTERVAL %s DAY, 'NONE', 'MAIL', 'bench')",
                        (k, sk, ln, d))
    else:
        cur.execute("DELETE FROM lineitem WHERE l_orderkey = %s", (op[1],))
        cur.execute("DELETE FROM orders WHERE o_orderkey = %s", (op[1],))


def restore(args, points):
    c = connect(args)
    cur = c.cursor()
    cur.execute("DELETE FROM lineitem WHERE l_orderkey >= %s", (NEW_KEY_BASE,))
    cur.execute("DELETE FROM orders WHERE o_orderkey >= %s", (NEW_KEY_BASE,))
    for ok, d in points:
        cur.execute("UPDATE lineitem SET l_receiptdate = %s WHERE l_orderkey = %s AND l_linenumber = 1 "
                    "AND l_receiptdate <> %s", (d, ok, d))
    c.close()


def write_run(args, system, mix, k, nations, f_orders, n_supp, points):
    gen = WriteGen(args.seed * 100 + k, f_orders, n_supp)
    qrng = random.Random(args.seed * 7 + k)
    c = connect(args)
    cur = c.cursor()
    if system != "base":
        cur.execute("SELECT mysql_accel_set_force_maintenance_action('ApplyDelta')")
        cur.fetchall()
        query(cur, system, nations[0])  # build outside the timed region
    wr, qs = [], []
    t0 = time.perf_counter()
    for _ in range(args.rounds):
        for _ in range(k):
            op = gen.op(mix)
            t = time.perf_counter()
            apply_op(cur, op)
            wr.append((time.perf_counter() - t) * 1e3)
        qs.append(query(cur, system, qrng.choice(nations))[0])
    total = (time.perf_counter() - t0) * 1e3
    check = None
    if system != "base":
        check = all(query(cur, system, n)[1] == query(cur, "base", n)[1] for n in nations[:3])
        cur.execute("SELECT mysql_accel_set_force_maintenance_action('')")
        cur.fetchall()
    c.close()
    restore(args, points)
    return {"total_ms": total, "query_ms_mean": statistics.mean(qs), "write_ms_mean": statistics.mean(wr),
            "writes": len(wr), "queries": len(qs), "matches_sql": check}


def read_only(args, system, nations):
    c = connect(args)
    cur = c.cursor()
    out = {"per_nation_ms": {}}
    if system != "base":
        out["cold_build_and_query_ms"] = query(cur, system, nations[0])[0]
        bad = 0
        for n in nations:
            if query(cur, system, n)[1] != query(cur, "base", n)[1]:
                bad += 1
        out["mismatching_nations"] = bad
    reps = args.base_reps if system == "base" else args.acc_reps
    for n in nations:
        query(cur, system, n)
        out["per_nation_ms"][n] = statistics.median(query(cur, system, n)[0] for _ in range(reps))
    c.close()
    print(f"read-only {system}: median over nations {statistics.median(out['per_nation_ms'].values()):.3f} ms"
          + (f", mismatching nations {out.get('mismatching_nations')}" if system != "base" else ""), flush=True)
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=3308)
    ap.add_argument("--base-reps", type=int, default=2)
    ap.add_argument("--acc-reps", type=int, default=30)
    ap.add_argument("--rounds", type=int, default=10)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--out", default="q21_verus_bench_mysql.json")
    args = ap.parse_args()

    restart(args)
    c = connect(args)
    cur = c.cursor()
    cur.execute("SELECT n_name FROM nation ORDER BY n_nationkey")
    nations = [r[0] for r in cur.fetchall()]
    cur.execute("SELECT DISTINCT o_orderkey FROM orders JOIN lineitem ON l_orderkey = o_orderkey "
                "AND l_linenumber = 1 WHERE o_orderstatus = 'F' ORDER BY o_orderkey")
    f_orders = random.Random(args.seed).sample([r[0] for r in cur.fetchall()], 5000)
    cur.execute("SELECT count(*) FROM supplier")
    n_supp = cur.fetchone()[0]
    ph = ",".join(["%s"] * len(f_orders))
    cur.execute(f"SELECT l_orderkey, l_receiptdate FROM lineitem WHERE l_linenumber = 1 AND l_orderkey IN ({ph})",
                f_orders)
    points = cur.fetchall()
    cur.execute("SELECT version()")
    version = cur.fetchone()[0]
    cur.execute("SELECT count(*) FROM lineitem")
    n_li = cur.fetchone()[0]
    c.close()
    out = {"mysql": version, "lineitem_rows": n_li, "nations": nations, "read_only": {}, "writes": {}}
    configs = [(m, k) for m in ["point", "rf", "mixed"] for k in [1, 10]]

    # phase 1: base MySQL (capture triggers dropped)
    drop_triggers(args)
    try:
        out["read_only"]["base"] = read_only(args, "base", nations)
        for m, k in configs:
            r = write_run(args, "base", m, k, nations, f_orders, n_supp, points)
            out["writes"].setdefault(f"{m}/k={k}", {})["base"] = r
            print(f"writes {m}/k={k} base: {r}", flush=True)
    finally:
        create_triggers(args)
    # phases 2-3: accelerators, one per mysqld lifetime
    for system in ["verus", "cpp"]:
        restart(args)
        out["read_only"][system] = read_only(args, system, nations)
        for m, k in configs:
            r = write_run(args, system, m, k, nations, f_orders, n_supp, points)
            out["writes"][f"{m}/k={k}"][system] = r
            print(f"writes {m}/k={k} {system}: {r}", flush=True)
    with open(args.out, "w") as f:
        json.dump(out, f, indent=1)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
