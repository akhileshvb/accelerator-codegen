"""Profile interpretation helpers — signals, investigation leads, and technique palette."""

from __future__ import annotations

from typing import Any

# Broad technique catalog — always surfaced when profile data exists so the agent
# is not steered toward a single signal (e.g. scan amplification only).
_TECHNIQUE_CATALOG: dict[str, str] = {
    # Scan, filters, join order
    "early_filter_pushdown": (
        "Move sub/pre/num predicates into NativeScanSpec or runtime TableFilterSet"
    ),
    "native_scan_column_projection": (
        "Project only join/filter/aggregate columns in NativeScanSpec — fewer columns per chunk"
    ),
    "build_scan_order": (
        "Build filtered dimension tables first (sub→pre) before the wide fact scan (num)"
    ),
    "semi_join_pushdown": (
        "After building a filtered dimension, push join-key membership into the fact scan "
        "(BFTableFilter / bloom on scan columns)"
    ),
    "multi_column_scan_filters": (
        "Stack multiple BFTableFilter entries on one fact scan (e.g. join key + secondary dimension "
        "column) to skip chunks before vector decode"
    ),
    "qualifying_composite_key_set": (
        "Build a hash set of composite keys seen in an earlier phase; O(1) reject on the fact "
        "consume path before expensive probes"
    ),
    "deferred_dimension_scan": (
        "Defer tag/pre resolution: scan tag or pre only for keys seen in an earlier build phase"
    ),
    "nested_lookup_by_adsh": (
        "Partition pre/tag lookups by adsh (flat or nested map) to shrink probe working set per num row"
    ),
    # Vector / chunk dispatch
    "vector_fast_paths": (
        "DICTIONARY/FLAT chunk dispatch via SelectProjectedColumn; avoid UnifiedVectorFormat per row"
    ),
    "specialized_chunk_handlers": (
        "Separate handlers for all-flat, all-dict, and mixed layouts instead of one generic loop"
    ),
    # Aggregation / GROUP BY structure
    "group_interning": (
        "Intern GROUP BY keys at build time; consume path uses uint32_t group_id + dense sum/count vectors"
    ),
    "packed_group_keys": (
        "Pack interned dimension ids into a single integer group key for O(1) indexing"
    ),
    "fixed_slots_with_overflow": (
        "Dense fixed slots for hot groups + hash overflow for rare groups (TPC-H Q12 pattern)"
    ),
    "join_multiplicity": (
        "Track (adsh,tag,version) multiplicity when SQL join duplicates rows — avoid under-counting"
    ),
    # Strings / hashing
    "build_phase_strings": (
        "Confine ToStoredString / heap string allocation to sub/pre build — not num consume"
    ),
    "integer_fingerprint_keys": (
        "Replace std::string join keys with fixed-width fingerprints (AdshKey, TagVersionKey, etc.)"
    ),
    "avoid_std_string_hash": (
        "No std::hash<std::string> on hot paths — use intern tables or fingerprints resolved at build"
    ),
    # Consume / probe structure
    "fused_consume_single_pass": (
        "Single fused num pass: filter → dimension probe → aggregate without revisiting rows"
    ),
    "split_probe_vs_aggregate": (
        "Separate timers/logic for rejected probes vs qualifying aggregates to find true hot work"
    ),
    "flat_pre_entry_arrays": (
        "Store matching pre rows as contiguous arrays per adsh/tv_id — reduce pointer chasing in inner loop"
    ),
    # Finalize / TOP-N
    "topk_heap_finalize": (
        "Maintain a size-LIMIT heap during aggregation instead of sorting all groups at finalize"
    ),
    "partial_sort_output": (
        "partial_sort or nth_element for ORDER BY … LIMIT instead of full sort of active_groups"
    ),
    "lazy_output_strings": (
        "Defer resolving interned ids to VARCHAR until ProduceOutput — not during aggregation"
    ),
    # Memory / sizing
    "reserve_from_row_estimate": (
        "Size hash maps and intern tables from bind-time row_estimate / GetNativeScanTableRowCount"
    ),
    "column_so_layout": (
        "Keep hot per-row fields contiguous (struct-of-arrays or packed structs) for cache locality"
    ),
    # Constants / bind-time work
    "bind_time_filter_constants": (
        "Hoist literal filters (stmt='IS', uom='USD', fy=2023) to bind-time constants and memcmp fast paths"
    ),
    # Production path / measurement
    "fused_production_path": (
        "Wire fused kernel through sec_edgar_query_runtime so benchmark and gates use the same path"
    ),
    "coordinated_bundle": (
        "Ship 2–4 related techniques per A/B trial — not isolated one-liners"
    ),
}

# Default palette order when no strong signal boosts apply (diverse, not scan-centric).
_BASE_PALETTE_ORDER: list[str] = [
    "coordinated_bundle",
    "build_scan_order",
    "group_interning",
    "vector_fast_paths",
    "early_filter_pushdown",
    "fused_consume_single_pass",
    "semi_join_pushdown",
    "multi_column_scan_filters",
    "qualifying_composite_key_set",
    "nested_lookup_by_adsh",
    "specialized_chunk_handlers",
    "flat_pre_entry_arrays",
    "topk_heap_finalize",
    "reserve_from_row_estimate",
    "native_scan_column_projection",
    "deferred_dimension_scan",
    "packed_group_keys",
    "lazy_output_strings",
    "bind_time_filter_constants",
    "fused_production_path",
]


def _format_technique(technique_id: str, *, boosted: bool = False) -> str:
    desc = _TECHNIQUE_CATALOG.get(technique_id, technique_id)
    prefix = f"{technique_id}"
    if boosted:
        return f"{prefix} ★: {desc}"
    return f"{prefix}: {desc}"


def _add_unique(bucket: list[str], item: str) -> None:
    if item and item not in bucket:
        bucket.append(item)


def _build_strategy_palette(
    *,
    phases: dict[str, float],
    counters: dict[str, Any],
    dominant_phase: str | None,
    kernel_smells: list[dict[str, Any]] | None,
    has_profile: bool,
    speedup: float = 1.0,
) -> list[str]:
    """Broad technique menu with signal-matched items boosted to the top."""
    if not has_profile:
        return [
            "benchmark_profile: ensure benchmark emits profile CSV (ACCEL_BENCH_PROFILE=1 on fused production path)",
            _format_technique("fused_production_path"),
        ]

    boosted_ids: list[str] = []

    pre_rows = int(counters.get("pre_rows") or 0)
    generic_rows = int(counters.get("generic_rows") or 0)
    qualifying_rows = int(counters.get("qualifying_rows") or 0)
    distinct_inserts = int(counters.get("distinct_inserts") or 0)
    flat_chunks = int(counters.get("flat_chunks") or 0)
    dict_chunks = int(counters.get("dict_chunks") or 0)
    generic_chunks = int(counters.get("generic_chunks") or 0)

    num_scan_us = float(phases.get("num_scan_us") or 0)
    num_aggregate_us = float(phases.get("num_aggregate_us") or 0)
    pre_consume_us = float(phases.get("pre_consume_us") or 0)
    aggregate_us = num_aggregate_us or pre_consume_us
    sub_scan_us = float(phases.get("sub_scan_us") or 0)
    pre_table_scan_us = float(phases.get("pre_table_scan_us") or 0)
    finalize_us = float(phases.get("finalize_us") or 0)
    pre_scan_us = float(phases.get("pre_scan_us") or 0)
    total_phase_us = sum(phases.values()) or 1.0

    if pre_rows > 0 and qualifying_rows > 0 and pre_rows / qualifying_rows >= 3:
        for tid in (
            "early_filter_pushdown",
            "build_scan_order",
            "semi_join_pushdown",
            "multi_column_scan_filters",
            "qualifying_composite_key_set",
            "deferred_dimension_scan",
            "nested_lookup_by_adsh",
        ):
            _add_unique(boosted_ids, tid)

    if generic_rows > 0 and pre_rows > 0 and generic_rows / pre_rows >= 0.1:
        for tid in ("vector_fast_paths", "specialized_chunk_handlers"):
            _add_unique(boosted_ids, tid)

    if generic_chunks > 0 and (flat_chunks + dict_chunks + generic_chunks) > 0:
        generic_share = generic_chunks / (flat_chunks + dict_chunks + generic_chunks)
        if generic_share >= 0.1:
            _add_unique(boosted_ids, "vector_fast_paths")

    if num_scan_us > 0 and aggregate_us / num_scan_us >= 0.35:
        for tid in ("fused_consume_single_pass", "split_probe_vs_aggregate", "flat_pre_entry_arrays"):
            _add_unique(boosted_ids, tid)

    if dominant_phase in {"sub_scan_us", "pre_table_scan_us"} or sub_scan_us + pre_table_scan_us > 0.2 * total_phase_us:
        for tid in ("build_phase_strings", "group_interning", "integer_fingerprint_keys", "reserve_from_row_estimate"):
            _add_unique(boosted_ids, tid)

    if dominant_phase == "finalize_us" or (finalize_us > 0 and pre_scan_us > 0 and finalize_us / pre_scan_us >= 0.12):
        for tid in ("topk_heap_finalize", "partial_sort_output", "lazy_output_strings"):
            _add_unique(boosted_ids, tid)

    if qualifying_rows > 0 and distinct_inserts > 0 and distinct_inserts / qualifying_rows < 0.5:
        for tid in ("group_interning", "packed_group_keys", "fixed_slots_with_overflow"):
            _add_unique(boosted_ids, tid)

    if speedup < 0.5:
        _add_unique(boosted_ids, "coordinated_bundle")

    smell_ids = {str(s.get("id", "")) for s in (kernel_smells or [])}
    if "string_keyed_hash_map" in smell_ids or "map_find_in_hot_path" in smell_ids:
        for tid in ("group_interning", "packed_group_keys", "avoid_std_string_hash"):
            _add_unique(boosted_ids, tid)
    if "heap_string_in_hot_path" in smell_ids or "heap_string_allocation" in smell_ids:
        _add_unique(boosted_ids, "build_phase_strings")
    if "std_string_hash" in smell_ids:
        _add_unique(boosted_ids, "avoid_std_string_hash")

    strategies: list[str] = []
    for tid in boosted_ids:
        _add_unique(strategies, _format_technique(tid, boosted=True))

    for tid in _BASE_PALETTE_ORDER:
        if tid in _TECHNIQUE_CATALOG:
            _add_unique(strategies, _format_technique(tid, boosted=False))

    for tid, desc in _TECHNIQUE_CATALOG.items():
        entry = _format_technique(tid, boosted=False)
        if entry not in strategies:
            _add_unique(strategies, entry)

    return strategies[:24]


def build_profile_investigation(
    *,
    phases: dict[str, float],
    counters: dict[str, Any],
    speedup: float,
    best_speedup: float,
    dominant_phase: str | None,
    kernel_smells: list[dict[str, Any]] | None = None,
) -> dict[str, Any]:
    """Turn profile counters into signals, questions, and a technique palette."""
    signals: list[str] = []
    questions: list[str] = []

    pre_rows = int(counters.get("pre_rows") or 0)
    generic_rows = int(counters.get("generic_rows") or 0)
    qualifying_rows = int(counters.get("qualifying_rows") or 0)
    distinct_inserts = int(counters.get("distinct_inserts") or 0)
    flat_chunks = int(counters.get("flat_chunks") or 0)
    dict_chunks = int(counters.get("dict_chunks") or 0)
    generic_chunks = int(counters.get("generic_chunks") or 0)

    num_scan_us = float(phases.get("num_scan_us") or 0)
    num_aggregate_us = float(phases.get("num_aggregate_us") or 0)
    pre_consume_us = float(phases.get("pre_consume_us") or 0)
    aggregate_us = num_aggregate_us or pre_consume_us
    sub_scan_us = float(phases.get("sub_scan_us") or 0)
    finalize_us = float(phases.get("finalize_us") or 0)
    pre_scan_us = float(phases.get("pre_scan_us") or 0)

    has_profile = bool(phases) or pre_rows > 0 or qualifying_rows > 0

    if not has_profile:
        signals.append("profile_missing: no phase timings or row counters from profile gate")
        _add_unique(
            questions,
            "Profile gate returned no counters — verify benchmark supports ACCEL_BENCH_PROFILE on the fused production path.",
        )

    if dominant_phase:
        dominant_us = float(phases.get(dominant_phase) or 0)
        signals.append(f"dominant_phase={dominant_phase} ({dominant_us:,.0f} us)")

    if pre_rows > 0:
        signals.append(f"pre_rows={pre_rows:,}")
    if qualifying_rows > 0:
        signals.append(f"qualifying_rows={qualifying_rows:,}")
    if distinct_inserts > 0:
        signals.append(f"distinct_inserts={distinct_inserts:,}")

    if pre_rows > 0 and qualifying_rows > 0:
        amplification = pre_rows / qualifying_rows
        if amplification >= 5:
            signals.append(f"scan_amplification={amplification:.1f}x (pre_rows / qualifying_rows)")
            _add_unique(
                questions,
                "Many scanned rows never reach aggregation — what predicates or joins reject them, "
                "and can any filter move earlier (NativeScanSpec, runtime TableFilterSet, scan order)?",
            )

    if generic_rows > 0 and pre_rows > 0:
        pct = 100.0 * generic_rows / pre_rows
        signals.append(f"generic_fallback_rows={generic_rows:,} ({pct:.1f}% of pre_rows)")
        _add_unique(
            questions,
            "Where does the kernel fall back to UnifiedVectorFormat or a generic per-row loop? "
            "grep_context for `UnifiedVectorFormat` and chunk dispatch.",
        )

    total_chunks = flat_chunks + dict_chunks + generic_chunks
    if total_chunks > 0:
        signals.append(f"flat_chunks={flat_chunks}, dict_chunks={dict_chunks}, generic_chunks={generic_chunks}")

    if num_scan_us > 0 and aggregate_us > 0:
        share = aggregate_us / num_scan_us
        signals.append(f"num_aggregate_us={aggregate_us:,.0f} ({share:.0%} of num_scan_us={num_scan_us:,.0f})")
        _add_unique(
            questions,
            "What does `num_aggregate_us` include — only qualifying rows or the full consume loop "
            "(including rejected probes)? Trace timers around ProcessNum* / Accumulate.",
        )
    elif num_scan_us > 0 and qualifying_rows > 1_000:
        signals.append("num_scan_us reported without separate num_aggregate_us")
        _add_unique(
            questions,
            "Fused scan may bundle consume into scan timing — grep the production path to separate I/O from per-row work.",
        )

    if finalize_us > 0 and pre_scan_us > 0 and finalize_us / pre_scan_us >= 0.15:
        signals.append(f"finalize_us={finalize_us:,.0f} ({finalize_us / pre_scan_us:.0%} of pre_scan_us)")

    if sub_scan_us > 0 and pre_scan_us > 0:
        signals.append(f"sub_scan_us={sub_scan_us:,.0f}")

    if speedup < max(best_speedup, 1.0) and has_profile:
        _add_unique(
            questions,
            "Ship a coordinated bundle (2–4 related changes) per A/B trial — micro-opts alone won't close a large gap.",
        )

    for smell in kernel_smells or []:
        if smell.get("severity") != "high":
            continue
        msg = str(smell.get("message") or smell.get("id") or "structure smell")
        signals.append(f"static_review: {msg}")
        _add_unique(
            questions,
            "Static review flagged a smell — confirm with grep_context whether it is on the hot path "
            "(not build-only code).",
        )

    if not questions:
        _add_unique(
            questions,
            "Trace the production path (fused prescan → consume → aggregate); "
            "identify the dominant per-row work and verify with grep_context before editing.",
        )

    strategy_palette = _build_strategy_palette(
        phases=phases,
        counters=counters,
        dominant_phase=dominant_phase,
        kernel_smells=kernel_smells,
        has_profile=has_profile,
        speedup=speedup,
    )

    return {
        "profile_signals": signals,
        "investigation_questions": questions[:6],
        "strategy_palette": strategy_palette,
    }
