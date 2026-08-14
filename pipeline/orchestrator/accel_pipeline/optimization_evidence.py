"""Extract and summarize measured bottleneck evidence (does not choose optimizations)."""

from __future__ import annotations

from typing import Any

STRUCTURAL_TIERS: tuple[str, ...] = (
    "eliminate_unnecessary_work",
    "exploit_selectivity",
    "reduce_decode_materialize",
    "scan_operator_fusion",
    "specialize_vector_layout",
    "hash_aggregation_state",
    "micro_optimization",
)

_SIGNAL_TIER: dict[str, str] = {
    "generic_fallback": "reduce_decode_materialize",
    "generic_rows": "reduce_decode_materialize",
    "generic_chunks": "reduce_decode_materialize",
    "vector_fast_paths": "specialize_vector_layout",
    "scan_amplification": "exploit_selectivity",
    "semi_join": "exploit_selectivity",
    "early_filter": "exploit_selectivity",
    "group_interning": "hash_aggregation_state",
    "string_keyed": "hash_aggregation_state",
    "topk_heap": "hash_aggregation_state",
    "fused": "scan_operator_fusion",
    "build_scan_order": "eliminate_unnecessary_work",
}


def _tier_for_signal(signal: str) -> str:
    lower = signal.lower()
    for key, tier in _SIGNAL_TIER.items():
        if key in lower:
            return tier
    return "micro_optimization"


def _amdahl_upper_bound_pct(phase_us: float, total_us: float) -> float | None:
    if total_us <= 0 or phase_us <= 0:
        return None
    return round(100.0 * phase_us / total_us, 1)


def extract_bottleneck_evidence(
    diagnosis: dict[str, Any],
    profile: dict[str, Any],
    kernel_review: dict[str, Any] | None = None,
    *,
    analysis: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Organize measured evidence by structural tier. Does not rank or prescribe fixes."""
    tiers: dict[str, dict[str, Any]] = {
        tier: {"tier": tier, "signals": [], "relevant_phases_us": {}, "suggested_grep_targets": []}
        for tier in STRUCTURAL_TIERS
    }

    phases = dict(diagnosis.get("profile_phases_us") or {})
    counters = dict(diagnosis.get("profile_counters") or {})
    total_phase_us = sum(float(v) for v in phases.values() if v) or 0.0

    dominant_phase = diagnosis.get("dominant_phase")
    dominant_us = float(diagnosis.get("dominant_phase_us") or 0.0)
    if dominant_phase and dominant_us > 0:
        tier = _tier_for_signal(str(dominant_phase))
        entry = tiers[tier]
        entry["signals"].append(
            {
                "metric": str(dominant_phase),
                "value": dominant_us,
                "context": f"dominant phase ({100.0 * dominant_us / max(total_phase_us, 1):.0f}% of profiled path)",
            }
        )
        entry["relevant_phases_us"][str(dominant_phase)] = dominant_us
        bound = _amdahl_upper_bound_pct(dominant_us, total_phase_us)
        if bound is not None:
            entry["upper_bound_pct"] = bound

    generic_rows = int(counters.get("generic_rows") or 0)
    pre_rows = int(counters.get("pre_rows") or 0)
    if generic_rows > 0 and pre_rows > 0:
        tier = tiers["reduce_decode_materialize"]
        pct = 100.0 * generic_rows / pre_rows
        tier["signals"].append(
            {
                "metric": "generic_rows",
                "value": generic_rows,
                "context": f"{pct:.1f}% of pre_rows={pre_rows}",
            }
        )

    qualifying_rows = int(counters.get("qualifying_rows") or 0)
    if pre_rows > 0 and qualifying_rows > 0:
        amplification = pre_rows / qualifying_rows
        if amplification >= 3:
            tier = tiers["exploit_selectivity"]
            tier["signals"].append(
                {
                    "metric": "scan_amplification",
                    "value": round(amplification, 2),
                    "context": f"pre_rows={pre_rows}, qualifying_rows={qualifying_rows}",
                }
            )

    for signal in diagnosis.get("profile_signals") or []:
        text = str(signal)
        tier_name = _tier_for_signal(text)
        tiers[tier_name]["signals"].append({"metric": "profile_signal", "value": text, "context": ""})

    for smell in (kernel_review or {}).get("smells") or []:
        tier_name = _tier_for_signal(str(smell.get("id", "")))
        tiers[tier_name]["signals"].append(
            {
                "metric": str(smell.get("id", "kernel_smell")),
                "value": smell.get("severity", ""),
                "context": str(smell.get("message", ""))[:120],
            }
        )
        line = smell.get("line")
        kernel_path = (kernel_review or {}).get("kernel_path", "")
        if line and kernel_path:
            tiers[tier_name]["suggested_grep_targets"].append(f"{kernel_path}:{line}")

    if analysis:
        for item in (analysis.get("likely_bottlenecks") or [])[:3]:
            tiers["eliminate_unnecessary_work"]["signals"].append(
                {"metric": "analysis_bottleneck", "value": str(item), "context": "from analysis.json"}
            )

    non_empty = [tiers[t] for t in STRUCTURAL_TIERS if tiers[t]["signals"]]
    for entry in non_empty:
        if "upper_bound_pct" not in entry:
            phase_us = sum(float(v) for v in entry["relevant_phases_us"].values())
            bound = _amdahl_upper_bound_pct(phase_us, total_phase_us) if phase_us > 0 else None
            if bound is not None:
                entry["upper_bound_pct"] = bound

    return {
        "tiers": non_empty,
        "speedup_median": diagnosis.get("speedup_median"),
        "dominant_phase": dominant_phase,
        "profile_counters": counters,
        "profile_phases_us": phases,
        "findings": (diagnosis.get("findings") or [])[:5],
        "lacking": (diagnosis.get("lacking") or [])[:4],
        "profile": profile,
    }
