# Q21 with a Verus-verified kernel (pilot)

The Q21 accelerator kernel is written in Rust and verified with Verus against a
reference specification generated from the catalog SQL by `sql2verus`. It is linked into the
Postgres extension as a static library and serves `accel_tpch_q21_verus(text)`.
It uses Verus only.

| Piece | Where | Status |
|---|---|---|
| q_ref (the SQL's meaning) | [`src/verus/q21/src/gen_spec.rs`](../../../src/verus/q21/src/gen_spec.rs), generated from [`queries/q21.sql`](../../../src/verus/sql2verus/queries/q21.sql) by [`sql2verus`](../../../src/verus/sql2verus/src/main.rs), + `bytes_lt`, `sum` in `common/` | **Trusted**: the translator's rules, applied mechanically; `scripts/gen_verus_specs.sh --check` fails if the file is stale |
| Proof vocabulary | [`bridge.rs`](../../../src/verus/q21/src/bridge.rs): `numwait` (= the generated `group_count`), `strict_full` (proved to imply the generated `full_result`), `late`, `is_f`, … | Verified, not trusted |
| Shared library (bag sums, merge sort, C-collation order incl. `bytes_lt`, bag edits of small tables, `Rat` for AVG, sorted-sequence uniqueness/existence) | [`src/verus/common/`](../../../src/verus/common/src/) | Verified (66 items), exported as `verus_common` |
| Kernel + Q21 proofs | `src/verus/q21/src/*.rs` (except `ffi.rs`) | Verified (144 items, incl. the generated facts) |
| Spec facts: `query_result` has exactly one solution, the first 100 rows of `answer` (`lemma_query_result_iff`) | Generated with the spec, in the "Facts" section of `gen_spec.rs` | Verified (included above), not trusted |
| C ABI shims | [`src/verus/q21/src/ffi.rs`](../../../src/verus/q21/src/ffi.rs) | Trusted glue (pointer → slice) |
| Host adapter | [`src/generated/tpch/q21/verus/module.cpp`](../../../src/generated/tpch/q21/verus/module.cpp), `TpchQ21VerusScanImpl` in `src/generated/postgres/runtime.cpp` | Trusted (scans, capture, sink) |

## What is proved

`State` carries the database it answers for (ghost), and its type invariant ties
every executable structure to that database (`state.rs`). The three operations
are:

- **Build** (`build.rs`). If the scans satisfy `build_contract(db)`, the result
  is a state for `db`, or `None` on i64 overflow, and the host then uses SQL.
- **Execute** (`execute.rs`). For every state `s` and nation `p`, the output
  satisfies the generated `query_result(s.db(), {p1: p}, out)`: the first 100
  rows (`LIMIT 100`) of the full answer `full_result`, whose rows are in
  `ORDER BY numwait DESC, s_name` order, one row per group with its `COUNT(*)`,
  and every group present. It can also return `None` on overflow.
- **ApplyDelta** (`maint.rs`). If `db2 = db + deltas` for all four tables
  (`db_step` for orders / lineitem, `dims_step` for supplier / nation, stated
  as bag equality) and the targeted scan satisfies `scan_contract(db2)`, the
  result is a state for `db2`, or `Err`, and the host then rebuilds.
  `lemma_bag_step_multiset` (`common/src/sums.rs`) shows the bag-equality form
  is exactly *after + deleted = before + inserted* as multisets.

Every change to lineitem, orders, supplier, or nation is maintained
incrementally. Only `TRUNCATE`, writes from another session (runtime
protocol), and kernel refusals (overflow, missing scan) lead to a rebuild.

The spec has plain SQL bag semantics. It assumes **no** primary or foreign keys
(`orders` has no key in this schema), no key uniqueness, and no referential
integrity; duplicate supplier or nation rows are handled exactly. The C++ Q21 kernel's `KeysOk`/`DomOk`
assumptions are therefore not needed.

## Trusted edges

- **spec**: `gen_spec.rs` is the meaning of the catalog SQL, as produced by
  `sql2verus` (DataFusion 55.1.0, pinned) under the rules at the top of
  `sql2verus/src/main.rs`. Every proof, and every host contract below
  (`build_contract`, `db_step`, `scan_contract`), is stated over
  `gen_spec::Db`. Dates are days since 1970-01-01, which is what all three
  hosts deliver (PostgreSQL adds 10957 to DateADT in its scans and capture).
- **scans**: the four Build scans and the PRE_COMMIT targeted lineitem scan
  return the rows their SQL names. The targeted scan runs after
  `CommandCounterIncrement()` under `GetLatestSnapshot()`.
- **capture**: the transition-table triggers plus `AccumulateMutation` produce
  exactly the transaction's signed row changes for all four tables.
  An orders delta carries only the flag `status_is_f`; the spec's `Order`
  has the real `o_orderstatus` bytes, which `apply` receives as a ghost
  argument. Its precondition `od_status_ok` (`state.rs`) states the trusted
  fact that the flag is set exactly when those bytes equal `"F"`.
- **runtime protocol**: the store serves a state only for snapshots that see its
  database (`aux_sync`; trusted, not modelled).
- **glue**: `ffi.rs` (11 `unsafe` pointer conversions), and the adapter's
  text/int8 sink.
- **tools**: Verus, Z3, the vstd specs of `HashMap`/`Vec`/`Arc`, rustc/LLVM.
- **collation**: `s_name` has a C collation (enforced by the runtime).
- **CHAR(25)**: `s_name` / `n_name` reach the kernel (scans, capture) in the
  form the engine compares them, and `:nation` as the engine compares it
  against: PostgreSQL `bpchar` values without their trailing-space padding
  (its `bpchar -> text` cast; `TypedCharText`, `NormalizeCapturedName`),
  MySQL `CHAR` as retrieved (padding already removed), DuckDB unchanged (its
  `CHAR` is `VARCHAR`). `:nation` is passed unchanged.

## Engines

The kernel (`libq21_verus.a`) and the engine-neutral module
([`src/generated/tpch/q21/verus/module.cpp`](../../../src/generated/tpch/q21/verus/module.cpp))
are linked into `accelerator_codegen`; each engine adds only its scans and
entry point. The proofs are the same for every engine; the trusted edges
(scans, capture, protocol, text semantics) are per engine.

| Engine | Entry point | Scans | Capture / maintenance | Text semantics |
|---|---|---|---|---|
| PostgreSQL 17 | `accel_tpch_q21_verus(text)` (SQL function) | SPI; targeted scan in PRE_COMMIT after `CommandCounterIncrement` | transition-table triggers, maintenance at PRE_COMMIT | runtime refuses unless `s_name` has C collation |
| DuckDB 1.5 | `accel_tpch_q21_verus(VARCHAR)` (table function) | runtime connection, inside the write transaction | explicit `DuckdbRuntime` write API (no automatic capture) | binary by default |
| MySQL 8.4 | `mysql_accel_tpch_q21_verus(str)` (UDF, TSV string) | `mysql` CLI connection (committed data only) | triggers + audit plugin, maintenance after each **autocommit** statement | kernel used only if `s_name`, `n_name`, `o_orderstatus` are `utf8mb4_0900_bin`; SQL otherwise |

Engine-specific limits:

- **MySQL.** The capture contract holds only for autocommit writes. Inside
  `BEGIN … COMMIT`, the substrate maintains after each statement, and a CLI
  scan cannot see uncommitted rows. The runtime is process-wide with no
  cross-session protocol. The result travels as `name|count` lines, so names
  must not contain `|` or newlines.
- **DuckDB.** Writes reach the store only through the runtime's write API.

## Run

```bash
scripts/build_q21_verus.sh                    # verify common + q21, build build-verus/libq21_verus.a
scripts/build_accel_postgres_extension.sh     # links it when present
psql -d <db> -c "LOAD 'accelerator_codegen_pg'" -f scripts/postgres_register_tpch_q21_verus.sql
python3 tests/verification/q21verus/q21_verus_stress.py 60 4 1   # differential vs SQL (accel_q21_rt)
# Q21_FORCE=ApplyDelta|RebuildPartition|RebuildAll|InvalidateAndFallback pins the maintenance
# action in the Postgres / MySQL stress tests (RebuildPartition runs as RebuildAll: no partitions);
# the DuckDB fuzz takes it as its last argument (policy = the cost model).
python3 benchmarks/postgres/q21_verus_bench.py                     # SF1 benchmark
# DuckDB: build-verus-duckdb (ACCEL_CODEGEN_ENGINE=duckdb), target q21_verus_bench_duckdb
./build-verus-duckdb/q21_verus_bench_duckdb <copy-of-tpch_sf1.duckdb> 20 30 3
# MySQL: scripts/build_accel_mysql_plugin.sh; CREATE FUNCTION mysql_accel_tpch_q21_verus ...
python3 tests/verification/q21verus/q21_verus_mysql_stress.py 1500 1        # accel_mysql_test (3307)
python3 benchmarks/mysql/q21_verus_bench.py                                  # accel_mysql_sf1 (3308)
```

## Update coverage of the differential tests

All three tests (Postgres stress, MySQL stress, DuckDB fuzz) update every column
Q21 reads on all four tables, alone and several at once, key changes included:
`l_orderkey`, `l_suppkey`, `l_commitdate`, `l_receiptdate`; `o_orderkey`,
`o_orderstatus`; `s_suppkey`, `s_name`, `s_nationkey`; `n_nationkey`, `n_name`
(plus `l_comment`, a column Q21 does not read). The Postgres and MySQL tests also
update all rows of an order in one statement and many orders in one statement.
The MySQL test respects the primary keys (`UPDATE IGNORE`, moving lineitems to
other orders with a fresh line number) and prints, per set of updated columns,
how many statements changed a row. Each test reports the maintenance action that
ran and fails if a forced action did not run.
