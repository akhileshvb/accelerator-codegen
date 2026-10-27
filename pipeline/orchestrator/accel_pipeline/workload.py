"""What a run accelerates: the workload and everything derived from it.

A workload is a yaml under config/workloads/, refined by its benchmark pack and the aux-state
defaults, and pinned to one engine. From it come the engine's profile (facts and hazards of the engine,
validated against ``schemas/engine_profile.schema.json``; a leaf is a value or ``{na: reason}``), the names the
naming convention derives from a query's benchmark and number (engine-specific symbols are patterns in the
profile, expanded here), the run's artifact paths, the paths and targets of the query's sources, and the
facts that are a function of the query's SQL and the benchmark schema.

Tables and the columns read from them come from sqlglot (qualified against the schema); output columns and
logical types come from DuckDB describing the query over an empty in-memory copy of workloads/<bench>/schema.sql.
A logical type is the DuckDB type family without precision or scale (DECIMAL(38,4) is DECIMAL); mapping a
logical type to an engine's own type is the engine backend's business.
"""

from __future__ import annotations

import copy
import json
import os
import re
from dataclasses import replace
from pathlib import Path
from typing import Any

import duckdb
import jsonschema
import sqlglot
import yaml
from sqlglot import exp
from sqlglot.optimizer.qualify import qualify
from sqlglot.optimizer.scope import traverse_scope

from accel_pipeline.config import (
    PipelineConfig,
    config_yaml,
    find_repo_root,
    load_pipeline_config,
    load_yaml,
    optimization_settings,
    pipeline_root,
)
from accel_pipeline.layout import backend_glob, query_dir, query_glob, workload_schema_sql

SCHEMA_RELATIVE = Path("schemas") / "engine_profile.schema.json"
ALL_SECTIONS = "all"
FACTS_FILE = "workload_facts.json"
SCRATCH_DIALECT = "duckdb"  # the scratch SQL engine's sqlglot dialect


# ---- derived names -----------------------------------------------------------------------------

def bench_pascal(benchmark: str) -> str:
    return benchmark.title().replace("_", "")


def base_function_name(benchmark: str, query: Any) -> str:
    return f"accel_{benchmark}_q{query}"


def aux_function_name(benchmark: str, query: Any) -> str:
    return f"{base_function_name(benchmark, query)}_aux"


def accelerator_id(benchmark: str, query: Any) -> str:
    return f"{benchmark}_q{query}_aux"


def header_basename(benchmark: str, query: Any) -> str:
    return f"{benchmark}_q{query}"


def unit_test_target(benchmark: str, query: Any) -> str:
    return f"test_{benchmark}_q{query}"


def benchmark_target(benchmark: str, query: Any) -> str:
    return f"benchmark_{benchmark}_q{query}"


def smoke_ctest_name(benchmark: str, query: Any) -> str:
    return f"{benchmark}_q{query}_smoke_test"


def aux_smoke_ctest_name(benchmark: str, query: Any) -> str:
    return f"{benchmark}_q{query}_aux_smoke_test"


def hybrid_benchmark_target(benchmark: str, query: Any) -> str:
    return f"benchmark_{benchmark}_q{query}_hybrid_eval"


def registration_symbol(pattern: str, benchmark: str, query: Any) -> str:
    """Expand a profile's registration-symbol pattern: {Bench} is the CamelCase benchmark, {N} the query."""
    return pattern.replace("{Bench}", bench_pascal(benchmark)).replace("{N}", str(query))


# ---- the engine --------------------------------------------------------------------------------

def sanitize_engine_name(name: str) -> str:
    """Normalize engine_name to a filesystem / C++ identifier fragment."""
    cleaned = "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in str(name).strip().lower())
    cleaned = "_".join(part for part in cleaned.split("_") if part)
    if not cleaned or not cleaned[0].isalpha():
        raise ValueError(f"invalid engine_name: {name!r} (need a letter-leading identifier)")
    return cleaned


def workload_engines(workload: dict[str, Any]) -> list[str]:
    """Engines a workload supports: its `engines:` list."""
    declared = workload.get("engines")
    if not declared:
        raise ValueError(f"workload {workload.get('query_id', '?')!r} declares no `engines:`")
    return [sanitize_engine_name(e) for e in declared]


def select_engine(workload: dict[str, Any], requested: str, name: str) -> str:
    """The run's engine: `requested`, or the workload's only engine; else an error."""
    supported = workload_engines(workload)
    engine = sanitize_engine_name(requested) if requested else ""
    if not engine and len(supported) == 1:
        engine = supported[0]
    if not engine:
        raise ValueError(f"workload {name!r} supports engines {supported}: pick one (--engine / pack `engines:`)")
    if engine not in supported:
        raise ValueError(f"workload {name!r} does not support engine {engine!r} (supports {supported})")
    return engine


def harness_targets(engine: str, bench: str) -> dict[str, str]:
    """The workload-harness targets every backend provides (one naming convention for all engines)."""
    return {
        "differential_binary": f"test_{engine}_{bench}_differential",
        "multi_conn_target": f"test_{engine}_aux_multi_connection_stress",
        "cross_accel_target": f"test_{engine}_cross_accel",
        "bench_target": f"benchmark_{engine}_{bench}_aux",
        "calibrate_target": f"calibrate_{engine}_{bench}_aux_cost_model",
    }


def engine_host_conventions(
    engine_name: str, bench: str = "<bench>", query: Any = "<N>", repo_root: Path | None = None
) -> dict[str, Any]:
    """Host registration / harness surfaces of an engine, derived from its profile.

    Each query registers from its own directory against the registrars listed in the
    engine's api/README.md (engine backends name no query). The workload harnesses are
    generated by host_bootstrap under backends/<engine>/tests/harness/ and bench/.
    """
    engine = sanitize_engine_name(engine_name)
    query_host = load_engine_profile(engine, pipeline_root(repo_root or find_repo_root()))["query_host"]
    # The layout's registrar files (generated_layout.md): the base kernel's and the aux module's.
    register = f"accelerators/{bench}/q{query}/{engine}/base/single-threaded/register.cpp"
    aux_register = f"accelerators/{bench}/q{query}/{engine}/aux/register.cpp"
    targets = harness_targets(engine, bench)
    diff = targets["differential_binary"]
    return {
        "engine_name": engine,
        "register_path": register,
        "aux_register_path": aux_register,
        "registry": f"the registration role of backends/{engine}/api/README.md",
        "sql_entrypoint_convention": (
            f"Host-native SQL entrypoints for {engine}, registered from the query's own directory "
            f"accelerators/{bench}/q{query}/{engine}/ through the backend's registrars "
            f"(backends/{engine}/api/README.md). Never edit the backend's shared files to add a query."
        ),
        "dataset_env": query_host["dataset_env"],
        "work_db_suffix": query_host["work_db_suffix"],
        "smoke_ready": True,
        **targets,
        "differential_source": f"backends/{engine}/tests/harness/test_{engine}_differential.cpp",  # one source, a target per benchmark
        "differential_command": f"host.sh run {diff} <query>",
        "query_host_ready_hint": (
            f"The {engine} workload harnesses ({', '.join(targets.values())}) are generated by the "
            f"host_bootstrap stage under backends/{engine}/tests/harness/ and backends/{engine}/bench/; "
            f"a query only registers from `{register}`. See pipeline/prompts/shared/query_host_bootstrap.md."
        ),
    }


class EngineProfileError(ValueError):
    """The workload's engine_profile is missing or does not match the schema."""


def _is_na(value: Any) -> bool:
    return isinstance(value, dict) and set(value) == {"na"}


def validate_engine_profile(profile: Any, pipeline_root: Path, *, source: str) -> None:
    if not isinstance(profile, dict):
        raise EngineProfileError(f"{source}: engine_profile is required (see schemas/engine_profile.schema.json)")
    schema = json.loads((pipeline_root / SCHEMA_RELATIVE).read_text(encoding="utf-8"))
    errors = sorted(
        jsonschema.Draft202012Validator(schema).iter_errors(profile),
        key=lambda e: [str(p) for p in e.absolute_path],
    )
    if errors:
        lines = [f"- {'.'.join(str(p) for p in e.absolute_path) or '<root>'}: {_describe(e)}" for e in errors[:20]]
        more = f"\n- ... {len(errors) - 20} more" if len(errors) > 20 else ""
        raise EngineProfileError(f"{source}: invalid engine_profile\n" + "\n".join(lines) + more)


def _describe(error: jsonschema.ValidationError) -> str:
    if error.validator == "required":
        return f"missing key {error.message.split(' ')[0]}"
    if error.validator in ("oneOf", "anyOf"):
        return "needs a value or {na: reason}"
    return error.message.splitlines()[0][:160]


def load_engine_profile(engine: str, pipeline_root: Path) -> dict[str, Any]:
    """The validated profile of `engine` from its engine_backend workload."""
    path = pipeline_root / "config" / "workloads" / f"engine_{engine}.yaml"
    if not path.is_file():
        raise EngineProfileError(f"no engine profile for {engine!r}: {path} is missing")
    profile = (yaml.safe_load(path.read_text(encoding="utf-8")) or {}).get("engine_profile")
    validate_engine_profile(profile, pipeline_root, source=str(path))
    return profile


def registration_symbol_pattern(profile: dict[str, Any]) -> str:
    """The profile's per-query registration symbol pattern; empty when the engine has none."""
    pattern = profile["entry_points"]["registration_symbol"]
    return "" if _is_na(pattern) else pattern


def host_runtime_of(profile: dict[str, Any]) -> dict[str, Any]:
    """The profile's host_runtime without its n/a leaves."""
    return {key: value for key, value in profile["host_runtime"].items() if not _is_na(value)}


def _render_value(value: Any) -> str:
    if _is_na(value):
        return f"n/a ({value['na']})"
    if isinstance(value, list):
        return "; ".join(str(v) for v in value)
    if isinstance(value, dict):
        return ", ".join(f"{k}={v}" for k, v in value.items())
    return str(value)


def render_engine_profile(
    profile: dict[str, Any],
    sections: list[str] | str,
    *,
    layer: str | None = None,
    host_layer_runs: bool = True,
) -> str:
    """Markdown of the requested sections; hazards are narrowed to `layer` when given.

    Without a host layer (embedded engines) the entry points are built in the runtime layer,
    which then also sees the hazards tagged `host`.
    """
    wanted = list(profile) if sections == ALL_SECTIONS else list(sections)
    out: list[str] = []
    for name in wanted:
        if name == "hazards":
            continue
        out.append(f"### {name}\n")
        out.extend(f"- **{key}:** {_render_value(value)}" for key, value in profile[name].items())
        out.append("")
    if "hazards" in wanted:
        shown = {layer, "host"} if layer == "runtime" and not host_layer_runs else {layer}
        hazards = [h for h in profile["hazards"] if layer is None or shown & set(h["layers"])]
        out.append("### hazards (engine facts, not instructions)\n")
        out.extend(
            f"- *{', '.join(h['layers'])}* — **{h['symptom']}** Engine fact: {h['engine_fact']}" for h in hazards
        )
        out.append("")
    return "\n".join(out).rstrip() + "\n"


# ---- loading a workload ------------------------------------------------------------------------

def load_aux_state_defaults(repo_root: Path | None = None) -> dict[str, Any]:
    return config_yaml(repo_root or find_repo_root(), "aux_state")


def load_engine_backend_defaults(repo_root: Path | None = None) -> dict[str, Any]:
    return config_yaml(repo_root or find_repo_root(), "engine_backend")


def load_benchmark_pack(name: str, repo_root: Path | None = None) -> dict[str, Any]:
    path = pipeline_root(repo_root or find_repo_root()) / "config" / "benchmark_packs" / f"{name}.yaml"
    if not path.is_file():
        raise FileNotFoundError(f"benchmark pack not found: {path}")
    return load_yaml(path)


_TPCH_WORKLOAD = re.compile(r"tpch_q(\d+)(_aux)?")


def _read_workload(root: Path, name: str) -> dict[str, Any] | None:
    """workloads/<name>.yaml, or a TPC-H query workload built from its template and tpch_queries.yaml; None when neither exists."""
    workloads = pipeline_root(root) / "config" / "workloads"
    if (workloads / f"{name}.yaml").is_file():
        return load_yaml(workloads / f"{name}.yaml")
    tpch = _TPCH_WORKLOAD.fullmatch(name)
    queries = load_yaml(workloads / "tpch_queries.yaml") if (workloads / "tpch_queries.yaml").is_file() else {}
    if not tpch or int(tpch[1]) not in queries:
        return None
    q, aux = int(tpch[1]), bool(tpch[2])
    entry = queries[q]
    return {
        **load_yaml(workloads / ("tpch_q_aux.template.yaml" if aux else "tpch_q.template.yaml")),
        "query_number": q,
        "reference_sql_path": f"workloads/tpch/instances/seed_1/{q:02d}.sql",
        "parameters": [{"name": n, "type": t} for n, t in entry["parameters"].items()],
        **((entry.get("aux") or {}) if aux else {}),
    }


def _read_workload_yaml(name: str, repo_root: Path | None) -> tuple[dict[str, Any], Path]:
    """The workload and the tree it came from: pack worktrees may lack untracked workloads, so the primary tree comes first."""
    primary = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    for root in ((Path(primary).resolve(),) if primary else ()) + (repo_root or find_repo_root(),):
        data = _read_workload(root, name)
        if data is not None:
            return data, root
    raise FileNotFoundError(f"workload not found: {name}")


def workload_supported_engines(name: str, repo_root: Path | None = None) -> list[str]:
    data, _ = _read_workload_yaml(name, repo_root)
    return workload_engines(data)


def load_workload(name: str, repo_root: Path | None = None, *, engine: str = "") -> dict[str, Any]:
    """Load a workload; accelerator workloads are engine-neutral and need `engine` (see select_engine)."""
    data, root = _read_workload_yaml(name, repo_root)
    if data.get("workload_kind") == "engine_backend":
        return _normalize_engine_backend_workload(data, root)

    engine = select_engine(data, engine, name)
    try:
        pack = load_benchmark_pack(data.get("benchmark_name", "tpch"), root)
        data = _merge_benchmark_pack(data, pack, root, load_pipeline_config(repo_root=root).datasets)
    except FileNotFoundError:
        data.setdefault("benchmark_name", "tpch")
        data.setdefault("artifact_namespace", "tpch")

    if data.get("workload_kind") == "aux_state":
        _merge_aux_defaults(data, root)
    _expand_fairness_search_paths(data, engine)
    _derive_query_names(data, engine, root)
    return _pin_engine(data, engine, root)


def load_run_workload(pipeline_config: PipelineConfig, name: str) -> dict[str, Any]:
    """`name` loaded for the engine of the run `pipeline_config` belongs to."""
    return load_workload(name, pipeline_config.repo_root, engine=pipeline_config.engine_name)


def _merge_benchmark_pack(workload: dict[str, Any], pack: dict[str, Any], repo_root: Path, datasets: dict[str, Path]) -> dict[str, Any]:
    merged = {**pack, **workload}
    benchmark = str(merged.get("benchmark_name", "tpch"))
    merged.setdefault("artifact_namespace", benchmark)
    merged.setdefault("display_name", benchmark)
    for key, target in (("dataset_key", "dataset_db"), ("scale_dataset_key", "scale_db")):
        if merged.get(key) in datasets:
            merged.setdefault(target, str(datasets[merged[key]]))
    for key in ("reference_sql_path", "schema_reference_path", "test_template_path", "benchmark_template_path"):
        if merged.get(key):
            merged[key] = str(_resolve_path(repo_root, merged[key]))
    return merged


# What aux_state.yaml sets on every aux workload, over the benchmark pack's values.
_AUX_OVERRIDES = (
    "post_implementation_gates", "post_optimization_gates", "post_audit_gates", "prompt_subdir",
    "analysis_critique_max_rounds", "aux_bench_sweep_optimization", "aux_bench_sweep_quick",
)


def _merge_aux_defaults(workload: dict[str, Any], repo_root: Path) -> None:
    aux = load_aux_state_defaults(repo_root)
    bench = config_yaml(repo_root, f"{workload.get('benchmark_name', 'tpch')}_aux_state")

    workload.update({key: copy.deepcopy(aux[key]) for key in _AUX_OVERRIDES if key in aux})
    if "fairness" in aux:
        workload["fairness"] = {**(workload.get("fairness") or {}), **copy.deepcopy(aux["fairness"])}
    for key in ("write_paths", "allowed_commands"):
        workload[key] = [*(aux.get(key) or []), *(bench.get(key) or []), *(workload.get(key) or [])]
    workload["stage_max_turns"] = {**aux.get("stage_max_turns", {}), **(workload.get("stage_max_turns") or {})}
    sweep = {**(bench.get("aux_bench_sweep") or aux.get("aux_bench_sweep") or {}), **(workload.get("aux_bench_sweep") or {})}
    if sweep:
        workload["aux_bench_sweep"] = sweep


def _expand_fairness_search_paths(workload: dict[str, Any], engine: str) -> None:
    """Expand {bench}/{engine} in `fairness.kernel_search_paths` for the run."""
    fairness = workload.get("fairness")
    if isinstance(fairness, dict) and fairness.get("kernel_search_paths"):
        bench = str(workload.get("benchmark_name", "tpch"))
        fairness["kernel_search_paths"] = [
            str(p).replace("{bench}", bench).replace("{engine}", engine) for p in fairness["kernel_search_paths"]
        ]


def _derive_query_names(data: dict[str, Any], engine: str, root: Path) -> None:
    """The query's names (py), plus the engine profile's registration symbol."""
    bench, q = str(data.get("benchmark_name", "tpch")), data.get("query_number")
    if q is None:
        return
    pattern = registration_symbol_pattern(load_engine_profile(engine, pipeline_root(root)))
    data.update(
        function_name=base_function_name(bench, q),
        create_fn=registration_symbol(pattern, bench, q) if pattern else "",
        header_basename=header_basename(bench, q),
        test_target=unit_test_target(bench, q),
        ctest_smoke_regex=f"^{smoke_ctest_name(bench, q)}$",
    )
    if data.get("workload_kind") == "aux_state":
        data.update(
            aux_function_name=aux_function_name(bench, q),
            accelerator_id=accelerator_id(bench, q),
        )


def _pin_engine(data: dict[str, Any], engine: str, repo_root: Path) -> dict[str, Any]:
    """Pin the run's engine, attach its host conventions and let the run own the query's directory."""
    data["engine_name"] = engine
    bench = str(data.get("benchmark_name") or data.get("benchmark") or "tpch")
    qnum = data.get("query_number")
    data["engine_host"] = engine_host_conventions(engine, bench, qnum if qnum is not None else "<N>", repo_root)
    # backends/<engine>/** belongs to the engine_backend run and substrate/** to nobody (layout enforces both).
    own_dir = [query_glob(bench, qnum)] if qnum is not None else []
    data["write_paths"] = _dedupe([*(data.get("write_paths") or []), *own_dir])
    return data


def _dedupe(items) -> list[str]:
    return list(dict.fromkeys(items))


def _expand_engine(value: str, engine: str) -> str:
    return str(value).replace("{engine}", engine)


def _normalize_engine_backend_workload(data: dict[str, Any], repo_root: Path) -> dict[str, Any]:
    """Fill the conventional paths, gates and write allowlists of an engine_backend workload from engine_backend.yaml."""
    defaults = load_engine_backend_defaults(repo_root)
    engine = sanitize_engine_name(str(data.get("engine_name") or ""))
    data["engine_name"] = engine
    data.setdefault("benchmark_name", "engine")
    data.setdefault("artifact_namespace", f"engines/{engine}")
    data.setdefault("query_number", 0)

    profile = data.get("engine_profile")
    validate_engine_profile(profile, pipeline_root(repo_root), source=f"workload {engine}")
    if profile["identity"]["name"] != engine:
        raise EngineProfileError(f"workload {engine}: engine_profile.identity.name is {profile['identity']['name']!r}")
    data["display_name"] = profile["identity"]["display_name"]
    data["host_runtime"] = host_runtime_of(profile)
    data["requires_in_process_host"] = profile["packaging"]["requires_in_process_host"]
    data["placement_candidates"] = list(profile["packaging"]["placement_candidates"])

    merged_below = ("write_paths", "allowed_commands", "default_target_paths", "default_cmake")
    for key, value in defaults.items():
        if key not in merged_below:
            data.setdefault(key, copy.deepcopy(value))

    data["implementation_layers"] = [
        {**layer, **{k: _expand_engine(layer[k], engine) for k in ("harness", "cmake_target", "title", "gate", "id") if k in layer}}
        for layer in data.get("implementation_layers") or []
    ]
    data["target_paths"] = {
        **{k: _expand_engine(v, engine) for k, v in (defaults.get("default_target_paths") or {}).items()},
        **{str(k): str(v) for k, v in (data.get("target_paths") or {}).items()},
    }
    data["cmake"] = {
        **{k: _expand_engine(v, engine) for k, v in (defaults.get("default_cmake") or {}).items()},
        **{str(k): str(v) for k, v in (data.get("cmake") or {}).items()},
        "build_flag": profile["build"]["cmake_option"],
    }
    # The engine's run owns backends/<engine>/** (incl. api/catalog/), whatever the YAML lists.
    data["write_paths"] = _dedupe([
        backend_glob(engine),
        *(_expand_engine(p, engine) for p in [*(defaults.get("write_paths") or []), *(data.get("write_paths") or [])]),
    ])
    data["allowed_commands"] = _dedupe(
        _expand_engine(c, engine) for c in [*(defaults.get("allowed_commands") or []), *(data.get("allowed_commands") or [])]
    )
    return data


# ---- where a workload's query lives ------------------------------------------------------------

def _resolve_path(repo_root: Path, value: str | Path) -> Path:
    path = Path(value)
    return path if path.is_absolute() else (repo_root / path).resolve()


def _dataset_path(path: str | Path) -> str:
    """A dataset database path; empty when the run parameter is unset."""
    return "" if Path(path) == Path() else str(path)


def resolve_workload_paths(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> dict[str, str]:
    benchmark = str(workload.get("benchmark_name", "tpch"))
    q = int(workload.get("query_number", query))
    kind = workload.get("workload_kind")
    engine = sanitize_engine_name(str(workload.get("engine_name") or ""))
    if not engine and kind != "engine_backend":
        raise ValueError("resolve_workload_paths: accelerator workload missing engine_name")
    # Per-engine query tree: accelerators/{bench}/q{N}/{engine}/... (source_subdir is
    # relative to accelerators/, the second include root).
    qdir = query_dir(benchmark, q)
    eng_seg = f"{engine}/" if engine else ""
    source_subdir = str(workload.get("source_subdir", f"{benchmark}/q{q}/{eng_seg}base/single-threaded"))
    generated_base = f"{qdir}/{eng_seg}base/single-threaded"
    # The base kernels' bench: one query-parameterised target per engine for the benchmarks listed in
    # shared_bench_harness, else one per query. (Aux targets are always the host_bootstrap harness's.)
    shared_harness = bool(engine) and kind != "engine_backend" and benchmark in pipeline_config.shared_bench_harness
    bench_target = f"benchmark_{engine}_{benchmark}_q{q}" if shared_harness else benchmark_target(benchmark, q)
    test_target = unit_test_target(benchmark, q)

    paths = {
        "source_subdir": source_subdir,
        "function_name": base_function_name(benchmark, q),
        "create_fn": str(workload.get("create_fn", "")),
        "test_target": test_target,
        "bench_target": bench_target,
        "ctest_smoke_name": smoke_ctest_name(benchmark, q),
        "ctest_smoke_regex": f"^{smoke_ctest_name(benchmark, q)}$",
        # the benchmark's own datasets (its pack's dataset_key), never another benchmark's
        "dataset_db": _dataset_path(workload.get("dataset_db", "")),
        "scale_db": _dataset_path(workload.get("scale_db", "")),
        "db_env_var": str(workload.get("db_env_var", f"ACCEL_{benchmark.upper()}_DB")),
        "header_path": f"{qdir}/{eng_seg}base/query.hpp",
        "kernel_path": f"{generated_base}/kernel.cpp",
        "package_path": f"{generated_base}/package.cpp",
        "fused_scan_path": f"{generated_base}/fused_scan.cpp",
        # Where the query's cost models live (AuxCostModelLocation), for every engine.
        "cost_model_header_path": f"{qdir}/cost_model.hpp",
        "test_path": f"{qdir}/tests/{test_target}.cpp",
        "benchmark_path": f"{qdir}/bench/benchmark_q{q}.cpp",
        "engine_name": engine,
    }

    if kind == "aux_state":
        aux_state_subdir = f"{qdir}/{eng_seg}aux"
        cost_model_stem = f"q{q}_aux_cost_model"  # the query's own config dir already names the benchmark
        bench_sweep = workload.get("aux_bench_sweep", {})
        if not isinstance(bench_sweep, dict):
            bench_sweep = {}
        paths.update(
            {
                "aux_function_name": aux_function_name(benchmark, q),
                "aux_test_target": f"{test_target}_aux",
                # host_bootstrap makes every engine's harnesses for every benchmark (benchmark-free sources)
                "aux_bench_target": harness_targets(engine, benchmark)["bench_target"],
                "aux_hybrid_bench_target": hybrid_benchmark_target(benchmark, q),
                "aux_calibrate_target": harness_targets(engine, benchmark)["calibrate_target"],
                "aux_ctest_smoke_regex": f"^{aux_smoke_ctest_name(benchmark, q)}$",
                "aux_state_subdir": aux_state_subdir,
                # The query-wide default model (cost_model.hpp's location); an engine with its
                # own calibration keeps it beside the default as <stem>_<engine>.json.
                "cost_model_default": f"{qdir}/config/{cost_model_stem}.json",
                "cost_model_config": f"{qdir}/config/{cost_model_stem}_{engine}.json",
                "aux_bench_work_dir_suffix": str(workload.get("aux_bench_work_dir_suffix", f"q{q}_aux_bench")),
                "aux_bench_suites": str(bench_sweep.get("suites") or workload.get("aux_bench_suites") or ""),
            }
        )
    return paths


def resolve_optimization_edit_paths(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> dict[str, Any]:
    """Repo-relative paths the optimization loop may edit and profile."""
    paths = resolve_workload_paths(pipeline_config, workload, query)
    if workload.get("workload_kind") == "aux_state":
        aux = paths["aux_state_subdir"]
        all_paths = [f"{aux}/module.hpp", f"{aux}/module.cpp", f"{aux}/finalize.cpp"]
        if paths.get("cost_model_config"):
            all_paths.append(paths["cost_model_config"])
        return {
            "primary_cpp": f"{aux}/module.cpp",
            "header": f"{aux}/module.hpp",
            "secondary_cpp": f"{aux}/finalize.cpp",
            "all_paths": all_paths,
            "is_aux": True,
        }

    all_paths = [paths["header_path"], paths["kernel_path"], paths["package_path"]]
    if paths.get("fused_scan_path"):
        all_paths.append(paths["fused_scan_path"])
    return {
        "primary_cpp": paths["kernel_path"],
        "header": paths["header_path"],
        "secondary_cpp": paths["package_path"],
        "all_paths": all_paths,
        "is_aux": False,
    }


def workload_benchmark_name(workload: dict[str, Any] | None) -> str:
    if not workload:
        return "tpch"
    return str(workload.get("artifact_namespace") or workload.get("benchmark_name") or "tpch")


def query_artifacts_dir(
    repo_root: Path,
    query_number: int,
    *,
    workload: dict[str, Any] | None = None,
    benchmark_name: str | None = None,
) -> Path:
    """artifacts/<bench>/q<N>/<engine>/ for accelerator runs (one per query x engine)."""
    if workload and workload.get("workload_kind") == "engine_backend":
        engine = str(workload.get("engine_name") or workload.get("artifact_namespace") or "engine")
        path = pipeline_root(repo_root) / "artifacts" / "engines" / engine
        path.mkdir(parents=True, exist_ok=True)
        return path

    bench = benchmark_name or workload_benchmark_name(workload)
    path = pipeline_root(repo_root) / "artifacts" / bench / f"q{query_number}"
    engine = str((workload or {}).get("engine_name") or "")
    if engine:
        path = path / engine
    path.mkdir(parents=True, exist_ok=True)
    return path


def engine_artifacts_dir(repo_root: Path, engine_name: str) -> Path:
    path = pipeline_root(repo_root) / "artifacts" / "engines" / engine_name
    path.mkdir(parents=True, exist_ok=True)
    return path


def gates_dir_for_query(artifacts_dir: Path) -> Path:
    return artifacts_dir / "gates"


def analysis_artifact_name(workload: dict[str, Any] | None) -> str:
    """Aux-state workloads use analysis.json; base accelerators use base_analysis.json."""
    if workload and workload.get("workload_kind") == "aux_state":
        return "analysis.json"
    return "base_analysis.json"


def analysis_artifact_path(
    repo_root: Path,
    query_number: int,
    *,
    workload: dict[str, Any] | None = None,
    benchmark_name: str | None = None,
) -> Path:
    art = query_artifacts_dir(repo_root, query_number, workload=workload, benchmark_name=benchmark_name)
    return art / analysis_artifact_name(workload)


def load_query_analysis(
    repo_root: Path,
    query_number: int,
    *,
    workload: dict[str, Any] | None = None,
) -> tuple[Path, dict[str, Any]]:
    art = query_artifacts_dir(repo_root, query_number, workload=workload)
    path = art / analysis_artifact_name(workload)
    if not path.is_file():
        return path, {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return path, {}
    return path, data if isinstance(data, dict) else {}


def read_json(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {}
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {}


# ---- facts of the query's SQL ------------------------------------------------------------------


def _schema_columns(con: duckdb.DuckDBPyConnection) -> dict[str, dict[str, str]]:
    rows = con.execute(
        "SELECT table_name, column_name FROM information_schema.columns ORDER BY table_name, ordinal_position"
    ).fetchall()
    schema: dict[str, dict[str, str]] = {}
    for table, column in rows:
        schema.setdefault(table, {})[column] = "TEXT"
    return schema


def _output_schema(con: duckdb.DuckDBPyConnection, sql: str) -> list[dict[str, str]]:
    described = con.execute("DESCRIBE " + sql).fetchall()
    return [{"name": name, "type": re.sub(r"\(.*", "", dtype)} for name, dtype, *_ in described]


def _exists_probes_read_no_columns(tree: exp.Expression) -> None:
    """`EXISTS (SELECT * ...)` reads no column of its source; qualify would expand the star."""
    for exists in tree.find_all(exp.Exists):
        for select in exists.find_all(exp.Select):
            if any(isinstance(e, exp.Star) for e in select.expressions):
                select.set("expressions", [exp.Literal.number(1)])


def _dependent_tables(sql: str, schema: dict[str, dict[str, str]]) -> list[dict[str, Any]]:
    tree = sqlglot.parse_one(sql, read=SCRATCH_DIALECT)
    _exists_probes_read_no_columns(tree)
    ctes = {cte.alias for cte in tree.find_all(exp.CTE)}
    appearance = list(dict.fromkeys(t.name for t in tree.find_all(exp.Table) if t.name not in ctes))
    qualified = qualify(tree, schema=schema, dialect=SCRATCH_DIALECT, validate_qualify_columns=False)
    read: dict[str, set[str]] = {}
    for scope in traverse_scope(qualified):
        for column in scope.columns:
            source = scope.sources.get(column.table)
            if isinstance(source, exp.Table):
                read.setdefault(source.name, set()).add(column.name)
    return [
        {"name": table, "watched_columns": [c for c in schema[table] if c in read.get(table, ())]}
        for table in appearance
    ]


def scratch_connection(path: str = ":memory:", read_only: bool = False) -> Any:
    """The embedded SQL engine the orchestrator derives facts and test subsets with."""
    return duckdb.connect(path, read_only=read_only)


def derive_query_facts(sql: str, schema_sql: str) -> dict[str, Any]:
    """output_schema and dependent_tables (with watched_columns) of `sql` over `schema_sql`."""
    sql = sql.strip().rstrip(";")
    con = scratch_connection()
    try:
        con.execute(schema_sql)
        return {
            "output_schema": _output_schema(con, sql),
            "dependent_tables": _dependent_tables(sql, _schema_columns(con)),
        }
    finally:
        con.close()


def workload_facts(repo: Path, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """The run's derived facts, written to the run's artifacts (workload_facts.json)."""
    benchmark = str(workload.get("benchmark_name", "tpch"))
    sql = Path(workload["reference_sql_path"]).read_text(encoding="utf-8")
    schema_sql = (repo / workload_schema_sql(benchmark)).read_text(encoding="utf-8")
    facts = derive_query_facts(sql, schema_sql)
    path = query_artifacts_dir(repo, query, workload=workload) / FACTS_FILE
    text = json.dumps(facts, indent=2) + "\n"
    if not path.is_file() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")
    return facts


def with_facts(repo: Path, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """`workload` plus its derived output_schema and dependent_tables."""
    return {**workload, **workload_facts(repo, workload, query)}


# ---- a workload's refinement of the run's settings ---------------------------------------------

def apply_workload_overrides(pipeline_config: PipelineConfig, workload: dict[str, Any]) -> PipelineConfig:
    kind = str(workload.get("workload_kind") or "")
    engine = sanitize_engine_name(str(workload["engine_name"])) if workload.get("engine_name") else ""
    if kind != "engine_backend" and not engine:
        raise ValueError("accelerator workload requires explicit engine_name (no default engine)")

    allowed = list(workload.get("allowed_commands") or [])
    # The engine's differential harness, built and run by the accelerator run (its source is the
    # backend's, made by host_bootstrap).
    binary = (workload.get("engine_host") or {}).get("differential_binary")
    if kind != "engine_backend" and binary:
        binary = re.escape(binary)
        allowed += [
            rf"^(\\.\\/)?build-release\\/{binary}( .*)?$",
            rf"^cmake --build build-release --target {binary}( .*)?$",
            rf"^ACCEL_[A-Z_]+=\\S+( ACCEL_[A-Z_]+=\\S+)* (\\.\\/)?build-release\\/{binary}( .*)?$",
        ]
    return replace(
        pipeline_config,
        **optimization_settings(workload.get("optimization") or {}, "workload optimization"),
        stage_max_turns={**pipeline_config.stage_max_turns, **(workload.get("stage_max_turns") or {})},
        write_paths=[*pipeline_config.write_paths, *(workload.get("write_paths") or [])],
        allowed_commands=[*pipeline_config.allowed_commands, *allowed],
        workload_kind=kind,
        engine_name=engine,
        benchmark_name=str(workload.get("benchmark_name") or ("" if kind == "engine_backend" else "tpch")),
    )
