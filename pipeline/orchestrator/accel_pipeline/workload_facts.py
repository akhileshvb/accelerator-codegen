"""Facts that are a function of a query's SQL and its benchmark schema, derived before analysis.

Tables and the columns read from them come from sqlglot (qualified against the schema);
output columns and logical types come from DuckDB describing the query over an empty
in-memory copy of workloads/<bench>/schema.sql.  A logical type is the DuckDB type
family without precision or scale (DECIMAL(38,4) is DECIMAL); mapping a logical type
to an engine's own type is the engine backend's business.
"""

from __future__ import annotations

import json
import re
from pathlib import Path
from typing import Any

import duckdb
import sqlglot
from sqlglot import exp
from sqlglot.optimizer.qualify import qualify
from sqlglot.optimizer.scope import traverse_scope

from accel_pipeline.layout import workload_schema_sql
from accel_pipeline.paths import query_artifacts_dir

FACTS_FILE = "workload_facts.json"


def _schema_columns(con: duckdb.DuckDBPyConnection) -> dict[str, dict[str, str]]:
    rows = con.execute(
        "SELECT table_name, column_name FROM information_schema.columns ORDER BY table_name, ordinal_position"
    ).fetchall()
    schema: dict[str, dict[str, str]] = {}
    for table, column in rows:
        schema.setdefault(table, {})[column] = "TEXT"
    return schema


def _output_schema(con: duckdb.DuckDBPyConnection, sql: str) -> list[dict[str, str]]:
    described = con.execute("DESCRIBE " + sql).fetchall()
    return [{"name": name, "type": re.sub(r"\(.*", "", dtype)} for name, dtype, *_ in described]


def _exists_probes_read_no_columns(tree: exp.Expression) -> None:
    """`EXISTS (SELECT * ...)` reads no column of its source; qualify would expand the star."""
    for exists in tree.find_all(exp.Exists):
        for select in exists.find_all(exp.Select):
            if any(isinstance(e, exp.Star) for e in select.expressions):
                select.set("expressions", [exp.Literal.number(1)])


def _dependent_tables(sql: str, schema: dict[str, dict[str, str]]) -> list[dict[str, Any]]:
    tree = sqlglot.parse_one(sql, read="duckdb")
    _exists_probes_read_no_columns(tree)
    ctes = {cte.alias for cte in tree.find_all(exp.CTE)}
    appearance = list(dict.fromkeys(t.name for t in tree.find_all(exp.Table) if t.name not in ctes))
    qualified = qualify(tree, schema=schema, dialect="duckdb", validate_qualify_columns=False)
    read: dict[str, set[str]] = {}
    for scope in traverse_scope(qualified):
        for column in scope.columns:
            source = scope.sources.get(column.table)
            if isinstance(source, exp.Table):
                read.setdefault(source.name, set()).add(column.name)
    return [
        {"name": table, "watched_columns": [c for c in schema[table] if c in read.get(table, ())]}
        for table in appearance
    ]


def scratch_connection(path: str = ":memory:", read_only: bool = False) -> Any:
    """The embedded SQL engine the orchestrator derives facts and test subsets with."""
    return duckdb.connect(path, read_only=read_only)


def derive_query_facts(sql: str, schema_sql: str) -> dict[str, Any]:
    """output_schema and dependent_tables (with watched_columns) of `sql` over `schema_sql`."""
    sql = sql.strip().rstrip(";")
    con = scratch_connection()
    try:
        con.execute(schema_sql)
        return {
            "output_schema": _output_schema(con, sql),
            "dependent_tables": _dependent_tables(sql, _schema_columns(con)),
        }
    finally:
        con.close()


def workload_facts(repo: Path, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """The run's derived facts, written to the run's artifacts (workload_facts.json)."""
    benchmark = str(workload.get("benchmark_name", "tpch"))
    sql = Path(workload["reference_sql_path"]).read_text(encoding="utf-8")
    schema_sql = (repo / workload_schema_sql(benchmark)).read_text(encoding="utf-8")
    facts = derive_query_facts(sql, schema_sql)
    path = query_artifacts_dir(repo, query, workload=workload) / FACTS_FILE
    text = json.dumps(facts, indent=2) + "\n"
    if not path.is_file() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")
    return facts


def with_facts(repo: Path, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """`workload` plus its derived output_schema and dependent_tables."""
    return {**workload, **workload_facts(repo, workload, query)}
