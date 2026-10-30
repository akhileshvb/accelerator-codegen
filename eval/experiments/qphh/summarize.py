"""TPC-H composite metric (Table 3): JetStream stateful (C++) over DuckDB, as ratios of per-system medians
over the runs of each SF directory in data/ (sf30 today). Fields and aggregation mirror run_compare.py's
summary table: power_at_size, throughput_at_size, qphh_at_size per run, median over runs.
Usage: summarize.py [DATA_DIR]"""
import glob, json, os, statistics, sys

DATA = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
METRICS = [("Power@Size", "power_at_size"), ("Throughput@Size", "throughput_at_size"), ("QphH@Size", "qphh_at_size")]


def medians(sf_dir, system):
    runs = [json.load(open(f)) for f in sorted(glob.glob(f"{sf_dir}/tpch_{system}_r*.json"))]
    if not runs:
        return None
    return {k: statistics.median(r[k] for r in runs)
            for _, k in METRICS}, len(runs)


for sf_dir in sorted(glob.glob(f"{DATA}/sf*")):
    base, cpp = medians(sf_dir, "base"), medians(sf_dir, "cpp")
    if not (base and cpp):
        continue
    print(f"{os.path.basename(sf_dir)}: median over {cpp[1]} runs (DuckDB -> stateful)")
    for name, key in METRICS:
        print(f"  {name:16s} {base[0][key]:>12,.0f} -> {cpp[0][key]:>12,.0f}  = {cpp[0][key] / base[0][key]:.2f}x")
