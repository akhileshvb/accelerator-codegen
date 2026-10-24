from __future__ import annotations

import copy
import os
import re
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline import naming
from accel_pipeline.engine_profile import (
    EngineProfileError,
    host_runtime_of,
    load_engine_profile,
    registration_symbol_pattern,
    validate_engine_profile,
)
from accel_pipeline.layout import backend_glob, query_dir, query_glob
from accel_pipeline.paths import find_repo_root, pipeline_root
from accel_pipeline.shared_file_lease import DEFAULT_SHARED_PATH_GLOBS


@dataclass
class ModelConfig:
    api_key_env: str
    presets: dict[str, str]
    stages: dict[str, str]
    stage_providers: dict[str, str] = field(default_factory=dict)
    codex_bin: str = "codex"
    codex_config_path: str = "pipeline/.codex/config.toml"
    codex_timeout_sec: int = 7200
    # Headless codex exec cancels MCP tools without this (openai/codex#24135).
    codex_bypass_approvals_and_sandbox: bool = True
    # Per-stage reasoning effort passed to codex (model_reasoning_effort):
    # minimal | low | medium | high.  Unset stages use the model's default.
    stage_reasoning_effort: dict[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class MeasurementConfig:
    """Thresholds of measurement.check_measurement (the `measurement:` block of default.yaml)."""

    enabled: bool = True
    min_hit_rate: float = 0.5
    max_fallbacks_on_hit: int = 0
    require_state_built: bool = True
    max_cv: float = 0.5
    min_ref_us: float = 1.0
    max_ref_s: float = 600.0

    @classmethod
    def from_dict(cls, raw: dict[str, Any] | None) -> "MeasurementConfig":
        raw = raw or {}
        return cls(
            enabled=bool(raw.get("enabled", cls.enabled)),
            min_hit_rate=float(raw.get("min_hit_rate", cls.min_hit_rate)),
            max_fallbacks_on_hit=int(raw.get("max_fallbacks_on_hit", cls.max_fallbacks_on_hit)),
            require_state_built=bool(raw.get("require_state_built", cls.require_state_built)),
            max_cv=float(raw.get("max_cv", cls.max_cv)),
            min_ref_us=float(raw.get("min_ref_us", cls.min_ref_us)),
            max_ref_s=float(raw.get("max_ref_s", cls.max_ref_s)),
        )


@dataclass(frozen=True)
class RepairConfig:
    max_code_rounds: int = 3
    infra_retries: int = 1
    max_replans: int = 1
    max_orchestrator_retries: int = 1
    load_factor: float = 1.5
    never_quarantine: tuple[str, ...] = ("aux_correctness", "aux_unit")
    parallel_gates: bool = True


@dataclass(frozen=True)
class SessionConfig:
    continuity: bool = True
    soft_deadline_sec: int = 5400
    idle_timeout_sec: int = 2400
    hard_deadline_sec: int = 14400
    max_resumes: int = 25
    max_context_tokens: int = 0


@dataclass
class PipelineConfig:
    repo_root: Path
    pipeline_root: Path
    build_dir: Path
    tpch_db_sf1: Path
    tpch_db_sf10: Path
    datasets: dict[str, Path] = field(default_factory=dict)
    # Benchmarks whose bench/calibrate harness is one query-parameterised target per engine.
    shared_bench_harness: tuple[str, ...] = ()
    optimization_max_iterations: int = 3
    optimization_no_improvement_stop: int = 2
    # Wall-clock budget of the whole optimization stage (seconds; 0: none).
    optimization_stage_budget_sec: float = 7200.0
    optimization_episodes_per_iteration: int = 4
    optimization_max_attempts_per_iteration: int = 4
    optimization_max_turns_per_episode: int = 8
    optimization_improvement_threshold_pct: float = 3.0
    optimization_reject_threshold_pct: float = 2.0
    optimization_consecutive_fail_stop: int = 0
    optimization_quick_benchmark_samples: int = 3
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
    optimization_build_fix_max_rounds: int = 0
    optimization_correctness_fix_max_rounds: int = 0
    optimization_build_fix_safety_cap: int = 12
    optimization_correctness_fix_safety_cap: int = 12
    optimization_fix_log_chars: int = 6000
    optimization_persistent_worker: bool = True
    optimization_worker_max_turns_per_candidate: int = 0
    optimization_worker_safety_cap_turns: int = 80
    optimization_hot_path_digest_max_chars: int = 25_000
    optimization_turn_delta_max_chars: int = 4_000
    optimization_checkpoint_file: str = "optimization_worker_checkpoint.json"
    # Agent may declare OPTIMIZATION_SATURATED when stuck (no numeric floors).
    optimization_early_stop: dict[str, Any] = field(default_factory=dict)
    repair: RepairConfig = field(default_factory=RepairConfig)
    session: SessionConfig = field(default_factory=SessionConfig)
    stage_max_turns: dict[str, int] = field(default_factory=dict)
    write_paths: list[str] = field(default_factory=list)
    allowed_commands: list[str] = field(default_factory=list)
    gates_dir: Path = field(default_factory=Path)
    # Concurrent pack workers (shared tree + separate builds).
    worker_id: str = ""
    codex_home: Path | None = None
    shared_paths: list[str] = field(default_factory=list)
    # The run's identity (set by apply_workload_overrides); write_ownership uses it
    # to apply the per-layer write rules (backends/<engine>/** for engine_backend
    # runs, accelerators/<bench>/q<N>/** for query runs).
    workload_kind: str = ""
    engine_name: str = ""
    benchmark_name: str = ""
    # bwrap | none: the OS sandbox of every C++-mode agent step (accel_pipeline.sandbox).
    sandbox: str = "bwrap"
    sandbox_extra_ro: list[str] = field(default_factory=list)
    sandbox_extra_rw: list[str] = field(default_factory=list)
    # enforce | warn | off: the post-stage ownership check (write_ownership.check_stage).
    write_policy: str = "warn"
    measurement: MeasurementConfig = field(default_factory=MeasurementConfig)


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


def _dataset_path(path: str | Path) -> str:
    """A dataset database path; empty when the run parameter is unset."""
    return "" if Path(path) == Path() else str(path)


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
    # Per-engine query tree: accelerators/{bench}/q{N}/{engine}/... (source_subdir is
    # relative to accelerators/, the second include root).
    qdir = query_dir(benchmark, q)
    eng_seg = f"{engine}/" if engine else ""
    source_subdir = str(
        workload.get(
            "source_subdir",
            f"{benchmark}/q{q}/{engine}/base/single-threaded" if engine else f"{benchmark}/q{q}/base/single-threaded",
        )
    )
    generated_base = f"{qdir}/{eng_seg}base/single-threaded"
    # Every engine ships one query-parameterised bench/calibrate harness per benchmark that has one
    # (backends/<engine>/bench/, made by host_bootstrap); the others keep per-query targets.
    shared_harness = (
        bool(engine)
        and workload.get("workload_kind") != "engine_backend"
        and benchmark in pipeline_config.shared_bench_harness
    )
    harness = harness_targets(engine, benchmark) if shared_harness else {}
    bench_target = f"benchmark_{engine}_{benchmark}_q{q}" if shared_harness else naming.benchmark_target(benchmark, q)
    test_target = naming.unit_test_target(benchmark, q)

    paths = {
        "source_subdir": source_subdir,
        "function_name": naming.base_function_name(benchmark, q),
        "create_fn": str(workload.get("create_fn", "")),
        "test_target": test_target,
        "bench_target": bench_target,
        "ctest_smoke_name": naming.smoke_ctest_name(benchmark, q),
        "ctest_smoke_regex": f"^{naming.smoke_ctest_name(benchmark, q)}$",
        "dataset_db": _dataset_path(workload.get("dataset_db", pipeline_config.tpch_db_sf1)),
        "scale_db": _dataset_path(workload.get("scale_db", pipeline_config.tpch_db_sf10)),
        "db_env_var": str(workload.get("db_env_var", "ACCEL_TPCH_DB")),
        "header_path": f"{qdir}/{eng_seg}base/query.hpp",
        "kernel_path": f"{generated_base}/kernel.cpp",
        "package_path": f"{generated_base}/package.cpp",
        "fused_scan_path": f"{generated_base}/fused_scan.cpp",
        "runtime_path": f"accelerators/{benchmark}/common/runtime.cpp",
        "fused_header_path": f"{qdir}/{eng_seg}base/fused.hpp",
        "aux_module_header_path": f"{qdir}/{eng_seg}aux/module.hpp",
        "aux_finalize_header_path": f"{qdir}/{eng_seg}aux/finalize.hpp",
        "catalog_path": f"backends/{engine}/api/catalog/query_catalog.hpp" if engine else "",
        # Where the query's cost models live (AuxCostModelLocation), for every engine.
        "cost_model_header_path": f"{qdir}/cost_model.hpp",
        "test_path": f"{qdir}/tests/{test_target}.cpp",
        "benchmark_path": f"{qdir}/bench/benchmark_q{q}.cpp",
        "engine_name": engine,
    }

    if workload.get("workload_kind") == "aux_state":
        aux_bench = harness.get("bench_target") or f"{naming.benchmark_target(benchmark, q)}_aux"
        calibrate = harness.get("calibrate_target") or f"calibrate_{benchmark}_q{q}_aux_cost_model"
        aux_state_subdir = f"{qdir}/{eng_seg}aux"
        aux_finalize = "finalize.cpp"
        if benchmark == "sec_edgar":
            cost_model_stem = f"sec_edgar_q{q}_aux_cost_model"
        else:
            cost_model_stem = f"q{q}_aux_cost_model"
        # The query-wide default model (cost_model.hpp's location); an engine with its
        # own calibration keeps it beside the default as <stem>_<engine>.json.
        default_cost_model = f"{qdir}/config/{cost_model_stem}.json"
        engine_cost_model = f"{qdir}/config/{cost_model_stem}_{engine}.json"
        bench_sweep = workload.get("aux_bench_sweep", {})
        if not isinstance(bench_sweep, dict):
            bench_sweep = {}
        if benchmark == "sec_edgar":
            bench_suffix = f"q{q}_aux_bench_sf3"
        else:
            bench_suffix = f"q{q}_aux_bench_sf1"
        paths.update(
            {
                "aux_function_name": naming.aux_function_name(benchmark, q),
                "aux_test_target": f"{test_target}_aux",
                "aux_bench_target": aux_bench,
                "aux_hybrid_bench_target": naming.hybrid_benchmark_target(benchmark, q),
                "aux_calibrate_target": calibrate,
                "aux_ctest_smoke_regex": f"^{naming.aux_smoke_ctest_name(benchmark, q)}$",
                "aux_state_subdir": aux_state_subdir,
                "aux_finalize_path": f"{aux_state_subdir}/{aux_finalize}",
                "cost_model_default": default_cost_model,
                "cost_model_config": engine_cost_model,
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
        all_paths = [
            f"{aux}/module.hpp",
            f"{aux}/module.cpp",
            f"{aux}/finalize.cpp",
        ]
        cost_model = paths.get("cost_model_config")
        if cost_model:
            all_paths.append(cost_model)
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


def _choice(data: dict[str, Any], key: str, allowed: tuple[str, ...], default: str) -> str:
    value = str(data.get(key, default))
    if value not in allowed:
        raise ValueError(f"{key}: {value!r} (expected one of {', '.join(allowed)})")
    return value


def _repair_config(block: dict[str, Any]) -> RepairConfig:
    defaults = RepairConfig()
    return RepairConfig(
        max_code_rounds=int(block.get("max_code_rounds", defaults.max_code_rounds)),
        infra_retries=int(block.get("infra_retries", defaults.infra_retries)),
        max_replans=int(block.get("max_replans", defaults.max_replans)),
        max_orchestrator_retries=int(block.get("max_orchestrator_retries", defaults.max_orchestrator_retries)),
        load_factor=float(block.get("load_factor", defaults.load_factor)),
        never_quarantine=tuple(str(g) for g in block.get("never_quarantine", defaults.never_quarantine)),
        parallel_gates=bool(block.get("parallel_gates", defaults.parallel_gates)),
    )


def _session_config(block: dict[str, Any]) -> SessionConfig:
    defaults = SessionConfig()
    return SessionConfig(
        continuity=bool(block.get("continuity", defaults.continuity)),
        soft_deadline_sec=int(block.get("soft_deadline_sec", defaults.soft_deadline_sec)),
        idle_timeout_sec=int(block.get("idle_timeout_sec", defaults.idle_timeout_sec)),
        hard_deadline_sec=int(block.get("hard_deadline_sec", defaults.hard_deadline_sec)),
        max_resumes=int(block.get("max_resumes", defaults.max_resumes)),
        max_context_tokens=int(block.get("max_context_tokens", defaults.max_context_tokens)),
    )


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
    max_turns = dict(opt.get("max_turns") or {})

    datasets = {
        key: Path(os.environ[env_var])
        for key, env_var in dict(data.get("datasets", {})).items()
        if os.environ.get(env_var)
    }

    shared_paths = [str(x) for x in list(data.get("shared_paths") or [])]
    if not shared_paths:
        shared_paths = list(DEFAULT_SHARED_PATH_GLOBS)

    cfg = PipelineConfig(
        repo_root=root,
        pipeline_root=pipe,
        build_dir=root / build_rel,
        tpch_db_sf1=datasets.get("tpch_sf1", Path()),
        tpch_db_sf10=datasets.get("tpch_sf10", Path()),
        datasets=datasets,
        shared_bench_harness=tuple(str(b) for b in data.get("shared_bench_harness") or ()),
        optimization_max_iterations=int(opt.get("max_iterations", 3)),
        optimization_no_improvement_stop=int(opt.get("no_improvement_stop", 2)),
        optimization_stage_budget_sec=float(opt.get("stage_budget_sec", 7200)),
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
        optimization_build_fix_max_rounds=int(opt.get("build_fix_max_rounds", 0)),
        optimization_correctness_fix_max_rounds=int(opt.get("correctness_fix_max_rounds", 0)),
        optimization_build_fix_safety_cap=int(opt.get("build_fix_safety_cap", 12)),
        optimization_correctness_fix_safety_cap=int(opt.get("correctness_fix_safety_cap", 12)),
        optimization_fix_log_chars=int(opt.get("fix_log_chars", 6000)),
        optimization_persistent_worker=bool(opt.get("persistent_worker", True)),
        optimization_worker_max_turns_per_candidate=int(opt.get("worker_max_turns_per_candidate", 0)),
        optimization_worker_safety_cap_turns=int(opt.get("worker_safety_cap_turns", 80)),
        optimization_hot_path_digest_max_chars=int(opt.get("hot_path_digest_max_chars", 25_000)),
        optimization_turn_delta_max_chars=int(opt.get("turn_delta_max_chars", 4_000)),
        optimization_checkpoint_file=str(opt.get("checkpoint_file", "optimization_worker_checkpoint.json")),
        optimization_early_stop=dict(opt.get("early_stop") or {}),
        repair=_repair_config(data.get("repair") or {}),
        session=_session_config(data.get("session") or {}),
        stage_max_turns=dict(data.get("stage_max_turns", {})),
        write_paths=list(data.get("write_paths", [])),
        allowed_commands=list(data.get("allowed_commands", [])),
        gates_dir=root / gates_rel,
        worker_id=str(os.environ.get("ACCEL_PIPELINE_WORKER_ID") or ""),
        shared_paths=shared_paths,
        sandbox=_choice(data, "sandbox", ("bwrap", "none"), "bwrap"),
        sandbox_extra_ro=[str(x) for x in data.get("sandbox_extra_ro") or []],
        sandbox_extra_rw=[str(x) for x in data.get("sandbox_extra_rw") or []],
        write_policy=_choice(data, "write_policy", ("enforce", "warn", "off"), "warn"),
        measurement=MeasurementConfig.from_dict(data.get("measurement")),
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
        api_key_env=str(data.get("api_key_env", "OPENROUTER_API_KEY")),
        presets=dict(data.get("presets", {})),
        stages=dict(data.get("stages", {})),
        stage_providers={str(k): str(v) for k, v in dict(data.get("stage_providers") or {}).items()},
        codex_bin=str(data.get("codex_bin", "codex")),
        codex_config_path=str(data.get("codex_config_path", "pipeline/.codex/config.toml")),
        codex_timeout_sec=int(data.get("codex_timeout_sec", 7200)),
        codex_bypass_approvals_and_sandbox=bool(
            data.get("codex_bypass_approvals_and_sandbox", True)
        ),
        stage_reasoning_effort={str(k): str(v) for k, v in (data.get("stage_reasoning_effort") or {}).items()},
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
        "differential_source": f"backends/{engine}/tests/harness/{diff}.cpp",
        "differential_command": f"host.sh run {diff} <query>",
        "query_host_ready_hint": (
            f"The {engine} workload harnesses ({', '.join(targets.values())}) are generated by the "
            f"host_bootstrap stage under backends/{engine}/tests/harness/ and backends/{engine}/bench/; "
            f"a query only registers from `{register}`. See pipeline/prompts/shared/query_host_bootstrap.md."
        ),
    }


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
        raise ValueError(
            f"workload {name!r} supports engines {supported}: pick one (--engine / pack `engines:`)"
        )
    if engine not in supported:
        raise ValueError(f"workload {name!r} does not support engine {engine!r} (supports {supported})")
    return engine


def _normalize_accelerator_engine(data: dict[str, Any], engine: str, repo_root: Path) -> dict[str, Any]:
    """Pin the run's engine and attach host conventions for accelerators."""
    data["engine_name"] = engine
    # Generated query tree for this engine.
    bench = str(data.get("benchmark_name") or data.get("benchmark") or "tpch")
    qnum = data.get("query_number")
    host = engine_host_conventions(engine, bench, qnum if qnum is not None else "<N>", repo_root)
    data["engine_host"] = host
    writes = list(data.get("write_paths") or [])
    # The query's run owns its directory (one glob); backends/<engine>/** belongs to
    # the engine_backend run and substrate/** to nobody (write_ownership enforces both).
    extra = query_glob(bench, qnum) if qnum is not None else ""
    if extra and extra not in writes:
        writes.append(extra)
    data["write_paths"] = _dedupe_preserve(writes)
    return data


def _derive_query_names(data: dict[str, Any], engine: str, root: Path) -> None:
    """The query's names (naming.py), plus the engine profile's registration symbol."""
    bench, q = str(data.get("benchmark_name", "tpch")), data.get("query_number")
    if q is None:
        return
    pattern = registration_symbol_pattern(load_engine_profile(engine, pipeline_root(root)))
    data.update(
        function_name=naming.base_function_name(bench, q),
        create_fn=naming.registration_symbol(pattern, bench, q) if pattern else "",
        header_basename=naming.header_basename(bench, q),
        test_target=naming.unit_test_target(bench, q),
        ctest_smoke_regex=f"^{naming.smoke_ctest_name(bench, q)}$",
    )
    if data.get("workload_kind") == "aux_state":
        data.update(
            base_function_name=naming.base_function_name(bench, q),
            aux_function_name=naming.aux_function_name(bench, q),
            accelerator_id=naming.accelerator_id(bench, q),
            prerequisite_workload=naming.base_workload_name(bench, q),
        )


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
    data.setdefault("prompt_subdir", str(defaults.get("prompt_subdir") or "engine_backend"))
    data.setdefault("query_number", 0)
    # The benchmark whose schema and dataset the backend gates load (the backend names no query).
    data.setdefault("dataset_benchmark", str(defaults.get("dataset_benchmark") or ""))
    data.setdefault(
        "allow_update_existing",
        bool(defaults.get("allow_update_existing", True)),
    )

    profile = data.get("engine_profile")
    validate_engine_profile(profile, pipeline_root(repo_root), source=f"workload {engine}")
    if profile["identity"]["name"] != engine:
        raise EngineProfileError(
            f"workload {engine}: engine_profile.identity.name is {profile['identity']['name']!r}"
        )
    data["display_name"] = profile["identity"]["display_name"]
    data["host_runtime"] = host_runtime_of(profile)
    data["requires_in_process_host"] = profile["packaging"]["requires_in_process_host"]
    data["placement_candidates"] = list(profile["packaging"]["placement_candidates"])

    for key in (
        "post_implementation_gates",
        "post_validation_gates",
        "stage_max_turns",
        "hide_foreign_engines",
        "agent_hidden_trees",
        "engine_profile_sections",
        "architecture_audit_max_rounds",
        "layer_critique_max_rounds",
        "layer_gate_max_rounds",
        "implementation_layers",
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
    cmake["build_flag"] = profile["build"]["cmake_option"]
    data["cmake"] = cmake

    expanded_writes = [
        _expand_engine_placeholders(str(p), engine)
        for p in list(defaults.get("write_paths") or []) + list(data.get("write_paths") or [])
    ]
    # The engine's run owns backends/<engine>/** (incl. api/catalog/), whatever the YAML lists.
    data["write_paths"] = _dedupe_preserve([backend_glob(engine), *expanded_writes])

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


def _expand_fairness_search_paths(workload: dict[str, Any], engine: str) -> None:
    """Expand {bench}/{engine} in `fairness.kernel_search_paths` for the run."""
    fairness = workload.get("fairness")
    paths = fairness.get("kernel_search_paths") if isinstance(fairness, dict) else None
    if paths:
        bench = str(workload.get("benchmark_name", "tpch"))
        fairness["kernel_search_paths"] = [
            str(p).replace("{bench}", bench).replace("{engine}", engine) for p in paths
        ]


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
                    merged_fair["kernel_search_paths"] = copy.deepcopy(
                        aux_defaults["fairness"]["kernel_search_paths"]
                    )
                if "cardinality_constants" not in merged_fair and workload.get("fairness"):
                    merged_fair["cardinality_constants"] = list(
                        (workload.get("fairness") or {}).get("cardinality_constants") or []
                    )
                workload["fairness"] = merged_fair
            else:
                workload[key] = aux_defaults[key]
    for key in ("requires_base_accelerator", "prerequisite_min_stage", "allow_poor_fitness", "allow_unsuccessful_design"):
        workload.setdefault(key, aux_defaults[key])
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
    return PipelineConfig(
        repo_root=pipeline_config.repo_root,
        pipeline_root=pipeline_config.pipeline_root,
        build_dir=pipeline_config.build_dir,
        tpch_db_sf1=pipeline_config.tpch_db_sf1,
        tpch_db_sf10=pipeline_config.tpch_db_sf10,
        datasets=dict(pipeline_config.datasets),
        shared_bench_harness=pipeline_config.shared_bench_harness,
        optimization_max_iterations=int(
            opt.get("max_iterations", pipeline_config.optimization_max_iterations)
        ),
        optimization_no_improvement_stop=int(
            opt.get("no_improvement_stop", pipeline_config.optimization_no_improvement_stop)
        ),
        optimization_stage_budget_sec=float(
            opt.get("stage_budget_sec", pipeline_config.optimization_stage_budget_sec)
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
        repair=pipeline_config.repair,
        session=pipeline_config.session,
        measurement=pipeline_config.measurement,
        stage_max_turns=dict(pipeline_config.stage_max_turns),
        write_paths=list(pipeline_config.write_paths),
        allowed_commands=list(pipeline_config.allowed_commands),
        gates_dir=pipeline_config.gates_dir,
        worker_id=pipeline_config.worker_id,
        codex_home=pipeline_config.codex_home,
        shared_paths=list(pipeline_config.shared_paths or []),
        workload_kind=pipeline_config.workload_kind,
        engine_name=pipeline_config.engine_name,
        benchmark_name=pipeline_config.benchmark_name,
        sandbox=pipeline_config.sandbox,
        sandbox_extra_ro=list(pipeline_config.sandbox_extra_ro),
        sandbox_extra_rw=list(pipeline_config.sandbox_extra_rw),
        write_policy=pipeline_config.write_policy,
    )


def apply_workload_overrides(pipeline_config: PipelineConfig, workload: dict[str, Any]) -> PipelineConfig:
    merged = _apply_workload_overrides(pipeline_config, workload)
    kind = str(workload.get("workload_kind") or "")
    engine = str(workload.get("engine_name") or "")
    if engine:
        engine = sanitize_engine_name(engine)
    bench = str(workload.get("benchmark_name") or ("" if kind == "engine_backend" else "tpch"))
    return replace(merged, workload_kind=kind, engine_name=engine, benchmark_name=bench)


def _apply_workload_overrides(pipeline_config: PipelineConfig, workload: dict[str, Any]) -> PipelineConfig:
    extra_write_paths = list(workload.get("write_paths", []))
    extra_allowed = list(workload.get("allowed_commands", []))
    stage_max_turns = dict(pipeline_config.stage_max_turns)
    optimization_max_turns_per_episode = pipeline_config.optimization_max_turns_per_episode
    engine = sanitize_engine_name(str(workload.get("engine_name") or ""))
    if workload.get("workload_kind") != "engine_backend" and not engine:
        raise ValueError(
            "accelerator workload requires explicit engine_name "
            "(no default engine)"
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

    # The engine's differential harness, built and run by the accelerator run (its source is the
    # backend's, made by host_bootstrap).
    host = workload.get("engine_host") or {}
    if workload.get("workload_kind") != "engine_backend" and host.get("differential_binary"):
        binary = re.escape(host["differential_binary"])
        extra_allowed = list(extra_allowed) + [
            rf"^(\\.\\/)?build-release\\/{binary}( .*)?$",
            rf"^cmake --build build-release --target {binary}( .*)?$",
            rf"^ACCEL_[A-Z_]+=\\S+( ACCEL_[A-Z_]+=\\S+)* (\\.\\/)?build-release\\/{binary}( .*)?$",
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
        shared_bench_harness=pipeline_config.shared_bench_harness,
        optimization_max_iterations=pipeline_config.optimization_max_iterations,
        optimization_no_improvement_stop=pipeline_config.optimization_no_improvement_stop,
        optimization_stage_budget_sec=pipeline_config.optimization_stage_budget_sec,
        optimization_episodes_per_iteration=pipeline_config.optimization_episodes_per_iteration,
        optimization_max_attempts_per_iteration=pipeline_config.optimization_max_attempts_per_iteration,
        optimization_max_turns_per_episode=optimization_max_turns_per_episode,
        optimization_improvement_threshold_pct=pipeline_config.optimization_improvement_threshold_pct,
        optimization_reject_threshold_pct=pipeline_config.optimization_reject_threshold_pct,
        optimization_consecutive_fail_stop=pipeline_config.optimization_consecutive_fail_stop,
        optimization_quick_benchmark_samples=pipeline_config.optimization_quick_benchmark_samples,
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
        optimization_build_fix_max_rounds=pipeline_config.optimization_build_fix_max_rounds,
        optimization_correctness_fix_max_rounds=pipeline_config.optimization_correctness_fix_max_rounds,
        optimization_build_fix_safety_cap=pipeline_config.optimization_build_fix_safety_cap,
        optimization_correctness_fix_safety_cap=pipeline_config.optimization_correctness_fix_safety_cap,
        optimization_fix_log_chars=pipeline_config.optimization_fix_log_chars,
        optimization_persistent_worker=pipeline_config.optimization_persistent_worker,
        optimization_worker_max_turns_per_candidate=pipeline_config.optimization_worker_max_turns_per_candidate,
        optimization_worker_safety_cap_turns=pipeline_config.optimization_worker_safety_cap_turns,
        optimization_hot_path_digest_max_chars=pipeline_config.optimization_hot_path_digest_max_chars,
        optimization_turn_delta_max_chars=pipeline_config.optimization_turn_delta_max_chars,
        optimization_checkpoint_file=pipeline_config.optimization_checkpoint_file,
        optimization_early_stop=dict(pipeline_config.optimization_early_stop),
        repair=pipeline_config.repair,
        session=pipeline_config.session,
        measurement=pipeline_config.measurement,
        stage_max_turns=stage_max_turns,
        write_paths=list(pipeline_config.write_paths) + extra_write_paths,
        allowed_commands=list(pipeline_config.allowed_commands) + extra_allowed,
        gates_dir=pipeline_config.gates_dir,
        worker_id=pipeline_config.worker_id,
        codex_home=pipeline_config.codex_home,
        shared_paths=list(pipeline_config.shared_paths or []),
        workload_kind=pipeline_config.workload_kind,
        engine_name=pipeline_config.engine_name,
        benchmark_name=pipeline_config.benchmark_name,
        sandbox=pipeline_config.sandbox,
        sandbox_extra_ro=list(pipeline_config.sandbox_extra_ro),
        sandbox_extra_rw=list(pipeline_config.sandbox_extra_rw),
        write_policy=pipeline_config.write_policy,
    )
    if has_opt:
        return _apply_optimization_block(merged, opt_block)
    return merged


def _read_workload_yaml(name: str, repo_root: Path | None) -> tuple[dict[str, Any], Path]:
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
    return _load_yaml(path), root


def workload_supported_engines(name: str, repo_root: Path | None = None) -> list[str]:
    data, _ = _read_workload_yaml(name, repo_root)
    return workload_engines(data)


def load_workload(name: str, repo_root: Path | None = None, *, engine: str = "") -> dict[str, Any]:
    """Load a workload; accelerator workloads are engine-neutral and need `engine` (see select_engine)."""
    data, root = _read_workload_yaml(name, repo_root)

    if data.get("workload_kind") == "engine_backend":
        return _normalize_engine_backend_workload(data, root)

    engine = select_engine(data, engine, name)
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
    _expand_fairness_search_paths(data, engine)
    _derive_query_names(data, engine, root)
    return _normalize_accelerator_engine(data, engine, root)


def load_run_workload(pipeline_config: PipelineConfig, name: str) -> dict[str, Any]:
    """`name` loaded for the engine of the run `pipeline_config` belongs to."""
    return load_workload(name, pipeline_config.repo_root, engine=pipeline_config.engine_name)
