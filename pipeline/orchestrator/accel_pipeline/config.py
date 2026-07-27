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
    optimization_max_iterations: int = 5
    optimization_no_improvement_stop: int = 2
    stage_max_turns: dict[str, int] = field(default_factory=dict)
    write_paths: list[str] = field(default_factory=list)
    allowed_commands: list[str] = field(default_factory=list)
    gates_dir: Path = field(default_factory=Path)


def _load_yaml(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as f:
        data = yaml.safe_load(f)
    return data or {}


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

    return PipelineConfig(
        repo_root=root,
        pipeline_root=pipe,
        build_dir=root / build_rel,
        tpch_db_sf1=Path(data["tpch_db_sf1"]),
        tpch_db_sf10=Path(data["tpch_db_sf10"]),
        optimization_max_iterations=int(opt.get("max_iterations", 5)),
        optimization_no_improvement_stop=int(opt.get("no_improvement_stop", 2)),
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
    if workload.get("workload_kind") != "aux_state":
        return pipeline_config
    aux_defaults = load_aux_state_defaults(pipeline_config.repo_root)
    extra_write_paths = list(aux_defaults.get("write_paths", []))
    extra_allowed = list(aux_defaults.get("allowed_commands", []))
    if not extra_write_paths and not extra_allowed:
        return pipeline_config
    return PipelineConfig(
        repo_root=pipeline_config.repo_root,
        pipeline_root=pipeline_config.pipeline_root,
        build_dir=pipeline_config.build_dir,
        tpch_db_sf1=pipeline_config.tpch_db_sf1,
        tpch_db_sf10=pipeline_config.tpch_db_sf10,
        optimization_max_iterations=pipeline_config.optimization_max_iterations,
        optimization_no_improvement_stop=pipeline_config.optimization_no_improvement_stop,
        stage_max_turns=dict(pipeline_config.stage_max_turns),
        write_paths=list(pipeline_config.write_paths) + extra_write_paths,
        allowed_commands=list(pipeline_config.allowed_commands) + extra_allowed,
        gates_dir=pipeline_config.gates_dir,
    )


def load_workload(name: str, repo_root: Path | None = None) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    path = pipeline_root(root) / "config" / "workloads" / f"{name}.yaml"
    # TODO: Support benchmark_packs/*.yaml for non-TPC-H workloads (naming, gates, write_paths).
    if not path.is_file():
        raise FileNotFoundError(f"workload not found: {path}")
    data = _load_yaml(path)
    if data.get("workload_kind") == "aux_state":
        aux_defaults = load_aux_state_defaults(root)
        for key in ("post_implementation_gates", "post_optimization_gates", "post_audit_gates", "prompt_subdir"):
            if key in aux_defaults and key not in data:
                data[key] = aux_defaults[key]
        if "speedup_verification" in aux_defaults and "speedup_verification" not in data:
            data["speedup_verification"] = dict(aux_defaults["speedup_verification"])
    return data
