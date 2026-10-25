import json

import pytest

from accel_pipeline import cli, run_pipeline, run_status
from accel_pipeline.state import STAGES, PipelineState


def stage_events(*done):
    return [{"ts": f"2026-10-25T10:0{i}:00+00:00", "event": kind, "stage": stage}
            for i, stage in enumerate(done) for kind in ("stage_start", "stage_complete")]


def make_run(tmp_path, state, events):
    art = tmp_path / "tree/pipeline/artifacts/tpch/q3/duckdb"
    art.mkdir(parents=True)
    state.save(art / "pipeline_state.json")
    (art / "pipeline_events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in events))
    (tmp_path / "pipeline.log").write_text("=== stage: analysis ===\n")
    return tmp_path


def test_status_names_the_stage_of_a_running_run(tmp_path):
    run = make_run(tmp_path, PipelineState(query=3, workload="tpch_q3_aux", engine="duckdb", stage="test_suite"),
                   stage_events("analysis"))
    text = run_status.render(run)
    assert "stage:  in test_suite" in text and "stage_complete  analysis" in text and "cost:" in text


def test_status_shows_a_pause_and_a_finished_run(tmp_path):
    paused = PipelineState(query=3, workload="w", engine="duckdb", stage="optimization", status="paused:until implementation")
    assert "stage:  paused:until implementation" in run_status.render(make_run(tmp_path / "a", paused, stage_events("analysis")))
    done = PipelineState(query=3, workload="w", engine="duckdb", stage="audit")
    assert "stage:  finished" in run_status.render(make_run(tmp_path / "b", done, stage_events(*STAGES)))


def test_status_of_an_empty_directory_says_so(tmp_path):
    assert "no pipeline state yet" in run_status.render(tmp_path)


def stages_run(argv, capsys):
    assert run_pipeline.main(argv) == 0
    return [line.split(": ")[1].strip(" =") for line in capsys.readouterr().err.splitlines() if line.startswith("=== stage:")]


def test_until_stops_after_that_stage_and_resume_continues_with_the_next(tmp_path, monkeypatch, capsys):
    path = tmp_path / "pipeline_state.json"
    monkeypatch.setattr(run_pipeline, "state_path", lambda *a: path)
    monkeypatch.setattr("accel_pipeline.state.state_path", lambda *a: path)
    ran = []
    monkeypatch.setattr(run_pipeline, "_run_stage", lambda stage, *a, **k: ran.append(stage))
    for patched in ("_sandbox_preflight", "ensure_base_accelerator_prerequisite"):
        monkeypatch.setattr(run_pipeline, patched, lambda *a, **k: None)
    monkeypatch.setattr(run_pipeline.preflight, "require", lambda *a, **k: None)
    monkeypatch.setattr("accel_pipeline.quick.prepare_inner_loop", lambda *a: None)
    monkeypatch.setattr("accel_pipeline.host_bootstrap.ensure_host_bootstrap_for_workload", lambda *a, **k: None)
    monkeypatch.setattr("accel_pipeline.backend_requests.reopen_engine_backend", lambda *a, **k: None)
    monkeypatch.setattr("accel_pipeline.core_budget.activate", lambda *a: type("B", (), {"cpus": (), "measure": (), "slot": 0})())
    monkeypatch.setattr(run_pipeline, "log_pipeline_summary", lambda *a: None)
    monkeypatch.setattr(run_pipeline, "_maybe_process_structural_reanalysis", lambda **k: None)
    monkeypatch.setattr(run_pipeline, "advance_stage", lambda state, root: (setattr(state, "stage", STAGES[STAGES.index(state.stage) + 1]), state.save(path)))
    args = ["--workload", "tpch_q14_aux", "--query", "14", "--engine", "duckdb"]

    assert run_pipeline.main([*args, "--until", "test_suite"]) == 0
    assert ran == ["analysis", "test_suite"]
    assert PipelineState.load(path).status == "paused:until test_suite"

    ran.clear()
    assert run_pipeline.main([*args, "--resume"]) == 0
    assert ran == ["implementation", "optimization", "audit"]


def test_until_conflicts_with_a_single_stage():
    with pytest.raises(SystemExit):
        cli.build_parser().parse_args(["run", "tpch_q3_aux", "--stage", "audit", "--until", "analysis"])
    with pytest.raises(SystemExit):
        run_pipeline.main(["--workload", "w", "--query", "1", "--stage", "audit", "--until", "analysis"])


def test_every_command_is_listed_in_the_help(capsys):
    with pytest.raises(SystemExit):
        cli.main(["--help"])
    out = capsys.readouterr().out
    for command in ("run", "canary", "check", "smoke", "status", "timeline"):
        assert f"\n    {command}" in out
