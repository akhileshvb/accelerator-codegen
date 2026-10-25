#!/usr/bin/env python3
"""Real-model canary: one query end to end, in a fresh clean tree, with the models of a models config.

    accel canary tpch_q14_aux --models models.yaml [--engine duckdb] [--until STAGE] [--rev HEAD] [--work DIR] [--probe-models]

The same clean tree, sandbox, databases and launch as the smoke test (pipeline_smoke.py), with the model in place
of the scripted agent: the run to read before a big one (20-30 min, about $1 for a small aux query). It writes
<work>/TIMELINE.md (where the wall time and the money went) and exits with the pipeline's status.

The key comes from $OPENROUTER_API_KEY or the source repository's .env. With --probe-models each model first has
to complete one tiny tool-calling turn (preflight --models). Paths: $ACCEL_CANARY_HOME (default /flash1/$USER/canary),
the smoke test's inputs (ACCEL_SMOKE_HOME, the fixture's databases and dataset).
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import pipeline_smoke as smoke  # noqa: E402

CANARY_HOME = Path(os.environ.get("ACCEL_CANARY_HOME", f"/flash1/{os.environ.get('USER', 'canary')}/canary"))


def api_key() -> str:
    if key := os.environ.get("OPENROUTER_API_KEY"):
        return key
    dotenv = smoke.SOURCE_ROOT / ".env"
    found = re.findall(r"^OPENROUTER_API_KEY=(.+)$", dotenv.read_text(), re.M) if dotenv.is_file() else []
    if not found:
        raise smoke.EnvironmentUnavailable("no OPENROUTER_API_KEY in the environment or in the source repository's .env")
    return found[-1].strip()


def run_to_end(proc: subprocess.Popen, log: Path, timeout: float) -> int:
    """Wait for the run, echoing its stage lines."""
    deadline, seen = time.monotonic() + timeout, 0
    while proc.poll() is None:
        if time.monotonic() > deadline:
            smoke.stop(proc)
            print("[canary] timed out", flush=True)
            return 124
        lines = log.read_text(errors="replace").splitlines()
        for line in lines[seen:]:
            if line.startswith("=== stage") or "gate complete" in line or line.startswith("pipeline "):
                print(line[:200], flush=True)
        seen = len(lines)
        time.sleep(10)
    return proc.returncode


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workload", required=True)
    parser.add_argument("--models", type=Path, required=True, help="the models config under test")
    parser.add_argument("--engine", default="duckdb")
    parser.add_argument("--rev", default="HEAD")
    parser.add_argument("--work", type=Path, default=None)
    parser.add_argument("--timeout", type=float, default=float(os.environ.get("ACCEL_CANARY_TIMEOUT_SEC", 4 * 3600)))
    parser.add_argument("--probe-models", action="store_true")
    parser.add_argument("--until", default=None, help="pause after this stage; --resume continues")
    parser.add_argument("--resume", action="store_true",
                        help="continue the run in --work: the pipeline and backend of --rev are committed into its tree first")
    args = parser.parse_args(argv)
    try:
        for tool in ("bwrap", "codex", "git", "cmake", "rsync"):
            if not shutil.which(tool):
                raise smoke.EnvironmentUnavailable(f"{tool} is not installed")
        fixture = smoke.read_fixture(args.workload, "fixture.json")
        env = smoke.run_inputs(args.engine, fixture)
        key = api_key()
        work = args.work or CANARY_HOME / "runs" / time.strftime("%Y%m%d-%H%M%S")
        if args.resume:
            tree = work / "tree"
            smoke.git_archive(args.rev, ["pipeline", f"backends/{args.engine}"], tree)
            smoke.run([*smoke.GIT, "add", "-A", "pipeline", f"backends/{args.engine}"], cwd=tree)
            smoke.run([*smoke.GIT, "commit", "-q", "--allow-empty", "-m", f"pipeline and backend from {args.rev}"], cwd=tree)
            (work / "pipeline.log").replace(work / f"pipeline.{time.strftime('%H%M%S')}.log")
        else:
            work.mkdir(parents=True)
            base = smoke.base_tree(args.rev, args.engine, fixture, env)
            tree = smoke.new_tree(base, work, fixture, env)
    except (smoke.EnvironmentUnavailable, smoke.SmokeError, OSError) as exc:
        print(f"canary cannot run: {exc}", file=sys.stderr)
        return 2
    (tree / ".env").write_text(f"OPENROUTER_API_KEY={key}\n")
    (tree / ".env").chmod(0o600)
    inputs = work / "inputs"
    inputs.mkdir(exist_ok=True)
    models = inputs / "models.yaml"
    shutil.copyfile(args.models, models)
    config = tree / "pipeline/config/default.yaml"
    query = int(fixture["query"])
    pipeline = smoke.pipeline_args(args.workload, query, args.engine, models, config) + (["--until", args.until] if args.until else []) \
        + (["--resume"] if args.resume else [])
    print(f"[canary] {work}: tail -f {work / 'pipeline.log'}", flush=True)

    if args.probe_models and not args.resume:
        probe = smoke.launch(tree, work, env, ["accel_pipeline.preflight", "--workload", args.workload, "--query", str(query),
                                               "--engine", args.engine, "--models-config", str(models), "--config", str(config),
                                               "--models", "--only", "live_models"], data=(inputs,), log_name="models_probe.log")
        if run_to_end(probe, work / "models_probe.log", 1800):
            print((work / "models_probe.log").read_text()[-3000:])
            return 1
    proc = smoke.launch(tree, work, env, pipeline, data=(inputs,))
    rc = run_to_end(proc, work / "pipeline.log", args.timeout)
    python = tree / "pipeline/orchestrator/.venv/bin/python"
    subprocess.run([str(python), "-m", "accel_pipeline", "timeline", str(work)], env={**os.environ, "PYTHONPATH": str(tree / "pipeline/orchestrator")})
    print(f"[canary] exit {rc}; read {work / 'TIMELINE.md'} before a big run")
    return rc


if __name__ == "__main__":
    raise SystemExit(main())
