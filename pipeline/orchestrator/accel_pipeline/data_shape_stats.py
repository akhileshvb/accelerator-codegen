"""Facts about the iteration dataset that bound a query's read work, derived from the query SQL.

sqlglot finds the columns a query uses (predicate, join, group, order), its join keys, its
parameterised predicates and the column pairs whose difference matters; the statistics are then
computed on the target engine through a `Runner` (host.sh `sql`).  The statements are portable SQL
(COUNT, COUNT DISTINCT, MIN/MAX, AVG, window functions for quantiles, `%` for sampling) written in
sqlglot's neutral dialect and transpiled to the engine's dialect (its profile's `data_sql.dialect`).
Each statement is time-boxed and tables above SAMPLE_ROWS are sampled by key modulo (a prefix of
the table when it has no integer column), so the work stays bounded on any dataset.
"""

from __future__ import annotations

import re
import time
from dataclasses import dataclass, field
from typing import Any, Protocol

import sqlglot
from sqlglot import exp
from sqlglot.optimizer.qualify import qualify
from sqlglot.optimizer.scope import traverse_scope


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
