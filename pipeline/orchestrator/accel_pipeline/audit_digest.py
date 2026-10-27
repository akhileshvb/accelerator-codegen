"""What the audit session is handed instead of re-reading the query's directory, all built
without a model: a digest (file map, entry points, the chosen design, the experiment log, open
findings) and the cumulative diff since implementation began. Also the audit's blocking findings."""

from __future__ import annotations

import hashlib
import json
import re
import subprocess
from pathlib import Path
from typing import Any

from accel_pipeline.analysis_context import analysis_for_audit
from accel_pipeline.gates.build_stamp import git, is_relevant
from accel_pipeline.config import PipelineConfig
from accel_pipeline.workload import analysis_artifact_path, gates_dir_for_query, query_artifacts_dir
from accel_pipeline.jsonio import read_json
from accel_pipeline.layout import query_dir as query_dir_of

IMPLEMENTATION_BASE_FILE = "implementation_base.txt"
AUDIT_FINDINGS_FILE = "audit_open_findings.md"
DIFF_LIMIT_CHARS = 120_000
MAX_SYMBOLS_PER_FILE = 40
LOG_TAIL_LINES = 25

_SOURCE_SUFFIXES = {".hpp", ".h", ".cpp", ".cc", ".inc", ".py", ".sh", ".cmake", ".json", ".rs", ".md", ".txt", ".sql"}
_ROLE_BY_NAME = {
    "module.hpp": "state and module declarations, public entry points",
    "module.cpp": "module implementation (build, execute, maintenance)",
    "finalize.cpp": "module test (unit contract)",
}
_ROLE_BY_DIR = {
    "tests": "tests", "bench": "benchmark code", "config": "cost-model and calibration config",
    "verus_kit": "verified kernel kit",
}
_TYPE_DECL = re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct|enum(?:\s+class)?)\s+(\w+)[^;]*$")
_FUNCTION_DECL = re.compile(r"^[\w:<>,*&\s~]+?\b(\w+)\s*\([^;]*\)[^;]*;?$")
_NOT_FUNCTIONS = frozenset({"if", "for", "while", "switch", "return", "sizeof", "static_assert", "using", "typedef"})


def record_implementation_base(repo_root: Path, art: Path) -> None:
    """Remember the commit the implementation starts from; a resumed run keeps the first one."""
    marker = art / IMPLEMENTATION_BASE_FILE
    if not marker.is_file():
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(git(repo_root, "rev-parse", "HEAD").decode().strip(), encoding="utf-8")


def _lines(repo_root: Path, *args: str) -> list[str]:
    try:
        return [line for line in git(repo_root, *args).decode("utf-8", "replace").split("\0" if "-z" in args else "\n") if line]
    except (OSError, subprocess.CalledProcessError):
        return []


def changed_files(repo_root: Path, base: str, query_dir: str) -> list[str]:
    """Files that differ from `base` (committed, uncommitted or new) and can affect this query."""
    paths = set(_lines(repo_root, "diff", "--name-only", "-z", base)) | set(
        _lines(repo_root, "ls-files", "--others", "--exclude-standard", "-z"))
    return sorted(p for p in paths if is_relevant(p, query_dir))


def snapshot(repo_root: Path, paths: list[str]) -> dict[str, str]:
    """path -> content hash, to tell later which files a repair round touched."""
    return {p: hashlib.sha256((repo_root / p).read_bytes()).hexdigest() if (repo_root / p).is_file() else "deleted"
            for p in paths}


def touched_since(repo_root: Path, before: dict[str, str], base: str, query_dir: str) -> list[str]:
    after = snapshot(repo_root, changed_files(repo_root, base, query_dir))
    return sorted(p for p in before.keys() | after.keys() if before.get(p) != after.get(p))


def cumulative_diff(repo_root: Path, base: str, paths: list[str], *, limit: int = DIFF_LIMIT_CHARS) -> str:
    """The diff of `paths` against `base` (new files as additions), the file order given, up to `limit`
    characters; files that no longer fit are listed with their size."""
    tracked = set(_lines(repo_root, "ls-files", "-z", "--", *paths)) if paths else set()
    parts: list[str] = []
    used = 0
    for path in paths:
        if path in tracked or not (repo_root / path).is_file():
            cmd = ["git", "diff", "--no-color", base, "--", path]
        else:
            cmd = ["git", "diff", "--no-color", "--no-index", "--", "/dev/null", path]
        text = subprocess.run(cmd, cwd=repo_root, capture_output=True, text=True, errors="replace").stdout
        if used + len(text) > limit:
            parts.append(f"# {path}: diff omitted ({text.count(chr(10))} lines; read the file)")
            continue
        parts.append(text.rstrip())
        used += len(text)
    return "\n".join(part for part in parts if part)


def _strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.DOTALL)


def public_symbols(header: str) -> list[str]:
    """Namespace-level types and function signatures of a C++ header, one line each."""
    symbols: list[str] = []
    depth = 0
    for line in _strip_comments(header).splitlines():
        stripped = line.strip()
        if depth == 0 and stripped and not stripped.startswith(("#", "namespace", "extern", "}")):
            if (m := _TYPE_DECL.match(line)) and not stripped.endswith(";"):
                symbols.append(f"type {m.group(1)}")
            elif (m := _FUNCTION_DECL.match(line.split("{", 1)[0].rstrip())) and m.group(1) not in _NOT_FUNCTIONS \
                    and stripped.endswith((";", "{", "}")):
                symbols.append(" ".join(stripped.split()).rstrip(";")[:120])
        opens = 0 if stripped.startswith("namespace") or stripped.startswith('extern "C"') else line.count("{")
        closes = line.count("}") if depth > 0 or opens else 0
        depth = max(0, depth + opens - closes)
    return symbols[:MAX_SYMBOLS_PER_FILE]


def _first_comment(text: str) -> str:
    for line in text.splitlines()[:8]:
        stripped = line.strip()
        if stripped.startswith(("//", "/*", "#", '"""')) and not stripped.startswith(("#include", "#pragma", "#!")):
            return stripped.lstrip("/*#\"' ").rstrip("*/\"' ")[:100]
    return ""


def _role(path: str, declared: dict[str, str], text: str) -> str:
    for suffix, role in declared.items():
        if path.endswith(suffix):
            return role
    rel = Path(path)
    return (_first_comment(text) or _ROLE_BY_NAME.get(rel.name)
            or next((role for part in rel.parts if (role := _ROLE_BY_DIR.get(part))), "") or "source")


def _file_map(repo_root: Path, query_dir: str, declared: dict[str, str]) -> tuple[list[str], list[str]]:
    files = sorted(set(_lines(repo_root, "ls-files", "-z", "--", query_dir))
                   | set(_lines(repo_root, "ls-files", "--others", "--exclude-standard", "-z", "--", query_dir)))
    rows, entry_points = [], []
    for path in files:
        file = repo_root / path
        if not file.is_file() or file.suffix not in _SOURCE_SUFFIXES:
            continue
        text = file.read_text(encoding="utf-8", errors="replace")
        rel = path.removeprefix(query_dir.rstrip("/") + "/")
        rows.append(f"- `{rel}` ({text.count(chr(10))} lines): {_role(rel, declared, text)}")
        if file.suffix in {".hpp", ".h"} and (symbols := public_symbols(text)):
            entry_points += [f"- `{rel}`: `{symbol}`" for symbol in symbols]
    return rows, entry_points


def _design(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> tuple[list[str], dict[str, str]]:
    path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
    analysis = read_json(path)
    if not analysis:
        return ["- (no analysis on disk)"], {}
    audit_view = analysis_for_audit(analysis)
    declared = {str(f["path"]): str(f["role"]) for f in audit_view["files_to_create"] if f.get("path") and f.get("role")}
    lines = [f"- selected candidate: `{audit_view.get('selected_candidate_id')}`"]
    lines += [f"- {key}: {value}" for key, value in (audit_view.get("state_design_summary") or {}).items()
              if key != "selected_candidate_id" and value]
    if audit_view.get("dependent_table_names"):
        lines.append(f"- dependent tables: {', '.join(audit_view['dependent_table_names'])}")
    lines.append(f"- mutation_coverage entries: {audit_view.get('mutation_coverage_entries')}")
    return lines, declared


def _log_summary(art: Path) -> list[str]:
    log = art / "experiment_log.md"
    if not log.is_file():
        return ["- (no experiment log)"]
    lines = [line for line in log.read_text(encoding="utf-8").splitlines() if line.strip()]
    repairs = sum("repair[" in line for line in lines)
    out = [f"{len(lines)} lines, {repairs} repair decisions. Last {min(LOG_TAIL_LINES, len(lines))}:"]
    return out + [f"  {line[:240]}" for line in lines[-LOG_TAIL_LINES:]]


def _open_findings(art: Path) -> list[str]:
    out: list[str] = []
    quarantine = art / "quarantine.json"
    if quarantine.is_file():
        out += [f"- gate `{q.get('gate')}` quarantined ({q.get('infra_reason')}) in {q.get('stage')}"
                for q in json.loads(quarantine.read_text(encoding="utf-8"))]
    for result in sorted(gates_dir_for_query(art).glob("*.json")):
        data = read_json(result)
        status = data.get("status") if isinstance(data, dict) else None  # not every file there is a gate result
        if status not in (None, "pass", "skip", "cancelled"):
            out.append(f"- gate `{result.stem}` last result: {status}")
    summary = read_json(art / "aux_bench_summary.json")
    if summary.get("measurement_ok") is False:
        out.append(f"- the last bench sweep was not trustworthy: {'; '.join(summary.get('measurement_diagnosis') or [])[:300]}")
    if (art / AUDIT_FINDINGS_FILE).is_file():
        out.append(f"- earlier audit findings left open: see `{AUDIT_FINDINGS_FILE}`")
    return out or ["- none recorded"]


def query_digest(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> str:
    """Markdown digest of the query's accelerator, from files and logs only."""
    bench = str(workload.get("benchmark_name") or "tpch")
    query_dir = query_dir_of(bench, query)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    design, declared = _design(pipeline_config, workload, query)
    files, entry_points = _file_map(pipeline_config.repo_root, query_dir, declared)
    sections = [
        ("File map", files or ["- (no files)"]),
        ("Public entry points (namespace-level declarations of the headers)", entry_points or ["- none found"]),
        ("Chosen design (from the analysis)", design),
        ("Experiment log", _log_summary(art)),
        ("Open findings", _open_findings(art)),
    ]
    return f"# Query digest: Q{query} (`{query_dir}`)\n\n" + "\n\n".join(
        f"## {title}\n" + "\n".join(lines) for title, lines in sections)


def audit_context(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> str:
    """The digest plus the cumulative diff since implementation began, as one user-message block."""
    repo_root = pipeline_config.repo_root
    art = query_artifacts_dir(repo_root, query, workload=workload)
    query_dir = query_dir_of(str(workload.get("benchmark_name") or "tpch"), query)
    marker = art / IMPLEMENTATION_BASE_FILE
    base = marker.read_text(encoding="utf-8").strip() if marker.is_file() else "HEAD"
    paths = changed_files(repo_root, base, query_dir)
    paths.sort(key=lambda p: (not p.startswith(query_dir + "/"), p))  # the query's own files first
    diff = cumulative_diff(repo_root, base, paths)
    digest = query_digest(pipeline_config, workload, query)
    from accel_pipeline.optimization.structural import design_revisions_markdown

    return (f"{digest}\n\n{design_revisions_markdown(art)}\n"
            f"## Cumulative diff since implementation began ({len(paths)} files; base `{base[:12]}`)\n\n"
            f"```diff\n{diff or '(no changes)'}\n```\n")


_VERDICTS_AGAINST = frozenset({"fail", "failed", "false"})


def _as_text(item: Any) -> str:
    return item if isinstance(item, str) else json.dumps(item, sort_keys=True)


def blocking_findings(audit_result: dict[str, Any]) -> list[str]:
    """What the audit says must be fixed: a failed correctness verdict, generality failures, COW
    findings, failed large-scale classes and a suspected fairness violation. Risks and notes are not blocking."""
    scale = audit_result.get("scale_review") or {}
    findings: list[str] = []
    if audit_result.get("correctness_ok") is False:
        findings.append("the audit's verdict is correctness_ok=false")
    findings += [f"generality failure: {_as_text(i)}" for i in scale.get("generality_failures") or []]
    findings += [f"copy-on-write finding: {_as_text(i)}" for i in scale.get("cow_findings") or []]
    for entry in scale.get("large_scale") or []:
        verdicts = (str(v).lower() for v in entry.values()) if isinstance(entry, dict) else iter(())
        if any(v in _VERDICTS_AGAINST for v in verdicts):
            findings.append(f"large-scale class failed: {_as_text(entry)}")
    fairness = audit_result.get("fairness_review") or {}
    if fairness.get("cheating_suspected"):
        findings += [f"fairness: {_as_text(i)}" for i in fairness.get("issues") or ["cheating suspected"]]
    return findings


def numbered(findings: list[str]) -> str:
    return "\n".join(f"{i}. {finding}" for i, finding in enumerate(findings, 1))
