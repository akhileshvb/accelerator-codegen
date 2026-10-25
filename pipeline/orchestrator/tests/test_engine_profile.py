"""The standardized engine profile: schema validation and per-stage rendering."""

from __future__ import annotations

import copy
import re
from pathlib import Path

import pytest

from accel_pipeline.config import load_pipeline_config, load_workload
from accel_pipeline.engine_profile import (
    EngineProfileError,
    host_runtime_of,
    render_engine_profile,
    validate_engine_profile,
)
from accel_pipeline.prompts import render_prompt
from accel_pipeline.engine_backend.stage import _layer_extra_context, implementation_layers

REPO_ROOT = Path(__file__).resolve().parents[3]
PIPELINE_ROOT = REPO_ROOT / "pipeline"
ENGINES = ("postgres", "mysql", "duckdb")
LAYERS = ("hostctl", "capture", "epochs", "scan", "runtime", "host", "api")


def _profile(engine: str = "postgres") -> dict:
    return copy.deepcopy(load_workload(f"engine_{engine}", REPO_ROOT)["engine_profile"])


def _validate(profile) -> None:
    validate_engine_profile(profile, PIPELINE_ROOT, source="test")


@pytest.mark.parametrize("engine", ENGINES)
def test_shipped_profiles_validate(engine):
    _validate(_profile(engine))


def test_all_engines_state_the_same_keys():
    shapes = [{s: sorted(v) if isinstance(v, dict) else "list" for s, v in _profile(e).items()} for e in ENGINES]
    assert shapes[0] == shapes[1] == shapes[2]


def test_template_is_a_complete_profile():
    _validate(load_workload("engine_.template", REPO_ROOT)["engine_profile"])


def test_missing_profile_is_rejected():
    with pytest.raises(EngineProfileError, match="engine_profile is required"):
        validate_engine_profile(None, PIPELINE_ROOT, source="test")


def test_missing_key_is_rejected_and_named():
    profile = _profile()
    del profile["capture"]["abort_signal"]
    with pytest.raises(EngineProfileError, match=r"capture.*abort_signal"):
        _validate(profile)


def test_missing_section_is_rejected():
    profile = _profile()
    del profile["hazards"]
    with pytest.raises(EngineProfileError, match="hazards"):
        _validate(profile)


def test_blank_leaf_is_not_a_decision():
    for blank in (None, "", [], {}):
        profile = _profile()
        profile["scan"]["type_quirks"] = blank
        with pytest.raises(EngineProfileError, match=r"scan\.type_quirks"):
            _validate(profile)


def test_na_needs_a_reason():
    profile = _profile()
    profile["scan"]["type_quirks"] = {"na": ""}
    with pytest.raises(EngineProfileError, match=r"scan\.type_quirks"):
        _validate(profile)
    profile["scan"]["type_quirks"] = {"na": "no engine-specific type facts"}
    _validate(profile)


def test_unknown_key_is_rejected():
    profile = _profile()
    profile["capture"]["advice"] = "do this"
    with pytest.raises(EngineProfileError, match="advice"):
        _validate(profile)


def test_hazard_needs_symptom_fact_and_layers():
    profile = _profile()
    profile["hazards"].append({"symptom": "something breaks", "engine_fact": "the engine does X"})
    with pytest.raises(EngineProfileError, match="layers"):
        _validate(profile)


def test_workload_identity_must_match_engine_name(tmp_path):
    import shutil

    shutil.copytree(PIPELINE_ROOT, tmp_path / "pipeline", ignore=shutil.ignore_patterns("artifacts", "orchestrator"))
    path = tmp_path / "pipeline" / "config" / "workloads" / "engine_mysql.yaml"
    path.write_text(path.read_text().replace("    name: mysql", "    name: other", 1))
    with pytest.raises(EngineProfileError, match="identity.name"):
        load_workload("engine_mysql", tmp_path)


def test_workload_derives_host_and_packaging_from_the_profile():
    pg = load_workload("engine_postgres", REPO_ROOT)
    assert pg["host_runtime"] == host_runtime_of(pg["engine_profile"])
    assert pg["host_runtime"]["kind"] == "docker"
    assert pg["requires_in_process_host"] is True
    assert pg["cmake"]["build_flag"] == "ACCELERATOR_CODEGEN_BUILD_POSTGRES"
    duck = load_workload("engine_duckdb", REPO_ROOT)
    assert duck["host_runtime"]["kind"] == "host_cmake"
    assert "ninja_targets_env" not in duck["host_runtime"]  # n/a leaves are dropped
    assert duck["requires_in_process_host"] is False
    assert duck["display_name"] == "DuckDB"


def test_render_sections_and_na():
    profile = _profile("duckdb")
    text = render_engine_profile(profile, ["capture", "transactions"])
    assert "### capture" in text and "### transactions" in text
    assert "### scan" not in text and "hazards" not in text
    assert "**ddl_events:** n/a (" in text


def test_render_narrows_hazards_to_the_layer():
    profile = _profile("mysql")
    scan = render_engine_profile(profile, ["hazards"], layer="scan")
    host = render_engine_profile(profile, ["hazards"], layer="host")
    everything = render_engine_profile(profile, ["hazards"])
    assert "metadata locks (MDL)" in scan and "metadata locks (MDL)" not in host
    assert everything.count("Engine fact:") == len(profile["hazards"])
    assert scan.count("Engine fact:") < len(profile["hazards"])


def test_embedded_engine_sees_host_hazards_at_runtime():
    profile = _profile("duckdb")
    host_only = [h for h in profile["hazards"] if h["layers"] == ["host"]]
    assert host_only
    plain = render_engine_profile(profile, ["hazards"], layer="runtime")
    folded = render_engine_profile(profile, ["hazards"], layer="runtime", host_layer_runs=False)
    assert all(h["engine_fact"] not in plain and h["engine_fact"] in folded for h in host_only)


def test_every_hazard_names_known_layers_and_each_tagged_layer_has_sections():
    w = load_workload("engine_postgres", REPO_ROOT)
    for engine in ENGINES:
        for hazard in _profile(engine)["hazards"]:
            assert set(hazard["layers"]) <= set(LAYERS), hazard
    assert set(LAYERS) <= set(w["engine_profile_sections"])


def test_hazards_state_facts_not_fixes():
    for engine in ENGINES:
        for hazard in _profile(engine)["hazards"]:
            fact = hazard["engine_fact"].lower()
            assert not re.search(r"\b(should|must|ought|instead|acquire|avoid)\b", fact), hazard


def _render(workload: dict, prompt: str, stage: str, layer: dict | None = None) -> str:
    pc = load_pipeline_config(repo_root=REPO_ROOT)
    extra = _layer_extra_context(workload, layer, REPO_ROOT) if layer else None
    return render_prompt(pc, prompt, workload, 0, stage, extra_context=extra)


@pytest.mark.parametrize("engine", ENGINES)
def test_every_engine_stage_prompt_resolves_and_carries_the_profile(engine):
    w = load_workload(f"engine_{engine}", REPO_ROOT)
    rendered = {"analysis": _render(w, "01_analysis.md", "analysis")}
    rendered["validation"] = _render(w, "03_validation.md", "validation")
    rendered["architecture_audit"] = _render(w, "04_architecture_audit.md", "architecture_audit")
    for layer in implementation_layers(w):
        lid = layer["id"]
        rendered[f"impl_{lid}"] = _render(w, "02_implementation.md", f"impl_{lid}", layer)
        rendered[f"critique_{lid}"] = _render(w, "05_layer_critique.md", f"critique_{lid}", layer)
    for stage, text in rendered.items():
        assert "{{" not in text, f"{engine} {stage}: unresolved token"
        assert "engine_sdk_hints" not in text
        assert "### identity" in text, f"{engine} {stage}: no profile"
    hook = _profile(engine)["capture"]["hook_mechanism"]
    assert hook in rendered["analysis"] and hook in rendered["impl_capture"] and hook in rendered["critique_capture"]


def test_layers_see_only_their_sections():
    w = load_workload("engine_postgres", REPO_ROOT)
    layers = {layer["id"]: layer for layer in implementation_layers(w)}
    capture = _render(w, "02_implementation.md", "impl_capture", layers["capture"])
    host = _render(w, "02_implementation.md", "impl_host", layers["host"])
    assert "### capture" in capture and "### entry_points" not in capture and "### host_runtime" not in capture
    assert "### entry_points" in host and "### capture" not in host
    assert "### oracle" not in capture + host
    assert "### oracle" in _render(w, "03_validation.md", "validation")


def test_layer_rules_are_checklist_ids():
    checklist = (PIPELINE_ROOT / "prompts" / "engine_backend" / "acceptance_checklist.md").read_text()
    ids = set(re.findall(r"^### `(\w+)`", checklist, re.M))
    for layer in implementation_layers(load_workload("engine_postgres", REPO_ROOT)):
        assert set(layer.get("rules") or []) <= ids, layer["id"]
