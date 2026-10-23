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
from accel_pipeline.backend_api import render_accelerator_engine_profile, render_backend_api
from accel_pipeline.base_bench import (
    compact_base_optimization_state,
    compact_summary_for_prompt,
    load_base_optimization_state,
)
from accel_pipeline.config import PipelineConfig, resolve_optimization_edit_paths, resolve_workload_paths
from accel_pipeline.engine_host import format_host_block
from accel_pipeline.engine_profile import render_engine_profile
from accel_pipeline.implementation_bootstrap import build_implementation_bootstrap
from accel_pipeline.models import is_critique_stage, is_implementation_stage
from accel_pipeline.optimization_runtime import runtime_path_for_prompt
from accel_pipeline.workload_facts import with_facts
from accel_pipeline.paths import (
    load_query_analysis,
    pipeline_root,
    query_artifacts_dir,
)

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
    analysis_path, analysis = load_query_analysis(repo, query, workload=workload)

    if workload.get("workload_kind") == "aux_state" and stage in (
        "optimization",
        "optimization_unified",
        "optimization_worker",
        "optimization_fix",
    ):
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
        f"Full analysis: read `{analysis_path.name}` in the query artifacts directory on demand.",
    ]
    return "\n".join(lines)


def _accelerator_engine_host_block(workload: dict[str, Any], repo_root: Path, build_dir: Path) -> str:
    """Host registration / differential conventions for query-accelerator workloads."""
    from accel_pipeline.config import engine_host_conventions, sanitize_engine_name

    raw = workload.get("engine_name")
    engine = sanitize_engine_name(str(raw) if raw else "")
    host = dict(workload.get("engine_host") or engine_host_conventions(engine, repo_root=repo_root))
    lines = [
        "### Engine host (query accelerator)",
        "",
        f"- **engine_name:** `{engine}`",
        f"- **Registration / SQL surface:** {host.get('sql_entrypoint_convention', '')}",
        f"- **Register path (convention):** `{host.get('register_path', '')}`",
    ]
    aux_reg = str(host.get("aux_register_path") or "").strip()
    if aux_reg:
        lines.append(f"- **Aux register path:** `{aux_reg}`")
    registry = str(host.get("registry") or "").strip()
    if registry:
        lines.append(
            f"- **Registration:** this query registers itself from its own directory: {registry}. "
            "The engine backend (backends/<engine>/**, incl. its catalog in backends/<engine>/api/catalog/) "
            "names no query and is owned by the engine_backend run; never add per-query code or lists there."
        )
    lines.extend(
        [
            f"- **Dataset env:** `{host.get('dataset_env', 'ACCEL_TPCH_DB')}`",
            f"- **Differential:** `{host.get('differential_binary', '')}` — "
            f"{host.get('differential_command', '')}",
        ]
    )
    hint = str(host.get("query_host_ready_hint") or "").strip()
    if hint:
        lines.extend(["", f"**Host readiness:** {hint}"])
    lines.extend(["", format_host_block(workload, repo_root, build_dir)])
    return "\n".join(lines)


def _engine_host_discovery_block(workload: dict[str, Any], repo_root: Path, build_dir: Path) -> str:
    return format_host_block(workload, repo_root, build_dir)


def _engine_profile_for_stage(workload: dict[str, Any], stage: str) -> str:
    """The engine profile sections this stage sees (impl_<layer> / critique_<layer> use the layer's)."""
    layer = stage.split("_", 1)[1] if stage.startswith(("impl_", "critique_")) else None
    sections = workload["engine_profile_sections"][layer or stage]
    return render_engine_profile(
        workload["engine_profile"],
        sections,
        layer=layer,
        host_layer_runs=bool(workload.get("requires_in_process_host", False)),
    )


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
    if workload.get("workload_kind") != "engine_backend":
        workload = with_facts(repo, workload, query)

    reference_sql = ""
    reference_sql_path = ""
    ref_path = workload.get("reference_sql_path")
    if ref_path:
        reference_sql_path = str(ref_path)
        if stage == "analysis" and Path(ref_path).is_file():
            reference_sql = Path(ref_path).read_text(encoding="utf-8")

    import yaml

    constraints_path = (
        pipe / "prompts" / "aux_state" / "shared" / "constraints.md"
        if is_aux
        else pipe / "prompts" / "shared" / "constraints.md"
    )

    art = query_artifacts_dir(repo, query, workload=workload)
    analysis_path, analysis = load_query_analysis(repo, query, workload=workload)

    shared_docs = build_shared_docs_reference("pipeline", stage) if is_aux else ""
    bench_summary = load_bench_summary(repo, query) if is_aux else {}
    opt_state = load_optimization_state(repo, query) if is_aux else load_base_optimization_state(art)

    paths = resolve_workload_paths(pipeline_config, workload, query)

    if is_aux and stage == "analysis":
        from accel_pipeline.data_shape import build_data_shape_markdown

        if not reference_sql.strip():
            reference_sql = (
                f"-- MISSING reference SQL at {reference_sql_path or '(unset)'}\n"
                "-- Regenerate with: python3 scripts/generate_tpch_sf1_instances.py"
            )
        data_shape_section = build_data_shape_markdown(
            workload, paths.get("dataset_db")
        )
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
                "",
                data_shape_section,
            ]
        )
    elif is_aux:
        workload_reference_section = (
            f"Workload SQL: `{reference_sql_path or 'see workload YAML'}` · "
            "Parameters/tables/partitioning: see `analysis.json` correctness_facts."
        )
    else:
        workload_reference_section = ""
    edit_paths = resolve_optimization_edit_paths(pipeline_config, workload, query)
    is_engine = workload.get("workload_kind") == "engine_backend"
    from accel_pipeline.config import (
        engine_host_conventions,
        engine_pascal_name,
        sanitize_engine_name,
    )

    raw_engine = workload.get("engine_name")
    if is_engine:
        engine_name = str(raw_engine or "")
    else:
        engine_name = sanitize_engine_name(str(raw_engine) if raw_engine else "")
    engine_pascal = engine_pascal_name(engine_name) if engine_name else ""
    engine_host = dict(
        workload.get("engine_host")
        or (engine_host_conventions(engine_name, repo_root=repo) if engine_name else {})
    )
    opt_stages = (
        "optimization",
        "optimization_unified",
        "optimization_worker",
        "optimization_fix",
        "optimization_unified_hypothesis",
        "optimization_unified_patch",
    )
    prompt_kernel_path = paths["kernel_path"]
    prompt_header_path = paths["header_path"]
    prompt_package_path = paths["package_path"]
    optimization_edit_paths = ""
    if is_aux and stage in opt_stages:
        prompt_kernel_path = edit_paths["primary_cpp"]
        prompt_header_path = edit_paths["header"]
        prompt_package_path = edit_paths["secondary_cpp"]
        optimization_edit_paths = "\n".join(f"- `{p}`" for p in edit_paths["all_paths"])
        if edit_paths.get("base_kernel_path"):
            optimization_edit_paths += (
                f"\n- `{edit_paths['base_kernel_path']}` (base kernel — **read-only**, do not edit)"
            )
    elif stage in opt_stages:
        optimization_edit_paths = "\n".join(
            f"- `{p}`"
            for p in (prompt_header_path, prompt_kernel_path, prompt_package_path)
        )
    benchmark = str(workload.get("benchmark_name", "tpch"))
    artifacts_rel = str(art.relative_to(repo))
    analysis_artifact_rel = f"{artifacts_rel}/{'backend_analysis.json' if is_engine else analysis_path.name}"

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
                f"\n- SEC EDGAR aux state: `accelerators/sec_edgar/q{query}/{engine_name}/aux/module.hpp` "
                f"(base API: `accelerators/sec_edgar/q{query}/{engine_name}/base/query.hpp`).\n"
                "- Write workload suite is **`filing_append`** / **`value_append`**, not TPC-H `lineitem` point updates.\n"
                "- Partition by filing semantics (`stmt`, `adsh`, `cik`+`fy`) — see `pipeline/prompts/sec_edgar/aux_state_notes.md`.\n"
                "- The aux state is discovered from `aux/finalize.cpp`; no CMake registration.\n"
                "- Scale: SF3 is both iteration and production scale here — still size from `row_estimate`, not profile snapshots."
            )
        optimization_gate_suffix = " → profile"
        audit_gate_suffix = ""
    else:
        benchmark_notes = (
            "- **Iteration scale (SF1)**: correctness, baseline, profile, and A/B benchmarks run on "
            f"`{paths['dataset_db']}` for fast feedback.\n"
            "- **Scale check (SF10)**: `sf10` gate runs on `scale_db` when configured — catches sizing "
            "and state blowups at ~10× rows.\n"
            "- **Production target (SF20+)**: final benchmarking uses much larger scale factors. "
            "Designs must scale: bind-time `row_estimate` sizing, no SF1-hardcoded capacities, "
            "bounded aux/read paths. See `pipeline/prompts/shared/scale_factor_contract.md`.\n"
            "- Do not optimize for SF1 micro-wins that add full-domain scans or cardinality constants "
            "from the iteration dataset."
        )
        optimization_gate_suffix = " → sf10 (scale check) → profile"
        audit_gate_suffix = " and `sf10`"

    if is_aux and stage in opt_stages:
        benchmark_notes += (
            "\n- **Dual-axis accept/reject score** (orchestrator-owned):\n"
            "  `optimization_score = read_only_query_speedup^0.75 * write_mix_workload_speedup^0.25` "
            "(read weighs more; write-mix still gates accepts).\n"
            "  Quick bench always includes **0% reads** plus **randomly sampled write %s** "
            "(re-sampled each attempt; locked only within an attempt — see "
            "`optimization_write_frequencies.json`). "
            "Write-mix score is the **geometric mean** of non-zero write-frequency "
            "`workload_speedup`s — do not overfit a single mix.\n"
            "- **Read axis**: be aggressive on the hit path (eliminate residual scans, dense layouts, "
            "fuse/decode).\n"
            "- **Write/maintenance axis**: prefer **ApplyDelta / partition-local** apply; "
            "**RebuildAll-on-every-point-write is a maintenance cliff**. Raising write-mix "
            "`workload_speedup` across sampled frequencies raises the accept score even when read is flat.\n"
            "- When read is already strong and write-mix workload ≪ 1×, **next hypothesis must attack "
            "incremental maintenance**, not ≤5% read micro-opts.\n"
            "- Do not trade a big write-mix regression for a small read win. End-of-iteration fair "
            "sweep still covers 0/1/10/50%.\n"
            "- **Scale**: per-query and per-write cost must stay O(touched rows/partitions) or better; "
            "size from bind-time `row_estimate` — never SF1-hardcoded capacities."
        )

    lean_stage = stage in (
        "optimization",
        "optimization_unified",
        "optimization_worker",
        "optimization_fix",
        "optimization_unified_hypothesis",
        "optimization_unified_patch",
        "crash_debug",
        "implementation",
        "adversarial_tests",
        "audit",
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
                "See `pipeline/prompts/shared/base_kernel_optimization.md` (read it if needed)."
            )
        constraints_text = (
            "See `pipeline/prompts/shared/hard_guidelines.md`, `constraints.md`, "
            "`generated_layout.md`, and `scale_factor_contract.md` (read it if needed)."
        )
        profiling_text = "See `pipeline/prompts/shared/profiling_conventions.md` (read it if needed)."
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
    if stage in (
        "analysis",
        "optimization",
        "optimization_unified",
        "optimization_worker",
    ):
        retrieval_brief = build_retrieval_brief(
            pipeline_config, workload, query, stage=stage, optimization_iter=optimization_iter
        )

    hard_guidelines_path = pipe / "prompts" / "shared" / "hard_guidelines.md"
    hard_guidelines = (
        _read_text(hard_guidelines_path)
        if is_implementation_stage(stage)
        or is_critique_stage(stage)
        or stage
        in (
            "analysis",
            "optimization",
            "optimization_unified",
            "optimization_unified_hypothesis",
            "optimization_unified_patch",
            "optimization_worker",
            "optimization_fix",
            "audit",
            "architecture_audit",
            "validation",
        )
        else ""
    )
    # Injected for Codex exec stages; empty for other providers unless prompt keeps the placeholder.
    codex_tooling = ""
    if is_implementation_stage(stage) or is_critique_stage(stage) or stage in (
        "validation",
        "adversarial_tests",
        "architecture_audit",
        "optimization_unified",
        "optimization_unified_hypothesis",
        "optimization_unified_patch",
        "optimization_worker",
        "optimization_fix",
    ):
        codex_tooling = (
            "When running under Codex exec, follow `pipeline/prompts/shared/codex_tooling.md` "
            "(native edit/search/shell + MCP profile/gate/A/B tools)."
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
        "header_path": prompt_header_path,
        "kernel_path": prompt_kernel_path,
        "package_path": prompt_package_path,
        "optimization_edit_paths": optimization_edit_paths,
        "test_path": paths["test_path"],
        "benchmark_path": paths["benchmark_path"],
        "test_target": paths["test_target"],
        "bench_target": paths["bench_target"],
        "ctest_smoke_regex": paths["ctest_smoke_regex"],
        "dataset_db": paths["dataset_db"],
        "db_env_var": paths["db_env_var"],
        "artifacts_path": artifacts_rel,
        "bootstrap_artifacts_path": f"pipeline/artifacts/engines/{engine_name}/host_bootstrap"
        if engine_name
        else "pipeline/artifacts/engines/host_bootstrap",
        "analysis_artifact_path": analysis_artifact_rel,
        "schema_reference_section": schema_reference_section,
        "benchmark_notes": benchmark_notes,
        "fused_scan_path": paths["fused_scan_path"],
        "fused_header_path": paths["fused_header_path"],
        "optimization_gate_suffix": optimization_gate_suffix,
        "audit_gate_suffix": audit_gate_suffix,
        "test_template_path": str(workload.get("test_template_path", "")),
        "benchmark_template_path": str(workload.get("benchmark_template_path", "")),
        "repo_root": str(repo),
        "build_dir": pipeline_config.build_dir.name,
        "reference_sql": reference_sql,
        "reference_sql_path": reference_sql_path,
        "workload_reference_section": workload_reference_section,
        "parameters_yaml": yaml.safe_dump(workload.get("parameters", [])) if stage == "analysis" else "",
        "output_schema_yaml": yaml.safe_dump(workload.get("output_schema", [])) if stage == "analysis" else "",
        "dependent_tables_yaml": yaml.safe_dump(workload.get("dependent_tables", [])) if stage == "analysis" else "",
        "partitioning_yaml": yaml.safe_dump(workload.get("partitioning", {})) if stage == "analysis" else "",
        "accelerator_id": str(workload.get("accelerator_id", "")),
        "base_function_name": str(workload.get("base_function_name", "")),
        "aux_function_name": str(workload.get("aux_function_name", "")),
        "cost_model_config": paths.get("cost_model_config", ""),
        "shared_docs_reference": shared_docs,
        "agent_boundary": (
            "See `pipeline/prompts/aux_state/shared/agent_boundary.md` (read it if needed)."
            if is_aux
            else ""
        ),
        "constraints": (
            "See `pipeline/prompts/aux_state/shared/constraints.md` (read it if needed)."
            if is_aux
            else constraints_text
        ),
        "profiling_conventions": (
            "See `pipeline/prompts/shared/profiling_conventions.md` (read it if needed)."
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
            "Maximize dual-axis score read_only^0.75 * write_mix^0.25: strong hit latency at 0% writes "
            "AND healthy 10% write-mix workload via cheap incremental maintenance (ApplyDelta / local "
            "rebuilds). When read is strong and 10% ≪ 1×, prioritize maintenance — it moves the accept gate."
            if is_aux and stage in opt_stages
            else "Beat the baseline engine path (`speedup_median` > 1.0) and keep improving "
            "`best_speedup_median` — there is no fixed speedup ceiling; stop only when gates stall progress."
        ),
        "hard_guidelines": hard_guidelines,
        "measurement_trust": _read_text(pipe / "prompts" / "shared" / "measurement_trust.md"),
        "codex_tooling": codex_tooling,
        "audit_result_json": _read_text(art / "audit_result.json", "{}"),
        "retrieval_brief": retrieval_brief,
        "experiment_log_excerpt": _tail_lines(art / "experiment_log.md", 2 if lean_stage else 5),
        "experiment_summary": _tail_lines(art / "experiment_log.md", 2 if lean_stage else 8),
        "optimization_iter": str(optimization_iter),
        "aux_bench_summary_json": compact_json(
            compact_bench_summary(bench_summary) if stage == "optimization" else bench_summary
        )
        if is_aux
        else "{}",
        "full_analysis_path": (
            str(analysis_path.relative_to(repo))
            if analysis_path.is_file()
            else analysis_artifact_rel
        ),
        "analysis_revision_request": "",
        "implementation_bootstrap": (
            build_implementation_bootstrap(
                repo, workload, query, analysis, cost_model_config=paths.get("cost_model_config", "")
            )
            if is_aux and stage == "implementation" and analysis
            else ""
        ),
        "engine_name": engine_name,
        "EngineName": engine_pascal,
        "register_path": str(engine_host.get("register_path") or ""),
        "aux_register_path": str(
            engine_host.get("aux_register_path") or engine_host.get("register_path") or ""
        ),
        "differential_binary": str(engine_host.get("differential_binary") or ""),
        "differential_source": str(engine_host.get("differential_source") or ""),
        "differential_command": str(engine_host.get("differential_command") or ""),
        "multi_conn_target": str(engine_host.get("multi_conn_target") or ""),
        "cross_accel_target": str(engine_host.get("cross_accel_target") or ""),
        "harness_bench_target": str(engine_host.get("bench_target") or ""),
        "harness_calibrate_target": str(engine_host.get("calibrate_target") or ""),
        "dataset_env": str(engine_host.get("dataset_env") or ""),
        "engine_profile": _engine_profile_for_stage(workload, stage) if is_engine else "",
        "backend_api": "" if is_engine or not engine_name else render_backend_api(repo, engine_name),
        "accelerator_engine_profile": (
            "" if is_engine or not engine_name else render_accelerator_engine_profile(pipe, engine_name)
        ),
        "engine_host_discovery": (
            _engine_host_discovery_block(workload, repo, pipeline_config.build_dir)
            if is_engine
            else _accelerator_engine_host_block(workload, repo, pipeline_config.build_dir)
        ),

        "optional_examples_section": (
            _read_text(pipe / "prompts" / "engine_backend" / "optional_examples.md")
            + (
                "\n\nWorkload `placement_candidates` (S6):\n"
                + "\n".join(f"- `{m}`" for m in (workload.get("placement_candidates") or []))
                if workload.get("placement_candidates")
                else ""
            )
            if is_engine
            else ""
        ),
        "engine_generation_mode": (
            (
                "update_existing"
                if (repo / str((workload.get("target_paths") or {}).get("host_header", ""))).is_file()
                and workload.get("allow_update_existing", True)
                else "generate_from_scratch"
            )
            if is_engine
            else ""
        ),
        "engine_target_paths": (
            "\n".join(f"- `{k}`: `{v}`" for k, v in dict(workload.get("target_paths") or {}).items())
            if is_engine
            else ""
        ),
        "engine_write_paths": (
            "\n".join(f"- `{p}`" for p in (workload.get("write_paths") or []))
            if is_engine
            else ""
        ),
        "engine_cmake_entry": (
            str((workload.get("cmake") or {}).get("entry") or "")
            if is_engine
            else ""
        ),
        "engine_cmake_build_flag": (
            str((workload.get("cmake") or {}).get("build_flag") or "(none)")
            if is_engine
            else ""
        ),
        "engine_layers_spec": (
            _read_text(pipe / "prompts" / "engine_backend" / "layers.md")
            if is_engine
            else ""
        ),
        "layer_contracts": (
            _read_text(pipe / "prompts" / "engine_backend" / "layer_contracts.md")
            if is_engine
            else ""
        ),
        "host_contract": (
            _read_text(pipe / "prompts" / "engine_backend" / "host_contract.md")
            if is_engine
            else ""
        ),
        "layer_id": "",
        "layer_title": "",
        "layer_gate": "",
        "layer_harness": "",
        "layer_cmake_target": "",
        "layer_rules": "",
        "prior_layers": "",
        "host_runtime_kind": (
            str((workload.get("host_runtime") or {}).get("kind") or "host_cmake")
            if is_engine
            else ""
        ),
    }
    return ctx
