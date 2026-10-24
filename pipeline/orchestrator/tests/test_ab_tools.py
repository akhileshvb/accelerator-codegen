from __future__ import annotations

from accel_pipeline.aux_bench import parse_aux_benchmark_output, summarize_sweep_rows
from accel_pipeline.tools.ab_tools import (
    _aux_quick_metrics,
    _aux_quick_write_frequencies,
    _is_aux_workload,
    _quick_benchmark_env,
)


Q18_AUX_OUTPUT = """\
suite,write_policy,locality,write_frequency_pct,quantity_threshold,num_queries,writes_executed,duckdb_ref_query_median_us,base_kernel_median_us,base_write_median_us,accel_write_median_us,accel_hit_median_us,fallback_median_us,accel_hit_rate,fallback_rate,total_duckdb_query_us,total_accel_query_us,total_base_write_us,total_accel_write_us,duckdb_workload_us,accel_workload_us,workload_speedup,cold_start_workload_speedup,query_only_speedup,blended_query_speedup,cold_start_accel_workload_us,setup_full_build_ms
lineitem_only,hybrid,global,0,312,5,0,150000.00,3500000.00,22000.00,0.00,3200000.00,0.00,1.00,0.00,750000.00,16000000.00,0.00,0.00,750000.00,16000000.00,0.05,0.04,4.70,4.70,16100000.00,9000.00
lineitem_only,hybrid,global,50,312,5,2,150000.00,3500000.00,22000.00,1200.00,3200000.00,0.00,1.00,0.00,750000.00,16000000.00,44000.00,2400.00,794000.00,16002400.00,0.05,0.04,4.70,4.70,161002400.00,9100.00
"""


def test_parse_aux_benchmark_output_single_row():
    rows = parse_aux_benchmark_output(Q18_AUX_OUTPUT.splitlines()[0] + "\n" + Q18_AUX_OUTPUT.splitlines()[1])
    assert len(rows) == 1
    assert rows[0]["write_frequency_pct"] == 0
    assert float(rows[0]["workload_speedup"]) == 0.05


def test_aux_quick_metrics_uses_min_workload_across_points():
    rows = parse_aux_benchmark_output(Q18_AUX_OUTPUT)
    summary = summarize_sweep_rows(rows)
    metrics = _aux_quick_metrics(rows)
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
    assert metrics["optimization_signal"] is not None


def test_aux_quick_write_frequencies_reads_workload_sweep():
    workload = {"aux_bench_sweep": {"write_frequencies": [0, 10]}}
    assert _aux_quick_write_frequencies(workload) == [0, 10]


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
    first = _aux_quick_write_frequencies(
        workload, art_dir=tmp_path, optimization_iter=1, attempt=1
    )
    second = _aux_quick_write_frequencies(
        workload, art_dir=tmp_path, optimization_iter=1, attempt=1
    )
    assert first[0] == 0
    assert len(first) == 3
    assert first == second
    assert all(f in (0, 5, 10, 25, 50) for f in first)
    third = _aux_quick_write_frequencies(
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
    env = _quick_benchmark_env(paths, query=18, workload=workload, include_profile=True)
    assert _is_aux_workload(workload)
    assert env["ACCEL_AUX_WRITE_FREQUENCIES"] == "0"
    assert env["ACCEL_BENCH_QUERIES"] == "10"
    assert env["ACCEL_BENCH_WARMUP"] == "3"
    assert env["ACCEL_AUX_CUSTOM_PROFILE"] == "1"
    assert "ACCEL_BENCH_PROFILE" not in env


def test_quick_benchmark_env_base_filters_single_seed():
    paths = {"db_env_var": "TPCH_SF1_DB", "dataset_db": "/data/tpch_sf1.duckdb"}
    workload = {"workload_kind": "base_kernel"}
    env = _quick_benchmark_env(paths, query=18, workload=workload, include_profile=True)
    assert env["ACCEL_BENCH_FILTER"] == "seed_1_q18"
    assert env["ACCEL_BENCH_ITERS"] == "3"
    assert env["ACCEL_BENCH_PROFILE"] == "1"


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
    quick = _quick_benchmark_env(paths, query=6, workload=workload, include_profile=False, acceptance=False)
    assert quick["ACCEL_AUX_WRITE_FREQUENCIES"] == "0"
    assert (quick["ACCEL_BENCH_WARMUP"], quick["ACCEL_BENCH_QUERIES"], quick["ACCEL_BENCH_REF_SAMPLES"]) == ("1", "5", "1")
    full = _quick_benchmark_env(paths, query=6, workload=workload, include_profile=False, acceptance=True)
    assert (full["ACCEL_BENCH_WARMUP"], full["ACCEL_BENCH_QUERIES"], full["ACCEL_BENCH_REF_SAMPLES"]) == ("3", "10", "2")


def test_quick_tier_defaults_apply_without_a_config_block():
    paths = {"db_env_var": "TPCH_SF1_DB", "dataset_db": "/d.db"}
    env = _quick_benchmark_env(paths, query=6, workload={"workload_kind": "aux_state"}, include_profile=False,
                               acceptance=False)
    assert env["ACCEL_BENCH_QUERIES"] == "5" and env["ACCEL_AUX_WRITE_FREQUENCIES"] == "0"


def test_quick_tier_never_samples_write_mixes_but_acceptance_does(tmp_path):
    workload = {
        "aux_bench_sweep_optimization": {"write_frequencies": [0], "write_mix_sample": {
            "enabled": True, "count": 2, "choices": [1, 5, 10], "lock_per_attempt": True}},
    }
    assert _aux_quick_write_frequencies(workload, art_dir=tmp_path, optimization_iter=1, attempt=1,
                                        acceptance=False) == [0]
    assert len(_aux_quick_write_frequencies(workload, art_dir=tmp_path, optimization_iter=1, attempt=1)) == 3


def test_the_shipped_quick_tier_is_cheaper_than_the_optimization_tier():
    from accel_pipeline.config import load_aux_state_defaults

    defaults = load_aux_state_defaults()
    quick, opt = defaults["aux_bench_sweep_quick"], defaults["aux_bench_sweep_optimization"]
    assert quick["write_frequencies"] == [0] and quick["bench_queries"] <= 5 and quick["bench_ref_samples"] == 1
    assert quick["bench_warmup"] < opt["bench_warmup"] and quick["bench_timeout_sec"] == 120


def test_a_quick_bench_that_overruns_its_limit_reports_it(monkeypatch, tmp_path):
    import subprocess

    from accel_pipeline.tools import ab_tools

    def hang(*a, **k):
        raise subprocess.TimeoutExpired("bench", k["timeout"])

    monkeypatch.setattr(ab_tools.subprocess, "run", hang)
    paths = {"db_env_var": "X", "dataset_db": "/d"}
    rows, output = ab_tools._run_aux_quick_sweep(
        tmp_path / "bench", repo=tmp_path, paths=paths, query=6,
        workload={"workload_kind": "aux_state", "aux_bench_sweep_quick": {"bench_timeout_sec": 7}}, acceptance=False)
    assert rows == [] and "did not finish within 7s" in output


def _quick_benchmark_world(monkeypatch, tmp_path, rows):
    """make_ab_tools' run_quick_benchmark with the build, the bench and the artifact writers stubbed."""
    import json
    from types import SimpleNamespace

    from accel_pipeline.config import MeasurementConfig
    from accel_pipeline.tools import ab_tools

    art = tmp_path / "art"
    art.mkdir()
    (art / ab_tools.ACTIVE_TRIAL_FILE).write_text(json.dumps(
        {"trial_id": "t", "reference_score": 1.0, "last_metrics": {"stale": True}, "last_score": 9.0}))
    paths = {"aux_bench_target": "bench", "dataset_db": "/d", "db_env_var": "X"}
    monkeypatch.setattr(ab_tools, "resolve_workload_paths", lambda cfg, wl, q: paths)
    monkeypatch.setattr("accel_pipeline.worker_isolation.ensure_private_dataset_db", lambda cfg, db, query: db)
    monkeypatch.setattr(ab_tools, "query_artifacts_dir", lambda root, q, workload=None: art)
    monkeypatch.setattr(ab_tools.subprocess, "run", lambda *a, **k: SimpleNamespace(returncode=0, stdout="", stderr=""))
    monkeypatch.setattr(ab_tools, "_run_aux_quick_sweep", lambda *a, **k: (rows, "out"))
    monkeypatch.setattr(ab_tools, "write_aux_profile_artifacts", lambda *a, **k: None)
    monkeypatch.setattr(ab_tools, "refresh_optimization_runtime", lambda *a, **k: None)
    monkeypatch.setattr(ab_tools, "build_scoreboard", lambda *a, **k: {"best_valid_score": 1.0})
    monkeypatch.setattr(ab_tools, "write_scoreboard", lambda *a, **k: None)
    monkeypatch.setattr(ab_tools, "_editable_rel_paths", lambda ctx: [])
    config = SimpleNamespace(repo_root=tmp_path, build_dir=tmp_path / "build", measurement=MeasurementConfig())
    ctx = ab_tools.AbToolContext(config, 6, {"workload_kind": "aux_state", "benchmark_name": "demo"})
    _, run_quick_benchmark, _ = ab_tools.make_ab_tools(ctx)
    return art, run_quick_benchmark


def test_a_quick_bench_that_measured_the_fallback_is_flagged_and_voids_the_trial(monkeypatch, tmp_path):
    import json

    rows = parse_aux_benchmark_output(Q18_AUX_OUTPUT.replace(",1.00,0.00,", ",0.00,1.00,", 2))
    art, run_quick_benchmark = _quick_benchmark_world(monkeypatch, tmp_path, rows)
    payload = json.loads(run_quick_benchmark(include_profile=False, acceptance=True))
    assert payload["ab_signal"] == "invalid_measurement"
    assert any("fallback" in line for line in payload["measurement_invalid"])
    trial = json.loads((art / "ab_active_trial.json").read_text())
    assert "last_metrics" not in trial and "last_score" not in trial
