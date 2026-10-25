"""The optimization stage's decisions: attempt verdicts, scores, saturation, scoreboard, structural redesigns."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from types import SimpleNamespace
import pytest
from accel_pipeline import frozen_paths
from accel_pipeline.optimization import attempt as optimization_attempt
from accel_pipeline.optimization import structural
from accel_pipeline.optimization.eval import EvalConfig, evaluate_attempt_outcome
from accel_pipeline.optimization.saturation import (
    REANALYSIS_MARKER_FILE,
    Verdict,
    judge_saturation_claim,
    mark_profile_run,
    profile_guidance_error,
    record_trial_protocol,
)
from accel_pipeline.optimization.scoreboard import build_scoreboard, read_axes
from accel_pipeline.optimization.runtime import _read_axis_lines
from accel_pipeline.optimization.eval import improvement_ratio
from accel_pipeline.optimization.eval import aux_optimization_score
from accel_pipeline.optimization.saturation import (
    evaluate_saturation_claim,
    parse_optimization_saturated,
)
import time
from accel_pipeline.optimization.scoreboard import (
    profile_freshness,
    write_scoreboard,
)


CFG = SimpleNamespace(optimization_early_stop={"enabled": True}, optimization_structural={}, repo_root=Path("."))
def _best_valid(art: Path, read: float) -> None:
    (art / "best_valid_baseline.json").write_text(json.dumps({"score": read, "metrics": {"read_only_query_speedup": read}}))
def _trials(art: Path, *strategies: str) -> None:
    for i, strategy in enumerate(strategies):
        record_trial_protocol(art, trial_id=f"t{i}", hot_spot=f"loop {i}", expected_gain="10%", strategy=strategy)
def test_saturation_rejected_below_floor_names_exactly_what_is_missing(tmp_path):
    _best_valid(tmp_path, 1.06)
    _trials(tmp_path, "other")
    verdict, detail, _ = judge_saturation_claim(CFG, tmp_path, {"reason": "stuck"})
    assert verdict is Verdict.REJECT
    assert "1.06x" in detail and "20x floor" in detail
    assert "2 more profile-guided trial(s): only 1 of the 3 required" in detail
    assert "one trial with strategy=structural" in detail
    assert "one trial with strategy=low_level" in detail
    assert "redesign it in this stage" in detail  # points at the structural-redesign option
def test_saturation_rejected_when_trials_repeat_the_same_target(tmp_path):
    _best_valid(tmp_path, 2.0)
    for i in range(3):
        record_trial_protocol(tmp_path, trial_id=f"t{i}", hot_spot="same loop", expected_gain="5%", strategy="structural")
    verdict, detail, _ = judge_saturation_claim(CFG, tmp_path, None)
    assert verdict is Verdict.REJECT
    assert "distinct strategies" in detail
    assert "one trial with strategy=low_level" in detail and "one trial with strategy=structural" not in detail
def test_saturation_triggers_one_structural_reanalysis_then_is_honored(tmp_path):
    _best_valid(tmp_path, 3.0)
    _trials(tmp_path, "structural", "low_level", "other")
    verdict, detail, _ = judge_saturation_claim(CFG, tmp_path, {"reason": "stuck"})
    assert verdict is Verdict.REANALYZE and "3.00x" in detail
    (tmp_path / REANALYSIS_MARKER_FILE).write_text(detail)
    verdict, detail, _ = judge_saturation_claim(CFG, tmp_path, {"reason": "stuck"})
    assert verdict is Verdict.HONOR and "already spent" in detail
def test_saturation_honored_at_or_above_floor_and_without_a_read_axis(tmp_path):
    _best_valid(tmp_path, 24.0)
    assert judge_saturation_claim(CFG, tmp_path, None)[0] is Verdict.HONOR
    other = tmp_path / "base"
    other.mkdir()
    assert judge_saturation_claim(CFG, other, None)[0] is Verdict.HONOR
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
    import os
    os.utime(best, (marker.stat().st_mtime + 5, marker.stat().st_mtime + 5))
    assert "stale" in profile_guidance_error(tmp_path, hot_spot="x", expected_gain="y", strategy="other")
def test_read_axis_is_the_speedup_over_the_engine():
    assert read_axes({"read_only_query_speedup": 11.6}) == {"vs_engine": 11.6}
    assert read_axes({}) == {"vs_engine": None}
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
def _outcome(before, after):
    return evaluate_attempt_outcome(
        baseline_score=5.0, candidate_score=6.0, baseline_metrics=before, candidate_metrics=after,
        config=EvalConfig(), correctness_ok=True, structural=True,
    )
def test_structural_is_inferred_from_the_rewritten_lines(tmp_path):
    root, art = tmp_path / "repo", tmp_path / "art"
    (art / "attempt_checkpoint" / "src").mkdir(parents=True)
    (root / "src").mkdir(parents=True)
    (art / "attempt_checkpoint" / "src" / "m.cpp").write_text("\n".join(f"line {i}" for i in range(100)))
    (root / "src" / "m.cpp").write_text("\n".join(f"line {i}" for i in range(100)))
    cfg = SimpleNamespace(optimization_structural={}, repo_root=root)
    assert not structural.is_structural_attempt(cfg, art, {"strategy": "low_level"})
    (root / "src" / "m.cpp").write_text("\n".join(f"line {i}" for i in range(60)) + "\n" + "\n".join(f"new {i}" for i in range(50)))
    assert structural.is_structural_attempt(cfg, art, {"strategy": "low_level"})
def test_a_structural_attempt_without_its_design_record_fails_before_the_gates(tmp_path, monkeypatch):
    monkeypatch.setattr(optimization_attempt, "run_full_tier_gates", lambda *a, **kw: pytest.fail("gates must not run"))
    cfg = SimpleNamespace(optimization_structural={}, repo_root=tmp_path)
    structural_, error = optimization_attempt._structural_failure(cfg, {"workload_kind": "aux_state"}, 3, tmp_path, {"strategy": "structural"}, 1)
    assert structural_ and "design_revision" in error
def test_incorrect_redesign_is_rolled_back(monkeypatch):
    from accel_pipeline.gates import run as gates

    def fake_run_gate(cfg, name, *a, **kw):
        return SimpleNamespace(name=name, status="fail" if name == "aux_tsan" else "pass", data={"detail": "data race in Execute"})

    monkeypatch.setattr(gates, "run_gate", fake_run_gate)
    error = structural.run_full_tier_gates(SimpleNamespace(optimization_structural={}), {}, 3, iteration=1)
    assert "aux_tsan" in error and "data race" in error  # a non-empty error reverts the attempt
def test_faster_passing_redesign_is_kept_and_recorded(tmp_path):
    before, after = _metrics(2.0, 3.0), _metrics(40.0, 3.0)
    assert _outcome(before, after).verdict == "accept"
    structural.record_design_revision(tmp_path, attempt_id="iter1_a3", hypothesis={"design_revision": REVISION}, before=before, after=after)
    record = json.loads((tmp_path / structural.DESIGN_REVISIONS_FILE).read_text())
    assert record["new_organization"] == "bucketed index" and record["measured"]["read_speedup_after"] == 40.0
    block = structural.design_revisions_markdown(tmp_path)
    assert "day map -> bucketed index" in block and "supersede the analysis" in block
def test_correct_but_slower_redesign_is_rolled_back():
    outcome = _outcome(_metrics(20.0, 3.0), _metrics(20.2, 3.0))
    assert outcome.verdict == "reject_regress" and "structural_read_not_improved" in outcome.reason
def test_redesign_that_regresses_writes_is_rolled_back():
    outcome = _outcome(_metrics(20.0, 3.0), _metrics(60.0, 1.0))
    assert outcome.verdict == "reject_regress" and "structural_write_regressed" in outcome.reason
def test_structural_attempt_never_gets_keep_explore():
    # Diagnostics that would earn a micro-optimization keep_explore do not keep a redesign.
    before = {**_metrics(20.0, 3.0), "partitions_read_per_query_median": 100.0}
    after = {**_metrics(30.0, 3.0), "partitions_read_per_query_median": 10.0}
    outcome = evaluate_attempt_outcome(
        baseline_score=5.0, candidate_score=5.0, baseline_metrics=before, candidate_metrics=after,
        config=EvalConfig(), correctness_ok=True, structural=True,
    )
    assert outcome.verdict != "keep_explore"
def test_frozen_edit_notice_names_the_edited_pipeline_file(tmp_path):
    from accel_pipeline.session import _frozen_edit_notice

    def git(*args):
        subprocess.run(["git", "-C", str(tmp_path), *args], check=True, capture_output=True)

    (tmp_path / "pipeline" / "gates").mkdir(parents=True)
    gate = tmp_path / "pipeline" / "gates" / "run_x.sh"
    gate.write_text("echo ok\n")
    git("init", "-q")
    git("-c", "user.email=a@b", "-c", "user.name=t", "add", "-A")
    git("-c", "user.email=a@b", "-c", "user.name=t", "commit", "-qm", "init")
    cfg = SimpleNamespace(repo_root=tmp_path)
    assert frozen_paths.edited(tmp_path) == [] and _frozen_edit_notice(cfg) == ""
    gate.write_text("echo changed\n")
    notice = _frozen_edit_notice(cfg)
    assert "pipeline/gates/run_x.sh" in notice and "report_blocker" in notice and "not yours" in notice
def test_agent_sources_get_a_build_target_automatically(tmp_path):
    root = tmp_path / "src"
    for rel in ("scratch/bench_reads.cpp", "tests/agent_stress.cpp", "tests/test_module.cpp", "bench/other.cpp"):
        path = root / "accelerators" / "b" / "q7" / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("int main() {}\n")
    script = tmp_path / "entries.cmake"
    script.write_text(
        f'include("{Path(__file__).resolve().parents[3] / "cmake" / "AgentExecutables.cmake"}")\n'
        f'accel_agent_executable_entries(entries "{root}")\n'
        'foreach(e IN LISTS entries)\n  message("ENTRY ${e}")\nendforeach()\n'
    )
    out = subprocess.run(["cmake", "-P", str(script)], capture_output=True, text=True, check=True).stderr
    targets = sorted(line.split("|")[0].removeprefix("ENTRY ") for line in out.splitlines() if line.startswith("ENTRY"))
    assert targets == ["agent_b_q7_bench_reads", "agent_b_q7_stress"]  # module tests and benches keep their own wiring
def test_agent_scratch_is_writable_and_invisible_to_gates_and_audit(tmp_path):
    from accel_pipeline.gates.build_stamp import is_relevant
    from accel_pipeline.write_ownership import is_owned_path

    assert is_owned_path(tmp_path, "accelerators/toybench/q3/scratch/bench.cpp", query=3, benchmark="toybench")
    assert is_owned_path(tmp_path, "accelerators/toybench/q3/tests/agent_stress.cpp", query=3, benchmark="toybench")
    assert not is_owned_path(tmp_path, "accelerators/toybench/q4/scratch/bench.cpp", query=3, benchmark="toybench")
    query_dir = "accelerators/toybench/q3"
    assert not is_relevant("accelerators/toybench/q3/scratch/bench.cpp", query_dir)
    assert is_relevant("accelerators/toybench/q3/toydb/aux/module.cpp", query_dir)


def test_accept_on_read_improvement():
    cfg = EvalConfig(improvement_threshold_pct=3.0, reject_threshold_pct=2.0)
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=11.0,
        baseline_metrics={"read_only_query_speedup": 10.0, "valid_partitions": 1000},
        candidate_metrics={"read_only_query_speedup": 11.0, "valid_partitions": 1000, "accel_hit_rate": 1.0},
        config=cfg,
        correctness_ok=True,
    )
    assert outcome.verdict == "accept"
def test_keep_explore_on_structural_progress():
    cfg = EvalConfig(improvement_threshold_pct=3.0, reject_threshold_pct=2.0)
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=10.1,
        baseline_metrics={
            "read_only_query_speedup": 10.0,
            "valid_partitions": 10000,
            "validity_slots_scanned_median": 10000,
            "accel_hit_rate": 1.0,
        },
        candidate_metrics={
            "read_only_query_speedup": 10.1,
            "valid_partitions": 1000,
            "validity_slots_scanned_median": 1000,
            "accel_hit_rate": 1.0,
        },
        config=cfg,
        correctness_ok=True,
        structural_progress_threshold_pct=5.0,
    )
    assert outcome.verdict == "keep_explore"
def test_reject_broken_on_failed_correctness():
    cfg = EvalConfig()
    outcome = evaluate_attempt_outcome(
        baseline_score=10.0,
        candidate_score=0.0,
        baseline_metrics={},
        candidate_metrics={},
        config=cfg,
        correctness_ok=False,
    )
    assert outcome.verdict == "reject_broken"


def test_improvement_ratio():
    assert improvement_ratio(1.0, 1.05) == pytest.approx(5.0)
    assert improvement_ratio(0.0, 1.0) == 100.0


def test_aux_score_uses_hit_time_when_read_only_zero():
    slow = {"read_only_query_speedup": 0.0, "accel_hit_median_us": 20000.0}
    fast = {"read_only_query_speedup": 0.0, "accel_hit_median_us": 10000.0}
    slow_score = aux_optimization_score(slow)
    fast_score = aux_optimization_score(fast)
    assert fast_score > slow_score
    assert improvement_ratio(slow_score, fast_score) > 0
def test_aux_score_dual_axis_weights_write_less_than_read():
    base = {
        "read_only_query_speedup": 9000.0,
        "write_mix_workload_speedup": 0.37,
    }
    base_score = aux_optimization_score(base)

    # Same write-mix, modest read gain — positive but limited.
    read_gain = aux_optimization_score(
        {"read_only_query_speedup": 10000.0, "write_mix_workload_speedup": 0.37}
    )
    read_imp = improvement_ratio(base_score, read_gain)

    # Same read, write-mix 0.37 → 2.0 — larger dual-axis lift than the read-only bump.
    write_gain = aux_optimization_score(
        {"read_only_query_speedup": 9000.0, "write_mix_workload_speedup": 2.0}
    )
    write_imp = improvement_ratio(base_score, write_gain)
    assert write_imp > read_imp

    # Read up but write cliff deepens — net regress.
    cliff = aux_optimization_score(
        {"read_only_query_speedup": 9500.0, "write_mix_workload_speedup": 0.15}
    )
    assert improvement_ratio(base_score, cliff) < 0
def test_aux_score_reads_write_mix_from_by_freq():
    metrics = {
        "read_only_query_speedup": 100.0,
        "by_write_frequency_pct": {"10": {"workload_speedup": 4.0}},
    }
    expected = (100.0**0.75) * (4.0**0.25)
    assert abs(aux_optimization_score(metrics) - expected) < 1e-6
def test_aux_score_geo_means_multiple_write_mixes():
    metrics = {
        "read_only_query_speedup": 16.0,
        "by_write_frequency_pct": {
            "0": {"write_frequency_pct": 0, "workload_speedup": 20.0},
            "10": {"write_frequency_pct": 10, "workload_speedup": 4.0},
            "50": {"write_frequency_pct": 50, "workload_speedup": 1.0},
        },
    }
    write_mix = (4.0 * 1.0) ** 0.5
    expected = (16.0**0.75) * (write_mix**0.25)
    assert abs(aux_optimization_score(metrics) - expected) < 1e-6


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
def test_evaluate_honors_agent_without_numeric_floors(tmp_path: Path):
    art = tmp_path / "art"
    art.mkdir()
    cfg = SimpleNamespace(optimization_early_stop={"enabled": True})
    ok, detail, _ = evaluate_saturation_claim(
        cfg,
        {"workload_kind": "aux_state", "name": "tpch_q16_aux"},
        art,
        {"reason": "stuck around 16x; no further dual-axis movement"},
    )
    assert ok
    assert "16x" in detail or "stuck" in detail
def test_evaluate_respects_disabled(tmp_path: Path):
    art = tmp_path / "art"
    art.mkdir()
    cfg = SimpleNamespace(optimization_early_stop={"enabled": False})
    ok, detail, _ = evaluate_saturation_claim(
        cfg,
        {"workload_kind": "aux_state"},
        art,
        {"reason": "stuck"},
    )
    assert not ok
    assert "disabled" in detail


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
