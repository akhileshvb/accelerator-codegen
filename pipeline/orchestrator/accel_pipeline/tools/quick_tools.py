"""MCP tool quick_check: the agent's seconds-level inner loop."""

from __future__ import annotations

from accel_pipeline.quick import render_quick_result, run_quick_check
from accel_pipeline.tools.gate_tools import ToolContext, admission_refusal_payload


def make_quick_tools(ctx: ToolContext):
    def quick_check(targets: list[str] | None = None, differential: bool = True, sanitizer: str = "",
                    threads: int = 4) -> str:
        """MCP TOOL. 10-30 s check of this query's accelerator; run it after every edit.

        Incremental build (a no-op when nothing changed), the query's own tests, your `targets` (extra build
        targets of your own test binaries, built and run: put the brute-force oracle test here) and, unless
        differential=False, the frozen suite at N and 1 threads on the join-closed test subset.
        A pass reports: the state build's seconds at N and 1 thread with the speedup, projected to the full
        dataset; read latency against the engine's own SQL; each write kind's maintenance action and projected
        full-scale cost; perf counters of the read and the build. A serial or over-budget build and a write that
        rebuilds the state fail the check, each with the next step. The subset is never a tuning target.
        sanitizer="thread" (or "address") runs the differential on the tiny database in an instrumented build
        (incremental, about 30 s) with every phase at `threads` threads; run it after touching concurrent code.
        Reports with a frame in the query's own directory fail; engine and substrate reports are counted as
        infrastructure.
        """
        if refusal := admission_refusal_payload(ctx, "quick_check"):
            return refusal
        result = run_quick_check(ctx.pipeline_config, ctx.workload or {}, ctx.query, targets=targets,
                                 differential=differential, sanitizer=sanitizer, threads=threads)
        return render_quick_result(result)

    return quick_check
