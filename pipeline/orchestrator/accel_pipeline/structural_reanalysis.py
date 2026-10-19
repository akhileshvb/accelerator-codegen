"""Bounded re-run of analysis when implementation/optimization finds a structural bottleneck."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.pipeline_log import (
    log_structural_reanalysis_skipped,
    log_structural_reanalysis_triggered,
)

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


def build_reanalysis_prompt_context(request: dict[str, Any], *, rerun_number: int, max_reruns: int) -> str:
    """Backward-compatible alias."""
    return build_analysis_revision_request(request, rerun_number=rerun_number, max_reruns=max_reruns)


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
    run_adversarial_tests=None,
) -> tuple[int, int] | None:
    """
    If a valid structural reanalysis request is pending and budget remains, re-run analysis
    (adversarial scenarios, implementation, plus optimization when triggered from optimization).

    Returns (new_structural_reanalysis_count, next_stage_index) or None if no reanalysis ran.
    """
    request = load_pending_request(repo_root, query, workload)
    if not request:
        return None

    errors = validate_request(request)
    if errors:
        from accel_pipeline.playbook import append_experiment_log

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
        from accel_pipeline.playbook import append_experiment_log

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
    from accel_pipeline.playbook import append_experiment_log

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
    if run_adversarial_tests is not None:
        run_adversarial_tests()
    run_implementation()
    if triggered_after == "optimization":
        run_optimization()

    from accel_pipeline.state import STAGES

    next_stage = STAGES.index("audit") if triggered_after == "optimization" else STAGES.index("optimization")
    return new_count, next_stage
