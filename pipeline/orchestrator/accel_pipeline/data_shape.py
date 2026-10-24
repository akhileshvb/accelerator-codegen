"""Dataset shape snapshots for analysis-stage prompts."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

# TPC-H SF1 nominal cardinalities (dbgen); used when the DB is unavailable.
TPCH_SF1_NOMINAL_ROWS: dict[str, int] = {
    "region": 5,
    "nation": 25,
    "supplier": 10_000,
    "customer": 150_000,
    "part": 200_000,
    "partsupp": 800_000,
    "orders": 1_500_000,
    "lineitem": 6_001_215,
}


def _dependent_table_names(workload: dict[str, Any]) -> list[str]:
    deps = workload.get("dependent_tables") or workload.get("tables") or []
    names: list[str] = []
    for item in deps:
        if isinstance(item, str):
            names.append(item)
        elif isinstance(item, dict) and item.get("name"):
            names.append(str(item["name"]))
    # Preserve order, drop dupes.
    seen: set[str] = set()
    out: list[str] = []
    for n in names:
        if n not in seen:
            seen.add(n)
            out.append(n)
    return out


def _watched_columns(workload: dict[str, Any]) -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    for item in workload.get("dependent_tables") or []:
        if isinstance(item, dict) and item.get("name"):
            cols = item.get("watched_columns") or []
            out[str(item["name"])] = [str(c) for c in cols]
    return out


def _probe_duckdb(dataset_db: Path, tables: list[str], watched: dict[str, list[str]]) -> dict[str, Any] | None:
    if not dataset_db.is_file() or not tables:
        return None
    try:
        import duckdb
    except ImportError:
        return None
    try:
        con = duckdb.connect(str(dataset_db), read_only=True)
    except Exception:
        return None
    try:
        shapes: dict[str, Any] = {}
        for table in tables:
            try:
                row_count = int(con.execute(f"SELECT COUNT(*) FROM {table}").fetchone()[0])
            except Exception:
                continue
            cols = con.execute(f"DESCRIBE {table}").fetchall()
            columns = [{"name": c[0], "type": c[1]} for c in cols]
            ndv: dict[str, int] = {}
            for col in watched.get(table, []):
                try:
                    # approx is fine for design guidance; exact COUNT DISTINCT on SF1 is OK too.
                    ndv[col] = int(
                        con.execute(
                            f"SELECT approx_count_distinct({col}) FROM {table}"
                        ).fetchone()[0]
                    )
                except Exception:
                    try:
                        ndv[col] = int(
                            con.execute(f"SELECT COUNT(DISTINCT {col}) FROM {table}").fetchone()[0]
                        )
                    except Exception:
                        continue
            shapes[table] = {
                "row_count": row_count,
                "columns": columns,
                "approx_distinct": ndv,
            }
        return {
            "source": str(dataset_db),
            "scale_note": "Live counts from iteration DB (typically SF1). Size aux from bind-time row_estimate, not these constants.",
            "tables": shapes,
        }
    finally:
        con.close()


def _fmt(v: Any) -> str:
    if v is None:
        return "-"
    if isinstance(v, float):
        return f"{v:.4g}"
    s = str(v)
    return s if len(s) <= 48 else s[:47] + "…"


def _table(headers: list[str], rows: list[list[Any]]) -> list[str]:
    out = ["| " + " | ".join(headers) + " |", "|" + "---|" * len(headers)]
    out += ["| " + " | ".join(_fmt(c) for c in r) + " |" for r in rows]
    return out + [""]


def _quant(q: Any) -> list[Any]:
    return list(q) if q else [None, None, None]


def render_shape_tables(facts: dict[str, Any], dataset_db: Path) -> list[str]:
    from accel_pipeline.data_shape_stats import SAMPLE_ROWS, type_family

    rows, stats, schema = facts["rows"], facts["stats"], facts["schema"]
    roles = facts["shape"].roles
    lines = [
        f"- Source DB: `{dataset_db}` (iteration scale). Row counts and fan-outs scale roughly linearly with the "
        "scale factor to SF3000; value ranges, quantiles, NULL fractions, join-key matches and cross-column "
        "spreads (differences) do not scale, and NDV scales only for keys that grow with the data.",
        f"- Statistics are exact below {SAMPLE_ROWS:,} rows per table and sampled above (counts rescaled); "
        f"each statement is time-boxed ({facts['timeouts']} timed out or skipped).",
        "",
        "### Tables and columns the query uses",
        "",
    ]
    lines += _table(["table", "rows"], [[t, f"{n:,}"] for t, n in rows.items()])
    col_rows = []
    for (t, c), r in sorted(roles.items()):
        st = stats.get(t, {}).get(c)
        if not st:
            continue
        q1, q50, q99 = _quant(st["q"])
        col_rows.append([f"{t}.{c}", schema[t][c], "".join(sorted(r)), f"{st['null_frac']:.1%}", st["ndv"],
                         st["min"], q1, q50, q99, st["max"]])
    lines += ["Roles: P predicate, J join key, G group by, O order by. NDV is approximate.", ""]
    lines += _table(["column", "type", "roles", "NULL", "~NDV", "min", "p1", "p50", "p99", "max"], col_rows)

    if facts["fanout"]:
        lines += ["### Join keys: rows per key (distinct keys present, avg / p99 / max) and match rate", ""]
        fr = []
        for a, b, fa, fb in facts["fanout"]:
            for key, (res, match), other in ((a, fa, b), (b, fb, a)):
                if res:
                    fr.append([f"{key[0]}.{key[1]} -> {other[0]}.{other[1]}", f"{res[0]:,}", res[1], res[2], res[3],
                               "-" if match is None else f"{match:.1%}"])
        lines += _table(["side (key -> joined to)", "keys", "avg", "p99", "max", "rows with a partner"], fr)

    param_rows = [(p, e) for p, e in facts["effects"]]
    if param_rows:
        lines += ["### Parameterised predicates: rows qualifying across the parameter's range", ""]
        for p, e in param_rows:
            head = f"- `{p.sql}` on `{p.table}.{p.column}` ({rows.get(p.table, 0):,} rows)"
            if e["kind"] == "range":
                pts = ", ".join(f"{n}={_fmt(v)}: {c if c is None else format(c, ',')}" for n, v, c in e["points"])
                lines.append(f"{head}: rows qualifying with the parameter at column {pts}")
            elif e["kind"] == "freq" and e["res"]:
                n, hi, med, lo, hv, lv = e["res"]
                what = e["what"]
                lines.append(
                    f"{head}: {n:,} distinct {what}s; rows per {what} most frequent {hi:,} (`{_fmt(hv)}`), "
                    f"median {med:,}, least frequent {lo:,} (`{_fmt(lv)}`)"
                    + (" (word tokens of the pattern text; `%` alone matches all rows, an absent word none)"
                       if p.op == "like" else "")
                )
            elif e["kind"] == "agg" and e["res"]:
                n, lo, qs, hi = e["res"]
                lines.append(
                    f"{head}: aggregate `{p.agg_sql}` per group over {n:,} groups: min {_fmt(lo)}, p1 {_fmt(qs[0])}, "
                    f"p50 {_fmt(qs[1])}, p99 {_fmt(qs[2])}, max {_fmt(hi)}; groups qualifying = those above the bound"
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
        lines += _table(["difference", "why", "joined rows", "min", "p1", "p50", "p99", "max"], sr)
    return lines


def build_data_shape_markdown(
    workload: dict[str, Any],
    dataset_db: str | Path | None,
    repo: Path | None = None,
    query: int | None = None,
) -> str:
    """Markdown section describing table cardinalities / schema for analysis."""
    sql = _catalog_sql(workload, repo, query)
    db = Path(dataset_db) if dataset_db else None
    if sql and db and db.is_file():
        try:
            from accel_pipeline.data_shape_stats import compute

            facts = compute(sql, str(db))
            return "\n".join(
                ["## Data shape (iteration scale)", "",
                 "Facts to bound read work over all parameter bindings and to narrow candidate ranges from column "
                 "spreads and correlations. Do **not** hardcode iteration-scale capacities into the implementation.",
                 ""] + render_shape_tables(facts, db)
            )
        except Exception:  # fall back to the counts-only snapshot
            pass
    return _build_counts_markdown(workload, dataset_db)


def _catalog_sql(workload: dict[str, Any], repo: Path | None, query: int | None) -> str:
    """The parameterised catalog text ($n placeholders) when the benchmark has one."""
    if repo is None or query is None:
        return ""
    path = repo / "workloads" / str(workload.get("benchmark_name", "tpch")) / f"q{query}.sql"
    return path.read_text(encoding="utf-8") if path.is_file() else ""


def _build_counts_markdown(workload: dict[str, Any], dataset_db: str | Path | None) -> str:
    tables = _dependent_table_names(workload)
    watched = _watched_columns(workload)
    db_path = Path(dataset_db) if dataset_db else None
    live = _probe_duckdb(db_path, tables, watched) if db_path else None

    lines = [
        "## Data shape (iteration scale)",
        "",
        "Use this with the query text to choose state grain and physical layout. "
        "Do **not** hardcode SF1 capacities into the implementation.",
        "",
    ]
    if live and live.get("tables"):
        lines.append(f"- Source DB: `{live['source']}`")
        lines.append(f"- Note: {live['scale_note']}")
        lines.append("")
        lines.append("```json")
        lines.append(json.dumps(live["tables"], indent=2, sort_keys=True))
        lines.append("```")
        return "\n".join(lines)

    # Fallback: nominal TPC-H SF1 (+ schema file hint).
    benchmark = str(workload.get("benchmark_name", "tpch"))
    lines.append(
        f"- Live DB unavailable (`{db_path or 'unset'}`); using nominal {benchmark.upper()} SF1 row counts."
    )
    lines.append("")
    nominal = {
        t: {"row_count_nominal_sf1": TPCH_SF1_NOMINAL_ROWS.get(t), "watched_columns": watched.get(t, [])}
        for t in tables
    }
    lines.append("```json")
    lines.append(json.dumps(nominal, indent=2, sort_keys=True))
    lines.append("```")
    if benchmark == "tpch":
        lines.append("")
        lines.append(
            "Schema reference: TPC-H column names/types from the workload "
            "SQL and peer query modules (no engine-specific schema script)."
        )
    return "\n".join(lines)
