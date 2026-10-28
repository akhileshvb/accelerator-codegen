"""Many query accelerators at once: the pack runner and what lets its workers share one host and one tree.

- Host resources: one flock-based fair lock (a mutex, or a semaphore of slots) behind the CPU budgets, the
  measurement lease and the perf turn. Every lock lives under the host lock dir, so it spans trees and worktrees,
  and a crashed holder releases it.
- Worker isolation: a build dir, a Codex home and a git worktree per worker; foreign engine trees quarantined.
- The shared-file lease around edits to files every worker shares, and the repo context pack that tells agents
  the layout once.
- The pack runner: `python -m accel_pipeline.pack --pack NAME` fans queries out as one `accel` run per worker;
  `python -m accel_pipeline.pack measure --holder H --reason R -- cmd...` is the gates' measurement wrapper.
"""

from __future__ import annotations

import argparse
import atexit
import fcntl
import fnmatch
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from contextlib import contextmanager
from dataclasses import asdict, dataclass, replace
from datetime import datetime, timezone
from pathlib import Path
from typing import TYPE_CHECKING, Any, BinaryIO, Callable, Iterator

import yaml

from accel_pipeline.agent import ensure_codex_config
from accel_pipeline.config import (
    ConcurrencyConfig, PipelineConfig, find_repo_root, load_model_config, load_pipeline_config, load_repo_dotenv, load_workload,
    pipeline_root, workload_engines,
)
from accel_pipeline.data import instance_tag
from accel_pipeline.layout import owned_path_globs, query_dir, query_id_from_rel
from accel_pipeline.prims import is_prims_path, refresh_prims

if TYPE_CHECKING:
    from accel_pipeline.mcp import ToolContext

# The fair lock

BEAT_SEC = 5.0


class LockTimeout(TimeoutError):
    """The lock was not acquired within the bound."""


def _try_flock(handle: BinaryIO) -> bool:
    try:
        fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        return False
    return True


def try_lock(path: Path) -> BinaryIO | None:
    """The flock-held file at `path`, or None when another process holds it."""
    path.parent.mkdir(parents=True, exist_ok=True)
    handle = path.open("a+b")
    if _try_flock(handle):
        return handle
    handle.close()
    return None


class FairLock:
    """First come, first served flock lock of `slots` holders. Waiters queue as ticket files, each flock-held by
    its waiter, so a waiter that died is seen (its flock is free) and dropped. Closing the returned file releases
    the slot; so does the holder exiting, however it exits."""

    def __init__(self, directory: Path, name: str, slots: int = 1) -> None:
        self.slots = max(1, slots)
        self.paths = [Path(directory) / (f"{name}.lock" if self.slots == 1 else f"{name}.{k}.lock") for k in range(self.slots)]
        self._queue = Path(directory) / f"{name}.queue"

    def held_slots(self) -> list[int]:
        busy = []
        for slot, path in enumerate(self.paths):
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open("a+b") as probe:
                if not _try_flock(probe):
                    busy.append(slot)
        return busy

    def is_held(self) -> bool:
        """Every slot is taken: an acquire would queue."""
        return len(self.held_slots()) == self.slots

    def _live_tickets(self) -> list[Path]:
        live = []
        for ticket in sorted(p for p in self._queue.glob("*") if not p.name.startswith(".")):
            try:
                with ticket.open("rb") as probe:
                    if _try_flock(probe):  # its waiter is gone
                        ticket.unlink(missing_ok=True)
                        continue
            except FileNotFoundError:
                continue
            live.append(ticket)
        return live

    def queue_depth(self) -> int:
        return len(self._live_tickets()) if self._queue.is_dir() else 0

    def acquire(self, **kwargs) -> BinaryIO:
        return self.acquire_slot(**kwargs)[0]

    def acquire_slot(
        self, *, timeout_sec: float, poll_sec: float = 0.1, on_wait: Callable[[float, int], None] | None = None
    ) -> tuple[BinaryIO, int]:
        """(the held slot file, its index). `on_wait(waited_sec, ahead)` runs about every BEAT_SEC while queued.
        Raises LockTimeout past `timeout_sec`."""
        started = last_beat = time.monotonic()
        self._queue.mkdir(parents=True, exist_ok=True)
        pending = self._queue / f".{os.getpid()}-{id(self)}"
        ticket = self._queue / f"{time.time_ns():020d}-{os.getpid()}"
        with pending.open("wb") as mine:
            fcntl.flock(mine.fileno(), fcntl.LOCK_EX)
            pending.rename(ticket)  # the flock follows the inode: the ticket is never visible unlocked
            try:
                while True:
                    tickets = self._live_tickets()
                    if tickets and tickets[0] == ticket:
                        for slot, path in enumerate(self.paths):
                            if (handle := try_lock(path)) is not None:
                                return handle, slot
                    now = time.monotonic()
                    if now - started >= timeout_sec:
                        raise LockTimeout(f"not acquired within {timeout_sec:.0f}s")
                    if on_wait is not None and now - last_beat >= BEAT_SEC:
                        last_beat = now
                        on_wait(now - started, tickets.index(ticket) if ticket in tickets else 0)
                    time.sleep(poll_sec)
            finally:
                ticket.unlink(missing_ok=True)


# CPU budgets
#
# The host's CPUs split into one disjoint set per worker plus a reserved measurement set that only benchmarks use,
# itself cut into slots so that several can measure at once. A worker claims its set with a host-wide flock slot,
# pins its own process to it, and exports the set and the job caps derived from it: every child (agent, builds,
# gates, engine instances) inherits both the affinity and the caps.

POOL_ENV = "ACCEL_CPU_POOL"
CORE_SET_ENV = "ACCEL_CORE_SET"
MEASURE_SET_ENV = "ACCEL_MEASURE_SET"
MEASURE_SLOTS_ENV = "ACCEL_MEASURE_SLOTS"
WORKERS_ENV = "ACCEL_CONCURRENT_WORKERS"
MEASURE_CORES_ENV = "ACCEL_MEASURE_CORES"
LOCK_DIR_ENV = "ACCEL_HOST_LOCK_DIR"
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
    if chosen := os.environ.get(LOCK_DIR_ENV) or configured:
        return Path(chosen)
    base = Path("/dev/shm") if Path("/dev/shm").is_dir() else Path(tempfile.gettempdir())
    return base / f"accel-pipeline-{os.getuid()}"


def cpu_pool() -> frozenset[int]:
    """The CPUs this host's pipelines share: what the first process of the run was allowed (an external cpuset or
    taskset narrows it), kept in the environment for its children."""
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


def partition(cores: list[frozenset[int]], workers: int, measure_cores: int) -> tuple[list[frozenset[int]], frozenset[int]]:
    """(one disjoint CPU set per worker, the measurement set) of a list of physical cores: the measurement set is
    the last `measure_cores` cores, the rest is cut into near-equal contiguous runs."""
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
    """This worker's budget: a free slot of the host's partition (workers > 1), else the whole pool less the
    measurement set."""
    pool = cpu_pool()
    workers = int(os.environ.get(WORKERS_ENV) or cfg.workers)
    measure_cores = os.environ.get(MEASURE_CORES_ENV)
    measure_cores = int(measure_cores) if measure_cores else cfg.measure_cores
    cores = core_groups(pool)
    if measure_cores is None:
        measure_cores = 0 if workers <= 1 else max(1, min(MAX_AUTO_MEASURE_CORES, max(2, len(cores) // 5), len(cores) // 2))
    sets, measure = partition(cores, workers, measure_cores)
    measure_slots = int(os.environ.get(MEASURE_SLOTS_ENV) or 0) or cfg.measure_slots or max(1, len(core_groups(measure)))
    if len(sets) == 1:
        return CoreBudget(sets[0], measure, pool, None, measure_slots)
    slot = _claim_slot(host_lock_dir(cfg.host_lock_dir), len(sets))
    return CoreBudget(sets[slot], measure, pool, slot, measure_slots)


def activate(cfg: ConcurrencyConfig) -> CoreBudget:
    """Pin this process to its budget and export it (set, measurement set, pool, build/ctest job caps) so that
    every child inherits it. A process whose ancestor already activated keeps that budget."""
    if os.environ.get(CORE_SET_ENV):
        budget = CoreBudget(
            parse_cpus(os.environ[CORE_SET_ENV]), parse_cpus(os.environ.get(MEASURE_SET_ENV, "")), cpu_pool(),
            measure_slots=int(os.environ.get(MEASURE_SLOTS_ENV) or 1),
        )
    else:
        budget = resolve(cfg)
    os.sched_setaffinity(0, budget.cpus)
    os.environ.update({
        POOL_ENV: format_cpus(budget.pool),
        CORE_SET_ENV: format_cpus(budget.cpus),
        MEASURE_SET_ENV: format_cpus(budget.measure),
        MEASURE_SLOTS_ENV: str(budget.measure_slots),
        LOCK_DIR_ENV: str(host_lock_dir(cfg.host_lock_dir)),
    })
    for name in ("ACCEL_BUILD_JOBS", "ACCEL_CTEST_JOBS", "CMAKE_BUILD_PARALLEL_LEVEL"):
        os.environ.setdefault(name, str(budget.jobs))
    return budget


# The measurement lease
#
# Benchmarks measure on the reserved measurement CPUs, one per slot:
#
#     with measurement_window(holder, reason) as window:
#         subprocess.run(window.argv(cmd), env={**os.environ, **window.env}, timeout=...)
#
# Waiting is first come first served, bounded, and beats the agent heartbeat.

WAIT_ENV = "ACCEL_MEASURE_WAIT_SEC"
ENABLED_ENV = "ACCEL_MEASURE_LEASE"
HEARTBEAT_ENV = "ACCEL_HEARTBEAT_FILE"
DEFAULT_WAIT_SEC = 3600.0
BUSY_EXIT = 75
WAITED = re.compile(r"waited ([0-9.]+) s for the measurement lease")
BUSY = re.compile(r"measurement lease not acquired")


def _settings() -> tuple[bool, float, frozenset[int], int]:
    """(enabled, wait bound, measurement CPUs, slots), as `activate` exported them."""
    return (
        os.environ.get(ENABLED_ENV, "1") != "0",
        float(os.environ.get(WAIT_ENV) or DEFAULT_WAIT_SEC),
        parse_cpus(os.environ.get(MEASURE_SET_ENV, "")),
        int(os.environ.get(MEASURE_SLOTS_ENV) or 1),
    )


def _measurement_lock() -> FairLock:
    return FairLock(host_lock_dir(), "measurement", _settings()[3])


def _holder_file(slot: int) -> Path:
    return host_lock_dir() / f"measurement.holder.{slot}.json"


def holders() -> list[dict]:
    """Who holds a measurement slot right now (none: the lease is idle)."""
    out = []
    for slot in _measurement_lock().held_slots():
        try:
            out.append(json.loads(_holder_file(slot).read_text(encoding="utf-8")))
        except (OSError, ValueError):
            out.append({"holder": "?"})
    return out


def held_by_other(me: str) -> bool:
    return any(info.get("holder") != me for info in holders())


def waited_seconds(text: str) -> float | None:
    """Seconds a run waited for the lease, from the line the wrapper prints (None: it never queued)."""
    match = WAITED.search(text)
    return float(match.group(1)) if match else None


def lease_note(text: str) -> dict | None:
    """What the benchmark output says of its queueing: {"waited_sec": total, "runs": n}, None if none."""
    waits = [float(w) for w in WAITED.findall(text)]
    return {"waited_sec": round(sum(waits), 1), "runs": len(waits)} if waits else None


def _describe_holders() -> str:
    held = [
        f"{info.get('holder')} ({info.get('reason') or 'no reason'})"
        + (f" for {time.time() - info['since']:.0f}s" if info.get("since") else "")
        for info in holders()
    ]
    return f"held by {', '.join(held)}" if held else "free"


@dataclass(frozen=True)
class Window:
    waited_sec: float = 0.0
    cpus: frozenset[int] = frozenset()
    leased: bool = False

    @property
    def env(self) -> dict[str, str]:
        if not self.cpus:
            return {}
        return {"ACCEL_MEASURE_CPUS": format_cpus(self.cpus), "ACCEL_MEASURE_THREADS": str(len(self.cpus))}

    def argv(self, cmd: list[str]) -> list[str]:
        """`cmd` pinned to the measurement CPUs (accelerator and baseline share the one budget)."""
        if not self.cpus:
            return cmd
        if shutil.which("taskset") is None:
            print("[measurement-lease] taskset missing: running unpinned", file=sys.stderr, flush=True)
            return cmd
        return ["taskset", "-c", format_cpus(self.cpus), *cmd]

    def line(self) -> str:
        if not self.leased:
            return "[measurement-lease] off"
        pinned = f"; pinned to cores {format_cpus(self.cpus)}" if self.cpus else ""
        return f"[measurement-lease] waited {self.waited_sec:.1f} s for the measurement lease{pinned}"


def _acquire_window(holder: str, reason: str, beat: Callable[[], None] | None = None) -> tuple[BinaryIO | None, Window]:
    """Queue for a measurement slot: (the held slot file, the window); no file when the lease is off."""
    enabled, wait_sec, measure, slots = _settings()
    if not enabled:
        return None, Window(cpus=measure)

    def on_wait(waited: float, ahead: int) -> None:
        print(f"[measurement-lease] queued {waited:.0f}s, {ahead} ahead; {_describe_holders()}", file=sys.stderr, flush=True)
        if path := os.environ.get(HEARTBEAT_ENV):
            Path(path).touch()
        if beat is not None:
            beat()

    started = time.monotonic()
    try:
        handle, slot = _measurement_lock().acquire_slot(timeout_sec=wait_sec, on_wait=on_wait)
    except LockTimeout as exc:
        raise LockTimeout(f"measurement lease not acquired within {wait_sec:.0f}s ({_describe_holders()}): {exc}") from None
    _holder_file(slot).write_text(
        json.dumps({"holder": holder, "reason": reason, "pid": os.getpid(), "since": time.time()}), encoding="utf-8"
    )
    # A slot's share of the measurement cores: a measurement runs the accelerator and the baseline on that budget.
    cpus = partition(core_groups(measure), slots, 0)[0][slot] if measure else frozenset()
    window = Window(time.monotonic() - started, cpus, True)
    print(window.line(), file=sys.stderr, flush=True)
    return handle, window


@contextmanager
def measurement_window(holder: str, reason: str, *, beat: Callable[[], None] | None = None) -> Iterator[Window]:
    """Hold the lease for the block; yields how long it queued and the CPUs to pin the measurement to."""
    handle, window = _acquire_window(holder, reason, beat)
    try:
        yield window
    finally:
        if handle is not None:
            handle.close()


def holder_name() -> str:
    return os.environ.get("ACCEL_INSTANCE_TAG") or os.environ.get("ACCEL_PIPELINE_WORKER_ID") or "w0"


def run_measured(cmd: list[str], *, reason: str, timeout: float | None = None, **kwargs) -> subprocess.CompletedProcess:
    """subprocess.run of a benchmark under the lease, pinned to the measurement CPUs. The queueing is outside
    `timeout`; the run's stderr ends with the line that reports it (see waited_seconds)."""
    with measurement_window(holder_name(), reason) as window:
        env = {**(kwargs.pop("env", None) or os.environ), **window.env}
        proc = subprocess.run(window.argv(cmd), env=env, timeout=timeout, **kwargs)
    proc.stderr = f"{proc.stderr or ''}\n{window.line()}\n"
    return proc


def noise_warning(load_factor: float = 1.5) -> str | None:
    """Why a benchmark run now without the lease is noisy, or None when the host looks idle."""
    reasons = []
    load, cpus = os.getloadavg()[0], len(os.sched_getaffinity(0))
    if load > load_factor * (os.cpu_count() or cpus):
        reasons.append(f"load average {load:.0f} on {os.cpu_count()} cores")
    if held := holders():
        reasons.append(f"measurements are running ({', '.join(str(i.get('holder')) for i in held)})")
    return f"noisy: {'; '.join(reasons)} (not measured under the measurement lease)" if reasons else None


def measure_main(argv: list[str]) -> int:
    """Queue, pin this process to the slot's CPUs and exec the command still holding the lease: it ends with the
    process."""
    parser = argparse.ArgumentParser(prog="pack measure", description="Run a command holding the measurement lease, pinned to the measurement CPUs.")
    parser.add_argument("--holder", required=True)
    parser.add_argument("--reason", default="")
    parser.add_argument("cmd", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    try:
        handle, window = _acquire_window(args.holder, args.reason)
    except LockTimeout as exc:
        print(exc, file=sys.stderr)
        return BUSY_EXIT
    if handle is not None:
        os.set_inheritable(handle.fileno(), True)
    if window.cpus:
        try:
            os.sched_setaffinity(0, window.cpus)
            os.environ.update(window.env)
        except OSError as exc:
            print(f"[measurement-lease] cannot pin to {format_cpus(window.cpus)} ({exc}): running unpinned", file=sys.stderr, flush=True)
    os.execvp(cmd[0], cmd)


# Worker isolation: one worker per query x engine, keyed by its id

WORKTREE_MODES = {"worktree", "worktrees", "git_worktree"}


def worker_id_for_query(query: int, workload_name: str = "", engine: str = "") -> str:
    base = re.sub(r"[^a-zA-Z0-9_.-]+", "_", workload_name.strip()) if workload_name else ""
    return "_".join(part for part in (base or f"q{query}", engine) if part)


def worker_id_from_env(default: str = "local") -> str:
    for key in ("ACCEL_PIPELINE_WORKER_ID", "ACCEL_PIPELINE_QUERY"):
        if (raw := os.environ.get(key, "").strip()):
            return raw
    return default


def default_build_dir_for_worker(repo_root: Path, worker_id: str) -> Path:
    """On the primary repo, so worktree checkouts do not duplicate multi-GB builds."""
    return Path(os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or repo_root) / f"build-release-{worker_id}"


def prepare_codex_home(*, repo_root: Path, worker_id: str, base_codex_home: Path | None = None) -> Path:
    """A per-worker CODEX_HOME whose config.toml links to the base one."""
    base = base_codex_home or (Path(repo_root) / "pipeline" / ".codex")
    dest = Path(os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or repo_root) / "pipeline" / ".codex" / "workers" / worker_id
    dest.mkdir(parents=True, exist_ok=True)
    src_cfg = ensure_codex_config(base / "config.toml")
    dst_cfg = dest / "config.toml"
    if not dst_cfg.exists():
        try:
            dst_cfg.symlink_to(src_cfg.resolve())
        except OSError:
            shutil.copy2(src_cfg, dst_cfg)
    return dest


def _rewrite_build_dir_patterns(patterns: list[str], build_dir_name: str) -> list[str]:
    """Allow both the plain `build-release` commands and this worker's build dir name."""
    out = list(dict.fromkeys(
        variant
        for pat in patterns
        for variant in (pat, pat.replace("build-release", build_dir_name) if "build-release" in pat else pat)
    ))
    name = re.escape(build_dir_name)
    generic = [
        rf"^cmake -S \. -B {name}( .*)?$",
        rf"^cmake --build {name}( .*)?$",
        rf"^ctest --test-dir {name}( .*)?$",
        rf"^(\./)?{name}/(test|benchmark|calibrate)_[A-Za-z0-9_]+( .*)?$",
    ]
    return out + [g for g in generic if g not in out]


def apply_worker_isolation(
    pipeline_config: PipelineConfig,
    *,
    query: int,
    workload_name: str = "",
    worker_id: str | None = None,
    build_dir: Path | str | None = None,
    codex_home: Path | str | None = None,
    set_environ: bool = True,
) -> PipelineConfig:
    """The config scoped to a concurrent pack worker: its build dir, Codex home and repo root."""
    wid = worker_id or worker_id_for_query(query, workload_name, pipeline_config.engine_name)
    env_repo = os.environ.get("ACCEL_PIPELINE_REPO_ROOT")
    repo = Path(env_repo or pipeline_config.repo_root).resolve()
    primary = Path(os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or pipeline_config.repo_root).resolve()

    def on_primary(path: Path | str) -> Path:
        return Path(path) if Path(path).is_absolute() else primary / path

    chosen_build = build_dir or os.environ.get("ACCEL_PIPELINE_BUILD_DIR")
    bdir = on_primary(chosen_build) if chosen_build else default_build_dir_for_worker(primary, wid)
    chosen_home = codex_home or os.environ.get("ACCEL_PIPELINE_CODEX_HOME")
    chome = on_primary(chosen_home) if chosen_home else prepare_codex_home(repo_root=primary, worker_id=wid)

    cfg = replace(
        pipeline_config,
        repo_root=repo,
        build_dir=bdir,
        allowed_commands=_rewrite_build_dir_patterns(list(pipeline_config.allowed_commands or []), bdir.name),
        worker_id=wid,
        codex_home=chome,
    )
    if set_environ:
        os.environ.update({
            "ACCEL_PIPELINE_WORKER_ID": wid,
            "ACCEL_PIPELINE_BUILD_DIR": str(bdir),
            "ACCEL_PIPELINE_CODEX_HOME": str(chome),
            "ACCEL_PIPELINE_QUERY": str(query),
            "ACCEL_PIPELINE_REPO_ROOT": str(repo),
        })
        os.environ.setdefault("ACCEL_PIPELINE_PRIMARY_REPO", str(primary))
        if workload_name:
            os.environ["ACCEL_PIPELINE_WORKLOAD"] = workload_name
    return cfg


def isolation_summary(pipeline_config: PipelineConfig) -> dict[str, Any]:
    return {
        "worker_id": pipeline_config.worker_id or None,
        "build_dir": str(pipeline_config.build_dir),
        "codex_home": str(pipeline_config.codex_home or ""),
        "repo_root": str(pipeline_config.repo_root),
        "primary_repo": os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO") or "",
        "isolation_mode": os.environ.get("ACCEL_PIPELINE_ISOLATION_MODE") or "",
    }


# --- per-worker git worktrees: concurrent opts cannot crosstalk on sources. A worker checks out an isolated tree
# --- under pipeline/worktrees/<worker_id>; artifacts and CODEX_HOME stay on the primary repo.

def using_worktree_isolation() -> bool:
    if (os.environ.get("ACCEL_PIPELINE_ISOLATION_MODE") or "").strip() in WORKTREE_MODES:
        return True
    primary, repo = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO"), os.environ.get("ACCEL_PIPELINE_REPO_ROOT")
    return bool(primary and repo and Path(primary).resolve() != Path(repo).resolve())


def primary_repo_root(repo_root: Path | None = None) -> Path:
    if env := os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO"):
        return Path(env).resolve()
    return Path(repo_root).resolve() if repo_root is not None else find_repo_root().resolve()


def pack_queries_from_env() -> list[int]:
    return [int(part) for part in (os.environ.get("ACCEL_PIPELINE_PACK_QUERIES") or "").split(",") if part.strip().isdigit()]


def _git(primary: Path, *args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["git", *args], cwd=str(primary), capture_output=True, text=True, check=False)


def remove_worker_worktree(primary: Path, worker_id: str) -> None:
    dest = Path(primary) / "pipeline" / "worktrees" / worker_id
    _git(primary, "worktree", "remove", "--force", str(dest))
    shutil.rmtree(dest, ignore_errors=True)
    _git(primary, "branch", "-D", f"pack/{worker_id}")


def _symlink_to_primary(primary: Path, dest: Path, rels: tuple[str, ...]) -> None:
    for rel in rels:
        if not (src := primary / rel).exists():
            continue
        dst = dest / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.is_symlink() or dst.is_file():
            dst.unlink()
        elif dst.is_dir():
            shutil.rmtree(dst)
        dst.symlink_to(src.resolve())


def _seed_generated_sources(primary: Path, dest: Path) -> None:
    """Overlay the primary working tree's sources the worktree needs to compile: `git worktree add` only
    materializes HEAD, while aux modules are often untracked and shared headers dirty on the primary. Uses
    `shutil.copy` so destination mtimes are now: build dirs persist across worktree resets, and preserved source
    mtimes made CMake skip rebuilds and left stale objects."""
    for rel in ("accelerators", "backends", "substrate", "eval/harness", "eval/tests", "eval/bench/tpch/single-threaded", "workloads", "cmake"):
        src = primary / rel
        if src.is_dir():
            for path in src.rglob("*"):
                if path.is_file():
                    (dest / rel / path.relative_to(src)).parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy(path, dest / rel / path.relative_to(src))
        elif src.exists():
            (dest / rel).parent.mkdir(parents=True, exist_ok=True)
            shutil.copy(src, dest / rel)


def ensure_worker_worktree(primary: Path, worker_id: str, workload: dict[str, Any], *, reset: bool = True,
                           pack_queries: list[int] | None = None) -> Path:
    """Create (or reset) an isolated git worktree for `worker_id`: from the primary HEAD, the primary's working
    tree overlaid, each pack peer's best_valid snapshot restored over its possibly mid-edit sources."""
    from accel_pipeline.repair import restore_aux_from_snapshot

    primary = Path(primary).resolve()
    dest = primary / "pipeline" / "worktrees" / worker_id
    dest.parent.mkdir(parents=True, exist_ok=True)
    if reset or not (dest / "CMakeLists.txt").is_file():
        remove_worker_worktree(primary, worker_id)
        proc = _git(primary, "worktree", "add", "-B", f"pack/{worker_id}", str(dest), "HEAD")
        if proc.returncode != 0:
            raise RuntimeError(f"git worktree add failed for {worker_id}:\n{(proc.stdout or '') + (proc.stderr or '')}")
        print(f"[pipeline] worker worktree ready: {dest.relative_to(primary)}", file=sys.stderr, flush=True)
    _seed_generated_sources(primary, dest)
    for q in pack_queries if pack_queries is not None else pack_queries_from_env():
        restore_aux_from_snapshot(dest, int(q), workload)
    refresh_prims(primary, dest)  # mirror the shared primitives library exactly (also reverts any direct edit)
    # Workload YAMLs and pack configs are often untracked, so the config/prompt/gate dirs and the artifacts (digests,
    # checkpoints) are the primary's own.
    _symlink_to_primary(primary, dest, (
        "pipeline/config", "pipeline/prompts", "pipeline/gates", "pipeline/schemas", "pipeline/templates", "pipeline/artifacts",
    ))
    return dest


def sync_owned_to_primary(*, primary: Path, worktree: Path, query: int, benchmark: str = "tpch") -> list[str]:
    """Copy this query's owned sources from the worker worktree back to the primary."""
    primary, worktree = Path(primary).resolve(), Path(worktree).resolve()
    if primary == worktree:
        return []
    copied: list[str] = []
    for src in _owned_files(worktree, query, benchmark):
        rel = str(src.relative_to(worktree))
        other = query_id_from_rel(rel)
        # The shared primitives library reaches the primary only via primitive_publish.
        if is_prims_path(rel) or (other is not None and other != int(query)):
            continue
        (primary / rel).parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, primary / rel)
        copied.append(rel)
    return copied


def _owned_files(root: Path, query: int, benchmark: str) -> list[Path]:
    """Source, config and test files the query owns under `root` (artifacts are not synced)."""
    files: set[Path] = set()
    for pat in owned_path_globs(query, benchmark=benchmark):
        if "artifacts" in pat:
            continue
        base = root / re.split(r"[*?\[]", pat, maxsplit=1)[0].rpartition("/")[0]
        if base.is_dir():
            files.update(p.resolve() for p in base.rglob("*") if p.is_file() and fnmatch.fnmatch(str(p.relative_to(root)), pat))
    return sorted(files)


# --- quarantine: Codex runs with full sandbox bypass for MCP, so prompt denylists are not enough; moving other
# --- engines' trees out of the workspace is the reliable hide.

@dataclass
class QuarantineSession:
    repo_root: Path
    quarantine_root: Path
    moved: list[tuple[str, str]]  # (rel_src, rel_dst under quarantine)
    manifest_path: Path

    def restore(self) -> None:
        for rel_src, rel_dst in reversed(self.moved):
            src, dst = self.repo_root / rel_src, self.quarantine_root / rel_dst
            if not src.exists() and dst.exists():
                src.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(dst), str(src))
        self.manifest_path.unlink(missing_ok=True)
        if self.quarantine_root.is_dir() and not any(self.quarantine_root.rglob("*")):
            shutil.rmtree(self.quarantine_root, ignore_errors=True)


def foreign_trees_for_workload(workload: dict[str, Any]) -> list[str]:
    """Repo-relative trees to hide, never the target engine's: during an accelerator run the query's trees of the
    other engines its workload supports; during an engine_backend run the workload's `agent_hidden_trees`."""
    engine = str(workload.get("engine_name") or "").strip()
    if workload.get("workload_kind") != "engine_backend":
        bench = str(workload.get("benchmark_name") or "tpch")
        query = workload.get("query_number")
        return [
            tree
            for other in workload_engines(workload)
            if other != engine
            for tree in (f"{query_dir(bench, query)}/{other}", f"pipeline/artifacts/{bench}/q{query}/{other}")
        ]
    configured = workload.get("agent_hidden_trees")
    trees = [str(p).strip().rstrip("/") for p in configured if str(p).strip()] if isinstance(configured, list) else []
    if engine:
        trees = [t for t in trees if f"/{engine}" not in f"/{t}" and t != engine]
    return trees


def apply_foreign_engine_quarantine(repo_root: Path, workload: dict[str, Any], *, enabled: bool | None = None) -> QuarantineSession | None:
    """Move foreign engine trees out of the repo; None when hiding is off (`hide_foreign_engines`, or the
    ACCEL_HIDE_FOREIGN_ENGINES env, which wins)."""
    if enabled is None:
        enabled = bool(workload.get("hide_foreign_engines", True))
        env = os.environ.get("ACCEL_HIDE_FOREIGN_ENGINES", "").strip().lower()
        if env in {"0", "false", "no", "off"}:
            enabled = False
        elif env in {"1", "true", "yes", "on"}:
            enabled = True
    if not enabled:
        return None
    root = Path(repo_root).resolve()
    qroot = root / "pipeline" / ".quarantine" / "foreign_engines"
    qroot.mkdir(parents=True, exist_ok=True)
    moved: list[tuple[str, str]] = []
    for rel in foreign_trees_for_workload(workload):
        if not (root / rel).exists():
            continue
        dst = qroot / rel.replace("/", "__")
        if dst.exists():
            shutil.rmtree(dst)
        shutil.move(str(root / rel), str(dst))
        moved.append((rel, dst.name))
    manifest = qroot / "manifest.json"
    manifest.write_text(json.dumps({"moved": [{"src": a, "dst": b} for a, b in moved]}, indent=2) + "\n", encoding="utf-8")
    if moved:
        print(f"[pipeline] quarantined {len(moved)} foreign engine tree(s) under {qroot}", flush=True)
    return QuarantineSession(root, qroot, moved, manifest)


def restore_quarantine(session: QuarantineSession | None) -> None:
    if session is None:
        return
    session.restore()
    if session.moved:
        print(f"[pipeline] restored {len(session.moved)} quarantined engine tree(s)", flush=True)


# The shared-file lease: edits (and builds that compile) the shared registration/CMake sources hold it, so two
# agents cannot interleave patches on the same files.

SHARED_WAIT_SEC = 300.0


@dataclass
class LeaseInfo:
    holder: str
    acquired_at: float
    pid: int
    reason: str = ""


class SharedFileLease:
    """flock around shared tree edits/builds; waiters are served in arrival order, with a bound."""

    def __init__(self, repo_root: Path) -> None:
        self.repo_root = Path(repo_root).resolve()
        self._fp: BinaryIO | None = None
        self._dir = self.repo_root / "pipeline" / ".locks"
        self._dir.mkdir(parents=True, exist_ok=True)
        self._lock = FairLock(self._dir, "shared_tree")
        self._meta = self._dir / "shared_tree_lease.json"

    def read_meta(self) -> LeaseInfo | None:
        try:
            data = json.loads(self._meta.read_text(encoding="utf-8"))
            return LeaseInfo(str(data.get("holder") or ""), float(data.get("acquired_at") or 0.0), int(data.get("pid") or 0), str(data.get("reason") or ""))
        except (OSError, ValueError, TypeError):
            return None

    def holder(self) -> str | None:
        """The current holder; None when free (a crashed holder's leftover metadata does not count)."""
        info = self.read_meta()
        return info.holder if info and self._lock.is_held() else None

    def acquire(self, holder: str, *, timeout_sec: float = SHARED_WAIT_SEC, poll_sec: float = 0.1, reason: str = "") -> LeaseInfo:
        def on_wait(waited: float, ahead: int) -> None:
            print(f"[pipeline] {holder}: waiting {waited:.0f}s for the shared-file lease ({ahead} ahead)", file=sys.stderr, flush=True)

        try:
            self._fp = self._lock.acquire(timeout_sec=max(0.0, float(timeout_sec)), poll_sec=poll_sec, on_wait=on_wait)
        except LockTimeout:
            info = self.read_meta()
            who = (
                f"held by {info.holder!r} for {time.time() - info.acquired_at:.0f}s ({info.reason or 'no reason'})"
                if info else "held by another worker"
            )
            raise TimeoutError(
                f"shared-file lease not acquired within {timeout_sec:.0f}s: {who}, {self._lock.queue_depth()} waiting; "
                "work that does not edit shared files, then retry"
            ) from None
        info = LeaseInfo(holder, time.time(), os.getpid(), reason)
        self._meta.write_text(json.dumps(asdict(info), indent=2) + "\n", encoding="utf-8")
        return info

    def release(self, holder: str | None = None) -> bool:
        meta = self.read_meta()
        if holder is not None and meta is not None and meta.holder != holder:
            return False
        self._meta.unlink(missing_ok=True)
        if self._fp is not None:
            self._fp.close()
            self._fp = None
        return True

    def ensure_held(self, holder: str, *, timeout_sec: float = SHARED_WAIT_SEC, reason: str = "") -> LeaseInfo:
        meta = self.read_meta()
        mine = meta is not None and meta.holder == holder
        if mine and self._fp is not None:
            return meta
        return self.acquire(holder, timeout_sec=timeout_sec, reason=reason or ("rebind" if mine else ""))


# The process-sticky lease: taken on the first shared write and kept until released or the process exits.
_sticky: tuple[str, Path, SharedFileLease] | None = None  # (holder, repo root, lease)


def sticky_holder() -> str | None:
    return _sticky[0] if _sticky else None


def sticky_lease_obj() -> SharedFileLease | None:
    return _sticky[2] if _sticky else None


def ensure_sticky_lease_for_write(repo_root: Path, *, holder: str | None = None, timeout_sec: float = SHARED_WAIT_SEC) -> LeaseInfo:
    global _sticky
    who = holder or worker_id_from_env()
    root = Path(repo_root).resolve()
    if _sticky and _sticky[:2] == (who, root) and _sticky[2]._fp is not None:
        if (meta := _sticky[2].read_meta()) and meta.holder == who:
            return meta
    lease = SharedFileLease(root)
    info = lease.ensure_held(who, timeout_sec=timeout_sec, reason="shared write")
    _sticky = (who, root, lease)
    return info


def release_sticky_lease(repo_root: Path | None = None, holder: str | None = None) -> bool:
    global _sticky
    who = holder or (_sticky[0] if _sticky else None) or worker_id_from_env()
    root = Path(repo_root or (_sticky[1] if _sticky else Path.cwd())).resolve()
    ok = (_sticky[2] if _sticky else SharedFileLease(root)).release(who)
    if _sticky and _sticky[0] == who:
        _sticky = None
    return ok


@atexit.register
def _release_sticky_at_exit() -> None:
    if _sticky:
        try:
            _sticky[2].release(_sticky[0])
        except Exception:
            pass


def command_touches_shared_build(cmd: str) -> bool:
    """A cmake configure/build of the shared library likely compiles shared TUs."""
    lowered = cmd.lower()
    return "cmake -s" in lowered or "cmake --build" in lowered


# The repo context pack: the layout and registration guide every worker's seed points at, so agents do not
# re-explore it each attempt

PACK_REL = Path("pipeline/artifacts/shared/repo_context_pack.md")


def build_repo_context_pack_markdown(repo_root: Path) -> str:
    root = Path(repo_root)
    modules = [str(p.relative_to(root)) for p in sorted(root.glob("accelerators/tpch/q*/*/aux/module.cpp"))]
    index = [
        f"- `{rel}` ({next((p for p in Path(rel).parts if p.startswith('q') and p[1:].isdigit()), '?')})" for rel in modules
    ] or ["(none found)"]
    return "\n".join([
        "# Shared repo context pack (read-only)",
        "",
        f"Generated: {datetime.now(timezone.utc).strftime('%Y-%m-%d %H:%M:%SZ')}",
        "",
        "Workers should **not** re-bootstrap this map every attempt. Use it as the",
        "canonical layout / registration guide. Editable work stays in **your** `qN/aux/**`.",
        "",
        "## Ownership rules (enforced)",
        "",
        "- **Writable:** only your query directory `accelerators/<bench>/q{N}/**` (aux module,",
        "  registration, tests/, bench/, config/ cost model). `substrate/**` and `workloads/**`",
        "  are read-only; `backends/<engine>/**` belongs to the engine_backend run;",
        "  `accelerators/prims/**` changes only via `primitive_publish`.",
        "- **Registration lives in the query's own directory** (registrars); the engine backend",
        "  names no query. Shared files (backend, arg variants, cross-accel helpers, CMake) are",
        "  frozen during optimization. Do not edit them mid-opt.",
        "- **Cross-query writes are denied.** Read peers (search with `rg`) only.",
        "",
        "## Registration surfaces (discover via peer search; do not invent partial wiring)",
        "",
        "| Surface | Where | Failure if missing |",
        "|---------|-------|--------------------|",
        "| Registrars and their headers | `backends/<engine>/api/README.md` (`## Contract roles` table) | compile / link errors |",
        "| The query's registration | `accelerators/<bench>/q{N}/<engine>/register.cpp` (peer queries show the pattern) | `unsupported shared query` / function does not exist |",
        "| Shared test and bench helpers | `eval/tests/test_support/`, `eval/bench/` (read peers) | multi-conn / writer SQL fail |",
        "",
        "## TPC-H aux module index (read-only references)",
        "",
        *index,
        "",
        "## Profiling / optimization loop (token-efficient)",
        "",
        "1. Measure with MCP `run_profile` / digests on the **current** owned tree.",
        "2. Patch only owned editable files (`module.hpp/cpp`, `finalize.cpp`, tests, cost model).",
        "3. Yield `CANDIDATE_READY` only after local build+smoke for **your** query.",
        "4. On `unsupported shared query` or foreign-file compile errors: **stop** —",
        "   orchestrator fail-fast repairs the tree; do not burn more turns.",
        "",
        "## Search examples",
        "",
        "```",
        "rg -n -C5 <Symbol> accelerators/<bench>/q<N>/<engine>/aux",
        "```",
        "",
    ])


def ensure_repo_context_pack(repo_root: Path, *, refresh: bool = False) -> Path:
    """The pack's markdown file, written when missing or when `refresh`."""
    out = Path(repo_root) / PACK_REL
    if refresh or not out.is_file():
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(build_repo_context_pack_markdown(repo_root), encoding="utf-8")
    return out


def format_seed_pointer(repo_root: Path, *, max_preview_lines: int = 40) -> str:
    """Short seed block pointing at the shared pack (avoids re-inlining the full map)."""
    text = ensure_repo_context_pack(repo_root).read_text(encoding="utf-8")
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]
    return "\n".join([
        "## Shared repo context pack (do not re-explore layout)",
        "",
        f"- path: `{PACK_REL.as_posix()}` (sha={digest})",
        "- Full ownership / registration / aux index lives there. Prefer that file over",
        "  rediscovering CMake/catalog/helpers each attempt.",
        "",
        "### Preview",
        "```",
        *text.splitlines()[:max_preview_lines],
        "```",
        "",
    ])


# The pack's MCP tools: the shared-file lease an agent holds around edits to the files every worker shares


def callables(ctx: ToolContext) -> dict[str, Callable[..., str]]:
    pipeline_config = ctx.pipeline_config
    holder = pipeline_config.worker_id or worker_id_from_env(f"q{ctx.query}")

    def acquire_shared_lease(reason: str = "", timeout_sec: float = 300.0) -> str:
        """Hold shared-file lease before editing catalog/CMake/shared registration files."""
        info = ensure_sticky_lease_for_write(
            pipeline_config.repo_root, holder=holder,
            timeout_sec=min(float(timeout_sec), pipeline_config.concurrency.shared_lease_wait_sec),
        )
        return json.dumps({"acquired": True, "holder": holder, "reason": reason, "lease": asdict(info)}, indent=2)

    def release_shared_lease() -> str:
        """Release sticky shared-file lease after shared edits + rebuild."""
        return json.dumps({"released": release_sticky_lease(pipeline_config.repo_root, holder=holder), "holder": holder})

    return {"acquire_shared_lease": acquire_shared_lease, "release_shared_lease": release_shared_lease}


def register(tool, call) -> None:
    @tool(read_only=False)
    def acquire_shared_lease(reason: str = "", timeout_sec: float = 300.0) -> str:
        """Hold shared-file lease before editing catalog/CMake/shared registration files."""
        return call("acquire_shared_lease", reason, timeout_sec)

    @tool(read_only=False)
    def release_shared_lease() -> str:
        """Release sticky shared-file lease after shared edits + rebuild."""
        return call("release_shared_lease")


# The pack runner: one run per query x engine, on the shared tree with separate builds, no stage barriers


@dataclass(frozen=True)
class PackQuery:
    workload: str
    query: int
    engine: str = ""  # "" when the workload supports a single engine
    stage: str = "all"
    resume: bool = True
    mode: str = ""  # "" (workload default / cpp) or "verus"


def load_pack(name_or_path: str, repo_root: Path | None = None) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    packs = pipeline_root(root) / "config" / "packs"
    path = Path(name_or_path)
    if not path.is_absolute():
        for cand in (path, root / path, packs / path, packs / f"{path.stem}.yaml", packs / f"{name_or_path}.yaml"):
            if cand.is_file():
                path = cand
                break
        else:
            raise FileNotFoundError(f"pack not found: {name_or_path}")
    with path.open(encoding="utf-8") as f:
        data = yaml.safe_load(f) or {}
    data["_pack_path"] = str(path)
    return data


def parse_pack_queries(pack: dict[str, Any]) -> list[PackQuery]:
    """One entry per query x engine; an entry's `engines:` overrides the pack's."""
    out: list[PackQuery] = []
    default_engines = list(pack.get("engines") or [])
    templates = pack.get("workload_template", "tpch_q{q}_aux")
    # A list runs each query's accelerators side by side (e.g. its base and its aux workload).
    default_templates = [str(t) for t in templates] if isinstance(templates, list) else [str(templates)]
    for item in pack.get("queries") or []:
        if isinstance(item, (int, str)) and str(item).isdigit():
            item = {"query": int(item)}
        if not isinstance(item, dict):
            raise ValueError(f"invalid pack query entry: {item!r}")
        q = int(item.get("query") or item.get("query_number"))
        workloads = [str(item["workload"])] if item.get("workload") else default_templates
        for engine in item.get("engines") or default_engines or [""]:
            for template in workloads:
                out.append(PackQuery(
                    workload=template.format(q=q),
                    query=q,
                    engine=str(engine),
                    stage=str(item.get("stage") or pack.get("stage") or "all"),
                    resume=bool(item.get("resume", pack.get("resume", True))),
                    mode=str(item.get("mode") or pack.get("mode") or ""),
                ))
    return out


def _chains_by_artifact_dir(entries: list[PackQuery]) -> list[list[PackQuery]]:
    """A query's workloads on one engine (its base kernel and its aux accelerator) share an artifact directory, so
    they run one after another; different query x engine pairs run in parallel."""
    chains: dict[tuple[int, str], list[PackQuery]] = {}
    for entry in entries:
        chains.setdefault((entry.query, entry.engine), []).append(entry)
    return list(chains.values())


def run_pack_query(
    *,
    repo_root: Path,
    entry: PackQuery,
    models_config: str | None,
    pipeline_config_path: str | None,
    dry_run: bool,
    skip_gates: bool,
    extra_args: list[str] | None = None,
    isolation_mode: str = "worktree",
    pack_queries: list[int] | None = None,
    concurrent_workers: int = 1,
) -> dict[str, Any]:
    worker_id = worker_id_for_query(entry.query, entry.workload, entry.engine)
    primary = Path(repo_root).resolve()
    # Build + Codex home always on primary (shared caches / sidecars).
    build_dir = default_build_dir_for_worker(primary, worker_id)
    codex_home = prepare_codex_home(repo_root=primary, worker_id=worker_id)
    worker_repo = ensure_worker_worktree(primary, worker_id, load_workload(entry.workload, primary, engine=entry.engine), reset=True,
                                           pack_queries=pack_queries) if isolation_mode in WORKTREE_MODES else primary
    log_path = pipeline_root(primary) / "artifacts" / "packs" / "logs" / f"{worker_id}.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)

    tag = instance_tag(worker_id, build_dir)  # one engine instance per worker: container names and ports derive from it
    env = {
        **os.environ,
        "ACCEL_PIPELINE_WORKER_ID": worker_id,
        "ACCEL_PIPELINE_BUILD_DIR": str(build_dir),
        "ACCEL_PIPELINE_CODEX_HOME": str(codex_home),
        "ACCEL_PIPELINE_QUERY": str(entry.query),
        "ACCEL_PIPELINE_WORKLOAD": entry.workload,
        "ACCEL_PIPELINE_REPO_ROOT": str(worker_repo),
        "ACCEL_PIPELINE_PRIMARY_REPO": str(primary),
        "ACCEL_PIPELINE_ISOLATION_MODE": isolation_mode,
        "CODEX_HOME": str(codex_home),
        "ACCEL_INSTANCE_TAG": tag,
        "ACCEL_ENGINE_INSTANCE": f"{tag}-{entry.engine or 'engine'}",
        WORKERS_ENV: str(concurrent_workers),  # the CPU partition
    }
    if pack_queries:
        env["ACCEL_PIPELINE_PACK_QUERIES"] = ",".join(str(q) for q in pack_queries)

    cmd = [sys.executable, "-m", "accel_pipeline.cli", "--query", str(entry.query), "--workload", entry.workload, "--stage", entry.stage]
    for flag, value in (("--engine", entry.engine), ("--mode", entry.mode), ("--config", pipeline_config_path), ("--models-config", models_config)):
        if value:
            cmd += [flag, value]
    cmd += [flag for flag, on in (("--resume", entry.resume), ("--dry-run", dry_run), ("--skip-gates", skip_gates)) if on]
    cmd += extra_args or []

    started = time.time()
    print(
        f"[pack] start worker={worker_id} query={entry.query} workload={entry.workload} engine={entry.engine or '-'} "
        f"build_dir={build_dir.name} repo={worker_repo.name if worker_repo != primary else 'primary'} "
        f"isolation={isolation_mode} stage={entry.stage}",
        file=sys.stderr, flush=True,
    )
    with log_path.open("w", encoding="utf-8") as logf:
        logf.write(
            f"# cmd: {' '.join(cmd)}\n# build_dir={build_dir}\n# codex_home={codex_home}\n"
            f"# repo_root={worker_repo}\n# primary={primary}\n# isolation={isolation_mode}\n\n"
        )
        logf.flush()
        proc = subprocess.run(cmd, cwd=primary / "pipeline" / "orchestrator", env=env, stdout=logf, stderr=subprocess.STDOUT, check=False)
    duration = time.time() - started
    print(f"[pack] {'ok' if proc.returncode == 0 else 'FAIL'} worker={worker_id} exit={proc.returncode} duration={duration:.1f}s log={log_path}", file=sys.stderr, flush=True)
    return {
        "worker_id": worker_id,
        "query": entry.query,
        "workload": entry.workload,
        "engine": entry.engine,
        "stage": entry.stage,
        "exit_code": proc.returncode,
        "duration_sec": round(duration, 2),
        "build_dir": str(build_dir),
        "codex_home": str(codex_home),
        "repo_root": str(worker_repo),
        "isolation_mode": isolation_mode,
        "log": str(log_path),
        "ok": proc.returncode == 0,
    }


def run_pack(
    pack: dict[str, Any],
    *,
    repo_root: Path,
    concurrency: int | None = None,
    models_config: str | None = None,
    pipeline_config_path: str | None = None,
    dry_run: bool = False,
    skip_gates: bool = False,
    extra_args: list[str] | None = None,
) -> dict[str, Any]:
    """`first_wave: N` in the pack runs its first N runs to completion before fanning out the rest, so the
    primitives they publish exist before the mass of runs starts."""
    from accel_pipeline.engine_backend import ensure_host_bootstrap_for_pack, reopen_engine_backend

    queries = parse_pack_queries(pack)
    if not queries:
        raise ValueError("pack has no queries")
    max_workers = max(1, min(int(concurrency or pack.get("concurrency") or 2), len(queries)))
    print(f"[pack] shared repo context pack ready: {ensure_repo_context_pack(repo_root, refresh=True)}", flush=True)

    pipeline_config_path = pipeline_config_path or pack.get("pipeline_config")
    models_config = models_config or pack.get("models_config")
    pipeline_config = load_pipeline_config(Path(pipeline_config_path) if pipeline_config_path else None, repo_root=repo_root)
    model_config = load_model_config(Path(models_config) if models_config else None, repo_root=repo_root)
    # Backend defects earlier query runs recorded go back to the engine's backend run first.
    for engine in sorted({q.engine for q in queries if q.engine}):
        reopen_engine_backend(pipeline_config, model_config, engine, dry_run=dry_run)
    # One-shot host bootstrap per engine before the parallel query workers.
    hb_results = ensure_host_bootstrap_for_pack(
        pack, repo_root=repo_root, pipeline_config=pipeline_config, model_config=model_config,
        queries=queries, dry_run=dry_run, skip_gates=skip_gates,
    )
    if hb_results:
        print(f"[pack] host_bootstrap: {hb_results}", flush=True)
        failed = [r for r in hb_results if r.get("status") != "pass"]
        if failed and not dry_run:
            raise RuntimeError(f"host_bootstrap failed before pack fan-out: {failed}")

    isolation_mode = str((pack.get("isolation") or {}).get("mode") or pack.get("isolation_mode") or "worktree")
    if isolation_mode in {"shared_tree_separate_builds", "shared_tree"}:
        isolation_mode = "worktree"
    pack_query_ids = sorted({e.query for e in queries})
    print(f"[pack] isolation_mode={isolation_mode} workers={len(queries)} queries={pack_query_ids}", flush=True)

    def run_chain(chain: list[PackQuery]) -> list[dict[str, Any]]:
        return [
            run_pack_query(
                repo_root=repo_root, entry=entry, models_config=models_config, pipeline_config_path=pipeline_config_path,
                dry_run=dry_run, skip_gates=skip_gates, extra_args=extra_args, isolation_mode=isolation_mode,
                pack_queries=pack_query_ids, concurrent_workers=max_workers,
            )
            for entry in chain
        ]

    first_wave = int(pack.get("first_wave") or 0)
    waves = [queries[:first_wave], queries[first_wave:]] if 0 < first_wave < len(queries) else [queries]
    results: list[dict[str, Any]] = []
    for wave in waves:
        chains = _chains_by_artifact_dir(wave)
        with ThreadPoolExecutor(max_workers=max(1, min(max_workers, len(chains)))) as pool:
            for fut in as_completed([pool.submit(run_chain, chain) for chain in chains]):
                results.extend(fut.result())

    results.sort(key=lambda r: (r["query"], r["workload"], r["engine"]))
    summary = {
        "pack_id": pack.get("pack_id") or pack.get("name") or "pack",
        "pack_path": pack.get("_pack_path"),
        "concurrency": max_workers,
        "isolation": isolation_mode,
        "shared_file_lease": True,
        "stage_barriers": False,
        "results": results,
        "ok_count": sum(1 for r in results if r["ok"]),
        "fail_count": sum(1 for r in results if not r["ok"]),
    }
    out_path = pipeline_root(repo_root) / "artifacts" / "packs" / f"{summary['pack_id']}_last_run.json"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    summary["summary_path"] = str(out_path)
    return summary


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ["measure"]:
        return measure_main(argv[1:])
    parser = argparse.ArgumentParser(
        description="Run many query pipelines concurrently on a shared tree with separate build dirs and a "
        "shared-file lease (no stage barriers). `pack measure ...` runs a command under the measurement lease."
    )
    parser.add_argument("--pack", required=True, help="Pack name or YAML path under pipeline/config/packs/")
    parser.add_argument("--concurrency", type=int, default=None)
    parser.add_argument("--models-config", default=None)
    parser.add_argument("--config", default=None, help="Pipeline config YAML")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--skip-gates", action="store_true")
    parser.add_argument("--mode", choices=["cpp", "verus"], default=None,
                        help="Pipeline mode for every query of the pack (overrides the pack's `mode`)")
    args = parser.parse_args(argv)

    repo_root = find_repo_root()
    load_repo_dotenv(repo_root)
    pack = load_pack(args.pack, repo_root)
    if args.mode:
        pack["mode"] = args.mode
        for item in pack.get("queries") or []:
            if isinstance(item, dict):
                item["mode"] = args.mode
    summary = run_pack(
        pack, repo_root=repo_root, concurrency=args.concurrency, models_config=args.models_config,
        pipeline_config_path=args.config, dry_run=args.dry_run, skip_gates=args.skip_gates,
    )
    print(json.dumps(summary, indent=2))
    return 0 if summary["fail_count"] == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
