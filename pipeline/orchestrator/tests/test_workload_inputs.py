"""Workload inputs: derived facts, derived names, repo-relative paths."""

from __future__ import annotations

import json
import re
import subprocess
from pathlib import Path

import pytest

from accel_pipeline import naming
from accel_pipeline.config import _read_workload_yaml, load_pipeline_config, load_workload, workload_supported_engines
from accel_pipeline.workload_facts import derive_query_facts

REPO_ROOT = Path(__file__).resolve().parents[3]
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
    assert naming.registration_symbol("Create{Bench}Q{N}FunctionSet", "sec_edgar", 24) == "CreateSecEdgarQ24FunctionSet"
    assert naming.aux_smoke_ctest_name("tpch", 6) == "tpch_q6_aux_smoke_test"


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
