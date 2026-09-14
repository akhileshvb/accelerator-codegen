from __future__ import annotations

from accel_pipeline.gates import GateFailure, GateResult, gate_failure_message, run_gate


GATES_ALLOW_SKIP = frozenset(
    {
        "baseline",
        "sf10",
        "profile",
        "aux_cost_model_calibrate",
        "aux_speedup_verification",
        "engine_scan_conformance",
        "engine_aux_smoke",
        "engine_host",
    }
)


def workload_post_implementation_gates(workload: dict) -> list[str]:
    if workload.get("workload_kind") == "aux_state":
        return list(workload.get("post_implementation_gates", ["aux_correctness", "aux_unit"]))
    if "post_implementation_gates" in workload:
        return list(workload["post_implementation_gates"])
    return ["correctness", "baseline", "profile"]


def workload_post_optimization_gates(workload: dict) -> list[str]:
    if workload.get("workload_kind") == "aux_state":
        return list(workload.get("post_optimization_gates", ["aux_correctness", "aux_unit"]))
    if "post_optimization_gates" in workload:
        return list(workload["post_optimization_gates"])
    return ["correctness", "baseline", "sf10", "profile"]


def workload_post_audit_gates(workload: dict) -> list[str]:
    if workload.get("workload_kind") == "aux_state":
        return list(workload.get("post_audit_gates", ["aux_correctness", "aux_unit", "aux_bench_sweep"]))
    if "post_audit_gates" in workload:
        return list(workload["post_audit_gates"])
    return ["fairness", "sf10"]


def run_post_stage_gates_collect(
    pipeline_config,
    workload_name: str,
    query: int,
    gate_names: list[str],
    *,
    change_kind: str = "",
    iteration: int = 0,
) -> tuple[list[GateResult], GateResult | None]:
    from accel_pipeline.config import load_workload

    workload = load_workload(workload_name, pipeline_config.repo_root)
    from accel_pipeline.pipeline_log import log_gate_batch_start

    log_gate_batch_start(
        query,
        gate_names,
        change_kind=change_kind,
        iteration=iteration,
    )
    results: list[GateResult] = []
    total = len(gate_names)
    for idx, gate_name in enumerate(gate_names, start=1):
        allow_skip = gate_name in GATES_ALLOW_SKIP
        result = run_gate(
            pipeline_config,
            gate_name,
            query,
            workload=workload,
            raise_on_failure=False,
            allow_skip=allow_skip,
            change_kind=change_kind,
            iteration=iteration,
            gate_index=idx,
            gate_count=total,
        )
        results.append(result)
        if result.status == "pass":
            continue
        if result.status == "skip" and allow_skip:
            continue
        return results, result
    return results, None


def run_post_stage_gates(
    pipeline_config,
    workload_name: str,
    query: int,
    gate_names: list[str],
    *,
    change_kind: str = "",
    iteration: int = 0,
) -> list[GateResult]:
    results, failed = run_post_stage_gates_collect(
        pipeline_config,
        workload_name,
        query,
        gate_names,
        change_kind=change_kind,
        iteration=iteration,
    )
    if failed is not None:
        raise GateFailure(gate_failure_message(failed))
    return results
