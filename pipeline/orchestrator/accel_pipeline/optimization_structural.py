"""Redesigns of the aux state inside the optimization stage.

When the profile shows the state itself is the bottleneck, an attempt may redesign it in place.
It is kept only if it passes the full tier of the correctness, unit, race and multi-connection
gates and measurably improves reads without regressing writes (see `optimization_eval`); the
redesign is then written to the design record the audit reads.
"""

from __future__ import annotations

import json
from collections import Counter
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from accel_pipeline.optimization_checkpoint import CHECKPOINT_DIRNAME
from accel_pipeline.optimization_saturation import STRATEGY_STRUCTURAL

DESIGN_REVISIONS_FILE = "design_revisions.jsonl"
DEFAULT_GATES = ("aux_correctness", "aux_unit", "aux_tsan", "aux_multi_connection")
DEFAULT_INFERRED_DIFF_FRACTION = 0.35
REVISION_FIELDS = ("profile_evidence", "old_organization", "new_organization", "read_work_ratio")


def structural_config(pipeline_config: Any) -> dict[str, Any]:
    raw = getattr(pipeline_config, "optimization_structural", None)
    cfg = dict(raw) if isinstance(raw, dict) else {}
    return {
        "gates": list(cfg.get("gates") or DEFAULT_GATES),
        "inferred_diff_fraction": float(cfg.get("inferred_diff_fraction", DEFAULT_INFERRED_DIFF_FRACTION)),
    }


def changed_line_fraction(art: Path, repo_root: Path) -> float:
    """Share of the attempt-start lines of the checkpointed sources that the attempt removed or replaced."""
    ckpt = art / CHECKPOINT_DIRNAME
    before_total = removed = 0
    for saved in ckpt.rglob("*") if ckpt.is_dir() else ():
        if not saved.is_file():
            continue
        current = repo_root / saved.relative_to(ckpt)
        before = Counter(saved.read_text(encoding="utf-8", errors="replace").splitlines())
        after = Counter(current.read_text(encoding="utf-8", errors="replace").splitlines()) if current.is_file() else Counter()
        before_total += sum(before.values())
        removed += sum((before - after).values())
    return removed / before_total if before_total else 0.0


def is_structural_attempt(pipeline_config: Any, art: Path, hypothesis: dict[str, Any] | None) -> bool:
    """Declared by the attempt (strategy=structural), or inferred from how much of the sources it rewrote."""
    declared = (hypothesis or {}).get("strategy") == STRATEGY_STRUCTURAL or (hypothesis or {}).get("tier") == "structural_redesign"
    return declared or changed_line_fraction(art, pipeline_config.repo_root) >= structural_config(pipeline_config)["inferred_diff_fraction"]


def design_revision_error(hypothesis: dict[str, Any] | None) -> str:
    revision = (hypothesis or {}).get("design_revision")
    if not isinstance(revision, dict):
        return "a structural attempt must carry design_revision in its hypothesis JSON: " + ", ".join(REVISION_FIELDS)
    missing = [key for key in REVISION_FIELDS if not str(revision.get(key) or "").strip()]
    return f"design_revision is missing: {', '.join(missing)}" if missing else ""


def run_full_tier_gates(pipeline_config: Any, workload: dict[str, Any], query: int, *, iteration: int) -> str:
    """Run the structural gates in order; return "" when all pass, else which one failed and why."""
    from accel_pipeline.gates import run_gate

    for name in structural_config(pipeline_config)["gates"]:
        result = run_gate(pipeline_config, name, query, workload, change_kind="optimization", iteration=iteration)
        if result.status != "pass":
            detail = str(result.data.get("detail") or result.data.get("message") or "")[-1500:]
            return f"structural attempt failed gate {name} ({result.status}): {detail}"
    return ""


def record_design_revision(
    art: Path, *, attempt_id: str, hypothesis: dict[str, Any] | None,
    before: dict[str, Any], after: dict[str, Any],
) -> None:
    """The design record for a kept redesign: what the profile showed, old to new organization, measured effect."""
    entry = {
        "attempt_id": attempt_id,
        "ts": datetime.now(timezone.utc).isoformat(),
        **{key: ((hypothesis or {}).get("design_revision") or {}).get(key) for key in REVISION_FIELDS},
        "measured": {
            "read_speedup_before": before.get("read_only_query_speedup"),
            "read_speedup_after": after.get("read_only_query_speedup"),
            "write_mix_before": before.get("write_mix_workload_speedup"),
            "write_mix_after": after.get("write_mix_workload_speedup"),
        },
    }
    with (art / DESIGN_REVISIONS_FILE).open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(entry) + "\n")


def design_revisions_markdown(art: Path) -> str:
    """Revisions made during optimization, for the audit: they supersede the analysis's design."""
    path = art / DESIGN_REVISIONS_FILE
    if not path.is_file():
        return ""
    lines = ["## Design revisions made during optimization (the final design; they supersede the analysis)", ""]
    for raw in path.read_text(encoding="utf-8").splitlines():
        entry = json.loads(raw)
        m = entry.get("measured") or {}
        lines += [
            f"- `{entry.get('attempt_id')}`: {entry.get('old_organization')} -> {entry.get('new_organization')}",
            f"  - profile evidence: {entry.get('profile_evidence')}",
            f"  - read_work_ratio: {entry.get('read_work_ratio')}; read speedup {m.get('read_speedup_before')} -> "
            f"{m.get('read_speedup_after')}, write mix {m.get('write_mix_before')} -> {m.get('write_mix_after')}",
        ]
    return "\n".join(lines) + "\n"
