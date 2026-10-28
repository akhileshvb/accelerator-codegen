"""The harness conformance check: one run of the harnesses side by side on a registered query, every read forced to
the fallback, each failure naming its clause and its owner (the harness)."""

from __future__ import annotations

import pytest

from test_gate_fast_path import suite_ctx
from test_gate_plumbing import HARNESS_HOST, repo, result, run_gate  # noqa: F401  (repo is a fixture)


def conformance(repo, **env):
    ctx = {"engine_host": HARNESS_HOST, **suite_ctx(repo, "point_writes: {max_probes: 2, min_count: 1}\n")}
    proc, calls, _, gj, _ = run_gate(repo, "run_harness_conformance.sh", "5", ctx_extra=ctx, env_extra=env)
    return proc, calls, result(gj, "harness_conformance")


def test_the_harnesses_run_side_by_side_then_the_bench_hitting_and_the_calibrator(repo):
    proc, calls, data = conformance(repo)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert data["status"] == "pass"
    bench = "run benchmark_fake_tpch_aux --query 5 0"
    assert {"run test_fake_tpch_differential 5", "run test_fake_aux_multi_connection_stress 5",
            "run test_fake_cross_accel 5"} <= set(calls)
    assert calls.count(bench) == 4  # the two read-only points, forced and then hitting
    assert len([c for c in calls if c.startswith("run calibrate_fake_tpch_aux_cost_model --query 5 ")]) == 1


def test_a_multi_point_bench_is_called_once_with_the_list(repo):
    proc, calls, _ = conformance(repo, FAKE_MULTI_POINT="1")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert calls.count("run benchmark_fake_tpch_aux --query 5") == 2  # forced, then hitting


def test_a_declining_module_hits_nothing_so_there_is_no_hit_rate_to_compare(repo):
    proc, _, data = conformance(repo, FAKE_BENCH_DECLINES="1", FAKE_BENCH_BUILDS="0")
    assert proc.returncode == 0, proc.stdout + proc.stderr


@pytest.mark.parametrize("env, clause", [
    ({"FAKE_RC_run": "1"}, "differential"),
    ({"FAKE_NO_SUITE_REPORT": "1"}, "suite-report"),
    ({"FAKE_MULTI_POINT": "1", "FAKE_BENCH_ONE_ROW": "0"}, "bench-rows"),
    ({"FAKE_BENCH_NO_HIT_MEDIAN": "other,"}, "bench-columns"),
    ({"FAKE_BENCH_NO_PROFILE": "1"}, "bench-profile-window"),
    ({"FAKE_BENCH_BUILDS": "0"}, "bench-state-builds"),
    ({"FAKE_BENCH_CONSTANT": "1"}, "bench-hit-rate"),
    ({"FAKE_COST_DELTA": "null"}, "calibrate-cost-model"),
])
def test_a_broken_clause_fails_naming_itself_and_the_harness_as_owner(repo, env, clause):
    proc, _, data = conformance(repo, **env)
    assert proc.returncode == 1
    assert data["reason"].startswith(f"clause {clause}:") and data["owner"] == "harness"


def test_a_calibrator_that_measures_no_interference_on_the_declining_module_conforms(repo):
    proc, _, data = conformance(repo, FAKE_COST_DELTA="0")  # nothing is maintained: no interference to measure
    assert proc.returncode == 0, data


def test_a_failed_harness_names_its_exit_code_and_the_query(repo):
    _, _, data = conformance(repo, FAKE_RC_run="1")
    assert "exited 1 on query 5 with every read answered by the reference SQL" in data["reason"] and data["exit_code"] == 1


def test_a_failed_engine_step_is_charged_to_its_owner(repo):
    _, _, data = conformance(repo, FAKE_RC_load="1")
    assert data["status"] == "fail" and data["owner"] == "data"
