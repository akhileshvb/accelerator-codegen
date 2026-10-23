"""Tests for stage provider resolution and Codex/MCP wiring."""

from __future__ import annotations

import json
from dataclasses import replace
from pathlib import Path

import pytest

from accel_pipeline.config import ModelConfig, load_model_config
from accel_pipeline.models import resolve_preset, resolve_stage_provider
from accel_pipeline.optimization_worker import worker_enabled
from accel_pipeline.config import PipelineConfig


REPO_ROOT = Path(__file__).resolve().parents[3]


def test_resolve_stage_provider_codex_overrides():
    cfg = load_model_config(REPO_ROOT / "pipeline" / "config" / "models.yaml", repo_root=REPO_ROOT)
    assert resolve_stage_provider("analysis", cfg) == "codex_exec"
    assert resolve_stage_provider("audit", cfg) == "codex_exec"
    assert resolve_stage_provider("implementation", cfg) == "codex_exec"
    assert resolve_stage_provider("impl_capture", cfg) == "codex_exec"
    assert resolve_stage_provider("impl_host", cfg) == "codex_exec"
    assert resolve_stage_provider("validation", cfg) == "codex_exec"
    assert resolve_stage_provider("optimization_worker", cfg) == "codex_exec"
    assert resolve_stage_provider("optimization_unified_hypothesis", cfg) == "codex_exec"
    assert resolve_stage_provider("crash_debug", cfg) == "codex_exec"
    assert resolve_preset("impl_capture", cfg) == "openai/gpt-6-luna-pro"
    assert resolve_preset("implementation", cfg) == "openai/gpt-6-luna-pro"
    assert resolve_stage_provider("critique_store", cfg) == "codex_exec"
    assert resolve_stage_provider("layer_critique", cfg) == "codex_exec"
    assert resolve_preset("critique_store", cfg) == "openai/gpt-6-luna"
    assert resolve_preset("layer_critique", cfg) == "openai/gpt-6-luna"
    assert resolve_stage_provider("critique_host_bootstrap", cfg) == "codex_exec"
    assert resolve_preset("critique_host_bootstrap", cfg) == "openai/gpt-6-luna"


def test_unknown_stage_provider_raises():
    cfg = load_model_config(REPO_ROOT / "pipeline" / "config" / "models.yaml", repo_root=REPO_ROOT)
    with pytest.raises(KeyError, match="no_such_stage"):
        resolve_stage_provider("no_such_stage", cfg)


def _pipeline_config(**overrides) -> PipelineConfig:
    return PipelineConfig(
        repo_root=REPO_ROOT,
        pipeline_root=REPO_ROOT / "pipeline",
        build_dir=REPO_ROOT / "build-release",
        tpch_db_sf1=REPO_ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb",
        tpch_db_sf10=REPO_ROOT.parent / "accelerator-codegen-duckdb" / "tpch_sf10.duckdb",
        **overrides,
    )


def test_worker_enabled_follows_persistent_worker_flag():
    assert worker_enabled(_pipeline_config(optimization_persistent_worker=True)) is True
    assert worker_enabled(_pipeline_config(optimization_persistent_worker=False)) is False


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


def test_codex_config_template_and_launcher_are_committed():
    launcher = REPO_ROOT / "pipeline" / "codex" / "run_mcp_server.sh"
    assert launcher.is_file()
    assert launcher.stat().st_mode & 0o111
    template = (REPO_ROOT / "pipeline" / "codex" / "config.template.toml").read_text(encoding="utf-8")
    assert 'model_provider = "openrouter"' in template
    assert "{{mcp_launcher}}" in template
    assert "required = true" in template


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


def test_mcp_tool_callables_include_profile_digest(tmp_path):
    from accel_pipeline.mcp_server import build_mcp_tool_callables
    from accel_pipeline.config import load_pipeline_config, load_workload, apply_workload_overrides
    from accel_pipeline.paths import query_artifacts_dir

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q3", REPO_ROOT, engine="duckdb")
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    art = query_artifacts_dir(REPO_ROOT, 3, workload=workload)
    art.mkdir(parents=True, exist_ok=True)
    (art / "profile_summary.json").write_text('{"dominant_phase":"agg"}', encoding="utf-8")
    tools = build_mcp_tool_callables(
        pipeline_config, workload, "tpch_q3", 3, "optimization_worker", 1, art
    )
    assert "get_profile_digest" in tools
    assert "run_fairness" in tools
    assert "run_correctness" in tools
    assert "run_multi_connection" in tools
    digest = tools["get_profile_digest"]()
    assert "dominant_phase" in digest or "agg" in digest


def test_mcp_aux_tools_include_multi_connection(tmp_path):
    from accel_pipeline.mcp_server import build_mcp_tool_callables
    from accel_pipeline.config import load_pipeline_config, load_workload, apply_workload_overrides
    from accel_pipeline.paths import query_artifacts_dir

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("tpch_q14_aux", REPO_ROOT, engine="duckdb")
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    art = query_artifacts_dir(REPO_ROOT, 14, workload=workload)
    art.mkdir(parents=True, exist_ok=True)
    tools = build_mcp_tool_callables(
        pipeline_config, workload, "tpch_q14_aux", 14, "implementation", 0, art
    )
    assert "run_multi_connection" in tools
    # Non-aux skip path is covered by tpch_q3 callables; aux must map to named gate helper.
    assert callable(tools["run_correctness"])
    assert callable(tools["run_multi_connection"])


def test_codex_exec_sets_codex_home(monkeypatch, tmp_path):
    from accel_pipeline.backends.codex_exec import CodexExecBackend
    from accel_pipeline.backends.base import StageRunRequest
    from accel_pipeline.config import load_pipeline_config, load_model_config

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    pipeline_config = replace(pipeline_config, write_policy="off")  # the fake subprocess.run sees only codex
    model = load_model_config(REPO_ROOT / "pipeline" / "config" / "models.yaml", repo_root=REPO_ROOT)
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
    assert not any("enabled_tools" in str(x) for x in captured["cmd"])


def test_mcp_env_sidecar_quotes_values_with_spaces(monkeypatch, tmp_path):
    from accel_pipeline.backends.codex_exec import CodexExecBackend, sidecar_env_line
    from accel_pipeline.config import load_pipeline_config, load_model_config

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


def test_resolve_query_zero_is_valid(monkeypatch):
    import argparse
    from accel_pipeline.mcp_server import _resolve_query

    monkeypatch.setenv("ACCEL_PIPELINE_QUERY", "0")
    args = argparse.Namespace(query=0)
    assert _resolve_query(args) == 0


def test_codex_exec_whitelists_engine_impl_mcp_tools(monkeypatch):
    from accel_pipeline.backends.codex_exec import CodexExecBackend
    from accel_pipeline.backends.base import StageRunRequest
    from accel_pipeline.config import load_pipeline_config, load_model_config

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    pipeline_config = replace(pipeline_config, write_policy="off")  # the fake subprocess.run sees only codex
    model = load_model_config(REPO_ROOT / "pipeline" / "config" / "models.yaml", repo_root=REPO_ROOT)
    backend = CodexExecBackend(
        pipeline_config=pipeline_config,
        model_config=model,
        query=0,
        stage="impl_epochs",
        workload_name="engine_postgres",
    )
    captured: dict = {}

    def fake_run(cmd, **kwargs):
        captured["cmd"] = cmd

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
            stage="impl_epochs",
            instructions="sys",
            user_message="user",
            agent_name="impl",
            model_id="deepseek/deepseek-v4-pro",
            agent_model="deepseek/deepseek-v4-pro",
            max_turns=1,
        )
    )
    assert result.status == "finished"
    enabled = next(
        str(x) for x in captured["cmd"] if "enabled_tools" in str(x)
    )
    assert "request_gate" in enabled
    assert "run_engine_build" in enabled
    assert "run_layer_gate" in enabled
    assert "run_quick_benchmark" not in enabled
    sidecar = REPO_ROOT / "pipeline" / ".codex" / "accel_pipeline_mcp.env"
    assert "ACCEL_PIPELINE_STAGE=impl_epochs" in sidecar.read_text(encoding="utf-8")


def test_impl_stage_tools_include_layer_gates():
    from accel_pipeline.tools.gate_tools import layer_gate_from_stage
    from accel_pipeline.backends.codex_exec import CodexExecBackend

    assert layer_gate_from_stage("impl_epochs") == "engine_epochs"
    assert layer_gate_from_stage("impl_scan") == "engine_scan"
    assert layer_gate_from_stage("implementation") is None
    names = CodexExecBackend._engine_impl_mcp_tools("impl_epochs")
    assert {"request_gate", "run_engine_build", "run_layer_gate"} <= set(names)
    assert CodexExecBackend._engine_impl_mcp_tools("implementation") is None


def test_mcp_run_layer_gate_maps_impl_stage(monkeypatch, tmp_path):
    from accel_pipeline.mcp_server import build_mcp_tool_callables
    from accel_pipeline.config import load_pipeline_config, load_workload, apply_workload_overrides
    from accel_pipeline.paths import query_artifacts_dir
    from accel_pipeline.gates import GateResult

    pipeline_config = load_pipeline_config(repo_root=REPO_ROOT)
    workload = load_workload("engine_postgres", REPO_ROOT)
    pipeline_config = apply_workload_overrides(pipeline_config, workload)
    art = query_artifacts_dir(REPO_ROOT, 0, workload=workload)
    called: list[str] = []

    def fake_run_gate(pc, gate_name, query, **kwargs):
        called.append(gate_name)
        return GateResult(
            name=gate_name,
            status="pass",
            data={"status": "pass", "notes": "ok"},
            path=tmp_path / f"{gate_name}.json",
        )

    monkeypatch.setattr("accel_pipeline.tools.gate_tools.run_gate", fake_run_gate)
    tools = build_mcp_tool_callables(
        pipeline_config, workload, "engine_postgres", 0, "impl_epochs", 0, art
    )
    assert "run_engine_build" in tools
    assert "run_layer_gate" in tools
    build_out = json.loads(tools["run_engine_build"]())
    layer_out = json.loads(tools["run_layer_gate"]())
    assert called == ["engine_build", "engine_epochs"]
    assert build_out["gate"] == "engine_build"
    assert layer_out["gate"] == "engine_epochs"
