#!/usr/bin/env python3
"""End-to-end smoke test of the accelerator pipeline with a scripted agent instead of a model.

Builds a clean tree from this repository (pipeline/clean_tree, plus the reference engine backend), runs
`run_pipeline --stage all` on it inside the real sandbox with `scripted_agent.py` in place of codex, and asserts
that every stage completed, every gate ran and passed, and nothing needed repairing: with known-good artifacts, a
repair round or a stop means the pipeline's own plumbing is broken. Everything but the model is real: the
orchestrator, the MCP tooling, the gates, the builds, host.sh, the sandboxes, the freezing, the stamps and the
measurement lease.

    scripts/pipeline_smoke.sh --workload <fixtures/NAME> --engine <fixtures/engines/NAME> [--rev HEAD] [--work DIR]

Exit status 0: the run passed; 1: a stage, gate or assertion failed (reported with the log tail); 2: the
environment cannot run it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
SOURCE_ROOT = HERE.parents[3]
SMOKE_HOME = Path(os.environ.get("ACCEL_SMOKE_HOME", f"/flash1/{os.environ.get('USER', 'smoke')}/smoke"))
STAGES = ("analysis", "test_suite", "implementation", "optimization", "audit")
GIT = ["git", "-c", "user.name=smoke", "-c", "user.email=smoke@localhost"]
NO_MODEL_ENV = "ACCEL_SMOKE_NO_MODEL"  # what the models config asks for instead of a provider key
BUILD_JOBS = os.environ.get("ACCEL_SMOKE_JOBS", "8")


class SmokeError(Exception):
    pass


class EnvironmentUnavailable(Exception):
    pass


def run(cmd: list[str], *, cwd: Path | None = None, env: dict[str, str] | None = None, check: bool = True,
        **kwargs) -> subprocess.CompletedProcess:
    proc = subprocess.run(cmd, cwd=cwd, env=env, text=True, capture_output=True, **kwargs)
    if check and proc.returncode:
        raise SmokeError(f"{' '.join(cmd[:4])} failed ({proc.returncode}):\n{(proc.stdout + proc.stderr)[-2000:]}")
    return proc


def git_archive(rev: str, paths: list[str], dest: Path) -> None:
    archive = subprocess.Popen(["git", "-C", str(SOURCE_ROOT), "archive", rev, *paths], stdout=subprocess.PIPE)
    subprocess.run(["tar", "-x", "-C", str(dest)], stdin=archive.stdout, check=True)
    if archive.wait():
        raise SmokeError(f"git archive {rev} {paths} failed")


# ---------------------------------------------------------------- the inputs of a run

def read_fixture(*parts: str) -> dict:
    return json.loads(HERE.joinpath("fixtures", *parts).read_text())


def run_inputs(engine: str, workload: dict) -> dict[str, str]:
    """The machine's inputs of a run: the engine's and the workload's environment variables (each overridable by the
    real environment; a value that names a path must exist). Data, not code: see fixtures/engines and fixtures/<workload>."""
    env: dict[str, str] = {}
    for source in (read_fixture("engines", f"{engine}.json"), workload):
        for key, default in source.get("environment", {}).items():
            env[key] = os.environ.get(key, default)
    for key, default in workload.get("databases", {}).items():
        env[key] = os.environ.get(key, default)
    missing = [f"{k}={v}" for k, v in env.items() if v.startswith("/") and not Path(v).exists()]
    if missing:
        raise EnvironmentUnavailable(f"missing: {', '.join(missing)}")
    return env


def check_tools() -> None:
    if os.environ.get("OPENROUTER_API_KEY"):
        raise EnvironmentUnavailable("OPENROUTER_API_KEY is set: the smoke test must not be able to reach a model "
                                     "(unset it)")
    for tool in ("bwrap", "codex", "git", "cmake", "rsync"):
        if not shutil.which(tool):
            raise EnvironmentUnavailable(f"{tool} is not installed")


# ---------------------------------------------------------------- the clean tree

def make_base(rev: str, engine: str, workload: dict, env: dict[str, str], dest: Path) -> None:
    """The clean tree of `rev` plus the engine's reference backend and the harness sources it builds with (the
    fixtures list them; the build names any that are still missing), committed as the one baseline commit."""
    run(["bash", str(SOURCE_ROOT / "pipeline/clean_tree/make_clean_tree.sh"), str(SOURCE_ROOT), str(dest), rev])
    sources = [p.format(engine=engine) for p in read_fixture("engines", f"{engine}.json")["base_sources"]
               + workload.get("base_sources", [])]
    git_archive(rev, sources, dest)
    probe = dest / workload["registration_probe"].format(engine=engine)  # a registration makes the build ask for the per-query sources
    probe.parent.mkdir(parents=True)
    probe.touch()
    scratch = Path(os.environ.get("TMPDIR", "/dev/shm")) / f"smoke-cfg-{os.getpid()}"
    for _ in range(12):
        shutil.rmtree(scratch, ignore_errors=True)
        proc = run(["cmake", "-S", str(dest), "-B", str(scratch), f"-DACCEL_CODEGEN_ENGINE={engine}"],
                   env={**os.environ, **env}, check=False)
        missing = sorted(set(re.findall(r"Cannot find source file:\s+((?:eval|backends|scripts)/\S+)", proc.stdout + proc.stderr)))
        if not missing:
            break
        git_archive(rev, missing, dest)
    shutil.rmtree(scratch, ignore_errors=True)
    shutil.rmtree(dest / "accelerators" / workload["benchmark"])
    run([*GIT, "add", "-A"], cwd=dest)
    run([*GIT, "commit", "-q", "-m", f"base: clean tree + backends/{engine} (from {rev})"], cwd=dest)


def base_tree(rev: str, engine: str, workload: dict, env: dict[str, str]) -> Path:
    """The base is kept per source commit: it only changes with the clean-tree tooling or the backend."""
    commit = run(["git", "-C", str(SOURCE_ROOT), "rev-parse", "--short", rev]).stdout.strip()
    base = SMOKE_HOME / "bases" / f"{engine}-{commit}"
    if not (base / ".git").is_dir():
        shutil.rmtree(base, ignore_errors=True)
        base.parent.mkdir(parents=True, exist_ok=True)
        make_base(rev, engine, workload, env, base)
    return base


def cached_venv(base: Path) -> Path:
    """One virtualenv per requirements file, kept between runs (a tree gets a copy: its python is all the run uses)."""
    requirements = base / "pipeline/orchestrator/requirements.txt"
    venv = SMOKE_HOME / "venvs" / hashlib.sha256(requirements.read_bytes()).hexdigest()[:12]
    if not (venv / "bin/python").is_file():
        shutil.rmtree(venv, ignore_errors=True)
        run([sys.executable, "-m", "venv", str(venv)])
        run([str(venv / "bin/pip"), "install", "-q", "-r", str(requirements)])
    return venv


def new_tree(base: Path, work: Path, workload: dict, env: dict[str, str]) -> Path:
    """A fresh tree from the base. The databases are copied into it (the runs may write to them) and `env`
    is repointed at the copies."""
    tree = work / "tree"
    run(["rsync", "-a", "--exclude=.git", f"{base}/", f"{tree}/"])
    run(["rsync", "-a", f"{cached_venv(base)}/", f"{tree}/pipeline/orchestrator/.venv/"])
    (tree / ".data").mkdir()
    for key in workload.get("databases", {}):
        copy = tree / ".data" / Path(env[key]).name
        shutil.copyfile(env[key], copy)
        env[key] = str(copy)
    run(["git", "init", "-q"], cwd=tree)
    run([*GIT, "add", "-A"], cwd=tree)
    run([*GIT, "commit", "-q", "-m", "initial"], cwd=tree)
    return tree


def make_kit(work: Path, reference_rev: str, workload: str, fixture: dict, query: int) -> Path:
    """What the scripted agent reads: its fixtures and the known-good tree it copies the implementation from.
    It lives outside the tree (the agents see it read-only), so it is never part of the tree's history."""
    kit = work / "kit"
    kit.mkdir()
    shutil.copytree(HERE / "fixtures", kit / "fixtures")
    shutil.copyfile(HERE / "scripted_agent.py", kit / "scripted_agent.py")
    (kit / "scripted_agent.py").chmod(0o755)
    benchmark = fixture["benchmark"]
    reference = kit / "reference"
    reference.mkdir()
    git_archive(reference_rev, [f"accelerators/{benchmark}/q{query}", f"accelerators/{benchmark}/common", "accelerators/prims"],
                reference)
    return kit


def write_configs(tree: Path, kit: Path) -> tuple[Path, Path]:
    """The model and pipeline configs of the run: the tree's own, with the scripted agent as `codex` and the
    kit mounted into the agents' sandbox."""
    models = (tree / "pipeline/config/models.yaml").read_text()
    models, n = re.subn(r"^codex_bin:.*$", f"codex_bin: {kit / 'scripted_agent.py'}", models, flags=re.M)
    models, k = re.subn(r"^api_key_env:.*$", f"api_key_env: {NO_MODEL_ENV}", models, flags=re.M)
    pipeline = (tree / "pipeline/config/default.yaml").read_text()
    pipeline, m = re.subn(r"^sandbox_extra_ro:.*$", f"sandbox_extra_ro: [{kit}]", pipeline, flags=re.M)
    if n != 1 or m != 1 or k != 1:
        raise SmokeError("pipeline/config no longer has the api_key_env / codex_bin / sandbox_extra_ro lines the smoke test overrides")
    (kit / "models.yaml").write_text(models)
    (kit / "pipeline.yaml").write_text(pipeline)
    return kit / "models.yaml", kit / "pipeline.yaml"


# ---------------------------------------------------------------- the run

def launch(tree: Path, work: Path, kit: Path, env: dict[str, str], workload: str, query: int, engine: str,
           models: Path, config: Path) -> subprocess.Popen:
    python = tree / "pipeline/orchestrator/.venv/bin/python"
    state = work / "state"
    state.mkdir()
    locks = Path(os.environ.get("ACCEL_HOST_LOCK_DIR") or SMOKE_HOME / "locks")
    instance = f"smoke_{os.getpid()}"
    inside = {
        "PYTHONPATH": str(tree / "pipeline/orchestrator"), "ACCEL_PIPELINE_PYTHON": str(python),
        **env, "ACCEL_ENGINE_INSTANCE": instance, "ACCEL_ENGINE_STATE_DIR": str(state),
        "ACCEL_CONCURRENT_WORKERS": os.environ.get("ACCEL_CONCURRENT_WORKERS", "4"),
        "CMAKE_BUILD_PARALLEL_LEVEL": BUILD_JOBS,
        NO_MODEL_ENV: "scripted", "ACCEL_SMOKE_KIT": str(kit), "ACCEL_SMOKE_REFERENCE": str(kit / "reference"),
        "ACCEL_SMOKE_CALLS": str(state / "agent_calls.jsonl"),
    }
    exports = " ".join(f"{k}={sh_quote(v)}" for k, v in inside.items())
    command = (f"export {exports}; cd {tree} && exec {python} -m accel_pipeline.run_pipeline --workload {workload} "
               f"--query {query} --engine {engine} --stage all --models-config {models} --config {config}")
    outer = {k: v for k, v in os.environ.items() if k != "OPENROUTER_API_KEY"}
    outer.update({
        "ACCEL_ENGINE_INSTANCE": instance, "ACCEL_ENGINE_STATE_DIR": str(state), "ACCEL_HOST_LOCK_DIR": str(locks),
        "ACCEL_SANDBOX_DATA": ":".join([*(v for v in env.values() if v.startswith("/") and not v.startswith(str(tree))), str(kit)]),
        "ACCEL_SANDBOX_STATE": str(state), **env,
    })
    log = (work / "pipeline.log").open("w")
    return subprocess.Popen([str(tree / "pipeline/clean_tree/sandbox.sh"), str(tree), "bash", "-c", command],
                            env=outer, stdin=subprocess.DEVNULL, stdout=log, stderr=subprocess.STDOUT,
                            start_new_session=True)


def sh_quote(text: str) -> str:
    import shlex

    return shlex.quote(text)


def wait(proc: subprocess.Popen, timeout: float, log: Path) -> int | None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            return proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            continue
    return None


def stop(proc: subprocess.Popen) -> None:
    import signal

    try:
        os.killpg(proc.pid, signal.SIGTERM)
        proc.wait(timeout=30)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--workload", required=True)
    parser.add_argument("--engine", required=True)
    parser.add_argument("--rev", default="HEAD", help="the source revision the clean tree is made from")
    parser.add_argument("--reference-rev", default="", help="where the known-good accelerator comes from (default: --rev)")
    parser.add_argument("--work", type=Path, default=None, help="run directory (default: a new one under the smoke home)")
    parser.add_argument("--timeout", type=float, default=float(os.environ.get("ACCEL_SMOKE_TIMEOUT_SEC", 3600)))
    args = parser.parse_args(argv)

    try:
        check_tools()
        fixture = read_fixture(args.workload, "fixture.json")
        env = run_inputs(args.engine, fixture)
    except (EnvironmentUnavailable, OSError) as exc:
        print(f"smoke test cannot run: {exc}", file=sys.stderr)
        return 2
    query = int(fixture["query"])
    work = args.work or SMOKE_HOME / "runs" / time.strftime("%Y%m%d-%H%M%S")
    work.mkdir(parents=True)
    timings: list[tuple[str, float]] = []

    def step(name: str, fn, *a):
        started = time.monotonic()
        value = fn(*a)
        timings.append((name, time.monotonic() - started))
        print(f"[smoke] {name}: {timings[-1][1]:.0f}s", flush=True)
        return value

    try:
        base = step("base tree", base_tree, args.rev, args.engine, fixture, env)
        tree = step("fresh tree", new_tree, base, work, fixture, env)
        kit = step("kit", make_kit, work, args.reference_rev or args.rev, args.workload, fixture, query)
        models, config = write_configs(tree, kit)
        started = time.monotonic()
        proc = launch(tree, work, kit, env, args.workload, query, args.engine, models, config)
        print(f"[smoke] pipeline running: tail -f {work / 'pipeline.log'}", flush=True)
        rc = wait(proc, args.timeout, work / "pipeline.log")
        if rc is None:
            stop(proc)
        timings.append(("pipeline", time.monotonic() - started))
    except (SmokeError, subprocess.CalledProcessError) as exc:
        print(f"[smoke] FAILED while preparing: {exc}", file=sys.stderr)
        return 1

    from smoke_report import verify

    return verify(tree, work, args.workload, query, args.engine, rc, timings)


if __name__ == "__main__":
    sys.path.insert(0, str(HERE))
    raise SystemExit(main())
