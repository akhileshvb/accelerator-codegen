#!/usr/bin/env python3
"""Split queries.sql into per-query files under queries/."""

from __future__ import annotations

import re
import sys
from pathlib import Path


def parse_queries(content: str) -> dict[str, str]:
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


def main() -> int:
    script_dir = Path(__file__).parent
    queries_path = script_dir / "queries.sql"
    out_dir = script_dir / "queries"
    out_dir.mkdir(parents=True, exist_ok=True)

    content = queries_path.read_text(encoding="utf-8")
    queries = parse_queries(content)
    for name, sql in sorted(queries.items(), key=lambda x: int(x[0][1:])):
        out_path = out_dir / f"{name.lower()}.sql"
        out_path.write_text(sql + "\n", encoding="utf-8")
        print(f"wrote {out_path}")

    print(f"split {len(queries)} queries into {out_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
