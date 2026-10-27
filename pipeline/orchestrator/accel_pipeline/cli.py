"""`accel`: the one front end of the accelerator pipeline (README, "Running the pipeline")."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from accel_pipeline.runlog import STAGES

SMOKE_DIR = Path(__file__).resolve().parents[1] / "tests" / "smoke"
DEFAULT_SMOKE_WORKLOAD = "tpch_q14_aux"


def _engine_of(workload: str, engine: str | None) -> str:
    from accel_pipeline.workload import workload_supported_engines
    from accel_pipeline.config import find_repo_root

    return engine or workload_supported_engines(workload, find_repo_root())[0]


def _query_of(workload: str, engine: str) -> int:
    from accel_pipeline.workload import load_workload
    from accel_pipeline.config import find_repo_root

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
    from accel_pipeline import run_pipeline

    query = args.query if args.query is not None else _query_of(args.workload, args.engine)
    argv = ["--workload", args.workload, "--query", str(query), "--engine", args.engine,
            *_flags({"--mode": args.mode, "--stage": args.stage, "--until": args.until, "--models-config": args.models,
                     "--resume": args.resume, "--dry-run": args.dry_run, "--skip-gates": args.skip_gates})]
    for override in args.model:
        argv += ["--model", override]
    return run_pipeline.main(argv)


def cmd_canary(args) -> int:
    argv = ["--workload", args.workload, "--models", str(args.models),
            *_flags({"--engine": args.engine, "--stage": args.stage, "--until": args.until, "--rev": args.rev, "--work": args.work,
                     "--resume": args.resume, "--probe-models": args.probe_models})]
    return _smoke_driver("canary").main(argv)


def cmd_check(args) -> int:
    from accel_pipeline import preflight

    query = args.query if args.query is not None else _query_of(args.workload, args.engine)
    argv = ["--workload", args.workload, "--query", str(query),
            *_flags({"--engine": args.engine, "--models-config": args.models_config, "--models": args.models})]
    for name in args.only:
        argv += ["--only", name]
    return preflight.main(argv)


def cmd_smoke(args) -> int:
    argv = ["--workload", args.workload, "--engine", args.engine,
            *_flags({"--case": args.case, "--adversarial": args.adversarial, "--jobs": args.jobs, "--rev": args.rev,
                     "--work": args.work})]
    return _smoke_driver("pipeline_smoke").main(argv)


def cmd_status(args) -> int:
    from accel_pipeline import runlog

    print(runlog.render_status(args.run_dir.resolve()), end="")
    return 0


def cmd_timeline(args) -> int:
    from accel_pipeline import runlog

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
