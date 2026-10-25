from accel_pipeline.optimization.profile_investigation import build_profile_investigation


def test_scan_amplification_produces_investigation_questions_and_strategies():
    result = build_profile_investigation(
        phases={"pre_scan_us": 2_800_000, "num_scan_us": 2_790_000},
        counters={
            "pre_rows": 33_000_000,
            "generic_rows": 0,
            "dict_chunks": 20_000,
            "qualifying_rows": 5_000_000,
            "distinct_inserts": 8_000,
        },
        speedup=1.02,
        best_speedup=1.03,
        dominant_phase="pre_scan_us",
    )
    assert any("scan_amplification" in s for s in result["profile_signals"])
    assert result["investigation_questions"]
    palette = result["strategy_palette"]
    assert len(palette) >= 12
    assert any("early_filter_pushdown" in s for s in palette)
    assert any("topk_heap_finalize" in s for s in palette)
    assert any("group_interning" in s for s in palette)


def test_generic_fallback_suggests_vector_investigation():
    result = build_profile_investigation(
        phases={"pre_consume_us": 3_000_000},
        counters={"pre_rows": 1_000_000, "generic_rows": 900_000},
        speedup=0.6,
        best_speedup=0.6,
        dominant_phase="pre_consume_us",
    )
    assert any("generic_fallback" in s for s in result["profile_signals"])
    assert any("UnifiedVectorFormat" in q for q in result["investigation_questions"])
    palette = result["strategy_palette"]
    assert any("vector_fast_paths" in s for s in palette)
    assert len(palette) >= 10


def test_missing_profile_suggests_benchmark_fix():
    result = build_profile_investigation(
        phases={},
        counters={},
        speedup=0.1,
        best_speedup=0.1,
        dominant_phase=None,
    )
    assert any("profile_missing" in s for s in result["profile_signals"])
    assert any("benchmark_profile" in s for s in result["strategy_palette"])
