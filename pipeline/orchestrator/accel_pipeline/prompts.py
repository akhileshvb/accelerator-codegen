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
    # Concurrent pack workers: rewrite legacy build-release tokens so Codex native
    # shell (which bypasses run_pipeline_command rewrites) uses this worker's build dir.
    build_name = pipeline_config.build_dir.name
    if build_name and build_name != "build-release" and "build-release" in rendered:
        rendered = rendered.replace("build-release", build_name)
    return rendered


def with_codex_tooling(pipeline_config: PipelineConfig, prompt: str) -> str:
    """Append the Codex tooling guide unless the prompt already points at it."""
    if "codex_tooling.md" in prompt:
        return prompt
    guide = pipeline_config.pipeline_root / "prompts" / "shared" / "codex_tooling.md"
    return f"{prompt.rstrip()}\n\n{guide.read_text(encoding='utf-8')}"


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
