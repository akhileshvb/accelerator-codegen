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


def build_data_shape_markdown(workload: dict[str, Any], dataset_db: str | Path | None) -> str:
    """Markdown section describing table cardinalities / schema for analysis."""
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
        lines.append("Schema reference: `scripts/postgres_tpch_schema.sql` (column names/types).")
    return "\n".join(lines)
