from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any

MUTATION_KINDS_DEFAULT = ("insert", "update", "delete")

BASE_PATH_DECOMPOSITION_FIELDS = (
    "dominant_scans",
    "dominant_joins",
    "aggregation_grouping_work",
    "sorting_topk_work",
    "parameter_dependent_work",
    "work_repeated_across_invocations",
    "cardinality_scaling",
    "parameter_range_scaling",
    "output_cardinality_scaling",
    "work_remaining_after_aux_replacement",
)

# Typical-binding read work of a stateful design must undercut the base path's by this factor.
MIN_TYPICAL_READ_WORK_RATIO = 100.0

STATE_DESIGN_READ_PATH_FIELDS = (
    "selected_candidate_id",
    "physical_organization",
    "composition_rule",
    "residual_read_algorithm",
    "read_path_bottleneck_risk",
    "read_path_scaling_variable",
    "estimated_state_units_read",
    "parameter_scaling_argument",
)

CANDIDATE_REQUIRED_FIELDS = (
    "id",
    "design_family",
    "viability",
    "selected",
    "state_grain",
    "output_grain",
    "physical_organization",
    "state_payload",
    "composition_rule",
    "operators_eliminated",
    "residual_read_algorithm",
    "estimated_state_units_read",
    "read_path_bottleneck_risk",
    "read_path_scaling_variable",
    "parameter_scaling_argument",
    "asymptotic_read_complexity",
    "practical_read_complexity",
    "point_update_complexity",
    "batch_update_complexity",
    "storage_complexity",
    "mutation_fanout",
    "rebuild_locality",
    "parameter_binding_behavior",
    "history_growth_behavior",
    "best_regime",
)

CANDIDATE_SELECTED_FIELDS = CANDIDATE_REQUIRED_FIELDS

CANDIDATE_ALTERNATE_VIABLE_FIELDS = (
    "id",
    "design_family",
    "viability",
    "selected",
    "state_grain",
    "physical_organization",
    "composition_rule",
    "operators_eliminated",
    "read_path_bottleneck_risk",
    "best_regime",
)

CANDIDATE_REJECTED_FIELDS = (
    "id",
    "design_family",
    "viability",
    "selected",
    "rejection_reason",
    "state_grain",
    "physical_organization",
)


def _required_candidate_fields(candidate: dict[str, Any]) -> tuple[str, ...]:
    if candidate.get("selected") is True:
        return CANDIDATE_SELECTED_FIELDS
    if candidate.get("viability") == "rejected":
        return CANDIDATE_REJECTED_FIELDS
    return CANDIDATE_ALTERNATE_VIABLE_FIELDS


class AuxAnalysisValidationError(Exception):
    """Raised when aux-state analysis.json fails validation."""


def _load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def _mutation_pairs_from_tables(
    dependent_tables: list[dict[str, Any]],
    mutation_kinds: tuple[str, ...] = MUTATION_KINDS_DEFAULT,
) -> set[tuple[str, str]]:
    pairs: set[tuple[str, str]] = set()
    for table in dependent_tables:
        name = table.get("name", "")
        if not name:
            continue
        for mutation in mutation_kinds:
            pairs.add((name, mutation))
    return pairs


def _normalize_mutation_kind(raw: Any) -> str:
    mutation = str(raw or "").strip().lower()
    if mutation.startswith("insert"):
        return "insert"
    if mutation.startswith("update"):
        return "update"
    if mutation.startswith("delete"):
        return "delete"
    return mutation


def repair_aux_analysis(analysis: dict[str, Any]) -> tuple[dict[str, Any], list[str]]:
    """Apply safe, deterministic fixes for common agent omissions before validation."""
    repaired = copy.deepcopy(analysis)
    repairs: list[str] = []

    search = repaired.get("state_candidate_search")
    if isinstance(search, dict):
        candidates = [c for c in (search.get("candidates") or []) if isinstance(c, dict)]
        viable = [c for c in candidates if c.get("viability") == "viable"]
        explanation = str(search.get("fewer_than_three_explanation") or "").strip()
        if 0 < len(viable) < 3 and not explanation:
            viable_ids = ", ".join(str(c.get("id", "?")) for c in viable)
            rejected_ids = ", ".join(
                str(c.get("id", "?")) for c in candidates if c.get("viability") == "rejected"
            )
            tail = f" Rejected: {rejected_ids}." if rejected_ids else ""
            search["fewer_than_three_explanation"] = (
                f"Only {len(viable)} materially different viable design(s) identified: {viable_ids}.{tail}"
            )
            repairs.append("filled state_candidate_search.fewer_than_three_explanation")

    facts = repaired.get("correctness_facts")
    if isinstance(facts, dict):
        coverage = facts.get("mutation_coverage")
        if isinstance(coverage, list):
            for entry in coverage:
                if not isinstance(entry, dict):
                    continue
                normalized = _normalize_mutation_kind(entry.get("mutation"))
                if normalized in MUTATION_KINDS_DEFAULT and normalized != entry.get("mutation"):
                    entry["mutation"] = normalized
                    repairs.append(f"normalized mutation_coverage mutation for {entry.get('table')}")

    return repaired, repairs


def _validate_mutation_coverage(
    dependent_tables: list[dict[str, Any]],
    mutation_coverage: list[dict[str, Any]],
) -> None:
    required = _mutation_pairs_from_tables(dependent_tables)
    covered: set[tuple[str, str]] = set()
    for entry in mutation_coverage:
        table = str(entry.get("table", "")).strip()
        mutation = _normalize_mutation_kind(entry.get("mutation"))
        behavior = entry.get("behavior", "")
        if not table or not mutation or not behavior:
            raise AuxAnalysisValidationError("mutation_coverage entries require table, mutation, behavior")
        pair = (table, mutation)
        if pair in covered:
            raise AuxAnalysisValidationError(f"duplicate mutation_coverage entry: {pair}")
        covered.add(pair)
    missing = required - covered
    if missing:
        raise AuxAnalysisValidationError(f"mutation_coverage missing pairs: {sorted(missing)}")


def _validate_estimated_state_units_read(value: Any, label: str) -> None:
    if not isinstance(value, dict):
        raise AuxAnalysisValidationError(f"{label}.estimated_state_units_read must be an object")
    for key in ("typical", "worst_case"):
        if not str(value.get(key, "")).strip():
            raise AuxAnalysisValidationError(f"{label}.estimated_state_units_read.{key} is required")


def _positive_number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        return None
    return float(value)


def _validate_read_work_ratio(state_design: dict[str, Any]) -> None:
    """The design's per-read work must be far below the base path's, or the miss must be explicit."""
    ratio = state_design.get("read_work_ratio")
    where = "correctness_facts.state_design.read_work_ratio"
    if not isinstance(ratio, dict):
        raise AuxAnalysisValidationError(
            f"{where} is required: base-path work / design work per read, typical and worst "
            "(numbers; `basis` names the measured figures behind them)"
        )
    if not str(ratio.get("basis", "")).strip():
        raise AuxAnalysisValidationError(f"{where}.basis is required (the measured base work and design work)")
    typical = _positive_number(ratio.get("typical"))
    worst = _positive_number(ratio.get("worst"))
    if typical is None or worst is None:
        raise AuxAnalysisValidationError(f"{where}.typical and {where}.worst must be positive numbers")
    if typical >= MIN_TYPICAL_READ_WORK_RATIO:
        return
    unmet = str(ratio.get("unmet_justification", "")).strip()
    families = [f for f in ratio.get("families_evaluated") or []
                if isinstance(f, dict) and str(f.get("family", "")).strip()
                and _positive_number(f.get("typical_ratio")) is not None]
    if len(unmet) < 40 or len(families) < 3:
        raise AuxAnalysisValidationError(
            f"design target unmet: {where}.typical is {typical:g}, below the required "
            f"{MIN_TYPICAL_READ_WORK_RATIO:g}x. Revise the design in this stage (finer partitioning by the "
            "predicate's columns, pre-aggregation at a coarser grain with output-sensitive refinement, a "
            "data-derived bound that tightens with the parameter) or select a candidate that meets it; if no "
            "candidate can, set unmet_justification (at least 40 characters) and families_evaluated (at least "
            "three families from the prompt's list, each with family, a numeric typical_ratio and why_short)"
        )


def _validate_base_path_cost_decomposition(decomposition: Any) -> None:
    if not isinstance(decomposition, dict):
        raise AuxAnalysisValidationError(
            "performance_hypotheses.base_path_cost_decomposition is required when fitness_assessment.recommended is true"
        )
    for key in BASE_PATH_DECOMPOSITION_FIELDS:
        if key not in decomposition:
            raise AuxAnalysisValidationError(
                f"performance_hypotheses.base_path_cost_decomposition.{key} is required"
            )
    for list_key in ("dominant_scans", "dominant_joins", "work_repeated_across_invocations"):
        if not isinstance(decomposition.get(list_key), list):
            raise AuxAnalysisValidationError(
                f"performance_hypotheses.base_path_cost_decomposition.{list_key} must be an array"
            )
    for list_key in ("work_remaining_after_aux_replacement",):
        remaining = decomposition.get(list_key)
        if not isinstance(remaining, list) or not remaining:
            raise AuxAnalysisValidationError(
                f"performance_hypotheses.base_path_cost_decomposition.{list_key} must be a non-empty array"
            )


def _candidate_material_signature(candidate: dict[str, Any]) -> tuple[str, str, str]:
    return (
        str(candidate.get("design_family", "")).strip().lower(),
        str(candidate.get("physical_organization", "")).strip().lower(),
        str(candidate.get("composition_rule", "")).strip().lower(),
    )


def _validate_state_candidate_search(analysis: dict[str, Any]) -> None:
    search = analysis.get("state_candidate_search")
    if not isinstance(search, dict):
        raise AuxAnalysisValidationError("state_candidate_search is required when fitness_assessment.recommended is true")

    candidates = search.get("candidates")
    if not isinstance(candidates, list) or not candidates:
        raise AuxAnalysisValidationError("state_candidate_search.candidates must be a non-empty array")

    explanation = str(search.get("fewer_than_three_explanation") or "").strip()
    viable = [c for c in candidates if isinstance(c, dict) and c.get("viability") == "viable"]
    if len(viable) < 3 and not explanation:
        raise AuxAnalysisValidationError(
            "state_candidate_search requires at least 3 viable candidates or fewer_than_three_explanation"
        )

    signatures: set[tuple[str, str, str]] = set()
    selected_ids: list[str] = []
    for index, candidate in enumerate(candidates):
        if not isinstance(candidate, dict):
            raise AuxAnalysisValidationError(f"state_candidate_search.candidates[{index}] must be an object")
        label = f"state_candidate_search.candidates[{index}]"
        required_fields = _required_candidate_fields(candidate)
        for key in required_fields:
            if key not in candidate:
                raise AuxAnalysisValidationError(f"{label}.{key} is required")
        if candidate.get("viability") == "rejected" and not str(candidate.get("rejection_reason") or "").strip():
            raise AuxAnalysisValidationError(f"{label}.rejection_reason is required when viability is rejected")
        if candidate.get("selected") is True:
            _validate_estimated_state_units_read(candidate.get("estimated_state_units_read"), label)
        signatures.add(_candidate_material_signature(candidate))
        if candidate.get("selected") is True:
            selected_ids.append(str(candidate.get("id", "")))

    if len(signatures) < min(3, len(candidates)) and not explanation:
        raise AuxAnalysisValidationError(
            "state_candidate_search.candidates must differ materially in design_family, "
            "physical_organization, or composition_rule"
        )

    if len(selected_ids) != 1:
        raise AuxAnalysisValidationError(
            "state_candidate_search.candidates must mark exactly one candidate selected=true"
        )

    state_design = analysis["correctness_facts"]["state_design"]
    selected_id = str(state_design.get("selected_candidate_id", ""))
    if selected_id != selected_ids[0]:
        raise AuxAnalysisValidationError(
            "correctness_facts.state_design.selected_candidate_id must match the selected candidate id"
        )


def _validate_state_design_read_path(state_design: dict[str, Any]) -> None:
    for key in STATE_DESIGN_READ_PATH_FIELDS:
        if key not in state_design:
            raise AuxAnalysisValidationError(f"correctness_facts.state_design.{key} is required")
    if state_design.get("read_path_bottleneck_risk") not in ("low", "medium", "high"):
        raise AuxAnalysisValidationError("correctness_facts.state_design.read_path_bottleneck_risk is invalid")
    _validate_estimated_state_units_read(
        state_design.get("estimated_state_units_read"),
        "correctness_facts.state_design",
    )


def _validate_required_fields(analysis: dict[str, Any]) -> None:
    for key in (
        "accelerator_id",
        "aux_function_name",
        "fitness_assessment",
        "correctness_facts",
        "files_to_create",
    ):
        if key not in analysis:
            raise AuxAnalysisValidationError(f"missing required field: {key}")

    fitness = analysis["fitness_assessment"]
    if "recommended" not in fitness:
        raise AuxAnalysisValidationError("fitness_assessment.recommended is required")

    facts = analysis["correctness_facts"]
    for key in (
        "dependent_tables",
        "partitioning",
        "state_design",
        "state_payload",
        "mutation_coverage",
        "fallback_conditions",
        "maintenance_actions_used",
    ):
        if key not in facts:
            raise AuxAnalysisValidationError(f"missing correctness_facts.{key}")

    state_design = facts["state_design"]
    if state_design.get("is_full_result_cache") is True:
        raise AuxAnalysisValidationError("state_design.is_full_result_cache must be false")

    alternatives = state_design.get("alternatives_considered", [])
    if len(alternatives) < 2:
        raise AuxAnalysisValidationError("state_design.alternatives_considered requires at least 2 entries")

    if not analysis.get("files_to_create"):
        raise AuxAnalysisValidationError("files_to_create must be non-empty")


def validate_aux_analysis(
    analysis: dict[str, Any],
    *,
    allow_poor_fitness: bool = False,
) -> None:
    """Validate aux-state analysis.json structure and business rules."""
    _validate_required_fields(analysis)

    fitness = analysis["fitness_assessment"]
    recommended = bool(fitness.get("recommended"))
    if not recommended and not allow_poor_fitness:
        raise AuxAnalysisValidationError(
            "fitness_assessment.recommended is false; set allow_poor_fitness or fix analysis"
        )

    facts = analysis["correctness_facts"]
    state_design = facts["state_design"]
    if state_design.get("negligible_speedup_risk") == "high" and not allow_poor_fitness:
        raise AuxAnalysisValidationError(
            "state_design.negligible_speedup_risk is high; set allow_poor_fitness or redesign"
        )

    _validate_state_design_read_path(state_design)
    _validate_mutation_coverage(facts["dependent_tables"], facts["mutation_coverage"])

    if recommended:
        performance = analysis.get("performance_hypotheses")
        if not isinstance(performance, dict):
            raise AuxAnalysisValidationError(
                "performance_hypotheses is required when fitness_assessment.recommended is true"
            )
        _validate_base_path_cost_decomposition(performance.get("base_path_cost_decomposition"))
        _validate_state_candidate_search(analysis)
        _validate_read_work_ratio(state_design)


def validate_aux_analysis_file(
    path: Path,
    *,
    allow_poor_fitness: bool = False,
) -> dict[str, Any]:
    if not path.is_file():
        raise AuxAnalysisValidationError(f"analysis file not found: {path}")
    analysis = _load_json(path)
    validate_aux_analysis(analysis, allow_poor_fitness=allow_poor_fitness)
    return analysis
