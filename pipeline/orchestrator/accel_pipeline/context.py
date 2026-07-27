from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.aux_bench import list_viable_candidate_ids, load_bench_summary, load_optimization_state
from accel_pipeline.config import PipelineConfig
from accel_pipeline.paths import pipeline_root, query_artifacts_dir

_AUX_SHARED = Path("prompts") / "aux_state" / "shared"


def _read_text(path: Path, default: str = "") -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else default


def _read_aux_shared(pipe: Path, name: str) -> str:
    return _read_text(pipe / _AUX_SHARED / name)


def _tail_lines(path: Path, n: int = 3) -> str:
    if not path.is_file():
        return ""
    lines = path.read_text(encoding="utf-8").strip().splitlines()
    return "\n".join(lines[-n:])


def build_retrieval_brief(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> str:
    repo = pipeline_config.repo_root
    art = query_artifacts_dir(repo, query)
    analysis_path = art / "analysis.json"
    analysis: dict[str, Any] = {}
    if analysis_path.is_file():
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))

    if workload.get("workload_kind") == "aux_state":
        fitness = analysis.get("fitness_assessment", {})
        perf = analysis.get("performance_hypotheses", {})
        bench = load_bench_summary(repo, query)
        opt_state = load_optimization_state(repo, query)
        viable = list_viable_candidate_ids(analysis)
        lines = [
            "## Retrieval brief (aux-state)",
            f"Query: Q{query}",
            f"Fitness signals: {json.dumps(fitness.get('signals_for', []))}",
            f"Likely actions: {json.dumps(perf.get('likely_best_actions', []))}",
            f"Active candidate: {opt_state.get('active_candidate_id', analysis.get('correctness_facts', {}).get('state_design', {}).get('selected_candidate_id', ''))}",
            f"Viable alternate candidates: {json.dumps(viable)}",
            "",
            "### Bench sweep signal (write freq 0–50%)",
            json.dumps(
                {
                    "composite_score": bench.get("composite_score"),
                    "composite_workload_speedup": bench.get("composite_workload_speedup"),
                    "read_only_query_speedup": bench.get("read_only_query_speedup"),
                    "by_write_frequency_pct": bench.get("by_write_frequency_pct", {}),
                },
                indent=2,
            ),
            "",
            "### Optimization state",
            json.dumps(
                {
                    "best_composite_score": opt_state.get("best_composite_score"),
                    "best_iteration": opt_state.get("best_iteration"),
                    "history_tail": (opt_state.get("history") or [])[-3:],
                },
                indent=2,
            ),
            "",
            "### Decision log excerpt",
            _tail_lines(art / "experiment_log.md", 5),
        ]
        return "\n".join(lines)

    bottlenecks = analysis.get("likely_bottlenecks", [])
    playbook_path = pipeline_root(repo) / "artifacts" / "shared" / "optimization_playbook.md"
    playbook_excerpt = _read_text(playbook_path)[:2000]

    handbook_path = pipeline_root(repo) / "prompts" / "shared" / "handbook.md"
    handbook = _read_text(handbook_path)
    handbook_excerpt = handbook[:2500]

    lines = [
        "## Retrieval brief",
        f"Query: Q{query}",
        f"Bottlenecks: {json.dumps(bottlenecks)}",
        "",
        "### Playbook excerpt",
        playbook_excerpt,
        "",
        "### Handbook excerpt",
        handbook_excerpt,
    ]
    return "\n".join(lines)


def build_prompt_context(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    stage: str,
    optimization_iter: int = 0,
) -> dict[str, str]:
    repo = pipeline_config.repo_root
    pipe = pipeline_config.pipeline_root
    art = query_artifacts_dir(repo, query)
    is_aux = workload.get("workload_kind") == "aux_state"

    reference_sql = ""
    ref_path = workload.get("reference_sql_path")
    if ref_path and Path(ref_path).is_file():
        reference_sql = Path(ref_path).read_text(encoding="utf-8")

    import yaml

    agent_boundary_path = workload.get("agent_boundary")
    agent_boundary = ""
    if agent_boundary_path:
        boundary_file = pipe / agent_boundary_path
        if not boundary_file.is_file():
            boundary_file = pipe / _AUX_SHARED / "agent_boundary.md"
        agent_boundary = _read_text(boundary_file)

    refs = workload.get("reference_accelerators", [])
    if refs:
        optional_reference_accelerators = (
            "## Optional reference accelerators\n\n"
            f"Inspect if helpful (not required): {', '.join(str(x) for x in refs)}"
        )
    else:
        optional_reference_accelerators = (
            "## Reference accelerators\n\n"
            "None configured. Design from SQL, workload YAML, and shared pattern docs only."
        )

    constraints_path = (
        pipe / "prompts" / "aux_state" / "shared" / "constraints.md"
        if is_aux
        else pipe / "prompts" / "shared" / "constraints.md"
    )

    ctx: dict[str, str] = {
        "query_number": str(query),
        "repo_root": str(repo),
        "duckdb_root": str(pipeline_config.repo_root.parent / "accelerator-codegen-duckdb"),
        "reference_sql": reference_sql,
        "parameters_yaml": yaml.safe_dump(workload.get("parameters", [])),
        "output_schema_yaml": yaml.safe_dump(workload.get("output_schema", [])),
        "dependent_tables_yaml": yaml.safe_dump(workload.get("dependent_tables", [])),
        "partitioning_yaml": yaml.safe_dump(workload.get("partitioning", {})),
        "reference_accelerators": ", ".join(str(x) for x in refs),
        "optional_reference_accelerators": optional_reference_accelerators,
        "accelerator_id": str(workload.get("accelerator_id", "")),
        "base_function_name": str(workload.get("base_function_name", "")),
        "aux_function_name": str(workload.get("aux_function_name", "")),
        "cost_model_config": str(workload.get("cost_model_config", "")),
        "agent_boundary": agent_boundary,
        "constraints": _read_text(constraints_path),
        "profiling_conventions": _read_text(pipe / "prompts" / "shared" / "profiling_conventions.md"),
        "analysis_json": _read_text(art / "analysis.json", "{}"),
        "baseline_summary_json": _read_text(art / "baseline_summary.json", "{}"),
        "audit_result_json": _read_text(art / "audit_result.json", "{}"),
        "retrieval_brief": build_retrieval_brief(pipeline_config, workload, query),
        "experiment_log_excerpt": _tail_lines(art / "experiment_log.md", 5),
        "experiment_summary": _read_text(art / "experiment_log.md")[:3000],
        "handbook_excerpt": _read_text(pipe / "prompts" / "shared" / "handbook.md")[:2500],
        "optimization_iter": str(optimization_iter),
        "aux_state_fitness": _read_aux_shared(pipe, "aux_state_fitness.md") if is_aux else "",
        "aux_state_design": _read_aux_shared(pipe, "aux_state_design.md") if is_aux else "",
        "mutation_coverage": _read_aux_shared(pipe, "mutation_coverage.md") if is_aux else "",
        "substrate_contract": _read_aux_shared(pipe, "substrate_contract.md") if is_aux else "",
        "maintenance_framework": _read_aux_shared(pipe, "maintenance_framework.md") if is_aux else "",
        "module_layout": _read_aux_shared(pipe, "module_layout.md") if is_aux else "",
        "registration_checklist": _read_aux_shared(pipe, "registration_checklist.md") if is_aux else "",
        "runtime_policy": _read_aux_shared(pipe, "runtime_policy.md") if is_aux else "",
        "aux_bench_summary_json": json.dumps(load_bench_summary(repo, query), indent=2) if is_aux else "{}",
        "optimization_state_json": json.dumps(load_optimization_state(repo, query), indent=2) if is_aux else "{}",
    }
    return ctx
