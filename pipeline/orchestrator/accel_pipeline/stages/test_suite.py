"""The test_suite stage: an agent of its own writes the acceptance suite the implementation is judged by.

It runs before the implementation, in its own session (never the implementer's), and its output is validated
mechanically (the test_suite_validate gate: files, then a run of every tuple and transaction on plain SQL) under
the repair loop. Afterwards the suite is frozen; the only way to change it is a change request that re-runs this
stage (``revise``), never an edit.
"""

from __future__ import annotations

import sys
from typing import Any

from accel_pipeline import test_suite as suite
from accel_pipeline.config import ModelConfig, PipelineConfig
from accel_pipeline.workload import load_run_workload, analysis_artifact_path, query_artifacts_dir
from accel_pipeline.failure import render_failure
from accel_pipeline.playbook import append_experiment_log
from accel_pipeline.repair import RepairCtx, run_with_repair
from accel_pipeline.scaffold_stubs import materialize_registration
from accel_pipeline.session import TESTER
from accel_pipeline.stages.base import run_agent_stage

STAGE = "test_suite"
PROMPT_FILE = "01b_test_suite.md"


def run(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
    dry_run: bool = False,
    *,
    change_request: str = "",
) -> Any:
    workload = load_run_workload(pipeline_config, workload_name)
    root, bench = pipeline_config.repo_root, str(workload.get("benchmark_name", "tpch"))
    directory = suite.suite_path(root, bench, query)
    art = query_artifacts_dir(root, query, workload=workload)
    analysis = analysis_artifact_path(root, query, workload=workload)
    if not dry_run and not analysis.is_file():
        raise RuntimeError(f"{STAGE} requires the analysis artifact: {analysis}")

    def author(message: str) -> Any:
        return run_agent_stage(
            stage=STAGE,
            prompt_file=PROMPT_FILE,
            pipeline_config=pipeline_config,
            model_config=model_config,
            workload_name=workload_name,
            query=query,
            user_message=message,
            model_overrides=model_overrides,
            dry_run=dry_run,
            must_change=directory,
            session_role=TESTER,
            extra_context={"change_request": change_request},
        )

    task = f"Write the test suite of Q{query} into {directory.relative_to(root)}."
    if change_request:
        task = (f"The implementation raised a change request against the frozen suite in {directory.relative_to(root)}. "
                "Decide whether the test is wrong (fix it, and say what changed) or right (leave it, and say why); "
                f"the suite must stay valid.\n\n{change_request}")
    if dry_run:
        return author(task)

    # The pipeline registers the query (declining module, the reference SQL as its fallback) before any test: the
    # tester owns tests/workload only, and the validation runs through the engine's normal entry point.
    created = materialize_registration(pipeline_config, workload, query)["created"]
    if created:
        print(f"[pipeline] {STAGE}: scaffolded the registration {created}", file=sys.stderr, flush=True)

    ctx = RepairCtx(pipeline_config, model_config, workload_name, query, stage=STAGE, change_kind=STAGE,
                    model_overrides=model_overrides)
    ctx.repair_turn = lambda failure: author(render_failure(failure))
    result = run_with_repair(ctx, lambda: author(task), gates=["test_suite_validate"])
    record = suite.freeze(art, directory, query=query, reason="change request" if change_request else "test_suite stage")
    append_experiment_log(root, query, f"- {STAGE}: suite frozen, hash {record['hash'][:12]} ({record['reason']})",
                          workload=workload)
    print(f"[pipeline] {STAGE} froze {directory} ({record['hash'][:12]})", file=sys.stderr, flush=True)
    if not change_request:  # the query's module is still the declining one: the harnesses must pass on it
        from accel_pipeline.engine_backend.host_bootstrap import accept_harness

        accept_harness(pipeline_config, model_config, workload_name, workload, query, model_overrides)
    return result


def revise(
    pipeline_config: PipelineConfig,
    model_config: ModelConfig,
    workload_name: str,
    query: int,
    model_overrides: dict[str, str] | None = None,
) -> None:
    """Handle the pending change request: the tester re-runs on it, the suite is re-validated and frozen again,
    and the request is archived with the outcome."""
    workload = load_run_workload(pipeline_config, workload_name)
    art = query_artifacts_dir(pipeline_config.repo_root, query, workload=workload)
    directory = suite.suite_path(pipeline_config.repo_root, str(workload.get("benchmark_name", "tpch")), query)
    before = suite.content_hash(directory)
    run(pipeline_config, model_config, workload_name, query, model_overrides,
        change_request=suite.pending_change_request(art))
    suite.archive_change_request(
        art, "the tester kept the suite as it was" if suite.content_hash(directory) == before else "the tester changed the suite")
