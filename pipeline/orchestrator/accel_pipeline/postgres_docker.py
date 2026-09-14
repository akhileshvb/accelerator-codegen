"""Docker-backed PostgreSQL host for engine_backend + host_bootstrap.

The pipeline no longer discovers a host-local server SDK (pacman / $HOME/opt).
Postgres builds and contract/differential runs use a persistent container
(``accel_pg_test``, ``postgres:17``) via ``scripts/ensure_accel_pg_test.sh``.
"""

from __future__ import annotations

import os
import subprocess
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Mapping, MutableMapping


DEFAULT_CONTAINER = "accel_pg_test"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = "5434"
DEFAULT_USER = "postgres"
DEFAULT_PASSWORD = "postgres"
DEFAULT_DB = "postgres"
EXTENSION_SO = "/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so"
IN_CONTAINER_BUILD = "/accel-build/accel"


@dataclass(frozen=True)
class PostgresDockerHost:
    """Connection + build metadata for the orchestrated Docker Postgres."""

    container: str
    host: str
    port: str
    user: str
    password: str
    database: str
    extension_so: str
    in_container_build: str
    image: str = "postgres:17"

    @property
    def version(self) -> str:
        return f"Docker {self.image} container={self.container}"


def _repo_root_from_env(env: Mapping[str, str]) -> Path:
    raw = (env.get("ACCEL_PIPELINE_REPO_ROOT") or "").strip()
    if raw:
        return Path(raw)
    # orchestrator package → repo root
    return Path(__file__).resolve().parents[3]


def docker_host_from_env(env: Mapping[str, str] | None = None) -> PostgresDockerHost:
    env = env if env is not None else os.environ
    return PostgresDockerHost(
        container=(env.get("ACCEL_PG_CONTAINER") or DEFAULT_CONTAINER).strip(),
        host=(env.get("PGHOST") or env.get("PG_HOST") or DEFAULT_HOST).strip(),
        port=(env.get("PGPORT") or env.get("PG_PORT") or DEFAULT_PORT).strip(),
        user=(env.get("PGUSER") or env.get("PG_USER") or DEFAULT_USER).strip(),
        password=(env.get("PGPASSWORD") or DEFAULT_PASSWORD).strip(),
        database=(env.get("PGDATABASE") or env.get("PG_DATABASE") or DEFAULT_DB).strip(),
        extension_so=(env.get("ACCEL_POSTGRES_SO") or EXTENSION_SO).strip(),
        in_container_build=(env.get("ACCEL_PG_IN_CONTAINER_BUILD") or IN_CONTAINER_BUILD).strip(),
        image=(env.get("ACCEL_PG_IMAGE") or "postgres:17").strip(),
    )


def ensure_script(repo_root: Path) -> Path:
    return repo_root / "scripts" / "ensure_accel_pg_test.sh"


def run_ensure_accel_pg_test(
    *,
    repo_root: Path | None = None,
    env: Mapping[str, str] | None = None,
    build: bool = True,
    load_tpch: bool = False,
    timeout: int = 3600,
) -> subprocess.CompletedProcess[str]:
    """Run scripts/ensure_accel_pg_test.sh (start container + optional build)."""
    env_map = dict(env if env is not None else os.environ)
    root = repo_root or _repo_root_from_env(env_map)
    script = ensure_script(root)
    if not script.is_file():
        raise FileNotFoundError(f"missing {script}")
    run_env = dict(env_map)
    run_env["ACCEL_PG_ENSURE_BUILD"] = "1" if build else "0"
    run_env["ACCEL_PG_ENSURE_TPCH"] = "1" if load_tpch else "0"
    # Ensure connection defaults are visible to child scripts.
    host = docker_host_from_env(run_env)
    run_env.setdefault("ACCEL_PG_CONTAINER", host.container)
    run_env.setdefault("PG_PORT", host.port)
    run_env.setdefault("PGPORT", host.port)
    run_env.setdefault("PGHOST", host.host)
    run_env.setdefault("PGPASSWORD", host.password)
    return subprocess.run(
        ["bash", str(script)],
        cwd=str(root),
        env=run_env,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def apply_postgres_docker_env(
    env: MutableMapping[str, str] | None = None,
    *,
    host: PostgresDockerHost | None = None,
) -> PostgresDockerHost:
    """Stamp Docker connection env for gates/agents/harnesses."""
    if env is None:
        env = os.environ
    host = host or docker_host_from_env(env)
    env["ACCEL_PG_DOCKER"] = "1"
    env["ACCEL_PG_BUILD_IN_DOCKER"] = "1"
    env["ACCEL_PG_CONTAINER"] = host.container
    env["ACCEL_PG_IMAGE"] = host.image
    env["PGHOST"] = host.host
    env["PG_HOST"] = host.host
    env["PGPORT"] = host.port
    env["PG_PORT"] = host.port
    env["PGUSER"] = host.user
    env["PG_USER"] = host.user
    env["PGPASSWORD"] = host.password
    env["PGDATABASE"] = host.database
    env["ACCEL_POSTGRES_SO"] = host.extension_so
    env["ACCEL_PG_IN_CONTAINER_BUILD"] = host.in_container_build
    # Host cmake must not try to build the PG MODULE against a local SDK.
    env.pop("ACCEL_CMAKE_POSTGRES_ARGS", None)
    env.pop("PG_CONFIG", None)
    env.pop("ACCEL_PG_CONFIG", None)
    env.pop("ACCEL_PG_BINDIR", None)
    return host


def ensure_postgres_docker_host(
    *,
    env: MutableMapping[str, str] | None = None,
    repo_root: Path | None = None,
    required: bool = False,
    build: bool = True,
    load_tpch: bool = False,
) -> PostgresDockerHost | None:
    """Ensure Docker Postgres is up (and optionally built); stamp env."""
    if env is None:
        env = os.environ
    host = apply_postgres_docker_env(env)
    root = repo_root or _repo_root_from_env(env)
    try:
        proc = run_ensure_accel_pg_test(
            repo_root=root,
            env=env,
            build=build,
            load_tpch=load_tpch,
        )
    except FileNotFoundError:
        if required:
            raise
        return None
    except subprocess.TimeoutExpired as exc:
        if required:
            raise RuntimeError(f"ensure_accel_pg_test timed out: {exc}") from exc
        return None

    if proc.returncode != 0:
        tail = ((proc.stdout or "") + (proc.stderr or ""))[-4000:]
        if required:
            raise RuntimeError(
                f"ensure_accel_pg_test failed (exit={proc.returncode}):\n{tail}"
            )
        env["ACCEL_PG_DOCKER_ENSURE_ERROR"] = tail
        return None

    env["ACCEL_PG_DISCOVERY"] = discovery_summary(host)
    env["ACCEL_PG_DOCKER_ENSURE_LOG"] = ((proc.stdout or "") + (proc.stderr or ""))[-2000:]
    return host


def discovery_summary(host: PostgresDockerHost | None) -> str:
    if host is None:
        return "postgres: docker host not ready"
    return (
        f"postgres: docker container={host.container} "
        f"{host.host}:{host.port} so={host.extension_so}"
    )


def host_to_dict(host: PostgresDockerHost) -> dict[str, str]:
    return {
        "mode": "docker",
        "version": host.version,
        "container": host.container,
        "host": host.host,
        "port": host.port,
        "user": host.user,
        "database": host.database,
        "extension_so": host.extension_so,
        "in_container_build": host.in_container_build,
        "image": host.image,
    }


def format_agent_discovery_block(host: PostgresDockerHost | None) -> str:
    """Markdown block injected into agent prompts for Docker Postgres."""
    if host is None:
        return (
            "### Host SDK discovery (Docker Postgres)\n\n"
            "**Docker Postgres host is not ready.**\n"
            "Start/build with `bash scripts/ensure_accel_pg_test.sh` "
            "(requires Docker + `psql` client). "
            "Do **not** install a host pacman/`$HOME/opt` server SDK for this pipeline — "
            "extension builds run **inside** `accel_pg_test` (`postgres:17`)."
        )
    d = host_to_dict(host)
    return (
        "### Host SDK discovery (Docker Postgres — use this)\n\n"
        "Postgres for this pipeline is a **persistent Docker container**. "
        "Edit sources on the host (mounted at `/accel-src`); rebuild via gates or "
        "`bash scripts/build_accel_postgres_extension.sh` (runs cmake/ninja **inside** the container).\n\n"
        f"- **mode:** `docker`\n"
        f"- **container:** `{d['container']}` (`{d['image']}`)\n"
        f"- **connect (from host):** `{d['host']}:{d['port']}` "
        f"(user `{d['user']}`, db `{d['database']}`)\n"
        f"- **extension .so (in container):** `{d['extension_so']}`\n"
        f"- **in-container build dir:** `{d['in_container_build']}`\n\n"
        "Env stamped for agents/gates: `ACCEL_PG_DOCKER=1`, `PGHOST`, `PGPORT`, "
        "`PGUSER`, `PGPASSWORD`, `ACCEL_PG_CONTAINER`, `ACCEL_POSTGRES_SO`.\n"
        "Harnesses must connect to this server (no host `initdb` / temp cluster). "
        "TPC-H for differentials lives in **Postgres-native tables** "
        "(e.g. DB `accel_tpch_test` via `scripts/setup_accel_postgres_tpch_test_db.sh`).\n"
        "Do **not** use DuckDB `*.duckdb` as the scan host dataset."
    )


def prepare_postgres_host_for_pipeline(
    *,
    env: MutableMapping[str, str] | None = None,
    required: bool = False,
    build: bool = True,
    load_tpch: bool = False,
) -> PostgresDockerHost | None:
    """Ensure Docker Postgres and stamp env (pipeline entrypoint)."""
    return ensure_postgres_docker_host(
        env=env,
        required=required,
        build=build,
        load_tpch=load_tpch,
    )


def inject_postgres_into_gate_env(
    env: MutableMapping[str, str],
    *,
    required: bool = False,
) -> PostgresDockerHost | None:
    """Stamp Docker env for a gate subprocess (ensure already done or light ensure)."""
    # Gates often run after pipeline prepare; still stamp connection vars.
    # Use build=False for speed if container already has the .so; callers that
    # need a rebuild invoke ensure/build scripts explicitly.
    host = apply_postgres_docker_env(env)
    env["ACCEL_PG_DISCOVERY"] = discovery_summary(host)
    if required and not _container_running(host.container):
        return ensure_postgres_docker_host(env=env, required=True, build=True)
    return host


def _container_running(name: str) -> bool:
    try:
        proc = subprocess.run(
            ["docker", "ps", "--format", "{{.Names}}"],
            capture_output=True,
            text=True,
            check=False,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    return name in {(ln or "").strip() for ln in (proc.stdout or "").splitlines()}


def docker_exec_build_cmd(host: PostgresDockerHost | None = None) -> list[str]:
    """Host-side command to rebuild the extension inside the container."""
    return ["bash", "scripts/build_accel_postgres_extension.sh"]


def docker_exec_test_cmd(
    binary: str,
    *args: str,
    host: PostgresDockerHost | None = None,
) -> list[str]:
    """Run an in-container test binary against the Docker Postgres server."""
    h = host or docker_host_from_env()
    # Inside the container the server listens on 5432.
    inner = [
        "env",
        "ACCEL_PG_DOCKER=1",
        "PGHOST=127.0.0.1",
        "PGPORT=5432",
        f"PGUSER={h.user}",
        f"PGPASSWORD={h.password}",
        f"PGDATABASE={h.database}",
        f"ACCEL_POSTGRES_SO={h.extension_so}",
        f"{h.in_container_build}/{binary}",
        *args,
    ]
    return ["docker", "exec", h.container, *inner]
