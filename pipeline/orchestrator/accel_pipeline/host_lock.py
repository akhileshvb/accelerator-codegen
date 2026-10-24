"""A first-come-first-served mutex between processes, on flock.

Waiters queue as ticket files; each ticket is flock-held by its waiter, so a waiter that died is
seen (its flock is free) and dropped. The lock itself is one flock file: it is released when the
holder exits, however it exits.
"""

from __future__ import annotations

import fcntl
import os
import time
from pathlib import Path
from typing import BinaryIO, Callable

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
    def __init__(self, directory: Path, name: str) -> None:
        self.path = Path(directory) / f"{name}.lock"
        self._queue = Path(directory) / f"{name}.queue"

    def _open_lock(self) -> BinaryIO:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        return self.path.open("a+b")

    def is_held(self) -> bool:
        with self._open_lock() as probe:
            return not _try_flock(probe)

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

    def acquire(
        self,
        *,
        timeout_sec: float,
        poll_sec: float = 0.1,
        on_wait: Callable[[float, int], None] | None = None,
    ) -> BinaryIO:
        """The held lock file (close it to release). `on_wait(waited_sec, ahead)` runs about every BEAT_SEC
        while queued. Raises LockTimeout past `timeout_sec`."""
        started = time.monotonic()
        self._queue.mkdir(parents=True, exist_ok=True)
        pending = self._queue / f".{os.getpid()}-{id(self)}"
        ticket = self._queue / f"{time.time_ns():020d}-{os.getpid()}"
        with pending.open("wb") as mine:
            fcntl.flock(mine.fileno(), fcntl.LOCK_EX)
            pending.rename(ticket)  # the flock follows the inode: the ticket is never visible unlocked
            last_beat = started
            try:
                while True:
                    tickets = self._live_tickets()
                    if tickets and tickets[0] == ticket:
                        lock = self._open_lock()
                        if _try_flock(lock):
                            return lock
                        lock.close()
                    now = time.monotonic()
                    if now - started >= timeout_sec:
                        raise LockTimeout(f"not acquired within {timeout_sec:.0f}s")
                    if on_wait is not None and now - last_beat >= BEAT_SEC:
                        last_beat = now
                        on_wait(now - started, tickets.index(ticket) if ticket in tickets else 0)
                    time.sleep(poll_sec)
            finally:
                ticket.unlink(missing_ok=True)
