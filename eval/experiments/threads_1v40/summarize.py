"""Section 6.2: stateful read latency barely changes between 1 and 40 read threads. Prints, per query, the
mean accel_us over all cases of data/q<N>_t40.json divided by that of q<N>_t1.json, and the geomean.
Usage: summarize.py [DATA_DIR]"""
import json, math, os, statistics, sys

DATA = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
QUERIES = [1, 3, 13, 18, 21]


def mean_us(q, t):
    return statistics.mean(c["accel_us"] for c in json.load(open(f"{DATA}/q{q}_t{t}.json"))["cases"])


ratios = {q: mean_us(q, 40) / mean_us(q, 1) for q in QUERIES}
for q, r in ratios.items():
    print(f"Q{q:<3d} t1 {mean_us(q, 1):>9.1f} us  t40 {mean_us(q, 40):>9.1f} us  t40/t1 {r:.3f}")
print(f"geomean t40/t1: {math.exp(statistics.mean(map(math.log, ratios.values()))):.3f}")
