from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from accel_pipeline.config import pipeline_root, query_artifacts_dir

PLAYBOOK_PROPOSAL_JSON = "playbook_proposal.json"
PLAYBOOK_TECHNIQUES_JSON = "playbook_techniques.json"


def _shared_playbook_path(repo_root: Path, workload: dict | None) -> Path:
    engine = str((workload or {}).get("engine_name") or "")
    return pipeline_root(repo_root) / "artifacts" / "shared" / engine / "optimization_playbook.md"


def _load_technique_store(path: Path) -> dict[str, Any]:
    if not path.is_file():
        return {"techniques": []}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return {"techniques": []}
    if not isinstance(data.get("techniques"), list):
        data["techniques"] = []
    return data


def _merge_technique_records(
    existing: list[dict[str, Any]],
    incoming: list[dict[str, Any]],
    *,
    iteration: int,
) -> list[dict[str, Any]]:
    merged = list(existing)
    for row in incoming:
        if not isinstance(row, dict):
            continue
        technique_id = str(row.get("technique_id") or row.get("id") or "").strip()
        if not technique_id:
            continue
        record = {
            "technique_id": technique_id,
            "iteration": int(row.get("iteration") or iteration),
            "hypothesis": str(row.get("hypothesis") or "")[:240],
            "kept": row.get("kept"),
            "speedup_median": row.get("speedup_median"),
            "speedup_delta": row.get("speedup_delta"),
            "profile_deltas": row.get("profile_deltas") or {},
        }
        merged = [
            item
            for item in merged
            if not (
                item.get("technique_id") == record["technique_id"]
                and int(item.get("iteration") or 0) == record["iteration"]
            )
        ]
        merged.append(record)
    return merged[-40:]


def _format_playbook_appendix(
    query: int,
    iteration: int,
    summary: str,
    techniques: list[dict[str, Any]],
) -> str:
    lines = [f"## Q{query} iter {iteration}", ""]
    if summary.strip():
        lines.append(summary.strip())
        lines.append("")
    for row in techniques:
        technique_id = row.get("technique_id", "unknown")
        kept = row.get("kept")
        verdict = "kept" if kept else "discarded" if kept is False else "n/a"
        delta = row.get("speedup_delta")
        delta_text = f", Δspeedup={float(delta):+.3f}x" if delta is not None else ""
        profile = row.get("profile_deltas") or {}
        profile_bits = ", ".join(f"{k}={v}" for k, v in list(profile.items())[:4])
        profile_text = f" ({profile_bits})" if profile_bits else ""
        lines.append(f"- `{technique_id}`: {verdict}{delta_text}{profile_text}")
    return "\n".join(lines).strip() + "\n"


def merge_playbook_proposal(repo_root: Path, query: int, *, workload: dict | None = None) -> bool:
    """Merge structured playbook proposal JSON into per-query technique memory and shared playbook."""
    art = query_artifacts_dir(repo_root, query, workload=workload)
    proposal_json = art / PLAYBOOK_PROPOSAL_JSON
    if not proposal_json.is_file():
        return False

    try:
        payload = json.loads(proposal_json.read_text(encoding="utf-8"))
    except json.JSONDecodeError:
        return False
    if not isinstance(payload, dict):
        return False

    iteration = int(payload.get("iteration") or 0)
    summary = str(payload.get("summary") or "").strip()
    techniques = [row for row in (payload.get("techniques") or []) if isinstance(row, dict)]
    if not summary and not techniques:
        return False

    store_path = art / PLAYBOOK_TECHNIQUES_JSON
    store = _load_technique_store(store_path)
    store["techniques"] = _merge_technique_records(
        list(store.get("techniques") or []),
        techniques,
        iteration=iteration,
    )
    store_path.write_text(json.dumps(store, indent=2), encoding="utf-8")

    playbook = _shared_playbook_path(repo_root, workload)
    appendix = _format_playbook_appendix(query, iteration, summary, techniques)
    existing = playbook.read_text(encoding="utf-8") if playbook.is_file() else ""
    playbook.parent.mkdir(parents=True, exist_ok=True)
    playbook.write_text(existing.rstrip() + "\n\n" + appendix, encoding="utf-8")
    return True


def append_experiment_log(
    repo_root: Path, query: int, line: str, *, workload: dict | None = None
) -> Path:
    path = query_artifacts_dir(repo_root, query, workload=workload) / "experiment_log.md"
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as f:
        f.write(line.rstrip() + "\n")
    return path
