"""MCP tool quick_check: the agent's seconds-level inner loop."""

from __future__ import annotations

from accel_pipeline.quick import render_quick_result, run_quick_check
from accel_pipeline.tools.gate_tools import ToolContext


def make_quick_tools(ctx: ToolContext):
    def quick_check(targets: list[str] | None = None, differential: bool = True, sanitizer: str = "",
                    threads: int = 4) -> str:
        """MCP TOOL. Seconds-level check of this query's accelerator; run it after every edit.

        Incremental build (a no-op when nothing changed), the query's own tests, and the
        SQL-vs-accelerator differential on the small test subset. targets: extra build targets of
        your own test binaries (built and run). differential=False skips the differential.
        Returns pass/fail with seconds per phase; on failure the failing step and how to reproduce it.
        Correctness only: it says nothing about speed, and the subset is never a tuning target.
        sanitizer="thread" (or "address") builds an instrumented variant in a build dir of its own
        (incremental) and runs everything with every phase at `threads` threads; run it after touching
        concurrent code. Reports with a frame in the query's own directory fail; engine and substrate
        reports are counted as infrastructure.
        """
        result = run_quick_check(ctx.pipeline_config, ctx.workload or {}, ctx.query, targets=targets,
                                 differential=differential, sanitizer=sanitizer, threads=threads)
        return render_quick_result(result)

    return quick_check
