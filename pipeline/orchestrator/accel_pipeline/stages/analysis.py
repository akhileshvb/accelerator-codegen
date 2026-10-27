from __future__ import annotations

import json
import shutil
import sys
from pathlib import Path
from typing import Any

from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.workload import load_run_workload, analysis_artifact_path
from accel_pipeline.failure import StageFailure
from accel_pipeline.pipeline_log import log_analysis_candidates, log_event
from accel_pipeline.session import MAIN
from accel_pipeline.stages.base import run_agent_stage
from accel_pipeline.validation import AuxAnalysisValidationError, repair_aux_analysis, validate_aux_analysis_file


def _ensure_dataset(pipeline_config: PipelineConfig, workload_name: str) -> None:
    """The data tools query the target engine: bring its instance up with the dataset loaded before the agent starts."""
    from accel_pipeline.engine_host import ensure_dataset_loaded

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


def run(
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
