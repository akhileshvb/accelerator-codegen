#!/usr/bin/env python3
"""Aggregate validated parallel TPC-H CSVs into a summary matrix with session drift."""

from __future__ import annotations

import argparse
import csv
import math
import statistics
from collections import defaultdict
from pathlib import Path


QUERIES = ["Q1", "Q3", "Q4", "Q6", "Q9", "Q12", "Q14"]


def geomean(values: list[float]) -> float:
    if not values:
        return float("nan")
    return math.exp(sum(math.log(v) for v in values) / len(values))


def median(values: list[float]) -> float:
    return float(statistics.median(values)) if values else float("nan")


def load_rows(paths: list[Path]) -> list[dict]:
    rows: list[dict] = []
    for path in paths:
        with path.open() as f:
            reader = csv.DictReader(f)
            for row in reader:
                row["_path"] = str(path)
                # Infer session from filename ..._sessionN_...
                name = path.name
                session = 0
                if "_session" in name:
                    try:
                        session = int(name.split("_session")[1].split("_")[0])
                    except ValueError:
                        session = 0
                row["_session"] = session
                # Infer scale from filename or ACCEL path convention
                scale = 1
                if "_sf10_" in name or "sf10" in name:
                    scale = 10
                elif "_sf1_" in name:
                    scale = 1
                row["_scale"] = scale
                rows.append(row)
    return rows


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("csvs", nargs="+", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--st-baseline", type=Path, action="append", default=[],
                    help="optional single-threaded accel CSVs for self-scaling")
    args = ap.parse_args()

    rows = load_rows(args.csvs)
    st_rows = load_rows(args.st_baseline) if args.st_baseline else []

    # ST baseline: per (scale, query) median of accel medians at threads=1 single
    st_accel: dict[tuple[int, str], list[float]] = defaultdict(list)
    for r in st_rows:
        if r.get("accelerator_mode", "single") not in ("single", ""):
            continue
        if int(float(r.get("threads") or 1)) != 1:
            continue
        st_accel[(int(r["_scale"]), r["query"])].append(float(r["accelerator_median_ms"]))
    st_med = {k: median(v) for k, v in st_accel.items()}

    # DuckDB @ t=1 for self-scaling: per (scale, query, session) from parallel t=1 or any t=1
    duck_t1: dict[tuple[int, str, int], list[float]] = defaultdict(list)
    for r in rows:
        if int(float(r.get("threads") or 0)) == 1:
            duck_t1[(int(r["_scale"]), r["query"], int(r["_session"]))].append(float(r["duckdb_median_ms"]))
    duck_t1_med = {k: median(v) for k, v in duck_t1.items()}

    # Group seed medians by (scale, threads, query, session)
    by_cell: dict[tuple[int, int, str, int], list[dict]] = defaultdict(list)
    for r in rows:
        if r.get("accelerator_mode") and r["accelerator_mode"] != "parallel":
            continue
        key = (int(r["_scale"]), int(float(r["threads"])), r["query"], int(r["_session"]))
        by_cell[key].append(r)

    # Aggregate across seeds within session, then across sessions
    # Output one row per (scale, threads, query)
    out_rows = []
    overall_geomean: dict[tuple[int, int], list[float]] = defaultdict(list)

    cells = sorted({(s, t, q) for (s, t, q, _) in by_cell})
    for scale, threads, query in cells:
        sess_duck = []
        sess_accel = []
        sess_speed = []
        sess_shared = []
        sess_worker = []
        sess_rss = []
        sess_mad_duck = []
        sess_mad_accel = []
        sess_cv_duck = []
        sess_cv_accel = []
        n_seeds = 0
        for session in sorted({k[3] for k in by_cell if k[:3] == (scale, threads, query)}):
            seed_rows = by_cell[(scale, threads, query, session)]
            n_seeds = max(n_seeds, len(seed_rows))
            duck = [float(r["duckdb_median_ms"]) for r in seed_rows]
            accel = [float(r["accelerator_median_ms"]) for r in seed_rows]
            speed = [float(r["speedup"]) for r in seed_rows]
            sess_duck.append(median(duck))
            sess_accel.append(median(accel))
            sess_speed.append(geomean(speed))
            sess_shared.append(median([float(r.get("shared_state_bytes") or 0) for r in seed_rows]))
            sess_worker.append(median([float(r.get("per_worker_state_bytes") or 0) for r in seed_rows]))
            sess_rss.append(median([float(r.get("peak_rss_kb") or 0) for r in seed_rows]))
            # Parse mad from dispersion string if present
            for r in seed_rows:
                dd = r.get("duckdb_dispersion", "")
                ad = r.get("accelerator_dispersion", "")
                if "mad_ms=" in dd:
                    sess_mad_duck.append(float(dd.split("mad_ms=")[1].split(";")[0]))
                if "mad_ms=" in ad:
                    sess_mad_accel.append(float(ad.split("mad_ms=")[1].split(";")[0]))
                sess_cv_duck.append(float(r.get("duckdb_cv") or 0))
                sess_cv_accel.append(float(r.get("accelerator_cv") or 0))

        duck_med = median(sess_duck)
        accel_med = median(sess_accel)
        speed = geomean(sess_speed) if sess_speed else float("nan")
        drift = 0.0
        if len(sess_speed) >= 2 and sess_speed[0] > 0:
            drift = abs(sess_speed[1] - sess_speed[0]) / sess_speed[0]

        # Self-scaling vs DuckDB@1 (same session median of session-1 duck@1 if available)
        duck_self = float("nan")
        d1 = duck_t1_med.get((scale, query, 1)) or duck_t1_med.get((scale, query, 2))
        if d1 and d1 > 0:
            duck_self = d1 / duck_med if duck_med > 0 else float("nan")

        accel_self = float("nan")
        st = st_med.get((scale, query))
        if st and st > 0 and accel_med > 0:
            accel_self = st / accel_med

        efficiency = float("nan")
        if threads > 0 and not math.isnan(accel_self):
            efficiency = accel_self / threads

        overall_geomean[(scale, threads)].append(speed)

        out_rows.append(
            {
                "scale": scale,
                "threads": threads,
                "query": query,
                "duckdb_median_ms": f"{duck_med:.4f}",
                "accel_parallel_median_ms": f"{accel_med:.4f}",
                "speedup_vs_duckdb": f"{speed:.6f}",
                "accel_self_vs_st_single": f"{accel_self:.6f}" if not math.isnan(accel_self) else "",
                "duckdb_self_vs_t1": f"{duck_self:.6f}" if not math.isnan(duck_self) else "",
                "parallel_efficiency": f"{efficiency:.6f}" if not math.isnan(efficiency) else "",
                "duckdb_mad_ms": f"{median(sess_mad_duck):.4f}" if sess_mad_duck else "",
                "accel_mad_ms": f"{median(sess_mad_accel):.4f}" if sess_mad_accel else "",
                "duckdb_cv": f"{median(sess_cv_duck):.6f}" if sess_cv_duck else "",
                "accel_cv": f"{median(sess_cv_accel):.6f}" if sess_cv_accel else "",
                "shared_state_bytes": f"{median(sess_shared):.0f}",
                "per_worker_state_bytes": f"{median(sess_worker):.0f}",
                "peak_rss_kb": f"{median(sess_rss):.0f}",
                "session_speedup_drift_frac": f"{drift:.6f}",
                "n_seeds": n_seeds,
                "n_sessions": len(sess_speed),
            }
        )

    # Attach overall geomean column
    og = {k: geomean(v) for k, v in overall_geomean.items()}
    for r in out_rows:
        g = og.get((int(r["scale"]), int(r["threads"])))
        r["overall_geomean_at_threads"] = f"{g:.6f}" if g and not math.isnan(g) else ""

    args.out.parent.mkdir(parents=True, exist_ok=True)
    fields = list(out_rows[0].keys()) if out_rows else []
    with args.out.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        w.writerows(out_rows)

    print(f"wrote {args.out} ({len(out_rows)} rows)")
    print("overall geomeans:")
    for (scale, threads), g in sorted(og.items()):
        print(f"  SF{scale} t={threads}: {g:.4f}x")


if __name__ == "__main__":
    main()
