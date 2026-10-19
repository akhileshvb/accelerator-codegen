"""Post-analysis adversarial write-scenario stage (aux_state)."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

from accel_pipeline.config import ModelConfig, PipelineConfig, load_run_workload
from accel_pipeline.paths import analysis_artifact_path, query_artifacts_dir
from accel_pipeline.stages.base import run_agent_stage

STAGE = "adversarial_tests"
PROMPT_FILE = "01b_adversarial_tests.md"


def scenarios_path(repo_root: Path, query: int, *, workload: dict) -> Path:
    return query_artifacts_dir(repo_root, query, workload=workload) / "adversarial_write_scenarios.json"


def validate_scenarios(path: Path, *, query: int) -> None:
    if not path.is_file():
        raise RuntimeError(f"adversarial stage missing deliverable: {path}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"invalid adversarial_write_scenarios.json: {exc}") from exc
    if not isinstance(data, dict):
        raise RuntimeError("adversarial_write_scenarios.json must be a JSON object")
    if int(data.get("version", 0)) != 1:
        raise RuntimeError("adversarial_write_scenarios.json version must be 1")
    if int(data.get("query", -1)) != int(query):
        raise RuntimeError(
            f"adversarial_write_scenarios.json query mismatch: "
            f"file={data.get('query')!r} expected={query}"
        )
    point = data.get("point_writes")
    if not isinstance(point, dict):
        raise RuntimeError("point_writes object required")
    min_count = int(point.get("min_count", 0))
    max_probes = int(point.get("max_probes", 0))
    if min_count < 3:
        raise RuntimeError("point_writes.min_count must be >= 3")
    if max_probes < min_count or max_probes > 16:
        raise RuntimeError("point_writes.max_probes must be in [min_count, 16]")
    tables = point.get("tables")
    if not isinstance(tables, list) or not tables:
        raise RuntimeError("point_writes.tables must be a non-empty list")
    bulk = data.get("bulk_writes")
    if not isinstance(bulk, dict):
        raise RuntimeError("bulk_writes object required")
    required = bool(bulk.get("required"))
    min_chunks = int(bulk.get("min_chunks", 0))
    if required and min_chunks < 1:
        raise RuntimeError("bulk_writes.min_chunks must be >= 1 when required=true")
    if not isinstance(data.get("rationale"), str) or not str(data["rationale"]).strip():
        raise RuntimeError("rationale string required")


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, Any] | None = None,
    dry_run: bool = False,
) -> Any:
    workload = load_run_workload(pipeline_config, workload_name)
    if workload.get("workload_kind") != "aux_state":
        print(
            f"[pipeline] skipping {STAGE} — workload kind is not aux_state",
            file=sys.stderr,
            flush=True,
        )
        return None

    analysis_path = analysis_artifact_path(
        pipeline_config.repo_root, query, workload=workload
    )
    if not dry_run and not analysis_path.is_file():
        raise RuntimeError(f"{STAGE} requires analysis artifact: {analysis_path}")

    out = scenarios_path(
        pipeline_config.repo_root, query, workload_name=workload_name
    )
    result = run_agent_stage(
        stage=STAGE,
        prompt_file=PROMPT_FILE,
        pipeline_config=pipeline_config,
        model_config=model_config,
        workload_name=workload_name,
        query=query,
        user_message=(
            f"Write adversarial invalidating-write scenarios for Q{query} to "
            f"{out.relative_to(pipeline_config.repo_root)}."
        ),
        model_overrides=model_overrides,
        dry_run=dry_run,
    )
    if dry_run:
        return result
    validate_scenarios(out, query=query)
    print(f"[pipeline] {STAGE} wrote {out}", file=sys.stderr, flush=True)
    return result
