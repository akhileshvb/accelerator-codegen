from __future__ import annotations

import json

from accel_pipeline.paths import (
    analysis_artifact_name,
    analysis_artifact_path,
    load_query_analysis,
    resolve_analysis_artifact_path,
)


def test_analysis_artifact_names_by_workload_kind():
    assert analysis_artifact_name({"workload_kind": "aux_state"}) == "analysis.json"
    assert analysis_artifact_name({"benchmark_name": "tpch"}) == "base_analysis.json"
    assert analysis_artifact_name(None) == "base_analysis.json"


def test_base_and_aux_analysis_paths_do_not_collide(tmp_path):
    workload_aux = {"workload_kind": "aux_state", "benchmark_name": "tpch"}
    workload_base = {"benchmark_name": "tpch"}
    art = tmp_path / "pipeline" / "artifacts" / "tpch" / "q18"
    art.mkdir(parents=True)

    aux_path = art / "analysis.json"
    base_path = art / "base_analysis.json"
    aux_path.write_text(json.dumps({"fitness_assessment": {"recommended": True}}), encoding="utf-8")
    base_path.write_text(json.dumps({"logical_plan": {"steps": []}}), encoding="utf-8")

    _, aux_analysis = load_query_analysis(tmp_path, 18, workload=workload_aux)
    _, base_analysis = load_query_analysis(tmp_path, 18, workload=workload_base)

    assert "fitness_assessment" in aux_analysis
    assert "logical_plan" in base_analysis
    assert analysis_artifact_path(tmp_path, 18, workload=workload_aux) == aux_path
    assert analysis_artifact_path(tmp_path, 18, workload=workload_base) == base_path


def test_legacy_base_analysis_fallback(tmp_path):
    art = tmp_path / "pipeline" / "artifacts" / "tpch" / "q18"
    art.mkdir(parents=True)
    legacy = art / "analysis.json"
    legacy.write_text(json.dumps({"logical_plan": {}, "scan_order": ["lineitem"]}), encoding="utf-8")

    resolved = resolve_analysis_artifact_path(art, {"benchmark_name": "tpch"})
    assert resolved == legacy
