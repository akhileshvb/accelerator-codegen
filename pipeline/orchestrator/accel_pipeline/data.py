"""The run's data, and the tests derived from it.

What the agents query about the dataset. sqlglot finds the columns a query uses (predicate, join, group, order),
its join keys, its parameterised predicates and the column pairs whose difference matters; the statistics are then
computed on the target engine through a `Runner` (host.sh `sql`: `HostSql`, `Dataset`). The statements are portable
SQL (COUNT, COUNT DISTINCT, MIN/MAX, AVG, window functions for quantiles, `%` for sampling) written in sqlglot's
neutral dialect and transpiled to the engine's dialect (its profile's `data_sql.dialect`). Each statement is
time-boxed and tables above SAMPLE_ROWS are sampled by key modulo (a prefix of the table when it has no integer
column), so the work stays bounded on any dataset. They feed the analysis prompt's Data section and the
`query_shape` tool.

Test cuts, made once out of the engine's loaded dataset right after host bootstrap. A cut keeps a sample of the
largest table and, along the query's join keys, every row those rows join to (the parents they reference and the
children that reference them), so the query's answers on it are as non-trivial as on the full data. It holds every
table of the dataset, because a harness may write tables the query does not read (a refresh function may insert
parent and child rows together): the ones the query does not join to follow key columns of the same name (`l_orderkey` and
`o_orderkey`, or `customer_id` in both). Two cuts, each a directory of <table>.tbl files (what every host.sh
`load` reads) written from the host's `sql` verb, so any engine works:

* `subset`: what quick_check's differential, build and read measurements run on;
* `tiny`: a few thousand rows in all, for the sanitizer runs (5-20x slower per row).

`manifest.json` beside them records the rows of every table in both cuts and in the full dataset.

The per-query test suite: its declarative format, mechanical validation, parameter tuples, random draws and
freezing. One suite per query, in ``layout.suite_dir`` (the format the Verus pipeline's tests stage writes too;
the engine harness runs it):

    params.sql    fixed parameter tuples, one per line, as SQL literals in the query's $n order
    domain.yaml   the parameter domain fresh random tuples are drawn from on every run
    txns/NNN.sql  write transactions, applied in order, committed (a first line ``-- rollback``: applied, then aborted)
    write_mix.yaml  the write-frequency range benchmarks sample from, and the write kinds' relative weights
    suite.yaml    manifest: what each transaction is, edge-case coverage, scale scenarios, bulk and point minimums
    setup.sql     optional; shapes the data before the accelerator is built

The test_suite stage writes it and validates it; afterwards it is frozen (content hash in the artifacts) and the
gates verify the hash before they run. A genuinely wrong test is raised as a change request, never edited.

The engine host, every engine driven through ``backends/<engine>/host.sh`` (the verb interface is
``prompts/engine_backend/host_contract.md``; nothing here names an engine, its facts come from the profile's
``host_runtime``), is how a dataset gets loaded and queried, so its adapters sit here. The analysis validation
and the aux benchmark's CSV reader close the module.
"""
from __future__ import annotations

import calendar
import copy
import csv
import datetime as dt
import fcntl
import hashlib
import json
import math
import os
import random
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
from dataclasses import asdict, dataclass, field
from pathlib import Path
from typing import Any, Protocol

import sqlglot
import yaml
from sqlglot import exp
from sqlglot.optimizer.qualify import qualify
from sqlglot.optimizer.scope import traverse_scope

from accel_pipeline.config import (
    benchmark_of,
    pipeline_root,
    PipelineConfig,
    query_artifacts_dir,
    SuiteConfig,
)
from accel_pipeline.layout import suite_dir, workload_schema_sql


SAMPLE_ROWS = 10_000_000
CATALOG_SECONDS = 30.0
STATEMENT_SECONDS = 10.0
TOTAL_SECONDS = 25.0
MAX_SPREAD_PAIRS = 6
QUANTILES = [0.01, 0.5, 0.99]

_RANGE = {exp.LT: "<", exp.LTE: "<=", exp.GT: ">", exp.GTE: ">="}
_FLIP = {"<": ">", "<=": ">=", ">": "<", ">=": "<="}
_PARAMS = (exp.Placeholder, exp.Parameter)
_AGGS = (exp.Sum, exp.Avg, exp.Count, exp.Min, exp.Max)


class Runner(Protocol):
    """Runs one statement on the target engine: rows as strings (NULL as None); raises on an error or the time box."""

    dialect: str

    def rows(self, sql: str, seconds: float) -> list[list[str | None]]: ...


_NUMBER_TYPE = re.compile(r"INT|DEC|NUM|DOUBLE|FLOAT|REAL|SERIAL|MONEY", re.I)
_DATE_TYPE = re.compile(r"DATE|TIME", re.I)


def type_family(dtype: str) -> str:
    """"date", "number" or "other" from an engine's type name (information_schema data_type)."""
    base = re.sub(r"\(.*", "", dtype)
    if _DATE_TYPE.search(base):
        return "date"
    return "number" if _NUMBER_TYPE.search(base) else "other"


def is_integer(dtype: str) -> bool:
    return bool(re.search(r"INT|SERIAL", re.sub(r"\(.*", "", dtype), re.I)) and "POINT" not in dtype.upper()


def num(value: Any) -> int | float | None:
    """A result cell as a number (None for NULL or a non-number)."""
    if value is None or value == "":
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        try:
            return float(value)
        except (TypeError, ValueError):
            return None


@dataclass
class ParamPredicate:
    sql: str
    params: list[str]
    table: str | None
    column: str | None
    op: str  # "<", "<=", ">", ">=", "=", "in", "like", "between", "agg>", ...
    group: list[str] = field(default_factory=list)  # group columns for an aggregate predicate
    agg_sql: str = ""


@dataclass
class QueryShape:
    roles: dict[tuple[str, str], set[str]] = field(default_factory=dict)
    joins: list[tuple[tuple[str, str], tuple[str, str]]] = field(default_factory=list)
    params: list[ParamPredicate] = field(default_factory=list)
    spread_pairs: list[tuple[tuple[str, str], tuple[str, str], str]] = field(default_factory=list)


def _owned(node: exp.Expression, select: exp.Expression, kinds) -> list[exp.Expression]:
    return [n for n in node.find_all(kinds) if n.find_ancestor(exp.Select) is select]


def _conjuncts(clause: exp.Expression | None) -> list[exp.Expression]:
    if clause is None:
        return []
    cond = clause.this
    return list(cond.flatten()) if isinstance(cond, exp.And) else [cond]


def parse_template(sql: str, dialect: str | None = None) -> exp.Expression:
    """A query template (`$n` parameters) as a tree: parsed neutrally with the parameters as placeholders
    (their syntax differs per dialect), else in the engine's dialect when it uses syntax only that parses."""
    text = re.sub(r"\$(\d+)", r":__p\1", sql.strip().rstrip(";"))
    try:
        return sqlglot.parse_one(text)
    except sqlglot.errors.SqlglotError:
        return sqlglot.parse_one(text, read=dialect)


def analyze_query(sql: str, schema: dict[str, dict[str, str]], dialect: str | None = None) -> QueryShape:
    qualified = qualify(parse_template(sql, dialect), schema=schema, validate_qualify_columns=False)
    shape = QueryShape()
    candidates: dict[tuple[str, str], set[str]] = {}  # constrained numeric/date column -> params it shares
    direct: list[tuple[tuple[str, str], tuple[str, str]]] = []

    for scope in traverse_scope(qualified):
        select = scope.expression
        if not isinstance(select, exp.Select):
            continue

        def resolve(col: exp.Column) -> tuple[str, str] | None:
            src = scope.sources.get(col.table)
            return (src.name, col.name) if isinstance(src, exp.Table) else None

        def mark(node: exp.Expression, role: str) -> None:
            for col in _owned(node, select, exp.Column):
                key = resolve(col)
                if key:
                    shape.roles.setdefault(key, set()).add(role)

        clauses = [(select.args.get("where"), "P"), (select.args.get("having"), "P")]
        clauses += [(j.args.get("on") and exp.Where(this=j.args["on"]), "P") for j in select.args.get("joins") or []]
        for clause, role in clauses:
            for conj in _conjuncts(clause):
                cols = [resolve(c) for c in _owned(conj, select, exp.Column)]
                params = _owned(conj, select, _PARAMS)
                if isinstance(conj, exp.EQ) and isinstance(conj.this, exp.Column) and isinstance(conj.expression, exp.Column):
                    a, b = resolve(conj.this), resolve(conj.expression)
                    if a and b and a[0] != b[0]:
                        shape.joins.append((a, b))
                        mark(conj, "J")
                        continue
                mark(conj, role)
                if type(conj) in _RANGE and isinstance(conj.this, exp.Column) and isinstance(conj.expression, exp.Column):
                    a, b = resolve(conj.this), resolve(conj.expression)
                    if a and b:
                        direct.append((a, b))
                names = [f"${p.name.removeprefix('__p')}" for p in params]
                for key in filter(None, cols):
                    candidates.setdefault(key, set()).update(names)
                if params:
                    pred = _param_predicate(conj, select, resolve, names)
                    if pred:
                        shape.params.append(pred)
        for key in ("group", "order"):
            if select.args.get(key):
                mark(select.args[key], key[0].upper())

    shape.spread_pairs = _spread_pairs(shape, candidates, direct, schema)
    return shape


def _param_predicate(conj, select, resolve, names) -> ParamPredicate | None:
    def has_param(n: exp.Expression) -> bool:
        return any(isinstance(x, _PARAMS) for x in n.find_all(_PARAMS))

    text = re.sub(r":__p(\d+)", r"$\1", conj.sql())
    if isinstance(conj, exp.In):
        subject, op = conj.this, "in"
    elif isinstance(conj, (exp.Like, exp.ILike)):
        subject, op = conj.this, "like"
    elif isinstance(conj, exp.Between):
        subject, op = conj.this, "between"
    elif type(conj) in _RANGE or isinstance(conj, exp.EQ):
        left, right = conj.this, conj.expression
        op = "=" if isinstance(conj, exp.EQ) else _RANGE[type(conj)]
        if has_param(left) and not has_param(right):
            left, right = right, left
            op = _FLIP.get(op, op)
        subject = left
    else:
        return None
    aggs = _owned(subject, select, _AGGS)
    cols = [resolve(c) for c in _owned(subject, select, exp.Column)]
    cols = [c for c in cols if c]
    if not cols:
        return None
    if aggs:
        group = [resolve(c) for c in _owned(select.args["group"], select, exp.Column)] if select.args.get("group") else []
        group = [g[1] for g in group if g and g[0] == cols[0][0]]
        return ParamPredicate(text, names, cols[0][0], cols[0][1], "agg" + op, group, subject.sql())
    return ParamPredicate(text, names, cols[0][0], cols[0][1], op)


def _spread_pairs(shape, candidates, direct, schema):
    linked = {frozenset((a, b)): (a, b) for a, b in shape.joins}
    tables_linked = {frozenset((a[0], b[0])) for a, b in shape.joins}

    def family(key):
        return type_family(schema.get(key[0], {}).get(key[1], ""))

    out: list[tuple[tuple[str, str], tuple[str, str], str]] = []
    seen: set[frozenset] = set()

    def add(a, b, why):
        key = frozenset((a, b))
        if a == b or key in seen or family(a) != family(b) or family(a) == "other":
            return
        if schema[a[0]][a[1]].split("(")[0] != schema[b[0]][b[1]].split("(")[0]:
            return
        if a[0] != b[0] and frozenset((a[0], b[0])) not in tables_linked:
            return
        seen.add(key)
        out.append((a, b, why))

    for a, b in direct:
        add(a, b, "compared")
    keys = [k for k in candidates if family(k) != "other"]
    for i, a in enumerate(keys):
        for b in keys[i + 1:]:
            shared = candidates[a] & candidates[b]
            if shared:
                add(a, b, "shares " + ",".join(sorted(shared)))
            elif family(a) == "date":
                add(a, b, "same conjunction")
    return out[:MAX_SPREAD_PAIRS]


class Probe:
    """Runs time-boxed statements through a Runner within a total time budget (statements in neutral SQL)."""

    def __init__(self, runner: Runner, statement_seconds: float = STATEMENT_SECONDS, total_seconds: float = TOTAL_SECONDS) -> None:
        self.runner = runner
        self.statement_seconds = statement_seconds
        self.deadline = time.monotonic() + total_seconds
        self.timeouts = 0
        self.executed: list[str] = []  # the statements run (as sent to the engine), for the caller's log

    def run(self, sql: str) -> list[list[str | None]] | None:
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            self.timeouts += 1
            return None
        try:
            sent = sqlglot.transpile(sql, write=self.runner.dialect)[0]
        except sqlglot.errors.SqlglotError:
            sent = sql
        self.executed.append(sent)
        try:
            return self.runner.rows(sent, min(self.statement_seconds, remaining))
        except (RuntimeError, TimeoutError):
            self.timeouts += 1
            return None

    def count(self, sql: str) -> int | None:
        res = self.run(sql)
        return None if not res else num(res[0][0])


def _q(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


_NUMBER = re.compile(r"^-?\d+(\.\d+)?([eE][-+]?\d+)?$")


def literal(value: Any) -> str:
    """A SQL literal for a value: numbers bare, anything else a quoted string."""
    text = str(value)
    return text if _NUMBER.match(text.strip()) else "'" + text.replace("'", "''") + "'"


def sample_of(table: str, rows: int, types: dict[str, str]) -> tuple[str, float]:
    """(a FROM item over `table`, factor that rescales counts over it): the table itself, or above
    SAMPLE_ROWS a sample by key modulo on its first integer column (a prefix of the table when it has none).
    Key-modulo samples keep every row of a sampled key, so per-key counts stay exact; a sample over a
    non-key column is approximate."""
    if rows <= SAMPLE_ROWS:
        return _q(table), 1.0
    key = next((c for c, t in types.items() if is_integer(t)), None)
    if key:
        step = -(-rows // SAMPLE_ROWS)
        return f"(SELECT * FROM {_q(table)} WHERE {_q(key)} % {step} = 0)", float(step)
    return f"(SELECT * FROM {_q(table)} LIMIT {SAMPLE_ROWS})", rows / SAMPLE_ROWS


def _ranked(values_sql: str) -> str:
    """Rows (v, rn, n): the non-NULL values of a query's column `v` with their rank and the count."""
    return (f"SELECT v, ROW_NUMBER() OVER (ORDER BY v) AS rn, COUNT(*) OVER () AS n "
            f"FROM ({values_sql}) AS i WHERE v IS NOT NULL")


def _quantile_cells(qs: list[float]) -> str:
    return ", ".join(f"MIN(CASE WHEN rn >= n * {q} THEN v END)" for q in qs)


def distribution(probe: Probe, values_sql: str) -> tuple[int, Any, list[Any], Any] | None:
    """(count, min, [p1, p50, p99], max) of the non-NULL column `v` of `values_sql`."""
    res = probe.run(f"SELECT COUNT(*), MIN(v), {_quantile_cells(QUANTILES)}, MAX(v) FROM ({_ranked(values_sql)}) AS w")
    if not res or num(res[0][0]) is None:
        return None
    r = res[0]
    return int(num(r[0])), r[1], list(r[2:2 + len(QUANTILES)]), r[-1]


def column_stats(probe, table, columns, types, rows):
    """{column: dict(null_frac, ndv, min, max, q)}: one pass for the counts, one ordered pass per numeric or date column."""
    src, _ = sample_of(table, rows, types)
    parts = [f"COUNT({_q(c)}), COUNT(DISTINCT {_q(c)}), MIN({_q(c)}), MAX({_q(c)})" for c in columns]
    res = probe.run(f"SELECT COUNT(*), {', '.join(parts)} FROM {src} AS t")
    if not res:
        return {}
    row, out = res[0], {}
    total = num(row[0]) or 0
    for i, c in enumerate(columns):
        nn, ndv, lo, hi = row[1 + 4 * i: 5 + 4 * i]
        q = None
        if type_family(types[c]) != "other":
            dist = distribution(probe, f"SELECT {_q(c)} AS v FROM {src} AS t")
            q = dist[2] if dist else None
        out[c] = {"null_frac": 1 - (num(nn) or 0) / total if total else 0.0, "ndv": num(ndv), "min": lo, "max": hi, "q": q}
    return out


def fanout(probe, table, column, other, other_col, rows, types):
    src, _ = sample_of(table, rows, types)
    per_key = f"SELECT COUNT(*) AS v FROM {src} AS t WHERE {_q(column)} IS NOT NULL GROUP BY {_q(column)}"
    dist = probe.run(
        f"SELECT COUNT(*), AVG(v), MIN(CASE WHEN rn >= n * 0.99 THEN v END), MAX(v) FROM ({_ranked(per_key)}) AS w"
    )
    match = probe.run(
        f"SELECT AVG(CASE WHEN o.k IS NULL THEN 0.0 ELSE 1.0 END) FROM {src} AS t LEFT JOIN "
        f"(SELECT DISTINCT {_q(other_col)} AS k FROM {_q(other)}) AS o ON t.{_q(column)} = o.k"
    )
    res = None
    if dist and num(dist[0][0]):
        n, avg, p99, hi = dist[0]
        res = (int(num(n)), num(avg), num(p99), num(hi))
    return res, (num(match[0][0]) if match else None)


def param_effect(probe, pred: ParamPredicate, rows: dict[str, int], stats: dict, types: dict) -> dict[str, Any]:
    """Rows (or groups) qualifying at the extremes of the column's range, or by value frequency."""
    t, c, op = pred.table, pred.column, pred.op
    src, scale = sample_of(t, rows[t], types[t])
    st = stats.get(t, {}).get(c, {})
    fam = type_family(types[t][c])
    if op.startswith("agg"):
        keys = ", ".join(_q(g) for g in pred.group)
        by = f" GROUP BY {keys}" if keys else ""
        return {"kind": "agg", "res": distribution(probe, f"SELECT {pred.agg_sql} AS v FROM {src} AS t{by}")}
    if op in _FLIP and fam != "other" and st.get("q"):
        points = [("min", st["min"]), ("p1", st["q"][0]), ("p50", st["q"][1]), ("p99", st["q"][2]), ("max", st["max"])]
        counts = []
        for name, v in points:
            n = None if v is None else probe.count(f"SELECT COUNT(*) FROM {src} AS t WHERE {_q(c)} {op} {literal(v)}")
            counts.append((name, v, None if n is None else int(n * scale)))
        return {"kind": "range", "points": counts}
    if op in ("=", "in", "<>"):
        per_value = f"SELECT {_q(c)} AS v, COUNT(*) AS n FROM {src} AS t WHERE {_q(c)} IS NOT NULL GROUP BY {_q(c)}"
        dist = distribution(probe, f"SELECT n AS v FROM ({per_value}) AS g")
        if not dist:
            return {"kind": "freq", "what": "value", "res": None, "scale": scale}
        top, bottom = (probe.run(f"SELECT v FROM ({per_value}) AS g ORDER BY n {d} LIMIT 1") for d in ("DESC", "ASC"))
        res = (dist[0], num(dist[3]), num(dist[2][1]), num(dist[1]), top[0][0] if top else None, bottom[0][0] if bottom else None)
        return {"kind": "freq", "what": "value", "res": res, "scale": scale}
    return {"kind": "none"}


def diff_sql(a: str, b: str, family: str) -> str:
    """`a - b` over two column expressions (neutral SQL): days between dates, else the numeric difference."""
    if family == "date":
        return exp.DateDiff(this=sqlglot.parse_one(a), expression=sqlglot.parse_one(b), unit=exp.var("day")).sql()
    return f"CAST({a} AS DOUBLE) - CAST({b} AS DOUBLE)"


def spread(probe, a, b, shape: QueryShape, rows, types):
    (ta, ca), (tb, cb) = a, b
    family = type_family(types[ta][ca])
    sa, scale_a = sample_of(ta, rows[ta], types[ta])
    if ta == tb:
        res = distribution(probe, f"SELECT {diff_sql(_q(ca), _q(cb), family)} AS v FROM {sa} AS a")
        return res, rows[ta] > SAMPLE_ROWS
    link = next(((x, y) for x, y in shape.joins if {x[0], y[0]} == {ta, tb}), None)
    if not link:
        return None, False
    ka, kb = (link[0][1], link[1][1]) if link[0][0] == ta else (link[1][1], link[0][1])
    sb, _ = sample_of(tb, rows[tb], types[tb])
    res = distribution(
        probe,
        f"SELECT {diff_sql(f'a.{_q(ca)}', f'b.{_q(cb)}', family)} AS v FROM {sa} AS a JOIN {sb} AS b "
        f"ON a.{_q(ka)} = b.{_q(kb)}",
    )
    return res, max(rows[ta], rows[tb]) > SAMPLE_ROWS


DEFAULT_CATALOG_SQL = (
    "SELECT table_name, column_name, data_type FROM information_schema.columns "
    "WHERE table_schema NOT LIKE '%schema' AND table_schema NOT LIKE '%catalog' AND table_schema <> 'sys' "
    "ORDER BY table_name, ordinal_position"
)


def read_schema(runner: Runner, seconds: float, catalog_sql: str = "") -> dict[str, dict[str, str]]:
    """{table: {column: data_type}} from the engine's catalog."""
    schema: dict[str, dict[str, str]] = {}
    for t, c, d in runner.rows(catalog_sql or DEFAULT_CATALOG_SQL, seconds):
        schema.setdefault(t, {})[c] = d or ""
    return schema


def compute(sql: str, runner: Runner, schema: dict[str, dict[str, str]], statement_seconds: float = STATEMENT_SECONDS,
            total_seconds: float = TOTAL_SECONDS) -> dict[str, Any]:
    shape = analyze_query(sql, schema, runner.dialect)
    probe = Probe(runner, statement_seconds, total_seconds)
    used = {k[0] for k in shape.roles} | {a[0] for a, _ in shape.joins} | {b[0] for _, b in shape.joins}
    tables = sorted(used)
    rows = {t: probe.count(f"SELECT COUNT(*) FROM {_q(t)}") or 0 for t in tables}
    stats = {
        t: column_stats(probe, t, [c for (tt, c) in shape.roles if tt == t], schema[t], rows[t]) for t in tables
    }
    return {
        "schema": schema, "shape": shape, "rows": rows, "stats": stats,
        "fanout": [
            (a, b, fanout(probe, a[0], a[1], b[0], b[1], rows[a[0]], schema[a[0]]),
             fanout(probe, b[0], b[1], a[0], a[1], rows[b[0]], schema[b[0]]))
            for a, b in shape.joins
        ],
        "effects": [(p, param_effect(probe, p, rows, stats, schema)) for p in shape.params],
        "spreads": [(a, b, why, *spread(probe, a, b, shape, rows, schema)) for a, b, why in shape.spread_pairs],
        "timeouts": probe.timeouts,
    }


def dependent_table_names(workload: dict[str, Any]) -> list[str]:
    deps = workload.get("dependent_tables") or workload.get("tables") or []
    names = [str(d if isinstance(d, str) else d.get("name") or "") for d in deps if isinstance(d, (str, dict))]
    return list(dict.fromkeys(n for n in names if n))  # in order, without repeats


def fmt(v: Any) -> str:
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.4g}"
    s = str(v)
    return s if len(s) <= 48 else s[:47] + "…"


def md_table(headers: list[str], rows: list[list[Any]]) -> list[str]:
    out = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    out += ["| " + " | ".join(fmt(c) for c in r) + " |" for r in rows]
    return out + [""]


def _quant(q: Any) -> list[Any]:
    return list(q) if q else [None, None, None]


def render_shape_tables(facts: dict[str, Any], source: str) -> list[str]:
    rows, stats, schema = facts["rows"], facts["stats"], facts["schema"]
    roles = facts["shape"].roles
    lines = [
        f"- Source: {source} (iteration scale). Row counts and fan-outs scale roughly linearly with the "
        "scale factor to SF3000; value ranges, quantiles, NULL fractions, join-key matches and cross-column "
        "spreads (differences) do not scale, and NDV scales only for keys that grow with the data.",
        f"- Statistics are exact below {SAMPLE_ROWS:,} rows per table and sampled above (counts rescaled); "
        f"each statement is time-boxed ({facts['timeouts']} timed out or skipped).",
        "",
        "### Tables and columns the query uses",
        "",
    ]
    lines += md_table(["table", "rows"], [[t, f"{n:,}"] for t, n in rows.items()])
    col_rows = []
    for (t, c), r in sorted(roles.items()):
        st = stats.get(t, {}).get(c)
        if not st:
            continue
        q1, q50, q99 = _quant(st["q"])
        col_rows.append([f"{t}.{c}", schema[t][c], "".join(sorted(r)), f"{st['null_frac']:.1%}", st["ndv"],
                         st["min"], q1, q50, q99, st["max"]])
    lines += ["Roles: P predicate, J join key, G group by, O order by. NDV is approximate.", ""]
    lines += md_table(["column", "type", "roles", "NULL", "~NDV", "min", "p1", "p50", "p99", "max"], col_rows)

    if facts["fanout"]:
        lines += ["### Join keys: rows per key (distinct keys present, avg / p99 / max) and match rate", ""]
        fr = []
        for a, b, fa, fb in facts["fanout"]:
            for key, (res, match), other in ((a, fa, b), (b, fb, a)):
                if res:
                    fr.append([f"{key[0]}.{key[1]} -> {other[0]}.{other[1]}", f"{res[0]:,}", res[1], res[2], res[3],
                               "-" if match is None else f"{match:.1%}"])
        lines += md_table(["side (key -> joined to)", "keys", "avg", "p99", "max", "rows with a partner"], fr)

    param_rows = [(p, e) for p, e in facts["effects"]]
    if param_rows:
        lines += ["### Parameterised predicates: rows qualifying across the parameter's range", ""]
        for p, e in param_rows:
            head = f"- `{p.sql}` on `{p.table}.{p.column}` ({rows.get(p.table, 0):,} rows)"
            if e["kind"] == "range":
                pts = ", ".join(f"{n}={fmt(v)}: {c if c is None else format(c, ',')}" for n, v, c in e["points"])
                lines.append(f"{head}: rows qualifying with the parameter at column {pts}")
            elif e["kind"] == "freq" and e["res"]:
                n, hi, med, lo, hv, lv = e["res"]
                what = e["what"]
                lines.append(
                    f"{head}: {n:,} distinct {what}s; rows per {what} most frequent {hi:,} (`{fmt(hv)}`), "
                    f"median {med:,}, least frequent {lo:,} (`{fmt(lv)}`)"
                )
            elif e["kind"] == "agg" and e["res"]:
                n, lo, qs, hi = e["res"]
                lines.append(
                    f"{head}: aggregate `{p.agg_sql}` per group over {n:,} groups: min {fmt(lo)}, p1 {fmt(qs[0])}, "
                    f"p50 {fmt(qs[1])}, p99 {fmt(qs[2])}, max {fmt(hi)}; groups qualifying = those above the bound"
                )
            else:
                lines.append(f"{head}: (no statistic: unsupported shape or timed out)")
        lines.append("")

    if facts["spreads"]:
        lines += ["### Cross-column spreads (first column minus second, over joined rows; dates in days)", ""]
        sr = []
        for a, b, why, res, sampled in facts["spreads"]:
            if res:
                n, lo, qs, hi = res
                sr.append([f"{a[0]}.{a[1]} - {b[0]}.{b[1]}", why, f"{n:,}" + (" (sample)" if sampled else ""),
                           lo, qs[0], qs[1], qs[2], hi])
        lines += md_table(["difference", "why", "joined rows", "min", "p1", "p50", "p99", "max"], sr)
    return lines


DATA_SECTION = """## Data

Measure the data; do not guess it. The iteration dataset is loaded in the target engine (`{engine}`); the tools
below query it there.
{counts}
Row counts and fan-outs scale roughly linearly with the scale factor to SF3000; value ranges, quantiles, NULL
fractions, join-key match rates and spreads between columns (differences) do not, and NDV scales only for keys
that grow with the data.

Tools (time-boxed, capped, logged to `data_queries.jsonl`):
`query_data(sql)` one read-only statement in the engine's SQL; `column_stats(["table.column"])`;
`join_fanout(left, right)`; `rows_qualifying(predicate_sql, values)` with `$1` bound to each value;
`column_spread(a, b, join_sql)`; `query_shape()` the statistics the query's own SQL implies.

Use them for two things. Describe the data: rows qualifying at each binding extreme, fan-outs, spreads between
columns the predicates combine. Then test each candidate design against it: express the candidate's read as SQL
over the base tables (the entries it keeps, the bound or partition it prunes with, the stopping rule it applies)
and count what it would visit at representative and extreme bindings. CTEs and window functions express most
designs in one statement. A candidate's units read come from that count, not from a guess; when the count
contradicts the design, revise the design. Record each figure you rely on in `evidence` with the call that
produced it, so a reviewer can re-run it. Do not hardcode iteration-scale capacities into the implementation."""


def build_data_section(engine: str, counts: dict[str, int]) -> str:
    """The analysis prompt's Data section: the engine holding the dataset, its tables' row counts, and the tools."""
    table = "\n".join(md_table(["table", "rows"], [[t, f"{n:,}"] for t, n in counts.items()])) if counts else ""
    return DATA_SECTION.format(engine=engine or "unset", counts=f"\n{table}" if table else "")


def catalog_sql(workload: dict[str, Any], repo: Path | None, query: int | None) -> str:
    """The parameterised catalog text ($n placeholders) when the benchmark has one."""
    if repo is None or query is None:
        return ""
    path = repo / "workloads" / str(workload.get("benchmark_name", "")) / f"q{query}.sql"
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def build_data_shape_markdown(sql: str, runner: Any, schema: dict[str, dict[str, str]], source: str,
                              statement_seconds: float, total_seconds: float) -> str:
    """The statistics `sql` (the parameterised query) implies, computed on the engine behind `runner`."""
    facts = compute(sql, runner, schema, statement_seconds, total_seconds)
    return "\n".join(
        ["## Data shape (iteration scale)", "",
         "Facts to bound read work over all parameter bindings and to narrow candidate ranges from column "
         "spreads and correlations.", ""] + render_shape_tables(facts, source)
    )


SUBSET_ROOT_ROWS = 40_000  # rows of the largest table in the quick cut
TINY_ROOT_ROWS = 1_500  # the adversarial phase needs a few rows the point-write probes can invalidate
CUT_STATEMENT_SECONDS = 900.0
FORMAT_VERSION = 8
ANSWER_KEY_SHARE = 4  # the binding's keys fill at most 1/this of a root's sample
ANSWER_KEY_SECONDS = 300.0
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


def foreign_key_links(schema_sql: str) -> list[tuple[tuple[str, str], tuple[str, str]]]:
    """The foreign keys a schema declares, column-level REFERENCES and table-level FOREIGN KEY alike, as
    ((table, column), (referenced table, column)): a cut keeps the rows a child references, whatever the key's type."""
    links = []
    for create in sqlglot.parse(schema_sql):
        if not isinstance(create, exp.Create) or not isinstance(create.this, exp.Schema):
            continue
        table = create.this.this.name
        for item in create.this.expressions:
            if isinstance(item, exp.ColumnDef):
                links += [((table, item.name), (c.kind.this.this.name, c.kind.this.expressions[0].name))
                          for c in item.constraints if isinstance(c.kind, exp.Reference)]
            elif isinstance(item, exp.ForeignKey):
                ref = item.args["reference"].this
                links += [((table, col.name), (ref.this.name, rcol.name)) for col, rcol in zip(item.expressions, ref.expressions)]
    return links


def key_links(schema: dict[str, dict[str, str]]) -> list[tuple[tuple[str, str], tuple[str, str]]]:
    """Integer key columns of different tables that name the same key: equal once a short table prefix (`l_`, `ps_`)
    is stripped, and ending in `key` or `id`."""
    named: dict[str, list[tuple[str, str]]] = {}
    for table, columns in schema.items():
        for column, dtype in columns.items():
            name = column.lower()
            name = name.split("_", 1)[1] if "_" in name[:4] else name
            if name not in ("key", "id") and name.endswith(("key", "id")) and is_integer(dtype):
                named.setdefault(name, []).append((table, column))
    return [(a, b) for columns in named.values() for i, a in enumerate(columns) for b in columns[i + 1:] if a[0] != b[0]]


def cut_selects(tables: list[str], joins: list[tuple[tuple[str, str], tuple[str, str]]], rows: dict[str, int],
                integer_columns: dict[str, list[str]], root_rows: int, answer_keys: AnswerKeys | None = None) -> dict[str, str]:
    """One SELECT per table returning its rows in the cut. Each group of tables the joins connect has a root, its
    largest table, sampled by key on an integer column (a join key when it has one) plus the keys the query answers
    from (`answer_keys`); every other table keeps the rows that join to its neighbour nearer the root."""
    adjacent: dict[str, list[tuple[str, str, str]]] = {t: [] for t in tables}  # table -> (its column, other, other's column)
    for (ta, ca), (tb, cb) in joins:
        if ta in adjacent and tb in adjacent:
            adjacent[ta].append((ca, tb, cb))
            adjacent[tb].append((cb, ta, ca))
    selects: dict[str, str] = {}
    for root in sorted(tables, key=lambda t: -rows[t]):
        if root in selects:
            continue
        join_columns = [c for c, _, _ in adjacent[root]]
        key = _key_column(join_columns, integer_columns[root])
        extra = answer_keys(root, key, integer_columns[root], root_rows) if answer_keys and key and rows[root] > root_rows else []
        selects[root] = _sample(root, rows[root], root_rows, join_columns, integer_columns[root], extra)
        queue = [root]
        while queue:
            parent = queue.pop(0)
            for parent_column, table, column in adjacent[parent]:
                if table not in selects:
                    selects[table] = f"SELECT * FROM {table} WHERE {column} IN (SELECT {parent_column} FROM ({selects[parent]}) AS kept)"
                    queue.append(table)
    return selects


def _key_column(join_columns: list[str], integers: list[str]) -> str | None:
    """The column a root is sampled by: an integer join key, else the first join key."""
    return next((c for c in join_columns if c in integers), join_columns[0] if join_columns else None)


def _sample(table: str, rows: int, root_rows: int, join_columns: list[str], integers: list[str], extra_keys: list[str]) -> str:
    """Every P-th key value, P a prime near rows / root_rows: an even spread over the key range (no bias to early
    keys), and a prime stride cannot line up with the blocks a generator numbers its keys in. An integer join key is
    sampled by value; a text one by its rank among the distinct keys (a modulo of another integer column would keep
    whatever few values happen to divide, e.g. none of a handful of dates). `extra_keys` (SQL literals) are kept too.
    A table no key links to any other has no neighbour to stay consistent with: its first rows."""
    if rows <= root_rows:
        return f"SELECT * FROM {table}"
    if not join_columns:
        order = f" ORDER BY {', '.join(integers)}" if integers else ""
        return f"SELECT * FROM {table}{order} LIMIT {root_rows}"
    stride = math.ceil(rows / root_rows)
    while any(stride % d == 0 for d in range(2, math.isqrt(stride) + 1)):
        stride += 1
    key = _key_column(join_columns, integers)
    if key in integers:
        even = f"{key} % {stride} = 0"
    else:
        ranked = f"SELECT {key}, ROW_NUMBER() OVER (ORDER BY {key}) AS key_rank FROM (SELECT DISTINCT {key} FROM {table}) AS keys"
        even = f"{key} IN (SELECT {key} FROM ({ranked}) AS ranked WHERE key_rank % {stride} = 0)"
    answered = f" OR {key} IN ({', '.join(extra_keys)})" if extra_keys else ""
    return f"SELECT * FROM {table} WHERE {even}{answered}"


class AnswerKeys:
    """The keys of the rows a query answers from at its binding. Each select of the bound query that reads a table
    gives a key-only select over the same FROM/JOIN/WHERE (ordered as the query is, so a top-k keeps its top rows
    first); the keys of a root are bounded to a share of its sample."""

    def __init__(self, runner: HostSql, sql: str, schema: dict[str, dict[str, str]], rows: dict[str, int]) -> None:
        self.runner, self.rows = runner, rows
        self.dialect = runner.dialect or None
        try:
            self.scopes = traverse_scope(qualify(parse_template(sql, self.dialect), schema=schema, validate_qualify_columns=False))[::-1]
        except sqlglot.errors.SqlglotError:
            self.scopes = []
        self.fetched: dict[tuple[str, str], list[str]] = {}
        self.distinct: dict[tuple[str, str], int] = {}

    def __call__(self, table: str, column: str, integers: list[str], root_rows: int) -> list[str]:
        """SQL literals of the `table` keys on `column` that fit the share of a sample of root_rows rows."""
        if (table, column) not in self.fetched:
            self.fetched[table, column] = self._fetch(table, column, SUBSET_ROOT_ROWS)
        keys = self.fetched[table, column][: self._bound(table, column, root_rows)]
        return [str(int(k)) if column in integers else "'" + k.replace("'", "''") + "'" for k in keys]

    def _bound(self, table: str, column: str, root_rows: int) -> int:
        """Keys that stay within the share, at the table's average rows per key."""
        if (table, column) not in self.distinct:
            counted = self.runner.rows(f"SELECT COUNT(DISTINCT {column}) FROM {table}", CUT_STATEMENT_SECONDS)
            self.distinct[table, column] = int(counted[0][0] or 1)
        return max(1, root_rows // ANSWER_KEY_SHARE * self.distinct[table, column] // max(self.rows[table], 1))

    def _fetch(self, table: str, column: str, root_rows: int) -> list[str]:
        limit = self._bound(table, column, root_rows)
        keys: dict[str, None] = {}
        for scope in self.scopes:
            for alias, source in scope.sources.items():
                if isinstance(source, exp.Table) and source.name.lower() == table.lower() and isinstance(scope.expression, exp.Select):
                    try:
                        found = self.runner.rows(self._key_select(scope.expression, alias, column, limit), ANSWER_KEY_SECONDS)
                    except (RuntimeError, TimeoutError):
                        continue  # e.g. a select correlated with its outer query: the even sample stands
                    keys.update((row[0], None) for row in found if row[0] is not None)
        return list(keys)

    def _key_select(self, select: exp.Select, alias: str, column: str, limit: int) -> str:
        aliases = {e.alias: e.this for e in select.expressions if isinstance(e, exp.Alias)}
        keyed = select.copy()
        for arg in ("group", "having", "qualify", "distinct", "offset"):
            keyed.set(arg, None)
        keyed.set("expressions", [exp.column(column, table=alias)])
        keyed.set("limit", exp.Limit(expression=exp.Literal.number(limit)))
        if order := keyed.args.get("order"):
            order = order.transform(lambda n: aliases[n.name].copy() if isinstance(n, exp.Column) and not n.table and n.name in aliases else n)
            if order.find(exp.AggFunc, exp.Window) or any(isinstance(o.this, exp.Literal) for o in order.expressions):
                order = None  # ordered by an aggregate or a select position, not by this table's columns
            keyed.set("order", order)
        return keyed.sql(dialect=self.dialect)


def _tbl_field(value: str | None) -> str:
    """A .tbl field: NULL is empty, the empty string is `""`, a value with a quote is quoted (host_contract.md)."""
    if value is None:
        return ""
    return '"' + value.replace('"', '""') + '"' if value == "" or '"' in value else value


def _write_cut(runner: HostSql, selects: dict[str, str], directory: Path) -> dict[str, int]:
    directory.mkdir(parents=True)
    counts: dict[str, int] = {}
    for table, select in selects.items():
        lines = []
        for row in runner.rows(select, CUT_STATEMENT_SECONDS):
            if any(v is not None and ("|" in v or "\n" in v) for v in row):
                raise ValueError(f"{table} holds a value with '|' or a newline: not representable in a .tbl file")
            lines.append("|".join(_tbl_field(v) for v in row) + "|\n")
        (directory / f"{table}.tbl").write_text("".join(lines), encoding="utf-8")
        counts[table] = len(lines)
    if not any(counts.values()):
        raise ValueError("the sample holds no rows: the key range is smaller than the sampling stride")
    return counts


def _bound_sql(workload: dict[str, Any]) -> str:
    """The query at its default binding (the workload's reference SQL); empty when there is none."""
    path = Path(str(workload.get("reference_sql_path") or ""))
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def _key(cfg: PipelineConfig, workload: dict[str, Any], query: int, source: Path) -> str:
    bench = str(workload.get("benchmark_name") or "")
    sql = catalog_sql(workload, cfg.repo_root, query)
    schema = cfg.repo_root / workload_schema_sql(str(workload.get("dataset_benchmark") or bench))
    digest = hashlib.sha256(f"{FORMAT_VERSION} {SUBSET_ROOT_ROWS} {TINY_ROOT_ROWS} {source.resolve()}".encode())
    digest.update(sql.encode() + (schema.read_bytes() if schema.is_file() else b"") + _bound_sql(workload).encode())
    for tbl in sorted(source.glob("*.tbl")):
        digest.update(f"{tbl.name}{tbl.stat().st_size}{tbl.stat().st_mtime_ns}".encode())
    return digest.hexdigest()[:16]


def _cut(cfg: PipelineConfig, workload: dict[str, Any], query: int, source: Path, destination: Path) -> dict[str, Any]:

    runner = HostSql(DataToolContext(cfg, workload, query, query_artifacts_dir(cfg.repo_root, query, workload=workload)))
    runner.require_up()
    schema = read_schema(runner, 60.0, runner.catalog_sql)
    names = {t.lower(): t for t in schema}
    tables = [names[p.stem.lower()] for p in sorted(source.glob("*.tbl")) if p.stem.lower() in names]
    for name in tables + [c for t in tables for c in schema[t]]:
        if not _IDENTIFIER.match(name):
            raise ValueError(f"{name!r} is not a plain identifier")
    rows = {t: int(runner.rows(f"SELECT COUNT(*) FROM {t}", CUT_STATEMENT_SECONDS)[0][0] or 0) for t in tables}
    schema_sql = (cfg.repo_root / workload_schema_sql(str(workload.get("dataset_benchmark") or benchmark_of(workload)))).read_text(encoding="utf-8")
    joins = (analyze_query(catalog_sql(workload, cfg.repo_root, query), schema, runner.dialect).joins
             + foreign_key_links(schema_sql) + key_links(schema))
    integers = {t: [c for c, dtype in schema[t].items() if is_integer(dtype)] for t in tables}
    answer_keys = AnswerKeys(runner, bound, schema, rows) if (bound := _bound_sql(workload)) else None
    manifest: dict[str, Any] = {"source_rows": rows}
    for name, root_rows in (("subset", SUBSET_ROOT_ROWS), ("tiny", TINY_ROOT_ROWS)):
        manifest[f"{name}_rows"] = _write_cut(runner, cut_selects(tables, joins, rows, integers, root_rows, answer_keys), destination / name)
    return manifest


class CutError(RuntimeError):
    """The test cuts cannot be made: the workload's data is at fault (data-owned). There is no fallback to the full
    dataset, whose quick tier would run for hours at scale and hide the cause."""


def ensure_cut_datasets(cfg: PipelineConfig, workload: dict[str, Any], query: int) -> CutDatasets:
    """The cuts of this query's tables, made once per dataset (cached by a content key; ACCEL_SUBSET_DATASET_DIR
    names ready-made ones). A cut that cannot be made records its reason with the key (.failed) and raises CutError
    on every call, without trying again; changing the data or the SQL changes the key."""
    if given := os.environ.get("ACCEL_SUBSET_DATASET_DIR"):
        return CutDatasets(Path(given), Path(given), {"source_rows": {}, "subset_rows": {}})
    source = Path(str(workload.get("dataset_dir") or os.environ.get("ACCEL_DATASET_DIR") or ""))
    if not source.is_dir() or not any(source.glob("*.tbl")):
        raise CutError(f"no test databases: the dataset directory {str(source)!r} is missing or holds no .tbl files")
    bench = str(workload.get("benchmark_name") or "")
    destination = Path(f"{cfg.build_dir}.subset") / f"{bench}-q{query}-{_key(cfg, workload, query, source)}"
    destination.parent.mkdir(parents=True, exist_ok=True)
    with (destination.parent / f"{destination.name}.lock").open("a") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        failed = destination.with_name(f"{destination.name}.failed")
        if failed.is_file():
            raise CutError(f"{failed.read_text(encoding='utf-8')} (recorded in {failed}; delete it to try again)")
        if not (destination / ".done").is_file():
            started = time.monotonic()
            try:
                problem = ensure_dataset_loaded(workload, cfg.repo_root, cfg.build_dir)
                if problem:
                    raise RuntimeError(problem)
                manifest = _cut(cfg, workload, query, source, destination)
            except (RuntimeError, ValueError, TimeoutError, OSError) as exc:
                shutil.rmtree(destination, ignore_errors=True)
                failed.write_text(f"the test databases of Q{query} cannot be made: {type(exc).__name__}: {exc}", encoding="utf-8")
                raise CutError(failed.read_text(encoding="utf-8")) from exc
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


def parse_csv(text: str) -> list[list[str | None]]:
    """CSV rows; a bare empty cell is NULL (None), a quoted one (`""`) the empty string."""
    rows: list[list[str | None]] = []
    row: list[str | None] = []
    cell: list[str] = []
    quoted = in_quotes = False
    i = 0
    while i < len(text):
        ch = text[i]
        if in_quotes:
            if ch == '"' and text[i + 1: i + 2] == '"':
                cell.append('"')
                i += 1
            elif ch == '"':
                in_quotes = False
            else:
                cell.append(ch)
        elif ch == '"':
            quoted = in_quotes = True
        elif ch in ",\n":
            row.append("".join(cell) if cell or quoted else None)
            cell, quoted = [], False
            if ch == "\n":
                rows.append(row)
                row = []
        elif ch != "\r":
            cell.append(ch)
        i += 1
    if cell or quoted or row:
        row.append("".join(cell) if cell or quoted else None)
        rows.append(row)
    return rows


class HostSql:
    """`host.sh sql -` of the run's engine instance: one statement in, CSV out (host_contract.md), time-boxed."""

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

    def _run(self, sql: str, seconds: float, max_lines: int) -> tuple[int, list[str], str]:
        """(exit code, first max_lines stdout lines, stderr); the process group is killed at the time box or the line cap."""
        # One statement at a time on the instance: an embedded engine's database file takes one process, and an agent
        # that issues several tool calls at once would otherwise get lock errors from all but the first.
        lock = (Path(self.env["ACCEL_ENGINE_STATE_DIR"]) / "data_tools.lock").open("a")
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            return self._run_locked(sql, seconds, max_lines)
        finally:
            lock.close()

    def _run_locked(self, sql: str, seconds: float, max_lines: int) -> tuple[int, list[str], str]:
        with tempfile.TemporaryFile("w+", encoding="utf-8") as err:  # a pipe could fill and stall the engine
            proc = subprocess.Popen(
                [str(host_script(self.cfg.repo_root, self.engine)), "sql", "-"], cwd=self.cfg.repo_root, env=self.env,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err, text=True, start_new_session=True,
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
            err.seek(0)
            return proc.returncode, lines, err.read()

    def _table(self, sql: str, seconds: float, max_rows: int) -> tuple[list[list[str | None]], bool]:
        code, lines, stderr = self._run(sql, seconds, max_rows + 2)  # header, the rows, one more to tell
        if code != 0 and len(lines) < max_rows + 2:
            raise RuntimeError((stderr or "\n".join(lines))[-1500:] or f"host.sh sql exited {code}")
        parsed = parse_csv("\n".join(lines))
        return parsed[: max_rows + 1], len(parsed) > max_rows + 1

    def query(self, sql: str, seconds: float, max_rows: int) -> tuple[list[list[str]], bool]:
        """(header + at most max_rows rows, more rows exist); NULL shows as an empty cell."""
        table, more = self._table(sql, seconds, max_rows)
        return [["" if c is None else c for c in row] for row in table], more

    def rows(self, sql: str, seconds: float) -> list[list[str | None]]:
        """The rows of a statement (no header), NULL as None and the empty string as ''."""
        return self._table(sql, seconds, 10_000_000)[0][1:]


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
        self.probe = Probe(self.runner, statement_seconds=seconds, total_seconds=seconds)
        try:
            self.schema = read_schema(self.runner, CATALOG_SECONDS, self.runner.catalog_sql)
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
            self._rows[table] = self.probe.count(f"SELECT COUNT(*) FROM {_q(table)}") or 0
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
        big = [f"{t} ({self.rows(t):,} rows)" for t in dict.fromkeys(tables) if self.rows(t) > SAMPLE_ROWS]
        if not big:
            return f"exact (tables are at most {SAMPLE_ROWS:,} rows)"
        return (f"sampled: about {SAMPLE_ROWS:,} rows of {', '.join(big)} (key modulo, or a prefix without an "
                "integer column); counts are rescaled, quantiles and NDV are estimates")

    def table_of(self, predicate: str) -> str:
        """The one table whose columns a predicate uses (qualified `table.column` or bare column names)."""
        tree = parse_template(f"SELECT 1 WHERE {predicate}", self.dialect)
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


PARAMS, DOMAIN, WRITE_MIX, MANIFEST, TXNS = "params.sql", "domain.yaml", "write_mix.yaml", "suite.yaml", "txns"
FROZEN_RECORD = "test_suite_frozen.json"
CHANGE_REQUEST = "test_suite_change_request.md"
ROLLBACK_MARK = "-- rollback"

EDGE_CLASSES = ("in_spec", "off_spec", "null_param", "empty_result", "limit_ties", "extreme_values", "large_keys")
TXN_KINDS = ("point_insert", "point_update", "point_delete", "bulk_insert", "bulk_update", "bulk_delete",
             "sequence", "rollback", "key_move", "other")
VERBS = ("INSERT", "UPDATE", "DELETE")
DOMAIN_TYPES = ("choice", "int", "decimal", "date", "pattern", "derived")


def suite_path(repo_root: Path, benchmark: str, query: int) -> Path:
    return repo_root / suite_dir(benchmark, query)


def gate_settings(pipeline_config: Any, workload: dict[str, Any], query: int) -> dict[str, Any]:
    """What the gates need to run the query's suite (the gate context's `test_suite`)."""
    from accel_pipeline.layout import workload_query_sql, workload_schema_constraints
    from accel_pipeline.config import aux_function_name, base_function_name

    root, bench = pipeline_config.repo_root, benchmark_of(workload)
    aux = workload.get("workload_kind") == "aux_state"
    constraints = workload_schema_constraints(bench)
    return {
        "dir": str(suite_path(root, bench, query)),
        "query_sql": str(root / workload_query_sql(bench, query)),
        "bound_sql": str(workload.get("reference_sql_path") or ""),  # the query at its default binding
        "function": (aux_function_name if aux else base_function_name)(bench, query),
        "arity": len(workload.get("parameters") or []),
        "param_names": [str(p["name"]) for p in workload.get("parameters") or []],
        "aux": aux,
        "schema_sql": str(root / constraints) if (root / constraints).is_file() else "",
        "config": asdict(pipeline_config.test_suite),
    }


# ---------------------------------------------------------------- reading


def read_yaml(path: Path) -> dict[str, Any]:
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError) as exc:
        raise ValueError(f"{path.name}: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError(f"{path.name} must be a mapping")
    return data


def dependent_tables(art: Path) -> list[str]:
    """The tables whose writes can change the query's answer, from the analysis."""
    try:
        facts = json.loads((art / "analysis.json").read_text(encoding="utf-8")).get("correctness_facts") or {}
    except (OSError, ValueError):
        return []
    return [str(t["name"]) for t in facts.get("dependent_tables") or [] if isinstance(t, dict) and t.get("name")]


def read_write_mix(suite: Path) -> dict[str, Any]:
    return read_yaml(suite / WRITE_MIX)


def read_params(path: Path) -> list[str]:
    """The tuples of a params file: one per non-empty, non-comment line."""
    if not path.is_file():
        return []
    return [ln.strip() for ln in path.read_text(encoding="utf-8").splitlines() if ln.strip() and not ln.lstrip().startswith("--")]


def txn_files(directory: Path) -> dict[str, str]:
    """{file name: SQL} of the write transactions, in the order they run."""
    return {f.name: f.read_text(encoding="utf-8") for f in sorted((directory / TXNS).glob("*.sql"))}


def statements(sql: str) -> list[tuple[str, str]]:
    """(verb, table) of each INSERT / UPDATE / DELETE statement of a transaction."""
    text = re.sub(r"--[^\n]*", "", sql)
    found = re.findall(r"(?:^|;)\s*(INSERT\s+INTO|UPDATE|DELETE\s+FROM)\s+([A-Za-z_]\w*)", text, re.I)
    return [(v.split()[0].upper(), t.lower()) for v, t in found]


def txn_profiles(suite: Path) -> dict[str, tuple[str, tuple[str, ...]]]:
    """{file name: (declared kind, tables written)} of the write transactions."""
    declared = read_yaml(suite / MANIFEST).get("txns") or {}
    return {name: (str((declared.get(name) or {}).get("kind") or "other"), tuple(sorted({t for _, t in statements(sql)})))
            for name, sql in txn_files(suite).items()}


def is_rollback(sql: str) -> bool:
    return sql.lstrip().lower().startswith(ROLLBACK_MARK)


# ---------------------------------------------------------------- parameter tuples (SQL literal lists)


def fresh_seed() -> int:
    """A new seed per call: the OS entropy pool (never the clock alone)."""
    return int.from_bytes(os.urandom(8), "little") % (2**31 - 1) + 1


def n_params(query_sql: str) -> int:
    """How many $n parameters the query takes (the highest n)."""
    nums = [int(m) for m in re.findall(r"\$(\d+)", query_sql)]
    return max(nums) if nums else 0


def split_tuple(t: str) -> list[str]:
    """The top-level comma-separated literals of a tuple (quotes and parentheses respected)."""
    out, depth, cur, i = [], 0, "", 0
    while i < len(t):
        c = t[i]
        if c == "'":
            j = i + 1
            while j < len(t):
                if t[j] == "'" and t[j + 1:j + 2] == "'":
                    j += 2
                    continue
                if t[j] == "'":
                    break
                j += 1
            cur += t[i:j + 1]
            i = j + 1
            continue
        if c in "([":
            depth += 1
        elif c in ")]":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    if cur.strip():
        out.append(cur.strip())
    return out


# ---------------------------------------------------------------- random draws (reproducible from a seed)


def _add_months(day: dt.date, months: int) -> dt.date:
    index = day.month - 1 + months
    year, month = day.year + index // 12, index % 12 + 1
    return dt.date(year, month, min(day.day, calendar.monthrange(year, month)[1]))


def _one(spec: dict[str, Any], rng: random.Random, drawn: dict[str, Any]) -> tuple[str, Any]:
    """(SQL literal, python value) of one parameter."""
    kind = spec["type"]
    if kind == "choice":
        return rng.choice(spec["values"]), None
    if kind == "int":
        value = rng.randint(int(spec["min"]), int(spec["max"]))
        return str(value), value
    if kind == "decimal":
        scale = int(spec.get("scale", 2))
        value = round(rng.uniform(float(spec["min"]), float(spec["max"])), scale)
        return f"{value:.{scale}f}", value
    if kind == "date":
        lo, hi = dt.date.fromisoformat(str(spec["min"])), dt.date.fromisoformat(str(spec["max"]))
        value = lo + dt.timedelta(days=rng.randint(0, (hi - lo).days))
        return f"DATE '{value.isoformat()}'", value
    if kind == "pattern":
        return "'" + str(spec["template"]).replace("{}", rng.choice(spec["words"])) + "'", None
    source = drawn[spec["from"]]
    value = _add_months(source, int(spec["add_months"]))
    return f"DATE '{value.isoformat()}'", value


def draw_tuples(domain: dict[str, Any], count: int, seed: int) -> list[str]:
    """`count` parameter tuples (SQL literals in $n order) from the domain; the same seed gives the same tuples."""
    rng = random.Random(seed)
    tuples = []
    for _ in range(count):
        drawn: dict[str, Any] = {}
        literals = []
        for spec in domain["params"]:
            literal, value = _one(spec, rng, drawn)
            drawn[spec["name"]] = value
            literals.append(literal)
        tuples.append(", ".join(literals))
    return tuples


def draw_write_frequencies(mix: dict[str, Any], count: int, seed: int) -> list[int]:
    """Write frequencies (% of operations) for one benchmark run: 0, then `count` distinct draws from the range
    (log-uniform or uniform), sorted; the same seed gives the same draw."""
    rng = random.Random(seed)
    spec = mix["frequency_pct"]
    lo, hi = int(spec["min"]), int(spec["max"])
    log = spec.get("distribution", "log") == "log"
    drawn: set[int] = set()
    for _ in range(count * 20):
        if len(drawn) >= min(count, hi - lo + 1):
            break
        drawn.add(round(lo * (hi / lo) ** rng.random()) if log else rng.randint(lo, hi))
    return [0, *sorted(drawn)]


# ---------------------------------------------------------------- validation


def _check_domain(domain: dict[str, Any], names: list[str], want: int) -> list[str]:
    params = domain.get("params")
    if not isinstance(params, list) or len(params) != want:
        return [f"{DOMAIN} needs one `params` entry per query parameter ({want}), in $n order"]
    problems = []
    for i, spec in enumerate(params, 1):
        kind = spec.get("type") if isinstance(spec, dict) else None
        if kind not in DOMAIN_TYPES or not spec.get("name"):
            problems.append(f"{DOMAIN} param {i}: needs a name and a type in {DOMAIN_TYPES}")
            continue
        needed = {"choice": ("values",), "int": ("min", "max"), "decimal": ("min", "max"), "date": ("min", "max"),
                  "pattern": ("template", "words"), "derived": ("from", "add_months")}[kind]
        problems += [f"{DOMAIN} param {spec['name']}: `{key}` missing" for key in needed if key not in spec]
    declared = [p.get("name") for p in params if isinstance(p, dict)]
    if names and len(names) == want and declared != names:
        problems.append(f"{DOMAIN} parameter names {declared} must be the workload's {names}")
    return problems


def _check_write_mix(mix: dict[str, Any], txn_kinds: set[str]) -> list[str]:
    spec = mix.get("frequency_pct")
    if not isinstance(spec, dict) or not all(isinstance(spec.get(k), (int, float)) for k in ("min", "max", "samples")):
        return [f"{WRITE_MIX} needs frequency_pct: {{min, max, samples, distribution}}"]
    if not 0 < spec["min"] <= spec["max"] <= 100 or spec["samples"] < 1:
        return [f"{WRITE_MIX} frequency_pct needs 0 < min <= max <= 100 and samples >= 1"]
    kinds = mix.get("kinds")
    if not isinstance(kinds, dict) or not kinds or not all(isinstance(w, (int, float)) and w > 0 for w in kinds.values()):
        return [f"{WRITE_MIX} needs `kinds`: relative weights (> 0) of the write kinds"]
    unknown = sorted(set(kinds) - txn_kinds)
    return [f"{WRITE_MIX} weights kinds no transaction has: {unknown}"] if unknown else []


def _check_edges(manifest: dict[str, Any], tuples: int) -> list[str]:
    edges = manifest.get("edge_coverage")
    if not isinstance(edges, dict):
        return [f"{MANIFEST} needs edge_coverage: {{class: [params.sql tuple numbers] | 'n/a: reason'}}"]
    problems = []
    for cls in EDGE_CLASSES:
        value = edges.get(cls)
        if isinstance(value, str):
            if not value.lower().startswith("n/a:") or len(value) < 12:
                problems.append(f"edge_coverage.{cls}: a string must be 'n/a: <why this class cannot apply>'")
        elif not isinstance(value, list) or not value or not all(isinstance(i, int) and 1 <= i <= tuples for i in value):
            problems.append(f"edge_coverage.{cls}: params.sql tuple numbers, 1-based, comments not counted (1..{tuples}), or 'n/a: reason'")
    return problems


_DATE_LITERAL = re.compile(r"^DATE\s*'(\d{4}-\d{2}-\d{2})'$", re.I)


def _check_derived(domain: dict[str, Any], tuples: list[str]) -> list[str]:
    """A fixed tuple must give every derived parameter the value the domain derives it as."""
    params = domain.get("params") or []
    names = [str(spec.get("name")) for spec in params]
    problems = []
    for j, spec in enumerate(params):
        if spec.get("type") != "derived" or spec.get("from") not in names:
            continue
        i, months = names.index(spec["from"]), int(spec.get("add_months") or 0)
        for n, t in enumerate(tuples, 1):
            values = split_tuple(t)
            if max(i, j) >= len(values) or not (source := _DATE_LITERAL.match(values[i].strip())):
                continue
            want = _add_months(dt.date.fromisoformat(source.group(1)), months).isoformat()
            if (got := _DATE_LITERAL.match(values[j].strip())) is None or got.group(1) != want:
                problems.append(f"{PARAMS} tuple {n}: {spec['name']} is derived as {spec['from']} + {months} month(s) "
                                f"= DATE '{want}', not {values[j].strip()}: {t}")
    return problems


def validate_static(suite: Path, *, query_sql: str, param_names: list[str], dependent_tables: list[str],
                    aux: bool) -> list[str]:
    """What is wrong with the suite's files, without running anything (empty: nothing)."""
    want = n_params(query_sql)
    problems: list[str] = []
    tuples = read_params(suite / PARAMS)
    if want and not tuples:
        problems.append(f"{PARAMS}: no tuples")
    problems += [f"{PARAMS} tuple {i} has {len(split_tuple(t))} values, the query takes {want}: {t}"
                 for i, t in enumerate(tuples, 1) if len(split_tuple(t)) != want]
    try:
        manifest = read_yaml(suite / MANIFEST)
    except ValueError as exc:
        return problems + [str(exc)]
    if want:
        try:
            domain = read_yaml(suite / DOMAIN)
            bad = _check_domain(domain, param_names, want)
            problems += bad
            if not bad:
                problems += _check_derived(domain, tuples)
                problems += [f"{DOMAIN} draws a tuple of {len(split_tuple(t))} values, the query takes {want}: {t}"
                             for t in draw_tuples(domain, 3, 1) if len(split_tuple(t)) != want]
        except ValueError as exc:
            problems.append(str(exc))
        except (KeyError, TypeError, AttributeError) as exc:
            problems.append(f"{DOMAIN} cannot draw tuples ({type(exc).__name__}: {exc}); a derived parameter needs a date source")
        problems += _check_edges(manifest, len(tuples))
    if not isinstance(manifest.get("rationale"), str) or not manifest["rationale"].strip():
        problems.append(f"{MANIFEST}: rationale string required")
    if not aux:
        return problems
    if not dependent_tables:
        problems.append("the analysis lists no dependent tables: write coverage cannot be checked")
    return problems + _validate_writes(suite, manifest, dependent_tables)


def _validate_writes(suite: Path, manifest: dict[str, Any], dependent_tables: list[str]) -> list[str]:
    problems: list[str] = []
    txns = txn_files(suite)
    declared = manifest.get("txns")
    if not txns:
        return [f"{TXNS}/: no write transactions"]
    if not isinstance(declared, dict) or set(declared) != set(txns):
        return [f"{MANIFEST} `txns` must list exactly the files of {TXNS}/ with a kind each"]
    kinds = {name: (spec or {}).get("kind") for name, spec in declared.items()}
    problems += [f"{MANIFEST} txns.{name}.kind must be one of {TXN_KINDS}" for name, k in kinds.items() if k not in TXN_KINDS]
    by_table: dict[str, set[str]] = {}
    for sql in txns.values():
        for verb, table in statements(sql):
            by_table.setdefault(table, set()).add(verb)
    waived = manifest.get("not_applicable") or {}
    for table in dependent_tables:
        for verb in VERBS:
            key = f"{table.lower()}:{verb}"
            if verb not in by_table.get(table.lower(), set()) and not str(waived.get(key, "")).strip():
                problems.append(f"no transaction runs {verb} on dependent table {table}: add one, or give "
                                f"not_applicable['{key}']: <why it cannot change this query's answer>")
    for kind in ("rollback", "key_move"):
        if kind not in kinds.values() and not str(waived.get(kind, "")).strip():
            problems.append(f"no `{kind}` transaction: add one, or give not_applicable['{kind}']: <why>")
    for name, sql in txns.items():
        if kinds.get(name) == "rollback" and not is_rollback(sql):
            problems.append(f"{TXNS}/{name} is a rollback transaction: its first line must be `{ROLLBACK_MARK}`")
    for key, label in (("point_writes", "point"), ("bulk_writes", "bulk")):
        if not isinstance(manifest.get(key), dict):
            problems.append(f"{MANIFEST}: {key} object required")
    point, bulk = manifest.get("point_writes") or {}, manifest.get("bulk_writes") or {}
    if isinstance(point, dict) and not (3 <= int(point.get("min_count", 0)) <= int(point.get("max_probes", 0)) <= 16):
        problems.append("point_writes needs 3 <= min_count <= max_probes <= 16")
    if isinstance(point, dict) and not point.get("tables"):
        problems.append("point_writes.tables must be a non-empty list")
    if isinstance(bulk, dict) and bulk.get("required") and int(bulk.get("min_chunks", 0)) < 1:
        problems.append("bulk_writes.min_chunks must be >= 1 when required")
    if isinstance(bulk, dict) and bulk.get("required") and not any(str(k).startswith("bulk_") for k in kinds.values()):
        problems.append("bulk_writes.required but no bulk_* transaction")
    if not manifest.get("scale_scenarios"):
        problems.append(f"{MANIFEST}: scale_scenarios: one entry per applicable failure class of scale_factor_contract.md")
    try:
        problems += _check_write_mix(read_yaml(suite / WRITE_MIX), {str(k) for k in kinds.values()})
    except ValueError as exc:
        problems.append(str(exc))
    return problems


def validate_report(report: dict[str, Any], suite: Path, dependent_tables: list[str], aux: bool) -> list[str]:
    """What the run of the suite on plain SQL (the harness's validate mode) shows is wrong."""
    problems = [str(e) for e in report.get("errors") or []]
    manifest = read_yaml(suite / MANIFEST)
    rows = report.get("tuple_rows") or []
    tuples = read_params(suite / PARAMS)
    edges = manifest.get("edge_coverage") or {}
    for i in edges.get("empty_result") if isinstance(edges.get("empty_result"), list) else []:
        if 1 <= i <= len(rows) and rows[i - 1] != 0:
            problems.append(f"edge_coverage.empty_result: tuple {i} returns {rows[i - 1]} rows, not none")
    for i in edges.get("null_param") if isinstance(edges.get("null_param"), list) else []:
        if 1 <= i <= len(tuples) and not re.search(r"\bnull\b", tuples[i - 1], re.I):
            problems.append(f"edge_coverage.null_param: tuple {i} contains no NULL")
    if not aux:
        return problems
    changes = {e["txn"]: int(e["changes_answers_of"]) for e in report.get("txn_effects") or []}
    for table in dependent_tables:
        touching = [n for n, sql in txn_files(suite).items() if any(t == table.lower() for _, t in statements(sql))]
        if touching and not any(changes.get(n, 0) > 0 for n in touching):
            problems.append(f"no transaction on {table} changes the answer of any tuple (checked: {', '.join(touching)}): "
                            "a stale maintenance of this table could not be caught; target rows that are in answers")
    return problems


# ---------------------------------------------------------------- the run of a tier


def run_environment(suite: Path, *, tier: str, cfg: SuiteConfig, seed: int | None, params_file: Path) -> dict[str, str]:
    """The ACCEL_SUITE_* environment (host_contract.md, "Test suite") for one run of `tier` (iterate, validate, or full, which the quick tier also is: the same
    tuples and transactions on the subset dataset) with `seed`;
    writes the run's random tuples to `params_file`."""
    seed = seed or fresh_seed()
    random_tuples = {"validate": cfg.validation_random_tuples, "iterate": cfg.iterate_random_tuples}.get(tier, cfg.acceptance_random_tuples)
    drawn = draw_tuples(read_yaml(suite / DOMAIN), random_tuples, seed) if (suite / DOMAIN).is_file() else []
    params_file.parent.mkdir(parents=True, exist_ok=True)
    params_file.write_text("\n".join(drawn) + ("\n" if drawn else ""), encoding="utf-8")
    env = {"ACCEL_SUITE_DIR": str(suite), "ACCEL_SUITE_PARAMS_FILE": str(params_file), "ACCEL_SUITE_SEED": str(seed),
           "ACCEL_SUITE_EVERY": str(cfg.iterate_every if tier == "iterate" else cfg.differential_every)}
    if tier == "validate":
        env["ACCEL_SUITE_MODE"] = "validate"
    return env


# ---------------------------------------------------------------- freezing


def content_hash(suite: Path) -> str:
    digest = hashlib.sha256()
    for f in sorted(p for p in suite.rglob("*") if p.is_file() and "__pycache__" not in p.parts):
        digest.update(f.relative_to(suite).as_posix().encode() + b"\0" + f.read_bytes() + b"\0")
    return digest.hexdigest()


def freeze(art: Path, suite: Path, *, query: int, reason: str = "test_suite stage") -> dict[str, Any]:
    record = {"query": query, "hash": content_hash(suite), "time": time.strftime("%Y-%m-%dT%H:%M:%S"), "reason": reason}
    (art / FROZEN_RECORD).write_text(json.dumps(record, indent=2), encoding="utf-8")
    return record


def default_binding(template_sql: str, bound_sql: str) -> list[str] | None:
    """The values the bound reference SQL has where the query template has `$1..$n`, as SQL (one suite tuple), or
    None when the two do not line up. Both are canonicalised first, which drops comments and spacing."""
    try:
        template, bound = (sqlglot.parse_one(sql).sql(comments=False) for sql in (template_sql, bound_sql))
    except sqlglot.errors.ParseError:
        return None
    pieces = re.split(r"\$(\d+)", template)
    if len(pieces) == 1:
        return []
    groups = [f"(?P<v{i}>.+?)" for i in range(1, len(pieces), 2)]
    match = re.fullmatch("".join(groups[i // 2] if i % 2 else re.escape(p) for i, p in enumerate(pieces)), bound, re.S)
    if not match:
        return None
    values: dict[int, str] = {}
    for i in range(1, len(pieces), 2):
        if values.setdefault(int(pieces[i]), match[f"v{i}"]) != match[f"v{i}"]:
            return None  # one parameter bound to two values
    return [values[n] for n in range(1, max(values) + 1)] if set(values) == set(range(1, max(values) + 1)) else None


def binding_texts(binding: list[str] | None) -> list[str] | None:
    """The default binding's values as plain text (a literal, or a cast of one), for a registration to cast to the
    parameters' types; None when one is an expression."""
    texts = []
    for value in binding or []:
        node = sqlglot.parse_one(value)
        node = node.this if isinstance(node, exp.Cast) else node
        if not isinstance(node, exp.Literal):
            return None
        texts.append(str(node.this))
    return texts if binding else None


def write_probe_suite(directory: Path, binding: list[str]) -> Path:
    """The smallest suite (one tuple at the default binding, no transactions) for checking that a harness reads the
    suite environment before the query's own suite is frozen."""
    shutil.rmtree(directory, ignore_errors=True)
    (directory / TXNS).mkdir(parents=True)
    (directory / PARAMS).write_text(", ".join(binding) + "\n", encoding="utf-8")
    return directory


def frozen_problem(art: Path, suite: Path) -> str:
    """'' when the suite is the one the test_suite stage froze; else what is wrong."""
    try:
        recorded = json.loads((art / FROZEN_RECORD).read_text(encoding="utf-8"))["hash"]
    except (OSError, ValueError, KeyError):
        return f"the suite {suite} was never frozen: run the test_suite stage"
    if content_hash(suite) != recorded:
        return (f"the frozen test suite {suite} was edited (hash differs from {FROZEN_RECORD}). It is only changed by "
                f"re-running the tester on a change request ({CHANGE_REQUEST} in the artifacts), never edited")
    return ""


def pending_change_request(art: Path) -> str:
    path = art / CHANGE_REQUEST
    return path.read_text(encoding="utf-8").strip() if path.is_file() else ""


def archive_change_request(art: Path, outcome: str) -> None:
    """Move the request to test_suite_change_requests/<n>.md with the tester's outcome."""
    path = art / CHANGE_REQUEST
    if not path.is_file():  # the tester already filed it itself
        return
    log = art / "test_suite_change_requests"
    log.mkdir(exist_ok=True)
    n = len(list(log.glob("*.md"))) + 1
    (log / f"{n:02d}.md").write_text(f"{path.read_text(encoding='utf-8').strip()}\n\n## Outcome\n\n{outcome}\n", encoding="utf-8")
    path.unlink()


MUTATION_KINDS_DEFAULT = ("insert", "update", "delete")

BASE_PATH_DECOMPOSITION_FIELDS = (
    "dominant_scans",
    "dominant_joins",
    "aggregation_grouping_work",
    "sorting_topk_work",
    "parameter_dependent_work",
    "work_repeated_across_invocations",
    "cardinality_scaling",
    "parameter_range_scaling",
    "output_cardinality_scaling",
    "work_remaining_after_aux_replacement",
)

# Typical-binding read work of a stateful design must undercut the base path's by this factor.
MIN_TYPICAL_READ_WORK_RATIO = 100.0

STATE_DESIGN_READ_PATH_FIELDS = (
    "selected_candidate_id",
    "physical_organization",
    "composition_rule",
    "residual_read_algorithm",
    "read_path_bottleneck_risk",
    "read_path_scaling_variable",
    "estimated_state_units_read",
    "parameter_scaling_argument",
)

CANDIDATE_REQUIRED_FIELDS = (
    "id",
    "design_family",
    "viability",
    "selected",
    "state_grain",
    "output_grain",
    "physical_organization",
    "state_payload",
    "composition_rule",
    "operators_eliminated",
    "residual_read_algorithm",
    "estimated_state_units_read",
    "read_path_bottleneck_risk",
    "read_path_scaling_variable",
    "parameter_scaling_argument",
    "asymptotic_read_complexity",
    "practical_read_complexity",
    "point_update_complexity",
    "batch_update_complexity",
    "storage_complexity",
    "mutation_fanout",
    "rebuild_locality",
    "parameter_binding_behavior",
    "history_growth_behavior",
    "best_regime",
)

CANDIDATE_SELECTED_FIELDS = CANDIDATE_REQUIRED_FIELDS

CANDIDATE_ALTERNATE_VIABLE_FIELDS = (
    "id",
    "design_family",
    "viability",
    "selected",
    "state_grain",
    "physical_organization",
    "composition_rule",
)

CANDIDATE_REJECTED_FIELDS = (
    "id",
    "design_family",
    "viability",
    "selected",
    "rejection_reason",
    "state_grain",
    "physical_organization",
)


def _required_candidate_fields(candidate: dict[str, Any]) -> tuple[str, ...]:
    if candidate.get("selected") is True:
        return CANDIDATE_SELECTED_FIELDS
    if candidate.get("viability") == "rejected":
        return CANDIDATE_REJECTED_FIELDS
    return CANDIDATE_ALTERNATE_VIABLE_FIELDS


class AuxAnalysisValidationError(Exception):
    """Raised when aux-state analysis.json fails validation."""


def _mutation_pairs_from_tables(
    dependent_tables: list[dict[str, Any]],
    mutation_kinds: tuple[str, ...] = MUTATION_KINDS_DEFAULT,
) -> set[tuple[str, str]]:
    pairs: set[tuple[str, str]] = set()
    for table in dependent_tables:
        name = table.get("name", "")
        if not name:
            continue
        for mutation in mutation_kinds:
            pairs.add((name, mutation))
    return pairs


def _normalize_mutation_kind(raw: Any) -> str:
    mutation = str(raw or "").strip().lower()
    if mutation.startswith("insert"):
        return "insert"
    if mutation.startswith("update"):
        return "update"
    if mutation.startswith("delete"):
        return "delete"
    return mutation


def repair_aux_analysis(analysis: dict[str, Any]) -> tuple[dict[str, Any], list[str]]:
    """Apply safe, deterministic fixes for common agent omissions before validation."""
    repaired = copy.deepcopy(analysis)
    repairs: list[str] = []

    search = repaired.get("state_candidate_search")
    if isinstance(search, dict):
        candidates = [c for c in (search.get("candidates") or []) if isinstance(c, dict)]
        viable = [c for c in candidates if c.get("viability") == "viable"]
        explanation = str(search.get("fewer_than_three_explanation") or "").strip()
        if 0 < len(viable) < 3 and not explanation:
            viable_ids = ", ".join(str(c.get("id", "?")) for c in viable)
            rejected_ids = ", ".join(
                str(c.get("id", "?")) for c in candidates if c.get("viability") == "rejected"
            )
            tail = f" Rejected: {rejected_ids}." if rejected_ids else ""
            search["fewer_than_three_explanation"] = (
                f"Only {len(viable)} materially different viable design(s) identified: {viable_ids}.{tail}"
            )
            repairs.append("filled state_candidate_search.fewer_than_three_explanation")

    facts = repaired.get("correctness_facts")
    if isinstance(facts, dict):
        coverage = facts.get("mutation_coverage")
        if isinstance(coverage, list):
            for entry in coverage:
                if not isinstance(entry, dict):
                    continue
                normalized = _normalize_mutation_kind(entry.get("mutation"))
                if normalized in MUTATION_KINDS_DEFAULT and normalized != entry.get("mutation"):
                    entry["mutation"] = normalized
                    repairs.append(f"normalized mutation_coverage mutation for {entry.get('table')}")

    return repaired, repairs


def _validate_mutation_coverage(
    dependent_tables: list[dict[str, Any]],
    mutation_coverage: list[dict[str, Any]],
) -> None:
    required = _mutation_pairs_from_tables(dependent_tables)
    covered: set[tuple[str, str]] = set()
    for entry in mutation_coverage:
        table = str(entry.get("table", "")).strip()
        mutation = _normalize_mutation_kind(entry.get("mutation"))
        behavior = entry.get("behavior", "")
        if not table or not mutation or not behavior:
            raise AuxAnalysisValidationError("mutation_coverage entries require table, mutation, behavior")
        pair = (table, mutation)
        if pair in covered:
            raise AuxAnalysisValidationError(f"duplicate mutation_coverage entry: {pair}")
        covered.add(pair)
    missing = required - covered
    if missing:
        raise AuxAnalysisValidationError(f"mutation_coverage missing pairs: {sorted(missing)}")


def _validate_estimated_state_units_read(value: Any, label: str) -> None:
    if not isinstance(value, dict):
        raise AuxAnalysisValidationError(f"{label}.estimated_state_units_read must be an object")
    for key in ("typical", "worst_case"):
        if not str(value.get(key, "")).strip():
            raise AuxAnalysisValidationError(f"{label}.estimated_state_units_read.{key} is required")


def _positive_number(value: Any) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value <= 0:
        return None
    return float(value)


def _validate_read_work_ratio(state_design: dict[str, Any]) -> None:
    """The design's per-read work must be far below the base path's, or the miss must be explicit."""
    ratio = state_design.get("read_work_ratio")
    where = "correctness_facts.state_design.read_work_ratio"
    if not isinstance(ratio, dict):
        raise AuxAnalysisValidationError(
            f"{where} is required: base-path work / design work per read, typical and worst "
            "(numbers; `basis` names the measured figures behind them)"
        )
    if not str(ratio.get("basis", "")).strip():
        raise AuxAnalysisValidationError(f"{where}.basis is required (the measured base work and design work)")
    typical = _positive_number(ratio.get("typical"))
    worst = _positive_number(ratio.get("worst"))
    if typical is None or worst is None:
        raise AuxAnalysisValidationError(f"{where}.typical and {where}.worst must be positive numbers")
    if typical >= MIN_TYPICAL_READ_WORK_RATIO:
        return
    unmet = str(ratio.get("unmet_justification", "")).strip()
    families = [f for f in ratio.get("families_evaluated") or []
                if isinstance(f, dict) and str(f.get("family", "")).strip()
                and _positive_number(f.get("typical_ratio")) is not None]
    if len(unmet) < 40 or len(families) < 3:
        raise AuxAnalysisValidationError(
            f"design target unmet: {where}.typical is {typical:g}, below the required "
            f"{MIN_TYPICAL_READ_WORK_RATIO:g}x. Revise the design in this stage (finer partitioning by the "
            "predicate's columns, pre-aggregation at a coarser grain with output-sensitive refinement, a "
            "data-derived bound that tightens with the parameter) or select a candidate that meets it; if no "
            "candidate can, set unmet_justification (at least 40 characters) and families_evaluated (at least "
            "three families from the prompt's list, each with family, a numeric typical_ratio and why_short)"
        )


STATE_STORAGE_TEXT_FIELDS = ("component", "primitive", "key", "value", "read_access", "write_access", "cow_unit")
STATE_STORAGE_NUMBER_FIELDS = ("entries", "bytes_per_entry", "target_bytes")


def _validate_state_storage(analysis: dict[str, Any]) -> None:
    """Each component of the selected state names the COW primitive that holds it, how it is accessed, and its size."""
    where = "correctness_facts.state_design.state_storage"
    storage = analysis["correctness_facts"]["state_design"].get("state_storage")
    if not isinstance(storage, list) or not storage:
        raise AuxAnalysisValidationError(
            f"{where} is required: one entry per state component with {', '.join(STATE_STORAGE_TEXT_FIELDS)} and "
            f"numeric {', '.join(STATE_STORAGE_NUMBER_FIELDS)}"
        )
    primitives = {str(p.get("name", "")).strip() for key in ("primitives_used", "primitives_proposed")
                  for p in analysis.get(key) or [] if isinstance(p, dict)}
    for index, entry in enumerate(storage):
        label = f"{where}[{index}]"
        if not isinstance(entry, dict):
            raise AuxAnalysisValidationError(f"{label} must be an object")
        for key in STATE_STORAGE_TEXT_FIELDS:
            if not str(entry.get(key) or "").strip():
                raise AuxAnalysisValidationError(f"{label}.{key} is required")
        for key in STATE_STORAGE_NUMBER_FIELDS:
            if not isinstance(entry.get(key), (int, float)) or isinstance(entry.get(key), bool) or entry[key] < 0:
                raise AuxAnalysisValidationError(f"{label}.{key} must be a non-negative number")
        if str(entry["primitive"]).strip() not in primitives:
            raise AuxAnalysisValidationError(
                f"{label}.primitive {entry['primitive']!r} is not listed in primitives_used or primitives_proposed"
            )
    holders = {str(entry["primitive"]).strip() for entry in storage}
    if len(storage) >= 3 and len(holders) == 1:
        raise AuxAnalysisValidationError(
            f"every {where} component is held by {holders.pop()!r}: name the narrowest generic primitive for each "
            "component (a directory or reverse map is a COW map, run pages are a paged COW array), reusing or "
            "proposing those instead of one primitive that does everything"
        )


def _validate_base_path_cost_decomposition(decomposition: Any) -> None:
    if not isinstance(decomposition, dict):
        raise AuxAnalysisValidationError(
            "performance_hypotheses.base_path_cost_decomposition is required when fitness_assessment.recommended is true"
        )
    for key in BASE_PATH_DECOMPOSITION_FIELDS:
        if key not in decomposition:
            raise AuxAnalysisValidationError(
                f"performance_hypotheses.base_path_cost_decomposition.{key} is required"
            )
    for list_key in ("dominant_scans", "dominant_joins", "work_repeated_across_invocations"):
        if not isinstance(decomposition.get(list_key), list):
            raise AuxAnalysisValidationError(
                f"performance_hypotheses.base_path_cost_decomposition.{list_key} must be an array"
            )
    for list_key in ("work_remaining_after_aux_replacement",):
        remaining = decomposition.get(list_key)
        if not isinstance(remaining, list) or not remaining:
            raise AuxAnalysisValidationError(
                f"performance_hypotheses.base_path_cost_decomposition.{list_key} must be a non-empty array"
            )


def _candidate_material_signature(candidate: dict[str, Any]) -> tuple[str, str, str]:
    return (
        str(candidate.get("design_family", "")).strip().lower(),
        str(candidate.get("physical_organization", "")).strip().lower(),
        str(candidate.get("composition_rule", "")).strip().lower(),
    )


def _validate_state_candidate_search(analysis: dict[str, Any]) -> None:
    search = analysis.get("state_candidate_search")
    if not isinstance(search, dict):
        raise AuxAnalysisValidationError("state_candidate_search is required when fitness_assessment.recommended is true")

    candidates = search.get("candidates")
    if not isinstance(candidates, list) or not candidates:
        raise AuxAnalysisValidationError("state_candidate_search.candidates must be a non-empty array")

    explanation = str(search.get("fewer_than_three_explanation") or "").strip()
    viable = [c for c in candidates if isinstance(c, dict) and c.get("viability") == "viable"]
    if len(viable) < 3 and not explanation:
        raise AuxAnalysisValidationError(
            "state_candidate_search requires at least 3 viable candidates or fewer_than_three_explanation"
        )

    signatures: set[tuple[str, str, str]] = set()
    selected_ids: list[str] = []
    for index, candidate in enumerate(candidates):
        if not isinstance(candidate, dict):
            raise AuxAnalysisValidationError(f"state_candidate_search.candidates[{index}] must be an object")
        label = f"state_candidate_search.candidates[{index}]"
        required_fields = _required_candidate_fields(candidate)
        for key in required_fields:
            if key not in candidate:
                raise AuxAnalysisValidationError(f"{label}.{key} is required")
        if candidate.get("viability") == "rejected" and not str(candidate.get("rejection_reason") or "").strip():
            raise AuxAnalysisValidationError(f"{label}.rejection_reason is required when viability is rejected")
        if candidate.get("selected") is True:
            _validate_estimated_state_units_read(candidate.get("estimated_state_units_read"), label)
        signatures.add(_candidate_material_signature(candidate))
        if candidate.get("selected") is True:
            selected_ids.append(str(candidate.get("id", "")))

    if len(signatures) < min(3, len(candidates)) and not explanation:
        raise AuxAnalysisValidationError(
            "state_candidate_search.candidates must differ materially in design_family, "
            "physical_organization, or composition_rule"
        )

    if len(selected_ids) != 1:
        raise AuxAnalysisValidationError(
            "state_candidate_search.candidates must mark exactly one candidate selected=true"
        )

    state_design = analysis["correctness_facts"]["state_design"]
    selected_id = str(state_design.get("selected_candidate_id", ""))
    if selected_id != selected_ids[0]:
        raise AuxAnalysisValidationError(
            "correctness_facts.state_design.selected_candidate_id must match the selected candidate id"
        )


def _validate_state_design_read_path(state_design: dict[str, Any]) -> None:
    for key in STATE_DESIGN_READ_PATH_FIELDS:
        if key not in state_design:
            raise AuxAnalysisValidationError(f"correctness_facts.state_design.{key} is required")
    if state_design.get("read_path_bottleneck_risk") not in ("low", "medium", "high"):
        raise AuxAnalysisValidationError("correctness_facts.state_design.read_path_bottleneck_risk is invalid")
    _validate_estimated_state_units_read(
        state_design.get("estimated_state_units_read"),
        "correctness_facts.state_design",
    )


def _validate_required_fields(analysis: dict[str, Any]) -> None:
    for key in (
        "accelerator_id",
        "aux_function_name",
        "fitness_assessment",
        "correctness_facts",
        "files_to_create",
    ):
        if key not in analysis:
            raise AuxAnalysisValidationError(f"missing required field: {key}")

    fitness = analysis["fitness_assessment"]
    if "recommended" not in fitness:
        raise AuxAnalysisValidationError("fitness_assessment.recommended is required")

    facts = analysis["correctness_facts"]
    for key in (
        "dependent_tables",
        "partitioning",
        "state_design",
        "state_payload",
        "mutation_coverage",
        "fallback_conditions",
        "maintenance_actions_used",
    ):
        if key not in facts:
            raise AuxAnalysisValidationError(f"missing correctness_facts.{key}")

    state_design = facts["state_design"]
    if state_design.get("is_full_result_cache") is True:
        raise AuxAnalysisValidationError("state_design.is_full_result_cache must be false")

    alternatives = state_design.get("alternatives_considered", [])
    if len(alternatives) < 2:
        raise AuxAnalysisValidationError("state_design.alternatives_considered requires at least 2 entries")

    if not analysis.get("files_to_create"):
        raise AuxAnalysisValidationError("files_to_create must be non-empty")


def validate_aux_analysis(
    analysis: dict[str, Any],
    *,
    allow_poor_fitness: bool = False,
) -> None:
    """Validate aux-state analysis.json structure and business rules."""
    _validate_required_fields(analysis)

    fitness = analysis["fitness_assessment"]
    recommended = bool(fitness.get("recommended"))
    if not recommended and not allow_poor_fitness:
        raise AuxAnalysisValidationError(
            "fitness_assessment.recommended is false; set allow_poor_fitness or fix analysis"
        )

    facts = analysis["correctness_facts"]
    state_design = facts["state_design"]
    if state_design.get("negligible_speedup_risk") == "high" and not allow_poor_fitness:
        raise AuxAnalysisValidationError(
            "state_design.negligible_speedup_risk is high; set allow_poor_fitness or redesign"
        )

    _validate_state_design_read_path(state_design)
    _validate_mutation_coverage(facts["dependent_tables"], facts["mutation_coverage"])

    if recommended:
        performance = analysis.get("performance_hypotheses")
        if not isinstance(performance, dict):
            raise AuxAnalysisValidationError(
                "performance_hypotheses is required when fitness_assessment.recommended is true"
            )
        _validate_base_path_cost_decomposition(performance.get("base_path_cost_decomposition"))
        _validate_state_candidate_search(analysis)
        _validate_read_work_ratio(state_design)
        _validate_state_storage(analysis)


def validate_aux_analysis_file(
    path: Path,
    *,
    allow_poor_fitness: bool = False,
) -> dict[str, Any]:
    if not path.is_file():
        raise AuxAnalysisValidationError(f"analysis file not found: {path}")
    analysis = json.loads(path.read_text(encoding="utf-8"))
    validate_aux_analysis(analysis, allow_poor_fitness=allow_poor_fitness)
    log = path.parent / "data_queries.jsonl"
    if log.is_file() and analysis["fitness_assessment"].get("recommended"):
        _validate_units_probes(analysis["state_candidate_search"]["candidates"], log)
    return analysis


def _squash(sql: str) -> str:
    return " ".join(sql.split()).rstrip(";").lower()


MIN_SIMULATED_CANDIDATES = 3


def _logged_queries(log: Path) -> set[str]:
    ran = set()
    for line in log.read_text(encoding="utf-8").splitlines():
        try:
            call = json.loads(line)
        except json.JSONDecodeError:
            continue
        if call.get("tool") == "query_data" and not call.get("error"):
            ran.add(_squash(str((call.get("args") or {}).get("sql", ""))))
    return ran


def _validate_units_probes(candidates: list[dict[str, Any]], log: Path) -> None:
    """Every candidate's units read are simulated on the data by a logged query_data call, not estimated on paper."""
    ran = _logged_queries(log)
    probes = set()
    for index, candidate in enumerate(candidates):
        where = f"state_candidate_search.candidates[{index}]"
        probe = candidate.get("units_probe")
        if not isinstance(probe, str) or not probe.strip():
            raise AuxAnalysisValidationError(
                f"{where}.units_probe is required: the SQL of the query_data call that simulates this candidate's "
                "read on the data (counts the entries it would visit at representative and extreme bindings)"
            )
        if _squash(probe) not in ran:
            raise AuxAnalysisValidationError(
                f"{where}.units_probe does not match a successful query_data call in {log.name}; "
                "run the simulation and copy its SQL"
            )
        if not str(candidate.get("measured_units") or "").strip():
            raise AuxAnalysisValidationError(f"{where}.measured_units is required: what units_probe returned")
        probes.add(_squash(probe))
    if len(probes) < MIN_SIMULATED_CANDIDATES:
        raise AuxAnalysisValidationError(
            f"at least {MIN_SIMULATED_CANDIDATES} candidates need their own simulated read (distinct units_probe); "
            f"found {len(probes)}"
        )


STORE_DIAGNOSTIC_FIELDS: tuple[str, ...] = (
    "aux_state_bytes",
    "valid_partitions",
    "total_slots",
    "validity_slots_scanned_median",
    "validity_scan_us_median",
    "partitions_read_per_query_median",
    "state_bytes_read_median",
    "store_execute_calls",
    "accel_hit_median_us",
    "accel_hit_rate",
)

_INT_FIELDS = {
    "aux_state_bytes",
    "valid_partitions",
    "total_slots",
    "state_bytes_read_median",
    "store_execute_calls",
}
_FLOAT_FIELDS = {
    "validity_slots_scanned_median",
    "validity_scan_us_median",
    "partitions_read_per_query_median",
    "accel_hit_median_us",
    "accel_hit_rate",
}


def coerce_diagnostic_field(name: str, value: Any) -> Any:
    if value is None or value == "":
        return None
    if name in _INT_FIELDS:
        try:
            return int(float(value))
        except (TypeError, ValueError):
            return None
    if name in _FLOAT_FIELDS:
        try:
            return float(value)
        except (TypeError, ValueError):
            return None
    return value


# What a bench reports so measurement.check_measurement can tell a hit from a fallback.
MEASUREMENT_COUNTERS = ("num_queries", "accel_fallback_reads", "state_builds")
MEASUREMENT_TIMES = ("fallback_median_us",)


def _normalize_sweep_row_keys(row: dict[str, str]) -> dict[str, str]:
    """Accept legacy benchmark CSV headers (e.g. write_freq_pct)."""
    normalized = dict(row)
    aliases = {
        "write_freq_pct": "write_frequency_pct",
        "policy": "write_policy",
        "queries": "num_queries",
        "writes": "writes_executed",
        "query_speedup": "query_only_speedup",
        "cold_start_speedup": "cold_start_workload_speedup",
        "blended_speedup": "blended_query_speedup",
        "cold_start_workload_us": "cold_start_accel_workload_us",
    }
    for legacy, canonical in aliases.items():
        if canonical not in normalized and legacy in normalized:
            normalized[canonical] = normalized[legacy]
    return normalized


def _is_data_row(row: dict[str, str]) -> bool:
    row = _normalize_sweep_row_keys(row)
    if not row:
        return False
    first = next(iter(row.keys()), "")
    if first.startswith("#"):
        return False
    if "write_frequency_pct" not in row or "workload_speedup" not in row:
        return False
    try:
        float(row["workload_speedup"])
        int(float(row["write_frequency_pct"]))
    except (TypeError, ValueError):
        return False
    return True


def _parse_sweep_reader(reader: csv.DictReader) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for row in reader:
        row = _normalize_sweep_row_keys(row)
        if not _is_data_row(row):
            continue
        parsed: dict[str, Any] = {}
        for key, value in row.items():
            if value is None or value == "":
                continue
            if key in {
                "write_frequency_pct",
                "num_queries",
                "writes_scheduled",
                "writes_executed",
                "rows_touched_per_write",
                "aux_state_bytes",
                "valid_partitions",
                "total_slots",
                "state_bytes_read_median",
                "store_execute_calls",
                *MEASUREMENT_COUNTERS,
            }:
                try:
                    parsed[key] = int(float(value))
                except ValueError:
                    parsed[key] = value
            elif key in STORE_DIAGNOSTIC_FIELDS or key.endswith("_rate") or key in {
                "workload_speedup",
                "cold_start_workload_speedup",
                "query_only_speedup",
                "blended_query_speedup",
                "oracle_regret_pct",
                "validity_slots_scanned_median",
                "validity_scan_us_median",
                "partitions_read_per_query_median",
                "accel_hit_median_us",
            }:
                coerced = coerce_diagnostic_field(key, value)
                if coerced is None or isinstance(coerced, str):
                    try:
                        parsed[key] = float(value)
                    except ValueError:
                        parsed[key] = value
                else:
                    parsed[key] = coerced
            else:
                parsed[key] = value
        rows.append(parsed)
    return rows


def parse_sweep_csv(path: Path) -> list[dict[str, Any]]:
    if not path.is_file():
        return []
    with path.open(encoding="utf-8", newline="") as handle:
        reader = csv.DictReader(line for line in handle if not line.startswith("#"))
        return _parse_sweep_reader(reader)


def parse_aux_benchmark_output(text: str) -> list[dict[str, Any]]:
    """Parse aux benchmark stdout (single-point or sweep CSV)."""
    reader = csv.DictReader(
        line for line in text.splitlines() if line.strip() and not line.startswith("#")
    )
    return _parse_sweep_reader(reader)


HOST_SCRIPT = "backends/{engine}/host.sh"
INFO_TIMEOUT_S = 30


def host_script(repo_root: Path, engine: str) -> Path:
    return repo_root / HOST_SCRIPT.format(engine=engine)


def instance_tag(worker_id: str, build_dir: Path) -> str:
    """A worker's engine-instance prefix, unique on the host: its id plus a digest of its build dir, so
    workers of different trees (same query, same engine) never share an instance, container or port."""
    digest = hashlib.sha1(str(Path(build_dir).resolve()).encode()).hexdigest()[:6]
    return f"{re.sub(r'[^A-Za-z0-9_.-]', '_', worker_id or 'w0')}-{digest}"


def instance_env(repo_root: Path, engine: str, build_dir: Path) -> dict[str, str]:
    """The caller-set variables of the contract; the environment wins over these defaults."""
    instance = os.environ.get("ACCEL_ENGINE_INSTANCE") or f"{os.environ.get('ACCEL_INSTANCE_TAG') or 'accel'}-{engine}"
    return {
        "ACCEL_ENGINE_INSTANCE": instance,
        "ACCEL_ENGINE_STATE_DIR": os.environ.get("ACCEL_ENGINE_STATE_DIR")
        or str(build_dir / "engine_state" / instance),
        "ACCEL_BUILD_DIR": str(build_dir),
        "ACCEL_CMAKE_ARGS": os.environ.get("ACCEL_CMAKE_ARGS") or cmake_args(engine),
    }


def cmake_args(engine: str) -> str:
    """How `host.sh build` configures the build dir: the root CMake's backend switch
    (cmake/Backend.cmake) and this engine's query sources. Same default as aux_gate_common.sh."""
    return (
        f"-G Ninja -DCMAKE_BUILD_TYPE=Release -DACCELERATOR_CODEGEN_BUILD_{engine.upper()}=ON "
        f"-DACCEL_CODEGEN_ENGINE={engine}"
    )


def run_host_verb(
    repo_root: Path,
    engine: str,
    verb: str,
    *args: str,
    build_dir: Path,
    timeout: float | None = None,
    env: dict[str, str] | None = None,
) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, **instance_env(repo_root, engine, build_dir), **(env or {})}
    return subprocess.run(
        [str(host_script(repo_root, engine)), verb, *args],
        cwd=repo_root, env=env, capture_output=True, text=True, timeout=timeout,
    )


def host_info(repo_root: Path, engine: str, build_dir: Path) -> dict[str, Any] | None:
    """The `info` verb's JSON object, or None when the backend has no usable host.sh yet."""
    if not host_script(repo_root, engine).is_file():
        return None
    try:
        done = run_host_verb(repo_root, engine, "info", build_dir=build_dir, timeout=INFO_TIMEOUT_S)
        info = json.loads(done.stdout)
    except (OSError, subprocess.SubprocessError, ValueError):
        return None
    return info if done.returncode == 0 and isinstance(info, dict) else None


def workload_dataset_dir(workload: dict[str, Any]) -> str:
    return str(workload.get("dataset_dir") or os.environ.get("ACCEL_DATASET_DIR") or "")


def ensure_dataset_loaded(workload: dict[str, Any], repo_root: Path, build_dir: Path, timeout: float = 3600,
                          instance_env: dict[str, str] | None = None) -> str | None:
    """Bring the engine instance (the default one, or the one `instance_env` names) up with the workload's dataset
    loaded (`up` and `load` are idempotent and cheap when done). None on success, else why not; never raises."""
    from accel_pipeline.layout import workload_schema_sql

    engine = str(workload.get("engine_name") or "")
    dataset_dir = workload_dataset_dir(workload)
    if not host_script(repo_root, engine).is_file():
        return f"no host.sh for engine {engine!r}"
    if not dataset_dir:
        return "no dataset directory (workload dataset_dir or ACCEL_DATASET_DIR)"
    schema = repo_root / workload_schema_sql(str(workload.get("dataset_benchmark") or workload.get("benchmark_name") or ""))
    try:
        for verb, args in (("up", ()), ("load", (str(schema),))):
            done = run_host_verb(repo_root, engine, verb, *args, build_dir=build_dir, timeout=timeout,
                                 env={"ACCEL_DATASET_DIR": dataset_dir, **(instance_env or {})})
            if done.returncode not in (0, 77):
                return f"host.sh {verb} failed (exit {done.returncode}): {(done.stderr or done.stdout)[-500:]}"
    except (OSError, subprocess.SubprocessError) as exc:
        return f"host.sh failed: {exc}"
    return None


def inject_host_runtime_into_gate_env(
    gate_env: dict[str, str], workload: dict[str, Any], repo_root: Path, build_dir: Path
) -> None:
    host = dict(workload.get("host_runtime") or {})
    if not host:
        return
    gate_env["ACCEL_HOST_RUNTIME_KIND"] = str(host.get("kind") or "host_cmake")
    for key, value in dict(host.get("env") or {}).items():
        gate_env[str(key)] = str(value)
    engine = str(workload.get("engine_name") or "")
    if engine:
        gate_env.update(instance_env(repo_root, engine, build_dir))


def prepare_host_sdk_payload(
    workload: dict[str, Any], repo_root: Path, build_dir: Path
) -> dict[str, Any] | None:
    """Analysis-time host metadata; None when the workload has no host_runtime."""
    host = dict(workload.get("host_runtime") or {})
    if not host:
        return None
    engine = str(workload.get("engine_name") or "")
    info = host_info(repo_root, engine, build_dir)
    kind = str(host.get("kind") or "host_cmake")
    return {
        "engine_name": engine,
        "kind": kind,
        "found": info is not None,
        "summary": f"{kind}; verbs: {', '.join(info['verbs'])}" if info else f"{kind}; no host.sh yet",
        "host_script": HOST_SCRIPT.format(engine=engine),
        "info": info,
        "connect": host.get("connect"),
    }


def discover_host_sdk_json(workload: dict[str, Any], repo_root: Path, build_dir: Path) -> str:
    """JSON for the ``discover_host_sdk`` agent tool."""
    payload = prepare_host_sdk_payload(workload, repo_root, build_dir)
    if payload is None:
        payload = {
            "engine": str(workload.get("engine_name") or ""),
            "found": False,
            "reason": "no host_runtime in the workload; see the engine profile",
        }
    return json.dumps(payload, indent=2)


def format_host_block(workload: dict[str, Any], repo_root: Path, build_dir: Path) -> str:
    """Markdown for prompts: how to reach the engine (host.sh verbs and the profile's connect fact)."""
    host = dict(workload.get("host_runtime") or {})
    engine = str(workload.get("engine_name") or "")
    info = host_info(repo_root, engine, build_dir)
    lines = ["### Engine host", ""]
    if info:
        lines.append(f"`{HOST_SCRIPT.format(engine=engine)}` verbs: {', '.join(info['verbs'])}.")
    else:
        lines.append(f"`{HOST_SCRIPT.format(engine=engine)}` is not available yet.")
    lines.append(f"host_runtime.kind=`{host.get('kind') or 'host_cmake'}`. Connect: {host.get('connect') or 'see the engine profile'}")
    return "\n".join(lines)
