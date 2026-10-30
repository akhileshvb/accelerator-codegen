#!/usr/bin/env python3
"""Postgres read-only sweep: plain Postgres vs the stateful (aux) accelerators, per TPC-H query (SF20) or, with
--workload sec_edgar, per SEC-EDGAR query.  Reuses eval/compare/run_sql_backend.py (connections, answer comparison)
and run_compare.py (parameter draws) from the repo read-only; the baseline text is workloads/tpch/qN.sql (the kit
text used by that module is gone), or for SEC-EDGAR the $n baseline of eval/compare/sec_edgar_params.py.

Per query: stop the shard workers (so the build is cold), then for REPEATS fresh random draws x SETS parameter sets:
  base  : plain SQL (one untimed warm-up run on the very first case of the query; then BASE_REPS timed runs, median)
  accel : SELECT * FROM accel_<workload>_qN_aux(args) (--system cpp) or accel_<workload>_qN_vgen(args) (--system verus):
          the first call of the query builds the state; every case then has one untimed result call (compared with
          the base answer) and REPS timed calls (median).
  --system verus serves the Verus kits from one world store that builds all the workload's kits: the shard workers
  are stopped once, the first call of every query waits (large accel.read_wait_ms / read_timeout_ms) until its kit is
  served, and each timed call must be a hit (a fallback is counted by cause).
"""
import argparse, csv, json, math, os, re, statistics, subprocess, sys, time
os.environ["PYTHONDONTWRITEBYTECODE"] = "1"; sys.dont_write_bytecode = True
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(REPO, "eval", "compare"))
import run_sql_backend as rsb  # noqa: E402
import run_compare  # noqa: E402
import sec_edgar_params  # noqa: E402


def kit_sql(q):
    t = open(os.path.join(REPO, "workloads", "tpch", f"q{q}.sql")).read()
    return "\n".join(l for l in t.splitlines() if not l.lstrip().startswith("--")).strip().rstrip(";")


DEFAULT_QUERIES = {"tpch": range(1, 23), "sec_edgar": (1, 2, 3, 4, 6, 24)}
SUFFIX = {"cpp": "aux", "verus": "vgen"}
WAIT_GUCS = ("accel.read_wait_ms", "accel.read_timeout_ms")


def first_call(db, sql):
    """The first call of a query on a cold worker builds the state and is answered from SQL meanwhile; a query without
    an SQL answer (SEC-EDGAR Q3) fails instead, so it is retried (once a second) until the state is served."""
    rows, us, status = rsb.timed(db, sql)
    waited_us = 0.0
    while "no SQL fallback" in status and waited_us < 1800e6:
        time.sleep(1)
        waited_us += 1e6
        rows, us, status = rsb.timed(db, sql)
    return rows, us + waited_us, status


def exec_counts(db):
    rows, _ = db.query("SELECT accel_get_execute_counts()")
    return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", rows[0][0])}


def served(db, sql, timeout_s):
    """Calls sql until one is a hit (no fallback counted): the Verus world answers once its kit is built."""
    t0 = time.time()
    while True:
        f0 = exec_counts(db).get("fallback", 0)
        rows, us, status = rsb.timed(db, sql)
        if status != "ok" or exec_counts(db).get("fallback", 0) == f0:
            return rows, us, status
        if time.time() - t0 > timeout_s:
            return rows, us, "not served"
        time.sleep(5)


def worker_rss_mb(pid):
    """Resident / peak resident MB of a shard worker (read in the server container)."""
    out = subprocess.run(["docker", "exec", os.environ["T"], "grep", "-E", "VmRSS|VmHWM", f"/proc/{pid}/status"],
                         capture_output=True, text=True).stdout if pid else ""
    return {k: round(int(v) / 1024, 1) for k, v in re.findall(r"(VmRSS|VmHWM):\s+(\d+)", out)}


def stats(db):
    try:
        rows, _ = db.query("SELECT accel_shard_stats()")
        t = str(rows[0][0])
        return {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", t)} | {"text": t}
    except Exception as e:  # noqa
        return {"text": "error " + str(e)[:100]}


def spread(xs, fmt="%.2f"):
    if not xs: return "-"
    return (fmt + " [" + fmt + "-" + fmt + "]") % (statistics.median(xs), min(xs), max(xs))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default="accel_tpch_sf20"); ap.add_argument("--port", type=int, default=int(os.environ.get("PG_PORT", 5460)))
    ap.add_argument("--workload", choices=("tpch", "sec_edgar"), default="tpch")
    ap.add_argument("--system", choices=tuple(SUFFIX), default="cpp")
    ap.add_argument("--out", required=True); ap.add_argument("--queries")
    ap.add_argument("--repeats", type=int, default=5); ap.add_argument("--sets", type=int, default=8)
    ap.add_argument("--reps", type=int, default=5); ap.add_argument("--base-reps", type=int, default=1)
    ap.add_argument("--seed", type=int, default=None); ap.add_argument("--sf", type=float, default=20)
    ap.add_argument("--query-timeout", type=float, default=1800)
    ap.add_argument("--keep-workers", action="store_true", help="--system verus: do not stop the shard workers first (a world already built is reused)")
    ap.add_argument("--wait-build", action="store_true",
                    help="before timing, trigger the build with one untimed call and wait until the shard worker reports it "
                         "built and synced (build = that wall time). Needed when plain SQL is faster than the build: the "
                         "runtime then falls back instead of waiting (WaitWorthIt), so 'first call builds' does not hold.")
    ap.add_argument("--force-workers", action="store_true",
                    help="set parallel_workers = N (the core count) on the TPC-H tables for every plain-Postgres run "
                         "(its planner otherwise sizes workers from table size: <= 7 at SF20); restored on exit")
    ap.add_argument("--pg-best-plan", action="store_true",
                    help="per query, time plain Postgres once (untimed, on the first case) under each of "
                         "{own worker sizing, parallel_workers = N on the tables} x {default, zero parallel_setup/tuple "
                         "costs}, each capped at the best time so far, and use the fastest for every timed baseline run "
                         "(forcing N workers everywhere makes Q17/Q18 time out at 80 cores)")
    a = ap.parse_args()
    a.backend = "postgres"; a.host = "127.0.0.1"; a.user = "postgres"; a.password = "postgres"
    a.pg_so = "/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so"; a.schema = "public"; a.pg_workers = int(os.environ.get("N", "8"))
    rsb.kit_sql = sec_edgar_params.baseline_sql if a.workload == "sec_edgar" else kit_sql
    a.queries = [int(x) for x in a.queries.split()] if a.queries else list(DEFAULT_QUERIES[a.workload])
    os.makedirs(a.out, exist_ok=True)
    if a.force_workers or a.pg_best_plan:
        saved = a.saved_reloptions = rsb.table_parallel_workers(a)
        open(os.path.join(a.out, "reloptions_saved.json"), "w").write(json.dumps(saved) + "\n")
        if not a.pg_best_plan:
            print("table parallel_workers now", rsb.table_parallel_workers(a, a.pg_workers), flush=True)
        try:
            return run(a)
        finally:
            print("table parallel_workers restored", rsb.table_parallel_workers(a, saved), flush=True)
    return run(a)


PLANS = [("own", "default"), ("own", "zero"), ("forced", "default"), ("forced", "zero")]


def apply_plan(a, db, plan):
    workers, costs = plan
    rsb.table_parallel_workers(a, a.pg_workers if workers == "forced" else a.saved_reloptions)
    for g in ("parallel_setup_cost", "parallel_tuple_cost"):
        db.cur.execute(f"SET {g} = 0" if costs == "zero" else f"RESET {g}")


def choose_plan(a, db, sql):
    """-> (plan, {plan: seconds or None}). The first plan runs twice (warm-up); later ones are capped at the best so far."""
    tried = {}; best = None; best_us = None
    for plan in PLANS:
        apply_plan(a, db, plan)
        if best is None:
            rsb.timed(db, sql)
        else:
            db.set_timeout(min(a.query_timeout, best_us / 1e6 * 1.05 + 1))
        _, us, st = rsb.timed(db, sql)
        db.set_timeout(a.query_timeout)
        tried["/".join(plan)] = round(us / 1e6, 3) if st == "ok" else st
        if st == "ok" and (best_us is None or us < best_us):
            best, best_us = plan, us
    best = best or PLANS[0]
    apply_plan(a, db, best)
    return best, tried


def planned_workers(db, sql):
    db.cur.execute("EXPLAIN " + sql)
    return [int(m) for (l,) in db.cur.fetchall() for m in re.findall(r"Workers Planned: (\d+)", l)]


def build_world(a, reps):
    """Verus world: stop the workers once, then wait until every query's kit is served (the world builds all kits in
    one worker); -> seconds from the first request to each query's first hit.  Nothing is timed meanwhile."""
    if not a.keep_workers:
        rsb.restart_shared_worker(a)
    db = rsb.Db(a, accel=True)
    db.set_timeout(8 * 3600)
    for g in WAIT_GUCS:
        db.cur.execute(f"SET {g} = 3600000")  # the GUC maximum (1 h); served() retries
    ready = {}; t0 = time.time()
    for q in a.queries:
        s0 = reps[0][2][str(q)]["sets"][0]
        _, _, status = served(db, f'SELECT * FROM accel_{a.workload}_q{q}_vgen({s0["verus"]})', 8 * 3600)
        ready[str(q)] = round(time.time() - t0, 1)
        print(f"q{q} kit served after {ready[str(q)]}s ({status})", flush=True)
    st = stats(db)
    print("world built:", st["text"], worker_rss_mb(st.get("pid")), flush=True)
    db.close()
    return ready


def run(a):
    reps = run_compare.draws(argparse.Namespace(seed=a.seed, repeats=a.repeats, sets=a.sets, sf=a.sf, out=a.out,
                                                workload=a.workload, queries=a.queries))
    csvf = open(os.path.join(a.out, "cases.csv"), "a", newline="")
    w = csv.writer(csvf)
    if csvf.tell() == 0:
        w.writerow("query rep seed set params base_us accel_us accel_first_us base_status accel_status match base_rows accel_rows".split())
    qf = open(os.path.join(a.out, "qstats.jsonl"), "a")
    world_ready_s = {}
    if a.system == "verus":
        world_ready_s = build_world(a, reps)
        json.dump(world_ready_s, open(os.path.join(a.out, "world_ready_s.json"), "w"))
    accel_name = lambda q: f"accel_{a.workload}_q{q}_{SUFFIX[a.system]}"
    for q in a.queries:
        t_q = time.time(); load0 = open('/proc/loadavg').read().split()[0]
        if a.system == "cpp":
            rsb.restart_shared_worker(a)
        db = rsb.Db(a, accel=True)
        db.query("SELECT accel_reset_execute_counts()")
        db.cur.execute(f'SET max_parallel_workers = {a.pg_workers}')  # the container's -c max_parallel_workers=48 beats ALTER SYSTEM
        s0 = stats(db)
        if q == a.queries[0]:
            db.cur.execute("select string_agg(name||'='||setting, ' ' order by name) from pg_settings where name in ('max_parallel_workers','max_parallel_workers_per_gather','accel.threads','accel.read_threads','accel.build_threads','accel.maint_threads','accel.shard_read_threads')")
            open(os.path.join(a.out,'session_gucs.txt'),'w').write(db.cur.fetchone()[0]+'\n')
        build_us = None; first = True
        s_first = reps[0][2][str(q)]["sets"][0]
        pg_plan = plans_tried = None
        if a.pg_best_plan:
            pg_plan, plans_tried = choose_plan(a, db, rsb.base_sql(q, s_first["verus"], "postgres"))
            print(f"q{q} postgres plan {'/'.join(pg_plan)} (tried {plans_tried})", flush=True)
        workers_planned = planned_workers(db, rsb.base_sql(q, s_first["verus"], "postgres"))
        if a.wait_build:
            k0, seed0, spec0 = reps[0]; s_0 = spec0[str(q)]["sets"][0]
            t0 = time.time()
            rsb.timed(db, f'SELECT * FROM {accel_name(q)}({s_0[a.system]})')  # untimed: starts the shard worker and its build
            while time.time() - t0 < a.query_timeout:
                st = stats(db)
                if st.get("builds", 0) >= 1 and st.get("synced", 0) >= 1:
                    break
                time.sleep(0.2)
            build_us = (time.time() - t0) * 1e6
            s_prebuild, s0 = s0, stats(db)  # fallbacks/requests are counted from here: the trigger call may fall back
        for k, seed, spec in reps:
            for i, s in enumerate(spec[str(q)]["sets"]):
                asql, bsql = f'SELECT * FROM {accel_name(q)}({s[a.system]})', rsb.base_sql(q, s["verus"], "postgres")
                if first:
                    rsb.timed(db, bsql)  # untimed warm-up of the baseline
                brows, b1, bst = rsb.timed(db, bsql)
                tb = [b1]
                if bst == "ok":
                    tb += [rsb.timed(db, bsql)[1] for _ in range(a.base_reps - 1)]
                arows, first_us, ast = (first_call if first else rsb.timed)(db, asql)  # the first call of the query builds the state
                match = nb = na = None
                if bst == "ok" and ast == "ok":
                    cb, ca = rsb.canon("postgres", brows, False), rsb.canon("postgres", arows, True)
                    match = rsb.same(cb, ca); nb, na = len(cb), len(ca)
                    if not match:
                        with open(os.path.join(a.out, f"mismatch_q{q}_r{k}_s{i}.json"), "w") as f:
                            json.dump({"params": s["params"], "base": cb[:60], "accel": ca[:60]}, f, default=str, indent=1)
                ta = [rsb.timed(db, asql)[1] for _ in range(a.reps)] if ast == "ok" else []
                if first and ta and not a.wait_build and a.system == "cpp":
                    build_us = first_us - statistics.median(ta)
                first = False
                w.writerow([q, k, seed, i, json.dumps(s["params"]), statistics.median(tb) if bst == "ok" else "",
                            statistics.median(ta) if ta else "", first_us, bst, ast, match, nb, na])
                csvf.flush()
            print(f"q{q} rep {k} done {time.time() - t_q:.0f}s", flush=True)
        s1 = stats(db)
        qf.write(json.dumps({"query": q, "system": a.system, "build_us": build_us, "stats_before": s0, "stats_after": s1,
                             "exec_counts": exec_counts(db), "fallback_causes": db.query("SELECT accel_fallback_causes()")[0][0][0],
                             "worker_rss_mb": worker_rss_mb(s1.get("pid")), "world_ready_s": world_ready_s.get(str(q)),
                             "stats_prebuild": s_prebuild if a.wait_build else None, "pg_workers_planned": workers_planned,
                             "pg_plan": "/".join(pg_plan) if pg_plan else None, "pg_plans_tried": plans_tried,
                             "wall_s": time.time() - t_q, "load_start": load0, "load_end": open("/proc/loadavg").read().split()[0]}) + "\n"); qf.flush()
        if a.pg_best_plan:
            apply_plan(a, db, PLANS[0])
        db.close()
    csvf.close()

if __name__ == "__main__":
    main()
