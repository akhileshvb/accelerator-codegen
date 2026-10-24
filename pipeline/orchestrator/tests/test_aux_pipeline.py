from __future__ import annotations

import json
import re
from pathlib import Path

import pytest

from accel_pipeline.workload_facts import with_facts
from accel_pipeline.validation import AuxAnalysisValidationError, validate_aux_analysis, validate_aux_analysis_file


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
        "aux_function_name": "accel_tpch_q14_aux",
        "primitives_used": [{"name": "CowOrderedMap", "why": "ordered month keys with COW leaves"}],
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
                "read_work_ratio": {"typical": 5000, "worst": 400, "basis": "base scans 6e6 rows; design reads 1 to 12 entries"},
                "alternatives_considered": [
                    {"representation": "join_intermediate_cache", "selected": False},
                    {"representation": "group_partition_tree", "selected": False},
                ],
                "correctness_argument": "sums are sufficient for promo ratio",
                "speedup_argument": "eliminates dominant scan+join",
                "state_storage": [
                    {"component": "month totals", "primitive": "CowOrderedMap", "key": "int64 month",
                     "value": "two int128 sums", "read_access": "range fold over months",
                     "write_access": "add a delta to one month", "cow_unit": "one leaf page",
                     "entries": 84, "bytes_per_entry": 48, "target_bytes": 4032},
                ],
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
            "maintenance_actions_used": ["NoOp", "ApplyDelta", "RebuildPartition", "InvalidateAndRebuildOnRead"],
        },
        "performance_hypotheses": {
            "base_path_cost_decomposition": _base_path_cost_decomposition(),
        },
        "files_to_create": [{"path": "accelerators/tpch/q14/duckdb/aux/module.cpp", "role": "aux_kernel"}],
    }


class TestPrerequisiteRegex:
    def test_q1_regex_does_not_match_q10(self):
        from accel_pipeline.prerequisites import base_smoke_test_ctest_regex

        tpch = {"benchmark_name": "tpch"}
        assert base_smoke_test_ctest_regex(tpch, 1) == "^tpch_q1_smoke_test$"
        assert base_smoke_test_ctest_regex(tpch, 14) == "^tpch_q14_smoke_test$"
        sec = {"benchmark_name": "sec_edgar"}
        assert base_smoke_test_ctest_regex(sec, 1) == "^sec_edgar_q1_smoke_test$"


class TestAuxAnalysisValidation:
    def test_valid_analysis_passes(self):
        validate_aux_analysis(_sample_valid_analysis())

    def test_validates_slim_alternate_candidates(self):
        analysis = _sample_valid_analysis()
        validate_aux_analysis(analysis)

    def test_rejects_scan_like_read_work_ratio(self):
        analysis = _sample_valid_analysis()
        analysis["correctness_facts"]["state_design"]["read_work_ratio"]["typical"] = 1.5
        with pytest.raises(AuxAnalysisValidationError, match="design target unmet"):
            validate_aux_analysis(analysis)

    def test_accepts_unmet_read_work_ratio_with_justification(self):
        analysis = _sample_valid_analysis()
        ratio = analysis["correctness_facts"]["state_design"]["read_work_ratio"]
        ratio["typical"] = 1.5
        ratio["unmet_justification"] = "every candidate must visit the full qualifying range because the output is that large"
        with pytest.raises(AuxAnalysisValidationError, match="families_evaluated"):
            validate_aux_analysis(analysis)
        ratio["families_evaluated"] = [
            {"family": f, "typical_ratio": 1.5, "why_short": "output-sized"}
            for f in ("partition by predicate columns", "coarser pre-aggregation", "bound-ordered top-k")
        ]
        validate_aux_analysis(analysis)

    def test_rejects_missing_read_work_ratio(self):
        analysis = _sample_valid_analysis()
        del analysis["correctness_facts"]["state_design"]["read_work_ratio"]
        with pytest.raises(AuxAnalysisValidationError, match="read_work_ratio is required"):
            validate_aux_analysis(analysis)

    def test_rejects_non_numeric_read_work_ratio(self):
        analysis = _sample_valid_analysis()
        analysis["correctness_facts"]["state_design"]["read_work_ratio"]["worst"] = "lots"
        with pytest.raises(AuxAnalysisValidationError, match="positive numbers"):
            validate_aux_analysis(analysis)

    @staticmethod
    def _write(tmp_path, analysis, calls):
        path = tmp_path / "analysis.json"
        path.write_text(json.dumps(analysis))
        (tmp_path / "data_queries.jsonl").write_text(
            "".join(json.dumps({"tool": "query_data", "args": {"sql": sql}, "error": err}) + "\n" for sql, err in calls))
        return path

    @staticmethod
    def _probe_all(analysis):
        for i, c in enumerate(analysis["state_candidate_search"]["candidates"]):
            c["units_probe"] = f"SELECT count(*) FROM t WHERE k < {i}"
            c["measured_units"] = f"{i} entries"
        return [c["units_probe"] for c in analysis["state_candidate_search"]["candidates"]]

    def test_every_candidate_needs_a_logged_units_probe(self, tmp_path):
        analysis = _sample_valid_analysis()
        assert len(analysis["state_candidate_search"]["candidates"]) >= 3
        path = self._write(tmp_path, analysis, [])
        with pytest.raises(AuxAnalysisValidationError, match="units_probe is required"):
            validate_aux_analysis_file(path)
        probes = self._probe_all(analysis)
        path = self._write(tmp_path, analysis, [(p, None) for p in probes[1:]])
        with pytest.raises(AuxAnalysisValidationError, match=r"candidates\[0\]\.units_probe does not match"):
            validate_aux_analysis_file(path)
        path = self._write(tmp_path, analysis, [(p.replace(" ", "\n  ", 1) + ";", None) for p in probes])
        validate_aux_analysis_file(path)

    def test_failed_query_is_not_a_probe(self, tmp_path):
        analysis = _sample_valid_analysis()
        probes = self._probe_all(analysis)
        path = self._write(tmp_path, analysis, [(probes[0], "time box")] + [(p, None) for p in probes[1:]])
        with pytest.raises(AuxAnalysisValidationError, match="does not match"):
            validate_aux_analysis_file(path)

    def test_probes_must_be_distinct(self, tmp_path):
        analysis = _sample_valid_analysis()
        probes = self._probe_all(analysis)
        for c in analysis["state_candidate_search"]["candidates"]:
            c["units_probe"] = probes[0]
        path = self._write(tmp_path, analysis, [(probes[0], None)])
        with pytest.raises(AuxAnalysisValidationError, match="distinct units_probe"):
            validate_aux_analysis_file(path)

    def test_state_storage_names_a_listed_primitive_and_sizes(self):
        analysis = _sample_valid_analysis()
        entry = analysis["correctness_facts"]["state_design"]["state_storage"][0]
        entry["primitive"] = "SomeUnlistedMap"
        with pytest.raises(AuxAnalysisValidationError, match="not listed in primitives_used"):
            validate_aux_analysis(analysis)
        entry["primitive"], entry["bytes_per_entry"] = "CowOrderedMap", "about 48"
        with pytest.raises(AuxAnalysisValidationError, match="bytes_per_entry must be a non-negative number"):
            validate_aux_analysis(analysis)
        del analysis["correctness_facts"]["state_design"]["state_storage"]
        with pytest.raises(AuxAnalysisValidationError, match="state_storage is required"):
            validate_aux_analysis(analysis)

    def test_one_primitive_may_not_hold_every_component(self):
        analysis = _sample_valid_analysis()
        storage = analysis["correctness_facts"]["state_design"]["state_storage"]
        storage += [dict(storage[0], component="order directory"), dict(storage[0], component="run pages")]
        with pytest.raises(AuxAnalysisValidationError, match="narrowest generic primitive"):
            validate_aux_analysis(analysis)
        analysis["primitives_used"].append({"name": "CowPagedArray", "why": "run pages"})
        storage[2]["primitive"] = "CowPagedArray"
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

    def test_repair_fills_fewer_than_three_explanation(self):
        from accel_pipeline.validation import repair_aux_analysis

        analysis = _sample_valid_analysis()
        analysis["state_candidate_search"]["candidates"] = analysis["state_candidate_search"]["candidates"][:2]
        analysis["state_candidate_search"]["fewer_than_three_explanation"] = None
        repaired, fixes = repair_aux_analysis(analysis)
        assert fixes
        assert repaired["state_candidate_search"]["fewer_than_three_explanation"]
        validate_aux_analysis(repaired)

    def test_rejects_missing_read_path_fields(self):
        analysis = _sample_valid_analysis()
        del analysis["correctness_facts"]["state_design"]["read_path_bottleneck_risk"]
        with pytest.raises(AuxAnalysisValidationError, match="read_path_bottleneck_risk"):
            validate_aux_analysis(analysis)


class TestWorkloadConfig:
    def test_q10_aux_workload_loads(self):
        from accel_pipeline.config import load_workload

        workload = load_workload("tpch_q10_aux", REPO_ROOT, engine="duckdb")
        assert workload["query_number"] == 10
        assert workload["workload_kind"] == "aux_state"
        assert workload["aux_function_name"] == "accel_tpch_q10_aux"
        assert workload["max_structural_reanalysis"] == 2
        assert len(workload["parameters"]) == 1
        tables = {t["name"] for t in with_facts(REPO_ROOT, workload, workload["query_number"])["dependent_tables"]}
        assert tables == {"customer", "orders", "lineitem", "nation"}

    def test_q12_aux_workload_loads(self):
        from accel_pipeline.config import load_workload

        workload = load_workload("tpch_q12_aux", REPO_ROOT, engine="duckdb")
        assert workload["query_number"] == 12
        assert workload["workload_kind"] == "aux_state"
        assert workload["aux_function_name"] == "accel_tpch_q12_aux"
        assert workload["max_structural_reanalysis"] == 2
        assert len(workload["parameters"]) == 3
        tables = {t["name"] for t in with_facts(REPO_ROOT, workload, workload["query_number"])["dependent_tables"]}
        assert tables == {"orders", "lineitem"}

    def test_sec_edgar_q1_aux_workload_loads(self):
        from accel_pipeline.config import load_pipeline_config, load_workload, resolve_workload_paths

        workload = load_workload("sec_edgar_q1_aux", REPO_ROOT)
        assert workload["query_number"] == 1
        assert workload["workload_kind"] == "aux_state"
        assert workload["benchmark_name"] == "sec_edgar"
        assert workload["aux_function_name"] == "accel_sec_edgar_q1_aux"
        assert workload["prerequisite_workload"] == "sec_edgar_q1"
        assert workload["aux_bench_sweep"]["suites"] == "filing_append"
        tables = {t["name"] for t in with_facts(REPO_ROOT, workload, workload["query_number"])["dependent_tables"]}
        assert tables == {"pre"}

        paths = resolve_workload_paths(load_pipeline_config(repo_root=REPO_ROOT), workload, 1)
        assert paths["aux_state_subdir"] == "accelerators/sec_edgar/q1/duckdb/aux"
        assert paths["aux_bench_target"] == "benchmark_sec_edgar_q1_aux"
        assert paths["cost_model_config"] == "accelerators/sec_edgar/q1/config/sec_edgar_q1_aux_cost_model_duckdb.json"
        assert workload.get("post_implementation_gates", [])[0] == "aux_correctness"
        assert "accelerators/sec_edgar/q1/**" in workload.get("write_paths", [])

    def test_sec_edgar_q2_aux_workload_loads(self):
        from accel_pipeline.config import load_pipeline_config, load_workload, resolve_workload_paths

        workload = load_workload("sec_edgar_q2_aux", REPO_ROOT)
        assert workload["query_number"] == 2
        assert workload["workload_kind"] == "aux_state"
        assert workload["benchmark_name"] == "sec_edgar"
        assert workload["aux_function_name"] == "accel_sec_edgar_q2_aux"
        assert workload["prerequisite_workload"] == "sec_edgar_q2"
        assert workload["aux_bench_sweep"]["suites"] == "value_append"
        tables = {t["name"] for t in with_facts(REPO_ROOT, workload, workload["query_number"])["dependent_tables"]}
        assert tables == {"num", "sub"}

        paths = resolve_workload_paths(load_pipeline_config(repo_root=REPO_ROOT), workload, 2)
        assert paths["aux_state_subdir"] == "accelerators/sec_edgar/q2/duckdb/aux"
        assert paths["aux_bench_target"] == "benchmark_sec_edgar_q2_aux"
        assert paths["cost_model_config"] == "accelerators/sec_edgar/q2/config/sec_edgar_q2_aux_cost_model_duckdb.json"
        assert paths["aux_bench_suites"] == "value_append"


class TestPromptRendering:
    def test_aux_analysis_prompt_has_no_q14_reference_leakage(self):
        from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload
        from accel_pipeline.prompts import render_prompt

        pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
        workload = load_workload("tpch_q14_aux", REPO_ROOT, engine="duckdb")
        pipeline_config = apply_workload_overrides(pipeline_config, workload)
        prompt = render_prompt(pipeline_config, "01_analysis.md", workload, 14, "analysis")
        lowered = prompt.lower()
        assert "accelerators/tpch/q14/duckdb/aux" not in lowered
        assert "mirroring q14" not in lowered
        assert "reference implementation" not in lowered
        assert not re.search(r"\{\{\w+\}\}", prompt), "unresolved placeholder"
        for field in ("fitness_assessment", "base_path_cost_decomposition", "units_probe", "measured_units",
                      "read_work_ratio", "state_storage", "scale_plan", "mutation_coverage", "files_to_create",
                      "validate_analysis", "update_analysis"):
            assert field in prompt, field
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
        assert not re.search(r"\{\{\w+\}\}", impl), "unresolved placeholder"
        assert "## Design contract" in impl and "```json" in impl
        opt = render_prompt(
            pipeline_config, "03_optimization_unified.md", workload, 14, "optimization_unified", optimization_iter=1
        )
        assert "hard guidelines" in opt.lower() or "native base" in opt.lower()
        assert "{{analysis_json}}" not in opt

    def test_dry_run_all_aux_stages(self):
        from accel_pipeline.config import apply_workload_overrides, load_model_config, load_pipeline_config, load_workload
        from accel_pipeline.stages.base import run_agent_stage

        pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
        workload = load_workload("tpch_q14_aux", REPO_ROOT, engine="duckdb")
        pipeline_config = apply_workload_overrides(pipeline_config, workload)
        model_config = load_model_config(repo_root=REPO_ROOT)
        for stage, prompt in [
            ("analysis", "01_analysis.md"),
            ("test_suite", "01b_test_suite.md"),
            ("implementation", "02_implementation.md"),
            ("optimization_unified", "03_optimization_unified.md"),
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
