from __future__ import annotations

import json

from accel_pipeline.paths import (
    analysis_artifact_name,
    analysis_artifact_path,
    load_query_analysis,
)


def test_analysis_artifact_names_by_workload_kind():
    assert analysis_artifact_name({"workload_kind": "aux_state"}) == "analysis.json"
    assert analysis_artifact_name({"benchmark_name": "tpch"}) == "base_analysis.json"
    assert analysis_artifact_name(None) == "base_analysis.json"


def test_base_and_aux_analysis_paths_do_not_collide(tmp_path):
    workload_aux = {"workload_kind": "aux_state", "benchmark_name": "tpch", "engine_name": "postgres"}
    workload_base = {"benchmark_name": "tpch", "engine_name": "postgres"}
    art = tmp_path / "pipeline" / "artifacts" / "tpch" / "q18" / "postgres"
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


def test_each_engine_has_its_own_artifact_tree(tmp_path):
    workloads = {e: {"workload_kind": "aux_state", "benchmark_name": "tpch", "engine_name": e} for e in ("a", "b")}
    paths = {e: analysis_artifact_path(tmp_path, 3, workload=w) for e, w in workloads.items()}
    assert paths["a"].parent == tmp_path / "pipeline" / "artifacts" / "tpch" / "q3" / "a"
    assert paths["a"].parent != paths["b"].parent
