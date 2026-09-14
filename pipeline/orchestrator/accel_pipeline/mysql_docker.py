"""Docker-backed MySQL host for engine_backend.

Builds and layer harnesses use a persistent container (``accel_mysql_test``,
``mysql:8.4``) via ``scripts/ensure_accel_mysql_test.sh``.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from typing import Mapping, MutableMapping


DEFAULT_CONTAINER = "accel_mysql_test"
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = "3307"
DEFAULT_USER = "root"
DEFAULT_PASSWORD = "mysql"
DEFAULT_DB = "accel_mysql_test"
PLUGIN_SO = "/usr/lib/mysql/plugin/accelerator_codegen_mysql.so"
IN_CONTAINER_BUILD = "/accel-build/accel"
DEFAULT_IMAGE = "mysql:8.4"


@dataclass(frozen=True)
class MysqlDockerHost:
    """Connection + build metadata for the orchestrated Docker MySQL."""

    container: str
    host: str
    port: str
    user: str
    password: str
    database: str
    plugin_so: str
    in_container_build: str
    image: str = DEFAULT_IMAGE

    @property
    def version(self) -> str:
        return f"Docker {self.image} container={self.container}"


def docker_host_from_env(env: Mapping[str, str] | None = None) -> MysqlDockerHost:
    env = env if env is not None else os.environ
    return MysqlDockerHost(
        container=(env.get("ACCEL_MYSQL_CONTAINER") or DEFAULT_CONTAINER).strip(),
        host=(env.get("MYSQL_HOST") or DEFAULT_HOST).strip(),
        port=(env.get("MYSQL_TCP_PORT") or env.get("MYSQL_PORT") or DEFAULT_PORT).strip(),
        user=(env.get("MYSQL_USER") or DEFAULT_USER).strip(),
        password=(env.get("MYSQL_PWD") or env.get("MYSQL_ROOT_PASSWORD") or DEFAULT_PASSWORD).strip(),
        database=(env.get("MYSQL_DATABASE") or DEFAULT_DB).strip(),
        plugin_so=(env.get("ACCEL_MYSQL_SO") or PLUGIN_SO).strip(),
        in_container_build=(env.get("ACCEL_MYSQL_IN_CONTAINER_BUILD") or IN_CONTAINER_BUILD).strip(),
        image=(env.get("ACCEL_MYSQL_IMAGE") or DEFAULT_IMAGE).strip(),
    )


def apply_mysql_docker_env(
    env: MutableMapping[str, str] | None = None,
    *,
    host: MysqlDockerHost | None = None,
) -> MysqlDockerHost:
    """Stamp Docker connection env for gates/agents/harnesses."""
    if env is None:
        env = os.environ
    host = host or docker_host_from_env(env)
    env["ACCEL_MYSQL_DOCKER"] = "1"
    env["ACCEL_MYSQL_BUILD_IN_DOCKER"] = "1"
    env["ACCEL_MYSQL_CONTAINER"] = host.container
    env["ACCEL_MYSQL_IMAGE"] = host.image
    env["MYSQL_HOST"] = host.host
    env["MYSQL_TCP_PORT"] = host.port
    env["MYSQL_PORT"] = host.port
    env["MYSQL_USER"] = host.user
    env["MYSQL_PWD"] = host.password
    env["MYSQL_ROOT_PASSWORD"] = host.password
    env["MYSQL_DATABASE"] = host.database
    env["ACCEL_MYSQL_SO"] = host.plugin_so
    env["ACCEL_MYSQL_IN_CONTAINER_BUILD"] = host.in_container_build
    return host


def discovery_summary(host: MysqlDockerHost | None) -> str:
    if host is None:
        return "mysql: docker host not ready"
    return (
        f"mysql: docker container={host.container} "
        f"{host.host}:{host.port} so={host.plugin_so}"
    )


def host_to_dict(host: MysqlDockerHost) -> dict[str, str]:
    return {
        "mode": "docker",
        "version": host.version,
        "container": host.container,
        "host": host.host,
        "port": host.port,
        "user": host.user,
        "database": host.database,
        "plugin_so": host.plugin_so,
        "in_container_build": host.in_container_build,
        "image": host.image,
    }


def format_agent_discovery_block(host: MysqlDockerHost | None) -> str:
    """Markdown block injected into agent prompts for Docker MySQL."""
    if host is None:
        return (
            "### Host SDK discovery (Docker MySQL)\n\n"
            "**Docker MySQL host is not ready.**\n"
            "Start/build with `bash scripts/ensure_accel_mysql_test.sh` "
            "(requires Docker). "
            "Do **not** install a host pacman/`$HOME/opt` server SDK for this pipeline — "
            "plugin builds run **inside** `accel_mysql_test` (`mysql:8.4`)."
        )
    d = host_to_dict(host)
    return (
        "### Host SDK discovery (Docker MySQL — use this)\n\n"
        "MySQL for this pipeline is a **persistent Docker container**. "
        "Edit sources on the host (mounted at `/accel-src`); rebuild via gates or "
        "`bash scripts/build_accel_mysql_plugin.sh` (runs cmake/ninja **inside** the container).\n\n"
        f"- **mode:** `docker`\n"
        f"- **container:** `{d['container']}` (`{d['image']}`)\n"
        f"- **connect (from host):** `{d['host']}:{d['port']}` "
        f"(user `{d['user']}`, db `{d['database']}`)\n"
        f"- **plugin .so (in container):** `{d['plugin_so']}`\n"
        f"- **in-container build dir:** `{d['in_container_build']}`\n\n"
        "Env stamped for agents/gates: `ACCEL_MYSQL_DOCKER=1`, `MYSQL_HOST`, "
        "`MYSQL_TCP_PORT`, `MYSQL_USER`, `MYSQL_PWD`, `ACCEL_MYSQL_CONTAINER`, "
        "`ACCEL_MYSQL_SO`.\n"
        "Inside the container, mysqld listens on **3306**. Harnesses that "
        "`docker exec` into the container must use `127.0.0.1:3306`, not the host port.\n"
        "Do **not** use DuckDB `*.duckdb` or Postgres as the scan host dataset."
    )


def prepare_mysql_host_for_pipeline(
    *,
    env: MutableMapping[str, str] | None = None,
    required: bool = False,
) -> MysqlDockerHost | None:
    """Stamp MySQL Docker env. Container start is the ensure script (gates)."""
    del required
    if env is None:
        env = os.environ
    host = apply_mysql_docker_env(env)
    env["ACCEL_MYSQL_DISCOVERY"] = discovery_summary(host)
    return host
