"""How a harness run ends reaches the gate's result: the reason, the exit code, every thread's stack at a deadline;
aux_exit reports every failed phase; a benchmark's build.cmake cannot redefine a harness target."""

from __future__ import annotations

import shutil
import sys
import time

import pytest

from test_gate_exit import exit_run
from test_gate_plumbing import GATES, repo, result, run_gate  # noqa: F401  (repo is a fixture)
from test_gate_threads import bench_calls, sweep

sys.path.insert(0, str(GATES))
import run_outcome  # noqa: E402


def gate(repo, script, name, **env):
    """run_gate with ACCEL_GATE_OUT on the result the script writes, as the orchestrator sets it."""
    return run_gate(repo, script, "3", env_extra={"ACCEL_GATE_OUT": str(repo / "art" / "gates" / f"{name}.json"), **env})


@pytest.mark.skipif(not shutil.which("gdb"), reason="needs gdb")
def test_a_run_past_its_deadline_reports_why_and_every_threads_stack(repo):
    started = time.monotonic()
    proc, _, _, gj, _ = gate(repo, "run_correctness.sh", "correctness", FAKE_RUN_HANG="1", ACCEL_GATE_RUN_TIMEOUT="1")
    assert proc.returncode != 0 and time.monotonic() - started < 25  # the hung harness (sleep 30) was killed
    data = result(gj, "correctness")
    assert data["exit_code"] == 124
    assert data["reason"].startswith("diff_bin failed for query 3; diff_bin did not finish within 1s")
    assert "== process" in data["stacks"] and "(sleep)" in data["stacks"]
    assert not (gj / "correctness.run.json").exists()


def test_a_failed_run_names_its_target_and_exit_code_when_the_gate_gives_no_reason(repo):
    proc, _, _, gj, _ = gate(repo, "run_aux_cross_accel.sh", "aux_cross_accel", FAKE_RC_run="3")
    assert proc.returncode == 1
    data = result(gj, "aux_cross_accel")
    assert data["reason"] == "cross_bin exited 3" and data["exit_code"] == 3
    assert "[host.sh build" in data["log_tail"] and "Building" not in data["log_tail"]  # a passing build adds one line


def test_a_passing_run_leaves_no_outcome(repo):
    proc, _, _, gj, _ = gate(repo, "run_aux_cross_accel.sh", "aux_cross_accel")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert "stacks" not in result(gj, "aux_cross_accel") and not (gj / "aux_cross_accel.run.json").exists()


def test_the_exit_run_reports_every_failed_phase_and_how_to_rerun_each(repo, tmp_path):
    proc, _, _, gj, _ = exit_run(repo, tmp_path, FAKE_RC_run="1", ACCEL_EXIT_PHASES="cross_accel multi_connection")
    assert proc.returncode == 1
    data = result(gj, "aux_exit")
    assert set(data["failed_phases"]) == {"cross_accel", "multi_connection"}
    assert "cross_accel: cross_bin exited 1" in data["reason"] and "multi_connection: " in data["reason"]
    assert data["reproduce"] == 'request_gate("aux_cross_accel"); request_gate("aux_multi_connection")'
    assert "=== cross_accel ===" in data["log_tail"] and "=== multi_connection ===" in data["log_tail"]


def test_a_benchmark_build_cmake_that_names_a_harness_target_fails_the_build(repo):
    (repo / "accelerators" / "toybench").mkdir(parents=True)
    (repo / "accelerators" / "toybench" / "build.cmake").write_text(
        "set_property(TARGET aux_unit_bin PROPERTY SOURCES q3/tests/mine.cpp)\n")
    proc, calls, _, gj, _ = gate(repo, "run_aux_correctness.sh", "aux_correctness")
    assert proc.returncode == 1 and not any(c.startswith("build") for c in calls)
    data = result(gj, "aux_correctness")
    assert "accelerators/toybench/build.cmake names the harness target aux_unit_bin" in data["reason"]


def test_a_multi_point_bench_gets_the_list_and_no_percent_argument(repo, tmp_path):
    proc, _, log, _, _ = sweep(repo, tmp_path, FAKE_MULTI_POINT="1", ACCEL_AUX_THREAD_POINT="0")
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert [call.split(" | ")[0] for call in bench_calls(log)] == ["run bench_aux --query 5"]


def test_condensed_stacks_count_threads_that_share_their_frames():
    gdb = """
== process 42 (harness) ==
Thread 3 (Thread 0x7f (LWP 45) "worker"):
#0  0x00007f in __futex_abstimed_wait_common (...) from /usr/lib/libc.so.6
#1  0x00007f in Pool::Wait (...) at /src/substrate/pool.cpp:88
#2  0x00007f in start_thread (...) from /usr/lib/libc.so.6

Thread 2 (Thread 0x7e (LWP 44) "worker"):
#0  0x00007f in __futex_abstimed_wait_common (...) from /usr/lib/libc.so.6
#1  0x00007f in Pool::Wait (...) at /src/substrate/pool.cpp:88
#2  0x00007f in start_thread (...) from /usr/lib/libc.so.6

Thread 1 (Thread 0x7d (LWP 42) "harness"):
#0  Build::Seal (...) at /src/q4/build_state.hpp:149
#1  0x00007f in main (...) at /src/harness.cpp:12
"""
    assert run_outcome.condense(gdb).splitlines() == [
        "== process 42 (harness) ==",
        "  2 threads:", "    __futex_abstimed_wait_common  libc.so.6", "    Pool::Wait  pool.cpp:88",
        "    start_thread  libc.so.6",
        "  1 thread:", "    Build::Seal  build_state.hpp:149", "    main  harness.cpp:12",
    ]


def test_harness_acceptance_runs_every_harness_on_the_query_with_its_suite(repo):
    from test_gate_fast_path import suite_ctx

    ctx = suite_ctx(repo, "point_writes: {max_probes: 2, min_count: 1}\n")
    proc, calls, _, gj, _ = run_gate(repo, "run_harness_acceptance.sh", "5", ctx_extra=ctx)
    assert proc.returncode == 0, proc.stdout + proc.stderr
    assert sorted(c for c in calls if c.startswith("run ")) == ["run cross_bin 5", "run diff_bin 5", "run multi_bin 5"]
    assert result(gj, "harness_acceptance")["status"] == "pass"


def test_a_harness_that_fails_on_the_declining_module_is_named_as_the_harness(repo):
    from test_gate_fast_path import suite_ctx

    ctx = suite_ctx(repo, "point_writes: {max_probes: 2, min_count: 1}\n")
    proc, _, _, gj, _ = run_gate(repo, "run_harness_acceptance.sh", "5", ctx_extra=ctx,
                                 env_extra={"ACCEL_GATE_OUT": str(repo / "art" / "gates" / "harness_acceptance.json"),
                                            "FAKE_RC_run": "1"})
    assert proc.returncode == 1
    data = result(gj, "harness_acceptance")
    assert data["reason"].startswith("diff_bin exited 1 on query 5 with the declining module") and "harness is wrong" in data["reason"]
    assert data["owner"] == "harness"
