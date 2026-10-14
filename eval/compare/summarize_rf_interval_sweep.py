#!/usr/bin/env python3
"""Summarize a run_rf_interval_sweep.sh directory: <out>/T<interval>s/conc_q<N>_<system>_r<k>.json.

  python3 eval/compare/summarize_rf_interval_sweep.py <out_dir>

Per (interval, query, system, repetition) -> <out>/rf_interval_summary.csv; prints per interval and query the
median over repetitions of the reader-throughput speedup (accelerator QPS / DuckDB QPS, same readers, writer
stream and run length), the writer's commit p50 for both systems, the refresh slots per second the accelerator
run achieved vs the target ("keeps up" when >= 90% of the target), and rows touched per second.

Workload speedup = time DuckDB needs for the work / time the accelerator needs for the same work, the work being
the reads the DuckDB run completed (N) plus the shared refresh stream:
  (N * DuckDB mean read + DuckDB commit time) / (N * accelerator mean read + accelerator commit time incl. maintenance)
(both runs commit the same writes; read and commit times from the measured window).
"""
import csv
import glob

from workload import workload_speedup
import json
import math
import os
import re
import statistics
import sys


def load(out):
    rows = []
    for d in sorted(glob.glob(os.path.join(out, "T*s"))):
        m = re.match(r"T([0-9.]+)s$", os.path.basename(d))
        if not m:
            continue
        T = float(m.group(1))
        for f in glob.glob(os.path.join(d, "conc_q*_*_r*.json")):
            r = json.load(open(f))
            r["interval_s"] = T
            rows.append(r)
    return rows


def main():
    out = sys.argv[1]
    rows = load(out)
    if not rows:
        sys.exit(f"no results under {out}/T*s")
    by = {}
    for r in rows:
        if "error" in r:
            print(f"error: T={r['interval_s']:g}s q{r.get('query')} {r.get('system')} rep {r.get('rep')}: {r['error']}")
            continue
        by.setdefault((r["interval_s"], r["query"], r["rep"]), {})[r["system"]] = r
    fields = ["interval_s", "query", "rep", "system", "reader_qps", "read_speedup_vs_duckdb", "workload_speedup_vs_duckdb",
              "read_p50_ms", "read_p99_ms", "read_mean_ms", "commit_sum_s",
              "commit_p50_ms", "commit_p99_ms", "max_commit_s", "target_slots_per_s", "slots_per_s", "keeps_up",
              "rows_touched_per_s", "writes", "invalidating_writes", "read_errors", "write_errors"]
    table = {}
    with open(os.path.join(out, "rf_interval_summary.csv"), "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for (T, q, k), sysmap in sorted(by.items()):
            base = sysmap.get("base")
            for s, r in sorted(sysmap.items()):
                target = 1.0 / T
                speed = (r["qps"] / base["qps"]) if (base and s != "base" and base["qps"] > 0) else None
                wl = workload_speedup(base, r) if speed is not None else None
                keeps = r.get("slots_per_sec", 0) >= 0.9 * target
                w.writerow({"interval_s": T, "query": q, "rep": k, "system": s, "reader_qps": r["qps"],
                            "read_speedup_vs_duckdb": speed, "workload_speedup_vs_duckdb": wl,
                            "read_mean_ms": r.get("read_mean_us", 0) / 1e3, "commit_sum_s": r.get("commit_sum_us", 0) / 1e6,
                            "read_p50_ms": r["read_p50_us"] / 1e3,
                            "read_p99_ms": r["read_p99_us"] / 1e3, "commit_p50_ms": r["commit_p50_us"] / 1e3,
                            "commit_p99_ms": r["commit_p99_us"] / 1e3, "max_commit_s": r.get("max_commit_us", 0) / 1e6,
                            "target_slots_per_s": target, "slots_per_s": r.get("slots_per_sec"), "keeps_up": keeps,
                            "rows_touched_per_s": r.get("rows_touched_per_sec"), "writes": r.get("writes"),
                            "invalidating_writes": r.get("invalidating_writes"), "read_errors": r.get("read_errors"),
                            "write_errors": r.get("write_errors")})
                if speed is not None:
                    table.setdefault((T, q, s), []).append((speed, keeps, base["commit_p50_us"], r["commit_p50_us"],
                                                            r.get("rows_touched_per_sec", 0), wl))
    systems = sorted({k[2] for k in table})
    for s in systems:
        print(f"\n{s} vs DuckDB, median over repetitions: reader-QPS speedup / workload speedup "
              "(! = the writer fell behind the refresh interval)")
        Ts = sorted({k[0] for k in table if k[2] == s}, reverse=True)
        print(f"{'q':>4} " + " ".join(f"{'T=%gs' % T:>24}" for T in Ts))
        for q in sorted({k[1] for k in table if k[2] == s}):
            cells = []
            for T in Ts:
                v = table.get((T, q, s))
                if not v:
                    cells.append(f"{'-':>24}")
                    continue
                sp = statistics.median(x[0] for x in v)
                keep = all(x[1] for x in v)
                wls = [x[5] for x in v if x[5]]
                wl = ('%.0fx' % statistics.median(wls)) if wls else '-'
                cells.append(f"{'%.0fx / %s%s' % (sp, wl, '' if keep else '!'):>24}")
            print(f"{q:>4} " + " ".join(cells))
        gm, rows_gm = [], []
        for T in Ts:
            sp = [statistics.median(x[0] for x in table[(T, q, s)]) for q in sorted({k[1] for k in table})
                  if (T, q, s) in table]
            rows_s = [statistics.median(x[4] for x in table[(T, q, s)]) for q in sorted({k[1] for k in table})
                      if (T, q, s) in table]
            wls = [statistics.median([x[5] for x in table[(T, q, s)] if x[5]] or [0]) for q in sorted({k[1] for k in table})
                   if (T, q, s) in table]
            wls = [x for x in wls if x > 0]
            wg = ('%.0fx' % math.exp(sum(map(math.log, wls)) / len(wls))) if wls else '-'
            gm.append(f"{'%.0fx / %s' % (math.exp(sum(map(math.log, sp)) / len(sp)), wg):>24}")
            rows_gm.append(f"{'%.0f rows/s' % statistics.median(rows_s):>24}")
        print(f"{'gm':>4} " + " ".join(gm))
        print(f"{'rows':>4} " + " ".join(rows_gm))
    print(f"\nwrote {os.path.join(out, 'rf_interval_summary.csv')}")


if __name__ == "__main__":
    main()
