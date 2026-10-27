"""The `accel` front end, the run (stage order, pause and resume, the clean stop of an exhausted repair loop) and the
preflight checks that guard it."""
from __future__ import annotations

import json
import stat
from dataclasses import replace
from pathlib import Path

import pytest

from accel_pipeline import cli, runlog
from accel_pipeline.agent import CodexExecBackend
from accel_pipeline.config import (
    find_repo_root,
    load_model_config,
    load_pipeline_config,
    load_workload,
    PipelineConfig,
    RepairConfig,
    workload_supported_engines,
)
from accel_pipeline.repair import EXIT_STOPPED, StageStopped
from accel_pipeline.runlog import PipelineState, STAGES


ENGINE = workload_supported_engines("tpch_q14_aux", find_repo_root())[0]


def stage_events(*done):
    return [{"ts": f"2026-10-25T10:0{i}:00+00:00", "event": kind, "stage": stage}
            for i, stage in enumerate(done) for kind in ("stage_start", "stage_complete")]


def make_run(tmp_path, state, events):
    art = tmp_path / "tree/pipeline/artifacts/tpch/q3/toydb"
    art.mkdir(parents=True)
    state.save(art / "pipeline_state.json")
    (art / "pipeline_events.jsonl").write_text("".join(json.dumps(e) + "\n" for e in events))
    (tmp_path / "pipeline.log").write_text("=== stage: analysis ===\n")
    return tmp_path


def test_status_names_the_stage_of_a_running_run(tmp_path):
    run = make_run(tmp_path, PipelineState(query=3, workload="tpch_q3_aux", engine="toydb", stage="test_suite"),
                   stage_events("analysis"))
    text = runlog.render_status(run)
    assert "stage:  in test_suite" in text and "stage_complete  analysis" in text and "cost:" in text


def test_status_shows_a_pause_and_a_finished_run(tmp_path):
    paused = PipelineState(query=3, workload="w", engine="toydb", stage="optimization", status="paused:until implementation")
    assert "stage:  paused:until implementation" in runlog.render_status(make_run(tmp_path / "a", paused, stage_events("analysis")))
    done = PipelineState(query=3, workload="w", engine="toydb", stage="audit")
    assert "stage:  finished" in runlog.render_status(make_run(tmp_path / "b", done, stage_events(*STAGES)))


def test_status_of_an_empty_directory_says_so(tmp_path):
    assert "no pipeline state yet" in runlog.render_status(tmp_path)


def stages_run(argv, capsys):
    assert cli.run_main(argv) == 0
    return [line.split(": ")[1].strip(" =") for line in capsys.readouterr().err.splitlines() if line.startswith("=== stage:")]


def test_until_stops_after_that_stage_and_resume_continues_with_the_next(tmp_path, monkeypatch, capsys):
    path = tmp_path / "pipeline_state.json"
    monkeypatch.setattr(cli, "state_path", lambda *a: path)
    monkeypatch.setattr("accel_pipeline.runlog.state_path", lambda *a: path)
    ran = []
    monkeypatch.setattr(cli, "_run_stage", lambda stage, *a, **k: ran.append(stage))
    for patched in ("_sandbox_preflight",):
        monkeypatch.setattr(cli, patched, lambda *a, **k: None)
    monkeypatch.setattr(cli, "require", lambda *a, **k: None)
    monkeypatch.setattr("accel_pipeline.gates.prepare_inner_loop", lambda *a: None)
    monkeypatch.setattr("accel_pipeline.engine_backend.host_bootstrap.ensure_host_bootstrap_for_workload", lambda *a, **k: None)
    monkeypatch.setattr("accel_pipeline.engine_backend.backend_requests.reopen_engine_backend", lambda *a, **k: None)
    monkeypatch.setattr("accel_pipeline.pack.core_budget.activate", lambda *a: type("B", (), {"cpus": (), "measure": (), "slot": 0})())
    monkeypatch.setattr(cli, "log_pipeline_summary", lambda *a: None)
    monkeypatch.setattr(cli, "_maybe_process_structural_reanalysis", lambda **k: None)
    monkeypatch.setattr(cli, "advance_stage", lambda state, root: (setattr(state, "stage", STAGES[STAGES.index(state.stage) + 1]), state.save(path)))
    args = ["--workload", "tpch_q14_aux", "--query", "14", "--engine", ENGINE]

    assert cli.run_main([*args, "--until", "test_suite"]) == 0
    assert ran == ["analysis", "test_suite"]
    assert PipelineState.load(path).status == "paused:until test_suite"

    ran.clear()
    assert cli.run_main([*args, "--resume"]) == 0
    assert ran == ["implementation", "optimization", "audit"]


def test_until_conflicts_with_a_single_stage():
    with pytest.raises(SystemExit):
        cli.build_parser().parse_args(["run", "tpch_q3_aux", "--stage", "audit", "--until", "analysis"])
    with pytest.raises(SystemExit):
        cli.run_main(["--workload", "w", "--query", "1", "--stage", "audit", "--until", "analysis"])


def test_every_command_is_listed_in_the_help(capsys):
    with pytest.raises(SystemExit):
        cli.main(["--help"])
    out = capsys.readouterr().out
    for command in ("run", "canary", "check", "smoke", "status", "timeline"):
        assert f"\n    {command}" in out


@pytest.fixture
def config(tmp_path, monkeypatch):
    monkeypatch.setattr("accel_pipeline.repair.load_run_workload", lambda cfg, name: {"benchmark_name": "tpch"})
    path = tmp_path / "pipeline/artifacts/tpch/q3/pipeline_state.json"
    path.parent.mkdir(parents=True)
    PipelineState(query=3, workload="w", engine="e", stage="implementation").save(path)
    monkeypatch.setattr(cli, "state_path", lambda *a: path)
    return PipelineConfig(repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "b",
                          repair=RepairConfig(max_orchestrator_retries=1))


def test_a_stage_exception_is_retried_once_then_recorded_as_a_stop(config, monkeypatch, tmp_path):
    calls = []

    def boom(*args, **kwargs):
        calls.append(kwargs["resume"])
        raise ValueError("bug")

    monkeypatch.setattr(cli, "run_stage", boom)
    with pytest.raises(StageStopped, match="orchestrator exception"):
        cli._run_stage("implementation", config, None, "w", 3, {}, False)
    assert calls == [False, True]
    state = json.loads((tmp_path / "pipeline/artifacts/tpch/q3/pipeline_state.json").read_text())
    assert state["status"].startswith("stopped:") and state["stage"] == "implementation"
    assert "ValueError" in (tmp_path / "pipeline/artifacts/tpch/q3/stop_report.md").read_text()


def test_a_stage_that_recovers_on_retry_is_not_a_stop(config, monkeypatch):
    outcomes = iter([ValueError("flaky"), None])

    def once(*args, **kwargs):
        outcome = next(outcomes)
        if outcome:
            raise outcome

    monkeypatch.setattr(cli, "run_stage", once)
    cli._run_stage("implementation", config, None, "w", 3, {}, False)


def test_dry_run_exceptions_still_surface(config, monkeypatch):
    monkeypatch.setattr(cli, "run_stage", lambda *a, **k: (_ for _ in ()).throw(ValueError("x")))
    with pytest.raises(ValueError):
        cli._run_stage("implementation", config, None, "w", 3, {}, True)


def test_main_exits_with_code_3_on_a_stop_and_on_a_stray_exception(monkeypatch, tmp_path):
    def stopped(argv):
        raise StageStopped("gate failed", tmp_path / "stop_report.md")

    monkeypatch.setattr(cli, "_run_main", stopped)
    assert cli.run_main([]) == EXIT_STOPPED == 3

    def stray(argv):
        raise KeyError("x")

    monkeypatch.setattr(cli, "_run_main", stray)
    assert cli.run_main([]) == 3


REPO_ROOT = Path(__file__).resolve().parents[3]


@pytest.fixture
def probe(tmp_path, monkeypatch) -> cli.Probe:
    monkeypatch.setattr(CodexExecBackend, "_resolve_codex_bin", lambda self: "codex")
    monkeypatch.setenv("OPENROUTER_API_KEY", "unused")
    engine = workload_supported_engines("tpch_q14_aux", REPO_ROOT)[0]
    cfg = replace(load_pipeline_config(None, REPO_ROOT), sandbox="none", engine_name=engine, codex_home=tmp_path / "codex_home")
    workload = load_workload("tpch_q14_aux", REPO_ROOT, engine=engine)
    models = load_model_config(None, repo_root=REPO_ROOT)
    return cli.Probe(cfg, models, workload, "tpch_q14_aux", 14)


def fake_perf(tmp_path, monkeypatch, stderr: str) -> None:
    script = tmp_path / "bin" / "perf"
    script.parent.mkdir()
    script.write_text(f"#!/bin/sh\ncat >&2 <<'EOF'\n{stderr}\nEOF\n")
    script.chmod(script.stat().st_mode | stat.S_IXUSR)
    monkeypatch.setenv("PATH", f"{script.parent}:/usr/bin:/bin")


def test_perf_without_pmu_is_reported(probe, tmp_path, monkeypatch):
    fake_perf(tmp_path, monkeypatch, "<not supported>,,cycles,0,100.00,,\n<not supported>,,instructions,0,100.00,,")
    assert "no counters" in cli.perf_counters(probe)[0]


def test_perf_with_counters_passes(probe, tmp_path, monkeypatch):
    fake_perf(tmp_path, monkeypatch, "1234,,cycles,100,100.00,,\n5678,,instructions,100,100.00,,")
    assert cli.perf_counters(probe) == []


def test_codex_line_carries_the_configured_window_and_compaction(probe):
    assert cli.codex_line(probe) == []


@pytest.mark.parametrize("field", ["codex_model_context_window", "codex_auto_compact_tokens", "codex_compact_prompt_file"])
def test_codex_line_without_a_setting_is_reported(probe, field):
    unset = replace(probe, models=replace(probe.models, **{field: 0 if "file" not in field else ""}))
    assert any("no" in p for p in cli.codex_line(unset))


def test_codex_line_with_a_missing_compact_prompt_file_is_reported(probe):
    missing = replace(probe, models=replace(probe.models, codex_compact_prompt_file="pipeline/prompts/shared/nope.md"))
    assert any("does not exist" in p for p in cli.codex_line(missing))


def test_gate_writers_of_this_repository_paste_no_log_text(probe):
    assert cli.gate_writers(probe) == []


@pytest.mark.parametrize("script, flagged", [
    ("python3 - <<PY\nprint('${LOG}')\nPY\n", True),                                  # the B7 pattern: log text in source
    ('python3 -c "print(\'${REASON}\')"\n', True),
    ("python3 - <<PY\nprint(${QUERY}, '${OUT}')\nPY\n", False),                        # numbers and paths are fine
    ("LOG_TEXT=\"${LOG}\" python3 - <<'PY'\nimport os\nprint(os.environ['LOG_TEXT'])\nPY\n", False),  # quoted heredoc
])
def test_gate_writer_scan(probe, tmp_path, script, flagged):
    gates = tmp_path / "gates"
    gates.mkdir()
    (gates / "run_x.sh").write_text(script)
    scanned = replace(probe, cfg=replace(probe.cfg, pipeline_root=tmp_path))
    assert bool(cli.gate_writers(scanned)) is flagged


def test_prompt_problems(tmp_path):
    (tmp_path / "pipeline").mkdir()
    (tmp_path / "pipeline" / "real.md").touch()
    text = "{{ghost}} `pipeline/real.md` `pipeline/gone.md` `pipeline/artifacts/tpch/q1/analysis.json`"
    assert cli.prompt_problems(text, tmp_path) == ["unresolved placeholder {{ghost}}", "names pipeline/gone.md, which does not exist"]


def test_stage_prompts_of_this_repository_render_clean(probe):
    assert cli.prompts(probe) == []


def test_the_live_model_probe_has_a_provider():
    from accel_pipeline.config import ModelConfig, resolve_stage_provider

    models = ModelConfig(api_key_env="K", presets={"default": "m"}, stages={}, stage_providers={"analysis": "codex_exec"})
    assert resolve_stage_provider("preflight_live", models) == "codex_exec"
