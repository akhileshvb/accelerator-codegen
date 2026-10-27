"""Preflight: the run's infrastructure and guardrail plumbing, checked before any model call.

Every check is a function returning its problems (none: ok); `run_pipeline` runs them all and refuses to start
a run with a problem, one message per failed item. Each check guards something that broke a live run
(pipeline ISSUES B1, B3, B4, B6, B7) or would only show once an agent hit it. Checks run in the sandboxes the
run uses (this process is inside `clean_tree/sandbox.sh`; the agents' bwrap is wrapped around the probes that
the agents' tools depend on).

    python -m accel_pipeline.preflight --workload W --query N --engine E [--models] [--only NAME]

`--models` also runs one tiny `codex exec` turn per configured model preset that calls an MCP tool: it
validates tool-call compatibility of a new model (costs cents; off by default).
"""

from __future__ import annotations

import argparse
import asyncio
import contextlib
import os
import re
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass
from pathlib import Path
from typing import Callable

from accel_pipeline.config import ModelConfig, PipelineConfig

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
        from accel_pipeline import sandbox

        return sandbox.cpp_prefix(self.cfg)


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
    from accel_pipeline.codex import StageRunRequest, create_agent_backend, _Outcome
    from accel_pipeline.config import make_model_id

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
    from accel_pipeline.config import query_artifacts_dir
    from accel_pipeline.tools.data_tools import LOG_FILE

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
    from accel_pipeline.repair import READ_ONLY_TOOLS
    from accel_pipeline.data import ensure_dataset_loaded

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
    from accel_pipeline.session import rollout_path

    sid = result.metadata.get("session_id")
    rollout = rollout_path(backend.codex_home(), sid) if sid else None
    return bool(rollout and rollout.is_file() and f'"name":"{tool}"' in rollout.read_text(errors="replace").replace(" ", ""))


def live_models(probe: Probe) -> list[str]:
    """One tiny codex turn per model the run uses, calling one MCP tool: the model speaks Codex's tool-call format."""
    from accel_pipeline.codex import StageRunRequest, create_agent_backend
    from accel_pipeline.config import make_model_id
    from accel_pipeline.runlog import STAGES

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
    from accel_pipeline.data import ensure_cut_datasets

    try:
        cuts = ensure_cut_datasets(probe.cfg, probe.workload, probe.query)
    except Exception as exc:  # noqa: BLE001 - any failure to build them is the finding
        return [f"the test databases cannot be built: {type(exc).__name__}: {exc}"]
    if cuts is None:
        return []  # no cuts for this workload: the quick tier runs on the full dataset
    return [f"test database {d} holds no tables" for d in (cuts.subset, cuts.tiny) if not any(Path(d).glob("*.tbl"))]


def tsan_build(probe: Probe) -> list[str]:
    """The instrumented build can be made: the engine builds variants and its sibling build dir is writable."""
    from accel_pipeline.data import host_info

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
    from accel_pipeline.prompts import render_prompt, resolve_prompt_path

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


def main(argv: list[str] | None = None) -> int:
    from accel_pipeline.config import apply_workload_overrides, load_workload
    from accel_pipeline.config import load_model_config, load_pipeline_config, load_repo_dotenv, find_repo_root

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


if __name__ == "__main__":
    raise SystemExit(main())
