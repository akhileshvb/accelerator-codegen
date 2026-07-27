from __future__ import annotations

from pathlib import Path

from accel_pipeline.aux_bench import (
    format_bench_experiment_log_line,
    parse_sweep_csv,
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


def parse_sweep_csv_from_text(text: str):
    from accel_pipeline.aux_bench import parse_sweep_csv

    path = Path("/tmp/aux_bench_test.csv")
    path.write_text(text, encoding="utf-8")
    return parse_sweep_csv(path)
