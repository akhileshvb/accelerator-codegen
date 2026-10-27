"""The `{{key}}` values a prompt template renders with: the keys every workload shares, then those of its kind."""

from __future__ import annotations

from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.analysis_context import (
    analysis_for_audit,
    analysis_for_implementation,
    build_shared_docs_reference,
    compact_base_analysis_for_implementation,
    compact_base_analysis_for_optimization,
    compact_json,
    stage_analysis_json,
)
from accel_pipeline.backend_api import render_accelerator_engine_profile, render_backend_api
from accel_pipeline.config import (
    PipelineConfig,
    engine_host_conventions,
    resolve_workload_paths,
    sanitize_engine_name,
)
from accel_pipeline.engine_host import format_host_block
from accel_pipeline.implementation_bootstrap import build_implementation_bootstrap
from accel_pipeline.models import is_critique_stage, is_implementation_stage
from accel_pipeline.paths import load_query_analysis, query_artifacts_dir
from accel_pipeline.workload_facts import with_facts

ANALYSIS_STAGES = ("analysis", "analysis_critic")  # both see the query, its parameters and the data
OPTIMIZATION_STAGES = (
    "optimization",
    "optimization_unified",
    "optimization_worker",
    "optimization_fix",
    "optimization_unified_hypothesis",
    "optimization_unified_patch",
)
_HARD_GUIDELINE_STAGES = frozenset(
    {"analysis", "audit", "architecture_audit", "validation", "test_suite", *OPTIMIZATION_STAGES}
)


def read_text(path: Path, default: str = "") -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else default


def build_prompt_context(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
    stage: str,
    optimization_iter: int = 0,
) -> dict[str, str]:
    repo = pipeline_config.repo_root
    kind = workload.get("workload_kind")
    if kind != "engine_backend":
        workload = with_facts(repo, workload, query)
    paths = resolve_workload_paths(pipeline_config, workload, query)
    analysis_path, analysis = load_query_analysis(repo, query, workload=workload)

    ctx = _shared_keys(pipeline_config, workload, query, stage, paths, analysis_path)
    if kind == "engine_backend":
        from accel_pipeline.engine_backend.prompt_context import engine_backend_keys

        ctx.update(_base_keys(analysis, analysis_path, stage))
        ctx.update(engine_backend_keys(pipeline_config, workload, stage))
    elif kind == "aux_state":
        ctx.update(_accelerator_keys(pipeline_config, workload))
        ctx.update(_aux_keys(pipeline_config, workload, query, stage, paths, analysis, ctx["reference_sql"]))
    else:
        ctx.update(_accelerator_keys(pipeline_config, workload))
        ctx.update(_base_keys(analysis, analysis_path, stage))
    if stage in OPTIMIZATION_STAGES:
        from accel_pipeline.optimization.prompt_context import optimization_keys

        ctx.update(optimization_keys(pipeline_config, workload, query, paths, optimization_iter, ctx["benchmark_notes"]))
    return ctx


def _shared_keys(cfg: PipelineConfig, workload: dict[str, Any], query: int, stage: str, paths: dict[str, Any],
                 analysis_path: Path) -> dict[str, str]:
    repo, pipe = cfg.repo_root, cfg.pipeline_root
    kind = workload.get("workload_kind")
    benchmark = str(workload.get("benchmark_name", "tpch"))
    raw_engine = workload.get("engine_name")
    engine_name = str(raw_engine or "") if kind == "engine_backend" else sanitize_engine_name(str(raw_engine) if raw_engine else "")
    engine_host = dict(workload.get("engine_host") or (engine_host_conventions(engine_name, repo_root=repo) if engine_name else {}))
    artifacts_rel = str(query_artifacts_dir(repo, query, workload=workload).relative_to(repo))
    analysis_artifact = f"{artifacts_rel}/{'backend_analysis.json' if kind == 'engine_backend' else analysis_path.name}"
    reference_sql, reference_sql_path = "", ""
    if ref_path := workload.get("reference_sql_path"):
        reference_sql_path = str(ref_path)
        if stage in ANALYSIS_STAGES and Path(ref_path).is_file():
            reference_sql = Path(ref_path).read_text(encoding="utf-8")
    constraints = pipe / "prompts" / ("aux_state" if kind == "aux_state" else "") / "shared" / "constraints.md"
    wants_guidelines = is_implementation_stage(stage) or is_critique_stage(stage) or stage in _HARD_GUIDELINE_STAGES
    return {
        "query_number": str(query),
        "benchmark_name": benchmark,
        "display_name": str(workload.get("display_name", benchmark)),
        "function_name": paths["function_name"],
        "header_path": paths["header_path"],
        "kernel_path": paths["kernel_path"],
        "package_path": paths["package_path"],
        "test_path": paths["test_path"],
        "benchmark_path": paths["benchmark_path"],
        "test_target": paths["test_target"],
        "bench_target": paths["bench_target"],
        "ctest_smoke_regex": paths["ctest_smoke_regex"],
        "artifacts_path": artifacts_rel,
        "change_request": "",
        "bootstrap_artifacts_path": f"pipeline/artifacts/engines/{engine_name}/host_bootstrap"
        if engine_name
        else "pipeline/artifacts/engines/host_bootstrap",
        "analysis_artifact_path": analysis_artifact,
        "full_analysis_path": str(analysis_path.relative_to(repo)) if analysis_path.is_file() else analysis_artifact,
        "schema_reference_section": _schema_reference_section(workload, repo),
        "benchmark_notes": _benchmark_notes(workload, paths, query, engine_name),
        "audit_gate_suffix": "" if benchmark == "sec_edgar" else " and `sf10`",
        "fused_scan_path": paths["fused_scan_path"],
        "test_template_path": str(workload.get("test_template_path", "")),
        "benchmark_template_path": str(workload.get("benchmark_template_path", "")),
        "repo_root": str(repo),
        "build_dir": cfg.build_dir.name,
        "reference_sql": reference_sql,
        "reference_sql_path": reference_sql_path,
        "parameters_yaml": yaml.safe_dump(workload.get("parameters", [])) if stage == "analysis" else "",
        "output_schema_yaml": yaml.safe_dump(workload.get("output_schema", [])) if stage == "analysis" else "",
        "accelerator_id": str(workload.get("accelerator_id", "")),
        "aux_function_name": str(workload.get("aux_function_name", "")),
        "cost_model_config": paths.get("cost_model_config", ""),
        "constraints": read_text(constraints),
        "hard_guidelines": read_text(pipe / "prompts" / "shared" / "hard_guidelines.md") if wants_guidelines else "",
        "measurement_trust": read_text(pipe / "prompts" / "shared" / "measurement_trust.md"),
        "fast_loop": read_text(pipe / "prompts" / "shared" / "fast_loop.md")
        if is_implementation_stage(stage) or stage.startswith("optimization")
        else "",
        "analysis_revision_request": "",
        "engine_name": engine_name,
        "register_path": str(engine_host.get("register_path") or ""),
        "aux_register_path": str(engine_host.get("aux_register_path") or engine_host.get("register_path") or ""),
        "differential_binary": str(engine_host.get("differential_binary") or ""),
        "differential_source": str(engine_host.get("differential_source") or ""),
        "multi_conn_target": str(engine_host.get("multi_conn_target") or ""),
        "cross_accel_target": str(engine_host.get("cross_accel_target") or ""),
        "harness_bench_target": str(engine_host.get("bench_target") or ""),
        "harness_calibrate_target": str(engine_host.get("calibrate_target") or ""),
    }


def _schema_reference_section(workload: dict[str, Any], repo: Path) -> str:
    schema_ref = workload.get("schema_reference_path", "")
    if not (schema_ref and Path(schema_ref).is_file()):
        return ""
    rel = Path(schema_ref)
    try:
        rel = rel.relative_to(repo)
    except ValueError:
        pass
    return f"## Schema reference\n\nRead `{rel}` for table/column layout."


def _benchmark_notes(workload: dict[str, Any], paths: dict[str, Any], query: int, engine_name: str) -> str:
    if str(workload.get("benchmark_name", "tpch")) != "sec_edgar":
        return (
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
    notes = (
        "- SEC-EDGAR queries use **embedded literals** (no bind parameters); table functions may be zero-arg.\n"
        "- Correctness requires **full multi-row ordered result match**, not a single scalar.\n"
        "- Benchmark/profile use **one scale factor** (SF3); size structures from bind-time `row_estimate`, "
        "not hardcoded row/distinct counts from the current profile."
    )
    if workload.get("workload_kind") == "aux_state":
        notes += (
            f"\n- SEC EDGAR aux state: `accelerators/sec_edgar/q{query}/{engine_name}/aux/module.hpp` "
            f"(base API: `accelerators/sec_edgar/q{query}/{engine_name}/base/query.hpp`).\n"
            "- Write workload suite is **`filing_append`** / **`value_append`**.\n"
            "- Partition by filing semantics (`stmt`, `adsh`, `cik`+`fy`) — see `pipeline/prompts/sec_edgar/aux_state_notes.md`.\n"
            "- The aux state is discovered from `aux/finalize.cpp`; no CMake registration.\n"
            "- Scale: SF3 is both iteration and production scale here — still size from `row_estimate`, not profile snapshots."
        )
    return notes


def _accelerator_keys(cfg: PipelineConfig, workload: dict[str, Any]) -> dict[str, str]:
    """What a query accelerator's prompts say of its engine (the engine_backend prompts have their own)."""
    engine_name = sanitize_engine_name(str(workload.get("engine_name") or ""))
    return {
        "backend_api": render_backend_api(cfg.repo_root, engine_name) if engine_name else "",
        "accelerator_engine_profile": render_accelerator_engine_profile(cfg.pipeline_root, engine_name) if engine_name else "",
        "engine_host_discovery": _accelerator_host_block(workload, cfg.repo_root, cfg.build_dir),
    }


def _accelerator_host_block(workload: dict[str, Any], repo_root: Path, build_dir: Path) -> str:
    engine = sanitize_engine_name(str(workload.get("engine_name") or ""))
    host = dict(workload.get("engine_host") or engine_host_conventions(engine, repo_root=repo_root))
    lines = [
        "### Engine host (query accelerator)",
        "",
        f"- **engine_name:** `{engine}`",
        f"- **Registration / SQL surface:** {host.get('sql_entrypoint_convention', '')}",
        f"- **Register path (convention):** `{host.get('register_path', '')}`",
    ]
    if aux_reg := str(host.get("aux_register_path") or "").strip():
        lines.append(f"- **Aux register path:** `{aux_reg}`")
    if registry := str(host.get("registry") or "").strip():
        lines.append(
            f"- **Registration:** this query registers itself from its own directory: {registry}. "
            "The engine backend (backends/<engine>/**, incl. its catalog in backends/<engine>/api/catalog/) "
            "names no query and is owned by the engine_backend run; never add per-query code or lists there."
        )
    lines.extend(
        [
            f"- **Dataset env:** `{host.get('dataset_env', '')}`",
            f"- **Differential:** `{host.get('differential_binary', '')}` — "
            f"{host.get('differential_command', '')}",
        ]
    )
    if hint := str(host.get("query_host_ready_hint") or "").strip():
        lines.extend(["", f"**Host readiness:** {hint}"])
    lines.extend(["", format_host_block(workload, repo_root, build_dir)])
    return "\n".join(lines)


def _aux_keys(cfg: PipelineConfig, workload: dict[str, Any], query: int, stage: str, paths: dict[str, Any],
              analysis: dict[str, Any], reference_sql: str) -> dict[str, str]:
    keys = {
        "shared_docs_reference": build_shared_docs_reference("pipeline", stage),
        "agent_boundary": "See `pipeline/prompts/aux_state/shared/agent_boundary.md` (read it if needed).",
        "analysis_json": stage_analysis_json(analysis, stage),
        "implementation_context_json": compact_json(analysis_for_implementation(analysis)) if analysis else "{}",
        "audit_context_json": compact_json(analysis_for_audit(analysis)) if analysis else "{}",
        "implementation_bootstrap": (
            build_implementation_bootstrap(
                cfg.repo_root, workload, query, analysis, cost_model_config=paths.get("cost_model_config", "")
            )
            if stage == "implementation" and analysis
            else ""
        ),
        "workload_reference_section": (
            f"Workload SQL: `{workload.get('reference_sql_path') or 'see workload YAML'}` · "
            "Parameters/tables/partitioning: see `analysis.json` correctness_facts."
        ),
    }
    if stage in ANALYSIS_STAGES:
        keys.update(_analysis_reference(cfg, workload, query, reference_sql))
    return keys


def _analysis_reference(cfg: PipelineConfig, workload: dict[str, Any], query: int, reference_sql: str) -> dict[str, str]:
    """The query, its parameters and the data, as the analysis stages see them."""
    from accel_pipeline.data_shape import build_data_section, dependent_table_names
    from accel_pipeline.tools.data_tools import DataToolContext, dataset_row_counts

    if not reference_sql.strip():
        reference_sql = (
            f"-- MISSING reference SQL at {workload.get('reference_sql_path') or '(unset)'}\n"
            "-- Regenerate the workload's reference SQL (see the workload config)"
        )
    art = query_artifacts_dir(cfg.repo_root, query, workload=workload)
    data_section = build_data_section(
        str(workload.get("engine_name") or ""),
        dataset_row_counts(DataToolContext(cfg, workload, query, art), dependent_table_names(workload)),
        workload.get("production"),
    )
    section = "\n".join(
        [
            "## Query", "", "```sql", reference_sql, "```", "",
            "## Parameters", "", yaml.safe_dump(workload.get("parameters", [])), "",
            "## Dependent tables (hints only)", "", yaml.safe_dump(workload.get("dependent_tables", [])), "",
            "## Partitioning (hints only)", "", yaml.safe_dump(workload.get("partitioning", {})), "",
            data_section,
        ]
    )
    return {"reference_sql": reference_sql, "workload_reference_section": section}


def _base_keys(analysis: dict[str, Any], analysis_path: Path, stage: str) -> dict[str, str]:
    if stage == "implementation":
        analysis_json = compact_base_analysis_for_implementation(analysis)
    elif stage == "optimization":
        analysis_json = compact_base_analysis_for_optimization(analysis)
    else:
        analysis_json = read_text(analysis_path, "{}") if stage == "analysis" else "{}"
    return {
        "workload_reference_section": "",
        "shared_docs_reference": "",
        "agent_boundary": "",
        "analysis_json": analysis_json,
        "implementation_context_json": "{}",
        "audit_context_json": "{}",
        "implementation_bootstrap": "",
    }
