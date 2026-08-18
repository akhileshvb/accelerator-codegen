#!/usr/bin/env python3
"""Move generated headers next to src/generated sources and rewrite includes."""

from __future__ import annotations

import re
import shutil
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
INCLUDE_GEN = REPO / "include" / "accelerator_codegen"
SRC_GEN = REPO / "src" / "generated"

# old include token -> new include token (relative to src/generated on -I path)
INCLUDE_REWRITES: list[tuple[str, str]] = []


def add_rewrite(old: str, new: str) -> None:
    INCLUDE_REWRITES.append((old, new))


def move_file(src: Path, dst: Path) -> None:
    if not src.is_file():
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.is_file():
        dst.unlink()
    shutil.move(str(src), str(dst))
    print(f"move {src.relative_to(REPO)} -> {dst.relative_to(REPO)}")


def main() -> None:
    # --- aux module headers already under include/generated -> src/generated ---
    for path in sorted(INCLUDE_GEN.glob("generated/**/aux/module.hpp")):
        rel = path.relative_to(INCLUDE_GEN / "generated")
        dst = SRC_GEN / rel
        add_rewrite(f"accelerator_codegen/generated/{rel.as_posix()}", rel.as_posix())
        move_file(path, dst)

    # catalog + postgres headers
    for rel in (
        "catalog/query_catalog.hpp",
        "postgres/kernel_runner.hpp",
        "postgres/runtime.hpp",
    ):
        src = INCLUDE_GEN / "generated" / rel
        dst = SRC_GEN / rel
        add_rewrite(f"accelerator_codegen/generated/{rel}", rel)
        move_file(src, dst)

    readme = INCLUDE_GEN / "generated" / "README.md"
    if readme.is_file():
        move_file(readme, SRC_GEN / "README.md")

    # --- flat tpch base query headers ---
    for path in sorted(INCLUDE_GEN.glob("tpch_q*.hpp")):
        name = path.name
        if name == "tpch_q1_fused.hpp":
            q = 1
            dst = SRC_GEN / f"tpch/q{q}/base/fused.hpp"
            add_rewrite("accelerator_codegen/tpch_q1_fused.hpp", dst.relative_to(SRC_GEN).as_posix())
            move_file(path, dst)
            continue
        if name == "tpch_q1_parallel_aligned_agg.hpp":
            dst = SRC_GEN / "tpch/q1/base/parallel/aligned_agg.hpp"
            add_rewrite("accelerator_codegen/tpch_q1_parallel_aligned_agg.hpp", dst.relative_to(SRC_GEN).as_posix())
            move_file(path, dst)
            continue
        if name.endswith("_finalize.hpp"):
            q = int(name.removeprefix("tpch_q").removesuffix("_finalize.hpp"))
            dst = SRC_GEN / f"tpch/q{q}/aux/finalize.hpp"
            add_rewrite(f"accelerator_codegen/tpch_q{q}_finalize.hpp", dst.relative_to(SRC_GEN).as_posix())
            move_file(path, dst)
            continue
        m = re.fullmatch(r"tpch_q(\d+)\.hpp", name)
        if not m:
            print(f"skip unknown tpch header: {name}")
            continue
        q = int(m.group(1))
        dst = SRC_GEN / f"tpch/q{q}/base/query.hpp"
        add_rewrite(f"accelerator_codegen/tpch_q{q}.hpp", dst.relative_to(SRC_GEN).as_posix())
        move_file(path, dst)

    # --- flat sec_edgar base query headers ---
    if (INCLUDE_GEN / "sec_edgar_query_runtime.hpp").is_file():
        add_rewrite(
            "accelerator_codegen/sec_edgar_query_runtime.hpp",
            "sec_edgar/runtime.hpp",
        )
        move_file(INCLUDE_GEN / "sec_edgar_query_runtime.hpp", SRC_GEN / "sec_edgar/runtime.hpp")

    for path in sorted(INCLUDE_GEN.glob("sec_edgar_q*.hpp")):
        name = path.name
        if name.endswith("_fused.hpp"):
            q = int(name.removeprefix("sec_edgar_q").removesuffix("_fused.hpp"))
            dst = SRC_GEN / f"sec_edgar/q{q}/base/fused.hpp"
            add_rewrite(f"accelerator_codegen/sec_edgar_q{q}_fused.hpp", dst.relative_to(SRC_GEN).as_posix())
            move_file(path, dst)
            continue
        if name.endswith("_finalize.hpp"):
            q = int(name.removeprefix("sec_edgar_q").removesuffix("_finalize.hpp"))
            dst = SRC_GEN / f"sec_edgar/q{q}/aux/finalize.hpp"
            add_rewrite(
                f"accelerator_codegen/sec_edgar_q{q}_finalize.hpp",
                dst.relative_to(SRC_GEN).as_posix(),
            )
            move_file(path, dst)
            continue
        m = re.fullmatch(r"sec_edgar_q(\d+)\.hpp", name)
        if not m:
            print(f"skip unknown sec_edgar header: {name}")
            continue
        q = int(m.group(1))
        dst = SRC_GEN / f"sec_edgar/q{q}/base/query.hpp"
        add_rewrite(f"accelerator_codegen/sec_edgar_q{q}.hpp", dst.relative_to(SRC_GEN).as_posix())
        move_file(path, dst)

    # Sort longest-first so partial replacements don't fire early.
    INCLUDE_REWRITES.sort(key=lambda pair: len(pair[0]), reverse=True)

    exts = {".cpp", ".hpp", ".h", ".cmake", ".tpl", ".md", ".yaml", ".py", ".sh", ".sql"}
    for path in REPO.rglob("*"):
        if not path.is_file():
            continue
        if path.suffix not in exts:
            continue
        if "build/" in path.as_posix() or ".git/" in path.as_posix():
            continue
        if path == Path(__file__).resolve():
            continue
        text = path.read_text(encoding="utf-8")
        original = text
        for old, new in INCLUDE_REWRITES:
            text = text.replace(f'#include "{old}"', f'#include "{new}"')
            text = text.replace(f"#include <{old}>", f'#include "{new}"')
        if text != original:
            path.write_text(text, encoding="utf-8")
            print(f"rewrite {path.relative_to(REPO)}")

    # Remove empty generated include tree
    gen_inc = INCLUDE_GEN / "generated"
    if gen_inc.is_dir():
        shutil.rmtree(gen_inc, ignore_errors=True)

    print("done")


if __name__ == "__main__":
    main()
