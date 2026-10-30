#!/usr/bin/env python3
"""C++ aux accelerators vs the Verus kits on TPC-H (eval/compare/aux_compare.cpp).

  read        per query and system: build time, then per TPC-H parameter set the
              median latency of the accelerator and of DuckDB, and whether the
              answers agree.  Systems: cpp (accel_tpch_qN, the C++ store),
              verus (accel_tpch_qN_vgen, ACCEL_AUX_STORE=verus_global),
              verus_cpp (accel_tpch_qN_vgen on the C++ store, ACCEL_AUX_STORE=cpp).
  concurrent  per query: R reader threads (closed loop over the parameter sets)
              and one writer at W writes/s, three runs on fresh copies of the
              database: base (DuckDB, plain SQL writes), cpp, verus.  The writer
              commits TPC-H refresh mini-chunks (RF2 delete / RF1 insert of one
              order with its lineitems) with a point write to a dimension table
              every fifth write, rotating over supplier, customer, part(+partsupp),
              supplier(+partsupp), partsupp, lineitem and orders.  Each accel run
              must see at least one write that makes the accelerator maintain
              (invalidate) its state; the summary flags any that did not.

  multi       the accelerators (default: all 22) live in
              ONE process, as in a database with many accelerators; readers
              rotate over them; each writer commit maintains all of them.  C++
              keeps one lock per accelerator; verus_global one store and lock
              for all, so unrelated accelerators' commits and reads meet.
              Runs base, cpp, verus on fresh copies of the database.

  tpch        the TPC-H performance test (spec 5.3): the 22 accelerators in one
              process, then per run a power test (RF1, the 22 queries one at a
              time in stream 0's order, RF2) and a throughput test (S query
              streams concurrently, each in its own order with its own
              parameters, and a refresh stream of S RF1 / RF2 pairs back to
              back); reports Power@Size, Throughput@Size and QphH@Size.

Both compare against the same DuckDB query: the TPC-H text the Verus kits
implement, with the same parameters (gen_tpch_params.py).  Known differences:
the C++ Q18 accelerator keeps LIMIT 100 and the C++ Q21 accelerator drops it, so
their answers are compared as sets and may report a row-count mismatch.

  python3 eval/compare/run_compare.py read --queries "1 3 6" --sets 4
  python3 eval/compare/run_compare.py read --workload sec_edgar --db <the sf3 database file> --systems cpp --queries "1 2 3 4 6 24"
  python3 eval/compare/run_compare.py refresh --queries "1 3" --sf 20 --pairs 2
  python3 eval/compare/run_compare.py concurrent --readers 8 --seconds 60 --wps 5
  python3 eval/compare/run_compare.py multi --readers 8 --seconds 120 --wps 5
  python3 eval/compare/run_compare.py tpch --streams 3 --runs 3 --systems "cpp verus"
  python3 eval/compare/run_compare.py summary <out_dir>
read also runs the SEC-EDGAR accelerators (--workload sec_edgar, systems cpp and verus): parameters from
eval/bench/sec_edgar/params, baseline text from the sec_edgar_params module beside this file.
Env defaults: DB=<repo>/../accelerator-codegen-duckdb/tpch_sf20.duckdb  WORK=/flash1/akhilvb/tpch/work (each run works on a copy of DB in WORK)
"""
import argparse
import json
import math
import os
import statistics
import subprocess
import sys
import time

import sec_edgar_params

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, "eval", "compare")
# verus: the one Verus world store for every accelerator; verus_kit: the same
# kits, each on its own Verus store (the control for the global store's cost);
# verus_cpp: the same kits on the C++ store protocol (one lock per accelerator)
SYSTEMS = {"cpp": {}, "verus": {"ACCEL_AUX_STORE": "verus_global"}, "verus_kit": {"ACCEL_AUX_STORE": "verus"},
           "verus_cpp": {"ACCEL_AUX_STORE": "cpp"}, "stateless": {}}
# stateless: the query's stateless kernel (accel_tpch_qN, no maintained state); concurrent and multi modes


def params(sets, seed, sf=20, workload="tpch", queries=()):
    if workload == "sec_edgar":
        return {str(q): sec_edgar_params.draw(q, sets, seed) for q in queries}
    out = subprocess.run([sys.executable, os.path.join(HERE, "gen_tpch_params.py"), str(sets), str(seed), str(sf)],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)["queries"]


def draws(a):
    """One parameter draw per repetition: (rep, seed, params).  Seeds are base+rep; the base is
    --seed or a fresh random one, recorded in <out>/seeds.json.  Within a repetition every system
    gets the same draw (paired); across repetitions the draws differ."""
    base = a.seed if a.seed is not None else int.from_bytes(os.urandom(4), "little")
    workload, queries = getattr(a, "workload", "tpch"), getattr(a, "queries", ())
    out = [(k, base + k, params(a.sets, base + k, a.sf, workload, queries)) for k in range(a.repeats)]
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "seeds.json"), "w") as f:
        json.dump({"base_seed": base, "repeats": a.repeats, "sets": a.sets, "sf": a.sf,
                   "seeds": [seed for _, seed, _ in out]}, f, indent=1)
    print(f"parameter draws: {a.repeats} repetition(s) x {a.sets} sets, seeds {base}..{base + a.repeats - 1}",
          flush=True)
    return out


def cases_file(work, q, system, spec, rep=0):
    path = os.path.join(work, f"cases_q{q}_{system}_r{rep}.tsv")
    with open(path, "w") as f:
        for s in spec["sets"]:
            f.write((s["cpp"] if system in ("cpp", "stateless") else s["verus"]) + "\t" + s["verus"] + "\n")
    return path


def env_for(system, q, sf=20, workload="tpch"):
    """The environment of a process for one query's accelerator: its store, and the cost model calibrated at this
    scale (config[/verus]/q<N>_aux_cost_model_sf<SF>.json) when there is one, else the runtime's default."""
    env = dict(os.environ)
    env.pop("ACCEL_AUX_STORE", None)
    env.update(SYSTEMS.get(system, {}))
    env["ACCEL_AUX_DIAG"] = "0"  # no per-read diagnostics sampling
    model = os.path.join(ROOT, "accelerators", workload, f"q{q}", "config", *(["verus"] if system.startswith("verus") else []),
                         f"q{q}_aux_cost_model_sf{sf:g}.json")
    if os.path.exists(model):
        env["ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL"] = model
    return env


def run(cmd, env, log, timeout, lines=1):
    t0 = time.time()
    with open(log, "a") as err:
        err.write("$ " + " ".join(cmd) + "\n")
        err.flush()
        try:
            p = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=err, text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return {"error": f"timeout after {timeout}s"}
    if p.returncode != 0:
        return {"error": f"exit {p.returncode} (see {log})"}
    try:
        rs = [json.loads(x) for x in p.stdout.strip().splitlines()[-lines:]]
        assert len(rs) == lines
    except (ValueError, AssertionError):
        return {"error": f"no JSON output (see {log})"}
    for r in rs:
        r["wall_s"] = round(time.time() - t0, 1)
    return rs[0] if lines == 1 else rs


def fresh_copy(db, work, name):
    """A private copy of db: a reflink where the filesystem supports it (XFS), else a full copy.  It is read once, so
    every run starts with the file in the page cache, as a database much smaller than RAM always does."""
    copy = os.path.join(work, name)
    drop(copy)
    subprocess.run(["cp", "--reflink=auto", db, copy], check=True)
    buf = bytearray(64 << 20)
    with open(copy, "rb", buffering=0) as f:
        while f.readinto(buf):
            pass
    return copy


def drop(path):
    for p in (path, path + ".wal"):
        if os.path.exists(p):
            os.remove(p)


def ids(q, system, sql_dir=None, workload="tpch"):
    """The accelerator and the baseline of query q; the baseline is its Verus kit's text, or sql_dir/q<N>.sql."""
    base = os.path.join(sql_dir, f"q{q}.sql") if sql_dir else f"{workload}_q{q}_vgen"
    return (f"{workload}:{q}" if system in ("cpp", "stateless") else f"{workload}_q{q}_vgen"), base


def cmd_read(a):
    """Read-only: per (query, system) ONE process builds the accelerator and answers every
    repetition's parameter sets (all draws in one cases file); the result is split back into
    one record per repetition, with its own geomean speedup, under read_q<N>_<system>_r<k>.json."""
    log = os.path.join(a.out, "read.log")
    reps = draws(a)
    sql_dir = a.baseline_sql_dir
    if a.workload == "sec_edgar":  # the baseline text is generated from the parameter specs
        sql_dir = sec_edgar_params.write_baselines(os.path.join(a.work, "sec_edgar_baseline"), a.queries)
    for q in a.queries:
        for system in a.systems:
            accel, base = ids(q, system, sql_dir, a.workload)
            path = os.path.join(a.work, f"cases_q{q}_{system}_all.tsv")
            spans = []  # (rep, seed, first case, number of cases)
            with open(path, "w") as f:
                for k, seed, spec in reps:
                    sets = spec[str(q)]["sets"]
                    spans.append((k, seed, sum(n for _, _, _, n in spans), len(sets)))
                    for st in sets:
                        f.write((st["cpp"] if system == "cpp" else st["verus"]) + "\t" + st["verus"] + "\n")
            # reads do not write; one copy per run keeps the WAL and catalog untouched
            db = fresh_copy(a.db, a.work, f"cmp_read_q{q}_{system}.duckdb") if a.copy else a.db
            print(f"read q{q} {system} ({len(reps)} draws) {time.strftime('%T')}", flush=True)
            r = run([a.bin, "read", db, accel, base, path, str(a.reps)], env_for(system, q, a.sf, a.workload), log, a.timeout)
            if a.copy:
                drop(db)
            for k, seed, first, n in spans:
                out = dict(r, query=q, system=system, rep=k, seed=seed, note=reps[k][2][str(q)]["note"])
                if "error" not in r:
                    cases = r["cases"][first:first + n]
                    logs = [math.log(c["base_us"] / max(c["accel_us"], 1.0)) for c in cases]
                    out.update(cases=cases, speedup_geomean=math.exp(sum(logs) / len(logs)) if logs else 0,
                               mismatches=sum(0 if c["match"] else 1 for c in cases),
                               # the process's counts cover every draw: carried by rep 0 only, so a sum
                               # over repetitions counts them once
                               hits=r["hits"] if k == 0 else 0, fallbacks=r["fallbacks"] if k == 0 else 0,
                               counts_cover_all_draws=True)
                with open(os.path.join(a.out, f"read_q{q}_{system}_r{k}.json"), "w") as f:
                    json.dump(out, f, indent=1)
                print(f"  rep {k}: " + line_read(out), flush=True)
    summary(a.out)


def cmd_refresh(a):
    """Maintenance cost: per query, one process builds the accelerator and applies --pairs RF2 + RF1 commits of
    --batch-orders orders each (default 0.1% of the orders, as the specification's refresh functions: the update
    sets of --rf-dir, else clones of existing orders), on a fresh copy of the database; result under refresh_q<N>.json.  --force-action pins the maintenance action
    (e.g. ApplyDelta: the policy would otherwise rebuild when it predicts that is cheaper)."""
    log = os.path.join(a.out, "refresh.log")
    os.makedirs(a.out, exist_ok=True)
    if not a.rf_dir:
        os.environ.setdefault("ACCEL_RF_BATCH_ORDERS", str(round(1500 * a.sf)))
    os.environ["ACCEL_RF_ONLY"] = "1"
    if a.force_action:
        os.environ["ACCEL_FORCE_MAINTENANCE_ACTION"] = a.force_action
    for q in a.queries:
        db = fresh_copy(a.db, a.work, f"cmp_refresh_q{q}.duckdb")
        print(f"refresh q{q} {time.strftime('%T')}", flush=True)
        r = run([a.bin, "refresh", db, f"tpch:{q}", str(a.pairs), "1"], env_for("cpp", q, a.sf), log, a.timeout)
        drop(db)
        r.update(query=q)
        with open(os.path.join(a.out, f"refresh_q{q}.json"), "w") as f:
            json.dump(r, f, indent=1)
        if "error" in r:
            print("  ERROR " + r["error"], flush=True)
            continue
        c = r["commits"]
        print(f"  build {r['build_ms'] / 1000:.1f}s  state {r['state_mb']:.0f}MB  actions {sorted({x['action'] for x in c})}  "
              f"maintenance/commit median {statistics.median(x['pre_commit_us'] for x in c) / 1000:.1f}ms "
              f"max {max(x['pre_commit_us'] for x in c) / 1000:.1f}ms", flush=True)


def cmd_concurrent(a):
    log = os.path.join(a.out, "concurrent.log")
    phases = [("base", "base")] * (not a.no_base) + [(s, "stateless" if s == "stateless" else "accel") for s in a.systems]
    for k, seed, spec in draws(a):
        for q in a.queries:
            for name, phase in phases:
                system = "cpp" if name == "base" else name
                accel, base = ids(q, system)
                cases = cases_file(a.work, q, "verus" if name == "base" else system, spec[str(q)], k)
                db = fresh_copy(a.db, a.work, f"cmp_conc_q{q}_{name}.duckdb")
                print(f"concurrent q{q} {name} rep {k} {time.strftime('%T')}", flush=True)
                r = run([a.bin, "concurrent", db, accel, base, cases, phase, str(a.readers), str(a.seconds),
                         str(a.wps), str(seed), str(a.warmup)], env_for(system, q, a.sf), log, a.timeout)
                drop(db)
                r.update({"query": q, "system": name, "rep": k, "seed": seed})
                with open(os.path.join(a.out, f"conc_q{q}_{name}_r{k}.json"), "w") as f:
                    json.dump(r, f, indent=1)
                print("  " + line_conc(r), flush=True)
    summary(a.out)


def rate_name(w):
    return "uncapped" if w < 0 else f"wps{w:g}"


def multi_line(r):
    if "error" in r:
        return "ERROR " + r["error"]
    return (f"qps {r['qps']:.0f}  writes {r['writes']} ({r['writes_per_sec']:.1f}/s of {r['writes_per_sec_target']:g}) "
            f"commit p50 {r['commit_p50_us'] / 1000:.1f}ms p99 {r['commit_p99_us'] / 1000:.1f}ms max "
            f"{r['max_commit_us'] / 1000:.0f}ms  errors read {r['read_errors']} write {r['write_errors']}")


def multi_dir(a, rates, w, k):
    d = os.path.join(a.out, rate_name(w)) if len(rates) > 1 else a.out
    return os.path.join(d, f"rep{k}") if a.repeats > 1 else d


def many_env(a, system):
    """The environment of a process holding many accelerators (multi, tpch): each picks its own cost model."""
    env = env_for(system, a.queries[0], a.sf)
    env.pop("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL", None)  # one process, many accelerators: no single model file
    if system.startswith("verus"):  # the Verus kits' own models (config/verus/qN_*), as env_for picks per query
        env["ACCEL_AUX_COST_MODEL_DIR"] = "verus"  # <bench>/q<N>/config/verus
    # each accelerator's runtime then picks accelerators/tpch/q<N>/config/q<N>_aux_cost_model_sf<SF>.json when ACCEL_AUX_BENCH_DB_LABEL is
    # tpch_sf<SF> (the models calibrated at this scale) and it exists, else the default accelerators/tpch/q<N>/config/q<N>_aux_cost_model.json
    label = os.environ.get("ACCEL_AUX_BENCH_DB_LABEL") or f"tpch_sf{a.sf:g}"
    env["ACCEL_AUX_BENCH_DB_LABEL"] = label
    cdir = lambda q: os.path.join(ROOT, "accelerators", "tpch", f"q{q}", "config", *(["verus"] if system.startswith("verus") else []))
    scale = label[5:] if label.startswith("tpch_sf") else ""
    have = [q for q in a.queries if scale and os.path.exists(os.path.join(cdir(q), f"q{q}_aux_cost_model_{scale}.json"))]
    print(f"  cost models: {len(have)}/{len(a.queries)} calibrated at {scale or '?'} "
          f"({os.path.relpath(cdir(0), ROOT).replace('q0', 'qN')}/qN_aux_cost_model_{scale}.json), the rest default", flush=True)
    if a.duckdb_memory:
        env["ACCEL_COMPARE_DUCKDB_MEMORY"] = a.duckdb_memory
    if a.duckdb_threads:
        env["ACCEL_COMPARE_DUCKDB_THREADS"] = str(a.duckdb_threads)
    return env


def cmd_tpch(a):
    """The TPC-H performance test (power test, then throughput test; spec 5.3) with the 22 accelerators in ONE process.

    Per system one process: the accelerators are built once (the load), then --runs runs back to back on the same
    database and state, as the specification's Run 1, Run 2.  Each query stream gets its own parameter draw (as qgen
    gives each stream its own seed) and its own query order (qgen's stream permutations); the refresh functions are the
    dbgen update sets.  Results: <out>/tpch_<system>_r<k>.json, and the summary."""
    log = os.path.join(a.out, "tpch.log")
    per_run = a.streams + 1
    base = a.seed if a.seed is not None else int.from_bytes(os.urandom(4), "little")
    seeds = [base + i for i in range(a.runs * per_run)]  # draw i: run i // (S+1), stream i % (S+1)
    drawn = [params(1, s, a.sf) for s in seeds]
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, "seeds.json"), "w") as f:
        json.dump({"base_seed": base, "runs": a.runs, "streams": a.streams, "sf": a.sf, "seeds": seeds,
                   "duckdb_threads": a.duckdb_threads, "rf_dir": os.environ["ACCEL_RF_DIR"]}, f, indent=1)
    print(f"TPC-H performance test: {a.runs} run(s), power + throughput with {a.streams} streams, "
          f"parameter seeds {base}..{seeds[-1]}", flush=True)
    for name, phase in [("base", "base")] * (not a.no_base) + [(s, "stateless" if s == "stateless" else "accel")
                                                                 for s in a.systems]:
        system = "cpp" if name == "base" else name
        lines = []
        for q in range(1, 23):
            accel, base_kit = ids(q, system, a.baseline_sql_dir or None)
            path = os.path.join(a.work, f"tpch_cases_q{q}_{name}.tsv")
            with open(path, "w") as f:
                for spec in drawn:
                    st = spec[str(q)]["sets"][0]
                    f.write((st["cpp"] if system in ("cpp", "stateless") else st["verus"]) + "\t" + st["verus"] + "\n")
            lines.append(f"{accel}\t{base_kit}\t{path}\n")
        spec_path = os.path.join(a.work, f"tpch_{name}.tsv")
        with open(spec_path, "w") as f:
            f.writelines(lines)
        db = fresh_copy(a.db, a.work, f"cmp_tpch_{name}.duckdb")
        print(f"tpch {name} {time.strftime('%T')}", flush=True)
        env = many_env(a, system)
        rs = run([a.bin, "tpch", db, phase, spec_path, str(a.streams), str(a.runs), f"{a.sf:g}", "1" if a.check else "0"],
                 env, log, a.timeout, lines=a.runs)
        drop(db)
        rs = rs if isinstance(rs, list) else [rs] * a.runs
        for k, r in enumerate(rs):
            r = dict(r, system=name, run=k, seeds=seeds[k * per_run:(k + 1) * per_run])
            with open(os.path.join(a.out, f"tpch_{name}_r{k}.json"), "w") as f:
                json.dump(r, f, indent=1)
            if k == 0 and "build_ms" in r and phase == "accel":
                print(f"  build (load) {r['build_ms'] / 1000:.0f}s", flush=True)
            print(f"  run {k}: " + tpch_line(r), flush=True)
    summary_tpch(a.out)


def tpch_line(r):
    if "error" in r:
        return "ERROR " + r["error"]
    rf = " ".join(f"{x['kind']} {x['us'] / 1e6:.2f}s" for x in r["power_refresh"])
    s = (f"Power@Size {r['power_at_size']:.0f} ({rf}, slowest query {max(r['power_query_us']) / 1e6:.2f}s)  "
         f"Throughput@Size {r['throughput_at_size']:.0f} (Ts {r['ts_s']:.1f}s: queries {r['query_streams_s']:.1f}s, "
         f"refresh {r['refresh_stream_s']:.1f}s)  QphH@Size {r['qphh_at_size']:.0f}")
    if r.get("power_mismatches"):
        s += f"  MISMATCH q{r['power_mismatches']}"
    if r.get("errors"):
        s += f"  ERRORS {r['errors']}"
    return s


def summary_tpch(out):
    runs = {}
    for name in sorted(os.listdir(out)):
        if name.startswith("tpch_") and name.endswith(".json"):
            r = json.load(open(os.path.join(out, name)))
            if "error" not in r:
                runs.setdefault(r["system"], []).append(r)
    if not runs:
        return
    order = [s for s in ("base", "cpp", "verus_cpp", "verus_kit", "verus", "stateless") if s in runs]
    n = max(len(v) for v in runs.values())
    print(f"\nTPC-H performance test, median [min-max] over {n} run(s); spec 5.4.3 reports the run with the lower QphH")
    print(f"{'system':>10} {'Power@Size':>24} {'Throughput@Size':>24} {'QphH@Size':>24} {'min QphH':>9} "
          f"{'RF1+RF2 s (power)':>18} {'Ts s':>16} {'load s':>7}")
    for s in order:
        rs = runs[s]
        print(f"{s:>10} {spread([r['power_at_size'] for r in rs], '%.0f'):>24} "
              f"{spread([r['throughput_at_size'] for r in rs], '%.0f'):>24} {spread([r['qphh_at_size'] for r in rs], '%.0f'):>24} "
              f"{min(r['qphh_at_size'] for r in rs):>9.0f} "
              f"{spread([sum(x['us'] for x in r['power_refresh']) / 1e6 for r in rs], '%.1f'):>18} "
              f"{spread([r['ts_s'] for r in rs], '%.1f'):>16} {rs[0]['build_ms'] / 1000:>7.0f}")
    print(f"\npower test, per query s (median over runs):")
    print(f"{'q':>4} " + " ".join(f"{s:>10}" for s in order))
    for q in range(22):
        print(f"{q + 1:>4} " + " ".join(f"{med([r['power_query_us'][q] for r in runs[s]]) / 1e6:>10.4f}" for s in order))
    for s in order:
        bad = [f"run {r['run']}: q{r['power_mismatches']}" for r in runs[s] if r.get("power_mismatches")]
        if bad:
            print(f"{s}: answers differing from DuckDB in the power test: " + "; ".join(bad))


def cmd_multi(a):
    """Every listed accelerator live in ONE process; one writer maintains all of them per commit.

    Per system one process: the accelerators are built once, then one run per
    (write rate, repetition), each repetition with its own parameter draw (the
    same draw for every system), results under <out>/<rate>/rep<k>/multi_<system>.json."""
    log = os.path.join(a.out, "multi.log")
    rates = [float(x) for x in str(a.wps).replace(",", " ").split()]
    reps = draws(a)
    for name, phase in [("base", "base")] * (not a.no_base) + [(s, "stateless" if s == "stateless" else "accel")
                                                                 for s in a.systems]:
        system = "cpp" if name == "base" else name
        lines = []
        for q in a.queries:
            accel, base = ids(q, system, a.baseline_sql_dir or None)
            files = [cases_file(a.work, q, "verus" if name == "base" else system, spec[str(q)], k)
                     for k, _, spec in reps]
            lines.append(f"{accel}\t{base}\t{'|'.join(files)}\n")
        spec_path = os.path.join(a.work, f"multi_{name}.tsv")
        with open(spec_path, "w") as f:
            f.writelines(lines)
        db = fresh_copy(a.db, a.work, f"cmp_multi_{name}.duckdb")
        print(f"multi {name} ({len(a.queries)} accelerators, {a.reader_mode} readers, rates "
              f"{' '.join(rate_name(w) for w in rates)}, {a.repeats} repetition(s)) {time.strftime('%T')}", flush=True)
        env = many_env(a, system)
        n = len(rates) * a.repeats
        rs = run([a.bin, "multi", db, phase, spec_path, str(a.readers), str(a.seconds), ",".join(f"{w:g}" for w in rates),
                  str(reps[0][1]), str(a.warmup), a.reader_mode], env, log, a.timeout, lines=n)
        drop(db)
        rs = rs if isinstance(rs, list) else [rs] * n  # an error: every run failed
        runs = [(w, k, seed) for w in rates for k, seed, _ in reps]  # the binary's order: rate-major
        for (w, k, seed), r in zip(runs, rs):
            r = dict(r, system=name, queries=a.queries, rate=rate_name(w), rep=k, seed=seed)
            d = multi_dir(a, rates, w, k)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, f"multi_{name}.json"), "w") as f:
                json.dump(r, f, indent=1)
            if "build_ms" in r and w == rates[0] and k == 0:
                print(f"  build {r['build_ms'] / 1000:.0f}s", flush=True)
            print(f"  {rate_name(w):>9} rep {k}: " + multi_line(r), flush=True)
    for w in rates:
        d = os.path.join(a.out, rate_name(w)) if len(rates) > 1 else a.out
        print(f"\n=== {rate_name(w)}")
        summary(d)


def cmd_cycle(a):
    """Per query, ONE accelerator alone in its own process (as `read`), the TPC-H refresh functions applied one commit at
    a time (RF1 of update set 1, RF2 of set 1, RF1 of set 2, ...), each followed by the point writes of the concurrent run, reads after every commit.
    Two runs per query and repetition on fresh copies of the database: base (plain SQL commits, DuckDB reads) and accel
    (commits through the accelerator: capture, maintenance, install; accelerator reads, answers checked against DuckDB).
    The speedup at any write interval is computed from the measured pieces by plot_cycle.py; results are one JSON per
    (query, system, repetition): <out>/cycle_q<N>_<base|accel>_r<k>.json."""
    log = os.path.join(a.out, "cycle.log")
    reps = draws(a)
    for k, seed, spec in reps:
        for q in a.queries:
            accel_id, base_kit = ids(q, a.system)
            cfile = cases_file(a.work, q, a.system, spec[str(q)], k)
            ncases = len(spec[str(q)]["sets"])
            for phase in ("base", "accel")[1 if a.no_base else 0:]:
                db = fresh_copy(a.db, a.work, f"cmp_cycle_{phase}.duckdb")
                env = env_for(a.system, q, a.sf)  # this query's cost model at this scale, if calibrated
                env["ACCEL_COMPARE_DUCKDB_MEMORY"] = a.duckdb_memory
                if a.duckdb_threads:
                    env["ACCEL_COMPARE_DUCKDB_THREADS"] = str(a.duckdb_threads)
                burst = 2 * ncases if phase == "base" else a.burst  # baseline reads are slow, and need no profile
                r = run([a.bin, "cycle", db, accel_id, base_kit, cfile, phase, str(a.pairs), str(burst), str(seed)],
                        env, log, a.timeout)
                drop(db)
                r = dict(r, query=q, system=phase, accel_system=a.system, rep=k, seed=seed, sf=a.sf,
                         threads=a.duckdb_threads, forced_action=os.environ.get("ACCEL_FORCE_MAINTENANCE_ACTION", ""))
                with open(os.path.join(a.out, f"cycle_q{q}_{phase}_r{k}.json"), "w") as f:
                    json.dump(r, f, indent=1)
                if "error" in r:
                    print(f"rep {k} q{q:>2} {phase}: ERROR {r['error']}", flush=True)
                elif phase == "base":
                    w = r["writes"]
                    rf = [x for x in w if x["kind"].startswith("rf")]
                    print(f"rep {k} q{q:>2} base : RF commits " + " ".join(f"{x['kind']} {x['commit_us'] / 1e6:.2f}s" for x in rf) +
                          f" | DuckDB read p50 {statistics.median(x['read_p50_us'] for x in rf) / 1e3:.1f}ms", flush=True)
                else:
                    w = r["writes"]
                    rf = [x for x in w if x["kind"].startswith("rf")]
                    pts = sum(x["commit_us"] for x in w if not x["kind"].startswith("rf")) / 1e6 / max(1, len(rf))
                    print(f"rep {k} q{q:>2} accel: build {r['build_ms'] / 1e3:.0f}s | " +
                          " ".join(f"{x['kind']} {x['commit_us'] / 1e6:.1f}s({x['action']})" for x in rf) +
                          f" | point writes {pts:.2f}s per slot | read p50 {statistics.median(x['read_p50_us'] for x in rf) / 1e3:.2f}ms"
                          f" | mismatches {r['mismatches']}", flush=True)


def line_read(r):
    if "error" in r:
        return "ERROR " + r["error"]
    return (f"build {r['build_ms'] / 1000:.1f}s  state {r.get('state_mb', 0) / 1024:.2f}GB  speedup(geomean) {r['speedup_geomean']:.1f}x  "
            f"mismatches {r['mismatches']}  hits {r['hits']} fallbacks {r['fallbacks']}")


def line_conc(r):
    if "error" in r:
        return "ERROR " + r["error"]
    s = (f"qps {r['qps']:.1f}  p50 {r['read_p50_us'] / 1000:.2f}ms p99 {r['read_p99_us'] / 1000:.2f}ms  "
         f"writes {r['writes']} ({r['writes_per_sec']:.1f}/s; {r.get('writes_total', '?')} incl. warmup) "
         f"commit p50 {r['commit_p50_us'] / 1000:.2f}ms p99 {r['commit_p99_us'] / 1000:.2f}ms "
         f"max {r.get('max_commit_us', 0) / 1000:.1f}ms")
    if "rows_touched_per_sec" in r:
        s += f"  slots {r['slots']} ({r['slots_per_sec']:.3g}/s) rows touched {r['rows_touched']} ({r['rows_touched_per_sec']:.0f}/s)"
    if r["phase"] == "accel":
        s += (f"  invalidating {r['invalidating_writes']}  maint {r['maintenance']}  "
              f"hits {r['hits']} fallbacks {r['fallbacks']}  slowest commit: {r.get('slowest_commit', '?')}")
    if r.get("read_errors") or r.get("write_errors"):
        s += f"  ERRORS read {r['read_errors']} write {r['write_errors']}"
    return s


def med(xs):
    xs = sorted(xs)
    n = len(xs)
    return None if not n else (xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2)


def spread(xs, fmt="%.1f"):
    """median [min-max] over repetitions (just the value for one)."""
    xs = [x for x in xs if x is not None]
    if not xs:
        return "-"
    if len(xs) == 1:
        return fmt % xs[0]
    return (fmt + " [" + fmt + "-" + fmt + "]") % (med(xs), min(xs), max(xs))


def load_multi(out):
    """system -> [result per repetition] from <out>/multi_*.json or <out>/rep*/multi_*.json."""
    dirs = sorted((os.path.join(out, d) for d in os.listdir(out) if d.startswith("rep")
                   and os.path.isdir(os.path.join(out, d))), key=lambda d: int(d.rsplit("rep", 1)[1])) or [out]
    runs = {}
    for d in dirs:
        for name in ("base", "cpp", "verus_cpp", "verus_kit", "verus"):
            p = os.path.join(d, f"multi_{name}.json")
            if os.path.exists(p):
                runs.setdefault(name, []).append(json.load(open(p)))
    return runs


def qkey(p):
    return p["accel"].replace("_vgen", "").split(":")[-1].replace("tpch_q", "")


def summary_multi(out):
    runs = load_multi(out)
    if not runs:
        return
    reps = max(len(v) for v in runs.values())
    print(f"\nmulti-accelerator (one process, one writer maintaining all): per-query read p50 ms, "
          f"median [min-max] over {reps} repetition(s) with different parameter draws")
    ok = {}
    for n, rs in runs.items():
        bad = [r for r in rs if "error" in r]
        for r in bad:
            print(f"  {n} rep {r.get('rep', '?')}: ERROR {r['error']}")
        if len(bad) < len(rs):
            ok[n] = [r for r in rs if "error" not in r]
    if not ok:
        return
    per = {n: {} for n in ok}
    for n, rs in ok.items():
        for r in rs:
            for p in r["per_accel"]:
                per[n].setdefault(qkey(p), []).append(p)
    names = list(ok)
    print(f"{'q':>4} " + " ".join(f"{n:>24}" for n in names))
    for q in sorted({k for d in per.values() for k in d}, key=lambda x: int(x)):
        cells = [spread([p["read_p50_us"] / 1000 for p in per[n].get(q, [])], "%.2f") for n in names]
        print(f"{q:>4} " + " ".join(f"{c:>24}" for c in cells))
    for n, rs in ok.items():
        print(f"totals {n}: qps {spread([r['qps'] for r in rs], '%.0f')}, writes/s "
              f"{spread([r['writes_per_sec'] for r in rs])}, commit p50 ms {spread([r['commit_p50_us'] / 1000 for r in rs])}, "
              f"p99 ms {spread([r['commit_p99_us'] / 1000 for r in rs])}")


def summary(out):
    summary_tpch(out)
    summary_multi(out)
    rows = {}
    for name in sorted(os.listdir(out)):
        if name.endswith(".json") and name.startswith(("read_", "conc_")):
            r = json.load(open(os.path.join(out, name)))
            kind = "read" if name.startswith("read_") else "conc"
            rows.setdefault((kind, r["query"]), {}).setdefault(r["system"], []).append(r)
    reads = sorted(k[1] for k in rows if k[0] == "read")
    if reads:
        reps = max(len(v) for k in rows if k[0] == "read" for v in rows[k].values())
        print(f"\nread-only: speedup over DuckDB (geomean over a draw's parameter sets), median [min-max] over "
              f"{reps} repetition(s) with different parameter draws; build s and state GB medians")
        print(f"{'q':>4} {'cpp':>22} {'verus':>22} {'cpp build':>10} {'verus build':>12} {'cpp GB':>7} "
              f"{'verus GB':>9}  notes")
        for q in reads:
            row = {s: [r for r in rs if "error" not in r] for s, rs in rows[("read", q)].items()}
            errs = [f"{s}: {len(rs) - len(row[s])} error" for s, rs in rows[("read", q)].items() if len(rs) > len(row[s])]
            sp = lambda s: spread([r["speedup_geomean"] for r in row.get(s, [])], "%.1fx") if row.get(s) else "-"
            m = lambda s, f: (f % med([g(r) for r in row[s]]) if row.get(s) else "-")
            notes = errs + [f"{s}: {sum(r['mismatches'] for r in rs)} mismatch" for s, rs in row.items()
                            if sum(r["mismatches"] for r in rs)]
            notes += [f"{s}: {sum(r['fallbacks'] for r in rs)} fallback" for s, rs in row.items()
                      if sum(r["fallbacks"] for r in rs)]
            g = lambda r: r["build_ms"] / 1000
            b_c, b_v = m("cpp", "%.1f"), m("verus", "%.1f")
            g = lambda r: r.get("state_mb", 0) / 1024
            s_c, s_v = m("cpp", "%.2f"), m("verus", "%.2f")
            print(f"{q:>4} {sp('cpp'):>22} {sp('verus'):>22} {b_c:>10} {b_v:>12} {s_c:>7} {s_v:>9}  {'; '.join(notes)}")
        for s in ("cpp", "verus"):
            per_rep = {}
            for q in reads:
                for r in rows[("read", q)].get(s, []):
                    if "error" not in r and r["speedup_geomean"] > 0:
                        per_rep.setdefault(r.get("rep", 0), []).append(r["speedup_geomean"])
            if per_rep:
                gm = [math.exp(sum(map(math.log, v)) / len(v)) for v in per_rep.values()]
                print(f"{s}: geomean speedup over queries, per repetition: {spread(gm, '%.0fx')}")
    concs = sorted(k[1] for k in rows if k[0] == "conc")
    if concs:
        print("\nconcurrent readers + writer: medians over repetitions -- reader QPS (p99 ms), commit p50 ms "
              "(slowest commit s), invalidating writes (summed)")
        print(f"{'q':>4} {'base qps':>16} {'cpp qps':>16} {'verus qps':>16} {'cpp commit':>14} {'verus commit':>14}"
              f" {'cpp inv':>8} {'verus inv':>10}")
        missing = []
        for q in concs:
            row = {s: [r for r in rs if "error" not in r] for s, rs in rows[("conc", q)].items()}

            def qps(s):
                rs = row.get(s)
                return "-" if not rs else "%.0f (%.1f)" % (med([r["qps"] for r in rs]),
                                                           med([r["read_p99_us"] for r in rs]) / 1000)

            def commit(s):
                rs = row.get(s)
                return "-" if not rs else "%.2f (%.1f)" % (med([r["commit_p50_us"] for r in rs]) / 1000,
                                                           max(r.get("max_commit_us", 0) for r in rs) / 1e6)

            inv = {s: sum(r["invalidating_writes"] for r in row.get(s, [])) for s in ("cpp", "verus")}
            for s in ("cpp", "verus"):
                if row.get(s) and inv[s] == 0:
                    missing.append(f"q{q} {s}")
            print(f"{q:>4} {qps('base'):>16} {qps('cpp'):>16} {qps('verus'):>16} {commit('cpp'):>14} "
                  f"{commit('verus'):>14} {inv['cpp'] if row.get('cpp') else '-':>8} {inv['verus'] if row.get('verus') else '-':>10}")
        if missing:
            print("NOT INVALIDATED (no write made the accelerator maintain): " + ", ".join(missing))
        else:
            print("every accelerator run saw at least one invalidating write")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    for mode in ("read", "refresh", "concurrent", "multi", "cycle", "tpch"):
        p = sub.add_parser(mode)
        p.add_argument("--db", default=os.environ.get("DB", os.path.join(ROOT, "..", "accelerator-codegen-duckdb", "tpch_sf20.duckdb")))
        p.add_argument("--work", default=os.environ.get("WORK", "/flash1/akhilvb/tpch/work"))
        p.add_argument("--out", default=os.path.join(ROOT, "eval", "compare", "results",
                                                     f"{mode}_{time.strftime('%Y%m%d_%H%M%S')}"))
        p.add_argument("--bin", default=os.path.join(ROOT, "build-verus-duckdb", "aux_compare"))
        p.add_argument("--queries", default=" ".join(map(str, range(1, 23))))
        p.add_argument("--systems", default="cpp verus")
        p.add_argument("--baseline-sql-dir", default=os.path.join(ROOT, "workloads", "tpch"),
                       help="the DuckDB baseline's queries, <dir>/q<N>.sql; '' = the Verus kits' own text (needs the kits built in)")
        p.add_argument("--sets", type=int, default=8, help="TPC-H parameter sets per query")
        p.add_argument("--seed", type=int, default=None,
                       help="base seed of the parameter draws (default: a fresh random one, recorded in seeds.json)")
        p.add_argument("--repeats", type=int, default=3,
                       help="repetitions, each with its own parameter draw (the same draw for every system)")
        p.add_argument("--sf", type=float, default=20, help="scale factor of --db (TPC-H Q11's fraction is 0.0001/SF)")
        p.add_argument("--timeout", type=int, default=4 * 3600, help="seconds per run")
    r = sub.choices["read"]
    r.add_argument("--reps", type=int, default=5)
    r.add_argument("--copy", action="store_true", help="run each on a fresh copy of the database")
    f = sub.choices["refresh"]
    f.add_argument("--pairs", type=int, default=2, help="RF2 + RF1 commit pairs")
    f.add_argument("--batch-orders", type=int, default=None,
                   help="orders per commit (default: the whole update set with --rf-dir, else 1500 x SF cloned orders)")
    f.add_argument("--rf-dir", default=None, help="the TPC-H update sets written by dbgen -U (scripts/gen_tpch_refresh_sets.sh)")
    f.add_argument("--force-action", default=None, help="pin the maintenance action (ApplyDelta, RebuildAll, ...)")
    c = sub.choices["concurrent"]
    c.add_argument("--readers", type=int, default=8)
    c.add_argument("--seconds", type=float, default=60)
    c.add_argument("--wps", type=float, default=5, help="writer target, writes per second")
    c.add_argument("--warmup", type=float, default=5)
    c.add_argument("--no-base", action="store_true",
                   help="skip the DuckDB baseline runs (pair with an earlier run's, same --seed and settings)")
    m = sub.choices["multi"]
    m.add_argument("--readers", type=int, default=8)
    m.add_argument("--seconds", type=float, default=120)
    m.add_argument("--wps", default="5", help="writer target(s), writes per second: one rate or a list "
                   "(\"5 10 20 40 -1\"; -1 uncapped); the accelerators are built once per system")
    m.add_argument("--duckdb-threads", type=int, default=None,
                   help="DuckDB's thread count (process-wide: the baseline queries and the accelerators' build / maintenance "
                        "scans); the accelerators' read kernels are single-threaded; default: all cores")
    m.add_argument("--duckdb-memory", default="16GB", help="DuckDB memory_limit in the multi process ('' for DuckDB's default)")
    m.add_argument("--reader-mode", choices=("rotate", "pinned"), default="rotate",
                   help="rotate: each reader cycles over the accelerators; pinned: --readers per accelerator")
    m.add_argument("--warmup", type=float, default=5)
    m.add_argument("--no-base", action="store_true",
                   help="skip the DuckDB baseline run (pair with an earlier run's, same --seed and settings)")
    for w in (c, m):
        w.add_argument("--batch-orders", type=int, default=None,
                       help="orders per RF1 / RF2 commit.  With --rf-dir: use only the first B orders of each update "
                            "set (default: the whole set, 1500 x SF orders).  Without it: clone B orders per commit "
                            "(default 1, the old mini-chunk).  Exported as ACCEL_RF_BATCH_ORDERS, recorded in every result")
        w.add_argument("--rf-dir", default=None,
                       help="directory of TPC-H update sets written by `dbgen -s SF -U N` (orders.tbl.uK, lineitem.tbl.uK, "
                            "delete.K): RF1 of set K inserts its orders, RF2 of set K deletes the listed ones (the "
                            "refresh functions of the specification).  Exported as ACCEL_RF_DIR")
    for w in (c, m):
        w.add_argument("--rf-only", action="store_true", help="the writer commits RF1 / RF2 only (no point writes); "
                       "exported as ACCEL_RF_ONLY")
        w.add_argument("--dim-after-rf", action="store_true",
                       help="every RF commit is followed by one point write of each kind on part / partsupp / supplier / "
                            "nation / region (the tables RF leaves alone), unpaced; exported as ACCEL_DIM_AFTER_RF")
        w.add_argument("--max-write-commits", type=int, default=0,
                       help="end the run after this many commits (calibration; exported as ACCEL_WRITE_MAX_COMMITS)")
    y = sub.choices["cycle"]
    y.add_argument("--pairs", type=int, default=2, help="RF1+RF2 pairs (update sets 1..N) per run")
    y.add_argument("--system", choices=("cpp", "verus", "verus_cpp", "verus_kit"), default="cpp",
                   help="the accelerator: the C++ one (accel_tpch_qN) or the Verus kit (tpch_qN_vgen) on that store")
    y.add_argument("--no-base", action="store_true", help="skip the DuckDB run (accel only)")
    y.add_argument("--burst", type=int, default=200, help="timed reads of the query after every commit (accelerator run)")
    y.add_argument("--rf-dir", default=None, required=True, help="the TPC-H update sets written by dbgen -U")
    y.add_argument("--batch-orders", type=int, default=None, help="use only the first B orders of each update set")
    y.add_argument("--duckdb-threads", type=int, default=None, help="DuckDB's thread count (the accelerators' read kernels are single-threaded)")
    y.add_argument("--duckdb-memory", default="8GB", help="DuckDB memory_limit")
    t = sub.choices["tpch"]
    t.add_argument("--streams", type=int, default=3,
                   help="query streams of the throughput test (the specification's minimum is 3 at SF10, 4 at SF30)")
    t.add_argument("--runs", type=int, default=3, help="performance-test runs (power + throughput) back to back, each "
                   "with its own parameter draws and update sets (the specification requires 2)")
    t.add_argument("--rf-dir", default="/flash1/akhilvb/tpch/refresh_sf20", help="the TPC-H update sets written by dbgen -U")
    t.add_argument("--duckdb-threads", type=int, default=None,
                   help="DuckDB's thread count (default: all cores, as a TPC-H run would; the accelerators' read kernels are single-threaded)")
    t.add_argument("--duckdb-memory", default="16GB", help="DuckDB memory_limit")
    t.add_argument("--no-base", action="store_true", help="skip the DuckDB run (pair with an earlier run's, same --seed)")
    t.add_argument("--no-check", dest="check", action="store_false",
                   help="do not compare the power test's accelerator answers with DuckDB's (untimed)")
    t.set_defaults(timeout=12 * 3600)
    sub.choices["read"].add_argument("--workload", choices=("tpch", "sec_edgar"), default="tpch",
                                     help="sec_edgar: --db is the SEC-EDGAR database; --baseline-sql-dir is ignored")
    s = sub.add_parser("summary")
    s.add_argument("out")
    a = ap.parse_args()
    if a.mode == "summary":
        return summary(a.out)
    if a.mode == "cycle":
        os.environ["ACCEL_DIM_AFTER_RF"] = "1"
    if a.mode == "tpch":  # the refresh functions only: no point writes
        os.environ["ACCEL_RF_ONLY"] = "1"
        os.environ.pop("ACCEL_DIM_AFTER_RF", None)  # a slot: an RF commit + the point writes after it (as in the concurrent run)
    if getattr(a, "max_write_commits", 0):
        os.environ["ACCEL_WRITE_MAX_COMMITS"] = str(a.max_write_commits)
    if getattr(a, "dim_after_rf", False):
        os.environ["ACCEL_DIM_AFTER_RF"] = "1"
    if getattr(a, "rf_only", False):
        os.environ["ACCEL_RF_ONLY"] = "1"
    if getattr(a, "rf_dir", None):
        os.environ["ACCEL_RF_DIR"] = os.path.abspath(a.rf_dir)
    if getattr(a, "batch_orders", None):
        os.environ["ACCEL_RF_BATCH_ORDERS"] = str(a.batch_orders)
    a.queries = [int(x) for x in a.queries.replace(",", " ").split()]
    a.systems = a.systems.replace(",", " ").split()
    assert all(s in SYSTEMS for s in a.systems), f"systems: {list(SYSTEMS)}"
    assert os.path.exists(a.bin), f"no {a.bin} (cmake --build build-verus-duckdb --target aux_compare)"
    assert os.path.exists(a.db), f"no database {a.db}"
    os.makedirs(a.work, exist_ok=True)
    {"read": cmd_read, "refresh": cmd_refresh, "concurrent": cmd_concurrent, "multi": cmd_multi, "cycle": cmd_cycle, "tpch": cmd_tpch}[a.mode](a)


if __name__ == "__main__":
    main()
