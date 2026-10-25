"""Read-only critic after host_bootstrap gates.

A cheap critic can fail a green differential *build* when the query host is
not actually usable (stub harness, wrong CMake target, second adapter tree).
On fail, the orchestrator re-invokes host_bootstrap with the feedback.
"""

from __future__ import annotations

from pathlib import Path
from typing import Any

from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.engine_backend.layer_critique import read_review, run_until_artifact

CRITIQUE_STAGE = "critique_host_bootstrap"
CRITIQUE_ROLE = "critic:host_bootstrap"


def host_bootstrap_critique_path(art: Path) -> Path:
    return art / "host_bootstrap_critique.json"


def load_host_bootstrap_critique(art: Path) -> dict[str, Any]:
    return read_review(host_bootstrap_critique_path(art), what="host_bootstrap critique")


def format_host_bootstrap_critique_feedback(
    critique: dict[str, Any], *, round_idx: int
) -> str:
    feedback = str(critique.get("feedback_for_implementation") or "").strip()
    violations = critique.get("violations") or []
    lines = [
        "## Host bootstrap critique FAILED — fix before this stage is done",
        "",
        f"- critique round: {round_idx}",
        f"- engine: `{critique.get('engine_name') or ''}`",
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
        "Fix the query host (SQL entrypoints, differential, CMake). Build "
        "through the engine's host.sh (`request_gate` `host_bootstrap`), not host "
        "`cmake --build build-release`. Mid-loop `request_gate` `host_bootstrap` "
        "until green. Do not invent a second adapter tree."
    )
    return "\n".join(lines)


def run_host_bootstrap_critique_agent(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None,
    dry_run: bool,
    *,
    engine_name: str,
    round_idx: int,
) -> Any:
    from accel_pipeline.engine_backend.host_bootstrap import host_bootstrap_dir

    art = host_bootstrap_dir(pipeline_config.repo_root, engine_name)
    json_path = host_bootstrap_critique_path(art)
    json_path.unlink(missing_ok=True)
    md_path = art / "host_bootstrap_critique.md"
    rel_art = str(art.relative_to(pipeline_config.repo_root))
    extra_context = {"bootstrap_artifacts_path": rel_art}
    base_msg = (
        f"Host bootstrap critique round={round_idx} engine={engine_name}. "
        "Read query_host_bootstrap.md + register/differential/CMake only. "
        f"Your LAST action must be writing `{json_path}` (pass|fail) and "
        f"`{md_path}`. Codex shell: `cat > {json_path} <<'EOF' ... EOF`. "
        "Then stop. Do not edit adapter, harness, or CMake sources."
    )

    def invoke(extra: str) -> Any:
        return run_agent_stage(
            stage=CRITIQUE_STAGE,
            prompt_file="host_bootstrap_critique.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=extra or base_msg,
            model_overrides=model_overrides,
            dry_run=dry_run,
            extra_context=extra_context,
            session_role=CRITIQUE_ROLE,
        )

    return run_until_artifact(artifact=json_path, invoke=invoke, attempts=2, dry_run=dry_run)
