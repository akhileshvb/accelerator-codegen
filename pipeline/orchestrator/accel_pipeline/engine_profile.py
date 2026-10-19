"""The standardized engine profile of an engine_backend workload.

Every engine states the same keys (``schemas/engine_profile.schema.json``); a leaf
is a value or ``{na: reason}``. The profile holds facts and hazards of the engine.
It is validated at workload load and rendered into the stage prompts, each stage
seeing the sections ``engine_profile_sections`` (engine_backend.yaml) gives it.
"""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import jsonschema

SCHEMA_RELATIVE = Path("schemas") / "engine_profile.schema.json"
ALL_SECTIONS = "all"


class EngineProfileError(ValueError):
    """The workload's engine_profile is missing or does not match the schema."""


def _is_na(value: Any) -> bool:
    return isinstance(value, dict) and set(value) == {"na"}


def validate_engine_profile(profile: Any, pipeline_root: Path, *, source: str) -> None:
    if not isinstance(profile, dict):
        raise EngineProfileError(f"{source}: engine_profile is required (see schemas/engine_profile.schema.json)")
    schema = json.loads((pipeline_root / SCHEMA_RELATIVE).read_text(encoding="utf-8"))
    errors = sorted(
        jsonschema.Draft202012Validator(schema).iter_errors(profile),
        key=lambda e: [str(p) for p in e.absolute_path],
    )
    if errors:
        lines = [f"- {'.'.join(str(p) for p in e.absolute_path) or '<root>'}: {_describe(e)}" for e in errors[:20]]
        more = f"\n- ... {len(errors) - 20} more" if len(errors) > 20 else ""
        raise EngineProfileError(f"{source}: invalid engine_profile\n" + "\n".join(lines) + more)


def _describe(error: jsonschema.ValidationError) -> str:
    if error.validator == "required":
        return f"missing key {error.message.split(' ')[0]}"
    if error.validator in ("oneOf", "anyOf"):
        return "needs a value or {na: reason}"
    return error.message.splitlines()[0][:160]


def host_runtime_of(profile: dict[str, Any]) -> dict[str, Any]:
    """The profile's host_runtime without its n/a leaves."""
    return {key: value for key, value in profile["host_runtime"].items() if not _is_na(value)}


def _render_value(value: Any) -> str:
    if _is_na(value):
        return f"n/a ({value['na']})"
    if isinstance(value, list):
        return "; ".join(str(v) for v in value)
    if isinstance(value, dict):
        return ", ".join(f"{k}={v}" for k, v in value.items())
    return str(value)


def render_engine_profile(
    profile: dict[str, Any],
    sections: list[str] | str,
    *,
    layer: str | None = None,
) -> str:
    """Markdown of the requested sections; hazards are narrowed to `layer` when given."""
    wanted = list(profile) if sections == ALL_SECTIONS else list(sections)
    out: list[str] = []
    for name in wanted:
        if name == "hazards":
            continue
        out.append(f"### {name}\n")
        out.extend(f"- **{key}:** {_render_value(value)}" for key, value in profile[name].items())
        out.append("")
    if "hazards" in wanted:
        hazards = [h for h in profile["hazards"] if layer is None or layer in h["layers"]]
        out.append("### hazards (engine facts, not instructions)\n")
        out.extend(
            f"- *{', '.join(h['layers'])}* — **{h['symptom']}** Engine fact: {h['engine_fact']}" for h in hazards
        )
        out.append("")
    return "\n".join(out).rstrip() + "\n"

