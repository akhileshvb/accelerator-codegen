#!/usr/bin/env python3
"""Read gate context JSON written by the orchestrator before gate scripts run."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def find_gate_context(repo_root: Path, query: str) -> dict:
    artifacts = repo_root / "pipeline" / "artifacts"
    benchmark = os.environ.get("ACCEL_AUX_BENCHMARK", "tpch")
    preferred = artifacts / benchmark / f"q{query}" / "gates" / "_gate_context.json"
    if preferred.is_file():
        return json.loads(preferred.read_text(encoding="utf-8"))

    candidates = sorted(artifacts.glob(f"*/q{query}/gates/_gate_context.json"))
    if candidates:
        return json.loads(candidates[-1].read_text(encoding="utf-8"))

    legacy = artifacts / f"q{query}" / "gates" / "_gate_context.json"
    if legacy.is_file():
        return json.loads(legacy.read_text(encoding="utf-8"))

    q = int(query)
    benchmark = "tpch"
    return {
        "query_number": q,
        "benchmark_name": benchmark,
        "artifact_namespace": benchmark,
        "source_subdir": f"{benchmark}/q{q}/base/single-threaded",
        "function_name": f"accel_tpch_q{q}",
        "create_fn": f"CreateTpchQ{q}FunctionSet",
        "test_target": f"test_tpch_q{q}",
        "bench_target": f"benchmark_tpch_q{q}",
        "ctest_smoke_regex": f"^tpch_q{q}_smoke_test$",
        "dataset_db": str(repo_root.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb"),
        "scale_db": str(repo_root.parent / "accelerator-codegen-duckdb" / "tpch_sf10.duckdb"),
        "db_env_var": "ACCEL_TPCH_DB",
        "fairness": {
            "cardinality_constants": ["150000", "600000", "800000"],
            "kernel_search_paths": [
                f"src/generated/tpch/q{q}/base/single-threaded/kernel.cpp",
            ],
        },
    }


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: read_gate_context.py <repo_root> <query>", file=sys.stderr)
        return 1
    ctx = find_gate_context(Path(sys.argv[1]), sys.argv[2])
    print(json.dumps(ctx))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
