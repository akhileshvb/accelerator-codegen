"""The aux-state prompts and the shared docs they cite name no benchmark, query or schema of their own."""

from __future__ import annotations

import re
from pathlib import Path

PROMPTS = Path(__file__).resolve().parents[2] / "prompts"
TPCH = (r"tpc-h|tpch|lineitem|l_shipdate|o_orderdate|c_mktsegment|furniture|"
        r"\bo_order\w*|\bl_(?:extendedprice|quantity|returnflag|discount)\b")
SEC_EDGAR = r"sec[-_ ]?edgar|filing_append|value_append|\badsh\b|\bcik\b"
BANNED = re.compile(rf"{TPCH}|{SEC_EDGAR}|\bQ\d+\b", re.I)
ALLOWED: set[tuple[str, str]] = set()


def scoped_files() -> list[Path]:
    return sorted([*(PROMPTS / "aux_state").rglob("*.md"), *(PROMPTS / "shared").glob("*.md")])


def test_prompts_and_shared_docs_name_no_benchmark_specifics():
    hits = []
    for path in scoped_files():
        rel = str(path.relative_to(PROMPTS))
        for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            for m in BANNED.finditer(line):
                if (rel, m.group(0).lower()) not in ALLOWED:
                    hits.append(f"{rel}:{n}: {m.group(0)}")
    assert not hits, "\n".join(hits)


def test_rendered_analysis_prompt_of_the_non_tpch_workload_has_no_tpch_text():
    from accel_pipeline.config import load_pipeline_config
    from accel_pipeline.workload import load_workload
    from accel_pipeline.prompts import render_prompt

    repo = Path(__file__).resolve().parents[3]
    workload = load_workload("sec_edgar_q1_aux", repo)
    text = render_prompt(load_pipeline_config(repo_root=repo), "01_analysis.md", workload, 1, "analysis")
    assert not re.findall(TPCH, text, re.I)  # its own schema and query number are its inputs


def test_the_rendered_notes_of_any_workload_come_from_its_own_settings():
    """benchmark_notes once carried SEC-EDGAR facts (a scale, write suites, a partition scheme) for its runs only:
    now two workloads' notes differ only in their own values."""
    from accel_pipeline.config import load_pipeline_config
    from accel_pipeline.workload import load_workload, resolve_workload_paths, workload_supported_engines
    from accel_pipeline.prompts import build_prompt_context

    repo = Path(__file__).resolve().parents[3]
    cfg = load_pipeline_config(repo_root=repo)
    notes = []
    for name in ("sec_edgar_q4_aux", "tpch_q14_aux"):
        workload = load_workload(name, repo, engine=workload_supported_engines(name, repo)[0])
        text = build_prompt_context(cfg, workload, int(workload["query_number"]), "implementation")["benchmark_notes"]
        for own in resolve_workload_paths(cfg, workload, int(workload["query_number"])).values():
            text = text.replace(f"`{own}`", "`<own>`") if own else text
        notes.append(text)
    assert notes[0] == notes[1], notes
