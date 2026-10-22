"""Architecture-audit agent loop for engine_backend workloads.

A read-only audit agent judges hard constraints (native scan, real mutation
hooks, fail-closed harnesses, host glue). On failure it re-invokes
implementation with actionable feedback. No regex heuristics.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.gates import GateFailure
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.stages.engine_layer_critique import run_until_artifact


def load_architecture_audit(art: Path) -> dict[str, Any]:
    path = art / "architecture_audit.json"
    if not path.is_file():
        raise GateFailure(
            "architecture audit agent did not write architecture_audit.json — "
            "cannot accept implementation"
        )
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise GateFailure(f"architecture_audit.json is not valid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise GateFailure("architecture_audit.json must be a JSON object")
    status = str(data.get("status") or "").strip().lower()
    if status not in {"pass", "fail"}:
        raise GateFailure(
            "architecture_audit.json missing status pass|fail — "
            f"got {data.get('status')!r}"
        )
    data["status"] = status
    return data


def format_implementation_feedback(audit: dict[str, Any], *, round_idx: int) -> str:
    feedback = str(audit.get("feedback_for_implementation") or "").strip()
    violations = audit.get("violations") or []
    lines = [
        "## Architecture audit FAILED — fix before validation",
        "",
        f"- audit round: {round_idx}",
        f"- summary: {audit.get('summary') or '(none)'}",
        "",
        "### Violations",
    ]
    if isinstance(violations, list) and violations:
        for v in violations:
            if not isinstance(v, dict):
                lines.append(f"- {v}")
                continue
            rule = v.get("rule", "constraint")
            layer = v.get("layer") or ""
            evidence = v.get("evidence", "")
            paths = v.get("paths") or []
            path_s = ", ".join(f"`{p}`" for p in paths) if paths else ""
            prefix = f"**{rule}**" + (f" (layer `{layer}`)" if layer else "")
            lines.append(f"- {prefix}: {evidence}" + (f" ({path_s})" if path_s else ""))
    else:
        lines.append("- (see feedback below)")
    lines.append("")
    lines.append("### Required fixes")
    lines.append(feedback or "(auditor left feedback_for_implementation empty — re-read violations)")
    lines.append("")
    lines.append(
        "Resolve every violation named for this layer against `layers.md` + "
        "`acceptance_checklist.md`, however large the change: a missing mechanism or "
        "placement is built now, not deferred. A gate that passes without exercising a "
        "violation does not resolve it: first add the harness assertion that fails on it, "
        "then make it pass. Do not weaken the harness to greenwash. Mid-loop MCP "
        "`engine_build` plus that layer's gate until green. The orchestrator re-runs only "
        "layers named in `violations[].layer` (pipeline order), then re-audits."
    )
    return "\n".join(lines)


def failing_layer_ids(audit: dict[str, Any], workload: dict[str, Any]) -> list[str]:
    """Owning layers named in audit violations, in implementation order (unique)."""
    order = [
        str(layer.get("id") or "")
        for layer in (workload.get("implementation_layers") or [])
        if isinstance(layer, dict)
    ]
    order = [lid for lid in order if lid]
    mentioned: set[str] = set()
    for item in audit.get("violations") or []:
        if not isinstance(item, dict):
            continue
        lid = str(item.get("layer") or "").strip()
        if lid in order:
            mentioned.add(lid)
    return [lid for lid in order if lid in mentioned]


def earliest_failing_layer_id(audit: dict[str, Any], workload: dict[str, Any]) -> str | None:
    """Earliest implementation layer named in audit violations, or None."""
    ids = failing_layer_ids(audit, workload)
    return ids[0] if ids else None


def run_architecture_audit_agent(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None,
    dry_run: bool,
    *,
    round_idx: int,
    user_message: str = "",
) -> Any:
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    json_path = art / "architecture_audit.json"
    engine = str(workload.get("engine_name") or "")
    write_rule = (
        f" Your LAST action must be writing `{json_path}` (pass|fail) and "
        f"`{art / 'architecture_audit.md'}`. Codex shell: "
        f"`cat > {json_path} <<'EOF' ... EOF`. Then stop. Do not edit adapters."
    )
    base_msg = (
        user_message.strip()
        if user_message.strip()
        else (
            f"Architecture audit round={round_idx} for engine={engine}. "
            "Read constraint docs + adapter + layer harnesses."
        )
    ) + write_rule

    def invoke(extra: str) -> Any:
        msg = base_msg if not extra else extra
        return run_agent_stage(
            stage="architecture_audit",
            prompt_file="04_architecture_audit.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=msg,
            model_overrides=model_overrides,
            dry_run=dry_run,
        )

    return run_until_artifact(
        artifact=json_path, invoke=invoke, attempts=2, dry_run=dry_run
    )


def run_architecture_audit_with_impl_retry(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None,
    dry_run: bool,
    *,
    skip_gates: bool = False,
    run_implementation_fn,
) -> dict[str, Any]:
    """Audit after implementation; on fail, re-run implementation with feedback.

    ``run_implementation_fn`` is injected to avoid circular imports
    (``stages.engine_backend.run_implementation``).
    """
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    art.mkdir(parents=True, exist_ok=True)

    max_rounds = int(workload.get("architecture_audit_max_rounds") or 3)
    max_rounds = max(1, max_rounds)
    engine = str(workload.get("engine_name") or "")

    last_audit: dict[str, Any] = {}
    for round_idx in range(1, max_rounds + 1):
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- architecture audit round {round_idx}/{max_rounds}",
            workload=workload,
        )

        if dry_run:
            last_audit = {
                "engine_name": engine,
                "status": "pass",
                "summary": "dry-run skip",
                "violations": [],
                "feedback_for_implementation": "",
                "round": round_idx,
            }
            (art / "architecture_audit.json").write_text(
                json.dumps(last_audit, indent=2) + "\n", encoding="utf-8"
            )
            return last_audit

        run_architecture_audit_agent(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            round_idx=round_idx,
            user_message=(
                f"Architecture audit round={round_idx}/{max_rounds} engine={engine}. "
                "Read layers.md, acceptance_checklist.md, adapter sources, and each "
                "layer harness. On fail set violations[].layer. Write architecture_audit.json."
            ),
        )
        last_audit = load_architecture_audit(art)
        last_audit["round"] = round_idx
        (art / "architecture_audit.json").write_text(
            json.dumps(last_audit, indent=2) + "\n", encoding="utf-8"
        )

        if last_audit["status"] == "pass":
            append_experiment_log(
                pipeline_config.repo_root,
                query,
                f"- architecture audit round {round_idx}: PASS",
                workload=workload,
            )
            return last_audit

        feedback = format_implementation_feedback(last_audit, round_idx=round_idx)
        feedback_path = art / f"architecture_audit_feedback_round{round_idx}.md"
        feedback_path.write_text(feedback + "\n", encoding="utf-8")
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- architecture audit round {round_idx}: FAIL — "
            f"re-invoking implementation (see {feedback_path.name})",
            workload=workload,
        )

        if round_idx >= max_rounds:
            break

        named = failing_layer_ids(last_audit, workload)
        if named:
            append_experiment_log(
                pipeline_config.repo_root,
                query,
                f"- architecture audit fix-up: re-running impl layers {', '.join(named)}",
                workload=workload,
            )
            for lid in named:
                run_implementation_fn(
                    pipeline_config,
                    model_config,
                    workload_name,
                    query,
                    model_overrides or {},
                    dry_run,
                    skip_gates=skip_gates,
                    extra_user_message=feedback,
                    only_layer_id=lid,
                )
        else:
            run_implementation_fn(
                pipeline_config,
                model_config,
                workload_name,
                query,
                model_overrides or {},
                dry_run,
                skip_gates=skip_gates,
                extra_user_message=feedback,
            )

    # Every layer gate passes; what the audit still asks moves to validation (a required fix
    # there, next to the adversarial tests and slot gates) instead of stopping the backend run.
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    (art / "architecture_audit.open_findings.md").write_text(feedback + "\n", encoding="utf-8")
    append_experiment_log(
        pipeline_config.repo_root,
        query,
        f"- architecture audit: open findings after {max_rounds} round(s) handed to validation "
        "(architecture_audit.open_findings.md)",
        workload=workload,
    )
