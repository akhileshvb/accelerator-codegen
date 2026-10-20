from __future__ import annotations

from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload
from accel_pipeline.context import build_prompt_context
from accel_pipeline.implementation_bootstrap import build_implementation_bootstrap
from accel_pipeline.paths import find_repo_root


REPO_ROOT = find_repo_root()


ANALYSIS = {
    "files_to_create": [
        {"path": "accelerators/tpch/q18/duckdb/aux/module.hpp", "role": "state layout"},
        {"path": "accelerators/tpch/q18/duckdb/aux/module.cpp", "role": "build and execute"},
    ]
}


def test_implementation_bootstrap_lists_q18_files() -> None:
    workload = load_workload("tpch_q18_aux", REPO_ROOT, engine="duckdb")
    text = build_implementation_bootstrap(REPO_ROOT, workload, 18, ANALYSIS, cost_model_config="")
    assert "module.hpp" in text
    assert "Reference aux modules" not in text


def test_implementation_prompt_includes_bootstrap(monkeypatch) -> None:
    from accel_pipeline.prompts import render_prompt

    monkeypatch.setattr(
        "accel_pipeline.context.load_query_analysis",
        lambda *args, **kwargs: (REPO_ROOT / "analysis.json", ANALYSIS),
    )
    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q18_aux", REPO_ROOT, engine="duckdb")
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    prompt = render_prompt(pipeline_config, "02_implementation.md", workload, 18, "implementation")
    assert "Implementation bootstrap" in prompt


