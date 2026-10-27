"""CPU budgets, the host-wide fair lock and the measurement lease (concurrent pipelines on one host)."""

from __future__ import annotations

import os
import re
import subprocess
import sys
import textwrap
import threading
import time
from pathlib import Path

import pytest

from accel_pipeline import repair

from accel_pipeline.pack import core_budget, measurement_lease

from accel_pipeline.config import ConcurrencyConfig
from accel_pipeline.pack.host_lock import FairLock, LockTimeout
from accel_pipeline.pack.shared_file_lease import SharedFileLease

PACKAGE_ROOT = str(Path(core_budget.__file__).resolve().parents[2])
CPUS = sorted(os.sched_getaffinity(0))
needs_two_cpus = pytest.mark.skipif(len(CPUS) < 2, reason="needs two CPUs")


@pytest.fixture
def host_dir(tmp_path, monkeypatch):
    monkeypatch.setenv(core_budget.LOCK_DIR_ENV, str(tmp_path / "host"))
    monkeypatch.delenv(measurement_lease.ENABLED_ENV, raising=False)
    return tmp_path / "host"


def spawn(code: str, *args: str, env: dict | None = None, **kw) -> subprocess.Popen:
    return subprocess.Popen(
        [sys.executable, "-c", textwrap.dedent(code), *args], text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        env={**os.environ, "PYTHONPATH": PACKAGE_ROOT, **(env or {})}, **kw,
    )


# --- core partitioning ---


def plain(n: int) -> list[frozenset[int]]:
    return [frozenset({cpu}) for cpu in range(n)]


def test_partition_is_disjoint_and_measurement_takes_the_top():
    sets, measure = core_budget.partition(plain(40), workers=4, measure_cores=8)
    assert measure == frozenset(range(32, 40))
    assert [len(s) for s in sets] == [8, 8, 8, 8]
    assert frozenset().union(*sets) == frozenset(range(32))


def test_partition_never_splits_a_physical_core():
    cores = [frozenset({i, i + 8}) for i in range(8)]  # hyperthread pairs
    sets, measure = core_budget.partition(cores, workers=3, measure_cores=2)
    assert measure == {6, 7, 14, 15}
    assert sorted(len(s) for s in sets) == [4, 4, 4]
    assert all(s == {c for c in range(16) if c % 8 in {x % 8 for x in s}} for s in sets)


def test_partition_spreads_the_remainder_and_never_starves_a_worker():
    sets, measure = core_budget.partition(plain(10), workers=3, measure_cores=1)
    assert sorted(len(s) for s in sets) == [3, 3, 3] and len(measure) == 1
    sets, _ = core_budget.partition(plain(3), workers=8, measure_cores=0)
    assert len(sets) == 3 and all(sets)


def test_core_groups_follow_the_sysfs_topology():
    groups = core_budget.core_groups(frozenset(os.sched_getaffinity(0)))
    assert frozenset().union(*groups) == frozenset(os.sched_getaffinity(0))
    assert sum(len(g) for g in groups) == len(os.sched_getaffinity(0))


def test_workers_claim_distinct_slots_host_wide(host_dir, monkeypatch):
    monkeypatch.setenv(core_budget.POOL_ENV, "0-15")
    monkeypatch.setattr(core_budget, "_slot_handles", [])
    cfg = ConcurrencyConfig(workers=4, measure_cores=4)
    first, second = core_budget.resolve(cfg), core_budget.resolve(cfg)
    assert {first.slot, second.slot} == {0, 1}
    assert not first.cpus & second.cpus and not (first.cpus | second.cpus) & first.measure
    assert first.measure == frozenset().union(*core_budget.core_groups(frozenset(range(16)))[-4:])


def test_a_dead_workers_slot_is_free_again(host_dir):
    code = f"""
        import sys
        from accel_pipeline.pack import core_budget
        from accel_pipeline.config import ConcurrencyConfig
        b = core_budget.resolve(ConcurrencyConfig(workers=2, measure_cores=0))
        print(b.slot, flush=True)
        sys.stdin.read()
    """
    env = {core_budget.POOL_ENV: "0-7", core_budget.LOCK_DIR_ENV: str(host_dir)}
    holder = spawn(code, env=env, stdin=subprocess.PIPE)
    assert holder.stdout.readline().strip() == "0"
    holder.kill()
    holder.wait()
    out = spawn(code, env=env, stdin=subprocess.DEVNULL).communicate()[0]
    assert out.strip() == "0"


@needs_two_cpus
def test_activate_pins_the_process_and_derives_job_caps(host_dir):
    code = """
        import os
        from accel_pipeline.pack import core_budget
        from accel_pipeline.config import ConcurrencyConfig
        from accel_pipeline.gates import build_jobs
        core_budget.activate(ConcurrencyConfig(workers=1, measure_cores=1))
        print(sorted(os.sched_getaffinity(0)), os.environ["ACCEL_BUILD_JOBS"], os.environ["ACCEL_CTEST_JOBS"],
              os.environ["CMAKE_BUILD_PARALLEL_LEVEL"], os.environ["ACCEL_MEASURE_SET"], build_jobs())
    """
    out = spawn(code).communicate()[0].strip()
    measure = core_budget.core_groups(frozenset(CPUS))[-1]
    worker = [c for c in CPUS if c not in measure]
    assert out == f"{worker} {len(worker)} {len(worker)} {len(worker)} {core_budget.format_cpus(measure)} {len(worker)}"


# --- the fair lock ---

def test_the_lock_serves_waiters_in_arrival_order(tmp_path):
    lock = FairLock(tmp_path, "t")
    held = lock.acquire(timeout_sec=1)
    order: list[int] = []

    def waiter(index: int) -> None:
        handle = lock.acquire(timeout_sec=20)
        order.append(index)
        handle.close()

    threads = []
    for index in range(4):
        threads.append(threading.Thread(target=waiter, args=(index,)))
        threads[-1].start()
        time.sleep(0.15)
    held.close()
    for thread in threads:
        thread.join(10)
    assert order == [0, 1, 2, 3]


def test_the_lock_times_out_and_leaves_no_ticket(tmp_path):
    lock = FairLock(tmp_path, "t")
    held = lock.acquire(timeout_sec=1)
    with pytest.raises(LockTimeout):
        lock.acquire(timeout_sec=0.3)
    assert lock.queue_depth() == 0
    held.close()


def test_a_crashed_holder_releases_the_lock(tmp_path):
    code = """
        import sys
        from pathlib import Path
        from accel_pipeline.pack.host_lock import FairLock
        held = FairLock(Path(sys.argv[1]), "t").acquire(timeout_sec=5)
        print("held", flush=True)
        sys.stdin.read()
    """
    holder = spawn(code, str(tmp_path), stdin=subprocess.PIPE)
    assert holder.stdout.readline().strip() == "held"
    lock = FairLock(tmp_path, "t")
    assert lock.is_held()
    holder.kill()
    holder.wait()
    lock.acquire(timeout_sec=2).close()


def test_shared_lease_waits_are_bounded_and_say_who_holds_it(tmp_path):
    holder = SharedFileLease(tmp_path)
    holder.acquire("q2", reason="editing CMakeLists")
    with pytest.raises(TimeoutError, match=r"held by 'q2' for \d+s \(editing CMakeLists\), 0 waiting"):
        SharedFileLease(tmp_path).acquire("q5", timeout_sec=0.3)
    holder.release("q2")


# --- the measurement lease ---

def window_script(hold_sec: float) -> str:
    return f"""
        import sys, time
        from accel_pipeline.pack.measurement_lease import measurement_window
        with measurement_window(sys.argv[1], "test") as w:
            print("in", w.waited_sec, flush=True)
            time.sleep({hold_sec})
    """


def test_measurement_lease_spans_trees_through_the_host_dir(host_dir, tmp_path):
    other_tree = tmp_path / "other-tree"
    other_tree.mkdir()
    holder = spawn(window_script(3), "tree-a", cwd=other_tree)
    assert holder.stdout.readline().startswith("in")
    (info,) = measurement_lease.holders()
    assert info["holder"] == "tree-a" and info["reason"] == "test"
    assert measurement_lease.held_by_other("tree-b") and not measurement_lease.held_by_other("tree-a")
    started = time.monotonic()
    with measurement_lease.measurement_window("tree-b", "second") as window:
        assert window.waited_sec > 1 and time.monotonic() - started > 1
    holder.wait()
    assert measurement_lease.holders() == []


def test_a_crashed_measurement_holder_frees_the_lease(host_dir):
    holder = spawn(window_script(60), "dead")
    holder.stdout.readline()
    holder.kill()
    holder.wait()
    with measurement_lease.measurement_window("next", "t") as window:
        assert window.waited_sec < 1


def test_measurement_waits_are_bounded_and_beat(host_dir, monkeypatch):
    monkeypatch.setenv(measurement_lease.WAIT_ENV, "1.5")
    monkeypatch.setattr("accel_pipeline.pack.host_lock.BEAT_SEC", 0.2)
    holder = spawn(window_script(5), "slow")
    holder.stdout.readline()
    beats = []
    with pytest.raises(LockTimeout, match=r"measurement lease not acquired within 2s \(held by slow"):
        with measurement_lease.measurement_window("late", "t", beat=lambda: beats.append(1)):
            pass
    assert beats
    holder.kill()
    holder.wait()


def test_slots_let_benchmarks_measure_at_once_on_disjoint_cpus(host_dir, monkeypatch):
    monkeypatch.setenv(core_budget.MEASURE_SET_ENV, "10-15")
    monkeypatch.setenv(core_budget.MEASURE_SLOTS_ENV, "3")
    code = """
        import sys, time
        from accel_pipeline.pack.measurement_lease import measurement_window
        from accel_pipeline.pack.core_budget import format_cpus
        with measurement_window(sys.argv[1], "t") as w:
            print(format_cpus(w.cpus), w.waited_sec, flush=True)
            time.sleep(2)
    """
    first = [spawn(code, name) for name in ("a", "b", "c")]
    cpus = [p.stdout.readline().split()[0] for p in first]
    assert sorted(cpus) == ["10-11", "12-13", "14-15"]
    fourth = time.monotonic()
    with measurement_lease.measurement_window("d", "t") as window:
        assert window.waited_sec > 0.5 and time.monotonic() - fourth > 0.5
        assert window.cpus in ({10, 11}, {12, 13}, {14, 15})
    for p in first:
        p.wait()


@needs_two_cpus
def test_the_wrapper_pins_the_command_and_holds_the_lease_until_it_exits(host_dir):
    measure = CPUS[-1]
    env = {**os.environ, "PYTHONPATH": PACKAGE_ROOT, core_budget.MEASURE_SET_ENV: str(measure)}
    code = "import os, sys, time; print(sorted(os.sched_getaffinity(0)), os.environ['ACCEL_MEASURE_THREADS'], flush=True); time.sleep(1.5)"
    proc = subprocess.Popen(
        [sys.executable, "-m", "accel_pipeline.pack.measurement_lease", "--holder", "h", "--reason", "bench", "--", sys.executable, "-c", code],
        env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    assert proc.stdout.readline().strip() == f"[{measure}] 1"
    assert [i["holder"] for i in measurement_lease.holders()] == ["h"]  # still held by the exec'd command
    proc.wait()
    assert measurement_lease.holders() == []
    assert re.search(r"waited 0\.\d s for the measurement lease; pinned to cores", proc.stderr.read())


# --- triage sees the lease ---

def test_a_timeout_while_another_worker_measures_is_infra(host_dir, monkeypatch):
    monkeypatch.setenv("ACCEL_INSTANCE_TAG", "me")
    text = "aux_bench did not finish within 900s"
    assert repair.classify("aux_bench_sweep", {"log_tail": text}, {}, None, query=3, load_factor=1e9) == ("code", None)
    holder = spawn(window_script(3), "other")
    holder.stdout.readline()
    assert repair.classify("aux_bench_sweep", {"log_tail": text}, {}, None, query=3, load_factor=1e9) == ("infra", "timeout_under_load")
    holder.kill()
    holder.wait()


def test_not_getting_the_lease_is_infra(host_dir):
    text = "measurement lease not acquired within 3600s (held by q9): not acquired within 3600s"
    assert repair.classify("aux_bench_sweep", {"log_tail": text}, {}, None, query=3) == ("infra", "measurement_lease_busy")


# --- nothing kills other workers' processes ---

def test_pipeline_code_never_kills_by_name():
    root = Path(__file__).resolve().parents[2]
    offenders = []
    for path in [*root.glob("orchestrator/accel_pipeline/**/*.py"), *root.glob("gates/*.sh"), *(root.parent / "backends").glob("*/host.sh"),
                 *(root.parent / "scripts").glob("*.sh")]:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if re.search(r"\b(pkill|killall)\b", line) and not line.lstrip().startswith("#"):
                offenders.append(f"{path.relative_to(root.parent)}:{number}")
    assert not offenders, offenders
