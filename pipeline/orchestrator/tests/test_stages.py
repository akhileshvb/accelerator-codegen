"""The accelerator stages: scaffolded stubs, structural re-analysis, stage turn limits, the analysis stage with its
critic, and the exit run planned at every stage exit."""
from __future__ import annotations

import json
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import MagicMock

import pytest

from accel_pipeline import stages as stage
from accel_pipeline.config import (
    find_repo_root,
    load_pipeline_config,
    load_workload,
    with_facts,
    workload_supported_engines,
)
from accel_pipeline.gates import DEFAULT_GROUPS, GATE_SCRIPTS, SUBSET_DATASET_GATES
from accel_pipeline.repair import Failure, StageFailure
from accel_pipeline.stages import (
    _build_stub_values,
    _type_prefix,
    clear_request,
    load_pending_request,
    materialize_workload_stubs,
    process_after_stage,
    resolve_optimization_max_turns,
    resolve_stage_max_turns,
    validate_request,
    workload_post_audit_gates,
    workload_post_implementation_gates,
    workload_post_optimization_gates,
    write_request,
)


def test_type_prefix_from_create_fn():
    assert _type_prefix("CreateSecEdgarQ1FunctionSet") == "SecEdgarQ1"
    assert _type_prefix("CreateTpchQ3FunctionSet") == "TpchQ3"


def test_build_stub_values_sec_edgar():
    repo = Path(__file__).resolve().parents[3]
    pipeline_config = load_pipeline_config(repo_root=repo)
    workload = load_workload("sec_edgar_q1", repo)
    paths = {
        "function_name": "accel_sec_edgar_q1",
        "create_fn": "CreateSecEdgarQ1FunctionSet",
        "test_target": "test_sec_edgar_q1",
        "bench_target": "benchmark_sec_edgar_q1",
        "header_path": "accelerators/sec_edgar/q1/duckdb/base/query.hpp",
    }
    values = _build_stub_values(with_facts(repo, workload, 1), paths, 1)
    assert values["type_prefix"] == "SecEdgarQ1"
    assert values["scan_spec_fn"] == "CreateSecEdgarQ1ScanSpec"
    assert '"stmt"' in values["output_schema_names"]


def test_materialize_workload_stubs_writes_missing_files(tmp_path):
    repo = tmp_path / "repo"
    pipe = repo / "pipeline"
    (pipe / "templates" / "scaffold").mkdir(parents=True)
    (pipe / "templates" / "sec_edgar").mkdir(parents=True)
    (pipe / "config" / "benchmark_packs").mkdir(parents=True)
    (pipe / "config" / "workloads").mkdir(parents=True)

    # minimal templates
    (pipe / "templates" / "scaffold" / "header.hpp.tpl").write_text(
        "#pragma once\n// {{type_prefix}} {{scan_spec_fn}}\n",
        encoding="utf-8",
    )
    (pipe / "templates" / "scaffold" / "kernel.cpp.tpl").write_text(
        "// {{kernel_fn}}\n",
        encoding="utf-8",
    )
    (pipe / "templates" / "scaffold" / "package.cpp.tpl").write_text(
        "// {{create_fn}}\n",
        encoding="utf-8",
    )

    (repo / "workloads" / "sec_edgar").mkdir(parents=True)
    (repo / "workloads" / "sec_edgar" / "schema.sql").write_text("CREATE TABLE sub (adsh VARCHAR, fy INTEGER);\n", encoding="utf-8")
    sql = repo / "workloads" / "sec_edgar" / "q1.sql"
    sql.write_text("SELECT adsh FROM sub WHERE fy = 2022\n", encoding="utf-8")

    workload = {
        "reference_sql_path": str(sql),
        "benchmark_name": "sec_edgar",
        "engine_name": "duckdb",
        "query_number": 1,
        "artifact_namespace": "sec_edgar",
        "source_subdir": "sec_edgar/q1/base/single-threaded",
        "function_name": "accel_sec_edgar_q1",
        "create_fn": "CreateSecEdgarQ1FunctionSet",
        "header_basename": "sec_edgar_q1",
        "test_target": "test_sec_edgar_q1",
        "bench_target": "benchmark_sec_edgar_q1",
    }

    from accel_pipeline.config import PipelineConfig

    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=pipe,
        build_dir=repo / "build-release",
    )

    result = materialize_workload_stubs(pipeline_config, workload, 1)
    header = repo / "accelerators/sec_edgar/q1/duckdb/base/query.hpp"
    assert header.is_file()
    assert "SecEdgarQ1" in header.read_text(encoding="utf-8")
    assert "query.hpp" in result["created"][0]


def test_materialize_registration_fills_the_engines_template_once(tmp_path, monkeypatch):
    from accel_pipeline.stages import materialize_registration

    repo = tmp_path / "repo"
    files = {
        "workloads/toybench/schema.sql": "CREATE TABLE t (a INTEGER, b INTEGER);\nCREATE TABLE u (a INTEGER, c INTEGER);\n",
        "workloads/toybench/q1.sql": "SELECT SUM(t.b) FROM t, u WHERE t.a = u.a AND u.c > $1\n",
        "workloads/toybench/instances/seed_1/1.sql": "SELECT SUM(t.b) FROM t, u WHERE t.a = u.a AND u.c > 5;\n",
        "backends/toyengine/api/scaffold/register.cpp.tpl":
            "{{query_number}} {{accelerator_id}} {{table_function}} {{argument_types}} {{tables}}\n{{fallback_sql}}\n",
    }
    for rel, text in files.items():
        (repo / rel).parent.mkdir(parents=True, exist_ok=True)
        (repo / rel).write_text(text)
    (tmp_path / "art").mkdir()
    monkeypatch.setattr("accel_pipeline.stages.query_artifacts_dir", lambda *a, **k: tmp_path / "art")
    register = "accelerators/toybench/q1/toyengine/aux/register.cpp"
    workload = {"workload_kind": "aux_state", "benchmark_name": "toybench", "engine_name": "toyengine",
                "accelerator_id": "toybench_q1_aux", "parameters": [{"name": "c", "type": "integer"}],
                "engine_host": {"aux_register_path": register},
                "reference_sql_path": str(repo / "workloads/toybench/instances/seed_1/1.sql")}
    config = type("Cfg", (), {"repo_root": repo})()

    stateless = {**workload, "workload_kind": "base"}  # a stateless query has no analysis accelerator_id and no aux module
    assert materialize_registration(config, stateless, 1)["created"] == [] and not (repo / register).exists()
    finalize = register.replace("register.cpp", "finalize.cpp")  # the file layout every aux module has
    assert materialize_registration(config, workload, 1) == {"created": [register, finalize]}
    assert (repo / finalize).is_file()
    text = (repo / register).read_text()
    assert '1 toybench_q1_aux accel_toybench_q1_aux {"INTEGER"} {"t", "u"}' in text and "u.c > $1" in text
    (repo / register).write_text("// the implementation's registration")
    assert materialize_registration(config, workload, 1) == {"created": [], "skipped_existing": 1}
    assert (repo / register).read_text() == "// the implementation's registration"


REPO_ROOT = Path(__file__).resolve().parents[3]
RUN = {"benchmark_name": "tpch", "engine_name": "duckdb"}


def _valid_request(**overrides) -> dict:
    payload = {
        "requested": True,
        "source_stage": "implementation",
        "source_iteration": 0,
        "bottleneck_type": "scan_for_scan",
        "evidence": "aux_bench_sweep at 50% writes: 63% fallback, workload_speedup 0.65 vs DuckDB",
        "proposed_direction": "Switch to hierarchical month buckets per order to avoid segment scan on read",
        "current_candidate_id": "c1",
        "alternate_candidate_ids": ["c2"],
        "cost_model_insufficient_reason": "Cost model cannot eliminate full segment partition scan on read path",
    }
    payload.update(overrides)
    return payload


class TestStructuralReanalysisValidation:
    def test_validate_rejects_short_evidence(self):
        data = _valid_request(evidence="too short")
        errors = validate_request(data)
        assert any("evidence" in e for e in errors)


class TestStructuralReanalysisFlow:
    @pytest.fixture
    def artifacts_dir(self, tmp_path):
        art = tmp_path / "pipeline" / "artifacts" / "q99"
        art.mkdir(parents=True)
        return art

    def test_write_and_load_request(self, tmp_path):
        write_request(
            tmp_path,
            99,
            RUN,
            source_stage="implementation",
            source_iteration=0,
            bottleneck_type="wrong_partition_grain",
            evidence="Read path scans all segment partitions despite selective predicate on shipdate",
            proposed_direction="Repartition by shipdate month instead of segment only",
            current_candidate_id="c1",
        )
        loaded = load_pending_request(tmp_path, 99, RUN)
        assert loaded is not None
        assert loaded["bottleneck_type"] == "wrong_partition_grain"
        clear_request(tmp_path, 99, RUN)
        assert load_pending_request(tmp_path, 99, RUN) is None

    def test_process_after_stage_runs_stages(self, tmp_path):
        write_request(
            tmp_path,
            99,
            RUN,
            source_stage="implementation",
            source_iteration=0,
            bottleneck_type="scan_for_scan",
            evidence="aux_bench_sweep at 50% writes: 63% fallback, workload_speedup 0.65 vs DuckDB",
            proposed_direction="Use orderdate btree index candidate instead of segment partitions",
            current_candidate_id="c1",
        )
        run_analysis = MagicMock()
        run_adv = MagicMock()
        run_impl = MagicMock()
        run_opt = MagicMock()
        workload = {**RUN, "max_structural_reanalysis": 2}

        result = process_after_stage(
            repo_root=tmp_path,
            query=99,
            workload=workload,
            triggered_after="implementation",
            structural_reanalysis_count=0,
            run_analysis=run_analysis,
            run_test_suite=run_adv,
            run_implementation=run_impl,
            run_optimization=run_opt,
            dry_run=False,
        )

        assert result == (1, 3)  # optimization index
        run_analysis.assert_called_once()
        run_adv.assert_called_once()
        run_impl.assert_called_once()
        run_opt.assert_not_called()
        assert load_pending_request(tmp_path, 99, RUN) is None


    def test_budget_exhausted_clears_request(self, tmp_path):
        write_request(
            tmp_path,
            99,
            RUN,
            source_stage="implementation",
            source_iteration=0,
            bottleneck_type="scan_for_scan",
            evidence="aux_bench_sweep at 50% writes: 63% fallback, workload_speedup 0.65 vs DuckDB",
            proposed_direction="Try alternate candidate with different partition grain",
            current_candidate_id="c1",
        )
        result = process_after_stage(
            repo_root=tmp_path,
            query=99,
            workload={**RUN, "max_structural_reanalysis": 2},
            triggered_after="implementation",
            structural_reanalysis_count=2,
            run_analysis=MagicMock(),
            run_implementation=MagicMock(),
            run_optimization=MagicMock(),
            dry_run=False,
        )
        assert result is None
        assert load_pending_request(tmp_path, 99, RUN) is None


class TestReanalysisToolPolicy:
    def test_request_tool_rejects_exhausted_budget(self, tmp_path, monkeypatch):
        from accel_pipeline.config import load_pipeline_config
        from accel_pipeline.runlog import PipelineState, state_path
        from accel_pipeline.stages import ReanalysisToolContext, make_reanalysis_tools

        monkeypatch.setenv("ACCEL_PIPELINE_PRIMARY_REPO", str(REPO_ROOT))
        state = PipelineState(query=3, workload="tpch_q3_aux", engine="duckdb", structural_reanalysis_count=2)
        state.save(state_path(tmp_path, 3, "tpch_q3_aux", "duckdb"))

        pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
        pipeline_config = type(pipeline_config)(
            **{**pipeline_config.__dict__, "repo_root": tmp_path, "engine_name": "duckdb"}
        )
        ctx = ReanalysisToolContext(pipeline_config, 3, "implementation", "tpch_q3_aux")
        tool = make_reanalysis_tools(ctx)
        monkeypatch.setattr(
            "accel_pipeline.stages.load_run_workload",
            lambda pc, name: {**RUN, "max_structural_reanalysis": 2},
        )
        out = json.loads(
            tool(
                bottleneck_type="scan_for_scan",
                evidence="aux_bench_sweep at 50% writes: 63% fallback, workload_speedup 0.65",
                proposed_direction="Switch to btree index on orderdate for selective reads",
                current_candidate_id="c1",
            )
        )
        assert out["status"] == "rejected"
        assert "budget" in out["reason"]


def test_resolve_stage_max_turns_unlimited_when_zero() -> None:
    assert resolve_stage_max_turns({"implementation": 0}, "implementation") == 10_000
    assert resolve_stage_max_turns({"implementation": 12}, "implementation") == 12


def test_resolve_optimization_max_turns_unlimited_when_zero() -> None:
    from accel_pipeline.config import PipelineConfig

    repo = find_repo_root()
    pc = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        optimization_max_turns_unified=0,
    )
    assert resolve_optimization_max_turns(pc, "unified") == 10_000
    pc.optimization_max_turns_unified = 50
    assert resolve_optimization_max_turns(pc, "unified") == 50


@pytest.fixture
def harness(tmp_path, monkeypatch):
    """The analysis stage with agents scripted: `critiques` are what successive critic runs write."""
    path = tmp_path / "analysis.json"
    path.write_text(json.dumps({"correctness_facts": {"state_design": {}}}))
    calls, critiques, events = [], [], []

    def fake_agent(*, stage, user_message, extra_context=None, session_role=None, **_):
        calls.append((stage, session_role, user_message))
        if stage == "analysis_critic":
            report = critiques.pop(0)
            if isinstance(report, Exception):
                raise report
            (tmp_path / "analysis_critique.json").write_text(json.dumps(report))
        return "result"

    monkeypatch.setattr(stage, "run_agent_stage", fake_agent)
    monkeypatch.setattr(stage, "_ensure_dataset", lambda *a: None)
    monkeypatch.setattr(stage, "load_run_workload", lambda *a: {"workload_kind": "aux_state", **harness_workload})
    monkeypatch.setattr(stage, "analysis_artifact_path", lambda *a, **k: path)
    invalid: list[str] = []

    def fake_validate(*_):
        if invalid:
            raise RuntimeError(invalid.pop(0))

    monkeypatch.setattr(stage, "_validate", fake_validate)
    monkeypatch.setattr(stage, "log_analysis_candidates", lambda *a, **k: None)
    monkeypatch.setattr(stage, "log_event", lambda *a, **k: events.append(k))
    harness_workload: dict = {}
    harness = SimpleNamespace(dir=tmp_path)

    def run(**workload):
        harness_workload.clear()
        harness_workload.update(workload)
        stage.run_analysis(SimpleNamespace(repo_root=harness.dir), object(), "w", 3)
        return [(s, role) for s, role, _ in calls], calls

    run.critiques, run.events, run.dir, run.invalid = critiques, events, tmp_path, invalid
    return run


def test_accepted_analysis_goes_straight_on(harness):
    harness.critiques.append({"verdict": "accept", "checks": [{"id": "R1", "ok": True}]})
    stages, _ = harness()
    assert stages == [("analysis", "main"), ("analysis_critic", None)]
    assert (harness.dir / "analysis_critique_r1.json").is_file()


def test_failed_check_sends_the_critique_back_to_the_main_session(harness):
    harness.critiques.append({"verdict": "accept", "checks": [{"id": "R1", "ok": False, "case": "global window"}]})
    stages, calls = harness()
    assert stages == [("analysis", "main"), ("analysis_critic", None), ("analysis", "main")]
    assert "global window" in calls[-1][2]


def test_a_better_alternative_is_a_revision(harness):
    harness.critiques.append({"verdict": "accept", "checks": [], "alternative": {"better": True}})
    stages, _ = harness()
    assert stages[-1] == ("analysis", "main")


def test_rounds_are_capped_and_zero_disables(harness):
    revise = {"verdict": "revise", "checks": []}
    harness.critiques.extend([revise, revise])
    stages, _ = harness(analysis_critique_max_rounds=2)
    assert stages.count(("analysis_critic", None)) == 2 and stages.count(("analysis", "main")) == 3
    stages, _ = harness(analysis_critique_max_rounds=0)
    assert ("analysis_critic", None) not in stages[-1:]


def test_a_critic_that_fails_never_stops_the_run(harness):
    harness.critiques.append(StageFailure(Failure(kind="infra", stage="analysis_critic", source="codex_exec", summary="timeout")))
    stages, _ = harness()
    assert stages == [("analysis", "main"), ("analysis_critic", None)]


def test_validation_errors_go_back_to_the_main_session(harness):
    harness.invalid.append("state_storage is required")
    harness.critiques.append({"verdict": "accept", "checks": []})
    stages, calls = harness()
    assert stages == [("analysis", "main"), ("analysis", "main"), ("analysis_critic", None)]
    assert "state_storage is required" in calls[1][2]


def test_validation_repair_is_bounded(harness):
    harness.invalid.extend(["bad"] * 3)
    with pytest.raises(RuntimeError, match="bad"):
        harness()




def test_the_exit_run_is_a_gate_with_its_script_and_the_subset_dataset():
    assert (REPO_ROOT / "pipeline/gates" / GATE_SCRIPTS["aux_exit"]).is_file()
    assert "aux_exit" in SUBSET_DATASET_GATES


def test_every_stage_exit_runs_it_after_the_unit_checks():
    workload = load_workload("tpch_q3_aux", REPO_ROOT, engine=workload_supported_engines("tpch_q3_aux", REPO_ROOT)[0])
    for gates in (workload_post_implementation_gates(workload), workload_post_optimization_gates(workload),
                  workload_post_audit_gates(workload)):
        assert gates.index("aux_unit") < gates.index("aux_exit")
        assert not {"aux_correctness", "aux_multi_connection", "aux_bench_sweep", "aux_tsan"} & set(gates)


def test_it_is_admitted_only_after_quick_check():
    quick = load_pipeline_config(repo_root=REPO_ROOT).quick_tier
    assert "aux_exit" in quick.admission_gates
    assert any(group.names == ("aux_exit",) for group in DEFAULT_GROUPS)
