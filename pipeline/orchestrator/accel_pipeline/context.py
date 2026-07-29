from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.analysis_context import (
    analysis_for_audit,
    analysis_for_implementation,
    analysis_for_optimization,
    build_optimization_retrieval_brief,
    build_shared_docs_reference,
    compact_bench_summary,
    compact_gate_summary,
    compact_json,
    compact_optimization_state,
    stage_analysis_json,
)
from accel_pipeline.aux_bench import load_bench_summary, load_optimization_state
from accel_pipeline.config import PipelineConfig
from accel_pipeline.paths import pipeline_root, query_artifacts_dir

_AUX_SHARED = Path("prompts") / "aux_state" / "shared"


def _read_text(path: Path, default: str = "") -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else default


def _tail_lines(path: Path, n: int = 3) -> str:
    if not path.is_file():
        return ""
    lines = path.read_text(encoding="utf-8").strip().splitlines()
    return "\n".join(lines[-n:])


def build_retrieval_brief(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    *,
    stage: str = "",
    optimization_iter: int = 0,
) -> str:
    repo = pipeline_config.repo_root
    art = query_artifacts_dir(repo, query)
    analysis_path = art / "analysis.json"
    analysis: dict[str, Any] = {}
    if analysis_path.is_file():
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))

    if workload.get("workload_kind") == "aux_state" and stage == "optimization":
        return build_optimization_retrieval_brief(
            query=query,
            analysis=analysis,
            bench=load_bench_summary(repo, query),
            opt_state=load_optimization_state(repo, query),
            experiment_log_tail=_tail_lines(art / "experiment_log.md", 5),
            optimization_iter=optimization_iter,
            gate_summary=compact_gate_summary(art / "gates"),
        )

    if workload.get("workload_kind") == "aux_state":
        fitness = analysis.get("fitness_assessment", {})
        lines = [
            "## Retrieval brief (aux-state)",
            f"Query: Q{query}",
            f"Fitness recommended: {fitness.get('recommended')}",
            f"Selected candidate: {analysis.get('correctness_facts', {}).get('state_design', {}).get('selected_candidate_id', '')}",
            "",
            "### Decision log excerpt",
            _tail_lines(art / "experiment_log.md", 5),
        ]
        return "\n".join(lines)

    bottlenecks = analysis.get("likely_bottlenecks", [])
    playbook_path = pipeline_root(repo) / "artifacts" / "shared" / "optimization_playbook.md"
    playbook_excerpt = _read_text(playbook_path)[:2000]

    handbook_path = pipeline_root(repo) / "prompts" / "shared" / "handbook.md"
    handbook_excerpt = _read_text(handbook_path)[:2500]

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
    reference_sql_path = ""
    ref_path = workload.get("reference_sql_path")
    if ref_path:
        reference_sql_path = str(ref_path)
        if stage == "analysis" and Path(ref_path).is_file():
            reference_sql = Path(ref_path).read_text(encoding="utf-8")

    import yaml

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

    analysis: dict[str, Any] = {}
    analysis_path = art / "analysis.json"
    if analysis_path.is_file():
        analysis = json.loads(analysis_path.read_text(encoding="utf-8"))

    shared_docs = build_shared_docs_reference("pipeline", stage) if is_aux else ""
    bench_summary = load_bench_summary(repo, query) if is_aux else {}
    opt_state = load_optimization_state(repo, query) if is_aux else {}

    if is_aux and stage == "analysis":
        workload_reference_section = "\n".join(
            [
                "## Query",
                "",
                "```sql",
                reference_sql,
                "```",
                "",
                "## Parameters",
                "",
                yaml.safe_dump(workload.get("parameters", [])),
                "",
                "## Dependent tables (hints only)",
                "",
                yaml.safe_dump(workload.get("dependent_tables", [])),
                "",
                "## Partitioning (hints only)",
                "",
                yaml.safe_dump(workload.get("partitioning", {})),
            ]
        )
    elif is_aux:
        workload_reference_section = (
            f"Workload SQL: `{reference_sql_path or 'see workload YAML'}` · "
            "Parameters/tables/partitioning: see `analysis.json` correctness_facts."
        )
    else:
        workload_reference_section = ""

    ctx: dict[str, str] = {
        "query_number": str(query),
        "repo_root": str(repo),
        "duckdb_root": str(pipeline_config.repo_root.parent / "accelerator-codegen-duckdb"),
        "reference_sql": reference_sql,
        "reference_sql_path": reference_sql_path,
        "workload_reference_section": workload_reference_section,
        "parameters_yaml": yaml.safe_dump(workload.get("parameters", [])) if stage == "analysis" else "",
        "output_schema_yaml": yaml.safe_dump(workload.get("output_schema", [])) if stage == "analysis" else "",
        "dependent_tables_yaml": yaml.safe_dump(workload.get("dependent_tables", [])) if stage == "analysis" else "",
        "partitioning_yaml": yaml.safe_dump(workload.get("partitioning", {})) if stage == "analysis" else "",
        "reference_accelerators": ", ".join(str(x) for x in refs),
        "optional_reference_accelerators": optional_reference_accelerators,
        "accelerator_id": str(workload.get("accelerator_id", "")),
        "base_function_name": str(workload.get("base_function_name", "")),
        "aux_function_name": str(workload.get("aux_function_name", "")),
        "cost_model_config": str(workload.get("cost_model_config", "")),
        "shared_docs_reference": shared_docs,
        "agent_boundary": (
            "See `pipeline/prompts/aux_state/shared/agent_boundary.md` (read_file if needed)."
            if is_aux
            else ""
        ),
        "constraints": (
            "See `pipeline/prompts/aux_state/shared/constraints.md` (read_file if needed)."
            if is_aux
            else _read_text(constraints_path)
        ),
        "profiling_conventions": (
            "See `pipeline/prompts/shared/profiling_conventions.md` (read_file if needed)."
            if is_aux
            else _read_text(pipe / "prompts" / "shared" / "profiling_conventions.md")
        ),
        "analysis_json": stage_analysis_json(analysis, stage) if is_aux else _read_text(analysis_path, "{}"),
        "implementation_context_json": compact_json(analysis_for_implementation(analysis))
        if is_aux and analysis
        else "{}",
        "optimization_context_json": compact_json(analysis_for_optimization(analysis))
        if is_aux and analysis
        else "{}",
        "audit_context_json": compact_json(analysis_for_audit(analysis))
        if is_aux and analysis
        else "{}",
        "baseline_summary_json": _read_text(art / "baseline_summary.json", "{}"),
        "audit_result_json": _read_text(art / "audit_result.json", "{}"),
        "retrieval_brief": build_retrieval_brief(
            pipeline_config,
            workload,
            query,
            stage=stage,
            optimization_iter=optimization_iter,
        ),
        "experiment_log_excerpt": _tail_lines(art / "experiment_log.md", 5),
        "experiment_summary": _tail_lines(art / "experiment_log.md", 8),
        "handbook_excerpt": "",
        "optimization_iter": str(optimization_iter),
        "aux_state_fitness": "",
        "aux_state_design": "",
        "mutation_coverage": "",
        "substrate_contract": "",
        "maintenance_framework": "",
        "module_layout": "",
        "registration_checklist": "",
        "runtime_policy": "",
        "aux_bench_summary_json": compact_json(
            compact_bench_summary(bench_summary) if stage == "optimization" else bench_summary
        )
        if is_aux
        else "{}",
        "optimization_state_json": compact_json(
            compact_optimization_state(opt_state, optimization_iter)
            if stage == "optimization"
            else opt_state
        )
        if is_aux
        else "{}",
        "full_analysis_path": (
            str(analysis_path.relative_to(repo))
            if analysis_path.is_file()
            else f"pipeline/artifacts/q{query}/analysis.json"
        ),
        "analysis_revision_request": "",
    }
    return ctx
