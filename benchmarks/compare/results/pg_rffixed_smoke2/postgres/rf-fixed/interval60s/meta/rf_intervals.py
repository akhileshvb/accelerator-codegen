#!/usr/bin/env python3
"""Pick the write intervals of the concurrent RF experiment from a measured commit time.

A 0.1%-of-orders RF1/RF2 commit (30000 orders at SF20) takes seconds to many minutes once the
accelerators have maintained their state, so the interval between writes cannot be chosen in absolute
seconds up front: below one commit time the writer is saturated (back to back), far above it the
system is in steady state almost all of the time and the experiment shows nothing new.  This reads a
writer-only calibration run (run_backends.sh calibrate) and returns intervals as multiples of the
accelerator's refresh-slot time T_c (one RF commit plus the point writes that follow it):  1 x T_c (the writer is busy 100% of the time: the worst case),
2 x (50%), 4 x (25%).

  python3 benchmarks/compare/rf_intervals.py <backend> <calibration dir> [--mult "1 2 4"] [--commits 4]
    -> one JSON object: tc_accel_s, tc_base_s, intervals_s, rates (commits/s), seconds, warmup, est_hours

Window per point: warmup = 0.5 x interval (the first commit, at t=0, falls in it), then --commits RF
commits (RF1, RF2, ...; use an even number) at the given interval: seconds = commits x interval.
est_hours: all points, base + accelerator, per repetition (multiply by the repetitions).
"""
import argparse
import glob
import json
import os
import statistics
import sys


def commit_times(backend, d, system):
    """Seconds of the RF1 / RF2 commits of a calibration run of one system (base | accel)."""
    if backend == "duckdb":
        name = "cpp" if system == "accel" else "base"
        f = (glob.glob(os.path.join(d, f"multi_{name}.json")) + glob.glob(os.path.join(d, "**", f"multi_{name}.json"), recursive=True))[0]
        log = json.load(open(f))["write_log"]
        by = {}
        for _, kind, us, *_ in log:
            by.setdefault(kind.split()[0] if kind.startswith("rf") else kind, []).append(us / 1e6)
        return by
    f = (glob.glob(os.path.join(d, "**", f"{system}.json"), recursive=True))[0]
    r = json.load(open(f))
    return {k: [v["p50"] / 1e6] * v["n"] for k, v in r["commit_us_by_kind"].items()}


def tc(by):
    """The time of one refresh slot, which the intervals are multiples of: the slower of RF1 and RF2
    (median) plus one commit of each point-write kind that follows every RF commit (median each)."""
    rf = max(statistics.median(v) for k, v in by.items() if k in ("rf1", "rf2") and v)
    dims = sum(statistics.median(v) for k, v in by.items() if k not in ("rf1", "rf2") and v)
    return rf + dims, rf, dims


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("backend", choices=("duckdb", "postgres", "mysql"))
    ap.add_argument("dir")
    ap.add_argument("--mult", default="1 2 4")
    ap.add_argument("--commits", type=int, default=4)
    a = ap.parse_args()
    (t_acc, rf_acc, dims_acc), (t_base, _, _) = tc(commit_times(a.backend, a.dir, "accel")), tc(commit_times(a.backend, a.dir, "base"))
    mult = [float(x) for x in a.mult.split()]
    ivs = [round(m * t_acc, 3) for m in mult]
    secs = [a.commits * i for i in ivs]
    warm = [round(0.5 * i, 3) for i in ivs]
    est = 2 * sum(s + w for s, w in zip(secs, warm)) / 3600
    json.dump({"tc_accel_s": round(t_acc, 3), "tc_base_s": round(t_base, 3),
               "tc_accel_rf_s": round(rf_acc, 3), "tc_accel_point_writes_s": round(dims_acc, 3), "intervals_s": ivs,
               "rates": [round(1.0 / i, 6) for i in ivs], "seconds": secs, "warmup": warm,
               "commits": a.commits, "est_hours_per_rep": round(est, 1)}, sys.stdout)
    print()


if __name__ == "__main__":
    main()
