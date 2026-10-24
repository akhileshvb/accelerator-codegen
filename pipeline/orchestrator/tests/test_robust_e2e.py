"""The repair loop, session continuity, parallel gates and stamps working together, on a fake codex:
implementation, a failing gate, a same-session repair, passing gates, a stage exit that reuses what
already passed, and an optimization start whose baseline measured the fallback."""

from __future__ import annotations

import json
import subprocess

import pytest
from test_repair import _implement, _stage_world

from accel_pipeline import optimization_attempt as attempt_mod
from accel_pipeline import repair as repair_mod
from accel_pipeline.crash_debug import run_post_stage_gates_with_crash_debug
from accel_pipeline.gate_stamps import current_stamp, record_pass
from accel_pipeline.gates import GateResult
from accel_pipeline.optimization_eval import BenchmarkSample, MultiSampleBenchmark
from accel_pipeline.repair import RepairCtx, run_with_repair

WORKLOAD = {"benchmark_name": "toybench", "workload_kind": "aux_state", "query_number": 3}
GATES = ["aux_unit", "aux_correctness", "aux_bench_sweep"]
SID = "11111111-2222-3333-4444-555555555555"
GIT = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]


def _sweep(hit_rate: float) -> MultiSampleBenchmark:
    point = {"accel_hit_rate": hit_rate, "num_queries": 10, "accel_fallback_reads": 0 if hit_rate else 10,
             "state_builds": 1, "accel_hit_median_us": 5.0}
    return MultiSampleBenchmark([BenchmarkSample({}, 1.0)], 1.0, {"by_write_frequency_pct": {"0": point}})


@pytest.fixture()
def world(tmp_path, monkeypatch):
    real_run_gates = repair_mod._run_gates
    world = _stage_world(tmp_path, monkeypatch)
    monkeypatch.setattr(repair_mod, "_run_gates", real_run_gates)
    monkeypatch.setattr(repair_mod, "load_run_workload", lambda cfg, name: WORKLOAD)
    monkeypatch.setattr("accel_pipeline.config.load_run_workload", lambda cfg, name: WORKLOAD)
    (tmp_path / ".gitignore").write_text("pipeline/artifacts/\nbuild/\nart/\n*.log\n")
    (tmp_path / "accelerators/toybench/q3/aux.cpp").write_text("// v0\n")
    subprocess.run(["git", "init", "-q"], cwd=tmp_path, check=True)
    subprocess.run([*GIT, "add", "-A"], cwd=tmp_path, check=True)
    subprocess.run([*GIT, "commit", "-qm", "base"], cwd=tmp_path, check=True)

    world.art = tmp_path / "pipeline/artifacts/toybench/q3"
    world.runs, world.scripts = [], {"aux_correctness": ["fail"]}

    def run_gate(config, name, query, workload, *, change_kind="", **kwargs):
        world.runs.append((name, change_kind))
        status = world.scripts.get(name, []).pop(0) if world.scripts.get(name) else "pass"
        data = {"status": status, **({"log_tail": "Segmentation fault in accelerators/toybench/q3/aux.cpp"}
                                     if status == "fail" else {})}
        out = world.art / "gates" / f"{name}.json"
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps(data))
        if status == "pass":  # what gates.run_gate records
            record_pass(config, workload, query, name, current_stamp(config, workload, query),
                        change_kind=change_kind, result=out)
        return GateResult(name, status, data, out)

    monkeypatch.setattr("accel_pipeline.gate_parallel.run_gate", run_gate)
    monkeypatch.setattr("accel_pipeline.gate_parallel.log_gate_batch_start", lambda *a, **k: None)
    return world


def test_implementation_repair_exit_reuse_and_a_measurement_repair_run_in_one_session(world, tmp_path, monkeypatch):
    # implementation: aux_correctness fails once, the repair resumes the session, then everything passes
    impl = RepairCtx(world.config, world.models, "w", 3, stage="implementation", change_kind="implementation")
    run_with_repair(impl, _implement(world), gates=GATES)
    first_round, second_round = world.runs[:2], world.runs[2:]
    assert first_round == [("aux_unit", "implementation"), ("aux_correctness", "implementation")]
    assert sorted(second_round) == sorted((g, "implementation") for g in GATES)

    # stage exit on identical code: what passed is reused, the acceptance timing of another stage is not
    world.runs.clear()
    run_post_stage_gates_with_crash_debug(world.config, world.models, "w", 3, GATES, change_kind="audit")
    assert world.runs == [("aux_bench_sweep", "audit")]

    # optimization start: the baseline measured the fallback, the repair resumes the same session, the rerun is trusted
    paths = {"aux_bench_target": "bench"}
    monkeypatch.setattr(attempt_mod, "resolve_workload_paths", lambda cfg, workload, query: paths)
    sweeps = [_sweep(0.0), _sweep(1.0)]

    def run_sweep(*args, **kwargs):
        (world.config.build_dir / "bench").parent.mkdir(parents=True, exist_ok=True)
        (world.config.build_dir / "bench").write_text("rebuilt")  # the bench target is rebuilt before it runs
        return sweeps.pop(0)

    monkeypatch.setattr(attempt_mod, "run_multi_sample_benchmark", run_sweep)
    eval_config = type("Eval", (), {"quick_benchmark_samples": 2})()
    opt = RepairCtx(world.config, world.models, "w", 3, stage="optimization", change_kind="optimization", iteration=1)
    baseline = run_with_repair(opt, lambda: attempt_mod._measure_baseline(world.config, WORKLOAD, 3, eval_config))
    assert baseline.median_metrics["by_write_frequency_pct"]["0"]["accel_hit_rate"] == 1.0

    # one main session id throughout: the first run starts it, every later turn resumes it
    args = (tmp_path / "args.log").read_text().splitlines()
    assert len(args) == 3 and "exec resume" not in args[0] and all("exec resume" in a and SID in a for a in args[1:])
    assert json.loads((world.art / "sessions.json").read_text()) == {"3": {"main": SID}}
    prompts = (tmp_path / "prompts.log").read_text()
    assert "## code failure in implementation (gate:aux_correctness)" in prompts
    assert "## measurement failure in optimization (bench_baseline)" in prompts
    assert "0 of 10 reads hit the accelerator" in prompts
