#!/usr/bin/env python3
"""Per-query breakdown of QphH@Size relative to DuckDB, from a `run_compare.py tpch` result directory.

QphH = sqrt(Power * Throughput), so for a system X against DuckDB (B), per run:

  QphH_X / QphH_B = prod_q f_q  *  f_RF  *  f_T

  f_q  = (QI_B(q) / QI_X(q)) ^ (1/48)     query q in the power test (1/24 of the geomean, square-rooted)   -- exact
  f_RF = prod_{RF1,RF2} (RI_B / RI_X) ^ (1/48)   the power test's two refresh functions                    -- exact
  f_T  = (Ts_B / Ts_X) ^ (1/2)            the throughput test

Ts is the later of the query streams' end and the refresh stream's end, so f_T is not separable exactly.  It is
attributed to what bounds Ts: for each system, Ts is split into the query streams' share and the refresh stream's
share (all to the refresh stream when it ends last; otherwise all to the queries, split by each query's share of the
summed stream time).  log f_T = log Ts_B - log Ts_X then splits by those shares.  The per-query factor reported is
f_q * (q's share of f_T); the remaining factor is "refresh".  The factors multiply back to the QphH ratio.

  python3 eval/compare/tpch_breakdown.py <out_dir> [--base-dir <dir with tpch_base_r*.json>]
"""
import argparse
import glob
import json
import math
import os
import statistics


def load(d, system):
    rs = [json.load(open(p)) for p in sorted(glob.glob(os.path.join(d, f"tpch_{system}_r*.json")))]
    return [r for r in rs if "error" not in r]


def ts_shares(r):
    """log Ts split into per-query and refresh parts: {q: weight}, refresh weight (weights sum to 1)."""
    per_q = [sum(s[q] for s in r["stream_query_us"]) for q in range(22)]
    if r["refresh_stream_s"] >= r["query_streams_s"]:
        return [0.0] * 22, 1.0
    total = sum(per_q)
    return [x / total for x in per_q], 0.0


def breakdown(base, acc):
    """log-factors for one paired run: per query (power, throughput), refresh (power, throughput)."""
    lq = [math.log(base["power_query_us"][q] / acc["power_query_us"][q]) / 48 for q in range(22)]
    lrf = sum(math.log(b["us"] / a["us"]) for b, a in zip(base["power_refresh"], acc["power_refresh"])) / 48
    # log f_T = 1/2 (log Ts_B - log Ts_X); each side's log Ts is attributed by its own shares
    wb, rb = ts_shares(base)
    wa, ra = ts_shares(acc)
    lb, la = math.log(base["ts_s"]), math.log(acc["ts_s"])
    lt_q = [0.5 * (wb[q] * lb - wa[q] * la) for q in range(22)]
    lt_rf = 0.5 * (rb * lb - ra * la)
    return lq, lt_q, lrf, lt_rf


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out")
    ap.add_argument("--base-dir", default=None, help="where the DuckDB runs are (default: out)")
    ap.add_argument("--systems", default="cpp verus")
    a = ap.parse_args()
    base = load(a.base_dir or a.out, "base")
    assert base, "no DuckDB (base) runs"
    for s in a.systems.split():
        acc = load(a.out, s)
        if not acc:
            continue
        n = min(len(base), len(acc))
        runs = [breakdown(base[k], acc[k]) for k in range(n)]
        ratio = [math.exp(sum(lq) + sum(lt) + lrf + ltr) for lq, lt, lrf, ltr in runs]
        check = [acc[k]["qphh_at_size"] / base[k]["qphh_at_size"] for k in range(n)]
        print(f"\n== {s} vs DuckDB, {n} paired run(s): QphH ratio {', '.join(f'{x:.3f}' for x in ratio)} "
              f"(direct: {', '.join(f'{x:.3f}' for x in check)})")
        bound = ["refresh" if r["refresh_stream_s"] >= r["query_streams_s"] else "queries" for r in acc[:n]]
        print(f"   throughput test bound by: {s}: {', '.join(bound)}; DuckDB: "
              f"{', '.join('refresh' if r['refresh_stream_s'] >= r['query_streams_s'] else 'queries' for r in base[:n])}")
        med = lambda xs: statistics.median(xs)
        print(f"   {'':>8} {'power speedup':>14} {'power factor':>13} {'tput factor':>12} {'QphH factor':>12}")
        rows = []
        for q in range(22):
            sp = med([base[k]["power_query_us"][q] / acc[k]["power_query_us"][q] for k in range(n)])
            fp = med([math.exp(runs[k][0][q]) for k in range(n)])
            ft = med([math.exp(runs[k][1][q]) for k in range(n)])
            fq = med([math.exp(runs[k][0][q] + runs[k][1][q]) for k in range(n)])
            rows.append((q + 1, sp, fp, ft, fq))
        for q, sp, fp, ft, fq in rows:
            print(f"   {'Q' + str(q):>8} {sp:>13.1f}x {fp:>13.3f} {ft:>12.3f} {fq:>12.3f}")
        rsp = med([sum(x["us"] for x in base[k]["power_refresh"]) / sum(x["us"] for x in acc[k]["power_refresh"])
                   for k in range(n)])
        print(f"   {'RF1+RF2':>8} {rsp:>13.4f}x {med([math.exp(r[2]) for r in runs]):>13.3f} "
              f"{med([math.exp(r[3]) for r in runs]):>12.3f} {med([math.exp(r[2] + r[3]) for r in runs]):>12.3f}")
        print(f"   {'product':>8} {'':>14} {med([math.exp(sum(r[0]) + r[2]) for r in runs]):>13.3f} "
              f"{med([math.exp(sum(r[1]) + r[3]) for r in runs]):>12.3f} {med(ratio):>12.3f}"
              f"   (= Power ratio^1/2, Throughput ratio^1/2, QphH ratio)")


if __name__ == "__main__":
    main()
