"""A failure any stage can hand to the repair loop (accel_pipeline.repair), and how a failed gate is told infra from code."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any, Literal, Mapping

from accel_pipeline.layout import query_dir
from accel_pipeline.paths import query_artifacts_dir

if TYPE_CHECKING:
    from accel_pipeline.gates.run import GateResult


PROVIDER_FLAKE = "provider_flake"  # the infra_reason of a network or provider error: the agent never ran
AGENT_BLOCKER = "agent_reported_blocker"  # the infra_reason of a blocker the agent handed to the operator
FailureKind = Literal["code", "infra", "timeout", "measurement", "orchestrator"]


@dataclass
class Failure:
    kind: FailureKind
    stage: str
    source: str
    summary: str
    evidence: str = ""
    reproduce: str = ""
    infra_reason: str | None = None
    diagnosis: list[str] = field(default_factory=list)


class StageFailure(Exception):
    def __init__(self, failure: Failure):
        super().__init__(failure.summary)
        self.failure = failure


def render_failure(f: Failure) -> str:
    """The markdown report fed to the agent."""
    lines = [f"## {f.kind} failure in {f.stage} ({f.source})", "", f.summary]
    if f.diagnosis:
        lines += ["", *(f"- {d}" for d in f.diagnosis)]
    if f.reproduce:
        lines += ["", f"Reproduce: `{f.reproduce}`"]
    if f.evidence:
        lines += ["", "```", f.evidence[-6000:], "```"]
    return "\n".join(lines)


_CRASH_SIGNALS = re.compile(
    r"(?i)(segmentation fault|sigsegv|sigabrt|internalexception|"
    r"assertion failed|stack trace|sanitizer|core dumped|"
    r"undefined reference|error:|fatal error:|terminate called|"
    r"tests passed.*failed|test.*failed)"
)


def extract_crash_highlights(log_tail: str, *, max_lines: int = 40) -> list[str]:
    if not log_tail:
        return []
    lines = log_tail.splitlines()
    highlighted = [line.strip() for line in lines if _CRASH_SIGNALS.search(line)]
    if highlighted:
        return highlighted[-max_lines:]
    return [line.strip() for line in lines[-max_lines:] if line.strip()]


def gate_log_tail(gate_data: dict[str, Any], *, max_chars: int = 6000) -> str:
    for key in ("log_tail", "stderr", "stdout", "notes", "reason"):
        text = str(gate_data.get(key) or "").strip()
        if text:
            return text[-max_chars:]
    return ""


def failure_from_gate(
    result: GateResult,
    *,
    stage: str,
    workload: dict[str, Any] | None = None,
    repo_root: Path | None = None,
    load_factor: float = 1.5,
) -> Failure:
    """A failed gate as a Failure, triaged as infra or code."""
    from accel_pipeline.gates.run import gate_failure_message
    workload = workload or {}
    kind, reason = classify(result.name, result.data, workload, repo_root, load_factor=load_factor)
    tail = gate_log_tail(result.data)
    reproduce = str(result.data.get("reproduce") or "")
    if not reproduce and workload.get("test_target"):
        reproduce = (f"cmake --build build-release --target {workload['test_target']} -j$(nproc) && "
                     f"ctest -R {workload.get('ctest_smoke_regex', '')}")
    return Failure(
        kind=kind,  # type: ignore[arg-type]
        stage=stage,
        source=f"gate:{result.name}",
        summary=gate_failure_message(result),
        evidence=tail,
        reproduce=reproduce,
        infra_reason=reason,
        diagnosis=_racy_diagnosis(result.data) + [f"`{line}`" for line in extract_crash_highlights(tail, max_lines=15)],
    )


def _racy_diagnosis(data: dict[str, Any]) -> list[str]:
    repeats = racy_repeats(data)
    if repeats is None:
        return []
    passed, failed_on = repeats
    return [f"racy: the harness passed {passed} of {failed_on} repeats. A timing-dependent failure is a real "
            "defect (a missing lock, a wait with no bound, an unsynchronised read), not infrastructure: find the "
            "shared state and fix it. Rerunning until it passes proves nothing."]


_TEXT_KEYS = ("log_tail", "stderr", "stdout", "notes", "reason")

_SANITIZER_REPORT = re.compile(r"(?:WARNING: ThreadSanitizer|ERROR: AddressSanitizer|ERROR: LeakSanitizer)")
_STACK_FRAME = re.compile(r"^\s*#\d+ .*? (?:in )?\S*?(/\S+?\.(?:c|cc|cpp|cxx|h|hh|hpp|rs)):\d+", re.MULTILINE)
_RACY = re.compile(r"failed on run (\d+) after passing (\d+) times")
_TIMEOUT = re.compile(r"did not finish within|timed out after")
_TIMEOUT_RC = frozenset({124, 137})
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
    from accel_pipeline.pack import measurement_lease

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
    from accel_pipeline.pack import measurement_lease

    if _queued_for_lease(text) or measurement_lease.held_by_other(os.environ.get("ACCEL_INSTANCE_TAG", "")):
        return True
    if repo_root is None:
        return False
    from accel_pipeline.pack.shared_file_lease import SharedFileLease, worker_id_from_env

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
    from accel_pipeline.pack import measurement_lease

    text = failure_text(data)
    if query is None and workload.get("query_number") is not None:
        query = int(workload["query_number"])
    if _HARNESS_ERROR.search(text):
        return "infra", "harness_error"
    if _MISSING_PRODUCER.search(text):
        return "infra", "missing_producer"
    if gate_name == "test_suite_validate" and _REGISTRATION_ERROR.search(text):
        return "infra", "harness_error"
    shared = classify_infra_failure(text, query=query if query is not None else -1)
    if shared:
        return "infra", shared
    if _engine_internal_sanitizer(text, workload, query, repo_root):
        return "infra", "engine_internal_sanitizer"
    if measurement_lease.BUSY.search(text):
        return "infra", "measurement_lease_busy"
    if (_TIMEOUT.search(text) or _returncode(data) in _TIMEOUT_RC) and _loaded(repo_root, load_factor, text):
        return "infra", "timeout_under_load"
    return "code", None


_UNSUPPORTED = re.compile(r"unsupported shared query(?:\s+binding)?", re.IGNORECASE)
_REBUILD_REVERT = re.compile(r"rebuild after (?:revert|restore) failed", re.IGNORECASE)
_FOREIGN_COMPILE = re.compile(
    r"(?:error|fatal error):.*?accelerators/(?:tpch|sec_edgar)/q(\d+)/",
    re.IGNORECASE | re.DOTALL,
)
_FOREIGN_COMPILE_PATH = re.compile(
    r"accelerators/(?:tpch|sec_edgar)/q(\d+)/[^\s:]+",
    re.IGNORECASE,
)


def classify_infra_failure(text: str, *, query: int) -> str | None:
    """Return a short infra reason if ``text`` is a shared-tree / catalog failure."""
    if not text:
        return None
    if _UNSUPPORTED.search(text):
        return "unsupported_shared_query"
    if _REBUILD_REVERT.search(text):
        return "rebuild_after_revert_failed"
    # Prefer the path on an actual error/fatal/undefined line — not every TU
    # mentioned while cmake compiles peers in the same shared library.
    foreign_from_errors: set[int] = set()
    for line in text.splitlines():
        low = line.lower()
        if not (
            "error:" in low
            or "fatal error:" in low
            or "undefined reference" in low
        ):
            continue
        for m in _FOREIGN_COMPILE_PATH.finditer(line):
            foreign_from_errors.add(int(m.group(1)))
    foreign_from_errors.discard(int(query))
    if foreign_from_errors:
        return f"foreign_file_compile:q{sorted(foreign_from_errors)[0]}"
    m = _FOREIGN_COMPILE.search(text)
    if m and int(m.group(1)) != int(query):
        return f"foreign_file_compile:q{m.group(1)}"
    return None


def release_shared_lease(repo_root: Path, *, query: int, worker_id: str | None = None) -> bool:
    from accel_pipeline.pack.shared_file_lease import release_sticky_lease, worker_id_from_env

    holder = worker_id or worker_id_from_env(f"q{query}")
    return bool(release_sticky_lease(repo_root, holder=holder))


def restore_aux_from_snapshot(repo_root: Path, query: int, workload: dict[str, Any] | None = None) -> list[str]:
    art = query_artifacts_dir(repo_root, query, workload=workload)
    # The snapshot mirrors repo-relative paths; everything a query owns (aux module,
    # tests, cost model under config/) lives in its directory accelerators/<bench>/q<N>/.
    snap_root = art / "best_valid_baseline_snapshot"
    bench = str((workload or {}).get("benchmark_name") or "tpch")
    rel_q = Path("accelerators") / bench / f"q{query}"
    snap = snap_root / rel_q
    restored: list[str] = []
    if not snap.is_dir():
        return restored
    for src in sorted(snap.rglob("*")):
        if src.is_file():
            dest = repo_root / src.relative_to(snap_root)
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, dest)
            restored.append(str(dest.relative_to(repo_root)))
    return restored


def repair_foreign_query_sources(
    repo_root: Path,
    *,
    owner_query: int,
    pack_queries: list[int] | None = None,
    only_queries: list[int] | None = None,
) -> dict[str, Any]:
    """Restore peer aux sources from best_valid snapshots.

    Prefer ``only_queries`` (e.g. the single foreign id from a compile error).
    Blasting every pack query wipes concurrent workers' in-progress edits.
    """
    if only_queries is not None:
        targets = list(only_queries)
    else:
        targets = list(pack_queries or [])
    if not targets and only_queries is None:
        # Conservative default: do not scan the whole artifacts tree under pack
        # concurrency — caller should pass an explicit foreign id.
        targets = []
    restored: dict[str, list[str]] = {}
    for q in sorted(set(targets)):
        if int(q) == int(owner_query):
            continue
        files = restore_aux_from_snapshot(repo_root, int(q))
        if files:
            restored[f"q{q}"] = files
    return {"owner_query": owner_query, "restored": restored}


def handle_infra_failure(
    pipeline_config: Any,
    workload: dict[str, Any],
    query: int,
    *,
    text: str,
    pack_queries: list[int] | None = None,
) -> dict[str, Any] | None:
    """If ``text`` is an infra failure, repair tree + release lease and return info."""
    reason = classify_infra_failure(text, query=query)
    if not reason:
        return None
    repo_root = Path(pipeline_config.repo_root)
    worker_id = getattr(pipeline_config, "worker_id", None)
    released = release_shared_lease(repo_root, query=query, worker_id=worker_id)

    only: list[int] | None = None
    m = re.match(r"foreign_file_compile:q(\d+)$", reason)
    if m:
        only = [int(m.group(1))]
    elif reason == "unsupported_shared_query":
        # Catalog desync — do not mass-restore peers; release lease + own restore only.
        only = []
    elif pack_queries:
        only = list(pack_queries)

    repair = repair_foreign_query_sources(
        repo_root, owner_query=query, pack_queries=pack_queries, only_queries=only
    )
    # Restore this query from its own baseline so a bad mid-edit does not linger.
    own = restore_aux_from_snapshot(repo_root, query, workload=workload)
    if own:
        repair.setdefault("restored", {})[f"q{query}_own"] = own
    return {
        "infra_reason": reason,
        "lease_released": released,
        "repair": repair,
        "fail_fast": True,
    }
