"""Facts about the iteration dataset that bound a query's read work, derived from the query SQL.

sqlglot finds the columns a query uses (predicate, join, group, order), its join keys, its
parameterised predicates and the column pairs whose difference matters; the statistics are then
computed on the iteration dataset through the scratch SQL connection.  Each statement is time-boxed
and tables above SAMPLE_ROWS are sampled, so rendering stays bounded on any dataset.
"""

from __future__ import annotations

import re
import threading
import time
from dataclasses import dataclass, field
from typing import Any

import sqlglot
from sqlglot import exp
from sqlglot.optimizer.qualify import qualify
from sqlglot.optimizer.scope import traverse_scope

from accel_pipeline.workload_facts import SCRATCH_DIALECT, SCRATCH_ERRORS, scratch_connection

SAMPLE_ROWS = 10_000_000
STATEMENT_SECONDS = 10.0
TOTAL_SECONDS = 25.0
MAX_SPREAD_PAIRS = 6
QUANTILES = [0.01, 0.5, 0.99]

_RANGE = {exp.LT: "<", exp.LTE: "<=", exp.GT: ">", exp.GTE: ">="}
_FLIP = {"<": ">", "<=": ">=", ">": "<", ">=": "<="}
_PARAMS = (exp.Placeholder, exp.Parameter)
_AGGS = (exp.Sum, exp.Avg, exp.Count, exp.Min, exp.Max)


def type_family(dtype: str) -> str:
    base = re.sub(r"\(.*", "", dtype).upper()
    if base in {"DATE", "TIMESTAMP", "TIMESTAMP_S", "TIMESTAMP_MS", "TIMESTAMP_NS", "TIMESTAMPTZ", "TIMESTAMP WITH TIME ZONE"}:
        return "date"
    if base in {"TINYINT", "SMALLINT", "INTEGER", "BIGINT", "HUGEINT", "UTINYINT", "USMALLINT", "UINTEGER", "UBIGINT",
                "UHUGEINT", "DECIMAL", "NUMERIC", "DOUBLE", "FLOAT", "REAL"}:
        return "number"
    return "other"


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


def analyze_query(sql: str, schema: dict[str, dict[str, str]]) -> QueryShape:
    tree = sqlglot.parse_one(sql.strip().rstrip(";"), read=SCRATCH_DIALECT)
    qualified = qualify(tree, schema=schema, dialect=SCRATCH_DIALECT, validate_qualify_columns=False)
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
                names = [f"${p.name}" for p in params]
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

    text = conj.sql(dialect=SCRATCH_DIALECT)
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
        return ParamPredicate(text, names, cols[0][0], cols[0][1], "agg" + op, group, subject.sql(dialect=SCRATCH_DIALECT))
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
    """Runs time-boxed statements on a scratch connection within a total time budget."""

    def __init__(self, con: Any) -> None:
        self.con = con
        self.deadline = time.monotonic() + TOTAL_SECONDS
        self.timeouts = 0

    def run(self, sql: str, params: list | None = None) -> list[tuple] | None:
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            self.timeouts += 1
            return None
        timer = threading.Timer(min(STATEMENT_SECONDS, remaining), self.con.interrupt)
        timer.start()
        try:
            return self.con.execute(sql, params or []).fetchall()
        except SCRATCH_ERRORS:
            self.timeouts += 1
            return None
        finally:
            timer.cancel()


def _q(name: str) -> str:
    return '"' + name.replace('"', '""') + '"'


def _source(probe: Probe, table: str, rows: int) -> str:
    if rows > SAMPLE_ROWS:
        return f"(SELECT * FROM {_q(table)} USING SAMPLE reservoir({SAMPLE_ROWS} ROWS) REPEATABLE (1))"
    return _q(table)


def column_stats(probe, table, columns, types, rows):
    """{column: dict(nulls, ndv, min, max, quantiles)} in one pass over `table` (sampled if large)."""
    parts = []
    for i, c in enumerate(columns):
        parts += [f"count({_q(c)})", f"approx_count_distinct({_q(c)})", f"min({_q(c)})", f"max({_q(c)})"]
        parts.append(
            f"quantile_disc({_q(c)}, {QUANTILES})" if type_family(types[c]) != "other" else "NULL"
        )
    res = probe.run(f"SELECT count(*), {', '.join(parts)} FROM {_source(probe, table, rows)}")
    if not res:
        return {}
    row, out = res[0], {}
    for i, c in enumerate(columns):
        nn, ndv, lo, hi, qs = row[1 + 5 * i: 6 + 5 * i]
        out[c] = {"null_frac": 1 - nn / row[0] if row[0] else 0.0, "ndv": ndv, "min": lo, "max": hi, "q": qs}
    return out


def fanout(probe, table, column, other, other_col, rows):
    src = _source(probe, table, rows)
    res = probe.run(
        f"SELECT count(*), avg(c), quantile_disc(c, 0.99), max(c) FROM (SELECT count(*) c FROM {src} "
        f"WHERE {_q(column)} IS NOT NULL GROUP BY {_q(column)})"
    )
    match = probe.run(
        f"SELECT avg(CASE WHEN o.k IS NULL THEN 0 ELSE 1 END) FROM {src} t LEFT JOIN "
        f"(SELECT DISTINCT {_q(other_col)} k FROM {_q(other)}) o ON t.{_q(column)} = o.k"
    )
    return res[0] if res else None, (match[0][0] if match else None)


def param_effect(probe, pred: ParamPredicate, rows: dict[str, int], stats: dict, types: dict) -> dict[str, Any]:
    """Rows (or groups) qualifying at the extremes of the column's range, or by value frequency."""
    t, c, op = pred.table, pred.column, pred.op
    src = _source(probe, t, rows[t])
    st = stats.get(t, {}).get(c, {})
    scale = rows[t] / min(rows[t], SAMPLE_ROWS)
    fam = type_family(types[t][c])
    if op.startswith("agg"):
        keys = ", ".join(_q(g) for g in pred.group) or "NULL"
        inner = f"SELECT {keys}, {pred.agg_sql} s FROM {src} GROUP BY {keys}"
        res = probe.run(f"SELECT count(*), min(s), quantile_disc(s, {QUANTILES}), max(s) FROM ({inner})")
        return {"kind": "agg", "res": res[0] if res else None}
    if op in _FLIP and fam != "other" and st.get("q"):
        points = [("min", st["min"]), ("p1", st["q"][0]), ("p50", st["q"][1]), ("p99", st["q"][2]), ("max", st["max"])]
        counts = []
        for name, v in points:
            r = probe.run(f"SELECT count(*) FROM {src} WHERE {_q(c)} {op} ?", [v])
            counts.append((name, v, None if not r else int(r[0][0] * scale)))
        return {"kind": "range", "points": counts}
    if op in ("=", "in", "<>", "like"):
        if op == "like" and fam == "other":
            expr = f"unnest(string_split({_q(c)}, ' '))"
            what = "word"
        else:
            expr, what = _q(c), "value"
        res = probe.run(
            f"SELECT count(*), max(n), quantile_disc(n, 0.5), min(n), arg_max(v, n), arg_min(v, n) FROM "
            f"(SELECT v, count(*) n FROM (SELECT {expr} v FROM {src}) WHERE v IS NOT NULL GROUP BY v)"
        )
        return {"kind": "freq", "what": what, "res": res[0] if res else None, "scale": scale}
    return {"kind": "none"}


def spread(probe, a, b, shape: QueryShape, rows, types):
    (ta, ca), (tb, cb) = a, b
    diff = (
        f"date_diff('day', b.{_q(cb)}, a.{_q(ca)})" if type_family(types[ta][ca]) == "date"
        else f"CAST(a.{_q(ca)} AS DOUBLE) - CAST(b.{_q(cb)} AS DOUBLE)"
    )
    sa, sb = _source(probe, ta, rows[ta]), _source(probe, tb, rows[tb])
    if ta == tb:
        res = probe.run(
            f"SELECT count(*), min(d), quantile_disc(d, {QUANTILES}), max(d) FROM "
            f"(SELECT {_same_table_diff(ca, cb, types[ta][ca])} d FROM {sa})"
        )
        return res[0] if res else None, rows[ta] > SAMPLE_ROWS
    link = next(((x, y) for x, y in shape.joins if {x[0], y[0]} == {ta, tb}), None)
    if not link:
        return None, False
    ka, kb = (link[0][1], link[1][1]) if link[0][0] == ta else (link[1][1], link[0][1])
    res = probe.run(
        f"SELECT count(*), min(d), quantile_disc(d, {QUANTILES}), max(d) FROM "
        f"(SELECT {diff} d FROM {sa} a JOIN {sb} b ON a.{_q(ka)} = b.{_q(kb)})"
    )
    return res[0] if res else None, max(rows[ta], rows[tb]) > SAMPLE_ROWS


def _same_table_diff(ca, cb, dtype):
    if type_family(dtype) == "date":
        return f"date_diff('day', {_q(cb)}, {_q(ca)})"
    return f"CAST({_q(ca)} AS DOUBLE) - CAST({_q(cb)} AS DOUBLE)"


def compute(sql: str, dataset_db: str) -> dict[str, Any]:
    con = scratch_connection(str(dataset_db), read_only=True)
    try:
        schema: dict[str, dict[str, str]] = {}
        for t, c, d in con.execute(
            "SELECT table_name, column_name, data_type FROM information_schema.columns "
            "WHERE table_schema = 'main' ORDER BY table_name, ordinal_position"
        ).fetchall():
            schema.setdefault(t, {})[c] = d
        shape = analyze_query(sql, schema)
        probe = Probe(con)
        used = {k[0] for k in shape.roles} | {a[0] for a, _ in shape.joins} | {b[0] for _, b in shape.joins}
        tables = sorted(used)
        rows = {t: int(probe.run(f"SELECT count(*) FROM {_q(t)}")[0][0]) for t in tables}
        stats = {
            t: column_stats(probe, t, [c for (tt, c) in shape.roles if tt == t], schema[t], rows[t]) for t in tables
        }
        return {
            "schema": schema, "shape": shape, "rows": rows, "stats": stats,
            "fanout": [
                (a, b, fanout(probe, a[0], a[1], b[0], b[1], rows[a[0]]), fanout(probe, b[0], b[1], a[0], a[1], rows[b[0]]))
                for a, b in shape.joins
            ],
            "effects": [(p, param_effect(probe, p, rows, stats, schema)) for p in shape.params],
            "spreads": [(a, b, why, *spread(probe, a, b, shape, rows, schema)) for a, b, why in shape.spread_pairs],
            "timeouts": probe.timeouts,
        }
    finally:
        con.close()
