from unittest.mock import MagicMock, patch

import pytest

from accel_pipeline.config import PipelineConfig
from accel_pipeline.crash_debug import _crash_debug_limit, run_post_stage_gates_with_crash_debug
from accel_pipeline.gates import GateFailure, GateResult


def _pipeline_config(max_rounds: int = 0, safety_cap: int = 25) -> PipelineConfig:
    return PipelineConfig(
        repo_root=MagicMock(),
        pipeline_root=MagicMock(),
        build_dir=MagicMock(),
        tpch_db_sf1=MagicMock(),
        tpch_db_sf10=MagicMock(),
        crash_debug_max_rounds=max_rounds,
        crash_debug_safety_cap=safety_cap,
    )


def test_crash_debug_limit_raises_at_safety_cap():
    config = _pipeline_config(max_rounds=0, safety_cap=3)
    with pytest.raises(GateFailure, match="safety_cap=3"):
        _crash_debug_limit(config, 3)


def test_crash_debug_limit_raises_at_max_rounds():
    config = _pipeline_config(max_rounds=2, safety_cap=25)
    with pytest.raises(GateFailure, match="max_rounds=2"):
        _crash_debug_limit(config, 2)


@patch("accel_pipeline.crash_debug.run_agent_stage")
@patch("accel_pipeline.crash_debug.run_post_stage_gates_collect")
@patch("accel_pipeline.crash_debug.load_workload")
@patch("accel_pipeline.crash_debug.query_artifacts_dir")
@patch("accel_pipeline.crash_debug.append_experiment_log")
def test_crash_debug_retries_until_gates_pass(
    _log,
    _art_dir,
    load_workload,
    collect,
    run_agent,
):
    load_workload.return_value = {
        "test_target": "test_x",
        "ctest_smoke_regex": "^x$",
    }
    failed = GateResult(name="correctness", status="fail", data={"log_tail": "Segmentation fault"})
    collect.side_effect = [
        ([], failed),
        ([], failed),
        ([GateResult(name="correctness", status="pass", data={})], None),
    ]

    config = _pipeline_config(max_rounds=0, safety_cap=10)
    config.repo_root = MagicMock()

    results = run_post_stage_gates_with_crash_debug(
        config,
        MagicMock(),
        "workload",
        1,
        ["correctness"],
        change_kind="optimization",
        iteration=2,
    )

    assert collect.call_count == 3
    assert run_agent.call_count == 2
    assert results[-1].status == "pass"
