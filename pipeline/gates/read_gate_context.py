#!/usr/bin/env python3
"""Read gate context JSON written by the orchestrator before gate scripts run."""

from __future__ import annotations

import json
import os
import sys
from pathlib import Path


def _default_tpch_context(repo_root: Path, query: str) -> dict:
    q = int(query)
    engine = (os.environ.get("ACCEL_WORKLOAD_ENGINE_NAME") or os.environ.get("ACCEL_ENGINE_NAME") or "").strip()
    ctx = {
        "query_number": q,
        "benchmark_name": "tpch",
        "artifact_namespace": "tpch",
        "source_subdir": f"tpch/q{q}/base/single-threaded",
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
        "aux_test_target": f"test_tpch_q{q}_aux",
        "aux_bench_target": f"benchmark_tpch_q{q}_aux",
        "aux_ctest_smoke_regex": f"^tpch_q{q}_aux_smoke_test$",
    }
    if engine:
        ctx["engine_name"] = engine
        if engine != "duckdb":
            ctx["engine_host"] = {
                "differential_binary": f"test_{engine}_tpch_differential",
                "differential_source": f"tests/aux_state/test_{engine}_tpch_differential.cpp",
                "agent_builds_query_host": True,
            }
            ctx["bench_target"] = f"benchmark_{engine}_tpch_q{q}"
            ctx["aux_bench_target"] = f"benchmark_{engine}_tpch_aux"
    return ctx


def find_gate_context(repo_root: Path, query: str) -> dict:
    artifacts = repo_root / "pipeline" / "artifacts"
    benchmark = os.environ.get("ACCEL_AUX_BENCHMARK") or os.environ.get("ACCEL_PIPELINE_BENCHMARK") or "tpch"
    preferred = artifacts / benchmark / f"q{query}" / "gates" / "_gate_context.json"
    if preferred.is_file():
        return json.loads(preferred.read_text(encoding="utf-8"))

    # Prefer same-benchmark contexts; never accidentally pick sec_edgar for tpch.
    candidates = sorted(artifacts.glob(f"{benchmark}/q{query}/gates/_gate_context.json"))
    if candidates:
        return json.loads(candidates[-1].read_text(encoding="utf-8"))

    legacy = artifacts / f"q{query}" / "gates" / "_gate_context.json"
    if legacy.is_file():
        data = json.loads(legacy.read_text(encoding="utf-8"))
        # Reject cross-benchmark pollution (legacy flat path used by older runs).
        if str(data.get("benchmark_name") or "") in ("", benchmark):
            return data

    return _default_tpch_context(repo_root, query)


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: read_gate_context.py <repo_root> <query>", file=sys.stderr)
        return 1
    ctx = find_gate_context(Path(sys.argv[1]), sys.argv[2])
    print(json.dumps(ctx))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
