"""Per-layer critique after engine_backend implementation gates.

A read-only critic (one session per layer, resumed across rounds) judges a green layer for design
flaws and engine hazards the pipeline's own contract checks cannot see. On fail, the layer's main
session gets the feedback. A report that never becomes valid JSON is recorded as status `error`: a
critic that cannot answer never stops the run.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from pathlib import Path
from typing import Any

from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload, query_artifacts_dir
from accel_pipeline.repair import StageFailure
from accel_pipeline.stages.base import run_agent_stage


def critique_stage_name(layer_id: str) -> str:
    return f"critique_{layer_id}"


def layer_critique_path(art: Path, layer_id: str) -> Path:
    return art / f"layer_critique_{layer_id}.json"


def report_problem(path: Path) -> str | None:
    """Why `path` is not a usable report (missing, or not one JSON object); None when it is."""
    if not path.is_file():
        return "missing"
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        return f"not valid JSON: {exc}"
    return None if isinstance(data, dict) else "not a JSON object"


def unreadable_artifact_nudge(path: Path, problem: str) -> str:
    return (
        f"`{path}` is {problem}. Rewrite it now as exactly one JSON object (status "
        "pass|fail, required fields) with nothing before or after it, then stop. "
        "Do not read more sources or edit adapter sources."
    )


def missing_artifact_nudge(path: Path) -> str:
    return (
        f"STOP EXPLORING. You ended without writing `{path}`. "
        "That means this critique/audit failed to complete. Do not read more "
        "sources. Immediately write that JSON (status pass|fail, required "
        "fields) and the matching `.md` using Codex shell:\n\n"
        f"  cat > {path} <<'EOF'\n"
        "  { ... }\n"
        "  EOF\n\n"
        "Then write the `.md` beside it and stop. Do not edit adapter sources."
    )


def run_until_artifact(
    *,
    artifact: Path,
    invoke: Callable[[str], Any],
    attempts: int = 2,
    dry_run: bool = False,
) -> Any:
    """Run `invoke(extra_user_message)` until `artifact` is one JSON object (or attempts run out).
    A run that fails (a deadline, a provider error) counts as an attempt without a report."""
    extra = ""
    result = None
    for attempt in range(1, max(1, attempts) + 1):
        try:
            result = invoke(extra)
        except StageFailure as exc:
            result = exc.failure
        problem = None if dry_run else report_problem(artifact)
        if problem is None:
            return result
        extra = (
            missing_artifact_nudge(artifact)
            if problem == "missing"
            else unreadable_artifact_nudge(artifact, problem)
        )
    return result


def read_review(path: Path, *, what: str) -> dict[str, Any]:
    """The review report at `path` with a normalised status (`pass`, `fail`), or, when it is
    missing or unusable, status `error` with the reason, written back so the run records it."""
    problem = report_problem(path)
    data: dict[str, Any] = {}
    if problem is None:
        data = json.loads(path.read_text(encoding="utf-8"))
        status = str(data.get("status") or "").strip().lower()
        if status in {"pass", "fail"}:
            data["status"] = status
            return data
        problem = f"has no status pass|fail (got {data.get('status')!r})"
    data = {
        "status": "error",
        "summary": f"{what} unavailable: {path.name} {problem}",
        "violations": [],
        "feedback_for_implementation": "",
    }
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    return data


def load_layer_critique(art: Path, layer_id: str) -> dict[str, Any]:
    data = read_review(layer_critique_path(art, layer_id), what=f"layer critique `{layer_id}`")
    data["layer"] = str(data.get("layer") or layer_id)
    data = _without_pipeline_harness_gaps(data)
    findings = data.get("violations") or []
    if data["status"] == "fail" and findings and not any(is_blocking(v) for v in findings):
        data["status"] = "pass"
    return data


def _without_pipeline_harness_gaps(data: dict[str, Any]) -> dict[str, Any]:
    """Drop harness gaps aimed at the pipeline's own files (gates, contracts): the backend agent
    cannot change them, so they are never backend violations. A critique left with none passes."""
    kept, dropped = [], []
    for v in data.get("violations") or []:
        paths = v.get("paths") or [] if isinstance(v, dict) else []
        aimed_at_pipeline = isinstance(v, dict) and v.get("rule") == "harness_gap" and any(
            str(p).startswith("pipeline/") for p in paths
        )
        (dropped if aimed_at_pipeline else kept).append(v)
    if dropped:
        data["violations"], data["dropped_pipeline_harness_gaps"] = kept, dropped
        if data["status"] == "fail" and not kept:
            data["status"] = "pass"
    return data


def is_blocking(finding: Any) -> bool:
    """A code fix the critic requires: not advisory, and not a missing test (those go to the canary
    requests)."""
    if not isinstance(finding, dict):
        return True
    return finding.get("severity", "blocking") != "advisory" and finding.get("kind", "code_fix") != "needs_canary"


def canary_requests(critique: dict[str, Any]) -> list[dict[str, Any]]:
    return [v for v in critique.get("violations") or [] if isinstance(v, dict) and v.get("kind") == "needs_canary"]


def format_finding(item: dict[str, Any]) -> list[str]:
    """One finding: where, which contract item, what is wrong, what must hold instead."""
    where = item.get("location") or ", ".join(f"`{p}`" for p in item.get("paths") or [])
    head = f"- **{item.get('rule', 'contract')}**" + (f" at `{where}`" if where else "")
    if item.get("contract_item"):
        head += f" (contract: {item['contract_item']})"
    lines = [f"{head}: {item.get('evidence', '')}"]
    if item.get("expected"):
        lines.append(f"  - expected: {item['expected']}")
    return lines


def format_layer_critique_feedback(critique: dict[str, Any], *, round_idx: int) -> str:
    feedback = str(critique.get("feedback_for_implementation") or "").strip()
    violations = [v for v in critique.get("violations") or [] if is_blocking(v)]
    layer = str(critique.get("layer") or "")
    lines = [
        "## Layer critique FAILED — fix before this stage is done",
        "",
        f"- critique round: {round_idx}",
        f"- layer: `{layer}`",
        f"- summary: {critique.get('summary') or '(none)'}",
        "",
        "### Violations",
    ]
    if isinstance(violations, list) and violations:
        for item in violations:
            if not isinstance(item, dict):
                lines.append(f"- {item}")
                continue
            lines.extend(format_finding(item))
    else:
        lines.append("- (see feedback below)")
    lines.append("")
    lines.append("### Required fixes")
    lines.append(
        feedback
        or "(critic left feedback_for_implementation empty — re-read violations)"
    )
    lines.append("")
    lines.append(
        "Fix this layer, including the wiring it needs in files other layers "
        "own (plugin registration, host.sh, runtime binding); change a prior "
        "layer's logic only if a real bug there blocks this contract (smallest "
        "patch, keep that prior harness green). Never stop to ask: nobody "
        "answers, so make the change. Mid-loop `request_gate` "
        "`engine_build` then this layer's gate until green. Do not weaken the "
        "harness to greenwash."
    )
    return "\n".join(lines)


def critic_role(layer_id: str) -> str:
    return f"critic:{layer_id}"


def run_layer_critique_agent(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None,
    dry_run: bool,
    layer: dict[str, Any],
    *,
    round_idx: int,
    extra_context: dict[str, str],
    brief: str = "",
) -> Any:
    """One critic turn. `brief`: round 1, the backend digest and where the layer's diff is; later
    rounds, the findings that were sent back to be re-checked."""
    workload = load_run_workload(pipeline_config, workload_name)
    lid = str(layer.get("id") or "")
    engine = str(workload.get("engine_name") or "")
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    json_path = layer_critique_path(art, lid)
    md_path = art / f"layer_critique_{lid}.md"
    json_path.unlink(missing_ok=True)
    scope = (
        "Review this layer from the diff and digest below; open a file only to check a concrete suspicion."
        if round_idx == 1
        else "The implementation changed to answer your last report. Re-check only the findings "
        "below and what their fixes touched; do not re-read the layer."
    )
    base_msg = (
        f"Layer critique round={round_idx} engine={engine} layer={lid}. {scope}\n\n{brief}\n\n"
        f"Your LAST action must be writing `{json_path}` (pass|fail) and "
        f"`{md_path}`. Codex shell: `cat > {json_path} <<'EOF' ... EOF`. "
        "Then stop. Do not edit adapter sources. Do not explore later layers."
    )

    def invoke(extra: str) -> Any:
        return run_agent_stage(
            stage=critique_stage_name(lid),
            prompt_file="05_layer_critique.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=extra or base_msg,
            model_overrides=model_overrides,
            dry_run=dry_run,
            extra_context=extra_context,
            session_role=critic_role(lid),
        )

    return run_until_artifact(artifact=json_path, invoke=invoke, attempts=2, dry_run=dry_run)
