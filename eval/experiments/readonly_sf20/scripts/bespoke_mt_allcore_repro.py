"""Reproduce the Bespoke-OLAP paper's TPC-H SF20 protocol on this host.
Same as BespokeOLAP benchmark/run.py: instantiations from Random(42+i), i < REPEAT, one timed
run each; Bespoke = single-threaded paper artifact's own "Execution ms"; DuckDB = Bespoke's
DuckDBConnectionManager (in-memory tables from Parquet, threads=1, pinned to core 3,
JSON-profiler latency). Speedups: total runtime, and per-query sum over instantiations.
usage: bespoke_mt_allcore_repro.py <repeat> <out_dir>
env: BESPOKE_ROOT (BespokeOLAP checkout), BESPOKE_DRIVER (built bespoke_tpch_mt binary), BESPOKE_PARQUET (dir with sf20/).
ALL-CORE VARIANT: bespoke_tpch_multithreading (80-thread pool, our driver, us timer of run_qN,
one run per instantiation); DuckDB same manager but unpinned and PRAGMA threads=os.cpu_count()."""
import csv, math, os, re, random, statistics, subprocess, sys
from pathlib import Path

def req(name):
    if name not in os.environ:
        sys.exit(f"{name} must be set (see the header)")
    return os.environ[name]

sys.path.insert(0, req("BESPOKE_ROOT"))
from dataset.gen_tpch.gen_tpch_query import gen_query                       # noqa: E402

def format_args_string(query_list, placeholder_list):
    # Verbatim logic of tools/validate_tool/query_validator_class.format_args_string
    # (inlined: importing that module pulls in the OpenAI agents SDK).
    out = []
    for qid, ph in zip(query_list, placeholder_list):
        vals = [v if isinstance(v, str) and v.startswith("(") else f'"{v}"' for v in ph.values()]
        out.append(f"{qid} " + " ".join(vals))
    return out

from tools.validate_tool.duckdb_connection_manager import DuckDBConnectionManager  # noqa: E402
import duckdb                                                                 # noqa: E402

REPEAT, OUT = int(sys.argv[1]), Path(sys.argv[2])
OUT.mkdir(parents=True, exist_ok=True)
PQ_BASE, SF = req("BESPOKE_PARQUET"), 20
BIN = Path(req("BESPOKE_DRIVER"))

qids, sqls, phs = [], [], []
for r in range(REPEAT):
    rnd = random.Random(42 + r)
    for q in range(1, 23):
        _, sql, ph = gen_query(f"Q{q}", rnd=rnd)
        qids.append(str(q)); sqls.append(sql); phs.append(ph)
args = format_args_string(qids, phs)
(OUT / "args.txt").write_text("\n".join(args) + "\n")

print(f"bespoke: {len(args)} requests", flush=True)
p = subprocess.run([str(BIN), f"{PQ_BASE}/sf{SF}/", str(OUT / "args.txt"), "0", str(OUT / "bespoke_rows")],
                   capture_output=True, text=True)
(OUT / "bespoke_stdout.txt").write_text(p.stdout); (OUT / "bespoke_stderr.txt").write_text(p.stderr)
lines = [l for l in p.stdout.splitlines() if re.match(r"^Q\d+ ", l)]
assert len(lines) == len(args) and not any("FAIL" in l for l in lines), f"bespoke: {len(lines)}/{len(args)} rc={p.returncode}"
bt = {i: float(l.split()[1]) for i, l in enumerate(lines)}
brows = {i: int(l.split("rows=")[1]) - 1 for i, l in enumerate(lines)}   # driver counts header row
print(f"duckdb {duckdb.__version__}: loading in-memory tables", flush=True)
mgr = DuckDBConnectionManager(pre_load_duckdb_tables=True, parquet_path=PQ_BASE, benchmark="tpch", sf=SF, pin_worker=False)
mgr.con.execute(f"PRAGMA threads={int(os.environ.get('THREADS', os.cpu_count()))}")  # env THREADS; engine uses CORE_IDS
print("duckdb threads", mgr.con.execute("select current_setting('threads')").fetchone()[0], flush=True)
dt, drows = {}, {}
for i, sql in enumerate(sqls):
    ms, df, _ = mgr.duckdb_sql(sql)
    dt[i], drows[i] = ms, len(df)

# Bespoke writes result<i>.csv (header + rows); compare row counts per instantiation.
rows_ok = {i: brows[i] == drows[i] for i in range(len(args))}
with open(OUT / "per_instantiation.csv", "w", newline="") as f:
    w = csv.writer(f); w.writerow(["idx", "query", "params", "bespoke_ms", "duckdb_ms", "rowcount_match"])
    for i in range(len(args)):
        w.writerow([i, f"Q{qids[i]}", args[i], bt[i], f"{dt[i]:.3f}", rows_ok[i]])

per_q = []
for q in range(1, 23):
    idx = [i for i in range(len(args)) if qids[i] == str(q)]
    b, d = sum(bt[i] for i in idx), sum(dt[i] for i in idx)
    per_q.append((q, b, d, d / b if b else float("inf"), all(rows_ok[i] for i in idx)))
B, D = sum(x[1] for x in per_q), sum(x[2] for x in per_q)
fin = [x[3] for x in per_q if math.isfinite(x[3])]
with open(OUT / "per_query.csv", "w", newline="") as f:
    w = csv.writer(f); w.writerow(["query", "bespoke_total_ms", "duckdb_total_ms", "speedup", "rowcounts_match"])
    for q, b, d, s, ok in per_q: w.writerow([f"Q{q}", b, f"{d:.3f}", f"{s:.2f}", ok])
for q, b, d, s, ok in per_q:
    print(f"Q{q:<3} bespoke {b:8.0f} ms  duckdb {d:9.1f} ms  {s:8.2f}x  rows {'ok' if ok else 'MISMATCH'}")
print(f"TOTAL bespoke {B:.0f} ms  duckdb {D:.0f} ms  speedup {D/B:.2f}x | median per-query {statistics.median(fin):.2f}x "
      f"| min {min(fin):.2f}x max {max(fin):.2f}x | {len(per_q)-len(fin)} queries at 0 ms (sub-ms, integer-ms timer)")
