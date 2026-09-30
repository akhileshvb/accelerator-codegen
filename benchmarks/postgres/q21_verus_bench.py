#!/usr/bin/env python3
"""TPC-H Q21 on PostgreSQL: base SQL vs the Verus-verified accelerator
(accel_tpch_q21_verus), with the existing C++ accelerator (accel_tpch_q21_aux)
as a reference point.

Read-only: every nation parameter; base = median of --base-reps warm runs,
accelerators = median of --acc-reps warm runs (the one-time build is reported
separately).

Under writes: one session interleaves k write transactions (k = 1, 10) with
one Q21 query on a random nation, for three write mixes:
  point   UPDATE one lineitem's l_receiptdate of an 'F' order (lateness flip)
  rf      TPC-H RF1/RF2 style: insert a new 'F' order with 1-7 lineitems, or
          delete an order inserted earlier (with its lineitems)
  mixed   random choice of the above
Every system replays the same seeded write sequence from the same starting
state (the database is restored after each run).  Base PostgreSQL runs with
the capture triggers disabled (plain PostgreSQL).  Accelerated runs keep the
store synced by maintaining it at each commit (same-session writer); after
each run the accelerator's answers are checked against the SQL.

Latencies are client-observed (psycopg2, localhost) and include fetching the
result rows.
"""
import argparse
import json
import random
import statistics
import subprocess
import time

import psycopg2

SQL = """SELECT s_name, COUNT(*) AS numwait FROM supplier, lineitem l1, orders, nation
WHERE s_suppkey = l1.l_suppkey AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F'
AND l1.l_receiptdate > l1.l_commitdate
AND EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey)
AND NOT EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND l3.l_receiptdate > l3.l_commitdate)
AND s_nationkey = n_nationkey AND n_name = %s GROUP BY s_name ORDER BY numwait DESC, s_name"""
FUNCS = {"verus": "accel_tpch_q21_verus", "cpp": "accel_tpch_q21_aux"}
TABLES = ["supplier", "orders", "lineitem", "nation"]
NEW_KEY_BASE = 900_000_000


def connect(args):
    c = psycopg2.connect(args.dsn)
    c.autocommit = True
    return c


def query(cur, system, nation):
    t = time.perf_counter()
    if system == "base":
        cur.execute(SQL, (nation,))
    else:
        cur.execute(f"SELECT s_name, numwait FROM {FUNCS[system]}(%s)", (nation,))
    rows = cur.fetchall()
    return (time.perf_counter() - t) * 1e3, rows


def backend_rss_anon_kb(cur, container):
    cur.execute("SELECT pg_backend_pid()")
    pid = cur.fetchone()[0]
    out = subprocess.run(["docker", "exec", container, "grep", "RssAnon", f"/proc/{pid}/status"],
                         capture_output=True, text=True).stdout
    return int(out.split()[1]) if out else -1


def set_triggers(args, enabled):
    c = connect(args)
    cur = c.cursor()
    for t in TABLES:
        cur.execute(f"ALTER TABLE {t} {'ENABLE' if enabled else 'DISABLE'} TRIGGER USER")
    c.close()


# ── read-only ───────────────────────────────────────────────────────────────
def read_only(args, nations):
    res = {}
    for system in ["base"] + args.accels:
        c = connect(args)
        cur = c.cursor()
        entry = {"per_nation_ms": {}}
        if system != "base":
            rss0 = backend_rss_anon_kb(cur, args.container)
            ms, _ = query(cur, system, nations[0])
            entry["cold_build_and_query_ms"] = ms
            entry["state_rss_anon_mb"] = (backend_rss_anon_kb(cur, args.container) - rss0) / 1024
        reps = args.base_reps if system == "base" else args.acc_reps
        for n in nations:
            query(cur, system, n)  # warm
            entry["per_nation_ms"][n] = statistics.median(query(cur, system, n)[0] for _ in range(reps))
        c.close()
        res[system] = entry
        print(f"read-only {system}: median over nations "
              f"{statistics.median(entry['per_nation_ms'].values()):.3f} ms", flush=True)
    return res


# ── writes ──────────────────────────────────────────────────────────────────
class WriteGen:
    """A deterministic write sequence (same for every system)."""

    def __init__(self, seed, f_orders, n_supp):
        self.rng = random.Random(seed)
        self.f_orders = f_orders
        self.n_supp = n_supp
        self.next_key = NEW_KEY_BASE
        self.inserted = []

    def op(self, mix):
        r = self.rng
        kind = mix if mix != "mixed" else r.choice(["point", "point", "rf"])
        if kind == "point":
            ok = r.choice(self.f_orders)
            return ("point", ok, r.choice([-5, 5]))
        if self.inserted and r.random() < 0.5:
            return ("rf2", self.inserted.pop(r.randrange(len(self.inserted))))
        k = self.next_key
        self.next_key += 1
        self.inserted.append(k)
        lines = [(r.randint(1, self.n_supp), r.choice([-3, 3])) for _ in range(r.randint(1, 7))]
        return ("rf1", k, lines)


def apply_op(cur, op):
    if op[0] == "point":
        cur.execute("UPDATE lineitem SET l_receiptdate = l_commitdate + %s "
                    "WHERE l_orderkey = %s AND l_linenumber = 1", (op[2], op[1]))
    elif op[0] == "rf1":
        _, k, lines = op
        cur.execute("INSERT INTO orders VALUES (%s, 1, 'F', 1000.00, DATE '1995-01-01', '1-URGENT', "
                    "'Clerk#000000001', 0, 'bench')", (k,))
        for ln, (sk, d) in enumerate(lines, 1):
            cur.execute("INSERT INTO lineitem VALUES (%s, 1, %s, %s, 1, 1000.00, 0.05, 0.02, 'R', 'F', "
                        "DATE '1995-01-10', DATE '1995-01-20', DATE '1995-01-20' + %s, "
                        "'NONE', 'MAIL', 'bench')", (k, sk, ln, d))
    else:
        cur.execute("DELETE FROM lineitem WHERE l_orderkey = %s", (op[1],))
        cur.execute("DELETE FROM orders WHERE o_orderkey = %s", (op[1],))


def snapshot_points(cur, keys):
    cur.execute("SELECT l_orderkey, l_receiptdate FROM lineitem WHERE l_linenumber = 1 "
                "AND l_orderkey = ANY(%s)", (list(keys),))
    return cur.fetchall()


def restore(args, points):
    c = connect(args)
    cur = c.cursor()
    cur.execute("DELETE FROM lineitem WHERE l_orderkey >= %s", (NEW_KEY_BASE,))
    cur.execute("DELETE FROM orders WHERE o_orderkey >= %s", (NEW_KEY_BASE,))
    for ok, d in points:
        cur.execute("UPDATE lineitem SET l_receiptdate = %s WHERE l_orderkey = %s AND l_linenumber = 1",
                    (d, ok))
    c.close()


def write_run(args, system, mix, k, nations, f_orders, n_supp, points):
    gen = WriteGen(args.seed * 100 + k, f_orders, n_supp)
    qrng = random.Random(args.seed * 7 + k)
    c = connect(args)
    cur = c.cursor()
    if system != "base":
        query(cur, system, nations[0])  # build outside the timed region
    wr_ms, q_ms = [], []
    t0 = time.perf_counter()
    for _ in range(args.rounds):
        for _ in range(k):
            op = gen.op(mix)
            t = time.perf_counter()
            c.autocommit = False
            apply_op(cur, op)
            c.commit()
            c.autocommit = True
            wr_ms.append((time.perf_counter() - t) * 1e3)
        ms, _ = query(cur, system, qrng.choice(nations))
        q_ms.append(ms)
    total = (time.perf_counter() - t0) * 1e3
    check = None
    if system != "base":
        check = True
        for n in nations[:3] + [qrng.choice(nations)]:
            _, a = query(cur, system, n)
            cur.execute(SQL, (n,))
            if a != cur.fetchall():
                check = False
    c.close()
    restore(args, points)
    return {"total_ms": total, "query_ms_mean": statistics.mean(q_ms),
            "query_ms_median": statistics.median(q_ms), "write_ms_mean": statistics.mean(wr_ms),
            "write_ms_median": statistics.median(wr_ms), "writes": len(wr_ms),
            "queries": len(q_ms), "matches_sql": check}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dsn", default="host=127.0.0.1 port=5434 user=postgres password=postgres dbname=accel_q21_bench")
    ap.add_argument("--container", default="accel_pg_test")
    ap.add_argument("--accels", default="verus,cpp")
    ap.add_argument("--base-reps", type=int, default=3)
    ap.add_argument("--acc-reps", type=int, default=30)
    ap.add_argument("--rounds", type=int, default=20)
    ap.add_argument("--ratios", default="1,10")
    ap.add_argument("--mixes", default="point,rf,mixed")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--skip-read-only", action="store_true")
    ap.add_argument("--out", default="q21_verus_bench.json")
    args = ap.parse_args()
    args.accels = [a for a in args.accels.split(",") if a]

    c = connect(args)
    cur = c.cursor()
    cur.execute("SELECT n_name FROM nation ORDER BY n_nationkey")
    nations = [r[0] for r in cur.fetchall()]
    cur.execute("SELECT o_orderkey FROM orders WHERE o_orderstatus = 'F' ORDER BY o_orderkey")
    all_f = [r[0] for r in cur.fetchall()]
    f_orders = random.Random(args.seed).sample(all_f, 5000)
    cur.execute("SELECT count(*) FROM supplier")
    n_supp = cur.fetchone()[0]
    points = snapshot_points(cur, f_orders)
    cur.execute("SELECT version()")
    version = cur.fetchone()[0]
    settings = {}
    for s in ["max_parallel_workers_per_gather", "jit", "shared_buffers", "work_mem"]:
        cur.execute(f"SHOW {s}")
        settings[s] = cur.fetchone()[0]
    cur.execute("SELECT count(*) FROM lineitem")
    n_li = cur.fetchone()[0]
    c.close()
    restore(args, [])

    out = {"postgres": version, "settings": settings, "lineitem_rows": n_li, "nations": nations}
    if not args.skip_read_only:
        out["read_only"] = read_only(args, nations)
    out["writes"] = {}
    for mix in args.mixes.split(","):
        for k in [int(x) for x in args.ratios.split(",")]:
            key = f"{mix}/k={k}"
            out["writes"][key] = {}
            for system in ["base"] + args.accels:
                if system == "base":
                    set_triggers(args, False)
                try:
                    r = write_run(args, system, mix, k, nations, f_orders, n_supp, points)
                finally:
                    if system == "base":
                        set_triggers(args, True)
                out["writes"][key][system] = r
                print(f"writes {key} {system}: total {r['total_ms']:.0f} ms, "
                      f"query mean {r['query_ms_mean']:.3f} ms, write mean {r['write_ms_mean']:.3f} ms, "
                      f"matches_sql={r['matches_sql']}", flush=True)
    with open(args.out, "w") as f:
        json.dump(out, f, indent=1)
    print("wrote", args.out)


if __name__ == "__main__":
    main()
