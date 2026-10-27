"""What earlier attempts teach: the record store, lessons, saturation claims, the scoreboard, the digest the agent reads."""

from __future__ import annotations

import json
import os
import time
from pathlib import Path
from types import SimpleNamespace

from accel_pipeline.config import load_pipeline_config, load_workload
from accel_pipeline.optimization import memory
from accel_pipeline.optimization.bench import write_aux_profile_artifacts
from accel_pipeline.optimization.memory import (
    HONOR,
    REANALYSIS_MARKER_FILE,
    REANALYZE,
    REJECT,
    _read_axis_lines,
    advise,
    aux_profile_digest,
    build_scoreboard,
    design_revision_error,
    design_revisions_markdown,
    diagnose,
    judge_saturation_claim,
    lessons,
    lessons_markdown,
    mark_profile_run,
    merge_playbook_proposal,
    parse_optimization_saturated,
    precedents,
    precedents_markdown,
    profile_freshness,
    profile_guidance_error,
    recall,
    record_attempt,
    record_design_revision,
    record_trial,
    refresh_optimization_runtime,
    remember,
    render_digest,
    review_kernel,
    signals_of,
    write_scoreboard,
)


EARLY_STOP = {"enabled": True, "saturation_required_attempts": 3}


CFG = SimpleNamespace(optimization_early_stop=EARLY_STOP, optimization_structural={}, repo_root=Path("."))


def _best_valid(art: Path, read: float) -> None:
    (art / "best_valid_baseline.json").write_text(json.dumps({"score": read, "metrics": {"read_only_query_speedup": read}}))


def _trials(art: Path, *strategies: str) -> None:
    for i, strategy in enumerate(strategies):
        record_trial(art, trial_id=f"t{i}", hot_spot=f"loop {i}", expected_gain="10%", strategy=strategy)


def test_saturation_rejected_below_floor_names_exactly_what_is_missing(tmp_path):
    _best_valid(tmp_path, 1.06)
    _trials(tmp_path, "other")
    verdict, detail, _ = judge_saturation_claim(CFG, tmp_path, {"reason": "stuck"})
    assert verdict is REJECT
    assert "1.06x" in detail and "20x floor" in detail
    assert "2 more profile-guided trial(s): only 1 of the 3 required" in detail
    assert "one trial with strategy=structural" in detail
    assert "one trial with strategy=low_level" in detail
    assert "redesign it in this stage" in detail  # points at the structural-redesign option


def test_saturation_rejected_when_trials_repeat_the_same_target(tmp_path):
    _best_valid(tmp_path, 2.0)
    for i in range(3):
        record_trial(tmp_path, trial_id=f"t{i}", hot_spot="same loop", expected_gain="5%", strategy="structural")
    verdict, detail, _ = judge_saturation_claim(CFG, tmp_path, None)
    assert verdict is REJECT
    assert "distinct strategies" in detail
    assert "one trial with strategy=low_level" in detail and "one trial with strategy=structural" not in detail


def test_saturation_triggers_one_structural_reanalysis_then_is_honored(tmp_path):
    _best_valid(tmp_path, 3.0)
    _trials(tmp_path, "structural", "low_level", "other")
    verdict, detail, _ = judge_saturation_claim(CFG, tmp_path, {"reason": "stuck"})
    assert verdict is REANALYZE and "3.00x" in detail
    (tmp_path / REANALYSIS_MARKER_FILE).write_text(detail)
    verdict, detail, _ = judge_saturation_claim(CFG, tmp_path, {"reason": "stuck"})
    assert verdict is HONOR and "already spent" in detail


def test_saturation_above_the_floor_still_needs_the_trials(tmp_path):
    # the clean Q3 run claimed saturation at 292x after five trials, before a build win could be kept
    _best_valid(tmp_path, 292.0)
    verdict, detail, _ = judge_saturation_claim(CFG, tmp_path, None)
    assert verdict is REJECT and "not saturated yet" in detail and "3 more profile-guided trial(s)" in detail
    _trials(tmp_path, "structural", "low_level", "other")
    assert judge_saturation_claim(CFG, tmp_path, None)[0] is HONOR
    other = tmp_path / "base"
    other.mkdir()
    _trials(other, "structural", "low_level", "other")
    assert judge_saturation_claim(CFG, other, None)[0] is HONOR


def test_trial_needs_a_fresh_profile_and_a_named_target(tmp_path):
    assert "run_profile" in profile_guidance_error(tmp_path, hot_spot="x", expected_gain="y", strategy="other")
    mark_profile_run(tmp_path)
    assert "hot_spot" in profile_guidance_error(tmp_path, hot_spot="", expected_gain="y", strategy="other")
    assert "strategy" in profile_guidance_error(tmp_path, hot_spot="x", expected_gain="y", strategy="magic")
    assert profile_guidance_error(tmp_path, hot_spot="x", expected_gain="y", strategy="low_level") == ""


def test_profile_is_stale_after_best_valid_moves(tmp_path):
    mark_profile_run(tmp_path)
    marker = tmp_path / "last_profile_run"
    _best_valid(tmp_path, 2.0)
    best = tmp_path / "best_valid_baseline.json"
    os.utime(best, (marker.stat().st_mtime + 5, marker.stat().st_mtime + 5))
    assert "stale" in profile_guidance_error(tmp_path, hot_spot="x", expected_gain="y", strategy="other")


def test_build_scoreboard_carries_the_read_axis_and_runtime_flags_the_floor(tmp_path):
    metrics = {"read_only_query_speedup": 10.0}
    (tmp_path / "best_valid_baseline.json").write_text(json.dumps({"metrics": metrics}))
    board = build_scoreboard(tmp_path, is_aux=True, last_metrics=metrics)
    assert board["read_speedup"]["best_valid"] == {"vs_engine": 10.0}
    lines = "\n".join(_read_axis_lines(board, 20.0))
    assert "10.00x vs the engine's own SQL" in lines and "READ FLOOR NOT MET" in lines
    assert "base kernel" not in lines


REVISION = {"profile_evidence": "read visits 7500 entries", "old_organization": "day map", "new_organization": "bucketed index", "read_work_ratio": "400x"}


def _metrics(read: float, write: float) -> dict:
    return {"read_only_query_speedup": read, "write_mix_workload_speedup": write}


def test_a_kept_redesign_is_recorded_for_the_audit(tmp_path):
    before, after = _metrics(2.0, 3.0), _metrics(40.0, 3.0)
    record_design_revision(tmp_path, attempt_id="iter1_a3", hypothesis={"design_revision": REVISION}, before=before, after=after)
    (record,) = recall(tmp_path, "revision")
    assert record["new_organization"] == "bucketed index" and record["measured"]["read_speedup_after"] == 40.0
    block = design_revisions_markdown(tmp_path)
    assert "day map -> bucketed index" in block and "supersede the analysis" in block
    assert design_revision_error({"design_revision": REVISION}) == ""
    assert "design_revision" in design_revision_error({"strategy": "structural"})


def test_parse_optimization_saturated_with_json():
    text = """
Done optimizing for now.

OPTIMIZATION_SATURATED

```json
{"status": "optimization_saturated", "reason": "stuck after aggressive pass", "remaining_headroom": "low"}
```
"""
    payload = parse_optimization_saturated(text)
    assert payload is not None
    assert payload["status"] == "optimization_saturated"
    assert "stuck" in payload["reason"]


def test_a_saturation_claim_with_the_required_trials_is_honored_without_numeric_floors(tmp_path: Path):
    _trials(tmp_path, "structural", "low_level", "other")
    verdict, detail, _ = judge_saturation_claim(
        SimpleNamespace(optimization_early_stop=EARLY_STOP), tmp_path, {"reason": "stuck around 16x; no further dual-axis movement"})
    assert verdict == HONOR and "stuck" in detail


def test_early_stop_can_be_disabled(tmp_path: Path):
    verdict, detail, _ = judge_saturation_claim(SimpleNamespace(optimization_early_stop={"enabled": False}), tmp_path, {"reason": "stuck"})
    assert verdict == REJECT and "disabled" in detail


def test_scoreboard_prefers_best_valid(tmp_path: Path):
    (tmp_path / "best_valid_baseline.json").write_text(
        json.dumps(
            {
                "attempt_id": "iter1_a1",
                "score": 4.405,
                "metrics": {
                    "read_only_query_speedup": 4.4,
                    "accel_hit_median_us": 9287.64,
                },
            }
        ),
        encoding="utf-8",
    )
    (tmp_path / "profile_summary.json").write_text(
        json.dumps(
            {
                "read_only_query_speedup": 2.0,
                "accel_hit_median_us": 20000.0,
            }
        ),
        encoding="utf-8",
    )
    board = build_scoreboard(tmp_path, is_aux=True)
    write_scoreboard(tmp_path, board)
    assert board["compare_against"] == "best_valid_score"
    # Recomputed from metrics (no write-mix → read_only alone).
    assert board["best_valid_score"] == 4.4
    assert board["score_metric"].startswith("dual_axis_weighted")
    assert board["best_valid_attempt_id"] == "iter1_a1"
    assert board["last_measured_score"] == 2.0
    assert board["improvement_pct_vs_best_valid"] < 0
    assert (tmp_path / "optimization_scoreboard.json").is_file()


def test_scoreboard_dual_axis_blends_write_mix(tmp_path: Path):
    (tmp_path / "best_valid_baseline.json").write_text(
        json.dumps(
            {
                "attempt_id": "iter1_a2",
                "score": 9000.0,  # stale absolute score; must recompute
                "metrics": {
                    "read_only_query_speedup": 9000.0,
                    "write_mix_workload_speedup": 0.37,
                    "accel_hit_median_us": 8.0,
                },
            }
        ),
        encoding="utf-8",
    )
    board = build_scoreboard(tmp_path, is_aux=True)
    expected = (9000.0**0.75) * (0.37**0.25)
    assert abs(float(board["best_valid_score"]) - expected) < 1e-6
    assert float(board["best_valid_score"]) < 9000.0


def test_profile_freshness_detects_newer_sources(tmp_path: Path):
    repo = tmp_path / "repo"
    src = repo / "src" / "module.cpp"
    src.parent.mkdir(parents=True)
    src.write_text("// v1\n", encoding="utf-8")
    art = tmp_path / "art"
    art.mkdir()
    (art / "profile_summary.json").write_text("{}", encoding="utf-8")
    time.sleep(0.05)
    src.write_text("// v2\n", encoding="utf-8")
    freshness = profile_freshness(
        art, repo_root=repo, editable_rel_paths=["src/module.cpp"]
    )
    assert freshness["stale"] is True
    assert any("editable sources newer" in r for r in freshness["reasons"])


def _store(tmp_path, monkeypatch):
    repo = tmp_path / "repo"
    (repo / "pipeline").mkdir(parents=True)
    monkeypatch.setattr(memory, "pipeline_root", lambda root: root / "pipeline")
    return repo


def _attempt(repo, art, workload, verdict, hypothesis, improvement_pct, metrics=None, query=5):
    return record_attempt(art, repo_root=repo, workload=workload, query=query, attempt=1, hypothesis=hypothesis, verdict=verdict,
                          reason="below_threshold", improvement_pct=improvement_pct, metrics=metrics)


def test_a_clear_reject_becomes_a_lesson_a_mild_one_does_not(tmp_path, monkeypatch):
    repo, art = _store(tmp_path, monkeypatch), tmp_path / "art"
    workload = {"workload_kind": "aux_state", "engine_name": "duckdb"}
    seeded = lessons(repo, "duckdb", set(), limit=20)
    assert len(seeded) >= 4 and all(row["source"] == "seed" for row in seeded)

    _attempt(repo, art, workload, "reject_regress", {"bottleneck": "tiny read shuffle"}, -2.0)
    assert len(lessons(repo, "duckdb", set(), limit=50)) == len(seeded)  # noise is no lesson

    hard = {"bottleneck": "hit-path hashmap materialize", "why": "hit 120ms"}
    _attempt(repo, art, workload, "reject_regress", hard, -40.0)
    _attempt(repo, art, workload, "reject_regress", hard, -41.0)  # the same approach again: not stored twice
    learned = [r for r in lessons(repo, "duckdb", set(), limit=50) if r["source"] == "orchestrator"]
    assert [r["lesson"] for r in learned] == ["Avoid repeating failed approach: hit-path hashmap materialize"]
    assert learned[0]["query"] == 5 and "Δ=-40.0%" in learned[0]["when"]
    assert len(recall(art, "attempt")) == 3  # every judged attempt is in the query's own store

    md = lessons_markdown(lessons(repo, "duckdb", set()))
    assert md[0].startswith("## Negative memory") and "advisory" in md[0]
    assert any("Q5 Avoid repeating failed approach" in line for line in md)


def test_lessons_are_per_engine(tmp_path, monkeypatch):
    repo, art = _store(tmp_path, monkeypatch), tmp_path / "art"
    _attempt(repo, art, {"engine_name": "postgres"}, "reject_broken", {"bottleneck": "rebuild the whole hash table on every point write"}, None, query=1)
    assert any("rebuild the whole hash table" in r["lesson"] for r in lessons(repo, "postgres", set(), limit=50))
    assert not any("rebuild the whole hash table" in r["lesson"] for r in lessons(repo, "mysql", set(), limit=50))


def test_lessons_are_retrieved_by_the_evidence_they_share(tmp_path, monkeypatch):
    repo, art = _store(tmp_path, monkeypatch), tmp_path / "art"
    workload = {"engine_name": "duckdb"}
    cliff = {"read_only_query_speedup": 30.0, "write_mix_workload_speedup": 0.4}
    assert signals_of(cliff) >= {"write_mix_cliff", "read_strong"}
    _attempt(repo, art, workload, "reject_broken", {"bottleneck": "cliff approach"}, None, metrics=cliff)
    for i in range(10):  # newer, unrelated lessons crowd it out of a recency-only list
        _attempt(repo, art, workload, "reject_broken", {"bottleneck": f"unrelated approach {i}"}, None, metrics={"read_only_query_speedup": 1.0})
    texts = [r["lesson"] for r in lessons(repo, "duckdb", signals_of(cliff), limit=5)]
    assert any("cliff approach" in t for t in texts) and any("RebuildAll" in t for t in texts)


def test_precedents_are_ranked_by_shared_signals(tmp_path):
    (tmp_path / "pipeline" / "catalog").mkdir(parents=True)
    (tmp_path / "pipeline" / "catalog" / "aux_transformations.yaml").write_text(
        "- {id: scalar_trick, source_query: 6, problem: scalar aggregate output, transformation: t1}\n"
        "- {id: fanout_trick, source_query: 7, problem: multi-partition fanout on reads, transformation: t2}\n")
    (top,) = precedents(tmp_path, {"high_partition_read_fanout"}, limit=3)
    assert top["id"] == "fanout_trick" and top["retrieval_score"] == 1.0
    assert [c["id"] for c in precedents(tmp_path, set())] == ["scalar_trick"]  # no evidence: the catalog's first entry, as a menu
    assert "fanout_trick" in precedents_markdown([top])


PROFILE_SUMMARY = {
    "source": "profile_gate",
    "speedup_median": 2.26,
    "phases_us": {"pre_scan_us": 46293.0, "pre_consume_us": 142962.0, "finalize_us": 16.0},
    "counters": {"qualifying_rows": 9599726, "pre_rows": 9599726, "output_groups": 14, "distinct_inserts": 379461},
    "raw": {"label": "profile", "reference_median_us": 432572.5, "accelerator_median_us": 191552.5,
            "speedup_median": 2.26, "pre_scan_us": 46293.0, "pre_consume_us": 142962.0, "finalize_us": 16.0,
            "qualifying_rows": 9599726, "pre_rows": 9599726},
}


def _runtime_world(tmp_path, monkeypatch):
    repo = Path(__file__).resolve().parents[3]
    art = tmp_path / "art"
    art.mkdir()
    monkeypatch.setattr(memory, "query_artifacts_dir", lambda root, q, workload=None: art)
    monkeypatch.setattr(memory, "shared_store_path", lambda root, engine: tmp_path / "shared.jsonl")
    return art, load_pipeline_config(repo_root=repo), load_workload("sec_edgar_q1", repo)


def test_the_digest_reads_the_measurements_and_what_was_tried(tmp_path, monkeypatch):
    art, cfg, workload = _runtime_world(tmp_path, monkeypatch)
    (art / "optimization_state.json").write_text(json.dumps({"best_speedup_median": 2.26, "iterations": []}))
    (art / "profile_summary.json").write_text(json.dumps(PROFILE_SUMMARY))
    (art / "baseline_summary.json").write_text(json.dumps(PROFILE_SUMMARY["raw"]))
    remember(art, "technique", technique_id="dict_index_adsh", iteration=1, kept=True, speedup_delta=0.1, profile_deltas={"pre_consume_us": -5000})
    record_attempt(art, repo_root=tmp_path, workload=workload, query=1, attempt=2, hypothesis={"bottleneck": "fuse the consume pass"},
                   verdict="accept", accepted=True, kept=True, improvement_pct=4.5)
    state = advise(cfg, workload, 1, optimization_iter=3, episode=2)
    assert float(state["best"]) > 0 and state["profile_lines"] and state["questions"]
    md = render_digest(state)
    for section in ("Profile signals", "Investigate", "Attempt history", "fuse the consume pass", "Technique memory", "dict_index_adsh",
                    "Technique palette", "Negative memory", "Optimization workflow", "Scale invariance"):
        assert section in md


def test_refresh_writes_the_digest_and_its_json(tmp_path, monkeypatch):
    art, cfg, workload = _runtime_world(tmp_path, monkeypatch)
    path = refresh_optimization_runtime(cfg, workload, 1, optimization_iter=2, episode=1)
    assert path.is_file() and (art / "optimization_runtime.json").is_file()


def test_the_digest_flags_a_missed_read_floor_and_a_write_cliff():
    state = {"optimization_iter": 2, "episode": 1, "workload_kind": "aux_state", "best": 10.0, "last": 10.0, "current": 10.0,
             "read_floor": 20.0, "write_mix_pct": 10, "write_mix_workload_speedup": 0.5,
             "scoreboard": {"read_speedup": {"best_valid": {"vs_engine": 10.0}, "last_measured": {"vs_engine": 10.0}}},
             "kernel_paths": {"kernel": "accelerators/b/q1/kernel.cpp"}}
    md = render_digest(state)
    assert "READ FLOOR NOT MET" in md and "WRITE-MIX CLIFF" in md and "`accelerators/b/q1/kernel.cpp`" in md


def test_the_techniques_the_profile_points_at_lead_the_palette():
    diagnosis = diagnose({"speedup_median": 0.8}, {"phases_us": {"num_scan_us": 100.0, "pre_consume_us": 60.0},
                                                    "counters": {"pre_rows": 1000, "qualifying_rows": 100}})
    palette = diagnosis["strategy_palette"]
    assert palette[0].startswith("early_filter_pushdown ★") and any(item.startswith("fused_consume_single_pass ★") for item in palette)
    assert any("scan_amplification" in sig for sig in diagnosis["profile_signals"])
    assert diagnosis["lacking"] and not diagnosis["beats_duckdb"]


def test_the_kernel_review_finds_per_row_string_hashing(tmp_path):
    kernel = tmp_path / "kernel.cpp"
    kernel.write_text("std::unordered_map<std::string, double> sums;\nvoid ProcessNum(int row) {\n  sums.find(key);\n  ToStoredString(v);\n}\n")
    ids = {s["id"] for s in review_kernel(kernel)["smells"]}
    assert {"string_keyed_hash_map", "map_find_in_hot_path", "heap_string_in_hot_path"} <= ids
    assert review_kernel(tmp_path / "missing.cpp")["status"] == "missing"


def test_reported_techniques_become_records(tmp_path: Path):
    repo = tmp_path / "repo"
    art = repo / "pipeline" / "artifacts" / "sec_edgar" / "q1"
    art.mkdir(parents=True)
    workload = {"artifact_namespace": "sec_edgar"}
    (art / "playbook_proposal.json").write_text(json.dumps({"iteration": 2, "summary": "Dict-index adsh helped consume.", "techniques": [
        {"technique_id": "dict_index_adsh", "hypothesis": "track adsh by dictionary child index", "kept": True,
         "speedup_delta": 0.15, "profile_deltas": {"pre_consume_us": -12000}}]}), encoding="utf-8")
    assert merge_playbook_proposal(repo, 1, workload=workload) is True
    (record,) = recall(art, "technique")
    assert record["technique_id"] == "dict_index_adsh" and record["iteration"] == 2 and record["kept"] is True


def test_write_and_digest_aux_profile(tmp_path: Path):
    metrics = {
        "read_only_query_speedup": 3.47,
        "composite_score": 3.56,
        "accel_hit_median_us": 11491.0,
        "accel_hit_rate": 1.0,
        "valid_partitions": 51,
        "partitions_read_per_query_median": 9.0,
        "store_diagnostics": {
            "accel_hit_median_us": 11491.0,
            "accel_hit_rate": 1.0,
            "valid_partitions": 51,
            "partitions_read_per_query_median": 9.0,
        },
    }
    summary = write_aux_profile_artifacts(
        tmp_path,
        metrics,
        source="test",
        bench_output='ACCEL_CUSTOM_PROFILE {"decode_us":100.0}\n',
    )
    assert summary["workload_kind"] == "aux_state"
    assert summary["read_only_query_speedup"] == 3.47
    assert summary["dominant_phase"] in {"accel_hit_us", "decode", "decode_us"}
    assert (tmp_path / "profile_summary.json").is_file()
    assert (tmp_path / "baseline_summary.json").is_file()

    (tmp_path / "aux_bench_summary.json").write_text(
        json.dumps({"read_only_query_speedup": 3.47, "composite_score": 3.56}),
        encoding="utf-8",
    )
    digest = aux_profile_digest(tmp_path)
    assert digest["profile_summary"]["read_only_query_speedup"] == 3.47
    assert digest["fair_sweep"]["read_only_query_speedup"] == 3.47
    assert digest["store_diagnostics"]["accel_hit_median_us"] == 11491.0
    assert "custom_profiling" in digest
    assert digest["scoreboard"]["compare_against"] == "best_valid_score"
