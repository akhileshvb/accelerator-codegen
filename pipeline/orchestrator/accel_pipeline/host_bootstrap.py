"""One-shot workload-harness bootstrap, for every engine.

Builds backends/<engine>/tests/harness/, bench/ and tests/harness.cmake (the differential,
multi-connection, cross-accelerator, bench and calibrate harnesses), once per engine before
pack workers fan out (and before a single-query accelerator pipeline), after the engine's
backend exists.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.config import (
    ModelConfig,
    PipelineConfig,
    apply_workload_overrides,
    load_run_workload,
    load_workload,
    sanitize_engine_name,
)
from accel_pipeline.gate_capture import format_reloop_failure_sections
from accel_pipeline.gates import GateFailure, GateResult, gate_failure_message
from accel_pipeline.paths import engine_artifacts_dir, pipeline_root

_GATE_LOG_TAIL_CHARS = 8000
_DEFAULT_GATE_MAX_ROUNDS = 3
_DEFAULT_CRITIQUE_MAX_ROUNDS = 2


def host_bootstrap_dir(repo_root: Path, engine_name: str) -> Path:
    path = engine_artifacts_dir(repo_root, sanitize_engine_name(engine_name)) / "host_bootstrap"
    path.mkdir(parents=True, exist_ok=True)
    return path


def host_bootstrap_status_path(repo_root: Path, engine_name: str) -> Path:
    return host_bootstrap_dir(repo_root, engine_name) / "host_bootstrap.json"


def needs_host_bootstrap(workload: dict[str, Any]) -> bool:
    """True for every accelerator workload: each engine's harnesses are made once."""
    return workload.get("workload_kind") != "engine_backend"


def backend_additions_path(repo_root: Path, engine_name: str) -> Path:
    return pipeline_root(repo_root) / "artifacts" / "backend_additions" / f"{sanitize_engine_name(engine_name)}.jsonl"


def pending_harness_requests(repo_root: Path, engine_name: str) -> str:
    """Harness defects query runs recorded since the last bootstrap pass (query_host_bootstrap.md,
    Ownership): a query run may not edit the harnesses, so the next bootstrap fixes them."""
    requests = backend_additions_path(repo_root, engine_name)
    status = host_bootstrap_status_path(repo_root, engine_name)
    if not requests.is_file() or (status.is_file() and status.stat().st_mtime >= requests.stat().st_mtime):
        return ""
    return requests.read_text(encoding="utf-8").strip()


def host_bootstrap_passed(repo_root: Path, engine_name: str) -> bool:
    """The last gate run passed (it built and smoked every harness), the sources are still there,
    and no query run has recorded a harness defect since."""
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
    harness = Path(repo_root) / "backends" / engine / "tests" / "harness"
    return any(harness.glob(f"test_{engine}_*_differential.cpp"))


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
    return path


def engines_needing_bootstrap_from_workloads(
    repo_root: Path,
    runs: list[tuple[str, str]],
) -> list[tuple[str, str]]:
    """Return unique (engine_name, representative_workload) needing bootstrap, from (workload, engine) runs."""
    seen: set[str] = set()
    out: list[tuple[str, str]] = []
    for name, run_engine in runs:
        try:
            w = load_workload(name, repo_root, engine=run_engine)
        except FileNotFoundError:
            continue
        if not needs_host_bootstrap(w):
            continue
        engine = sanitize_engine_name(str(w.get("engine_name") or ""))
        if engine in seen:
            continue
        if host_bootstrap_passed(repo_root, engine):
            continue
        seen.add(engine)
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
    from accel_pipeline.gates import run_gate
    from accel_pipeline.stages.host_bootstrap import run as run_stage

    engine = sanitize_engine_name(engine_name)
    if not force and host_bootstrap_passed(pipeline_config.repo_root, engine):
        return {
            "engine_name": engine,
            "status": "pass",
            "skipped": True,
            "path": str(host_bootstrap_status_path(pipeline_config.repo_root, engine)),
        }

    if not (dry_run or skip_gates) and not (pipeline_config.repo_root / f"backends/{engine}/host.sh").is_file():
        raise GateFailure(f"host_bootstrap needs the {engine} backend first: run its engine_backend workload")

    workload = load_workload(representative_workload, pipeline_config.repo_root, engine=engine)

    query = int(workload.get("query_number") or 0)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)

    print(
        f"[host_bootstrap] engine={engine} workload={representative_workload} "
        f"(one-shot before query workstreams)",
        flush=True,
    )

    if dry_run or skip_gates:
        run_stage(
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

    from accel_pipeline.playbook import append_experiment_log
    from accel_pipeline.stages.host_bootstrap_critique import (
        format_host_bootstrap_critique_feedback,
        load_host_bootstrap_critique,
        run_host_bootstrap_critique_agent,
    )

    configured_gate = int(
        workload.get("host_bootstrap_gate_max_rounds") or _DEFAULT_GATE_MAX_ROUNDS
    )
    max_gate = max(1, configured_gate)
    configured_critique = int(
        workload.get("host_bootstrap_critique_max_rounds") or _DEFAULT_CRITIQUE_MAX_ROUNDS
    )
    max_critique = max(0, configured_critique)
    requests = pending_harness_requests(pipeline_config.repo_root, engine)
    extra = (
        f"Query runs recorded these harness defects in "
        f"`pipeline/artifacts/backend_additions/{engine}.jsonl`; fix every one in the harnesses:\n\n{requests}"
        if requests
        else ""
    )
    gate_fails = 0
    critique_round = 0
    last_gate: GateResult | None = None
    art = host_bootstrap_dir(pipeline_config.repo_root, engine)

    while True:
        run_stage(
            pipeline_config,
            model_config,
            representative_workload,
            query,
            model_overrides=model_overrides,
            dry_run=dry_run,
            engine_name=engine,
            extra_user_message=extra,
        )
        result = run_gate(
            pipeline_config,
            "host_bootstrap",
            query,
            workload=workload,
            raise_on_failure=False,
            change_kind="host_bootstrap",
            iteration=gate_fails,
        )
        last_gate = result
        if result.status != "pass":
            gate_fails += 1
            feedback = format_host_bootstrap_gate_feedback(
                result, round_idx=gate_fails, max_rounds=max_gate
            )
            feedback_path = art / f"host_bootstrap_gate_feedback_round{gate_fails}.md"
            feedback_path.write_text(feedback + "\n", encoding="utf-8")
            append_experiment_log(
                pipeline_config.repo_root,
                query,
                f"- host_bootstrap gate FAIL attempt {gate_fails}/{max_gate} — "
                f"re-invoking (see {feedback_path.name})",
                workload=workload,
            )
            extra = feedback
            write_host_bootstrap_status(
                pipeline_config.repo_root,
                engine,
                status="fail",
                reason=str((result.data or {}).get("reason") or ""),
                extra={"gate_data": result.data, "attempt": gate_fails},
            )
            if gate_fails >= max_gate:
                raise GateFailure(
                    "host_bootstrap still failing after "
                    f"{max_gate} attempt(s): {gate_failure_message(result)}"
                )
            continue

        if max_critique <= 0:
            break

        critique_round += 1
        critique_json = art / "host_bootstrap_critique.json"
        if critique_json.is_file():
            critique_json.unlink()
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- host_bootstrap critique round {critique_round}/{max_critique}",
            workload=workload,
        )
        run_host_bootstrap_critique_agent(
            pipeline_config,
            model_config,
            representative_workload,
            query,
            model_overrides,
            dry_run,
            engine_name=engine,
            round_idx=critique_round,
        )
        critique = load_host_bootstrap_critique(art)
        if str(critique.get("status") or "") == "pass":
            append_experiment_log(
                pipeline_config.repo_root,
                query,
                f"- host_bootstrap critique round {critique_round}: PASS",
                workload=workload,
            )
            break
        feedback = format_host_bootstrap_critique_feedback(
            critique, round_idx=critique_round
        )
        feedback_path = art / f"host_bootstrap_critique_feedback_round{critique_round}.md"
        feedback_path.write_text(feedback + "\n", encoding="utf-8")
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- host_bootstrap critique round {critique_round}: FAIL — "
            f"re-invoking (see {feedback_path.name})",
            workload=workload,
        )
        extra = feedback
        if critique_round >= max_critique:
            raise GateFailure(
                "host_bootstrap critique still failing after "
                f"{max_critique} round(s)"
            )

    assert last_gate is not None
    path = write_host_bootstrap_status(
        pipeline_config.repo_root,
        engine,
        status=last_gate.status,
        reason=str((last_gate.data or {}).get("reason") or ""),
        extra={"gate_data": last_gate.data},
    )
    return {
        "engine_name": engine,
        "status": last_gate.status,
        "skipped": False,
        "path": str(path),
        "gate": last_gate.data,
    }


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
    """Pack entry: bootstrap each distinct engine once before its workers."""
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
