"""Thin shared negative memory for optimization (advisory traps, not a bible)."""

from __future__ import annotations

import json
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from accel_pipeline.paths import pipeline_root

NEGATIVE_MEMORY_FILE = "optimization_negative_memory.json"
MAX_ENTRIES = 24
MAX_TRAP_CHARS = 160
MAX_WHEN_CHARS = 120
MAX_EVIDENCE_CHARS = 160

_SEED_ENTRIES: list[dict[str, Any]] = [
    {
        "id": "write_mix_cliff_full_rebuild",
        "trap": "Do not treat RebuildAll-on-every-point-write as finished when 10% write-mix ≪ 1×",
        "when": "aux; read already strong; write-mix cliff",
        "evidence": "Prefer ApplyDelta / touched partitions; dual-axis score weights write_mix",
        "source": "seed",
    },
    {
        "id": "huge_maps_on_hit_path",
        "trap": "Do not put huge rebuild/scratch maps on the accel hit path",
        "when": "read path regressions after maintenance refactors",
        "evidence": "Hit latency can jump to 100ms+ while write-mix looks improved",
        "source": "seed",
    },
    {
        "id": "sf1_hardcoded_capacities",
        "trap": "Do not hardcode SF1 cardinalities as live capacities on the hot path",
        "when": "sizing maps/arrays/partitions",
        "evidence": "Size from bind-time row_estimate / measured domain",
        "source": "seed",
    },
    {
        "id": "micro_opt_while_cliff",
        "trap": "Do not spend an attempt on ≤5% read micro-opts while write-mix cliff remains",
        "when": "aux dual-axis optimization; read already ≫1× and 10% ≪1×",
        "evidence": "Maintenance hypothesis usually moves the accept score more",
        "source": "seed",
    },
]


def negative_memory_path(repo_root: Path, engine: str) -> Path:
    """Per engine: one engine's learned traps never reach another engine's analysis."""
    return pipeline_root(repo_root) / "artifacts" / "shared" / engine / NEGATIVE_MEMORY_FILE


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _clip(text: str, limit: int) -> str:
    cleaned = " ".join((text or "").split())
    if len(cleaned) <= limit:
        return cleaned
    return cleaned[: limit - 3] + "..."


def _norm_key(trap: str) -> str:
    return re.sub(r"[^a-z0-9]+", " ", (trap or "").lower()).strip()


def _default_store() -> dict[str, Any]:
    return {
        "version": 1,
        "guidance": (
            "Advisory negative memory only. Prefer current profile/scoreboard over this list. "
            "Do not copy traps as recipes; ignore entries that contradict fresh measurements."
        ),
        "entries": [],
    }


def ensure_negative_memory(repo_root: Path, engine: str) -> Path:
    """Create the engine's store with seed traps if missing."""
    path = negative_memory_path(repo_root, engine)
    if path.is_file():
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    store = _default_store()
    for row in _SEED_ENTRIES:
        store["entries"].append(
            {
                **row,
                "trap": _clip(str(row["trap"]), MAX_TRAP_CHARS),
                "when": _clip(str(row.get("when") or ""), MAX_WHEN_CHARS),
                "evidence": _clip(str(row.get("evidence") or ""), MAX_EVIDENCE_CHARS),
                "query": None,
                "workload_kind": "aux_state",
                "created_at": _now(),
            }
        )
    path.write_text(json.dumps(store, indent=2) + "\n", encoding="utf-8")
    return path


def load_negative_memory(repo_root: Path, engine: str, *, limit: int = 12) -> list[dict[str, Any]]:
    path = ensure_negative_memory(repo_root, engine)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return []
    entries = [e for e in (data.get("entries") or []) if isinstance(e, dict)]
    return entries[-max(1, limit) :]


def load_negative_memory_guidance(repo_root: Path, engine: str) -> str:
    path = ensure_negative_memory(repo_root, engine)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        return _default_store()["guidance"]
    return str(data.get("guidance") or _default_store()["guidance"])


def format_negative_memory_markdown(
    entries: list[dict[str, Any]],
    *,
    guidance: str = "",
    limit: int = 8,
) -> list[str]:
    if not entries:
        return []
    lines = [
        "## Negative memory (advisory — not authority)",
        guidance
        or (
            "Prefer current profile/scoreboard. Ignore traps that contradict fresh measurements."
        ),
    ]
    for row in entries[-limit:]:
        trap = _clip(str(row.get("trap") or ""), MAX_TRAP_CHARS)
        when = _clip(str(row.get("when") or ""), MAX_WHEN_CHARS)
        q = row.get("query")
        qbit = f" Q{q}" if q else ""
        when_bit = f" _{when}_" if when else ""
        lines.append(f"-{qbit} {trap}{when_bit}")
    return lines


def append_negative_memory(
    repo_root: Path,
    engine: str,
    *,
    trap: str,
    when: str = "",
    evidence: str = "",
    query: int | None = None,
    workload_kind: str | None = None,
    source: str = "orchestrator",
    entry_id: str | None = None,
) -> dict[str, Any] | None:
    """Append a trap if novel. Returns the stored entry, or None if skipped/duplicate."""
    trap_text = _clip(trap, MAX_TRAP_CHARS)
    if len(trap_text) < 12:
        return None
    path = ensure_negative_memory(repo_root, engine)
    try:
        store = json.loads(path.read_text(encoding="utf-8"))
    except (json.JSONDecodeError, OSError):
        store = _default_store()
    if not isinstance(store, dict):
        store = _default_store()
    entries = [e for e in (store.get("entries") or []) if isinstance(e, dict)]
    key = _norm_key(trap_text)
    for existing in entries:
        if _norm_key(str(existing.get("trap") or "")) == key:
            return None
    entry = {
        "id": (entry_id or f"trap_{abs(hash(key)) % 10_000_000}").strip()[:64],
        "trap": trap_text,
        "when": _clip(when, MAX_WHEN_CHARS),
        "evidence": _clip(evidence, MAX_EVIDENCE_CHARS),
        "query": int(query) if query else None,
        "workload_kind": workload_kind,
        "source": source,
        "created_at": _now(),
    }
    entries.append(entry)
    store["guidance"] = store.get("guidance") or _default_store()["guidance"]
    store["version"] = int(store.get("version") or 1)
    store["entries"] = entries[-MAX_ENTRIES:]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(store, indent=2) + "\n", encoding="utf-8")
    return entry


def maybe_record_reject_trap(
    repo_root: Path,
    *,
    query: int,
    workload: dict[str, Any] | None,
    hypothesis: dict[str, Any] | None,
    verdict: str,
    reason: str = "",
    improvement_pct: float | None = None,
) -> dict[str, Any] | None:
    """Record a short trap from clear rejects only (avoid spam)."""
    if verdict not in {"reject_regress", "reject_broken"}:
        return None
    if verdict == "reject_regress" and improvement_pct is not None and improvement_pct > -8.0:
        # Mild noise — don't pollute global memory.
        return None
    hypo = hypothesis or {}
    bottleneck = str(hypo.get("bottleneck") or hypo.get("hypothesis") or "").strip()
    why = str(hypo.get("why") or reason or "").strip()
    trap = bottleneck or why
    if not trap:
        return None
    when = f"verdict={verdict}"
    if improvement_pct is not None:
        when += f"; Δ={improvement_pct:+.1f}%"
    return append_negative_memory(
        repo_root,
        str((workload or {}).get("engine_name") or ""),
        trap=f"Avoid repeating failed approach: {trap}",
        when=when,
        evidence=_clip(why or reason, MAX_EVIDENCE_CHARS),
        query=query,
        workload_kind=str((workload or {}).get("workload_kind") or "") or None,
        source="orchestrator",
    )
