from __future__ import annotations

import re
from pathlib import Path

from accel_pipeline.context import build_prompt_context
from accel_pipeline.config import PipelineConfig


def render_prompt(
    pipeline_config: PipelineConfig,
    prompt_name: str,
    workload: dict,
    query: int,
    stage: str,
    optimization_iter: int = 0,
) -> str:
    path = pipeline_config.pipeline_root / "prompts" / prompt_name
    if not path.is_file():
        raise FileNotFoundError(path)

    template = path.read_text(encoding="utf-8")
    ctx = build_prompt_context(pipeline_config, workload, query, stage, optimization_iter)

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
    suffix = f"_{optimization_iter}" if stage == "optimization" else ""
    out = prompts_dir / f"{stage}{suffix}.md"
    out.write_text(content, encoding="utf-8")
    return out
