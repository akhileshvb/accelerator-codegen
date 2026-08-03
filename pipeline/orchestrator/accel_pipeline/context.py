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
    compact_base_analysis_for_implementation,
    compact_base_analysis_for_optimization,
    compact_bench_summary,
    compact_gate_summary,
    compact_json,
    compact_optimization_state,
    stage_analysis_json,
)
from accel_pipeline.aux_bench import load_bench_summary, load_optimization_state
from accel_pipeline.base_bench import (
    compact_base_optimization_state,
    compact_summary_for_prompt,
    load_base_optimization_state,
)
from accel_pipeline.config import PipelineConfig, resolve_workload_paths
from accel_pipeline.kernel_inspiration import format_kernel_inspiration_section
from accel_pipeline.optimization_runtime import runtime_path_for_prompt
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
    art = query_artifacts_dir(repo, query, workload=workload)
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
    hypotheses = analysis.get("optimization_hypotheses", [])

    lines = [
        "## Retrieval brief",
        f"Query: Q{query}",
        f"Bottlenecks: {json.dumps(bottlenecks)}",
        f"Hypotheses: {json.dumps(hypotheses[:5])}",
        "",
        "Full analysis: read `analysis.json` in the query artifacts directory on demand.",
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
    art = query_artifacts_dir(repo, query, workload=workload)
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
    opt_state = load_optimization_state(repo, query) if is_aux else load_base_optimization_state(art)

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

    paths = resolve_workload_paths(pipeline_config, workload, query)
    benchmark = str(workload.get("benchmark_name", "tpch"))
    artifacts_rel = f"pipeline/artifacts/{workload.get('artifact_namespace', benchmark)}/q{query}"

    schema_ref = workload.get("schema_reference_path", "")
    if schema_ref and Path(schema_ref).is_file():
        rel = Path(schema_ref)
        try:
            rel = rel.relative_to(repo)
        except ValueError:
            pass
        schema_reference_section = (
            "## Schema reference\n\n"
            f"Read `{rel}` for table/column layout."
        )
    else:
        schema_reference_section = ""

    if benchmark == "sec_edgar":
        benchmark_notes = (
            "- SEC-EDGAR queries use **embedded literals** (no bind parameters); table functions may be zero-arg.\n"
            "- Correctness requires **full multi-row ordered result match**, not a single scalar.\n"
            "- Benchmark/profile use **one scale factor** (SF3); size structures from bind-time `row_estimate`, "
            "not hardcoded row/distinct counts from the current profile."
        )
        if is_aux:
            benchmark_notes += (
                f"\n- SEC EDGAR aux state lives under `src/aux_state/sec_edgar/q{query}/` with headers "
                f"`sec_edgar_q{query}_*.hpp` (not `tpch_q*` paths).\n"
                "- Write workload suite is **`filing_append`** (new `pre`/`num` rows), not TPC-H `lineitem` point updates.\n"
                "- Partition by filing semantics (`stmt`, `adsh`, `cik`+`fy`) — see `pipeline/prompts/sec_edgar/aux_state_notes.md`.\n"
                "- Register with `accel_add_aux_state_query(BENCHMARK sec_edgar QUERY_NUM N ...)` in CMakeLists.txt."
            )
        optimization_gate_suffix = " → profile"
        audit_gate_suffix = ""
    else:
        benchmark_notes = ""
        optimization_gate_suffix = " → sf10 (if state-sizing) → profile"
        audit_gate_suffix = " and `sf10`"

    lean_stage = stage in ("optimization", "crash_debug", "implementation", "audit")
    compact_inspiration = stage in ("optimization", "crash_debug", "implementation", "analysis")
    kernel_inspiration_section = ""
    if stage in ("implementation", "optimization", "crash_debug", "analysis"):
        kernel_inspiration_section = format_kernel_inspiration_section(
            repo,
            workload_refs=refs,
            stage=stage,
            compact=compact_inspiration,
        )

    profile_summary_path = art / "profile_summary.json"
    profile_data: dict[str, Any] = {}
    if profile_summary_path.is_file():
        try:
            profile_data = json.loads(profile_summary_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            profile_data = {}
    baseline_data: dict[str, Any] = {}
    baseline_summary_path = art / "baseline_summary.json"
    if baseline_summary_path.is_file():
        try:
            baseline_data = json.loads(baseline_summary_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            baseline_data = {}

    if lean_stage:
        profile_summary_json = compact_summary_for_prompt(profile_data)
        baseline_summary_json = compact_summary_for_prompt(baseline_data)
    else:
        profile_summary_json = profile_summary_path.read_text(encoding="utf-8") if profile_summary_path.is_file() else "{}"
        baseline_summary_json = (
            baseline_summary_path.read_text(encoding="utf-8") if baseline_summary_path.is_file() else "{}"
        )
    optimization_feedback = _read_text(art / "optimization_feedback.md", "(no gate feedback yet — first optimization iter uses implementation gates)")

    base_kernel_opt_path = pipe / "prompts" / "shared" / "base_kernel_optimization.md"
    if lean_stage:
        if stage == "optimization":
            base_kernel_optimization = ""
        else:
            base_kernel_optimization = (
                "See `pipeline/prompts/shared/base_kernel_optimization.md` (read_file if needed)."
            )
        constraints_text = "See `pipeline/prompts/shared/constraints.md` (read_file if needed)."
        profiling_text = "See `pipeline/prompts/shared/profiling_conventions.md` (read_file if needed)."
    else:
        base_kernel_optimization = _read_text(base_kernel_opt_path)
        constraints_text = _read_text(constraints_path)
        profiling_text = _read_text(pipe / "prompts" / "shared" / "profiling_conventions.md")

    if is_aux:
        analysis_json = stage_analysis_json(analysis, stage)
    elif stage == "implementation":
        analysis_json = compact_base_analysis_for_implementation(analysis)
    elif stage == "optimization":
        analysis_json = compact_base_analysis_for_optimization(analysis)
    else:
        analysis_json = _read_text(analysis_path, "{}") if stage == "analysis" else "{}"

    retrieval_brief = ""
    if stage == "analysis":
        retrieval_brief = build_retrieval_brief(
            pipeline_config, workload, query, stage=stage, optimization_iter=optimization_iter
        )

    optimization_runtime_path = ""
    if stage == "optimization":
        optimization_runtime_path = runtime_path_for_prompt(pipeline_config, workload, query)

    ctx: dict[str, str] = {
        "query_number": str(query),
        "benchmark_name": benchmark,
        "display_name": str(workload.get("display_name", benchmark)),
        "function_name": paths["function_name"],
        "create_fn": paths["create_fn"],
        "source_subdir": paths["source_subdir"],
        "header_path": paths["header_path"],
        "kernel_path": paths["kernel_path"],
        "package_path": paths["package_path"],
        "test_path": paths["test_path"],
        "benchmark_path": paths["benchmark_path"],
        "test_target": paths["test_target"],
        "bench_target": paths["bench_target"],
        "ctest_smoke_regex": paths["ctest_smoke_regex"],
        "dataset_db": paths["dataset_db"],
        "db_env_var": paths["db_env_var"],
        "artifacts_path": artifacts_rel,
        "schema_reference_section": schema_reference_section,
        "benchmark_notes": benchmark_notes,
        "kernel_inspiration_section": kernel_inspiration_section,
        "fused_scan_path": paths["fused_scan_path"],
        "fused_header_path": paths["fused_header_path"],
        "optimization_gate_suffix": optimization_gate_suffix,
        "audit_gate_suffix": audit_gate_suffix,
        "test_template_path": str(workload.get("test_template_path", "")),
        "benchmark_template_path": str(workload.get("benchmark_template_path", "")),
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
            else constraints_text
        ),
        "profiling_conventions": (
            "See `pipeline/prompts/shared/profiling_conventions.md` (read_file if needed)."
            if is_aux or lean_stage
            else profiling_text
        ),
        "analysis_json": analysis_json,
        "implementation_context_json": compact_json(analysis_for_implementation(analysis))
        if is_aux and analysis
        else "{}",
        "optimization_context_json": compact_json(analysis_for_optimization(analysis))
        if is_aux and analysis
        else "{}",
        "audit_context_json": compact_json(analysis_for_audit(analysis))
        if is_aux and analysis
        else "{}",
        "optimization_runtime_path": optimization_runtime_path,
        "baseline_summary_json": baseline_summary_json if stage != "optimization" else "(see optimization_runtime.md)",
        "profile_summary_json": profile_summary_json if stage != "optimization" else "(see optimization_runtime.md)",
        "optimization_feedback": (
            "(gates write optimization_feedback.md for humans; metrics live in runtime after optimization)"
            if stage == "implementation"
            else "(see optimization_runtime.md)"
            if stage == "optimization"
            else "(see optimization_runtime.md or experiment_log.md)"
            if lean_stage
            else optimization_feedback
        ),
        "optimization_state_json": (
            "(see optimization_runtime.md)"
            if stage == "optimization" and not is_aux
            else compact_json(
                compact_optimization_state(opt_state, optimization_iter)
                if is_aux and stage == "optimization"
                else compact_base_optimization_state(opt_state, optimization_iter)
                if stage == "optimization"
                else opt_state
            )
        ),
        "base_kernel_optimization": base_kernel_optimization,
        "optimization_max_iterations": str(pipeline_config.optimization_max_iterations),
        "optimization_goal": (
            "Beat DuckDB (`speedup_median` > 1.0) and keep improving `best_speedup_median` — "
            "there is no fixed speedup ceiling; stop only when gates stall progress."
        ),
        "audit_result_json": _read_text(art / "audit_result.json", "{}"),
        "retrieval_brief": retrieval_brief,
        "experiment_log_excerpt": _tail_lines(art / "experiment_log.md", 2 if lean_stage else 5),
        "experiment_summary": _tail_lines(art / "experiment_log.md", 2 if lean_stage else 8),
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
        "full_analysis_path": (
            str(analysis_path.relative_to(repo))
            if analysis_path.is_file()
            else f"{artifacts_rel}/analysis.json"
        ),
        "analysis_revision_request": "",
    }
    return ctx
