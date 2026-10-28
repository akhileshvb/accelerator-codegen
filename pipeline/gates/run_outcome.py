#!/usr/bin/env python3
"""How a gate's harness run ends, and how that reaches the gate's result (aux_gate_common.sh, _engine_run).

run <seconds> <outcome json> <label> -- <command...>
    Runs the command in this process group (a killed gate takes it along). A failed run leaves <outcome json>
    ({"exit_code", "reason", "stacks_log"}); a passing one removes it, so the file is always the gate's last run.
    Past the deadline this process becomes gdb, the one tracer kernel.yama.ptrace_scope=1 allows (an ancestor),
    dumps the user stacks of every process the command started, then kills them and exits 124.
cap -- <command...>
    Execs the command in a user scope capped at ACCEL_RUN_MEMORY_MB (swap off), where scopes work: a run over its
    memory dies alone (exit 137) instead of starving the machine. `run` caps its command the same way.
fold <gate json>
    Merges the last run's outcome into a failed result the gate script wrote, whatever wrote it: the exit code, the
    run's reason after the script's own, and the stacks condensed (threads with the same frames counted once). A run
    that hit its deadline after queueing for the measurement lease makes the failure infrastructure's.
"""

from __future__ import annotations

import contextlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time
import uuid
from pathlib import Path

GRACE_S = 15
QUEUED_S = 5.0  # a wait for the measurement lease this long means other benchmarks were running
MAX_TRACED = 6
MAX_FRAMES = 30
STACKS_CHARS = 8000
# Launchers between the gate and the harness: their stacks say nothing.
_LAUNCHERS = frozenset({"bash", "sh", "dash", "env", "flock", "taskset", "timeout", "python3", "python"})


def _children() -> dict[int, list[int]]:
    tree: dict[int, list[int]] = {}
    for stat in Path("/proc").glob("[0-9]*/stat"):
        try:
            fields = stat.read_text().rsplit(")", 1)[1].split()
        except (OSError, IndexError):
            continue
        tree.setdefault(int(fields[1]), []).append(int(stat.parent.name))
    return tree


def _descendants(root: int) -> list[int]:
    tree, out, todo = _children(), [], [root]
    while todo:
        pid = todo.pop()
        out.append(pid)
        todo.extend(tree.get(pid, []))
    return out


def _comm(pid: int) -> str:
    try:
        return Path(f"/proc/{pid}/comm").read_text().strip()
    except OSError:
        return ""


def _alive(pid: int) -> bool:
    try:
        return Path(f"/proc/{pid}/stat").read_text().rsplit(")", 1)[1].split()[0] != "Z"
    except (OSError, IndexError):
        return False


def _reap(pids: list[int]) -> None:
    """SIGTERM, then SIGKILL after the grace period, to every pid still alive."""
    for sig in (signal.SIGTERM, signal.SIGKILL):
        for pid in pids:
            try:
                os.kill(pid, sig)
            except OSError:
                pass
        deadline = time.monotonic() + GRACE_S
        while sig == signal.SIGTERM and time.monotonic() < deadline and any(map(_alive, pids)):
            time.sleep(0.2)


def _write(path: str, outcome: dict) -> None:
    if path:
        Path(path).write_text(json.dumps(outcome), encoding="utf-8")


def _exec_gdb(pids: list[int], stacks_log: str) -> None:
    """Become gdb: as this process it is the ancestor of every pid, so the attach is allowed. It writes the stacks,
    then runs `reap` on the pids and quits 124."""
    traced = [p for p in pids if _comm(p) not in _LAUNCHERS][:MAX_TRACED]
    gdb = shutil.which("gdb")
    if not (gdb and traced and stacks_log):
        _reap(pids)
        sys.exit(124)
    cmds = ["set pagination off", "set print frame-arguments none", f"set logging file {stacks_log}",
            "set logging overwrite on", "set logging redirect on", "set logging enabled on"]
    for pid in traced:
        cmds += [f"echo \\n== process {pid} ({_comm(pid)}) ==\\n", f"attach {pid}", f"thread apply all bt {MAX_FRAMES}",
                 "detach"]
    cmds += ["set logging enabled off", f"shell {sys.executable} {__file__} reap {' '.join(map(str, pids))}", "quit 124"]
    os.execv(gdb, [gdb, "-batch", "-nx", "-q", *(a for c in cmds for a in ("-ex", c))])


def capped(cmd: list[str], mb: str = "") -> list[str]:
    """`cmd` under a memory cap of `mb` (default ACCEL_RUN_MEMORY_MB; swap off) when the budget is set: in a cgroup
    of its own under the sandbox's delegated one (ACCEL_CGROUP, clean_tree/sandbox.sh), else in a user scope when
    the user's systemd can make one; else `cmd` unchanged. Either way the command runs in place (it stays our
    descendant) and past the cap the kernel kills the process that grew, not its peers."""
    mb = mb or os.environ.get("ACCEL_RUN_MEMORY_MB", "")
    if not mb.isdigit():
        return cmd
    if root := os.environ.get("ACCEL_CGROUP"):
        return ["sh", "-c", 'echo $$ >"$0/cgroup.procs" && exec "$@"', str(_run_cgroup(Path(root), mb)), *cmd]
    if not _scopes():
        return cmd
    return ["systemd-run", "--user", "--scope", "-q", "--collect", "-p", f"MemoryMax={mb}M", "-p", "MemorySwapMax=0",
            "-p", "OOMPolicy=continue", "--", *cmd]


def _run_cgroup(root: Path, mb: str) -> Path:
    """A cgroup for this process's run, named for it: it is in use while that process lives (the cap execs in place,
    a run waits for its child), even before the run enters it, so only those of ended processes are pruned."""
    for done in root.glob("run-*"):
        with contextlib.suppress(OSError, ValueError):
            if not _alive(int(done.name.split("-")[1])):
                done.rmdir()  # a populated cgroup refuses it
    cgroup = root / f"run-{os.getpid()}-{uuid.uuid4().hex[:8]}"
    cgroup.mkdir()
    (cgroup / "memory.max").write_text(f"{mb}M")
    (cgroup / "memory.swap.max").write_text("0")
    return cgroup


def _over_cap(cmd: list[str]) -> bool:
    """Whether this capped run may have been killed for its cap: its cgroup counts the kills; a scope's is not ours to read."""
    if cmd[:1] == ["sh"]:
        with contextlib.suppress(OSError, ValueError):
            return dict(line.split() for line in Path(cmd[3], "memory.events").read_text().splitlines())["oom_kill"] != "0"
        return False
    return cmd[:1] == ["systemd-run"]


def _scopes() -> bool:
    return bool(shutil.which("systemd-run")) and subprocess.run(
        ["systemd-run", "--user", "--scope", "-q", "--collect", "true"], capture_output=True).returncode == 0


def run(seconds: float, outcome_path: str, label: str, cmd: list[str]) -> int:
    stacks_log = f"{outcome_path}.stacks" if outcome_path else ""  # without an outcome nothing reads the stacks
    for stale in filter(None, (outcome_path, stacks_log)):
        Path(stale).unlink(missing_ok=True)
    cmd = capped(cmd)
    child = subprocess.Popen(cmd)

    def forward(signum, _frame):
        _reap(_descendants(child.pid))
        sys.exit(128 + signum)

    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, forward)
    try:
        rc = child.wait(timeout=seconds)
    except subprocess.TimeoutExpired:
        _write(outcome_path, {"exit_code": 124, "stacks_log": stacks_log, "reason": (
            f"{label} did not finish within {seconds:.0f}s; `stacks` holds every thread's stack at the deadline "
            "(blocked threads: a hang, busy ones: a phase too slow for its data)")})
        _exec_gdb(_descendants(child.pid), stacks_log)
    rc = 128 - rc if rc < 0 else rc
    if rc:
        over = rc == 137 and _over_cap(cmd)
        _write(outcome_path, {"exit_code": rc, "reason": (
            f"{label} was killed (exit 137): over its memory cap of {os.environ['ACCEL_RUN_MEMORY_MB']} MB "
            "(ACCEL_RUN_MEMORY_MB), or by another signal" if over else f"{label} exited {rc}")})
    elif outcome_path:
        Path(outcome_path).unlink(missing_ok=True)
    return rc


_THREAD = re.compile(r"^Thread \d+ .*?(?:\"([^\"]*)\")?:?$")
_FRAME = re.compile(r"^#\d+\s+(?:0x[0-9a-f]+ in )?(.+?)(?: \(.*?\))?(?: (?:at|from) (\S+))?$")


def condense(text: str) -> str:
    """gdb's `thread apply all bt` per process, with threads that share their frames listed once with a count."""
    out: list[str] = []
    process, stacks, frames = "", {}, None

    def flush():
        if process:
            out.append(process)
            for stack, count in sorted(stacks.items(), key=lambda kv: -kv[1]):
                out.append(f"  {count} thread{'s' if count > 1 else ''}:")
                out.extend(f"    {frame}" for frame in stack)

    for line in text.splitlines():
        line = line.rstrip()
        if line.startswith("== process "):
            flush()
            process, stacks, frames = line, {}, None
        elif _THREAD.match(line):
            frames = []
        elif frames is not None and (m := _FRAME.match(line)):
            frames.append(m.group(1) + (f"  {Path(m.group(2)).name}" if m.group(2) else ""))
        elif frames is not None and not line:
            if frames:
                stacks[tuple(frames)] = stacks.get(tuple(frames), 0) + 1
            frames = None
    if frames:
        stacks[tuple(frames)] = stacks.get(tuple(frames), 0) + 1
    flush()
    return "\n".join(out)[:STACKS_CHARS]


def _lease_wait(result_path: Path) -> float:
    """Seconds the gate's runs waited for the measurement lease (the gate's `.phases` file)."""
    phases = Path(f"{str(result_path).removesuffix('.json')}.phases")
    text = phases.read_text(encoding="utf-8") if phases.is_file() else ""
    return sum(float(m) for m in re.findall(r"^measurement_lease_wait=([\d.]+)$", text, re.MULTILINE))


def fold(result_path: Path) -> None:
    outcome_path = Path(f"{str(result_path).removesuffix('.json')}.run.json")
    if not outcome_path.is_file():
        return
    outcome = json.loads(outcome_path.read_text(encoding="utf-8"))
    stacks_log = Path(outcome.get("stacks_log") or "/nonexistent")
    try:
        data = json.loads(result_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        data = None
    if isinstance(data, dict) and data.get("status") not in ("pass", "skip"):
        if data.get("exit_code") is None and data.get("returncode") is None:
            data["exit_code"] = outcome["exit_code"]
        reason, ran = str(data.get("reason") or "").strip(), outcome.get("reason") or ""
        if ran not in reason:
            data["reason"] = f"{reason}; {ran}" if reason else ran
        if outcome["exit_code"] == 124 and _lease_wait(result_path) >= QUEUED_S:
            data["owner"] = "infra"
        if stacks_log.is_file() and (stacks := condense(stacks_log.read_text(errors="replace"))):
            data["stacks"] = stacks
        result_path.write_text(json.dumps(data, indent=2), encoding="utf-8")
    outcome_path.unlink(missing_ok=True)
    stacks_log.unlink(missing_ok=True)


def main(argv: list[str]) -> int:
    verb, *args = argv
    if verb == "run":
        seconds, outcome, label, sep, *cmd = args
        assert sep == "--" and cmd, "usage: run <seconds> <outcome json> <label> -- <command...>"
        return run(float(seconds), outcome, label, cmd)
    if verb == "cap":
        mb = args.pop(0) if args and args[0] != "--" else ""
        sep, *cmd = args
        assert sep == "--" and cmd, "usage: cap [<mb>] -- <command...>"
        cmd = capped(cmd, mb)
        os.execvp(cmd[0], cmd)
    if verb == "fold":
        fold(Path(args[0]))
        return 0
    if verb == "reap":
        _reap([int(p) for p in args])
        return 0
    raise SystemExit(f"unknown verb {verb}")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
