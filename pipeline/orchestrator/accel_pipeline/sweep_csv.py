"""Reading the CSV an aux benchmark prints: one row per write frequency, with the store's diagnostics."""

from __future__ import annotations

import csv
from pathlib import Path
from typing import Any

STORE_DIAGNOSTIC_FIELDS: tuple[str, ...] = (
    "aux_state_bytes",
    "valid_partitions",
    "total_slots",
    "validity_slots_scanned_median",
    "validity_scan_us_median",
    "partitions_read_per_query_median",
    "state_bytes_read_median",
    "store_execute_calls",
    "accel_hit_median_us",
    "accel_hit_rate",
)

_INT_FIELDS = {
    "aux_state_bytes",
    "valid_partitions",
    "total_slots",
    "state_bytes_read_median",
    "store_execute_calls",
}
_FLOAT_FIELDS = {
    "validity_slots_scanned_median",
    "validity_scan_us_median",
    "partitions_read_per_query_median",
    "accel_hit_median_us",
    "accel_hit_rate",
}


def coerce_diagnostic_field(name: str, value: Any) -> Any:
    if value is None or value == "":
        return None
    if name in _INT_FIELDS:
        try:
            return int(float(value))
        except (TypeError, ValueError):
            return None
    if name in _FLOAT_FIELDS:
        try:
            return float(value)
        except (TypeError, ValueError):
            return None
    return value


# What a bench reports so measurement.check_measurement can tell a hit from a fallback.
MEASUREMENT_COUNTERS = ("num_queries", "accel_fallback_reads", "state_builds")
MEASUREMENT_TIMES = ("fallback_median_us",)


def _normalize_sweep_row_keys(row: dict[str, str]) -> dict[str, str]:
    """Accept legacy benchmark CSV headers (e.g. write_freq_pct)."""
    normalized = dict(row)
    aliases = {
        "write_freq_pct": "write_frequency_pct",
        "policy": "write_policy",
        "queries": "num_queries",
        "writes": "writes_executed",
        "query_speedup": "query_only_speedup",
        "cold_start_speedup": "cold_start_workload_speedup",
        "blended_speedup": "blended_query_speedup",
        "cold_start_workload_us": "cold_start_accel_workload_us",
    }
    for legacy, canonical in aliases.items():
        if canonical not in normalized and legacy in normalized:
            normalized[canonical] = normalized[legacy]
    return normalized


def _is_data_row(row: dict[str, str]) -> bool:
    row = _normalize_sweep_row_keys(row)
    if not row:
        return False
    first = next(iter(row.keys()), "")
    if first.startswith("#"):
        return False
    if "write_frequency_pct" not in row or "workload_speedup" not in row:
        return False
    try:
        float(row["workload_speedup"])
        int(float(row["write_frequency_pct"]))
    except (TypeError, ValueError):
        return False
    return True


def _parse_sweep_reader(reader: csv.DictReader) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for row in reader:
        row = _normalize_sweep_row_keys(row)
        if not _is_data_row(row):
            continue
        parsed: dict[str, Any] = {}
        for key, value in row.items():
            if value is None or value == "":
                continue
            if key in {
                "write_frequency_pct",
                "num_queries",
                "writes_scheduled",
                "writes_executed",
                "rows_touched_per_write",
                "aux_state_bytes",
                "valid_partitions",
                "total_slots",
                "state_bytes_read_median",
                "store_execute_calls",
                *MEASUREMENT_COUNTERS,
            }:
                try:
                    parsed[key] = int(float(value))
                except ValueError:
                    parsed[key] = value
            elif key in STORE_DIAGNOSTIC_FIELDS or key.endswith("_rate") or key in {
                "workload_speedup",
                "cold_start_workload_speedup",
                "query_only_speedup",
                "blended_query_speedup",
                "oracle_regret_pct",
                "validity_slots_scanned_median",
                "validity_scan_us_median",
                "partitions_read_per_query_median",
                "accel_hit_median_us",
            }:
                coerced = coerce_diagnostic_field(key, value)
                if coerced is None or isinstance(coerced, str):
                    try:
                        parsed[key] = float(value)
                    except ValueError:
                        parsed[key] = value
                else:
                    parsed[key] = coerced
            else:
                parsed[key] = value
        rows.append(parsed)
    return rows


def parse_sweep_csv(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(line for line in handle if not line.startswith("#"))
        return _parse_sweep_reader(reader)


def parse_aux_benchmark_output(text: str) -> list[dict[str, Any]]:
    """Parse aux benchmark stdout (single-point or sweep CSV)."""
    reader = csv.DictReader(
        line for line in text.splitlines() if line.strip() and not line.startswith("#")
    )
    return _parse_sweep_reader(reader)
