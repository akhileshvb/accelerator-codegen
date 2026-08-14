from __future__ import annotations

import re
from pathlib import Path

from accel_pipeline.context import build_prompt_context
from accel_pipeline.config import PipelineConfig


def resolve_prompt_path(pipeline_config: PipelineConfig, workload: dict, prompt_name: str) -> Path:
    subdir = workload.get("prompt_subdir")
    if subdir:
        path = pipeline_config.pipeline_root / "prompts" / subdir / prompt_name
        if path.is_file():
            return path
    return pipeline_config.pipeline_root / "prompts" / prompt_name


def render_prompt(
    pipeline_config: PipelineConfig,
    prompt_name: str,
    workload: dict,
    query: int,
    stage: str,
    optimization_iter: int = 0,
    extra_context: dict[str, str] | None = None,
) -> str:
    path = resolve_prompt_path(pipeline_config, workload, prompt_name)
    if not path.is_file():
        raise FileNotFoundError(path)

    template = path.read_text(encoding="utf-8")
    ctx = build_prompt_context(pipeline_config, workload, query, stage, optimization_iter)
    ctx.setdefault("analysis_revision_request", "")
    if extra_context:
        ctx.update(extra_context)

    def repl(match: re.Match[str]) -> str:
        key = match.group(1)
        return ctx.get(key, match.group(0))

    rendered = re.sub(r"\{\{(\w+)\}\}", repl, template)
    return rendered


def save_rendered_prompt(
    pipeline_config: PipelineConfig,
    query: int,
    stage: str,
    content: str,
    optimization_iter: int = 0,
) -> Path:
    prompts_dir = pipeline_config.pipeline_root / "artifacts" / f"q{query}" / "prompts"
    prompts_dir.mkdir(parents=True, exist_ok=True)
    suffix = f"_{optimization_iter}" if stage.startswith("optimization") else ""
    out = prompts_dir / f"{stage}{suffix}.md"
    out.write_text(content, encoding="utf-8")
    return out
