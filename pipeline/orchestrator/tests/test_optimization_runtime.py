from __future__ import annotations

import json
from pathlib import Path


from accel_pipeline.config import load_pipeline_config, load_workload
from accel_pipeline.optimization_runtime import (
    build_runtime_state,
    format_runtime_markdown,
    load_episode_log,
    refresh_optimization_runtime,
)


PROFILE_SUMMARY = {
    "source": "profile_gate",
    "speedup_median": 2.26,
    "phases_us": {"pre_scan_us": 46293.0, "pre_consume_us": 142962.0, "finalize_us": 16.0},
    "counters": {"qualifying_rows": 9599726, "pre_rows": 9599726, "output_groups": 14, "distinct_inserts": 379461},
    "raw": {"label": "profile", "reference_median_us": 432572.5, "accelerator_median_us": 191552.5,
            "speedup_median": 2.26, "pre_scan_us": 46293.0, "pre_consume_us": 142962.0, "finalize_us": 16.0,
            "qualifying_rows": 9599726, "pre_rows": 9599726},
}


def test_build_runtime_state_includes_profile_guidance(tmp_path, monkeypatch):
    """Runs on artifacts written here, never on whatever a developer's earlier runs left in the tree."""
    repo = Path(__file__).resolve().parents[3]
    art = tmp_path / "art"
    art.mkdir()
    (art / "optimization_state.json").write_text(json.dumps({"best_speedup_median": 2.26, "iterations": []}))
    (art / "profile_summary.json").write_text(json.dumps(PROFILE_SUMMARY))
    monkeypatch.setattr("accel_pipeline.optimization_runtime.query_artifacts_dir", lambda root, q, workload=None: art)
    pipeline_config = load_pipeline_config(repo_root=repo)
    workload = load_workload("sec_edgar_q1", repo)
    state = build_runtime_state(pipeline_config, workload, 1, optimization_iter=3, episode=1)
    assert float(state.get("best_speedup_median") or 0) > 0
    assert state.get("profile_guidance")
    assert state.get("investigation_questions") is not None
    md = format_runtime_markdown(state)
    assert "Profile signals" in md
    assert "Investigate" in md


def test_refresh_writes_runtime_files(tmp_path, monkeypatch):
    repo = Path(__file__).resolve().parents[3]
    art = tmp_path / "art"
    art.mkdir()
    monkeypatch.setattr("accel_pipeline.optimization_runtime.query_artifacts_dir", lambda root, q, workload=None: art)
    pipeline_config = load_pipeline_config(repo_root=repo)
    workload = load_workload("sec_edgar_q1", repo)
    path = refresh_optimization_runtime(pipeline_config, workload, 1, optimization_iter=2, episode=1)
    assert path.is_file()
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
        "investigation_questions": ["Read the kernel for per-row loops"],
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
