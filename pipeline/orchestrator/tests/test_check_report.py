"""The lines quick_check reports besides correctness, and the defects among them."""

from __future__ import annotations

import pytest

from accel_pipeline import check_report as cr


def build(n, one=None, scale=50.0, threads=40, budget=15.0, expected=True):
    reports = [{"build_seconds": n}] + ([{"build_seconds": one}] if one is not None else [])
    return cr.build_report(reports, scale=scale, threads=threads, budget_s=budget, expected_scaling=expected)


def test_a_parallel_build_inside_the_budget_is_a_line_and_no_defect():
    line, defect = build(0.2, 1.0)
    assert line == "0.20 s at 40 threads, 1.00 s at 1 thread (5.0x) on the subset; projected 10 s on the full dataset (budget 15 s)"
    assert defect == ""


def test_a_build_that_ignores_its_threads_is_a_defect_naming_the_primitive_to_use():
    line, defect = build(1.9, 2.0, scale=1.0)
    assert "(1.1x)" in line
    assert defect.startswith("the state build does not scale with threads: 1.90 s at 40 threads vs 2.00 s at 1 (1.1x)")
    assert "core::PartitionedBuild" in defect


def test_a_build_the_analysis_did_not_plan_to_split_only_has_to_fit_the_budget():
    assert build(0.10, 0.10, expected=False)[1] == ""
    assert "too slow for the full dataset" in build(1.2, 1.2, expected=False)[1]


def test_a_slow_build_is_a_defect_with_the_projection():
    _, defect = build(0.5, 3.0)
    assert "0.50 s on the subset at 40 threads, projected ~25 s at full scale" in defect and "budget 15 s" in defect


def test_a_build_too_short_to_time_gets_no_verdict_from_the_subset():
    # the clean Q3 run: 0.67 s at 8 threads vs 0.24 s at 1 on the subset, 6.6 s vs 21 s on the full SF1 build
    line, defect = build(0.67, 0.24, threads=8, scale=130.0)
    assert defect == "" and line.endswith("too short to time (the exit gate times the full build)")


def test_scaling_is_not_judged_with_few_threads():
    assert "does not scale" not in build(1.5, 1.6, threads=2, scale=1.0)[1]


def test_without_a_one_thread_run_there_is_only_the_projection():
    line, defect = build(1.0, None, scale=10.0)
    assert line == "1.00 s at 40 threads on the subset; projected 10 s on the full dataset (budget 15 s)" and defect == ""


def test_no_report_no_line():
    assert cr.build_report([], scale=1, threads=4, budget_s=1, expected_scaling=True) == ("", "")
    assert cr.build_report([{"build_seconds": 0}], scale=1, threads=4, budget_s=1, expected_scaling=True) == ("", "")


@pytest.mark.parametrize("plan,expected", [
    ("PartitionedBuild partials merged by addition", True), ("", False), ("n/a: one pass", False), ("Serial scan, output bound", False),
    ("None", False)])
def test_the_analysis_expects_scaling_unless_its_build_plan_says_serial(plan, expected):
    assert cr.expects_scaling({"correctness_facts": {"scale_plan": {"build_parallelism": plan}}}) is expected
    assert cr.expects_scaling({}) is False


def test_the_read_line_compares_the_engine_sql_with_the_accelerator():
    row = {"engine_ref_query_median_us": 2000.0, "accel_hit_median_us": 250.0, "accel_hit_rate": 1.0}
    assert cr.read_line(row) == "engine SQL 2000 us, accelerator 250 us (8.0x faster)"
    assert cr.read_line({**row, "accel_hit_rate": 0.4}).endswith("only 40% of the reads were answered by the accelerator, the rest fell back to the SQL")


def test_counters_are_short_numbers():
    both = {"read": {"instructions": 650e3, "cycles": 900e3, "cache-misses": 400}, "build": {"instructions": 2e9, "cycles": 1.5e9, "cache-misses": 4e6}}
    assert cr.counters_line(both) == "per read 650.0k instructions, 900.0k cycles, 400 cache misses; build run 2.0G instructions, 1.5G cycles, 4.0M cache misses"
    assert cr.counters_line({"unavailable": "perf is not installed"}) == "unavailable (perf is not installed)"


def test_the_gate_thread_cap_is_the_env_or_the_cpus(monkeypatch):
    monkeypatch.setenv("ACCEL_AUX_THREADS", "6")
    assert cr.gate_threads() == 6
    monkeypatch.delenv("ACCEL_AUX_THREADS")
    assert cr.gate_threads() >= 1


SF1_SERIAL = {"threads": 8, "build_ms_at_1": 6935.0, "build_ms_at_N": 6577.0}  # the Sol q14 sweep, with 8 threads


def test_a_subset_build_too_short_to_time_is_judged_on_the_last_sweeps_full_scale_builds():
    reports = [{"build_seconds": 0.05}, {"build_seconds": 0.06}]
    kwargs = dict(scale=50.0, threads=8, budget_s=15.0, expected_scaling=True)
    _, defect = cr.build_report(reports, sweep_scaling=SF1_SERIAL, **kwargs)
    assert "6.58 s at 8 threads vs 6.93 s at 1 (1.1x)" in defect and "full SF1" in defect and "core::PartitionedBuild" in defect
    assert cr.build_report(reports, sweep_scaling={**SF1_SERIAL, "build_ms_at_N": 1200.0}, **kwargs)[1] == ""
    assert cr.build_report(reports, sweep_scaling={**SF1_SERIAL, "threads": 2}, **kwargs)[1] == ""
    assert cr.build_report(reports, **kwargs)[1] == ""
    assert cr.build_report(reports, sweep_scaling=SF1_SERIAL, **{**kwargs, "expected_scaling": False})[1] == ""
