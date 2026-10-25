"""Defects query runs find in an engine's backend or harnesses reach the run that owns them.

A query run never edits `backends/<engine>/`. It appends what it found to
`pipeline/artifacts/backend_additions/<engine>.jsonl` (query_host_bootstrap.md, Ownership), one
JSON object per line with a `scope`: "harness" (the workload harnesses: `host_bootstrap` fixes
it) or "backend" (the engine backend: its `validation` stage re-runs with it as a required fix).
A request without a scope goes to both. Each owner records how many of its requests it has
handled, so a request is acted on once.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.paths import engine_artifacts_dir, pipeline_root


def requests_path(repo_root: Path, engine: str) -> Path:
    return pipeline_root(repo_root) / "artifacts" / "backend_additions" / f"{engine}.jsonl"


def _requests(repo_root: Path, engine: str, scope: str) -> list[dict[str, Any]]:
    path = requests_path(repo_root, engine)
    if not path.is_file():
        return []
    out = []
    for line in path.read_text(encoding="utf-8").splitlines():
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            continue
        if isinstance(request, dict) and _route(request.get("scope"), scope):
            out.append(request)
    return out


def _route(declared: Any, scope: str) -> bool:
    """A request reaches the harness run only when it says "harness"; any other scope (agents also write
    descriptive ones, e.g. "runtime registration") is the backend's; a request without one goes to both."""
    if declared is None:
        return True
    return (str(declared).strip().lower() == "harness") == (scope == "harness")


def _done_path(repo_root: Path, engine: str, scope: str) -> Path:
    return engine_artifacts_dir(repo_root, engine) / f"{scope}_requests_done.json"


def pending(repo_root: Path, engine: str, scope: str) -> list[dict[str, Any]]:
    done = _done_path(repo_root, engine, scope)
    handled = int(json.loads(done.read_text(encoding="utf-8")).get("count", 0)) if done.is_file() else 0
    return _requests(repo_root, engine, scope)[handled:]


def mark_handled(repo_root: Path, engine: str, scope: str) -> None:
    count = len(_requests(repo_root, engine, scope))
    _done_path(repo_root, engine, scope).write_text(json.dumps({"count": count}) + "\n", encoding="utf-8")


def as_required_fixes(requests: list[dict[str, Any]]) -> str:
    return "\n".join(
        f"- **{r.get('helper', 'defect')}** (found by {r.get('benchmark', '?')} Q{r.get('query', '?')}): "
        f"{r.get('reason', '')}"
        for r in requests
    )


def reopen_engine_backend(
    pipeline_config: Any,
    model_config: Any,
    engine: str,
    *,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
) -> bool:
    """Re-run the engine backend's validation with the pending backend requests as required
    fixes (its open findings); True when it ran."""
    requests = pending(pipeline_config.repo_root, engine, "backend")
    if not requests:
        return False
    from accel_pipeline.engine_backend.stage import run_engine_pipeline

    print(f"[pipeline] {len(requests)} backend defect(s) recorded for {engine}: re-running its validation", flush=True)
    findings = engine_artifacts_dir(pipeline_config.repo_root, engine) / "architecture_audit.open_findings.md"
    findings.write_text(
        "## Backend defects found by query runs (required fixes)\n\n" + as_required_fixes(requests) + "\n",
        encoding="utf-8",
    )
    run_engine_pipeline(
        pipeline_config, model_config, f"engine_{engine}", 0, model_overrides or {}, dry_run, stage="validation"
    )
    if not dry_run:
        mark_handled(pipeline_config.repo_root, engine, "backend")
    return True
