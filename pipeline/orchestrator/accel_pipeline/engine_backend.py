"""The engine-backend pipeline and the workload harnesses of an engine.

An engine's backend (`backends/<engine>/`) is built layer by layer in one main agent session (a fresh one
starts from the backend digest past the context cap). A layer's gates and repairs run under the repair loop;
its critic reviews in parallel with the gates and its findings come back as one batched repair round. After the
last layer an architecture audit routes each violation to the layer that owns it, then validation gates run.
Finished steps are marker files, so a relaunch continues where the run stopped.

host_bootstrap writes the engine's workload harnesses once per engine and benchmark, after the backend exists;
query runs hand it harness defects, and backend defects reopen the backend's validation (backend requests).

A critic (layer critique, architecture audit, host_bootstrap critique) is data: a rubric prompt, where its verdict
is written, how it phrases feedback and how many rounds it gets. One runner serves all three.
"""

from __future__ import annotations

import contextlib
import dataclasses
import difflib
import importlib.util
import json
import os
import re
import shutil
import threading
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from accel_pipeline import gates as gate_runner
from accel_pipeline.agent import SessionStore
from accel_pipeline.config import (
    ModelConfig,
    PipelineConfig,
    apply_workload_overrides,
    engine_artifacts_dir,
    harness_targets,
    load_run_workload,
    load_workload,
    pipeline_root,
    query_artifacts_dir,
    sanitize_engine_name,
)
from accel_pipeline.data import prepare_host_sdk_payload
from accel_pipeline.gates import GateResult, format_reloop_failure_sections
from accel_pipeline.mcp import ToolContext, make_gate_tools
from accel_pipeline.pack import apply_foreign_engine_quarantine, restore_quarantine
from accel_pipeline.repair import (
    Failure,
    RepairCtx,
    StageFailure,
    StageStopped,
    failure_from_gate,
    render_failure,
    run_with_repair,
)
from accel_pipeline.runlog import append_experiment_log
from accel_pipeline.stages import run_agent_stage, run_post_stage_gates_collect

MAIN_ROLE = "main"
VALIDATION_ROLE = "validation"
_GATE_LOG_TAIL_CHARS = 8000

# --------------------------------------------------------------------------------------------------------------
# Progress markers


class Progress:
    """What a run has finished, as `<step>.passed` files in its artifacts directory. Anything that changes a
    layer's code clears the markers of the work that judged the earlier code (the audit, the validation gates)."""

    def __init__(self, art: Path) -> None:
        self.art = art

    def _marker(self, name: str) -> Path:
        return self.art / f"{name}.passed"

    def done(self, step: str) -> bool:
        return self._marker(step).exists()

    def mark(self, step: str) -> None:
        self._marker(step).touch()

    def layer_passed(self, layer_id: str) -> bool:
        return self.done(f"layer_{layer_id}")

    def mark_layer(self, layer_id: str) -> None:
        self.mark(f"layer_{layer_id}")

    def gate_passed(self, gate: str) -> bool:
        return self.done(f"validation_gate_{gate}")

    def mark_gate(self, gate: str) -> None:
        self.mark(f"validation_gate_{gate}")

    def reopen_layer(self, layer_id: str) -> None:
        self._marker(f"layer_{layer_id}").unlink(missing_ok=True)
        self._marker("architecture_audit").unlink(missing_ok=True)
        self.invalidate_gates()

    def invalidate_gates(self) -> None:
        for path in self.art.glob("validation_gate_*.passed"):
            path.unlink(missing_ok=True)


# --------------------------------------------------------------------------------------------------------------
# Backend requests: defects query runs find in an engine's backend or harnesses reach the run that owns them.
# A query run never edits `backends/<engine>/`; it appends what it found to
# `pipeline/artifacts/backend_additions/<engine>.jsonl`, one JSON object per line with a `scope`: "harness"
# (host_bootstrap fixes it) or "backend" (the backend's validation re-runs with it as a required fix). Each owner
# records how many of its requests it has handled, so a request is acted on once.

_REQUEST_FIELDS = ("benchmark", "query", "scope", "helper", "reason")


def requests_path(repo_root: Path, engine: str) -> Path:
    return pipeline_root(repo_root) / "artifacts" / "backend_additions" / f"{engine}.jsonl"


def _requests(repo_root: Path, engine: str, scope: str) -> list[dict[str, Any]]:
    """The well-formed requests for `scope`: one that misses a field names no defect its owner could act on. Only
    "harness" reaches the harness run; any other scope (agents also write descriptive ones) is the backend's."""
    path = requests_path(repo_root, engine)
    out = []
    for line in path.read_text(encoding="utf-8").splitlines() if path.is_file() else []:
        try:
            request = json.loads(line)
        except json.JSONDecodeError:
            continue
        if (isinstance(request, dict) and all(request.get(k) not in (None, "") for k in _REQUEST_FIELDS)
                and (str(request["scope"]).strip().lower() == "harness") == (scope == "harness")):
            out.append(request)
    return out


def _handled_path(repo_root: Path, engine: str, scope: str) -> Path:
    return engine_artifacts_dir(repo_root, engine) / f"{scope}_requests_done.json"


def pending(repo_root: Path, engine: str, scope: str) -> list[dict[str, Any]]:
    done = _handled_path(repo_root, engine, scope)
    handled = int(json.loads(done.read_text(encoding="utf-8")).get("count", 0)) if done.is_file() else 0
    return _requests(repo_root, engine, scope)[handled:]


def record(repo_root: Path, engine: str, request: dict[str, Any]) -> None:
    path = requests_path(repo_root, engine)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as out:
        out.write(json.dumps(request) + "\n")


def mark_handled(repo_root: Path, engine: str, scope: str) -> None:
    count = len(_requests(repo_root, engine, scope))
    _handled_path(repo_root, engine, scope).write_text(json.dumps({"count": count}) + "\n", encoding="utf-8")


def as_required_fixes(requests: list[dict[str, Any]]) -> str:
    return "\n".join(
        f"- **{r.get('helper', 'defect')}** (found by {r.get('benchmark', '?')} Q{r.get('query', '?')}): "
        f"{r.get('reason', '')}"
        for r in requests
    )


def checked_backend_requests(repo_root: Path, engine: str) -> list[dict[str, Any]]:
    """The pending backend requests a failing check backs: the ones that reopen the backend."""
    return [r for r in pending(repo_root, engine, "backend") if r.get("check")]


def reopen_engine_backend(pipeline_config: PipelineConfig, model_config: ModelConfig, engine: str, *,
                          model_overrides: dict[str, str] | None = None, dry_run: bool = False) -> bool:
    """Re-run the backend's validation with the pending backend requests a failing check backs (`check`: the gate,
    or a blocked agent) as required fixes; True when it ran. A request without one (a harness wishing for an API) is
    the engine backend pipeline's to weigh, not a reason to reopen the backend inside a query run."""
    requests = checked_backend_requests(pipeline_config.repo_root, engine)
    if not requests:
        return False
    try:
        load_workload(f"engine_{engine}", pipeline_config.repo_root)
    except FileNotFoundError:  # no backend pipeline for this engine: the requests wait for its owner
        return False
    print(f"[pipeline] {len(requests)} backend defect(s) recorded for {engine}: re-running its validation", flush=True)
    findings = engine_artifacts_dir(pipeline_config.repo_root, engine) / "architecture_audit.open_findings.md"
    findings.write_text("## Backend defects found by query runs (required fixes)\n\n"
                        + as_required_fixes(requests) + "\n", encoding="utf-8")
    run_engine_pipeline(pipeline_config, model_config, f"engine_{engine}", 0, model_overrides or {}, dry_run,
                        stage="validation")
    if not dry_run:
        mark_handled(pipeline_config.repo_root, engine, "backend")
    return True


# --------------------------------------------------------------------------------------------------------------
# Backend digest: what an agent starting a layer (or a fresh session after the cap) is told exists, built from
# backends/<engine>/ and the run's artifacts, without a model; and the per-layer snapshots a diff is taken against.

_SOURCE_SUFFIXES = {".hpp", ".cpp", ".h", ".c", ".cc", ".cmake", ".sh", ".py", ".sql", ".md", ".tpl"}
_C_SUFFIXES = {".hpp", ".cpp", ".h", ".c", ".cc", ".tpl"}
_C_COMMENT = re.compile(r"^\s*(?://+|/\*+|\*)\s*(.*?)\s*(?:\*/)?$")
_HASH_COMMENT = re.compile(r"^\s*#+\s*(.*?)\s*$")
_DIR_ROLES = {"api": "accelerator-facing api", "engine": "engine internals", "state": "backend state",
              "tests": "layer harness or test", "bench": "benchmark driver", "host": "host helper"}
_LOG_TAIL_LINES = 25
_MAX_FILES = 120


def backend_dir(repo_root: Path, engine: str) -> Path:
    return repo_root / "backends" / engine


def _sources(root: Path) -> list[Path]:
    return sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in _SOURCE_SUFFIXES) if root.is_dir() else []


def _role(path: Path, root: Path) -> str:
    """The first comment line of the file; else what its directory is for."""
    try:
        comment = _C_COMMENT if path.suffix in _C_SUFFIXES else _HASH_COMMENT
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines()[:12]:
            match = comment.match(line)
            if match and len(match.group(1)) > 8 and not match.group(1).startswith(("GENERATED", "SCAFFOLDING", "!/")):
                return match.group(1)[:110]
    except OSError:
        pass
    parts = path.relative_to(root).parts
    return _DIR_ROLES.get(parts[0], "") if len(parts) > 1 else ""


def _file_map(repo_root: Path, engine: str) -> list[str]:
    root = backend_dir(repo_root, engine)
    files = _sources(root)
    lines = [f"- `{p.relative_to(repo_root)}` ({p.stat().st_size // 1024 + 1} KB): {_role(p, root) or '-'}"
             for p in files[:_MAX_FILES]]
    if len(files) > _MAX_FILES:
        lines.append(f"- ... and {len(files) - _MAX_FILES} more under `backends/{engine}/`")
    return lines or ["- (the backend tree is empty)"]


def _api_symbols(repo_root: Path, engine: str) -> list[str]:
    script = Path(__file__).resolve().parents[2] / "gates" / "check_api_manifest.py"
    manifest = backend_dir(repo_root, engine) / "api" / "README.md"
    if not manifest.is_file() or not script.is_file():
        return []
    spec = importlib.util.spec_from_file_location("_check_api_manifest", script)
    module = importlib.util.module_from_spec(spec)  # type: ignore[arg-type]
    spec.loader.exec_module(module)  # type: ignore[union-attr]
    rows = module.parse_roles(manifest.read_text(encoding="utf-8")) or {}
    return [f"- {role}: " + ", ".join(f"`{s}`" for s in symbols[:14]) + (" ..." if len(symbols) > 14 else "")
            for role, (_headers, symbols) in rows.items() if symbols]


def _decisions(art: Path) -> list[str]:
    lines: list[str] = []
    log = art / "experiment_log.md"
    if log.is_file():
        tail = [ln for ln in log.read_text(encoding="utf-8").splitlines() if ln.strip()][-_LOG_TAIL_LINES:]
        lines += ["Experiment log (tail):", *tail]
    for path in sorted(art.glob("layer_*.open_findings.md")):
        lines += ["", f"Open finding `{path.name}`:", path.read_text(encoding="utf-8").strip()[:1500]]
    return lines


def _next_layer_files(art: Path, layer_id: str) -> list[str]:
    try:
        analysis = json.loads((art / "backend_analysis.json").read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return []
    layer = next((l for l in analysis.get("layers") or [] if isinstance(l, dict) and l.get("id") == layer_id), {})
    return [str(f) for f in layer.get("files") or []]


def backend_digest(repo_root: Path, engine: str, art: Path, *, next_layer: str = "") -> str:
    parts = [f"## Backend digest (`backends/{engine}/`, generated from the tree and the run's artifacts)", "",
             "What exists now. Read a file only when you are about to change it or must match its exact "
             "signatures; this digest replaces a survey of the tree.", "", "### Files", *_file_map(repo_root, engine)]
    if symbols := _api_symbols(repo_root, engine):
        parts += ["", "### Public api symbols per contract role (api/README.md)", *symbols]
    if decisions := _decisions(art):
        parts += ["", "### Decisions and open findings so far", *decisions]
    if likely := (_next_layer_files(art, next_layer) if next_layer else []):
        parts += ["", f"### Files layer `{next_layer}` will most likely touch (analysis sketch)",
                  *(f"- `{f}`" for f in likely)]
    return "\n".join(parts) + "\n\n"


def snapshot_backend(repo_root: Path, engine: str, dest: Path) -> None:
    """Copy the backend's sources to `dest` (replacing it): the baseline of a later layer diff."""
    shutil.rmtree(dest, ignore_errors=True)
    root = backend_dir(repo_root, engine)
    for path in _sources(root):
        target = dest / path.relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, target)
    dest.mkdir(parents=True, exist_ok=True)


def backend_diff(repo_root: Path, engine: str, baseline: Path, *, max_chars: int = 60000) -> str:
    """Unified diff of the backend's sources against a snapshot_backend() baseline."""
    root = backend_dir(repo_root, engine)
    now = {p.relative_to(root): p for p in _sources(root)}
    before = {p.relative_to(baseline): p for p in _sources(baseline)}

    def lines(path: Path | None) -> list[str]:
        return path.read_text(encoding="utf-8", errors="replace").splitlines(keepends=True) if path else []

    text = "".join(
        "".join(difflib.unified_diff(lines(before.get(rel)), lines(now.get(rel)),
                                     f"a/backends/{engine}/{rel}", f"b/backends/{engine}/{rel}", n=3))
        for rel in sorted(set(now) | set(before)) if lines(before.get(rel)) != lines(now.get(rel)))
    return text if len(text) <= max_chars else text[:max_chars] + f"\n... [diff truncated at {max_chars} chars]\n"


_BULLET = re.compile(r"^- \*\*", re.MULTILINE)
_LAYER_IDS = re.compile(r"`(\w+)`")


def layer_contracts_digest(contracts: str, layer_id: str, path: str) -> str:
    """layer_contracts.md cut to one layer: the intro, the cross-layer requirements that name this layer (or all
    layers), and the layer's own section. The rest stays on disk at `path`."""
    sections = re.split(r"^(?=## )", contracts, flags=re.MULTILINE)
    intro = sections[0].split("\n---")[0].strip()
    requirements = next((s for s in sections if s.startswith("## Requirements every backend meets")), "")
    own = next((s for s in sections if re.match(rf"## `{re.escape(layer_id)}`", s)), "")
    kept = []
    for bullet in _BULLET.split(requirements)[1:]:
        header = bullet.split("\n", 1)[0]
        scope = header[header.find("(") : header.find(")") + 1] if "(" in header else ""
        if "all layers" in scope or layer_id in _LAYER_IDS.findall(scope):
            kept.append("- **" + bullet.rstrip())
    parts = [intro, "## Requirements that name this layer", *(kept or ["(none beyond the layer section)"]), own.rstrip(),
             f"Other layers' sections, and the requirements above that do not name this layer: `{path}` (read on demand)."]
    return "\n\n".join(p for p in parts if p)


# --------------------------------------------------------------------------------------------------------------
# Critics. A read-only critic judges finished work and writes a verdict (JSON, status pass|fail, with violations
# and feedback_for_implementation; an .md beside it). A verdict that never becomes valid JSON is recorded as status
# `error`: a critic that cannot answer never stops the run.

@dataclass(frozen=True)
class Critic:
    name: str            # what the verdict is called in logs and in the `error` verdict
    stage: str           # agent stage (the write allowlist is keyed by it)
    role: str            # session role
    prompt: str          # the rubric prompt file
    verdict: Path        # where the verdict is written
    heading: str         # first line of the feedback sent when the verdict fails
    fixes: str           # what the feedback asks of the implementer
    forbidden: str       # what the critic may not do (appended to its last-action instruction)
    rounds_key: str      # workload key: how many rounds the critic gets
    rounds_default: int
    context: dict[str, str] = field(default_factory=dict)
    layer: str = ""      # the layer a layer critic judges
    blocking_only: bool = False  # only code fixes against the backend count (layer critique)

    def rounds(self, workload: dict) -> int:
        return max(0, int(workload.get(self.rounds_key) or self.rounds_default))


def layer_critic(art: Path, layer_id: str, context: dict[str, str]) -> Critic:
    return Critic(
        name=f"layer critique `{layer_id}`", stage=f"critique_{layer_id}", role=f"critic:{layer_id}",
        prompt="05_layer_critique.md", verdict=art / f"layer_critique_{layer_id}.json",
        heading="## Layer critique FAILED — fix before this stage is done",
        fixes=("Fix this layer, including the wiring it needs in files other layers own (plugin registration, host.sh, "
               "runtime binding); change a prior layer's logic only if a real bug there blocks this contract (smallest "
               "patch, keep that prior harness green). Never stop to ask: nobody answers, so make the change. Mid-loop "
               "`request_gate` `engine_build` then this layer's gate until green. Do not weaken the harness to "
               "greenwash."),
        forbidden="Do not edit adapter sources. Do not explore later layers.",
        rounds_key="layer_critique_max_rounds", rounds_default=0, context=context, layer=layer_id, blocking_only=True)


def architecture_critic(art: Path) -> Critic:
    return Critic(
        name="architecture audit", stage="architecture_audit", role="audit", prompt="04_architecture_audit.md",
        verdict=art / "architecture_audit.json", heading="## Architecture audit FAILED — fix before validation",
        fixes=("Resolve every violation named for this layer against `layers.md` + `acceptance_checklist.md`, however "
               "large the change: a missing mechanism or placement is built now, not deferred. A gate that passes "
               "without exercising a violation does not resolve it: first add the harness assertion that fails on it, "
               "then make it pass. Do not weaken the harness to greenwash. Mid-loop MCP `engine_build` plus this "
               "layer's gate until green."),
        forbidden="Do not edit adapters.", rounds_key="architecture_audit_max_rounds", rounds_default=1)


def host_bootstrap_critic(art: Path, repo_root: Path) -> Critic:
    return Critic(
        name="host_bootstrap critique", stage="critique_host_bootstrap", role="critic:host_bootstrap",
        prompt="host_bootstrap_critique.md", verdict=art / "host_bootstrap_critique.json",
        heading="## Host bootstrap critique FAILED — fix before this stage is done",
        fixes=("Fix the query host (SQL entrypoints, differential, CMake). Build through the engine's host.sh "
               "(`request_gate` `host_bootstrap`), not host `cmake --build build-release`. Mid-loop `request_gate` "
               "`host_bootstrap` until green. Do not invent a second adapter tree."),
        forbidden="Do not edit adapter, harness, or CMake sources.",
        rounds_key="host_bootstrap_critique_max_rounds", rounds_default=1,
        context={"bootstrap_artifacts_path": os.path.relpath(art, repo_root)})


def report_problem(path: Path) -> str | None:
    """Why `path` is not a usable report (missing, or not one JSON object); None when it is."""
    if not path.is_file():
        return "missing"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        return f"not valid JSON: {exc}"
    return None if isinstance(data, dict) else "not a JSON object"


def _nudge(path: Path, problem: str) -> str:
    if problem == "missing":
        return (f"STOP EXPLORING. You ended without writing `{path}`. That means this critique/audit failed to "
                "complete. Do not read more sources. Immediately write that JSON (status pass|fail, required "
                f"fields) and the matching `.md` using Codex shell:\n\n  cat > {path} <<'EOF'\n  {{ ... }}\n  EOF\n\n"
                "Then write the `.md` beside it and stop. Do not edit adapter sources.")
    return (f"`{path}` is {problem}. Rewrite it now as exactly one JSON object (status pass|fail, required fields) "
            "with nothing before or after it, then stop. Do not read more sources or edit adapter sources.")


def run_until_artifact(*, artifact: Path, invoke: Callable[[str], Any], attempts: int = 2, dry_run: bool = False) -> Any:
    """Run `invoke(extra_user_message)` until `artifact` is one JSON object (or attempts run out). A run that
    fails (a deadline, a provider error) counts as an attempt without a report."""
    extra, result = "", None
    for _ in range(max(1, attempts)):
        try:
            result = invoke(extra)
        except StageFailure as exc:
            result = exc.failure
        problem = None if dry_run else report_problem(artifact)
        if problem is None:
            return result
        extra = _nudge(artifact, problem)
    return result


def run_critic(critic: Critic, pipeline_config: PipelineConfig, model_config: ModelConfig, workload_name: str,
               query: int, model_overrides: dict[str, str] | None, message: str, *, dry_run: bool = False) -> Any:
    """One critic turn on `message`, repeated with a nudge until its verdict file is valid JSON."""
    critic.verdict.unlink(missing_ok=True)
    task = (f"{message}\n\nYour LAST action must be writing `{critic.verdict}` (pass|fail) and "
            f"`{critic.verdict.with_suffix('.md')}`. Codex shell: `cat > {critic.verdict} <<'EOF' ... EOF`. "
            f"Then stop. {critic.forbidden}")

    def invoke(extra: str) -> Any:
        return run_agent_stage(
            stage=critic.stage, prompt_file=critic.prompt, pipeline_config=pipeline_config,
            model_config=model_config, workload_name=workload_name, query=query, user_message=extra or task,
            model_overrides=model_overrides, dry_run=dry_run, extra_context=critic.context or None,
            session_role=critic.role)

    return run_until_artifact(artifact=critic.verdict, invoke=invoke, dry_run=dry_run)


def read_review(path: Path, *, what: str) -> dict[str, Any]:
    """The report at `path` with a normalised status (`pass`, `fail`), or, when it is missing or unusable, status
    `error` with the reason, written back so the run records it."""
    problem = report_problem(path)
    if problem is None:
        data = json.loads(path.read_text(encoding="utf-8"))
        status = str(data.get("status") or "").strip().lower()
        if status in {"pass", "fail"}:
            data["status"] = status
            return data
        problem = f"has no status pass|fail (got {data.get('status')!r})"
    data = {"status": "error", "summary": f"{what} unavailable: {path.name} {problem}", "violations": [],
            "feedback_for_implementation": ""}
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return data


def is_blocking(finding: Any) -> bool:
    """A code fix the critic requires: not advisory, and not a missing test (those go to the canary requests)."""
    if not isinstance(finding, dict):
        return True
    return finding.get("severity", "blocking") != "advisory" and finding.get("kind", "code_fix") != "needs_canary"


def load_verdict(critic: Critic) -> dict[str, Any]:
    data = read_review(critic.verdict, what=critic.name)
    if not critic.blocking_only:
        return data
    data["layer"] = str(data.get("layer") or critic.layer)
    # A harness gap aimed at the pipeline's own files (gates, contracts) is never a backend violation: the backend
    # agent cannot change them.
    kept, dropped = [], []
    for v in data.get("violations") or []:
        aimed_at_pipeline = isinstance(v, dict) and v.get("rule") == "harness_gap" and any(
            str(p).startswith("pipeline/") for p in v.get("paths") or [])
        (dropped if aimed_at_pipeline else kept).append(v)
    if dropped:
        data["violations"], data["dropped_pipeline_harness_gaps"] = kept, dropped
        if data["status"] == "fail" and not kept:
            data["status"] = "pass"
    findings = data.get("violations") or []
    if data["status"] == "fail" and findings and not any(is_blocking(v) for v in findings):
        data["status"] = "pass"
    return data


def canary_requests(verdict: dict[str, Any]) -> list[dict[str, Any]]:
    return [v for v in verdict.get("violations") or [] if isinstance(v, dict) and v.get("kind") == "needs_canary"]


def _finding(item: dict[str, Any]) -> str:
    """One finding: which rule, where, which contract item, what is wrong, what must hold instead."""
    where = item.get("location") or ", ".join(f"`{p}`" for p in item.get("paths") or [])
    head = f"- **{item.get('rule', 'contract')}**" + (f" (layer `{item['layer']}`)" if item.get("layer") else "")
    head += (f" at `{where}`" if item.get("location") else f" at {where}" if where else "")
    head += f" (contract: {item['contract_item']})" if item.get("contract_item") else ""
    out = f"{head}: {item.get('evidence', '')}"
    return out + (f"\n  - expected: {item['expected']}" if item.get("expected") else "")


def verdict_feedback(critic: Critic, verdict: dict[str, Any], round_idx: int, *, layer: str = "") -> str:
    """A failed verdict as feedback for the implementer; with `layer`, only the violations that layer owns."""
    violations = [
        v for v in verdict.get("violations") or []
        if (not critic.blocking_only or is_blocking(v))
        and (not layer or not isinstance(v, dict) or str(v.get("layer") or layer) == layer)]
    lines = [critic.heading, "", f"- round: {round_idx}"]
    lines += [f"- {key}: `{verdict[key]}`" for key in ("engine_name", "layer") if verdict.get(key)]
    lines += [f"- summary: {verdict.get('summary') or '(none)'}", "", "### Violations"]
    lines += [_finding(v) if isinstance(v, dict) else f"- {v}" for v in violations] or ["- (see feedback below)"]
    lines += ["", "### Required fixes",
              str(verdict.get("feedback_for_implementation") or "").strip()
              or "(critic left feedback_for_implementation empty — re-read violations)", "", critic.fixes]
    return "\n".join(lines)


def _write_feedback(critic: Critic, text: str, round_idx: int, suffix: str = "") -> None:
    name = f"{critic.verdict.stem}_feedback_round{round_idx}{suffix}.md"
    (critic.verdict.parent / name).write_text(text + "\n", encoding="utf-8")


# --------------------------------------------------------------------------------------------------------------
# MCP tools of an engine_backend run: the engine build and the layer gate.

def layer_gate_from_stage(stage: str, workload: dict[str, Any] | None = None) -> str | None:
    """Map `impl_<layer>` to that layer's gate in the workload. None when this is not a layer impl stage."""
    layer_id = str(stage or "")[len("impl_"):].strip() if str(stage or "").startswith("impl_") else ""
    if not layer_id:
        return None
    for layer in (workload or {}).get("implementation_layers") or []:
        if str(layer.get("id") or "") == layer_id:
            return str(layer.get("gate") or "") or None
    return f"engine_{layer_id}"


def enabled_tools(stage: str) -> list[str]:
    """The tools an engine_backend layer stage (impl_<layer>) may call."""
    return ["request_gate", "run_engine_build", "run_layer_gate", "discover_host_sdk", "report_blocker",
            "acquire_shared_lease", "release_shared_lease"]


def callables(ctx: ToolContext) -> dict[str, Callable[..., str]]:
    request_gate, _ = make_gate_tools(ctx)

    def run_engine_build() -> str:
        """MCP TOOL. Official ``engine_build`` gate (host cmake or docker ensure+build)."""
        return request_gate("engine_build")

    def run_layer_gate() -> str:
        """MCP TOOL. Official gate for this impl_* layer (e.g. impl_epochs → engine_epochs)."""
        gate = layer_gate_from_stage(ctx.stage, ctx.workload)
        if not gate:
            return json.dumps({"error": "run_layer_gate is for engine_backend impl_* stages", "stage": ctx.stage,
                               "hint": "Call request_gate with engine_capture / engine_epochs / … or run_engine_build()."},
                              indent=2)
        return request_gate(gate)

    return {"run_engine_build": run_engine_build, "run_layer_gate": run_layer_gate}


def register(tool, call) -> None:
    @tool(read_only=False)
    def run_engine_build() -> str:
        """MCP TOOL. Official engine_build gate (docker ensure+build or host cmake).

        Call this after CMake/adapter changes. On fail, read failure_summary, fix, retry.
        """
        return call("run_engine_build")

    @tool(read_only=False)
    def run_layer_gate() -> str:
        """MCP TOOL. Official harness gate for this impl_* layer.

        Maps impl_capture→engine_capture, impl_epochs→engine_epochs, etc. Call after
        run_engine_build. On fail, read failure_summary, fix, retry until pass.
        """
        return call("run_layer_gate")


# --------------------------------------------------------------------------------------------------------------
# The layered stage

def implementation_layers(workload: dict) -> list[dict[str, Any]]:
    return [dict(layer) for layer in (workload.get("implementation_layers") or [])]


def layer_gates(layer: dict) -> list[str]:
    return [g for g in ("engine_build", str(layer.get("gate") or ""), *(layer.get("extra_gates") or [])) if g]


def _layer_should_run(workload: dict, layer: dict) -> bool:
    if str(layer.get("skip_unless") or "") == "requires_in_process_host":
        return bool(workload.get("requires_in_process_host", False))
    return True


def _prepare_engine_host_sdk(pipeline_config: PipelineConfig, workload: dict, query: int) -> None:
    payload = prepare_host_sdk_payload(workload, pipeline_config.repo_root, pipeline_config.build_dir)
    if payload is None:
        return
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    art.mkdir(parents=True, exist_ok=True)
    (art / "host_sdk_discovery.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")


def _user_message(pipeline_config: PipelineConfig, workload: dict, stage: str, *, extra: str = "",
                  layer: dict | None = None) -> str:
    header = (workload.get("target_paths") or {}).get("host_header")
    exists = bool(header) and (pipeline_config.repo_root / header).is_file()
    mode = "update_existing" if exists and workload.get("allow_update_existing", True) else "generate_from_scratch"
    base = (f"Engine backend stage={stage} engine={workload.get('engine_name', 'engine')} mode={mode}. "
            "Implement this engine's native slot implementations (S1-S8 of the frozen substrate "
            "flow) from templates and the engine profile; read no other engine's tree. "
            "Follow pipeline/prompts/engine_backend/layers.md.")
    if layer:
        base += (
            f" This layer is `{layer.get('id')}` ({layer.get('title')}): ship its objects plus "
            f"`{layer.get('harness')}`. MCP tools on server `accel_pipeline` (tools, not resources): "
            f"`run_engine_build()` and `run_layer_gate()` (or `request_gate` `engine_build`, "
            f"`{layer.get('gate')}`) after each increment until both pass; not host cmake or `docker exec` in their "
            f"place. host_runtime.kind=`{(workload.get('host_runtime') or {}).get('kind') or 'host_cmake'}`. "
            "Do not implement later layers' contracts, but the wiring this layer's contract needs "
            "(registration, SQL that host.sh installs, a binding into another layer's object) is this "
            "layer's work wherever the file lives. Reuse prior layers; patch one only for a real bug "
            "that blocks this layer, then re-gate it. You run unattended: make the change.")
    return base + ("\n\n" + extra.strip() if extra.strip() else "")


def _layer_extra_context(workload: dict, layer: dict, repo_root: Path) -> dict[str, str]:
    prior = []
    for item in implementation_layers(workload):
        if item.get("id") == layer.get("id"):
            break
        prior.append(f"- `{item.get('id')}` ({item.get('title')}): `{item.get('harness')}`")
    contracts = "pipeline/prompts/engine_backend/layer_contracts.md"
    return {
        "layer_contracts": layer_contracts_digest(
            (Path(repo_root) / contracts).read_text(encoding="utf-8"), str(layer.get("id") or ""), contracts),
        "layer_id": str(layer.get("id") or ""),
        "layer_title": str(layer.get("title") or ""),
        "layer_gate": str(layer.get("gate") or ""),
        "layer_harness": str(layer.get("harness") or ""),
        "layer_cmake_target": str(layer.get("cmake_target") or ""),
        "prior_layers": "\n".join(prior) or "- (this is the first layer)",
        "host_runtime_kind": str((workload.get("host_runtime") or {}).get("kind") or "host_cmake"),
        "layer_rules": ", ".join(str(r) for r in (layer.get("rules") or [])) or "(none)",
    }


def _capped(pipeline_config: PipelineConfig, workload: dict) -> PipelineConfig:
    """The main session's context cap (`main_session_max_context_tokens`): past it a fresh session starts from the
    backend digest."""
    cap = int(workload.get("main_session_max_context_tokens") or 0)
    if not cap:
        return pipeline_config
    return dataclasses.replace(pipeline_config, session=dataclasses.replace(pipeline_config.session, max_context_tokens=cap))


def gate_feedback(failed: GateResult, *, title: str, fix: str, round_idx: int, max_rounds: int, layer_id: str = "") -> str:
    """A failed gate as feedback: its identity, trace and what the implementer must fix."""
    data = failed.data or {}
    lines = [title, "", *([f"- layer: `{layer_id}`"] if layer_id else []), f"- gate: `{failed.name}`",
             f"- status: `{failed.status}`", f"- attempt: {round_idx}/{max_rounds}"]
    if failed.path:
        lines.append(f"- gate json: `{failed.path}`")
    lines += format_reloop_failure_sections(data, max_chars=_GATE_LOG_TAIL_CHARS)
    return "\n".join([*lines, "", fix])


def _gate_runner(pipeline_config, workload_name, query, change_kind, iteration: Callable[[], int],
                 after_gate: Callable[[str], None] | None = None):
    """The repair loop's gate runner: the gates one at a time, in order, stopping at the first failure."""

    def run(names: list[str]) -> tuple[list[GateResult], GateResult | None]:
        results: list[GateResult] = []
        for name in names:
            batch, failed = run_post_stage_gates_collect(
                pipeline_config, workload_name, query, [name], change_kind=change_kind, iteration=iteration())
            results += batch
            if failed is not None:
                return results, failed
            if after_gate is not None:
                after_gate(name)
        return results, None

    return run


def _agent_turns(pipeline_config, model_config, workload_name, query, model_overrides, stage, turn):
    """A stage that is one agent turn, under the repair loop: a turn that fails (a deadline, a provider error) is
    resumed in its session with the failure as the message; past the budget the run stops cleanly."""
    pending_text = {"feedback": ""}

    def attempt():
        text, pending_text["feedback"] = pending_text["feedback"], ""
        return turn(text)

    def note(failure: Failure) -> None:
        pending_text["feedback"] = render_failure(failure)

    ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage=stage, model_overrides=model_overrides,
                    repair_turn=note)
    return run_with_repair(ctx, attempt)


def run_analysis(pipeline_config, model_config, workload_name, query, model_overrides, dry_run):
    workload = load_run_workload(pipeline_config, workload_name)
    progress = Progress(query_artifacts_dir(pipeline_config.repo_root, query, workload=workload))
    if not dry_run and progress.done("analysis"):
        return None

    def turn(feedback: str):
        return run_agent_stage(
            stage="analysis", prompt_file="01_analysis.md", pipeline_config=pipeline_config,
            model_config=model_config, workload_name=workload_name, query=query,
            user_message=_user_message(pipeline_config, workload, "analysis", extra=feedback),
            model_overrides=model_overrides, dry_run=dry_run, session_role="analysis")

    if dry_run:
        return turn("")
    result = _agent_turns(pipeline_config, model_config, workload_name, query, model_overrides, "analysis", turn)
    progress.mark("analysis")
    return result


class _ParallelReview:
    """A layer's critic, reviewing in a thread while the layer's gates run."""

    def __init__(self, run: "_LayerRun") -> None:
        self.run, self.report, self.thread = run, None, None

    def start(self) -> None:
        if self.thread is None:
            self.thread = threading.Thread(target=self._review, name=f"critic-{self.run.lid}", daemon=True)
            self.thread.start()

    def _review(self) -> None:
        run = self.run
        diff = backend_diff(run.pc.repo_root, run.engine, run.art / f"layer_{run.lid}.before")
        diff_path = run.art / "layer_diffs" / f"{run.lid}.diff"
        diff_path.parent.mkdir(parents=True, exist_ok=True)
        diff_path.write_text(diff, encoding="utf-8")
        self.report = run.critique(1, backend_digest(run.pc.repo_root, run.engine, run.art)
                                   + f"### This layer's diff (also at `{diff_path}`)\n\n```diff\n{diff[:30000]}\n```\n")

    def join(self) -> dict[str, Any] | None:
        if self.thread is not None:
            self.thread.join()
        return self.report


class _LayerRun:
    """One layer of the backend, in the main session: the agent's turns, the gates and their repairs, the critic and
    its batched findings."""

    def __init__(self, pipeline_config, model_config, workload_name, query, model_overrides, layer, workload):
        self.pc, self.mc, self.workload_name, self.workload = _capped(pipeline_config, workload), model_config, workload_name, workload
        self.query, self.overrides, self.layer = query, model_overrides, layer
        self.lid = str(layer.get("id") or "")
        self.engine = str(workload.get("engine_name") or "")
        self.stage = f"impl_{self.lid}"
        self.art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
        self.art.mkdir(parents=True, exist_ok=True)
        self.extra_context = _layer_extra_context(workload, layer, pipeline_config.repo_root)
        self.critic = layer_critic(self.art, self.lid, self.extra_context)
        self.base_extra = ""
        self.parallel_review: _ParallelReview | None = None
        self.feedback_file = self.art / f"layer_{self.lid}.feedback.md"

    def log(self, line: str) -> None:
        append_experiment_log(self.pc.repo_root, self.query, f"- layer `{self.lid}` {line}", workload=self.workload)

    def rounds(self) -> int | None:
        return int(self.workload.get("layer_gate_max_rounds") or 0) or None

    def digest(self) -> str:
        return backend_digest(self.pc.repo_root, self.engine, self.art, next_layer=self.lid)

    def turn(self, feedback: str = "", *, dry_run: bool = False) -> Any:
        """One main-session turn. In a session that already worked on this layer only the feedback is sent; in one
        that has not (the next layer) the layer's first message; with no session the first message after the
        backend digest."""
        store = SessionStore(self.art)
        sid = store.session_id(self.query, MAIN_ROLE)
        if sid and self.stage in store.phases(self.query, MAIN_ROLE):
            text = feedback.strip() or "Continue this layer: run the gates and fix what is red."
            message = text + f"\n\nThis layer's gates: {', '.join(layer_gates(self.layer))}."
        else:
            extra = "\n\n".join(x for x in (self.base_extra, feedback) if x.strip())
            message = ("" if sid else self.digest()) + _user_message(self.pc, self.workload, self.stage, extra=extra,
                                                                    layer=self.layer)
        return run_agent_stage(
            stage=self.stage, prompt_file=str(self.layer.get("prompt") or "02_implementation.md"),
            pipeline_config=self.pc, model_config=self.mc, workload_name=self.workload_name, query=self.query,
            user_message=message, model_overrides=self.overrides, extra_context=self.extra_context,
            dry_run=dry_run, session_role=MAIN_ROLE, handoff=self.digest)

    def critique(self, round_idx: int, brief: str) -> dict[str, Any]:
        """One critic turn: round 1 gets the backend digest and the layer's diff; later rounds the findings that
        were sent back to be re-checked."""
        scope = ("Review this layer from the diff and digest below; open a file only to check a concrete suspicion."
                 if round_idx == 1 else "The implementation changed to answer your last report. Re-check only the "
                 "findings below and what their fixes touched; do not re-read the layer.")
        message = f"Layer critique round={round_idx} engine={self.engine} layer={self.lid}. {scope}\n\n{brief}"
        try:
            run_critic(self.critic, self.pc, self.mc, self.workload_name, self.query, self.overrides, message)
        except Exception as exc:  # noqa: BLE001 - a critic that cannot answer never stops the run
            self.log(f"critic round {round_idx} raised {type(exc).__name__}: {exc}")
        return load_verdict(self.critic)

    def converge(self, gates: list[str], feedback: str = "", *, critic: bool = False) -> Any:
        """The turn on `feedback`, then `gates`, repaired in this session until they pass (or the run stops).
        `critic`: start the layer's critic once the build is green."""
        pending_text, failures = {"feedback": feedback}, {"n": 0}
        ctx = RepairCtx(self.pc, self.mc, self.workload_name, self.query, stage=self.stage, change_kind=self.stage,
                        model_overrides=self.overrides, max_code_rounds=self.rounds())

        def attempt() -> Any:
            text, pending_text["feedback"] = pending_text["feedback"], ""
            return self.turn(text)

        def repair(failure: Failure) -> None:
            failures["n"] += 1
            text = self.failure_feedback(ctx, failure, failures["n"])
            self.feedback_file.write_text(text + "\n", encoding="utf-8")
            self.log(f"{failure.source} failed (repair {failures['n']}); resuming the main session")
            if gates:
                self.turn(text)
            else:
                pending_text["feedback"] = text

        def after_gate(name: str) -> None:
            if critic and name == "engine_build" and self.parallel_review is not None:
                self.parallel_review.start()

        ctx.repair_turn = repair
        ctx.gate_runner = _gate_runner(self.pc, self.workload_name, self.query, self.stage, lambda: failures["n"],
                                       after_gate)
        result = run_with_repair(ctx, attempt, gates=gates)
        self.feedback_file.unlink(missing_ok=True)
        return result

    def failure_feedback(self, ctx: RepairCtx, failure: Failure, round_idx: int) -> str:
        failed = next((r for r in ctx.results if f"gate:{r.name}" == failure.source and r.status != "pass"), None)
        if failed is None:
            return render_failure(failure)
        text = gate_feedback(
            failed, title="## Layer gate FAILED — fix from this trace; this stage will re-run implementation and gates",
            fix=("Fix the adapter, harness, and/or CMake so this gate passes. Mid-loop MCP `request_gate` "
                 "`engine_build` then this layer's gate until both are green. Do not start a later layer. "
                 "Do not ignore this log."),
            round_idx=round_idx, max_rounds=ctx.max_code_rounds or ctx.pipeline_config.repair.max_code_rounds,
            layer_id=self.lid)
        return text + "".join(f"\n- {d}" for d in failure.diagnosis if d.startswith("racy"))

    def run(self, gates: list[str], *, carried: str, with_critic: bool) -> Any:
        progress = Progress(self.art)
        progress.reopen_layer(self.lid)
        snapshot_backend(self.pc.repo_root, self.engine, self.art / f"layer_{self.lid}.before")
        if with_critic:
            self.parallel_review = _ParallelReview(self)
        result = self.converge(gates, carried, critic=with_critic)
        if with_critic:
            self.review(gates)
        (self.art / "layer_diffs").mkdir(exist_ok=True)
        (self.art / "layer_diffs" / f"{self.lid}.diff").write_text(
            backend_diff(self.pc.repo_root, self.engine, self.art / f"layer_{self.lid}.before"), encoding="utf-8")
        progress.mark_layer(self.lid)
        return result

    def review(self, gates: list[str]) -> None:
        """The critic's findings, all at once, to the main session; then the critic re-checks only those."""
        assert self.parallel_review is not None
        report = self.parallel_review.join() or load_verdict(self.critic)
        max_rounds = self.critic.rounds(self.workload) or 1
        for round_idx in range(1, max_rounds + 1):
            self.record_canary_requests(report)
            if report["status"] != "fail":
                self.log(f"critique round {round_idx}: {report['status'].upper()}")
                return
            feedback = verdict_feedback(self.critic, report, round_idx)
            _write_feedback(self.critic, feedback, round_idx)
            self.log(f"critique round {round_idx}: FAIL; one batched repair round in the main session")
            self.converge(gates, feedback)
            report = self.critique(round_idx + 1, feedback)
        self.record_canary_requests(report)
        if report["status"] == "fail":
            (self.art / f"layer_{self.lid}.open_findings.md").write_text(
                verdict_feedback(self.critic, report, max_rounds + 1) + "\n", encoding="utf-8")
            self.log(f"accepted with open findings after {max_rounds} repair round(s) (for the architecture audit)")

    def record_canary_requests(self, report: dict[str, Any]) -> None:
        requests = canary_requests(report)
        if requests:
            with (self.art / "canary_requests.md").open("a", encoding="utf-8") as out:
                for item in requests:
                    out.write(f"- layer `{self.lid}`: {item.get('contract_item') or item.get('rule')}: "
                              f"{item.get('expected') or item.get('evidence')}\n")


def _carried_layer_feedback(art: Path, lid: str) -> str:
    """The feedback the previous run of this layer stopped on (a relaunch starts from it), or "" when that run
    passed or there is none."""
    path = art / f"layer_{lid}.feedback.md"
    if Progress(art).layer_passed(lid) or not path.is_file():
        return ""
    return "## Carried over: the previous run of this layer stopped on this feedback\n\n" + path.read_text(encoding="utf-8")


def run_implementation_layer(pipeline_config, model_config, workload_name, query, model_overrides, dry_run, layer: dict,
                             *, skip_gates=False, extra_user_message: str = ""):
    workload = load_run_workload(pipeline_config, workload_name)
    run = _LayerRun(pipeline_config, model_config, workload_name, query, model_overrides, layer, workload)
    run.base_extra = extra_user_message.strip()
    if dry_run:
        return run.turn(dry_run=True)
    gates = [] if skip_gates else layer_gates(layer)
    return run.run(gates, carried=_carried_layer_feedback(run.art, run.lid),
                   with_critic=bool(gates) and run.critic.rounds(workload) > 0)


def repair_layer(pipeline_config, model_config, workload_name, query, model_overrides, layer_id: str, feedback: str,
                 *, skip_gates: bool = False) -> None:
    """Fixes found after a layer passed (audit, validation): the layer's own session turn on `feedback`, then its
    gates. Whatever judged the old code is stale afterwards."""
    workload = load_run_workload(pipeline_config, workload_name)
    layer = next((l for l in implementation_layers(workload) if str(l.get("id")) == layer_id), None)
    if layer is None:
        raise ValueError(f"unknown layer {layer_id!r}")
    run = _LayerRun(pipeline_config, model_config, workload_name, query, model_overrides, layer, workload)
    run.converge([] if skip_gates else layer_gates(layer), feedback)
    Progress(run.art).invalidate_gates()


def run_implementation(pipeline_config, model_config, workload_name, query, model_overrides, dry_run, *,
                       skip_gates=False, extra_user_message: str = "", only_layer_id: str | None = None,
                       from_layer_id: str | None = None):
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    layers = implementation_layers(workload)
    ids = [str(layer.get("id") or "") for layer in layers]
    start = ids.index(from_layer_id) if from_layer_id in ids else 0
    result = None
    for layer in layers[start:]:
        lid = str(layer.get("id") or "")
        if not _layer_should_run(workload, layer) or (only_layer_id and lid != only_layer_id):
            continue
        if not only_layer_id and not dry_run and Progress(art).layer_passed(lid):
            continue
        result = run_implementation_layer(pipeline_config, model_config, workload_name, query, model_overrides,
                                          dry_run, layer, skip_gates=skip_gates, extra_user_message=extra_user_message)
    return result


def owning_layer_ids(audit: dict[str, Any], workload: dict[str, Any]) -> list[str]:
    """The layers that own the violations, in implementation order. A violation that names no layer belongs to the
    last one (the accelerator-facing surface sees every layer's work)."""
    order = [str(l["id"]) for l in implementation_layers(workload) if l.get("id")]
    owners = set()
    for item in audit.get("violations") or []:
        lid = str(item.get("layer") or "").strip() if isinstance(item, dict) else ""
        owners.add(lid if lid in order else order[-1])
    return [lid for lid in order if lid in owners]


def run_architecture_audit(pipeline_config, model_config, workload_name, query, model_overrides, dry_run, *,
                           skip_gates=False):
    """Audit (`architecture_audit_max_rounds` passes at most, each after the fixes of the one before); the owning
    layers fix their violations through their repair loops."""
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    progress = Progress(art)
    if not dry_run and progress.done("architecture_audit"):
        return None
    art.mkdir(parents=True, exist_ok=True)
    critic = architecture_critic(art)
    max_rounds = max(1, critic.rounds(workload))
    engine = str(workload.get("engine_name") or "")

    def log(line: str) -> None:
        append_experiment_log(pipeline_config.repo_root, query, f"- architecture audit {line}", workload=workload)

    audit: dict[str, Any] = {}
    for round_idx in range(1, max_rounds + 1):
        log(f"round {round_idx}/{max_rounds}")
        if dry_run:
            audit = {"engine_name": engine, "status": "pass", "summary": "dry-run skip", "violations": [],
                     "feedback_for_implementation": "", "round": round_idx}
            critic.verdict.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
            return audit
        message = (
            backend_digest(pipeline_config.repo_root, engine, art) + f"Layer diffs: `{art}/layer_diffs/<layer>.diff`.\n\n"
            + f"Architecture audit round={round_idx}/{max_rounds} engine={engine}. "
            if round_idx == 1 else
            f"Architecture audit round={round_idx}/{max_rounds}: the layers named in your last report were fixed. "
            "Re-check those violations and what the fixes touched. ") + "On fail set violations[].layer. Write architecture_audit.json."
        run_critic(critic, pipeline_config, model_config, workload_name, query, model_overrides, message)
        audit = load_verdict(critic)
        audit["round"] = round_idx
        critic.verdict.write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
        if audit["status"] != "fail":
            log(f"round {round_idx}: {audit['status'].upper()}"
                + (f" ({audit['summary']})" if audit["status"] == "error" else ""))
            break
        owners = owning_layer_ids(audit, workload)
        log(f"round {round_idx}: FAIL, fixes routed to layers {', '.join(owners)}")
        for lid in owners:
            feedback = verdict_feedback(critic, audit, round_idx, layer=lid)
            _write_feedback(critic, feedback, round_idx, f"_{lid}")
            repair_layer(pipeline_config, model_config, workload_name, query, model_overrides, lid, feedback,
                         skip_gates=skip_gates)
    else:
        log(f"fixes of round {max_rounds} routed; validation judges them")
    progress.mark("architecture_audit")
    return audit


def validation_owner(gate: str, data: dict[str, Any], workload: dict) -> str:
    """The layer whose main session fixes a failing validation gate (or `validation`, the session that wrote the
    adversarial tests): the gate's own report, the configured owner, the layer whose gate it is, else the last."""
    layers = implementation_layers(workload)
    ids = [str(l.get("id")) for l in layers]
    configured = (workload.get("validation_gate_owners") or {}).get(gate)
    own = next((str(l.get("id")) for l in layers if l.get("gate") == gate), "")
    for candidate in (str((data or {}).get("owner_layer") or ""), str(configured or ""), own):
        if candidate in ids or candidate == VALIDATION_ROLE:
            return candidate
    return ids[-1]


def run_validation(pipeline_config, model_config, workload_name, query, model_overrides, dry_run, *, skip_gates=False):
    workload = load_run_workload(pipeline_config, workload_name)
    pipeline_config = _capped(pipeline_config, workload)
    progress = Progress(query_artifacts_dir(pipeline_config.repo_root, query, workload=workload))

    def validation_turn(message: str) -> Any:
        return run_agent_stage(
            stage="validation", prompt_file="03_validation.md", pipeline_config=pipeline_config,
            model_config=model_config, workload_name=workload_name, query=query, user_message=message,
            model_overrides=model_overrides, dry_run=dry_run, session_role=VALIDATION_ROLE)

    if dry_run:
        return validation_turn(_user_message(pipeline_config, workload, "validation"))
    result = None
    if not progress.done("validation_authored"):
        result = _agent_turns(
            pipeline_config, model_config, workload_name, query, model_overrides, "validation",
            lambda feedback: validation_turn(_user_message(pipeline_config, workload, "validation", extra=feedback)))
        progress.mark("validation_authored")
    if not skip_gates:
        _run_validation_gates(pipeline_config, model_config, workload_name, workload, query, model_overrides,
                              list(workload.get("post_validation_gates") or []), validation_turn, progress)
    return result


def _run_validation_gates(pipeline_config, model_config, workload_name, workload, query, model_overrides, gates,
                          validation_turn, progress: Progress) -> None:
    """Each gate under the repair loop. A failure goes to the owning layer's session (or the validation session) as
    a Failure; after the fix every gate runs again, cheap ones first."""
    for gate in gates:
        if progress.gate_passed(gate):
            continue
        ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="validation", change_kind=gate,
                        model_overrides=model_overrides)

        def route(failure: Failure, ctx=ctx, gate=gate) -> None:
            failed = next((r for r in ctx.results if r.name == gate and r.status != "pass"), None)
            owner = validation_owner(gate, failed.data if failed else {}, workload)
            ctx.log(f"validation gate `{gate}` failed; fix goes to `{owner}`")
            feedback = "## Validation gate failed\n\n" + render_failure(failure)
            if owner == VALIDATION_ROLE:
                validation_turn(feedback)
            else:
                repair_layer(pipeline_config, model_config, workload_name, query, model_overrides, owner, feedback)
            progress.invalidate_gates()

        ctx.repair_turn = route
        ctx.gate_runner = _gate_runner(pipeline_config, workload_name, query, gate, lambda: 0)
        run_with_repair(ctx, lambda: None, gates=[gate])
        progress.mark_gate(gate)


def run_engine_pipeline(pipeline_config: PipelineConfig, model_config: ModelConfig, workload_name: str, query: int,
                        model_overrides: dict[str, str], dry_run: bool, *, stage: str = "all",
                        skip_gates: bool = False) -> None:
    workload = load_run_workload(pipeline_config, workload_name)
    _prepare_engine_host_sdk(pipeline_config, workload, query)
    qsession = None if dry_run else apply_foreign_engine_quarantine(pipeline_config.repo_root, workload)
    args = (pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
    try:
        if stage == "analysis":
            run_analysis(*args)
        elif stage.startswith("impl_"):
            run_implementation(*args, skip_gates=skip_gates, only_layer_id=stage[len("impl_"):])
        elif stage == "implementation":
            run_implementation(*args, skip_gates=skip_gates)
            if not skip_gates:
                run_architecture_audit(*args)
        elif stage == "architecture_audit":
            run_architecture_audit(*args, skip_gates=skip_gates)
        elif stage in ("validation", "audit"):
            run_validation(*args, skip_gates=skip_gates)
        else:
            run_analysis(*args)
            run_implementation(*args, skip_gates=skip_gates)
            run_architecture_audit(*args, skip_gates=skip_gates)
            run_validation(*args, skip_gates=skip_gates)
    finally:
        restore_quarantine(qsession)


# --------------------------------------------------------------------------------------------------------------
# host_bootstrap: the engine's workload harnesses

_DEFAULT_GATE_MAX_ROUNDS = 3
_CONFORMANCE_REPAIRS = 2
BOOTSTRAP_GATES = ["host_bootstrap", "harness_conformance"]


def host_bootstrap_dir(repo_root: Path, engine_name: str) -> Path:
    path = engine_artifacts_dir(repo_root, sanitize_engine_name(engine_name)) / "host_bootstrap"
    path.mkdir(parents=True, exist_ok=True)
    return path


def host_bootstrap_status_path(repo_root: Path, engine_name: str) -> Path:
    return host_bootstrap_dir(repo_root, engine_name) / "host_bootstrap.json"


def needs_host_bootstrap(workload: dict[str, Any]) -> bool:
    """True for every accelerator workload: each engine's harnesses are made once."""
    return workload.get("workload_kind") != "engine_backend"


def pending_harness_requests(repo_root: Path, engine_name: str) -> str:
    """Harness defects query runs recorded that no bootstrap has fixed yet."""
    return as_required_fixes(pending(repo_root, sanitize_engine_name(engine_name), "harness"))


def host_bootstrap_passed(repo_root: Path, engine_name: str, benchmark: str) -> bool:
    """The last gate run passed and built and smoked this benchmark's harness targets, the sources are still there,
    and no query run has recorded a harness defect since."""
    if pending_harness_requests(repo_root, engine_name):
        return False
    path = host_bootstrap_status_path(repo_root, engine_name)
    if not path.is_file():
        return False
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return False
    gate = data.get("gate_data") or {}  # markers written by unit tests / dry runs carry no gate result
    if str(data.get("status") or "") != "pass" or gate.get("status") != "pass" or not gate.get("smokes"):
        return False
    engine = sanitize_engine_name(engine_name)
    if not set(harness_targets(engine, benchmark).values()) <= set(gate.get("targets") or []):
        return False
    return any((Path(repo_root) / "backends" / engine / "tests" / "harness").glob("*.cpp"))


def write_host_bootstrap_status(repo_root: Path, engine_name: str, *, status: str, reason: str = "",
                                extra: dict[str, Any] | None = None) -> Path:
    path = host_bootstrap_status_path(repo_root, engine_name)
    engine = sanitize_engine_name(engine_name)
    payload = {"gate": "host_bootstrap", "engine_name": engine, "status": status, "reason": reason, **(extra or {})}
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    if status == "pass" and (payload.get("gate_data") or {}).get("status") == "pass":
        mark_handled(repo_root, engine, "harness")
    return path


def engines_needing_bootstrap_from_workloads(repo_root: Path, runs: list[tuple[str, str]]) -> list[tuple[str, str]]:
    """(engine_name, representative_workload) needing bootstrap, one per engine and benchmark, from (workload,
    engine) runs."""
    seen: set[tuple[str, str]] = set()
    out: list[tuple[str, str]] = []
    for name, run_engine in runs:
        try:
            w = load_workload(name, repo_root, engine=run_engine)
        except FileNotFoundError:
            continue
        if not needs_host_bootstrap(w):
            continue
        engine = sanitize_engine_name(str(w.get("engine_name") or ""))
        key = (engine, str(w.get("benchmark_name") or ""))
        if key in seen or host_bootstrap_passed(repo_root, *key):
            continue
        seen.add(key)
        out.append((engine, name))
    return out


def run_host_bootstrap_agent(pipeline_config: PipelineConfig, model_config: ModelConfig, workload_name: str,
                             query: int, model_overrides: dict[str, str] | None = None, dry_run: bool = False, *,
                             engine_name: str = "", extra_user_message: str = ""):
    engine = engine_name or "engine"
    user_message = (
        f"Build the workload harnesses of engine={engine}: the differential, multi-connection, "
        f"cross-accelerator, bench and calibrate targets and the per-query module test under "
        f"backends/{engine}/tests/harness/ and bench/, declared in backends/{engine}/tests/harness.cmake "
        "(the only paths you may write). One query- and benchmark-parameterised harness per kind, "
        "naming no benchmark, reaching the engine through the backend api/ and "
        "host.sh only. Build through the engine's host.sh (request_gate host_bootstrap), not a host "
        "cmake/make. Call request_gate host_bootstrap until it passes, then finish.")
    if extra := (extra_user_message or "").strip():
        user_message = f"{user_message}\n\n{extra}"
    return run_agent_stage(
        stage="host_bootstrap", prompt_file="host_bootstrap.md", pipeline_config=pipeline_config,
        model_config=model_config, workload_name=workload_name, query=query, user_message=user_message,
        model_overrides=model_overrides, dry_run=dry_run, session_role="host_bootstrap")


def run_host_bootstrap(pipeline_config: PipelineConfig, model_config: ModelConfig, *, engine_name: str,
                       representative_workload: str, model_overrides: dict[str, str] | None = None,
                       dry_run: bool = False, skip_gates: bool = False, force: bool = False) -> dict[str, Any]:
    """Run the host_bootstrap agent stage for one engine (idempotent unless force)."""
    engine = sanitize_engine_name(engine_name)
    repo_root = pipeline_config.repo_root
    if not (dry_run or skip_gates) and not (repo_root / f"backends/{engine}/host.sh").is_file():
        report = host_bootstrap_dir(repo_root, engine) / "stop_report.md"
        report.write_text(f"# Stopped: the {engine} backend is missing\n\nhost_bootstrap needs the backend first: run its "
                          "engine_backend workload, then `--resume`.\n", encoding="utf-8")
        raise StageStopped("the engine backend is missing", report)

    workload = load_workload(representative_workload, repo_root, engine=engine)
    status_path = host_bootstrap_status_path(repo_root, engine)
    if not force and host_bootstrap_passed(repo_root, engine, str(workload.get("benchmark_name") or "")):
        return {"engine_name": engine, "status": "pass", "skipped": True, "path": str(status_path)}

    query = int(workload.get("query_number") or 0)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    print(f"[host_bootstrap] engine={engine} workload={representative_workload} (one-shot before query workstreams)",
          flush=True)

    if dry_run or skip_gates:
        run_host_bootstrap_agent(pipeline_config, model_config, representative_workload, query,
                                 model_overrides=model_overrides, dry_run=dry_run, engine_name=engine)
        path = write_host_bootstrap_status(repo_root, engine, status="pass" if dry_run else "skip",
                                           reason="dry_run" if dry_run else "skip_gates")
        return {"engine_name": engine, "status": "pass", "skipped": False, "path": str(path)}

    with declining_query(pipeline_config, workload, query):
        if not force and not pending_harness_requests(repo_root, engine):
            results, failed = run_bootstrap_gates(pipeline_config, workload, query, iteration=0)
            if failed is None:
                path = write_host_bootstrap_status(repo_root, engine, status="pass",
                                                   reason="the existing harness passes the gate",
                                                   extra={"gate_data": results[0].data})
                print(f"[host_bootstrap] engine={engine}: the existing harness passes; no agent run", flush=True)
                return {"engine_name": engine, "status": "pass", "skipped": True, "path": str(path)}
        return _bootstrap_under_repair(pipeline_config, model_config, engine, representative_workload, workload, query,
                                       model_overrides)


@contextlib.contextmanager
def declining_query(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int):
    """The query registered with the module that declines every call while host_bootstrap works: its own module
    may be mid-implementation and not compile, and the harnesses link it. Its engine directory waits in the query's
    artifacts (outside every build glob) and is put back however the bootstrap ends, its files touched: a move keeps
    their old mtimes, and the build would keep the declining module's newer objects."""
    from accel_pipeline.stages import materialize_registration

    engine_dir, aside = recover_query_module(pipeline_config, workload, query)
    if engine_dir is None or not engine_dir.is_dir():
        yield
        return
    aside.parent.mkdir(parents=True, exist_ok=True)
    shutil.move(engine_dir, aside)
    try:
        materialize_registration(pipeline_config, workload, query)
        yield
    finally:
        _put_back(aside, engine_dir)


def recover_query_module(pipeline_config: PipelineConfig, workload: dict[str, Any],
                         query: int) -> tuple[Path | None, Path]:
    """The query's engine directory and where a bootstrap sets it aside; one a dead bootstrap left aside is put back."""
    repo = pipeline_config.repo_root
    register = str((workload.get("engine_host") or {}).get("aux_register_path") or "")
    engine_dir = (repo / register).parent.parent if register else None  # accelerators/<bench>/q<N>/<engine>
    aside = query_artifacts_dir(repo, query, workload=workload) / "implementation_aside"
    if engine_dir is not None and aside.is_dir():
        _put_back(aside, engine_dir)
    return engine_dir, aside


def _put_back(aside: Path, engine_dir: Path) -> None:
    shutil.rmtree(engine_dir, ignore_errors=True)
    shutil.move(aside, engine_dir)
    for path in engine_dir.rglob("*"):
        if path.is_file():
            path.touch()


def run_conformance(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, *,
                    change_kind: str, iteration: int = 0) -> GateResult:
    """The one conformance check (run_harness_conformance.sh) on a registered query: every read is answered by the
    reference SQL, so the harnesses must pass whatever its module is."""
    return gate_runner.run_gate(pipeline_config, "harness_conformance", query, workload, raise_on_failure=False,
                                change_kind=change_kind, iteration=iteration)


def run_bootstrap_gates(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int, *,
                        iteration: int) -> tuple[list[GateResult], GateResult | None]:
    """The harnesses built and smoked, then conformant on the representative query; the results and the failing one
    (a harness failure is this stage's own: the repair loop repairs it here)."""
    from accel_pipeline.stages import materialize_registration

    materialize_registration(pipeline_config, workload, query)  # the module test of a registered query is built too
    built = gate_runner.run_gate(pipeline_config, "host_bootstrap", query, workload=workload, raise_on_failure=False,
                                 change_kind="host_bootstrap", iteration=iteration)
    if built.status != "pass":
        return [built], built
    conformance = run_conformance(pipeline_config, workload, query, change_kind="host_bootstrap", iteration=iteration)
    return [built, conformance], conformance if conformance.status == "fail" else None


def _bootstrap_under_repair(pipeline_config, model_config, engine, representative_workload, workload, query,
                            model_overrides) -> dict[str, Any]:
    """The harness agent turn, its gate (repaired in the same session on failure) and the critic (one batched
    repair round for its findings, then a re-check); a failure the loop cannot repair ends in a recorded stop,
    never a traceback."""
    repo_root = pipeline_config.repo_root
    art = host_bootstrap_dir(repo_root, engine)
    critic = host_bootstrap_critic(art, repo_root)
    ctx = RepairCtx(pipeline_config, model_config, representative_workload, query, stage="host_bootstrap",
                    change_kind="host_bootstrap", model_overrides=model_overrides)
    ctx.workload = workload
    ctx.art = query_artifacts_dir(repo_root, query, workload=workload)
    requests = pending_harness_requests(repo_root, engine)
    pending_text = {"feedback": f"Query runs recorded these harness defects; fix every one in the harnesses:\n\n{requests}"
                    if requests else ""}
    last: dict[str, GateResult] = {}
    rounds = {"gate": 0}

    def attempt() -> Any:
        text, pending_text["feedback"] = pending_text["feedback"], ""
        return run_host_bootstrap_agent(pipeline_config, model_config, representative_workload, query,
                                        model_overrides=model_overrides, dry_run=False, engine_name=engine,
                                        extra_user_message=text)

    def run_gates(_names: list[str]) -> tuple[list[GateResult], GateResult | None]:
        results, failed = run_bootstrap_gates(pipeline_config, workload, query, iteration=rounds["gate"])
        last["gate"] = failed or results[0]
        if failed is not None:
            write_host_bootstrap_status(repo_root, engine, status="fail", reason=str((failed.data or {}).get("reason") or ""),
                                        extra={"gate_data": failed.data, "attempt": rounds["gate"] + 1})
        return results, failed

    def repair(failure: Failure) -> None:
        rounds["gate"] += 1
        failed = last.get("gate")
        if failed is not None and failure.source.startswith("gate:"):
            text = gate_feedback(
                failed, title="## Host bootstrap gate FAILED — fix from this trace; this stage will re-run",
                fix=("Fix the workload harnesses (backends/<engine>/tests/harness/, bench/) and/or tests/harness.cmake "
                     "so this gate passes. Build through the engine's host.sh (`request_gate` `host_bootstrap`), not a "
                     "host cmake build. Mid-loop MCP `request_gate` `host_bootstrap` until green. Do not ignore this log."),
                round_idx=rounds["gate"], max_rounds=ctx.max_code_rounds or pipeline_config.repair.max_code_rounds)
        else:
            text = render_failure(failure)
        (art / f"host_bootstrap_gate_feedback_round{rounds['gate']}.md").write_text(text + "\n", encoding="utf-8")
        pending_text["feedback"] = text
        attempt()

    ctx.repair_turn, ctx.gate_runner = repair, run_gates
    ctx.max_code_rounds = int(workload.get("host_bootstrap_gate_max_rounds") or _DEFAULT_GATE_MAX_ROUNDS)
    run_with_repair(ctx, attempt, gates=BOOTSTRAP_GATES)

    max_critique = critic.rounds(workload)
    for critique_round in range(1, max_critique + 1):
        append_experiment_log(repo_root, query, f"- host_bootstrap critique round {critique_round}/{max_critique}",
                              workload=workload)
        run_critic(critic, pipeline_config, model_config, representative_workload, query, model_overrides,
                   f"Host bootstrap critique round={critique_round} engine={engine}. "
                   "Read query_host_bootstrap.md + register/differential/CMake only.")
        verdict = load_verdict(critic)
        append_experiment_log(repo_root, query, f"- host_bootstrap critique round {critique_round}: "
                              f"{verdict['status'].upper()}", workload=workload)
        if verdict["status"] != "fail":
            break
        feedback = verdict_feedback(critic, verdict, critique_round)
        _write_feedback(critic, feedback, critique_round)
        pending_text["feedback"] = feedback
        run_with_repair(ctx, attempt, gates=BOOTSTRAP_GATES)

    final = last["gate"]
    path = write_host_bootstrap_status(repo_root, engine, status=final.status,
                                       reason=str((final.data or {}).get("reason") or ""),
                                       extra={"gate_data": final.data})
    return {"engine_name": engine, "status": final.status, "skipped": False, "path": str(path), "gate": final.data}


def check_conformance(pipeline_config: PipelineConfig, model_config: ModelConfig, workload_name: str,
                      workload: dict[str, Any], query: int, model_overrides: dict[str, str] | None = None) -> None:
    """The conformance check on this query when its suite freezes (the bootstrap ran it before any suite). A failure is
    the harness's: it is recorded as a harness request, host_bootstrap re-runs with it as a required fix (and its own
    gates repeat the check), and the check repeats."""
    if workload.get("workload_kind") != "aux_state" or not workload.get("engine_name"):
        return  # no engine harnesses to run
    engine = sanitize_engine_name(str(workload["engine_name"]))
    for repairs in range(_CONFORMANCE_REPAIRS + 1):
        result = run_conformance(pipeline_config, workload, query, change_kind="harness_conformance")
        if result.status in ("pass", "skip"):
            return
        failure = failure_from_gate(result, stage="harness_conformance", workload=workload,
                                    repo_root=pipeline_config.repo_root)
        if repairs == _CONFORMANCE_REPAIRS:
            break
        print(f"[host_bootstrap] the {engine} harnesses fail on Q{query}: {failure.summary}; host_bootstrap repairs them",
              flush=True)
        record(pipeline_config.repo_root, engine, {
            "benchmark": str(workload.get("benchmark_name") or ""), "query": query, "scope": "harness",
            "helper": "harness conformance", "check": "harness_conformance", "reason": render_failure(failure)})
        run_host_bootstrap(pipeline_config, model_config, engine_name=engine, representative_workload=workload_name,
                           model_overrides=model_overrides)
    report = host_bootstrap_dir(pipeline_config.repo_root, engine) / "stop_report.md"
    report.write_text(f"# Stopped: the {engine} harnesses fail on Q{query} after {_CONFORMANCE_REPAIRS} repairs\n\n"
                      f"{render_failure(failure)}\n", encoding="utf-8")
    raise StageStopped(f"the {engine} harnesses fail on Q{query} with every read answered by the reference SQL", report)


def ensure_host_bootstrap_for_workload(pipeline_config: PipelineConfig, model_config: ModelConfig, workload_name: str,
                                       *, model_overrides: dict[str, str] | None = None, dry_run: bool = False,
                                       skip_gates: bool = False) -> dict[str, Any] | None:
    """Single-query entry: bootstrap the engine's harnesses if needed, else None."""
    workload = load_run_workload(pipeline_config, workload_name)
    if not needs_host_bootstrap(workload):
        return None
    recover_query_module(pipeline_config, workload, int(workload.get("query_number") or 0))
    return run_host_bootstrap(pipeline_config, model_config,
                              engine_name=sanitize_engine_name(str(workload.get("engine_name") or "")),
                              representative_workload=workload_name, model_overrides=model_overrides,
                              dry_run=dry_run, skip_gates=skip_gates)


def ensure_host_bootstrap_for_pack(pack: dict[str, Any], *, repo_root: Path, pipeline_config: PipelineConfig,
                                   model_config: ModelConfig, queries: list[Any],
                                   model_overrides: dict[str, str] | None = None, dry_run: bool = False,
                                   skip_gates: bool = False) -> list[dict[str, Any]]:
    """Pack entry: bootstrap each distinct engine and benchmark once before its workers."""
    needed = engines_needing_bootstrap_from_workloads(repo_root, [(str(q.workload), str(q.engine)) for q in queries])
    results = [run_host_bootstrap(pipeline_config, model_config, engine_name=engine, representative_workload=rep,
                                  model_overrides=model_overrides, dry_run=dry_run, skip_gates=skip_gates)
               for engine, rep in needed]
    if results:
        marker = pipeline_root(repo_root) / "artifacts" / "packs" / "host_bootstrap_last.json"
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(json.dumps({"results": results}, indent=2) + "\n", encoding="utf-8")
    return results
