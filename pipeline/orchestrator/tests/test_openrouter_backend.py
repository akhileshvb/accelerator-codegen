from __future__ import annotations

import os
from unittest.mock import MagicMock

import pytest

from accel_pipeline.backends import create_agent_backend
from accel_pipeline.backends.openai_agents import require_litellm
from accel_pipeline.config import ModelConfig, PipelineConfig, load_model_config
from accel_pipeline.models import make_model_id, resolve_agent_model
from accel_pipeline.tools.tool_policy import STAGE_TOOL_NAMES, build_stage_tool_registry


def test_openrouter_model_config_loads():
    from accel_pipeline.paths import find_repo_root

    repo = find_repo_root()
    cfg = load_model_config(repo / "pipeline" / "config" / "models.openrouter.yaml", repo_root=repo)
    assert cfg.provider == "openrouter"
    assert cfg.api_key_env == "OPENROUTER_API_KEY"
    assert cfg.stages["analysis"] == "reasoning"
    assert cfg.stages["implementation"] in cfg.presets
    assert cfg.presets[cfg.stages["analysis"]]


def test_resolve_agent_model_openrouter_prefixes_model():
    model_config = ModelConfig(
        provider="openrouter",
        api_key_env="OPENROUTER_API_KEY",
        presets={"default": "anthropic/claude-sonnet-4"},
        stages={"analysis": "default"},
    )
    os.environ["OPENROUTER_API_KEY"] = "test-key"
    try:
        model = resolve_agent_model("analysis", MagicMock(), model_config)
    finally:
        os.environ.pop("OPENROUTER_API_KEY", None)

    assert getattr(model, "model", None) == "openrouter/anthropic/claude-sonnet-4"


def test_resolve_agent_model_openrouter_keeps_existing_prefix():
    model_config = ModelConfig(
        provider="openrouter",
        api_key_env="OPENROUTER_API_KEY",
        presets={"default": "openrouter/openai/gpt-4.1-mini"},
        stages={"analysis": "default"},
    )
    os.environ["OPENROUTER_API_KEY"] = "test-key"
    try:
        model = resolve_agent_model("analysis", MagicMock(), model_config)
    finally:
        os.environ.pop("OPENROUTER_API_KEY", None)

    assert getattr(model, "model", None) == "openrouter/openai/gpt-4.1-mini"


def test_make_model_id_returns_preset_string_for_openrouter():
    model_config = ModelConfig(
        provider="openrouter",
        api_key_env="OPENROUTER_API_KEY",
        presets={"default": "anthropic/claude-sonnet-4"},
        stages={"analysis": "default"},
    )
    os.environ["OPENROUTER_API_KEY"] = "test-key"
    try:
        assert make_model_id("analysis", MagicMock(), model_config) == "anthropic/claude-sonnet-4"
    finally:
        os.environ.pop("OPENROUTER_API_KEY", None)


def test_openrouter_backend_has_same_tools_as_cursor_registry():
    from accel_pipeline.paths import find_repo_root

    repo = find_repo_root()
    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        tpch_db_sf1=repo.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb",
        tpch_db_sf10=repo.parent / "accelerator-codegen-duckdb" / "tpch_sf10.duckdb",
    )
    model_config = ModelConfig(
        provider="openrouter",
        api_key_env="OPENROUTER_API_KEY",
        presets={"default": "anthropic/claude-sonnet-4"},
        stages={"implementation": "default"},
    )
    stage = "implementation"
    registry = build_stage_tool_registry(pipeline_config, 3, stage)
    backend = create_agent_backend(
        provider="openrouter",
        pipeline_config=pipeline_config,
        query=3,
        stage=stage,
        model_config=model_config,
    )
    assert len(backend._tools) == len(STAGE_TOOL_NAMES[stage])
    assert len(backend._tools) == len(registry)


def test_is_transient_api_error_detects_openrouter_json_failures():
    from accel_pipeline.backends.openai_agents import _is_transient_api_error

    exc = Exception(
        "Unable to get json response - Expecting value: line 337 column 1 (char 1848)"
    )
    assert _is_transient_api_error(exc)
    assert not _is_transient_api_error(ValueError("bad patch syntax"))


def test_require_litellm_raises_without_dependency(monkeypatch):
    import builtins

    real_import = builtins.__import__

    def fake_import(name, *args, **kwargs):
        if name == "litellm":
            raise ImportError("no litellm")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", fake_import)
    with pytest.raises(ImportError, match="openrouter"):
        require_litellm()


def test_wrap_tool_returns_errors_to_agent():
    from accel_pipeline.tools.pipeline_tool_wrapper import wrap_pipeline_tool

    def boom() -> str:
        raise ValueError("bad input")

    wrapped = wrap_pipeline_tool(boom, "boom")
    assert wrapped() == "ERROR: ValueError: bad input"


def test_function_tool_accepts_positional_args():
    from accel_pipeline.config import PipelineConfig
    from accel_pipeline.paths import find_repo_root
    from accel_pipeline.tools.pipeline_tool_wrapper import wrap_pipeline_tool
    from accel_pipeline.tools.repo_tools import ToolContext, make_repo_tools
    from agents import function_tool
    import asyncio

    repo = find_repo_root()
    pipeline_config = PipelineConfig(
        repo_root=repo,
        pipeline_root=repo / "pipeline",
        build_dir=repo / "build-release",
        tpch_db_sf1=repo.parent / "accelerator-codegen-duckdb" / "tpch_sf1.duckdb",
        tpch_db_sf10=repo.parent / "accelerator-codegen-duckdb" / "tpch_sf10.duckdb",
    )
    read_file, *_ = make_repo_tools(ToolContext(pipeline_config, 18))
    wrapped = wrap_pipeline_tool(read_file, "read_file")
    tool = function_tool(wrapped)

    async def invoke(arguments: str) -> str:
        from agents.tool_context import ToolContext as AgentToolContext

        ctx = AgentToolContext(
            context=None,
            tool_name="read_file",
            tool_call_id="test",
            tool_arguments=arguments,
        )
        return await tool.on_invoke_tool(ctx, arguments)

    result = asyncio.run(
        invoke('{"path":"pipeline/prompts/aux_state/shared/taxonomy_index.md"}')
    )
    assert "State family index" in result
    assert "An error occurred while running the tool" not in result

    null_args = asyncio.run(
        invoke(
            '{"path":"pipeline/prompts/aux_state/shared/taxonomy_index.md","offset":null,"limit":null}'
        )
    )
    assert "State family index" in null_args
