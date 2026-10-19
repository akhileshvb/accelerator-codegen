from __future__ import annotations

from accel_pipeline.config import apply_workload_overrides, load_pipeline_config, load_workload
from accel_pipeline.context import build_prompt_context
from accel_pipeline.implementation_bootstrap import build_implementation_bootstrap
from accel_pipeline.paths import find_repo_root


REPO_ROOT = find_repo_root()


def test_implementation_bootstrap_lists_q18_files_and_references() -> None:
    import json

    analysis_path = REPO_ROOT / "pipeline" / "artifacts" / "tpch" / "q18" / "analysis.json"
    analysis = json.loads(analysis_path.read_text(encoding="utf-8"))
    workload = load_workload("tpch_q18_aux", REPO_ROOT)
    text = build_implementation_bootstrap(REPO_ROOT, workload, 18, analysis)
    assert "module.hpp" in text
    assert "read_reference_files" in text
    assert "accelerators/tpch/q10/duckdb/aux/module.cpp" in text or "q10" in text


def test_implementation_prompt_includes_bootstrap() -> None:
    from accel_pipeline.prompts import render_prompt

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q18_aux", REPO_ROOT)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    prompt = render_prompt(pipeline_config, "02_implementation.md", workload, 18, "implementation")
    assert "Implementation bootstrap" in prompt
    assert "read_reference_files" in prompt


