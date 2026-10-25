from __future__ import annotations

from pathlib import Path

from accel_pipeline.analysis_context import (
    analysis_for_audit,
    analysis_for_implementation,
    analysis_for_optimization,
    build_shared_docs_reference,
    compact_base_analysis_for_implementation,
)


def _sample_analysis() -> dict:
    selected = {
        "id": "daily_partial",
        "design_family": "flat_partitioned",
        "viability": "viable",
        "selected": True,
        "state_grain": "day",
        "output_grain": "grouped",
        "physical_organization": "hash by shipdate day",
        "state_payload": "partial sums",
        "composition_rule": "merge days",
        "operators_eliminated": ["Scan"],
        "residual_read_algorithm": "merge partition payloads",
        "estimated_state_units_read": {"typical": "P partitions", "worst_case": "all days"},
        "read_path_bottleneck_risk": "low",
        "read_path_scaling_variable": "partitions",
        "parameter_scaling_argument": "prefix by day_offset",
        "asymptotic_read_complexity": "O(P)",
        "practical_read_complexity": "small P",
        "point_update_complexity": "O(1)",
        "batch_update_complexity": "O(touched)",
        "storage_complexity": "O(days)",
        "mutation_fanout": "1-2",
        "rebuild_locality": "per day",
        "parameter_binding_behavior": "stable",
        "history_growth_behavior": "grows with days",
        "best_regime": "read-heavy",
    }
    alternate = {
        "id": "monthly_partial",
        "design_family": "monthly",
        "viability": "viable",
        "selected": False,
        "state_grain": "month",
        "physical_organization": "month buckets",
        "composition_rule": "merge months",
        "operators_eliminated": ["Scan"],
        "read_path_bottleneck_risk": "medium",
        "best_regime": "narrow month binds",
    }
    return {
        "accelerator_id": "tpch_q1_aux",
        "aux_function_name": "accel_tpch_q1_aux",
        "correctness_facts": {
            "state_design": {"selected_candidate_id": "daily_partial"},
            "dependent_tables": [{"name": "lineitem"}],
            "mutation_coverage": [{"table": "lineitem", "mutation": "update"}],
        },
        "state_candidate_search": {
            "candidates": [selected, alternate],
        },
        "performance_hypotheses": {
            "base_path_cost_decomposition": {"dominant_scans": ["lineitem"]},
            "initial_cost_model": {"config_path": "accelerators/tpch/q1/config/q1_aux_cost_model.json"},
            "likely_best_actions": ["ApplyDelta"],
        },
        "files_to_create": [{"path": "accelerators/tpch/q1/duckdb/aux/module.cpp", "role": "kernel"}],
    }


class TestAnalysisContextSlicing:
    def test_implementation_slice_omits_candidate_search(self):
        slim = analysis_for_implementation(_sample_analysis())
        assert "correctness_facts" in slim
        assert slim["selected_candidate_id"] == "daily_partial"
        assert "state_candidate_search" not in slim
        assert "base_path_cost_decomposition" not in slim.get("performance_hypotheses", {})

    def test_optimization_slice_has_alternates_only(self):
        slim = analysis_for_optimization(_sample_analysis())
        assert slim["active_candidate_id"] == "daily_partial"
        assert len(slim["viable_alternate_candidates"]) == 1
        assert slim["viable_alternate_candidates"][0]["id"] == "monthly_partial"

    def test_audit_slice_is_summary_only(self):
        slim = analysis_for_audit(_sample_analysis())
        assert slim["selected_candidate_id"] == "daily_partial"
        assert slim["dependent_table_names"] == ["lineitem"]
        assert "correctness_facts" not in slim

    def test_stage_specific_shared_docs(self):
        analysis_docs = build_shared_docs_reference(stage="analysis")
        impl_docs = build_shared_docs_reference(stage="implementation")
        assert "taxonomy_index.md" in analysis_docs
        assert "read_path_scaffold.md" in impl_docs
        assert "unit_test_contract.md" in impl_docs
        assert "aux_state_taxonomy.md" not in impl_docs

    def test_compact_base_analysis_accepts_dict_scan_order(self):
        payload = compact_base_analysis_for_implementation(
            {
                "query_summary": "Large orders query",
                "logical_plan": {"simplest_execution_strategy": "scan lineitem first"},
                "scan_order": {
                    "1": "lineitem aggregate",
                    "2": "orders probe",
                },
                "likely_bottlenecks": ["lineitem scan"],
                "optimization_hypotheses": ["dense array for order totals"],
                "files_to_create": [{"path": "accelerators/tpch/q18/duckdb/base/single-threaded/kernel.cpp"}],
            }
        )
        assert "lineitem aggregate" in payload
        assert "slice" not in payload
