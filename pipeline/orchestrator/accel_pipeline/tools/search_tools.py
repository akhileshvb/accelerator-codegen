"""Fast code search tools: ripgrep (rip_grep) and ast-grep (ast_grep)."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path
from typing import Callable

from accel_pipeline.config import PipelineConfig
from accel_pipeline.tools.tool_args import none_default as _none_default

_MAX_OUTPUT_CHARS = 24_000


def _truncate(text: str, limit: int = _MAX_OUTPUT_CHARS) -> str:
    if len(text) <= limit:
        return text
    return text[: limit - 80] + f"\n... [truncated {len(text) - limit} chars]"


def _resolve_rg_bin() -> str:
    for candidate in (
        os.environ.get("RG_BIN"),
        shutil.which("rg"),
        "/home/akhilvb/.cursor-server/bin/linux-x64/042b3c1a4c53f2c3808067f519fbfc67b72cad80/node_modules/@vscode/ripgrep/bin/rg",
    ):
        if candidate and Path(candidate).is_file():
            return candidate
    return "rg"


def _is_ast_grep_binary(path: str) -> bool:
    try:
        proc = subprocess.run(
            [path, "--version"],
            capture_output=True,
            text=True,
            check=False,
            timeout=5,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False
    combined = f"{proc.stdout}\n{proc.stderr}".lower()
    return "ast-grep" in combined or "astgrep" in combined


def _resolve_ast_grep_bin() -> str | None:
    candidates: list[str] = []
    env_bin = os.environ.get("AST_GREP_BIN")
    if env_bin:
        candidates.append(env_bin)
    which = shutil.which("ast-grep")
    if which:
        candidates.append(which)
    home = Path.home()
    candidates.extend(
        str(p)
        for p in (
            home / ".cargo" / "bin" / "ast-grep",
            home / ".local" / "bin" / "ast-grep",
        )
        if p.is_file()
    )
    for candidate in candidates:
        if _is_ast_grep_binary(candidate):
            return candidate
    return None


def _resolve_search_root(pipeline_config: PipelineConfig, path: str) -> Path:
    root = pipeline_config.repo_root
    target = Path(path)
    if not target.is_absolute():
        target = root / path
    resolved = target.resolve()
    repo = root.resolve()
    if repo not in resolved.parents and resolved != repo:
        raise PermissionError(f"path outside repo: {path}")
    return resolved


def make_search_tools(pipeline_config: PipelineConfig) -> tuple[Callable[..., str], Callable[..., str]]:
    rg_bin = _resolve_rg_bin()

    def rip_grep(
        pattern: str,
        path: str | None = ".",
        glob: str | None = None,
        context: int | None = 2,
        max_matches: int | None = 24,
        case_insensitive: bool | None = False,
    ) -> str:
        """Fast repo search with ripgrep. Use on reference aux modules (read-only snippets)."""
        path = str(_none_default(path, "."))
        context = int(_none_default(context, 2))
        max_matches = int(_none_default(max_matches, 24))
        case_insensitive = bool(_none_default(case_insensitive, False))
        if context < 0:
            raise ValueError("context must be >= 0")
        if max_matches < 1:
            raise ValueError("max_matches must be >= 1")

        root = _resolve_search_root(pipeline_config, path)
        cmd: list[str] = [
            rg_bin,
            "-n",
            "--no-follow",
            "--max-count",
            str(max_matches),
            "-C",
            str(context),
        ]
        if case_insensitive:
            cmd.append("-i")
        if glob:
            cmd.extend(["-g", str(glob)])
        cmd.extend([pattern, str(root)])

        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        output = proc.stdout or ""
        if proc.returncode not in (0, 1):
            err = proc.stderr or output or "(no output)"
            return f"rip_grep failed (exit={proc.returncode})\n{_truncate(err)}"
        if not output.strip():
            hint = f"(no matches) pattern={pattern!r} path={path}"
            if glob:
                hint += f" glob={glob!r}"
            return hint
        header = f"rip_grep: {max_matches} max matches, ±{context} context, path={path}\n"
        return _truncate(header + output)

    def ast_grep(
        pattern: str,
        path: str | None = ".",
        lang: str | None = "cpp",
        glob: str | None = None,
        max_matches: int | None = 20,
    ) -> str:
        """Structural AST search (ast-grep). Prefer for finding classes, functions, call sites."""
        path = str(_none_default(path, "."))
        lang = str(_none_default(lang, "cpp"))
        max_matches = int(_none_default(max_matches, 20))
        ast_bin = _resolve_ast_grep_bin()
        if not ast_bin:
            return (
                "ast_grep unavailable: install ast-grep (https://ast-grep.github.io/) "
                "or set AST_GREP_BIN. Use rip_grep for text search meanwhile."
            )

        root = _resolve_search_root(pipeline_config, path)
        cmd: list[str] = [
            ast_bin,
            "run",
            "--pattern",
            pattern,
            "--lang",
            lang,
            "--json=stream",
        ]
        if glob:
            cmd.extend(["--globs", str(glob)])
        cmd.append(str(root))

        proc = subprocess.run(cmd, capture_output=True, text=True, check=False)
        raw = proc.stdout or proc.stderr or ""
        if proc.returncode not in (0, 1) and not raw.strip():
            return f"ast_grep failed (exit={proc.returncode})\n{_truncate(raw)}"

        lines = [line for line in raw.splitlines() if line.strip()]
        if not lines:
            return f"(no ast matches) pattern={pattern!r} path={path} lang={lang}"

        if len(lines) > max_matches:
            lines = lines[:max_matches]
            lines.append(f"(truncated at {max_matches} matches)")

        header = f"ast_grep: lang={lang} path={path}\n"
        return _truncate(header + "\n".join(lines))

    return rip_grep, ast_grep
