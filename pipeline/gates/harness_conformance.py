#!/usr/bin/env python3
"""The output clauses of the harness conformance check (run_harness_conformance.sh); each prints nothing when it holds
and a message naming its clause when not. The clauses are stated once, in the contract the bootstrap agent reads:
bench_sweep_contract.md (bench-*) and host_contract.md (calibrate-cost-model).

bench-rows <log> <points>        one row per entry of ACCEL_AUX_WRITE_FREQUENCIES, with every required column
bench-hits <hitting> <forced>    the run whose reads hit built a state, and counts hits: it tells the forced run apart
cost-model <json>                the calibrator wrote the keys the cost model needs
"""

from __future__ import annotations

import csv
import io
import json
import math
import sys
from pathlib import Path

REQUIRED_COLUMNS = ("suite write_frequency_pct workload_speedup query_only_speedup accel_hit_rate num_queries "
                    "accel_fallback_reads state_builds engine_ref_query_median_us accel_hit_median_us").split()
SPEEDUP_TOLERANCE = 0.02  # rounding of the printed medians
COST_MODEL_KEYS = ("interference_delta_per_query_us", "interference_rebuild_per_query_us")
MIN_HIT_RATE_GAP = 0.4


def bench_rows(log: str) -> list[dict[str, str]]:
    """The CSV rows under the `suite,` header; lines that are not the CSV are ignored."""
    lines = log.splitlines()
    start = next((i for i, line in enumerate(lines) if line.startswith("suite,")), None)
    if start is None:
        return []
    width = len(lines[start].split(","))
    body = [line for line in lines[start + 1:] if line.count(",") + 1 == width and not line.startswith(("#", "speedup_gate", "suite,"))]
    return list(csv.DictReader(io.StringIO("\n".join([lines[start], *body]))))


def check_rows(log: str, points: int) -> str:
    rows = bench_rows(log)
    if len(rows) != points:
        return (f"clause bench-rows: the bench printed {len(rows)} rows for {points} entries of "
                "ACCEL_AUX_WRITE_FREQUENCIES (one header starting `suite,` and one row per entry; bench_sweep_contract.md)")
    if missing := [c for c in REQUIRED_COLUMNS if c not in rows[0]]:
        return f"clause bench-columns: the bench row lacks the required columns {' '.join(missing)} (bench_sweep_contract.md)"
    for row in rows:  # the read axis is the ratio of the two medians it reports (bench_sweep_contract.md)
        try:
            ref, hit, speedup = (float(row[c]) for c in ("engine_ref_query_median_us", "accel_hit_median_us",
                                                         "query_only_speedup"))
        except (KeyError, ValueError):
            continue
        if hit > 0 and abs(speedup - ref / hit) > SPEEDUP_TOLERANCE * (ref / hit):
            return (f"clause bench-speedup: query_only_speedup {speedup:g} is not engine_ref_query_median_us / "
                    f"accel_hit_median_us = {ref / hit:g} (bench_sweep_contract.md)")
    return ""


def _number(rows: list[dict[str, str]], column: str) -> float:
    try:
        return float(rows[0][column])
    except (IndexError, KeyError, ValueError):
        return float("nan")


def check_hits(hitting: str, forced: str) -> str:
    """A declining module (rate 0) hits nothing to compare, so only a bench that reads a state is held to the rule."""
    hit, fallback = (_number(bench_rows(log), "accel_hit_rate") for log in (hitting, forced))
    if not hit > 0:
        return ""
    builds = _number(bench_rows(hitting), "state_builds")
    if not builds >= 1:
        return (f"clause bench-state-builds: the bench reports state_builds={builds:g} on a run whose reads hit a state: "
                "count the whole run, warmup included (bench_sweep_contract.md)")
    if not fallback == fallback or hit - fallback < MIN_HIT_RATE_GAP:
        return (f"clause bench-hit-rate: the bench reports hit rate {hit:.2f} with reads hitting and {fallback:.2f} with "
                "every read forced to the fallback (ACCEL_AUX_FORCE_FALLBACK=1): it must count hits, not assume them")
    return ""


def check_cost_model(path: str) -> str:
    try:
        model = json.loads(Path(path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return "clause calibrate-cost-model: the calibrator wrote no cost model JSON at the path it was given"
    # >= 0 only: the module conformance runs on declines, so there is no maintenance whose interference to measure;
    # the implementation's exit (aux_cost_model_calibrate) requires the measured costs > 0
    if bad := [k for k in COST_MODEL_KEYS if not _non_negative(model.get(k))]:
        return f"clause calibrate-cost-model: the cost model needs {' and '.join(bad)} as numbers >= 0 (host_bootstrap.md)"
    return ""


def _non_negative(value) -> bool:
    try:
        return math.isfinite(number := float(value)) and number >= 0
    except (TypeError, ValueError):
        return False


def main(argv: list[str]) -> int:
    command, *args = argv
    read = lambda p: Path(p).read_text(encoding="utf-8", errors="replace")  # noqa: E731
    if command == "bench-rows":
        message = check_rows(read(args[0]), len(args[1].split(",")))
    elif command == "bench-hits":
        message = check_hits(read(args[0]), read(args[1]))
    elif command == "cost-model":
        message = check_cost_model(args[0])
    else:
        raise SystemExit(__doc__)
    print(message)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
