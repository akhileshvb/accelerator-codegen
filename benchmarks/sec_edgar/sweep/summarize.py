#!/usr/bin/env python3
"""Summarize the write-frequency sweep CSVs into one table (stdout + summary.md).

Writes are batched: "Writes" counts write transactions (batches); "Rows/s",
"Rows touched", "Batches" and "Rows/batch" describe the rows they update;
"write_sum/row" is the mode's total write time over the rows it touched.
"Ceiling" is the best workload speedup any accelerator can reach with base
writes: (base read_sum + base write_sum) / base write_sum.
"""
import csv
import glob
import os
import sys

out_dir = sys.argv[1]
rows = []
for path in sorted(glob.glob(os.path.join(out_dir, "*.csv"))):
    with open(path) as f:
        rows.extend(csv.DictReader(f))

by_key = {}
for r in rows:
    key = (r["engine"], int(r["query"]), float(r["write_interval_sec"]), r.get("params") or "default")
    by_key.setdefault(key, {})[r["mode"]] = r

lines = ["| Engine | Query | Params | Writes/s | Writes | Rows/s | Rows touched | Batches | Rows/batch | "
         "Invalidating rows | Base read p50 | Accel read p50 | Base write p50 | Accel write p50 | "
         "Base write_sum/row | Accel write_sum/row | Workload speedup | Speedup incl. build | "
         "Ceiling | Correct |",
         "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]


def ms(us):
    us = float(us)
    return f"{us/1000:.3g} ms" if us >= 1000 else f"{us:.3g} µs"


def get(r, k, default=""):
    v = r.get(k)
    return default if v is None or v == "" else v


for (engine, q, iv, pm) in sorted(by_key):
    m = by_key[(engine, q, iv, pm)]
    if "base" not in m or "shared" not in m:
        continue
    b, s = m["base"], m["shared"]
    wps = "read-only" if iv == 0 else f"{1/iv:,.0f}"
    rps = int(float(get(s, "rows_per_sec", 0)))
    touched = int(float(get(s, "rows_touched", 0)))
    per_batch = float(get(s, "rows_per_batch", 0))
    b_ws, b_rs = float(b["write_sum_us"]), float(b["read_sum_us"])
    ceiling = f"{(b_rs + b_ws) / b_ws:,.1f}×" if b_ws > 0 else "–"
    per_row = lambda r: ms(get(r, "write_sum_per_row_us", 0)) if int(float(get(r, "rows_touched", 0))) else "–"
    lines.append(
        f"| {engine} | q{q} | {pm} | {wps} | {s['writes']} | {rps:,} | {touched:,} | "
        f"{get(s, 'batches', s['writes'])} | {per_batch:,.0f} | {int(float(get(s, 'invalidating_rows', 0))):,} | "
        f"{ms(b['read_p50_us'])} | {ms(s['read_p50_us'])} | "
        f"{ms(b['write_p50_us']) if int(b['writes']) else '–'} | "
        f"{ms(s['write_p50_us']) if int(s['writes']) else '–'} | "
        f"{per_row(b)} | {per_row(s)} | "
        f"{float(s['workload_speedup_vs_base']):,.1f}× | "
        f"{float(s['workload_with_build_speedup_vs_base']):,.2f}× | {ceiling} | "
        f"{s.get('correct') or ('checked' if engine == 'duckdb' else '')} |")

text = "\n".join(lines)
print(text)
with open(os.path.join(out_dir, "summary.md"), "w") as f:
    f.write("# SEC-EDGAR SF3 write-frequency sweep (batched writes)\n\n"
            "Each write is one transaction: a set-based UPDATE of `Rows/batch` distinct rows "
            "(all row-writes due before a read, on the reader's virtual clock). "
            "Ceiling = (base read_sum + base write_sum) / base write_sum.\n\n" + text + "\n")
