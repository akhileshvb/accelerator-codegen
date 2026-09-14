from __future__ import annotations

import copy
import os
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.paths import find_repo_root, pipeline_root
from accel_pipeline.shared_file_lease import DEFAULT_SHARED_PATH_GLOBS


@dataclass
class ModelConfig:
    provider: str
    api_key_env: str
    presets: dict[str, str]
    stages: dict[str, str]
    openrouter_base_url: str = "https://openrouter.ai/api/v1"
    stage_providers: dict[str, str] = field(default_factory=dict)
    codex_bin: str = "codex"
    codex_config_path: str = "pipeline/.codex/config.toml"
    codex_timeout_sec: int = 7200
    codex_seed_max_chars: int = 120_000
    codex_seed_preserve_recent_cycles: int = 2
    # Headless codex exec cancels MCP tools without this (openai/codex#24135).
    codex_bypass_approvals_and_sandbox: bool = True


@dataclass
class PipelineConfig:
    repo_root: Path
    pipeline_root: Path
    build_dir: Path
    tpch_db_sf1: Path
    tpch_db_sf10: Path
    datasets: dict[str, Path] = field(default_factory=dict)
    optimization_max_iterations: int = 3
    optimization_no_improvement_stop: int = 4
    optimization_episodes_per_iteration: int = 4
    optimization_max_attempts_per_iteration: int = 4
    optimization_max_turns_per_episode: int = 8
    optimization_improvement_threshold_pct: float = 3.0
    optimization_reject_threshold_pct: float = 2.0
    optimization_consecutive_fail_stop: int = 0
    optimization_quick_benchmark_samples: int = 3
    optimization_confirmation_samples: int = 3
    optimization_context_max_chars: int = 80_000
    optimization_context_max_lines: int = 1200
    optimization_bootstrap_max_chars: int = 300_000
    optimization_bootstrap_max_lines_per_file: int = 0
    optimization_skip_per_attempt_correctness: bool = True
    optimization_accept_on_screen: bool = True
    optimization_skip_confirmation: bool = True
    optimization_smoke_runs: int = 2
    optimization_max_turns_analysis: int = 0
    optimization_max_turns_patch: int = 0
    optimization_max_turns_unified: int = 0
    optimization_attempt_budgets: dict[str, int] = field(default_factory=dict)
    optimization_patch_budgets: dict[str, int] = field(default_factory=dict)
    optimization_build_fix_max_rounds: int = 0
    optimization_correctness_fix_max_rounds: int = 0
    optimization_build_fix_safety_cap: int = 12
    optimization_correctness_fix_safety_cap: int = 12
    optimization_fix_log_chars: int = 6000
    optimization_persistent_worker: bool = True
    optimization_worker_max_turns_per_candidate: int = 0
    optimization_worker_safety_cap_turns: int = 80
    optimization_openai_auto_previous_response_id: bool = True
    optimization_openrouter_history_max_chars: int = 120_000
    optimization_openrouter_preserve_recent_cycles: int = 2
    optimization_openrouter_tool_output_summary_chars: int = 80
    optimization_hot_path_digest_max_chars: int = 25_000
    optimization_turn_delta_max_chars: int = 4_000
    optimization_checkpoint_file: str = "optimization_worker_checkpoint.json"
    # Agent may declare OPTIMIZATION_SATURATED when stuck (no numeric floors).
    optimization_early_stop: dict[str, Any] = field(default_factory=dict)
    crash_debug_max_rounds: int = 0
    crash_debug_safety_cap: int = 25
    stage_max_turns: dict[str, int] = field(default_factory=dict)
    write_paths: list[str] = field(default_factory=list)
    allowed_commands: list[str] = field(default_factory=list)
    gates_dir: Path = field(default_factory=Path)
    # Concurrent pack workers (shared tree + separate builds).
    worker_id: str = ""
    codex_home: Path | None = None
    shared_paths: list[str] = field(default_factory=list)


def _load_yaml(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return data or {}


def load_benchmark_pack(name: str, repo_root: Path | None = None) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    path = pipeline_root(root) / "config" / "benchmark_packs" / f"{name}.yaml"
    if not path.is_file():
        raise FileNotFoundError(f"benchmark pack not found: {path}")
    return _load_yaml(path)


def _resolve_path(repo_root: Path, value: str | Path) -> Path:
    path = Path(value)
    if path.is_absolute():
        return path
    return (repo_root / path).resolve()


def _merge_pack_into_workload(
    workload: dict[str, Any],
    pack: dict[str, Any],
    repo_root: Path,
    pipeline_config: PipelineConfig | None = None,
) -> dict[str, Any]:
    merged: dict[str, Any] = dict(pack)
    merged.update(workload)

    benchmark = str(merged.get("benchmark_name", "tpch"))
    merged.setdefault("artifact_namespace", benchmark)
    merged.setdefault("display_name", benchmark)

    query_num = merged.get("query_number")
    if query_num is not None:
        q = int(query_num)
        header_prefix = str(merged.get("header_prefix", f"{benchmark}_q"))
        fn_prefix = str(merged.get("function_prefix", f"accel_{benchmark}_q"))
        create_prefix = str(merged.get("create_fn_prefix", f"Create{benchmark.title().replace('_', '')}Q"))
        test_prefix = str(merged.get("test_target_prefix", f"test_{benchmark}_q"))
        bench_prefix = str(merged.get("bench_target_prefix", f"benchmark_{benchmark}_q"))

        merged.setdefault("function_name", f"{fn_prefix}{q}")
        merged.setdefault("create_fn", f"{create_prefix}{q}FunctionSet")
        merged.setdefault("test_target", f"{test_prefix}{q}")
        merged.setdefault("bench_target", f"{bench_prefix}{q}")
        merged.setdefault("ctest_smoke_name", f"{benchmark}_q{q}{merged.get('ctest_smoke_suffix', '_smoke_test')}")
        merged.setdefault("ctest_smoke_regex", f"^{benchmark}_q{q}{merged.get('ctest_smoke_suffix', '_smoke_test')}$")
        merged.setdefault("header_basename", f"{header_prefix}{q}")

    dataset_key = merged.get("dataset_key")
    if dataset_key and pipeline_config and dataset_key in pipeline_config.datasets:
        merged.setdefault("dataset_db", str(pipeline_config.datasets[dataset_key]))
    elif "dataset_db" in pack and "dataset_db" not in workload:
        merged["dataset_db"] = str(_resolve_path(repo_root, pack["dataset_db"]))

    scale_key = merged.get("scale_dataset_key")
    if scale_key and pipeline_config and scale_key in pipeline_config.datasets:
        merged.setdefault("scale_db", str(pipeline_config.datasets[scale_key]))

    ref_sql = merged.get("reference_sql_path")
    if ref_sql:
        merged["reference_sql_path"] = str(_resolve_path(repo_root, ref_sql))

    for key in ("schema_reference_path", "test_template_path", "benchmark_template_path"):
        if key in merged and merged[key]:
            merged[key] = str(_resolve_path(repo_root, merged[key]))

    return merged


def resolve_workload_paths(
    pipeline_config: PipelineConfig,
    workload: dict[str, Any],
    query: int,
) -> dict[str, str]:
    repo = pipeline_config.repo_root
    benchmark = str(workload.get("benchmark_name", "tpch"))
    q = int(workload.get("query_number", query))
    engine = sanitize_engine_name(str(workload.get("engine_name") or ""))
    if not engine and workload.get("workload_kind") != "engine_backend":
        raise ValueError(
            "resolve_workload_paths: accelerator workload missing engine_name"
        )
    # Per-engine query tree: src/generated/{bench}/q{N}/{engine}/...
    eng_seg = f"{engine}/" if engine else ""
    source_subdir = str(
        workload.get(
            "source_subdir",
            f"{benchmark}/q{q}/{engine}/base/single-threaded" if engine else f"{benchmark}/q{q}/base/single-threaded",
        )
    )
    generated_base = f"src/generated/{benchmark}/q{q}/{eng_seg}base/single-threaded"
    header_prefix = str(workload.get("header_prefix", f"{benchmark}_q"))
    header = str(workload.get("header_basename", f"{header_prefix}{q}"))
    fn_prefix = str(workload.get("function_prefix", f"accel_{benchmark}_q"))
    test_prefix = str(workload.get("test_target_prefix", f"test_{benchmark}_q"))
    bench_prefix = str(workload.get("bench_target_prefix", f"benchmark_{benchmark}_q"))
    smoke_suffix = str(workload.get("ctest_smoke_suffix", "_smoke_test"))

    paths = {
        "source_subdir": source_subdir,
        "function_name": str(workload.get("function_name", f"{fn_prefix}{q}")),
        "create_fn": str(
            workload.get(
                "create_fn",
                f"Create{benchmark.title().replace('_', '')}Q{q}FunctionSet",
            )
        ),
        "test_target": str(workload.get("test_target", f"{test_prefix}{q}")),
        "bench_target": str(workload.get("bench_target", f"{bench_prefix}{q}")),
        "ctest_smoke_name": str(workload.get("ctest_smoke_name", f"{benchmark}_q{q}{smoke_suffix}")),
        "ctest_smoke_regex": str(
            workload.get("ctest_smoke_regex", f"^{benchmark}_q{q}{smoke_suffix}$")
        ),
        "dataset_db": str(workload.get("dataset_db", pipeline_config.tpch_db_sf1)),
        "scale_db": str(workload.get("scale_db", pipeline_config.tpch_db_sf10)),
        "db_env_var": str(workload.get("db_env_var", "ACCEL_TPCH_DB")),
        "header_path": f"src/generated/{benchmark}/q{q}/{eng_seg}base/query.hpp",
        "kernel_path": f"{generated_base}/kernel.cpp",
        "package_path": f"{generated_base}/package.cpp",
        "fused_scan_path": f"{generated_base}/fused_scan.cpp",
        "runtime_path": f"src/generated/{benchmark}/runtime.cpp",
        "fused_header_path": f"src/generated/{benchmark}/q{q}/{eng_seg}base/fused.hpp",
        "aux_module_header_path": f"src/generated/{benchmark}/q{q}/{eng_seg}aux/module.hpp",
        "aux_finalize_header_path": f"src/generated/{benchmark}/q{q}/{eng_seg}aux/finalize.hpp",
        "catalog_path": f"src/generated/catalog/{engine}/query_catalog.hpp" if engine else "",
        "test_path": f"tests/single-threaded/{workload.get('test_target', f'{test_prefix}{q}')}.cpp",
        "benchmark_path": f"benchmarks/{source_subdir.split('/')[0]}/single-threaded/benchmark_q{q}.cpp",
        "engine_name": engine,
    }

    if workload.get("workload_kind") == "aux_state":
        aux_fn = str(workload.get("aux_function_name", f"{fn_prefix}{q}_aux"))
        aux_test = str(workload.get("aux_test_target", f"{test_prefix}{q}_aux"))
        aux_bench = str(workload.get("aux_bench_target", f"{bench_prefix}{q}_aux"))
        aux_hybrid = str(workload.get("aux_hybrid_bench_target", f"benchmark_{benchmark}_q{q}_hybrid_eval"))
        calibrate = str(
            workload.get("aux_calibrate_target", f"calibrate_{benchmark}_q{q}_aux_cost_model")
        )
        aux_state_subdir = f"src/generated/{benchmark}/q{q}/{eng_seg}aux"
        aux_finalize = "finalize.cpp"
        if benchmark == "sec_edgar":
            default_cost_model = f"config/sec_edgar_q{q}_aux_cost_model.json"
        else:
            default_cost_model = f"config/q{q}_aux_cost_model.json"
        cost_model = str(workload.get("cost_model_config", default_cost_model))
        bench_sweep = workload.get("aux_bench_sweep", {})
        if not isinstance(bench_sweep, dict):
            bench_sweep = {}
        if benchmark == "sec_edgar":
            bench_suffix = f"q{q}_aux_bench_sf3"
        else:
            bench_suffix = f"q{q}_aux_bench_sf1"
        paths.update(
            {
                "aux_function_name": aux_fn,
                "aux_test_target": aux_test,
                "aux_bench_target": aux_bench,
                "aux_hybrid_bench_target": aux_hybrid,
                "aux_calibrate_target": calibrate,
                "aux_ctest_smoke_regex": str(
                    workload.get("aux_ctest_smoke_regex", f"^{benchmark}_q{q}_aux_smoke_test$")
                ),
                "aux_state_subdir": aux_state_subdir,
                "aux_finalize_path": f"{aux_state_subdir}/{aux_finalize}",
                "cost_model_config": cost_model,
                "aux_bench_work_dir_suffix": str(
                    workload.get("aux_bench_work_dir_suffix", bench_suffix)
                ),
                "aux_bench_suites": str(
                    bench_sweep.get("suites", workload.get("aux_bench_suites", "lineitem_only"))
                ),
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
        aux_test = f"tests/single-threaded/{paths['aux_test_target']}.cpp"
        all_paths = [
            f"{aux}/module.hpp",
            f"{aux}/module.cpp",
            f"{aux}/finalize.cpp",
            aux_test,
        ]
        cost_model = paths.get("cost_model_config")
        if cost_model:
            all_paths.append(cost_model)
        test_support = f"tests/test_support/q{int(workload.get('query_number', query))}_aux_helpers.hpp"
        if (pipeline_config.repo_root / test_support).is_file():
            all_paths.append(test_support)
        return {
            "primary_cpp": f"{aux}/module.cpp",
            "header": f"{aux}/module.hpp",
            "secondary_cpp": f"{aux}/finalize.cpp",
            "all_paths": all_paths,
            "base_kernel_path": paths["kernel_path"],
            "is_aux": True,
        }

    all_paths = [paths["header_path"], paths["kernel_path"], paths["package_path"]]
    fused = paths.get("fused_scan_path")
    if fused:
        all_paths.append(fused)
    return {
        "primary_cpp": paths["kernel_path"],
        "header": paths["header_path"],
        "secondary_cpp": paths["package_path"],
        "all_paths": all_paths,
        "base_kernel_path": None,
        "is_aux": False,
    }


def load_pipeline_config(
    config_path: Path | None = None,
    repo_root: Path | None = None,
) -> PipelineConfig:
    root = repo_root or find_repo_root()
    pipe = pipeline_root(root)
    cfg_path = config_path or (pipe / "config" / "default.yaml")
    data = _load_yaml(cfg_path)

    build_rel = data.get("build_dir", "build-release")
    gates_rel = data.get("gates_dir", "pipeline/gates")
    opt = data.get("optimization", {})
    crash_debug = data.get("crash_debug", {})
    continuation = dict(opt.get("continuation") or {})
    openrouter_history = dict(opt.get("openrouter_history") or {})
    attempt_budgets = dict(opt.get("attempt_budgets") or {})
    patch_budgets = dict(opt.get("patch_budgets") or {})
    max_turns = dict(opt.get("max_turns") or {})

    datasets: dict[str, Path] = {}
    for key, value in dict(data.get("datasets", {})).items():
        datasets[key] = Path(value)
    datasets.setdefault("tpch_sf1", Path(data["tpch_db_sf1"]))
    datasets.setdefault("tpch_sf10", Path(data["tpch_db_sf10"]))

    shared_paths = [str(x) for x in list(data.get("shared_paths") or [])]
    if not shared_paths:
        shared_paths = list(DEFAULT_SHARED_PATH_GLOBS)

    cfg = PipelineConfig(
        repo_root=root,
        pipeline_root=pipe,
        build_dir=root / build_rel,
        tpch_db_sf1=Path(data["tpch_db_sf1"]),
        tpch_db_sf10=Path(data["tpch_db_sf10"]),
        datasets=datasets,
        optimization_max_iterations=int(opt.get("max_iterations", 3)),
        optimization_no_improvement_stop=int(opt.get("no_improvement_stop", 4)),
        optimization_episodes_per_iteration=int(
            opt.get("max_attempts_per_iteration", opt.get("episodes_per_iteration", 4))
        ),
        optimization_max_attempts_per_iteration=int(
            opt.get("max_attempts_per_iteration", opt.get("episodes_per_iteration", 4))
        ),
        optimization_max_turns_per_episode=int(opt.get("max_turns_per_episode", 8)),
        optimization_improvement_threshold_pct=float(opt.get("improvement_threshold_pct", 3.0)),
        optimization_reject_threshold_pct=float(opt.get("reject_threshold_pct", 2.0)),
        optimization_consecutive_fail_stop=int(opt.get("consecutive_fail_stop", 0)),
        optimization_quick_benchmark_samples=int(opt.get("quick_benchmark_samples", 3)),
        optimization_confirmation_samples=int(opt.get("confirmation_samples", 3)),
        optimization_context_max_chars=int(opt.get("context_max_chars", 80_000)),
        optimization_context_max_lines=int(opt.get("context_max_lines", 1200)),
        optimization_bootstrap_max_chars=int(opt.get("bootstrap_max_chars", 300_000)),
        optimization_bootstrap_max_lines_per_file=int(opt.get("bootstrap_max_lines_per_file", 0)),
        optimization_skip_per_attempt_correctness=bool(
            opt.get("skip_per_attempt_correctness", True)
        ),
        optimization_accept_on_screen=bool(opt.get("accept_on_screen", True)),
        optimization_skip_confirmation=bool(opt.get("skip_confirmation", True)),
        optimization_smoke_runs=int(opt.get("smoke_runs", 2)),
        optimization_max_turns_analysis=int(max_turns.get("analysis", 0)),
        optimization_max_turns_patch=int(max_turns.get("patch", 0)),
        optimization_max_turns_unified=int(max_turns.get("unified", 0)),
        optimization_attempt_budgets=attempt_budgets,
        optimization_patch_budgets=patch_budgets,
        optimization_build_fix_max_rounds=int(opt.get("build_fix_max_rounds", 0)),
        optimization_correctness_fix_max_rounds=int(opt.get("correctness_fix_max_rounds", 0)),
        optimization_build_fix_safety_cap=int(opt.get("build_fix_safety_cap", 12)),
        optimization_correctness_fix_safety_cap=int(opt.get("correctness_fix_safety_cap", 12)),
        optimization_fix_log_chars=int(opt.get("fix_log_chars", 6000)),
        optimization_persistent_worker=bool(opt.get("persistent_worker", True)),
        optimization_worker_max_turns_per_candidate=int(opt.get("worker_max_turns_per_candidate", 0)),
        optimization_worker_safety_cap_turns=int(opt.get("worker_safety_cap_turns", 80)),
        optimization_openai_auto_previous_response_id=bool(
            continuation.get("openai_auto_previous_response_id", True)
        ),
        optimization_openrouter_history_max_chars=int(openrouter_history.get("max_chars", 120_000)),
        optimization_openrouter_preserve_recent_cycles=int(openrouter_history.get("preserve_recent_cycles", 2)),
        optimization_openrouter_tool_output_summary_chars=int(
            openrouter_history.get("tool_output_summary_chars", 80)
        ),
        optimization_hot_path_digest_max_chars=int(opt.get("hot_path_digest_max_chars", 25_000)),
        optimization_turn_delta_max_chars=int(opt.get("turn_delta_max_chars", 4_000)),
        optimization_checkpoint_file=str(opt.get("checkpoint_file", "optimization_worker_checkpoint.json")),
        optimization_early_stop=dict(opt.get("early_stop") or {}),
        crash_debug_max_rounds=int(crash_debug.get("max_rounds", 0)),
        crash_debug_safety_cap=int(crash_debug.get("safety_cap", 25)),
        stage_max_turns=dict(data.get("stage_max_turns", {})),
        write_paths=list(data.get("write_paths", [])),
        allowed_commands=list(data.get("allowed_commands", [])),
        gates_dir=root / gates_rel,
        worker_id=str(os.environ.get("ACCEL_PIPELINE_WORKER_ID") or ""),
        shared_paths=shared_paths,
    )

    env_build = os.environ.get("ACCEL_PIPELINE_BUILD_DIR")
    if env_build:
        bdir = Path(env_build)
        if not bdir.is_absolute():
            bdir = root / bdir
        from accel_pipeline.worker_isolation import _rewrite_build_dir_patterns

        cfg = replace(
            cfg,
            build_dir=bdir,
            allowed_commands=_rewrite_build_dir_patterns(list(cfg.allowed_commands), bdir.name),
        )
    env_codex = os.environ.get("ACCEL_PIPELINE_CODEX_HOME")
    if env_codex:
        chome = Path(env_codex)
        if not chome.is_absolute():
            chome = root / chome
        cfg = replace(cfg, codex_home=chome)
    return cfg


def _resolve_config_path(repo_root: Path, path: Path) -> Path:
    if path.is_absolute():
        return path
    pipe = pipeline_root(repo_root)
    candidates = (
        path,
        repo_root / path,
        pipe / path,
        pipe / "config" / path.name,
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return repo_root / path


def load_model_config(path: Path | None = None, repo_root: Path | None = None) -> ModelConfig:
    root = repo_root or find_repo_root()
    cfg_path = _resolve_config_path(root, path or (pipeline_root(root) / "config" / "models.yaml"))
    data = _load_yaml(cfg_path)
    return ModelConfig(
        provider=str(data.get("provider", "openai")),
        api_key_env=str(data.get("api_key_env", "OPENAI_API_KEY")),
        presets=dict(data.get("presets", {})),
        stages=dict(data.get("stages", {})),
        openrouter_base_url=str(data.get("openrouter_base_url", "https://openrouter.ai/api/v1")),
        stage_providers={str(k): str(v) for k, v in dict(data.get("stage_providers") or {}).items()},
        codex_bin=str(data.get("codex_bin", "codex")),
        codex_config_path=str(data.get("codex_config_path", "pipeline/.codex/config.toml")),
        codex_timeout_sec=int(data.get("codex_timeout_sec", 7200)),
        codex_seed_max_chars=int(data.get("codex_seed_max_chars", 120_000)),
        codex_seed_preserve_recent_cycles=int(data.get("codex_seed_preserve_recent_cycles", 2)),
        codex_bypass_approvals_and_sandbox=bool(
            data.get("codex_bypass_approvals_and_sandbox", True)
        ),
    )


def load_aux_state_defaults(repo_root: Path | None = None) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    path = pipeline_root(root) / "config" / "aux_state.yaml"
    if not path.is_file():
        return {}
    return _load_yaml(path)


def load_engine_backend_defaults(repo_root: Path | None = None) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    path = pipeline_root(root) / "config" / "engine_backend.yaml"
    if not path.is_file():
        return {}
    return _load_yaml(path)


def sanitize_engine_name(name: str) -> str:
    """Normalize engine_name to a filesystem / C++ identifier fragment."""
    cleaned = "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in str(name).strip().lower())
    cleaned = "_".join(part for part in cleaned.split("_") if part)
    if not cleaned or not cleaned[0].isalpha():
        raise ValueError(f"invalid engine_name: {name!r} (need a letter-leading identifier)")
    return cleaned


def engine_pascal_name(engine_name: str) -> str:
    return "".join(part[:1].upper() + part[1:] for part in sanitize_engine_name(engine_name).split("_") if part)


def engine_host_conventions(engine_name: str) -> dict[str, Any]:
    """Conventional host registration / differential surfaces for accelerator workloads."""
    engine = sanitize_engine_name(engine_name)
    host: dict[str, Any] = {
        "engine_name": engine,
        "register_path": f"src/runtime/{engine}/register.cpp",
        "sql_entrypoint_convention": f"accel_* table function / SQL UDF on host {engine}",
        "dataset_env": "ACCEL_TPCH_DB",
        "differential_binary": "test_tpch_shared_differential",
        "differential_command": "./build-release/test_tpch_shared_differential \"$ACCEL_TPCH_DB\" <query>",
        "differential_source": "tests/aux_state/test_tpch_shared_differential.cpp",
        "smoke_ready": True,
        "agent_builds_query_host": False,
        "aux_register_path": "",
        "query_host_ready_hint": "",
    }
    if engine == "duckdb":
        host["aux_register_path"] = "src/stateful/duckdb/duckdb_aux_table_functions.cpp"
        host["sql_entrypoint_convention"] = (
            "DuckDB table functions via Create*FunctionSet + RegisterAcceleratorFunctions "
            "(src/runtime/duckdb/register.cpp); aux TFs in duckdb_aux_table_functions.cpp"
        )
        host["multi_conn_ctest_regex"] = "test_aux_multi_connection_stress"
        host["cross_accel_ctest_regex"] = "test_cross_accel"
        host["work_db_suffix"] = ".duckdb"
        return host

    # Any non-duckdb engine: agent-buildable query host on the existing backend surface.
    host["register_path"] = f"src/generated/{engine}/"
    host["aux_register_path"] = f"src/generated/{engine}/"
    host["differential_binary"] = f"test_{engine}_tpch_differential"
    host["differential_source"] = f"tests/aux_state/test_{engine}_tpch_differential.cpp"
    host["differential_command"] = (
        f"./build-release/test_{engine}_tpch_differential <query> "
        "(create if missing during accelerator implementation)"
    )
    host["smoke_ready"] = True
    host["agent_builds_query_host"] = True
    host["multi_conn_ctest_regex"] = f"test_{engine}_aux_multi_connection_stress"
    host["cross_accel_ctest_regex"] = f"test_{engine}_cross_accel"
    host["work_db_suffix"] = f".{engine}_work"
    host["query_host_ready_hint"] = (
        f"Query host for {engine} is agent-buildable: if SQL entrypoints / "
        f"{host['differential_binary']} / multi-conn / cross-accel / bench harnesses "
        f"are missing, replicate the duckdb gate surfaces for this backend under "
        f"`{host['register_path']}` (reuse runtime|stateful/{engine} adapters). "
        "See pipeline/prompts/shared/query_host_bootstrap.md."
    )
    host["sql_entrypoint_convention"] = (
        f"Host-native SQL entrypoints for {engine} (UDF / TF / SRF / extension) under "
        f"src/generated/{engine}/ or runtime/{engine}/. Call generated kernels under "
        f"src/generated/{{bench}}/q{{N}}/{engine}/ and AuxQueryModule via this engine's "
        f"adapters (mutation/store). Do not require a shared DataChunk IR."
    )
    if engine == "postgres":
        # Narrow path hints only; behavior matches any agent-buildable engine.
        host["register_path"] = f"src/generated/{engine}/postgres_extension.cpp"
        host["aux_register_path"] = host["register_path"]
        host["dataset_env"] = "PG_CONFIG"
    elif engine == "mysql":
        host["register_path"] = f"src/generated/{engine}/"
        host["aux_register_path"] = host["register_path"]
        host["dataset_env"] = "ACCEL_MYSQL_DOCKER"
    return host


def _normalize_accelerator_engine(data: dict[str, Any]) -> dict[str, Any]:
    """Require explicit engine_name and attach host conventions for accelerators."""
    env_engine = os.environ.get("ACCEL_WORKLOAD_ENGINE_NAME", "").strip()
    raw = data.get("engine_name") or (env_engine or None)
    if raw is None or str(raw).strip() == "":
        raise ValueError(
            "accelerator workload requires explicit engine_name "
            "(set in workload YAML or ACCEL_WORKLOAD_ENGINE_NAME). "
            "No default to duckdb or postgres."
        )
    engine = sanitize_engine_name(str(raw))
    data["engine_name"] = engine
    host = engine_host_conventions(engine)
    data["engine_host"] = host

    # Generated query tree for this engine.
    bench = str(data.get("benchmark_name") or data.get("benchmark") or "tpch")
    qnum = data.get("query_number")
    writes = list(data.get("write_paths") or [])
    for extra in (
        f"src/generated/{bench}/q{qnum}/{engine}/**" if qnum is not None else "",
        f"src/generated/catalog/{engine}/**",
        f"src/generated/{engine}/**",
        f"src/runtime/{engine}/**",
        f"src/stateful/{engine}/**",
        f"tests/aux_state/test_{engine}_*.cpp",
        f"tests/engine_backend/test_{engine}_*.cpp",
        "CMakeLists.txt",
        "cmake/**",
    ):
        if extra and extra not in writes:
            writes.append(extra)

    if engine != "duckdb":
        duckdb_tf = "src/stateful/duckdb/duckdb_aux_table_functions.cpp"
        writes = [w for w in writes if w != duckdb_tf]
        for extra in (
            host.get("register_path") or "",
            host.get("aux_register_path") or "",
            host.get("differential_source") or "",
            "tests/aux_state/**",
        ):
            if extra and extra not in writes:
                writes.append(extra)
    data["write_paths"] = _dedupe_preserve(writes)
    return data


def _expand_engine_placeholders(value: str, engine: str) -> str:
    return (
        str(value)
        .replace("{engine}", engine)
        .replace("{ENGINE}", engine.upper())
        .replace("{Engine}", engine_pascal_name(engine))
    )


def _dedupe_preserve(items: list[str]) -> list[str]:
    seen: set[str] = set()
    out: list[str] = []
    for item in items:
        if item in seen:
            continue
        seen.add(item)
        out.append(item)
    return out


def _normalize_engine_backend_workload(data: dict[str, Any], repo_root: Path) -> dict[str, Any]:
    """Fill conventional paths/write allowlists for any engine_name."""
    defaults = load_engine_backend_defaults(repo_root)
    engine = sanitize_engine_name(str(data.get("engine_name") or ""))
    data["engine_name"] = engine
    data.setdefault("benchmark_name", "engine")
    data.setdefault("artifact_namespace", f"engines/{engine}")
    data.setdefault("display_name", engine_pascal_name(engine))
    data.setdefault("prompt_subdir", str(defaults.get("prompt_subdir") or "engine_backend"))
    data.setdefault("query_number", 0)
    data.setdefault(
        "allow_update_existing",
        bool(defaults.get("allow_update_existing", True)),
    )
    if "store_mode_candidates" not in data and defaults.get("store_mode_candidates"):
        data["store_mode_candidates"] = list(defaults["store_mode_candidates"])

    for key in (
        "post_implementation_gates",
        "post_validation_gates",
        "stage_max_turns",
        "hide_foreign_engines",
        "agent_hidden_trees",
        "requires_in_process_host",
        "architecture_audit_max_rounds",
        "layer_critique_max_rounds",
        "layer_gate_max_rounds",
        "implementation_layers",
        "host_runtime",
    ):
        if key not in data and key in defaults:
            data[key] = copy.deepcopy(defaults[key])

    layers = []
    for raw in list(data.get("implementation_layers") or []):
        if not isinstance(raw, dict):
            continue
        layer = dict(raw)
        for field in ("harness", "cmake_target", "title", "gate", "id"):
            if field in layer:
                layer[field] = _expand_engine_placeholders(str(layer[field]), engine)
        layers.append(layer)
    data["implementation_layers"] = layers

    default_targets = {
        str(k): _expand_engine_placeholders(str(v), engine)
        for k, v in dict(defaults.get("default_target_paths") or {}).items()
    }
    targets = dict(default_targets)
    targets.update({str(k): str(v) for k, v in dict(data.get("target_paths") or {}).items()})
    data["target_paths"] = targets

    default_cmake = {
        str(k): _expand_engine_placeholders(str(v), engine)
        for k, v in dict(defaults.get("default_cmake") or {}).items()
    }
    cmake = dict(default_cmake)
    cmake.update({str(k): str(v) for k, v in dict(data.get("cmake") or {}).items()})
    data["cmake"] = cmake

    expanded_writes = [
        _expand_engine_placeholders(str(p), engine)
        for p in list(defaults.get("write_paths") or []) + list(data.get("write_paths") or [])
    ]
    data["write_paths"] = _dedupe_preserve(expanded_writes)

    expanded_cmds = [
        _expand_engine_placeholders(str(c), engine)
        for c in list(defaults.get("allowed_commands") or []) + list(data.get("allowed_commands") or [])
    ]
    data["allowed_commands"] = _dedupe_preserve(expanded_cmds)
    return data


def load_benchmark_aux_state_defaults(
    benchmark_name: str,
    repo_root: Path | None = None,
) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    path = pipeline_root(root) / "config" / f"{benchmark_name}_aux_state.yaml"
    if not path.is_file():
        return {}
    return _load_yaml(path)


def _merge_aux_defaults(workload: dict[str, Any], repo_root: Path) -> None:
    aux_defaults = load_aux_state_defaults(repo_root)
    benchmark = str(workload.get("benchmark_name", "tpch"))
    bench_aux = load_benchmark_aux_state_defaults(benchmark, repo_root)

    for key in (
        "post_implementation_gates",
        "post_optimization_gates",
        "post_audit_gates",
        "prompt_subdir",
        "max_structural_reanalysis",
        "fairness",
        "host_bootstrap_gate_max_rounds",
        "host_bootstrap_critique_max_rounds",
    ):
        if key in aux_defaults:
            if key == "fairness":
                merged_fair = dict(workload.get("fairness") or {})
                merged_fair.update(dict(aux_defaults["fairness"] or {}))
                # Prefer aux_state search paths when provided.
                if aux_defaults["fairness"].get("kernel_search_paths"):
                    merged_fair["kernel_search_paths"] = list(
                        aux_defaults["fairness"]["kernel_search_paths"]
                    )
                if "cardinality_constants" not in merged_fair and workload.get("fairness"):
                    merged_fair["cardinality_constants"] = list(
                        (workload.get("fairness") or {}).get("cardinality_constants") or []
                    )
                workload["fairness"] = merged_fair
            else:
                workload[key] = aux_defaults[key]
    if "speedup_verification" in aux_defaults:
        workload.setdefault("speedup_verification", dict(aux_defaults["speedup_verification"]))

    for key in ("write_paths", "allowed_commands"):
        merged = list(aux_defaults.get(key, []))
        merged.extend(bench_aux.get(key, []))
        pack_extra = list(workload.get(key, []))
        workload[key] = merged + pack_extra

    workload_sweep = workload.get("aux_bench_sweep")
    if isinstance(workload_sweep, dict) and workload_sweep:
        base = dict(bench_aux.get("aux_bench_sweep") or aux_defaults.get("aux_bench_sweep") or {})
        base.update(workload_sweep)
        workload["aux_bench_sweep"] = base
    elif "aux_bench_sweep" in bench_aux:
        workload["aux_bench_sweep"] = dict(bench_aux["aux_bench_sweep"])
    elif "aux_bench_sweep" in aux_defaults:
        workload["aux_bench_sweep"] = dict(aux_defaults["aux_bench_sweep"])

    if "aux_bench_sweep_optimization" in aux_defaults:
        workload["aux_bench_sweep_optimization"] = dict(aux_defaults["aux_bench_sweep_optimization"])


def _apply_optimization_block(pipeline_config: PipelineConfig, opt: dict[str, Any]) -> PipelineConfig:
    """Merge workload-level optimization: {...} overrides into PipelineConfig."""
    if not opt:
        return pipeline_config
    max_turns = dict(opt.get("max_turns") or {})
    continuation = dict(opt.get("continuation") or {})
    openrouter_history = dict(opt.get("openrouter_history") or {})
    attempt_budgets = dict(pipeline_config.optimization_attempt_budgets)
    if opt.get("attempt_budgets"):
        attempt_budgets.update(dict(opt["attempt_budgets"]))
    patch_budgets = dict(pipeline_config.optimization_patch_budgets)
    if opt.get("patch_budgets"):
        patch_budgets.update(dict(opt["patch_budgets"]))
    return PipelineConfig(
        repo_root=pipeline_config.repo_root,
        pipeline_root=pipeline_config.pipeline_root,
        build_dir=pipeline_config.build_dir,
        tpch_db_sf1=pipeline_config.tpch_db_sf1,
        tpch_db_sf10=pipeline_config.tpch_db_sf10,
        datasets=dict(pipeline_config.datasets),
        optimization_max_iterations=int(
            opt.get("max_iterations", pipeline_config.optimization_max_iterations)
        ),
        optimization_no_improvement_stop=int(
            opt.get("no_improvement_stop", pipeline_config.optimization_no_improvement_stop)
        ),
        optimization_episodes_per_iteration=int(
            opt.get(
                "max_attempts_per_iteration",
                opt.get("episodes_per_iteration", pipeline_config.optimization_episodes_per_iteration),
            )
        ),
        optimization_max_attempts_per_iteration=int(
            opt.get(
                "max_attempts_per_iteration",
                opt.get("episodes_per_iteration", pipeline_config.optimization_max_attempts_per_iteration),
            )
        ),
        optimization_max_turns_per_episode=int(
            opt.get("max_turns_per_episode", pipeline_config.optimization_max_turns_per_episode)
        ),
        optimization_improvement_threshold_pct=float(
            opt.get("improvement_threshold_pct", pipeline_config.optimization_improvement_threshold_pct)
        ),
        optimization_reject_threshold_pct=float(
            opt.get("reject_threshold_pct", pipeline_config.optimization_reject_threshold_pct)
        ),
        optimization_consecutive_fail_stop=int(
            opt.get("consecutive_fail_stop", pipeline_config.optimization_consecutive_fail_stop)
        ),
        optimization_quick_benchmark_samples=int(
            opt.get("quick_benchmark_samples", pipeline_config.optimization_quick_benchmark_samples)
        ),
        optimization_confirmation_samples=int(
            opt.get("confirmation_samples", pipeline_config.optimization_confirmation_samples)
        ),
        optimization_context_max_chars=int(
            opt.get("context_max_chars", pipeline_config.optimization_context_max_chars)
        ),
        optimization_context_max_lines=int(
            opt.get("context_max_lines", pipeline_config.optimization_context_max_lines)
        ),
        optimization_bootstrap_max_chars=int(
            opt.get("bootstrap_max_chars", pipeline_config.optimization_bootstrap_max_chars)
        ),
        optimization_bootstrap_max_lines_per_file=int(
            opt.get(
                "bootstrap_max_lines_per_file",
                pipeline_config.optimization_bootstrap_max_lines_per_file,
            )
        ),
        optimization_skip_per_attempt_correctness=bool(
            opt.get(
                "skip_per_attempt_correctness",
                pipeline_config.optimization_skip_per_attempt_correctness,
            )
        ),
        optimization_accept_on_screen=bool(
            opt.get("accept_on_screen", pipeline_config.optimization_accept_on_screen)
        ),
        optimization_skip_confirmation=bool(
            opt.get("skip_confirmation", pipeline_config.optimization_skip_confirmation)
        ),
        optimization_smoke_runs=int(
            opt.get("smoke_runs", pipeline_config.optimization_smoke_runs)
        ),
        optimization_max_turns_analysis=int(
            max_turns.get("analysis", pipeline_config.optimization_max_turns_analysis)
        ),
        optimization_max_turns_patch=int(
            max_turns.get("patch", pipeline_config.optimization_max_turns_patch)
        ),
        optimization_max_turns_unified=int(
            max_turns.get("unified", pipeline_config.optimization_max_turns_unified)
        ),
        optimization_attempt_budgets=attempt_budgets,
        optimization_patch_budgets=patch_budgets,
        optimization_build_fix_max_rounds=int(
            opt.get("build_fix_max_rounds", pipeline_config.optimization_build_fix_max_rounds)
        ),
        optimization_correctness_fix_max_rounds=int(
            opt.get(
                "correctness_fix_max_rounds",
                pipeline_config.optimization_correctness_fix_max_rounds,
            )
        ),
        optimization_build_fix_safety_cap=int(
            opt.get("build_fix_safety_cap", pipeline_config.optimization_build_fix_safety_cap)
        ),
        optimization_correctness_fix_safety_cap=int(
            opt.get(
                "correctness_fix_safety_cap",
                pipeline_config.optimization_correctness_fix_safety_cap,
            )
        ),
        optimization_fix_log_chars=int(
            opt.get("fix_log_chars", pipeline_config.optimization_fix_log_chars)
        ),
        optimization_persistent_worker=bool(
            opt.get("persistent_worker", pipeline_config.optimization_persistent_worker)
        ),
        optimization_worker_max_turns_per_candidate=int(
            opt.get("worker_max_turns_per_candidate", pipeline_config.optimization_worker_max_turns_per_candidate)
        ),
        optimization_worker_safety_cap_turns=int(
            opt.get("worker_safety_cap_turns", pipeline_config.optimization_worker_safety_cap_turns)
        ),
        optimization_openai_auto_previous_response_id=bool(
            continuation.get(
                "openai_auto_previous_response_id",
                pipeline_config.optimization_openai_auto_previous_response_id,
            )
        ),
        optimization_openrouter_history_max_chars=int(
            openrouter_history.get(
                "max_chars", pipeline_config.optimization_openrouter_history_max_chars
            )
        ),
        optimization_openrouter_preserve_recent_cycles=int(
            openrouter_history.get(
                "preserve_recent_cycles", pipeline_config.optimization_openrouter_preserve_recent_cycles
            )
        ),
        optimization_openrouter_tool_output_summary_chars=int(
            openrouter_history.get(
                "tool_output_summary_chars",
                pipeline_config.optimization_openrouter_tool_output_summary_chars,
            )
        ),
        optimization_hot_path_digest_max_chars=int(
            opt.get("hot_path_digest_max_chars", pipeline_config.optimization_hot_path_digest_max_chars)
        ),
        optimization_turn_delta_max_chars=int(
            opt.get("turn_delta_max_chars", pipeline_config.optimization_turn_delta_max_chars)
        ),
        optimization_checkpoint_file=str(
            opt.get("checkpoint_file", pipeline_config.optimization_checkpoint_file)
        ),
        optimization_early_stop=(
            dict(opt["early_stop"])
            if isinstance(opt.get("early_stop"), dict)
            else dict(pipeline_config.optimization_early_stop)
        ),
        crash_debug_max_rounds=pipeline_config.crash_debug_max_rounds,
        crash_debug_safety_cap=pipeline_config.crash_debug_safety_cap,
        stage_max_turns=dict(pipeline_config.stage_max_turns),
        write_paths=list(pipeline_config.write_paths),
        allowed_commands=list(pipeline_config.allowed_commands),
        gates_dir=pipeline_config.gates_dir,
        worker_id=pipeline_config.worker_id,
        codex_home=pipeline_config.codex_home,
        shared_paths=list(pipeline_config.shared_paths or []),
    )


def apply_workload_overrides(pipeline_config: PipelineConfig, workload: dict[str, Any]) -> PipelineConfig:
    extra_write_paths = list(workload.get("write_paths", []))
    extra_allowed = list(workload.get("allowed_commands", []))
    stage_max_turns = dict(pipeline_config.stage_max_turns)
    optimization_max_turns_per_episode = pipeline_config.optimization_max_turns_per_episode
    engine = sanitize_engine_name(str(workload.get("engine_name") or ""))
    if workload.get("workload_kind") != "engine_backend" and not engine:
        raise ValueError(
            "accelerator workload requires explicit engine_name "
            "(no default to duckdb or postgres)"
        )

    if workload.get("workload_kind") == "aux_state":
        aux_defaults = load_aux_state_defaults(pipeline_config.repo_root)
        extra_write_paths = list(aux_defaults.get("write_paths", [])) + extra_write_paths
        extra_allowed = list(aux_defaults.get("allowed_commands", [])) + extra_allowed
        aux_stage_turns = aux_defaults.get("stage_max_turns")
        if isinstance(aux_stage_turns, dict):
            stage_max_turns.update(aux_stage_turns)
        if "optimization_max_turns_per_episode" in aux_defaults:
            optimization_max_turns_per_episode = int(aux_defaults["optimization_max_turns_per_episode"])

    # Non-duckdb accelerators: drop duckdb-only TF host file; keep engine host paths.
    duckdb_tf = "src/stateful/duckdb/duckdb_aux_table_functions.cpp"
    if (
        workload.get("workload_kind") != "engine_backend"
        and engine
        and engine != "duckdb"
    ):
        host = dict(workload.get("engine_host") or engine_host_conventions(engine))
        extra_write_paths = [p for p in extra_write_paths if p != duckdb_tf]
        for extra in (
            host.get("register_path") or "",
            host.get("aux_register_path") or "",
            host.get("differential_source") or "",
            f"src/generated/{engine}/**",
            f"src/runtime/{engine}/**",
            f"src/stateful/{engine}/**",
            f"tests/aux_state/test_{engine}_*.cpp",
            "tests/aux_state/**",
            "CMakeLists.txt",
            "cmake/**",
        ):
            if extra and extra not in extra_write_paths:
                extra_write_paths.append(extra)
        extra_write_paths = _dedupe_preserve(extra_write_paths)
        extra_allowed = list(extra_allowed) + [
            rf"^(\\.\\/)?build-release\\/test_{engine}_tpch_differential( .*)?$",
            rf"^cmake --build build-release --target test_{engine}_tpch_differential( .*)?$",
            rf"^ACCEL_[A-Z_]+=\\S+( ACCEL_[A-Z_]+=\\S+)* (\\.\\/)?build-release\\/test_{engine}_tpch_differential( .*)?$",
        ]

    workload_stage_turns = workload.get("stage_max_turns")
    if isinstance(workload_stage_turns, dict):
        stage_max_turns.update(workload_stage_turns)
    if "optimization_max_turns_per_episode" in workload:
        optimization_max_turns_per_episode = int(workload["optimization_max_turns_per_episode"])

    opt_block = workload.get("optimization")
    has_opt = isinstance(opt_block, dict) and bool(opt_block)
    unchanged = (
        not extra_write_paths
        and not extra_allowed
        and stage_max_turns == pipeline_config.stage_max_turns
        and optimization_max_turns_per_episode == pipeline_config.optimization_max_turns_per_episode
        and not has_opt
    )
    if unchanged:
        return pipeline_config

    merged = PipelineConfig(
        repo_root=pipeline_config.repo_root,
        pipeline_root=pipeline_config.pipeline_root,
        build_dir=pipeline_config.build_dir,
        tpch_db_sf1=pipeline_config.tpch_db_sf1,
        tpch_db_sf10=pipeline_config.tpch_db_sf10,
        datasets=dict(pipeline_config.datasets),
        optimization_max_iterations=pipeline_config.optimization_max_iterations,
        optimization_no_improvement_stop=pipeline_config.optimization_no_improvement_stop,
        optimization_episodes_per_iteration=pipeline_config.optimization_episodes_per_iteration,
        optimization_max_attempts_per_iteration=pipeline_config.optimization_max_attempts_per_iteration,
        optimization_max_turns_per_episode=optimization_max_turns_per_episode,
        optimization_improvement_threshold_pct=pipeline_config.optimization_improvement_threshold_pct,
        optimization_reject_threshold_pct=pipeline_config.optimization_reject_threshold_pct,
        optimization_consecutive_fail_stop=pipeline_config.optimization_consecutive_fail_stop,
        optimization_quick_benchmark_samples=pipeline_config.optimization_quick_benchmark_samples,
        optimization_confirmation_samples=pipeline_config.optimization_confirmation_samples,
        optimization_context_max_chars=pipeline_config.optimization_context_max_chars,
        optimization_context_max_lines=pipeline_config.optimization_context_max_lines,
        optimization_bootstrap_max_chars=pipeline_config.optimization_bootstrap_max_chars,
        optimization_bootstrap_max_lines_per_file=pipeline_config.optimization_bootstrap_max_lines_per_file,
        optimization_skip_per_attempt_correctness=pipeline_config.optimization_skip_per_attempt_correctness,
        optimization_accept_on_screen=pipeline_config.optimization_accept_on_screen,
        optimization_skip_confirmation=pipeline_config.optimization_skip_confirmation,
        optimization_smoke_runs=pipeline_config.optimization_smoke_runs,
        optimization_max_turns_analysis=pipeline_config.optimization_max_turns_analysis,
        optimization_max_turns_patch=pipeline_config.optimization_max_turns_patch,
        optimization_max_turns_unified=pipeline_config.optimization_max_turns_unified,
        optimization_attempt_budgets=dict(pipeline_config.optimization_attempt_budgets),
        optimization_patch_budgets=dict(pipeline_config.optimization_patch_budgets),
        optimization_build_fix_max_rounds=pipeline_config.optimization_build_fix_max_rounds,
        optimization_correctness_fix_max_rounds=pipeline_config.optimization_correctness_fix_max_rounds,
        optimization_build_fix_safety_cap=pipeline_config.optimization_build_fix_safety_cap,
        optimization_correctness_fix_safety_cap=pipeline_config.optimization_correctness_fix_safety_cap,
        optimization_fix_log_chars=pipeline_config.optimization_fix_log_chars,
        optimization_persistent_worker=pipeline_config.optimization_persistent_worker,
        optimization_worker_max_turns_per_candidate=pipeline_config.optimization_worker_max_turns_per_candidate,
        optimization_worker_safety_cap_turns=pipeline_config.optimization_worker_safety_cap_turns,
        optimization_openai_auto_previous_response_id=pipeline_config.optimization_openai_auto_previous_response_id,
        optimization_openrouter_history_max_chars=pipeline_config.optimization_openrouter_history_max_chars,
        optimization_openrouter_preserve_recent_cycles=pipeline_config.optimization_openrouter_preserve_recent_cycles,
        optimization_openrouter_tool_output_summary_chars=pipeline_config.optimization_openrouter_tool_output_summary_chars,
        optimization_hot_path_digest_max_chars=pipeline_config.optimization_hot_path_digest_max_chars,
        optimization_turn_delta_max_chars=pipeline_config.optimization_turn_delta_max_chars,
        optimization_checkpoint_file=pipeline_config.optimization_checkpoint_file,
        optimization_early_stop=dict(pipeline_config.optimization_early_stop),
        crash_debug_max_rounds=pipeline_config.crash_debug_max_rounds,
        crash_debug_safety_cap=pipeline_config.crash_debug_safety_cap,
        stage_max_turns=stage_max_turns,
        write_paths=list(pipeline_config.write_paths) + extra_write_paths,
        allowed_commands=list(pipeline_config.allowed_commands) + extra_allowed,
        gates_dir=pipeline_config.gates_dir,
        worker_id=pipeline_config.worker_id,
        codex_home=pipeline_config.codex_home,
        shared_paths=list(pipeline_config.shared_paths or []),
    )
    if has_opt:
        return _apply_optimization_block(merged, opt_block)
    return merged


def load_workload(name: str, repo_root: Path | None = None) -> dict[str, Any]:
    # Pack worktrees may not contain untracked workload YAMLs; prefer primary.
    primary = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    root = Path(primary).resolve() if primary else (repo_root or find_repo_root())
    path = pipeline_root(root) / "config" / "workloads" / f"{name}.yaml"
    if not path.is_file():
        # Fall back to the caller's repo_root (non-pack / single-worker).
        alt_root = repo_root or find_repo_root()
        alt = pipeline_root(alt_root) / "config" / "workloads" / f"{name}.yaml"
        if alt.is_file():
            root = Path(alt_root)
            path = alt
        else:
            raise FileNotFoundError(f"workload not found: {path}")
    data = _load_yaml(path)

    if data.get("workload_kind") == "engine_backend":
        return _normalize_engine_backend_workload(data, root)

    benchmark_name = data.get("benchmark_name", "tpch")
    try:
        pack = load_benchmark_pack(benchmark_name, root)
        pipeline_config = load_pipeline_config(repo_root=root)
        data = _merge_pack_into_workload(data, pack, root, pipeline_config)
    except FileNotFoundError:
        data.setdefault("benchmark_name", "tpch")
        data.setdefault("artifact_namespace", "tpch")

    if data.get("workload_kind") == "aux_state":
        _merge_aux_defaults(data, root)
    return _normalize_accelerator_engine(data)