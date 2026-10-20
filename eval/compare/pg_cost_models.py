#!/usr/bin/env python3
"""Postgres cost models for the TPC-H accelerators, from a shared-state worker's log.

The Postgres extension prefers config/qN_aux_cost_model_postgres.json over the DuckDB-calibrated
config/qN_aux_cost_model.json (engine_helpers.hpp, ResolveCostModelPath).  The DuckDB numbers do not fit
Postgres: a rebuild there is a few seconds of vectorized scan, here one to three minutes of SPI scans, so
with them the selector picks a rebuild for RF-sized commits that the module applies as a delta in about a second.

Input: the server log of a run with accel.shared_state (e.g. `docker logs accel_pg_sf20`), whose worker
logs one line per build and per applied commit (engine_runtime.hpp):
  accel rebuild tpch_q3: synced ms=175957.3
  accel apply tpch_q3: applied batches=31 rows=150222 ms=1518.8 action=ApplyDelta rows=60023 ...
Best from a run with the action forced to ApplyDelta (accel_set_force_maintenance_action), so every
accelerator's delta cost is measured.  Per accelerator:
  full_rebuild_us (= partition_rebuild_us, first_query_blocking_us)  median build time
  apply_delta_overhead_us + apply_delta_per_row_us x rows             least-squares fit of the applies' wall
                                                                      time (capture decode, lookups, staging)
                                                                      over the rows the module counted
Everything else is copied from the DuckDB model.

  docker logs --since 3h accel_pg_sf20 2>&1 | python3 eval/compare/pg_cost_models.py [--since HH:MM:SS]
      [--until HH:MM:SS] [--exclude q21] [--write]
Without --write it only prints the table.
"""
import argparse
import json
import os
import re
import statistics
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
LINE = re.compile(r"(\d\d:\d\d:\d\d)\.\d+ UTC .*accel (rebuild|apply) tpch_q(\d+)\w*: (\w+)(.*)")


def fit(points):
    """y = a + b x by least squares, a >= 0 and b >= 0; through the origin when the row counts barely differ
    (every apply the same size: the slope would be noise)."""
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    if max(xs) < 2 * max(1, min(xs)):
        return 0.0, sum(ys) / max(1, sum(xs))
    mx, my = statistics.mean(xs), statistics.mean(ys)
    b = max(0.0, sum((x - mx) * (y - my) for x, y in points) / sum((x - mx) ** 2 for x in xs))
    return max(0.0, my - b * mx), b


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--since", default="00:00:00", help="UTC time of day")
    ap.add_argument("--until", default="99:99:99", help="UTC time of day")
    ap.add_argument("--exclude", default="", help="queries whose applies to ignore (e.g. measured before a fix)")
    ap.add_argument("--write", action="store_true")
    a = ap.parse_args()
    excluded = {int(q.lstrip("q")) for q in a.exclude.replace(",", " ").split()}
    builds, applies = {}, {}
    for line in sys.stdin:
        m = LINE.search(line)
        if not m or not (a.since <= m.group(1) <= a.until):
            continue
        kind, q, outcome, rest = m.group(2), int(m.group(3)), m.group(4), m.group(5)
        ms = float(re.search(r"ms=([\d.]+)", rest).group(1))
        if kind == "rebuild":
            builds.setdefault(q, []).append(ms * 1000)
        elif outcome == "applied" and "action=ApplyDelta" in rest and q not in excluded:
            rows = int(re.search(r"action=ApplyDelta rows=(\d+)", rest).group(1))
            applies.setdefault(q, []).append((rows, ms * 1000))
    print(f"{'query':>5} {'builds':>6} {'rebuild s':>10} {'applies':>7} {'overhead ms':>11} {'per row us':>10} "
          f"{'apply @150k rows s':>18}")
    for q in sorted(set(builds) | set(applies)):
        if q not in builds or q not in applies:
            print(f"q{q:<4} skipped: {'no build' if q not in builds else 'no apply'} measured")
            continue
        rebuild = statistics.median(builds[q])
        overhead, per_row = fit(applies[q])
        print(f"q{q:<4} {len(builds[q]):>6} {rebuild / 1e6:>10.1f} {len(applies[q]):>7} {overhead / 1000:>11.1f} "
              f"{per_row:>10.2f} {(overhead + per_row * 150000) / 1e6:>18.2f}")
        if not a.write:
            continue
        base = json.load(open(os.path.join(ROOT, "accelerators", "tpch", f"q{q}", "config", f"q{q}_aux_cost_model.json")))
        base.update({
            "engine": "postgres",
            "full_rebuild_us": rebuild, "partition_rebuild_us": rebuild, "first_query_blocking_us": rebuild,
            "apply_delta_overhead_us": overhead, "apply_delta_base_us": 0.0, "apply_delta_per_row_us": per_row,
            "invalidation_overhead_us": 1000.0,
            "calibration_source": f"eval/compare/pg_cost_models.py: {len(builds[q])} builds, "
                                  f"{len(applies[q])} applies (shared-state worker log, SF20)",
        })
        for k in [k for k in base if k.startswith("calibration_") and k != "calibration_source"]:
            del base[k]
        with open(os.path.join(ROOT, "accelerators", "tpch", f"q{q}", "config", f"q{q}_aux_cost_model_postgres.json"), "w") as f:
            json.dump(base, f, indent=2)
            f.write("\n")


if __name__ == "__main__":
    main()
