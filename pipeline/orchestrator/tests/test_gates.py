"""Running a gate and its timeouts and captures, parallel and background gates, code stamps and the results recorded
against them, the build skipped when the code is unchanged, and the quick check (its measurements, report, write-cost
projection and config)."""
from __future__ import annotations

import json
import os
import stat
import subprocess
import threading
import time
from pathlib import Path
from types import SimpleNamespace

import pytest
from conftest import Gate, GIT, WORKLOAD

from accel_pipeline import gates
from accel_pipeline.config import (
    apply_workload_overrides,
    gates_dir_for_query,
    load_pipeline_config,
    load_workload,
    query_artifacts_dir,
    QuickTierConfig,
    workload_supported_engines,
)
from accel_pipeline.data import CutDatasets, instance_tag
from accel_pipeline.gates import (
    background_status,
    Bench,
    build_lock,
    check_version,
    current_stamp,
    enrich_failed_gate_data,
    GateResult,
    log_tail_for_agent,
    passed_at_current_stamp,
    read_phase_log,
    record_pass,
    render_quick_result,
    reusable_background_result,
    reusable_gate_result,
    run_gate,
    run_gates_parallel,
    run_quick_check,
    start_background_gate,
)
from accel_pipeline.layout import BACKENDS, code_stamp, PRIMS, suite_dir
from accel_pipeline.optimization import bench as perf_bench, loop as optimization_loop
from accel_pipeline.mcp import _compact_gate_payload


CONFIG = SimpleNamespace(quick_tier=SimpleNamespace(gate_jobs=0), worker_id="w3", repo_root=None, build_dir=Path("/builds/b3"))
TAG = instance_tag("w3", CONFIG.build_dir)
PARALLEL_WORKLOAD = {"engine_name": "eng"}


class Fake:
    """Stands in for gates.run_gate: records starts, overlaps and per-gate environments."""

    def __init__(self, monkeypatch, *, fail=(), hold=0.0, stamped=()):
        self.started: list[str] = []
        self.envs: dict[str, dict | None] = {}
        self.running = 0
        self.max_running_when: dict[str, int] = {}
        self.lock = threading.Lock()
        self.fail = set(fail)
        self.hold = hold
        self.stamped = set(stamped)
        monkeypatch.setattr(gates, "run_gate", self.run_gate)
        monkeypatch.setattr(
            gates, "reusable_gate_result",
            lambda cfg, wl, q, name, change_kind="": GateResult(name, "pass", {"reused": True}) if name in self.stamped else None,
        )
        monkeypatch.setattr(gates, "log_gate_batch_start", lambda *a, **k: None)

    def run_gate(self, config, name, query, workload, *, env=None, cancel=None, **kwargs):
        with self.lock:
            self.started.append(name)
            self.envs[name] = env
            self.running += 1
            self.max_running_when[name] = self.running
        try:
            deadline = time.monotonic() + self.hold
            while time.monotonic() < deadline:
                if cancel is not None and cancel.is_set():
                    return GateResult(name, "cancelled", {"reason": "cancelled"})
                time.sleep(0.01)
            return GateResult(name, "fail" if name in self.fail else "pass", {})
        finally:
            with self.lock:
                self.running -= 1


def statuses(results):
    return {r.name: r.status for r in results}


def test_cheap_gates_run_first_then_the_parallel_group_then_the_bench(monkeypatch):
    fake = Fake(monkeypatch)
    names = ["aux_bench_sweep", "aux_cross_accel", "aux_correctness", "aux_multi_connection", "aux_unit", "prims_tests"]
    results = run_gates_parallel(CONFIG, names, 6, PARALLEL_WORKLOAD)
    assert [r.name for r in results] == names
    assert fake.started[:3] == ["aux_unit", "aux_correctness", "prims_tests"]
    assert set(fake.started[3:5]) == {"aux_multi_connection", "aux_cross_accel"}
    assert fake.started[5] == "aux_bench_sweep"
    assert all(r.status == "pass" for r in results)


def test_gates_in_no_group_run_first_in_the_order_given(monkeypatch):
    fake = Fake(monkeypatch)
    run_gates_parallel(CONFIG, ["aux_unit", "custom_b", "custom_a"], 6, PARALLEL_WORKLOAD)
    assert fake.started == ["custom_b", "custom_a", "aux_unit"]


def test_the_parallel_group_runs_at_once_each_on_its_own_instance(monkeypatch):
    barrier = threading.Barrier(3, timeout=10)  # never opens if the gates ran one after the other
    fake = Fake(monkeypatch)
    inner = fake.run_gate

    def meet(config, name, *args, **kwargs):
        barrier.wait()
        return inner(config, name, *args, **kwargs)

    monkeypatch.setattr(gates, "run_gate", meet)
    results = run_gates_parallel(
        CONFIG, ["aux_multi_connection", "aux_cross_accel", "aux_cost_model_calibrate"], 6, PARALLEL_WORKLOAD
    )
    assert all(r.status == "pass" for r in results)
    instances = {name: env["ACCEL_ENGINE_INSTANCE"] for name, env in fake.envs.items()}
    assert instances == {
        "aux_multi_connection": f"{TAG}-eng-aux_multi_connection",
        "aux_cross_accel": f"{TAG}-eng-aux_cross_accel",
        "aux_cost_model_calibrate": f"{TAG}-eng-aux_cost_model_calibrate",
    }


def test_serial_gates_share_the_default_instance(monkeypatch):
    fake = Fake(monkeypatch)
    run_gates_parallel(CONFIG, ["aux_unit", "aux_correctness"], 6, PARALLEL_WORKLOAD)
    assert fake.envs == {"aux_unit": None, "aux_correctness": None}


def test_a_pinned_state_dir_is_split_per_parallel_gate(monkeypatch):
    monkeypatch.setenv("ACCEL_ENGINE_STATE_DIR", "/state/base")
    fake = Fake(monkeypatch)
    run_gates_parallel(CONFIG, ["aux_multi_connection", "aux_cross_accel"], 6, PARALLEL_WORKLOAD)
    assert fake.envs["aux_cross_accel"]["ACCEL_ENGINE_STATE_DIR"] == "/state/base-aux_cross_accel"


def test_a_failing_cheap_gate_cancels_everything_after_it(monkeypatch):
    fake = Fake(monkeypatch, fail={"aux_correctness"})
    names = ["aux_unit", "aux_correctness", "prims_tests", "aux_cross_accel", "aux_bench_sweep"]
    results = run_gates_parallel(CONFIG, names, 6, PARALLEL_WORKLOAD)
    assert statuses(results) == {
        "aux_unit": "pass", "aux_correctness": "fail", "prims_tests": "cancelled",
        "aux_cross_accel": "cancelled", "aux_bench_sweep": "cancelled",
    }
    assert fake.started == ["aux_unit", "aux_correctness"]
    assert "aux_correctness" in results[2].data["reason"]


def test_a_failure_in_the_parallel_group_kills_its_running_siblings(monkeypatch):
    fake = Fake(monkeypatch, fail={"aux_cross_accel"}, hold=0.0)
    inner = fake.run_gate

    def slow_sibling(config, name, *args, **kwargs):
        if name == "aux_multi_connection":
            fake.hold = 30.0  # holds until cancelled
        else:
            fake.hold = 0.0
        return inner(config, name, *args, **kwargs)

    monkeypatch.setattr(gates, "run_gate", slow_sibling)
    started = time.monotonic()
    results = run_gates_parallel(CONFIG, ["aux_multi_connection", "aux_cross_accel", "aux_bench_sweep"], 6, PARALLEL_WORKLOAD)
    assert time.monotonic() - started < 20
    assert statuses(results)["aux_cross_accel"] == "fail"
    assert statuses(results)["aux_bench_sweep"] == "cancelled"


def test_the_bench_gate_runs_alone(monkeypatch):
    fake = Fake(monkeypatch, hold=0.05)
    run_gates_parallel(CONFIG, ["aux_bench_sweep", "aux_unit", "aux_cross_accel", "aux_multi_connection"], 6, PARALLEL_WORKLOAD)
    assert fake.max_running_when["aux_bench_sweep"] == 1
    assert fake.max_running_when["aux_unit"] == 1


def test_without_fail_fast_every_gate_runs(monkeypatch):
    fake = Fake(monkeypatch, fail={"aux_unit"})
    results = run_gates_parallel(CONFIG, ["aux_unit", "aux_correctness"], 6, PARALLEL_WORKLOAD, fail_fast=False)
    assert statuses(results) == {"aux_unit": "fail", "aux_correctness": "pass"}
    assert fake.started == ["aux_unit", "aux_correctness"]


def test_a_skip_a_gate_may_not_make_counts_as_a_failure(monkeypatch):
    fake = Fake(monkeypatch)
    monkeypatch.setattr(gates, "run_gate", lambda cfg, name, *a, **k: GateResult(name, "skip", {}))
    results = run_gates_parallel(CONFIG, ["aux_unit", "aux_correctness"], 6, PARALLEL_WORKLOAD)
    assert statuses(results) == {"aux_unit": "skip", "aux_correctness": "cancelled"}
    assert fake.started == []


def test_reuse_skips_gates_that_passed_on_this_code(monkeypatch):
    fake = Fake(monkeypatch, stamped={"aux_unit"})
    results = run_gates_parallel(CONFIG, ["aux_unit", "aux_correctness"], 6, PARALLEL_WORKLOAD, reuse=True)
    assert fake.started == ["aux_correctness"]
    assert results[0].data["reused"] is True


def test_reuse_is_off_by_default(monkeypatch):
    fake = Fake(monkeypatch, stamped={"aux_unit"})
    run_gates_parallel(CONFIG, ["aux_unit"], 6, PARALLEL_WORKLOAD)
    assert fake.started == ["aux_unit"]


def test_jobs_bounds_the_parallel_group(monkeypatch):
    fake = Fake(monkeypatch, hold=0.1)
    run_gates_parallel(
        CONFIG, ["aux_multi_connection", "aux_cross_accel", "aux_cost_model_calibrate"], 6, PARALLEL_WORKLOAD, jobs=1
    )
    assert max(fake.max_running_when.values()) == 1


def test_first_failure_skips_cancelled_gates_and_allowed_skips(monkeypatch):
    results = [GateResult("aux_unit", "pass", {}), GateResult("aux_cost_model_calibrate", "skip", {}),
               GateResult("aux_cross_accel", "cancelled", {}), GateResult("aux_correctness", "fail", {}),
               GateResult("prims_tests", "fail", {})]
    assert gates.first_failure(results[:3]) is None
    assert gates.first_failure(results).name == "aux_correctness"
    assert gates.first_failure([GateResult("aux_unit", "skip", {})]).name == "aux_unit"


STAMP_WORKLOAD = {"workload_kind": "aux_state", "benchmark_name": "demo", "engine_name": "eng"}


@pytest.fixture()
def stamp_config(tmp_path):
    root = tmp_path / "repo"
    for q in (5, 6):
        (root / "accelerators" / "demo" / f"q{q}").mkdir(parents=True)
        (root / "accelerators" / "demo" / f"q{q}" / "module.cpp").write_text("// v1\n")
    (root / "pipeline" / "artifacts").mkdir(parents=True)
    (root / ".gitignore").write_text("pipeline/artifacts/\n")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run([*GIT, "add", "-A"], cwd=root, check=True)
    subprocess.run([*GIT, "commit", "-qm", "base"], cwd=root, check=True)
    return SimpleNamespace(repo_root=root, build_dir=tmp_path / "build", quick_tier=QuickTierConfig(), worker_id="")


def write(stamp_config, rel, text):
    path = stamp_config.repo_root / rel
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)


def test_the_stamp_follows_the_query_and_shared_files_only(stamp_config):
    root = stamp_config.repo_root
    base = code_stamp(root, query_dir="accelerators/demo/q5")
    write(stamp_config, "accelerators/demo/q6/module.cpp", "// other query\n")
    write(stamp_config, "pipeline/gates/x.sh", "# pipeline code\n")
    write(stamp_config, "pipeline/artifacts/tpch/q5/note.json", "{}")
    assert code_stamp(root, query_dir="accelerators/demo/q5") == base
    write(stamp_config, "accelerators/demo/q5/module.cpp", "// v2\n")
    own = code_stamp(root, query_dir="accelerators/demo/q5")
    assert own != base
    write(stamp_config, "substrate/shared.hpp", "// shared\n")
    assert code_stamp(root, query_dir="accelerators/demo/q5") != own


@pytest.mark.parametrize("shared", [
    f"{PRIMS}/ring.hpp", "accelerators/demo/common/scan.hpp", f"{BACKENDS}/eng/host.sh",
    "substrate/store.hpp", "cmake/Find.cmake", "CMakeLists.txt", "workloads/demo/q5.sql", "eval/run.py",
    "accelerators/demo/qregistry.cpp",
])
def test_a_shared_file_changes_every_querys_stamp(stamp_config, shared):
    base = {q: code_stamp(stamp_config.repo_root, query_dir=f"accelerators/demo/q{q}") for q in (5, 6)}
    write(stamp_config, shared, "// changed\n")
    assert all(code_stamp(stamp_config.repo_root, query_dir=f"accelerators/demo/q{q}") != base[q] for q in (5, 6))


@pytest.mark.parametrize("excluded", [
    "pipeline/prompts/a.md", "scripts/x.py", "docs/y.md", "build-release/z.o", "build/z.o", "build-release.build.lock", ".gitignore",
    "accelerators/demo/q50/module.cpp", "accelerators/other/q5/module.cpp",
])
def test_only_pipeline_scripts_docs_build_and_other_queries_are_excluded(stamp_config, excluded):
    base = code_stamp(stamp_config.repo_root, query_dir="accelerators/demo/q5")
    write(stamp_config, excluded, "// changed\n")
    assert code_stamp(stamp_config.repo_root, query_dir="accelerators/demo/q5") == base


def test_the_stamp_follows_content_not_timestamps(stamp_config):
    root = stamp_config.repo_root
    write(stamp_config, "substrate/a.hpp", "// one\n")
    first = code_stamp(root)
    write(stamp_config, "substrate/a.hpp", "// two\n")
    write(stamp_config, "substrate/a.hpp", "// one\n")
    assert code_stamp(root) == first


def test_extras_and_deleted_files_change_the_stamp(stamp_config):
    root = stamp_config.repo_root
    base = code_stamp(root, query_dir="accelerators/demo/q5")
    assert code_stamp(root, query_dir="accelerators/demo/q5", extra=["target"]) != base
    (root / "accelerators" / "demo" / "q5" / "module.cpp").unlink()
    assert code_stamp(root, query_dir="accelerators/demo/q5") != base


def test_a_gate_passes_at_the_stamp_it_was_recorded_for(stamp_config):
    record_pass(stamp_config, STAMP_WORKLOAD, 5, "aux_unit", current_stamp(stamp_config, STAMP_WORKLOAD, 5))
    assert passed_at_current_stamp(stamp_config, STAMP_WORKLOAD, 5, "aux_unit")
    assert not passed_at_current_stamp(stamp_config, STAMP_WORKLOAD, 5, "aux_correctness")
    write(stamp_config, "accelerators/demo/q5/module.cpp", "// v2\n")
    assert not passed_at_current_stamp(stamp_config, STAMP_WORKLOAD, 5, "aux_unit")


def test_a_pass_recorded_under_older_checks_does_not_count(stamp_config):
    record_pass(stamp_config, STAMP_WORKLOAD, 5, "aux_unit", current_stamp(stamp_config, STAMP_WORKLOAD, 5))
    write(stamp_config, "pipeline/gates/run_aux_unit_tests.sh", "# the check changed\n")
    assert not passed_at_current_stamp(stamp_config, STAMP_WORKLOAD, 5, "aux_unit")
    record_pass(stamp_config, STAMP_WORKLOAD, 5, "aux_unit", current_stamp(stamp_config, STAMP_WORKLOAD, 5))
    assert passed_at_current_stamp(stamp_config, STAMP_WORKLOAD, 5, "aux_unit")


def test_a_background_pass_under_older_checks_is_not_reused(stamp_config):
    finished(stamp_config, "aux_unit")
    write(stamp_config, "eval/harness/rules.cpp", "// the harness changed\n")
    assert reusable_background_result(stamp_config, STAMP_WORKLOAD, 5, "aux_unit") is None


def test_the_exit_run_is_reused_across_acceptance_stages_but_not_the_iteration_tier(stamp_config):
    record_pass(stamp_config, STAMP_WORKLOAD, 5, "aux_exit", current_stamp(stamp_config, STAMP_WORKLOAD, 5), change_kind="implementation")
    assert passed_at_current_stamp(stamp_config, STAMP_WORKLOAD, 5, "aux_exit", change_kind="audit")
    assert not passed_at_current_stamp(stamp_config, STAMP_WORKLOAD, 5, "aux_exit", change_kind="optimization")
    record_pass(stamp_config, STAMP_WORKLOAD, 5, "aux_exit", current_stamp(stamp_config, STAMP_WORKLOAD, 5), change_kind="optimization")
    assert not passed_at_current_stamp(stamp_config, STAMP_WORKLOAD, 5, "aux_exit", change_kind="audit")


def test_concurrent_records_all_survive(stamp_config):
    stamp = current_stamp(stamp_config, STAMP_WORKLOAD, 5)
    gates = [f"gate{i}" for i in range(8)]
    threads = [threading.Thread(target=record_pass, args=(stamp_config, STAMP_WORKLOAD, 5, g, stamp)) for g in gates]
    [t.start() for t in threads]
    [t.join() for t in threads]
    assert all(passed_at_current_stamp(stamp_config, STAMP_WORKLOAD, 5, g) for g in gates)


def test_build_lock_is_exclusive(tmp_path):
    order = []

    def second():
        with build_lock(tmp_path / "build"):
            order.append("second")

    with build_lock(tmp_path / "build"):
        thread = threading.Thread(target=second)
        thread.start()
        time.sleep(0.2)
        order.append("first")
    thread.join()
    assert order == ["first", "second"]


def finished(stamp_config, gate, *, status="pass", stamp_end=None, stamp_start=None):
    state_path = gates._state_path(stamp_config, STAMP_WORKLOAD, 5, gate)
    state_path.parent.mkdir(parents=True, exist_ok=True)
    current = current_stamp(stamp_config, STAMP_WORKLOAD, 5)
    state_path.write_text(json.dumps({
        "gate": gate, "status": status, "data": {"status": status, "reason": "r"},
        "stamp_start": stamp_start or current, "stamp_end": stamp_end or current,
        "check_version": check_version(stamp_config.repo_root),
    }))


def test_a_background_pass_on_the_current_code_is_reused(stamp_config):
    finished(stamp_config, "aux_correctness")
    result = reusable_background_result(stamp_config, STAMP_WORKLOAD, 5, "aux_correctness")
    assert result.status == "pass" and result.data["reused"] == "background"


def test_a_stale_background_stamp_triggers_a_rerun(stamp_config):
    finished(stamp_config, "aux_correctness")
    write(stamp_config, "accelerators/demo/q5/module.cpp", "// edited after the gate ran\n")
    assert reusable_background_result(stamp_config, STAMP_WORKLOAD, 5, "aux_correctness") is None
    assert background_status(stamp_config, STAMP_WORKLOAD, 5, "aux_correctness")["stale"] is True


def test_code_that_moved_during_the_run_is_not_reused_even_if_it_moved_back(stamp_config):
    finished(stamp_config, "aux_correctness", stamp_start="another-stamp")
    assert reusable_background_result(stamp_config, STAMP_WORKLOAD, 5, "aux_correctness") is None


def test_a_failed_or_missing_background_run_is_never_reused(stamp_config):
    finished(stamp_config, "aux_unit", status="fail")
    assert reusable_background_result(stamp_config, STAMP_WORKLOAD, 5, "aux_unit") is None
    assert reusable_background_result(stamp_config, STAMP_WORKLOAD, 5, "prims_tests") is None


def test_a_background_run_whose_process_vanished_is_reported_died(stamp_config):
    path = gates._state_path(stamp_config, STAMP_WORKLOAD, 5, "aux_unit")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps({"gate": "aux_unit", "status": "running", "pid": 2**22 + 12345, "started_at": time.time()}))
    assert background_status(stamp_config, STAMP_WORKLOAD, 5, "aux_unit")["status"] == "died"


def test_a_running_background_gate_says_what_it_is_doing_and_how_long(stamp_config):
    import os

    path = gates._state_path(stamp_config, STAMP_WORKLOAD, 5, "aux_exit")
    out = path.parent / "aux_exit"
    out.mkdir(parents=True)
    path.write_text(json.dumps({"gate": "aux_exit", "status": "running", "pid": os.getpid(), "started_at": time.time() - 42}))
    (out / "aux_exit.phase").write_text("running: correctness bench\n")
    (out / "aux_correctness.json.run.k3j2.log").write_text("phase build: 61.4 s\nphase suite: 70.5 s\n")
    (out / "aux_bench_sweep.json.run.q9x1.log").write_text("")
    status = background_status(stamp_config, STAMP_WORKLOAD, 5, "aux_exit")
    assert status["status"] == "running" and status["elapsed_sec"] >= 42
    assert status["phase"] == "running: correctness bench"
    assert status["progress"] == {"aux_correctness": "phase suite: 70.5 s"}
    from accel_pipeline.mcp import _compact_background_state

    assert {"elapsed_sec", "phase", "progress"} <= set(_compact_background_state(status))


def test_starting_a_background_gate_records_it_running_and_does_not_start_it_twice(stamp_config, monkeypatch):
    spawned = []

    class Proc:
        pid = 4242

    fake_subprocess = SimpleNamespace(Popen=lambda *a, **k: spawned.append(a) or Proc(), STDOUT=subprocess.STDOUT)
    monkeypatch.setattr(gates, "subprocess", fake_subprocess)
    monkeypatch.setattr(gates, "_alive", lambda pid: True)
    state = start_background_gate(stamp_config, STAMP_WORKLOAD, 5, "aux_unit", workload_name="tpch_q5_aux")
    assert state["status"] == "running" and state["pid"] == 4242 and state["stamp_start"]
    assert "--gate" in spawned[0][0] and "aux_unit" in spawned[0][0]
    again = start_background_gate(stamp_config, STAMP_WORKLOAD, 5, "aux_unit", workload_name="tpch_q5_aux")
    assert again["pid"] == 4242 and len(spawned) == 1
    status = background_status(stamp_config, STAMP_WORKLOAD, 5, "aux_unit")
    assert status["status"] == "running" and "elapsed_sec" in status


def test_a_timing_gate_is_reused_only_from_the_same_stages_acceptance_run(stamp_config):
    stamp = current_stamp(stamp_config, STAMP_WORKLOAD, 5)
    path = stamp_config.repo_root / "pipeline" / "artifacts" / "sweep.json"
    path.write_text('{"status": "pass"}')
    record_pass(stamp_config, STAMP_WORKLOAD, 5, "aux_bench_sweep", stamp, change_kind="audit", result=path)
    assert reusable_gate_result(stamp_config, STAMP_WORKLOAD, 5, "aux_bench_sweep", change_kind="audit").data["reused"] is True
    assert reusable_gate_result(stamp_config, STAMP_WORKLOAD, 5, "aux_bench_sweep", change_kind="implementation") is None
    assert reusable_gate_result(stamp_config, STAMP_WORKLOAD, 5, "aux_bench_sweep") is None
    record_pass(stamp_config, STAMP_WORKLOAD, 5, "aux_bench_sweep", stamp, change_kind="optimization", result=path)
    assert reusable_gate_result(stamp_config, STAMP_WORKLOAD, 5, "aux_bench_sweep", change_kind="optimization") is None


def test_a_correctness_gate_is_reused_across_stages_on_identical_code_only(stamp_config):
    stamp = current_stamp(stamp_config, STAMP_WORKLOAD, 5)
    path = stamp_config.repo_root / "pipeline" / "artifacts" / "unit.json"
    path.write_text('{"status": "pass", "log": "x"}')
    record_pass(stamp_config, STAMP_WORKLOAD, 5, "aux_unit", stamp, change_kind="implementation", result=path)
    assert reusable_gate_result(stamp_config, STAMP_WORKLOAD, 5, "aux_unit", change_kind="audit").data["log"] == "x"
    write(stamp_config, f"{PRIMS}/ring.hpp", "// edited\n")
    assert reusable_gate_result(stamp_config, STAMP_WORKLOAD, 5, "aux_unit", change_kind="audit") is None


def test_a_gate_without_a_readable_passing_result_is_not_reused(stamp_config):
    record_pass(stamp_config, STAMP_WORKLOAD, 5, "aux_unit", current_stamp(stamp_config, STAMP_WORKLOAD, 5), result=stamp_config.repo_root / "gone.json")
    assert reusable_gate_result(stamp_config, STAMP_WORKLOAD, 5, "aux_unit") is None


def test_a_background_pass_is_reused_for_a_non_timing_gate(stamp_config):
    finished(stamp_config, "aux_correctness")
    assert reusable_gate_result(stamp_config, STAMP_WORKLOAD, 5, "aux_correctness").data["reused"] == "background"


def test_log_tail_for_agent_never_empty_on_abort():
    text = log_tail_for_agent(
        {
            "notes": "layer harness failed via docker exec: test_mysql_epochs",
            "log_tail": "",
            "returncode": 134,
        }
    )
    assert "no captured stdout/stderr" in text
    assert "134" in text
    assert "SIGABRT" in text
    assert "test_mysql_epochs" in text


def test_enrich_failed_gate_uses_wrapper_output_when_json_tail_empty():
    out = enrich_failed_gate_data(
        {"status": "fail", "notes": "harness failed", "log_tail": ""},
        proc_returncode=1,
        proc_output="FUNCTION accel_mysql_test.mysql_accel_txn_init does not exist\n",
    )
    assert "mysql_accel_txn_init" in out["log_tail"]
    assert out["returncode"] == 1


def test_compact_gate_payload_always_has_failure_summary():
    compact = _compact_gate_payload(
        "engine_epochs",
        {
            "status": "fail",
            "notes": "layer harness failed via docker exec: test_mysql_epochs",
            "log_tail": "",
            "returncode": 134,
        },
    )
    assert compact["returncode"] == 134
    assert "SIGABRT" in compact["failure_summary"]
    assert compact["failure_summary"].strip()


def test_read_phase_log_adds_up_repeated_phases(tmp_path):
    gate_json = tmp_path / "aux_unit.json"
    gate_json.with_suffix(".phases").write_text("load_context=0.1\nrun_ctest=2.5\nrun_ctest=1.5\n")
    assert read_phase_log(gate_json) == {"load_context": 0.1, "run_ctest": 4.0}


def test_a_missing_sidecar_is_no_phases(tmp_path):
    assert read_phase_log(tmp_path / "none.json") == {}


def run_with_fake_script(monkeypatch, status, rc):
    repo = Path(__file__).resolve().parents[3]
    pc = load_pipeline_config(repo_root=repo)
    profile = sorted((repo / "pipeline" / "config" / "workloads").glob("engine_[a-z]*.yaml"))[0]
    workload = load_workload(profile.stem, repo)

    def fake_script(script, query, cwd, env):
        out = Path(env["ACCEL_GATE_OUT"])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(json.dumps({"gate": "engine_epochs", "status": status, "log_tail": "x", "returncode": rc}))
        out.with_suffix(".phases").write_text("load_context=0.25\nhost_build=3.0\nrun_x=1.0\n")

        class Proc:
            returncode = rc
            stdout = ""
            stderr = ""

        return Proc()

    monkeypatch.setattr("accel_pipeline.gates._run_gate_script", fake_script)
    monkeypatch.setattr("accel_pipeline.gates.ensure_build_dir", lambda *a, **k: None)
    return run_gate(pc, "engine_epochs", 0, workload=workload, raise_on_failure=False)


def test_run_gate_puts_phase_timings_in_the_result_and_the_gate_json(monkeypatch):
    result = run_with_fake_script(monkeypatch, "pass", 0)
    assert result.data["phase_timings"]["host_build"] == 3.0
    assert "gate_wall" in result.data["phase_timings"]
    assert json.loads(result.path.read_text())["phase_timings"]["run_x"] == 1.0


def test_a_failing_gate_keeps_its_phase_timings(monkeypatch):
    result = run_with_fake_script(monkeypatch, "fail", 1)
    assert result.status == "fail"
    assert json.loads(result.path.read_text())["phase_timings"]["load_context"] == 0.25


def test_hung_gate_is_killed_with_its_children(tmp_path, monkeypatch):
    marker = tmp_path / "child.pid"
    script = tmp_path / "hang.sh"
    script.write_text(f"sleep 300 & echo $! > {marker}; wait\n")
    monkeypatch.setattr(gates, "GATE_TIMEOUT_S", 1.0)

    started = time.monotonic()
    proc = gates._run_gate_script(script, 0, tmp_path, dict(os.environ))

    assert time.monotonic() - started < 30
    assert json.loads(proc.stdout.strip().splitlines()[-1])["status"] == "fail"
    child = int(marker.read_text())
    time.sleep(0.2)
    assert not os.path.exists(f"/proc/{child}") or "Z" in open(f"/proc/{child}/stat").read().split()[2]


def test_a_cancelled_gate_is_killed_and_reads_as_cancelled(tmp_path):
    script = tmp_path / "hang.sh"
    script.write_text("sleep 300\n")
    cancel = threading.Event()
    threading.Timer(0.5, cancel.set).start()

    started = time.monotonic()
    proc = gates._run_gate_script(script, 0, tmp_path, dict(os.environ), cancel=cancel)

    assert time.monotonic() - started < 30
    assert json.loads(proc.stdout.strip().splitlines()[-1])["status"] == "cancelled"


def test_the_timeout_can_be_given_per_call(tmp_path):
    script = tmp_path / "hang.sh"
    script.write_text("sleep 300\n")

    proc = gates._run_gate_script(script, 0, tmp_path, dict(os.environ), timeout_s=0.5)

    assert json.loads(proc.stdout.strip().splitlines()[-1])["status"] == "fail"


def test_a_tool_call_limit_kills_the_gate_group_before_the_client_gives_up(tmp_path, monkeypatch):
    marker = tmp_path / "child.pid"
    script = tmp_path / "hang.sh"
    script.write_text(f"sleep 300 & echo $! > {marker}; wait\n")
    monkeypatch.setattr(gates, "TOOL_CALL_MARGIN_S", 0.0)

    started = time.monotonic()
    with gates.tool_call_limit(1.0):
        proc = gates._run_gate_script(script, 0, tmp_path, dict(os.environ))  # no gate timeout of its own (2 h)

    assert time.monotonic() - started < 30
    verdict = json.loads(proc.stdout.strip().splitlines()[-1])
    assert verdict["status"] == "fail" and "tool-call limit" in verdict["reason"] and "background=True" in verdict["reason"]
    child = int(marker.read_text())
    time.sleep(0.2)
    assert not os.path.exists(f"/proc/{child}") or "Z" in open(f"/proc/{child}/stat").read().split()[2]


def test_outside_a_tool_call_the_gate_timeout_alone_applies(tmp_path):
    script = tmp_path / "quick.sh"
    script.write_text("echo done\n")
    with gates.tool_call_limit(1.0):
        pass
    assert gates._tool_deadline.get() is None
    assert gates._run_gate_script(script, 0, tmp_path, dict(os.environ)).stdout.strip() == "done"


def test_a_gate_still_running_when_the_server_exits_is_killed(tmp_path):
    script = tmp_path / "hang.sh"
    script.write_text("sleep 300\n")
    proc = subprocess.Popen(["bash", str(script)], start_new_session=True)
    gates._live_groups.add(proc.pid)
    gates._kill_live_groups()
    assert proc.wait(timeout=10) == -9
    gates._live_groups.discard(proc.pid)


HOST = """#!/usr/bin/env bash
echo "$@" >>"$FAKE_HOST_LOG"
if [[ "$1" == run && "$2" == perf ]]; then exec "$@"; fi
[[ "$1" == run ]] || exit 2
[[ -z "$FAKE_BENCH_FAIL" ]] || { echo "no such table" >&2; exit 1; }
echo "suite,write_frequency_pct,engine_ref_query_median_us,accel_hit_median_us,accel_write_median_us,accel_hit_rate,setup_full_build_ms,workload_speedup"
echo "lineitem_only,$ACCEL_AUX_WRITE_FREQUENCIES,2000.0,250.0,900.0,1.00,40.0,8.0"
"""


@pytest.fixture()
def bench(tmp_path, monkeypatch):
    repo = tmp_path / "repo"
    host = repo / "backends" / "fake" / "host.sh"
    host.parent.mkdir(parents=True)
    host.write_text(HOST)
    host.chmod(host.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("FAKE_HOST_LOG", str(tmp_path / "host.log"))
    monkeypatch.setattr(gates, "resolve_workload_paths", lambda *a: {"aux_bench_target": "bench_aux", "aux_bench_suites": "lineitem_only"})
    monkeypatch.setattr(gates, "host_info", lambda *a: {"embedded": True})
    cfg = SimpleNamespace(repo_root=repo, build_dir=tmp_path / "build")
    instance = {"engine_instance": "i-quick", "engine_state_dir": "/state-quick", "dataset": "/subset"}
    return Bench(cfg, {"engine_name": "fake", "benchmark_name": "acme"}, 3, instance), tmp_path / "host.log"


def test_the_bench_env_names_the_instance_the_write_frequency_and_the_repetitions(bench):
    env = bench[0].env("/work", 20, pct=50)
    assert env["ACCEL_ENGINE_INSTANCE"] == "i-quick" and env["ACCEL_ENGINE_STATE_DIR"] == "/state-quick"
    assert env["ACCEL_DATASET_DIR"] == "/subset" and env["ACCEL_AUX_WRITE_FREQUENCIES"] == "50"
    assert env["ACCEL_Q3_BENCH_SUITE"] == "lineitem_only" and env["ACCEL_BENCH_QUERIES"] == "20"


def test_a_row_is_the_benchs_csv_run_through_the_host(bench):
    b, log = bench
    row = b.row(pct=50)
    assert float(row["engine_ref_query_median_us"]) == 2000.0 and row["write_frequency_pct"] == 50
    assert log.read_text().startswith(f"run {b.cfg.build_dir}/bench_aux --query 3 50")


def test_a_failing_bench_raises_with_its_output(bench, monkeypatch):
    monkeypatch.setenv("FAKE_BENCH_FAIL", "1")
    with pytest.raises(RuntimeError, match="no such table"):
        bench[0].row()


def test_a_server_engine_has_no_counters_and_no_profile(bench, monkeypatch):
    monkeypatch.setattr(gates, "host_info", lambda *a: {"embedded": False})
    assert "server process" in bench[0].counters({"accel_hit_median_us": 250.0})["unavailable"]
    assert {r["status"] for r in bench[0].profile(1.0).values()} == {"not_applicable"}


def test_the_outermost_perf_command_goes_through_the_host(bench, monkeypatch):
    b, log = bench
    seen = []
    monkeypatch.setattr(gates.subprocess, "run", lambda command, **kw: seen.append(command))
    monkeypatch.setattr(perf_bench, "perf_binary", lambda: "/usr/bin/perf")
    run = b.perf_runner()
    run(["/usr/bin/perf", "stat", "--", "bench"], env={})
    run(["/usr/bin/perf", "report", "-i", "x"], env={})
    assert seen[0][1:3] == ["run", "/usr/bin/perf"] and seen[0][0].endswith("host.sh")
    assert seen[1] == ["/usr/bin/perf", "report", "-i", "x"]


def test_the_profile_samples_read_write_and_build(bench, monkeypatch):
    b, _ = bench
    calls = []
    monkeypatch.setattr(perf_bench, "profile_hit_path", lambda cmd, env, **kw: calls.append(("hit", cmd[-1], env["ACCEL_AUX_WRITE_FREQUENCIES"], kw["hit_us"])) or {"status": "ok"})
    monkeypatch.setattr(perf_bench, "profile_build", lambda cmd, env, **kw: calls.append(("build",)) or {"status": "ok"})
    assert set(b.profile(5.0)) == {"read", "write", "build"}
    assert calls[0][:3] == ("hit", "0", "0") and calls[0][3] == 250.0
    assert calls[1][:3] == ("hit", "50", "50") and calls[1][3] == pytest.approx(575.0)  # half reads (250 us), half writes (900 us)
    assert calls[2] == ("build",)


def build(n, one=None, scale=50.0, threads=40, budget=15.0, expected=True):
    reports = [{"build_seconds": n}] + ([{"build_seconds": one}] if one is not None else [])
    return gates.build_report(reports, scale=scale, threads=threads, budget_s=budget, expected_scaling=expected)


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
    assert gates.build_report([], scale=1, threads=4, budget_s=1, expected_scaling=True) == ("", "")
    assert gates.build_report([{"build_seconds": 0}], scale=1, threads=4, budget_s=1, expected_scaling=True) == ("", "")


@pytest.mark.parametrize("plan,expected", [
    ("PartitionedBuild partials merged by addition", True), ("", False), ("n/a: one pass", False), ("Serial scan, output bound", False),
    ("None", False)])
def test_the_analysis_expects_scaling_unless_its_build_plan_says_serial(plan, expected):
    assert gates.expects_scaling({"correctness_facts": {"scale_plan": {"build_parallelism": plan}}}) is expected
    assert gates.expects_scaling({}) is False


def test_the_read_line_compares_the_engine_sql_with_the_accelerator():
    row = {"engine_ref_query_median_us": 2000.0, "accel_hit_median_us": 250.0, "accel_hit_rate": 1.0}
    assert gates.read_line(row) == "engine SQL 2000 us, accelerator 250 us (8.0x faster)"
    assert gates.read_line({**row, "accel_hit_rate": 0.4}).endswith("only 40% of the reads were answered by the accelerator, the rest fell back to the SQL")


def test_counters_are_short_numbers():
    both = {"read": {"instructions": 650e3, "cycles": 900e3, "cache-misses": 400}, "build": {"instructions": 2e9, "cycles": 1.5e9, "cache-misses": 4e6}}
    assert gates.counters_line(both) == "per read 650.0k instructions, 900.0k cycles, 400 cache misses; build run 2.0G instructions, 1.5G cycles, 4.0M cache misses"
    assert gates.counters_line({"unavailable": "perf is not installed"}) == "unavailable (perf is not installed)"


def test_the_gate_thread_cap_is_the_env_or_the_cpus(monkeypatch):
    monkeypatch.setenv("ACCEL_AUX_THREADS", "6")
    assert gates.gate_threads() == 6
    monkeypatch.delenv("ACCEL_AUX_THREADS")
    assert gates.gate_threads() >= 1


SF1_SERIAL = {"threads": 8, "build_ms_at_1": 6935.0, "build_ms_at_N": 6577.0}  # the Sol q14 sweep, with 8 threads


def test_a_subset_build_too_short_to_time_is_judged_on_the_last_sweeps_full_scale_builds():
    reports = [{"build_seconds": 0.05}, {"build_seconds": 0.06}]
    kwargs = dict(scale=50.0, threads=8, budget_s=15.0, expected_scaling=True)
    _, defect = gates.build_report(reports, sweep_scaling=SF1_SERIAL, **kwargs)
    assert "6.58 s at 8 threads vs 6.93 s at 1 (1.1x)" in defect and "full SF1" in defect and "core::PartitionedBuild" in defect
    assert gates.build_report(reports, sweep_scaling={**SF1_SERIAL, "build_ms_at_N": 1200.0}, **kwargs)[1] == ""
    assert gates.build_report(reports, sweep_scaling={**SF1_SERIAL, "threads": 2}, **kwargs)[1] == ""
    assert gates.build_report(reports, **kwargs)[1] == ""
    assert gates.build_report(reports, sweep_scaling=SF1_SERIAL, **{**kwargs, "expected_scaling": False})[1] == ""


def edit_query(config):
    (config.repo_root / "accelerators" / "tpch" / "q6" / "module.cpp").write_text("// v2\n")


def test_a_passing_check_runs_the_quick_gate_with_the_configured_limits(config, monkeypatch):
    gate = Gate(monkeypatch)
    result = run_quick_check(config, WORKLOAD, 6, targets=["my_test"], differential=False)
    assert result.status == "pass" and result.phase_timings["host_build"] == 1.5
    call = gate.calls[0]
    assert call["name"] == "quick_check" and call["query"] == 6 and call["timeout_s"] == 120
    assert call["env"]["ACCEL_QUICK_TARGETS"] == "my_test" and call["env"]["ACCEL_QUICK_DIFFERENTIAL"] == "0"
    assert call["env"]["ACCEL_QUICK_PROBES"] == "3" and call["env"]["ACCEL_BUILD_STAMP_CACHE"] == "1"


def test_an_unchanged_tree_passes_without_running_anything_again(config, monkeypatch):
    gate = Gate(monkeypatch)
    run_quick_check(config, WORKLOAD, 6)
    again = run_quick_check(config, WORKLOAD, 6)
    assert again.status == "pass" and "unchanged" in again.summary
    assert len(gate.calls) == 1


def test_an_edit_runs_the_gate_again(config, monkeypatch):
    gate = Gate(monkeypatch)
    run_quick_check(config, WORKLOAD, 6)
    edit_query(config)
    run_quick_check(config, WORKLOAD, 6)
    assert len(gate.calls) == 2


def test_a_failing_differential_returns_a_failure_with_a_reproduce_command(config, monkeypatch):
    Gate(monkeypatch, "fail", {
        "reason": "differential failed", "step": "differential", "log_tail": "row 3 differs",
        "reproduce": "ACCEL_ENGINE_INSTANCE=i host.sh run diff_bin 6",
    })
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "fail"
    failure = result.failure
    assert failure.kind == "code" and failure.source == "quick_check"
    assert failure.reproduce == "ACCEL_ENGINE_INSTANCE=i host.sh run diff_bin 6"
    assert "row 3 differs" in failure.evidence and "differential" in failure.summary
    assert any("host_build" in line for line in failure.diagnosis)
    rendered = render_quick_result(result)
    assert "Reproduce: `ACCEL_ENGINE_INSTANCE=i host.sh run diff_bin 6`" in rendered


def test_a_failure_is_not_cached_as_a_pass(config, monkeypatch):
    gate = Gate(monkeypatch, "fail", {"reason": "x"})
    run_quick_check(config, WORKLOAD, 6)
    run_quick_check(config, WORKLOAD, 6)
    assert len(gate.calls) == 2


def test_a_timeout_becomes_a_timeout_failure(config, monkeypatch):
    Gate(monkeypatch, "fail", {"reason": "gate did not finish within 120s"})
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "fail" and result.failure.kind == "timeout"


def test_the_configured_timeout_and_probes_reach_the_gate(config, monkeypatch):
    gate = Gate(monkeypatch)
    config.quick_tier = QuickTierConfig(timeout_sec=45, differential_probes=2, build_stamp_cache=False)
    run_quick_check(config, WORKLOAD, 6)
    assert gate.calls[0]["timeout_s"] == 45
    assert gate.calls[0]["env"]["ACCEL_QUICK_PROBES"] == "2" and gate.calls[0]["env"]["ACCEL_BUILD_STAMP_CACHE"] == "0"


# the test databases and the report -------------------------------------------------------------------------

class FakeBench:
    """The aux bench on the quick instance: fixed read latency and counters."""
    instances: list = []

    def __init__(self, cfg, workload, query, instance):
        self.instance = instance
        FakeBench.instances.append(instance)

    def row(self, pct=0):
        return {"engine_ref_query_median_us": 2000.0, "accel_hit_median_us": 250.0, "accel_hit_rate": 1.0}

    def counters(self, row):
        return {"read": {"instructions": 650e3, "cycles": 900e3, "cache-misses": 400}, "build": {"instructions": 2e9, "cycles": 1.5e9, "cache-misses": 4e6}}


def stage_reports(config, build_n, build_1, plan="Partitioned build over the thread cap"):
    """The suite reports of the N-thread and the one-thread run, and the analysis' scale plan."""
    art = query_artifacts_dir(config.repo_root, 6, workload=WORKLOAD)
    art.mkdir(parents=True, exist_ok=True)
    (art / "analysis.json").write_text(json.dumps({"correctness_facts": {"scale_plan": {"build_parallelism": plan}}}))
    gates_dir = gates_dir_for_query(art)
    gates_dir.mkdir(parents=True, exist_ok=True)
    return [(path, {"build_seconds": seconds, "write_costs": []})
            for path, seconds in zip(gates.report_paths(gates_dir), (build_n, build_1))]


class ReportGate(Gate):
    def __init__(self, monkeypatch, reports, data=None):
        super().__init__(monkeypatch, data={"engine_instance": "i-quick", "engine_state_dir": "/s-quick", "dataset": "/d", **(data or {})})
        self.reports = reports
        monkeypatch.setattr(gates, "Bench", FakeBench)
        monkeypatch.setattr(gates, "gate_threads", lambda: 40)

    def run_gate(self, config, name, query, workload, **kwargs):
        for path, report in self.reports:
            path.write_text(json.dumps(report))
        return super().run_gate(config, name, query, workload, **kwargs)


def cuts(scale=50):
    return CutDatasets(Path("/subset"), Path("/tiny"), {"source_rows": {"orders": 1000 * scale}, "subset_rows": {"orders": 1000}})


def test_the_gate_runs_on_the_subset_and_a_sanitizer_run_on_the_tiny_database(config, monkeypatch):
    gate = Gate(monkeypatch)
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts())
    run_quick_check(config, WORKLOAD, 6)
    run_quick_check(config, WORKLOAD, 6, sanitizer="thread", threads=8)
    assert [c["env"]["ACCEL_SUBSET_DATASET_DIR"] for c in gate.calls] == ["/subset", "/tiny"]


def test_a_passing_check_reports_build_read_and_counters(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.2, build_1=1.0))
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts())
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "pass"
    assert result.report["build"] == "0.20 s at 40 threads, 1.00 s at 1 thread (5.0x) on the subset; projected 10 s on the full dataset (budget 15 s)"
    assert result.report["read"] == "engine SQL 2000 us, accelerator 250 us (8.0x faster)"
    assert result.report["counters"].startswith("per read 650.0k instructions, 900.0k cycles, 400 cache misses; build run 2.0G")
    assert FakeBench.instances[-1] == {"engine_instance": "i-quick", "engine_state_dir": "/s-quick", "dataset": "/d"}
    assert json.loads(render_quick_result(result))["build"] == result.report["build"]


def test_a_serial_build_fails_the_check_as_a_defect_with_the_next_step(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=1.60, build_1=1.64))
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts())
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "fail" and result.failure.kind == "code"
    assert "does not scale with threads: 1.60 s at 40 threads vs 1.64 s at 1 (1.0x)" in result.summary
    assert "PartitionedBuild" in result.summary
    assert gates.admission_refusal(config, WORKLOAD, 6, "aux_correctness")  # a failed check admits nothing


def test_a_serial_build_the_analysis_declared_serial_only_has_to_fit_the_budget(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.10, build_1=0.10, plan="n/a: a single pass"))
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts())
    assert run_quick_check(config, WORKLOAD, 6).status == "pass"


def test_a_build_too_slow_for_the_full_dataset_fails_even_when_it_scales(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.50, build_1=3.0))
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts())
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "fail" and "projected ~25 s at full scale" in result.summary and "budget 15 s" in result.summary


def test_an_unchanged_check_hands_back_its_report(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.2, build_1=1.0))
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts())
    first = run_quick_check(config, WORKLOAD, 6)
    again = run_quick_check(config, WORKLOAD, 6)
    assert "unchanged" in again.summary and again.report == first.report


def test_a_bench_that_cannot_run_is_reported_not_fatal(config, monkeypatch):
    ReportGate(monkeypatch, stage_reports(config, build_n=0.2, build_1=1.0))
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts())

    class Broken(FakeBench):
        def row(self, pct=0):
            raise RuntimeError("the bench exited 1: no such table")
    monkeypatch.setattr(gates, "Bench", Broken)
    result = run_quick_check(config, WORKLOAD, 6)
    assert result.status == "pass" and result.report["read"].startswith("unavailable (the bench exited 1")


def test_the_run_prepares_the_databases_and_prebuilds_the_sanitizer_variant_in_the_background(config, monkeypatch):
    calls = []
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts())
    monkeypatch.setattr(gates, "run_gate", lambda cfg, name, query, workload, **kw: calls.append((name, kw)))
    gates.prepare_inner_loop(config, WORKLOAD, 6).join()
    (name, kwargs), = calls
    assert name == "quick_check" and kwargs["out_dir"].name == "prebuild"
    assert kwargs["env"] == {"ACCEL_QUICK_SANITIZER": "thread", "ACCEL_QUICK_PREBUILD": "1", "ACCEL_SUBSET_DATASET_DIR": "/tiny"}
    config.quick_tier = QuickTierConfig(enabled=False)
    assert gates.prepare_inner_loop(config, WORKLOAD, 6) is None


# run_profile ----------------------------------------------------------------------------------------------

class FakeProfiles(FakeBench):
    result: dict = {}

    def profile(self, seconds):
        return self.result


OK = {"status": "ok", "sampling": {"self_pct_by_bucket": {"own_code": 90.0}, "top_functions": [
    {"self_pct": 60.0, "bucket": "own_code", "symbol": "q::Build()"}]}}


def profile(config, monkeypatch, profiles, gate_status="pass", **kwargs):
    Gate(monkeypatch, gate_status, {"engine_instance": "i", "engine_state_dir": "/s", "dataset": "/d", "reason": "boom"})
    monkeypatch.setattr(gates, "Bench", FakeProfiles)
    FakeProfiles.result = profiles
    FakeBench.instances = []
    return json.loads(gates.profile_hot_spots(config, WORKLOAD, 6, **kwargs))


def test_run_profile_returns_the_hot_spots_of_each_path(config, monkeypatch):
    out = profile(config, monkeypatch, {"read": OK, "write": OK, "build": OK})
    assert out["status"] == "pass" and set(out) == {"status", "scale", "read_path", "write_path", "build_path"}
    assert any("q::Build()" in line for line in out["build_path"])


def test_run_profile_defaults_to_the_subset_instance(config, monkeypatch):
    loaded = []
    monkeypatch.setattr(gates, "ensure_dataset_loaded", lambda *a, **k: loaded.append(k) or None)
    out = profile(config, monkeypatch, {"read": OK, "write": OK, "build": OK})
    assert out["scale"] == "subset" and not loaded
    assert FakeBench.instances == [{"engine_instance": "i", "engine_state_dir": "/s", "dataset": "/d"}]


def test_a_full_profile_runs_on_the_full_dataset_in_an_instance_of_its_own(config, monkeypatch):
    loaded = []
    monkeypatch.setattr(gates, "ensure_dataset_loaded", lambda workload, *a, **k: loaded.append(k["instance_env"]) or None)
    monkeypatch.setenv("ACCEL_DATASET_DIR", "/full")
    out = profile(config, monkeypatch, {"read": OK, "write": OK, "build": OK}, scale="full")
    (own,) = FakeBench.instances
    assert out["scale"] == "full" and own["dataset"] == "/full"
    assert own["engine_instance"] != "i" and own["engine_state_dir"] != "/s"
    assert loaded == [{"ACCEL_ENGINE_INSTANCE": own["engine_instance"], "ACCEL_ENGINE_STATE_DIR": own["engine_state_dir"]}]


def test_a_full_profile_whose_dataset_cannot_load_fails(config, monkeypatch):
    monkeypatch.setattr(gates, "ensure_dataset_loaded", lambda *a, **k: "host.sh load failed (exit 1)")
    out = profile(config, monkeypatch, {}, scale="full")
    assert out["status"] == "fail" and "host.sh load failed" in out["summary"]


def test_a_profiler_that_cannot_run_fails_run_profile_loudly(config, monkeypatch):
    out = profile(config, monkeypatch, {"read": {"status": "error", "reason": "perf is not installed on this host"}, "write": OK, "build": OK})
    assert out["status"] == "fail"
    assert "perf is not installed" in out["failure"] and "not because there is nothing to profile" in out["failure"]


def test_a_server_engine_is_not_applicable_rather_than_a_failure(config, monkeypatch):
    na = {"status": "not_applicable", "reason": "the engine runs the hit path in a server process"}
    out = profile(config, monkeypatch, {"read": na, "write": na, "build": na})
    assert out["status"] == "pass" and "not applicable" in out["read_path"][0]


def test_run_profile_needs_a_tree_that_passes_the_check(config, monkeypatch):
    out = profile(config, monkeypatch, {}, gate_status="fail")
    assert out["status"] == "fail" and "boom" in out["failure"]


REPO = Path(__file__).resolve().parents[3]
ENGINE = workload_supported_engines("tpch_q6_aux", REPO)[0]


def test_the_shipped_quick_tier_block_matches_the_design():
    config = load_pipeline_config(repo_root=REPO).quick_tier
    assert config == QuickTierConfig(
        enabled=True, timeout_sec=120, build_stamp_cache=True, differential_probes=3,
        parallel_gates=True, gate_jobs=0, write_cost_budget_share=0.5,
        admission_gates=("aux_exit", "aux_correctness", "aux_bench_sweep", "aux_tsan"),
    )


def test_the_quick_tier_survives_the_workload_overrides():
    config = load_pipeline_config(repo_root=REPO)
    workload = load_workload("tpch_q6_aux", REPO, engine=ENGINE)
    assert apply_workload_overrides(config, workload).quick_tier == config.quick_tier


def test_the_aux_workload_carries_the_quick_bench_tier():
    workload = load_workload("tpch_q6_aux", REPO, engine=ENGINE)
    assert workload["aux_bench_sweep_quick"]["bench_queries"] == 5


PROFILES = {"001.sql": ("point_update", ("orders",)), "002.sql": ("bulk_delete", ("orders",))}


def scale_of(**ratios):
    return lambda tables: max([ratios.get(t, 1.0) for t in tables] or list(ratios.values()))


def report(write_ms, read_ms=0.0, build_s=0.05, probes=(), maint_ms=-1):
    return {"build_seconds": build_s, "probe_costs_ms": list(probes),
            "write_costs": [{"txn": "001.sql", "write_ms": write_ms, "read_ms": read_ms, "maint_ms": maint_ms}]}


def project(reports, ratio=250.0, budget_s=30.0, probes_full=5):
    return gates.project(reports, profiles=PROFILES, dependent=["orders"], scale=scale_of(orders=ratio),
                              probes_full=probes_full, budget_s=budget_s)


def test_a_write_as_costly_as_a_rebuild_is_projected_linearly_and_named():
    out = project([report(write_ms=12, read_ms=168)])  # 180 ms against a 50 ms build
    assert out.over
    assert out.worst == ("point_update on orders: 180 ms on the subset, projected ~45.0 s per write at full scale (x250): "
                         "as costly as a rebuild; likely a full rebuild per write, make it incremental")


def test_the_engines_own_cost_of_the_sql_is_not_the_accelerators():
    slow_sql = report(write_ms=60, read_ms=0.3, build_s=0.08, maint_ms=0.05)  # a 60 ms commit, 50 us of maintenance
    out = project([slow_sql], ratio=1000.0)
    assert not out.over and "rebuild" not in out.worst and out.lines[0].startswith("point_update on orders: 0 ms on")
    assert project([report(write_ms=60, read_ms=0.3, build_s=0.08)], ratio=1000.0).over  # unreported: all of it counts


def test_an_incremental_write_grows_with_the_square_root_only():
    out = project([report(write_ms=0.4, read_ms=0.1, build_s=0.2)], ratio=10000.0, budget_s=100.0)
    assert not out.over and "rebuild" not in out.worst
    assert out.lines[0].startswith("point_update on orders: 0 ms on the subset, projected ~0.1 s per write at full scale (x100)")


def test_adversarial_probes_are_projected_over_the_full_gates_probe_count():
    out = project([report(0.1, build_s=1.0, probes=[30.0, 50.0])], ratio=100.0, probes_full=5)
    assert any(line.startswith("adversarial_probe on orders: 40 ms") for line in out.lines)


def test_the_projection_passes_within_the_budget_and_reports_the_costliest_kind_first():
    out = project([report(write_ms=1, read_ms=1, build_s=1.0)], ratio=10.0, budget_s=1000.0)
    assert not out.over and out.summary().startswith("projected full-gate write time")
    assert out.lines == sorted(out.lines, key=lambda line: -float(line.split("~")[1].split(" s")[0]))


def test_reports_are_read_from_both_thread_runs_and_ignore_a_report_without_costs(tmp_path):
    (tmp_path / "suite_quick_report.json").write_text(json.dumps(report(1)))
    (tmp_path / "suite_quick_report_threads1.json").write_text(json.dumps({"ok": True}))
    assert len(gates.load_reports(tmp_path)) == 1


# quick_check wiring ---------------------------------------------------------------------------------------

def stage_suite(config, subset_rows, full_rows, reports):
    """A suite, test databases of those row counts and the reports of a quick differential, for the real projection code."""
    root = config.repo_root
    suite = root / suite_dir("tpch", 6)
    (suite / "txns").mkdir(parents=True)
    (suite / "txns" / "001.sql").write_text("UPDATE orders SET o_flag = 'B' WHERE o_orderkey = 1;\n")
    (suite / "suite.yaml").write_text("txns:\n  001.sql: {kind: point_update}\npoint_writes: {max_probes: 5}\n")
    cuts = CutDatasets(root / "subset", root / "tiny", {"source_rows": {"orders": full_rows}, "subset_rows": {"orders": subset_rows}})
    art = query_artifacts_dir(root, 6, workload=WORKLOAD)
    art.mkdir(parents=True, exist_ok=True)
    (art / "analysis.json").write_text(json.dumps({"correctness_facts": {"dependent_tables": [{"name": "orders"}]}}))
    gates_dir = gates_dir_for_query(art)
    gates_dir.mkdir(parents=True, exist_ok=True)
    for path, rep in zip(gates.report_paths(gates_dir), reports):
        path.write_text(json.dumps(rep))
    return WORKLOAD, cuts


class ReportingGate(Gate):
    """A passing gate that leaves the suite reports of a differential behind."""

    def __init__(self, monkeypatch, config, workload, reports):
        super().__init__(monkeypatch)
        self.reports = reports
        self.config, self.workload = config, workload

    def run_gate(self, config, name, query, workload, **kwargs):
        gates_dir = gates_dir_for_query(query_artifacts_dir(config.repo_root, query, workload=workload))
        for path, rep in zip(gates.report_paths(gates_dir), self.reports):
            path.write_text(json.dumps(rep))
        return super().run_gate(config, name, query, workload, **kwargs)


def check(config, monkeypatch, write_ms, timeout=30.0):
    reports = [report(write_ms=write_ms, build_s=0.05)] * 2
    workload, cuts = stage_suite(config, 40, 10_000, [])  # 250x the subset
    monkeypatch.setattr(gates, "GATE_RUN_TIMEOUT_S", timeout / config.quick_tier.write_cost_budget_share)
    gate = ReportingGate(monkeypatch, config, workload, reports)
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts)
    return gates.run_quick_check(config, workload, 6), gate


def test_a_write_that_rebuilds_fails_quick_check_with_an_actionable_message(config, monkeypatch):
    result, _ = check(config, monkeypatch, write_ms=180)
    assert result.status == "fail" and result.failure.kind == "code"
    assert "point_update on orders: 180 ms on the subset, projected ~45.0 s per write" in result.summary
    assert "make it incremental" in result.summary and "would time out" in result.summary
    assert gates.admission_refusal(config, WORKLOAD, 6, "aux_correctness")  # a failed check admits nothing


def test_an_incremental_write_passes_and_the_projection_is_reported(config, monkeypatch):
    result, _ = check(config, monkeypatch, write_ms=0.5)
    assert result.status == "pass" and "projected full-gate write time" in result.report["writes_total"]
    assert result.report["writes"] and json.loads(gates.render_quick_result(result))["writes"] == result.report["writes"]


def test_a_stale_report_from_an_earlier_run_is_not_projected(config, monkeypatch):
    workload, cuts = stage_suite(config, 40, 10_000, [report(write_ms=500)] * 2)
    Gate(monkeypatch)  # leaves no report
    monkeypatch.setattr(gates, "ensure_cut_datasets", lambda *a: cuts)
    assert gates.run_quick_check(config, workload, 6).status == "pass"


# gate admission -------------------------------------------------------------------------------------------

def test_heavy_gates_are_admitted_only_after_a_passing_quick_check_on_this_code(config, monkeypatch):
    refusal = gates.admission_refusal(config, WORKLOAD, 6, "aux_correctness")
    assert "run quick_check first" in refusal and "\n" not in refusal
    Gate(monkeypatch)
    assert gates.run_quick_check(config, WORKLOAD, 6).status == "pass"
    assert gates.admission_refusal(config, WORKLOAD, 6, "aux_correctness") == ""
    (config.repo_root / "accelerators" / "tpch" / "q6" / "module.cpp").write_text("// v2\n")
    assert "run quick_check first" in gates.admission_refusal(config, WORKLOAD, 6, "aux_bench_sweep")


@pytest.mark.parametrize("kwargs", [{"differential": False}, {"sanitizer": "thread"}])
def test_only_the_plain_check_with_the_differential_admits(config, monkeypatch, kwargs):
    Gate(monkeypatch)
    gates.run_quick_check(config, WORKLOAD, 6, **kwargs)
    assert gates.admission_refusal(config, WORKLOAD, 6, "aux_tsan")


def test_an_unchanged_passing_check_still_admits(config, monkeypatch):
    Gate(monkeypatch)
    gates.run_quick_check(config, WORKLOAD, 6)
    gates.run_quick_check(config, WORKLOAD, 6, differential=False)  # another check, same code
    assert gates.admission_refusal(config, WORKLOAD, 6, "aux_correctness") == ""


def test_other_gates_and_other_workloads_are_never_refused(config):
    assert gates.admission_refusal(config, WORKLOAD, 6, "aux_unit") == ""
    assert gates.admission_refusal(config, {**WORKLOAD, "workload_kind": "base"}, 6, "aux_correctness") == ""
    config.quick_tier = QuickTierConfig(enabled=False)
    assert gates.admission_refusal(config, WORKLOAD, 6, "aux_correctness") == ""


def test_rebuilds_of_supported_delta_writes_are_unfaithful():
    from accel_pipeline.data import statements

    report = {"build_seconds": 0.020, "write_costs": [
        {"txn": "001.sql", "maint_ms": 15.0, "write_ms": 40.0, "read_ms": 1.0},   # rebuild-sized
        {"txn": "002.sql", "maint_ms": 0.2, "write_ms": 40.0, "read_ms": 1.0},    # incremental
        {"txn": "003.sql", "maint_ms": 15.0, "write_ms": 40.0, "read_ms": 1.0}]}  # rebuild, but the analysis allows it
    sql = {"001.sql": "UPDATE accounts SET status = 'x' WHERE id = 1;",
           "002.sql": "UPDATE accounts SET status = 'y' WHERE id = 2;",
           "003.sql": "DELETE FROM events WHERE id = 3;"}
    coverage = [{"table": "accounts", "mutation": "update", "behavior": "supported_delta"},
                {"table": "events", "mutation": "delete", "behavior": "bounded_rebuild"}]
    lines = gates.unfaithful_rebuilds([report], sql, coverage, statements)
    assert len(lines) == 1 and lines[0].startswith("001.sql (update accounts)")


def test_a_rebuild_shown_by_one_thread_run_only_is_noise():
    from accel_pipeline.data import statements

    slow = {"build_seconds": 0.020, "write_costs": [{"txn": "001.sql", "maint_ms": 15.0, "write_ms": 40.0, "read_ms": 1.0}]}
    fast = {"build_seconds": 0.020, "write_costs": [{"txn": "001.sql", "maint_ms": 0.2, "write_ms": 40.0, "read_ms": 1.0}]}
    args = ({"001.sql": "UPDATE accounts SET status = 'x' WHERE id = 1;"}, [{"table": "accounts", "mutation": "update", "behavior": "supported_delta"}], statements)
    assert gates.unfaithful_rebuilds([slow, fast], *args) == []
    assert len(gates.unfaithful_rebuilds([slow, slow], *args)) == 1


BUILD_WORKLOAD = {"workload_kind": "aux_state", "benchmark_name": "demo"}


@pytest.fixture()
def setup(tmp_path, monkeypatch):
    root = tmp_path / "repo"
    (root / "accelerators" / "demo" / "q4").mkdir(parents=True)
    (root / "accelerators" / "demo" / "q4" / "module.cpp").write_text("// v1\n")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run([*GIT, "add", "-A"], cwd=root, check=True)
    subprocess.run([*GIT, "commit", "-qm", "base"], cwd=root, check=True)
    build = tmp_path / "build"
    build.mkdir()
    (build / "CMakeCache.txt").write_text("")
    config = SimpleNamespace(repo_root=root, build_dir=build, quick_tier=QuickTierConfig())
    builds: list[list[str]] = []

    def fake_run(cmd, **kwargs):
        builds.append(cmd)
        return SimpleNamespace(returncode=0, stdout="ok", stderr="")

    monkeypatch.setattr(optimization_loop, "subprocess", SimpleNamespace(run=fake_run))
    monkeypatch.setattr(optimization_loop, "ensure_build_dir", lambda *a, **k: None)
    monkeypatch.setattr(optimization_loop, "resolve_workload_paths",
                        lambda *a: {"aux_bench_target": "bench_t", "aux_test_target": "test_t"})
    return config, builds


def test_the_second_build_of_unchanged_code_is_a_no_op(setup):
    config, builds = setup
    assert optimization_loop.run_build(config, BUILD_WORKLOAD, 4)[0]
    assert len(builds) == 2
    ok, out = optimization_loop.run_build(config, BUILD_WORKLOAD, 4)
    assert ok and "skipped" in out and len(builds) == 2


def test_an_edit_builds_again(setup):
    config, builds = setup
    optimization_loop.run_build(config, BUILD_WORKLOAD, 4)
    (config.repo_root / "accelerators" / "demo" / "q4" / "module.cpp").write_text("// v2\n")
    optimization_loop.run_build(config, BUILD_WORKLOAD, 4)
    assert len(builds) == 4


def test_a_failed_build_is_not_remembered(setup, monkeypatch):
    config, builds = setup
    monkeypatch.setattr(
        optimization_loop, "subprocess",
        SimpleNamespace(run=lambda cmd, **k: builds.append(cmd) or SimpleNamespace(returncode=1, stdout="", stderr="boom")),
    )
    assert not optimization_loop.run_build(config, BUILD_WORKLOAD, 4)[0]
    assert not optimization_loop.run_build(config, BUILD_WORKLOAD, 4)[0]
    assert len(builds) == 2


def test_an_unconfigured_build_dir_always_builds(setup):
    config, builds = setup
    optimization_loop.run_build(config, BUILD_WORKLOAD, 4)
    (config.build_dir / "CMakeCache.txt").unlink()
    optimization_loop.run_build(config, BUILD_WORKLOAD, 4)
    assert len(builds) == 4


def test_the_cache_can_be_turned_off(setup):
    config, builds = setup
    config.quick_tier = QuickTierConfig(build_stamp_cache=False)
    optimization_loop.run_build(config, BUILD_WORKLOAD, 4)
    optimization_loop.run_build(config, BUILD_WORKLOAD, 4)
    assert len(builds) == 4
