"""The optimization loop: checkpoints and snapshots, the optimization state, the attempt gates, the A/B trial tools."""

from __future__ import annotations

import json
import shutil
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest
from test_optimization_bench import Q18_AUX_OUTPUT, SAMPLE_CSV, parse_sweep_csv_from_text, row

from accel_pipeline import layout
from accel_pipeline.config import MeasurementConfig, PipelineConfig
from accel_pipeline.data import parse_aux_benchmark_output
from accel_pipeline.optimization import bench, loop, memory
from accel_pipeline.optimization.bench import (
    BenchmarkSample,
    EvalConfig,
    MultiSampleBenchmark,
    bench_summary_path,
    evaluate_attempt_outcome,
    summarize_sweep_rows,
)
from accel_pipeline.optimization.loop import (
    format_bench_experiment_log_line,
    has_best_kernel_snapshot,
    process_aux_bench_after_gates,
    record_aux_bench_sweep_outcome,
    record_sweep_in_optimization_state,
    resolve_snapshot_paths,
    restore_best_kernel,
    snapshot_best_kernel,
)
from accel_pipeline.repair import StageFailure


def test_structural_is_inferred_from_the_rewritten_lines(tmp_path):
    root, art = tmp_path / "repo", tmp_path / "art"
    (art / "attempt_checkpoint" / "src").mkdir(parents=True)
    (root / "src").mkdir(parents=True)
    (art / "attempt_checkpoint" / "src" / "m.cpp").write_text("\n".join(f"line {i}" for i in range(100)))
    (root / "src" / "m.cpp").write_text("\n".join(f"line {i}" for i in range(100)))
    cfg = SimpleNamespace(optimization_structural={}, repo_root=root)
    assert not loop.is_structural_attempt(cfg, art, {"strategy": "low_level"})
    (root / "src" / "m.cpp").write_text("\n".join(f"line {i}" for i in range(60)) + "\n" + "\n".join(f"new {i}" for i in range(50)))
    assert loop.is_structural_attempt(cfg, art, {"strategy": "low_level"})


def _iteration(tmp_path, **attrs):
    """An iteration with only the state the tested step reads."""
    it = loop._Iteration.__new__(loop._Iteration)
    it.__dict__.update(art=tmp_path, query=3, iteration=1, workload_name="w", **attrs)
    return it


def test_a_structural_attempt_without_its_design_record_fails_before_the_gates(tmp_path, monkeypatch):
    monkeypatch.setattr(loop, "run_full_tier_gates", lambda *a, **kw: pytest.fail("gates must not run"))
    monkeypatch.setattr(loop, "run_build", lambda *a: (True, ""))
    monkeypatch.setattr(loop, "run_smoke_correctness_repeated", lambda *a, **kw: loop.SmokeResult(True))
    monkeypatch.setattr(loop, "run_smoke_correctness", lambda *a, **kw: loop.SmokeResult(True))
    monkeypatch.setattr(loop, "resolve_workload_paths", lambda *a: {"aux_bench_target": "b", "test_target": "t"})
    cfg = SimpleNamespace(optimization_structural={}, optimization_smoke_runs=1, repo_root=tmp_path, build_dir=tmp_path)
    it = _iteration(tmp_path, cfg=cfg, workload={"workload_kind": "aux_state"}, worker=object())
    gated = it.gate(1, {"strategy": "structural"})
    assert gated.label == "structural_gates_failed" and "design_revision" in gated.detail and gated.candidate is None


def test_incorrect_redesign_is_rolled_back(monkeypatch):
    def fake_run_gate(cfg, name, *a, **kw):
        return SimpleNamespace(name=name, status="fail" if name == "aux_tsan" else "pass", data={"detail": "data race in Execute"})

    monkeypatch.setattr(loop, "run_gate", fake_run_gate)
    error = loop.run_full_tier_gates(SimpleNamespace(optimization_structural={}), {}, 3, iteration=1)
    assert "aux_tsan" in error and "data race" in error  # a non-empty error reverts the attempt


def test_frozen_edit_notice_names_the_edited_pipeline_file(tmp_path):
    from accel_pipeline.agent import _frozen_edit_notice

    def git(*args):
        subprocess.run(["git", "-C", str(tmp_path), *args], check=True, capture_output=True)

    (tmp_path / "pipeline" / "gates").mkdir(parents=True)
    gate = tmp_path / "pipeline" / "gates" / "run_x.sh"
    gate.write_text("echo ok\n")
    git("init", "-q")
    git("-c", "user.email=a@b", "-c", "user.name=t", "add", "-A")
    git("-c", "user.email=a@b", "-c", "user.name=t", "commit", "-qm", "init")
    cfg = SimpleNamespace(repo_root=tmp_path)
    assert layout.frozen_edited(tmp_path) == [] and _frozen_edit_notice(cfg) == ""
    gate.write_text("echo changed\n")
    notice = _frozen_edit_notice(cfg)
    assert "pipeline/gates/run_x.sh" in notice and "report_blocker" in notice and "not yours" in notice


def test_agent_sources_get_a_build_target_automatically(tmp_path):
    root = tmp_path / "src"
    for rel in ("scratch/bench_reads.cpp", "tests/agent_stress.cpp", "tests/test_module.cpp", "bench/other.cpp"):
        path = root / "accelerators" / "b" / "q7" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("int main() {}\n")
    script = tmp_path / "entries.cmake"
    script.write_text(
        f'include("{Path(__file__).resolve().parents[3] / "cmake" / "AgentExecutables.cmake"}")\n'
        f'accel_agent_executable_entries(entries "{root}")\n'
        'foreach(e IN LISTS entries)\n  message("ENTRY ${e}")\nendforeach()\n'
    )
    out = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, check=True).stderr
    targets = sorted(line.split("|")[0].removeprefix("ENTRY ") for line in out.splitlines() if line.startswith("ENTRY"))
    assert targets == ["agent_b_q7_bench_reads", "agent_b_q7_stress"]  # module tests and benches keep their own wiring


def test_agent_scratch_is_writable_and_invisible_to_gates_and_audit(tmp_path):
    from accel_pipeline.layout import is_owned_path, is_relevant

    assert is_owned_path(tmp_path, "accelerators/toybench/q3/scratch/bench.cpp", query=3, benchmark="toybench")
    assert is_owned_path(tmp_path, "accelerators/toybench/q3/tests/agent_stress.cpp", query=3, benchmark="toybench")
    assert not is_owned_path(tmp_path, "accelerators/toybench/q4/scratch/bench.cpp", query=3, benchmark="toybench")
    query_dir = "accelerators/toybench/q3"
    assert not is_relevant("accelerators/toybench/q3/scratch/bench.cpp", query_dir)
    assert is_relevant("accelerators/toybench/q3/toydb/aux/module.cpp", query_dir)


def test_kernel_snapshot_roundtrip(tmp_path: Path):
    repo = tmp_path / "repo"
    repo.mkdir()
    (repo / "pipeline" / "config").mkdir(parents=True)
    (repo / "pipeline" / "config" / "default.yaml").write_text(
        "repo_root: .\nbuild_dir: build-release\ngates_dir: pipeline/gates\n",
        encoding="utf-8",
    )

    header = repo / "accelerators/sec_edgar/q1/duckdb/base/query.hpp"
    kernel = repo / "accelerators/sec_edgar/q1/duckdb/base/single-threaded/kernel.cpp"
    package = repo / "accelerators/sec_edgar/q1/duckdb/base/single-threaded/package.cpp"
    fused = repo / "accelerators/sec_edgar/q1/duckdb/base/single-threaded/fused_scan.cpp"
    runtime_hpp = repo / "accelerators/sec_edgar/common/runtime.hpp"
    runtime_cpp = repo / "accelerators/sec_edgar/common/runtime.cpp"
    for path in (header, kernel, package, fused, runtime_hpp, runtime_cpp):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(f"content:{path.name}:v1\n", encoding="utf-8")

    workload = {
        "benchmark_name": "sec_edgar",
        "query_number": 1,
        "header_basename": "sec_edgar_q1",
        "function_name": "accel_sec_edgar_q1",
        "test_target": "test_sec_edgar_q1",
        "bench_target": "benchmark_sec_edgar_q1",
        "artifact_namespace": "sec_edgar",
        "snapshot_paths": [
            "accelerators/sec_edgar/q{N}/duckdb/base/query.hpp",
            "accelerators/sec_edgar/q{N}/duckdb/base/fused.hpp",
            "accelerators/sec_edgar/q{N}/duckdb/base/single-threaded/kernel.cpp",
            "accelerators/sec_edgar/q{N}/duckdb/base/single-threaded/package.cpp",
            "accelerators/sec_edgar/q{N}/duckdb/base/single-threaded/fused_scan.cpp",
            "accelerators/sec_edgar/common/runtime.hpp",
            "accelerators/sec_edgar/common/runtime.cpp",
        ],
    }
    art = repo / "pipeline" / "artifacts" / "sec_edgar" / "q1"
    art.mkdir(parents=True)

    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        gates_dir=repo / "pipeline" / "gates",
    )

    snap_paths = resolve_snapshot_paths(pipeline_config, workload, 1)
    assert fused in snap_paths
    assert runtime_hpp in snap_paths

    snapshot_best_kernel(
        pipeline_config,
        workload,
        1,
        speedup_median=1.09,
        iteration=0,
        change_kind="implementation",
    )
    assert has_best_kernel_snapshot(art)
    manifest = (art / "best_kernel_snapshot" / "manifest.json").read_text(encoding="utf-8")
    assert "fused_scan.cpp" in manifest
    assert "runtime.hpp" in manifest

    kernel.write_text("content:kernel.cpp:v2-broken\n", encoding="utf-8")
    manifest = restore_best_kernel(pipeline_config, workload, 1)
    assert manifest is not None
    assert "v1" in kernel.read_text(encoding="utf-8")


def test_record_optimization_state(tmp_path: Path):
    repo = tmp_path
    art = repo / "pipeline" / "artifacts" / "q1"
    art.mkdir(parents=True)
    summary = summarize_sweep_rows(parse_sweep_csv_from_text(SAMPLE_CSV))
    state = record_sweep_in_optimization_state(
        repo,
        1,
        iteration=1,
        summary=summary,
        change_kind="baseline",
        active_candidate_id="daily_partial_aggregate",
    )
    assert state["active_candidate_id"] == "daily_partial_aggregate"
    assert state["best_iteration"] == 1
    line = format_bench_experiment_log_line(1, summary, state)
    assert "composite=" in line
    assert "0%→wl=" in line
    assert "/cold=" in line


def test_record_aux_bench_sweep_outcome_writes_experiment_log(tmp_path: Path):
    repo = tmp_path
    art = repo / "pipeline" / "artifacts" / "tpch" / "q18" / "duckdb"
    art.mkdir(parents=True)
    (repo / "pipeline" / "config").mkdir()
    (repo / "pipeline" / "config" / "default.yaml").write_text(
        (Path(__file__).resolve().parents[2] / "config" / "default.yaml").read_text(encoding="utf-8"),
        encoding="utf-8",
    )
    summary = summarize_sweep_rows(parse_sweep_csv_from_text(SAMPLE_CSV))
    workload = {"artifact_namespace": "tpch", "workload_kind": "aux_state", "engine_name": "duckdb"}
    state = record_aux_bench_sweep_outcome(
        repo,
        18,
        summary,
        iteration=0,
        change_kind="bench_sweep",
        active_candidate_id="flat_orderkey_quantity_sums",
        workload=workload,
    )
    log_path = art / "experiment_log.md"
    assert log_path.is_file()
    text = log_path.read_text(encoding="utf-8")
    assert "bench_sweep composite=" in text
    assert state["best_iteration"] == 0
    # Idempotent second call should not duplicate the log line.
    record_aux_bench_sweep_outcome(
        repo,
        18,
        summary,
        iteration=0,
        change_kind="bench_sweep",
        workload=workload,
    )
    assert log_path.read_text(encoding="utf-8").count("bench_sweep composite=") == 1


def test_attempt_with_zero_hit_rate_is_not_judged():
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=50.0,
        baseline_metrics={"read_only_query_speedup": 10.0},
        candidate_metrics={"read_only_query_speedup": 50.0, "accel_hit_rate": 0.0},
        config=EvalConfig(),
        correctness_ok=True,
    )
    assert outcome.verdict == "reject_invalid_measurement"
    assert outcome.measurement_failures[0].kind == "measurement"
    assert "fallback path" in outcome.reason


def test_baseline_with_zero_hit_rate_raises(monkeypatch, tmp_path):
    measured = MultiSampleBenchmark(
        samples=[BenchmarkSample(metrics=row(0, accel_hit_rate=0.0), score=1.0)],
        median_score=1.0,
        median_metrics={"by_write_frequency_pct": {"0": row(0, accel_hit_rate=0.0)}},
    )
    monkeypatch.setattr(loop, "run_multi_sample_benchmark", lambda *a, **k: measured)
    monkeypatch.setattr(loop, "resolve_workload_paths", lambda *a, **k: {"aux_bench_target": "b", "bench_target": "b"})
    config = SimpleNamespace(measurement=MeasurementConfig(), build_dir=tmp_path, repo_root=tmp_path)
    it = _iteration(tmp_path, cfg=config, workload={"workload_kind": "aux_state"}, eval_config=SimpleNamespace(quick_benchmark_samples=1))
    with pytest.raises(StageFailure) as raised:
        it.measure_baseline()
    assert raised.value.failure.kind == "measurement"
    assert raised.value.failure.source == "bench_baseline"


def test_baseline_that_did_not_run_raises(monkeypatch, tmp_path):
    def failing(*args, errors=None, **kwargs):
        errors.append("build failed: undefined reference")

    monkeypatch.setattr(loop, "run_multi_sample_benchmark", failing)
    it = _iteration(tmp_path, cfg=SimpleNamespace(), workload={}, eval_config=SimpleNamespace(quick_benchmark_samples=1))
    with pytest.raises(StageFailure) as raised:
        it.measure_baseline()
    assert "undefined reference" in raised.value.failure.evidence


def test_sweep_records_measurement_ok(tmp_path, monkeypatch):
    snapshots = []
    monkeypatch.setattr(loop, "snapshot_best_kernel", lambda *a, **k: snapshots.append(k))

    from accel_pipeline.config import find_repo_root

    workload = {"name": "q6", "workload_kind": "aux_state"}
    (tmp_path / "pipeline" / "config").mkdir(parents=True)
    shutil.copy(find_repo_root() / "pipeline" / "config" / "default.yaml", tmp_path / "pipeline" / "config")
    for iteration, (hit_rate, ok) in enumerate(((1.0, True), (0.0, False))):
        summary = {"composite_score": 5.0, "read_only_query_speedup": 5.0,
                   "by_write_frequency_pct": {"0": row(0, accel_hit_rate=hit_rate)}}
        _write_summary(tmp_path, workload, summary)
        result = process_aux_bench_after_gates(tmp_path, 6, iteration=iteration, change_kind="bench_sweep", workload=workload)
        assert result["summary"]["measurement_ok"] is ok
        assert result["state"]["history"][-1]["measurement_ok"] is ok
    assert result["state"]["best_composite_score"] == 5.0  # the invalid sweep did not set the best
    assert len(snapshots) == 1  # ...nor became the best kernel
    assert "fallback path" in result["summary"]["measurement_diagnosis"][0]


def _write_summary(repo, workload, summary):
    path = bench_summary_path(repo, 6, workload)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary))


def _quick_benchmark_world(monkeypatch, tmp_path, rows):
    """run_quick_benchmark with the build, the bench sweep and the artifact writers stubbed."""
    art = tmp_path / "art"
    art.mkdir()
    (art / loop.ACTIVE_TRIAL_FILE).write_text(json.dumps(
        {"trial_id": "t", "reference_score": 1.0, "last_metrics": {"stale": True}, "last_score": 9.0}))
    paths = {"aux_bench_target": "bench", "dataset_db": "/d", "db_env_var": "X", "aux_bench_suites": "s"}
    monkeypatch.setattr(bench, "_private_paths", lambda cfg, wl, q: paths)
    monkeypatch.setattr(bench, "_build_benchmark", lambda cfg, target: None)
    monkeypatch.setattr(bench, "_run_aux_sweep", lambda *a, **k: (rows, "out"))
    monkeypatch.setattr(bench, "write_aux_profile_artifacts", lambda *a, **k: None)
    monkeypatch.setattr(loop, "query_artifacts_dir", lambda root, q, workload=None: art)
    monkeypatch.setattr(memory, "refresh_optimization_runtime", lambda *a, **k: None)
    monkeypatch.setattr(memory, "build_scoreboard", lambda *a, **k: {"best_valid_score": 1.0})
    monkeypatch.setattr(memory, "write_scoreboard", lambda *a, **k: None)
    config = SimpleNamespace(repo_root=tmp_path, build_dir=tmp_path / "build", measurement=MeasurementConfig())
    ctx = loop.AbToolContext(config, 6, {"workload_kind": "aux_state", "benchmark_name": "demo"})
    _, run_quick_benchmark, _ = loop.make_ab_tools(ctx)
    return art, run_quick_benchmark


def test_a_quick_bench_that_measured_the_fallback_is_flagged_and_voids_the_trial(monkeypatch, tmp_path):
    rows = parse_aux_benchmark_output(Q18_AUX_OUTPUT.replace(",1.00,0.00,", ",0.00,1.00,", 2))
    art, run_quick_benchmark = _quick_benchmark_world(monkeypatch, tmp_path, rows)
    payload = json.loads(run_quick_benchmark(include_profile=False, acceptance=True))
    assert payload["ab_signal"] == "invalid_measurement"
    assert any("fallback" in line for line in payload["measurement_invalid"])
    trial = json.loads((art / "ab_active_trial.json").read_text())
    assert "last_metrics" not in trial and "last_score" not in trial
