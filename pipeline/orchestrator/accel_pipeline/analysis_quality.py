"""Analysis-quality scoring for aux-state analysis.json fixtures."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.validation import validate_aux_analysis


def _load_fixture(path: Path) -> dict[str, Any]:
    return yaml.safe_load(path.read_text(encoding="utf-8")) or {}


def _score_dependencies(analysis: dict[str, Any], expected: dict[str, Any]) -> float:
    facts = analysis.get("correctness_facts", {})
    dep_tables = {t["name"] for t in facts.get("dependent_tables", [])}
    exp_tables = set(expected.get("tables", []))
    if not exp_tables:
        return 1.0
    if not exp_tables.issubset(dep_tables):
        return 0.0
    exp_cols = expected.get("columns", {})
    for table, cols in exp_cols.items():
        match = next((t for t in facts.get("dependent_tables", []) if t.get("name") == table), None)
        if not match:
            return 0.0
        watched = set(match.get("watched_columns", []))
        if not set(cols).issubset(watched):
            return 0.0
    return 1.0


def _score_partition_mapping(analysis: dict[str, Any], expected: dict[str, Any]) -> float:
    key_source = expected.get("key_source")
    if not key_source:
        return 1.0
    partitioning = analysis.get("correctness_facts", {}).get("partitioning", {})
    actual = partitioning.get("key_source")
    fitness = analysis.get("fitness_assessment", {})
    if actual == key_source or fitness.get("partition_key_source") == key_source:
        return 1.0
    return 0.0


def _score_state_design(analysis: dict[str, Any], expected: dict[str, Any]) -> float:
    state_design = analysis.get("correctness_facts", {}).get("state_design", {})
    if state_design.get("is_full_result_cache") != expected.get("is_full_result_cache", False):
        return 0.0
    contains = expected.get("chosen_representation_contains")
    if contains and contains not in str(state_design.get("chosen_representation", "")):
        return 0.0
    if state_design.get("negligible_speedup_risk") == "high":
        return 0.0
    max_bottleneck = expected.get("max_read_path_bottleneck_risk")
    if max_bottleneck:
        order = {"low": 0, "medium": 1, "high": 2}
        actual = state_design.get("read_path_bottleneck_risk")
        if order.get(actual, 99) > order.get(max_bottleneck, 99):
            return 0.0
    return 1.0


def _score_mutation_coverage(analysis: dict[str, Any], expected: dict[str, Any]) -> float:
    min_entries = int(expected.get("min_entries", 1))
    coverage = analysis.get("correctness_facts", {}).get("mutation_coverage", [])
    if len(coverage) < min_entries:
        return 0.0
    return 1.0


def _score_fitness(analysis: dict[str, Any], expected: dict[str, Any]) -> float:
    fitness = analysis.get("fitness_assessment", {})
    if fitness.get("recommended") != expected.get("recommended"):
        return 0.0
    signals_for = set(fitness.get("signals_for", []))
    for sig in expected.get("required_signals", []):
        if sig not in signals_for:
            return 0.5
    signals_against = set(fitness.get("signals_against", []))
    for sig in expected.get("required_signals_against", []):
        if sig not in signals_against:
            return 0.5
    return 1.0


def _score_base_path_decomposition(analysis: dict[str, Any], expected: dict[str, Any]) -> float:
    if not expected.get("required"):
        return 1.0
    decomposition = (analysis.get("performance_hypotheses") or {}).get("base_path_cost_decomposition")
    if not isinstance(decomposition, dict):
        return 0.0
    for key in expected.get("required_fields", []):
        if key not in decomposition:
            return 0.0
    return 1.0


def _score_candidate_search(analysis: dict[str, Any], expected: dict[str, Any]) -> float:
    if not expected.get("required"):
        return 1.0
    search = analysis.get("state_candidate_search")
    if not isinstance(search, dict):
        return 0.0
    candidates = search.get("candidates", [])
    min_viable = int(expected.get("min_viable_candidates", 3))
    viable = [c for c in candidates if isinstance(c, dict) and c.get("viability") == "viable"]
    if len(viable) < min_viable and not str(search.get("fewer_than_three_explanation") or "").strip():
        return 0.0
    selected = [c for c in candidates if isinstance(c, dict) and c.get("selected") is True]
    if len(selected) != 1:
        return 0.0
    required_families = expected.get("required_design_families", [])
    if required_families:
        families = {str(c.get("design_family", "")) for c in viable}
        if not set(required_families).issubset(families):
            return 0.5
    return 1.0


def score_analysis_against_fixture(analysis: dict[str, Any], fixture: dict[str, Any]) -> dict[str, float]:
    expected = fixture.get("expected", {})
    return {
        "dependencies": _score_dependencies(analysis, expected.get("dependencies", {})),
        "partition_mapping": _score_partition_mapping(analysis, expected.get("partition_mapping", {})),
        "state_design": _score_state_design(analysis, expected.get("state_design", {})),
        "mutation_coverage": _score_mutation_coverage(analysis, expected.get("mutation_coverage", {})),
        "fitness": _score_fitness(analysis, expected.get("fitness", {})),
        "base_path_decomposition": _score_base_path_decomposition(
            analysis, expected.get("base_path_decomposition", {})
        ),
        "candidate_search": _score_candidate_search(analysis, expected.get("candidate_search", {})),
    }


def load_fixture_dir(repo_root: Path) -> list[Path]:
    fixture_dir = repo_root / "pipeline" / "fixtures" / "analysis_quality"
    return sorted(fixture_dir.glob("*.yaml"))
