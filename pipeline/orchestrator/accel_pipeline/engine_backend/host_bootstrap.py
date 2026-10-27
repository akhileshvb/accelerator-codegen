"""One-shot workload-harness bootstrap, for every engine.

Builds backends/<engine>/tests/harness/, bench/ and tests/harness.cmake (the differential,
multi-connection, cross-accelerator, bench and calibrate harnesses), once per engine and benchmark
(the harness sources are benchmark-free, the targets are per benchmark) before pack workers fan out (and before a single-query accelerator pipeline), after the engine's
backend exists.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.engine_backend.backend_requests import as_required_fixes, mark_handled, pending, record
from accel_pipeline.config import ModelConfig, PipelineConfig, pipeline_root
from accel_pipeline.workload import (
    apply_workload_overrides,
    harness_targets,
    load_run_workload,
    load_workload,
    sanitize_engine_name,
    engine_artifacts_dir,
)
from accel_pipeline.gates.run import format_reloop_failure_sections, GateResult
from accel_pipeline.repair import StageStopped
from accel_pipeline.stages.base import run_agent_stage

_GATE_LOG_TAIL_CHARS = 8000
_DEFAULT_GATE_MAX_ROUNDS = 3
_DEFAULT_CRITIQUE_MAX_ROUNDS = 1
_ACCEPTANCE_REPAIRS = 2


def host_bootstrap_dir(repo_root: Path, engine_name: str) -> Path:
    path = engine_artifacts_dir(repo_root, sanitize_engine_name(engine_name)) / "host_bootstrap"
    path.mkdir(parents=True, exist_ok=True)
    return path


def host_bootstrap_status_path(repo_root: Path, engine_name: str) -> Path:
    return host_bootstrap_dir(repo_root, engine_name) / "host_bootstrap.json"


def needs_host_bootstrap(workload: dict[str, Any]) -> bool:
    """True for every accelerator workload: each engine's harnesses are made once."""
    return workload.get("workload_kind") != "engine_backend"


def pending_harness_requests(repo_root: Path, engine_name: str) -> str:
    """Harness defects query runs recorded that no bootstrap has fixed yet (backend_requests.py)."""
    return as_required_fixes(pending(repo_root, sanitize_engine_name(engine_name), "harness"))


def host_bootstrap_passed(repo_root: Path, engine_name: str, benchmark: str) -> bool:
    """The last gate run passed and built and smoked this benchmark's harness targets, the sources are still
    there, and no query run has recorded a harness defect since."""
    if pending_harness_requests(repo_root, engine_name):
        return False
    path = host_bootstrap_status_path(repo_root, engine_name)
    if not path.is_file():
        return False
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return False
    if str(data.get("status") or "") != "pass":
        return False
    # Markers written by unit tests / dry runs carry no gate result.
    gate = data.get("gate_data") or {}
    if gate.get("status") != "pass" or not gate.get("smokes"):
        return False
    engine = sanitize_engine_name(engine_name)
    if not set(harness_targets(engine, benchmark).values()) <= set(gate.get("targets") or []):
        return False
    return any((Path(repo_root) / "backends" / engine / "tests" / "harness").glob("*.cpp"))


def write_host_bootstrap_status(
    repo_root: Path,
    engine_name: str,
    *,
    status: str,
    reason: str = "",
    extra: dict[str, Any] | None = None,
) -> Path:
    path = host_bootstrap_status_path(repo_root, engine_name)
    payload: dict[str, Any] = {
        "gate": "host_bootstrap",
        "engine_name": sanitize_engine_name(engine_name),
        "status": status,
        "reason": reason,
    }
    if extra:
        payload.update(extra)
    path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    if status == "pass" and (payload.get("gate_data") or {}).get("status") == "pass":
        mark_handled(repo_root, sanitize_engine_name(engine_name), "harness")
    return path


def engines_needing_bootstrap_from_workloads(
    repo_root: Path,
    runs: list[tuple[str, str]],
) -> list[tuple[str, str]]:
    """Return (engine_name, representative_workload) needing bootstrap, one per engine and benchmark, from
    (workload, engine) runs."""
    seen: set[tuple[str, str]] = set()
    out: list[tuple[str, str]] = []
    for name, run_engine in runs:
        try:
            w = load_workload(name, repo_root, engine=run_engine)
        except FileNotFoundError:
            continue
        if not needs_host_bootstrap(w):
            continue
        engine = sanitize_engine_name(str(w.get("engine_name") or ""))
        key = (engine, str(w.get("benchmark_name") or ""))
        if key in seen or host_bootstrap_passed(repo_root, *key):
            continue
        seen.add(key)
        out.append((engine, name))
    return out


def format_host_bootstrap_gate_feedback(
    failed: GateResult,
    *,
    round_idx: int,
    max_rounds: int,
) -> str:
    data = failed.data or {}
    path = str(failed.path) if failed.path else ""
    lines = [
        "## Host bootstrap gate FAILED — fix from this trace; this stage will re-run",
        "",
        f"- gate: `{failed.name}`",
        f"- status: `{failed.status}`",
        f"- attempt: {round_idx}/{max_rounds}",
    ]
    if path:
        lines.append(f"- gate json: `{path}`")
    lines.extend(format_reloop_failure_sections(data, max_chars=_GATE_LOG_TAIL_CHARS))
    lines.extend(
        [
            "",
            "Fix the workload harnesses (backends/<engine>/tests/harness/, bench/) and/or "
            "tests/harness.cmake so this gate passes. Build through the engine's host.sh (`request_gate` "
            "`host_bootstrap`), not a host cmake build. Mid-loop MCP "
            "`request_gate` `host_bootstrap` until green. Do not ignore this log.",
        ]
    )
    return "\n".join(lines)


def run_host_bootstrap(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    *,
    engine_name: str,
    representative_workload: str,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    skip_gates: bool = False,
    force: bool = False,
) -> dict[str, Any]:
    """Run the host_bootstrap agent stage for one engine (idempotent unless force)."""
    engine = sanitize_engine_name(engine_name)
    if not (dry_run or skip_gates) and not (pipeline_config.repo_root / f"backends/{engine}/host.sh").is_file():
        report = host_bootstrap_dir(pipeline_config.repo_root, engine) / "stop_report.md"
        report.write_text(f"# Stopped: the {engine} backend is missing\n\nhost_bootstrap needs the backend first: run its "
                          "engine_backend workload, then `--resume`.\n", encoding="utf-8")
        raise StageStopped("the engine backend is missing", report)

    workload = load_workload(representative_workload, pipeline_config.repo_root, engine=engine)
    if not force and host_bootstrap_passed(pipeline_config.repo_root, engine, str(workload.get("benchmark_name") or "")):
        return {
            "engine_name": engine,
            "status": "pass",
            "skipped": True,
            "path": str(host_bootstrap_status_path(pipeline_config.repo_root, engine)),
        }

    query = int(workload.get("query_number") or 0)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)

    print(
        f"[host_bootstrap] engine={engine} workload={representative_workload} "
        f"(one-shot before query workstreams)",
        flush=True,
    )

    if dry_run or skip_gates:
        run_host_bootstrap_agent(
            pipeline_config,
            model_config,
            representative_workload,
            query,
            model_overrides=model_overrides,
            dry_run=dry_run,
            engine_name=engine,
        )
        path = write_host_bootstrap_status(
            pipeline_config.repo_root,
            engine,
            status="pass" if dry_run else "skip",
            reason="dry_run" if dry_run else "skip_gates",
        )
        return {"engine_name": engine, "status": "pass", "skipped": False, "path": str(path)}

    if not force and not pending_harness_requests(pipeline_config.repo_root, engine):
        from accel_pipeline.gates.run import run_gate

        existing = run_gate(pipeline_config, "host_bootstrap", query, workload=workload, raise_on_failure=False,
                            change_kind="host_bootstrap", iteration=0)
        if existing.status == "pass":
            path = write_host_bootstrap_status(pipeline_config.repo_root, engine, status="pass",
                                               reason="the existing harness passes the gate",
                                               extra={"gate_data": existing.data})
            print(f"[host_bootstrap] engine={engine}: the existing harness passes; no agent run", flush=True)
            return {"engine_name": engine, "status": "pass", "skipped": True, "path": str(path)}

    return _bootstrap_under_repair(
        pipeline_config, model_config, engine, representative_workload, workload, query, model_overrides
    )


def _bootstrap_under_repair(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    engine: str,
    representative_workload: str,
    workload: dict[str, Any],
    query: int,
    model_overrides: dict[str, str] | None,
) -> dict[str, Any]:
    """The harness agent turn, its gate (repaired in the same session on failure) and the critic
    (one batched repair round for its findings, then a re-check); a failure the loop cannot repair
    ends in a recorded stop, never a traceback."""
    from accel_pipeline.failure import Failure, render_failure
    from accel_pipeline.gates.run import run_gate
    from accel_pipeline.workload import query_artifacts_dir
    from accel_pipeline.runlog import append_experiment_log
    from accel_pipeline.repair import RepairCtx, run_with_repair
    from accel_pipeline.engine_backend.host_bootstrap_critique import (
        format_host_bootstrap_critique_feedback,
        load_host_bootstrap_critique,
        run_host_bootstrap_critique_agent,
    )

    art = host_bootstrap_dir(pipeline_config.repo_root, engine)
    ctx = RepairCtx(pipeline_config, model_config, representative_workload, query, stage="host_bootstrap",
                    change_kind="host_bootstrap", model_overrides=model_overrides)
    ctx.workload = workload
    ctx.art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    requests = pending_harness_requests(pipeline_config.repo_root, engine)
    pending = {
        "feedback": f"Query runs recorded these harness defects; fix every one in the harnesses:\n\n{requests}"
        if requests
        else ""
    }
    last: dict[str, GateResult] = {}
    rounds = {"gate": 0}

    def attempt() -> Any:
        text, pending["feedback"] = pending["feedback"], ""
        return run_host_bootstrap_agent(pipeline_config, model_config, representative_workload, query,
                         model_overrides=model_overrides, dry_run=False, engine_name=engine, extra_user_message=text)

    def run_gates(_names: list[str]) -> tuple[list[GateResult], GateResult | None]:
        result = run_gate(pipeline_config, "host_bootstrap", query, workload=workload, raise_on_failure=False,
                          change_kind="host_bootstrap", iteration=rounds["gate"])
        last["gate"] = result
        if result.status == "pass":
            return [result], None
        write_host_bootstrap_status(pipeline_config.repo_root, engine, status="fail",
                                    reason=str((result.data or {}).get("reason") or ""),
                                    extra={"gate_data": result.data, "attempt": rounds["gate"] + 1})
        return [result], result

    def repair(failure: Failure) -> None:
        rounds["gate"] += 1
        failed = last.get("gate")
        text = (format_host_bootstrap_gate_feedback(failed, round_idx=rounds["gate"], max_rounds=ctx.max_code_rounds
                                                    or pipeline_config.repair.max_code_rounds)
                if failed is not None and failure.source.startswith("gate:") else render_failure(failure))
        (art / f"host_bootstrap_gate_feedback_round{rounds['gate']}.md").write_text(text + "\n", encoding="utf-8")
        pending["feedback"] = text
        attempt()

    ctx.repair_turn, ctx.gate_runner = repair, run_gates
    ctx.max_code_rounds = int(workload.get("host_bootstrap_gate_max_rounds") or _DEFAULT_GATE_MAX_ROUNDS)
    run_with_repair(ctx, attempt, gates=["host_bootstrap"])

    max_critique = max(0, int(workload.get("host_bootstrap_critique_max_rounds") or _DEFAULT_CRITIQUE_MAX_ROUNDS))
    for critique_round in range(1, max_critique + 1):
        append_experiment_log(pipeline_config.repo_root, query,
                              f"- host_bootstrap critique round {critique_round}/{max_critique}", workload=workload)
        run_host_bootstrap_critique_agent(pipeline_config, model_config, representative_workload, query,
                                          model_overrides, False, engine_name=engine, round_idx=critique_round)
        critique = load_host_bootstrap_critique(art)
        append_experiment_log(pipeline_config.repo_root, query,
                              f"- host_bootstrap critique round {critique_round}: {critique['status'].upper()}",
                              workload=workload)
        if critique["status"] != "fail":
            break
        feedback = format_host_bootstrap_critique_feedback(critique, round_idx=critique_round)
        (art / f"host_bootstrap_critique_feedback_round{critique_round}.md").write_text(feedback + "\n", encoding="utf-8")
        pending["feedback"] = feedback
        run_with_repair(ctx, attempt, gates=["host_bootstrap"])

    final = last["gate"]
    path = write_host_bootstrap_status(
        pipeline_config.repo_root,
        engine,
        status=final.status,
        reason=str((final.data or {}).get("reason") or ""),
        extra={"gate_data": final.data},
    )
    return {"engine_name": engine, "status": final.status, "skipped": False, "path": str(path), "gate": final.data}


def accept_harness(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    workload: dict[str, Any],
    query: int,
    model_overrides: dict[str, str] | None = None,
) -> None:
    """The harnesses pass on this query, with the declining module the test_suite stage registered (every answer is
    the reference SQL's), before any accelerator depends on them: the bootstrap gate can only smoke them, no query
    being registered yet. A failure is the harness's: it is recorded as a harness request, host_bootstrap re-runs
    with it as a required fix, and the check repeats."""
    from accel_pipeline.failure import failure_from_gate, render_failure
    from accel_pipeline.gates.run import run_gate

    if workload.get("workload_kind") != "aux_state" or not workload.get("engine_name"):
        return  # no declining module, or no engine harnesses to run
    engine = sanitize_engine_name(str(workload["engine_name"]))
    for repairs in range(_ACCEPTANCE_REPAIRS + 1):
        result = run_gate(pipeline_config, "harness_acceptance", query, workload, change_kind="harness_acceptance")
        if result.status in ("pass", "skip"):
            return
        failure = failure_from_gate(result, stage="harness_acceptance", workload=workload, repo_root=pipeline_config.repo_root)
        if repairs == _ACCEPTANCE_REPAIRS:
            break
        print(f"[host_bootstrap] the {engine} harnesses fail on Q{query}: {failure.summary}; host_bootstrap repairs them",
              flush=True)
        record(pipeline_config.repo_root, engine, {
            "benchmark": str(workload.get("benchmark_name") or ""), "query": query, "scope": "harness",
            "helper": "harness acceptance", "reason": render_failure(failure)})
        run_host_bootstrap(pipeline_config, model_config, engine_name=engine, representative_workload=workload_name,
                           model_overrides=model_overrides)
    report = host_bootstrap_dir(pipeline_config.repo_root, engine) / "stop_report.md"
    report.write_text(f"# Stopped: the {engine} harnesses fail on Q{query} after {_ACCEPTANCE_REPAIRS} repairs\n\n"
                      f"{render_failure(failure)}\n", encoding="utf-8")
    raise StageStopped(f"the {engine} harnesses fail on Q{query} with the declining module", report)


def ensure_host_bootstrap_for_workload(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    *,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    skip_gates: bool = False,
) -> dict[str, Any] | None:
    """Single-query entry: bootstrap engine host if needed, else None."""
    workload = load_run_workload(pipeline_config, workload_name)
    if not needs_host_bootstrap(workload):
        return None
    engine = sanitize_engine_name(str(workload.get("engine_name") or ""))
    return run_host_bootstrap(
        pipeline_config,
        model_config,
        engine_name=engine,
        representative_workload=workload_name,
        model_overrides=model_overrides,
        dry_run=dry_run,
        skip_gates=skip_gates,
    )


def ensure_host_bootstrap_for_pack(
    pack: dict[str, Any],
    *,
    repo_root: Path,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    queries: list[Any],
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    skip_gates: bool = False,
) -> list[dict[str, Any]]:
    """Pack entry: bootstrap each distinct engine and benchmark once before its workers."""
    needed = engines_needing_bootstrap_from_workloads(
        repo_root, [(str(q.workload), str(q.engine)) for q in queries]
    )

    results: list[dict[str, Any]] = []
    for engine, rep in needed:
        results.append(
            run_host_bootstrap(
                pipeline_config,
                model_config,
                engine_name=engine,
                representative_workload=rep,
                model_overrides=model_overrides,
                dry_run=dry_run,
                skip_gates=skip_gates,
            )
        )
    # Record pack-level marker
    if results:
        marker = pipeline_root(repo_root) / "artifacts" / "packs" / "host_bootstrap_last.json"
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(json.dumps({"results": results}, indent=2) + "\n", encoding="utf-8")
    return results


def run_host_bootstrap_agent(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    engine_name: str = "",
    extra_user_message: str = "",
):
    engine = engine_name or "engine"
    user_message = (
        f"Build the workload harnesses of engine={engine}: the differential, multi-connection, "
        f"cross-accelerator, bench and calibrate targets and the per-query module test under "
        f"backends/{engine}/tests/harness/ and bench/, declared in backends/{engine}/tests/harness.cmake "
        "(the only paths you may write). One query- and benchmark-parameterised harness per kind, "
        "naming no benchmark, reaching the engine through the backend api/ and "
        "host.sh only. Build through the engine's host.sh (request_gate host_bootstrap), not a host "
        "cmake/make. Call request_gate host_bootstrap until it passes, then finish."
    )
    extra = (extra_user_message or "").strip()
    if extra:
        user_message = f"{user_message}\n\n{extra}"
    return run_agent_stage(
        stage="host_bootstrap",
        prompt_file="host_bootstrap.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=user_message,
        model_overrides=model_overrides,
        dry_run=dry_run,
        session_role="host_bootstrap",
    )
