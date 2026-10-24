"""Stage-specific slices of analysis and optimization artifacts for lean prompts."""

from __future__ import annotations

import copy
import json
from pathlib import Path
from typing import Any

from accel_pipeline.textutil import squeeze_truncate

_STAGE_SHARED_DOCS: dict[str, list[tuple[str, str]]] = {
    "analysis": [
        ("Low-level execution", "../../shared/low_level_execution.md"),
        ("COW primitives", "cow_primitives.md"),
        ("Scale factors", "../../shared/scale_factor_contract.md"),
        ("Substrate primitives", "../../shared/substrate_primitives.md"),
        ("Fitness rubric", "aux_state_fitness.md"),
        ("Taxonomy index", "taxonomy_index.md"),
        ("Full taxonomy", "aux_state_taxonomy.md"),
        ("Design families", "aux_state_design.md"),
        ("Mutation coverage", "mutation_coverage.md"),
    ],
    "test_suite": [
        ("Test suite format", "../../shared/test_suite.md"),
        ("Scale factors", "../../shared/scale_factor_contract.md"),
        ("Mutation coverage", "mutation_coverage.md"),
        ("Bench sweep contract", "bench_sweep_contract.md"),
    ],
    "implementation": [
        ("Low-level execution", "../../shared/low_level_execution.md"),
        ("COW primitives", "cow_primitives.md"),
        ("Scale factors", "../../shared/scale_factor_contract.md"),
        ("Generated layout", "../../shared/generated_layout.md"),
        ("Engine interfaces", "../../shared/engine_interfaces.md"),
        ("Substrate primitives", "../../shared/substrate_primitives.md"),
        ("Module layout", "module_layout.md"),
        ("Unit-test contract", "unit_test_contract.md"),
        ("Read-path scaffold", "read_path_scaffold.md"),
        ("Bench sweep contract", "bench_sweep_contract.md"),
        ("Concurrency contract", "concurrency_contract.md"),
        ("Registration", "registration_checklist.md"),
        ("Agent boundary", "agent_boundary.md"),
    ],
    "optimization": [
        ("Scale factors", "../../shared/scale_factor_contract.md"),
        ("Substrate primitives", "../../shared/substrate_primitives.md"),
        ("Generated layout", "../../shared/generated_layout.md"),
        ("Bench sweep contract", "bench_sweep_contract.md"),
        ("Maintenance framework", "maintenance_framework.md"),
        ("Taxonomy index", "taxonomy_index.md"),
    ],
    "audit": [
        ("Low-level execution", "../../shared/low_level_execution.md"),
        ("COW primitives", "cow_primitives.md"),
        ("Scale factors", "../../shared/scale_factor_contract.md"),
        ("Engine interfaces", "../../shared/engine_interfaces.md"),
        ("Mutation coverage", "mutation_coverage.md"),
        ("Concurrency contract", "concurrency_contract.md"),
    ],
}


def build_shared_docs_reference(pipeline_root_rel: str = "pipeline", stage: str = "") -> str:
    base = f"{pipeline_root_rel}/prompts/aux_state/shared"
    shared_base = f"{pipeline_root_rel}/prompts/shared"
    docs = _STAGE_SHARED_DOCS.get(stage, _STAGE_SHARED_DOCS["analysis"])
    lines = ["Read only when needed:", ""]
    for label, path in docs:
        if path.startswith("../../shared/"):
            rel = f"{shared_base}/{path.removeprefix('../../shared/')}"
        else:
            rel = f"{base}/{path}"
        lines.append(f"- **{label}**: `{rel}`")
    if stage == "analysis":
        lines.append(f"- **Analysis schema**: `{pipeline_root_rel}/schemas/aux_state_analysis.schema.json`")
    return "\n".join(lines)


def compact_json(data: Any) -> str:
    return json.dumps(data, separators=(",", ":"), ensure_ascii=False)


def _compact_scan_order(scan: Any) -> Any:
    if scan is None:
        return []
    if isinstance(scan, str):
        return squeeze_truncate(scan, max_chars=240)
    if isinstance(scan, list):
        return scan[:2]
    if isinstance(scan, dict):
        return [
            f"{key}: {squeeze_truncate(str(value), max_chars=80)}"
            for key, value in list(scan.items())[:2]
        ]
    return squeeze_truncate(str(scan), max_chars=240)


def _compact_string_list(values: Any, *, limit: int) -> list[Any]:
    if not isinstance(values, list):
        return []
    compact: list[Any] = []
    for item in values[:limit]:
        if isinstance(item, str):
            compact.append(squeeze_truncate(item, max_chars=160))
        elif isinstance(item, dict):
            compact.append(
                {
                    key: squeeze_truncate(str(value), max_chars=80)
                    for key, value in list(item.items())[:4]
                }
            )
        else:
            compact.append(item)
    return compact


def compact_base_analysis_for_implementation(analysis: dict[str, Any]) -> str:
    if not analysis:
        return "{}"
    logical = analysis.get("logical_plan") if isinstance(analysis.get("logical_plan"), dict) else {}
    scan_order = _compact_scan_order(analysis.get("scan_order"))
    files = analysis.get("files_to_create") or []
    file_paths = []
    for item in files[:6]:
        if isinstance(item, dict):
            file_paths.append(item.get("path") or item.get("file") or str(item)[:80])
        else:
            file_paths.append(str(item))
    payload = {
        "query_summary": squeeze_truncate(str(analysis.get("query_summary") or ""), max_chars=360),
        "execution_strategy": squeeze_truncate(
            str(logical.get("simplest_execution_strategy") or ""), max_chars=280
        ),
        "scan_order": scan_order,
        "likely_bottlenecks": _compact_string_list(analysis.get("likely_bottlenecks"), limit=2),
        "optimization_hypotheses": _compact_string_list(
            analysis.get("optimization_hypotheses"), limit=3
        ),
        "files_to_create": file_paths,
    }
    return compact_json(payload)


def compact_base_analysis_for_optimization(analysis: dict[str, Any]) -> str:
    if not analysis:
        return "{}"
    payload = {
        "likely_bottlenecks": (analysis.get("likely_bottlenecks") or [])[:3],
        "optimization_hypotheses": (analysis.get("optimization_hypotheses") or [])[:4],
    }
    return compact_json(payload)


def compact_bench_summary(summary: dict[str, Any]) -> dict[str, Any]:
    if not summary:
        return {}
    by_freq: dict[str, Any] = {}
    for key, row in (summary.get("by_write_frequency_pct") or {}).items():
        if not isinstance(row, dict):
            continue
        by_freq[key] = {
            "workload_speedup": row.get("workload_speedup"),
            "query_only_speedup": row.get("query_only_speedup"),
            "accel_hit_rate": row.get("accel_hit_rate"),
        }
    return {
        "composite_score": summary.get("composite_score"),
        "composite_workload_speedup": summary.get("composite_workload_speedup"),
        "read_only_query_speedup": summary.get("read_only_query_speedup"),
        "thread_scaling": summary.get("thread_scaling"),
        "by_write_frequency_pct": by_freq,
    }


def compact_gate_summary(gates_dir: Path) -> dict[str, Any]:
    if not gates_dir.is_dir():
        return {}
    summary: dict[str, Any] = {}
    for path in sorted(gates_dir.glob("*.json")):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (json.JSONDecodeError, OSError):
            continue
        if not isinstance(data, dict):  # other artifacts share the directory (the bench's draws are a list)
            continue
        gate = str(data.get("gate") or path.stem)
        entry: dict[str, Any] = {"status": data.get("status")}
        for key in ("composite_score", "composite_workload_speedup", "reason", "exit_code"):
            if key in data and data[key] is not None:
                entry[key] = data[key]
        if data.get("status") == "fail":
            tail = str(data.get("log_tail") or data.get("notes") or "").strip()
            if tail:
                entry["failure_summary"] = tail[-400:]
        summary[gate] = entry
    return summary


def compact_optimization_state(state: dict[str, Any], optimization_iter: int) -> dict[str, Any]:
    if not state:
        return {}
    history = state.get("history") or []
    last = history[-1] if history else {}
    prior = history[-2] if len(history) >= 2 else {}
    compact: dict[str, Any] = {
        "active_candidate_id": state.get("active_candidate_id"),
        "best_composite_score": state.get("best_composite_score"),
        "best_iteration": state.get("best_iteration"),
        "current_iteration": optimization_iter,
    }
    if last:
        compact["last_result"] = {
            "iteration": last.get("iteration"),
            "change_kind": last.get("change_kind"),
            "composite_score": last.get("composite_score"),
            "composite_workload_speedup": last.get("composite_workload_speedup"),
        }
    if prior:
        compact["prior_composite_score"] = prior.get("composite_score")
    return compact


def _slim_candidate(candidate: dict[str, Any]) -> dict[str, Any]:
    if candidate.get("selected") is True:
        return copy.deepcopy(candidate)
    if candidate.get("viability") == "rejected":
        return {
            key: candidate[key]
            for key in (
                "id",
                "design_family",
                "viability",
                "selected",
                "rejection_reason",
                "state_grain",
                "physical_organization",
            )
            if key in candidate
        }
    return {
        key: candidate[key]
        for key in (
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
        if key in candidate
    }


def analysis_for_implementation(analysis: dict[str, Any]) -> dict[str, Any]:
    perf = analysis.get("performance_hypotheses") or {}
    slim_perf: dict[str, Any] = {}
    if isinstance(perf, dict):
        if perf.get("initial_cost_model"):
            slim_perf["initial_cost_model"] = perf["initial_cost_model"]
        if perf.get("likely_best_actions"):
            slim_perf["likely_best_actions"] = perf["likely_best_actions"]
    state_design = (analysis.get("correctness_facts") or {}).get("state_design") or {}
    out: dict[str, Any] = {
        "accelerator_id": analysis.get("accelerator_id"),
        "aux_function_name": analysis.get("aux_function_name"),
        "selected_candidate_id": state_design.get("selected_candidate_id"),
        "correctness_facts": copy.deepcopy(analysis.get("correctness_facts", {})),
        "files_to_create": copy.deepcopy(analysis.get("files_to_create", [])),
    }
    if slim_perf:
        out["performance_hypotheses"] = slim_perf
    return out


def analysis_for_optimization(analysis: dict[str, Any]) -> dict[str, Any]:
    facts = analysis.get("correctness_facts") or {}
    state_design = facts.get("state_design") or {}
    perf = analysis.get("performance_hypotheses") or {}
    alternates: list[dict[str, Any]] = []
    search = analysis.get("state_candidate_search") or {}
    selected_id = str(state_design.get("selected_candidate_id", ""))
    for candidate in search.get("candidates") or []:
        if not isinstance(candidate, dict):
            continue
        cid = str(candidate.get("id", ""))
        if candidate.get("viability") != "viable" or cid == selected_id:
            continue
        alternates.append(_slim_candidate(candidate))
    out: dict[str, Any] = {
        "active_candidate_id": selected_id,
        "cost_model_config": perf.get("initial_cost_model", {}).get("config_path")
        if isinstance(perf.get("initial_cost_model"), dict)
        else None,
        "viable_alternate_candidates": alternates,
        "state_design_summary": {
            key: state_design.get(key)
            for key in (
                "selected_candidate_id",
                "physical_organization",
                "composition_rule",
                "read_path_bottleneck_risk",
            )
            if key in state_design
        },
    }
    initial = perf.get("initial_cost_model")
    if isinstance(initial, dict) and initial.get("config_path"):
        out["cost_model_config"] = initial["config_path"]
    return out


def analysis_for_test_suite(analysis: dict[str, Any]) -> dict[str, Any]:
    facts = analysis.get("correctness_facts") or {}
    return {
        "accelerator_id": analysis.get("accelerator_id"),
        "aux_function_name": analysis.get("aux_function_name"),
        "dependent_tables": copy.deepcopy(facts.get("dependent_tables", [])),
        "partitioning": copy.deepcopy(facts.get("partitioning", {})),
        "mutation_coverage": copy.deepcopy(facts.get("mutation_coverage", {})),
        "legal_deltas": copy.deepcopy(facts.get("legal_deltas", [])),
        "fallback_conditions": copy.deepcopy(facts.get("fallback_conditions", [])),
        "selected_candidate_id": (facts.get("state_design") or {}).get(
            "selected_candidate_id"
        ),
    }


def analysis_for_audit(analysis: dict[str, Any]) -> dict[str, Any]:
    fitness = analysis.get("fitness_assessment") or {}
    facts = analysis.get("correctness_facts") or {}
    state_design = facts.get("state_design") or {}
    return {
        "accelerator_id": analysis.get("accelerator_id"),
        "aux_function_name": analysis.get("aux_function_name"),
        "fitness_recommended": fitness.get("recommended"),
        "selected_candidate_id": state_design.get("selected_candidate_id"),
        "state_design_summary": {
            key: state_design.get(key)
            for key in (
                "selected_candidate_id",
                "physical_organization",
                "composition_rule",
                "read_path_bottleneck_risk",
                "residual_read_algorithm",
            )
            if key in state_design
        },
        "dependent_table_names": [
            str(t.get("name", "")) for t in (facts.get("dependent_tables") or []) if isinstance(t, dict)
        ],
        "mutation_coverage_entries": len(facts.get("mutation_coverage") or []),
        "files_to_create": [
            {"path": item.get("path"), "role": item.get("role")}
            for item in (analysis.get("files_to_create") or [])
            if isinstance(item, dict)
        ],
    }


def stage_analysis_json(analysis: dict[str, Any], stage: str) -> str:
    if not analysis:
        return "{}"
    if stage == "implementation":
        payload = analysis_for_implementation(analysis)
    elif stage == "test_suite":
        payload = analysis_for_test_suite(analysis)
    elif stage == "optimization":
        payload = analysis_for_optimization(analysis)
    elif stage == "audit":
        payload = analysis_for_audit(analysis)
    elif stage == "analysis_critic":
        payload = analysis
    else:
        payload = {
            "note": "Write full analysis.json per schema; read shared docs on demand.",
            "schema_path": "pipeline/schemas/aux_state_analysis.schema.json",
        }
    return compact_json(payload)


def build_optimization_retrieval_brief(
    *,
    query: int,
    analysis: dict[str, Any],
    bench: dict[str, Any],
    opt_state: dict[str, Any],
    experiment_log_tail: str,
    optimization_iter: int,
    gate_summary: dict[str, Any] | None = None,
) -> str:
    opt_ctx = analysis_for_optimization(analysis)
    bench_compact = compact_bench_summary(bench)
    state_compact = compact_optimization_state(opt_state, optimization_iter)
    lines = [
        f"## Retrieval brief (Q{query} optimization iter {optimization_iter})",
        "",
        "### Active design",
        compact_json(opt_ctx.get("state_design_summary", {})),
        "",
        "### Bench sweep (0–50% writes)",
        compact_json(bench_compact),
        "",
        "### Optimization state",
        compact_json(state_compact),
    ]
    if gate_summary:
        lines.extend(["", "### Last official gates", compact_json(gate_summary)])
    lines.extend(["", "### Experiment log (recent)", experiment_log_tail or "(empty)"])
    if opt_ctx.get("viable_alternate_candidates"):
        lines.extend(
            [
                "",
                "### Viable alternates (read full analysis.json for candidate_switch)",
                compact_json(opt_ctx["viable_alternate_candidates"]),
            ]
        )
    return "\n".join(lines)
