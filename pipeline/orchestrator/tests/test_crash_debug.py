from unittest.mock import MagicMock, patch

import pytest

from accel_pipeline.config import PipelineConfig, RepairConfig
from accel_pipeline.crash_debug import run_post_stage_gates_with_crash_debug
from accel_pipeline.gates import GateResult
from accel_pipeline.repair import StageStopped


def _config(tmp_path, **repair) -> PipelineConfig:
    return PipelineConfig(
        repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
        tpch_db_sf1=tmp_path, tpch_db_sf10=tmp_path, repair=RepairConfig(**repair),
    )


FAILED = GateResult(name="correctness", status="fail", data={"log_tail": "Segmentation fault"})
PASSED = GateResult(name="correctness", status="pass", data={})


@pytest.fixture(autouse=True)
def stubs(monkeypatch):
    monkeypatch.setattr("accel_pipeline.repair.load_run_workload", lambda cfg, name: {"benchmark_name": "tpch"})


@patch("accel_pipeline.stages.base.run_agent_stage")
@patch("accel_pipeline.stages.gate_plan.run_post_stage_gates_collect")
def test_crash_debug_retries_until_gates_pass(collect, run_agent, tmp_path):
    collect.side_effect = [([], FAILED), ([], FAILED), ([PASSED], None)]
    results = run_post_stage_gates_with_crash_debug(
        _config(tmp_path), MagicMock(), "workload", 1, ["correctness"], change_kind="optimization", iteration=2,
    )
    assert collect.call_count == 3 and run_agent.call_count == 2
    assert results[-1].status == "pass"
    assert run_agent.call_args.kwargs["session_role"] == "main"


@patch("accel_pipeline.stages.base.run_agent_stage")
@patch("accel_pipeline.stages.gate_plan.run_post_stage_gates_collect")
def test_max_rounds_overrides_the_configured_code_budget(collect, run_agent, tmp_path):
    collect.return_value = ([], FAILED)
    with pytest.raises(StageStopped):
        run_post_stage_gates_with_crash_debug(
            _config(tmp_path), MagicMock(), "workload", 1, ["correctness"], change_kind="audit", max_rounds=2,
        )
    assert run_agent.call_count == 2


def test_dry_run_runs_nothing(tmp_path):
    assert run_post_stage_gates_with_crash_debug(_config(tmp_path), MagicMock(), "w", 1, ["g"], dry_run=True) == []
