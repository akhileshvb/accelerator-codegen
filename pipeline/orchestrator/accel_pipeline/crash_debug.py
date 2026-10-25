"""Post-stage gates with crash debugging: a failing gate goes to the repair loop (repair.py)."""

from __future__ import annotations

from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.failure import extract_crash_highlights  # noqa: F401 - re-exported
from accel_pipeline.gates.run import GateResult
from accel_pipeline.repair import RepairCtx, run_with_repair


def run_post_stage_gates_with_crash_debug(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    gate_names: list[str],
    *,
    change_kind: str = "",
    iteration: int = 0,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    max_rounds: int | None = None,
) -> list[GateResult]:
    """Run gates; on failure repair (at most `max_rounds` code rounds) until they pass, or stop the run cleanly."""
    if dry_run:
        return []
    ctx = RepairCtx(
        pipeline_config, model_config, workload_name, query, stage=change_kind or "gates",
        change_kind=change_kind, iteration=iteration, model_overrides=model_overrides, max_code_rounds=max_rounds,
    )
    run_with_repair(ctx, lambda: None, gates=gate_names)
    return ctx.results
