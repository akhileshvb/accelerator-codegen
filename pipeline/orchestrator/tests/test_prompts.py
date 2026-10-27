"""Every stage prompt a run can render is complete, the analysis prompt names what the validator requires, and the
keys it renders with (backend API index, engine profile, reference-SQL predicate checklist) are right."""

from __future__ import annotations

import re
from dataclasses import replace
from pathlib import Path

import pytest

from accel_pipeline import preflight
from accel_pipeline.config import (
    apply_workload_overrides,
    load_workload,
    workload_supported_engines,
    load_pipeline_config,
)
from accel_pipeline.prompts import (
    ContractRole,
    extract_predicate_checklist,
    format_predicate_bootstrap_section,
    parse_contract_roles,
    render_accelerator_engine_profile,
    render_backend_api,
    render_prompt,
)
from accel_pipeline.validation import STATE_STORAGE_TEXT_FIELDS

PIPELINE_ROOT = Path(__file__).resolve().parents[2]
REPO = Path(__file__).resolve().parents[3]
RUNS = [("tpch_q14_aux", engine) for engine in workload_supported_engines("tpch_q14_aux", REPO)] + [
    ("sec_edgar_q1_aux", workload_supported_engines("sec_edgar_q1_aux", REPO)[0])]


def _render(name: str, engine: str, prompt: str, stage: str) -> str:
    cfg = load_pipeline_config(repo_root=REPO)
    workload = load_workload(name, REPO, engine=engine)
    cfg = replace(apply_workload_overrides(cfg, workload), engine_name=engine)
    return render_prompt(cfg, prompt, workload, int(workload["query_number"]), stage)


@pytest.mark.parametrize("name,engine", RUNS)
def test_every_stage_prompt_renders_with_every_path_it_names_existing(name, engine):
    problems = []
    for stage, prompt in preflight.PROMPTS.items():
        problems += [f"{stage}: {p}" for p in preflight.prompt_problems(_render(name, engine, prompt, stage), REPO)]
    assert not problems, problems


@pytest.mark.parametrize("name,engine", RUNS)
def test_the_analysis_prompt_names_the_fields_the_validator_requires(name, engine):
    text = _render(name, engine, "01_analysis.md", "analysis")
    required = ("units_probe", "read_work_ratio", "state_storage", *STATE_STORAGE_TEXT_FIELDS)  # candidate fields come from validate_analysis
    missing = [f for f in required if not re.search(rf"\b{f}\b", text)]
    assert not missing, missing


def test_aux_prompts_name_no_benchmark_of_their_own():
    """The aux prompts once carried TPC-H column names, which misled the agent on other schemas (sec_edgar)."""
    banned = re.compile(r"tpc-h|tpch|lineitem|l_shipdate|o_orderdate|c_mktsegment|furniture|\bo_order\w*|\bl_(?:extendedprice|quantity|returnflag|discount)\b", re.I)
    text = _render("sec_edgar_q1_aux", workload_supported_engines("sec_edgar_q1_aux", REPO)[0], "01_analysis.md", "analysis")
    assert not banned.findall(text)


README = """# toy api

## Contract roles

| role | headers | symbols |
|---|---|---|
| registration | `api/accelerator_api.hpp`, `api/toy_registry.hpp` | `RegisterQuery`, `QueryRegistrar` |
| scan | `api/toy_scan.hpp` | `ScanTable` |
| maintenance | | |

| later | `api/x.hpp` | `Ignored` |
"""


def _write_readme(root: Path, engine: str, text: str = README) -> None:
    api = root / "backends" / engine / "api"
    api.mkdir(parents=True)
    (api / "README.md").write_text(text, encoding="utf-8")


def test_parse_reads_only_the_first_table():
    assert parse_contract_roles(README) == [
        ContractRole("registration", ("api/accelerator_api.hpp", "api/toy_registry.hpp"), ("RegisterQuery", "QueryRegistrar")),
        ContractRole("scan", ("api/toy_scan.hpp",), ("ScanTable",)),
        ContractRole("maintenance", (), ()),
    ]


def test_parse_without_heading_is_empty():
    assert parse_contract_roles("| role | headers | symbols |\n|---|---|---|\n") == []


def test_render_lists_roles_with_engine_paths_and_na(tmp_path):
    _write_readme(tmp_path, "toy")
    out = render_backend_api(tmp_path, "toy")
    assert "- **registration:** `backends/toy/api/accelerator_api.hpp`, `backends/toy/api/toy_registry.hpp`; symbols `RegisterQuery`, `QueryRegistrar`" in out
    assert "- **maintenance:** n/a" in out
    assert "Ignored" not in out


def test_render_differs_only_by_manifest(tmp_path):
    _write_readme(tmp_path, "a")
    _write_readme(tmp_path, "b", README.replace("toy_scan", "b_scan"))
    a, b = render_backend_api(tmp_path, "a"), render_backend_api(tmp_path, "b")
    assert a.replace("backends/a/", "backends/b/") != b
    assert a.replace("backends/a/", "backends/b/").replace("toy_scan", "b_scan") == b


def test_render_without_manifest_says_so(tmp_path):
    assert "absent" in render_backend_api(tmp_path, "none")


def test_profile_rendered_for_shipped_engines_and_empty_for_unknown():
    out = render_accelerator_engine_profile(PIPELINE_ROOT, "postgres")
    assert "### hazards" in out and "### scan" in out
    assert render_accelerator_engine_profile(PIPELINE_ROOT, "nope") == ""


Q16_SQL = """
SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM partsupp, part
WHERE p_partkey = ps_partkey
  AND p_brand <> 'Brand#13'
  AND p_type NOT LIKE 'SMALL BRUSHED%'
  AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33)
  AND ps_suppkey NOT IN (
    SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
GROUP BY p_brand, p_type, p_size
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size;
"""


def test_q16_checklist_covers_polarity():
    checks = extract_predicate_checklist(Q16_SQL)
    joined = "\n".join(checks)
    assert "NOT LIKE" in joined
    assert "Inequality" in joined
    assert "Anti-join" in joined or "complaint" in joined.lower()
    assert "Hot-path" in joined


def test_bootstrap_section_mentions_contract(tmp_path):
    sql_path = tmp_path / "16.sql"
    sql_path.write_text(Q16_SQL, encoding="utf-8")
    section = format_predicate_bootstrap_section(Q16_SQL, str(sql_path))
    assert "unit_test_contract.md" in section
    assert "NOT LIKE" in section
    assert "Brand#13" in section or "<>" in section
