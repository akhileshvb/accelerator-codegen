"""The prompt keys an optimization stage adds: the files it may edit and the score it is judged by."""

from __future__ import annotations

from typing import Any

from accel_pipeline.config import PipelineConfig, resolve_optimization_edit_paths

DUAL_AXIS_NOTES = (
    "\n- **Dual-axis accept/reject score** (orchestrator-owned):\n"
    "  `optimization_score = read_only_query_speedup^0.75 * write_mix_workload_speedup^0.25` "
    "(read weighs more; write-mix still gates accepts).\n"
    "  Quick bench always includes **0% reads** plus **randomly sampled write %s** "
    "(re-sampled each attempt; locked only within an attempt — see "
    "`optimization_write_frequencies.json`). "
    "Write-mix score is the **geometric mean** of non-zero write-frequency "
    "`workload_speedup`s — do not overfit a single mix.\n"
    "- **Read axis**: be aggressive on the hit path (eliminate residual scans, dense layouts, "
    "fuse/decode).\n"
    "- **Write/maintenance axis**: prefer **ApplyDelta / partition-local** apply; "
    "**RebuildAll-on-every-point-write is a maintenance cliff**. Raising write-mix "
    "`workload_speedup` across sampled frequencies raises the accept score even when read is flat.\n"
    "- When read is already strong and write-mix workload ≪ 1×, **next hypothesis must attack "
    "incremental maintenance**, not ≤5% read micro-opts.\n"
    "- Do not trade a big write-mix regression for a small read win. End-of-iteration fair "
    "sweep still covers 0/1/10/50%.\n"
    "- **Scale**: per-query and per-write cost must stay O(touched rows/partitions) or better; "
    "size from bind-time `row_estimate` — never SF1-hardcoded capacities."
)


def optimization_keys(cfg: PipelineConfig, workload: dict[str, Any], query: int, paths: dict[str, Any],
                      optimization_iter: int, benchmark_notes: str) -> dict[str, str]:
    edit = resolve_optimization_edit_paths(cfg, workload, query)
    is_aux = workload.get("workload_kind") == "aux_state"
    header, kernel, package = (edit["header"], edit["primary_cpp"], edit["secondary_cpp"]) if is_aux else (
        paths["header_path"], paths["kernel_path"], paths["package_path"])
    return {
        "header_path": header,
        "kernel_path": kernel,
        "package_path": package,
        "optimization_edit_paths": "\n".join(f"- `{p}`" for p in (edit["all_paths"] if is_aux else (header, kernel, package))),
        "optimization_max_iterations": str(cfg.optimization_max_iterations),
        "optimization_iter": str(optimization_iter),
        "benchmark_notes": benchmark_notes + DUAL_AXIS_NOTES if is_aux else benchmark_notes,
    }
