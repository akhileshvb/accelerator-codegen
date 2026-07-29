from __future__ import annotations

import json
from pathlib import Path

import pytest

from accel_pipeline.analysis_quality import load_fixture_dir, score_analysis_against_fixture, _load_fixture
from accel_pipeline.validation import AuxAnalysisValidationError, validate_aux_analysis


REPO_ROOT = Path(__file__).resolve().parents[3]


def _base_path_cost_decomposition() -> dict:
    return {
        "dominant_scans": ["lineitem fact scan"],
        "dominant_joins": ["lineitem join part on l_partkey"],
        "aggregation_grouping_work": "grouped ratio aggregate over joined rows",
        "sorting_topk_work": "none",
        "parameter_dependent_work": "month window predicate on l_shipdate",
        "work_repeated_across_invocations": ["full lineitem-part join scan"],
        "cardinality_scaling": "O(lineitem rows) per query",
        "parameter_range_scaling": "selected month partition only; narrow bind range",
        "output_cardinality_scaling": "O(1) scalar output",
        "work_remaining_after_aux_replacement": [
            "ratio of two pre-aggregated sums",
            "possible partition lookup for bind month",
        ],
    }


def _alternate_viable_candidate(
    *,
    cid: str,
    family: str,
    organization: str,
    composition: str,
) -> dict:
    return {
        "id": cid,
        "design_family": family,
        "viability": "viable",
        "selected": False,
        "state_grain": f"{cid} grain",
        "physical_organization": organization,
        "composition_rule": composition,
        "operators_eliminated": ["ScanLineitem"],
        "read_path_bottleneck_risk": "medium",
        "best_regime": "alternate regime summary",
    }


def _candidate(
    *,
    cid: str,
    family: str,
    organization: str,
    composition: str,
    selected: bool = False,
    viability: str = "viable",
    rejection_reason: str | None = None,
) -> dict:
    return {
        "id": cid,
        "design_family": family,
        "viability": viability,
        "selected": selected,
        "rejection_reason": rejection_reason,
        "state_grain": f"{cid} grain",
        "output_grain": "scalar_aggregate",
        "physical_organization": organization,
        "state_payload": f"{cid} payload",
        "composition_rule": composition,
        "operators_eliminated": ["ScanLineitem", "HashJoinPart"],
        "residual_read_algorithm": f"read {cid} and combine",
        "estimated_state_units_read": {"typical": "1 partition", "worst_case": "12 partitions"},
        "read_path_bottleneck_risk": "low",
        "read_path_scaling_variable": "number_of_partitions",
        "parameter_scaling_argument": "one month partition per binding",
        "asymptotic_read_complexity": "O(1) typical",
        "practical_read_complexity": "single partition read",
        "point_update_complexity": "O(1) partition touch",
        "batch_update_complexity": "O(days touched)",
        "storage_complexity": "O(months in history)",
        "mutation_fanout": "1-2 partitions per row",
        "rebuild_locality": "per-month partition",
        "parameter_binding_behavior": "stable for fixed month binding",
        "history_growth_behavior": "grows with calendar months stored",
        "best_regime": "read-heavy repeated month binding",
    }


def _sample_valid_analysis() -> dict:
    selected = _candidate(
        cid="monthly_partial_aggregate",
        family="flat_partitioned_sufficient_summaries",
        organization="hash map keyed by YYYYMM shipdate month",
        composition="promo_sum / total_sum from month partition payload",
        selected=True,
    )
    return {
        "accelerator_id": "tpch_q14_aux",
        "base_function_name": "accel_tpch_q14",
        "aux_function_name": "accel_tpch_q14_aux",
        "fitness_assessment": {
            "recommended": True,
            "confidence": "high",
            "scope": "per_query",
            "signals_for": ["stable_partition_grain", "bounded_invalidation"],
            "signals_against": [],
            "partition_key_source": "bind_parameter",
            "incidental_reuse": None,
            "rationale": "Monthly partition grain with partial aggregates.",
        },
        "state_candidate_search": {
            "hints_adopted": {"partitioning": "monthly shipdate hint adopted after comparison"},
            "hints_rejected": {"flat_day_partitions": "too many partitions on read for narrow month bind"},
            "fewer_than_three_explanation": None,
            "candidates": [
                selected,
                _alternate_viable_candidate(
                    cid="join_intermediate_cache",
                    family="compact_join_intermediate",
                    organization="materialized lineitem rows joined to promo part subset",
                    composition="scan narrow joined subset and aggregate",
                ),
                _alternate_viable_candidate(
                    cid="group_partition_tree",
                    family="tree_or_index_aggregate_organization",
                    organization="ordered index over shipdate months with subtree sums",
                    composition="tree descent to month range then aggregate",
                ),
            ],
        },
        "correctness_facts": {
            "dependent_tables": [
                {"name": "lineitem", "watched_columns": ["l_shipdate", "l_partkey", "l_extendedprice", "l_discount"]},
                {"name": "part", "watched_columns": ["p_partkey", "p_type"]},
            ],
            "partitioning": {
                "strategy": "monthly_shipdate",
                "key_function": "PartitionKeyFromStartDate",
                "key_source": "bind_parameter",
                "mutation_to_partition_mapping": "shipdate column maps to monthly bucket",
            },
            "state_design": {
                "selected_candidate_id": "monthly_partial_aggregate",
                "chosen_representation": "partial_aggregate",
                "state_grain": "monthly_partition",
                "output_grain": "scalar_aggregate",
                "physical_organization": "hash map keyed by YYYYMM shipdate month",
                "composition_rule": "promo_sum / total_sum from month partition payload",
                "is_full_result_cache": False,
                "negligible_speedup_risk": "low",
                "work_eliminated_on_fast_path": ["lineitem scan", "part join"],
                "operators_claimed_eliminated": ["ScanLineitem", "HashJoinPart"],
                "residual_read_algorithm": "read monthly partition and combine two sums",
                "read_path_remaining_work": ["ratio from two sums"],
                "read_path_bottleneck_risk": "low",
                "read_path_scaling_variable": "number_of_partitions",
                "estimated_state_units_read": {"typical": "1 partition", "worst_case": "12 partitions"},
                "parameter_scaling_argument": "one month partition per binding",
                "alternatives_considered": [
                    {"representation": "join_intermediate_cache", "selected": False},
                    {"representation": "group_partition_tree", "selected": False},
                ],
                "correctness_argument": "sums are sufficient for promo ratio",
                "speedup_argument": "eliminates dominant scan+join",
            },
            "state_payload": {"struct_name": "MonthTotals", "fields": [], "what_is_cached": "sums"},
            "mutation_coverage": [
                {"table": "lineitem", "mutation": "insert", "behavior": "supported_delta"},
                {"table": "lineitem", "mutation": "update", "behavior": "supported_delta"},
                {"table": "lineitem", "mutation": "delete", "behavior": "bounded_rebuild"},
                {"table": "part", "mutation": "insert", "behavior": "invalidate_safely"},
                {"table": "part", "mutation": "update", "behavior": "supported_delta"},
                {"table": "part", "mutation": "delete", "behavior": "invalidate_safely"},
            ],
            "fallback_conditions": ["partition_missing", "txn_blocked"],
            "maintenance_actions_used": ["NoOp", "ApplyDelta", "RebuildPartition", "InvalidateAndFallback"],
        },
        "performance_hypotheses": {
            "base_path_cost_decomposition": _base_path_cost_decomposition(),
        },
        "files_to_create": [{"path": "src/aux_state/q14/q14_aux_kernel.cpp", "role": "aux_kernel"}],
    }


class TestPrerequisiteRegex:
    def test_q1_regex_does_not_match_q10(self):
        from accel_pipeline.prerequisites import base_smoke_test_ctest_regex

        assert base_smoke_test_ctest_regex(1) == "^tpch_q1_smoke_test$"
        assert base_smoke_test_ctest_regex(14) == "^tpch_q14_smoke_test$"


class TestAuxAnalysisValidation:
    def test_valid_analysis_passes(self):
        validate_aux_analysis(_sample_valid_analysis())

    def test_validates_slim_alternate_candidates(self):
        analysis = _sample_valid_analysis()
        validate_aux_analysis(analysis)

    def test_rejects_poor_fitness(self):
        analysis = _sample_valid_analysis()
        analysis["fitness_assessment"]["recommended"] = False
        with pytest.raises(AuxAnalysisValidationError):
            validate_aux_analysis(analysis)

    def test_allows_poor_fitness_override(self):
        analysis = _sample_valid_analysis()
        analysis["fitness_assessment"]["recommended"] = False
        validate_aux_analysis(analysis, allow_poor_fitness=True)

    def test_rejects_incomplete_mutation_coverage(self):
        analysis = _sample_valid_analysis()
        analysis["correctness_facts"]["mutation_coverage"] = analysis["correctness_facts"]["mutation_coverage"][:2]
        with pytest.raises(AuxAnalysisValidationError):
            validate_aux_analysis(analysis)

    def test_rejects_full_result_cache(self):
        analysis = _sample_valid_analysis()
        analysis["correctness_facts"]["state_design"]["is_full_result_cache"] = True
        with pytest.raises(AuxAnalysisValidationError):
            validate_aux_analysis(analysis)

    def test_rejects_missing_base_path_decomposition(self):
        analysis = _sample_valid_analysis()
        analysis["performance_hypotheses"] = {}
        with pytest.raises(AuxAnalysisValidationError, match="base_path_cost_decomposition"):
            validate_aux_analysis(analysis)

    def test_rejects_insufficient_candidates(self):
        analysis = _sample_valid_analysis()
        analysis["state_candidate_search"]["candidates"] = analysis["state_candidate_search"]["candidates"][:1]
        analysis["state_candidate_search"]["fewer_than_three_explanation"] = None
        with pytest.raises(AuxAnalysisValidationError, match="at least 3 viable candidates"):
            validate_aux_analysis(analysis)

    def test_rejects_missing_read_path_fields(self):
        analysis = _sample_valid_analysis()
        del analysis["correctness_facts"]["state_design"]["read_path_bottleneck_risk"]
        with pytest.raises(AuxAnalysisValidationError, match="read_path_bottleneck_risk"):
            validate_aux_analysis(analysis)


class TestAnalysisQualityFixtures:
    def test_q14_fixture_scores_golden_analysis(self):
        fixture_path = REPO_ROOT / "pipeline" / "fixtures" / "analysis_quality" / "q14_strong.yaml"
        fixture = _load_fixture(fixture_path)
        scores = score_analysis_against_fixture(_sample_valid_analysis(), fixture)
        assert scores["dependencies"] == 1.0
        assert scores["fitness"] == 1.0
        assert scores["state_design"] == 1.0
        assert scores["base_path_decomposition"] == 1.0
        assert scores["candidate_search"] == 1.0

    def test_fixtures_exist(self):
        paths = load_fixture_dir(REPO_ROOT)
        assert len(paths) >= 3


class TestWorkloadConfig:
    def test_q10_aux_workload_loads(self):
        from accel_pipeline.config import load_workload

        workload = load_workload("tpch_q10_aux", REPO_ROOT)
        assert workload["query_number"] == 10
        assert workload["workload_kind"] == "aux_state"
        assert workload["aux_function_name"] == "accel_tpch_q10_aux"
        assert workload["max_structural_reanalysis"] == 2
        assert len(workload["parameters"]) == 1
        tables = {t["name"] for t in workload["dependent_tables"]}
        assert tables == {"customer", "orders", "lineitem", "nation"}

    def test_q12_aux_workload_loads(self):
        from accel_pipeline.config import load_workload

        workload = load_workload("tpch_q12_aux", REPO_ROOT)
        assert workload["query_number"] == 12
        assert workload["workload_kind"] == "aux_state"
        assert workload["aux_function_name"] == "accel_tpch_q12_aux"
        assert workload["max_structural_reanalysis"] == 2
        assert len(workload["parameters"]) == 3
        tables = {t["name"] for t in workload["dependent_tables"]}
        assert tables == {"orders", "lineitem"}


class TestPromptRendering:
    def test_aux_analysis_prompt_has_no_q14_reference_leakage(self):
        from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload
        from accel_pipeline.prompts import render_prompt

        pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
        workload = load_workload("tpch_q14_aux", REPO_ROOT)
        pipeline_config = apply_workload_overrides(pipeline_config, workload)
        prompt = render_prompt(pipeline_config, "01_analysis.md", workload, 14, "analysis")
        lowered = prompt.lower()
        assert "src/aux_state/q14" not in lowered
        assert "mirroring q14" not in lowered
        assert "reference implementation" not in lowered
        assert "{{aux_state_fitness}}" not in prompt
        assert "fitness" in lowered
        assert "hints only" in lowered
        assert "read on demand" in lowered or "read_file" in lowered
        assert "base-path cost decomposition" in lowered or "cost decomposition" in lowered
        assert "state candidate search" in lowered or "materially different" in lowered
        assert "revision evidence" in lowered
        assert "taxonomy_index" in lowered
        assert "{{analysis_revision_request}}" not in prompt
        revision_prompt = render_prompt(
            pipeline_config,
            "01_analysis.md",
            workload,
            14,
            "analysis",
            extra_context={"analysis_revision_request": "Bottleneck: scan_for_scan at 50% writes"},
        )
        assert "Bottleneck: scan_for_scan at 50% writes" in revision_prompt
        assert "revision" in revision_prompt.lower()
        impl = render_prompt(pipeline_config, "02_implementation.md", workload, 14, "implementation")
        assert "implementation contract" in impl.lower()
        assert "full analysis" in impl.lower()
        opt = render_prompt(pipeline_config, "03_optimization.md", workload, 14, "optimization", optimization_iter=1)
        assert "retrieval brief" in opt.lower()
        assert "full analysis" in opt.lower()
        assert "{{analysis_json}}" not in opt

    def test_dry_run_all_aux_stages(self):
        from accel_pipeline.config import apply_workload_overrides, load_model_config, load_pipeline_config, load_workload
        from accel_pipeline.stages.base import run_agent_stage

        pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
        workload = load_workload("tpch_q14_aux", REPO_ROOT)
        pipeline_config = apply_workload_overrides(pipeline_config, workload)
        model_config = load_model_config(repo_root=REPO_ROOT)
        for stage, prompt in [
            ("analysis", "01_analysis.md"),
            ("implementation", "02_implementation.md"),
            ("optimization", "03_optimization.md"),
            ("audit", "04_audit.md"),
        ]:
            result = run_agent_stage(
                stage=stage,
                prompt_file=prompt,
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name="tpch_q14_aux",
                query=14,
                user_message="dry-run",
                dry_run=True,
            )
            assert result["dry_run"] is True
