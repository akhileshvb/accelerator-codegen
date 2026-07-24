from __future__ import annotations

import fnmatch
from dataclasses import dataclass
from pathlib import Path

from accel_pipeline.config import PipelineConfig
from accel_pipeline.playbook import append_patch_log


@dataclass
class ToolContext:
    pipeline_config: PipelineConfig
    query: int


def _resolve_path(ctx: ToolContext, path: str) -> Path:
    p = Path(path)
    if not p.is_absolute():
        p = ctx.pipeline_config.repo_root / p
    resolved = p.resolve()
    root = ctx.pipeline_config.repo_root.resolve()
    if root not in resolved.parents and resolved != root:
        raise PermissionError(f"path outside repo: {path}")
    return resolved


def _is_writable(ctx: ToolContext, path: Path) -> bool:
    rel = str(path.relative_to(ctx.pipeline_config.repo_root))
    for pattern in ctx.pipeline_config.write_paths:
        if fnmatch.fnmatch(rel, pattern):
            return True
    return False


def make_repo_tools(ctx: ToolContext):
    def read_file(path: str, offset: int = 0, limit: int = 0) -> str:
        """Read a file under the repo. Optional offset/limit (line-based)."""
        target = _resolve_path(ctx, path)
        lines = target.read_text(encoding="utf-8").splitlines()
        if offset or limit:
            end = offset + limit if limit else None
            lines = lines[offset:end]
        return "\n".join(lines)

    def write_file(path: str, content: str) -> str:
        """Create or overwrite a file in allowlisted paths."""
        target = _resolve_path(ctx, path)
        if not _is_writable(ctx, target):
            raise PermissionError(f"write not allowed: {path}")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(content, encoding="utf-8")
        return f"wrote {len(content)} bytes to {path}"

    def apply_patch(path: str, old_string: str, new_string: str) -> str:
        """Replace exactly one occurrence of old_string with new_string."""
        target = _resolve_path(ctx, path)
        if not _is_writable(ctx, target):
            raise PermissionError(f"write not allowed: {path}")
        text = target.read_text(encoding="utf-8")
        count = text.count(old_string)
        if count != 1:
            raise ValueError(f"old_string must match exactly once (found {count})")
        updated = text.replace(old_string, new_string, 1)
        target.write_text(updated, encoding="utf-8")
        append_patch_log(
            ctx.pipeline_config.repo_root,
            ctx.query,
            f"apply_patch {path} (-{len(old_string)} +{len(new_string)})",
        )
        return f"patched {path}"

    def delete_file(path: str) -> str:
        """Delete a file in allowlisted paths."""
        target = _resolve_path(ctx, path)
        if not _is_writable(ctx, target):
            raise PermissionError(f"delete not allowed: {path}")
        if target.is_file():
            target.unlink()
        return f"deleted {path}"

    def list_dir(path: str, glob: str = "*") -> str:
        """List files in a directory."""
        target = _resolve_path(ctx, path)
        if not target.is_dir():
            raise FileNotFoundError(path)
        entries = sorted(p.name for p in target.glob(glob))
        return "\n".join(entries)

    return read_file, write_file, apply_patch, delete_file, list_dir
