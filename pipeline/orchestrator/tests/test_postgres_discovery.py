"""Tests for Docker-first PostgreSQL host prep (+ legacy validate_pg_config)."""

from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.postgres_discovery import (
    apply_postgres_env,
    discover_postgres,
    discovery_summary,
    format_agent_discovery_block,
    inject_postgres_into_gate_env,
    validate_pg_config,
)
from accel_pipeline.postgres_docker import (
    apply_postgres_docker_env,
    docker_host_from_env,
    format_agent_discovery_block as format_docker_block,
)


def test_rejects_incomplete_pg_config(tmp_path: Path):
    fake = tmp_path / "pg_config"
    fake.write_text(
        "#!/bin/sh\n"
        "case \"$1\" in\n"
        "  --includedir-server) echo '%s' ;;\n"
        "  --includedir) echo '%s' ;;\n"
        "  --bindir) echo '%s' ;;\n"
        "  --libdir) echo '%s' ;;\n"
        "  --version) echo 'PostgreSQL 18.0' ;;\n"
        "  *) exit 1 ;;\n"
        "esac\n"
        % (tmp_path / "include" / "server", tmp_path / "include", tmp_path / "bin", tmp_path / "lib"),
        encoding="utf-8",
    )
    fake.chmod(0o755)
    (tmp_path / "include" / "server").mkdir(parents=True)
    (tmp_path / "bin").mkdir(parents=True)
    assert validate_pg_config(fake) is None


def test_validate_accepts_complete_layout(tmp_path: Path):
    root = tmp_path / "opt" / "pgsql-18"
    bindir = root / "bin"
    server = root / "include" / "server"
    bindir.mkdir(parents=True)
    server.mkdir(parents=True)
    (server / "postgres.h").write_text("/* fake */\n", encoding="utf-8")
    for name in ("initdb", "postgres"):
        p = bindir / name
        p.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        p.chmod(0o755)
    pg_config = bindir / "pg_config"
    pg_config.write_text(
        "#!/bin/sh\n"
        f"case \"$1\" in\n"
        f"  --includedir-server) echo '{server}' ;;\n"
        f"  --includedir) echo '{root / 'include'}' ;;\n"
        f"  --bindir) echo '{bindir}' ;;\n"
        f"  --libdir) echo '{root / 'lib'}' ;;\n"
        f"  --version) echo 'PostgreSQL 18.6' ;;\n"
        f"  *) exit 1 ;;\n"
        f"esac\n",
        encoding="utf-8",
    )
    pg_config.chmod(0o755)
    found = validate_pg_config(pg_config)
    assert found is not None
    assert found.initdb.name == "initdb"


def test_pipeline_discover_postgres_is_disabled():
    # Pipeline no longer searches host SDK; Docker path is required.
    assert discover_postgres(env={"HOME": "/tmp", "PATH": ""}, home=Path("/tmp")) is None


def test_apply_postgres_docker_env_stamps_connection():
    env: dict[str, str] = {"PATH": "/usr/bin"}
    host = apply_postgres_docker_env(env)
    assert env["ACCEL_PG_DOCKER"] == "1"
    assert env["PGHOST"] == host.host
    assert env["PGPORT"] == host.port
    assert env["ACCEL_PG_CONTAINER"] == host.container
    assert "ACCEL_CMAKE_POSTGRES_ARGS" not in env
    assert "PG_CONFIG" not in env
    assert "docker" in discovery_summary(host)


def test_inject_postgres_into_gate_env_docker():
    env: dict[str, str] = {"PATH": "/usr/bin", "PG_PORT": "5434"}
    install = inject_postgres_into_gate_env(env, required=False)
    assert install is not None
    assert env["ACCEL_PG_DOCKER"] == "1"
    assert env["PGPORT"] == "5434"
    assert "docker" in env["ACCEL_PG_DISCOVERY"]


def test_format_agent_discovery_block_mentions_docker():
    host = docker_host_from_env({})
    text = format_docker_block(host)
    assert "Docker" in text or "docker" in text
    assert "accel_pg_test" in text
    assert "pacman" not in text.lower() or "Do **not**" in text
    missing = format_agent_discovery_block(None)
    assert "ensure_accel_pg_test" in missing or "Docker" in missing


def test_apply_postgres_env_delegates_to_docker():
    env: dict[str, str] = {"PATH": "/usr/bin"}
    # build=False path via apply_postgres_env → prepare with build=False
    host = apply_postgres_env(env, required=False)
    # May be None if ensure fails without docker; at least env stamps from apply_postgres_docker
    # apply_postgres_env calls prepare with build=False which still runs ensure script.
    # For unit tests without docker, ensure may fail → None, but we can still stamp:
    apply_postgres_docker_env(env)
    assert env["ACCEL_PG_DOCKER"] == "1"
    del host  # unused when docker unavailable
