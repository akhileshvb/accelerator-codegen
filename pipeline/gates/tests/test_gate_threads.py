"""Thread checks in the gates: the differential at N and 1 threads (full tier), the thread-scaling point of the
bench sweep, and the static thread-count check, against the fake host of test_gate_plumbing."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from test_gate_plumbing import GATES, repo, result, run_gate  # noqa: F401  (repo is a fixture)

sys.path.insert(0, str(GATES))


def qdir(bench, query):
    return "/".join(("accelerators", bench, f"q{query}"))


def thread_values(log):
    return [l.rsplit("threads=", 1)[1] for l in log.read_text().splitlines() if l.startswith("run diff_bin")]


def test_full_tier_differential_runs_at_n_then_one_thread(repo):
    proc, _, log, gj, _ = run_gate(repo, "run_aux_correctness.sh", "5", env_extra={"ACCEL_AUX_THREADS": "4"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert sorted(thread_values(log)) == ["1", "4"]  # side by side: the order is not fixed
    assert result(gj, "aux_correctness")["threads_checked"] == [4, 1]


def test_quick_tier_differential_runs_once_at_n(repo):
    _, calls, log, gj, _ = run_gate(repo, "run_aux_correctness.sh", "5",
                                    env_extra={"ACCEL_AUX_THREADS": "4", "ACCEL_GATE_TIER": "quick"})
    assert thread_values(log) == ["4"]
    assert result(gj, "aux_correctness")["threads_checked"] == [4]


def test_quick_check_runs_the_differential_at_n_then_one_thread_like_the_full_tier(repo):
    proc, _, log, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5",
                                   env_extra={"ACCEL_GATE_NAME": "quick_check", "ACCEL_AUX_THREADS": "4"})
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert thread_values(log) == ["4", "1"]


def test_a_quick_check_that_fails_only_at_one_thread_names_it(repo):
    proc, _, _, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra={
        "ACCEL_GATE_NAME": "quick_check", "ACCEL_AUX_THREADS": "4", "FAKE_FAIL_AT_ONE_THREAD": "1"})
    assert proc.returncode == 1
    assert result(gj, "quick_check")["step"] == "differential_threads1"


def test_a_differential_that_fails_at_n_threads_is_reported_with_the_one_thread_run_done(repo):
    proc, _, log, gj, _ = run_gate(repo, "run_aux_correctness.sh", "5",
                                   env_extra={"ACCEL_AUX_THREADS": "4", "FAKE_FAIL_AT_N_THREADS": "1"})
    assert proc.returncode == 1
    assert sorted(thread_values(log)) == ["1", "4"]
    assert "threads=4" in result(gj, "aux_correctness")["reason"]


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
    # N is the configured cap (4), measured on 4 CPUs of the worker's core set although the lease slot is smaller.
    proc, _, log, gj, _ = sweep(repo, tmp_path, ACCEL_CORE_SET="0-7")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    gate = result(gj, "aux_bench_sweep")
    scaling = gate["thread_scaling"]
    assert scaling["threads"] == 4
    assert scaling["read_us_at_1"] == 400 and scaling["read_us_at_N"] == 100
    assert scaling["read_speedup_at_N_vs_1"] == 4.0
    assert scaling["build_speedup_at_N_vs_1"] == 4.0
    assert "cpus 0,1,2,3)" in gate["log_tail"]
    summary = json.loads((repo / "art" / "aux_bench_summary.json").read_text())
    assert summary["thread_scaling"] == scaling
    assert gate["status"] == "pass"


def bench_calls(log):
    return [l for l in log.read_text().splitlines() if l.startswith("run bench_aux")]


def test_a_multi_point_bench_measures_every_frequency_in_one_call(repo, tmp_path):
    proc, _, log, gj, _ = sweep(repo, tmp_path, FAKE_MULTI_POINT="1", ACCEL_AUX_THREAD_POINT="0")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert len(bench_calls(log)) == 1
    csv = Path(result(gj, "aux_bench_sweep")["results_csv"]).read_text().splitlines()
    assert [row.split(",")[1] for row in csv[1:]] == ["0", "10"]


def test_a_single_point_bench_is_called_once_per_frequency(repo, tmp_path):
    proc, _, log, gj, _ = sweep(repo, tmp_path, ACCEL_AUX_THREAD_POINT="0")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert len(bench_calls(log)) == 2
    assert len(Path(result(gj, "aux_bench_sweep")["results_csv"]).read_text().splitlines()) == 3


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


SAN_ENV = {"ACCEL_GATE_NAME": "quick_check", "ACCEL_QUICK_SANITIZER": "thread", "ACCEL_QUICK_THREADS": "3", "FAKE_EMBEDDED": "true"}


def test_sanitizer_check_builds_its_own_instrumented_dir_with_every_phase_at_n_threads(repo):
    proc, _, log, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra=SAN_ENV)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    lines = log.read_text().splitlines()
    assert all("build=" in l and "-thread" in l.split("build=")[1].split(" ")[0] for l in lines if l.startswith("build"))
    assert all(l.endswith("threads=3") for l in lines if l.startswith("run"))
    cache = (repo / "build-release-thread" / "CMakeCache.txt").read_text()
    assert "CMAKE_BUILD_TYPE:STRING=RelWithDebInfo" in cache
    data = result(gj, "quick_check")
    assert data["status"] == "pass" and data["sanitizer"] == "thread" and data["threads"] == 3


def test_a_sanitizer_quick_check_runs_the_differential_at_its_one_thread_count_only(repo):
    _, _, log, _, _ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra=SAN_ENV)
    assert thread_values(log) == ["3"]


def test_a_report_with_a_frame_in_the_query_fails_and_engine_reports_do_not(repo):
    proc, _, _, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra={**SAN_ENV, "FAKE_TSAN_REPORT": f"/r/{qdir('tpch', 5)}"})
    assert proc.returncode == 1
    data = result(gj, "quick_check")
    found = data["sanitizer_findings"]
    assert data["step"] == "sanitizer" and found["yours_count"] == 1 and found["infra"]["count"] == 1
    assert found["yours"][0]["frames"][0].startswith(f"#0 Add /r/{qdir('tpch', 5)}/module.cpp:9")


def test_a_report_outside_the_query_is_infrastructure_only(repo):
    proc, _, _, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra={**SAN_ENV, "FAKE_TSAN_REPORT": f"/r/{qdir('tpch', 6)}"})
    assert proc.returncode == 0, proc.stdout
    found = result(gj, "quick_check")["sanitizer_findings"]
    assert found["yours_count"] == 0 and found["infra"]["count"] == 2


def test_a_host_that_cannot_build_the_variant_reports_not_applicable(repo):
    proc, calls, _, gj, _ = run_gate(repo, "run_aux_quick_check.sh", "5", env_extra={**SAN_ENV, "FAKE_EMBEDDED": "false"})
    assert proc.returncode == 0 and not any(c.startswith("build") for c in calls)
    data = result(gj, "quick_check")
    assert data["status"] == "skip" and "not applicable" in data["reason"]


def test_an_engine_race_under_a_call_from_the_query_is_infrastructure(tmp_path):
    import sanitizer_findings

    Q = qdir("b", 1)

    (tmp_path / "tsan.1").write_text(
        "WARNING: ThreadSanitizer: data race (pid=1)\n"
        "  Write of size 4 at 0x1 by thread T1:\n"
        "    #0 __exchange_and_add /usr/include/c++/16/ext/atomicity.h:71 (bin+0x1)\n"
        "    #1 Event::Finish() <null> (libengine.so+0x2)\n"
        f"    #2 Scan /r/{Q}/module.cpp:5 (lib+0x3)\n\n"
        "  Previous read of size 4 at 0x1 by thread T2:\n"
        "    #0 Task::Run() <null> (libengine.so+0x4)\n"
        f"    #1 Scan /r/{Q}/module.cpp:5 (lib+0x3)\n\n"
        "SUMMARY: ThreadSanitizer: data race (libengine.so+0x2) in Event::Finish()\n")
    (tmp_path / "tsan.2").write_text(
        "WARNING: ThreadSanitizer: data race (pid=1)\n"
        "  Write of size 8 at 0x1 by thread T1:\n"
        "    #0 std::vector<int>::push_back /usr/include/c++/16/bits/vector.h:1 (bin+0x1)\n"
        f"    #1 Add /r/{Q}/module.cpp:9 (lib+0x5)\n\n"
        "SUMMARY: ThreadSanitizer: data race in Add\n")
    found = sanitizer_findings.classify(tmp_path, Q + "/")
    assert found["yours_count"] == 1 and found["yours"][0]["frames"][0].startswith("#1 Add") or "module.cpp:9" in found["yours"][0]["frames"][0]
    assert found["infra"]["count"] == 1


def test_a_race_on_a_global_of_the_query_is_the_querys_even_when_the_access_frames_are_the_runtimes(tmp_path):
    import sanitizer_findings

    Q = qdir("b", 1)
    (tmp_path / "tsan.1").write_text(
        "WARNING: ThreadSanitizer: data race (pid=1)\n"
        "  Write of size 8 at 0x1 by thread T1:\n"
        "    #0 std::thread::_State_impl::_M_run() /usr/include/c++/16/bits/std_thread.h:76 (lib+0x1)\n\n"
        "  Previous read of size 8 at 0x1 by thread T2:\n"
        "    #0 std::thread::_State_impl::_M_run() /usr/include/c++/16/bits/std_thread.h:76 (lib+0x1)\n\n"
        f"  Location is global 'query::g_total' of size 8 at 0x1 (/r/{Q}/module.cpp:3)\n\n"
        "SUMMARY: ThreadSanitizer: data race in std::thread::_State_impl::_M_run()\n")
    found = sanitizer_findings.classify(tmp_path, Q + "/")
    assert found["yours_count"] == 1 and found["infra"]["count"] == 0
