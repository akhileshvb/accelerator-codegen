"""PostgreSQL host prep for the pipeline (Docker-first).

Historically this module discovered a complete host-local server SDK
(pacman / ``$HOME/opt``). The engine_backend + host_bootstrap paths now use
Docker (``accel_pg_test``). Prefer :mod:`accel_pipeline.postgres_docker`.

This module re-exports the Docker entrypoints and keeps a small local-SDK
validator for unit tests / optional tooling.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping, MutableMapping

from accel_pipeline.postgres_docker import (
    PostgresDockerHost,
    apply_postgres_docker_env,
    discovery_summary as docker_discovery_summary,
    ensure_postgres_docker_host,
    format_agent_discovery_block as format_docker_discovery_block,
    host_to_dict,
    inject_postgres_into_gate_env,
    prepare_postgres_host_for_pipeline,
)


# ---------------------------------------------------------------------------
# Legacy local-SDK helpers (unit tests / optional); not used by the pipeline.
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class PostgresInstall:
    pg_config: Path
    bindir: Path
    includedir: Path
    includedir_server: Path
    libdir: Path
    version: str

    @property
    def initdb(self) -> Path:
        return self.bindir / "initdb"

    @property
    def postgres(self) -> Path:
        return self.bindir / "postgres"


def _run_pg_config(pg_config: Path, *args: str) -> str | None:
    import subprocess

    try:
        proc = subprocess.run(
            [str(pg_config), *args],
            capture_output=True,
            text=True,
            check=False,
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    if proc.returncode != 0:
        return None
    return (proc.stdout or "").strip()


def _is_exe(path: Path) -> bool:
    return path.is_file() and os.access(path, os.X_OK)


def validate_pg_config(pg_config: Path | str) -> PostgresInstall | None:
    """Return install info if ``pg_config`` points at a complete server SDK."""
    cfg = Path(pg_config).expanduser()
    if not _is_exe(cfg):
        return None
    includedir_server = _run_pg_config(cfg, "--includedir-server")
    includedir = _run_pg_config(cfg, "--includedir")
    bindir_s = _run_pg_config(cfg, "--bindir")
    libdir_s = _run_pg_config(cfg, "--libdir")
    version = _run_pg_config(cfg, "--version") or ""
    if not includedir_server or not bindir_s:
        return None
    server_inc = Path(includedir_server)
    bindir = Path(bindir_s)
    if not (server_inc / "postgres.h").is_file():
        return None
    if not _is_exe(bindir / "initdb") or not _is_exe(bindir / "postgres"):
        return None
    return PostgresInstall(
        pg_config=cfg.resolve(),
        bindir=bindir.resolve(),
        includedir=Path(includedir or (server_inc.parent)).resolve(),
        includedir_server=server_inc.resolve(),
        libdir=Path(libdir_s or (bindir.parent / "lib")).resolve(),
        version=version,
    )


def discover_postgres(
    *,
    env: Mapping[str, str] | None = None,
    home: Path | None = None,
) -> PostgresInstall | None:
    """Deprecated for pipeline use — returns None (Docker path is required)."""
    del env, home
    return None


def apply_postgres_env(
    env: MutableMapping[str, str] | None = None,
    *,
    required: bool = False,
) -> PostgresDockerHost | None:
    """Stamp Docker Postgres env (pipeline path)."""
    return prepare_postgres_host_for_pipeline(env=env, required=required, build=False)


def discovery_summary(install: PostgresDockerHost | PostgresInstall | None) -> str:
    if isinstance(install, PostgresDockerHost) or install is None:
        return docker_discovery_summary(install)  # type: ignore[arg-type]
    return (
        f"postgres: {install.version} pg_config={install.pg_config} "
        f"bindir={install.bindir} server_include={install.includedir_server}"
    )


def install_to_dict(install: PostgresDockerHost | PostgresInstall) -> dict[str, str]:
    if isinstance(install, PostgresDockerHost):
        return host_to_dict(install)
    return {
        "mode": "local_sdk",
        "version": install.version,
        "pg_config": str(install.pg_config),
        "bindir": str(install.bindir),
        "initdb": str(install.initdb),
        "postgres": str(install.postgres),
        "includedir": str(install.includedir),
        "includedir_server": str(install.includedir_server),
        "libdir": str(install.libdir),
        "root": str(install.bindir.parent),
    }


def format_agent_discovery_block(
    install: PostgresDockerHost | PostgresInstall | None,
) -> str:
    if isinstance(install, PostgresInstall):
        # Should not appear in pipeline prompts anymore.
        return format_docker_discovery_block(None)
    return format_docker_discovery_block(install)


def cmake_postgres_args(install: PostgresDockerHost | PostgresInstall | None) -> list[str]:
    """Host cmake does not build the PG MODULE in Docker mode — return []."""
    del install
    return []


__all__ = [
    "PostgresDockerHost",
    "PostgresInstall",
    "apply_postgres_docker_env",
    "apply_postgres_env",
    "cmake_postgres_args",
    "discover_postgres",
    "discovery_summary",
    "ensure_postgres_docker_host",
    "format_agent_discovery_block",
    "inject_postgres_into_gate_env",
    "install_to_dict",
    "prepare_postgres_host_for_pipeline",
    "validate_pg_config",
]
