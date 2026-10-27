"""The analysis prompt's pointer to the dataset, and the query-shape tables of the `query_shape` tool."""

from __future__ import annotations

from pathlib import Path
from typing import Any


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


def growth_note(production: dict[str, Any] | None) -> str:
    """How iteration-scale facts carry over to production (the benchmark pack's `production`)."""
    p = production or {}
    to = f"{p['scale']:,}-fold in production" if p.get("scale") else "with the data in production"
    fixed = f" ({', '.join(p['fixed'])} stay fixed)" if p.get("fixed") else ""
    return (f"Row counts grow {to}{fixed}. NDV grows only for keys that grow with the data (ids); rows per "
            "key grow for the others (codes, categories, dates of a fixed range). Value ranges, quantiles, NULL "
            "fractions, join-key match rates and spreads between columns do not grow.")


def render_shape_tables(facts: dict[str, Any], source: str, production: dict[str, Any] | None = None) -> list[str]:
    rows, stats, schema = facts["rows"], facts["stats"], facts["schema"]
    roles = facts["shape"].roles
    lines = [
        f"- Source: {source} (iteration scale). {growth_note(production)}",
        ("- Statistics are exact" if max(rows.values(), default=0) <= facts["sample_rows"] else
         f"- Statistics are exact below {facts['sample_rows']:,} rows per table and sampled above (counts rescaled)")
        + f"; each statement is time-boxed ({facts['timeouts']} timed out or skipped).",
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
{growth}

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


def build_data_section(engine: str, counts: dict[str, int], production: dict[str, Any] | None = None) -> str:
    """The analysis prompt's Data section: the engine holding the dataset, its tables' row counts, and the tools."""
    table = "\n".join(md_table(["table", "rows"], [[t, f"{n:,}"] for t, n in counts.items()])) if counts else ""
    return DATA_SECTION.format(engine=engine or "unset", counts=f"\n{table}" if table else "",
                               growth=growth_note(production))


def catalog_sql(workload: dict[str, Any], repo: Path | None, query: int | None) -> str:
    """The parameterised catalog text ($n placeholders) when the benchmark has one."""
    if repo is None or query is None:
        return ""
    path = repo / "workloads" / str(workload.get("benchmark_name", "")) / f"q{query}.sql"
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def build_data_shape_markdown(sql: str, runner: Any, schema: dict[str, dict[str, str]], source: str,
                              statement_seconds: float, total_seconds: float,
                              production: dict[str, Any] | None = None) -> str:
    """The statistics `sql` (the parameterised query) implies, computed on the engine behind `runner`."""
    from accel_pipeline.data_shape_stats import compute

    facts = compute(sql, runner, schema, statement_seconds, total_seconds)
    return "\n".join(
        ["## Data shape (iteration scale)", "",
         "Facts to bound read work over all parameter bindings and to narrow candidate ranges from column "
         "spreads and correlations.", ""] + render_shape_tables(facts, source, production)
    )
