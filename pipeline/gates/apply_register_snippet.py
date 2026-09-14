#!/usr/bin/env python3
"""Idempotently insert forward decl + RegisterTableFunction into scaffold markers.

DuckDB host: patches `src/runtime/duckdb/register.cpp` with TableFunctionSet snippets.
Other engines: write a placeholder note under scaffold artifacts; do not invent adapters.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path


def _patch_block(text: str, begin: str, end: str, line: str) -> str:
    pattern = re.compile(
        rf"({re.escape(begin)}\n)(.*?)({re.escape(end)})",
        re.DOTALL,
    )
    stripped_line = line.strip()

    def repl(match: re.Match[str]) -> str:
        inner = match.group(2)
        if stripped_line in inner:
            return match.group(0)
        addition = f"{line.rstrip()}\n"
        if inner.strip():
            new_inner = inner.rstrip("\n") + "\n" + addition
        else:
            new_inner = addition
        return match.group(1) + new_inner + match.group(3)

    new_text, count = pattern.subn(repl, text, count=1)
    if count != 1:
        raise RuntimeError(f"marker block not found: {begin} ... {end}")
    return new_text


def apply_duckdb_snippet(register_path: Path, create_fn: str, marker: str) -> None:
    text = register_path.read_text(encoding="utf-8")
    forward_begin = f"// BEGIN_{marker}_REGISTRATIONS"
    forward_end = f"// END_{marker}_REGISTRATIONS"
    register_begin = f"        // BEGIN_{marker}_REGISTRATIONS"
    register_end = f"        // END_{marker}_REGISTRATIONS"

    forward = f"duckdb::TableFunctionSet {create_fn}();"
    register = f"        RegisterTableFunction(catalog, context, {create_fn}());"

    text = _patch_block(text, forward_begin, forward_end, forward)
    text = _patch_block(text, register_begin, register_end, register)
    register_path.write_text(text, encoding="utf-8")


def write_non_duckdb_placeholder(
    note_path: Path,
    *,
    engine_name: str,
    create_fn: str,
    register_path: str,
) -> None:
    note_path.parent.mkdir(parents=True, exist_ok=True)
    note_path.write_text(
        "\n".join(
            [
                f"// AUTO-GENERATED scaffold note for engine={engine_name}",
                f"// Query entrypoint symbol (conventional): {create_fn}",
                f"// Expected host registration surface: {register_path}",
                "//",
                "// Accelerator implementation should extend this host (SRFs/UDFs +",
                "// differential) per pipeline/prompts/shared/query_host_bootstrap.md.",
                "// Do not invent a parallel adapter tree — reuse engine_backend",
                "// runtime|stateful adapters. Auto-apply snippets are duckdb-only.",
                "",
            ]
        ),
        encoding="utf-8",
    )


def apply_snippet(
    repo_root: Path,
    create_fn: str,
    marker: str,
    *,
    engine_name: str = "duckdb",
    register_rel: str | None = None,
    note_path: Path | None = None,
) -> dict:
    engine = (engine_name or "duckdb").strip().lower()
    if engine == "duckdb":
        rel = register_rel or "src/runtime/duckdb/register.cpp"
        register_path = repo_root / rel
        # Legacy fallback used by older checkouts.
        if not register_path.is_file():
            legacy = repo_root / "src" / "register.cpp"
            if legacy.is_file():
                register_path = legacy
        if not register_path.is_file():
            raise FileNotFoundError(f"register.cpp not found: {register_path}")
        apply_duckdb_snippet(register_path, create_fn, marker)
        return {"engine_name": engine, "applied": True, "register_path": str(register_path)}

    rel = register_rel or f"src/generated/{engine}/"
    note = note_path or (repo_root / "pipeline" / "artifacts" / "_scaffold" / f"{engine}_register_note.cpp")
    write_non_duckdb_placeholder(
        note,
        engine_name=engine,
        create_fn=create_fn,
        register_path=rel,
    )
    return {
        "engine_name": engine,
        "applied": False,
        "register_path": rel,
        "note_path": str(note),
        "message": (
            f"Skipped auto-apply for engine={engine}; host must expose entrypoint "
            f"(see {note})."
        ),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("create_fn", type=str)
    parser.add_argument("marker", type=str, help="e.g. SEC_EDGAR")
    parser.add_argument(
        "--engine-name",
        default="duckdb",
        help="Host engine (default duckdb). Non-duckdb writes a placeholder note.",
    )
    parser.add_argument(
        "--register-path",
        default="",
        help="Optional repo-relative register file override",
    )
    parser.add_argument(
        "--note-path",
        type=Path,
        default=None,
        help="Where to write the non-duckdb placeholder note",
    )
    args = parser.parse_args()
    try:
        result = apply_snippet(
            args.repo_root,
            args.create_fn,
            args.marker,
            engine_name=args.engine_name,
            register_rel=args.register_path or None,
            note_path=args.note_path,
        )
    except (FileNotFoundError, RuntimeError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
