"""The run's settings: config/default.yaml and models.yaml, the repo roots, and the environment.

Every tunable is a field below with its default; default.yaml and models.yaml only set what differs.
A workload refines the settings (workload.apply_workload_overrides); which model a stage runs is
resolved here from the model config.
"""
from __future__ import annotations

import dataclasses
import os
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any

import yaml

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
    # The ownership check of an agent stage (write_ownership.StageWriteCheck): enforce refuses gates while the session
    # has out-of-bounds edits, then reverts them and fails the stage; warn only logs them to the run's events.
    write_policy: str = "enforce"
    measurement: MeasurementConfig = field(default_factory=MeasurementConfig)
    quick_tier: QuickTierConfig = field(default_factory=QuickTierConfig)
    concurrency: ConcurrencyConfig = field(default_factory=ConcurrencyConfig)
    test_suite: SuiteConfig = field(default_factory=SuiteConfig)

    def __post_init__(self) -> None:
        for name, allowed in (("sandbox", SANDBOXES), ("write_policy", WRITE_POLICIES)):
            if getattr(self, name) not in allowed:
                raise ValueError(f"{name}: {getattr(self, name)!r} (expected one of {', '.join(allowed)})")


# ---- repo roots and the environment -----------------------------------------------------------

def find_repo_root(start: Path | None = None) -> Path:
    cur = (start or Path.cwd()).resolve()
    for parent in [cur, *cur.parents]:
        if (parent / "CMakeLists.txt").is_file() and (parent / "pipeline").is_dir():
            return parent
    raise FileNotFoundError("could not locate accelerator-codegen repo root")


def pipeline_root(repo_root: Path | None = None) -> Path:
    return (repo_root or find_repo_root()) / "pipeline"


def _strip_quotes(value: str) -> str:
    value = value.strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in {"'", '"'}:
        return value[1:-1]
    return value


def load_repo_dotenv(repo_root: Path) -> Path | None:
    """Load `.env` then `.env.local` from repo_root. Returns path loaded, if any."""
    loaded: Path | None = None
    for name in (".env", ".env.local"):
        path = repo_root / name
        if not path.is_file():
            continue
        for raw_line in path.read_text(encoding="utf-8").splitlines():
            line = raw_line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("export "):
                line = line[len("export ") :].strip()
            if "=" not in line:
                continue
            key, _, value = line.partition("=")
            key = key.strip()
            if not key:
                continue
            if not os.environ.get(key):  # set and non-empty in the environment wins
                os.environ[key] = _strip_quotes(value)
        loaded = path
    return loaded


# ---- loading default.yaml and models.yaml -----------------------------------------------------

def load_yaml(path: Path) -> dict[str, Any]:
    with path.open(encoding="utf-8") as f:
        return yaml.safe_load(f) or {}


def config_yaml(repo_root: Path, name: str) -> dict[str, Any]:
    """pipeline/config/<name>.yaml; empty when the file does not exist."""
    path = pipeline_root(repo_root) / "config" / f"{name}.yaml"
    return load_yaml(path) if path.is_file() else {}


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


def optimization_settings(block: dict[str, Any], where: str) -> dict[str, Any]:
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
    data = load_yaml(config_path or pipe / "config" / "default.yaml")
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
        **optimization_settings(data.get("optimization") or {}, "optimization"),
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
    return _section(ModelConfig, load_yaml(models_yaml), "models config")


def _resolve_config_path(repo_root: Path, path: Path) -> Path:
    if path.is_absolute():
        return path
    pipe = pipeline_root(repo_root)
    for candidate in (path, repo_root / path, pipe / path, pipe / "config" / path.name):
        if candidate.is_file():
            return candidate
    return repo_root / path


# ---- which model a stage runs ----------------------------------------------------------------

_OPTIMIZATION_STAGE_FALLBACKS = (
    "optimization",
    "optimization_unified",
    "optimization_unified_hypothesis",
    "optimization_unified_patch",
    "optimization_fix",
    "optimization_worker",
)

_STAGE_PROVIDER_PARENTS = {
    "optimization": "optimization_unified",
    "optimization_unified_hypothesis": "optimization_unified",
    "optimization_unified_patch": "optimization_unified",
    "optimization_fix": "optimization_worker",
    "optimization_worker": "optimization_unified",
    "preflight_live": "analysis",  # the live model probe runs the way an analysis turn does
}


def is_implementation_stage(stage: str) -> bool:
    return stage == "implementation" or stage.startswith("impl_")


def is_critique_stage(stage: str) -> bool:
    return stage == "layer_critique" or stage.startswith("critique_")


def resolve_stage_provider(stage: str, model_config: ModelConfig) -> str:
    """Per-stage provider: the stage's own entry, else its family or parent stage's; unknown stages raise."""
    providers = model_config.stage_providers
    if stage in providers:
        return providers[stage]
    if is_critique_stage(stage) and "layer_critique" in providers:
        return providers["layer_critique"]
    if is_implementation_stage(stage) and "implementation" in providers:
        return providers["implementation"]
    parent = _STAGE_PROVIDER_PARENTS.get(stage)
    if parent and parent in providers:
        return providers[parent]
    if stage in _OPTIMIZATION_STAGE_FALLBACKS and "optimization_unified" in providers:
        return providers["optimization_unified"]
    raise KeyError(f"no provider configured for stage {stage!r} (stage_providers)")


def resolve_preset(stage: str, model_config: ModelConfig, overrides: dict[str, str] | None = None) -> str:
    overrides = overrides or {}
    preset_name = overrides.get(stage)
    if not preset_name and is_implementation_stage(stage):
        preset_name = overrides.get("implementation")
    if not preset_name and is_critique_stage(stage):
        preset_name = overrides.get("layer_critique")
    if not preset_name:
        preset_name = model_config.stages.get(stage)
    if not preset_name and is_critique_stage(stage):
        preset_name = model_config.stages.get("layer_critique")
        if not preset_name and "cheap_code" in model_config.presets:
            preset_name = "cheap_code"
    if not preset_name and is_implementation_stage(stage):
        preset_name = model_config.stages.get("implementation")
    if not preset_name and stage in _OPTIMIZATION_STAGE_FALLBACKS:
        preset_name = (
            model_config.stages.get("optimization_unified")
            or model_config.stages.get("optimization_worker")
            or "default"
        )
    preset_name = preset_name or "default"
    if preset_name not in model_config.presets:
        raise KeyError(f"unknown model preset: {preset_name}")
    return model_config.presets[preset_name]


def _require_api_key(model_config: ModelConfig) -> None:
    if model_config.api_key_env not in os.environ and os.environ.get("ACCEL_PIPELINE_DRY_RUN") != "1":
        raise EnvironmentError(f"set {model_config.api_key_env} or use --dry-run")


def make_model_id(
    stage: str,
    pipeline_config: PipelineConfig,  # noqa: ARG001 — reserved for future provider options
    model_config: ModelConfig,
    overrides: dict[str, str] | None = None,
) -> str:
    """Return the model id string for logging and dry-run display."""
    provider = resolve_stage_provider(stage, model_config)
    if provider != "codex_exec":
        raise ValueError(f"unsupported provider: {provider!r}")
    _require_api_key(model_config)
    return resolve_preset(stage, model_config, overrides)
