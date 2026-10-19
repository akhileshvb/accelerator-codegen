from __future__ import annotations

import json
from pathlib import Path

import pytest

from accel_pipeline.config import load_pipeline_config, load_workload
from accel_pipeline.optimization_runtime import (
    build_runtime_state,
    format_runtime_markdown,
    load_episode_log,
    refresh_optimization_runtime,
)


def test_build_runtime_state_includes_profile_guidance():
    repo = Path(__file__).resolve().parents[3]
    art = repo / "pipeline" / "artifacts" / "sec_edgar" / "q1"
    if not art.is_dir():
        pytest.skip("sec_edgar q1 artifacts not present")
    pipeline_config = load_pipeline_config(repo_root=repo)
    workload = load_workload("sec_edgar_q1", repo)
    state = build_runtime_state(pipeline_config, workload, 1, optimization_iter=3, episode=1)
    assert float(state.get("best_speedup_median") or 0) > 0
    assert state.get("profile_guidance")
    assert state.get("investigation_questions") is not None
    md = format_runtime_markdown(state)
    assert "Profile signals" in md
    assert "Investigate" in md


def test_refresh_writes_runtime_files():
    repo = Path(__file__).resolve().parents[3]
    pipeline_config = load_pipeline_config(repo_root=repo)
    workload = load_workload("sec_edgar_q1", repo)
    path = refresh_optimization_runtime(pipeline_config, workload, 1, optimization_iter=2, episode=1)
    assert path.is_file()
    art = repo / "pipeline" / "artifacts" / "sec_edgar" / "q1"
    assert (art / "optimization_runtime.json").is_file()


def test_runtime_markdown_includes_structured_sections(tmp_path: Path):
    art = tmp_path / "art"
    art.mkdir()
    episode_log = art / "episode_log.jsonl"
    episode_log.write_text(
        json.dumps({"episode": 1, "summary": "kept dict-index adsh trial"}) + "\n",
        encoding="utf-8",
    )
    techniques = art / "playbook_techniques.json"
    techniques.write_text(
        json.dumps(
            {
                "techniques": [
                    {
                        "technique_id": "dict_index_adsh",
                        "iteration": 1,
                        "kept": True,
                        "speedup_delta": 0.1,
                        "profile_deltas": {"pre_consume_us": -5000},
                    }
                ]
            }
        ),
        encoding="utf-8",
    )
    state = {
        "optimization_iter": 2,
        "episode": 2,
        "best_speedup_median": 2.0,
        "last_speedup_median": 1.9,
        "speedup_median": 2.0,
        "episode_summaries": load_episode_log(art),
        "playbook_techniques": json.loads(techniques.read_text())["techniques"],
        "optimization_workflow_compact": "# Workflow\ngrep then A/B.",
        "profile_guidance": ["- scan_amplification=3.3x (pre_rows / qualifying_rows)"],
        "investigation_questions": ["grep_context the kernel for per-row loops"],
        "bottleneck_evidence": {
            "tiers": [
                {
                    "tier": "exploit_selectivity",
                    "signals": [{"metric": "scan_amplification", "value": 3.3, "context": "pre_rows / qualifying_rows"}],
                }
            ]
        },
        "ab_trials": [],
        "kernel_paths": {"kernel": "accelerators/sec_edgar/q1/duckdb/base/single-threaded/kernel.cpp"},
    }
    md = format_runtime_markdown(state)
    assert "Episode history" in md
    assert "dict-index adsh" in md
    assert "Technique memory" in md
    assert "Optimization workflow" in md
    assert "Bottleneck evidence" in md
    assert "Investigate" in md
