from __future__ import annotations

from pathlib import Path

from accel_pipeline.aux_bench import (
    format_bench_experiment_log_line,
    parse_sweep_csv,
    record_aux_bench_sweep_outcome,
    record_sweep_in_optimization_state,
    summarize_sweep_rows,
)


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


def test_parse_keeps_duckdb_profiler_accel_columns():
    text = """\
suite,write_policy,locality,write_frequency_pct,write_interval_sec,num_queries,writes_executed,duckdb_workload_us,accel_workload_us,workload_speedup,cold_start_workload_speedup,query_only_speedup,query_only_speedup_profiler,blended_query_speedup,accel_hit_median_us,accel_profiler_query_median_us,accel_hit_rate
lineitem_only,hybrid,global,0,0,100,0,1000,250,4.00,0.80,5.00,12.50,5.00,12000,800,1.0
"""
    rows = parse_sweep_csv_from_text(text)
    summary = summarize_sweep_rows(rows)
    point = summary["by_write_frequency_pct"]["0"]
    assert point["accel_hit_median_us"] == 12000.0
    assert point["accel_profiler_query_median_us"] == 800.0
    assert point["query_only_speedup_profiler"] == 12.5


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
    art = repo / "pipeline" / "artifacts" / "tpch" / "q18"
    art.mkdir(parents=True)
    summary = summarize_sweep_rows(parse_sweep_csv_from_text(SAMPLE_CSV))
    workload = {"artifact_namespace": "tpch", "workload_kind": "aux_state"}
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


def parse_sweep_csv_from_text(text: str):
    from accel_pipeline.aux_bench import parse_sweep_csv

    path = Path("/tmp/aux_bench_test.csv")
    path.write_text(text, encoding="utf-8")
    return parse_sweep_csv(path)
