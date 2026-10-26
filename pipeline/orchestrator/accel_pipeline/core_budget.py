"""CPU budgets for concurrent pipeline workers on one host.

The host's CPUs split into one disjoint set per worker plus a reserved measurement set that only
benchmarks (measurement_lease) use, itself cut into slots so that several can measure at once. A worker
claims its set with a host-wide flock slot, pins its own process to it, and exports the set and the job
caps derived from it: every child (agent, builds, gates, engine instances) inherits both the affinity
and the caps.
"""

from __future__ import annotations

import os
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO

from accel_pipeline.config import ConcurrencyConfig
from accel_pipeline.host_lock import try_lock

POOL_ENV = "ACCEL_CPU_POOL"
CORE_SET_ENV = "ACCEL_CORE_SET"
MEASURE_SET_ENV = "ACCEL_MEASURE_SET"
MEASURE_SLOTS_ENV = "ACCEL_MEASURE_SLOTS"
WORKERS_ENV = "ACCEL_CONCURRENT_WORKERS"
MEASURE_CORES_ENV = "ACCEL_MEASURE_CORES"
LOCK_DIR_ENV = "ACCEL_HOST_LOCK_DIR"
MEASURE_LEASE_ENV = "ACCEL_MEASURE_LEASE"
MEASURE_WAIT_ENV = "ACCEL_MEASURE_WAIT_SEC"
MAX_AUTO_MEASURE_CORES = 8

_slot_handles: list[BinaryIO] = []  # held for the process lifetime: the slot frees when it exits


def parse_cpus(spec: str) -> frozenset[int]:
    cpus: set[int] = set()
    for part in spec.split(","):
        lo, _, hi = part.strip().partition("-")
        if lo:
            cpus.update(range(int(lo), int(hi or lo) + 1))
    return frozenset(cpus)


def format_cpus(cpus: frozenset[int] | set[int]) -> str:
    runs: list[list[int]] = []
    for cpu in sorted(cpus):
        if runs and cpu == runs[-1][1] + 1:
            runs[-1][1] = cpu
        else:
            runs.append([cpu, cpu])
    return ",".join(str(lo) if lo == hi else f"{lo}-{hi}" for lo, hi in runs)


def host_lock_dir(configured: str = "") -> Path:
    chosen = os.environ.get(LOCK_DIR_ENV) or configured
    if chosen:
        return Path(chosen)
    base = Path("/dev/shm") if Path("/dev/shm").is_dir() else Path(tempfile.gettempdir())
    return base / f"accel-pipeline-{os.getuid()}"


def cpu_pool() -> frozenset[int]:
    """The CPUs this host's pipelines share: what the first process of the run was allowed (an
    external cpuset or taskset narrows it), kept in the environment for its children."""
    if os.environ.get(POOL_ENV):
        return parse_cpus(os.environ[POOL_ENV])
    return frozenset(os.sched_getaffinity(0))


def core_groups(pool: frozenset[int]) -> list[frozenset[int]]:
    """The pool's physical cores, each as its hardware threads (a CPU alone when the topology is unknown), so a
    budget never splits a core between two workers: hyperthread siblings slow each other down."""
    groups: dict[frozenset[int], None] = {}
    for cpu in sorted(pool):
        siblings = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
        try:
            group = parse_cpus(siblings.read_text()) & pool
        except (OSError, ValueError):
            group = frozenset()
        groups.setdefault(group or frozenset({cpu}), None)
    return sorted(groups, key=min)


def auto_measure_cores(pool_cores: int, workers: int) -> int:
    if workers <= 1:
        return 0
    return max(1, min(MAX_AUTO_MEASURE_CORES, max(2, pool_cores // 5), pool_cores // 2))


def auto_measure_slots(measure_cores: int) -> int:
    return max(1, measure_cores)


def partition(
    cores: list[frozenset[int]], workers: int, measure_cores: int
) -> tuple[list[frozenset[int]], frozenset[int]]:
    """(one disjoint CPU set per worker, the measurement set) of a list of physical cores: the measurement
    set is the last `measure_cores` cores, the rest is cut into near-equal contiguous runs."""
    measure_cores = max(0, min(measure_cores, len(cores) - 1))
    rest = cores[: len(cores) - measure_cores]
    measure = frozenset().union(*cores[len(rest):])
    workers = max(1, min(workers, len(rest)))
    base, extra = divmod(len(rest), workers)
    sets, start = [], 0
    for index in range(workers):
        size = base + (index < extra)
        sets.append(frozenset().union(*rest[start:start + size]))
        start += size
    return sets, measure


@dataclass(frozen=True)
class CoreBudget:
    cpus: frozenset[int]
    measure: frozenset[int]
    pool: frozenset[int]
    slot: int | None = None
    measure_slots: int = 1

    @property
    def jobs(self) -> int:
        return max(1, len(self.cpus))


def _claim_slot(lock_dir: Path, workers: int) -> int:
    for slot in range(workers):
        if (handle := try_lock(lock_dir / "slots" / f"slot-{slot}.lock")) is not None:
            _slot_handles.append(handle)
            return slot
    slot = os.getpid() % workers
    print(f"[pipeline] all {workers} core slots are taken: sharing slot {slot}", file=sys.stderr, flush=True)
    return slot


def resolve(cfg: ConcurrencyConfig) -> CoreBudget:
    """This worker's budget: a free slot of the host's partition (workers > 1), else the whole pool
    less the measurement set."""
    pool = cpu_pool()
    workers = int(os.environ.get(WORKERS_ENV) or cfg.workers)
    measure_cores = os.environ.get(MEASURE_CORES_ENV)
    measure_cores = int(measure_cores) if measure_cores else cfg.measure_cores
    cores = core_groups(pool)
    if measure_cores is None:
        measure_cores = auto_measure_cores(len(cores), workers)
    sets, measure = partition(cores, workers, measure_cores)
    measure_slots = int(os.environ.get(MEASURE_SLOTS_ENV) or 0) or cfg.measure_slots or auto_measure_slots(len(core_groups(measure)))
    if len(sets) == 1:
        return CoreBudget(sets[0], measure, pool, None, measure_slots)
    slot = _claim_slot(host_lock_dir(cfg.host_lock_dir), len(sets))
    return CoreBudget(sets[slot], measure, pool, slot, measure_slots)


def activate(cfg: ConcurrencyConfig) -> CoreBudget:
    """Pin this process to its budget and export it (set, measurement set, pool, build/ctest job caps)
    so that every child inherits it. A process whose ancestor already activated keeps that budget."""
    if os.environ.get(CORE_SET_ENV):
        budget = CoreBudget(
            parse_cpus(os.environ[CORE_SET_ENV]), parse_cpus(os.environ.get(MEASURE_SET_ENV, "")), cpu_pool(),
            measure_slots=int(os.environ.get(MEASURE_SLOTS_ENV) or 1),
        )
    else:
        budget = resolve(cfg)
    os.sched_setaffinity(0, budget.cpus)
    exported = {
        POOL_ENV: format_cpus(budget.pool),
        CORE_SET_ENV: format_cpus(budget.cpus),
        MEASURE_SET_ENV: format_cpus(budget.measure),
        MEASURE_SLOTS_ENV: str(budget.measure_slots),
        LOCK_DIR_ENV: str(host_lock_dir(cfg.host_lock_dir)),
    }
    os.environ.update(exported)
    # The measurement lease's settings; an explicit environment value wins.
    os.environ.setdefault(MEASURE_LEASE_ENV, "1" if cfg.measurement_lease else "0")
    os.environ.setdefault(MEASURE_WAIT_ENV, str(cfg.measurement_wait_sec))
    for name in ("ACCEL_BUILD_JOBS", "ACCEL_CTEST_JOBS", "CMAKE_BUILD_PARALLEL_LEVEL"):
        os.environ.setdefault(name, str(budget.jobs))
    return budget


def build_jobs() -> int:
    """Parallel compile jobs a build in this process may use."""
    return max(1, int(os.environ.get("ACCEL_BUILD_JOBS") or len(os.sched_getaffinity(0))))


def jobs_per_gate(parallel_gates: int) -> int:
    """Build/ctest job cap of each of `parallel_gates` gates running at once inside this worker's set."""
    return max(1, len(os.sched_getaffinity(0)) // max(1, parallel_gates))
