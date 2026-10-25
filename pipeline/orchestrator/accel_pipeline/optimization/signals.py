"""Measured store/execution diagnostics assembly (facts only, no prescriptions)."""

from __future__ import annotations

from typing import Any

from accel_pipeline.sweep_csv import STORE_DIAGNOSTIC_FIELDS, coerce_diagnostic_field

def extract_store_diagnostics(metrics: dict[str, Any] | None) -> dict[str, Any]:
    """Pull factual store/execution measurements from benchmark metrics."""
    if not metrics:
        return {}
    nested = metrics.get("store_diagnostics")
    if isinstance(nested, dict) and nested:
        return {k: nested.get(k) for k in STORE_DIAGNOSTIC_FIELDS if nested.get(k) is not None}

    out: dict[str, Any] = {}
    for key in STORE_DIAGNOSTIC_FIELDS:
        if key in metrics and metrics[key] is not None:
            coerced = coerce_diagnostic_field(key, metrics[key])
            if coerced is not None:
                out[key] = coerced
    return out


def attach_store_diagnostics(metrics: dict[str, Any]) -> dict[str, Any]:
    """Ensure metrics dict carries a normalized store_diagnostics block."""
    block = extract_store_diagnostics(metrics)
    if not block:
        return metrics
    merged = dict(metrics)
    merged["store_diagnostics"] = block
    for key, value in block.items():
        merged.setdefault(key, value)
    return merged


def diagnostic_deltas(
    baseline: dict[str, Any] | None,
    candidate: dict[str, Any] | None,
) -> dict[str, Any]:
    """Compute candidate minus baseline on numeric diagnostic axes."""
    base = extract_store_diagnostics(baseline or {})
    cand = extract_store_diagnostics(candidate or {})
    deltas: dict[str, Any] = {}
    for key in STORE_DIAGNOSTIC_FIELDS:
        if key not in base or key not in cand:
            continue
        b_val = base[key]
        c_val = cand[key]
        if isinstance(b_val, (int, float)) and isinstance(c_val, (int, float)):
            deltas[key] = c_val - b_val
    return deltas


def organize_diagnostics_evidence(diagnostics: dict[str, Any]) -> dict[str, Any]:
    """Bucket measured facts for context organization — no fix recommendations."""
    diag = extract_store_diagnostics(diagnostics)
    if not diag:
        return {}
    return {
        "partition_store_cost": {
            k: diag[k]
            for k in (
                "aux_state_bytes",
                "valid_partitions",
                "total_slots",
                "validity_slots_scanned_median",
                "validity_scan_us_median",
            )
            if k in diag
        },
        "read_path_cost": {
            k: diag[k]
            for k in (
                "partitions_read_per_query_median",
                "accel_hit_median_us",
                "accel_hit_rate",
                "state_bytes_read_median",
            )
            if k in diag
        },
        "execution_volume": {
            k: diag[k] for k in ("store_execute_calls",) if k in diag
        },
    }


def format_store_diagnostics_markdown(
    diagnostics: dict[str, Any],
    *,
    delta: dict[str, Any] | None = None,
    label: str = "baseline",
) -> str:
    diag = extract_store_diagnostics(diagnostics)
    if not diag:
        return f"## Store diagnostics ({label})\n\n(no measurements)\n"
    lines = [f"## Store diagnostics ({label})", "", "```json"]
    import json

    payload: dict[str, Any] = {"measurements": diag}
    if delta:
        payload["delta_vs_baseline"] = delta
    lines.append(json.dumps(payload, indent=2))
    lines.append("```")
    return "\n".join(lines)
