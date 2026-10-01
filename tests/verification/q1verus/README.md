# Stateless Q1 with a Verus-verified kernel

TPC-H Q1 on DuckDB, PostgreSQL and MySQL, answered by a kernel written in Rust
and verified with Verus against a reference specification generated from the
catalog SQL. There is no aux state: every query scans lineitem and aggregates.
The kernel avoids low-level optimizations that it cannot prove. It has no SIMD
intrinsics, no `unsafe`, no unchecked indexing and no type punning. It uses
Verus only.

| Piece | Where | Status |
|---|---|---|
| q_ref (the SQL's meaning) | [`src/verus/q1/src/gen_spec.rs`](../../../src/verus/q1/src/gen_spec.rs), generated from [`queries/q1.sql`](../../../src/verus/sql2verus/queries/q1.sql) by [`sql2verus`](../../../src/verus/sql2verus/src/main.rs), + `bytes_lt`, `sum`, `Rat` in `common/` | **Trusted**: the translator's rules, applied mechanically; `scripts/gen_verus_specs.sh --check` fails if the file is stale |
| Proof vocabulary | [`bridge.rs`](../../../src/verus/q1/src/bridge.rs): the accumulator sums `gsum` and `lemma_gsum_eq` (`gsum` = the generated `group_sum`, in any day encoding) | Verified, not trusted |
| Kernel and proofs | [`src/verus/q1/src/kernel.rs`](../../../src/verus/q1/src/kernel.rs), plus the generated facts (`lemma_query_result_iff`: exactly one answer) | Verified (53 items) |
| C ABI shims | [`src/verus/q1/src/ffi.rs`](../../../src/verus/q1/src/ffi.rs), [`include/accelerator_codegen/verus/q1_verus.h`](../../../include/accelerator_codegen/verus/q1_verus.h) | Trusted glue (pointer → slice) |
| DuckDB entry points | [`src/generated/tpch/q1/duckdb/verus/package.cpp`](../../../src/generated/tpch/q1/duckdb/verus/package.cpp) | Trusted (chunk decoding, output formatting, threads) |
| PostgreSQL entry point | [`src/generated/tpch/q1/postgres/verus/entry.cpp`](../../../src/generated/tpch/q1/postgres/verus/entry.cpp) | Trusted (heap scan, NUMERIC decoding, output formatting, SQL fallback) |
| MySQL entry point | [`src/generated/tpch/q1/mysql/verus/entry.cpp`](../../../src/generated/tpch/q1/mysql/verus/entry.cpp) | Trusted (CLI scan, text decoding, output formatting, SQL fallback) |

## What is proved

A `Q1State` carries, as ghost state, the rows it has consumed. Its invariant
says that slot `k` holds exactly the spec's sums and `COUNT(*)` for group
`k`. The group is `(l_returnflag, l_linestatus)` = (`'A' + k / 26`,
`'A' + k % 26`), so the key is a dense direct-addressed index with no hashing.
All sums are exact `i128`s.

- **`consume(batch)`**: if it returns true, the state's rows are the old rows
  followed by the batch. It works in blocks of at most 2048 rows. Each block is
  summed into `i64` block-local slots, and the touched slots are then folded
  into the `i128` slots. This is the C++ kernel's chunk-local accumulator
  trick, proved.
- **`consume_simple(batch)`**: the same contract, with `i128` slots updated per
  row. It is kept for comparison.
- **`merge(other)`**: `rows := rows ++ other.rows`. Per-thread states are
  combined with it.
- **`finish()`**: the output satisfies the generated
  `query_result(db_at(rows, e), {p1: d}, out)` for every day encoding `e`
  and `$1 = d` with `cutoff + e = 10561 - d`:
  - every row is a correct group (every sum, `AVG` as the exact pair
    (SUM, COUNT), and `COUNT(*) > 0`);
  - the rows are in `ORDER BY` order, one per group;
  - every non-empty group is present.

The kernel refuses (returns false) instead of overflowing. The host then
answers with SQL on PostgreSQL and MySQL, and raises an error on DuckDB. It
refuses when:

- a qualifying row's flags are not both single upper-case ASCII letters;
- a qualifying row has |quantity| or |extendedprice| ≥ 2^36 hundredths,
  or |discount| or |tax| ≥ 2^7 hundredths (≥ 1.28);
- the state would hold more than 2^40 rows.

The bounds, together with the block size, prove that no `i64` or `i128`
arithmetic overflows. Rows that do not qualify are only counted toward the
row budget.

The spec has plain SQL bag semantics. Decimals are exact integers of
hundredths, so `extendedprice * (1 - discount) * (1 + tax)` is an exact integer
at scale 6. The AVG columns are exact rationals `sum / count`; rounding them to
the engine's AVG result type is host formatting.

## Trusted edges

- **spec**: `gen_spec.rs` is the meaning of the SQL, as produced by
  `sql2verus` (DataFusion 55.1.0, pinned) under the rules at the top of
  `sql2verus/src/main.rs`. The host passes `cutoff = 1998-12-01 - $1` in its
  own day encoding (DateADT on PostgreSQL, `TO_DAYS` on MySQL), i.e.
  `cutoff + e = 10561 - $1`; `finish`'s contract covers every such `e`.
- **scans**: the batches handed to the kernel are lineitem's rows: every row
  with `l_shipdate <= cutoff` exactly once, plus any rows with a later
  shipdate. The columns must be decoded exactly.
  - DuckDB: the native scan with `l_shipdate <= cutoff` pushed down, under the
    query's transaction.
  - PostgreSQL: `table_beginscan` under the statement's active snapshot. NUMERIC
    is decoded from its base-10000 digits; a value that is not exact in
    hundredths answers with SQL.
  - MySQL: `SELECT TO_DAYS(l_shipdate), ...` through the `mysql` CLI, a
    separate connection that sees committed data only. The DECIMAL text is
    parsed exactly.
- **NULLs**: DuckDB skips its per-row NULL check only when lineitem declares
  the seven columns NOT NULL. PostgreSQL checks every row.
- **output formatting**: the DECIMAL/NUMERIC sums are exact.
  - AVG on DuckDB: `DOUBLE`, the same formula as the C++ `accel_tpch_q1`.
  - AVG on PostgreSQL: `numeric` at PostgreSQL's division scale
    (`pg_numeric::AvgOfScaledSum`).
  - AVG on MySQL: `DECIMAL` at scale 6 (2 + `div_precision_increment`),
    rounded half away from zero.
- **collation**: the kernel orders by bytes.
  - PostgreSQL requires the C collation on the flag columns.
  - MySQL's `utf8mb4_0900_ai_ci` agrees with byte order and byte equality on
    upper-case ASCII letters, which is the kernel's domain.
- **glue and tools**: `ffi.rs`, the entry points, the host thread pool and tree
  merge (DuckDB parallel), Verus, Z3, the vstd `Vec` specs, and rustc/LLVM.

## Entry points

| Engine | Call |
|---|---|
| DuckDB | `SELECT * FROM accel_tpch_q1_verus(90)` (single thread), `accel_tpch_q1_verus_simple(90)`, `accel_tpch_q1_verus_parallel(90)` (needs `-DACCEL_ENABLE_PARALLEL_TPCH=ON`) |
| PostgreSQL 17 | `SELECT * FROM accel_tpch_q1_verus(90)`, registered by `scripts/postgres_register_tpch_q1_verus.sql` |
| MySQL 8.4 | `SELECT mysql_accel_tpch_q1_verus(90)` (a `|`-separated text result); `CREATE FUNCTION mysql_accel_tpch_q1_verus RETURNS STRING SONAME 'accelerator_codegen_mysql.so'` |

## Run

```bash
scripts/build_q1_verus.sh                          # verify + build build-verus/libq1_verus.a
# DuckDB (build-verus-duckdb: ACCEL_CODEGEN_ENGINE=duckdb, ACCEL_ENABLE_PARALLEL_TPCH=ON)
ninja -C build-verus-duckdb q1_verus_bench_duckdb
./build-verus-duckdb/q1_verus_bench_duckdb <tpch_sf1.duckdb> 3 10 1
# PostgreSQL / MySQL
scripts/build_accel_postgres_extension.sh
docker exec -i accel_pg_test psql -U postgres -d accel_q21_bench < scripts/postgres_register_tpch_q1_verus.sql
ACCEL_MYSQL_CONTAINER=accel_mysql_sf1 scripts/build_accel_mysql_plugin.sh
python3 benchmarks/q1_verus_bench_pg_mysql.py --warmup 1 --iters 5
```

## Results (SF1, 5 Q1 parameters, medians)

| Engine | Base SQL | Verus kernel | C++ kernel (for reference) |
|---|---|---|---|
| DuckDB, 1 thread | 220 ms | 127 ms (1.7×); simple kernel 148 ms | 76 ms |
| DuckDB, 40 threads | 23.4 ms | 13.6 ms (1.7×) | 12.5 ms |
| PostgreSQL, 1 core (`max_parallel_workers_per_gather = 0`) | 5160 ms | 915 ms (5.6×) | none (stateless) |
| PostgreSQL, default (2 parallel workers) | 1865 ms | 915 ms (2.0×, kernel on 1 core) | none |
| MySQL | 12500 ms | 9760 ms (1.3×) | none (stateless) |

Isolated kernel loop (6M synthetic rows): 7.2 ns/row, against 4.6 ns/row for
the same algorithm in unchecked C++. The difference is the per-row domain
checks and slice bounds checks that the proofs need. On MySQL the time is
dominated by the client-protocol scan (a bare CLI scan of the seven columns
takes ~6.3 s).

See the benchmark CSVs in `benchmarks/duckdb/results/` and
`benchmarks/q1_verus_results/`. Every accelerator run returned the SQL's
result. On PostgreSQL and MySQL, every run is checked to have been answered by
the kernel and not by the SQL fallback.
