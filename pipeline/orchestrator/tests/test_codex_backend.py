"""Tests for stage provider resolution and Codex/MCP wiring."""

from __future__ import annotations

from pathlib import Path

import pytest

from accel_pipeline.config import ModelConfig, load_model_config
from accel_pipeline.models import resolve_stage_provider
from accel_pipeline.optimization_worker import worker_enabled
from accel_pipeline.config import PipelineConfig


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_resolve_stage_provider_codex_overrides():
    cfg = load_model_config(REPO_ROOT / "pipeline" / "config" / "models.codex.yaml", repo_root=REPO_ROOT)
    assert cfg.provider == "openrouter"
    assert resolve_stage_provider("analysis", cfg) == "openrouter"
    assert resolve_stage_provider("audit", cfg) == "openrouter"
    assert resolve_stage_provider("implementation", cfg) == "codex_exec"
    assert resolve_stage_provider("optimization_worker", cfg) == "codex_exec"
    assert resolve_stage_provider("optimization_unified_hypothesis", cfg) == "codex_exec"


def test_worker_enabled_for_codex_exec():
    model = ModelConfig(
        provider="openrouter",
        api_key_env="OPENROUTER_API_KEY",
        presets={"code": "x"},
        stages={"optimization_worker": "code"},
        stage_providers={"optimization_worker": "codex_exec"},
    )
    pc = PipelineConfig(
        repo_root=REPO_ROOT,
        pipeline_root=REPO_ROOT / "pipeline",
        build_dir=REPO_ROOT / "build-release",
        tpch_db_sf1=REPO_ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb",
        tpch_db_sf10=REPO_ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf10.duckdb",
        optimization_persistent_worker=True,
    )
    assert worker_enabled(pc, model) is True


def test_worker_disabled_for_cursor():
    model = ModelConfig(
        provider="cursor",
        api_key_env="CURSOR_API_KEY",
        presets={"default": "x"},
        stages={},
    )
    pc = PipelineConfig(
        repo_root=REPO_ROOT,
        pipeline_root=REPO_ROOT / "pipeline",
        build_dir=REPO_ROOT / "build-release",
        tpch_db_sf1=REPO_ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb",
        tpch_db_sf10=REPO_ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf10.duckdb",
        optimization_persistent_worker=True,
    )
    assert worker_enabled(pc, model) is False


def test_mcp_server_uses_modern_sdk():
    """mcp>=2 removed FastMCP; server must construct via MCPServer (or legacy FastMCP)."""
    import accel_pipeline.mcp_server as mod

    # Import path used by main() must succeed in this env.
    try:
        from mcp.server import MCPServer

        server = MCPServer("accel_pipeline_test")
        assert server is not None
    except ImportError:
        from mcp.server.fastmcp import FastMCP

        server = FastMCP("accel_pipeline_test")
        assert server is not None
    assert hasattr(mod, "build_mcp_tool_callables")
    assert hasattr(mod, "main")


def test_codex_mcp_launcher_exists():
    launcher = REPO_ROOT / "pipeline" / ".codex" / "run_mcp_server.sh"
    assert launcher.is_file()
    assert launcher.stat().st_mode & 0o111
    cfg = (REPO_ROOT / "pipeline" / ".codex" / "config.toml").read_text(encoding="utf-8")
    assert "run_mcp_server.sh" in cfg


def test_mcp_tool_callables_include_profile_digest(tmp_path):
    from accel_pipeline.mcp_server import build_mcp_tool_callables
    from accel_pipeline.config import load_pipeline_config, load_workload, apply_workload_overrides
    from accel_pipeline.paths import query_artifacts_dir

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q3", REPO_ROOT)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    art = query_artifacts_dir(REPO_ROOT, 3, workload=workload)
    art.mkdir(parents=True, exist_ok=True)
    (art / "profile_summary.json").write_text('{"dominant_phase":"agg"}', encoding="utf-8")
    tools = build_mcp_tool_callables(
        pipeline_config, workload, "tpch_q3", 3, "optimization_worker", 1, art
    )
    assert "get_profile_digest" in tools
    assert "run_fairness" in tools
    digest = tools["get_profile_digest"]()
    assert "dominant_phase" in digest or "agg" in digest


def test_codex_exec_sets_codex_home(monkeypatch, tmp_path):
    from accel_pipeline.backends.codex_exec import CodexExecBackend
    from accel_pipeline.backends.base import StageRunRequest
    from accel_pipeline.config import load_pipeline_config, load_model_config

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    model = load_model_config(REPO_ROOT / "pipeline" / "config" / "models.codex.yaml", repo_root=REPO_ROOT)
    backend = CodexExecBackend(
        pipeline_config=pipeline_config,
        model_config=model,
        query=16,
        stage="implementation",
        workload_name="tpch_q16_aux",
    )

    captured: dict = {}

    def fake_run(cmd, **kwargs):
        captured["cmd"] = cmd
        captured["env"] = kwargs.get("env") or {}

        class Proc:
            returncode = 0
            stdout = "ok"
            stderr = ""

        return Proc()

    monkeypatch.setattr("accel_pipeline.backends.codex_exec.subprocess.run", fake_run)
    monkeypatch.setattr(
        "accel_pipeline.backends.codex_exec.shutil.which",
        lambda name: "/usr/bin/codex" if name == "codex" else None,
    )

    result = backend.run_stage(
        StageRunRequest(
            stage="implementation",
            instructions="sys",
            user_message="user",
            agent_name="impl",
            model_id="deepseek/deepseek-v4-pro",
            agent_model="deepseek/deepseek-v4-pro",
            max_turns=1,
        )
    )
    assert result.status == "finished"
    expected_home = str((REPO_ROOT / "pipeline" / ".codex").resolve())
    assert captured["env"].get("CODEX_HOME") == expected_home
    assert 'model_provider="openrouter"' in captured["cmd"]
    assert any("mcp_servers.accel_pipeline.env=" in str(x) for x in captured["cmd"])
    assert "--dangerously-bypass-approvals-and-sandbox" in captured["cmd"]
    sidecar = REPO_ROOT / "pipeline" / ".codex" / "accel_pipeline_mcp.env"
    assert sidecar.is_file()
    assert "ACCEL_PIPELINE_QUERY=16" in sidecar.read_text(encoding="utf-8")
