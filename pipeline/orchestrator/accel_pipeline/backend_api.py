"""The run's backend API index and engine profile, rendered into accelerator prompts.

The engine_backend run writes ``backends/<engine>/api/README.md``; its first table
under ``## Contract roles`` (format: engine_backend/docs/host_contract.md) maps each role
to the headers and symbols that provide it. Accelerator prompts name roles only.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import yaml

from accel_pipeline.engine_profile import render_engine_profile

ROLES_HEADING = "## Contract roles"
ACCELERATOR_PROFILE_SECTIONS = [
    "identity",
    "process_model",
    "transactions",
    "scan",
    "entry_points",
    "thread_knobs",
    "hazards",
]


@dataclass(frozen=True)
class ContractRole:
    role: str
    headers: tuple[str, ...]
    symbols: tuple[str, ...]


def _backticked(cell: str) -> tuple[str, ...]:
    return tuple(re.findall(r"`([^`]+)`", cell))


def parse_contract_roles(readme: str) -> list[ContractRole]:
    """The rows of the first table under `## Contract roles`."""
    _, found, tail = readme.partition(ROLES_HEADING)
    if not found:
        return []
    roles: list[ContractRole] = []
    seen_table = False
    for line in tail.splitlines():
        if not line.lstrip().startswith("|"):
            if seen_table:
                break
            continue
        seen_table = True
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) != 3 or cells[0] == "role" or set(cells[0]) <= set("-: "):
            continue
        roles.append(ContractRole(cells[0], _backticked(cells[1]), _backticked(cells[2])))
    return roles


def render_backend_api(repo_root: Path, engine: str) -> str:
    """The role index of `backends/<engine>/api/README.md` as prompt markdown."""
    readme = repo_root / "backends" / engine / "api" / "README.md"
    api = f"backends/{engine}/api"
    if not readme.is_file():
        return f"### Backend API\n\n`{api}/README.md` is absent: the backend has not been generated for this run.\n"
    lines = [
        "### Backend API",
        "",
        f"Include only the `{api}/` headers below, by role (`{api}/README.md` has the prose).",
        "",
    ]
    for r in parse_contract_roles(readme.read_text(encoding="utf-8")):
        if not r.headers:
            lines.append(f"- **{r.role}:** n/a")
            continue
        symbols = f"; symbols {', '.join(f'`{s}`' for s in r.symbols)}" if r.symbols else ""
        headers = ", ".join(f"`{api}/{h.removeprefix('api/')}`" for h in r.headers)
        lines.append(f"- **{r.role}:** {headers}{symbols}")
    return "\n".join(lines) + "\n"


def render_accelerator_engine_profile(pipeline_root: Path, engine: str) -> str:
    """The engine's profile sections an accelerator stage sees; empty when the engine has none."""
    path = pipeline_root / "config" / "workloads" / f"engine_{engine}.yaml"
    if not path.is_file():
        return ""
    profile: dict[str, Any] = yaml.safe_load(path.read_text(encoding="utf-8"))["engine_profile"]
    return "### Engine profile\n\n" + render_engine_profile(profile, ACCELERATOR_PROFILE_SECTIONS)
