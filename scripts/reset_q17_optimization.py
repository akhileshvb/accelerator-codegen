#!/usr/bin/env python3
"""Seed best-kernel snapshot and reset Q17 optimization artifacts for a clean rerun."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--query", type=int, default=17)
    parser.add_argument("--workload", default="tpch_q17_aux")
    parser.add_argument("--repo", type=Path, default=Path("/home/akhilvb/accelerator-codegen"))
    args = parser.parse_args()

    import sys

    sys.path.insert(0, str(args.repo / "pipeline" / "orchestrator"))
    from accel_pipeline.config import load_pipeline_config, load_workload
    from accel_pipeline.kernel_snapshot import snapshot_best_kernel
    from accel_pipeline.paths import query_artifacts_dir

    pipeline_config = load_pipeline_config(repo_root=args.repo)
    workload = load_workload(args.workload, args.repo)
    art = query_artifacts_dir(args.repo, args.query, workload=workload)

    opt_state_path = art / "optimization_state.json"
    read_only = 9.4
    if opt_state_path.is_file():
        state = json.loads(opt_state_path.read_text(encoding="utf-8"))
        best = state.get("best_summary") or {}
        read_only = float(best.get("read_only_query_speedup") or read_only)

    dest = snapshot_best_kernel(
        pipeline_config,
        workload,
        args.query,
        speedup_median=read_only,
        iteration=0,
        change_kind="implementation",
    )
    print(f"seeded kernel snapshot at {dest} (read_only={read_only:.2f}x)")

    for name in (
        "attempt_log.jsonl",
        "ab_trials.json",
        "active_attempt.json",
        "attempt_state.json",
    ):
        path = art / name
        if path.is_file():
            path.unlink()
            print(f"removed {path}")

    ckpt = art / "attempt_checkpoint"
    if ckpt.exists():
        shutil.rmtree(ckpt)
        print(f"removed {ckpt}")

    pipeline_state = art / "pipeline_state.json"
    if pipeline_state.is_file():
        state = json.loads(pipeline_state.read_text(encoding="utf-8"))
        state["stage"] = "optimization"
        state["optimization_iter"] = 0
        pipeline_state.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
        print(f"reset {pipeline_state}")

    print(
        "\nResume with:\n"
        f"  cd {args.repo / 'pipeline' / 'orchestrator'} && "
        f"python -m accel_pipeline.run_pipeline --query {args.query} "
        f"--workload {args.workload} --resume --models-config ../config/models.openrouter.yaml"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
