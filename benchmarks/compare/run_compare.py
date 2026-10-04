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

Both compare against the same DuckDB query: the TPC-H text the Verus kits
implement, with the same parameters (gen_tpch_params.py).  Known differences:
the C++ Q18 accelerator keeps LIMIT 100 and the C++ Q21 accelerator drops it, so
their answers are compared as sets and may report a row-count mismatch.

  python3 benchmarks/compare/run_compare.py read --queries "1 3 6" --sets 4
  python3 benchmarks/compare/run_compare.py concurrent --readers 8 --seconds 60 --wps 5
  python3 benchmarks/compare/run_compare.py summary <out_dir>
Env defaults: DB=/flash1/akhilvb/tpch/tpch_sf20.duckdb  WORK=/flash1/akhilvb/tpch/work
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HERE = os.path.join(ROOT, "benchmarks", "compare")
SYSTEMS = {"cpp": {}, "verus": {"ACCEL_AUX_STORE": "verus_global"}}


def params(sets, seed):
    out = subprocess.run([sys.executable, os.path.join(HERE, "gen_tpch_params.py"), str(sets), str(seed)],
                         check=True, capture_output=True, text=True).stdout
    return json.loads(out)["queries"]


def cases_file(work, q, system, spec):
    path = os.path.join(work, f"cases_q{q}_{system}.tsv")
    with open(path, "w") as f:
        for s in spec["sets"]:
            f.write((s["cpp"] if system == "cpp" else s["verus"]) + "\t" + s["verus"] + "\n")
    return path


def env_for(system, q):
    env = dict(os.environ)
    env.pop("ACCEL_AUX_STORE", None)
    env.update(SYSTEMS.get(system, {}))
    env["ACCEL_AUX_DIAG"] = "0"  # no per-read diagnostics sampling
    model = (os.path.join(ROOT, "config", "verus", f"q{q}_aux_cost_model_sf20.json") if system == "verus"
             else os.path.join(ROOT, "config", f"q{q}_aux_cost_model_sf20.json"))
    if os.path.exists(model):
        env["ACCELERATOR_CODEGEN_DUCKDB_AUX_COST_MODEL"] = model
    return env


def run(cmd, env, log, timeout):
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
        r = json.loads(p.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        return {"error": f"no JSON output (see {log})"}
    r["wall_s"] = round(time.time() - t0, 1)
    return r


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
    spec = params(a.sets, a.seed)
    os.makedirs(a.out, exist_ok=True)
    log = os.path.join(a.out, "read.log")
    for q in a.queries:
        for system in a.systems:
            accel, base = ids(q, system)
            cases = cases_file(a.work, q, system, spec[str(q)])
            # reads do not write; one copy per run keeps the WAL and catalog untouched
            db = fresh_copy(a.db, a.work, f"cmp_read_q{q}_{system}.duckdb") if a.copy else a.db
            print(f"read q{q} {system} {time.strftime('%T')}", flush=True)
            r = run([a.bin, "read", db, accel, base, cases, str(a.reps)], env_for(system, q), log, a.timeout)
            if a.copy:
                drop(db)
            r.update({"query": q, "system": system, "note": spec[str(q)]["note"]})
            with open(os.path.join(a.out, f"read_q{q}_{system}.json"), "w") as f:
                json.dump(r, f, indent=1)
            print("  " + line_read(r), flush=True)
    summary(a.out)


def cmd_concurrent(a):
    spec = params(a.sets, a.seed)
    os.makedirs(a.out, exist_ok=True)
    log = os.path.join(a.out, "concurrent.log")
    phases = [("base", "base")] + [(s, "accel") for s in a.systems]
    for q in a.queries:
        for name, phase in phases:
            system = "cpp" if name == "base" else name
            accel, base = ids(q, system)
            cases = cases_file(a.work, q, "verus" if name == "base" else system, spec[str(q)])
            db = fresh_copy(a.db, a.work, f"cmp_conc_q{q}_{name}.duckdb")
            print(f"concurrent q{q} {name} {time.strftime('%T')}", flush=True)
            r = run([a.bin, "concurrent", db, accel, base, cases, phase, str(a.readers), str(a.seconds), str(a.wps),
                     str(a.seed), str(a.warmup)], env_for(system, q), log, a.timeout)
            drop(db)
            r.update({"query": q, "system": name})
            with open(os.path.join(a.out, f"conc_q{q}_{name}.json"), "w") as f:
                json.dump(r, f, indent=1)
            print("  " + line_conc(r), flush=True)
    summary(a.out)


def line_read(r):
    if "error" in r:
        return "ERROR " + r["error"]
    return (f"build {r['build_ms'] / 1000:.1f}s  speedup(geomean) {r['speedup_geomean']:.1f}x  "
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
              f"hits {r['hits']} fallbacks {r['fallbacks']}")
    if r.get("read_errors") or r.get("write_errors"):
        s += f"  ERRORS read {r['read_errors']} write {r['write_errors']}"
    return s


def summary(out):
    rows = {}
    for name in sorted(os.listdir(out)):
        if name.endswith(".json") and name.startswith(("read_", "conc_")):
            r = json.load(open(os.path.join(out, name)))
            rows.setdefault(("read" if name.startswith("read_") else "conc", r["query"]), {})[r["system"]] = r
    reads = sorted(k[1] for k in rows if k[0] == "read")
    if reads:
        print("\nread-only: speedup over DuckDB (geomean over parameter sets), build seconds")
        print(f"{'q':>4} {'cpp':>10} {'verus':>10} {'cpp build':>10} {'verus build':>12}  notes")
        for q in reads:
            row = rows[("read", q)]
            cell = lambda s, f: ("err" if "error" in row.get(s, {}) else f(row[s])) if s in row else "-"
            notes = [f"{s}: {row[s]['mismatches']} mismatch" for s in row if row[s].get("mismatches")]
            notes += [f"{s}: {row[s]['fallbacks']} fallback" for s in row if row[s].get("fallbacks")]
            print(f"{q:>4} {cell('cpp', lambda r: '%.1fx' % r['speedup_geomean']):>10} "
                  f"{cell('verus', lambda r: '%.1fx' % r['speedup_geomean']):>10} "
                  f"{cell('cpp', lambda r: '%.1f' % (r['build_ms'] / 1000)):>10} "
                  f"{cell('verus', lambda r: '%.1f' % (r['build_ms'] / 1000)):>12}  {'; '.join(notes)}")
    concs = sorted(k[1] for k in rows if k[0] == "conc")
    if concs:
        print("\nconcurrent readers + writer: reader QPS (p99 ms), commit p50 ms (slowest commit s), "
              "invalidating writes (whole run)")
        print(f"{'q':>4} {'base qps':>16} {'cpp qps':>16} {'verus qps':>16} {'cpp commit':>14} {'verus commit':>14}"
              f" {'cpp inv':>8} {'verus inv':>10}")
        missing = []
        for q in concs:
            row = rows[("conc", q)]

            def qps(s):
                r = row.get(s)
                if r is None:
                    return "-"
                return "err" if "error" in r else "%.0f (%.1f)" % (r["qps"], r["read_p99_us"] / 1000)

            def get(s, f):
                r = row.get(s)
                return "-" if r is None or "error" in r else f(r)

            for s in ("cpp", "verus"):
                r = row.get(s)
                if r is not None and "error" not in r and r["invalidating_writes"] == 0:
                    missing.append(f"q{q} {s}")
            print(f"{q:>4} {qps('base'):>16} {qps('cpp'):>16} {qps('verus'):>16} "
                  f"{get('cpp', lambda r: '%.2f (%.1f)' % (r['commit_p50_us'] / 1000, r.get('max_commit_us', 0) / 1e6)):>14} "
                  f"{get('verus', lambda r: '%.2f (%.1f)' % (r['commit_p50_us'] / 1000, r.get('max_commit_us', 0) / 1e6)):>14} "
                  f"{get('cpp', lambda r: str(r['invalidating_writes'])):>8} "
                  f"{get('verus', lambda r: str(r['invalidating_writes'])):>10}")
        if missing:
            print("NOT INVALIDATED (no write made the accelerator maintain): " + ", ".join(missing))
        else:
            print("every accelerator run saw at least one invalidating write")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="mode", required=True)
    for mode in ("read", "concurrent"):
        p = sub.add_parser(mode)
        p.add_argument("--db", default=os.environ.get("DB", "/flash1/akhilvb/tpch/tpch_sf20.duckdb"))
        p.add_argument("--work", default=os.environ.get("WORK", "/flash1/akhilvb/tpch/work"))
        p.add_argument("--out", default=os.path.join(ROOT, "benchmarks", "compare", "results",
                                                     f"{mode}_{time.strftime('%Y%m%d_%H%M%S')}"))
        p.add_argument("--bin", default=os.path.join(ROOT, "build-verus-duckdb", "aux_compare"))
        p.add_argument("--queries", default=" ".join(map(str, range(1, 23))))
        p.add_argument("--systems", default="cpp verus")
        p.add_argument("--sets", type=int, default=8, help="TPC-H parameter sets per query")
        p.add_argument("--seed", type=int, default=20261004)
        p.add_argument("--timeout", type=int, default=4 * 3600, help="seconds per run")
    r = sub.choices["read"]
    r.add_argument("--reps", type=int, default=5)
    r.add_argument("--copy", action="store_true", help="run each on a fresh copy of the database")
    c = sub.choices["concurrent"]
    c.add_argument("--readers", type=int, default=8)
    c.add_argument("--seconds", type=float, default=60)
    c.add_argument("--wps", type=float, default=5, help="writer target, writes per second")
    c.add_argument("--warmup", type=float, default=5)
    s = sub.add_parser("summary")
    s.add_argument("out")
    a = ap.parse_args()
    if a.mode == "summary":
        return summary(a.out)
    a.queries = [int(x) for x in a.queries.replace(",", " ").split()]
    a.systems = a.systems.replace(",", " ").split()
    assert all(s in SYSTEMS for s in a.systems), f"systems: {list(SYSTEMS)}"
    assert os.path.exists(a.bin), f"no {a.bin} (cmake --build build-verus-duckdb --target aux_compare)"
    assert os.path.exists(a.db), f"no database {a.db}"
    os.makedirs(a.work, exist_ok=True)
    (cmd_read if a.mode == "read" else cmd_concurrent)(a)


if __name__ == "__main__":
    main()
