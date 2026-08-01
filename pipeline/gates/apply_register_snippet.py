#!/usr/bin/env python3
"""Idempotently insert forward decl + RegisterTableFunction into scaffold markers."""

from __future__ import annotations

import argparse
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


def apply_snippet(register_path: Path, create_fn: str, marker: str) -> None:
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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("repo_root", type=Path)
    parser.add_argument("create_fn", type=str)
    parser.add_argument("marker", type=str, help="e.g. SEC_EDGAR")
    args = parser.parse_args()
    register_path = args.repo_root / "src" / "register.cpp"
    if not register_path.is_file():
        print(f"register.cpp not found: {register_path}", file=sys.stderr)
        return 1
    apply_snippet(register_path, args.create_fn, args.marker)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
