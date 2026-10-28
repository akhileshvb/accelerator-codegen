from unittest.mock import MagicMock, patch

import pytest

from accel_pipeline.config import PipelineConfig, QuickTierConfig, RepairConfig
from accel_pipeline.repair import run_gates_with_repair, StageStopped
from accel_pipeline.gates import GateResult


def _config(tmp_path, *, parallel, **repair) -> PipelineConfig:
    return PipelineConfig(
        repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
        repair=RepairConfig(**repair),
        quick_tier=QuickTierConfig(parallel_gates=parallel),
    )


FAILED = GateResult(name="correctness", status="fail", data={"log_tail": "Segmentation fault"})
PASSED = GateResult(name="correctness", status="pass", data={})


@pytest.fixture(autouse=True)
def stubs(monkeypatch):
    for module in ("accel_pipeline.repair", "accel_pipeline.stages", "accel_pipeline.config"):
        monkeypatch.setattr(f"{module}.load_run_workload", lambda cfg, name: {"benchmark_name": "tpch"})


@pytest.fixture(params=[False, True], ids=["serial", "parallel"])
def parallel(request):
    return request.param


class GateRuns(list):
    """The gate runs (serial and parallel paths alike): `script[name]` is that gate's results, the last one repeating."""

    def __init__(self, monkeypatch):
        super().__init__()
        self.script: dict[str, list[GateResult]] = {}
        monkeypatch.setattr("accel_pipeline.stages.run_gate", self._run_gate)
        monkeypatch.setattr("accel_pipeline.gates.run_gate", self._run_gate)

    def _run_gate(self, config, name, query, workload, **kwargs):
        self.append(name)
        queue = self.script[name]
        return queue.pop(0) if len(queue) > 1 else queue[0]


@pytest.fixture()
def gate_runs(monkeypatch):
    return GateRuns(monkeypatch)


@patch("accel_pipeline.stages.run_agent_stage")
def test_crash_debug_retries_until_gates_pass(run_agent, parallel, gate_runs, tmp_path):
    gate_runs.script["correctness"] = [FAILED, FAILED, PASSED]
    results = run_gates_with_repair(
        _config(tmp_path, parallel=parallel), MagicMock(), "workload", 1, ["correctness"],
        change_kind="optimization", iteration=2,
    )
    assert gate_runs == ["correctness"] * 3 and run_agent.call_count == 2
    assert results[-1].status == "pass"
    assert run_agent.call_args.kwargs["session_role"] == "main"


@patch("accel_pipeline.stages.run_agent_stage")
def test_max_rounds_overrides_the_configured_code_budget(run_agent, parallel, gate_runs, tmp_path):
    gate_runs.script["correctness"] = [FAILED]
    with pytest.raises(StageStopped):
        run_gates_with_repair(
            _config(tmp_path, parallel=parallel), MagicMock(), "workload", 1, ["correctness"],
            change_kind="audit", max_rounds=2,
        )
    assert run_agent.call_count == 2


@patch("accel_pipeline.stages.run_agent_stage")
def test_the_failing_gate_of_a_batch_is_the_one_repaired(run_agent, parallel, gate_runs, tmp_path):
    unit_failed = GateResult(name="aux_unit", status="fail", data={"log_tail": "Segmentation fault"})
    gate_runs.script["aux_unit"] = [unit_failed, GateResult(name="aux_unit", status="pass", data={})]
    gate_runs.script["aux_correctness"] = [GateResult(name="aux_correctness", status="pass", data={})]
    run_gates_with_repair(
        _config(tmp_path, parallel=parallel), MagicMock(), "workload", 1, ["aux_correctness", "aux_unit"],
        change_kind="optimization",
    )
    assert run_agent.call_count == 1
    assert "aux_unit" in run_agent.call_args.kwargs["user_message"]


def test_dry_run_runs_nothing(tmp_path):
    assert run_gates_with_repair(
        _config(tmp_path, parallel=True), MagicMock(), "w", 1, ["g"], dry_run=True) == []
