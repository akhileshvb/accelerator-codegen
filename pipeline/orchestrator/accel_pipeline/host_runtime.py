"""Host-runtime dispatch for engine_backend gates (engine-agnostic).

Workloads set ``host_runtime`` (kind, scripts, env, optional ``discovery``).
Shared prompts/gates never switch on ``engine_name``. New backends add a
workload YAML block and, if needed, a named discovery plugin below.
"""

from __future__ import annotations

import json
from typing import Any, Callable


def inject_host_runtime_into_gate_env(
    gate_env: dict[str, str], workload: dict[str, Any]
) -> None:
    host = dict(workload.get("host_runtime") or {})
    if not host:
        return
    kind = str(host.get("kind") or "host_cmake")
    gate_env["ACCEL_HOST_RUNTIME_KIND"] = kind
    for key, value in dict(host.get("env") or {}).items():
        gate_env[str(key)] = str(value)
    plugin = _discovery_plugin(str(host.get("discovery") or ""))
    if plugin is None:
        return
    inject = plugin.get("inject_env")
    if inject:
        inject(gate_env, workload, host)


def prepare_host_sdk_payload(workload: dict[str, Any]) -> dict[str, Any] | None:
    """Optional analysis-time host metadata. Returns None when unused."""
    host = dict(workload.get("host_runtime") or {})
    if not host:
        return None
    payload: dict[str, Any] = {
        "engine_name": str(workload.get("engine_name") or ""),
        "kind": str(host.get("kind") or "host_cmake"),
        "found": True,
        "summary": str(host.get("kind") or "host_cmake"),
    }
    plugin = _discovery_plugin(str(host.get("discovery") or ""))
    if plugin is None:
        return payload
    prepare = plugin.get("prepare_payload")
    if prepare:
        payload.update(prepare(workload, host))
    return payload


def discover_host_sdk_json(workload: dict[str, Any]) -> str:
    """JSON for the ``discover_host_sdk`` agent tool."""
    payload = prepare_host_sdk_payload(workload)
    if payload is None:
        return json.dumps(
            {
                "engine": str(workload.get("engine_name") or ""),
                "found": False,
                "reason": "no host_runtime auto-discovery; see the engine profile",
            },
            indent=2,
        )
    return json.dumps(payload, indent=2)


def _postgres_docker_inject(
    gate_env: dict[str, str], workload: dict[str, Any], host: dict[str, Any]
) -> None:
    from accel_pipeline.postgres_discovery import inject_postgres_into_gate_env

    inject_postgres_into_gate_env(gate_env, required=False)


def _postgres_docker_payload(workload: dict[str, Any], host: dict[str, Any]) -> dict[str, Any]:
    import os

    from accel_pipeline.postgres_discovery import (
        discovery_summary,
        install_to_dict,
        prepare_postgres_host_for_pipeline,
    )

    install = prepare_postgres_host_for_pipeline(
        env=os.environ, required=False, build=False
    )
    return {
        "mode": "docker",
        "found": install is not None,
        "summary": discovery_summary(install),
        "install": install_to_dict(install) if install else None,
    }


def _mysql_docker_inject(
    gate_env: dict[str, str], workload: dict[str, Any], host: dict[str, Any]
) -> None:
    from accel_pipeline.mysql_docker import apply_mysql_docker_env

    apply_mysql_docker_env(gate_env)


def _mysql_docker_payload(workload: dict[str, Any], host: dict[str, Any]) -> dict[str, Any]:
    import os

    from accel_pipeline.mysql_docker import (
        discovery_summary,
        host_to_dict,
        prepare_mysql_host_for_pipeline,
    )

    install = prepare_mysql_host_for_pipeline(env=os.environ, required=False)
    return {
        "mode": "docker",
        "found": install is not None,
        "summary": discovery_summary(install),
        "install": host_to_dict(install) if install else None,
    }


DiscoveryPlugin = dict[str, Callable[..., Any]]

# Named plugins referenced from workload YAML ``host_runtime.discovery``.
# Add a new backend by registering here — do not switch on engine_name.
_DISCOVERY_PLUGINS: dict[str, DiscoveryPlugin] = {
    "postgres_docker": {
        "inject_env": _postgres_docker_inject,
        "prepare_payload": _postgres_docker_payload,
    },
    "mysql_docker": {
        "inject_env": _mysql_docker_inject,
        "prepare_payload": _mysql_docker_payload,
    },
}


def _discovery_plugin(name: str) -> DiscoveryPlugin | None:
    if not name:
        return None
    return _DISCOVERY_PLUGINS.get(name)
