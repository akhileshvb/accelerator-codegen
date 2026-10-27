"""The pipeline's settings: config/default.yaml and models.yaml, and the workloads that refine them.

Every tunable is a field below with its default; default.yaml and models.yaml only set what differs.
"""
from __future__ import annotations

import copy
import dataclasses
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
from accel_pipeline.test_suite import SuiteConfig

DEFAULT_BUILD_DIR = "build-release"
DEFAULT_GATES_DIR = "pipeline/gates"
SANDBOXES = ("bwrap", "none")
WRITE_POLICIES = ("enforce", "warn", "off")

# Paths that need the shared-file lease under concurrent pack workers.
DEFAULT_SHARED_PATH_GLOBS = (
    "backends/*/api/**",  # engine backends: generic only, queries register from their own directories
    "backends/*/engine/**",
    "backends/*/state/**",
    "eval/tests/test_support/**",
    "eval/bench/**/*_arg_variants.*",
    "accelerators/prims/**",  # changed only through primitive_publish
    "accelerators/*/common/**",  # helpers shared by one benchmark's queries
)


# ---- settings ---------------------------------------------------------------------------------

@dataclass
class ModelConfig:
    api_key_env: str = "OPENROUTER_API_KEY"
    presets: dict[str, str] = field(default_factory=dict)
    stages: dict[str, str] = field(default_factory=dict)
    stage_providers: dict[str, str] = field(default_factory=dict)
    codex_bin: str = "codex"
    codex_config_path: str = "pipeline/.codex/config.toml"
    codex_timeout_sec: int = 7200
    # Headless codex exec cancels MCP tools without this (openai/codex#24135).
    codex_bypass_approvals_and_sandbox: bool = True
    # model_reasoning_effort per stage (minimal | low | medium | high); unset stages use the model's default.
    stage_reasoning_effort: dict[str, str] = field(default_factory=dict)
    # Codex knows no OpenRouter model's window: it assumes ~258k and compacts at ~300k, folding a
    # session into a few-kilobyte summary every few turns.  0 leaves Codex's defaults.
    codex_model_context_window: int = 0
    codex_auto_compact_tokens: int = 0
    # What a compaction summary keeps (repo-relative); empty leaves Codex's generic summary.
    codex_compact_prompt_file: str = ""


@dataclass(frozen=True)
class MeasurementConfig:
    """Thresholds of measurement.check_measurement: is a benchmark measuring the accelerator?"""

    enabled: bool = True
    min_hit_rate: float = 0.5  # accel_hit_rate required at write frequency 0
    max_fallbacks_on_hit: int = 0
    require_state_built: bool = True
    max_cv: float = 0.5  # sample spread (stdev / mean) beyond which a score is noise
    min_ref_us: float = 1.0  # plausible range of the reference query time
    max_ref_s: float = 600.0


@dataclass(frozen=True)
class RepairConfig:
    """Failures go to a bounded repair loop: infra failures retry without spending the budget, code failures resume the agent's session."""

    max_code_rounds: int = 3
    infra_retries: int = 1
    max_replans: int = 1
    max_orchestrator_retries: int = 1
    load_factor: float = 1.5  # a gate timeout under loadavg > load_factor * cores is infra
    never_quarantine: tuple[str, ...] = ("aux_correctness", "aux_correctness_iterate", "aux_unit")
    provider_retries: int = 3  # network/provider flakes: their own budget, with a growing wait
    provider_backoff_sec: tuple[float, ...] = (30.0, 90.0, 270.0)

    def provider_backoff(self, retry: int) -> float:
        """Seconds to wait before provider retry number `retry` (1-based; the last wait repeats)."""
        return self.provider_backoff_sec[min(retry, len(self.provider_backoff_sec)) - 1] if self.provider_backoff_sec else 0.0


@dataclass(frozen=True)
class AdvisorConfig:
    """A stronger model forks the stuck agent's session for a second opinion (accel_pipeline.advisor)."""

    max_consults_per_stage: int = 4  # 0 disables the advisor
    stall_rounds: int = 2  # the same failure this many repair rounds in a row triggers an orchestrator consult
    timeout_sec: float = 480  # under the client's limit on the agent's ask_advisor call (gates.run.TOOL_CALL_LIMIT_S)


@dataclass(frozen=True)
class NoProgressConfig:
    """Seconds an agent may go without real progress (accel_pipeline.no_progress) before the
    footer asks it to state its blocker (nudge) and before its turn is ended with a directed
    resume (redirect); 0 disables. `stages` maps a stage to its own nudge_sec / redirect_sec."""

    nudge_sec: float = 480
    redirect_sec: float = 1200
    stages: dict[str, dict[str, float]] = field(default_factory=dict)

    def for_stage(self, stage: str) -> tuple[float, float]:
        """(nudge, redirect) seconds of one stage."""
        override = dict(dict(self.stages).get(stage, ()))
        return float(override.get("nudge_sec", self.nudge_sec)), float(override.get("redirect_sec", self.redirect_sec))


@dataclass(frozen=True)
class DataToolsConfig:
    """Limits of the agents' data exploration tools (tools/data_tools.py)."""

    query_seconds: float = 60.0  # time box of one statement
    default_rows: int = 200
    max_rows: int = 2000


@dataclass(frozen=True)
class SessionConfig:
    """One codex session per role (accel_pipeline/session.py) and the wall budgets of an agent run."""

    continuity: bool = True
    soft_deadline_sec: int = 5400  # MCP tool results start asking the agent to wrap up
    stage_soft_deadline_sec: dict[str, int] = field(default_factory=dict)  # per stage, instead of soft_deadline_sec
    idle_timeout_sec: int = 2400  # no rollout, tool or heartbeat activity: killed, then resumed
    hard_deadline_sec: int = 14400
    max_resumes: int = 25  # 0: no cap
    max_context_tokens: int = 0  # 0: no cap; Codex compacts at codex_auto_compact_tokens
    no_progress: NoProgressConfig = field(default_factory=NoProgressConfig)
    exit_gate: dict[str, str] = field(default_factory=dict)  # stage -> gate whose pass during the turn ends the agent's turn

    def exit_gate_for(self, stage: str) -> str:
        return dict(self.exit_gate).get(stage, "")

    def soft_deadline_for(self, stage: str) -> int:
        return dict(self.stage_soft_deadline_sec).get(stage, self.soft_deadline_sec)


@dataclass(frozen=True)
class QuickTierConfig:
    """The fast inner loop: quick_check, the quick bench tier, parallel gates."""

    enabled: bool = True
    timeout_sec: float = 120.0
    build_stamp_cache: bool = True
    differential_probes: int = 3
    parallel_gates: bool = True
    gate_jobs: int = 0  # concurrent gates in a parallel group; 0 = one per gate
    build_budget_sec: float = 15.0  # quick_check fails when the state build, projected to the full dataset, takes longer
    write_cost_budget_share: float = 0.5  # of one gate differential run's timeout (ACCEL_GATE_RUN_TIMEOUT): the projected writes and builds may use this much
    admission_gates: tuple[str, ...] = ("aux_exit", "aux_correctness", "aux_bench_sweep", "aux_tsan")  # request_gate refuses these until quick_check passed on the current code


@dataclass(frozen=True)
class ConcurrencyConfig:
    """CPU budgets and host-wide leases between concurrently running pipelines (core_budget, measurement_lease)."""

    workers: int = 1  # pipeline workers sharing this host's CPUs (a pack's concurrency overrides it)
    measure_cores: int | None = None  # CPUs reserved for benchmarks; None: none for one worker, else a fifth (2..8)
    measure_slots: int | None = None  # benchmarks measuring at once, each on its share of the measurement CPUs; None: one per 2 CPUs
    host_lock_dir: str = ""  # shared by every tree on the host; "": /dev/shm/accel-pipeline-<uid>
    measurement_lease: bool = True
    shared_lease_wait_sec: float = 300.0  # bound on queuing for the shared-tree lease

    def __post_init__(self) -> None:
        object.__setattr__(self, "workers", max(1, self.workers))
        if self.measure_slots is not None:
            object.__setattr__(self, "measure_slots", max(1, self.measure_slots))


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
    # The optimization stage: `optimization:` of default.yaml, refined by a workload's block.
    optimization_max_iterations: int = 3
    optimization_no_improvement_stop: int = 2
    optimization_stage_budget_sec: float = 7200.0  # no new iteration or attempt starts after it; 0: none
    optimization_max_attempts_per_iteration: int = 4
    optimization_improvement_threshold_pct: float = 3.0
    optimization_reject_threshold_pct: float = 2.0
    optimization_consecutive_fail_stop: int = 0
    optimization_quick_benchmark_samples: int = 3
    optimization_context_max_chars: int = 80_000
    optimization_context_max_lines: int = 1200
    optimization_bootstrap_max_chars: int = 300_000
    optimization_bootstrap_max_lines_per_file: int = 0
    optimization_smoke_runs: int = 2
    optimization_max_turns_analysis: int = 0  # the max_turns limits: 0 = unlimited
    optimization_max_turns_patch: int = 0
    optimization_max_turns_unified: int = 0
    # Fix rounds within one attempt until build/smoke pass: 0 = keep going up to the safety cap.
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
    # When the agent may declare OPTIMIZATION_SATURATED.
    optimization_early_stop: dict[str, Any] = field(default_factory=dict)
    # Redesigns of the state inside optimization: the full-tier gates they must pass, and the
    # changed-line fraction above which an undeclared attempt counts as one.
    optimization_structural: dict[str, Any] = field(default_factory=dict)
    repair: RepairConfig = field(default_factory=RepairConfig)
    advisor: AdvisorConfig = field(default_factory=AdvisorConfig)
    session: SessionConfig = field(default_factory=SessionConfig)
    data_tools: DataToolsConfig = field(default_factory=DataToolsConfig)
    stage_max_turns: dict[str, int] = field(default_factory=dict)
    write_paths: list[str] = field(default_factory=list)
    allowed_commands: list[str] = field(default_factory=list)
    gates_dir: Path = field(default_factory=Path)
    worker_id: str = ""  # one of the concurrent pack workers
    codex_home: Path | None = None
    shared_paths: list[str] = field(default_factory=lambda: list(DEFAULT_SHARED_PATH_GLOBS))
    # The run's identity (set by apply_workload_overrides); write_ownership applies the per-layer
    # write rules by it: backends/<engine>/** for engine_backend runs, accelerators/<bench>/q<N>/** for query runs.
    workload_kind: str = ""
    engine_name: str = ""
    benchmark_name: str = ""
    # The OS sandbox of every C++-mode agent step (accel_pipeline.sandbox); `none` runs codex unsandboxed.
    sandbox: str = "bwrap"
    sandbox_extra_ro: list[str] = field(default_factory=list)  # more host paths for the sandbox, absolute
    sandbox_extra_rw: list[str] = field(default_factory=list)
    # The post-stage ownership check (write_ownership.check_stage): enforce fails the stage, warn logs it to the run's events.
    write_policy: str = "warn"
    measurement: MeasurementConfig = field(default_factory=MeasurementConfig)
    quick_tier: QuickTierConfig = field(default_factory=QuickTierConfig)
    concurrency: ConcurrencyConfig = field(default_factory=ConcurrencyConfig)
    test_suite: SuiteConfig = field(default_factory=SuiteConfig)

    def __post_init__(self) -> None:
        for name, allowed in (("sandbox", SANDBOXES), ("write_policy", WRITE_POLICIES)):
            if getattr(self, name) not in allowed:
                raise ValueError(f"{name}: {getattr(self, name)!r} (expected one of {', '.join(allowed)})")


# ---- loading default.yaml and models.yaml -----------------------------------------------------

def _load_yaml(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def _config_yaml(repo_root: Path, name: str) -> dict[str, Any]:
    """pipeline/config/<name>.yaml; empty when the file does not exist."""
    path = pipeline_root(repo_root) / "config" / f"{name}.yaml"
    return _load_yaml(path) if path.is_file() else {}


def _coerce(f: dataclasses.Field, value: Any, where: str) -> Any:
    """`value` as the type of the field's default; a nested settings block becomes its dataclass."""
    default = f.default_factory() if f.default is dataclasses.MISSING else f.default
    if dataclasses.is_dataclass(default):
        return _section(type(default), value, f"{where}.{f.name}")
    if isinstance(default, (bool, int, float, str, tuple, dict, list)):
        return type(default)(value)
    return value


def _section(cls: type, raw: dict[str, Any] | None, where: str) -> Any:
    """A settings dataclass from a yaml block; a key that names no field is an error, a null keeps the default."""
    fields = {f.name: f for f in dataclasses.fields(cls)}
    raw = raw or {}
    unknown = sorted(set(raw) - set(fields))
    if unknown:
        raise ValueError(f"{where}: unknown settings {unknown}")
    return cls(**{k: _coerce(fields[k], v, where) for k, v in raw.items() if v is not None})


def _optimization_settings(block: dict[str, Any], where: str) -> dict[str, Any]:
    """PipelineConfig fields set by an `optimization:` block (`max_turns:` maps phases to optimization_max_turns_<phase>)."""
    flat = {f"optimization_{k}": v for k, v in block.items() if k != "max_turns"}
    flat.update({f"optimization_max_turns_{phase}": n for phase, n in (block.get("max_turns") or {}).items()})
    fields = {f.name: f for f in dataclasses.fields(PipelineConfig) if f.name.startswith("optimization_")}
    unknown = sorted(set(flat) - set(fields))
    if unknown:
        raise ValueError(f"{where}: unknown optimization settings {unknown}")
    return {k: _coerce(fields[k], v, where) for k, v in flat.items()}


_YAML_SETTINGS = (
    "sandbox", "sandbox_extra_ro", "sandbox_extra_rw", "write_policy", "shared_bench_harness", "stage_max_turns",
    "write_paths", "allowed_commands", "repair", "advisor", "session", "data_tools", "measurement", "quick_tier", "concurrency",
)


def _under(root: Path, path: str | Path) -> Path:
    path = Path(path)
    return path if path.is_absolute() else root / path


def load_pipeline_config(config_path: Path | None = None, repo_root: Path | None = None) -> PipelineConfig:
    root = repo_root or find_repo_root()
    pipe = pipeline_root(root)
    data = _load_yaml(config_path or pipe / "config" / "default.yaml")
    fields = {f.name: f for f in dataclasses.fields(PipelineConfig)}
    # A dataset is available to the run when the environment variable that default.yaml names for it is set.
    datasets = {key: Path(os.environ[var]) for key, var in (data.get("datasets") or {}).items() if os.environ.get(var)}

    cfg = PipelineConfig(
        repo_root=root,
        pipeline_root=pipe,
        build_dir=root / data.get("build_dir", DEFAULT_BUILD_DIR),
        gates_dir=root / data.get("gates_dir", DEFAULT_GATES_DIR),
        tpch_db_sf1=datasets.get("tpch_sf1", Path()),
        tpch_db_sf10=datasets.get("tpch_sf10", Path()),
        datasets=datasets,
        worker_id=os.environ.get("ACCEL_PIPELINE_WORKER_ID") or "",
        test_suite=SuiteConfig.from_dict(data.get("test_suite")),
        **_optimization_settings(data.get("optimization") or {}, "optimization"),
        **{k: _coerce(fields[k], data[k], k) for k in _YAML_SETTINGS if data.get(k) is not None},
    )

    if os.environ.get("ACCEL_PIPELINE_BUILD_DIR"):
        from accel_pipeline.pack.worker_isolation import _rewrite_build_dir_patterns

        build_dir = _under(root, os.environ["ACCEL_PIPELINE_BUILD_DIR"])
        cfg = replace(cfg, build_dir=build_dir,
                      allowed_commands=_rewrite_build_dir_patterns(cfg.allowed_commands, build_dir.name))
    if os.environ.get("ACCEL_PIPELINE_CODEX_HOME"):
        cfg = replace(cfg, codex_home=_under(root, os.environ["ACCEL_PIPELINE_CODEX_HOME"]))
    return cfg


def load_model_config(path: Path | None = None, repo_root: Path | None = None) -> ModelConfig:
    root = repo_root or find_repo_root()
    models_yaml = _resolve_config_path(root, path or pipeline_root(root) / "config" / "models.yaml")
    return _section(ModelConfig, _load_yaml(models_yaml), "models config")


def _resolve_config_path(repo_root: Path, path: Path) -> Path:
    if path.is_absolute():
        return path
    pipe = pipeline_root(repo_root)
    for candidate in (path, repo_root / path, pipe / path, pipe / "config" / path.name):
        if candidate.is_file():
            return candidate
    return repo_root / path


def load_aux_state_defaults(repo_root: Path | None = None) -> dict[str, Any]:
    return _config_yaml(repo_root or find_repo_root(), "aux_state")


def load_engine_backend_defaults(repo_root: Path | None = None) -> dict[str, Any]:
    return _config_yaml(repo_root or find_repo_root(), "engine_backend")


def load_benchmark_pack(name: str, repo_root: Path | None = None) -> dict[str, Any]:
    path = pipeline_root(repo_root or find_repo_root()) / "config" / "benchmark_packs" / f"{name}.yaml"
    if not path.is_file():
        raise FileNotFoundError(f"benchmark pack not found: {path}")
    return _load_yaml(path)


# ---- paths and names of a workload's query ----------------------------------------------------

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
        # Where the query's cost models live (AuxCostModelLocation), for every engine.
        "cost_model_header_path": f"{qdir}/cost_model.hpp",
        "test_path": f"{qdir}/tests/{test_target}.cpp",
        "benchmark_path": f"{qdir}/bench/benchmark_q{q}.cpp",
        "engine_name": engine,
    }

    if kind == "aux_state":
        aux_state_subdir = f"{qdir}/{eng_seg}aux"
        sec_edgar = benchmark == "sec_edgar"
        cost_model_stem = f"sec_edgar_q{q}_aux_cost_model" if sec_edgar else f"q{q}_aux_cost_model"
        bench_sweep = workload.get("aux_bench_sweep", {})
        if not isinstance(bench_sweep, dict):
            bench_sweep = {}
        paths.update(
            {
                "aux_function_name": naming.aux_function_name(benchmark, q),
                "aux_test_target": f"{test_target}_aux",
                # host_bootstrap makes every engine's harnesses for every benchmark (benchmark-free sources)
                "aux_bench_target": harness_targets(engine, benchmark)["bench_target"],
                "aux_hybrid_bench_target": naming.hybrid_benchmark_target(benchmark, q),
                "aux_calibrate_target": harness_targets(engine, benchmark)["calibrate_target"],
                "aux_ctest_smoke_regex": f"^{naming.aux_smoke_ctest_name(benchmark, q)}$",
                "aux_state_subdir": aux_state_subdir,
                # The query-wide default model (cost_model.hpp's location); an engine with its
                # own calibration keeps it beside the default as <stem>_<engine>.json.
                "cost_model_default": f"{qdir}/config/{cost_model_stem}.json",
                "cost_model_config": f"{qdir}/config/{cost_model_stem}_{engine}.json",
                "aux_bench_work_dir_suffix": str(
                    workload.get("aux_bench_work_dir_suffix", f"q{q}_aux_bench_sf3" if sec_edgar else f"q{q}_aux_bench_sf1")
                ),
                "aux_bench_suites": str(bench_sweep.get("suites", workload.get("aux_bench_suites", "lineitem_only"))),
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


def sanitize_engine_name(name: str) -> str:
    """Normalize engine_name to a filesystem / C++ identifier fragment."""
    cleaned = "".join(ch if ch.isalnum() or ch == "_" else "_" for ch in str(name).strip().lower())
    cleaned = "_".join(part for part in cleaned.split("_") if part)
    if not cleaned or not cleaned[0].isalpha():
        raise ValueError(f"invalid engine_name: {name!r} (need a letter-leading identifier)")
    return cleaned


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


# ---- loading a workload -----------------------------------------------------------------------

_TPCH_WORKLOAD = re.compile(r"tpch_q(\d+)(_aux)?")


def _read_workload(root: Path, name: str) -> dict[str, Any] | None:
    """workloads/<name>.yaml, or a TPC-H query workload built from its template and tpch_queries.yaml; None when neither exists."""
    workloads = pipeline_root(root) / "config" / "workloads"
    if (workloads / f"{name}.yaml").is_file():
        return _load_yaml(workloads / f"{name}.yaml")
    tpch = _TPCH_WORKLOAD.fullmatch(name)
    queries = _load_yaml(workloads / "tpch_queries.yaml") if (workloads / "tpch_queries.yaml").is_file() else {}
    if not tpch or int(tpch[1]) not in queries:
        return None
    q, aux = int(tpch[1]), bool(tpch[2])
    entry = queries[q]
    return {
        **_load_yaml(workloads / ("tpch_q_aux.template.yaml" if aux else "tpch_q.template.yaml")),
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
    bench = _config_yaml(repo_root, f"{workload.get('benchmark_name', 'tpch')}_aux_state")

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
            aux_function_name=naming.aux_function_name(bench, q),
            accelerator_id=naming.accelerator_id(bench, q),
        )


def _pin_engine(data: dict[str, Any], engine: str, repo_root: Path) -> dict[str, Any]:
    """Pin the run's engine, attach its host conventions and let the run own the query's directory."""
    data["engine_name"] = engine
    bench = str(data.get("benchmark_name") or data.get("benchmark") or "tpch")
    qnum = data.get("query_number")
    data["engine_host"] = engine_host_conventions(engine, bench, qnum if qnum is not None else "<N>", repo_root)
    # backends/<engine>/** belongs to the engine_backend run and substrate/** to nobody (write_ownership enforces both).
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


# ---- a workload's refinement of the run's settings --------------------------------------------

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
        **_optimization_settings(workload.get("optimization") or {}, "workload optimization"),
        stage_max_turns={**pipeline_config.stage_max_turns, **(workload.get("stage_max_turns") or {})},
        write_paths=[*pipeline_config.write_paths, *(workload.get("write_paths") or [])],
        allowed_commands=[*pipeline_config.allowed_commands, *allowed],
        workload_kind=kind,
        engine_name=engine,
        benchmark_name=str(workload.get("benchmark_name") or ("" if kind == "engine_backend" else "tpch")),
    )
