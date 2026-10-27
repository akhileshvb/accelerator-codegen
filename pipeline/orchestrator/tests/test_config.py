"""The run's settings, and the workload with what derives from it: facts, names, artifact paths and the engine profile."""
from __future__ import annotations

import copy
import json
import os
import re
import subprocess
from pathlib import Path

import pytest

from accel_pipeline.config import (
    _read_workload_yaml,
    analysis_artifact_name,
    analysis_artifact_path,
    aux_smoke_ctest_name,
    derive_query_facts,
    EngineProfileError,
    host_runtime_of,
    load_pipeline_config,
    load_query_analysis,
    load_repo_dotenv,
    load_workload,
    registration_symbol,
    render_engine_profile,
    validate_engine_profile,
    workload_supported_engines,
)
from accel_pipeline.engine_backend.stage import _layer_extra_context, implementation_layers
from accel_pipeline.prompts import render_prompt


def test_load_repo_dotenv_sets_missing_keys(tmp_path: Path):
    env_file = tmp_path / ".env"
    env_file.write_text(
        "OPENROUTER_API_KEY=from-file\n"
        "# comment\n"
        'QUOTED="hello"\n',
        encoding="utf-8",
    )
    os.environ.pop("OPENROUTER_API_KEY", None)
    os.environ.pop("QUOTED", None)
    loaded = load_repo_dotenv(tmp_path)
    assert loaded == env_file
    assert os.environ["OPENROUTER_API_KEY"] == "from-file"
    assert os.environ["QUOTED"] == "hello"


def test_load_repo_dotenv_does_not_override_existing(tmp_path: Path):
    env_file = tmp_path / ".env"
    env_file.write_text("OPENROUTER_API_KEY=from-file\n", encoding="utf-8")
    os.environ["OPENROUTER_API_KEY"] = "from-shell"
    try:
        load_repo_dotenv(tmp_path)
        assert os.environ["OPENROUTER_API_KEY"] == "from-shell"
    finally:
        os.environ.pop("OPENROUTER_API_KEY", None)


REPO_ROOT = Path(__file__).resolve().parents[3]
PIPELINE_ROOT = REPO_ROOT / "pipeline"
ENGINES = ("postgres", "mysql", "duckdb")
LAYERS = ("hostctl", "capture", "epochs", "scan", "runtime", "host", "api")
WORKLOADS = REPO_ROOT / "pipeline" / "config" / "workloads"
GOLDEN = json.loads((Path(__file__).parent / "data" / "workload_facts_golden.json").read_text())
QUERY_WORKLOADS = sorted(
    [p.stem for p in WORKLOADS.glob("*.yaml") if not p.stem.startswith("engine_") and not p.stem.endswith("template")
     and p.stem != "tpch_queries"]
    + [f"tpch_q{n}{kind}" for n in range(1, 23) for kind in ("", "_aux")]
)
# Keys that follow from bench + query + engine, or from the SQL and schema: never hand-written.
DERIVED_KEYS = {
    "query_id", "function_name", "create_fn", "aux_function_name", "accelerator_id",
    "cmake", "reference_accelerators", "seed_manifest_root", "output_schema",
    "dependent_tables", "tables", "engine_name",
}


def _facts(name: str) -> dict:
    raw, _ = _read_workload_yaml(name, REPO_ROOT)
    sql = (REPO_ROOT / raw["reference_sql_path"]).read_text()
    schema = (REPO_ROOT / "workloads" / raw["benchmark_name"] / "schema.sql").read_text()
    return derive_query_facts(sql, schema)


@pytest.mark.parametrize("name", [n for n in QUERY_WORKLOADS if n != "tpch_q21_verus_smoke"])
def test_derived_facts_match_golden(name):
    assert _facts(name) == GOLDEN[name.removesuffix("_aux")]


def test_exists_probe_star_reads_no_columns():
    facts = derive_query_facts(
        "SELECT a FROM t WHERE EXISTS (SELECT * FROM u WHERE u.k = t.k)",
        "CREATE TABLE t (a INTEGER, k INTEGER, z INTEGER); CREATE TABLE u (k INTEGER, y INTEGER);",
    )
    assert facts["dependent_tables"] == [
        {"name": "t", "watched_columns": ["a", "k"]},
        {"name": "u", "watched_columns": ["k"]},
    ]
    assert facts["output_schema"] == [{"name": "a", "type": "INTEGER"}]


@pytest.mark.parametrize("name", QUERY_WORKLOADS)
def test_workload_yaml_holds_only_inputs(name):
    raw, _ = _read_workload_yaml(name, REPO_ROOT)
    assert not DERIVED_KEYS & set(raw)
    assert not Path(raw["reference_sql_path"]).is_absolute()
    assert (REPO_ROOT / raw["reference_sql_path"]).is_file()


def test_names_follow_the_convention_and_the_engine_profile():
    duck = load_workload("tpch_q3_aux", REPO_ROOT, engine="duckdb")
    assert duck["function_name"] == "accel_tpch_q3"
    assert "base_function_name" not in duck  # an aux workload has no stateless kernel to name
    assert duck["aux_function_name"] == "accel_tpch_q3_aux"
    assert duck["accelerator_id"] == "tpch_q3_aux"
    assert duck["create_fn"] == "CreateTpchQ3FunctionSet"
    assert load_workload("sec_edgar_q2", REPO_ROOT)["create_fn"] == "CreateSecEdgarQ2FunctionSet"
    assert load_workload("tpch_q3_aux", REPO_ROOT, engine="postgres")["create_fn"] == ""


def test_naming_functions():
    assert registration_symbol("Create{Bench}Q{N}FunctionSet", "sec_edgar", 24) == "CreateSecEdgarQ24FunctionSet"
    assert aux_smoke_ctest_name("tpch", 6) == "tpch_q6_aux_smoke_test"


def test_tpch_workloads_are_a_template_and_the_query_table():
    engine = workload_supported_engines("tpch_q18", REPO_ROOT)[0]
    aux = load_workload("tpch_q18_aux", REPO_ROOT, engine=engine)
    assert aux["parameters"] == [{"name": "quantity_threshold", "type": "BIGINT"}]
    assert aux["reference_sql_path"].endswith("workloads/tpch/instances/seed_1/18.sql")
    assert aux["optimization"]["max_attempts_per_iteration"] == 8
    base = load_workload("tpch_q18", REPO_ROOT, engine=engine)
    assert "optimization" not in base and "workload_kind" not in base
    assert set(workload_supported_engines("tpch_q18", REPO_ROOT)) < set(workload_supported_engines("tpch_q18_aux", REPO_ROOT))
    with pytest.raises(FileNotFoundError):
        load_workload("tpch_q23", REPO_ROOT)


def test_datasets_are_run_parameters(monkeypatch):
    monkeypatch.delenv("ACCEL_TPCH_DB", raising=False)
    assert "tpch_sf1" not in load_pipeline_config(repo_root=REPO_ROOT).datasets
    monkeypatch.setenv("ACCEL_TPCH_DB", "/data/sf1.duckdb")
    assert load_pipeline_config(repo_root=REPO_ROOT).datasets["tpch_sf1"] == Path("/data/sf1.duckdb")


def test_pipeline_and_workloads_name_no_home_path():
    offenders = []
    tracked = subprocess.run(
        ["git", "ls-files", "pipeline", "workloads"], cwd=REPO_ROOT, capture_output=True, text=True, check=True
    ).stdout.split()
    for path in (REPO_ROOT / rel for rel in tracked):  # local run state (.codex, artifacts) is not the tree's
        if not path.is_file() or path.suffix in {".db", ".duckdb"} or path == Path(__file__).resolve():
            continue
            try:
                text = path.read_text(encoding="utf-8")
            except UnicodeDecodeError:
                continue
            if re.search(r"/home/", text):
                offenders.append(str(path.relative_to(REPO_ROOT)))
    assert not offenders


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
