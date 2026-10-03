"""Entry of the Verus pipeline: ``run_pipeline --mode verus`` forks here."""

from __future__ import annotations

import sys
from typing import Any

from accel_pipeline.verus.context import build_context
from accel_pipeline.verus.stages import STAGE_FUNCS, STAGES, PipelineStop, Run, VerusState


def run_verus_pipeline(
    pipeline_config: Any,
    model_config: Any,
    workload: dict[str, Any],
    workload_name: str,
    query: int,
    *,
    stage: str = "all",
    resume: bool = False,
    dry_run: bool = False,
    model_overrides: dict[str, str] | None = None,
) -> int:
    ctx = build_context(pipeline_config, workload, workload_name, query)
    ctx.save()
    state = VerusState.load(ctx) if resume or stage != "all" else VerusState()
    run = Run(ctx, state, workload, pipeline_config, model_config, model_overrides or {}, dry_run)
    if stage != "all":
        if stage not in STAGES:
            print(f"unknown Verus stage {stage!r}; stages: {', '.join(STAGES)}", file=sys.stderr)
            return 2
        todo = [stage]
        state.done = [s for s in state.done if s != stage]  # asked for explicitly: run it again
    else:
        start = STAGES.index(state.stage) if resume and state.stage in STAGES else 0
        todo = list(STAGES[start:])
    for name in todo:
        print(f"=== verus stage: {name} ===", file=sys.stderr)
        if not dry_run:
            state.stage = name
            state.save(ctx)
        try:
            STAGE_FUNCS[name](run)
        except PipelineStop as exc:
            run.note(f"stopped in {name}: {exc}")
            print(f"verus pipeline stopped in {name}: {exc}", file=sys.stderr)
            _print_usage(ctx)
            return 1
    if not dry_run and stage == "all":
        state.stage = "done"
        state.save(ctx)
    print("verus pipeline complete" + (" (dry run)" if dry_run else ""), file=sys.stderr)
    _print_usage(ctx)
    return 0


def _print_usage(ctx) -> None:
    """Agent usage of the whole run, per stage (also in <artifacts>/usage.jsonl)."""
    from accel_pipeline.verus import usage

    sm = usage.summary(ctx.art)
    if sm["stages"]:
        print("agent usage (cost estimated from per-token prices):\n" + usage.format_summary(sm), file=sys.stderr)
