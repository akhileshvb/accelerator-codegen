"""Tells infrastructure failures from code failures, from the failure's own text (engine-generic)."""

from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path
from typing import Any, Mapping

from accel_pipeline import measurement_lease
from accel_pipeline.layout import query_dir

_TEXT_KEYS = ("log_tail", "stderr", "stdout", "notes", "reason")

_SANITIZER_REPORT = re.compile(r"(?:WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer)")
_STACK_FRAME = re.compile(r"^\s*#\d+ .*? (?:in )?\S*?(/\S+?\.(?:c|cc|cpp|cxx|h|hh|hpp|rs)):\d+", re.MULTILINE)
_RACY = re.compile(r"failed on run (\d+) after passing (\d+) times")
_TIMEOUT = re.compile(r"did not finish within|timed out after")
_TIMEOUT_RC = frozenset({124, 137})
_LEASE_BUSY = measurement_lease.BUSY
_QUEUED_SEC = 5.0  # a wait for the measurement lease this long means other benchmarks were running
_MISSING_PRODUCER = re.compile(
    r"(?:analysis\.json|suite\.yaml)[^\n]*(?:not found|missing|no such file)"
    r"|(?:not found|missing|no such file)[^\n]*(?:analysis\.json|suite\.yaml)"
    r"|target missing from the gate context",
    re.IGNORECASE,
)
_HARNESS_ERROR = re.compile(
    r"unbound variable"
    r"|Traceback \(most recent call last\):\n(?:[ \t]+.*\n)*?[ \t]+File \"[^\"]*(?:gates/[^\"]*\.py|read_gate_context\.py)\""
    r"|host\.sh[^\n]*(?:exit(?:ed)?(?: with)?(?: code| status)?[ =:]+2\b|returned 2\b)",
    re.IGNORECASE,
)
# The test_suite validation runs on the registration the pipeline scaffolded: a failure to find or serve it is the
# pipeline's, never the tester's (who owns tests/workload only).
_REGISTRATION_ERROR = re.compile(r"unsupported shared query|Table Function with name \S+ does not exist", re.IGNORECASE)
_TRANSIENT_PROVIDER = (
    "stream disconnected", "error sending request", "connection refused", "cannot connect to host",
    "timed out", "timeout", "temporarily unavailable", "503", "502", "429", "rate limit", "reconnecting...",
)


def is_transient_provider_failure(reason: str | None) -> bool:
    """True when the agent never ran (network/provider flake), not a real hyp fail."""
    text = (reason or "").lower()
    return bool(text) and any(needle in text for needle in _TRANSIENT_PROVIDER)


def failure_text(data: Mapping[str, Any]) -> str:
    return "\n".join(str(data[k]) for k in _TEXT_KEYS if data.get(k))


def _returncode(data: Mapping[str, Any]) -> int | None:
    rc = data.get("returncode", data.get("exit_code"))
    return int(rc) if rc is not None and str(rc).lstrip("-").isdigit() else None


def _changed_files(repo_root: Path) -> set[str]:
    cmds = (["git", "diff", "--name-only", "HEAD"], ["git", "ls-files", "--others", "--exclude-standard"])
    changed: set[str] = set()
    for cmd in cmds:
        try:
            out = subprocess.run(cmd, cwd=repo_root, capture_output=True, text=True, timeout=60).stdout
        except (OSError, subprocess.SubprocessError):
            continue
        changed.update(line.strip() for line in out.splitlines() if line.strip())
    return changed


def racy_repeats(data: Mapping[str, Any]) -> tuple[int, int] | None:
    """(passed, total) of a gate that repeats its harness and failed on a later repeat, else None."""
    match = _RACY.search(failure_text(data))
    if match is None:
        return None
    failed_on, passed = int(match.group(1)), int(match.group(2))
    return passed, failed_on


def _own_paths(workload: Mapping[str, Any], query: int | None, repo_root: Path) -> set[str]:
    """What counts as the failing code's own: files changed this stage plus the stage's home, a
    query's directory or an engine backend's tree."""
    ours = set(_changed_files(repo_root))
    if workload.get("workload_kind") == "engine_backend":
        ours.add(f"backends/{workload.get('engine_name') or ''}/")
    elif query is not None:
        ours.add(query_dir(str(workload.get("benchmark_name") or "tpch"), query) + "/")
    return ours


def _engine_internal_sanitizer(text: str, workload: Mapping[str, Any], query: int | None, repo_root: Path | None) -> bool:
    """A sanitizer report none of whose frames is in the failing code's own files."""
    if repo_root is None or not _SANITIZER_REPORT.search(text):
        return False
    frames = _STACK_FRAME.findall(text)
    if not frames:
        return False
    ours = _own_paths(workload, query, repo_root)
    return not any(rel in frame for frame in frames for rel in ours)


def _queued_for_lease(text: str) -> bool:
    waited = measurement_lease.waited_seconds(text)
    return waited is not None and waited >= _QUEUED_SEC


def _loaded(repo_root: Path | None, load_factor: float, text: str = "") -> bool:
    """The machine is oversubscribed, the failing run queued for the measurement lease, or another
    worker holds the measurement lease or the shared-tree lease."""
    try:
        if os.getloadavg()[0] > load_factor * (os.cpu_count() or 1):
            return True
    except OSError:
        pass
    if _queued_for_lease(text) or measurement_lease.held_by_other(os.environ.get("ACCEL_INSTANCE_TAG", "")):
        return True
    if repo_root is None:
        return False
    from accel_pipeline.shared_file_lease import SharedFileLease, worker_id_from_env

    holder = SharedFileLease(repo_root).holder()
    return holder is not None and holder != worker_id_from_env("")


def classify(
    gate_name: str,
    data: Mapping[str, Any],
    workload: Mapping[str, Any],
    repo_root: Path | None,
    *,
    query: int | None = None,
    load_factor: float = 1.5,
) -> tuple[str, str | None]:
    """(kind, infra_reason) of a failed gate: kind is "infra" or "code"."""
    text = failure_text(data)
    if query is None and workload.get("query_number") is not None:
        query = int(workload["query_number"])
    if _HARNESS_ERROR.search(text):
        return "infra", "harness_error"
    if _MISSING_PRODUCER.search(text):
        return "infra", "missing_producer"
    if gate_name == "test_suite_validate" and _REGISTRATION_ERROR.search(text):
        return "infra", "harness_error"
    from accel_pipeline.infra_fail_fast import classify_infra_failure

    shared = classify_infra_failure(text, query=query if query is not None else -1)
    if shared:
        return "infra", shared
    if _engine_internal_sanitizer(text, workload, query, repo_root):
        return "infra", "engine_internal_sanitizer"
    if _LEASE_BUSY.search(text):
        return "infra", "measurement_lease_busy"
    if (_TIMEOUT.search(text) or _returncode(data) in _TIMEOUT_RC) and _loaded(repo_root, load_factor, text):
        return "infra", "timeout_under_load"
    return "code", None
