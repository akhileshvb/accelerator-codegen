from __future__ import annotations

import fnmatch
from dataclasses import dataclass
from pathlib import Path

from accel_pipeline.config import PipelineConfig
from accel_pipeline.playbook import append_patch_log

_MAX_READ_BYTES = 512_000


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


def _format_patch_hint(text: str, needle: str) -> str:
    lines = text.splitlines()
    for idx, line in enumerate(lines):
        if needle in line:
            start = max(0, idx - 2)
            end = min(len(lines), idx + 3)
            snippet = "\n".join(f"{i + 1}: {lines[i]}" for i in range(start, end))
            return f"near line {idx + 1}:\n{snippet}"
    return "needle not found on any single line; check whitespace and surrounding context"


def make_repo_tools(ctx: ToolContext):
    def read_file(
        path: str,
        offset: int = 0,
        limit: int = 0,
        numbered: bool = False,
    ) -> str:
        """Read a file under the repo. Optional offset/limit are 0-based line numbers."""
        target = _resolve_path(ctx, path)
        if not target.is_file():
            raise FileNotFoundError(f"file not found: {path}")
        raw = target.read_bytes()
        if b"\x00" in raw[:4096]:
            raise ValueError(f"refusing to read binary file: {path}")
        if len(raw) > _MAX_READ_BYTES:
            raise ValueError(
                f"file too large ({len(raw)} bytes, max {_MAX_READ_BYTES}): {path}; "
                "use offset/limit or glob_files to narrow scope"
            )
        lines = raw.decode("utf-8").splitlines()
        if offset or limit:
            end = offset + limit if limit else None
            lines = lines[offset:end]
        if numbered:
            start_line = offset + 1
            return "\n".join(f"{start_line + i}: {line}" for i, line in enumerate(lines))
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
        if not target.is_file():
            raise FileNotFoundError(f"file not found: {path}")
        text = target.read_text(encoding="utf-8")
        count = text.count(old_string)
        if count != 1:
            hint = _format_patch_hint(text, old_string[:120]) if old_string else ""
            raise ValueError(
                f"old_string must match exactly once (found {count}) in {path}"
                + (f"; {hint}" if hint else "")
            )
        updated = text.replace(old_string, new_string, 1)
        target.write_text(updated, encoding="utf-8")
        append_patch_log(
            ctx.pipeline_config.repo_root,
            ctx.query,
            f"apply_patch {path} (-{len(old_string)} +{len(new_string)})",
        )
        return f"patched {path}"

    def insert_lines(path: str, after_line: int, content: str) -> str:
        """Insert content after a 1-based line number. Use after_line=0 for file start."""
        target = _resolve_path(ctx, path)
        if not _is_writable(ctx, target):
            raise PermissionError(f"write not allowed: {path}")
        if after_line < 0:
            raise ValueError("after_line must be >= 0")
        if target.is_file():
            lines = target.read_text(encoding="utf-8").splitlines(keepends=True)
        else:
            lines = []
            target.parent.mkdir(parents=True, exist_ok=True)
        insert = content if content.endswith("\n") else content + "\n"
        if after_line == 0:
            updated = insert + "".join(lines)
        elif after_line > len(lines):
            raise ValueError(
                f"after_line {after_line} out of range for {path} ({len(lines)} lines)"
            )
        else:
            updated = "".join(lines[:after_line]) + insert + "".join(lines[after_line:])
        target.write_text(updated, encoding="utf-8")
        append_patch_log(
            ctx.pipeline_config.repo_root,
            ctx.query,
            f"insert_lines {path} after={after_line} (+{len(insert)} bytes)",
        )
        return f"inserted {len(insert)} bytes into {path} after line {after_line}"

    def delete_file(path: str) -> str:
        """Delete a file in allowlisted paths."""
        target = _resolve_path(ctx, path)
        if not _is_writable(ctx, target):
            raise PermissionError(f"delete not allowed: {path}")
        if target.is_file():
            target.unlink()
        return f"deleted {path}"

    def list_dir(path: str = ".", glob: str = "*") -> str:
        """List files in a directory."""
        target = _resolve_path(ctx, path)
        if not target.is_dir():
            raise FileNotFoundError(path)
        entries = sorted(p.name for p in target.glob(glob))
        if not entries:
            return f"(empty) {path}"
        return "\n".join(entries)

    def glob_files(pattern: str, path: str = ".", limit: int = 100) -> str:
        """Find files under path matching a glob pattern."""
        target = _resolve_path(ctx, path)
        if not target.is_dir():
            raise FileNotFoundError(path)
        matches: list[str] = []
        for match in sorted(target.glob(pattern)):
            rel = match.relative_to(ctx.pipeline_config.repo_root)
            matches.append(str(rel))
            if len(matches) >= limit:
                break
        if not matches:
            return f"(no matches) pattern={pattern!r} path={path}"
        suffix = "" if len(matches) < limit else f"\n(truncated at {limit} matches)"
        return "\n".join(matches) + suffix

    def file_info(path: str) -> str:
        """Return file metadata and whether writes are allowed."""
        target = _resolve_path(ctx, path)
        rel = str(target.relative_to(ctx.pipeline_config.repo_root))
        if not target.exists():
            return f"path={rel} exists=false writable={_is_writable(ctx, target)}"
        kind = "dir" if target.is_dir() else "file"
        size = target.stat().st_size if target.is_file() else 0
        return (
            f"path={rel} exists=true kind={kind} size={size} "
            f"writable={_is_writable(ctx, target)}"
        )

    return read_file, write_file, apply_patch, insert_lines, delete_file, list_dir, glob_files, file_info
