"""The audit's deterministic inputs (digest, cumulative diff) and its one batched repair round."""

from __future__ import annotations

import json
import subprocess
from types import SimpleNamespace

import pytest

from accel_pipeline import audit_digest
from accel_pipeline.audit_digest import (
    audit_context, blocking_findings, changed_files, cumulative_diff, public_symbols, query_digest,
    record_implementation_base, snapshot, touched_since,
)
from accel_pipeline.config import QuickTierConfig
from accel_pipeline.failure import Failure
from accel_pipeline.repair import StageStopped
from accel_pipeline.stages import audit as audit_stage

GIT = ["git", "-c", "user.name=t", "-c", "user.email=t@t"]
WORKLOAD = {"workload_kind": "aux_state", "benchmark_name": "demo", "engine_name": "eng"}
QDIR = "accelerators/demo/q5"

HEADER = """// State of the demo accelerator
#pragma once
namespace demo {
struct State {
  int hidden_member();
};
class Module {
 public:
  void Build(const Table &t);
};
std::shared_ptr<State> BuildState(const Table &t, int64_t rows);
inline int Helper(int x) { return x + 1; }
}  // namespace demo
"""


@pytest.fixture()
def repo(tmp_path):
    root = tmp_path / "repo"
    (root / QDIR / "tests").mkdir(parents=True)
    (root / QDIR / "module.hpp").write_text(HEADER)
    (root / QDIR / "module.cpp").write_text("// implementation\nint x;\n")
    (root / QDIR / "tests" / "unit.cpp").write_text("int t;\n")
    (root / "substrate").mkdir()
    (root / "substrate" / "shared.hpp").write_text("// shared\n")
    (root / "pipeline" / "artifacts" / "demo" / "q5").mkdir(parents=True)
    (root / ".gitignore").write_text("pipeline/artifacts/\n")
    subprocess.run(["git", "init", "-q"], cwd=root, check=True)
    subprocess.run([*GIT, "add", "-A"], cwd=root, check=True)
    subprocess.run([*GIT, "commit", "-qm", "base"], cwd=root, check=True)
    return root


@pytest.fixture()
def config(repo, monkeypatch):
    art = repo / "pipeline" / "artifacts" / "demo" / "q5"
    monkeypatch.setattr(audit_digest, "query_artifacts_dir", lambda root, q, workload=None: art)
    monkeypatch.setattr(audit_digest, "gates_dir_for_query", lambda a: a / "gates")
    monkeypatch.setattr(audit_digest, "analysis_artifact_path", lambda root, q, workload=None: art / "analysis.json")
    return SimpleNamespace(repo_root=repo, quick_tier=QuickTierConfig()), art


def test_public_symbols_are_the_namespace_level_declarations():
    assert public_symbols(HEADER) == [
        "type State", "type Module",
        "std::shared_ptr<State> BuildState(const Table &t, int64_t rows)",
        "inline int Helper(int x) { return x + 1; }",
    ]


def test_the_digest_maps_files_entry_points_design_log_and_open_findings(config):
    cfg, art = config
    (art / "analysis.json").write_text(json.dumps({
        "correctness_facts": {"state_design": {"selected_candidate_id": "c2", "physical_organization": "hash by key"}},
        "files_to_create": [{"path": "module.cpp", "role": "build and execute"}],
    }))
    (art / "experiment_log.md").write_text("- impl: one\n- repair[implementation]: two\n")
    (art / "quarantine.json").write_text(json.dumps([{"gate": "aux_unit", "infra_reason": "harness_error", "stage": "audit"}]))
    (art / "gates").mkdir()
    (art / "gates" / "aux_correctness.json").write_text('{"status": "fail"}')
    digest = query_digest(cfg, WORKLOAD, 5)
    assert "`module.hpp` (13 lines): State of the demo accelerator" in digest
    assert "`module.cpp` (2 lines): build and execute" in digest
    assert "`tests/unit.cpp` (1 lines): tests" in digest
    assert "`module.hpp`: `type State`" in digest and "BuildState(const Table &t, int64_t rows)" in digest
    assert "selected candidate: `c2`" in digest and "physical_organization: hash by key" in digest
    assert "2 lines, 1 repair decisions" in digest
    assert "gate `aux_unit` quarantined" in digest and "gate `aux_correctness` last result: fail" in digest
    assert digest == query_digest(cfg, WORKLOAD, 5)  # deterministic


def test_files_in_the_gates_directory_that_are_not_gate_results_are_ignored(config):
    cfg, art = config
    (art / "gates").mkdir()
    (art / "gates" / "aux_bench_draws.json").write_text("[1, 2, 3]")
    assert "none recorded" in query_digest(cfg, WORKLOAD, 5)


def test_the_digest_without_an_analysis_or_log_still_renders(config):
    cfg, _ = config
    digest = query_digest(cfg, WORKLOAD, 5)
    assert "(no analysis on disk)" in digest and "(no experiment log)" in digest and "none recorded" in digest


def test_the_cumulative_diff_covers_edits_and_new_files_since_the_base(config, repo):
    cfg, art = config
    record_implementation_base(repo, art)
    (repo / QDIR / "module.cpp").write_text("// implementation\nint y;\n")
    (repo / QDIR / "new.cpp").write_text("int fresh;\n")
    (repo / "substrate" / "shared.hpp").write_text("// shared, edited\n")
    (repo / "pipeline" / "x.py").parent.mkdir(exist_ok=True)
    (repo / "pipeline" / "x.py").write_text("ignored\n")
    context = audit_context(cfg, WORKLOAD, 5)
    assert "3 files" in context
    assert "-int x;" in context and "+int y;" in context and "+int fresh;" in context and "shared, edited" in context
    assert "x.py" not in context
    assert context.index("module.cpp") < context.index("substrate/shared.hpp")  # the query's own files first


def test_the_diff_limit_lists_what_no_longer_fits(repo):
    (repo / QDIR / "module.cpp").write_text("// implementation\n" + "int big;\n" * 400)
    (repo / QDIR / "tests" / "unit.cpp").write_text("int small;\n")
    diff = cumulative_diff(repo, "HEAD", [f"{QDIR}/tests/unit.cpp", f"{QDIR}/module.cpp"], limit=500)
    assert "+int small;" in diff and "module.cpp: diff omitted" in diff


def test_touched_since_names_the_files_a_round_changed(repo):
    base = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo, capture_output=True, text=True).stdout.strip()
    (repo / QDIR / "module.cpp").write_text("// implementation\nint y;\n")
    before = snapshot(repo, changed_files(repo, base, QDIR))
    (repo / QDIR / "module.cpp").write_text("// implementation\nint z;\n")
    (repo / QDIR / "added.cpp").write_text("int a;\n")
    assert touched_since(repo, before, base, QDIR) == [f"{QDIR}/added.cpp", f"{QDIR}/module.cpp"]


def test_blocking_findings_are_the_verdicts_the_audit_says_must_change():
    assert blocking_findings({}) == []
    clean = {"correctness_ok": True, "scale_review": {"risks": ["slow at SF3000"], "large_scale": [
        {"class": "overflow", "result": "pass"}, {"class": "x", "result": "n/a"}]},
        "fairness_review": {"cheating_suspected": False, "issues": ["noted"]}}
    assert blocking_findings(clean) == []
    bad = {"correctness_ok": False, "scale_review": {
        "generality_failures": ["NULL params crash (module.cpp:10)"],
        "cow_findings": [{"file": "module.cpp", "line": 40}],
        "large_scale": [{"class": "overflow", "result": "fail", "evidence": "module.cpp:5"}]},
        "fairness_review": {"cheating_suspected": True, "issues": ["result cache"]}}
    found = blocking_findings(bad)
    assert len(found) == 5
    assert any("NULL params" in f for f in found) and any("result cache" in f for f in found)


class Round:
    """Stand-ins for the audit turn and the repair turn; the audit result moves as the script says."""

    def __init__(self, ctx, results):
        self.ctx, self.results, self.audit_messages, self.repairs = ctx, list(results), [], []

    def audit_turn(self, message):
        self.audit_messages.append(message)
        (self.ctx.art / "audit_result.json").write_text(json.dumps(self.results.pop(0)))

    def repair(self, ctx, failure, *, must_change=None):
        self.repairs.append((failure, must_change))


@pytest.fixture()
def stage_ctx(repo, monkeypatch):
    art = repo / "pipeline" / "artifacts" / "demo" / "q5"
    ctx = SimpleNamespace(
        pipeline_config=SimpleNamespace(repo_root=repo), workload=WORKLOAD, query=5, art=art, log=lambda line: None)
    monkeypatch.setattr(audit_stage, "append_experiment_log", lambda *a, **k: None)
    return ctx


BLOCKED = {"correctness_ok": False, "scale_review": {"generality_failures": ["dates: Feb 29"]}}


def test_blocking_findings_go_to_the_main_session_in_one_round_then_the_audit_rechecks_only_them(stage_ctx, monkeypatch):
    rnd = Round(stage_ctx, [{"correctness_ok": True}])
    (stage_ctx.art / "audit_result.json").write_text(json.dumps(BLOCKED))
    monkeypatch.setattr(audit_stage, "repair_turn", rnd.repair)
    audit_stage._fix_blocking_findings(stage_ctx, rnd.audit_turn)
    (failure, must_change), = rnd.repairs
    assert isinstance(failure, Failure) and failure.kind == "code" and must_change is False
    assert "dates: Feb 29" in failure.evidence and "correctness_ok=false" in failure.evidence
    assert len(rnd.audit_messages) == 1 and "Re-check only these findings" in rnd.audit_messages[0]
    assert not (stage_ctx.art / "audit_open_findings.md").exists()


GENERALITY_ONLY = {"correctness_ok": True, "scale_review": {"generality_failures": ["dates: Feb 29"]}}


def test_findings_that_survive_the_recheck_are_recorded_not_looped_on(stage_ctx, monkeypatch):
    rnd = Round(stage_ctx, [GENERALITY_ONLY])
    (stage_ctx.art / "audit_result.json").write_text(json.dumps(BLOCKED))
    monkeypatch.setattr(audit_stage, "repair_turn", rnd.repair)
    audit_stage._fix_blocking_findings(stage_ctx, rnd.audit_turn)
    assert len(rnd.repairs) == 1 and len(rnd.audit_messages) == 1
    assert "dates: Feb 29" in (stage_ctx.art / "audit_open_findings.md").read_text()


def test_a_surviving_correctness_finding_stops_the_stage(stage_ctx, monkeypatch):
    stage_ctx.stage, stage_ctx.change_kind, stage_ctx.iteration = "audit", "", 0
    rnd = Round(stage_ctx, [BLOCKED])
    (stage_ctx.art / "audit_result.json").write_text(json.dumps(BLOCKED))
    monkeypatch.setattr(audit_stage, "repair_turn", rnd.repair)
    with pytest.raises(StageStopped):
        audit_stage._fix_blocking_findings(stage_ctx, rnd.audit_turn)
    assert "correctness_ok=false" in (stage_ctx.art / "stop_report.md").read_text()


def test_a_clean_audit_has_no_repair_round(stage_ctx, monkeypatch):
    rnd = Round(stage_ctx, [])
    (stage_ctx.art / "audit_result.json").write_text(json.dumps({"correctness_ok": True}))
    monkeypatch.setattr(audit_stage, "repair_turn", rnd.repair)
    audit_stage._fix_blocking_findings(stage_ctx, rnd.audit_turn)
    assert rnd.repairs == [] and rnd.audit_messages == []
