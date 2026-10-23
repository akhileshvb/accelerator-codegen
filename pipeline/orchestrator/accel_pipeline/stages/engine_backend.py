"""Engine backend onboarding stages (substrate-first, sequential layers)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.engine_quarantine import apply_foreign_engine_quarantine, restore_quarantine
from accel_pipeline.gate_capture import format_reloop_failure_sections
from accel_pipeline.engine_host import prepare_host_sdk_payload
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.gates import GateFailure, GateResult, gate_failure_message
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.stages.engine_architecture_audit import (
    run_architecture_audit_with_impl_retry,
)
from accel_pipeline.stages.engine_layer_critique import (
    format_layer_critique_feedback,
    load_layer_critique,
    run_layer_critique_agent,
)
from accel_pipeline.stages.gate_plan import (
    run_post_stage_gates,
    run_post_stage_gates_collect,
)


def implementation_layers(workload: dict) -> list[dict[str, Any]]:
    return [dict(layer) for layer in (workload.get("implementation_layers") or [])]


def layer_stage_name(layer_id: str) -> str:
    return f"impl_{layer_id}"


def _prepare_engine_host_sdk(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
) -> None:
    payload = prepare_host_sdk_payload(
        workload, pipeline_config.repo_root, pipeline_config.build_dir
    )
    if payload is None:
        return
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    art.mkdir(parents=True, exist_ok=True)
    (art / "host_sdk_discovery.json").write_text(
        json.dumps(payload, indent=2) + "\n", encoding="utf-8"
    )


def _engine_exists(pipeline_config: PipelineConfig, workload: dict) -> bool:
    targets = workload.get("target_paths") or {}
    header = targets.get("host_header")
    if not header:
        return False
    return (pipeline_config.repo_root / header).is_file()


def _layer_should_run(workload: dict, layer: dict) -> bool:
    skip_unless = str(layer.get("skip_unless") or "")
    if skip_unless == "requires_in_process_host":
        return bool(workload.get("requires_in_process_host", False))
    return True


def _user_message(
    pipeline_config: PipelineConfig,
    workload: dict,
    stage: str,
    *,
    extra: str = "",
    layer: dict | None = None,
) -> str:
    engine = workload.get("engine_name", "engine")
    exists = _engine_exists(pipeline_config, workload)
    mode = (
        "update_existing"
        if exists and workload.get("allow_update_existing", True)
        else "generate_from_scratch"
    )
    base = (
        f"Engine backend stage={stage} engine={engine} mode={mode}. "
        "Primary context is the fixed substrate (aux/runtime skeleton: the flow is "
        "frozen, you implement its slots S1-S8). "
        "Treat this as from-scratch onboarding: implement only this engine's native "
        "slot implementations using templates + the engine profile. Do not read or copy "
        "any other engine tree. Follow pipeline/prompts/engine_backend/layers.md."
    )
    if layer:
        gate = layer.get("gate")
        harness = layer.get("harness")
        base += (
            f" This layer is `{layer.get('id')}` ({layer.get('title')}). "
            f"Ship the layer objects plus `{harness}`. MCP tools on server "
            f"`accel_pipeline` (tools, not resources — do not list_mcp_resources): "
            f"`run_engine_build()`, `run_layer_gate()`, or "
            f"`request_gate(gate_name=\"engine_build\")` then "
            f"`request_gate(gate_name=\"{gate}\")`. After each increment call those "
            f"until both return status=pass. Do not substitute host cmake or "
            f"`docker exec` of the harness for the MCP gates. "
            f"host_runtime.kind=`{(workload.get('host_runtime') or {}).get('kind') or 'host_cmake'}`. "
            "Do not implement later layers' contracts, but the wiring this "
            "layer's contract needs (plugin or extension registration, SQL "
            "that host.sh installs, a binding into another layer's object) is "
            "this layer's work, wherever the file lives. Prior layers: reuse; "
            "smallest patch only if a real bug/design flaw blocks this layer — "
            "then re-gate the prior layer too. You run unattended: never stop "
            "to ask for scope or permission; make the change."
        )
    if extra.strip():
        return base + "\n\n" + extra.strip()
    return base


def _layer_extra_context(workload: dict, layer: dict) -> dict[str, str]:
    prior = []
    for item in implementation_layers(workload):
        if item.get("id") == layer.get("id"):
            break
        prior.append(f"- `{item.get('id')}` ({item.get('title')}): `{item.get('harness')}`")
    host = dict(workload.get("host_runtime") or {})
    return {
        "layer_id": str(layer.get("id") or ""),
        "layer_title": str(layer.get("title") or ""),
        "layer_gate": str(layer.get("gate") or ""),
        "layer_harness": str(layer.get("harness") or ""),
        "layer_cmake_target": str(layer.get("cmake_target") or ""),
        "prior_layers": "\n".join(prior) or "- (this is the first layer)",
        "host_runtime_kind": str(host.get("kind") or "host_cmake"),
        "layer_rules": ", ".join(str(r) for r in (layer.get("rules") or [])) or "(none)",
    }


_GATE_LOG_TAIL_CHARS = 8000


def _carried_layer_feedback(art: Path, lid: str) -> str:
    """The feedback the previous run of this layer stopped on (a relaunch starts from it), or ""
    when that run passed or there is none."""
    if (art / f"layer_{lid}.passed").exists():
        return ""
    rounds = list(art.glob(f"layer_gate_{lid}_feedback_round*.md")) + list(
        art.glob(f"layer_critique_{lid}_feedback_round*.md")
    )
    if not rounds:
        return ""
    last = max(rounds, key=lambda p: p.stat().st_mtime)
    return (
        "## Carried over: the previous run of this layer stopped on this feedback\n\n"
        + last.read_text(encoding="utf-8")
    )


def format_layer_gate_feedback(
    failed: GateResult,
    *,
    layer_id: str,
    round_idx: int,
    max_rounds: int,
) -> str:
    data = failed.data or {}
    path = str(failed.path) if failed.path else ""
    lines = [
        "## Layer gate FAILED — fix from this trace; this stage will re-run implementation and gates",
        "",
        f"- layer: `{layer_id}`",
        f"- gate: `{failed.name}`",
        f"- status: `{failed.status}`",
        f"- attempt: {round_idx}/{max_rounds}",
    ]
    if path:
        lines.append(f"- gate json: `{path}`")
    lines.extend(format_reloop_failure_sections(data, max_chars=_GATE_LOG_TAIL_CHARS))
    lines.extend(
        [
            "",
            "Fix the adapter, harness, and/or CMake so this gate passes. "
            "Mid-loop MCP `request_gate` `engine_build` then this layer's gate "
            "until both are green. Do not start a later layer. Do not ignore this log.",
        ]
    )
    return "\n".join(lines)


def run_analysis(pipeline_config, model_config, workload_name, query, model_overrides, dry_run):
    workload = load_run_workload(pipeline_config, workload_name)
    return run_agent_stage(
        stage="analysis",
        prompt_file="01_analysis.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=_user_message(pipeline_config, workload, "analysis"),
        model_overrides=model_overrides,
        dry_run=dry_run,
    )


def run_implementation_layer(
    pipeline_config,
    model_config,
    workload_name,
    query,
    model_overrides,
    dry_run,
    layer: dict,
    *,
    skip_gates=False,
    extra_user_message: str = "",
):
    workload = load_run_workload(pipeline_config, workload_name)
    stage = layer_stage_name(str(layer.get("id") or "layer"))
    extra_context = _layer_extra_context(workload, layer)
    lid = str(layer.get("id") or "")
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    art.mkdir(parents=True, exist_ok=True)
    extra = "\n\n".join(x for x in (extra_user_message.strip(), _carried_layer_feedback(art, lid)) if x)
    (art / f"layer_{lid}.passed").unlink(missing_ok=True)
    configured_rounds = int(workload.get("layer_critique_max_rounds") or 0)
    run_critique = (not dry_run) and configured_rounds > 0
    max_rounds = 1 if dry_run or not run_critique else max(1, configured_rounds)
    configured_gate = int(workload.get("layer_gate_max_rounds") or 0)
    max_gate = 1 if dry_run or skip_gates else max(1, configured_gate)

    result = None
    gate_fails = 0
    critique_round = 0
    while True:
        result = run_agent_stage(
            stage=stage,
            prompt_file=str(layer.get("prompt") or "02_implementation.md"),
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=_user_message(
                pipeline_config,
                workload,
                stage,
                extra=extra,
                layer=layer,
            ),
            model_overrides=model_overrides,
            dry_run=dry_run,
            extra_context=extra_context,
        )
        if dry_run:
            return result
        if not skip_gates:
            gates = [g for g in ("engine_build", str(layer.get("gate") or "")) if g]
            _results, failed = run_post_stage_gates_collect(
                pipeline_config,
                workload_name,
                query,
                gates,
                change_kind=stage,
                iteration=gate_fails,
            )
            if failed is not None:
                gate_fails += 1
                feedback = format_layer_gate_feedback(
                    failed,
                    layer_id=lid,
                    round_idx=gate_fails,
                    max_rounds=max_gate,
                )
                art = query_artifacts_dir(
                    pipeline_config.repo_root, query, workload=workload
                )
                art.mkdir(parents=True, exist_ok=True)
                feedback_path = art / f"layer_gate_{lid}_feedback_round{gate_fails}.md"
                feedback_path.write_text(feedback + "\n", encoding="utf-8")
                append_experiment_log(
                    pipeline_config.repo_root,
                    query,
                    f"- layer `{lid}` gate `{failed.name}` FAIL "
                    f"attempt {gate_fails}/{max_gate} — "
                    f"re-invoking implementation (see {feedback_path.name})",
                    workload=workload,
                )
                extra = "\n\n".join(
                    x for x in (extra_user_message.strip(), feedback) if x
                )
                if gate_fails >= max_gate:
                    raise GateFailure(
                        f"layer `{lid}` post-stage gate still failing after "
                        f"{max_gate} implementation attempt(s): "
                        f"{gate_failure_message(failed)}"
                    )
                continue
        if not run_critique:
            (art / f"layer_{lid}.passed").touch()
            return result

        critique_round += 1
        art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
        art.mkdir(parents=True, exist_ok=True)
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- layer critique `{lid}` round {critique_round}/{max_rounds}",
            workload=workload,
        )
        run_layer_critique_agent(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            layer,
            round_idx=critique_round,
            extra_context=extra_context,
        )
        critique = load_layer_critique(art, lid)
        critique["round"] = critique_round
        (art / f"layer_critique_{lid}.json").write_text(
            json.dumps(critique, indent=2) + "\n", encoding="utf-8"
        )
        if critique["status"] == "pass":
            append_experiment_log(
                pipeline_config.repo_root,
                query,
                f"- layer critique `{lid}` round {critique_round}: PASS",
                workload=workload,
            )
            (art / f"layer_{lid}.passed").touch()
            return result

        feedback = format_layer_critique_feedback(critique, round_idx=critique_round)
        feedback_path = art / f"layer_critique_{lid}_feedback_round{critique_round}.md"
        feedback_path.write_text(feedback + "\n", encoding="utf-8")
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- layer critique `{lid}` round {critique_round}: FAIL — "
            f"re-invoking implementation (see {feedback_path.name})",
            workload=workload,
        )
        extra = "\n\n".join(
            x for x in (extra_user_message.strip(), feedback) if x
        )
        gate_fails = 0
        if critique_round >= max_rounds:
            # The layer's gates pass; what the critique still asks moves to the architecture
            # audit (a required fix there) instead of stopping the whole backend run.
            (art / f"layer_{lid}.open_findings.md").write_text(feedback + "\n", encoding="utf-8")
            append_experiment_log(
                pipeline_config.repo_root,
                query,
                f"- layer `{lid}` accepted with open critique findings after {max_rounds} round(s) "
                f"(layer_{lid}.open_findings.md, for the architecture audit)",
                workload=workload,
            )
            (art / f"layer_{lid}.passed").touch()
            return result


def run_implementation(
    pipeline_config,
    model_config,
    workload_name,
    query,
    model_overrides,
    dry_run,
    *,
    skip_gates=False,
    extra_user_message: str = "",
    only_layer_id: str | None = None,
    from_layer_id: str | None = None,
):
    workload = load_run_workload(pipeline_config, workload_name)
    known = {str(layer.get("id") or "") for layer in implementation_layers(workload)}
    if from_layer_id and from_layer_id not in known:
        from_layer_id = None
    started = from_layer_id is None
    result = None
    for layer in implementation_layers(workload):
        if not _layer_should_run(workload, layer):
            continue
        lid = str(layer.get("id") or "")
        if only_layer_id and lid != only_layer_id:
            continue
        if from_layer_id and not started:
            if lid == from_layer_id:
                started = True
            else:
                continue
        result = run_implementation_layer(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            layer,
            skip_gates=skip_gates,
            extra_user_message=extra_user_message,
        )
    return result


def run_validation(
    pipeline_config, model_config, workload_name, query, model_overrides, dry_run, *, skip_gates=False
):
    workload = load_run_workload(pipeline_config, workload_name)
    result = run_agent_stage(
        stage="validation",
        prompt_file="03_validation.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=_user_message(pipeline_config, workload, "validation"),
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
    if dry_run or skip_gates:
        return result
    gates = list(
        workload.get("post_validation_gates")
        or [
            "engine_build",
            "engine_capture",
            "engine_epochs",
            "engine_scan",
            "engine_runtime",
            "engine_host",
            "engine_api_conformance",
            "engine_adversarial",
            "slot_epoch_stress",
            "slot_hits_under_writes",
            "slot_read_after_commit",
            "slot_txn_edges",
            "slot_restart",
            "slot_own_writes",
            "slot_ddl_events",
            "slot_tsan",
        ]
    )
    run_post_stage_gates(pipeline_config, workload_name, query, gates)
    return result


def run_engine_pipeline(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str],
    dry_run: bool,
    *,
    stage: str = "all",
    skip_gates: bool = False,
) -> None:
    workload = load_run_workload(pipeline_config, workload_name)
    _prepare_engine_host_sdk(pipeline_config, workload, query)
    qsession = None if dry_run else apply_foreign_engine_quarantine(
        pipeline_config.repo_root, workload
    )
    try:
        if stage == "analysis":
            run_analysis(
                pipeline_config, model_config, workload_name, query, model_overrides, dry_run
            )
            return
        if stage.startswith("impl_"):
            only = stage[len("impl_") :]
            run_implementation(
                pipeline_config,
                model_config,
                workload_name,
                query,
                model_overrides,
                dry_run,
                skip_gates=skip_gates,
                only_layer_id=only,
            )
            return
        if stage == "implementation":
            run_implementation(
                pipeline_config,
                model_config,
                workload_name,
                query,
                model_overrides,
                dry_run,
                skip_gates=skip_gates,
            )
            if not skip_gates:
                run_architecture_audit_with_impl_retry(
                    pipeline_config,
                    model_config,
                    workload_name,
                    query,
                    model_overrides,
                    dry_run,
                    skip_gates=skip_gates,
                    run_implementation_fn=run_implementation,
                )
            return
        if stage == "architecture_audit":
            run_architecture_audit_with_impl_retry(
                pipeline_config,
                model_config,
                workload_name,
                query,
                model_overrides,
                dry_run,
                skip_gates=skip_gates,
                run_implementation_fn=run_implementation,
            )
            return
        if stage in ("validation", "audit"):
            run_validation(
                pipeline_config,
                model_config,
                workload_name,
                query,
                model_overrides,
                dry_run,
                skip_gates=skip_gates,
            )
            return

        run_analysis(
            pipeline_config, model_config, workload_name, query, model_overrides, dry_run
        )
        run_implementation(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            skip_gates=skip_gates,
        )
        run_architecture_audit_with_impl_retry(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            skip_gates=skip_gates,
            run_implementation_fn=run_implementation,
        )
        run_validation(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            skip_gates=skip_gates,
        )
    finally:
        restore_quarantine(qsession)
