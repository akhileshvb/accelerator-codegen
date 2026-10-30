"""Reproduce GenDB's TPC-H SF10 protocol with GenDB's own harness pieces.
GenDB: per query 1 warmup + 3 hot process runs, mean of ([TIMING] total - output), as
benchmarks/lib/gendb.py:gendb_benchmark_best. DuckDB: benchmarks/lib/runners.py:duckdb_benchmark
(persistent tpch_sf10.duckdb, read-only, SET threads = os.cpu_count(), 1 warmup + 3 hot, fetchall).
usage: gendb_paper_repro.py <paper|release|sf20> <out_dir>
  sf20: SF20 port of the release kernels on official-dbgen SF20 data.
  env THREADS=N (sf20 only): use the q<N>.t<N> builds, OMP_NUM_THREADS=N and DuckDB threads=N
  (pin the whole run with taskset from the caller). Default: GenDB harness = all cores."""
import csv, json, math, os, shutil, subprocess, sys, tempfile, importlib.util
from pathlib import Path
from types import SimpleNamespace

G = Path.home() / "GenDB"
sys.path.insert(0, str(G))
from benchmarks.lib.gendb import parse_timing_output, _read_param_cli_args   # noqa: E402
from benchmarks.lib.runners import duckdb_benchmark                           # noqa: E402
from benchmarks.lib.utils import BENCHMARK_THREADS                            # noqa: E402
import duckdb                                                                 # noqa: E402
import benchmarks.lib.runners as gendb_runners                                # noqa: E402

SF = 10
THREADS = os.environ.get("THREADS")
if THREADS:
    os.environ["OMP_NUM_THREADS"] = THREADS
    gendb_runners.BENCHMARK_THREADS = int(THREADS)   # read by duckdb_benchmark at call time
    BENCHMARK_THREADS = int(THREADS)

which, out = sys.argv[1], Path(sys.argv[2]); out.mkdir(parents=True, exist_ok=True)
if which == "paper":
    binaries = {q: Path(f"/spinning/akhilvb/gendb/paper_build/{q.lower()}") for q in ["Q1", "Q3", "Q6", "Q9", "Q18"]}
    storage, workload_dir = Path("/spinning/akhilvb/gendb/sf10_paper_storage"), None
    wl_file = Path(os.environ["ARXIV_WORKLOAD"])      # workload.py from arxiv-03-02-2026 branch
elif which == "sf20":
    SF = 20
    W = Path("/ssd1/akhilvb/gendb/output/tpc-h-sf20")
    suffix = f".t{THREADS}" if THREADS and THREADS != "80" else ""
    binaries = {f"Q{i}": W / f"queries/Q{i}/best/q{i}{suffix}" for i in range(1, 23)}
    storage, workload_dir = Path("/spinning/akhilvb/gendb/sf20_dbgen_storage"), W
    wl_file = G / "benchmarks/tpc-h/workload.py"
else:
    binaries = {f"Q{i}": Path(f"/spinning/akhilvb/gendb/rel319_build/q{i}") for i in range(1, 23)}
    storage, workload_dir = Path("/spinning/akhilvb/gendb/sf10_319_storage"), G / "output/tpc-h-sf10-3.19"
    wl_file = G / "benchmarks/tpc-h/workload.py"

# GenDB's harness SQL: workload.get_queries(sf) (Q11 threshold = 0.0001/SF)
spec = importlib.util.spec_from_file_location("tpch_workload", wl_file)
wl = importlib.util.module_from_spec(spec); spec.loader.exec_module(wl)
queries = wl.get_queries(SF)
qids = list(binaries)

results = {"GenDB": {}, "DuckDB": {}}
for qid, binary in binaries.items():
    with tempfile.TemporaryDirectory() as tmp:
        cmd = [str(binary), str(storage), tmp] + (_read_param_cli_args(workload_dir, qid) if workload_dir else [])
        times = []
        for _ in range(4):
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            t = parse_timing_output(p.stdout)
            if p.returncode != 0 or "query_ms" not in t:
                print(f"  GenDB {qid}: FAIL rc={p.returncode} {p.stderr[-200:]}"); times = []; break
            times.append(t["query_ms"])
        dest = out / f"gendb_rows{('_' + THREADS + 't') if THREADS else ''}" / qid; shutil.rmtree(dest, ignore_errors=True); shutil.copytree(tmp, dest)
    if times:
        results["GenDB"][qid] = times[1:]
        print(f"  GenDB {qid}: {sum(times[1:])/3:.1f} ms (avg of 3 hot runs)", flush=True)

cfg = SimpleNamespace(duckdb_path=G / f"benchmarks/tpc-h/duckdb/tpch_sf{SF}.duckdb")
print(f"DuckDB {duckdb.__version__}, threads = BENCHMARK_THREADS = {BENCHMARK_THREADS}", flush=True)
results["DuckDB"] = duckdb_benchmark(cfg, "hot", qids, queries)

# Validate GenDB output vs DuckDB (row count + values to 0.01 / 1e-6 rel, order-insensitive on ties)
con = duckdb.connect(str(cfg.duckdb_path), read_only=True)
def norm(v):
    try: return float(v)
    except (TypeError, ValueError): return str(v).strip()
def eq(a, b):
    return (abs(a - b) <= max(0.011, 1e-6 * abs(b))) if isinstance(a, float) and isinstance(b, float) else str(a) == str(b)
rows = []
for qid in qids:
    ref = [tuple(norm(c) for c in r) for r in con.execute(queries[qid]).fetchall()]
    files = list((out / f"gendb_rows{('_' + THREADS + 't') if THREADS else ''}" / qid).glob("*.csv"))
    got = [tuple(norm(c) for c in r) for r in list(csv.reader(open(files[0])))[1:]] if files else []
    key = lambda r: tuple(str(c) if not isinstance(c, float) else f"{c:.2f}" for c in r)
    ok = len(got) == len(ref) and all(len(g) == len(r) and all(eq(x, y) for x, y in zip(g, r))
                                       for g, r in zip(sorted(got, key=key), sorted(ref, key=key)))
    g = results["GenDB"].get(qid); d = results["DuckDB"].get(qid)
    gm, dm = (sum(g) / len(g) if g else None), (sum(d) / len(d) if d else None)
    rows.append({"query": qid, "gendb_avg_ms": gm, "duckdb_avg_ms": dm,
                 "speedup": (dm / gm) if gm and dm else None, "valid": ok, "rows": len(got), "ref_rows": len(ref)})
tag = f"duckdb{duckdb.__version__}" + (f"_{THREADS}t" if THREADS else "")
with open(out / f"summary_{tag}.csv", "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=list(rows[0])); w.writeheader(); w.writerows(rows)
json.dump({"duckdb_version": duckdb.__version__, "threads": BENCHMARK_THREADS, **results},
          open(out / f"raw_{tag}.json", "w"), indent=1)
for r in rows:
    print(f"{r['query']:<4} GenDB {r['gendb_avg_ms']:8.1f}  DuckDB {r['duckdb_avg_ms']:8.1f}  {r['speedup']:6.2f}x  "
          f"{'valid' if r['valid'] else 'INVALID (%d vs %d rows)' % (r['rows'], r['ref_rows'])}")
ok = [r for r in rows if r["speedup"]]
G_, D_ = sum(r["gendb_avg_ms"] for r in ok), sum(r["duckdb_avg_ms"] for r in ok)
gm = math.exp(sum(math.log(r["speedup"]) for r in ok) / len(ok))
print(f"TOTAL GenDB {G_:.1f} ms  DuckDB {D_:.1f} ms  total-runtime speedup {D_/G_:.2f}x  geomean {gm:.2f}x  "
      f"({sum(r['valid'] for r in rows)}/{len(rows)} valid)")
