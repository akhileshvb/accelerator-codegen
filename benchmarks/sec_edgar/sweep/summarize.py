#!/usr/bin/env python3
"""Summarize the write-frequency sweep CSVs into one table (stdout + summary.md)."""
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

lines = ["| Engine | Query | Params | Writes/s | Writes | Base read p50 | Accel read p50 | Base write p50 | "
         "Accel write p50 | Workload speedup | Speedup incl. build | Correct |",
         "|---|---|---|---|---|---|---|---|---|---|---|---|"]


def ms(us):
    us = float(us)
    return f"{us/1000:.3g} ms" if us >= 1000 else f"{us:.3g} µs"


for (engine, q, iv, pm) in sorted(by_key):
    m = by_key[(engine, q, iv, pm)]
    if "base" not in m or "shared" not in m:
        continue
    b, s = m["base"], m["shared"]
    wps = "read-only" if iv == 0 else f"{1/iv:,.0f}"
    lines.append(
        f"| {engine} | q{q} | {pm} | {wps} | {s['writes']} | {ms(b['read_p50_us'])} | "
        f"{ms(s['read_p50_us'])} | {ms(b['write_p50_us']) if int(b['writes']) else '–'} | "
        f"{ms(s['write_p50_us']) if int(s['writes']) else '–'} | "
        f"{float(s['workload_speedup_vs_base']):,.1f}× | "
        f"{float(s['workload_with_build_speedup_vs_base']):,.2f}× | "
        f"{s.get('correct') or ('checked' if engine == 'duckdb' else '')} |")

text = "\n".join(lines)
print(text)
with open(os.path.join(out_dir, "summary.md"), "w") as f:
    f.write("# SEC-EDGAR SF3 write-frequency sweep\n\n" + text + "\n")
