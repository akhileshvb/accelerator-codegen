"""The accelerator stages: analysis, test suite, implementation, audit, structural re-analysis and scaffolding.

What every agent stage shares comes first: running one (turns, session, failure triage) and the gates that run
after it (`run_agent_stage`, `run_post_stage_gates_collect`). Then each stage by name (`STAGE_RUNNERS`, run through
`run_stage`): analysis (the design, validated mechanically and critiqued), the test suite (written in its own
session before the implementation, validated by the test_suite_validate gate under the repair loop and frozen
afterwards; the only way to change it is a change request that re-runs the stage, `revise_test_suite`), the
implementation, optimization (the optimization package's stage, imported when it runs) and the audit.

Scaffolding materializes the stubs of a query's registration and workload files from the engine's API manifest
before the agent starts.

Structural re-analysis is a bounded re-run of analysis when implementation or optimization finds a structural
bottleneck. The agent files a request (the `request_structural_reanalysis` tool in mcp.py); after the stage the
orchestrator validates it, spends one of the workload's reanalysis budget, hands the analysis stage the evidence
and re-runs the stages downstream of it.
"""
from __future__ import annotations

import json
import re
import shutil
import sys
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Callable

from accel_pipeline.agent import (
    create_agent_backend,
    MAIN,
    READONLY,
    REDIRECT,
    run_in_session,
    StageRunRequest,
    TESTER,
)
from accel_pipeline.config import (
    load_pipeline_config,
    resolve_stage_provider,
    analysis_artifact_path,
    apply_workload_overrides,
    benchmark_of,
    load_run_workload,
    load_workload,
    make_model_id,
    ModelConfig,
    PipelineConfig,
    query_artifacts_dir,
    read_json,
    resolve_workload_paths,
    with_facts,
)
from accel_pipeline.data import (
    archive_change_request,
    AuxAnalysisValidationError,
    content_hash,
    ensure_dataset_loaded,
    freeze,
    pending_change_request,
    repair_aux_analysis,
    suite_path,
    validate_aux_analysis_file,
)
from accel_pipeline.gates import GateResult, reusable_gate_result, run_gate
from accel_pipeline.prims import refresh_for_stage
from accel_pipeline.layout import frozen_restore, frozen_snapshot, query_dir
from accel_pipeline.prompts import (
    audit_context,
    AUDIT_FINDINGS_FILE,
    blocking_findings,
    changed_files,
    cumulative_diff,
    IMPLEMENTATION_BASE_FILE,
    numbered,
    record_implementation_base,
    render_prompt,
    save_rendered_prompt,
    snapshot,
    touched_since,
    with_codex_tooling,
)
from accel_pipeline.repair import (
    revise_pending_suite,
    AGENT_BLOCKER,
    Failure,
    is_out_of_credits,
    is_transient_provider_failure,
    PROVIDER_CREDITS,
    PROVIDER_FLAKE,
    render_failure,
    repair_turn,
    RepairCtx,
    run_with_repair,
    StageFailure,
    stop_stage,
)
from accel_pipeline.runlog import (
    append_experiment_log,
    load_or_create,
    log_analysis_candidates,
    log_event,
    log_gate_batch_start,
    log_stage_complete,
    log_stage_start,
    log_structural_reanalysis_request,
    log_structural_reanalysis_skipped,
    log_structural_reanalysis_triggered,
    STAGES,
    usage_from_metadata,
)


_UNLIMITED_STAGE_TURNS = 10_000


def resolve_stage_max_turns(stage_max_turns: dict[str, int], stage: str, default: int = 15) -> int:
    if stage not in stage_max_turns and stage.startswith("impl_"):
        raw = int(stage_max_turns.get("implementation", default))
    elif stage not in stage_max_turns and (
        stage == "layer_critique" or stage.startswith("critique_")
    ):
        raw = int(stage_max_turns.get("layer_critique", default))
    else:
        raw = int(stage_max_turns.get(stage, default))
    if raw <= 0:
        return _UNLIMITED_STAGE_TURNS
    return raw


def resolve_optimization_max_turns(pipeline_config: Any, phase: str) -> int:
    """Resolve optimization sub-stage turns; 0 or unset means unlimited."""
    key = f"optimization_max_turns_{phase}"
    raw = int(getattr(pipeline_config, key, 0) or 0)
    if raw <= 0:
        return _UNLIMITED_STAGE_TURNS
    return raw


def run_agent_stage(
    *,
    stage: str,
    prompt_file: str,
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    user_message: str,
    model_overrides: dict[str, str] | None = None,
    optimization_iter: int = 0,
    dry_run: bool = False,
    analysis_revision_request: str = "",
    extra_context: dict[str, str] | None = None,
    must_change: Path | None = None,
    session_role: str | None = None,
    resume: bool = True,
    handoff: Callable[[], str] | None = None,
) -> Any:
    """Run one agent stage. With `must_change`, a run that leaves every file under that
    directory as it was is re-invoked (twice at most): agents run unattended, so a turn that
    ends with a question or a plan instead of edits did nothing.

    With a `session_role`, the stage runs in that role's codex session (`resume`: continue the
    stored one). A run that fails raises StageFailure for the repair loop."""
    workload = load_run_workload(pipeline_config, workload_name)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    if not dry_run:
        # pick up primitives other workers published (no-op outside worker worktrees)

        refresh_for_stage(pipeline_config.repo_root)
    merged_extra = dict(extra_context or {})
    if analysis_revision_request:
        merged_extra["analysis_revision_request"] = analysis_revision_request
    prompt = render_prompt(
        pipeline_config,
        prompt_file,
        workload,
        query,
        stage,
        optimization_iter=optimization_iter,
        extra_context=merged_extra or None,
    )
    save_rendered_prompt(
        pipeline_config,
        workload,
        query,
        stage,
        prompt,
        optimization_iter=optimization_iter,
    )

    if dry_run:
        log_stage_complete(
            pipeline_config.repo_root,
            query,
            workload,
            stage=stage,
            model_id="(dry-run)",
            backend="dry-run",
            duration_ms=None,
            usage=None,
            optimization_iter=optimization_iter,
            dry_run=True,
            prompt_chars=len(prompt),
        )
        return {"dry_run": True, "stage": stage, "prompt_chars": len(prompt)}

    model_id = make_model_id(stage, pipeline_config, model_config, model_overrides)
    agent_model = model_id
    log_stage_start(
        pipeline_config.repo_root,
        query,
        workload,
        stage=stage,
        model_id=model_id,
        optimization_iter=optimization_iter,
        revision=bool(analysis_revision_request),
    )

    stage_provider = resolve_stage_provider(stage, model_config)
    backend = create_agent_backend(
        provider=stage_provider,
        pipeline_config=pipeline_config,
        query=query,
        stage=stage,
        model_config=model_config,
        workload_name=workload_name,
        optimization_iter=optimization_iter,
    )
    max_turns = resolve_stage_max_turns(pipeline_config.stage_max_turns, stage)
    instructions = with_codex_tooling(pipeline_config, prompt,
                                      engine_backend=workload.get("workload_kind") == "engine_backend")

    print(f"[pipeline] agent run start: stage={stage} max_turns={max_turns} prompt_chars={len(instructions)}",
          file=sys.stderr, flush=True)

    request = StageRunRequest(
        stage=stage,
        instructions=instructions,
        user_message=user_message,
        agent_name=f"accel_{stage}",
        model_id=model_id,
        agent_model=agent_model,
        max_turns=max_turns,
    )
    frozen = frozen_snapshot(pipeline_config.repo_root)
    before = _tree_state(must_change) if must_change else None
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)

    def run(req: StageRunRequest) -> Any:
        if session_role and resume:
            return run_in_session(backend, req, pipeline_config=pipeline_config, art=art, query=query, role=session_role,
                                  handoff=handoff)
        return backend.run_stage(req)

    result = run(request)
    for _ in range(2):
        if before is None or result.status != "finished" or _tree_state(must_change) != before:
            break
        print(f"[pipeline] {stage}: no file under {must_change} changed; re-invoking", flush=True)
        request = replace(request, user_message=request.user_message + "\n\n" + _no_change_nudge(must_change))
        result = run(request)
    reverted = frozen_restore(pipeline_config.repo_root, frozen)
    if reverted:
        print(f"[pipeline] {stage}: reverted agent edits to frozen paths: {', '.join(reverted)}", flush=True)
        append_experiment_log(
            pipeline_config.repo_root,
            query,
            f"- {stage}: reverted agent edits to frozen pipeline/substrate paths ({', '.join(reverted)})",
            workload=workload,
        )
    failure = None if result.status == "finished" else stage_failure(stage, backend.name, result)
    if failure is not None and failure.kind != "timeout":
        raise StageFailure(failure)
    log_stage_complete(
        pipeline_config.repo_root,
        query,
        workload,
        stage=stage,
        model_id=model_id,
        backend=result.backend,
        duration_ms=result.metadata.get("duration_ms"),
        usage=usage_from_metadata(result.metadata),
        optimization_iter=optimization_iter,
        run_id=result.run_id,
    )
    if failure is not None:
        append_experiment_log(pipeline_config.repo_root, query, f"- {stage}: {failure.summary}", workload=workload)
        raise StageFailure(failure)
    return result


def stage_failure(stage: str, backend_name: str, result: Any) -> Failure:
    """What a codex run that did not finish failed on: the deadline, a write-policy violation the
    agent can fix, a provider flake, or something only the orchestrator can answer for."""
    detail = (result.error or result.status or "").strip()
    stopped, evidence = result.metadata.get("stop_reason"), str(result.metadata.get("stop_detail") or "")
    if stopped == "blocker":
        return Failure(kind="infra", stage=stage, source="agent", infra_reason=AGENT_BLOCKER, evidence=evidence,
                       summary="the agent reported a blocker outside its files (blocker.md)")
    if stopped == "no_progress":
        return Failure(kind="timeout", stage=stage, source="no_progress", summary=detail, evidence=evidence,
                       diagnosis=[REDIRECT])
    if result.metadata.get("timed_out"):
        return Failure(
            kind="timeout", stage=stage, source="agent", summary=detail,
            diagnosis=["You were stopped at the deadline. State your status, checkpoint the tree "
                       "(it must build), and continue from where you were."],
        )
    final = (result.final_text or "").strip()
    if final and final not in detail:
        clipped = final if len(final) <= 3000 else final[:3000] + "\n... [truncated]"
        detail = f"{detail}\n\n--- agent output ---\n{clipped}"
    summary = f"{backend_name} stage {stage} failed"
    if result.metadata.get("write_violations"):
        return Failure(kind="code", stage=stage, source="write_policy", summary=summary, evidence=detail)
    if is_out_of_credits(detail):
        return Failure(kind="infra", stage=stage, source="agent", evidence=detail, infra_reason=PROVIDER_CREDITS,
                       summary="the model provider's account is out of credits (402): add credits, then --resume")
    if is_transient_provider_failure(detail):
        return Failure(kind="infra", stage=stage, source="agent", summary=summary, evidence=detail,
                       infra_reason=PROVIDER_FLAKE)
    return Failure(kind="orchestrator", stage=stage, source="agent", summary=summary, evidence=detail)


GATES_ALLOW_SKIP = frozenset(
    {
        "baseline",
        "sf10",
        "profile",
        "module_tsan",  # skips when the engine-free TSAN driver has no case for the query (aux_tsan covers it)
        "aux_cost_model_calibrate",
        "aux_speedup_verification",
        "engine_scan_conformance",
        "engine_aux_smoke",
        "engine_host",
        "engine_hit_contract",
        "engine_canary",
        "engine_canary_asan",
        "engine_canary_tsan",
        "slot_restart",
        "slot_own_writes",
        "slot_ddl_events",
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
    reuse: bool = False,
) -> tuple[list[GateResult], GateResult | None]:
    """Run `gate_names` in order until one fails. reuse: skip a gate that already passed on identical code."""
    workload = load_run_workload(pipeline_config, workload_name)
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
        reused = reuse and reusable_gate_result(pipeline_config, workload, query, gate_name, change_kind=change_kind)
        result = reused or run_gate(
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


def _tree_state(root: Path) -> dict[str, tuple[int, int]]:
    if not root.is_dir():
        return {}
    return {
        str(f): (f.stat().st_mtime_ns, f.stat().st_size)
        for f in root.rglob("*") if f.is_file()
    }


def _no_change_nudge(root: Path) -> str:
    return (
        f"Your last run ended without changing any file under `{root}`. Nobody reads a question "
        "or approves a next step: this run is the work. Write the code now (create the files "
        "that are missing), run the gate, and finish only when it passes."
    )


def _ensure_dataset(pipeline_config: PipelineConfig, workload_name: str) -> None:
    """The data tools query the target engine: bring its instance up with the dataset loaded before the agent starts."""

    workload = load_run_workload(pipeline_config, workload_name)
    if workload.get("workload_kind") != "aux_state":
        return
    problem = ensure_dataset_loaded(workload, pipeline_config.repo_root, pipeline_config.build_dir)
    if problem:
        print(f"[pipeline] data tools unavailable: {problem}", file=sys.stderr, flush=True)


REVISION_MESSAGE = """An independent critic reviewed your analysis and asks for a revision:

{critique}

Address every failed check and, if its alternative measured better, simulate it yourself and adopt it or show
with a measured count why yours is better. Edit only what the critique touches with update_analysis (JSON Patch
operations on analysis.json; it validates after applying) rather than regenerating the file, then stop."""


VALIDATION_REPAIR_ROUNDS = 2
VALIDATION_MESSAGE = """analysis.json does not pass the stage's validation:

{error}

Fix what it names with update_analysis (JSON Patch operations on analysis.json; it validates after applying)
until it reports "valid", and stop. The rule is in the analysis prompt's Task section."""


def _validate(analysis_path: Path, allow_poor: bool) -> None:
    try:
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise RuntimeError(f"aux analysis validation failed: {analysis_path} is missing or not JSON: {exc}") from exc
    try:
        repaired, repairs = repair_aux_analysis(analysis)
        if repairs:
            analysis_path.write_text(json.dumps(repaired, indent=2), encoding="utf-8")
            print(
                f"[pipeline] repaired analysis.json before validation: {', '.join(repairs)}",
                file=sys.stderr,
                flush=True,
            )
        validate_aux_analysis_file(
            analysis_path,
            allow_poor_fitness=allow_poor,
        )
    except AuxAnalysisValidationError as exc:
        raise RuntimeError(f"aux analysis validation failed: {exc}") from exc


def _revise(pipeline_config, model_config, workload_name, query, model_overrides, message: str) -> None:
    run_agent_stage(
        stage="analysis",
        prompt_file="01_analysis.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=message,
        model_overrides=model_overrides,
        session_role=MAIN,
    )


def validate_with_repair(pipeline_config, model_config, workload_name, query, model_overrides,
                         analysis_path: Path, allow_poor: bool) -> None:
    """Validation failures go back to the analysis session, which can fix what it wrote."""
    for attempt in range(VALIDATION_REPAIR_ROUNDS + 1):
        try:
            return _validate(analysis_path, allow_poor)
        except RuntimeError as exc:
            if attempt == VALIDATION_REPAIR_ROUNDS:
                raise
            _revise(pipeline_config, model_config, workload_name, query, model_overrides,
                    VALIDATION_MESSAGE.format(error=exc))


def _asks_revision(critique: dict[str, Any]) -> bool:
    failed = [c for c in critique.get("checks") or [] if isinstance(c, dict) and c.get("ok") is False]
    better = bool((critique.get("alternative") or {}).get("better"))
    return critique.get("verdict") == "revise" or bool(failed) or better


def _critique(pipeline_config, model_config, workload_name, query, analysis_path: Path,
              model_overrides, round_: int) -> dict[str, Any] | None:
    """A fresh critic session judges the analysis; None when it produced no usable report (never stops the run)."""
    path = analysis_path.parent / "analysis_critique.json"
    path.unlink(missing_ok=True)
    try:
        run_agent_stage(
            stage="analysis_critic",
            prompt_file="01c_analysis_critic.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=f"Critique the analysis; write {path} and stop.",
            model_overrides=model_overrides,
            extra_context={"critique_path": str(path)},
        )
        critique = json.loads(path.read_text(encoding="utf-8"))
    except (StageFailure, OSError, json.JSONDecodeError) as exc:
        print(f"[pipeline] analysis critic produced no report: {exc}", file=sys.stderr, flush=True)
        return None
    if not isinstance(critique, dict):
        return None
    shutil.copyfile(path, path.with_name(f"analysis_critique_r{round_ + 1}.json"))
    log_event(pipeline_config.repo_root, query, load_run_workload(pipeline_config, workload_name), "analysis_critique",
              round=round_ + 1, verdict=critique.get("verdict"), revise=_asks_revision(critique))
    return critique


def run_analysis(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    user_message: str | None = None,
    analysis_revision_request: str = "",
):
    if not dry_run:
        _ensure_dataset(pipeline_config, workload_name)
    result = run_agent_stage(
        stage="analysis",
        prompt_file="01_analysis.md",
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=user_message
        or "Analyze the query and write the analysis artifact to the artifacts directory.",
        model_overrides=model_overrides,
        dry_run=dry_run,
        analysis_revision_request=analysis_revision_request,
        session_role=MAIN,  # implementation continues this conversation: no re-reading the analysis
    )
    if dry_run:
        return result

    workload = load_run_workload(pipeline_config, workload_name)
    if workload.get("workload_kind") != "aux_state":
        return result

    analysis_path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
    allow_poor = bool(workload.get("allow_poor_fitness", False))
    def revise(message: str) -> None:
        _revise(pipeline_config, model_config, workload_name, query, model_overrides, message)

    def validated() -> None:
        validate_with_repair(pipeline_config, model_config, workload_name, query, model_overrides,
                             analysis_path, allow_poor)

    validated()
    for round_ in range(max(0, int(workload.get("analysis_critique_max_rounds", 1)))):
        critique = _critique(pipeline_config, model_config, workload_name, query, analysis_path, model_overrides, round_)
        if critique is None or not _asks_revision(critique):
            break
        revise(REVISION_MESSAGE.format(critique=json.dumps(critique, indent=2)))
        validated()

    analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    ratio = analysis["correctness_facts"]["state_design"].get("read_work_ratio")
    if isinstance(ratio, dict) and ratio.get("unmet_justification"):
        log_event(
            pipeline_config.repo_root, query, workload, "design_target_unmet",
            typical=ratio.get("typical"), worst=ratio.get("worst"),
            justification=str(ratio["unmet_justification"])[:500],
        )
    log_analysis_candidates(
        pipeline_config.repo_root,
        query,
        analysis,
        revision=bool(analysis_revision_request),
        workload=workload,
    )
    return result


STAGE = "test_suite"
PROMPT_FILE = "01b_test_suite.md"


def run_test_suite(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    change_request: str = "",
) -> Any:
    workload = load_run_workload(pipeline_config, workload_name)
    root, bench = pipeline_config.repo_root, benchmark_of(workload)
    directory = suite_path(root, bench, query)
    art = query_artifacts_dir(root, query, workload=workload)
    analysis = analysis_artifact_path(root, query, workload=workload)
    if not dry_run and not analysis.is_file():
        raise RuntimeError(f"{STAGE} requires the analysis artifact: {analysis}")

    def author(message: str) -> Any:
        return run_agent_stage(
            stage=STAGE,
            prompt_file=PROMPT_FILE,
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=message,
            model_overrides=model_overrides,
            dry_run=dry_run,
            must_change=directory,
            session_role=TESTER,
            extra_context={"change_request": change_request},
        )

    task = f"Write the test suite of Q{query} into {directory.relative_to(root)}."
    if change_request:
        task = (f"The implementation raised a change request against the frozen suite in {directory.relative_to(root)}. "
                "Decide whether the test is wrong (fix it, and say what changed) or right (leave it, and say why); "
                f"the suite must stay valid.\n\n{change_request}")
    if dry_run:
        return author(task)

    # The pipeline registers the query (declining module, the reference SQL as its fallback) before any test: the
    # tester owns tests/workload only, and the validation runs through the engine's normal entry point.
    created = materialize_registration(pipeline_config, workload, query)["created"]
    if created:
        print(f"[pipeline] {STAGE}: scaffolded the registration {created}", file=sys.stderr, flush=True)

    ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage=STAGE, change_kind=STAGE,
                    model_overrides=model_overrides)
    ctx.repair_turn = lambda failure: author(render_failure(failure))
    result = run_with_repair(ctx, lambda: author(task), gates=["test_suite_validate"])
    record = freeze(art, directory, query=query, reason="change request" if change_request else "test_suite stage")
    append_experiment_log(root, query, f"- {STAGE}: suite frozen, hash {record['hash'][:12]} ({record['reason']})",
                          workload=workload)
    print(f"[pipeline] {STAGE} froze {directory} ({record['hash'][:12]})", file=sys.stderr, flush=True)
    if not change_request:  # the query's module is still the declining one: the harnesses must pass on it
        from accel_pipeline.engine_backend.host_bootstrap import accept_harness

        accept_harness(pipeline_config, model_config, workload_name, workload, query, model_overrides)
    return result


def revise_test_suite(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
) -> None:
    """Handle the pending change request: the tester re-runs on it, the suite is re-validated and frozen again,
    and the request is archived with the outcome."""
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    directory = suite_path(pipeline_config.repo_root, benchmark_of(workload), query)
    before = content_hash(directory)
    run_test_suite(pipeline_config, model_config, workload_name, query, model_overrides,
                   change_request=pending_change_request(art))
    archive_change_request(
        art, "the tester kept the suite as it was" if content_hash(directory) == before else "the tester changed the suite")


def _implementation_user_message(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
) -> str:
    if workload.get("workload_kind") != "aux_state":
        return "Implement the accelerator, tests, and benchmark. Build and run correctness tests."

    paths = resolve_workload_paths(pipeline_config, workload, query)
    aux_module = pipeline_config.repo_root / paths["aux_state_subdir"] / "module.cpp"
    if not aux_module.is_file():
        return "Implement the accelerator, tests, and benchmark. Build and run correctness tests."

    return (
        "RESUME partial aux implementation: the query's sources already exist on disk from a prior run. Continue "
        "from them; do not rewrite the module from scratch.\n"
        "Validate with `quick_check` after each edit, then the gates (`request_gate`, e.g. `aux_exit`). The workload "
        "harnesses (differential, multi-connection, cross-accelerator, bench, calibrate, module test) and their "
        "targets belong to host_bootstrap: build them through the gate tools, never define or redirect them. If one "
        "is missing or broken, call `report_blocker` with the evidence.\n"
        "Fix failures with minimal edits to the query's own files. Do not weaken tests or fixtures to hide bugs."
    )


def run_implementation(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    skip_gates: bool = False,
):
    workload = load_run_workload(pipeline_config, workload_name)
    if workload.get("workload_kind") != "aux_state":
        run_gate(pipeline_config, "scaffold", query, workload=workload)
    else:
        analysis_path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
        if analysis_path.is_file() and not dry_run:
            # An analysis written under older rules goes back to its session rather than stopping the run.
            validate_with_repair(pipeline_config, model_config, workload_name, query, model_overrides,
                                 analysis_path, bool(workload.get("allow_poor_fitness", False)))

    def implement():
        return run_agent_stage(
            stage="implementation",
            prompt_file="02_implementation.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=_implementation_user_message(pipeline_config, workload, query),
            model_overrides=model_overrides,
            dry_run=dry_run,
            must_change=pipeline_config.repo_root / query_dir(benchmark_of(workload), query),
            session_role=MAIN,
        )

    if dry_run:
        return implement()
    record_implementation_base(
        pipeline_config.repo_root, query_artifacts_dir(pipeline_config.repo_root, query, workload=workload))
    repair_ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="implementation",
                           change_kind="implementation", model_overrides=model_overrides,
                           quick_first=workload.get("workload_kind") == "aux_state")
    if skip_gates:
        return run_with_repair(repair_ctx, implement)
    workload = load_run_workload(pipeline_config, workload_name)
    analysis_path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
    impl_gates = workload_post_implementation_gates(workload)

    def implement_and_seed():
        result = implement()
        if analysis_path.is_file() and workload.get("workload_kind") == "aux_state":
            from accel_pipeline.optimization.aux_bench import initialize_optimization_state

            analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
            initialize_optimization_state(pipeline_config.repo_root, query, analysis, workload)
        return result

    result = run_with_repair(repair_ctx, implement_and_seed, gates=impl_gates)
    run_with_repair(repair_ctx, lambda: _record_baseline(pipeline_config, workload, query, analysis_path, impl_gates))
    return result


def _record_baseline(pipeline_config, workload: dict, query: int, analysis_path, impl_gates: list[str]) -> None:
    """The implementation's bench result is the optimization stage's baseline."""
    if workload.get("workload_kind") != "aux_state":
        from accel_pipeline.optimization.base_bench import process_base_bench_after_gates

        process_base_bench_after_gates(
            pipeline_config.repo_root,
            query,
            workload,
            iteration=0,
            change_kind="implementation",
        )
    elif {"aux_bench_sweep", "aux_exit"} & set(impl_gates):
        from accel_pipeline.optimization.aux_bench import process_aux_bench_after_gates

        selected = ""
        if analysis_path.is_file():
            selected = (
                json.loads(analysis_path.read_text(encoding="utf-8"))
                .get("correctness_facts", {})
                .get("state_design", {})
                .get("selected_candidate_id", "")
            )
        process_aux_bench_after_gates(
            pipeline_config.repo_root,
            query,
            iteration=0,
            change_kind="baseline",
            active_candidate_id=selected or None,
            workload=workload,
        )


AUDIT_TASK = (
    "Produce final_audit.md: verify correctness, cite SF1 sweep numbers, "
    "and review scale-safety + harness fairness (no brittle gate chasing).\n\n"
    "Start from the query digest and the cumulative diff below; read a file only where they leave a question open."
)


def run_audit(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
):
    workload = load_run_workload(pipeline_config, workload_name)

    def audit_turn(message: str):
        return run_agent_stage(
            stage="audit",
            prompt_file="04_audit.md",
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=message,
            model_overrides=model_overrides,
            dry_run=dry_run,
            session_role=READONLY,
        )

    if dry_run:
        return audit_turn(AUDIT_TASK)
    # a failing audit gate goes to the repair loop (the fix runs in the implementation session, not the reviewer's)
    ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage="audit",
                    change_kind="audit", model_overrides=model_overrides)

    def audit_and_fix():
        result = audit_turn(f"{AUDIT_TASK}\n\n{audit_context(pipeline_config, workload, query)}")
        _fix_blocking_findings(ctx, audit_turn)
        return result

    return run_with_repair(ctx, audit_and_fix, gates=workload_post_audit_gates(workload))


def _audit_result(ctx: RepairCtx) -> dict:
    return read_json(ctx.art / "audit_result.json")


def _fix_blocking_findings(ctx: RepairCtx, audit_turn) -> None:
    """The audit's blocking findings go to the implementation session as one batched repair round; the
    audit session then re-checks only those findings. What is still blocking is recorded, not looped on;
    an open correctness or fairness finding stops the stage."""
    findings = blocking_findings(_audit_result(ctx))
    if not findings:
        return
    repo_root = ctx.pipeline_config.repo_root
    qdir = query_dir(str(ctx.workload.get("benchmark_name") or "tpch"), ctx.query)
    base_file = ctx.art / IMPLEMENTATION_BASE_FILE
    base = base_file.read_text(encoding="utf-8").strip() if base_file.is_file() else "HEAD"
    before = snapshot(repo_root, changed_files(repo_root, base, qdir))
    ctx.log(f"audit raised {len(findings)} blocking findings; one batched repair round")
    revise_pending_suite(ctx)  # a request left pending by an earlier round goes first: repair_turn would run only the tester
    repair_turn(ctx, Failure(
        kind="code", stage="audit", source="audit_findings",
        summary=f"The independent audit found {len(findings)} blocking problems. Fix each in your query's files "
                "(or, if one is wrong, say why in the experiment log and leave it). A gap in the frozen test suite is not yours to "
                "edit: write it as a test_suite_change_request.md in the artifacts and the tester will handle it.",
        evidence=numbered(findings)), must_change=False)
    # a finding about the frozen suite is answered by a change request: the tester handles it before the re-check
    revised = revise_pending_suite(ctx)
    touched = touched_since(repo_root, before, base, qdir)
    audit_turn(
        ("The tester revised the frozen test suite on the implementer's change request (see test_suite_change_requests/).\n\n"
         if revised else "")
        + "Re-check only these findings against the current tree and update audit_result.json and final_audit.md "
        "(a finding that is resolved leaves the lists; one that is not stays, with its evidence).\n\n"
        f"{numbered(findings)}\n\nFiles changed by the repair round: {', '.join(touched) or 'none'}\n\n"
        f"```diff\n{cumulative_diff(repo_root, base, touched) or '(no changes)'}\n```"
    )
    remaining = blocking_findings(_audit_result(ctx))
    report = ctx.art / AUDIT_FINDINGS_FILE
    if remaining:
        report.write_text("# Audit findings still open after the repair round\n\n" + numbered(remaining) + "\n",
                          encoding="utf-8")
        append_experiment_log(repo_root, ctx.query, f"- audit: {len(remaining)} findings still open (see {report.name})",
                              workload=ctx.workload)
        fatal = [f for f in remaining if f.startswith(("the audit's verdict is correctness_ok=false", "fairness:"))]
        if fatal:
            raise stop_stage(ctx, Failure(kind="code", stage="audit", source="audit_findings",
                                          summary="correctness or fairness findings survived the repair round",
                                          evidence=numbered(fatal)), "audit correctness/fairness findings open")
    else:
        report.unlink(missing_ok=True)
        ctx.log("the re-check cleared every blocking finding")


def _substitute(template: str, values: dict[str, str]) -> str:
    out = template
    for key, value in values.items():
        out = out.replace("{{" + key + "}}", value)
    return out


def _type_prefix(create_fn: str) -> str:
    name = create_fn
    if name.startswith("Create"):
        name = name[6:]
    if name.endswith("FunctionSet"):
        name = name[: -len("FunctionSet")]
    return name


def read_api_manifest(repo_root: Path, engine: str) -> dict[str, dict[str, list[str]]]:
    """Role -> {headers, symbols} from the first table under `## Contract roles` of the engine's api/README.md."""
    readme = repo_root / "backends" / engine / "api" / "README.md"
    if not readme.is_file():
        return {}
    roles: dict[str, dict[str, list[str]]] = {}
    in_section = False
    for line in readme.read_text(encoding="utf-8").splitlines():
        if line.startswith("## "):
            in_section = line.strip() == "## Contract roles"
            continue
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if not in_section or not line.startswith("|") or len(cells) != 3 or set(cells[0]) <= set("-: "):
            continue
        if cells[0] != "role":
            roles[cells[0]] = {"headers": _backticked(cells[1]), "symbols": _backticked(cells[2])}
    return roles


def _backticked(cell: str) -> list[str]:
    return re.findall(r"`([^`]+)`", cell)


def _api_stub_values(manifest: dict[str, dict[str, list[str]]]) -> dict[str, str]:
    headers = dict.fromkeys(h for role in manifest.values() for h in role["headers"])
    return {
        "api_includes": "".join(f'#include "{h}"\n' for h in headers),
        "api_manifest": "\n".join(
            f"//   {role}: {', '.join(spec['symbols'])} ({', '.join(spec['headers'])})"
            for role, spec in manifest.items()
        )
        or "//   (backends/<engine>/api/README.md lists none)",
    }


def _build_stub_values(
    workload: dict[str, Any],
    paths: dict[str, str],
    query: int,
    manifest: dict[str, dict[str, list[str]]] | None = None,
) -> dict[str, str]:
    benchmark = benchmark_of(workload)
    header_basename = str(
        workload.get("header_basename", paths["header_path"].split("/")[-1].replace(".hpp", ""))
    )
    type_prefix = _type_prefix(paths["create_fn"])
    output_schema = list(workload.get("output_schema", []))
    schema_names = ", ".join(f'"{col["name"]}"' for col in output_schema) or '"result"'

    # cost_model.hpp: AuxCostModelLocation{<config dir>, <file stem>} for every engine.
    bench_camel = "".join(part.title() for part in benchmark.split("_"))
    cost_model_json = str(paths.get("cost_model_default") or f"accelerators/{benchmark}/q{query}/config/q{query}_aux_cost_model.json")
    cost_model_dir, _, cost_model_file = cost_model_json.rpartition("/")

    return {
        "query_number": str(query),
        "engine_name": str(workload.get("engine_name") or ""),
        "cost_model_const": f"k{bench_camel}Q{query}CostModel",
        "cost_model_dir": cost_model_dir,
        "cost_model_stem": cost_model_file.removesuffix(".json"),
        "function_name": paths["function_name"],
        "create_fn": paths["create_fn"],
        "header_basename": header_basename,
        "type_prefix": type_prefix,
        "scan_spec_fn": f"Create{type_prefix}ScanSpec",
        "kernel_fn": f"Create{type_prefix}Kernel",
        "profiling_kernel_fn": f"Create{type_prefix}ProfilingKernel",
        "display_name": str(workload.get("display_name", benchmark)),
        "benchmark_name": benchmark,
        "test_target": paths["test_target"],
        "bench_target": paths["bench_target"],
        "output_schema_names": schema_names,
        **_api_stub_values(manifest or {}),
    }


def _write_if_missing(path: Path, content: str) -> bool:
    if path.is_file():
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return True


def materialize_workload_stubs(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> dict[str, Any]:
    """Create missing header/kernel/package/test/benchmark files from templates."""
    repo = pipeline_config.repo_root
    pipe = pipeline_config.pipeline_root
    paths = resolve_workload_paths(pipeline_config, workload, query)
    benchmark = benchmark_of(workload)
    engine = str(workload.get("engine_name") or "")
    values = _build_stub_values(with_facts(repo, workload, query), paths, query, read_api_manifest(repo, engine))

    created: list[str] = []
    tpl_root = pipe / "templates" / "scaffold"
    bench_tpl_root = pipe / "templates" / benchmark

    stub_specs: list[tuple[Path, Path]] = [
        # Every query declares where its cost models live; each engine's
        # registration passes it on (catalog entry / Runtime).
        (tpl_root / "cost_model.hpp.tpl", repo / paths["cost_model_header_path"]),
        (tpl_root / "header.hpp.tpl", repo / paths["header_path"]),
        (tpl_root / "kernel.cpp.tpl", repo / paths["kernel_path"]),
        (tpl_root / "package.cpp.tpl", repo / paths["package_path"]),
    ]
    # The query registers itself from its own directory.
    register_path = str((workload.get("engine_host") or {}).get("register_path") or "")
    if register_path:
        stub_specs.append((tpl_root / "register.cpp.tpl", repo / register_path))
    if (bench_tpl_root / "test_template.cpp").is_file():
        stub_specs.append((bench_tpl_root / "test_template.cpp", repo / paths["test_path"]))
    if (bench_tpl_root / "benchmark_template.cpp").is_file():
        stub_specs.append((bench_tpl_root / "benchmark_template.cpp", repo / paths["benchmark_path"]))

    for tpl_path, dest in stub_specs:
        if not tpl_path.is_file():
            continue
        content = _substitute(tpl_path.read_text(encoding="utf-8"), values)
        if _write_if_missing(dest, content):
            created.append(str(dest.relative_to(repo)))

    return {
        "created": created,
        "skipped_existing": max(0, len(stub_specs) - len(created)),
    }


def materialize_from_gate(
    repo_root: Path, query: int, engine: str, workload_name: str | None = None
) -> dict[str, Any]:

    pipeline_config = load_pipeline_config(repo_root=repo_root)
    if workload_name is None:
        workload_name = _workload_name_from_gate(repo_root, query, engine)
    workload = load_workload(workload_name, repo_root, engine=engine)
    if workload.get("query_number") is None:
        workload = dict(workload)
        workload["query_number"] = query
    return materialize_workload_stubs(pipeline_config, workload, query)


def _workload_name_from_gate(repo_root: Path, query: int, engine: str) -> str:
    import json

    artifacts = repo_root / "pipeline" / "artifacts"
    candidates = sorted(artifacts.glob(f"*/q{query}/{engine}/gates/_gate_context.json"))
    if not candidates:
        raise FileNotFoundError(f"gate context not found for query {query} on {engine}")
    ctx = json.loads(candidates[-1].read_text(encoding="utf-8"))
    workload_name = ctx.get("workload_name")
    if workload_name:
        return str(workload_name)
    benchmark = benchmark_of(ctx)
    return f"{benchmark}_q{query}"


def _cpp_string_list(items: list[str]) -> str:
    return "{" + ", ".join('"' + item.replace("\\", "\\\\").replace('"', '\\"') + '"' for item in items) + "}"


def materialize_registration(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """The query's registration before any test is written: the engine's scaffold (`backends/<engine>/api/scaffold/
    register.cpp.tpl`) filled with the query's SQL and facts, so the query is registered with a module that declines
    every call and the fallback SQL answers. Written only when missing: the implementation replaces it in place."""
    import json


    repo = pipeline_config.repo_root
    if workload.get("workload_kind") != "aux_state":  # the declining module is an aux module; a stateless query has none
        return {"created": [], "skipped": "not an aux-state workload"}
    engine = str(workload.get("engine_name") or "")
    host = workload.get("engine_host") or {}
    destination = str(host.get("aux_register_path") or "")
    template = repo / "backends" / engine / "api" / "scaffold" / "register.cpp.tpl"
    if not destination or not template.is_file():
        return {"created": [], "skipped": f"engine {engine!r} has no registration scaffold"}
    if (repo / destination).is_file():
        return {"created": [], "skipped_existing": 1}
    benchmark = benchmark_of(workload)
    analysis = json.loads(analysis_artifact_path(repo, query, workload=workload).read_text(encoding="utf-8"))
    facts = with_facts(repo, workload, query)
    accelerator_id = str(analysis["accelerator_id"])
    values = {
        "benchmark_name": benchmark,
        "engine_name": engine,
        "query_number": str(query),
        "accelerator_id": accelerator_id,
        "table_function": "accel_" + accelerator_id,
        "argument_types": _cpp_string_list([str(p["type"]).upper() for p in workload.get("parameters", [])]),
        "base_sql": Path(workload["reference_sql_path"]).read_text(encoding="utf-8").strip().rstrip(";"),
        "fallback_sql": (repo / "workloads" / benchmark / f"q{query}.sql").read_text(encoding="utf-8").strip().rstrip(";"),
        "tables": _cpp_string_list([t["name"] for t in facts["dependent_tables"]]),
    }
    content = _substitute(template.read_text(encoding="utf-8"), values)
    _write_if_missing(repo / destination, content)
    return {"created": [destination]}


REQUEST_FILENAME = "structural_reanalysis_request.json"
HISTORY_FILENAME = "structural_reanalysis_history.jsonl"

BOTTLENECK_TYPES = frozenset(
    {
        "residual_read_path",
        "scan_for_scan",
        "wrong_partition_grain",
        "maintenance_infeasible",
        "insufficient_state_expressiveness",
        "other",
    }
)

REQUIRED_FIELDS = (
    "bottleneck_type",
    "evidence",
    "proposed_direction",
    "current_candidate_id",
)


def request_path(repo_root: Path, query: int, workload: dict[str, Any]) -> Path:
    return query_artifacts_dir(repo_root, query, workload=workload) / REQUEST_FILENAME


def history_path(repo_root: Path, query: int, workload: dict[str, Any]) -> Path:
    return query_artifacts_dir(repo_root, query, workload=workload) / HISTORY_FILENAME


def max_structural_reanalysis_count(workload: dict[str, Any]) -> int:
    value = workload.get("max_structural_reanalysis")
    if value is None:
        return 2
    return int(value)


def load_pending_request(repo_root: Path, query: int, workload: dict[str, Any]) -> dict[str, Any] | None:
    path = request_path(repo_root, query, workload)
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return None
    if not isinstance(data, dict) or not data.get("requested"):
        return None
    return data


def validate_request(data: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    for field in REQUIRED_FIELDS:
        if not str(data.get(field, "")).strip():
            errors.append(f"missing or empty field: {field}")
    bottleneck = str(data.get("bottleneck_type", "")).strip()
    if bottleneck and bottleneck not in BOTTLENECK_TYPES:
        errors.append(f"invalid bottleneck_type: {bottleneck}")
    evidence = str(data.get("evidence", "")).strip()
    if evidence and len(evidence) < 40:
        errors.append("evidence must be at least 40 characters with concrete measurements or gate output")
    direction = str(data.get("proposed_direction", "")).strip()
    if direction and len(direction) < 20:
        errors.append("proposed_direction must describe the structural change needed")
    return errors


def write_request(
    repo_root: Path,
    query: int,
    workload: dict[str, Any],
    *,
    source_stage: str,
    source_iteration: int,
    bottleneck_type: str,
    evidence: str,
    proposed_direction: str,
    current_candidate_id: str,
    alternate_candidate_ids: list[str] | None = None,
    cost_model_insufficient_reason: str = "",
) -> dict[str, Any]:
    payload = {
        "requested": True,
        "source_stage": source_stage,
        "source_iteration": int(source_iteration),
        "bottleneck_type": bottleneck_type.strip(),
        "evidence": evidence.strip(),
        "proposed_direction": proposed_direction.strip(),
        "current_candidate_id": current_candidate_id.strip(),
        "alternate_candidate_ids": list(alternate_candidate_ids or []),
        "cost_model_insufficient_reason": cost_model_insufficient_reason.strip(),
    }
    errors = validate_request(payload)
    if errors:
        raise ValueError("; ".join(errors))
    path = request_path(repo_root, query, workload)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return payload


def clear_request(repo_root: Path, query: int, workload: dict[str, Any]) -> None:
    path = request_path(repo_root, query, workload)
    if path.is_file():
        path.unlink()


def append_history(repo_root: Path, query: int, workload: dict[str, Any], entry: dict[str, Any]) -> None:
    path = history_path(repo_root, query, workload)
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(entry, sort_keys=True) + "\n")


def build_reanalysis_user_message(request: dict[str, Any], *, rerun_number: int) -> str:
    alternates = request.get("alternate_candidate_ids") or []
    alt_line = f"Alternate candidate IDs to reconsider: {', '.join(alternates)}." if alternates else ""
    cost_line = request.get("cost_model_insufficient_reason") or ""
    return (
        f"Structural re-analysis #{rerun_number} requested after {request.get('source_stage')} "
        f"(iteration {request.get('source_iteration', 0)}).\n\n"
        f"Bottleneck type: {request.get('bottleneck_type')}\n"
        f"Current candidate: {request.get('current_candidate_id')}\n"
        f"Evidence: {request.get('evidence')}\n"
        f"Proposed direction: {request.get('proposed_direction')}\n"
        f"{alt_line}\n"
        f"{('Cost-model tuning insufficient because: ' + cost_line) if cost_line else ''}\n\n"
        "Revise analysis.json: update state_candidate_search, select a new candidate if warranted, "
        "and refresh correctness_facts for the new design. Preserve prior rejected candidates in search "
        "with rejection reasons. Do not implement code."
    ).strip()


def build_analysis_revision_request(request: dict[str, Any], *, rerun_number: int, max_reruns: int) -> str:
    return (
        f"Structural re-analysis run {rerun_number} of {max_reruns} allowed "
        f"(triggered after {request.get('source_stage')}, iteration {request.get('source_iteration', 0)}).\n\n"
        f"Bottleneck type: {request.get('bottleneck_type')}\n"
        f"Current candidate: {request.get('current_candidate_id')}\n"
        f"Measured evidence: {request.get('evidence')}\n"
        f"Proposed direction: {request.get('proposed_direction')}\n"
        + (
            f"Alternate candidates to reconsider: {', '.join(request.get('alternate_candidate_ids') or [])}\n"
            if request.get("alternate_candidate_ids")
            else ""
        )
        + (
            f"Cost-model tuning insufficient: {request.get('cost_model_insufficient_reason')}\n"
            if request.get("cost_model_insufficient_reason")
            else ""
        )
        + f"\nFull request:\n```json\n{json.dumps(request, indent=2)}\n```"
    )


def can_rerun_analysis(structural_reanalysis_count: int, workload: dict[str, Any]) -> bool:
    return structural_reanalysis_count < max_structural_reanalysis_count(workload)


def process_after_stage(
    *,
    repo_root: Path,
    query: int,
    workload: dict[str, Any],
    triggered_after: str,
    structural_reanalysis_count: int,
    run_analysis,
    run_implementation,
    run_optimization,
    dry_run: bool,
    run_test_suite=None,
) -> tuple[int, int] | None:
    """
    If a valid structural reanalysis request is pending and budget remains, re-run analysis
    (the test suite, implementation, plus optimization when triggered from optimization).

    Returns (new_structural_reanalysis_count, next_stage_index) or None if no reanalysis ran.
    """
    request = load_pending_request(repo_root, query, workload)
    if not request:
        return None

    errors = validate_request(request)
    if errors:
        reason = "; ".join(errors)
        log_structural_reanalysis_skipped(
            repo_root, query, workload, triggered_after=triggered_after, reason=f"invalid request: {reason}"
        )
        append_experiment_log(
            repo_root,
            query,
            f"- structural_reanalysis: rejected invalid request after {triggered_after}: "
            + "; ".join(errors),
            workload=workload,
        )
        clear_request(repo_root, query, workload)
        return None

    if not can_rerun_analysis(structural_reanalysis_count, workload):
        reason = (
            f"budget exhausted ({structural_reanalysis_count}/"
            f"{max_structural_reanalysis_count(workload)})"
        )
        log_structural_reanalysis_skipped(
            repo_root, query, workload, triggered_after=triggered_after, reason=reason
        )
        append_experiment_log(
            repo_root,
            query,
            f"- structural_reanalysis: budget exhausted ({structural_reanalysis_count}/"
            f"{max_structural_reanalysis_count(workload)}); ignoring request after {triggered_after}",
            workload=workload,
        )
        clear_request(repo_root, query, workload)
        return None

    if dry_run:
        return None

    new_count = structural_reanalysis_count + 1
    rerun_number = new_count
    log_structural_reanalysis_triggered(
        repo_root,
        query,
        workload,
        rerun_number=rerun_number,
        triggered_after=triggered_after,
        bottleneck_type=str(request.get("bottleneck_type", "")),
        current_candidate_id=str(request.get("current_candidate_id", "")),
    )
    append_history(
        repo_root,
        query,
        workload,
        {
            "rerun_number": rerun_number,
            "triggered_after": triggered_after,
            "request": request,
        },
    )
    append_experiment_log(
        repo_root,
        query,
        f"- structural_reanalysis #{rerun_number}: triggered after {triggered_after}; "
        f"bottleneck={request.get('bottleneck_type')}; "
        f"candidate={request.get('current_candidate_id')}",
        workload=workload,
    )

    user_message = build_reanalysis_user_message(request, rerun_number=rerun_number)
    revision_request = build_analysis_revision_request(
        request,
        rerun_number=rerun_number,
        max_reruns=max_structural_reanalysis_count(workload),
    )

    clear_request(repo_root, query, workload)

    run_analysis(user_message=user_message, analysis_revision_request=revision_request)
    if run_test_suite is not None:
        run_test_suite()
    run_implementation()
    if triggered_after == "optimization":
        run_optimization()

    next_stage = STAGES.index("audit") if triggered_after == "optimization" else STAGES.index("optimization")
    return new_count, next_stage


@dataclass
class ReanalysisToolContext:
    pipeline_config: PipelineConfig
    query: int
    stage: str
    workload_name: str
    optimization_iter: int = 0


def make_reanalysis_tools(ctx: ReanalysisToolContext):
    def request_structural_reanalysis(
        bottleneck_type: str,
        evidence: str,
        proposed_direction: str,
        current_candidate_id: str,
        alternate_candidate_ids: list[str] | None = None,
        cost_model_insufficient_reason: str | None = "",
    ) -> str:
        """The tool as the agent sees it is declared in mcp.register_core."""
        cost_model_insufficient_reason = str(cost_model_insufficient_reason or "")
        workload = load_run_workload(ctx.pipeline_config, ctx.workload_name)
        state = load_or_create(
            ctx.pipeline_config.repo_root, ctx.query, ctx.workload_name, ctx.pipeline_config.engine_name, resume=True
        )
        if state.structural_reanalysis_count >= max_structural_reanalysis_count(workload):
            log_structural_reanalysis_request(
                ctx.pipeline_config.repo_root,
                ctx.query,
                workload,
                status="rejected",
                source_stage=ctx.stage,
                source_iteration=ctx.optimization_iter,
                bottleneck_type=bottleneck_type,
                current_candidate_id=current_candidate_id,
                reason=f"budget exhausted ({state.structural_reanalysis_count})",
            )
            return json.dumps(
                {
                    "status": "rejected",
                    "reason": f"structural reanalysis budget exhausted ({state.structural_reanalysis_count})",
                }
            )

        try:
            payload = write_request(
                ctx.pipeline_config.repo_root,
                ctx.query,
                workload,
                source_stage=ctx.stage,
                source_iteration=ctx.optimization_iter,
                bottleneck_type=bottleneck_type,
                evidence=evidence,
                proposed_direction=proposed_direction,
                current_candidate_id=current_candidate_id,
                alternate_candidate_ids=alternate_candidate_ids,
                cost_model_insufficient_reason=cost_model_insufficient_reason,
            )
        except ValueError as exc:
            log_structural_reanalysis_request(
                ctx.pipeline_config.repo_root,
                ctx.query,
                workload,
                status="rejected",
                source_stage=ctx.stage,
                source_iteration=ctx.optimization_iter,
                bottleneck_type=bottleneck_type,
                current_candidate_id=current_candidate_id,
                reason=str(exc),
            )
            return json.dumps({"status": "rejected", "reason": str(exc), "allowed_bottleneck_types": sorted(BOTTLENECK_TYPES)})

        allowed = sorted(BOTTLENECK_TYPES)
        log_structural_reanalysis_request(
            ctx.pipeline_config.repo_root,
            ctx.query,
            workload,
            status="queued",
            source_stage=ctx.stage,
            source_iteration=ctx.optimization_iter,
            bottleneck_type=payload["bottleneck_type"],
            current_candidate_id=payload["current_candidate_id"],
        )
        return json.dumps(
            {
                "status": "queued",
                "message": (
                    "Request recorded. The orchestrator will re-run analysis after this stage "
                    "if the request is valid and budget remains."
                ),
                "bottleneck_type": payload["bottleneck_type"],
                "allowed_bottleneck_types": allowed,
                "validation_errors": [],
            }
        )

    return request_structural_reanalysis


# ---- the stages by name -------------------------------------------------------------------------

def _run_optimization(*args, **kwargs) -> None:
    from accel_pipeline.optimization.stage import run  # the optimization package is imported only when its stage runs

    run(*args, **kwargs)


STAGE_RUNNERS = {
    "analysis": run_analysis,
    "test_suite": run_test_suite,
    "implementation": run_implementation,
    "optimization": _run_optimization,
    "audit": run_audit,
}
# what a stage's run takes besides (pipeline_config, model_config, workload_name, query, model_overrides, dry_run)
STAGE_OPTIONS = {
    "implementation": ("skip_gates",),
    "optimization": ("skip_gates", "resume"),
}


def run_stage(stage: str, pipeline_config, model_config, workload_name: str, query: int,
              model_overrides: dict[str, str], dry_run: bool, **options) -> None:
    """Run `stage`, passing along those of `options` that it takes."""
    if stage not in STAGE_RUNNERS:
        raise ValueError(f"unknown stage: {stage}")
    accepted = {name: options[name] for name in STAGE_OPTIONS.get(stage, ()) if name in options}
    STAGE_RUNNERS[stage](pipeline_config, model_config, workload_name, query, model_overrides, dry_run, **accepted)
