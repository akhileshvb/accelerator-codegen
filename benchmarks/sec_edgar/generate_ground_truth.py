#!/usr/bin/env python3
"""Generate ground truth query results for SEC EDGAR benchmark using DuckDB."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from pathlib import Path

import duckdb


def parse_queries(queries_path: Path) -> dict[str, str]:
    content = queries_path.read_text(encoding="utf-8")
    queries: dict[str, str] = {}
    parts = re.split(r"--\s*(Q\d+):\s*([^\n]*)\n", content)
    i = 1
    while i + 2 <= len(parts):
        name = parts[i].strip()
        sql = parts[i + 2].strip()
        sql = re.sub(r"--[^\n]*$", "", sql, flags=re.MULTILINE).strip()
        sql = sql.rstrip(";").strip()
        if sql:
            queries[name] = sql
        i += 3
    return queries


def main() -> None:
    parser = argparse.ArgumentParser(description="Generate SEC EDGAR ground truth results")
    parser.add_argument("--db-path", type=Path, default=None, help="Path to DuckDB database")
    parser.add_argument("--output-dir", type=Path, default=None, help="Output directory for query results")
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    db_path = args.db_path or (script_dir / "duckdb" / "sec_edgar.duckdb")
    queries_path = script_dir / "queries.sql"
    output_dir = args.output_dir or (script_dir / "ground_truth")

    if not db_path.exists():
        print(f"Error: database not found at {db_path}")
        print("Run: python3 benchmarks/sec_edgar/load_data.py")
        sys.exit(1)

    output_dir.mkdir(parents=True, exist_ok=True)

    print(f"Database: {db_path}")
    print(f"Output: {output_dir}")

    con = duckdb.connect(str(db_path), read_only=True)
    queries = parse_queries(queries_path)

    for name, sql in sorted(queries.items(), key=lambda x: int(x[0][1:])):
        print(f"\nRunning {name}...")
        try:
            result = con.execute(sql)
            columns = [desc[0] for desc in result.description]
            rows = result.fetchall()
            output_path = output_dir / f"{name.lower()}.csv"
            with output_path.open("w", newline="", encoding="utf-8") as f:
                writer = csv.writer(f)
                writer.writerow(columns)
                for row in rows:
                    processed = []
                    for val in row:
                        if isinstance(val, float):
                            processed.append(f"{val:.6f}")
                        elif val is None:
                            processed.append("")
                        else:
                            processed.append(str(val))
                    writer.writerow(processed)
            print(f"  {name}: {len(rows)} rows -> {output_path}")
        except Exception as e:
            print(f"  {name}: ERROR - {e}")

    con.close()
    print(f"\nGround truth generated in: {output_dir}")


if __name__ == "__main__":
    main()
