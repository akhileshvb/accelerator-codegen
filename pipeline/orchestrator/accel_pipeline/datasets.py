"""The run's data: what the agents query about the dataset, and the test cuts made out of it.

Shape statistics. sqlglot finds the columns a query uses (predicate, join, group, order), its join keys, its
parameterised predicates and the column pairs whose difference matters; the statistics are then computed on the
target engine through a `Runner` (host.sh `sql`). The statements are portable SQL (COUNT, COUNT DISTINCT, MIN/MAX,
AVG, window functions for quantiles, `%` for sampling) written in sqlglot's neutral dialect and transpiled to the
engine's dialect (its profile's `data_sql.dialect`). Each statement is time-boxed and tables above SAMPLE_ROWS are
sampled by key modulo (a prefix of the table when it has no integer column), so the work stays bounded on any
dataset. They feed the analysis prompt's Data section and the `query_shape` tool.

Test cuts, made once out of the engine's loaded dataset right after host bootstrap. A cut keeps a sample of the
largest table and, along the query's join keys, every row those rows join to (the parents they reference and the
children that reference them), so the query's answers on it are as non-trivial as on the full data. It holds every
table of the dataset, because a harness may write tables the query does not read (TPC-H's refresh functions need
orders beside lineitem): the ones the query does not join to follow key columns of the same name (`l_orderkey` and
`o_orderkey`, or `customer_id` in both). Two cuts, each a directory of <table>.tbl files (what every host.sh
`load` reads) written from the host's `sql` verb, so any engine works:

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
from dataclasses import dataclass, field
from pathlib import Path
from typing import TYPE_CHECKING, Any, Protocol

import sqlglot
from sqlglot import exp
from sqlglot.optimizer.qualify import qualify
from sqlglot.optimizer.scope import traverse_scope

from accel_pipeline.config import PipelineConfig
from accel_pipeline.engine_host import ensure_dataset_loaded
from accel_pipeline.layout import workload_schema_sql
from accel_pipeline.workload import query_artifacts_dir

if TYPE_CHECKING:
    from accel_pipeline.tools.data_tools import HostSql


SAMPLE_ROWS = 10_000_000
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
FORMAT_VERSION = 6
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
    keys), and a prime stride cannot line up with the blocks a generator numbers its keys in. An integer join key is
    sampled by value; a text one by its rank among the distinct keys (a modulo of another integer column would keep
    whatever few values happen to divide, e.g. none of a handful of dates). A table no key links to any other has no
    neighbour to stay consistent with: its first rows."""
    if rows <= root_rows:
        return f"SELECT * FROM {table}"
    if not join_columns:
        order = f" ORDER BY {', '.join(integers)}" if integers else ""
        return f"SELECT * FROM {table}{order} LIMIT {root_rows}"
    stride = math.ceil(rows / root_rows)
    while any(stride % d == 0 for d in range(2, math.isqrt(stride) + 1)):
        stride += 1
    if column := next((c for c in join_columns if c in integers), None):
        return f"SELECT * FROM {table} WHERE {column} % {stride} = 0"
    key = join_columns[0]
    ranked = f"SELECT {key}, ROW_NUMBER() OVER (ORDER BY {key}) AS key_rank FROM (SELECT DISTINCT {key} FROM {table}) AS keys"
    return f"SELECT * FROM {table} WHERE {key} IN (SELECT {key} FROM ({ranked}) AS ranked WHERE key_rank % {stride} = 0)"


def _write_cut(runner: HostSql, selects: dict[str, str], directory: Path) -> dict[str, int]:
    directory.mkdir(parents=True)
    counts: dict[str, int] = {}
    for table, select in selects.items():
        lines = []
        for row in runner.rows(select, CUT_STATEMENT_SECONDS):
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


def _cut(cfg: PipelineConfig, workload: dict[str, Any], query: int, source: Path, destination: Path) -> dict[str, Any]:
    from accel_pipeline.tools.data_tools import DataToolContext, HostSql  # data_tools imports this module

    runner = HostSql(DataToolContext(cfg, workload, query, query_artifacts_dir(cfg.repo_root, query, workload=workload)))
    runner.require_up()
    schema = read_schema(runner, 60.0, runner.catalog_sql)
    names = {t.lower(): t for t in schema}
    tables = [names[p.stem.lower()] for p in sorted(source.glob("*.tbl")) if p.stem.lower() in names]
    for name in tables + [c for t in tables for c in schema[t]]:
        if not _IDENTIFIER.match(name):
            raise ValueError(f"{name!r} is not a plain identifier")
    rows = {t: int(runner.rows(f"SELECT COUNT(*) FROM {t}", CUT_STATEMENT_SECONDS)[0][0] or 0) for t in tables}
    joins = analyze_query(catalog_sql(workload, cfg.repo_root, query), schema, runner.dialect).joins + key_links(schema)
    integers = {t: [c for c, dtype in schema[t].items() if is_integer(dtype)] for t in tables}
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
                manifest = _cut(cfg, workload, query, source, destination)
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
