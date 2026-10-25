"""Saturation floors, profile-guided trials, structural redesigns, the read axes and the frozen-edit notice."""

from __future__ import annotations

import json
import subprocess
from pathlib import Path
from types import SimpleNamespace

import pytest

from accel_pipeline import frozen_paths

from accel_pipeline.optimization import attempt as optimization_attempt

from accel_pipeline.optimization import structural as structural
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

CFG = SimpleNamespace(optimization_early_stop={"enabled": True}, optimization_structural={}, repo_root=Path("."))


def _best_valid(art: Path, read: float) -> None:
    (art / "best_valid_baseline.json").write_text(json.dumps({"score": read, "metrics": {"read_only_query_speedup": read}}))


def _trials(art: Path, *strategies: str) -> None:
    for i, strategy in enumerate(strategies):
        record_trial_protocol(art, trial_id=f"t{i}", hot_spot=f"loop {i}", expected_gain="10%", strategy=strategy)


# --- saturation -----------------------------------------------------------------------------

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


def test_reanalysis_request_is_written_once_when_the_claim_is_honored_for_reanalysis(tmp_path, monkeypatch):
    _best_valid(tmp_path, 3.0)
    _trials(tmp_path, "structural", "low_level", "other")
    written = {}
    monkeypatch.setattr(optimization_attempt, "write_structural_request", lambda *a, **kw: written.update(kw))
    monkeypatch.setattr(optimization_attempt, "log_event", lambda *a, **kw: None)
    monkeypatch.setattr(optimization_attempt, "append_experiment_log", lambda *a, **kw: None)
    monkeypatch.setattr(optimization_attempt, "clear_attempt_checkpoint", lambda art: None)
    monkeypatch.setattr(optimization_attempt, "analysis_artifact_path", lambda *a, **kw: tmp_path / "missing.json")
    cfg = SimpleNamespace(optimization_early_stop={}, optimization_structural={}, repo_root=tmp_path)
    args = dict(pipeline_config=cfg, workload={}, query=3, art=tmp_path, optimization_iter=1, attempt=4, payload={"reason": "stuck"})
    assert optimization_attempt._try_honor_saturation(**args)[0] is True
    assert written["bottleneck_type"] == "residual_read_path" and "3.00x" in written["evidence"]
    written.clear()
    assert optimization_attempt._try_honor_saturation(**args)[0] is True
    assert written == {}  # the marker keeps it to one re-analysis


# --- profile-guided trials ------------------------------------------------------------------

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


# --- scoreboard: the read axis is against the engine's own SQL -------------------------------

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


# --- structural redesigns -------------------------------------------------------------------

REVISION = {"profile_evidence": "read visits 7500 entries", "old_organization": "day map", "new_organization": "bucketed index", "read_work_ratio": "400x"}


def _metrics(read: float, write: float) -> dict:
    return {"read_only_query_speedup": read, "write_mix_workload_speedup": write}


def _outcome(before, after):
    return evaluate_attempt_outcome(
        baseline_score=5.0, candidate_score=6.0, baseline_metrics=before, candidate_metrics=after,
        config=EvalConfig(), correctness_ok=True, structural=True,
    )


def test_structural_attempt_routes_to_the_full_tier(tmp_path, monkeypatch):
    ran = []
    monkeypatch.setattr(structural, "run_full_tier_gates", lambda *a, **kw: ran.append(kw) or "")
    monkeypatch.setattr(optimization_attempt, "run_full_tier_gates", structural.run_full_tier_gates)
    cfg = SimpleNamespace(optimization_structural={}, repo_root=tmp_path)
    aux = {"workload_kind": "aux_state"}
    assert optimization_attempt._structural_failure(cfg, aux, 3, tmp_path, {"strategy": "structural", "design_revision": REVISION}, 2) == (True, "")
    assert ran == [{"iteration": 2}]
    assert optimization_attempt._structural_failure(cfg, aux, 3, tmp_path, {"strategy": "low_level"}, 2) == (False, "")
    assert optimization_attempt._structural_failure(cfg, {}, 3, tmp_path, {"strategy": "structural"}, 2) == (False, "")  # base kernels have no redesign tier


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
    from accel_pipeline import gates

    def fake_run_gate(cfg, name, *a, **kw):
        return SimpleNamespace(name=name, status="fail" if name == "aux_tsan" else "pass", data={"detail": "data race in Execute"})

    monkeypatch.setattr(gates, "run_gate", fake_run_gate)
    error = structural.run_full_tier_gates(SimpleNamespace(optimization_structural={}), {}, 3, iteration=1)
    assert "aux_tsan" in error and "data race" in error  # a non-empty error reverts the attempt


def test_all_gates_pass_runs_the_full_list_in_order(monkeypatch):
    from accel_pipeline import gates

    seen = []
    monkeypatch.setattr(gates, "run_gate", lambda cfg, name, *a, **kw: seen.append(name) or SimpleNamespace(status="pass", data={}))
    assert structural.run_full_tier_gates(SimpleNamespace(optimization_structural={}), {}, 3, iteration=1) == ""
    assert seen == ["aux_correctness", "aux_unit", "aux_tsan", "aux_multi_connection"]


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


# --- frozen-edit notice ---------------------------------------------------------------------

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


# --- agent-written tests and probes ---------------------------------------------------------

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
    from accel_pipeline.build_stamp import is_relevant
    from accel_pipeline.write_ownership import is_owned_path

    assert is_owned_path(tmp_path, "accelerators/toybench/q3/scratch/bench.cpp", query=3, benchmark="toybench")
    assert is_owned_path(tmp_path, "accelerators/toybench/q3/tests/agent_stress.cpp", query=3, benchmark="toybench")
    assert not is_owned_path(tmp_path, "accelerators/toybench/q4/scratch/bench.cpp", query=3, benchmark="toybench")
    query_dir = "accelerators/toybench/q3"
    assert not is_relevant("accelerators/toybench/q3/scratch/bench.cpp", query_dir)
    assert is_relevant("accelerators/toybench/q3/toydb/aux/module.cpp", query_dir)
