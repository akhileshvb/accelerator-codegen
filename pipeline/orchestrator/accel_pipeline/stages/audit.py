from __future__ import annotations

from accel_pipeline.audit_digest import (
    AUDIT_FINDINGS_FILE,
    IMPLEMENTATION_BASE_FILE,
    audit_context,
    blocking_findings,
    changed_files,
    cumulative_diff,
    numbered,
    snapshot,
    touched_since,
)
from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.failure import Failure
from accel_pipeline.jsonio import read_json
from accel_pipeline.layout import query_dir
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.repair import RepairCtx, repair_turn, run_with_repair
from accel_pipeline.session import READONLY
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.stages.gate_plan import workload_post_audit_gates

AUDIT_TASK = (
    "Produce final_audit.md: verify correctness, cite SF1 sweep numbers, "
    "and review scale-safety + harness fairness (no brittle gate chasing).\n\n"
    "Start from the query digest and the cumulative diff below; read a file only where they leave a question open."
)


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    workload = load_run_workload(pipeline_config, workload_name)

    def audit_turn(message: str):
        return run_agent_stage(
            stage="audit",
            prompt_file="04_audit.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=message,
            model_overrides=model_overrides,
            dry_run=dry_run,
            session_role=READONLY,
        )

    if dry_run:
        return audit_turn(AUDIT_TASK)
    # a failing audit gate goes to the repair loop (the fix runs in the implementation session, not the reviewer's)
    ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="audit",
                    change_kind="audit", model_overrides=model_overrides)

    def audit_and_fix():
        result = audit_turn(f"{AUDIT_TASK}\n\n{audit_context(pipeline_config, workload, query)}")
        _fix_blocking_findings(ctx, audit_turn)
        return result

    return run_with_repair(ctx, audit_and_fix, gates=workload_post_audit_gates(workload))


def _audit_result(ctx: RepairCtx) -> dict:
    return read_json(ctx.art / "audit_result.json")


def _fix_blocking_findings(ctx: RepairCtx, audit_turn) -> None:
    """The audit's blocking findings go to the implementation session as one batched repair round; the
    audit session then re-checks only those findings. What is still blocking is recorded, not looped on."""
    findings = blocking_findings(_audit_result(ctx))
    if not findings:
        return
    repo_root = ctx.pipeline_config.repo_root
    qdir = query_dir(str(ctx.workload.get("benchmark_name") or "tpch"), ctx.query)
    base_file = ctx.art / IMPLEMENTATION_BASE_FILE
    base = base_file.read_text(encoding="utf-8").strip() if base_file.is_file() else "HEAD"
    before = snapshot(repo_root, changed_files(repo_root, base, qdir))
    ctx.log(f"audit raised {len(findings)} blocking findings; one batched repair round")
    repair_turn(ctx, Failure(
        kind="code", stage="audit", source="audit_findings",
        summary=f"The independent audit found {len(findings)} blocking problems. Fix each in your query's files "
                "(or, if one is wrong, say why in the experiment log and leave it).",
        evidence=numbered(findings)), must_change=False)
    touched = touched_since(repo_root, before, base, qdir)
    audit_turn(
        "Re-check only these findings against the current tree and update audit_result.json and final_audit.md "
        "(a finding that is resolved leaves the lists; one that is not stays, with its evidence).\n\n"
        f"{numbered(findings)}\n\nFiles changed by the repair round: {', '.join(touched) or 'none'}\n\n"
        f"```diff\n{cumulative_diff(repo_root, base, touched) or '(no changes)'}\n```"
    )
    remaining = blocking_findings(_audit_result(ctx))
    report = ctx.art / AUDIT_FINDINGS_FILE
    if remaining:
        report.write_text("# Audit findings still open after the repair round\n\n" + numbered(remaining) + "\n",
                          encoding="utf-8")
        append_experiment_log(repo_root, ctx.query, f"- audit: {len(remaining)} findings still open (see {report.name})",
                              workload=ctx.workload)
    else:
        report.unlink(missing_ok=True)
        ctx.log("the re-check cleared every blocking finding")
