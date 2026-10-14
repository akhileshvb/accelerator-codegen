#!/usr/bin/env python3
"""Calibrate the TPC-H accelerators' maintenance cost models on the refresh functions, at one scale factor.

The default models (accelerators/tpch/qN/config/qN_aux_cost_model.json) are fitted on UPDATE calibration writes over small date ranges,
which extrapolate badly to the refresh functions: an RF touches ~0.1% of the orders spread over every key range, so a
module's fixed per-partition costs dominate and the per-row fit predicts far too much (Q9 at SF1: ~45 s predicted for
an ApplyDelta that takes a fraction of that).  This measures the two options the policy weighs on the writes the
benchmarks actually make:

  per query and system, two `run_compare.py cycle` runs on fresh copies of the database (RF1 / RF2 of the dbgen update
  sets, each followed by the usual point writes), one with ApplyDelta forced, one with RebuildAll forced
  (ACCEL_FORCE_MAINTENANCE_ACTION); from the RF commits:
    apply_delta_per_row_us = median(maintenance time / rows touched)     (apply_delta_overhead_us = base_us = 0)
                             or apply_delta_overhead_us = median time when the module reports no rows
    full_rebuild_us = partition_rebuild_us = median RebuildAll maintenance time

and writes config[/verus]/q<N>_aux_cost_model_sf<SF>.json: the default model with those fields replaced and the raw
measurements recorded (calibration_rf_*).  run_compare.py selects these files for runs at that scale factor
(ACCEL_AUX_BENCH_DB_LABEL=tpch_sf<SF>).  Queries whose accelerator no RF maintains (it reads none of orders /
lineitem) get no file: they keep the default model.

  python3 scripts/calibrate_rf_cost_models.py --sf 1 --db ../accelerator-codegen-duckdb/tpch_sf1.duckdb \\
      --rf-dir /flash1/akhilvb/tpch/refresh_sf1 [--systems "cpp verus"] [--queries "1 9 13"]
"""
import argparse
import json
import os
import statistics
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUN_COMPARE = os.path.join(ROOT, "eval", "compare", "run_compare.py")


def cycle(a, system, q, action, out):
    os.makedirs(os.path.join(a.work, f"{system}_q{q}"), exist_ok=True)  # its own DB copy: queries run in parallel
    env = dict(os.environ, ACCEL_FORCE_MAINTENANCE_ACTION=action)
    cmd = [sys.executable, RUN_COMPARE, "cycle", "--db", a.db, "--sf", f"{a.sf:g}", "--rf-dir", a.rf_dir,
           "--work", os.path.join(a.work, f"{system}_q{q}"), "--system", system, "--queries", str(q), "--repeats", "1", "--sets", "2",
           "--seed", str(a.seed), "--pairs", str(a.pairs), "--burst", "3", "--no-base", "--out", out]
    if a.duckdb_threads:
        cmd += ["--duckdb-threads", str(a.duckdb_threads)]
    subprocess.run(cmd, env=env, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, cwd=ROOT)
    r = json.load(open(os.path.join(out, f"cycle_q{q}_accel_r0.json")))
    if "error" in r:
        raise RuntimeError(f"q{q} {system} {action}: {r['error']}")
    return [w for w in r["writes"] if w["kind"].startswith("rf")], r.get("mismatches", 0)


def calibrate_one(a, system, q, out_root, scale):
    """One query: returns a line to print."""
    qcfg = os.path.join(ROOT, "accelerators", "tpch", f"q{q}", "config")
    cdir = os.path.join(qcfg, *(["verus"] if system.startswith("verus") else []))
    default = os.path.join(cdir, f"q{q}_aux_cost_model.json")
    if not os.path.exists(default):
        default = os.path.join(qcfg, f"q{q}_aux_cost_model.json")
    actions = ("ApplyDelta",) if a.rebuild_from_build else ("ApplyDelta", "RebuildAll")
    measured = {}
    for action in actions:
        d = os.path.join(out_root, f"{system}_q{q}_{action}")
        measured[action] = cycle(a, system, q, action, d)
    delta, mism_d = measured["ApplyDelta"]
    rebuild, mism_r = measured.get("RebuildAll", ([], 0))
    if all(w["action"] in ("none", "NoOp") for w in delta + rebuild):
        return f"{system} q{q:>2}: no RF maintains it; keeps {os.path.relpath(default, ROOT)}"
    took = lambda ws, act: [w["pre_commit_us"] for w in ws if w["action"] == act]
    d_us = took(delta, "ApplyDelta")
    if a.rebuild_from_build:  # a full rebuild redoes the build: the measured build time
        build = json.load(open(os.path.join(out_root, f"{system}_q{q}_ApplyDelta", f"cycle_q{q}_accel_r0.json")))["build_ms"]
        r_us = [build * 1000.0]
    else:
        r_us = took(rebuild, "RebuildAll")
    if not d_us or not r_us:
        return (f"{system} q{q:>2}: forcing was not honoured (ApplyDelta runs: {[w['action'] for w in delta]}, "
                f"RebuildAll runs: {[w['action'] for w in rebuild]}); skipped")
    rows = [w["rows_touched"] for w in delta if w["action"] == "ApplyDelta"]
    model = json.load(open(default))
    if all(n > 0 for n in rows):
        model["apply_delta_per_row_us"] = statistics.median(t / n for t, n in zip(d_us, rows))
        model["apply_delta_overhead_us"] = 0.0
    else:  # the module reports no rows to the policy: a flat cost per RF
        model["apply_delta_per_row_us"] = 0.0
        model["apply_delta_overhead_us"] = statistics.median(d_us)
    model["apply_delta_base_us"] = 0.0
    model["full_rebuild_us"] = model["partition_rebuild_us"] = statistics.median(r_us)
    model.update({
        "source": f"scripts/calibrate_rf_cost_models.py {time.strftime('%Y-%m-%d')}: TPC-H RF1/RF2 at SF{a.sf:g} "
                  f"({a.pairs} pairs), ApplyDelta forced; RebuildAll "
                  f"{'= the measured build time' if a.rebuild_from_build else 'forced'}; other fields from "
                  f"{os.path.relpath(default, ROOT)}",
        "calibration_rf_apply_delta_us": d_us, "calibration_rf_rows_touched": rows,
        "calibration_rf_rebuild_all_us": r_us,
        "calibration_rf_mismatches": {"ApplyDelta": mism_d, "RebuildAll": mism_r}})
    path = os.path.join(cdir, f"q{q}_aux_cost_model_{scale}.json")
    with open(path, "w") as f:
        json.dump(model, f, indent=1)
        f.write("\n")
    pred_delta = model["apply_delta_overhead_us"] + model["apply_delta_per_row_us"] * max(rows or [0])
    verdict = "ApplyDelta" if pred_delta < model["full_rebuild_us"] else "REBUILD WINS"
    return (f"{system} q{q:>2}: ApplyDelta median {statistics.median(d_us) / 1e3:9.1f} ms "
            f"(rows {statistics.median(rows) if rows else 0:>8.0f}), RebuildAll {statistics.median(r_us) / 1e3:10.1f} ms "
            f"-> policy picks {verdict} -> {os.path.relpath(path, ROOT)}"
            f"{'  MISMATCHES ' + str((mism_d, mism_r)) if mism_d or mism_r else ''}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sf", type=float, required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--rf-dir", required=True)
    ap.add_argument("--systems", default="cpp verus")
    ap.add_argument("--queries", default=" ".join(map(str, range(1, 23))))
    ap.add_argument("--pairs", type=int, default=2, help="RF1+RF2 pairs per run")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--duckdb-threads", type=int, default=None)
    ap.add_argument("--work", default="/flash1/akhilvb/tpch/work_calibrate_rf")
    ap.add_argument("--jobs", type=int, default=1, help="queries calibrated at once (each its own process and DB copy)")
    ap.add_argument("--rebuild-from-build", action="store_true",
                    help="take RebuildAll's cost as the measured build time instead of forcing rebuilds (fast at large SF)")
    ap.add_argument("--out", default=None, help="where the cycle results go (default: a temp dir under --work)")
    a = ap.parse_args()
    out_root = a.out or os.path.join(a.work, f"calibrate_rf_sf{a.sf:g}_{time.strftime('%Y%m%d_%H%M%S')}")
    os.makedirs(out_root, exist_ok=True)
    scale = f"sf{a.sf:g}"
    from concurrent.futures import ThreadPoolExecutor
    for system in a.systems.split():
        qs = [int(x) for x in a.queries.split()]
        with ThreadPoolExecutor(max_workers=a.jobs) as pool:
            for line in pool.map(lambda q: calibrate_one(a, system, q, out_root, scale), qs):
                print(line, flush=True)

if __name__ == "__main__":
    main()
