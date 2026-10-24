#!/usr/bin/env python3
"""Print the gate context the orchestrator wrote for this gate run (ACCEL_GATE_CONTEXT).

Default: the context JSON. `--shell`: the `export` lines that load_*_gate_context evals,
one process for every field a gate script needs (`--engine-only`: just the engine fields).
"""

from __future__ import annotations

import json
import os
import shlex
import sys
from pathlib import Path


def find_gate_context(repo_root: Path, query: str) -> dict:
    path = os.environ.get("ACCEL_GATE_CONTEXT")
    if not path:
        raise SystemExit("ACCEL_GATE_CONTEXT is not set: gates run through the orchestrator's run_gate")
    return json.loads(Path(path).read_text(encoding="utf-8"))


def shell_variables(ctx: dict, *, aux: bool) -> dict[str, str]:
    """The shell variables of a gate script, from its context."""
    variables = {
        "CTX": json.dumps(ctx),
        "ENGINE_NAME": ctx["engine_name"],
        "SCHEMA_SQL": ctx.get("schema_sql", ""),
        "DATASET_DIR": ctx.get("dataset_dir", ""),
    }
    if not aux:
        return variables
    host = ctx.get("engine_host") or {}
    variables.update(
        {
            "GATE_ART": ctx["artifacts_dir"],
            "BENCHMARK_NAME": ctx["benchmark_name"],
            "DIFF_BINARY": host.get("differential_binary") or "",
            "DIFF_SOURCE": host.get("differential_source") or "",
            "SMOKE_READY": "1" if host.get("smoke_ready", True) else "0",
            "HOST_HINT": host.get("query_host_ready_hint") or "",
            "MULTI_CONN_TARGET": host.get("multi_conn_target") or "",
            "CROSS_ACCEL_TARGET": host.get("cross_accel_target") or "",
            "AUX_TEST_TARGET": ctx.get("aux_test_target", ""),
            "AUX_BENCH_TARGET": ctx.get("aux_bench_target", ""),
            "AUX_CALIBRATE_TARGET": ctx.get("aux_calibrate_target", ""),
            "AUX_CTEST_SMOKE_REGEX": ctx.get("aux_ctest_smoke_regex", ""),
            "BENCH_TARGET": ctx["bench_target"],
            "COST_MODEL_CONFIG": ctx.get("cost_model_config", ""),
            "AUX_BENCH_SUITES": ctx.get("aux_bench_suites", "lineitem_only"),
            "AUX_BENCH_WORK_DIR_SUFFIX": ctx.get("aux_bench_work_dir_suffix", ""),
            "AUX_WRITE_FREQUENCIES_CTX": ctx.get("aux_write_frequencies", ""),
            "HARNESS_BENCH_TARGET": host.get("bench_target") or "",
            "HARNESS_CALIBRATE_TARGET": host.get("calibrate_target") or "",
        }
    )
    return variables


def shell_exports(ctx: dict, *, aux: bool) -> str:
    return "".join(f"export {k}={shlex.quote(str(v))}\n" for k, v in shell_variables(ctx, aux=aux).items())


def main() -> int:
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    if len(args) < 2 or flags - {"--shell", "--engine-only"}:
        print("usage: read_gate_context.py [--shell [--engine-only]] <repo_root> <query>", file=sys.stderr)
        return 1
    ctx = find_gate_context(Path(args[0]), args[1])
    if "--shell" in flags:
        print(shell_exports(ctx, aux="--engine-only" not in flags), end="")
    else:
        print(json.dumps(ctx))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
