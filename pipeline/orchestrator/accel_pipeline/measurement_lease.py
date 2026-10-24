"""The host-wide measurement lease: one benchmark at a time, on the reserved measurement CPUs.

    with measurement_window(holder, reason) as window:
        subprocess.run(window.argv(cmd), env={**os.environ, **window.env}, timeout=...)

or, from a shell gate, `python -m accel_pipeline.measurement_lease --holder H --reason R -- cmd...`, which
queues, pins itself and execs `cmd` still holding the lease (it ends with the process). The lock lives
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

from accel_pipeline.core_budget import MEASURE_SET_ENV, format_cpus, host_lock_dir, parse_cpus
from accel_pipeline.host_lock import FairLock, LockTimeout

WAIT_ENV = "ACCEL_MEASURE_WAIT_SEC"
ENABLED_ENV = "ACCEL_MEASURE_LEASE"
HEARTBEAT_ENV = "ACCEL_HEARTBEAT_FILE"
DEFAULT_WAIT_SEC = 3600.0
BUSY_EXIT = 75
WAITED = re.compile(r"waited ([0-9.]+) s for the measurement lease")
BUSY = re.compile(r"measurement lease not acquired")


def _lock() -> FairLock:
    return FairLock(host_lock_dir(), "measurement")


def _meta_path() -> Path:
    return host_lock_dir() / "measurement.holder.json"


def holder_info() -> dict | None:
    """Who holds the measurement lease right now, or None when it is free."""
    if not _lock().is_held():
        return None
    try:
        return json.loads(_meta_path().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return {"holder": "?"}


def held_by_other(me: str) -> bool:
    info = holder_info()
    return info is not None and info.get("holder") != me


def waited_seconds(text: str) -> float | None:
    """Seconds a run waited for the lease, from the line the wrapper prints (None: it never queued)."""
    match = WAITED.search(text)
    return float(match.group(1)) if match else None


def _heartbeat() -> None:
    beat = os.environ.get(HEARTBEAT_ENV)
    if beat:
        Path(beat).touch()


def _describe_holder() -> str:
    info = holder_info() or {}
    if not info:
        return "free"
    since = f" for {time.time() - info['since']:.0f}s" if info.get("since") else ""
    return f"held by {info.get('holder')} ({info.get('reason') or 'no reason'}){since}"


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


def _acquire(holder: str, reason: str, wait_sec: float, beat: Callable[[], None] | None):
    def on_wait(waited: float, ahead: int) -> None:
        print(f"[measurement-lease] queued {waited:.0f}s, {ahead} ahead; {_describe_holder()}", file=sys.stderr, flush=True)
        _heartbeat()
        if beat is not None:
            beat()

    started = time.monotonic()
    try:
        handle = _lock().acquire(timeout_sec=wait_sec, on_wait=on_wait)
    except LockTimeout as exc:
        raise LockTimeout(f"measurement lease not acquired within {wait_sec:.0f}s ({_describe_holder()}): {exc}") from None
    waited = time.monotonic() - started
    _meta_path().write_text(
        json.dumps({"holder": holder, "reason": reason, "pid": os.getpid(), "since": time.time()}), encoding="utf-8"
    )
    return handle, waited


def _settings() -> tuple[bool, float, frozenset[int]]:
    return (
        os.environ.get(ENABLED_ENV, "1") != "0",
        float(os.environ.get(WAIT_ENV) or DEFAULT_WAIT_SEC),
        parse_cpus(os.environ.get(MEASURE_SET_ENV, "")),
    )


@contextmanager
def measurement_window(holder: str, reason: str, *, beat: Callable[[], None] | None = None) -> Iterator[Window]:
    """Hold the lease for the block; yields how long it queued and the CPUs to pin the measurement to."""
    enabled, wait_sec, cpus = _settings()
    if not enabled:
        yield Window(cpus=cpus)
        return
    handle, waited = _acquire(holder, reason, wait_sec, beat)
    window = Window(waited, cpus, True, handle)
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
    if (info := holder_info()) is not None:
        reasons.append(f"the measurement lease is held by {info.get('holder')}")
    return f"noisy: {'; '.join(reasons)} (not measured under the measurement lease)" if reasons else None


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Run a command holding the measurement lease, pinned to the measurement CPUs.")
    parser.add_argument("--holder", required=True)
    parser.add_argument("--reason", default="")
    parser.add_argument("cmd", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    cmd = args.cmd[1:] if args.cmd[:1] == ["--"] else args.cmd
    enabled, wait_sec, cpus = _settings()
    if enabled:
        try:
            handle, waited = _acquire(args.holder, args.reason, wait_sec, None)
        except LockTimeout as exc:
            print(exc, file=sys.stderr)
            return BUSY_EXIT
        os.set_inheritable(handle.fileno(), True)  # the lease ends with the exec'd command
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
