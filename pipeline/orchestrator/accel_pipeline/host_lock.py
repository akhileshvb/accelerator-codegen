"""A first-come-first-served lock between processes, on flock: a mutex, or a semaphore of `slots` holders.

Waiters queue as ticket files; each ticket is flock-held by its waiter, so a waiter that died is
seen (its flock is free) and dropped. A slot is one flock file: it is released when the holder
exits, however it exits.
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

    def _take_free_slot(self) -> tuple[BinaryIO, int] | None:
        for slot, path in enumerate(self.paths):
            if (handle := try_lock(path)) is not None:
                return handle, slot
        return None

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
        self,
        *,
        timeout_sec: float,
        poll_sec: float = 0.1,
        on_wait: Callable[[float, int], None] | None = None,
    ) -> tuple[BinaryIO, int]:
        """(the held slot file, its index); close the file to release. `on_wait(waited_sec, ahead)` runs about
        every BEAT_SEC while queued. Raises LockTimeout past `timeout_sec`."""
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
                    if tickets and tickets[0] == ticket and (taken := self._take_free_slot()) is not None:
                        return taken
                    now = time.monotonic()
                    if now - started >= timeout_sec:
                        raise LockTimeout(f"not acquired within {timeout_sec:.0f}s")
                    if on_wait is not None and now - last_beat >= BEAT_SEC:
                        last_beat = now
                        on_wait(now - started, tickets.index(ticket) if ticket in tickets else 0)
                    time.sleep(poll_sec)
            finally:
                ticket.unlink(missing_ok=True)
