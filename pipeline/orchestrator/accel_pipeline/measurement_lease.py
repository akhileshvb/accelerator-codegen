"""The host-wide measurement lease: benchmarks measure on the reserved measurement CPUs, one per slot.

    with measurement_window(holder, reason) as window:
        subprocess.run(window.argv(cmd), env={**os.environ, **window.env}, timeout=...)

or, from a shell gate, `python -m accel_pipeline.measurement_lease --holder H --reason R -- cmd...`, which
queues, pins itself to its slot's CPUs and execs `cmd` still holding the lease (it ends with the process). The lock lives
under the host lock dir, so it spans every tree and worktree on the host, and it is a flock: a crashed
holder releases it. Waiting is first come first served, bounded, and beats the agent heartbeat.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from contextlib import contextmanager
from dataclasses import dataclass, field
from pathlib import Path
from typing import Callable, Iterator

from accel_pipeline.core_budget import (
    MEASURE_SET_ENV, MEASURE_SLOTS_ENV, core_groups, format_cpus, host_lock_dir, parse_cpus, partition,
)
from accel_pipeline.host_lock import FairLock, LockTimeout

WAIT_ENV = "ACCEL_MEASURE_WAIT_SEC"
ENABLED_ENV = "ACCEL_MEASURE_LEASE"
HEARTBEAT_ENV = "ACCEL_HEARTBEAT_FILE"
DEFAULT_WAIT_SEC = 3600.0
BUSY_EXIT = 75
WAITED = re.compile(r"waited ([0-9.]+) s for the measurement lease")
BUSY = re.compile(r"measurement lease not acquired")


def _lock() -> FairLock:
    return FairLock(host_lock_dir(), "measurement", _settings()[3])


def _meta_path(slot: int) -> Path:
    return host_lock_dir() / f"measurement.holder.{slot}.json"


def holders() -> list[dict]:
    """Who holds a measurement slot right now (none: the lease is idle)."""
    out = []
    for slot in _lock().held_slots():
        try:
            out.append(json.loads(_meta_path(slot).read_text(encoding="utf-8")))
        except (OSError, ValueError):
            out.append({"holder": "?"})
    return out


def held_by_other(me: str) -> bool:
    return any(info.get("holder") != me for info in holders())


def waited_seconds(text: str) -> float | None:
    """Seconds a run waited for the lease, from the line the wrapper prints (None: it never queued)."""
    match = WAITED.search(text)
    return float(match.group(1)) if match else None


def _heartbeat() -> None:
    beat = os.environ.get(HEARTBEAT_ENV)
    if beat:
        Path(beat).touch()


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
    _handle: object = field(default=None, repr=False, compare=False)

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


def _acquire(holder: str, reason: str, wait_sec: float, beat: Callable[[], None] | None) -> tuple[object, int, float]:
    def on_wait(waited: float, ahead: int) -> None:
        print(f"[measurement-lease] queued {waited:.0f}s, {ahead} ahead; {_describe_holders()}", file=sys.stderr, flush=True)
        _heartbeat()
        if beat is not None:
            beat()

    started = time.monotonic()
    try:
        handle, slot = _lock().acquire_slot(timeout_sec=wait_sec, on_wait=on_wait)
    except LockTimeout as exc:
        raise LockTimeout(f"measurement lease not acquired within {wait_sec:.0f}s ({_describe_holders()}): {exc}") from None
    waited = time.monotonic() - started
    _meta_path(slot).write_text(
        json.dumps({"holder": holder, "reason": reason, "pid": os.getpid(), "since": time.time()}), encoding="utf-8"
    )
    return handle, slot, waited


def _settings() -> tuple[bool, float, frozenset[int], int]:
    """(enabled, wait bound, measurement CPUs, slots), as core_budget.activate exported them."""
    return (
        os.environ.get(ENABLED_ENV, "1") != "0",
        float(os.environ.get(WAIT_ENV) or DEFAULT_WAIT_SEC),
        parse_cpus(os.environ.get(MEASURE_SET_ENV, "")),
        int(os.environ.get(MEASURE_SLOTS_ENV) or 1),
    )


def _slot_cpus(measure: frozenset[int], slots: int, slot: int) -> frozenset[int]:
    """The slot's share of the measurement cores (a measurement runs the accelerator and the baseline engine
    on that one budget)."""
    return partition(core_groups(measure), slots, 0)[0][slot] if measure else frozenset()


@contextmanager
def measurement_window(holder: str, reason: str, *, beat: Callable[[], None] | None = None) -> Iterator[Window]:
    """Hold the lease for the block; yields how long it queued and the CPUs to pin the measurement to."""
    enabled, wait_sec, measure, slots = _settings()
    if not enabled:
        yield Window(cpus=measure)
        return
    handle, slot, waited = _acquire(holder, reason, wait_sec, beat)
    window = Window(waited, _slot_cpus(measure, slots, slot), True, handle)
    print(window.line(), file=sys.stderr, flush=True)
    try:
        yield window
    finally:
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


def lease_note(text: str) -> dict | None:
    """What the benchmark output says of its queueing: {"waited_sec": total, "runs": n}, None if none."""
    waits = [float(w) for w in WAITED.findall(text)]
    return {"waited_sec": round(sum(waits), 1), "runs": len(waits)} if waits else None


def noise_warning(load_factor: float = 1.5) -> str | None:
    """Why a benchmark run now without the lease is noisy, or None when the host looks idle."""
    reasons = []
    load, cpus = os.getloadavg()[0], len(os.sched_getaffinity(0))
    if load > load_factor * (os.cpu_count() or cpus):
        reasons.append(f"load average {load:.0f} on {os.cpu_count()} cores")
    if held := holders():
        reasons.append(f"measurements are running ({', '.join(str(i.get('holder')) for i in held)})")
    return f"noisy: {'; '.join(reasons)} (not measured under the measurement lease)" if reasons else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run a command holding the measurement lease, pinned to the measurement CPUs.")
    parser.add_argument("--holder", required=True)
    parser.add_argument("--reason", default="")
    parser.add_argument("cmd", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    enabled, wait_sec, measure, slots = _settings()
    cpus = measure
    if enabled:
        try:
            handle, slot, waited = _acquire(args.holder, args.reason, wait_sec, None)
        except LockTimeout as exc:
            print(exc, file=sys.stderr)
            return BUSY_EXIT
        os.set_inheritable(handle.fileno(), True)  # the lease ends with the exec'd command
        cpus = _slot_cpus(measure, slots, slot)
        print(Window(waited, cpus, True).line(), file=sys.stderr, flush=True)
    if cpus:
        try:
            os.sched_setaffinity(0, cpus)
            os.environ.update(Window(cpus=cpus).env)
        except OSError as exc:
            print(f"[measurement-lease] cannot pin to {format_cpus(cpus)} ({exc}): running unpinned", file=sys.stderr, flush=True)
    os.execvp(cmd[0], cmd)


if __name__ == "__main__":
    raise SystemExit(main())
