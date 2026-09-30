# Q21 with a Verus-verified kernel (pilot)

The Q21 accelerator kernel is written in Rust and verified with Verus against a
hand-written reference specification of the catalog SQL. It is linked into the
Postgres extension as a static library and serves `accel_tpch_q21_verus(text)`.
It uses Verus only: no Logos/FormalSQL, Rocq, or Dafny.

| Piece | Where | Status |
|---|---|---|
| q_ref (the SQL's meaning) | [`src/verus/q21/src/spec.rs`](../../../src/verus/q21/src/spec.rs) + `bytes_lt` in `common/src/bytes.rs` | **Trusted**: reviewed against the SQL |
| Shared library (bag sums, merge sort, C-collation order incl. `bytes_lt`, bag edits of small tables) | [`src/verus/common/`](../../../src/verus/common/src/) | Verified (59 items), exported as `verus_common` |
| Kernel + Q21 proofs | `src/verus/q21/src/*.rs` (except `spec.rs`, `ffi.rs`) | Verified (134 items) |
| C ABI shims | [`src/verus/q21/src/ffi.rs`](../../../src/verus/q21/src/ffi.rs) | Trusted glue (pointer → slice) |
| Host adapter | [`src/generated/tpch/q21/verus/module.cpp`](../../../src/generated/tpch/q21/verus/module.cpp), `TpchQ21VerusScanImpl` in `src/generated/postgres/runtime.cpp` | Trusted (scans, capture, sink) |

## What is proved

`State` carries the database it answers for (ghost), and its type invariant ties
every executable structure to that database (`state.rs`). The three operations
are:

- **Build** (`build.rs`). If the scans satisfy `build_contract(db)`, the result
  is a state for `db`, or `None` on i64 overflow, and the host then uses SQL.
- **Execute** (`execute.rs`). For every state `s` and nation `p`, the output
  satisfies `q21_result(s.db(), p, out)`: rows in `ORDER BY numwait DESC, s_name`
  order, one row per group with its `COUNT(*)`, and every group present. It can
  also return `None` on overflow.
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

- **spec**: `spec.rs` is the meaning of the catalog SQL. Semantic decisions are
  listed at the top of that file.
- **scans**: the four Build scans and the PRE_COMMIT targeted lineitem scan
  return the rows their SQL names. The targeted scan runs after
  `CommandCounterIncrement()` under `GetLatestSnapshot()`.
- **capture**: the transition-table triggers plus `AccumulateMutation` produce
  exactly the transaction's signed row changes for all four tables.
  `status_is_f` is computed as `o_orderstatus == "F"`.
- **runtime protocol**: the store serves a state only for snapshots that see its
  database (`aux_sync`, modelled in `substrate/dafny/AuxSyncProtocol.dfy`).
- **glue**: `ffi.rs` (11 `unsafe` pointer conversions), and the adapter's
  text/int8 sink.
- **tools**: Verus, Z3, the vstd specs of `HashMap`/`Vec`/`Arc`, rustc/LLVM.
- **collation**: `s_name` has a C collation (enforced by the runtime).

Compared with the Rocq+Dafny stack for C++ Q21, this removes `cpp_model` (the
C++ ↔ twin link via extract-diff) and `spec_export_B` (the Rocq ↔ Dafny
interpreter correspondence). In exchange, the SQL meaning is a reviewed spec,
not FormalSQL.

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
python3 benchmarks/postgres/q21_verus_bench.py                     # SF1 benchmark
# DuckDB: build-verus-duckdb (ACCEL_CODEGEN_ENGINE=duckdb), target q21_verus_bench_duckdb
./build-verus-duckdb/q21_verus_bench_duckdb <copy-of-tpch_sf1.duckdb> 20 30 3
# MySQL: scripts/build_accel_mysql_plugin.sh; CREATE FUNCTION mysql_accel_tpch_q21_verus ...
python3 tests/verification/q21verus/q21_verus_mysql_stress.py 1500 1        # accel_mysql_test (3307)
python3 benchmarks/mysql/q21_verus_bench.py                                  # accel_mysql_sf1 (3308)
```
