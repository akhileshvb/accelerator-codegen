"""How a harness run ends reaches the gate's result: the reason, the exit code, every thread's stack at a deadline;
aux_exit reports every failed phase; a benchmark's build.cmake cannot redefine a harness target."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

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


@pytest.mark.parametrize("waited, owner", [("12.5", "infra"), ("0.2", "query")])
def test_a_deadline_after_queueing_for_the_measurement_lease_is_infrastructures(tmp_path, waited, owner):
    import json

    result_path = tmp_path / "bench.json"
    result_path.write_text(json.dumps({"status": "fail", "reason": "bench failed", "owner": "query"}))
    (tmp_path / "bench.run.json").write_text(json.dumps({"exit_code": 124, "reason": "bench did not finish within 9s"}))
    (tmp_path / "bench.phases").write_text(f"build=3.0\nmeasurement_lease_wait={waited}\n")
    run_outcome.fold(result_path)
    assert json.loads(result_path.read_text())["owner"] == owner


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




def test_a_run_without_a_budget_runs_as_it_is(monkeypatch):
    monkeypatch.delenv("ACCEL_RUN_MEMORY_MB", raising=False)
    assert run_outcome.capped(["true"]) == ["true"]


@pytest.mark.skipif(not run_outcome._scopes(), reason="needs systemd user scopes")
def test_a_run_over_its_memory_cap_dies_alone_and_says_so(tmp_path, monkeypatch):
    monkeypatch.setenv("ACCEL_RUN_MEMORY_MB", "64")
    outcome = tmp_path / "run.json"
    rc = run_outcome.run(30, str(outcome), "hog", [sys.executable, "-c", "x = bytearray(400 * 1024 * 1024)"])
    assert rc == 137 and "over its memory cap of 64 MB" in json.loads(outcome.read_text())["reason"]


def test_in_a_sandbox_a_run_gets_a_cgroup_under_the_delegated_one_and_ended_runs_are_pruned(tmp_path, monkeypatch):
    monkeypatch.setenv("ACCEL_CGROUP", str(tmp_path))
    ended = subprocess.Popen(["true"])
    ended.wait()
    (tmp_path / f"run-{ended.pid}-0").mkdir()
    (tmp_path / f"run-{os.getpid()}-1").mkdir()  # a live process's run, not entered yet: kept
    cmd = run_outcome.capped(["true"], "64")
    cgroup = Path(cmd[3])
    assert cmd[:2] == ["sh", "-c"] and cmd[4:] == ["true"] and cgroup.parent == tmp_path
    assert (cgroup / "memory.max").read_text() == "64M" and (cgroup / "memory.swap.max").read_text() == "0"
    assert not (tmp_path / f"run-{ended.pid}-0").exists() and (tmp_path / f"run-{os.getpid()}-1").exists()
    (cgroup / "memory.events").write_text("low 0\nhigh 0\nmax 3\noom 1\noom_kill 0\n")
    assert not run_outcome._over_cap(cmd)  # killed by another signal
    (cgroup / "memory.events").write_text("low 0\nhigh 0\nmax 3\noom 1\noom_kill 1\n")
    assert run_outcome._over_cap(cmd)
