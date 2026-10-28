"""Measuring and scoring: the bench rows, the write-frequency draw, the trust checks, the score and the verdict, the perf profile."""

from __future__ import annotations

from pathlib import Path
import random
import os
import shutil
from types import SimpleNamespace
import pytest
from accel_pipeline.config import MeasurementConfig
from accel_pipeline.repair import StageFailure
import stat
import sys
import textwrap
import json
import subprocess
from accel_pipeline import layout
import time
from accel_pipeline.data import parse_aux_benchmark_output
from accel_pipeline.optimization.bench import _optimization_signal, _aux_write_frequencies, _aux_bench_env, aux_metrics, is_aux_workload, BenchmarkSample, EvalConfig, MultiSampleBenchmark, aggregate_write_mix_workload_speedup, aux_optimization_score, check_measurement, ensure_attempt_write_frequencies, evaluate_attempt_outcome, improvement_ratio, parse_custom_profile_events, parse_sweep_csv, read_axes, resolve_optimization_write_frequencies, summarize_sweep_rows
from accel_pipeline.optimization import bench


SAMPLE_CSV = """\
suite,write_policy,locality,write_frequency_pct,query_month,num_queries,writes_scheduled,writes_executed,rows_touched_per_write,duckdb_ref_query_median_us,base_write_median_us,accel_write_median_us,accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,total_duckdb_query_us,total_accel_query_us,total_base_write_us,total_accel_write_us,total_incremental_apply_us,incremental_apply_per_write_us,incremental_apply_attempts,duckdb_workload_us,accel_workload_us,workload_speedup,query_only_speedup,blended_query_speedup
lineitem_only,hybrid,single_month,0,1995-06-01,500,0,0,0,166496.00,22845.00,0.00,4100.00,0.00,1.00,0.00,83248000.00,2102036.00,0.00,0.00,0.00,0.00,0,83248000.00,2102036.00,39.60,40.61,40.61
lineitem_only,hybrid,single_month,1,1995-06-01,500,5,5,1,166496.00,22845.00,1200.00,4100.00,0.00,1.00,0.00,83248000.00,2500000.00,114225.00,6000.00,0.00,0.00,0,83362225.00,2506000.00,33.27,40.61,40.61
lineitem_only,hybrid,single_month,10,1995-06-01,500,50,50,1,166496.00,22845.00,1200.00,4100.00,0.00,1.00,0.00,83248000.00,4000000.00,1142250.00,60000.00,0.00,0.00,0,84390250.00,4060000.00,20.78,40.61,40.61
lineitem_only,hybrid,single_month,50,1995-06-01,500,250,250,1,166496.00,22845.00,1200.00,4100.00,0.00,1.00,0.00,83248000.00,12000000.00,5711250.00,300000.00,0.00,0.00,0,88959250.00,12300000.00,7.23,40.61,40.61
"""


def test_parse_sweep_csv_ignores_speedup_gate_lines(tmp_path: Path):
    csv_path = tmp_path / "results.csv"
    csv_path.write_text(
        SAMPLE_CSV.splitlines()[0] + "\n"
        + SAMPLE_CSV.splitlines()[1] + "\n"
        + "speedup_gate wall_time_base_us=1.00\n"
        + SAMPLE_CSV.splitlines()[2] + "\n",
        encoding="utf-8",
    )
    rows = parse_sweep_csv(csv_path)
    assert len(rows) == 2
    assert rows[0]["write_frequency_pct"] == 0
    assert rows[1]["write_frequency_pct"] == 1


def test_parse_sweep_csv(tmp_path: Path):
    csv_path = tmp_path / "results.csv"
    csv_path.write_text(SAMPLE_CSV, encoding="utf-8")
    rows = parse_sweep_csv(csv_path)
    assert len(rows) == 4
    assert rows[0]["write_frequency_pct"] == 0
    assert rows[-1]["workload_speedup"] == 7.23


def test_composite_score_uses_min_workload_speedup():
    rows = parse_sweep_csv_from_text(SAMPLE_CSV)
    summary = summarize_sweep_rows(rows)
    assert summary["composite_workload_speedup"] == 7.23
    assert summary["read_only_query_speedup"] == 40.61
    assert summary["composite_score"] == round(7.23 + 0.1 * 40.61, 4)


def test_summarize_ignores_non_discriminative_write_intervals():
    """All-zero write_interval_sec must not collapse distinct write_frequency_pct rows."""
    text = """\
suite,write_policy,locality,write_frequency_pct,write_interval_sec,num_queries,writes_executed,duckdb_workload_us,accel_workload_us,workload_speedup,cold_start_workload_speedup,query_only_speedup,blended_query_speedup,accel_hit_median_us,accel_hit_rate
lineitem_only,hybrid,global,0,0.00,100,0,1000,250,4.00,0.80,3.48,3.48,12000,1.0
lineitem_only,hybrid,global,0,0.00,100,0,1000,260,3.85,0.75,3.86,3.86,11000,1.0
lineitem_only,hybrid,global,1,0.00,100,1,1000,280,3.57,0.70,3.50,3.50,12100,1.0
lineitem_only,hybrid,global,50,0.00,100,50,1000,310,3.23,0.60,3.47,3.47,12200,1.0
"""
    rows = parse_sweep_csv_from_text(text)
    summary = summarize_sweep_rows(rows)
    assert set(summary["by_write_frequency_pct"]) == {"0", "1", "50"}
    assert summary["by_write_frequency_pct"]["0"]["write_frequency_pct"] == 0
    # Worst-case among 0% replicates.
    assert summary["read_only_query_speedup"] == 3.48
    assert summary["by_write_frequency_pct"]["0"]["query_only_speedup"] == 3.48
    assert summary["by_write_frequency_pct"]["50"]["query_only_speedup"] == 3.47
    assert summary["composite_workload_speedup"] == 3.23


def test_summarize_keys_by_interval_when_discriminative():
    text = """\
suite,write_policy,locality,write_frequency_pct,write_interval_sec,num_queries,writes_executed,duckdb_workload_us,accel_workload_us,workload_speedup,cold_start_workload_speedup,query_only_speedup,blended_query_speedup,accel_hit_median_us,accel_hit_rate
lineitem_only,hybrid,global,0,0,100,0,1000,250,4.00,0.80,5.00,5.00,12000,1.0
lineitem_only,hybrid,global,10,2.5,100,4,1000,300,3.30,0.70,4.80,4.80,12100,1.0
"""
    rows = parse_sweep_csv_from_text(text)
    summary = summarize_sweep_rows(rows)
    assert set(summary["by_write_frequency_pct"]) == {"0", "2.5"}
    assert summary["read_only_query_speedup"] == 5.0
    assert summary["by_write_frequency_pct"]["0"]["write_frequency_pct"] == 0


def test_parse_reads_the_engine_reference_and_its_read_speedup():
    text = """\
suite,write_policy,locality,write_frequency_pct,write_interval_sec,num_queries,writes_executed,duckdb_workload_us,accel_workload_us,workload_speedup,cold_start_workload_speedup,engine_ref_query_median_us,accel_hit_median_us,query_only_speedup,blended_query_speedup,accel_hit_rate
lineitem_only,hybrid,global,0,0,100,0,1000,250,4.00,0.80,60000,12000,5.00,5.00,1.0
"""
    point = summarize_sweep_rows(parse_sweep_csv_from_text(text))["by_write_frequency_pct"]["0"]
    assert point["ref_query_median_us"] == 60000.0
    assert point["query_only_speedup"] == 5.0 == point["ref_query_median_us"] / point["accel_hit_median_us"]


def parse_sweep_csv_from_text(text: str):
    from accel_pipeline.optimization.bench import parse_sweep_csv

    path = Path("/tmp/aux_bench_test.csv")
    path.write_text(text, encoding="utf-8")
    return parse_sweep_csv(path)


def test_resolve_static_when_sample_disabled():
    workload = {
        "aux_bench_sweep_optimization": {
            "write_frequencies": [0, 10],
            "write_mix_sample": {"enabled": False},
        }
    }
    assert resolve_optimization_write_frequencies(workload) == [0, 10]


def test_resolve_samples_per_attempt(tmp_path):
    workload = {
        "aux_bench_sweep_optimization": {
            "write_frequencies": [0],
            "write_mix_sample": {
                "enabled": True,
                "count": 2,
                "choices": [5, 15, 25, 40],
                "lock_per_attempt": True,
                "seed": 1,
            },
        }
    }
    a1 = ensure_attempt_write_frequencies(
        tmp_path, workload, optimization_iter=2, attempt=1, rng=random.Random(1)
    )
    assert a1 == resolve_optimization_write_frequencies(
        workload, art_dir=tmp_path, optimization_iter=2, attempt=1
    )
    assert a1[0] == 0
    assert len(a1) == 3
    assert set(a1[1:]).issubset({5, 15, 25, 40})

    a2 = ensure_attempt_write_frequencies(
        tmp_path, workload, optimization_iter=2, attempt=2, rng=random.Random(2)
    )
    assert a2[0] == 0
    assert len(a2) == 3
    assert a2 == resolve_optimization_write_frequencies(
        workload, art_dir=tmp_path, optimization_iter=2, attempt=2
    )
    # Seeded draws differ by attempt index.
    assert a1 != a2


def test_aggregate_write_mix_geo_mean():
    speedup, pcts = aggregate_write_mix_workload_speedup(
        {
            "by_write_frequency_pct": {
                "0": {"write_frequency_pct": 0, "workload_speedup": 9.0},
                "5": {"write_frequency_pct": 5, "workload_speedup": 4.0},
                "25": {"write_frequency_pct": 25, "workload_speedup": 1.0},
            }
        }
    )
    assert pcts == [5, 25]
    assert abs(speedup - 2.0) < 1e-9


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


FAKE_PERF = textwrap.dedent(
    """\
    #!{python}
    import os, subprocess, sys
    args = sys.argv[1:]
    verb = args[0]
    def opt(flag):
        return args[args.index(flag) + 1]
    if verb == "stat":
        queries = int(os.environ["ACCEL_BENCH_QUERIES"])
        assert os.environ["ACCEL_PROFILE_CONTROL"].count(",") in (0, 1)
        with open(opt("-o"), "w") as out:
            out.write(f"{{0.5 * queries}},,task-clock:u,1,100.00,,\\n")
            out.write(f"{{5000 * queries}},,cycles:u,1,100.00,,\\n")
            out.write(f"{{12000 * queries}},,instructions:u,1,100.00,,\\n")
            out.write(f"{{800 * queries}},,branches:u,1,100.00,,\\n")
            out.write(f"{{8 * queries}},,branch-misses:u,1,100.00,,\\n")
            out.write("<not supported>,,L1-dcache-load-misses:u,0,0.00,,\\n")
            out.write(f"{{100 * queries}},,cache-references:u,1,100.00,,\\n")
            out.write(f"{{10 * queries}},,cache-misses:u,1,100.00,,\\n")
        sep = args.index("--")
        sys.exit(subprocess.run(args[sep + 1:]).returncode)
    elif verb == "record":
        if os.environ.get("FAKE_NO_WINDOW") != "1":
            open(opt("-o"), "w").write("data")
        sep = args.index("--")
        sys.exit(subprocess.run(args[sep + 1:]).returncode)
    elif verb == "report":
        print("# Overhead  Shared Object  Symbol")
        print("    60.00%  libaccelerator_codegen.so  [.] accelerator_codegen::aux_state::query::ScanState(int)")
        print("    15.00%  libaccelerator_codegen.so  [.] accelerator_codegen::aux_state::core::DecodeBlock()")
        print("    20.00%  libtoydb.so  [.] toydb::Execute()")
        print("     5.00%  libc.so.6  [.] 0x00000000000a73e4")
    elif verb == "annotate":
        print("  0.10 :   401000:   vpaddq (%rdi),%ymm0,%ymm0")
        print("  0.20 :   401005:   vmovdqu %ymm0,(%rsi)")
        print("  0.30 :   40100a:   add    $0x20,%rdi")
    else:
        sys.exit(2)
    """
)


@pytest.fixture
def fake_perf(tmp_path, monkeypatch):
    path = tmp_path / "perf"
    path.write_text(FAKE_PERF.format(python=sys.executable), encoding="utf-8")
    path.chmod(path.stat().st_mode | stat.S_IEXEC)
    monkeypatch.setenv(bench.PERF_BIN_ENV, str(path))
    return path


def test_parse_stat_marks_unsupported_events_none():
    counters = bench.parse_stat("100,,cycles:u,1,100.00,,\n<not supported>,,L1-dcache-load-misses:u,0,0.00,,\n")
    assert counters == {"cycles": 100.0, "L1-dcache-load-misses": None}


def test_per_query_counters_divide_the_window_and_derive_rates():
    window = {"cycles": 10000.0, "instructions": 30000.0, "branches": 1000.0, "branch-misses": 50.0}
    per_query = bench.per_query_counters(window, 10)
    assert per_query["cycles"] == 1000.0
    assert per_query["ipc"] == 3.0
    assert per_query["branch_miss_rate"] == 0.05
    assert per_query["cache-misses"] is None


def test_parse_report_folds_into_buckets_and_functions():
    text = (
        "    40.00%  libaccelerator_codegen.so  [.] ns::query::ScanState(int)\n"
        "    20.00%  libaccelerator_codegen.so  [.] std::sort<ns::query::Entry*>()\n"
        "    15.00%  libaccelerator_codegen.so  [.] ns::core::DecodeBlock()\n"
        "    10.00%  libtoydb.so  [.] toydb::Execute()\n"
        "     5.00%  benchmark_x_aux  [.] ns::query::Compose()\n"
    )
    parsed = bench.parse_report(text)
    assert parsed["self_pct_by_bucket"] == {
        "engine_or_system": 10.0, "own_code": 65.0, "substrate_or_backend": 15.0}
    assert parsed["top"][0]["symbol"] == "ns::query::ScanState(int)"
    assert bench.function_totals(parsed["top"])[0]["bucket"] == "own_code"


def test_server_engine_is_explicitly_not_applicable():
    assert bench.engine_profile_gap({"embedded": False})
    assert bench.engine_profile_gap({"embedded": True}) == ""
    assert bench.engine_profile_gap({"embedded": False, "profile": True}) == ""
    assert bench.engine_profile_gap({"embedded": True, "profile": False})
    assert bench.engine_profile_gap(None)


def test_work_counters_say_what_to_call_when_the_read_reports_nothing():
    silent = bench.work_counter_report({"partitions_read_per_query_median": 0.0}, {})
    assert "RecordPartitionsRead" in silent["note"]
    reported = bench.work_counter_report({"partitions_read_per_query_median": 12.0, "state_bytes_read_median": 4096}, {"x": 1})
    assert reported["state_units_visited_per_query"] == 12.0 and "note" not in reported


def test_profile_hit_path_against_fake_perf(fake_perf):
    profile = bench.profile_hit_path(
        [sys.executable, "-c", "pass"], {}, hit_us=10_000.0, diagnostics={"partitions_read_per_query_median": 3.0}
    )
    assert profile["status"] == "ok"
    assert profile["sampling"]["self_pct_by_bucket"]["own_code"] == 60.0
    assert profile["sampling"]["top_functions"][0]["symbol"].endswith("query::ScanState(int)")
    counters = profile["counters_per_query"]
    assert counters["cycles"] == 5000.0 and counters["ipc"] == 2.4
    assert profile["vectorization_of_hottest_own_function"]["uses_wide_vectors"] is True
    assert profile["work_counters"]["state_units_visited_per_query"] == 3.0
    text = bench.render_for_agent(profile)
    assert "ScanState" in text and "ipc=2.4" in text and "cpu_ms=0.5" in text


def test_bench_that_never_opens_the_window_is_an_error(fake_perf):
    profile = bench.profile_hit_path([sys.executable, "-c", "pass"], {"FAKE_NO_WINDOW": "1"}, hit_us=10_000.0)
    assert profile["status"] == "error"
    assert bench.render_for_agent(profile).startswith("profile FAILED")


def test_profile_hit_path_without_perf_is_an_error_not_a_waiver(monkeypatch):
    monkeypatch.setenv(bench.PERF_BIN_ENV, "/nonexistent/perf")
    profile = bench.profile_hit_path(["true"], {})
    assert profile["status"] == "error" and "perf" in profile["reason"]
    assert "FAILED" in bench.render_for_agent(profile)


def test_a_server_engine_stays_not_applicable():
    assert "not applicable" in bench.render_for_agent(bench.not_applicable(bench.engine_profile_gap({"embedded": False})))


def test_the_build_profile_lists_the_hot_spots(fake_perf):
    profile = bench.profile_build([sys.executable, "-c", "pass"], {})
    assert profile["status"] == "ok"
    assert profile["sampling"]["top_functions"][0]["symbol"].endswith("query::ScanState(int)")
    assert "ScanState" in bench.render_for_agent(profile)


def test_a_build_with_no_samples_is_an_error(fake_perf):
    assert bench.profile_build([sys.executable, "-c", "pass"], {"FAKE_NO_WINDOW": "1"})["status"] == "error"


def test_concurrent_sampling_sessions_take_turns(tmp_path, monkeypatch):
    # two pipelines profiling at once exhausted perf's per-user locked-memory budget ("Permission error mapping pages")
    import threading
    import time

    monkeypatch.setattr(bench, "host_lock_dir", lambda: tmp_path)
    active, overlaps = [], []

    def runner(cmd, **kwargs):
        active.append(cmd)
        overlaps.append(len(active) > 1)
        time.sleep(0.2)
        active.remove(cmd)

    threads = [threading.Thread(target=bench._run, args=(runner, ["perf", "record", str(i)], {}, None)) for i in range(2)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert overlaps == [False, False]


def test_read_axis_is_the_speedup_over_the_engine():
    assert read_axes({"read_only_query_speedup": 11.6}) == {"vs_engine": 11.6}
    assert read_axes({}) == {"vs_engine": None}


def _metrics(read: float, write: float) -> dict:
    return {"read_only_query_speedup": read, "write_mix_workload_speedup": write}


def _outcome(before, after):
    return evaluate_attempt_outcome(
        baseline_score=5.0, candidate_score=6.0, baseline_metrics=before, candidate_metrics=after,
        config=EvalConfig(), correctness_ok=True, structural=True,
    )


def test_correct_but_slower_redesign_is_rolled_back():
    outcome = _outcome(_metrics(20.0, 3.0), _metrics(20.2, 3.0))
    assert outcome.verdict == "reject_regress" and "structural_read_not_improved" in outcome.reason


def test_redesign_that_regresses_writes_is_rolled_back():
    outcome = _outcome(_metrics(20.0, 3.0), _metrics(60.0, 1.0))
    assert outcome.verdict == "reject_regress" and "structural_write_regressed" in outcome.reason


def test_structural_attempt_never_gets_keep_explore():
    # Diagnostics that would earn a micro-optimization keep_explore do not keep a redesign.
    before = {**_metrics(20.0, 3.0), "partitions_read_per_query_median": 100.0}
    after = {**_metrics(30.0, 3.0), "partitions_read_per_query_median": 10.0}
    outcome = evaluate_attempt_outcome(
        baseline_score=5.0, candidate_score=5.0, baseline_metrics=before, candidate_metrics=after,
        config=EvalConfig(), correctness_ok=True, structural=True,
    )
    assert outcome.verdict != "keep_explore"


def test_accept_on_read_improvement():
    cfg = EvalConfig(improvement_threshold_pct=3.0, reject_threshold_pct=2.0)
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=11.0,
        baseline_metrics={"read_only_query_speedup": 10.0, "valid_partitions": 1000},
        candidate_metrics={"read_only_query_speedup": 11.0, "valid_partitions": 1000, "accel_hit_rate": 1.0},
        config=cfg,
        correctness_ok=True,
    )
    assert outcome.verdict == "accept"


def test_keep_explore_on_structural_progress():
    cfg = EvalConfig(improvement_threshold_pct=3.0, reject_threshold_pct=2.0)
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=10.1,
        baseline_metrics={
            "read_only_query_speedup": 10.0,
            "valid_partitions": 10000,
            "validity_slots_scanned_median": 10000,
            "accel_hit_rate": 1.0,
        },
        candidate_metrics={
            "read_only_query_speedup": 10.1,
            "valid_partitions": 1000,
            "validity_slots_scanned_median": 1000,
            "accel_hit_rate": 1.0,
        },
        config=cfg,
        correctness_ok=True,
        structural_progress_threshold_pct=5.0,
    )
    assert outcome.verdict == "keep_explore"


def test_reject_broken_on_failed_correctness():
    cfg = EvalConfig()
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=0.0,
        baseline_metrics={},
        candidate_metrics={},
        config=cfg,
        correctness_ok=False,
    )
    assert outcome.verdict == "reject_broken"


def test_improvement_ratio():
    assert improvement_ratio(1.0, 1.05) == pytest.approx(5.0)
    assert improvement_ratio(0.0, 1.0) == 100.0


def test_aux_score_uses_hit_time_when_read_only_zero():
    slow = {"read_only_query_speedup": 0.0, "accel_hit_median_us": 20000.0}
    fast = {"read_only_query_speedup": 0.0, "accel_hit_median_us": 10000.0}
    slow_score = aux_optimization_score(slow)
    fast_score = aux_optimization_score(fast)
    assert fast_score > slow_score
    assert improvement_ratio(slow_score, fast_score) > 0


def test_aux_score_dual_axis_weights_write_less_than_read():
    base = {
        "read_only_query_speedup": 9000.0,
        "write_mix_workload_speedup": 0.37,
    }
    base_score = aux_optimization_score(base)

    # Same write-mix, modest read gain — positive but limited.
    read_gain = aux_optimization_score(
        {"read_only_query_speedup": 10000.0, "write_mix_workload_speedup": 0.37}
    )
    read_imp = improvement_ratio(base_score, read_gain)

    # Same read, write-mix 0.37 → 2.0 — larger dual-axis lift than the read-only bump.
    write_gain = aux_optimization_score(
        {"read_only_query_speedup": 9000.0, "write_mix_workload_speedup": 2.0}
    )
    write_imp = improvement_ratio(base_score, write_gain)
    assert write_imp > read_imp

    # Read up but write cliff deepens — net regress.
    cliff = aux_optimization_score(
        {"read_only_query_speedup": 9500.0, "write_mix_workload_speedup": 0.15}
    )
    assert improvement_ratio(base_score, cliff) < 0


def test_aux_score_reads_write_mix_from_by_freq():
    metrics = {
        "read_only_query_speedup": 100.0,
        "by_write_frequency_pct": {"10": {"workload_speedup": 4.0}},
    }
    expected = (100.0**0.75) * (4.0**0.25)
    assert abs(aux_optimization_score(metrics) - expected) < 1e-6


def test_aux_score_geo_means_multiple_write_mixes():
    metrics = {
        "read_only_query_speedup": 16.0,
        "by_write_frequency_pct": {
            "0": {"write_frequency_pct": 0, "workload_speedup": 20.0},
            "10": {"write_frequency_pct": 10, "workload_speedup": 4.0},
            "50": {"write_frequency_pct": 50, "workload_speedup": 1.0},
        },
    }
    write_mix = (4.0 * 1.0) ** 0.5
    expected = (16.0**0.75) * (write_mix**0.25)
    assert abs(aux_optimization_score(metrics) - expected) < 1e-6


def _built(read: float, write: float, build_ms: float) -> dict:
    return {**_metrics(read, write), "by_write_frequency_pct": {"0": {"setup_full_build_ms": build_ms}}}


@pytest.mark.parametrize("structural_attempt", [False, True])
def test_a_much_faster_build_at_an_equal_score_is_kept(structural_attempt):
    # the clean Q3 run's fused rank build: SF1 build 19.4 s -> 16.8 s, score flat, dropped by the old rule
    outcome = evaluate_attempt_outcome(
        baseline_score=261.3, candidate_score=259.0, baseline_metrics=_built(292.0, 180.0, 19400.0),
        candidate_metrics=_built(290.0, 179.0, 16800.0), config=EvalConfig(), correctness_ok=True,
        structural=structural_attempt)
    assert outcome.verdict == "accept" and "13% faster" in outcome.reason


@pytest.mark.parametrize("candidate_score,build_ms,write", [(250.0, 16800.0, 180.0), (261.3, 18500.0, 180.0),
                                                            (261.3, 16800.0, 150.0)])
def test_a_build_win_does_not_buy_a_score_drop_a_small_gain_or_a_write_regression(candidate_score, build_ms, write):
    outcome = evaluate_attempt_outcome(
        baseline_score=261.3, candidate_score=candidate_score, baseline_metrics=_built(292.0, 180.0, 19400.0),
        candidate_metrics=_built(292.0, write, build_ms), config=EvalConfig(), correctness_ok=True)
    assert outcome.verdict != "accept"


Q18_AUX_OUTPUT = """\
suite,write_policy,locality,write_frequency_pct,quantity_threshold,num_queries,writes_executed,duckdb_ref_query_median_us,base_kernel_median_us,base_write_median_us,accel_write_median_us,accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,total_duckdb_query_us,total_accel_query_us,total_base_write_us,total_accel_write_us,duckdb_workload_us,accel_workload_us,workload_speedup,cold_start_workload_speedup,query_only_speedup,blended_query_speedup,cold_start_accel_workload_us,setup_full_build_ms
lineitem_only,hybrid,global,0,312,5,0,150000.00,3500000.00,22000.00,0.00,3200000.00,0.00,1.00,0.00,750000.00,16000000.00,0.00,0.00,750000.00,16000000.00,0.05,0.04,4.70,4.70,16100000.00,9000.00
lineitem_only,hybrid,global,50,312,5,2,150000.00,3500000.00,22000.00,1200.00,3200000.00,0.00,1.00,0.00,750000.00,16000000.00,44000.00,2400.00,794000.00,16002400.00,0.05,0.04,4.70,4.70,161002400.00,9100.00
"""


def test_aux_quick_metrics_uses_min_workload_across_points():
    rows = parse_aux_benchmark_output(Q18_AUX_OUTPUT)
    summary = summarize_sweep_rows(rows)
    metrics = aux_metrics(rows)
    assert metrics["mode"] == "aux_quick_sweep"
    assert metrics["write_frequency_points"] == 2
    assert metrics["composite_score"] == summary["composite_score"]
    assert float(metrics["composite_workload_speedup"]) == 0.05
    assert metrics["read_only_query_speedup"] == 4.7
    # Write mix uses the non-zero frequency (50%), not a hard-coded 10% key.
    assert metrics["write_mix_pcts"] == [50]
    assert abs(float(metrics["write_mix_workload_speedup"]) - 0.05) < 1e-9
    expected = (4.7**0.75) * (0.05**0.25)
    assert abs(metrics["speedup_median"] - expected) < 1e-6
    assert metrics["setup_full_build_ms"] == 9000.0
    assert _optimization_signal(metrics) is not None


def test_aux_quick_write_frequencies_reads_workload_sweep():
    workload = {"aux_bench_sweep": {"write_frequencies": [0, 10]}}
    assert _aux_write_frequencies(workload) == [0, 10]


def test_aux_quick_write_frequencies_samples_locked(tmp_path):
    workload = {
        "aux_bench_sweep_optimization": {
            "write_frequencies": [0],
            "write_mix_sample": {
                "enabled": True,
                "count": 2,
                "choices": [5, 10, 25, 50],
                "lock_per_attempt": True,
                "seed": 7,
            },
        }
    }
    first = _aux_write_frequencies(
        workload, art_dir=tmp_path, optimization_iter=1, attempt=1
    )
    second = _aux_write_frequencies(
        workload, art_dir=tmp_path, optimization_iter=1, attempt=1
    )
    assert first[0] == 0
    assert len(first) == 3
    assert first == second
    assert all(f in (0, 5, 10, 25, 50) for f in first)
    third = _aux_write_frequencies(
        workload, art_dir=tmp_path, optimization_iter=1, attempt=2
    )
    # Fresh ensure isn't called here — resolve with new attempt samples+persists.
    assert third[0] == 0
    assert len(third) == 3


def test_quick_benchmark_env_aux_uses_fast_settings():
    paths = {
        "db_env_var": "TPCH_SF1_DB",
        "dataset_db": "/data/tpch_sf1.duckdb",
        "aux_bench_suites": "lineitem_only",
    }
    workload = {
        "workload_kind": "aux_state",
        "aux_bench_sweep_optimization": {
            "write_frequencies": [0],
            "bench_warmup": 3,
            "quick_bench_queries": 10,
        },
    }
    env = _aux_bench_env(paths, query=18, workload=workload, freqs=[0], include_profile=True)
    assert is_aux_workload(workload)
    assert env["ACCEL_AUX_WRITE_FREQUENCIES"] == "0"
    assert env["ACCEL_BENCH_QUERIES"] == "10"
    assert env["ACCEL_BENCH_WARMUP"] == "3"
    assert env["ACCEL_AUX_CUSTOM_PROFILE"] == "1"
    assert "ACCEL_BENCH_PROFILE" not in env
    assert env["ACCEL_DB"] == env[paths["db_env_var"]] == str(paths["dataset_db"])  # the host contract's variable too


def test_quick_tier_env_is_the_cheap_read_only_sweep():
    paths = {"db_env_var": "TPCH_SF1_DB", "dataset_db": "/d.db", "aux_bench_suites": "lineitem_only"}
    workload = {
        "workload_kind": "aux_state",
        "aux_bench_sweep_optimization": {"write_frequencies": [0], "bench_warmup": 3, "quick_bench_queries": 10,
                                         "bench_ref_samples": 2, "write_mix_sample": {"enabled": True, "count": 2,
                                                                                       "choices": [1, 5, 10]}},
        "aux_bench_sweep_quick": {"bench_warmup": 1, "bench_queries": 5, "bench_ref_samples": 1,
                                  "write_frequencies": [0]},
    }
    quick = _aux_bench_env(paths, query=6, workload=workload, freqs=[0], acceptance=False)
    assert quick["ACCEL_AUX_WRITE_FREQUENCIES"] == "0"
    assert (quick["ACCEL_BENCH_WARMUP"], quick["ACCEL_BENCH_QUERIES"], quick["ACCEL_BENCH_REF_SAMPLES"]) == ("1", "5", "1")
    full = _aux_bench_env(paths, query=6, workload=workload, freqs=[0], acceptance=True)
    assert (full["ACCEL_BENCH_WARMUP"], full["ACCEL_BENCH_QUERIES"], full["ACCEL_BENCH_REF_SAMPLES"]) == ("3", "10", "2")


def test_quick_tier_never_samples_write_mixes_but_acceptance_does(tmp_path):
    workload = {
        "aux_bench_sweep_optimization": {"write_frequencies": [0], "write_mix_sample": {
            "enabled": True, "count": 2, "choices": [1, 5, 10], "lock_per_attempt": True}},
    }
    assert _aux_write_frequencies(workload, art_dir=tmp_path, optimization_iter=1, attempt=1,
                                        acceptance=False) == [0]
    assert len(_aux_write_frequencies(workload, art_dir=tmp_path, optimization_iter=1, attempt=1)) == 3


def test_a_quick_bench_that_overruns_its_limit_reports_it(monkeypatch, tmp_path):
    def hang(*a, **k):
        raise subprocess.TimeoutExpired("bench", k["timeout"])

    monkeypatch.setattr(bench.subprocess, "run", hang)
    paths = {"db_env_var": "X", "dataset_db": "/d", "aux_bench_suites": "s", "aux_bench_target": "bench"}
    monkeypatch.setattr(bench, "_private_paths", lambda cfg, wl, q: paths)
    monkeypatch.setattr(bench, "_build_benchmark", lambda cfg, target: None)
    config = SimpleNamespace(repo_root=tmp_path, build_dir=tmp_path / "build", measurement=MeasurementConfig())
    workload = {"workload_kind": "aux_state", "aux_bench_sweep_quick": {"bench_timeout_sec": 7}}
    error = bench.measure_quick_benchmark(config, workload, 6, art=tmp_path, optimization_iter=1, attempt=1,
                                          include_profile=False, acceptance=False)
    assert isinstance(error, str) and "timed out after 7s" in error and "acceptance=True lifts the limit" in error


def test_parse_custom_profile_events():
    output = """
noise
ACCEL_CUSTOM_PROFILE {"phase":"decode","decode_us":12.5}
ACCEL_CUSTOM_PROFILE: {"join_us":3.0}
"""
    events = parse_custom_profile_events(output)
    assert len(events) == 2
    assert events[0]["decode_us"] == 12.5
    assert events[1]["join_us"] == 3.0
