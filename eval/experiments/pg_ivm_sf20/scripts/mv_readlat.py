"""Read-only latency of the current MV design (and optionally the base query): per query, the seed's --sets
parameter sets, 1 untimed warm call + --reps timed calls each; prints/records the median per set and per query.

  MV_DESIGN=... python3 mv_readlat.py --queries "1 2" --out lat.json [--base]
"""
import argparse, json, os, statistics, sys, time, types
sys.path.insert(0, os.path.join(os.environ["REPO"], "eval/compare"))
import mv_defs, run_sql_backend as R, run_compare

ap = argparse.ArgumentParser()
ap.add_argument("--queries", default=" ".join(map(str, range(1, 23))))
ap.add_argument("--sets", type=int, default=4)
ap.add_argument("--reps", type=int, default=5)
ap.add_argument("--seed", type=int, default=int(os.environ.get("SEED", 35489017)))
ap.add_argument("--workers", type=int, default=int(os.environ.get("PGW", 4)))
ap.add_argument("--out", required=True)
ap.add_argument("--base", action="store_true", help="also time the base query (and compare answers)")
a = ap.parse_args()
qs = [int(x) for x in a.queries.split()]
d = types.SimpleNamespace(seed=a.seed, repeats=1, sets=a.sets, sf=20, queries=qs, out=os.path.dirname(os.path.abspath(a.out)))
_, _, spec = run_compare.draws(d)[0]
ns = types.SimpleNamespace(db=os.environ.get("PG_DB", "accel_tpch_sf20"), workers=a.workers)
c, cur = mv_defs.connect(ns, "public")
cur.execute("SET work_mem = '256MB'")  # the run's setting, not the build's 1GB


def timed(sql):
    cur.execute(sql); rows = cur.fetchall()  # warm
    ts = []
    for _ in range(a.reps):
        t = time.perf_counter_ns(); cur.execute(sql); cur.fetchall(); ts.append((time.perf_counter_ns() - t) / 1e6)
    return rows, statistics.median(ts)


res = {"design": os.environ.get("MV_DESIGN", "v1"), "workers": a.workers, "reps": a.reps, "q": {}}
for q in qs:
    per = []
    for s in spec[str(q)]["sets"]:
        m_rows, m_ms = timed(mv_defs.mv_sql(q, s["verus"]))
        e = {"params": s["params"], "mv_ms": m_ms}
        if a.base:
            b_rows, b_ms = timed(R.base_sql(q, s["verus"], "postgres"))
            e["base_ms"] = b_ms
            e["ok"] = R.same(R.canon("postgres", b_rows, False), R.canon("postgres", m_rows, False))
        per.append(e)
    med = statistics.median(e["mv_ms"] for e in per)
    res["q"][q] = {"mv_ms_median": med, "sets": per}
    print(f"Q{q:<3} mv {med:10.2f} ms" + (f"  base {statistics.median(e['base_ms'] for e in per):10.1f} ms  ok={all(e['ok'] for e in per)}" if a.base else ""), flush=True)
json.dump(res, open(a.out, "w"), indent=1, default=str)
