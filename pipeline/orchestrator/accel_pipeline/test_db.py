"""The run's test databases, cut once out of the engine's loaded dataset, right after host bootstrap.

A cut keeps a sample of the query's largest table and, along the query's join keys, every row those rows join
to (the parents they reference and the children that reference them), so the query's answers on it are as
non-trivial as on the full data. Two cuts, each a directory of <table>.tbl files (what every host.sh `load`
reads) written from the host's `sql` verb, so any engine works:

* `subset`: what quick_check's differential, build and read measurements run on;
* `tiny`: a few thousand rows in all, for the sanitizer runs (5-20x slower per row).

`manifest.json` beside them records the rows of every table in both cuts and in the full dataset.
"""

from __future__ import annotations

import fcntl
import hashlib
import json
import math
import os
import re
import shutil
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from accel_pipeline import data_shape_stats as stats
from accel_pipeline.data_shape import catalog_sql, dependent_table_names
from accel_pipeline.engine_host import ensure_dataset_loaded
from accel_pipeline.config import PipelineConfig
from accel_pipeline.layout import workload_schema_sql
from accel_pipeline.paths import query_artifacts_dir
from accel_pipeline.tools.data_tools import DataToolContext, HostSql
from accel_pipeline.workload_facts import with_facts

SUBSET_ROOT_ROWS = 40_000  # rows of the largest table in the quick cut
TINY_ROOT_ROWS = 1_500
STATEMENT_SECONDS = 900.0
FORMAT_VERSION = 2
_IDENTIFIER = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")


@dataclass(frozen=True)
class CutDatasets:
    subset: Path
    tiny: Path
    manifest: dict[str, Any]  # source_rows, subset_rows, tiny_rows: {table: rows}

    def scale(self, tables: list[str] | None = None) -> float:
        """Rows of the full dataset per row of the subset, over `tables` (all cut tables when empty); at least 1."""
        names = [t.lower() for t in tables or self.manifest["source_rows"]]
        full = sum(n for t, n in self.manifest["source_rows"].items() if t.lower() in names)
        cut = sum(n for t, n in self.manifest["subset_rows"].items() if t.lower() in names)
        return max(1.0, full / cut) if full and cut else 1.0


def cut_selects(tables: list[str], joins: list[tuple[tuple[str, str], tuple[str, str]]], rows: dict[str, int],
                integer_columns: dict[str, list[str]], root_rows: int) -> dict[str, str]:
    """One SELECT per table returning its rows in the cut. Each group of tables the joins connect has a root, its
    largest table, sampled by key on an integer column (a join key when it has one); every other table
    keeps the rows that join to its neighbour nearer the root."""
    adjacent: dict[str, list[tuple[str, str, str]]] = {t: [] for t in tables}  # table -> (its column, other, other's column)
    for (ta, ca), (tb, cb) in joins:
        if ta in adjacent and tb in adjacent:
            adjacent[ta].append((ca, tb, cb))
            adjacent[tb].append((cb, ta, ca))
    selects: dict[str, str] = {}
    for root in sorted(tables, key=lambda t: -rows[t]):
        if root in selects:
            continue
        selects[root] = _sample(root, rows[root], root_rows, [c for c, _, _ in adjacent[root]], integer_columns[root])
        queue = [root]
        while queue:
            parent = queue.pop(0)
            for parent_column, table, column in adjacent[parent]:
                if table not in selects:
                    selects[table] = f"SELECT * FROM {table} WHERE {column} IN (SELECT {parent_column} FROM ({selects[parent]}) AS kept)"
                    queue.append(table)
    return selects


def _sample(table: str, rows: int, root_rows: int, join_columns: list[str], integers: list[str]) -> str:
    """Every P-th key value, P a prime near rows / root_rows: an even spread over the key range (no bias to early
    keys), and a prime stride cannot line up with the blocks a generator numbers its keys in."""
    if rows <= root_rows:
        return f"SELECT * FROM {table}"
    column = next((c for c in join_columns + integers if c in integers), None)
    if column is None:
        raise ValueError(f"{table} has no integer column to sample by key")
    stride = math.ceil(rows / root_rows)
    while any(stride % d == 0 for d in range(2, math.isqrt(stride) + 1)):
        stride += 1
    return f"SELECT * FROM {table} WHERE {column} % {stride} = 0"


def _write_cut(runner: HostSql, selects: dict[str, str], directory: Path) -> dict[str, int]:
    directory.mkdir(parents=True)
    counts: dict[str, int] = {}
    for table, select in selects.items():
        lines = []
        for row in runner.rows(select, STATEMENT_SECONDS):
            fields = ["" if v is None else v for v in row]
            if any("|" in f or "\n" in f for f in fields):
                raise ValueError(f"{table} holds a value with '|' or a newline: not representable in a .tbl file")
            lines.append("|".join(fields) + "|\n")
        (directory / f"{table}.tbl").write_text("".join(lines), encoding="utf-8")
        counts[table] = len(lines)
    if not any(counts.values()):
        raise ValueError("the sample holds no rows: the key range is smaller than the sampling stride")
    return counts


def _key(cfg: PipelineConfig, workload: dict[str, Any], query: int, source: Path) -> str:
    bench = str(workload.get("benchmark_name") or "")
    sql = catalog_sql(workload, cfg.repo_root, query)
    schema = cfg.repo_root / workload_schema_sql(str(workload.get("dataset_benchmark") or bench))
    digest = hashlib.sha256(f"{FORMAT_VERSION} {SUBSET_ROOT_ROWS} {TINY_ROOT_ROWS} {source.resolve()}".encode())
    digest.update(sql.encode() + (schema.read_bytes() if schema.is_file() else b""))
    for tbl in sorted(source.glob("*.tbl")):
        digest.update(f"{tbl.name}{tbl.stat().st_size}{tbl.stat().st_mtime_ns}".encode())
    return digest.hexdigest()[:16]


def _cut(cfg: PipelineConfig, workload: dict[str, Any], query: int, destination: Path) -> dict[str, Any]:
    runner = HostSql(DataToolContext(cfg, workload, query, query_artifacts_dir(cfg.repo_root, query, workload=workload)))
    runner.require_up()
    schema = stats.read_schema(runner, 60.0, runner.catalog_sql)
    names = {t.lower(): t for t in schema}
    tables = [names[t.lower()] for t in dependent_table_names(with_facts(cfg.repo_root, workload, query)) if t.lower() in names]
    for name in tables + [c for t in tables for c in schema[t]]:
        if not _IDENTIFIER.match(name):
            raise ValueError(f"{name!r} is not a plain identifier")
    rows = {t: int(runner.rows(f"SELECT COUNT(*) FROM {t}", STATEMENT_SECONDS)[0][0] or 0) for t in tables}
    joins = stats.analyze_query(catalog_sql(workload, cfg.repo_root, query), schema, runner.dialect).joins
    integers = {t: [c for c, dtype in schema[t].items() if stats.is_integer(dtype)] for t in tables}
    manifest: dict[str, Any] = {"source_rows": rows}
    for name, root_rows in (("subset", SUBSET_ROOT_ROWS), ("tiny", TINY_ROOT_ROWS)):
        manifest[f"{name}_rows"] = _write_cut(runner, cut_selects(tables, joins, rows, integers, root_rows), destination / name)
    return manifest


def ensure_cut_datasets(cfg: PipelineConfig, workload: dict[str, Any], query: int) -> CutDatasets | None:
    """The cuts of this query's tables, made once per dataset (cached by a content key; ACCEL_SUBSET_DATASET_DIR
    names ready-made ones). None, with the reason on stderr, when they cannot be made: the quick tier then runs on
    the full dataset."""
    if given := os.environ.get("ACCEL_SUBSET_DATASET_DIR"):
        return CutDatasets(Path(given), Path(given), {"source_rows": {}, "subset_rows": {}})
    source = Path(str(workload.get("dataset_dir") or os.environ.get("ACCEL_DATASET_DIR") or ""))
    if not source.is_dir() or not any(source.glob("*.tbl")):
        return None
    bench = str(workload.get("benchmark_name") or "")
    destination = Path(f"{cfg.build_dir}.subset") / f"{bench}-q{query}-{_key(cfg, workload, query, source)}"
    destination.parent.mkdir(parents=True, exist_ok=True)
    with (destination.parent / f"{destination.name}.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        if not (destination / ".done").is_file():
            started = time.monotonic()
            try:
                problem = ensure_dataset_loaded(workload, cfg.repo_root, cfg.build_dir)
                if problem:
                    raise RuntimeError(problem)
                manifest = _cut(cfg, workload, query, destination)
            except (RuntimeError, ValueError, TimeoutError, OSError) as exc:
                shutil.rmtree(destination, ignore_errors=True)
                print(f"[pipeline] test databases not made ({exc}); quick_check runs on the full dataset", file=sys.stderr, flush=True)
                return None
            (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
            (destination / ".done").touch()
            print(f"[pipeline] test databases of Q{query} in {time.monotonic() - started:.0f} s: {describe(manifest)}",
                  file=sys.stderr, flush=True)
    manifest = json.loads((destination / "manifest.json").read_text(encoding="utf-8"))
    return CutDatasets(destination / "subset", destination / "tiny", manifest)


def describe(manifest: dict[str, Any]) -> str:
    total = {name: sum(manifest[f"{name}_rows"].values()) for name in ("source", "subset", "tiny")}
    return (f"subset {total['subset']:,} rows (of {total['source']:,}), tiny {total['tiny']:,} rows; per table "
            + ", ".join(f"{t} {manifest['subset_rows'][t]:,}/{manifest['tiny_rows'][t]:,}" for t in manifest["source_rows"]))
