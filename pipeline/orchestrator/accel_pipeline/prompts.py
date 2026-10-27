"""The prompt a stage renders, and what it is handed instead of re-reading the repository.

A template under `pipeline/prompts/` is filled from one key dictionary: the keys every workload shares, then those
of its kind (query accelerator, aux-state accelerator, engine backend) and, for optimization stages, the files it may
edit and the score it is judged by. The digests that fill it are built without a model: the analysis, cut to the
slice each stage needs (and the shared docs it cites), and for the audit a digest of the query's directory (file
map, entry points, the chosen design, the experiment log, open findings), the cumulative diff since implementation
began and the audit's blocking findings.
"""
from __future__ import annotations

import copy
import hashlib
import json
import re
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.config import (
    analysis_artifact_path,
    engine_host_conventions,
    gates_dir_for_query,
    is_critique_stage,
    is_implementation_stage,
    load_query_analysis,
    PipelineConfig,
    query_artifacts_dir,
    read_json,
    render_engine_profile,
    resolve_optimization_edit_paths,
    resolve_workload_paths,
    sanitize_engine_name,
    with_facts,
)
from accel_pipeline.engine_host import format_host_block
from accel_pipeline.gates.build_stamp import git, is_relevant
from accel_pipeline.layout import query_dir as query_dir_of
from accel_pipeline.runlog import squeeze_truncate


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
ROLES_HEADING = "## Contract roles"
ACCELERATOR_PROFILE_SECTIONS = [
    "identity",
    "process_model",
    "transactions",
    "scan",
    "entry_points",
    "thread_knobs",
    "hazards",
]


def resolve_prompt_path(pipeline_config: PipelineConfig, workload: dict, prompt_name: str) -> Path:
    subdir = workload.get("prompt_subdir")
    if subdir:
        path = pipeline_config.pipeline_root / "prompts" / subdir / prompt_name
        if path.is_file():
            return path
    return pipeline_config.pipeline_root / "prompts" / prompt_name


def render_prompt(
    pipeline_config: PipelineConfig,
    prompt_name: str,
    workload: dict,
    query: int,
    stage: str,
    optimization_iter: int = 0,
    extra_context: dict[str, str] | None = None,
) -> str:
    path = resolve_prompt_path(pipeline_config, workload, prompt_name)
    if not path.is_file():
        raise FileNotFoundError(path)

    template = path.read_text(encoding="utf-8")
    ctx = build_prompt_context(pipeline_config, workload, query, stage, optimization_iter)
    ctx.setdefault("analysis_revision_request", "")
    if extra_context:
        ctx.update(extra_context)

    def repl(match: re.Match[str]) -> str:
        key = match.group(1)
        return ctx.get(key, match.group(0))

    # twice: injected shared docs (hard_guidelines, ...) carry tokens of their own
    rendered = re.sub(r"\{\{(\w+)\}\}", repl, re.sub(r"\{\{(\w+)\}\}", repl, template))
    # Concurrent pack workers: rewrite legacy build-release tokens so Codex native
    # shell (which bypasses run_pipeline_command rewrites) uses this worker's build dir.
    build_name = pipeline_config.build_dir.name
    if build_name and build_name != "build-release" and "build-release" in rendered:
        rendered = rendered.replace("build-release", build_name)
    return rendered


def with_codex_tooling(pipeline_config: PipelineConfig, prompt: str, *, engine_backend: bool = False) -> str:
    """Append the tool guide (and, for engine-backend stages, their pass bars)."""
    prompts = pipeline_config.pipeline_root / "prompts"
    guides = [prompts / "shared" / "codex_tooling.md"] + ([prompts / "engine_backend" / "codex_tooling.md"] if engine_backend else [])
    return "\n\n".join([prompt.rstrip()] + [g.read_text(encoding="utf-8") for g in guides])


def save_rendered_prompt(
    pipeline_config: PipelineConfig,
    workload: dict,
    query: int,
    stage: str,
    content: str,
    optimization_iter: int = 0,
) -> Path:
    prompts_dir = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload) / "prompts"
    prompts_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"_{optimization_iter}" if stage.startswith("optimization") else ""
    out = prompts_dir / f"{stage}{suffix}.md"
    out.write_text(content, encoding="utf-8")
    return out


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
        ctx.update(_base_keys(analysis, analysis_path, stage))
        ctx.update(_engine_backend_keys(pipeline_config, workload, stage))
    elif kind == "aux_state":
        ctx.update(_accelerator_keys(pipeline_config, workload))
        ctx.update(_aux_keys(pipeline_config, workload, query, stage, paths, analysis, ctx["reference_sql"]))
    else:
        ctx.update(_accelerator_keys(pipeline_config, workload))
        ctx.update(_base_keys(analysis, analysis_path, stage))
    if stage in OPTIMIZATION_STAGES:
        ctx.update(_optimization_keys(pipeline_config, workload, query, paths, optimization_iter, ctx["benchmark_notes"]))
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
        "benchmark_notes": _benchmark_notes(workload, paths),
        "audit_gate_suffix": "".join(f" and `{g}`" for g in _audit_gates(workload) if g != "fairness"),
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


def _benchmark_notes(workload: dict[str, Any], paths: dict[str, Any]) -> str:
    """What the workload's own settings say about its data and checks (no benchmark is special)."""
    db = f" (`{paths['dataset_db']}`)" if paths["dataset_db"] else ""
    lines = [f"- **Iteration data**: correctness, profile and A/B runs use the iteration dataset{db} for fast feedback."]
    if "sf10" in _planned_gates(workload):
        lines.append(f"- **Scale check**: the `sf10` gate reruns on `{paths['scale_db']}`, about 10x the rows: it "
                     "catches sizing and state blowups.")
    lines += [
        "- **Production target**: far larger data. Size from bind-time `row_estimate` and table statistics, never "
        "from the iteration data's row or distinct counts; keep aux and read paths bounded "
        "(`pipeline/prompts/shared/scale_factor_contract.md`).",
        "- Do not optimize for iteration-scale wins that add full-domain scans or cardinality constants.",
    ]
    if not workload.get("parameters"):
        lines.append("- The query has **no bind parameters**: its literals are embedded, so its table function may "
                     "take no arguments.")
    if workload.get("workload_kind") == "aux_state":
        lines.append(f"- The bench's write suites are `{paths['aux_bench_suites']}` (the workload's "
                     "`aux_bench_sweep.suites`).")
    return "\n".join(lines)


def _audit_gates(workload: dict[str, Any]) -> list[str]:
    from accel_pipeline.stages.base import workload_post_audit_gates

    return workload_post_audit_gates(workload)


def _planned_gates(workload: dict[str, Any]) -> list[str]:
    from accel_pipeline.stages.base import workload_post_optimization_gates

    return [*workload_post_optimization_gates(workload), *_audit_gates(workload)]


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
    from accel_pipeline.datasets import build_data_section, dependent_table_names
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


def _engine_backend_keys(cfg: PipelineConfig, workload: dict[str, Any], stage: str) -> dict[str, str]:
    repo, prompts = cfg.repo_root, cfg.pipeline_root / "prompts" / "engine_backend"
    target_paths = dict(workload.get("target_paths") or {})
    cmake = workload.get("cmake") or {}
    placement = workload.get("placement_candidates") or []
    optional_examples = read_text(prompts / "optional_examples.md")
    if placement:
        optional_examples += "\n\nWorkload `placement_candidates` (S6):\n" + "\n".join(f"- `{m}`" for m in placement)
    update_existing = (repo / str(target_paths.get("host_header", ""))).is_file() and workload.get("allow_update_existing", True)
    return {
        "engine_profile": _profile_for_stage(workload, stage),
        "backend_api": "",
        "accelerator_engine_profile": "",
        "engine_host_discovery": format_host_block(workload, repo, cfg.build_dir),
        "optional_examples_section": optional_examples,
        "engine_generation_mode": "update_existing" if update_existing else "generate_from_scratch",
        "engine_target_paths": "\n".join(f"- `{k}`: `{v}`" for k, v in target_paths.items()),
        "engine_write_paths": "\n".join(f"- `{p}`" for p in (workload.get("write_paths") or [])),
        "engine_cmake_entry": str(cmake.get("entry") or ""),
        "engine_cmake_build_flag": str(cmake.get("build_flag") or "(none)"),
        "engine_layers_spec": read_text(prompts / "layers.md"),
        "layer_contracts": read_text(prompts / "layer_contracts.md"),
        "host_contract": read_text(prompts / "host_contract.md"),
        "host_runtime_kind": str((workload.get("host_runtime") or {}).get("kind") or "host_cmake"),
        # a layer stage supplies these (stage._layer_extra_context)
        **dict.fromkeys(
            ("layer_id", "layer_title", "layer_gate", "layer_harness", "layer_cmake_target", "layer_rules", "prior_layers"), ""
        ),
    }


def _profile_for_stage(workload: dict[str, Any], stage: str) -> str:
    """The engine profile sections this stage sees (impl_<layer> / critique_<layer> use the layer's)."""
    layer = stage.split("_", 1)[1] if stage.startswith(("impl_", "critique_")) else None
    return render_engine_profile(
        workload["engine_profile"],
        workload["engine_profile_sections"][layer or stage],
        layer=layer,
        host_layer_runs=bool(workload.get("requires_in_process_host", False)),
    )


DUAL_AXIS_NOTES = (
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


def _optimization_keys(cfg: PipelineConfig, workload: dict[str, Any], query: int, paths: dict[str, Any],
                       optimization_iter: int, benchmark_notes: str) -> dict[str, str]:
    edit = resolve_optimization_edit_paths(cfg, workload, query)
    is_aux = workload.get("workload_kind") == "aux_state"
    header, kernel, package = (edit["header"], edit["primary_cpp"], edit["secondary_cpp"]) if is_aux else (
        paths["header_path"], paths["kernel_path"], paths["package_path"])
    return {
        "header_path": header,
        "kernel_path": kernel,
        "package_path": package,
        "optimization_edit_paths": "\n".join(f"- `{p}`" for p in (edit["all_paths"] if is_aux else (header, kernel, package))),
        "optimization_max_iterations": str(cfg.optimization_max_iterations),
        "optimization_iter": str(optimization_iter),
        "benchmark_notes": benchmark_notes + DUAL_AXIS_NOTES if is_aux else benchmark_notes,
    }


@dataclass(frozen=True)
class ContractRole:
    role: str
    headers: tuple[str, ...]
    symbols: tuple[str, ...]


def _backticked(cell: str) -> tuple[str, ...]:
    return tuple(re.findall(r"`([^`]+)`", cell))


def parse_contract_roles(readme: str) -> list[ContractRole]:
    """The rows of the first table under `## Contract roles`."""
    _, found, tail = readme.partition(ROLES_HEADING)
    if not found:
        return []
    roles: list[ContractRole] = []
    seen_table = False
    for line in tail.splitlines():
        if not line.lstrip().startswith("|"):
            if seen_table:
                break
            continue
        seen_table = True
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 3 or cells[0] == "role" or set(cells[0]) <= set("-: "):
            continue
        roles.append(ContractRole(cells[0], _backticked(cells[1]), _backticked(cells[2])))
    return roles


def render_backend_api(repo_root: Path, engine: str) -> str:
    """The role index of `backends/<engine>/api/README.md` as prompt markdown."""
    readme = repo_root / "backends" / engine / "api" / "README.md"
    api = f"backends/{engine}/api"
    if not readme.is_file():
        return f"### Backend API\n\n`{api}/README.md` is absent: the backend has not been generated for this run.\n"
    lines = [
        "### Backend API",
        "",
        f"Include only the `{api}/` headers below, by role (`{api}/README.md` has the prose).",
        "",
    ]
    for r in parse_contract_roles(readme.read_text(encoding="utf-8")):
        if not r.headers:
            lines.append(f"- **{r.role}:** n/a")
            continue
        symbols = f"; symbols {', '.join(f'`{s}`' for s in r.symbols)}" if r.symbols else ""
        lines.append(f"- **{r.role}:** {', '.join(f'`{api}/{h.removeprefix('api/')}`' for h in r.headers)}{symbols}")
    return "\n".join(lines) + "\n"


def render_accelerator_engine_profile(pipeline_root: Path, engine: str) -> str:
    """The engine's profile sections an accelerator stage sees; empty when the engine has none."""
    path = pipeline_root / "config" / "workloads" / f"engine_{engine}.yaml"
    if not path.is_file():
        return ""
    profile: dict[str, Any] = yaml.safe_load(path.read_text(encoding="utf-8"))["engine_profile"]
    return "### Engine profile\n\n" + render_engine_profile(profile, ACCELERATOR_PROFILE_SECTIONS)


def _existing(repo: Path, rel: str) -> str | None:
    path = repo / rel
    return rel if path.is_file() else None


def build_implementation_bootstrap(
    repo: Path,
    workload: dict[str, Any],
    query: int,
    analysis: dict[str, Any],
    *,
    cost_model_config: str,
) -> str:
    benchmark = str(workload.get("benchmark_name", "tpch"))
    engine = str(workload.get("engine_name") or "")
    q = f"q{query}"
    qdir = f"accelerators/{benchmark}/{q}"
    lines = [
        "## Implementation bootstrap (authoritative — do not re-discover by listing directories)",
        "",
        "### Files to create (write these; `implementation_context_json` has semantics)",
        "",
    ]
    for item in analysis.get("files_to_create") or []:
        if isinstance(item, dict):
            path = item.get("path", "")
            role = item.get("role", "")
            lines.append(f"- `{path}` — {role}")
        else:
            lines.append(f"- `{item}`")

    lines.extend(["", "### Templates & gates (only if needed after module.cpp exists)", ""])
    templates = [
        f"{qdir}/{engine}/base/single-threaded/kernel.cpp",
        f"{qdir}/{engine}/base/query.hpp",
        f"{qdir}/tests/test_{benchmark}_{q}.cpp",
        f"{qdir}/tests/{q}_manifest.hpp",
        "eval/tests/test_tpch_module.cpp",
        f"accelerators/{benchmark}/q14/bench/benchmark_q14.cpp",
        cost_model_config,
        f"accelerators/{benchmark}/q14/cost_model.hpp",
        f"accelerators/{benchmark}/q14/layout.hpp",
        f"accelerators/{benchmark}/q14/{engine}/register.cpp",
        "eval/tests/test_support/cross_accel_helpers.cpp",
        "pipeline/prompts/aux_state/shared/read_path_scaffold.md",
        "pipeline/prompts/aux_state/shared/unit_test_contract.md",
    ]
    for rel in templates:
        if _existing(repo, rel):
            lines.append(f"- `{rel}`")

    ref_sql_path = str(workload.get("reference_sql_path") or "")
    ref_sql = load_reference_sql(ref_sql_path)
    lines.extend(["", format_predicate_bootstrap_section(ref_sql, ref_sql_path)])

    lines.extend(
        [
            "### Turn discipline",
            "",
            "1. Read the reference aux paths above once (they are the layout to mirror).",
            "2. **Immediately** write `module.hpp`, then `module.cpp`, then other `files_to_create`.",
            "3. Unit tests: none to write (shared ModuleRuntime test); make the catalog probe/arguments follow `unit_test_contract.md` + the predicate checklist above.",
            "4. Do **not** list the repo root or search other queries' layouts after step 1. Do re-read the part of analysis.json you are implementing (`state_storage`, `composition_rule`, `mutation_coverage`, `legal_deltas`) whenever you start on it.",
            "5. Search several files with `rg`; read a single file with `sed -n`.",
            "6. Build/test only after all primary sources exist; **register everywhere** (mirror peer-query wiring); finish only after MCP `run_correctness` **and** `run_multi_connection` both pass.",
            "",
        ]
    )
    return "\n".join(lines)


def _norm(sql: str) -> str:
    return re.sub(r"\s+", " ", sql.strip())


def extract_predicate_checklist(sql: str) -> list[str]:
    """Return human checklist lines derived from SQL operator polarity."""
    if not sql or not sql.strip():
        return []
    text = sql
    upper = sql.upper()
    checks: list[str] = []

    if re.search(r"\bNOT\s+LIKE\b", upper):
        patterns = re.findall(r"NOT\s+LIKE\s+('[^']+'|\$\d+)", text, flags=re.IGNORECASE)
        detail = ", ".join(patterns) if patterns else "bound pattern"
        checks.append(
            f"NOT LIKE ({detail}): surviving fixture values must NOT match the pattern; "
            "never bind a pattern that matches the rows you expect to keep."
        )
    elif re.search(r"\bLIKE\b", upper):
        checks.append(
            "LIKE filter present: surviving fixture values must match the pattern; "
            "add a non-matching row that is excluded."
        )

    if re.search(r"<>|!=", text):
        checks.append(
            "Inequality (<> / !=): include one row that differs (kept) and one that equals "
            "the excluded key (dropped)."
        )

    if re.search(r"\bNOT\s+IN\b", upper) or "CUSTOMER%COMPLAINTS" in upper.replace(" ", ""):
        checks.append(
            "Anti-join / NOT IN / complaint exclusion: excluded keys contribute 0 to counts; "
            "non-excluded keys must be counted. Do not store eligibility as the inverse bit."
        )

    if re.search(r"\bCOUNT\s*\(\s*DISTINCT\b", upper):
        checks.append(
            "COUNT(DISTINCT …): duplicate fact links to the same key must not inflate the measure."
        )

    if re.search(r"\bIN\s*\(", upper) and "SIZE" in upper:
        checks.append(
            "Size / IN-list window: only requested keys contribute; absent partitions emit nothing."
        )

    if not checks:
        checks.append(
            "Derive every Execute filter from this SQL; dual-path (hot Execute vs fallback) must agree."
        )
    else:
        checks.append(
            "Hot-path Execute must apply the same predicates as fallback / SQL (never filter only in fallback)."
        )
    return checks


def load_reference_sql(path: str | Path | None, *, max_chars: int = 2500) -> str:
    if not path:
        return ""
    p = Path(path)
    if not p.is_file():
        return ""
    text = p.read_text(encoding="utf-8")
    text = text.strip()
    if len(text) > max_chars:
        return text[: max_chars - 20] + "\n-- … truncated …\n"
    return text


def format_predicate_bootstrap_section(sql: str, sql_path: str = "") -> str:
    checklist = extract_predicate_checklist(sql)
    lines = [
        "### Reference SQL predicate contract (authoritative)",
        "",
        "Catalog arguments and probes **must** obey this SQL. See `pipeline/prompts/aux_state/shared/unit_test_contract.md`.",
        "",
    ]
    if sql_path:
        lines.append(f"Source: `{sql_path}`")
        lines.append("")
    if sql.strip():
        lines.extend(["```sql", _norm(sql) if len(sql) < 400 else sql.strip(), "```", ""])
    lines.append("Checklist:")
    for item in checklist:
        lines.append(f"- {item}")
    lines.append("")
    return "\n".join(lines)


def read_text(path: Path, default: str = "") -> str:
    return path.read_text(encoding="utf-8") if path.is_file() else default


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


IMPLEMENTATION_BASE_FILE = "implementation_base.txt"
AUDIT_FINDINGS_FILE = "audit_open_findings.md"
DIFF_LIMIT_CHARS = 120_000
MAX_SYMBOLS_PER_FILE = 40
LOG_TAIL_LINES = 25

_SOURCE_SUFFIXES = {".hpp", ".h", ".cpp", ".cc", ".inc", ".py", ".sh", ".cmake", ".json", ".rs", ".md", ".txt", ".sql"}
_ROLE_BY_NAME = {
    "module.hpp": "state and module declarations, public entry points",
    "module.cpp": "module implementation (build, execute, maintenance)",
    "finalize.cpp": "module test (unit contract)",
}
_ROLE_BY_DIR = {
    "tests": "tests", "bench": "benchmark code", "config": "cost-model and calibration config",
    "verus_kit": "verified kernel kit",
}
_TYPE_DECL = re.compile(r"^\s*(?:template\s*<[^>]*>\s*)?(?:class|struct|enum(?:\s+class)?)\s+(\w+)[^;]*$")
_FUNCTION_DECL = re.compile(r"^[\w:<>,*&\s~]+?\b(\w+)\s*\([^;]*\)[^;]*;?$")
_NOT_FUNCTIONS = frozenset({"if", "for", "while", "switch", "return", "sizeof", "static_assert", "using", "typedef"})


def record_implementation_base(repo_root: Path, art: Path) -> None:
    """Remember the commit the implementation starts from; a resumed run keeps the first one."""
    marker = art / IMPLEMENTATION_BASE_FILE
    if not marker.is_file():
        marker.parent.mkdir(parents=True, exist_ok=True)
        marker.write_text(git(repo_root, "rev-parse", "HEAD").decode().strip(), encoding="utf-8")


def _lines(repo_root: Path, *args: str) -> list[str]:
    try:
        return [line for line in git(repo_root, *args).decode("utf-8", "replace").split("\0" if "-z" in args else "\n") if line]
    except (OSError, subprocess.CalledProcessError):
        return []


def changed_files(repo_root: Path, base: str, query_dir: str) -> list[str]:
    """Files that differ from `base` (committed, uncommitted or new) and can affect this query."""
    paths = set(_lines(repo_root, "diff", "--name-only", "-z", base)) | set(
        _lines(repo_root, "ls-files", "--others", "--exclude-standard", "-z"))
    return sorted(p for p in paths if is_relevant(p, query_dir))


def snapshot(repo_root: Path, paths: list[str]) -> dict[str, str]:
    """path -> content hash, to tell later which files a repair round touched."""
    return {p: hashlib.sha256((repo_root / p).read_bytes()).hexdigest() if (repo_root / p).is_file() else "deleted"
            for p in paths}


def touched_since(repo_root: Path, before: dict[str, str], base: str, query_dir: str) -> list[str]:
    after = snapshot(repo_root, changed_files(repo_root, base, query_dir))
    return sorted(p for p in before.keys() | after.keys() if before.get(p) != after.get(p))


def cumulative_diff(repo_root: Path, base: str, paths: list[str], *, limit: int = DIFF_LIMIT_CHARS) -> str:
    """The diff of `paths` against `base` (new files as additions), the file order given, up to `limit`
    characters; files that no longer fit are listed with their size."""
    tracked = set(_lines(repo_root, "ls-files", "-z", "--", *paths)) if paths else set()
    parts: list[str] = []
    used = 0
    for path in paths:
        if path in tracked or not (repo_root / path).is_file():
            cmd = ["git", "diff", "--no-color", base, "--", path]
        else:
            cmd = ["git", "diff", "--no-color", "--no-index", "--", "/dev/null", path]
        text = subprocess.run(cmd, cwd=repo_root, capture_output=True, text=True, errors="replace").stdout
        if used + len(text) > limit:
            parts.append(f"# {path}: diff omitted ({text.count(chr(10))} lines; read the file)")
            continue
        parts.append(text.rstrip())
        used += len(text)
    return "\n".join(part for part in parts if part)


def _strip_comments(text: str) -> str:
    return re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.DOTALL)


def public_symbols(header: str) -> list[str]:
    """Namespace-level types and function signatures of a C++ header, one line each."""
    symbols: list[str] = []
    depth = 0
    for line in _strip_comments(header).splitlines():
        stripped = line.strip()
        if depth == 0 and stripped and not stripped.startswith(("#", "namespace", "extern", "}")):
            if (m := _TYPE_DECL.match(line)) and not stripped.endswith(";"):
                symbols.append(f"type {m.group(1)}")
            elif (m := _FUNCTION_DECL.match(line.split("{", 1)[0].rstrip())) and m.group(1) not in _NOT_FUNCTIONS \
                    and stripped.endswith((";", "{", "}")):
                symbols.append(" ".join(stripped.split()).rstrip(";")[:120])
        opens = 0 if stripped.startswith("namespace") or stripped.startswith('extern "C"') else line.count("{")
        closes = line.count("}") if depth > 0 or opens else 0
        depth = max(0, depth + opens - closes)
    return symbols[:MAX_SYMBOLS_PER_FILE]


def _first_comment(text: str) -> str:
    for line in text.splitlines()[:8]:
        stripped = line.strip()
        if stripped.startswith(("//", "/*", "#", '"""')) and not stripped.startswith(("#include", "#pragma", "#!")):
            return stripped.lstrip("/*#\"' ").rstrip("*/\"' ")[:100]
    return ""


def _role(path: str, declared: dict[str, str], text: str) -> str:
    for suffix, role in declared.items():
        if path.endswith(suffix):
            return role
    rel = Path(path)
    return (_first_comment(text) or _ROLE_BY_NAME.get(rel.name)
            or next((role for part in rel.parts if (role := _ROLE_BY_DIR.get(part))), "") or "source")


def _file_map(repo_root: Path, query_dir: str, declared: dict[str, str]) -> tuple[list[str], list[str]]:
    files = sorted(set(_lines(repo_root, "ls-files", "-z", "--", query_dir))
                   | set(_lines(repo_root, "ls-files", "--others", "--exclude-standard", "-z", "--", query_dir)))
    rows, entry_points = [], []
    for path in files:
        file = repo_root / path
        if not file.is_file() or file.suffix not in _SOURCE_SUFFIXES:
            continue
        text = file.read_text(encoding="utf-8", errors="replace")
        rel = path.removeprefix(query_dir.rstrip("/") + "/")
        rows.append(f"- `{rel}` ({text.count(chr(10))} lines): {_role(rel, declared, text)}")
        if file.suffix in {".hpp", ".h"} and (symbols := public_symbols(text)):
            entry_points += [f"- `{rel}`: `{symbol}`" for symbol in symbols]
    return rows, entry_points


def _design(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> tuple[list[str], dict[str, str]]:
    path = analysis_artifact_path(pipeline_config.repo_root, query, workload=workload)
    analysis = read_json(path)
    if not analysis:
        return ["- (no analysis on disk)"], {}
    audit_view = analysis_for_audit(analysis)
    declared = {str(f["path"]): str(f["role"]) for f in audit_view["files_to_create"] if f.get("path") and f.get("role")}
    lines = [f"- selected candidate: `{audit_view.get('selected_candidate_id')}`"]
    lines += [f"- {key}: {value}" for key, value in (audit_view.get("state_design_summary") or {}).items()
              if key != "selected_candidate_id" and value]
    if audit_view.get("dependent_table_names"):
        lines.append(f"- dependent tables: {', '.join(audit_view['dependent_table_names'])}")
    lines.append(f"- mutation_coverage entries: {audit_view.get('mutation_coverage_entries')}")
    return lines, declared


def _log_summary(art: Path) -> list[str]:
    log = art / "experiment_log.md"
    if not log.is_file():
        return ["- (no experiment log)"]
    lines = [line for line in log.read_text(encoding="utf-8").splitlines() if line.strip()]
    repairs = sum("repair[" in line for line in lines)
    out = [f"{len(lines)} lines, {repairs} repair decisions. Last {min(LOG_TAIL_LINES, len(lines))}:"]
    return out + [f"  {line[:240]}" for line in lines[-LOG_TAIL_LINES:]]


def _open_findings(art: Path) -> list[str]:
    out: list[str] = []
    quarantine = art / "quarantine.json"
    if quarantine.is_file():
        out += [f"- gate `{q.get('gate')}` quarantined ({q.get('infra_reason')}) in {q.get('stage')}"
                for q in json.loads(quarantine.read_text(encoding="utf-8"))]
    for result in sorted(gates_dir_for_query(art).glob("*.json")):
        data = read_json(result)
        status = data.get("status") if isinstance(data, dict) else None  # not every file there is a gate result
        if status not in (None, "pass", "skip", "cancelled"):
            out.append(f"- gate `{result.stem}` last result: {status}")
    summary = read_json(art / "aux_bench_summary.json")
    if summary.get("measurement_ok") is False:
        out.append(f"- the last bench sweep was not trustworthy: {'; '.join(summary.get('measurement_diagnosis') or [])[:300]}")
    if (art / AUDIT_FINDINGS_FILE).is_file():
        out.append(f"- earlier audit findings left open: see `{AUDIT_FINDINGS_FILE}`")
    return out or ["- none recorded"]


def query_digest(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> str:
    """Markdown digest of the query's accelerator, from files and logs only."""
    bench = str(workload.get("benchmark_name") or "tpch")
    query_dir = query_dir_of(bench, query)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    design, declared = _design(pipeline_config, workload, query)
    files, entry_points = _file_map(pipeline_config.repo_root, query_dir, declared)
    sections = [
        ("File map", files or ["- (no files)"]),
        ("Public entry points (namespace-level declarations of the headers)", entry_points or ["- none found"]),
        ("Chosen design (from the analysis)", design),
        ("Experiment log", _log_summary(art)),
        ("Open findings", _open_findings(art)),
    ]
    return f"# Query digest: Q{query} (`{query_dir}`)\n\n" + "\n\n".join(
        f"## {title}\n" + "\n".join(lines) for title, lines in sections)


def audit_context(pipeline_config: PipelineConfig, workload: dict[str, Any], query: int) -> str:
    """The digest plus the cumulative diff since implementation began, as one user-message block."""
    repo_root = pipeline_config.repo_root
    art = query_artifacts_dir(repo_root, query, workload=workload)
    query_dir = query_dir_of(str(workload.get("benchmark_name") or "tpch"), query)
    marker = art / IMPLEMENTATION_BASE_FILE
    base = marker.read_text(encoding="utf-8").strip() if marker.is_file() else "HEAD"
    paths = changed_files(repo_root, base, query_dir)
    paths.sort(key=lambda p: (not p.startswith(query_dir + "/"), p))  # the query's own files first
    diff = cumulative_diff(repo_root, base, paths)
    digest = query_digest(pipeline_config, workload, query)
    from accel_pipeline.optimization.structural import design_revisions_markdown

    return (f"{digest}\n\n{design_revisions_markdown(art)}\n"
            f"## Cumulative diff since implementation began ({len(paths)} files; base `{base[:12]}`)\n\n"
            f"```diff\n{diff or '(no changes)'}\n```\n")


_VERDICTS_AGAINST = frozenset({"fail", "failed", "false"})


def _as_text(item: Any) -> str:
    return item if isinstance(item, str) else json.dumps(item, sort_keys=True)


def blocking_findings(audit_result: dict[str, Any]) -> list[str]:
    """What the audit says must be fixed: a failed correctness verdict, generality failures, COW
    findings, failed large-scale classes and a suspected fairness violation. Risks and notes are not blocking."""
    scale = audit_result.get("scale_review") or {}
    findings: list[str] = []
    if audit_result.get("correctness_ok") is False:
        findings.append("the audit's verdict is correctness_ok=false")
    findings += [f"generality failure: {_as_text(i)}" for i in scale.get("generality_failures") or []]
    findings += [f"copy-on-write finding: {_as_text(i)}" for i in scale.get("cow_findings") or []]
    for entry in scale.get("large_scale") or []:
        verdicts = (str(v).lower() for v in entry.values()) if isinstance(entry, dict) else iter(())
        if any(v in _VERDICTS_AGAINST for v in verdicts):
            findings.append(f"large-scale class failed: {_as_text(entry)}")
    fairness = audit_result.get("fairness_review") or {}
    if fairness.get("cheating_suspected"):
        findings += [f"fairness: {_as_text(i)}" for i in fairness.get("issues") or ["cheating suspected"]]
    return findings


def numbered(findings: list[str]) -> str:
    return "\n".join(f"{i}. {finding}" for i, finding in enumerate(findings, 1))
