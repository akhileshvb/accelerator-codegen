from __future__ import annotations

import threading
import time
from types import SimpleNamespace

import pytest

from accel_pipeline import gate_parallel
from accel_pipeline.gate_parallel import run_gates_parallel
from accel_pipeline.gates import GateResult

CONFIG = SimpleNamespace(quick_tier=SimpleNamespace(gate_jobs=0), worker_id="w3", repo_root=None)
WORKLOAD = {"engine_name": "eng"}


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
        monkeypatch.setattr(gate_parallel, "run_gate", self.run_gate)
        monkeypatch.setattr(gate_parallel, "passed_at_current_stamp", lambda cfg, wl, q, name: name in self.stamped)
        monkeypatch.setattr(gate_parallel, "_reused", lambda cfg, name, q, wl: GateResult(name, "pass", {"reused": True}))

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
    results = run_gates_parallel(CONFIG, names, 6, WORKLOAD)
    assert [r.name for r in results] == names
    assert fake.started[:3] == ["aux_unit", "aux_correctness", "prims_tests"]
    assert set(fake.started[3:5]) == {"aux_multi_connection", "aux_cross_accel"}
    assert fake.started[5] == "aux_bench_sweep"
    assert all(r.status == "pass" for r in results)


def test_gates_in_no_group_run_first_in_the_order_given(monkeypatch):
    fake = Fake(monkeypatch)
    run_gates_parallel(CONFIG, ["aux_unit", "custom_b", "custom_a"], 6, WORKLOAD)
    assert fake.started == ["custom_b", "custom_a", "aux_unit"]


def test_the_parallel_group_runs_at_once_each_on_its_own_instance(monkeypatch):
    barrier = threading.Barrier(3, timeout=10)  # never opens if the gates ran one after the other
    fake = Fake(monkeypatch)
    inner = fake.run_gate

    def meet(config, name, *args, **kwargs):
        barrier.wait()
        return inner(config, name, *args, **kwargs)

    monkeypatch.setattr(gate_parallel, "run_gate", meet)
    results = run_gates_parallel(
        CONFIG, ["aux_multi_connection", "aux_cross_accel", "aux_cost_model_calibrate"], 6, WORKLOAD
    )
    assert all(r.status == "pass" for r in results)
    instances = {name: env["ACCEL_ENGINE_INSTANCE"] for name, env in fake.envs.items()}
    assert instances == {
        "aux_multi_connection": "w3-eng-aux_multi_connection",
        "aux_cross_accel": "w3-eng-aux_cross_accel",
        "aux_cost_model_calibrate": "w3-eng-aux_cost_model_calibrate",
    }


def test_serial_gates_share_the_default_instance(monkeypatch):
    fake = Fake(monkeypatch)
    run_gates_parallel(CONFIG, ["aux_unit", "aux_correctness"], 6, WORKLOAD)
    assert fake.envs == {"aux_unit": None, "aux_correctness": None}


def test_a_pinned_state_dir_is_split_per_parallel_gate(monkeypatch):
    monkeypatch.setenv("ACCEL_ENGINE_STATE_DIR", "/state/base")
    fake = Fake(monkeypatch)
    run_gates_parallel(CONFIG, ["aux_multi_connection", "aux_cross_accel"], 6, WORKLOAD)
    assert fake.envs["aux_cross_accel"]["ACCEL_ENGINE_STATE_DIR"] == "/state/base-aux_cross_accel"


def test_a_failing_cheap_gate_cancels_everything_after_it(monkeypatch):
    fake = Fake(monkeypatch, fail={"aux_correctness"})
    names = ["aux_unit", "aux_correctness", "prims_tests", "aux_cross_accel", "aux_bench_sweep"]
    results = run_gates_parallel(CONFIG, names, 6, WORKLOAD)
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

    monkeypatch.setattr(gate_parallel, "run_gate", slow_sibling)
    started = time.monotonic()
    results = run_gates_parallel(CONFIG, ["aux_multi_connection", "aux_cross_accel", "aux_bench_sweep"], 6, WORKLOAD)
    assert time.monotonic() - started < 20
    assert statuses(results)["aux_cross_accel"] == "fail"
    assert statuses(results)["aux_bench_sweep"] == "cancelled"


def test_the_bench_gate_runs_alone(monkeypatch):
    fake = Fake(monkeypatch, hold=0.05)
    run_gates_parallel(CONFIG, ["aux_bench_sweep", "aux_unit", "aux_cross_accel", "aux_multi_connection"], 6, WORKLOAD)
    assert fake.max_running_when["aux_bench_sweep"] == 1
    assert fake.max_running_when["aux_unit"] == 1


def test_without_fail_fast_every_gate_runs(monkeypatch):
    fake = Fake(monkeypatch, fail={"aux_unit"})
    results = run_gates_parallel(CONFIG, ["aux_unit", "aux_correctness"], 6, WORKLOAD, fail_fast=False)
    assert statuses(results) == {"aux_unit": "fail", "aux_correctness": "pass"}
    assert fake.started == ["aux_unit", "aux_correctness"]


def test_a_skip_a_gate_may_not_make_counts_as_a_failure(monkeypatch):
    fake = Fake(monkeypatch)
    monkeypatch.setattr(gate_parallel, "run_gate", lambda cfg, name, *a, **k: GateResult(name, "skip", {}))
    results = run_gates_parallel(CONFIG, ["aux_unit", "aux_correctness"], 6, WORKLOAD)
    assert statuses(results) == {"aux_unit": "skip", "aux_correctness": "cancelled"}
    assert fake.started == []


def test_reuse_skips_gates_that_passed_on_this_code(monkeypatch):
    fake = Fake(monkeypatch, stamped={"aux_unit"})
    results = run_gates_parallel(CONFIG, ["aux_unit", "aux_correctness"], 6, WORKLOAD, reuse=True)
    assert fake.started == ["aux_correctness"]
    assert results[0].data["reused"] is True


def test_reuse_is_off_by_default(monkeypatch):
    fake = Fake(monkeypatch, stamped={"aux_unit"})
    run_gates_parallel(CONFIG, ["aux_unit"], 6, WORKLOAD)
    assert fake.started == ["aux_unit"]


def test_jobs_bounds_the_parallel_group(monkeypatch):
    fake = Fake(monkeypatch, hold=0.1)
    run_gates_parallel(
        CONFIG, ["aux_multi_connection", "aux_cross_accel", "aux_cost_model_calibrate"], 6, WORKLOAD, jobs=1
    )
    assert max(fake.max_running_when.values()) == 1
