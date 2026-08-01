# SEC-EDGAR schema reference

Database: `sec_edgar_sf3.duckdb` (3 years, 2022–2024). Env var: `ACCEL_SEC_EDGAR_DB`.

## Tables

| Table | Description | Typical row count (SF3) |
|-------|-------------|-------------------------|
| `sub` | One row per SEC filing (header) | ~hundreds of thousands |
| `num` | One row per numeric fact in filings | ~tens of millions |
| `tag` | XBRL tag definitions (deduplicated) | ~thousands |
| `pre` | Presentation lines per filing | ~millions |

## Key columns for accelerators

### `sub` (submissions)
- `adsh` VARCHAR(20) PK — accession number
- `cik` INTEGER — company ID
- `name` VARCHAR(150) — company name
- `sic` INTEGER — industry code
- `fy` INTEGER — fiscal year
- `form` VARCHAR(10) — e.g. 10-K, 10-Q

### `num` (numeric facts)
- `adsh` — FK to sub
- `tag`, `version` — XBRL concept
- `ddate` INTEGER — YYYYMMDD
- `uom` VARCHAR — USD, pure, shares, etc.
- `value` DOUBLE — reported amount

### `tag` (tag definitions)
- `tag`, `version` PK
- `abstract` INTEGER — 1=header, 0=concrete
- `tlabel` VARCHAR — human label

### `pre` (presentation)
- `adsh`, `report`, `line` — composite key
- `stmt` VARCHAR(2) — BS, IS, CF, EQ, CI
- `tag`, `version`, `plabel`
- `rfile` VARCHAR(1)

## Join keys

- `num.adsh = sub.adsh`
- `num.tag = tag.tag AND num.version = tag.version`
- `pre.adsh = sub.adsh`
- `pre.tag = tag.tag AND pre.version = tag.version`
- `num` ↔ `pre`: `adsh`, `tag`, `version`

## Benchmark queries

| Query | Tables | Pattern |
|-------|--------|---------|
| Q1 | pre | Single-table GROUP BY |
| Q2 | num, sub | Join + decorrelated MAX |
| Q3 | num, sub | Join + nested aggregate HAVING |
| Q4 | num, sub, tag, pre | 4-way join + HAVING |
| Q6 | num, sub, pre | 3-way join + GROUP BY |
| Q24 | num, pre | LEFT JOIN anti-join |

Reference SQL: `benchmarks/sec_edgar/queries/q{N}.sql`

## Native scan binding

Use `NativeScanSpec` with exact DuckDB logical types from the loaded database. Bind only columns referenced by the query. Push filters via `NativeFilterSpec` where possible.
