"""Per-layer critique after engine_backend implementation gates.

A cheap read-only critic can fail a green harness when the layer contract is
not actually met (untested paths, wrong substrate APIs). On fail, the
orchestrator re-invokes that implementation layer with the feedback.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from pathlib import Path
from typing import Any

from accel_pipeline.config import ModelConfig, PipelineConfig, load_workload
from accel_pipeline.gates import GateFailure
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.stages.base import run_agent_stage


def critique_stage_name(layer_id: str) -> str:
    return f"critique_{layer_id}"


def layer_critique_path(art: Path, layer_id: str) -> Path:
    return art / f"layer_critique_{layer_id}.json"


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
    """Run `invoke(extra_user_message)` until `artifact` exists (or attempts exhausted)."""
    extra = ""
    result = None
    for attempt in range(1, max(1, attempts) + 1):
        result = invoke(extra)
        if dry_run or artifact.is_file():
            return result
        extra = missing_artifact_nudge(artifact)
        if attempt >= attempts:
            break
    return result


def load_layer_critique(art: Path, layer_id: str) -> dict[str, Any]:
    path = layer_critique_path(art, layer_id)
    if not path.is_file():
        raise GateFailure(
            f"layer critique agent did not write {path.name} — "
            "cannot accept this implementation layer"
        )
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise GateFailure(f"{path.name} is not valid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise GateFailure(f"{path.name} must be a JSON object")
    status = str(data.get("status") or "").strip().lower()
    if status not in {"pass", "fail"}:
        raise GateFailure(
            f"{path.name} missing status pass|fail — got {data.get('status')!r}"
        )
    data["status"] = status
    data["layer"] = str(data.get("layer") or layer_id)
    return data


def format_layer_critique_feedback(critique: dict[str, Any], *, round_idx: int) -> str:
    feedback = str(critique.get("feedback_for_implementation") or "").strip()
    violations = critique.get("violations") or []
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
            rule = item.get("rule", "contract")
            evidence = item.get("evidence", "")
            paths = item.get("paths") or []
            path_s = ", ".join(f"`{p}`" for p in paths) if paths else ""
            suffix = f" ({path_s})" if path_s else ""
            lines.append(f"- **{rule}**: {evidence}{suffix}")
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
        "Fix this layer (and a prior layer only if a real bug there blocks this "
        "contract — smallest patch, keep that prior harness green). Add harness "
        "asserts for any untested contract path. Mid-loop `request_gate` "
        "`engine_build` then this layer's gate until green. Do not weaken the "
        "harness to greenwash."
    )
    return "\n".join(lines)


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
) -> Any:
    workload = load_workload(workload_name, pipeline_config.repo_root)
    lid = str(layer.get("id") or "")
    engine = str(workload.get("engine_name") or "")
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    json_path = layer_critique_path(art, lid)
    md_path = art / f"layer_critique_{lid}.md"
    base_msg = (
        f"Layer critique round={round_idx} engine={engine} layer={lid}. "
        "Read this layer's contract + adapter + harness only. "
        f"Your LAST action must be writing `{json_path}` (pass|fail) and "
        f"`{md_path}`. Codex shell: `cat > {json_path} <<'EOF' ... EOF`. "
        "Then stop. Do not edit adapter sources. Do not explore later layers."
    )

    def invoke(extra: str) -> Any:
        msg = base_msg if not extra else extra
        return run_agent_stage(
            stage=critique_stage_name(lid),
            prompt_file="05_layer_critique.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=msg,
            model_overrides=model_overrides,
            dry_run=dry_run,
            extra_context=extra_context,
        )

    return run_until_artifact(
        artifact=json_path, invoke=invoke, attempts=2, dry_run=dry_run
    )
