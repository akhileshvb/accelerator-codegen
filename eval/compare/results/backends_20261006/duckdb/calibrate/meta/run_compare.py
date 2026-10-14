#!/usr/bin/env python3
"""C++ aux accelerators vs the Verus kits on TPC-H (benchmarks/compare/aux_compare.cpp).

  read        per query and system: build time, then per TPC-H parameter set the
              median latency of the accelerator and of DuckDB, and whether the
              answers agree.  Systems: cpp (accel_tpch_qN, the C++ store),
              verus (accel_tpch_qN_vgen, ACCEL_AUX_STORE=verus_global).
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

Both compare against the same DuckDB query: the TPC-H text the Verus kits
implement, with the same parameters (gen_tpch_params.py).  Known differences:
the C++ Q18 accelerator keeps LIMIT 100 and the C++ Q21 accelerator drops it, so
their answers are compared as sets and may report a row-count mismatch.

  python3 benchmarks/compare/run_compare.py read --queries "1 3 6" --sets 4
  python3 benchmarks/compare/run_compare.py concurrent --readers 8 --seconds 60 --wps 5
  python3 benchmarks/compare/run_compare.py multi --readers 8 --seconds 120 --wps 5
  python3 benchmarks/compare/run_compare.py summary <out_dir>
Env defaults: DB=<repo>/../accelerator-codegen-duckdb/tpch_sf20.duckdb  WORK=/flash1/akhilvb/tpch/work (each run works on a copy of DB in WORK)
"""
import argparse
import json
import math
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, "benchmarks", "compare")
# verus: the one Verus world store for every accelerator; verus_kit: the same
# kits, each on its own Verus store (the control for the global store's cost)
SYSTEMS = {"cpp": {}, "verus": {"ACCEL_AUX_STORE": "verus_global"}, "verus_kit": {"ACCEL_AUX_STORE": "verus"}}


def params(sets, seed, sf=20):
    out = subprocess.run([sys.executable, os.path.join(HERE, "gen_tpch_params.py"), str(sets), str(seed), str(sf)],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)["queries"]


def draws(a):
    """One parameter draw per repetition: (rep, seed, params).  Seeds are base+rep; the base is
    --seed or a fresh random one, recorded in <out>/seeds.json.  Within a repetition every system
    gets the same draw (paired); across repetitions the draws differ."""
    base = a.seed if a.seed is not None else int.from_bytes(os.urandom(4), "little")
    out = [(k, base + k, params(a.sets, base + k, a.sf)) for k in range(a.repeats)]
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
            f.write((s["cpp"] if system == "cpp" else s["verus"]) + "\t" + s["verus"] + "\n")
    return path


def env_for(system, q):
    env = dict(os.environ)
    env.pop("ACCEL_AUX_STORE", None)
    env.update(SYSTEMS.get(system, {}))
    env["ACCEL_AUX_DIAG"] = "0"  # no per-read diagnostics sampling
    model = (os.path.join(ROOT, "config", "verus", f"q{q}_aux_cost_model_sf20.json") if system.startswith("verus")
             else os.path.join(ROOT, "config", f"q{q}_aux_cost_model_sf20.json"))
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
    copy = os.path.join(work, name)
    for p in (copy, copy + ".wal"):
        if os.path.exists(p):
            os.remove(p)
    shutil.copyfile(db, copy)
    return copy


def drop(path):
    for p in (path, path + ".wal"):
        if os.path.exists(p):
            os.remove(p)


def ids(q, system):
    return (f"tpch:{q}" if system == "cpp" else f"tpch_q{q}_vgen"), f"tpch_q{q}_vgen"


def cmd_read(a):
    """Read-only: per (query, system) ONE process builds the accelerator and answers every
    repetition's parameter sets (all draws in one cases file); the result is split back into
    one record per repetition, with its own geomean speedup, under read_q<N>_<system>_r<k>.json."""
    log = os.path.join(a.out, "read.log")
    reps = draws(a)
    for q in a.queries:
        for system in a.systems:
            accel, base = ids(q, system)
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
            r = run([a.bin, "read", db, accel, base, path, str(a.reps)], env_for(system, q), log, a.timeout)
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


def cmd_concurrent(a):
    log = os.path.join(a.out, "concurrent.log")
    phases = [("base", "base")] + [(s, "accel") for s in a.systems]
    for k, seed, spec in draws(a):
        for q in a.queries:
            for name, phase in phases:
                system = "cpp" if name == "base" else name
                accel, base = ids(q, system)
                cases = cases_file(a.work, q, "verus" if name == "base" else system, spec[str(q)], k)
                db = fresh_copy(a.db, a.work, f"cmp_conc_q{q}_{name}.duckdb")
                print(f"concurrent q{q} {name} rep {k} {time.strftime('%T')}", flush=True)
                r = run([a.bin, "concurrent", db, accel, base, cases, phase, str(a.readers), str(a.seconds),
                         str(a.wps), str(seed), str(a.warmup)], env_for(system, q), log, a.timeout)
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


def cmd_multi(a):
    """Every listed accelerator live in ONE process; one writer maintains all of them per commit.

    Per system one process: the accelerators are built once, then one run per
    (write rate, repetition), each repetition with its own parameter draw (the
    same draw for every system), results under <out>/<rate>/rep<k>/multi_<system>.json."""
    log = os.path.join(a.out, "multi.log")
    rates = [float(x) for x in str(a.wps).replace(",", " ").split()]
    reps = draws(a)
    for name, phase in [("base", "base")] + [(s, "accel") for s in a.systems]:
        system = "cpp" if name == "base" else name
        lines = []
        for q in a.queries:
            accel, base = ids(q, system)
            files = [cases_file(a.work, q, "verus" if name == "base" else system, spec[str(q)], k)
                     for k, _, spec in reps]
            lines.append(f"{accel}\t{base}\t{'|'.join(files)}\n")
        spec_path = os.path.join(a.work, f"multi_{name}.tsv")
        with open(spec_path, "w") as f:
            f.writelines(lines)
        db = fresh_copy(a.db, a.work, f"cmp_multi_{name}.duckdb")
        print(f"multi {name} ({len(a.queries)} accelerators, {a.reader_mode} readers, rates "
              f"{' '.join(rate_name(w) for w in rates)}, {a.repeats} repetition(s)) {time.strftime('%T')}", flush=True)
        env = env_for(system, a.queries[0])
        env.pop("ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL", None)  # one process, many accelerators: defaults
        if a.duckdb_memory:
            env["ACCEL_COMPARE_DUCKDB_MEMORY"] = a.duckdb_memory
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
        for name in ("base", "cpp", "verus_kit", "verus"):
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
    for mode in ("read", "concurrent", "multi"):
        p = sub.add_parser(mode)
        p.add_argument("--db", default=os.environ.get("DB", os.path.join(ROOT, "..", "accelerator-codegen-duckdb", "tpch_sf20.duckdb")))
        p.add_argument("--work", default=os.environ.get("WORK", "/flash1/akhilvb/tpch/work"))
        p.add_argument("--out", default=os.path.join(ROOT, "benchmarks", "compare", "results",
                                                     f"{mode}_{time.strftime('%Y%m%d_%H%M%S')}"))
        p.add_argument("--bin", default=os.path.join(ROOT, "build-verus-duckdb", "aux_compare"))
        p.add_argument("--queries", default=" ".join(map(str, range(1, 23))))
        p.add_argument("--systems", default="cpp verus")
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
    c = sub.choices["concurrent"]
    c.add_argument("--readers", type=int, default=8)
    c.add_argument("--seconds", type=float, default=60)
    c.add_argument("--wps", type=float, default=5, help="writer target, writes per second")
    c.add_argument("--warmup", type=float, default=5)
    m = sub.choices["multi"]
    m.add_argument("--readers", type=int, default=8)
    m.add_argument("--seconds", type=float, default=120)
    m.add_argument("--wps", default="5", help="writer target(s), writes per second: one rate or a list "
                   "(\"5 10 20 40 -1\"; -1 uncapped); the accelerators are built once per system")
    m.add_argument("--duckdb-memory", default="16GB", help="DuckDB memory_limit in the multi process ('' for DuckDB's default)")
    m.add_argument("--reader-mode", choices=("rotate", "pinned"), default="rotate",
                   help="rotate: each reader cycles over the accelerators; pinned: --readers per accelerator")
    m.add_argument("--warmup", type=float, default=5)
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
    s = sub.add_parser("summary")
    s.add_argument("out")
    a = ap.parse_args()
    if a.mode == "summary":
        return summary(a.out)
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
    {"read": cmd_read, "concurrent": cmd_concurrent, "multi": cmd_multi}[a.mode](a)


if __name__ == "__main__":
    main()
