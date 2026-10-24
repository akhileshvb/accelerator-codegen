"""MCP tools that let an agent explore the iteration dataset instead of reading a precomputed table.

Every statement runs on the target engine through its host.sh `sql` verb.  `query_data` runs the
agent's own read-only statement (in the engine's SQL); the convenience tools (column_stats,
join_fanout, rows_qualifying, column_spread, query_shape) emit portable SQL through data_shape_stats,
transpiled to the dialect the engine profile names (`data_sql.dialect`).  Tables and columns come
from the engine's catalog and the query template, never from this code.  Every statement is
time-boxed, every result capped, and every call is logged to the run's artifacts (data_queries.jsonl)
so analysis can cite it and the audit can check it.
"""

from __future__ import annotations

import csv
import io
import json
import os
import re
import signal
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

import sqlglot
from sqlglot import exp
from sqlglot.tokens import Tokenizer, TokenType

from accel_pipeline import data_shape_stats as stats
from accel_pipeline.config import PipelineConfig
from accel_pipeline.data_shape import md_table
from accel_pipeline.engine_host import host_script, instance_env
from accel_pipeline.paths import pipeline_root

LOG_FILE = "data_queries.jsonl"
CATALOG_SECONDS = 30.0

_LEADING = {"SELECT", "WITH", "EXPLAIN", "DESCRIBE", "DESC", "SUMMARIZE", "SHOW", "VALUES", "FROM"}
_FORBIDDEN = {
    "INSERT", "UPDATE", "DELETE", "MERGE", "UPSERT", "CREATE", "ALTER", "DROP", "TRUNCATE", "ATTACH", "DETACH",
    "COPY", "EXPORT", "IMPORT", "PRAGMA", "CALL", "INSTALL", "LOAD", "SET", "RESET", "GRANT", "REVOKE", "VACUUM",
    "CHECKPOINT", "INTO", "OUTFILE", "DUMPFILE", "LOCK", "COMMENT", "USE", "BEGIN", "COMMIT", "ROLLBACK",
    "EXECUTE", "PREPARE", "DO", "ANALYZE",
}
# Table and scalar functions that reach files, other databases or arbitrary SQL text.
_REACHING_FUNCTIONS = re.compile(
    r"^(read_\w*|glob|\w*file\w*|lo_\w+|dblink\w*|\w+_scan|query|query_table|copy_\w+|sniff_csv)$", re.I
)


def check_read_only(sql: str) -> str | None:
    """Why `sql` is not a single read-only statement, or None when it is."""
    try:
        tokens = Tokenizer().tokenize(sql)
    except Exception as exc:  # noqa: BLE001 — an unlexable statement is refused
        return f"cannot tokenize the statement: {exc}"
    while tokens and tokens[-1].token_type == TokenType.SEMICOLON:
        tokens.pop()
    if not tokens:
        return "empty statement"
    if any(t.token_type == TokenType.SEMICOLON for t in tokens):
        return "exactly one statement is allowed"
    if tokens[0].text.upper() not in _LEADING:
        return f"only SELECT/WITH/EXPLAIN/DESCRIBE/SUMMARIZE/SHOW/VALUES reads are allowed, not {tokens[0].text.upper()}"
    for i, tok in enumerate(tokens):
        if tok.token_type in (TokenType.STRING, TokenType.IDENTIFIER, TokenType.NUMBER):
            continue
        word = tok.text.upper()
        if word in _FORBIDDEN:
            return f"{word} is not a read (quote the identifier if it is a column name)"
        follows_paren = i + 1 < len(tokens) and tokens[i + 1].token_type == TokenType.L_PAREN
        if follows_paren and _REACHING_FUNCTIONS.match(tok.text):
            return f"{tok.text}() reaches outside the dataset tables"
    return None


def _log(art: Path, record: dict[str, Any]) -> None:
    with (art / LOG_FILE).open("a", encoding="utf-8") as f:
        f.write(json.dumps({"ts": time.strftime("%Y-%m-%dT%H:%M:%S"), **record}, default=str) + "\n")


def _csv(header: list[str], rows: list[tuple] | list[list]) -> str:
    out = io.StringIO()
    writer = csv.writer(out, lineterminator="\n")
    writer.writerow(header)
    writer.writerows([["" if v is None else v for v in row] for row in rows])
    return out.getvalue().rstrip("\n")


class HostSql:
    """`host.sh sql -` of the run's engine instance: one statement in, CSV out, time-boxed."""

    def __init__(self, ctx: "DataToolContext") -> None:
        cfg = ctx.pipeline_config
        self.cfg = cfg
        self.engine = str(ctx.workload.get("engine_name") or "")
        profile = _read_data_sql(self.engine, pipeline_root(cfg.repo_root))
        self.dialect = str(profile.get("dialect") or "")  # empty: the neutral dialect
        self.catalog_sql = str(profile.get("catalog_columns_sql") or "")
        self.env = {**os.environ, **instance_env(cfg.repo_root, self.engine, cfg.build_dir)}

    def require_up(self) -> None:
        if not self.engine or not host_script(self.cfg.repo_root, self.engine).is_file():
            raise RuntimeError(f"no host.sh for engine {self.engine!r}: the data tools need the target engine")
        if not (Path(self.env["ACCEL_ENGINE_STATE_DIR"]) / "env").is_file():  # written by `up`
            raise RuntimeError("the engine instance is not up (host.sh up, then load the dataset)")

    def _run(self, sql: str, seconds: float, max_lines: int) -> tuple[int, list[str]]:
        """(exit code, first max_lines output lines); the process group is killed at the time box or the line cap."""
        proc = subprocess.Popen(
            [str(host_script(self.cfg.repo_root, self.engine)), "sql", "-"], cwd=self.cfg.repo_root, env=self.env,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, start_new_session=True,
        )
        timer = _Killer(proc, time.monotonic() + seconds)
        try:
            proc.stdin.write(sql)
            proc.stdin.close()
            lines: list[str] = []
            for line in proc.stdout:
                lines.append(line.rstrip("\n"))
                if len(lines) >= max_lines:
                    timer.kill()
                    break
            proc.wait()
        finally:
            timer.cancel()
        if timer.fired_on_deadline:
            raise TimeoutError(f"exceeded the {seconds:g} s time box")
        return proc.returncode, lines

    def query(self, sql: str, seconds: float, max_rows: int) -> tuple[list[list[str]], bool]:
        """(header + at most max_rows rows, more rows exist)."""
        code, lines = self._run(sql, seconds, max_rows + 2)  # header, the rows, one more to tell
        text = "\n".join(lines)
        if code != 0 and len(lines) < max_rows + 2:
            raise RuntimeError(text[-1500:] or f"host.sh sql exited {code}")
        parsed = list(csv.reader(io.StringIO(text)))
        return parsed[: max_rows + 1], len(parsed) > max_rows + 1

    def rows(self, sql: str, seconds: float) -> list[list[str | None]]:
        """The rows of a statement (no header), NULL as None (CSV shows it as an empty cell)."""
        table, more = self.query(sql, seconds, 10_000_000)
        return [[c if c != "" else None for c in row] for row in table[1:]]


def _read_data_sql(engine: str, root: Path) -> dict[str, Any]:
    """The engine profile's data_sql block ({} when the engine states none)."""
    import yaml

    path = root / "config" / "workloads" / f"engine_{engine}.yaml"
    if not engine or not path.is_file():
        return {}
    data = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
    return dict((data.get("engine_profile") or {}).get("data_sql") or {})


@dataclass
class DataToolContext:
    pipeline_config: PipelineConfig
    workload: dict[str, Any]
    query: int
    art: Path  # the run's artifacts: data_queries.jsonl goes here

    @property
    def limits(self):
        return self.pipeline_config.data_tools


class Dataset:
    """The engine's dataset: its catalog, row counts and time-boxed portable statements."""

    def __init__(self, ctx: DataToolContext) -> None:
        self.runner = HostSql(ctx)
        self.runner.require_up()
        seconds = ctx.limits.query_seconds
        self.probe = stats.Probe(self.runner, statement_seconds=seconds, total_seconds=seconds)
        try:
            self.schema = stats.read_schema(self.runner, CATALOG_SECONDS, self.runner.catalog_sql)
        except (RuntimeError, TimeoutError) as exc:
            raise RuntimeError(f"cannot read the engine's catalog: {exc}") from exc
        if not self.schema:
            raise RuntimeError("the engine instance has no tables: load the dataset (host.sh load)")
        self._rows: dict[str, int] = {}

    @property
    def dialect(self) -> str | None:
        return self.runner.dialect or None

    def rows(self, table: str) -> int:
        if table not in self._rows:
            self._rows[table] = self.probe.count(f"SELECT COUNT(*) FROM {stats._q(table)}") or 0
        return self._rows[table]

    def column(self, spec: str) -> tuple[str, str]:
        """`table.column` (any case) resolved against the catalog."""
        table, _, column = spec.strip().partition(".")
        tables = {t.lower(): t for t in self.schema}
        if table.lower() not in tables:
            raise ValueError(f"unknown table {table!r} in {spec!r}; tables: {', '.join(sorted(self.schema))}")
        table = tables[table.lower()]
        columns = {c.lower(): c for c in self.schema[table]}
        if column.lower() not in columns:
            raise ValueError(f"unknown column {column!r} in {table}; columns: {', '.join(self.schema[table])}")
        return table, columns[column.lower()]

    def sampling_note(self, tables: list[str]) -> str:
        big = [f"{t} ({self.rows(t):,} rows)" for t in dict.fromkeys(tables) if self.rows(t) > stats.SAMPLE_ROWS]
        if not big:
            return f"exact (tables are at most {stats.SAMPLE_ROWS:,} rows)"
        return (f"sampled: about {stats.SAMPLE_ROWS:,} rows of {', '.join(big)} (key modulo, or a prefix without an "
                "integer column); counts are rescaled, quantiles and NDV are estimates")

    def table_of(self, predicate: str) -> str:
        """The one table whose columns a predicate uses (qualified `table.column` or bare column names)."""
        tree = stats.parse_template(f"SELECT 1 WHERE {predicate}", self.dialect)
        candidates: set[str] | None = None
        for col in tree.find_all(exp.Column):
            owners = (
                {t for t in self.schema if t.lower() == col.table.lower()} if col.table
                else {t for t, cols in self.schema.items() if col.name.lower() in {c.lower() for c in cols}}
            )
            candidates = owners if candidates is None else candidates & owners
        if not candidates or len(candidates) > 1:
            raise ValueError(
                "the predicate must use columns of exactly one table (qualify them as table.column); "
                f"candidates: {sorted(candidates or [])}"
            )
        return next(iter(candidates))


def dataset_row_counts(ctx: DataToolContext, tables: list[str]) -> dict[str, int]:
    """Row counts of `tables` in the engine's dataset; {} when the engine cannot be reached."""
    try:
        ds = Dataset(ctx)
        found = {t.lower(): t for t in ds.schema}
        return {found[t.lower()]: ds.rows(found[t.lower()]) for t in tables if t.lower() in found}
    except (RuntimeError, TimeoutError, OSError):
        return {}


def _source(ds: Dataset, table: str) -> tuple[str, float]:
    return stats.sample_of(table, ds.rows(table), ds.schema[table])


def _quantile_cells(res: tuple) -> list[Any]:
    n, lo, qs, hi = res
    return [f"{n:,}", lo, qs[0], qs[1], qs[2], hi]


class _Killer:
    """Kills a process group at a deadline or on request."""

    def __init__(self, proc: subprocess.Popen, deadline: float) -> None:
        self.proc = proc
        self.fired_on_deadline = False
        self._timer = threading.Timer(max(deadline - time.monotonic(), 0.0), self._expire)
        self._timer.start()

    def _expire(self) -> None:
        self.fired_on_deadline = True
        self.kill()

    def kill(self) -> None:
        try:
            os.killpg(self.proc.pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError):
            pass

    def cancel(self) -> None:
        self._timer.cancel()


def make_data_tools(ctx: DataToolContext) -> dict[str, Callable[..., str]]:
    def logged(tool: str, args: dict[str, Any], run: Callable[[], tuple[str, dict[str, Any]]]) -> str:
        """Run a tool body, log the call (sql, rows, seconds), and return its text; errors come back as text."""
        started = time.monotonic()
        try:
            text, info = run()
            error = None
        except (ValueError, RuntimeError, TimeoutError, OSError, sqlglot.errors.SqlglotError) as exc:
            text, info, error = f"error: {exc}", {}, str(exc)
        seconds = round(time.monotonic() - started, 3)
        _log(ctx.art, {"tool": tool, "args": args, "seconds": seconds, "error": error, **info})
        return text if error else f"{text}\n({seconds:.2f} s)"

    def query_data(sql: str, max_rows: int | None = None) -> str:
        """MCP TOOL. Run ONE read-only statement (SELECT/WITH/EXPLAIN/DESCRIBE/SUMMARIZE) on the iteration dataset.

        Use it to measure what the design depends on: rows qualifying for a predicate, distinct counts,
        value ranges, fan-out, how two columns relate. The statement runs on the target engine, in its SQL.
        Results are CSV with a header, capped at max_rows (default and maximum are configured); a
        statement is stopped at the time box (default 60 s). Counts and spreads measured here are at
        iteration scale. Every call is logged to data_queries.jsonl.
        """
        limits = ctx.limits
        rows_cap = max(1, min(int(max_rows or limits.default_rows), limits.max_rows))

        def run() -> tuple[str, dict[str, Any]]:
            why = check_read_only(sql)
            if why:
                raise ValueError(f"refused: {why}")
            runner = HostSql(ctx)
            runner.require_up()
            table, more = runner.query(sql, limits.query_seconds, rows_cap)
            n = len(table) - 1
            note = f"; capped at {rows_cap} rows, more exist" if more else ""
            return (f"-- {n} rows{note}\n{_csv(table[0], table[1:])}",
                    {"sql": sql, "rows": n, "capped": more})

        return logged("query_data", {"sql": sql, "max_rows": rows_cap}, run)

    def column_stats(columns: list[str]) -> str:
        """MCP TOOL. Per column ("table.column"): type, NULL fraction, distinct count, min, p1, p50, p99, max.

        One pass per table; tables above the sampling threshold are sampled (the output says so).
        """

        def run() -> tuple[str, dict[str, Any]]:
            ds = Dataset(ctx)
            refs = [ds.column(c) for c in columns]
            rows = []
            for table in dict.fromkeys(t for t, _ in refs):
                cols = [c for t, c in refs if t == table]
                found = stats.column_stats(ds.probe, table, cols, ds.schema[table], ds.rows(table))
                for c in cols:
                    st = found.get(c)
                    if st:
                        q = st["q"] or [None] * 3
                        rows.append([f"{table}.{c}", ds.schema[table][c], f"{st['null_frac']:.1%}", st["ndv"],
                                     st["min"], q[0], q[1], q[2], st["max"]])
            lines = md_table(["column", "type", "NULL", "NDV", "min", "p1", "p50", "p99", "max"], rows)
            text = f"{ds.sampling_note([t for t, _ in refs])}\n\n" + "\n".join(lines)
            return text, {"sql": ds.probe.executed, "rows": len(rows)}

        return logged("column_stats", {"columns": columns}, run)

    def join_fanout(left: str, right: str) -> str:
        """MCP TOOL. Fan-out of a join between two key columns ("table.column" each), in both directions.

        Per side: distinct keys present, rows per key (avg, p99, max), and the share of rows that have a partner.
        """

        def run() -> tuple[str, dict[str, Any]]:
            ds = Dataset(ctx)
            (lt, lc), (rt, rc) = ds.column(left), ds.column(right)
            rows = []
            for (t, c), (ot, oc) in (((lt, lc), (rt, rc)), ((rt, rc), (lt, lc))):
                res, match = stats.fanout(ds.probe, t, c, ot, oc, ds.rows(t), ds.schema[t])
                if res:
                    rows.append([f"{t}.{c} -> {ot}.{oc}", f"{res[0]:,}", res[1], res[2], res[3],
                                 "-" if match is None else f"{match:.1%}"])
            text = ds.sampling_note([lt, rt]) + "\n\n" + "\n".join(
                md_table(["side (key -> joined to)", "keys", "avg", "p99", "max", "rows with a partner"], rows))
            return text, {"sql": ds.probe.executed, "rows": len(rows)}

        return logged("join_fanout", {"left": left, "right": right}, run)

    def rows_qualifying(predicate_sql: str, values: list[str]) -> str:
        """MCP TOOL. Rows qualifying for a predicate template, with $1 replaced by each value.

        predicate_sql: a condition over one table's columns, e.g. "t.d < $1". values: the
        bindings to try (dates and strings are quoted for you); pick the ends of the parameter's domain
        and a few between. Counts are rescaled when the table is sampled (the output says so).
        """

        def run() -> tuple[str, dict[str, Any]]:
            why = check_read_only(f"SELECT 1 WHERE {predicate_sql}")
            if why:
                raise ValueError(f"refused: {why}")
            if set(re.findall(r"\$(\d+)", predicate_sql)) - {"1"}:
                raise ValueError("only $1 is substituted; write the other parameters as literals")
            if not values:
                raise ValueError("values must not be empty")
            ds = Dataset(ctx)
            table = ds.table_of(predicate_sql)
            total = ds.rows(table)
            src, scale = _source(ds, table)
            rows = []
            for v in values:
                where = re.sub(r"\$1\b", lambda _: stats.literal(v), predicate_sql)
                n = ds.probe.count(f"SELECT COUNT(*) FROM {src} AS {stats._q(table)} WHERE {where}")
                n = None if n is None else int(n * scale)
                rows.append([v, "-" if n is None else f"{n:,}", "-" if n is None or not total else f"{n / total:.2%}"])
            text = (f"table {table} ({total:,} rows); {ds.sampling_note([table])}\n\n"
                    + "\n".join(md_table(["$1", "rows qualifying", "of table"], rows)))
            return text, {"sql": ds.probe.executed, "rows": len(rows)}

        return logged("rows_qualifying", {"predicate_sql": predicate_sql, "values": values}, run)

    def column_spread(a: str, b: str, join_sql: str = "") -> str:
        """MCP TOOL. Distribution of a - b ("table.column" each; dates in days) over the joined rows.

        join_sql: the equality linking the two tables, e.g. "a.id = b.a_id"
        (empty when both columns are in one table). Reports rows, min, p1, p50, p99, max.
        """

        def run() -> tuple[str, dict[str, Any]]:
            ds = Dataset(ctx)
            ka, kb = ds.column(a), ds.column(b)
            shape = stats.QueryShape()
            if ka[0] != kb[0]:
                shape.joins.append(_equality(ds, join_sql, {ka[0], kb[0]}))
            res, _ = stats.spread(ds.probe, ka, kb, shape, {t: ds.rows(t) for t in {ka[0], kb[0]}}, ds.schema)
            if not res:
                raise RuntimeError("no result (timed out, or no joined rows)")
            unit = "days" if stats.type_family(ds.schema[ka[0]][ka[1]]) == "date" else "units"
            text = (f"{a} - {b} over {'joined ' if ka[0] != kb[0] else ''}rows, in {unit}; "
                    f"{ds.sampling_note([ka[0], kb[0]])}\n\n"
                    + "\n".join(md_table(["rows", "min", "p1", "p50", "p99", "max"], [_quantile_cells(res)])))
            return text, {"sql": ds.probe.executed, "rows": res[0]}

        return logged("column_spread", {"a": a, "b": b, "join_sql": join_sql}, run)

    def query_shape() -> str:
        """MCP TOOL. The statistics the query's own SQL implies, as tables: per-column stats of the columns it
        uses, join-key fan-outs, rows qualifying at the extremes of each parameterised predicate, and spreads
        between the columns its predicates combine. A starting point; follow up with the other tools.
        """

        def run() -> tuple[str, dict[str, Any]]:
            from accel_pipeline.data_shape import build_data_shape_markdown, catalog_sql

            sql = catalog_sql(ctx.workload, ctx.pipeline_config.repo_root, ctx.query)
            if not sql:
                raise RuntimeError("the query template is not available to this run")
            ds = Dataset(ctx)
            seconds = ctx.limits.query_seconds
            text = build_data_shape_markdown(sql, ds.runner, ds.schema, f"engine `{ds.runner.engine}`",
                                             seconds, 3 * seconds)
            return text, {"sql": "(derived from the query template)", "rows": text.count("\n")}

        return logged("query_shape", {}, run)

    return {
        "query_data": query_data,
        "column_stats": column_stats,
        "join_fanout": join_fanout,
        "rows_qualifying": rows_qualifying,
        "column_spread": column_spread,
        "query_shape": query_shape,
    }


def _equality(ds: Dataset, join_sql: str, tables: set[str]) -> tuple[tuple[str, str], tuple[str, str]]:
    """The two (table, column) sides of `join_sql`, an equality between columns of `tables`."""
    if not join_sql.strip():
        raise ValueError("join_sql is required when the columns are in different tables")
    tree = stats.parse_template(f"SELECT 1 WHERE {join_sql}", ds.dialect).args["where"].this
    if not (isinstance(tree, exp.EQ) and isinstance(tree.this, exp.Column) and isinstance(tree.expression, exp.Column)):
        raise ValueError("join_sql must be one equality between two columns")
    sides = []
    for col in (tree.this, tree.expression):
        owners = (
            [t for t in tables if t.lower() == col.table.lower()] if col.table
            else [t for t in tables if col.name.lower() in {c.lower() for c in ds.schema[t]}]
        )
        if len(owners) != 1:
            raise ValueError(f"cannot place {col.sql()} in {sorted(tables)}; qualify it as table.column")
        sides.append(ds.column(f"{owners[0]}.{col.name}"))
    if {s[0] for s in sides} != tables:
        raise ValueError("join_sql must link the two columns' tables")
    return sides[0], sides[1]
