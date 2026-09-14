"""Capture gate/harness process output even when the binary abort()s.

Piped docker exec + std::abort() often yields an empty log_tail (unflushed
stdio, hijack stream dropped). Subsequent impl retries then get "failed"
with no error text. Write a sidecar log inside the container, cat it after
the process exits, and always surface returncode/signal to the agent.
"""

from __future__ import annotations

import os
import shlex
import subprocess
from dataclasses import dataclass
from typing import Any, Mapping, Sequence

_SIGNAL_NAMES = {
    132: "SIGILL",
    134: "SIGABRT/abort",
    136: "SIGFPE",
    137: "SIGKILL",
    139: "SIGSEGV",
    143: "SIGTERM",
}


def describe_returncode(returncode: int | None) -> str:
    if returncode is None:
        return ""
    if returncode == 0:
        return "exit 0"
    if returncode < 0:
        sig = _SIGNAL_NAMES.get(128 + (-returncode), f"signal {-returncode}")
        return f"killed by {sig} (returncode={returncode})"
    name = _SIGNAL_NAMES.get(int(returncode))
    if name:
        return f"exit {returncode} ({name})"
    return f"exit {returncode}"


def log_tail_for_agent(data: Mapping[str, Any], *, max_chars: int = 8000) -> str:
    """Never-empty failure text for reloop prompts / MCP failure_summary."""
    tail = str(data.get("log_tail") or "").strip()
    if tail:
        return tail[-max_chars:]
    rc = data.get("returncode")
    if rc is None:
        rc = data.get("exit_code")
    bits = ["(no captured stdout/stderr from the gate process)"]
    desc = describe_returncode(int(rc) if rc is not None and str(rc).lstrip("-").isdigit() else None)
    if desc:
        bits.append(desc)
    notes = str(data.get("notes") or data.get("reason") or "").strip()
    if notes:
        bits.append(notes)
    return "\n".join(bits)[-max_chars:]


def enrich_failed_gate_data(
    data: Mapping[str, Any],
    *,
    proc_returncode: int | None = None,
    proc_output: str = "",
) -> dict[str, Any]:
    """Ensure a failing gate JSON always has returncode + non-empty log_tail."""
    out = dict(data)
    status = str(out.get("status") or "").strip().lower()
    if status != "fail":
        return out
    if out.get("returncode") is None and out.get("exit_code") is None:
        if proc_returncode is not None:
            out["returncode"] = proc_returncode
    tail = str(out.get("log_tail") or "").strip()
    extra = str(proc_output or "").strip()
    if not tail and extra:
        out["log_tail"] = extra[-8000:]
    if not str(out.get("log_tail") or "").strip():
        out["log_tail"] = log_tail_for_agent(out)
    return out


def format_reloop_failure_sections(
    data: Mapping[str, Any],
    *,
    max_chars: int = 8000,
) -> list[str]:
    """Markdown sections appended to layer/host-bootstrap gate-fail prompts."""
    rc = data.get("returncode")
    if rc is None:
        rc = data.get("exit_code")
    desc = ""
    if rc is not None and str(rc).lstrip("-").isdigit():
        desc = describe_returncode(int(rc))
    notes = str(data.get("notes") or data.get("reason") or "").strip()
    tail = log_tail_for_agent(data, max_chars=max_chars)
    lines = [
        "",
        "### Notes",
        notes or "(none)",
        "",
        "### returncode",
        desc or "(unset)",
        "",
        "### log_tail",
        "```",
        tail,
        "```",
    ]
    return lines


@dataclass
class CapturedRun:
    returncode: int
    output: str


def run_command(
    cmd: Sequence[str],
    *,
    cwd: str | os.PathLike[str] | None = None,
    env: Mapping[str, str] | None = None,
    timeout: float | None = None,
) -> CapturedRun:
    proc = subprocess.run(
        list(cmd),
        cwd=str(cwd) if cwd is not None else None,
        capture_output=True,
        text=True,
        env=dict(env) if env is not None else os.environ,
        timeout=timeout,
        start_new_session=True,
    )
    output = ((proc.stdout or "") + (proc.stderr or ""))
    return CapturedRun(returncode=int(proc.returncode), output=output)


def docker_exec_harness(
    container: str,
    binary: str,
    *,
    extra_env: Mapping[str, str] | None = None,
    timeout: float = 300.0,
    log_path: str = "/tmp/accel_gate_harness.log",
) -> CapturedRun:
    """Run an in-container harness; recover output after abort() via sidecar log."""
    quoted_bin = shlex.quote(binary)
    quoted_log = shlex.quote(log_path)
    script = f"""
set +e
rm -f {quoted_log}
if command -v stdbuf >/dev/null 2>&1; then
  stdbuf -o0 -e0 {quoted_bin} >{quoted_log} 2>&1
else
  {quoted_bin} >{quoted_log} 2>&1
fi
rc=$?
if [[ ! -s {quoted_log} ]]; then
  printf '(no captured stdout/stderr; exit=%s)\\n' "$rc" >{quoted_log}
fi
cat {quoted_log}
exit $rc
"""
    cmd: list[str] = ["docker", "exec"]
    for key, value in dict(extra_env or {}).items():
        cmd.extend(["-e", f"{key}={value}"])
    cmd.extend([container, "bash", "-lc", script])
    captured = run_command(cmd, timeout=timeout)
    if captured.returncode != 0 and not captured.output.strip():
        recovered = run_command(
            ["docker", "exec", container, "cat", log_path],
            timeout=15.0,
        )
        text = recovered.output.strip()
        if text:
            captured = CapturedRun(returncode=captured.returncode, output=text)
        else:
            captured = CapturedRun(
                returncode=captured.returncode,
                output=f"(no captured stdout/stderr; {describe_returncode(captured.returncode)})\n",
            )
    return captured


def local_harness(
    argv: Sequence[str],
    *,
    cwd: str | os.PathLike[str] | None = None,
    env: Mapping[str, str] | None = None,
    timeout: float = 180.0,
) -> CapturedRun:
    captured = run_command(argv, cwd=cwd, env=env, timeout=timeout)
    if captured.returncode != 0 and not captured.output.strip():
        captured = CapturedRun(
            returncode=captured.returncode,
            output=f"(no captured stdout/stderr; {describe_returncode(captured.returncode)})\n",
        )
    return captured
