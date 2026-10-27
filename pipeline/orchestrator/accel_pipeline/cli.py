"""The entry and the run: `accel` commands, running a query's stages in order, and the preflight before it.

`accel run` is one query end to end in this tree (`run_main`: stages in order, pause and resume, the repair loop's
clean stop, the structural re-analysis between stages); `accel check` is the preflight, every check a function
returning its problems (none: ok), which `run_main` runs first and refuses to start a run with a problem, one
message per failed item. Each check guards something that broke a live run (pipeline ISSUES B1, B3, B4, B6, B7) or
would only show once an agent hit it. Checks run in the sandboxes the run uses (this process is inside
`clean_tree/sandbox.sh`; the agents' bwrap is wrapped around the probes that the agents' tools depend on).
`accel check --models` also runs one tiny `codex exec` turn per configured model preset that calls an MCP tool: it
validates tool-call compatibility of a new model (costs cents; off by default).

`python -m accel_pipeline` is the `accel` front end (`main`); `python -m accel_pipeline.cli` takes the flags of
`run_main` directly, which is how the pack's workers start a query.
"""
from __future__ import annotations

import argparse
import asyncio
import atexit
import contextlib
import os
import re
import shutil
import subprocess
import sys
import time
import tomllib
import traceback
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from accel_pipeline.config import (
    apply_workload_overrides,
    find_repo_root,
    load_model_config,
    load_pipeline_config,
    load_repo_dotenv,
    load_workload,
    ModelConfig,
    PipelineConfig,
    query_artifacts_dir,
    workload_facts,
    make_model_id,
    workload_supported_engines,
)
from accel_pipeline.repair import (
    EXIT_STOPPED,
    Failure,
    REPAIR_STATE_FILE,
    RepairCtx,
    StageStopped,
    stop_stage,
)
from accel_pipeline.runlog import (
    advance_stage,
    load_or_create,
    log_event,
    log_pipeline_summary,
    PipelineState,
    STAGES,
    state_path,
)
from accel_pipeline.stages import process_after_stage, run_analysis, run_stage
from accel_pipeline.verus import STAGES as VERUS_STAGES


def _parse_model_overrides(values: list[str] | None) -> dict[str, str]:
    overrides: dict[str, str] = {}
    for item in values or []:
        if "=" not in item:
            raise ValueError(f"expected stage=preset, got {item!r}")
        stage, preset = item.split("=", 1)
        overrides[stage.strip()] = preset.strip()
    return overrides


def _maybe_process_structural_reanalysis(
    *,
    triggered_after: str,
    pipeline_config,
    model_config,
    workload_name: str,
    workload: dict,
    query: int,
    model_overrides: dict[str, str],
    dry_run: bool,
    state,
) -> int | None:
    """Run bounded re-analysis if requested. Returns next STAGES index, or None."""
    if workload.get("workload_kind") != "aux_state":
        return None

    def reanalyse(**kwargs):
        run_analysis(
            pipeline_config,
            model_config,
            workload_name,
            query,
            model_overrides,
            dry_run,
            user_message=kwargs.get("user_message"),
            analysis_revision_request=kwargs.get("analysis_revision_request", ""),
        )

    def run_suite():
        run_stage("test_suite", pipeline_config, model_config, workload_name, query, model_overrides, dry_run)

    def run_impl():
        run_stage("implementation", pipeline_config, model_config, workload_name, query, model_overrides, dry_run)

    def run_opt():
        run_stage("optimization", pipeline_config, model_config, workload_name, query, model_overrides, dry_run)

    result = process_after_stage(
        repo_root=pipeline_config.repo_root,
        query=query,
        workload=workload,
        triggered_after=triggered_after,
        structural_reanalysis_count=state.structural_reanalysis_count,
        run_analysis=reanalyse,
        run_test_suite=run_suite,
        run_implementation=run_impl,
        run_optimization=run_opt,
        dry_run=dry_run,
    )
    if not result:
        return None
    new_count, next_idx = result
    state.structural_reanalysis_count = new_count
    state.stage = STAGES[next_idx]
    state.save(state_path(pipeline_config.repo_root, query, workload_name, pipeline_config.engine_name))
    log_event(
        pipeline_config.repo_root,
        query,
        workload,
        "structural_reanalysis_resume",
        rerun_number=new_count,
        resume_stage=STAGES[next_idx],
        triggered_after=triggered_after,
    )
    return next_idx


def _mark_stopped(pipeline_config, workload_name: str, query: int, stage: str, reason: str) -> None:
    path = state_path(pipeline_config.repo_root, query, workload_name, pipeline_config.engine_name)
    if not path.is_file():
        return
    state = PipelineState.load(path)
    state.stage = stage if stage in STAGES else state.stage
    state.status = f"stopped:{reason}"
    state.save(path)


def _run_stage(
    stage: str,
    pipeline_config,
    model_config,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str],
    dry_run: bool,
    *,
    skip_gates: bool = False,
    resume: bool = False,
) -> None:
    """One stage, with a last-resort catch: an exception nothing below handled is retried
    (max_orchestrator_retries), then recorded as a clean stop."""
    retries = pipeline_config.repair.max_orchestrator_retries
    for attempt in range(retries + 1):
        try:
            return run_stage(stage, pipeline_config, model_config, workload_name, query, model_overrides,
                             dry_run, skip_gates=skip_gates, resume=resume or attempt > 0)
        except StageStopped as stopped:
            _mark_stopped(pipeline_config, workload_name, query, stage, stopped.reason)
            raise
        except Exception as exc:  # noqa: BLE001
            if dry_run:
                raise
            print(f"[pipeline] stage {stage} raised {type(exc).__name__}: {exc}", file=sys.stderr, flush=True)
            if attempt < retries:
                print(f"[pipeline] retrying stage {stage} ({attempt + 1}/{retries})", file=sys.stderr, flush=True)
                continue
            ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage=stage)
            failure = Failure(kind="orchestrator", stage=stage, source="exception",
                              summary=f"{type(exc).__name__}: {exc}", evidence=traceback.format_exc())
            stopped = stop_stage(ctx, failure, f"orchestrator exception in {stage}")
            _mark_stopped(pipeline_config, workload_name, query, stage, stopped.reason)
            raise stopped from exc


def _sandbox_preflight(pipeline_config, query: int) -> None:
    """Stop at run start when the agents' bwrap sandbox is unavailable or does not hold."""
    if not shutil.which("bwrap"):
        raise SystemExit("sandbox: bwrap is selected but bubblewrap is not installed "
                         "(install it, or set sandbox: none in pipeline/config/default.yaml)")
    check = agent.cpp_selfcheck(pipeline_config, query)
    print(f"sandbox (bwrap) selfcheck: {'ok' if check['ok'] else check['problems']}", file=sys.stderr)
    if not check["ok"]:
        raise SystemExit(f"agent sandbox selfcheck failed: {check['problems']}")


def run_main(argv: list[str] | None = None) -> int:
    try:
        return _run_main(argv)
    except StageStopped as stopped:
        print(f"pipeline stopped: {stopped}; fix the cause and continue with --resume", file=sys.stderr)
        return EXIT_STOPPED
    except Exception:  # noqa: BLE001 - a run ends with an exit code and a record, never a traceback
        traceback.print_exc()
        print("pipeline stopped on an orchestrator error; --resume continues", file=sys.stderr)
        return EXIT_STOPPED


def _run_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Accelerator codegen agent pipeline")
    parser.add_argument("--query", type=int, required=True)
    parser.add_argument("--workload", type=str, default="tpch_q3")
    parser.add_argument(
        "--engine",
        default="",
        help="Engine to build the accelerator for; required when the "
        "workload supports more than one. Each query x engine is an independent run.",
    )
    parser.add_argument(
        "--stage",
        choices=[
            *STAGES,
            *VERUS_STAGES,
            "host_bootstrap",
            "validation",
            "architecture_audit",
            "impl_hostctl",
            "impl_capture",
            "impl_epochs",
            "impl_scan",
            "impl_runtime",
            "impl_host",
            "impl_api",
            "all",
        ],
        default="all",
    )
    parser.add_argument(
        "--mode",
        choices=["cpp", "verus"],
        default=None,
        help="cpp: C++ accelerator pipeline (default); verus: the Verus accelerator pipeline "
        "(accel_pipeline/verus; same front end, its own stages). Default: the workload's `mode`.",
    )
    parser.add_argument("--until", choices=STAGES[:-1], default=None,
                        help="with --stage all: pause after this stage completes (--resume continues with the next)")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="Render prompts; skip LLM calls")
    parser.add_argument(
        "--skip-gates",
        action="store_true",
        help="Skip post-stage gates (agent only; no calibrate/sweep/ctest after the stage).",
    )
    parser.add_argument("--config", type=Path, default=None)
    parser.add_argument(
        "--models-config",
        type=Path,
        default=None,
        help="Path to models.yaml (default: pipeline/config/models.yaml)",
    )
    parser.add_argument("--model", action="append", dest="models", default=[])
    args = parser.parse_args(argv)
    if args.until and args.stage != "all":
        parser.error("--until applies to --stage all")

    if args.dry_run:
        os.environ["ACCEL_PIPELINE_DRY_RUN"] = "1"

    # Pack worktree isolation: prefer explicit ACCEL_PIPELINE_REPO_ROOT.
    env_repo = os.environ.get("ACCEL_PIPELINE_REPO_ROOT")
    primary_env = os.environ.get("ACCEL_PIPELINE_PRIMARY_REPO")
    repo_root = Path(env_repo).resolve() if env_repo else find_repo_root()
    primary_root = Path(primary_env).resolve() if primary_env else find_repo_root()
    dotenv_path = load_repo_dotenv(primary_root)
    if dotenv_path is not None:
        try:
            print(f"loaded env file: {dotenv_path.relative_to(primary_root)}", file=sys.stderr)
        except ValueError:
            print(f"loaded env file: {dotenv_path}", file=sys.stderr)
    pipeline_config = load_pipeline_config(args.config, repo_root)
    workload = load_workload(args.workload, primary_root, engine=args.engine)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    # Concurrent pack workers: separate build dir + CODEX_HOME (+ optional worktree).
    if os.environ.get("ACCEL_PIPELINE_WORKER_ID") or os.environ.get("ACCEL_PIPELINE_BUILD_DIR"):
        from accel_pipeline.pack import apply_worker_isolation, isolation_summary

        pipeline_config = apply_worker_isolation(
            pipeline_config,
            query=args.query,
            workload_name=args.workload,
            set_environ=True,
        )
        print(f"worker isolation: {isolation_summary(pipeline_config)}", file=sys.stderr)
    repo_root = Path(pipeline_config.repo_root)
    if not args.dry_run:
        from accel_pipeline.pack import activate, format_cpus

        budget = activate(pipeline_config.concurrency)
        print(f"core budget: cpus={format_cpus(budget.cpus)} measure={format_cpus(budget.measure) or '-'} slot={budget.slot}", file=sys.stderr)
    if pipeline_config.engine_name:
        # The host-contract environment the gates use, so the agents' own host.sh calls
        # (forwarded to their tools as ACCEL_*) configure and reach the same build and instance.
        os.environ["ACCEL_INSTANCE_TAG"] = instance_tag(pipeline_config.worker_id, Path(pipeline_config.build_dir))
        for key, value in instance_env(repo_root, pipeline_config.engine_name, Path(pipeline_config.build_dir)).items():
            os.environ.setdefault(key, value)
    model_config = load_model_config(args.models_config, repo_root=primary_root)
    print(f"agent provider: {', '.join(sorted(set(model_config.stage_providers.values())))}", file=sys.stderr)
    query = args.query
    if workload.get("query_number") is not None and int(workload["query_number"]) != query:
        if workload.get("workload_kind") != "engine_backend":
            print("warning: --query does not match workload query_number", file=sys.stderr)

    model_overrides = _parse_model_overrides(args.models)

    mode = args.mode or str(workload.get("mode") or "cpp")
    if not args.dry_run:
        clear_stale_run(query_artifacts_dir(repo_root, query, workload=workload))
    if mode == "cpp" and not args.dry_run and pipeline_config.sandbox == "bwrap":
        _sandbox_preflight(pipeline_config, query)
    if mode == "cpp" and not args.dry_run and workload.get("workload_kind") == "aux_state":
        require(Probe(pipeline_config, model_config, workload, args.workload, query))

    if workload.get("workload_kind") == "engine_backend":
        from accel_pipeline import engine_backend as engine_backend_stage

        print(f"=== engine_backend workload: {args.workload} ===", file=sys.stderr)
        engine_backend_stage.run_engine_pipeline(
            pipeline_config,
            model_config,
            args.workload,
            query,
            model_overrides,
            args.dry_run,
            stage=args.stage if args.stage != "all" else "all",
            skip_gates=args.skip_gates,
        )
        print("pipeline complete", file=sys.stderr)
        return 0

    facts = workload_facts(repo_root, workload, query)
    print(
        f"derived workload facts: {len(facts['output_schema'])} output columns, "
        f"tables {[t['name'] for t in facts['dependent_tables']]}",
        file=sys.stderr,
    )

    # Worker worktrees are private, so the query's other engines can be hidden from the
    # agent (a shared tree would hide them from the sibling runs too).
    if repo_root != primary_root and not args.dry_run:
        from accel_pipeline.pack import apply_foreign_engine_quarantine, restore_quarantine

        atexit.register(restore_quarantine, apply_foreign_engine_quarantine(repo_root, workload))

    # Verus mode: same front end, then the Verus pipeline's own stages.  It
    # runs on DuckDB (in-process; no host bootstrap) and skips the C++
    # base-accelerator prerequisite: it must not see C++ accelerators.
    if mode == "verus":
        from accel_pipeline.verus.run import run_verus_pipeline

        print(f"=== verus mode: {args.workload} ===", file=sys.stderr)
        return run_verus_pipeline(
            pipeline_config,
            model_config,
            workload,
            args.workload,
            query,
            stage=args.stage,
            resume=args.resume,
            dry_run=args.dry_run,
            model_overrides=model_overrides,
        )
    if args.stage in VERUS_STAGES and args.stage not in STAGES:
        print(f"stage {args.stage!r} belongs to --mode verus", file=sys.stderr)
        return 2

    # One-shot query-host bootstrap (engines without a query-scoped harness) before per-query stages.
    if args.stage in {"all", "host_bootstrap"}:
        from accel_pipeline.engine_backend import ensure_host_bootstrap_for_workload, reopen_engine_backend

        if pipeline_config.engine_name and repo_root == primary_root:
            # Backend defects earlier query runs recorded go back to the engine's backend run first
            # (a pack does this before its workers).
            reopen_engine_backend(
                pipeline_config, model_config, pipeline_config.engine_name,
                model_overrides=model_overrides, dry_run=args.dry_run,
            )

        hb = ensure_host_bootstrap_for_workload(
            pipeline_config,
            model_config,
            args.workload,
            model_overrides=model_overrides,
            dry_run=args.dry_run,
            skip_gates=args.skip_gates,
        )
        if hb is not None:
            print(f"=== host_bootstrap: {hb} ===", file=sys.stderr)
        if args.stage == "host_bootstrap":
            print("pipeline complete", file=sys.stderr)
            return 0

    state = load_or_create(repo_root, query, args.workload, pipeline_config.engine_name, resume=args.resume)
    if args.resume and state.status:
        print(f"resuming a run that {state.status}", file=sys.stderr)
        state.status = ""
        state.save(state_path(repo_root, query, args.workload, pipeline_config.engine_name))
        (query_artifacts_dir(repo_root, query, workload=workload) / REPAIR_STATE_FILE).unlink(missing_ok=True)

    if workload.get("workload_kind") == "aux_state":
        if not args.dry_run:
            from accel_pipeline.gates import prepare_inner_loop

            prepare_inner_loop(pipeline_config, workload, query)

    if args.stage != "all":
        print(f"=== stage: {args.stage} ===", file=sys.stderr)
        _run_stage(
            args.stage,
            pipeline_config,
            model_config,
            args.workload,
            query,
            model_overrides,
            args.dry_run,
            skip_gates=args.skip_gates,
            resume=args.resume,
        )
        state.stage = args.stage
        state.save(state_path(repo_root, query, args.workload, pipeline_config.engine_name))
        print("pipeline complete", file=sys.stderr)
        return 0

    start_idx = STAGES.index(state.stage) if args.resume else 0
    initial_stage = state.stage
    idx = start_idx
    while idx < len(STAGES):
        stage = STAGES[idx]
        print(f"=== stage: {stage} ===", file=sys.stderr)
        if not args.dry_run:
            state.stage = stage
            state.save(state_path(repo_root, query, args.workload, pipeline_config.engine_name))
        _run_stage(stage, pipeline_config, model_config, args.workload, query, model_overrides, args.dry_run, skip_gates=args.skip_gates, resume=args.resume)
        if not args.dry_run:  # a repair re-plan inside the stage counts against the same budget
            on_disk = PipelineState.load(state_path(repo_root, query, args.workload, pipeline_config.engine_name))
            state.structural_reanalysis_count = max(state.structural_reanalysis_count, on_disk.structural_reanalysis_count)

        if stage == "implementation":
            next_idx = _maybe_process_structural_reanalysis(
                triggered_after="implementation",
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name=args.workload,
                workload=workload,
                query=query,
                model_overrides=model_overrides,
                dry_run=args.dry_run,
                state=state,
            )
            if next_idx is not None:
                idx = next_idx
                continue

        if stage == "optimization":
            next_idx = _maybe_process_structural_reanalysis(
                triggered_after="optimization",
                pipeline_config=pipeline_config,
                model_config=model_config,
                workload_name=args.workload,
                workload=workload,
                query=query,
                model_overrides=model_overrides,
                dry_run=args.dry_run,
                state=state,
            )
            if next_idx is not None:
                idx = next_idx
                continue

        if stage != STAGES[-1] and not args.dry_run:
            advance_stage(state, repo_root)
        if stage == args.until:
            if not args.dry_run:
                state.status = f"paused:until {stage}"
                state.save(state_path(repo_root, query, args.workload, pipeline_config.engine_name))
            print(f"pipeline paused after {stage}; --resume continues", file=sys.stderr)
            return 0
        idx += 1

    if args.dry_run:
        # Dry-run must not poison --resume (e.g. leave stage=audit with no sources).
        state.stage = initial_stage
        state.save(state_path(repo_root, query, args.workload, pipeline_config.engine_name))
        print("pipeline dry-run complete (state unchanged)", file=sys.stderr)
        return 0

    state.stage = STAGES[-1]
    state.save(state_path(repo_root, query, args.workload, pipeline_config.engine_name))
    log_pipeline_summary(repo_root, query, workload)
    print("pipeline complete", file=sys.stderr)
    return 0


PERF_EVENTS = "cycles,instructions"
PARALLEL_DATA_CALLS = 8
LIVE_TOOL = "discover_host_sdk"  # read-only, no arguments
# the tools each stage's prompt tells the agent to call
STAGE_TOOLS = {
    "analysis": ["query_data", "column_stats", "validate_analysis", "update_analysis"],
    "implementation": ["quick_check", "request_gate", "gate_status", "report_blocker", "ask_advisor",
                       "primitive_publish", "discover_host_sdk"],
    "optimization": ["run_profile", "get_profile_digest", "run_quick_benchmark", "begin_ab_trial", "end_ab_trial"],
}
# Variables that may be pasted into Python source in a gate script: names, numbers and paths, never log text.
SAFE_INTERPOLATIONS = frozenset("""
    ART AUX_RC BASE_RC BENCHMARK_NAME CONFIG DRAWS_JSON ENGINE_NAME EXCLUDE FREQ_COUNTS MIN_REDUCTION_PCT MIN_SPEEDUP
    OUT PATTERN PROBE_REGEX QUERY RC RC_RUN RESULT_CSVS ROOT STATUS SUITE_LIST SWEEP_LOG SWEEP_RC THREADS1_CSV THREADSN_CSV THREADS_N
""".split())


@dataclass(frozen=True)
class Probe:
    """What every check sees: the run as configured."""

    cfg: PipelineConfig
    models: ModelConfig
    workload: dict
    workload_name: str
    query: int

    @property
    def sandbox_prefix(self) -> list[str]:
        """The command prefix of the agents' sandbox (empty when the run has none)."""
        if self.cfg.sandbox != "bwrap":
            return []
        return agent.cpp_prefix(self.cfg)


Check = Callable[[Probe], list[str]]


def _run(cmd: list[str], timeout: float = 30) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


# ---------------------------------------------------------------- the sandbox

def perf_counters(probe: Probe) -> list[str]:
    """`perf stat` returns real counters here and inside the agents' sandbox (B1: no /sys there, no PMU)."""
    problems = []
    for where, prefix in (("the run's sandbox", []), ("the agents' sandbox", probe.sandbox_prefix)):
        if where == "the agents' sandbox" and not prefix:
            continue
        proc = _run([*prefix, "perf", "stat", "-x,", "-e", PERF_EVENTS, "true"])
        counters = [line.split(",") for line in proc.stderr.splitlines() if line.count(",") >= 2]
        counted = [c for c in counters if c[0].strip().isdigit()]
        if proc.returncode or len(counted) < len(PERF_EVENTS.split(",")):
            problems.append(f"perf stat returns no counters in {where} ({(proc.stderr or proc.stdout).strip()[-200:]})")
    return problems


# ---------------------------------------------------------------- codex

def _capture_argv(probe: Probe, stage: str) -> tuple[object, list[str]]:
    """(the backend, the `codex exec` command line it builds for `stage`), without starting codex."""
    backend = create_agent_backend(pipeline_config=probe.cfg, query=probe.query, stage=stage, model_config=probe.models,
                                   workload_name=probe.workload_name)
    seen: list[list[str]] = []
    backend._supervise = lambda cmd, *a, **k: seen.append(cmd) or _Outcome(0, "", "")
    backend.run_stage(StageRunRequest(stage=stage, instructions="preflight", user_message="", agent_name="preflight",
                                      model_id=make_model_id(stage, probe.cfg, probe.models, None)))
    return backend, seen[0]


def _settings(argv: list[str]) -> dict[str, str]:
    """The `-c key=value` overrides of a codex command line."""
    return {k: v for k, _, v in (argv[i + 1].partition("=") for i, a in enumerate(argv[:-1]) if a == "-c")}


def codex_line(probe: Probe) -> list[str]:
    """The command line carries the configured context window, compaction limit and compact prompt (B3, B4)."""
    m = probe.models
    wanted = {"model_context_window": m.codex_model_context_window, "model_auto_compact_token_limit": m.codex_auto_compact_tokens,
              "experimental_compact_prompt_file": m.codex_compact_prompt_file}
    problems = [f"models config sets no {key}" for key, value in wanted.items() if not value]
    settings = _settings(_capture_argv(probe, "implementation")[1])
    for key, value in wanted.items():
        if value and key not in settings:
            problems.append(f"the codex command line carries no {key}")
    compact = settings.get("experimental_compact_prompt_file", "").strip('"')
    if compact and not Path(compact).is_file():
        problems.append(f"compact prompt file {compact} does not exist")
    for key, value in (("model_context_window", m.codex_model_context_window), ("model_auto_compact_token_limit", m.codex_auto_compact_tokens)):
        if value and settings.get(key) not in (None, str(value)):
            problems.append(f"{key} is {settings[key]} on the command line, {value} in the config")
    return problems


# ---------------------------------------------------------------- the MCP server

def _mcp_server_parameters(probe: Probe, stage: str):
    """Parameters that start the MCP server of `stage` the way codex does, inside the agents' sandbox."""
    from mcp import StdioServerParameters

    backend, argv = _capture_argv(probe, stage)
    env = tomllib.loads(f"env = {_settings(argv)['mcp_servers.accel_pipeline.env']}")["env"]
    launcher = tomllib.loads(backend._config_path().read_text())["mcp_servers"]["accel_pipeline"]["command"]
    prefix = probe.sandbox_prefix
    return StdioServerParameters(command=(prefix or [launcher])[0], args=[*prefix[1:], *([launcher] if prefix else [])],
                                 env={**os.environ, **env})


async def _mcp_probe(probe: Probe) -> tuple[dict[str, set[str]], list[str]]:
    """({stage: its server's registered tool names}, the results of PARALLEL_DATA_CALLS concurrent data-tool calls)."""
    from mcp import ClientSession
    from mcp.client.stdio import stdio_client

    names: dict[str, set[str]] = {}
    results: list = []
    for stage in STAGE_TOOLS:
        async with stdio_client(_mcp_server_parameters(probe, stage)) as (read, write):
            async with ClientSession(read, write, read_timeout_seconds=60.0) as session:
                await session.initialize()
                names[stage] = {t.name for t in (await session.list_tools()).tools}
                if stage == "analysis":
                    calls = [session.call_tool("query_data", {"sql": "SELECT 1"}) for _ in range(PARALLEL_DATA_CALLS)]
                    results = await asyncio.gather(*calls, return_exceptions=True)
    return names, [r if isinstance(r, Exception) else r.content[0].text for r in results]


@contextlib.contextmanager
def _unlogged(probe: Probe):
    """The data tools log every query to the run's artifacts, which analysis cites: the probe leaves that log as it was."""
    log = query_artifacts_dir(probe.cfg.repo_root, probe.query, workload=probe.workload) / LOG_FILE
    before = log.read_bytes() if log.is_file() else None
    try:
        yield
    finally:
        log.unlink(missing_ok=True)
        if before is not None:
            log.write_bytes(before)


def mcp_server(probe: Probe) -> list[str]:
    """The server starts for each stage, serves its tools (and the advisor's), and answers parallel data calls (B6)."""
    if unreachable := ensure_dataset_loaded(probe.workload, probe.cfg.repo_root, probe.cfg.build_dir):
        return [f"the data tools cannot reach the dataset: {unreachable}"]
    with _unlogged(probe):
        names, results = asyncio.run(asyncio.wait_for(_mcp_probe(probe), 90))
    problems = [f"stage {stage}: MCP tool {tool} is not registered" for stage, tools in STAGE_TOOLS.items() for tool in tools if tool not in names[stage]]
    registered = set().union(*names.values())
    problems += [f"advisor read-only tool {tool} is not registered" for tool in READ_ONLY_TOOLS if tool not in registered]
    failed = [str(r) for r in results if isinstance(r, Exception) or "error" in str(r)[:40].lower() or "lock" in str(r).lower()]
    if failed:
        problems.append(f"{len(failed)} of {PARALLEL_DATA_CALLS} parallel query_data calls failed, e.g. {failed[0][:200]}")
    return problems


def _rollout_calls(backend, result, tool: str) -> bool:
    """The codex session of `result` holds a call of the MCP tool `tool`."""
    sid = result.metadata.get("session_id")
    rollout = rollout_path(backend.codex_home(), sid) if sid else None
    return bool(rollout and rollout.is_file() and f'"name":"{tool}"' in rollout.read_text(errors="replace").replace(" ", ""))


def live_models(probe: Probe) -> list[str]:
    """One tiny codex turn per model the run uses, calling one MCP tool: the model speaks Codex's tool-call format."""
    models = {make_model_id(stage, probe.cfg, probe.models, None) for stage in (*STAGES, "analysis_critic", "advisor")}
    problems = []
    for model in sorted(models):
        backend = create_agent_backend(pipeline_config=probe.cfg, query=probe.query, stage="preflight_live",
                                       model_config=probe.models, workload_name=probe.workload_name)
        backend._enabled_mcp_tools = lambda _stage: [LIVE_TOOL]  # only this tool is offered
        result = backend.run_stage(StageRunRequest(
            stage="preflight_live", model_id=model, agent_name="preflight", user_message="",
            instructions=f"Call the MCP tool {LIVE_TOOL} once, then reply with the single word DONE."))
        if result.status != "finished" or not _rollout_calls(backend, result, LIVE_TOOL):
            problems.append(f"model {model}: no clean tool call ({result.status}: {(result.error or result.final_text)[-300:]})")
    return problems


# ---------------------------------------------------------------- gates, data, build, prompts

def _python_sources(script: str) -> list[tuple[int, str]]:
    """(line, text) of every Python source a shell script builds with the shell's expansion on: an unquoted heredoc
    into python3 and a double-quoted `python3 -c "..."` string."""
    lines, found, i = script.splitlines(), [], 0
    while i < len(lines):
        heredoc = re.search(r"python3?\b.*<<-?\s*(['\"\\]?)(\w+)\1?\s*$", lines[i])
        if heredoc and not heredoc.group(1):
            end = next((j for j in range(i + 1, len(lines)) if lines[j].strip() == heredoc.group(2)), len(lines))
            found.append((i + 1, "\n".join(lines[i + 1:end])))
            i = end
        elif re.search(r'python3?\s+-c\s+"', lines[i]):
            body = re.search(r'python3?\s+-c\s+"((?:\\.|[^"\\])*)"', "\n".join(lines[i:]), re.S)
            found.append((i + 1, body.group(1) if body else ""))
        i += 1
    return found


def gate_writers(probe: Probe) -> list[str]:
    """No gate pastes text that can hold quotes (a log, a reason) into Python source (B7: a quote was a SyntaxError)."""
    problems = []
    for path in sorted((probe.cfg.pipeline_root / "gates").glob("*.sh")):
        for line, source in _python_sources(path.read_text()):
            for var in sorted(set(re.findall(r"\$\{?([A-Za-z_]\w*)", source)) - SAFE_INTERPOLATIONS):
                if var not in ("1", "2"):
                    problems.append(f"{path.name}:{line} pastes ${var} into Python source (pass it through the environment or argv)")
    return problems


def test_database(probe: Probe) -> list[str]:
    """The join-closed test databases (the differential subset and the tiny sanitizer cut) exist or can be built now."""
    try:
        cuts = ensure_cut_datasets(probe.cfg, probe.workload, probe.query)
    except Exception as exc:  # noqa: BLE001 - any failure to build them is the finding
        return [f"the test databases cannot be built: {type(exc).__name__}: {exc}"]
    if cuts is None:
        return []  # no cuts for this workload: the quick tier runs on the full dataset
    return [f"test database {d} holds no tables" for d in (cuts.subset, cuts.tiny) if not any(Path(d).glob("*.tbl"))]


def tsan_build(probe: Probe) -> list[str]:
    """The instrumented build can be made: the engine builds variants and its sibling build dir is writable."""
    info = host_info(probe.cfg.repo_root, probe.cfg.engine_name, Path(probe.cfg.build_dir)) if probe.cfg.engine_name else None
    if info is not None and not info.get("embedded"):
        return []  # the sanitizer gates report "not applicable" for this host
    sibling = Path(f"{probe.cfg.build_dir}-thread")
    try:
        sibling.mkdir(parents=True, exist_ok=True)
        (sibling / ".probe").touch()
        (sibling / ".probe").unlink()
    except OSError as exc:
        return [f"the thread-sanitizer build dir {sibling} is not writable: {exc}"]
    return []


PROMPTS = {"analysis": "01_analysis.md", "test_suite": "01b_test_suite.md", "implementation": "02_implementation.md",
           "optimization": "03_optimization_unified.md", "audit": "04_audit.md", "advisor": "05_advisor.md"}
SOURCE_PATH = re.compile(r"`((?:pipeline/(?!artifacts|prims_staging)|scripts/|substrate/|workloads/)[\w./-]+?)`")  # not a path the run creates


def prompt_problems(text: str, root: Path) -> list[str]:
    """Placeholders a rendered prompt left unresolved, and the repository paths it names that do not exist."""
    problems = [f"unresolved placeholder {{{{{key}}}}}" for key in sorted(set(re.findall(r"\{\{(\w+)\}\}", text)))]
    return problems + [f"names {ref}, which does not exist" for ref in sorted(set(SOURCE_PATH.findall(text))) if not any(root.glob(ref))]


def prompts(probe: Probe) -> list[str]:
    """Every stage prompt of the workload renders, with no unresolved placeholder and only existing pipeline paths."""
    problems = []
    for stage, name in PROMPTS.items():
        if not resolve_prompt_path(probe.cfg, probe.workload, name).is_file():
            problems.append(f"stage {stage}: prompt {name} does not exist")
            continue
        try:
            text = render_prompt(probe.cfg, name, probe.workload, probe.query, stage)
        except Exception as exc:  # noqa: BLE001
            problems.append(f"stage {stage}: prompt {name} does not render: {type(exc).__name__}: {exc}")
            continue
        problems += [f"stage {stage}: {name} {problem}" for problem in prompt_problems(text, probe.cfg.repo_root)]
    return problems


CHECKS: dict[str, Check] = {
    "perf": perf_counters,
    "codex_line": codex_line,
    "mcp_server": mcp_server,
    "gate_writers": gate_writers,
    "test_database": test_database,
    "tsan_build": tsan_build,
    "prompts": prompts,
}
OPTIONAL_CHECKS: dict[str, Check] = {"live_models": live_models}


def run_checks(probe: Probe, names: list[str] | None = None, *, models: bool = False) -> dict[str, list[str]]:
    """{check: its problems}, printing each check's wall time."""
    selected = {**CHECKS, **(OPTIONAL_CHECKS if models else {})}
    report: dict[str, list[str]] = {}
    for name, check in selected.items():
        if names and name not in names:
            continue
        started = time.monotonic()
        try:
            report[name] = check(probe)
        except Exception as exc:  # noqa: BLE001 - a check that crashes is a failed check
            report[name] = [f"the check itself failed: {type(exc).__name__}: {exc}"]
        print(f"[preflight] {name}: {'ok' if not report[name] else 'FAILED'} ({time.monotonic() - started:.1f}s)", file=sys.stderr, flush=True)
    return report


def require(probe: Probe, **kwargs) -> None:
    """Exit with one message per failed item, or return."""
    failed = {name: problems for name, problems in run_checks(probe, **kwargs).items() if problems}
    if failed:
        lines = [f"preflight failed ({len(failed)} item(s)); no model was called:"]
        lines += [f"  [{name}] {problem}" for name, problems in failed.items() for problem in problems]
        raise SystemExit("\n".join(lines))


def check_main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workload", required=True)
    parser.add_argument("--query", type=int, required=True)
    parser.add_argument("--engine", default="")
    parser.add_argument("--config", type=Path, default=None)
    parser.add_argument("--models-config", type=Path, default=None)
    parser.add_argument("--models", action="store_true", help="also probe every configured model with one tiny codex turn")
    parser.add_argument("--only", action="append", default=[], help="run only this check (repeatable)")
    args = parser.parse_args(argv)
    root = find_repo_root()
    load_repo_dotenv(root)
    cfg = load_pipeline_config(args.config, root)
    workload = load_workload(args.workload, root, engine=args.engine)
    cfg = apply_workload_overrides(cfg, workload)
    probe = Probe(cfg, load_model_config(args.models_config, repo_root=root), workload, args.workload, args.query)
    report = run_checks(probe, args.only or None, models=args.models)
    for name, problems in report.items():
        for problem in problems:
            print(f"[{name}] {problem}")
    return 1 if any(report.values()) else 0
from accel_pipeline import agent, runlog
from accel_pipeline.data import (
    instance_env,
    instance_tag,
    ensure_dataset_loaded,
    ensure_cut_datasets,
    host_info,
)
from accel_pipeline.agent import (
    clear_stale_run,
    StageRunRequest,
    create_agent_backend,
    _Outcome,
    READ_ONLY_TOOLS,
    rollout_path,
)
from accel_pipeline.mcp import LOG_FILE
from accel_pipeline.prompts import render_prompt, resolve_prompt_path


SMOKE_DIR = Path(__file__).resolve().parents[1] / "tests" / "smoke"
DEFAULT_SMOKE_WORKLOAD = "tpch_q14_aux"


def _engine_of(workload: str, engine: str | None) -> str:
    return engine or workload_supported_engines(workload, find_repo_root())[0]


def _query_of(workload: str, engine: str) -> int:
    return int(load_workload(workload, find_repo_root(), engine=engine).get("query_number") or 0)


def _flags(pairs: dict[str, object]) -> list[str]:
    """argv for the flags that are set: {"--stage": "audit", "--resume": True}."""
    argv: list[str] = []
    for flag, value in pairs.items():
        if value is True:
            argv.append(flag)
        elif value not in (False, None, ""):
            argv += [flag, str(value)]
    return argv


def _smoke_driver(module: str):
    """The smoke and canary drivers live with the scripted agent and fixtures in tests/smoke."""
    if not SMOKE_DIR.is_dir():
        raise SystemExit(f"{SMOKE_DIR} not found: smoke and canary run from a source checkout, not a clean tree")
    sys.path.insert(0, str(SMOKE_DIR))
    return __import__(module)


def cmd_run(args) -> int:

    query = args.query if args.query is not None else _query_of(args.workload, args.engine)
    argv = ["--workload", args.workload, "--query", str(query), "--engine", args.engine,
            *_flags({"--mode": args.mode, "--stage": args.stage, "--until": args.until, "--models-config": args.models,
                     "--resume": args.resume, "--dry-run": args.dry_run, "--skip-gates": args.skip_gates})]
    for override in args.model:
        argv += ["--model", override]
    return run_main(argv)


def cmd_canary(args) -> int:
    argv = ["--workload", args.workload, "--models", str(args.models),
            *_flags({"--engine": args.engine, "--stage": args.stage, "--until": args.until, "--rev": args.rev, "--work": args.work,
                     "--resume": args.resume, "--probe-models": args.probe_models})]
    return _smoke_driver("canary").main(argv)


def cmd_check(args) -> int:

    query = args.query if args.query is not None else _query_of(args.workload, args.engine)
    argv = ["--workload", args.workload, "--query", str(query),
            *_flags({"--engine": args.engine, "--models-config": args.models_config, "--models": args.models})]
    for name in args.only:
        argv += ["--only", name]
    return check_main(argv)


def cmd_smoke(args) -> int:
    argv = ["--workload", args.workload, "--engine", args.engine,
            *_flags({"--case": args.case, "--adversarial": args.adversarial, "--jobs": args.jobs, "--rev": args.rev,
                     "--work": args.work})]
    return _smoke_driver("pipeline_smoke").main(argv)


def cmd_status(args) -> int:
    print(runlog.render_status(args.run_dir.resolve()), end="")
    return 0


def cmd_timeline(args) -> int:
    print(runlog.write_timeline(args.run_dir.resolve()))
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="accel", description="Run, check and inspect the accelerator pipeline.")
    commands = parser.add_subparsers(dest="command", required=True, metavar="command")

    def add(name: str, func, summary: str) -> argparse.ArgumentParser:
        sub = commands.add_parser(name, help=summary, description=summary)
        sub.set_defaults(func=func)
        return sub

    run = add("run", cmd_run, "run one query end to end in this tree (agents call real models)")
    run.add_argument("workload", help="e.g. tpch_q3_aux")
    run.add_argument("--engine", default=None, help="default: the workload's first supported engine")
    run.add_argument("--query", type=int, help="default: the workload's query number")
    run.add_argument("--models", type=Path, help="models config (default: pipeline/config/models.yaml)")
    run.add_argument("--model", action="append", default=[], metavar="STAGE=PRESET", help="override one stage's model preset")
    run.add_argument("--mode", choices=["cpp", "verus"], help="default: the workload's mode")
    when = run.add_mutually_exclusive_group()
    when.add_argument("--stage", default="all", help="run only this stage")
    when.add_argument("--until", choices=STAGES[:-1], help="run all stages, then pause after this one")
    run.add_argument("--resume", action="store_true", help="continue a stopped or paused run")
    run.add_argument("--dry-run", action="store_true", help="render prompts, call no model")
    run.add_argument("--skip-gates", action="store_true", help="agent only; skip the gates after each stage")

    canary = add("canary", cmd_canary, "run one query with real models in a fresh sandboxed clean tree, then write its TIMELINE.md")
    canary.add_argument("workload")
    canary.add_argument("--models", type=Path, required=True, help="the models config under test")
    canary.add_argument("--engine", default=None, help="default: the workload's first supported engine")
    canary.add_argument("--stage", choices=STAGES, help="run this stage only (e.g. optimization again, with --resume)")
    canary.add_argument("--until", choices=STAGES[:-1], help="pause after this stage; --resume continues")
    canary.add_argument("--resume", action="store_true", help="continue the run in --work, with the pipeline of --rev")
    canary.add_argument("--probe-models", action="store_true", help="first make every model complete a tool-calling turn")
    canary.add_argument("--rev", default="HEAD", help="source revision the clean tree is made from")
    canary.add_argument("--work", type=Path, help="run directory (default: a new one under $ACCEL_CANARY_HOME)")

    check = add("check", cmd_check, "preflight: fail before any model call when the environment cannot run the pipeline")
    check.add_argument("workload")
    check.add_argument("--engine", default=None, help="default: the workload's first supported engine")
    check.add_argument("--query", type=int, help="default: the workload's query number")
    check.add_argument("--models", action="store_true", help="also probe every configured model with one tiny turn (cents)")
    check.add_argument("--models-config", type=Path)
    check.add_argument("--only", action="append", default=[], metavar="CHECK", help="run only this check (repeatable)")

    smoke = add("smoke", cmd_smoke, "run the pipeline with a scripted agent in a clean tree: no model calls, minutes")
    smoke.add_argument("--workload", default=DEFAULT_SMOKE_WORKLOAD)
    smoke.add_argument("--engine", default=None, help="default: the workload's first supported engine")
    cases = smoke.add_mutually_exclusive_group()
    cases.add_argument("--case", help="one adversarial case: the agent misbehaves and a guardrail must stop it")
    cases.add_argument("--adversarial", action="store_true", help="every adversarial case")
    smoke.add_argument("--jobs", type=int, help="adversarial cases run at a time")
    smoke.add_argument("--rev", default="HEAD")
    smoke.add_argument("--work", type=Path)

    status = add("status", cmd_status, "where a run is: stage, last events, cost so far")
    status.add_argument("run_dir", type=Path, help="a canary run directory, or a tree")
    timeline = add("timeline", cmd_timeline, "where a run's time and money went (writes <run dir>/TIMELINE.md)")
    timeline.add_argument("run_dir", type=Path)
    return parser


def _resolve_engine(args) -> None:
    if hasattr(args, "engine") and hasattr(args, "workload"):
        args.engine = _engine_of(args.workload, args.engine)


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    _resolve_engine(args)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(run_main())
