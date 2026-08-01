from __future__ import annotations

import json
from pathlib import Path
from unittest.mock import MagicMock

import pytest

from accel_pipeline.structural_reanalysis import (
    BOTTLENECK_TYPES,
    can_rerun_analysis,
    clear_request,
    load_pending_request,
    max_structural_reanalysis_count,
    process_after_stage,
    request_path,
    validate_request,
    write_request,
)

REPO_ROOT = Path(__file__).resolve().parents[3]


def _valid_request(**overrides) -> dict:
    payload = {
        "requested": True,
        "source_stage": "implementation",
        "source_iteration": 0,
        "bottleneck_type": "scan_for_scan",
        "evidence": "aux_bench_sweep at 50% writes: 63% fallback, workload_speedup 0.65 vs DuckDB",
        "proposed_direction": "Switch to hierarchical month buckets per order to avoid segment scan on read",
        "current_candidate_id": "c1",
        "alternate_candidate_ids": ["c2"],
        "cost_model_insufficient_reason": "Cost model cannot eliminate full segment partition scan on read path",
    }
    payload.update(overrides)
    return payload


class TestStructuralReanalysisValidation:
    def test_validate_rejects_short_evidence(self):
        data = _valid_request(evidence="too short")
        errors = validate_request(data)
        assert any("evidence" in e for e in errors)

    def test_validate_rejects_unknown_bottleneck(self):
        data = _valid_request(bottleneck_type="not_a_real_type")
        errors = validate_request(data)
        assert any("bottleneck_type" in e for e in errors)

    def test_bottleneck_types_complete(self):
        assert "scan_for_scan" in BOTTLENECK_TYPES
        assert "residual_read_path" in BOTTLENECK_TYPES


class TestStructuralReanalysisBudget:
    def test_default_max_is_two(self):
        assert max_structural_reanalysis_count({}) == 2

    def test_workload_override(self):
        assert max_structural_reanalysis_count({"max_structural_reanalysis": 1}) == 1

    def test_can_rerun_respects_budget(self):
        workload = {"max_structural_reanalysis": 2}
        assert can_rerun_analysis(0, workload)
        assert can_rerun_analysis(1, workload)
        assert not can_rerun_analysis(2, workload)


class TestStructuralReanalysisFlow:
    @pytest.fixture
    def artifacts_dir(self, tmp_path):
        art = tmp_path / "pipeline" / "artifacts" / "q99"
        art.mkdir(parents=True)
        return art

    def test_write_and_load_request(self, tmp_path):
        write_request(
            tmp_path,
            99,
            source_stage="implementation",
            source_iteration=0,
            bottleneck_type="wrong_partition_grain",
            evidence="Read path scans all segment partitions despite selective predicate on shipdate",
            proposed_direction="Repartition by shipdate month instead of segment only",
            current_candidate_id="c1",
        )
        loaded = load_pending_request(tmp_path, 99)
        assert loaded is not None
        assert loaded["bottleneck_type"] == "wrong_partition_grain"
        clear_request(tmp_path, 99)
        assert load_pending_request(tmp_path, 99) is None

    def test_process_after_stage_runs_stages(self, tmp_path):
        write_request(
            tmp_path,
            99,
            source_stage="implementation",
            source_iteration=0,
            bottleneck_type="scan_for_scan",
            evidence="aux_bench_sweep at 50% writes: 63% fallback, workload_speedup 0.65 vs DuckDB",
            proposed_direction="Use orderdate btree index candidate instead of segment partitions",
            current_candidate_id="c1",
        )
        run_analysis = MagicMock()
        run_impl = MagicMock()
        run_opt = MagicMock()
        workload = {"max_structural_reanalysis": 2}

        result = process_after_stage(
            repo_root=tmp_path,
            query=99,
            workload=workload,
            triggered_after="implementation",
            structural_reanalysis_count=0,
            run_analysis=run_analysis,
            run_implementation=run_impl,
            run_optimization=run_opt,
            dry_run=False,
        )

        assert result == (1, 2)  # optimization index
        run_analysis.assert_called_once()
        run_impl.assert_called_once()
        run_opt.assert_not_called()
        assert load_pending_request(tmp_path, 99) is None

    def test_process_after_optimization_runs_full_chain(self, tmp_path):
        write_request(
            tmp_path,
            99,
            source_stage="optimization",
            source_iteration=2,
            bottleneck_type="maintenance_infeasible",
            evidence="50% write rate causes 63% invalidation; hybrid maintenance cannot keep up",
            proposed_direction="Move to incremental delta structure with bounded rebuild",
            current_candidate_id="c1",
        )
        run_analysis = MagicMock()
        run_impl = MagicMock()
        run_opt = MagicMock()
        workload = {}

        result = process_after_stage(
            repo_root=tmp_path,
            query=99,
            workload=workload,
            triggered_after="optimization",
            structural_reanalysis_count=0,
            run_analysis=run_analysis,
            run_implementation=run_impl,
            run_optimization=run_opt,
            dry_run=False,
        )

        assert result == (1, 3)  # audit index
        run_analysis.assert_called_once()
        run_impl.assert_called_once()
        run_opt.assert_called_once()

    def test_budget_exhausted_clears_request(self, tmp_path):
        write_request(
            tmp_path,
            99,
            source_stage="implementation",
            source_iteration=0,
            bottleneck_type="scan_for_scan",
            evidence="aux_bench_sweep at 50% writes: 63% fallback, workload_speedup 0.65 vs DuckDB",
            proposed_direction="Try alternate candidate with different partition grain",
            current_candidate_id="c1",
        )
        result = process_after_stage(
            repo_root=tmp_path,
            query=99,
            workload={"max_structural_reanalysis": 2},
            triggered_after="implementation",
            structural_reanalysis_count=2,
            run_analysis=MagicMock(),
            run_implementation=MagicMock(),
            run_optimization=MagicMock(),
            dry_run=False,
        )
        assert result is None
        assert load_pending_request(tmp_path, 99) is None

    def test_dry_run_skips_rerun(self, tmp_path):
        write_request(
            tmp_path,
            99,
            source_stage="implementation",
            source_iteration=0,
            bottleneck_type="scan_for_scan",
            evidence="aux_bench_sweep at 50% writes: 63% fallback, workload_speedup 0.65 vs DuckDB",
            proposed_direction="Re-evaluate candidate search with different taxonomy family",
            current_candidate_id="c1",
        )
        result = process_after_stage(
            repo_root=tmp_path,
            query=99,
            workload={},
            triggered_after="implementation",
            structural_reanalysis_count=0,
            run_analysis=MagicMock(),
            run_implementation=MagicMock(),
            run_optimization=MagicMock(),
            dry_run=True,
        )
        assert result is None
        assert load_pending_request(tmp_path, 99) is not None


class TestPipelineStateBackwardCompat:
    def test_load_ignores_unknown_fields(self, tmp_path):
        from accel_pipeline.state import PipelineState

        path = tmp_path / "pipeline_state.json"
        path.write_text(
            json.dumps(
                {
                    "query": 3,
                    "workload": "tpch_q3_aux",
                    "stage": "implementation",
                    "structural_reanalysis_count": 1,
                    "future_field": "ignored",
                }
            ),
            encoding="utf-8",
        )
        state = PipelineState.load(path)
        assert state.structural_reanalysis_count == 1
        assert state.query == 3


class TestReanalysisToolPolicy:
    def test_implementation_stage_has_tool(self):
        from accel_pipeline.tools.tool_policy import STAGE_TOOL_NAMES

        assert "request_structural_reanalysis" in STAGE_TOOL_NAMES["implementation"]
        assert "request_structural_reanalysis" in STAGE_TOOL_NAMES["optimization"]
        assert "request_structural_reanalysis" not in STAGE_TOOL_NAMES["analysis"]

    def test_request_tool_rejects_exhausted_budget(self, tmp_path, monkeypatch):
        from accel_pipeline.config import load_pipeline_config
        from accel_pipeline.state import PipelineState, state_path
        from accel_pipeline.tools.reanalysis_tools import ToolContext, make_reanalysis_tools

        art = tmp_path / "pipeline" / "artifacts" / "q3"
        art.mkdir(parents=True)
        state = PipelineState(query=3, workload="tpch_q3_aux", structural_reanalysis_count=2)
        state.save(state_path(tmp_path, 3, "tpch_q3_aux"))

        pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
        pipeline_config = type(pipeline_config)(
            **{**pipeline_config.__dict__, "repo_root": tmp_path}
        )
        ctx = ToolContext(pipeline_config, 3, "implementation", "tpch_q3_aux")
        tool = make_reanalysis_tools(ctx)
        monkeypatch.setattr(
            "accel_pipeline.tools.reanalysis_tools.load_workload",
            lambda name, root: {"max_structural_reanalysis": 2},
        )
        out = json.loads(
            tool(
                bottleneck_type="scan_for_scan",
                evidence="aux_bench_sweep at 50% writes: 63% fallback, workload_speedup 0.65",
                proposed_direction="Switch to btree index on orderdate for selective reads",
                current_candidate_id="c1",
            )
        )
        assert out["status"] == "rejected"
        assert "budget" in out["reason"]
