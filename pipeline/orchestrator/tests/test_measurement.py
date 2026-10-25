"""check_measurement: a benchmark that did not measure the accelerator is refused."""

from __future__ import annotations

import os
import shutil
from types import SimpleNamespace

import pytest

from accel_pipeline.config import MeasurementConfig
from accel_pipeline.failure import StageFailure, render_failure
from accel_pipeline.optimization.measurement import check_measurement
from accel_pipeline.optimization.eval import EvalConfig, MultiSampleBenchmark, BenchmarkSample, evaluate_attempt_outcome


def row(freq=0, **over):
    base = {
        "write_frequency_pct": freq,
        "num_queries": 100,
        "accel_hit_rate": 1.0,
        "accel_fallback_reads": 0,
        "state_builds": 1,
        "accel_hit_median_us": 50.0,
        "fallback_median_us": 0.0,
        "ref_query_median_us": 5000.0,
        "workload_speedup": 40.0,
        "query_only_speedup": 100.0,
    }
    return {**base, **over}


def check(rows, **kw):
    return check_measurement(rows, workload={"name": "q6"}, phase=kw.pop("phase", "baseline"), **kw)


def diagnoses(rows, **kw):
    return [d for f in check(rows, **kw) for d in f.diagnosis]


def test_all_good_is_clean():
    assert check([row(0), row(10, accel_hit_rate=0.7)]) == []


def test_zero_hit_rate_names_the_fallback_path():
    failures = check([row(0, accel_hit_rate=0.0, accel_fallback_reads=100)])
    assert failures[0].kind == "measurement" and failures[0].source == "bench_baseline"
    assert "bench measured the fallback path: 0 of 100 reads hit the accelerator" in failures[0].diagnosis[0]
    assert "query not registered / first-read build not triggered / harness uses a different SQL entry" in (
        failures[0].diagnosis[0]
    )


def test_low_hit_rate_only_counts_at_write_frequency_zero():
    assert check([row(0), row(50, accel_hit_rate=0.1)]) == []
    assert "accel_hit_rate 0.300" in diagnoses([row(0, accel_hit_rate=0.3)])[0]


def test_attempt_that_broke_the_hit_path_says_so():
    found = diagnoses([row(0, accel_hit_rate=0.0)], phase="attempt", baseline={"accel_hit_rate": 0.98})
    assert any("baseline hit rate was 0.980" in d for d in found)


def test_fallback_reads_on_a_hit_row():
    assert "3 fallback reads" in diagnoses([row(0, accel_fallback_reads=3)])[0]
    assert check([row(0, accel_fallback_reads=3)], config=MeasurementConfig(max_fallbacks_on_hit=3)) == []


def test_counters_a_bench_does_not_report_are_skipped():
    point = row(0)
    del point["accel_fallback_reads"], point["state_builds"]
    assert check([point]) == []


def test_state_never_built():
    assert "no state was built" in diagnoses([row(0, state_builds=0)])[0]
    assert check([row(0, state_builds=0)], config=MeasurementConfig(require_state_built=False)) == []


@pytest.mark.parametrize(
    "over, needle",
    [
        ({"accel_hit_median_us": 0.0}, "hit median is 0"),
        ({"query_only_speedup": float("inf")}, "query_only_speedup is inf"),
        ({"workload_speedup": float("nan")}, "workload_speedup is nan"),
        ({"ref_query_median_us": 0.1}, "reference query median"),
        ({"ref_query_median_us": 7e8}, "reference query median"),
        ({"accel_hit_median_us": 90000.0}, "more than 10x the fallback"),
        ({"fallback_median_us": 100.0, "accel_hit_median_us": 5000.0}, "more than 10x the fallback"),
    ],
)
def test_implausible_timings(over, needle):
    assert any(needle in d for d in diagnoses([row(0, **over)]))


def test_read_axis_must_be_the_engine_reference_over_the_hit():
    assert any("against the engine's own SQL" in d for d in diagnoses([row(0, query_only_speedup=1.06)]))
    assert check([row(0, query_only_speedup=100.4)]) == []


def test_unmeasured_reference_is_not_implausible():
    assert check([row(0, ref_query_median_us=0.0)]) == []


def test_sample_spread():
    scores = [10.0, 30.0, 1.0]
    bench = MultiSampleBenchmark(
        samples=[BenchmarkSample(metrics={}, score=s) for s in scores], median_score=10.0, median_metrics=row(0)
    )
    assert "CV" in diagnoses(bench)[0]
    tight = MultiSampleBenchmark(
        samples=[BenchmarkSample(metrics={}, score=s) for s in (10.0, 10.5)], median_score=10.0, median_metrics=row(0)
    )
    assert check(tight) == []


def test_metrics_dict_with_by_write_frequency():
    metrics = {"by_write_frequency_pct": {"0": row(0, accel_hit_rate=0.0), "10": row(10)}}
    assert "fallback path" in diagnoses(metrics)[0]
    assert check({"by_write_frequency_pct": {"0": row(0)}}) == []


def test_stale_binary(tmp_path):
    qdir = tmp_path / "q6"
    (qdir / "aux").mkdir(parents=True)
    (qdir / "tests").mkdir()
    src, test_src = qdir / "aux" / "kernel.cpp", qdir / "tests" / "t.cpp"
    binary = tmp_path / "bench"
    for path in (src, test_src, binary):
        path.write_text("x")
    os.utime(src, (100, 100))
    os.utime(binary, (200, 200))
    os.utime(test_src, (300, 300))  # another target's source never makes the bench stale
    assert check([row(0)], bench_binary=binary, query_dir=qdir) == []
    os.utime(src, (400, 400))
    assert "stale build" in diagnoses([row(0)], bench_binary=binary, query_dir=qdir)[0]


def test_disabled_check_is_silent():
    assert check([row(0, accel_hit_rate=0.0)], config=MeasurementConfig(enabled=False)) == []


def test_failure_renders_the_diagnosis():
    text = render_failure(check([row(0, accel_hit_rate=0.0)])[0])
    assert "measurement failure" in text and "fallback path" in text


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


def test_attempt_without_a_hit_rate_metric_is_unaffected():
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=11.0,
        baseline_metrics={},
        candidate_metrics={"speedup_median": 11.0},
        config=EvalConfig(),
        correctness_ok=True,
    )
    assert outcome.verdict == "accept"


def test_baseline_with_zero_hit_rate_raises(monkeypatch, tmp_path):
    from accel_pipeline.optimization import attempt as attempt

    bench = MultiSampleBenchmark(
        samples=[BenchmarkSample(metrics=row(0, accel_hit_rate=0.0), score=1.0)],
        median_score=1.0,
        median_metrics={"by_write_frequency_pct": {"0": row(0, accel_hit_rate=0.0)}},
    )
    monkeypatch.setattr(attempt, "run_multi_sample_benchmark", lambda *a, **k: bench)
    monkeypatch.setattr(
        attempt, "resolve_workload_paths", lambda *a, **k: {"aux_bench_target": "b", "bench_target": "b"}
    )
    config = SimpleNamespace(measurement=MeasurementConfig(), build_dir=tmp_path, repo_root=tmp_path)
    with pytest.raises(StageFailure) as raised:
        attempt._measure_baseline(config, {"workload_kind": "aux_state"}, 6, SimpleNamespace(quick_benchmark_samples=1))
    assert raised.value.failure.kind == "measurement"
    assert raised.value.failure.source == "bench_baseline"


def test_baseline_that_did_not_run_raises(monkeypatch, tmp_path):
    from accel_pipeline.optimization import attempt as attempt

    def failing(*args, errors=None, **kwargs):
        errors.append("build failed: undefined reference")

    monkeypatch.setattr(attempt, "run_multi_sample_benchmark", failing)
    with pytest.raises(StageFailure) as raised:
        attempt._measure_baseline(SimpleNamespace(), {}, 6, SimpleNamespace(quick_benchmark_samples=1))
    assert "undefined reference" in raised.value.failure.evidence


def test_hit_contract_gate_is_registered():
    from accel_pipeline.gates.run import GATE_SCRIPTS, GATES_ALLOW_SKIP
    from accel_pipeline.paths import find_repo_root

    script = GATE_SCRIPTS["engine_hit_contract"]
    assert (find_repo_root() / "pipeline" / "gates" / script).is_file()
    assert "engine_hit_contract" in GATES_ALLOW_SKIP


def test_sweep_records_measurement_ok(tmp_path, monkeypatch):
    from accel_pipeline.optimization import kernel_snapshot
    from accel_pipeline.optimization.aux_bench import process_aux_bench_after_gates

    snapshots = []
    monkeypatch.setattr(kernel_snapshot, "snapshot_best_kernel", lambda *a, **k: snapshots.append(k))

    from accel_pipeline.paths import find_repo_root

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
    import json
    from accel_pipeline.optimization.aux_bench import bench_summary_path

    path = bench_summary_path(repo, 6, workload)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary))
