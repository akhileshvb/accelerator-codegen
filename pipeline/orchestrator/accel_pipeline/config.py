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


def load_workload(name: str, repo_root: Path | None = None) -> dict[str, Any]:
    root = repo_root or find_repo_root()
    path = pipeline_root(root) / "config" / "workloads" / f"{name}.yaml"
    # TODO: Support benchmark_packs/*.yaml for non-TPC-H workloads (naming, gates, write_paths).
    if not path.is_file():
        raise FileNotFoundError(f"workload not found: {path}")
    return _load_yaml(path)
