"""Tests for stage provider resolution and Codex/MCP wiring."""

from __future__ import annotations

from pathlib import Path


from accel_pipeline.backends.codex_exec import CodexExecBackend
from accel_pipeline.config import ModelConfig, load_model_config
from accel_pipeline.config import PipelineConfig


REPO_ROOT = Path(__file__).resolve().parents[3]


def _pipeline_config(**overrides) -> PipelineConfig:
    return PipelineConfig(
        repo_root=REPO_ROOT,
        pipeline_root=REPO_ROOT / "pipeline",
        build_dir=REPO_ROOT / "build-release",
        **overrides,
    )


def test_ensure_codex_config_materializes_template_once(tmp_path):
    from accel_pipeline.backends.codex_exec import ensure_codex_config

    config = tmp_path / "home" / "config.toml"
    assert ensure_codex_config(config) == config
    text = config.read_text(encoding="utf-8")
    assert f'command = "{REPO_ROOT}/pipeline/codex/run_mcp_server.sh"' in text
    assert "{{" not in text
    config.write_text("custom", encoding="utf-8")
    ensure_codex_config(config)
    assert config.read_text(encoding="utf-8") == "custom"


def test_mcp_env_sidecar_quotes_values_with_spaces(monkeypatch, tmp_path):
    from accel_pipeline.backends.codex_exec import sidecar_env_line
    from accel_pipeline.config import load_pipeline_config

    discovery = (
        "postgres: docker container=accel_pg_test 127.0.0.1:5434 "
        "so=/usr/lib/postgresql/17/lib/accelerator_codegen_pg.so"
    )
    monkeypatch.setenv("ACCEL_PG_DISCOVERY", discovery)
    line = sidecar_env_line("ACCEL_PG_DISCOVERY", discovery)
    assert " " not in line.split("=", 1)[0]
    assert line.startswith("ACCEL_PG_DISCOVERY=")
    assert "docker" in line

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    model = load_model_config(REPO_ROOT / "pipeline" / "config" / "models.yaml", repo_root=REPO_ROOT)
    backend = CodexExecBackend(
        pipeline_config=pipeline_config,
        model_config=model,
        query=0,
        stage="impl_epochs",
        workload_name="engine_postgres",
    )
    path = backend._write_mcp_env_sidecar(tmp_path)
    text = path.read_text(encoding="utf-8")
    assert "ACCEL_PG_DISCOVERY=" in text
    proc = __import__("subprocess").run(
        [
            "bash",
            "-c",
            "set -euo pipefail; set -a; source \"$1\"; set +a; "
            "test -n \"$ACCEL_PG_DISCOVERY\"; "
            "test \"$ACCEL_PIPELINE_QUERY\" = \"0\"; "
            "printf '%s' \"$ACCEL_PG_DISCOVERY\"",
            "source_sidecar",
            str(path),
        ],
        capture_output=True,
        text=True,
        check=False,
    )
    assert proc.returncode == 0, proc.stderr
    assert "docker container=accel_pg_test" in proc.stdout


FAKE_CODEX = Path(__file__).parent / "fixtures" / "fake_codex.sh"


def _fake_backend(tmp_path, monkeypatch, *, mode="ok", idle=0, hard=60, art=True):
    from accel_pipeline.config import SessionConfig

    monkeypatch.setenv("FAKE_CODEX_MODE", mode)
    monkeypatch.setenv("FAKE_CODEX_DIR", str(tmp_path))
    config = PipelineConfig(
        repo_root=tmp_path, pipeline_root=tmp_path / "pipeline", build_dir=tmp_path / "build",
        codex_home=tmp_path / "home", sandbox="none",
        write_policy="off", session=SessionConfig(idle_timeout_sec=idle, hard_deadline_sec=hard),
    )
    models = ModelConfig(api_key_env="K", presets={}, stages={}, codex_bin=str(FAKE_CODEX))
    return CodexExecBackend(pipeline_config=config, model_config=models, query=1, stage="implementation",
                            art_dir=tmp_path / "art" if art else None)


def _request(message="do it", **metadata):
    from accel_pipeline.backends.base import StageRunRequest

    return StageRunRequest(stage="implementation", instructions="INSTRUCTIONS", user_message=message,
                           agent_name="a", model_id="m", metadata=metadata)


def test_fake_codex_finishes_and_reports_its_session(tmp_path, monkeypatch):
    result = _fake_backend(tmp_path, monkeypatch).run_stage(_request())
    assert result.status == "finished"
    assert result.metadata["session_id"] == "11111111-2222-3333-4444-555555555555"
    assert "INSTRUCTIONS" in (tmp_path / "prompts.log").read_text()


def test_idle_timeout_kills_a_silent_codex_and_keeps_its_session(tmp_path, monkeypatch):
    result = _fake_backend(tmp_path, monkeypatch, mode="sleep", idle=1).run_stage(_request())
    assert result.status == "error" and result.metadata["timed_out"]
    assert "idle" in result.error
    assert result.metadata["session_id"] == "11111111-2222-3333-4444-555555555555"
    assert result.metadata["duration_ms"] < 15_000


def test_hard_deadline_kills_even_a_busy_codex(tmp_path, monkeypatch):
    result = _fake_backend(tmp_path, monkeypatch, mode="chatty", idle=30, hard=1).run_stage(_request())
    assert result.metadata["timed_out"] and "hard deadline" in result.error


def test_activity_defers_the_idle_timeout(tmp_path, monkeypatch):
    result = _fake_backend(tmp_path, monkeypatch, mode="chatty", idle=2).run_stage(_request())
    assert result.status == "finished"


def test_heartbeat_counts_as_activity(tmp_path, monkeypatch):
    import threading
    import time

    from accel_pipeline.session import heartbeat

    stop = threading.Event()
    (tmp_path / "art").mkdir()

    def beat():
        while not stop.is_set():
            heartbeat(tmp_path / "art")
            time.sleep(0.2)

    thread = threading.Thread(target=beat)
    thread.start()
    try:
        monkeypatch.setenv("FAKE_CODEX_MODE", "chatty")
        backend = _fake_backend(tmp_path, monkeypatch, mode="sleep", idle=1, hard=3)
        result = backend.run_stage(_request())
    finally:
        stop.set()
        thread.join()
    assert "hard deadline" in result.error  # silent for 3s, never idle: only the hard deadline ended it


def test_resume_sends_only_the_message_and_continues_the_session(tmp_path, monkeypatch):
    sid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
    _fake_backend(tmp_path, monkeypatch).run_stage(_request("fix the gate", resume_session_id=sid))
    args = (tmp_path / "args.log").read_text()
    assert "exec resume" in args and sid in args
    prompt = (tmp_path / "prompts.log").read_text()
    assert "fix the gate" in prompt and "INSTRUCTIONS" not in prompt


def test_phase_change_sends_the_instructions_once_under_a_header(tmp_path, monkeypatch):
    sid = "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee"
    _fake_backend(tmp_path, monkeypatch).run_stage(_request("next", resume_session_id=sid, phase_change=True))
    prompt = (tmp_path / "prompts.log").read_text()
    assert "NEW PHASE" in prompt and "INSTRUCTIONS" in prompt and "next" in prompt


def test_agent_run_file_marks_the_run_for_the_budget_footer(tmp_path, monkeypatch):
    (tmp_path / "art").mkdir()
    backend = _fake_backend(tmp_path, monkeypatch, mode="chatty")
    seen = []
    original = backend._supervise

    def spy(*args, **kwargs):
        import threading

        threading.Timer(0.5, lambda: seen.append((tmp_path / "art" / "agent_run.json").is_file())).start()
        return original(*args, **kwargs)

    backend._supervise = spy
    backend.run_stage(_request())
    assert seen == [True] and not (tmp_path / "art" / "agent_run.json").exists()


def test_query_stages_get_the_artifact_dir_so_their_progress_clock_runs(monkeypatch, tmp_path):
    """analysis and test_suite are watched like the implementation stages: the backend knows the query's artifact dir."""
    from types import SimpleNamespace

    from accel_pipeline import backends

    monkeypatch.setattr(backends, "load_run_workload", lambda cfg, name: "wl")
    monkeypatch.setattr(backends, "query_artifacts_dir", lambda root, q, workload=None: tmp_path / f"q{q}")
    cfg = SimpleNamespace(repo_root=tmp_path, sandbox="none")
    for stage in ("analysis", "test_suite", "implementation", "audit"):
        b = backends.create_agent_backend(provider="codex_exec", pipeline_config=cfg, query=3, stage=stage,
                                          model_config=SimpleNamespace(), workload_name="w")
        assert b._art_dir == tmp_path / "q3", stage
    for stage in ("host_bootstrap", "validation", "architecture_audit"):
        b = backends.create_agent_backend(provider="codex_exec", pipeline_config=cfg, query=3, stage=stage,
                                          model_config=SimpleNamespace(), workload_name="w")
        assert b._art_dir is None, stage
