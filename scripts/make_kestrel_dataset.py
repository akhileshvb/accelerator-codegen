#!/usr/bin/env python3
"""The Kestrel synthetic dataset (workloads/kestrel): one '|'-separated .tbl per table, NULL as an empty field,
deterministic for a seed. With --duckdb-cli it also loads them into a DuckDB database of the schema.

usage: make_kestrel_dataset.py <out dir> [--scale N] [--seed S] [--duckdb-cli PATH --database FILE]"""

from __future__ import annotations

import argparse
import datetime
import random
import subprocess
from pathlib import Path

REGIONS = ("north", "south", "east", "west", "central")
FAMILIES = ("audio", "bakery", "books", "dairy", "garden", "hardware", "kitchen", "outdoor", "pets", "produce",
            "stationery", "toys")
SCHEMA = Path(__file__).resolve().parents[1] / "workloads" / "kestrel" / "schema.sql"


def write(path: Path, rows) -> int:
    count = 0
    with path.open("w", encoding="utf-8") as out:
        for row in rows:
            out.write("|".join("" if v is None else str(v) for v in row) + "|\n")
            count += 1
    return count


def tables(scale: int, rng: random.Random):
    start = datetime.date(2023, 1, 1)
    shops = [f"S{rng.randrange(16**6):06x}{i:04d}" for i in range(300 * scale)]
    skus = [f"{rng.choice(FAMILIES)[:3].upper()}-{i:05d}" for i in range(800 * scale)]
    yield "shop", ((code, rng.choice(REGIONS), start - datetime.timedelta(days=rng.randrange(2000))) for code in shops)
    yield "item", ((sku, FAMILIES[i % len(FAMILIES)], None if rng.random() < 0.1 else round(rng.uniform(0.05, 40.0), 3))
                   for i, sku in enumerate(skus))
    yield "sale", ((f"R{i:08d}", rng.choice(shops), rng.choice(skus), rng.randint(1, 12),
                    f"{rng.randint(50, 50000) / 100:.2f}", start + datetime.timedelta(days=rng.randrange(730)),
                    None if rng.random() < 0.7 else rng.choice(("gift", "returned once", "loyalty", "bulk")))
                   for i in range(400_000 * scale))
    # A table no key links to the others, with small-domain integers (the case a modulo sample empties).
    yield "audit_log", ((i % 50, rng.randint(1, 5), None if rng.random() < 0.2 else f"event {rng.randrange(10**6)}")
                        for i in range(200_000 * scale))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("out", type=Path)
    parser.add_argument("--scale", type=int, default=1)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--duckdb-cli")
    parser.add_argument("--database", type=Path)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)
    for name, rows in tables(args.scale, rng):
        print(f"{name}: {write(args.out / f'{name}.tbl', rows)} rows")
    if args.duckdb_cli and args.database:
        args.database.unlink(missing_ok=True)
        load = SCHEMA.read_text() + "".join(
            f"COPY {t} FROM '{args.out / (t + '.tbl')}' (DELIMITER '|', HEADER false);\n"
            for t in ("shop", "item", "sale", "audit_log"))
        subprocess.run([args.duckdb_cli, str(args.database)], input=load, text=True, check=True)
        print(f"loaded {args.database}")


if __name__ == "__main__":
    main()
