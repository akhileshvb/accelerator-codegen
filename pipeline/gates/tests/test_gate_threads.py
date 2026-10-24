"""Thread checks in the gates: the differential at N and 1 threads (full tier), the thread-scaling point of the
bench sweep, and the static thread-count check, against the fake host of test_gate_plumbing."""

from __future__ import annotations

import json

from test_gate_plumbing import GATES, repo, result, run_gate  # noqa: F401  (repo is a fixture)


def thread_values(log):
    return [l.rsplit("threads=", 1)[1] for l in log.read_text().splitlines() if l.startswith("run diff_bin")]


def test_full_tier_differential_runs_at_n_then_one_thread(repo):
    proc, _, log, gj, _ = run_gate(repo, "run_aux_correctness.sh", "5", env_extra={"ACCEL_AUX_THREADS": "4"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert thread_values(log) == ["4", "1"]
    assert result(gj, "aux_correctness")["threads_checked"] == [4, 1]


def test_quick_tier_differential_runs_once_at_n(repo):
    _, calls, log, gj, _ = run_gate(repo, "run_aux_correctness.sh", "5",
                                    env_extra={"ACCEL_AUX_THREADS": "4", "ACCEL_GATE_TIER": "quick"})
    assert thread_values(log) == ["4"]
    assert result(gj, "aux_correctness")["threads_checked"] == [4]


def test_a_differential_that_fails_only_at_one_thread_fails_the_gate(repo):
    proc, _, _, gj, _ = run_gate(repo, "run_aux_correctness.sh", "5",
                                 env_extra={"ACCEL_AUX_THREADS": "4", "FAKE_FAIL_AT_ONE_THREAD": "1"})
    assert proc.returncode == 1
    assert "threads=1" in result(gj, "aux_correctness")["reason"]


def test_a_single_thread_budget_runs_the_differential_once(repo):
    _, _, log, gj, _ = run_gate(repo, "run_aux_correctness.sh", "5", env_extra={"ACCEL_AUX_THREADS": "1"})
    assert thread_values(log) == ["1"]


def sweep(repo, tmp_path, **env):
    (repo / "pipeline" / "orchestrator").symlink_to(GATES.parents[0] / "orchestrator")
    return run_gate(repo, "run_aux_bench_sweep.sh", "5", env_extra={
        "FAKE_SWEEP": "1", "ACCEL_AUX_WRITE_FREQUENCIES": "0,10", "ACCEL_AUX_THREADS": "4",
        "ACCEL_HOST_LOCK_DIR": str(tmp_path / "locks"), "ACCEL_MEASURE_SET": "0-3", **env,
    })


def test_bench_sweep_reports_the_read_speedup_of_n_threads_over_one(repo, tmp_path):
    # ACCEL_AUX_THREADS is the ambient cap here; the extra point overrides it to 1 for that one run.
    proc, _, log, gj, _ = sweep(repo, tmp_path, ACCEL_AUX_THREADS="")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    gate = result(gj, "aux_bench_sweep")
    scaling = gate["thread_scaling"]
    assert scaling["threads"] == 4
    assert scaling["read_us_at_1"] == 400 and scaling["read_us_at_N"] == 100
    assert scaling["read_speedup_at_N_vs_1"] == 4.0
    assert scaling["build_speedup_at_N_vs_1"] == 4.0
    summary = json.loads((repo / "art" / "aux_bench_summary.json").read_text())
    assert summary["thread_scaling"] == scaling
    assert gate["status"] == "pass"


def test_the_thread_point_is_report_only(repo, tmp_path):
    proc, _, _, gj, _ = sweep(repo, tmp_path, ACCEL_AUX_THREAD_POINT="0", ACCEL_AUX_THREADS="")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert result(gj, "aux_bench_sweep")["thread_scaling"] == {}


def offending_query(repo):
    src = repo / "accelerators" / "tpch" / "q5"
    src.mkdir(parents=True)
    (src / "module.cpp").write_text("void Run() { unsigned threads = 8; }\n")


def test_aux_unit_fails_on_a_hard_coded_thread_count(repo):
    offending_query(repo)
    proc, calls, _, gj, _ = run_gate(repo, "run_aux_unit_tests.sh", "5")
    assert proc.returncode == 1 and not any(c.startswith("run") for c in calls)
    assert "thread" in result(gj, "aux_unit")["reason"]


def test_quick_check_only_warns_on_a_hard_coded_thread_count(repo):
    offending_query(repo)
    proc, _, _, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra={"ACCEL_GATE_NAME": "quick_check"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    data = result(gj, "quick_check")
    assert data["status"] == "pass" and "fixed_thread_count" in data["thread_lint_warning"]
