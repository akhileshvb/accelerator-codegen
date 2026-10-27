"""Smoke tests for engine_backend workloads."""

from __future__ import annotations

import json
from pathlib import Path

from accel_pipeline.config import (
    apply_workload_overrides,
    load_workload,
    load_pipeline_config,
    load_model_config,
)
from accel_pipeline.engine_backend.stage import (
    implementation_layers,
    run_engine_pipeline,
)
from accel_pipeline.layout import write_allowed


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_backend_stage_gates_are_wired():
    from accel_pipeline.gates.run import GATE_SCRIPTS

    gates_dir = REPO_ROOT / "pipeline" / "gates"
    for engine in ("engine_mysql", "engine_postgres", "engine_duckdb"):
        w = load_workload(engine, REPO_ROOT)
        wired = list(w["post_validation_gates"]) + [layer["gate"] for layer in w["implementation_layers"]]
        for gate in wired:
            assert gate in GATE_SCRIPTS, gate
            assert (gates_dir / GATE_SCRIPTS[gate]).is_file(), gate
    slot_gates = [g for g in GATE_SCRIPTS if g.startswith("slot_")]
    assert len(slot_gates) == 8  # the differential runs in the accelerator pipeline (aux_correctness)
    assert not (gates_dir / "slot_adapters").exists()


def test_engine_write_paths_allow_adapter_edits():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_mysql", REPO_ROOT)
    merged = apply_workload_overrides(pc, w)
    ok, reason = write_allowed(
        REPO_ROOT,
        "backends/mysql/engine/scan_helper.hpp",
        query=0,
        stage="impl_scan",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok, reason
    ok2, reason2 = write_allowed(
        REPO_ROOT,
        "backends/mysql/api/mysql_host.cpp",
        query=0,
        stage="impl_runtime",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok2, reason2
    ok3, reason3 = write_allowed(
        REPO_ROOT,
        "backends/mysql/tests/test_mysql_capture.cpp",
        query=0,
        stage="impl_capture",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok3, reason3
    ok4, reason4 = write_allowed(
        REPO_ROOT,
        "backends/mysql/api/catalog/query_catalog.hpp",
        query=0,
        stage="impl_runtime",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok4, reason4
    # another engine's backend, the substrate and query directories stay closed
    for rel in ("backends/duckdb/api/register.cpp", "substrate/aux/core/aux_types.hpp",
                "accelerators/tpch/q6/mysql/aux/module.cpp", "workloads/tpch/q6.sql"):
        ok5, reason5 = write_allowed(
            REPO_ROOT, rel, query=0, stage="impl_runtime", write_paths=merged.write_paths + ["**"],
            pipeline_config=merged, benchmark="engine",
        )
        assert not ok5, (rel, reason5)


def test_critique_capture_cannot_write_adapter():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    w = load_workload("engine_postgres", REPO_ROOT)
    merged = apply_workload_overrides(pc, w)
    ok, reason = write_allowed(
        REPO_ROOT,
        "backends/postgres/api/postgres_host.cpp",
        query=0,
        stage="critique_capture",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert not ok
    assert "pipeline/artifacts" in reason
    ok2, reason2 = write_allowed(
        REPO_ROOT,
        "pipeline/artifacts/engines/postgres/layer_critique_capture.json",
        query=0,
        stage="critique_capture",
        write_paths=merged.write_paths,
        pipeline_config=merged,
        benchmark="engine",
    )
    assert ok2, reason2


def test_engine_pipeline_dry_run():
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    run_engine_pipeline(pc, mc, "engine_mysql", 0, {}, dry_run=True, stage="all", skip_gates=True)


def test_embedded_engine_needs_no_docker_host():
    from accel_pipeline.engine_backend.stage import _layer_should_run

    w = load_workload("engine_duckdb", REPO_ROOT)
    assert w["host_runtime"]["kind"] == "host_cmake"
    host_layer = next(layer for layer in implementation_layers(w) if layer["id"] == "host")
    assert not _layer_should_run(w, host_layer)
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    mc = load_model_config(repo_root=REPO_ROOT)
    run_engine_pipeline(pc, mc, "engine_duckdb", 0, {}, dry_run=True, stage="all", skip_gates=True)


def test_run_until_artifact_retries_then_succeeds(tmp_path):
    from accel_pipeline.engine_backend.layer_critique import run_until_artifact

    path = tmp_path / "layer_critique_epochs.json"
    calls: list[str] = []

    def invoke(extra: str):
        calls.append(extra)
        if len(calls) == 2:
            path.write_text('{"status":"pass"}\n', encoding="utf-8")
        return object()

    run_until_artifact(artifact=path, invoke=invoke, attempts=2)
    assert len(calls) == 2
    assert calls[0] == ""
    assert "STOP EXPLORING" in calls[1]
    assert path.is_file()


def test_run_until_artifact_retries_on_invalid_json(tmp_path):
    from accel_pipeline.engine_backend.layer_critique import run_until_artifact

    path = tmp_path / "layer_critique_api.json"
    calls: list[str] = []

    def invoke(extra: str):
        calls.append(extra)
        path.write_text('{"status":"fail"}\n  }\n' if len(calls) == 1 else '{"status":"fail"}\n',
                        encoding="utf-8")
        return object()

    run_until_artifact(artifact=path, invoke=invoke, attempts=2)
    assert len(calls) == 2
    assert "not valid JSON" in calls[1]


def test_layer_critique_feedback_and_load(tmp_path):
    from accel_pipeline.engine_backend.layer_critique import (
        format_layer_critique_feedback,
        load_layer_critique,
    )

    missing = load_layer_critique(tmp_path, "capture")  # recorded, never raised
    assert missing["status"] == "error" and "unavailable" in missing["summary"]
    assert json.loads((tmp_path / "layer_critique_capture.json").read_text())["status"] == "error"
    (tmp_path / "layer_critique_capture.json").write_text(
        json.dumps(
            {
                "engine_name": "postgres",
                "layer": "capture",
                "status": "FAIL",
                "summary": "Invalidate uses caller partition ids",
                "violations": [
                    {
                        "rule": "harness_gap",
                        "evidence": "TryPublish remaps partitions; harness only tests InvalidateAll",
                        "paths": ["backends/postgres/api/postgres_host.cpp"],
                    }
                ],
                "feedback_for_implementation": "Assert Invalidate(PartitionSet) after remap.",
            }
        ),
        encoding="utf-8",
    )
    critique = load_layer_critique(tmp_path, "capture")
    assert critique["status"] == "fail"
    text = format_layer_critique_feedback(critique, round_idx=1)
    assert "Layer critique FAILED" in text
    assert "harness_gap" in text
    assert "Invalidate(PartitionSet)" in text


def test_critique_harness_gaps_aimed_at_the_pipeline_are_dropped(tmp_path):
    from accel_pipeline.engine_backend.layer_critique import layer_critique_path, load_layer_critique

    def critique(violations):
        layer_critique_path(tmp_path, "hostctl").write_text(json.dumps(
            {"status": "fail", "layer": "hostctl", "violations": violations}), encoding="utf-8")
        return load_layer_critique(tmp_path, "hostctl")

    gate_gap = {"rule": "harness_gap", "paths": ["pipeline/gates/run_engine_hostctl.sh", "backends/x/host.sh"]}
    real = {"rule": "host_contract", "paths": ["backends/x/host.sh", "pipeline/prompts/engine_backend/host_contract.md"]}
    only_gate = critique([gate_gap])
    assert only_gate["status"] == "pass" and only_gate["dropped_pipeline_harness_gaps"] == [gate_gap]
    mixed = critique([gate_gap, real])
    assert mixed["status"] == "fail" and mixed["violations"] == [real]
