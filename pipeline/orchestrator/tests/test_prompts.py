"""Every stage prompt a run can render is complete, and the analysis prompt names what the validator requires."""

from __future__ import annotations

import re
from dataclasses import replace
from pathlib import Path

import pytest

from accel_pipeline import preflight
from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload, workload_supported_engines
from accel_pipeline.prompts import render_prompt
from accel_pipeline.validation import STATE_STORAGE_TEXT_FIELDS

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
