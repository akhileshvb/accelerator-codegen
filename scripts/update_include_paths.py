#!/usr/bin/env python3
"""Update include paths after src/include reorg."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

REPLACEMENTS = [
    # runtime duckdb
    ("accelerator_codegen/runtime/duckdb/query_runtime.hpp", "accelerator_codegen/runtime/duckdb/query_runtime.hpp"),
    ("accelerator_codegen/runtime/duckdb/kernel.hpp", "accelerator_codegen/runtime/duckdb/kernel.hpp"),
    ("accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp", "accelerator_codegen/runtime/duckdb/duckdb_scan_adapter.hpp"),
    ("accelerator_codegen/runtime/duckdb/chunk_source.hpp", "accelerator_codegen/runtime/duckdb/chunk_source.hpp"),
    ("accelerator_codegen/runtime/duckdb/register.hpp", "accelerator_codegen/runtime/duckdb/register.hpp"),
    ("accelerator_codegen/runtime/duckdb/function_lifecycle.hpp", "accelerator_codegen/runtime/duckdb/function_lifecycle.hpp"),
    ("accelerator_codegen/runtime/parallel/", "accelerator_codegen/runtime/parallel/"),
    # runtime catalog
    ("accelerator_codegen/runtime/catalog/shared_query_engine.hpp",
     "accelerator_codegen/runtime/catalog/shared_query_engine.hpp"),
    ("accelerator_codegen/generated/catalog/query_catalog.hpp",
     "accelerator_codegen/generated/catalog/query_catalog.hpp"),
    # runtime duckdb aux engine
    ("accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp",
     "accelerator_codegen/runtime/duckdb/duckdb_runtime.hpp"),
    # runtime postgres
    ("accelerator_codegen/runtime/postgres/scan_adapter.hpp",
     "accelerator_codegen/runtime/postgres/scan_adapter.hpp"),
    ("accelerator_codegen/runtime/postgres/kernel_executor.hpp",
     "accelerator_codegen/runtime/postgres/kernel_executor.hpp"),
    ("accelerator_codegen/runtime/postgres/bind_helper.hpp",
     "accelerator_codegen/runtime/postgres/bind_helper.hpp"),
    ("accelerator_codegen/runtime/postgres/row_writer.hpp",
     "accelerator_codegen/runtime/postgres/row_writer.hpp"),
    ("accelerator_codegen/runtime/postgres/kernel_dispatch.hpp",
     "accelerator_codegen/runtime/postgres/kernel_dispatch.hpp"),
    # generated postgres
    ("accelerator_codegen/generated/postgres/runtime.hpp",
     "accelerator_codegen/generated/postgres/runtime.hpp"),
    ("accelerator_codegen/generated/postgres/kernel_runner.hpp",
     "accelerator_codegen/generated/postgres/kernel_runner.hpp"),
    # stateful
    ("accelerator_codegen/stateful/core/", "accelerator_codegen/stateful/core/"),
    ("accelerator_codegen/stateful/maintenance/", "accelerator_codegen/stateful/maintenance/"),
    ("accelerator_codegen/stateful/state/partitioned_state_store.hpp",
     "accelerator_codegen/stateful/state/partitioned_state_store.hpp"),
    ("accelerator_codegen/stateful/state/invalidation_plan.hpp",
     "accelerator_codegen/stateful/state/invalidation_plan.hpp"),
    ("accelerator_codegen/stateful/state/partition_key.hpp",
     "accelerator_codegen/stateful/state/partition_key.hpp"),
    ("accelerator_codegen/stateful/core/incremental_txn_buffer.hpp",
     "accelerator_codegen/stateful/core/incremental_txn_buffer.hpp"),
    ("accelerator_codegen/harness/", "accelerator_codegen/harness/"),
    ("accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp",
     "accelerator_codegen/stateful/duckdb/duckdb_aux_backend.hpp"),
    ("accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp",
     "accelerator_codegen/stateful/duckdb/duckdb_state_store.hpp"),
    ("accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp",
     "accelerator_codegen/stateful/postgres/postgres_aux_backend.hpp"),
    ("accelerator_codegen/stateful/postgres/postgres_state_store.hpp",
     "accelerator_codegen/stateful/postgres/postgres_state_store.hpp"),
    ("accelerator_codegen/stateful/postgres/postgres_mutation_capture.hpp",
     "accelerator_codegen/stateful/postgres/postgres_mutation_capture.hpp"),
    ("accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp",
     "accelerator_codegen/stateful/postgres/postgres_xact_callbacks.hpp"),
]

AUX_MODULE_RE = re.compile(
    r"accelerator_codegen/aux_state/query/(tpch|sec_edgar)/(tpch|sec_edgar)_q(\d+)_common\.hpp"
)


def aux_module_repl(match: re.Match[str]) -> str:
    workload = match.group(1)
    q = match.group(3)
    return f"accelerator_codegen/generated/{workload}/q{q}/aux/module.hpp"


EXTENSIONS = {".cpp", ".hpp", ".h", ".cmake", ".md", ".sh", ".py", ".tpl", ".yaml", ".json", ".txt"}


def should_process(path: Path) -> bool:
    if path.suffix not in EXTENSIONS:
        return False
    parts = path.parts
    if "build" in parts or ".git" in parts:
        return False
    return True


def update_file(path: Path) -> bool:
    text = path.read_text()
    original = text
    for old, new in REPLACEMENTS:
        text = text.replace(old, new)
    text = AUX_MODULE_RE.sub(aux_module_repl, text)
    if text != original:
        path.write_text(text)
        return True
    return False


def main() -> int:
    changed = 0
    for path in ROOT.rglob("*"):
        if not path.is_file() or not should_process(path):
            continue
        if update_file(path):
            changed += 1
    print(f"updated {changed} files")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
