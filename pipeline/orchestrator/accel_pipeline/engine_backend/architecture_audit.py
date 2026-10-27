"""Architecture audit for engine_backend workloads.

One read-only audit session judges the hard constraints (native scan, real mutation hooks,
fail-closed harnesses, host glue) and writes a report. Each required fix goes to the main session of
the layer that owns it, through that layer's repair loop. A report that cannot be read is recorded as
status `error`, never raised. No regex heuristics.
"""

from __future__ import annotations

import json
from collections.abc import Callable
from pathlib import Path
from typing import Any

from accel_pipeline.engine_backend.backend_digest import backend_digest
from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.workload import load_run_workload, query_artifacts_dir
from accel_pipeline.runlog import append_experiment_log
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.engine_backend.layer_critique import read_review, run_until_artifact


AUDIT_ROLE = "audit"


def load_architecture_audit(art: Path) -> dict[str, Any]:
    return read_review(art / "architecture_audit.json", what="architecture audit")


def format_implementation_feedback(audit: dict[str, Any], *, round_idx: int, layer: str = "") -> str:
    """The report as feedback; with `layer`, only the violations that layer owns."""
    feedback = str(audit.get("feedback_for_implementation") or "").strip()
    violations = [
        v for v in audit.get("violations") or []
        if not layer or not isinstance(v, dict) or str(v.get("layer") or layer) == layer
    ]
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
        "`engine_build` plus this layer's gate until green."
    )
    return "\n".join(lines)


def owning_layer_ids(audit: dict[str, Any], workload: dict[str, Any]) -> list[str]:
    """The layers that own the violations, in implementation order. A violation that names no
    layer belongs to the last one (the accelerator-facing surface sees every layer's work)."""
    order = [
        str(layer.get("id") or "")
        for layer in (workload.get("implementation_layers") or [])
        if isinstance(layer, dict) and layer.get("id")
    ]
    owners: set[str] = set()
    for item in audit.get("violations") or []:
        lid = str(item.get("layer") or "").strip() if isinstance(item, dict) else ""
        owners.add(lid if lid in order else order[-1])
    return [lid for lid in order if lid in owners]


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

    json_path.unlink(missing_ok=True)

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
            session_role=AUDIT_ROLE,
        )

    return run_until_artifact(artifact=json_path, invoke=invoke, attempts=2, dry_run=dry_run)


def run_architecture_audit(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None,
    dry_run: bool,
    *,
    repair_layer: Callable[[str, str], None],
) -> dict[str, Any]:
    """Audit once (`architecture_audit_max_rounds` passes at most, each after the fixes of the one
    before); the owning layers fix their violations through `repair_layer(layer_id, feedback)`."""
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    art.mkdir(parents=True, exist_ok=True)
    max_rounds = max(1, int(workload.get("architecture_audit_max_rounds") or 1))
    engine = str(workload.get("engine_name") or "")

    def log(line: str) -> None:
        append_experiment_log(pipeline_config.repo_root, query, f"- architecture audit {line}", workload=workload)

    audit: dict[str, Any] = {}
    for round_idx in range(1, max_rounds + 1):
        log(f"round {round_idx}/{max_rounds}")
        if dry_run:
            audit = {"engine_name": engine, "status": "pass", "summary": "dry-run skip", "violations": [],
                     "feedback_for_implementation": "", "round": round_idx}
            (art / "architecture_audit.json").write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
            return audit

        run_architecture_audit_agent(
            pipeline_config, model_config, workload_name, query, model_overrides, dry_run,
            round_idx=round_idx,
            user_message=(
                backend_digest(pipeline_config.repo_root, engine, art)
                + f"Layer diffs: `{art}/layer_diffs/<layer>.diff`.\n\n"
                + f"Architecture audit round={round_idx}/{max_rounds} engine={engine}. "
                if round_idx == 1
                else f"Architecture audit round={round_idx}/{max_rounds}: the layers named in your last "
                "report were fixed. Re-check those violations and what the fixes touched. "
            ) + "On fail set violations[].layer. Write architecture_audit.json.",
        )
        audit = load_architecture_audit(art)
        audit["round"] = round_idx
        (art / "architecture_audit.json").write_text(json.dumps(audit, indent=2) + "\n", encoding="utf-8")
        if audit["status"] != "fail":
            log(f"round {round_idx}: {audit['status'].upper()}" + (f" ({audit['summary']})" if audit["status"] == "error" else ""))
            return audit

        owners = owning_layer_ids(audit, workload)
        log(f"round {round_idx}: FAIL, fixes routed to layers {', '.join(owners)}")
        for lid in owners:
            feedback = format_implementation_feedback(audit, round_idx=round_idx, layer=lid)
            (art / f"architecture_audit_feedback_round{round_idx}_{lid}.md").write_text(feedback + "\n", encoding="utf-8")
            repair_layer(lid, feedback)

    log(f"fixes of round {max_rounds} routed; validation judges them")
    return audit
