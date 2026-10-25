from __future__ import annotations

import json
from dataclasses import dataclass

from accel_pipeline.config import PipelineConfig
from accel_pipeline.pipeline_log import log_structural_reanalysis_request
from accel_pipeline.structural_reanalysis import (
    BOTTLENECK_TYPES,
    max_structural_reanalysis_count,
    write_request,
)
from accel_pipeline.config import load_run_workload
from accel_pipeline.state import load_or_create


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int
    stage: str
    workload_name: str
    optimization_iter: int = 0


def make_reanalysis_tools(ctx: ToolContext):
    def request_structural_reanalysis(
        bottleneck_type: str,
        evidence: str,
        proposed_direction: str,
        current_candidate_id: str,
        alternate_candidate_ids: list[str] | None = None,
        cost_model_insufficient_reason: str | None = "",
    ) -> str:
        """Request a bounded structural re-analysis (max 2 per pipeline run).

        Use only when a structural bottleneck is confirmed — e.g. residual O(n) state scan,
        scan-for-scan swap, wrong partition grain, or maintenance semantics that cannot be fixed
        by cost-model tuning or switching among viable alternates already in analysis.json.

        Do NOT use for coefficient tweaks, EWMA tuning, or minor threshold changes.
        """
        cost_model_insufficient_reason = str(cost_model_insufficient_reason or "")
        workload = load_run_workload(ctx.pipeline_config, ctx.workload_name)
        state = load_or_create(
            ctx.pipeline_config.repo_root, ctx.query, ctx.workload_name, ctx.pipeline_config.engine_name, resume=True
        )
        if state.structural_reanalysis_count >= max_structural_reanalysis_count(workload):
            log_structural_reanalysis_request(
                ctx.pipeline_config.repo_root,
                ctx.query,
                workload,
                status="rejected",
                source_stage=ctx.stage,
                source_iteration=ctx.optimization_iter,
                bottleneck_type=bottleneck_type,
                current_candidate_id=current_candidate_id,
                reason=f"budget exhausted ({state.structural_reanalysis_count})",
            )
            return json.dumps(
                {
                    "status": "rejected",
                    "reason": f"structural reanalysis budget exhausted ({state.structural_reanalysis_count})",
                }
            )

        try:
            payload = write_request(
                ctx.pipeline_config.repo_root,
                ctx.query,
                workload,
                source_stage=ctx.stage,
                source_iteration=ctx.optimization_iter,
                bottleneck_type=bottleneck_type,
                evidence=evidence,
                proposed_direction=proposed_direction,
                current_candidate_id=current_candidate_id,
                alternate_candidate_ids=alternate_candidate_ids,
                cost_model_insufficient_reason=cost_model_insufficient_reason,
            )
        except ValueError as exc:
            log_structural_reanalysis_request(
                ctx.pipeline_config.repo_root,
                ctx.query,
                workload,
                status="rejected",
                source_stage=ctx.stage,
                source_iteration=ctx.optimization_iter,
                bottleneck_type=bottleneck_type,
                current_candidate_id=current_candidate_id,
                reason=str(exc),
            )
            return json.dumps({"status": "rejected", "reason": str(exc), "allowed_bottleneck_types": sorted(BOTTLENECK_TYPES)})

        allowed = sorted(BOTTLENECK_TYPES)
        log_structural_reanalysis_request(
            ctx.pipeline_config.repo_root,
            ctx.query,
            workload,
            status="queued",
            source_stage=ctx.stage,
            source_iteration=ctx.optimization_iter,
            bottleneck_type=payload["bottleneck_type"],
            current_candidate_id=payload["current_candidate_id"],
        )
        return json.dumps(
            {
                "status": "queued",
                "message": (
                    "Request recorded. The orchestrator will re-run analysis after this stage "
                    "if the request is valid and budget remains."
                ),
                "bottleneck_type": payload["bottleneck_type"],
                "allowed_bottleneck_types": allowed,
                "validation_errors": [],
            }
        )

    return request_structural_reanalysis
