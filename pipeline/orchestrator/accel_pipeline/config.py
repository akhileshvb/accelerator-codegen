from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.paths import find_repo_root, pipeline_root


@dataclass
class ModelConfig:
    provider: str
    api_key_env: str
    presets: dict[str, str]
    stages: dict[str, str]


@dataclass
class PipelineConfig:
    repo_root: Path
    pipeline_root: Path
    build_dir: Path
    tpch_db_sf1: Path
    tpch_db_sf10: Path
    datasets: dict[str, Path] = field(default_factory=dict)
    optimization_max_iterations: int = 5
    optimization_no_improvement_stop: int = 5
    optimization_episodes_per_iteration: int = 4
    optimization_max_turns_per_episode: int = 8
    crash_debug_max_rounds: int = 0
    crash_debug_safety_cap: int = 25
    stage_max_turns: dict[str, int] = field(default_factory=dict)
    write_paths: list[str] = field(default_factory=list)
    allowed_commands: list[str] = field(default_factory=list)
    gates_dir: Path = field(default_factory=Path)


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
    source_subdir = str(workload.get("source_subdir", "tpch/single-threaded"))
    q = int(workload.get("query_number", query))
    header = str(workload.get("header_basename", f"tpch_q{q}"))
    return {
        "source_subdir": source_subdir,
        "function_name": str(workload.get("function_name", f"accel_tpch_q{q}")),
        "create_fn": str(workload.get("create_fn", f"CreateTpchQ{q}FunctionSet")),
        "test_target": str(workload.get("test_target", f"test_tpch_q{q}")),
        "bench_target": str(workload.get("bench_target", f"benchmark_tpch_q{q}")),
        "ctest_smoke_name": str(workload.get("ctest_smoke_name", f"tpch_q{q}_smoke_test")),
        "ctest_smoke_regex": str(workload.get("ctest_smoke_regex", f"^tpch_q{q}_smoke_test$")),
        "dataset_db": str(
            workload.get("dataset_db", pipeline_config.tpch_db_sf1)
        ),
        "scale_db": str(workload.get("scale_db", pipeline_config.tpch_db_sf10)),
        "db_env_var": str(workload.get("db_env_var", "ACCEL_TPCH_DB")),
        "header_path": f"include/accelerator_codegen/{header}.hpp",
        "kernel_path": f"src/generated/{source_subdir}/q{q}_kernel.cpp",
        "package_path": f"src/generated/{source_subdir}/q{q}_package.cpp",
        "fused_scan_path": f"src/generated/{source_subdir}/q{q}_fused_scan.cpp",
        "fused_header_path": f"include/accelerator_codegen/{header}_fused.hpp",
        "test_path": f"tests/single-threaded/{workload.get('test_target', f'test_tpch_q{q}')}.cpp",
        "benchmark_path": f"benchmarks/{source_subdir.split('/')[0]}/single-threaded/benchmark_q{q}.cpp",
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

    datasets: dict[str, Path] = {}
    for key, value in dict(data.get("datasets", {})).items():
        datasets[key] = Path(value)
    datasets.setdefault("tpch_sf1", Path(data["tpch_db_sf1"]))
    datasets.setdefault("tpch_sf10", Path(data["tpch_db_sf10"]))

    return PipelineConfig(
        repo_root=root,
        pipeline_root=pipe,
        build_dir=root / build_rel,
        tpch_db_sf1=Path(data["tpch_db_sf1"]),
        tpch_db_sf10=Path(data["tpch_db_sf10"]),
        datasets=datasets,
        optimization_max_iterations=int(opt.get("max_iterations", 5)),
        optimization_no_improvement_stop=int(opt.get("no_improvement_stop", 5)),
        optimization_episodes_per_iteration=int(opt.get("episodes_per_iteration", 4)),
        optimization_max_turns_per_episode=int(opt.get("max_turns_per_episode", 8)),
        crash_debug_max_rounds=int(crash_debug.get("max_rounds", 0)),
        crash_debug_safety_cap=int(crash_debug.get("safety_cap", 25)),
        stage_max_turns=dict(data.get("stage_max_turns", {})),
        write_paths=list(data.get("write_paths", [])),
        allowed_commands=list(data.get("allowed_commands", [])),
        gates_dir=root / gates_rel,
    )


def load_model_config(path: Path | None = None, repo_root: Path | None = None) -> ModelConfig:
    root = repo_root or find_repo_root()
    cfg_path = path or (pipeline_root(root) / "config" / "models.yaml")
    data = _load_yaml(cfg_path)
    return ModelConfig(
        provider=str(data.get("provider", "openai")),
        api_key_env=str(data.get("api_key_env", "OPENAI_API_KEY")),
        presets=dict(data.get("presets", {})),
        stages=dict(data.get("stages", {})),
    )


def load_aux_state_defaults(repo_root: Path | None = None) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    path = pipeline_root(root) / "config" / "aux_state.yaml"
    if not path.is_file():
        return {}
    return _load_yaml(path)


def apply_workload_overrides(pipeline_config: PipelineConfig, workload: dict[str, Any]) -> PipelineConfig:
    extra_write_paths = list(workload.get("write_paths", []))
    extra_allowed = list(workload.get("allowed_commands", []))

    if workload.get("workload_kind") == "aux_state":
        aux_defaults = load_aux_state_defaults(pipeline_config.repo_root)
        extra_write_paths = list(aux_defaults.get("write_paths", [])) + extra_write_paths
        extra_allowed = list(aux_defaults.get("allowed_commands", [])) + extra_allowed

    if not extra_write_paths and not extra_allowed:
        return pipeline_config

    return PipelineConfig(
        repo_root=pipeline_config.repo_root,
        pipeline_root=pipeline_config.pipeline_root,
        build_dir=pipeline_config.build_dir,
        tpch_db_sf1=pipeline_config.tpch_db_sf1,
        tpch_db_sf10=pipeline_config.tpch_db_sf10,
        datasets=dict(pipeline_config.datasets),
        optimization_max_iterations=pipeline_config.optimization_max_iterations,
        optimization_no_improvement_stop=pipeline_config.optimization_no_improvement_stop,
        optimization_episodes_per_iteration=pipeline_config.optimization_episodes_per_iteration,
        optimization_max_turns_per_episode=pipeline_config.optimization_max_turns_per_episode,
        crash_debug_max_rounds=pipeline_config.crash_debug_max_rounds,
        crash_debug_safety_cap=pipeline_config.crash_debug_safety_cap,
        stage_max_turns=dict(pipeline_config.stage_max_turns),
        write_paths=list(pipeline_config.write_paths) + extra_write_paths,
        allowed_commands=list(pipeline_config.allowed_commands) + extra_allowed,
        gates_dir=pipeline_config.gates_dir,
    )


def load_workload(name: str, repo_root: Path | None = None) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    path = pipeline_root(root) / "config" / "workloads" / f"{name}.yaml"
    if not path.is_file():
        raise FileNotFoundError(f"workload not found: {path}")
    data = _load_yaml(path)

    benchmark_name = data.get("benchmark_name", "tpch")
    try:
        pack = load_benchmark_pack(benchmark_name, root)
        pipeline_config = load_pipeline_config(repo_root=root)
        data = _merge_pack_into_workload(data, pack, root, pipeline_config)
    except FileNotFoundError:
        data.setdefault("benchmark_name", "tpch")
        data.setdefault("artifact_namespace", "tpch")

    if data.get("workload_kind") == "aux_state":
        aux_defaults = load_aux_state_defaults(root)
        for key in (
            "post_implementation_gates",
            "post_optimization_gates",
            "post_audit_gates",
            "prompt_subdir",
            "max_structural_reanalysis",
        ):
            if key in aux_defaults and key not in data:
                data[key] = aux_defaults[key]
        if "speedup_verification" in aux_defaults and "speedup_verification" not in data:
            data["speedup_verification"] = dict(aux_defaults["speedup_verification"])
    return data
