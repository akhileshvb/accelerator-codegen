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
    assert rows[0]["workload_speedup"] == 0.05


def test_aux_quick_metrics_uses_min_workload_across_points():
    rows = parse_aux_benchmark_output(Q18_AUX_OUTPUT)
    summary = summarize_sweep_rows(rows)
    metrics = _aux_quick_metrics(rows)
    assert metrics["mode"] == "aux_quick_sweep"
    assert metrics["write_frequency_points"] == 2
    assert metrics["composite_score"] == summary["composite_score"]
    assert metrics["composite_workload_speedup"] == 0.05
    assert metrics["read_only_query_speedup"] == 4.7
    assert metrics["speedup_median"] == 4.7
    assert metrics["setup_full_build_ms"] == 9000.0
    assert metrics["optimization_signal"] is not None


def test_aux_quick_write_frequencies_reads_workload_sweep():
    workload = {"aux_bench_sweep": {"write_frequencies": [0, 10]}}
    assert _aux_quick_write_frequencies(workload) == [0, 10]


def test_quick_benchmark_env_aux_uses_fast_settings():
    paths = {
        "db_env_var": "TPCH_SF1_DB",
        "dataset_db": "/data/tpch_sf1.duckdb",
        "aux_bench_suites": "lineitem_only",
    }
    workload = {"workload_kind": "aux_state"}
    env = _quick_benchmark_env(paths, query=18, workload=workload, include_profile=True)
    assert _is_aux_workload(workload)
    assert env["ACCEL_AUX_WRITE_FREQUENCIES"] == "0,1,10,50"
    assert env["ACCEL_BENCH_QUERIES"] == "3"
    assert env["ACCEL_BENCH_WARMUP"] == "1"
    assert "ACCEL_BENCH_PROFILE" not in env


def test_quick_benchmark_env_base_filters_single_seed():
    paths = {"db_env_var": "TPCH_SF1_DB", "dataset_db": "/data/tpch_sf1.duckdb"}
    workload = {"workload_kind": "base_kernel"}
    env = _quick_benchmark_env(paths, query=18, workload=workload, include_profile=True)
    assert env["ACCEL_BENCH_FILTER"] == "seed_1_q18"
    assert env["ACCEL_BENCH_ITERS"] == "3"
    assert env["ACCEL_BENCH_PROFILE"] == "1"
