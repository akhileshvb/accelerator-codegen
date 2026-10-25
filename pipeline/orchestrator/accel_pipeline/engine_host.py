"""The engine host: every engine is driven through ``backends/<engine>/host.sh``.

The verb interface is ``prompts/engine_backend/host_contract.md``. Nothing here
names an engine; its facts come from the profile's ``host_runtime``.
"""

from __future__ import annotations

import hashlib
import json
import os
import re
import subprocess
from pathlib import Path
from typing import Any

HOST_SCRIPT = "backends/{engine}/host.sh"
INFO_TIMEOUT_S = 30


def host_script(repo_root: Path, engine: str) -> Path:
    return repo_root / HOST_SCRIPT.format(engine=engine)


def instance_tag(worker_id: str, build_dir: Path) -> str:
    """A worker's engine-instance prefix, unique on the host: its id plus a digest of its build dir, so
    workers of different trees (same query, same engine) never share an instance, container or port."""
    digest = hashlib.sha1(str(Path(build_dir).resolve()).encode()).hexdigest()[:6]
    return f"{re.sub(r'[^A-Za-z0-9_.-]', '_', worker_id or 'w0')}-{digest}"


def instance_env(repo_root: Path, engine: str, build_dir: Path) -> dict[str, str]:
    """The caller-set variables of the contract; the environment wins over these defaults."""
    instance = os.environ.get("ACCEL_ENGINE_INSTANCE") or f"{os.environ.get('ACCEL_INSTANCE_TAG') or 'accel'}-{engine}"
    return {
        "ACCEL_ENGINE_INSTANCE": instance,
        "ACCEL_ENGINE_STATE_DIR": os.environ.get("ACCEL_ENGINE_STATE_DIR")
        or str(build_dir / "engine_state" / instance),
        "ACCEL_BUILD_DIR": str(build_dir),
        "ACCEL_CMAKE_ARGS": os.environ.get("ACCEL_CMAKE_ARGS") or cmake_args(engine),
    }


def cmake_args(engine: str) -> str:
    """How `host.sh build` configures the build dir: the root CMake's backend switch
    (cmake/Backend.cmake) and this engine's query sources. Same default as aux_gate_common.sh."""
    return (
        f"-G Ninja -DCMAKE_BUILD_TYPE=Release -DACCELERATOR_CODEGEN_BUILD_{engine.upper()}=ON "
        f"-DACCEL_CODEGEN_ENGINE={engine}"
    )


def run_host_verb(
    repo_root: Path,
    engine: str,
    verb: str,
    *args: str,
    build_dir: Path,
    timeout: float | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, **instance_env(repo_root, engine, build_dir), **(env or {})}
    return subprocess.run(
        [str(host_script(repo_root, engine)), verb, *args],
        cwd=repo_root, env=env, capture_output=True, text=True, timeout=timeout,
    )


def host_info(repo_root: Path, engine: str, build_dir: Path) -> dict[str, Any] | None:
    """The `info` verb's JSON object, or None when the backend has no usable host.sh yet."""
    if not host_script(repo_root, engine).is_file():
        return None
    try:
        done = run_host_verb(repo_root, engine, "info", build_dir=build_dir, timeout=INFO_TIMEOUT_S)
        info = json.loads(done.stdout)
    except (OSError, subprocess.SubprocessError, ValueError):
        return None
    return info if done.returncode == 0 and isinstance(info, dict) else None


def workload_dataset_dir(workload: dict[str, Any]) -> str:
    return str(workload.get("dataset_dir") or os.environ.get("ACCEL_DATASET_DIR") or "")


def ensure_dataset_loaded(workload: dict[str, Any], repo_root: Path, build_dir: Path, timeout: float = 3600,
                          instance_env: dict[str, str] | None = None) -> str | None:
    """Bring the engine instance (the default one, or the one `instance_env` names) up with the workload's dataset
    loaded (`up` and `load` are idempotent and cheap when done). None on success, else why not; never raises."""
    from accel_pipeline.layout import workload_schema_sql

    engine = str(workload.get("engine_name") or "")
    dataset_dir = workload_dataset_dir(workload)
    if not host_script(repo_root, engine).is_file():
        return f"no host.sh for engine {engine!r}"
    if not dataset_dir:
        return "no dataset directory (workload dataset_dir or ACCEL_DATASET_DIR)"
    schema = repo_root / workload_schema_sql(str(workload.get("dataset_benchmark") or workload.get("benchmark_name") or ""))
    try:
        for verb, args in (("up", ()), ("load", (str(schema),))):
            done = run_host_verb(repo_root, engine, verb, *args, build_dir=build_dir, timeout=timeout,
                                 env={"ACCEL_DATASET_DIR": dataset_dir, **(instance_env or {})})
            if done.returncode not in (0, 77):
                return f"host.sh {verb} failed (exit {done.returncode}): {(done.stderr or done.stdout)[-500:]}"
    except (OSError, subprocess.SubprocessError) as exc:
        return f"host.sh failed: {exc}"
    return None


def inject_host_runtime_into_gate_env(
    gate_env: dict[str, str], workload: dict[str, Any], repo_root: Path, build_dir: Path
) -> None:
    host = dict(workload.get("host_runtime") or {})
    if not host:
        return
    gate_env["ACCEL_HOST_RUNTIME_KIND"] = str(host.get("kind") or "host_cmake")
    for key, value in dict(host.get("env") or {}).items():
        gate_env[str(key)] = str(value)
    engine = str(workload.get("engine_name") or "")
    if engine:
        gate_env.update(instance_env(repo_root, engine, build_dir))


def prepare_host_sdk_payload(
    workload: dict[str, Any], repo_root: Path, build_dir: Path
) -> dict[str, Any] | None:
    """Analysis-time host metadata; None when the workload has no host_runtime."""
    host = dict(workload.get("host_runtime") or {})
    if not host:
        return None
    engine = str(workload.get("engine_name") or "")
    info = host_info(repo_root, engine, build_dir)
    kind = str(host.get("kind") or "host_cmake")
    return {
        "engine_name": engine,
        "kind": kind,
        "found": info is not None,
        "summary": f"{kind}; verbs: {', '.join(info['verbs'])}" if info else f"{kind}; no host.sh yet",
        "host_script": HOST_SCRIPT.format(engine=engine),
        "info": info,
        "connect": host.get("connect"),
    }


def discover_host_sdk_json(workload: dict[str, Any], repo_root: Path, build_dir: Path) -> str:
    """JSON for the ``discover_host_sdk`` agent tool."""
    payload = prepare_host_sdk_payload(workload, repo_root, build_dir)
    if payload is None:
        payload = {
            "engine": str(workload.get("engine_name") or ""),
            "found": False,
            "reason": "no host_runtime in the workload; see the engine profile",
        }
    return json.dumps(payload, indent=2)


def format_host_block(workload: dict[str, Any], repo_root: Path, build_dir: Path) -> str:
    """Markdown for prompts: how to reach the engine (host.sh verbs and the profile's connect fact)."""
    host = dict(workload.get("host_runtime") or {})
    engine = str(workload.get("engine_name") or "")
    info = host_info(repo_root, engine, build_dir)
    lines = ["### Engine host", ""]
    if info:
        lines.append(f"`{HOST_SCRIPT.format(engine=engine)}` verbs: {', '.join(info['verbs'])}.")
    else:
        lines.append(f"`{HOST_SCRIPT.format(engine=engine)}` is not available yet.")
    lines.append(f"host_runtime.kind=`{host.get('kind') or 'host_cmake'}`. Connect: {host.get('connect') or 'see the engine profile'}")
    return "\n".join(lines)
