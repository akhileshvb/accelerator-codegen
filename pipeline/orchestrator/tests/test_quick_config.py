from __future__ import annotations

from pathlib import Path

from accel_pipeline.config import (
    QuickTierConfig,
    apply_workload_overrides,
    load_pipeline_config,
    load_workload,
    workload_supported_engines,
)

REPO = Path(__file__).resolve().parents[3]
ENGINE = workload_supported_engines("tpch_q6_aux", REPO)[0]


def test_the_shipped_quick_tier_block_matches_the_design():
    config = load_pipeline_config(repo_root=REPO).quick_tier
    assert config == QuickTierConfig(
        enabled=True, timeout_sec=120, build_stamp_cache=True, differential_probes=3,
        background_gates=("aux_exit", "aux_correctness", "aux_unit", "prims_tests", "aux_multi_connection", "aux_cross_accel"),
        parallel_gates=True, gate_jobs=0, write_cost_budget_share=0.5,
        admission_gates=("aux_exit", "aux_correctness", "aux_bench_sweep", "aux_tsan"),
    )


def test_the_quick_tier_survives_the_workload_overrides():
    config = load_pipeline_config(repo_root=REPO)
    workload = load_workload("tpch_q6_aux", REPO, engine=ENGINE)
    assert apply_workload_overrides(config, workload).quick_tier == config.quick_tier


def test_the_aux_workload_carries_the_quick_bench_tier():
    workload = load_workload("tpch_q6_aux", REPO, engine=ENGINE)
    assert workload["aux_bench_sweep_quick"]["bench_queries"] == 5
