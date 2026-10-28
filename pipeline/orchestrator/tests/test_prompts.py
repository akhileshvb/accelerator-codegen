"""Every stage prompt a run can render is complete, the analysis prompt names what the validator requires, and the
keys it renders with (backend API index, engine profile, reference-SQL predicate checklist) are right; the slices
of the analysis a stage is handed, and the audit's deterministic inputs (digest, cumulative diff) with its one
batched repair round."""
from __future__ import annotations

import json
import re
import subprocess
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest

from accel_pipeline import prompts, cli, stages as audit_stage
from accel_pipeline.config import (
    apply_workload_overrides,
    load_pipeline_config,
    load_workload,
    QuickTierConfig,
    workload_supported_engines,
)
from accel_pipeline.repair import Failure, StageStopped
from accel_pipeline.prompts import (
    analysis_for_audit,
    analysis_for_implementation,
    analysis_for_optimization,
    audit_context,
    blocking_findings,
    changed_files,
    ContractRole,
    extract_predicate_checklist,
    format_predicate_bootstrap_section,
    parse_contract_roles,
    query_digest,
    record_implementation_base,
    render_accelerator_engine_profile,
    render_backend_api,
    render_prompt,
    snapshot,
    touched_since,
)
from accel_pipeline.data import STATE_STORAGE_TEXT_FIELDS


PIPELINE_ROOT = Path(__file__).resolve().parents[2]
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
    for stage, prompt in cli.PROMPTS.items():
        problems += [f"{stage}: {p}" for p in cli.prompt_problems(_render(name, engine, prompt, stage), REPO)]
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


README = """# toy api

## Contract roles

| role | headers | symbols |
|---|---|---|
| registration | `api/accelerator_api.hpp`, `api/toy_registry.hpp` | `RegisterQuery`, `QueryRegistrar` |
| scan | `api/toy_scan.hpp` | `ScanTable` |
| maintenance | | |

| later | `api/x.hpp` | `Ignored` |
"""


def _write_readme(root: Path, engine: str, text: str = README) -> None:
    api = root / "backends" / engine / "api"
    api.mkdir(parents=True)
    (api / "README.md").write_text(text, encoding="utf-8")


def test_parse_reads_only_the_first_table():
    assert parse_contract_roles(README) == [
        ContractRole("registration", ("api/accelerator_api.hpp", "api/toy_registry.hpp"), ("RegisterQuery", "QueryRegistrar")),
        ContractRole("scan", ("api/toy_scan.hpp",), ("ScanTable",)),
        ContractRole("maintenance", (), ()),
    ]


def test_parse_without_heading_is_empty():
    assert parse_contract_roles("| role | headers | symbols |\n|---|---|---|\n") == []


def test_render_lists_roles_with_engine_paths_and_na(tmp_path):
    _write_readme(tmp_path, "toy")
    out = render_backend_api(tmp_path, "toy")
    assert "- **registration:** `backends/toy/api/accelerator_api.hpp`, `backends/toy/api/toy_registry.hpp`; symbols `RegisterQuery`, `QueryRegistrar`" in out
    assert "- **maintenance:** n/a" in out
    assert "Ignored" not in out


def test_render_differs_only_by_manifest(tmp_path):
    _write_readme(tmp_path, "a")
    _write_readme(tmp_path, "b", README.replace("toy_scan", "b_scan"))
    a, b = render_backend_api(tmp_path, "a"), render_backend_api(tmp_path, "b")
    assert a.replace("backends/a/", "backends/b/") != b
    assert a.replace("backends/a/", "backends/b/").replace("toy_scan", "b_scan") == b


def test_render_without_manifest_says_so(tmp_path):
    assert "absent" in render_backend_api(tmp_path, "none")


def test_profile_rendered_for_shipped_engines_and_empty_for_unknown():
    out = render_accelerator_engine_profile(PIPELINE_ROOT, "postgres")
    assert "### hazards" in out and "### scan" in out
    assert render_accelerator_engine_profile(PIPELINE_ROOT, "nope") == ""


Q16_SQL = """
SELECT p_brand, p_type, p_size, COUNT(DISTINCT ps_suppkey) AS supplier_cnt
FROM partsupp, part
WHERE p_partkey = ps_partkey
  AND p_brand <> 'Brand#13'
  AND p_type NOT LIKE 'SMALL BRUSHED%'
  AND p_size IN (26, 24, 27, 29, 7, 20, 22, 33)
  AND ps_suppkey NOT IN (
    SELECT s_suppkey FROM supplier WHERE s_comment LIKE '%Customer%Complaints%')
GROUP BY p_brand, p_type, p_size
ORDER BY supplier_cnt DESC, p_brand, p_type, p_size;
"""


def test_q16_checklist_covers_polarity():
    checks = extract_predicate_checklist(Q16_SQL)
    joined = "\n".join(checks)
    assert "NOT LIKE" in joined
    assert "Inequality" in joined
    assert "Anti-join" in joined or "complaint" in joined.lower()
    assert "Hot-path" in joined


def test_bootstrap_section_mentions_contract(tmp_path):
    sql_path = tmp_path / "16.sql"
    sql_path.write_text(Q16_SQL, encoding="utf-8")
    section = format_predicate_bootstrap_section(Q16_SQL, str(sql_path))
    assert "unit_test_contract.md" in section
    assert "NOT LIKE" in section
    assert "Brand#13" in section or "<>" in section


def _sample_analysis() -> dict:
    selected = {
        "id": "daily_partial",
        "design_family": "flat_partitioned",
        "viability": "viable",
        "selected": True,
        "state_grain": "day",
        "output_grain": "grouped",
        "physical_organization": "hash by shipdate day",
        "state_payload": "partial sums",
        "composition_rule": "merge days",
        "operators_eliminated": ["Scan"],
        "residual_read_algorithm": "merge partition payloads",
        "estimated_state_units_read": {"typical": "P partitions", "worst_case": "all days"},
        "read_path_bottleneck_risk": "low",
        "read_path_scaling_variable": "partitions",
        "parameter_scaling_argument": "prefix by day_offset",
        "asymptotic_read_complexity": "O(P)",
        "practical_read_complexity": "small P",
        "point_update_complexity": "O(1)",
        "batch_update_complexity": "O(touched)",
        "storage_complexity": "O(days)",
        "mutation_fanout": "1-2",
        "rebuild_locality": "per day",
        "parameter_binding_behavior": "stable",
        "history_growth_behavior": "grows with days",
        "best_regime": "read-heavy",
    }
    alternate = {
        "id": "monthly_partial",
        "design_family": "monthly",
        "viability": "viable",
        "selected": False,
        "state_grain": "month",
        "physical_organization": "month buckets",
        "composition_rule": "merge months",
        "operators_eliminated": ["Scan"],
        "read_path_bottleneck_risk": "medium",
        "best_regime": "narrow month binds",
    }
    return {
        "accelerator_id": "tpch_q1_aux",
        "aux_function_name": "accel_tpch_q1_aux",
        "correctness_facts": {
            "state_design": {"selected_candidate_id": "daily_partial"},
            "dependent_tables": [{"name": "lineitem"}],
            "mutation_coverage": [{"table": "lineitem", "mutation": "update"}],
        },
        "state_candidate_search": {
            "candidates": [selected, alternate],
        },
        "performance_hypotheses": {
            "base_path_cost_decomposition": {"dominant_scans": ["lineitem"]},
            "initial_cost_model": {"config_path": "accelerators/tpch/q1/config/q1_aux_cost_model.json"},
            "likely_best_actions": ["ApplyDelta"],
        },
        "files_to_create": [{"path": "accelerators/tpch/q1/duckdb/aux/module.cpp", "role": "kernel"}],
    }


class TestAnalysisContextSlicing:
    def test_implementation_slice_omits_candidate_search(self):
        slim = analysis_for_implementation(_sample_analysis())
        assert "correctness_facts" in slim
        assert slim["selected_candidate_id"] == "daily_partial"
        assert "state_candidate_search" not in slim
        assert "base_path_cost_decomposition" not in slim.get("performance_hypotheses", {})

    def test_optimization_slice_has_alternates_only(self):
        slim = analysis_for_optimization(_sample_analysis())
        assert slim["active_candidate_id"] == "daily_partial"
        assert len(slim["viable_alternate_candidates"]) == 1
        assert slim["viable_alternate_candidates"][0]["id"] == "monthly_partial"

    def test_audit_slice_is_summary_only(self):
        slim = analysis_for_audit(_sample_analysis())
        assert slim["selected_candidate_id"] == "daily_partial"
        assert slim["dependent_table_names"] == ["lineitem"]
        assert "correctness_facts" not in slim


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
    monkeypatch.setattr(prompts, "query_artifacts_dir", lambda root, q, workload=None: art)
    monkeypatch.setattr(prompts, "gates_dir_for_query", lambda a: a / "gates")
    monkeypatch.setattr(prompts, "analysis_artifact_path", lambda root, q, workload=None: art / "analysis.json")
    return SimpleNamespace(repo_root=repo, quick_tier=QuickTierConfig()), art


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
        pipeline_config=SimpleNamespace(repo_root=repo), workload=WORKLOAD, query=5, art=art, stage="audit",
        log=lambda line: None)
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


def test_a_change_request_raised_in_the_repair_round_goes_to_the_tester_before_the_recheck(stage_ctx, monkeypatch):
    rnd = Round(stage_ctx, [{"correctness_ok": True}])
    (stage_ctx.art / "audit_result.json").write_text(json.dumps(BLOCKED))
    monkeypatch.setattr(audit_stage, "repair_turn", rnd.repair)
    handled = []
    monkeypatch.setattr(audit_stage, "revise_upstream", lambda ctx: handled.append(ctx) or True)
    audit_stage._fix_blocking_findings(stage_ctx, rnd.audit_turn)
    assert handled == [stage_ctx, stage_ctx] and "tester revised the frozen test suite" in rnd.audit_messages[0]


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


def test_the_implementation_bootstrap_points_at_an_existing_sibling_query_of_any_benchmark(tmp_path):
    from accel_pipeline.prompts import build_implementation_bootstrap

    for rel in ("accelerators/other/q9/toyengine/aux/register.cpp", "accelerators/other/q9/layout.hpp",
                "accelerators/other/q9/bench/benchmark_q9.cpp", "accelerators/new/q2/toyengine/aux/register.cpp",
                "accelerators/new/q2/layout.hpp"):
        (tmp_path / rel).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / rel).write_text("")
    workload = {"benchmark_name": "new", "engine_name": "toyengine"}
    text = build_implementation_bootstrap(tmp_path, workload, 5, {}, cost_model_config="cfg.json")
    assert "`accelerators/new/q2/layout.hpp`" in text and "accelerators/other/q9" not in text  # the same benchmark first
    for rel in ("accelerators/new/q2",):
        (tmp_path / rel / "layout.hpp").unlink()
        (tmp_path / rel / "toyengine").rename(tmp_path / rel / "gone")
    text = build_implementation_bootstrap(tmp_path, workload, 5, {}, cost_model_config="cfg.json")
    assert "`accelerators/other/q9/layout.hpp`" in text and "`accelerators/other/q9/bench/benchmark_q9.cpp`" in text
