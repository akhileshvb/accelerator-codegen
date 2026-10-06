# Maintenance actions

Every accelerator offers the same actions; the cost model
(`aux_maintenance_policy.cpp`) picks the cheapest for each commit:

| Action | Effect | Cost model |
|---|---|---|
| `NoOp` | nothing relevant changed | 0 |
| `ApplyDelta` | the state is updated from the captured rows | overhead + per row |
| `RebuildPartition` | the touched partitions are rebuilt from the tables | base + per partition (an accelerator without finer partitions rebuilds its whole state, priced as `RebuildAll`, which wins a tie) |
| `RebuildAll` | the whole state is rebuilt | full rebuild |
| `InvalidateAndRebuildOnRead` | invalidated now; the next reader rebuilds it and answers from it | one full rebuild, times min(1, reads expected per write): it wins only when writes outnumber reads; offered only where readers can build the state |

Readers answer from SQL (`ExecuteFallback`) only as a last resort: when no
state can serve their snapshot (their own uncommitted writes; on PostgreSQL,
a snapshot that no certified per-backend state matches). Each engine runtime
counts how queries were answered (`aux_execute_stats.hpp`: hit, rebuilt on
read, fallback); the fuzz harnesses report it.

**Calibration** (`aux_calibration.hpp`, engine-agnostic): the policy reads
`config/qN_aux_cost_model[_postgres|_mysql].json`. `CalibrateFromWrites`
runs the module's `CalibrationWrites()` (real writes of increasing size and
spread over its partitions, each with a revert and a control write on a
column it does not read) through an engine's `CalibrationEngine` hook, with
each action forced. An action's cost is the coordinator's own maintenance
time for the commit (recorded in the decision log) plus the first read's
cost over a hit, so the capture of the changed rows and the engine's commit,
the same for every action, are not charged. It fits ApplyDelta = overhead +
per row, RebuildPartition = base + per partition, RebuildAll = mean. The
engine hooks: `DuckdbCalibrationEngine` (DuckDB runtime backend), and the
PostgreSQL / MySQL calibrators' clients (`benchmarks/{postgres,mysql}/
calibrate_*_tpch_aux_cost_model.cpp`, over the extension's SQL controls).

# Concurrency: versioned state and pending commits

All engines use one store, `VersionedAuxStore`
(`state/versioned_aux_store.hpp`); each engine store only configures it.

**Versions.** Each accelerator's state is a short list of versions. A version
is the accelerator's whole state (an immutable partition map; unchanged
partitions are shared between versions) and the **epoch** from which it is
current. A reader at snapshot epoch `E` sees the newest version with epoch
`<= E`. Every partition it reads comes from that one version.

- A reader epoch of `kNewestSnapshot` (0) means "no snapshot: the newest".
- A version epoch of `kAllSnapshots` (0) means "current for every snapshot";
  it replaces the history.

**One version per query.** Inside an `AuxReadScope` (opened per query by each
engine's runtime), the first read of an accelerator pins its version. Later
reads in the query reuse that version.

**Commits** (the coordinator, `aux_registry.cpp`, per accelerator):

- **PRE_COMMIT**: `StageMaintenance` builds the next state (no store lock).
  Then `BeginCommit` marks the accelerator pending from the commit's epoch:
  - readers whose snapshot may include the commit (`E >=` that epoch, or
    newest) wait for it, without a timeout (`kWaitForPublish`; set
    `ACCEL_AUX_PENDING_WAIT_US` to bound the wait);
  - older snapshots read on.

  **Readers that cannot build the state.** An engine whose reader cannot
  build the state at its own snapshot (`AuxEngineServices::ReadersCanBuildState()
  == false`; DuckDB, where an aux table function cannot query its own context)
  is not offered `InvalidateAndRebuildOnRead`, and never gets an invalid state
  published: in PRE_COMMIT any outcome that would leave a partition
  invalidated and not rebuilt becomes a synchronous `RebuildAll`.

  At most one commit per accelerator is in flight; a second `BeginCommit`
  throws.
- **COMMIT**: `FinishCommit` appends one new version (invalidations and
  updates applied together) at the commit's epoch, and ends the pending mark.
  A full rebuild (invalidate all partitions, with updates) replaces the
  state: partitions the rebuild no longer has are dropped, not left invalid.
- **ABORT**: `AbortCommit` ends it; nothing changes.
- **COMMIT throws**: the touched partitions are invalidated for every
  snapshot.

**Outside commits:**

- `Invalidate` makes partitions unusable for every snapshot (every kept
  version).
- `Install` replaces the state with a fresh build, current from a given
  epoch.

**Pruning.** Old versions are pruned once no open snapshot can select them.
The engine supplies the floor (the oldest open snapshot's epoch). Without a
floor provider only the newest version is kept.

**Locking.** Readers take a shared (writer-preferring) lock, writers an
exclusive one. A waiting reader sleeps on a condition variable. Readers get
`shared_ptr` payloads, and a module must hold the payload while using it.

What each engine supplies:

| Engine | Snapshot epoch | Commit epoch | Floor |
|---|---|---|---|
| DuckDB | the reading transaction's start time (`DuckDBAuxBackend::ReaderScope`) | commit id + 1 (`AuxCommitClock`, told by the fork's `CommitObserver`) | lowest active start time |
| PostgreSQL | the statement's pinned `AuxMutationEpoch` | `AuxMutationEpoch` at COMMIT | none (per-backend store) |
| MySQL | newest | the runtime's epoch provider, if any | none |

DuckDB visibility is exact: a transaction sees commit `c` iff `c` is below
its start time, which is the condition for the version at `c + 1` to be
selected. A DuckDB build is current from the start time of a transaction
begun when the build starts.

**The verified Verus store.** A Verus accelerator's state can instead live
in its Verus store (`src/verus/substrate`, `stateful/state/verus_aux_store.hpp`):
the same protocol (versions from engine epochs, a pending commit readers
wait for, pruning to the floor) behind one verified reader-writer lock, with
a proof that every kept version's state answers for the database at that
version. Differences from `VersionedAuxStore`:

- the module prepares each next version from the store's newest version
  (`q21vs_prepare_*`), not from a payload the host passes back;
- one commit at a time (`BeginCommit` while one is in flight throws, as
  here); an install whose base is no longer the newest leaves one invalid
  version (`StaleInstalls`, should stay 0);
- a waiting reader spins with backoff (no condition variable); no
  `AuxReadScope` pinning (a module reads its state once per query).

`ACCEL_AUX_STORE=verus` selects it for accelerators that have one (the Verus
pipeline's kits), on DuckDB; anything else keeps the C++ store. Postgres
and MySQL use the C++ store.

# Shared auxiliary-state calibration

The shared core carries immutable, query-owned state images as typed payloads.
Engine adapters decide store mode:

- **typed_in_process** — publish typed `StateImage` payloads directly (no
  serialize/deserialize on the hot path).
- **serialized_boundary** — store bytes at a process/SHM boundary and decode
  before exposing the same typed `StateSnapshot` to a module (`byte_stream.hpp`).

`aux_calibration.hpp` contains the engine-neutral calibration runner. A backend
supplies callbacks that perform the real operation against its own base tables
and state store:

- build the initial image;
- execute the auxiliary and fallback paths;
- apply a delta batch of a requested size;
- rebuild one partition and the complete state; and
- invalidate the state.

The runner uses warmups and median wall-clock samples. It fits the complete
shared-interface delta path, including state materialization, the maintenance
kernel, output refresh, and publication preparation. The fitted intercept is
stored in `apply_delta_overhead_us`, the per-row slope in
`apply_delta_per_row_us`, and the compatibility field
`apply_delta_base_us` is set to zero so the intercept is not charged twice by
the selector. The result is written as flat JSON consumed by
`LoadAuxCostModelJson`.

The engine adapter should seed calibration from its existing query-specific
configuration so thresholds and confidence parameters survive calibration.
Only the measured timing fields are replaced. This keeps calibration from
silently changing the query module's correctness or safety limits.

Adapters typically accept a `CostModelCallback` and/or an env var such as
`ACCELERATOR_CODEGEN_{ENGINE}_AUX_COST_MODEL` (see the active host adapter).
Missing files intentionally fall back to default parameters.

The maintenance selector also consumes the adapter's estimate of queries
before the next write (`WorkloadCallback` or an engine-specific expected-queries
env/GUC).

## Generic data-structure primitives

Workload-agnostic building blocks (header-only, namespace
`accelerator_codegen::aux_state::core` unless noted). Every header starts with a
comment giving purpose, complexity, memory per element and COW/thread rules.
Unit and randomized reference tests: `tests/aux_state/test_core_primitives.cpp`.
"COW" means: copying is cheap (pointer copies), copies share payload, and a
write clones only the touched chunk/shard; mutate a private copy and publish it,
concurrent const reads of a published copy are safe.

### Per-key columns and maps

| Header | Primitive | Key | Value | Memory per element | Notes |
| --- | --- | --- | --- | --- | --- |
| `dense_id_array.hpp` | `DenseIdArray<T>` | id >= 0 | fixed-size | sizeof(T) per slot of touched 64K leaves | Not COW; single owner / scratch. |
| `chunked_cow_array.hpp` | `ChunkedCowArray<T, kShift>` | id >= 0 | fixed-size | sizeof(T) per slot of allocated chunks + 16 B per directory entry | COW base block; directory has one entry per 2^kShift ids. |
| `any_key_cow_array.hpp` | `AnyKeyCowArray<T, kShift, kShardBits>` | any int64 | fixed-size | sizeof(T) per slot of touched chunks; ~60 B per far key | COW. Zigzag dense range [-2^35, 2^35) for both signs, two-level sparse directory (memory only for touched pages), sharded hash for far keys, ordered / full iteration. |
| `flat_hash_map.hpp` | `FlatHashMap<V>` | any int64 | any V | ~10 B + 1.14 sizeof(V) | Not COW. Swiss-table style. |
| `flat_hash_map.hpp` | `ShardedCowHashMap<V, kShardBits>` | any int64 | any V | same | COW shards. |
| `chunked_blob_map.hpp` | `ChunkedBlobMap` | any int64 | byte string | 4 B offset + ~1 B length + payload | COW. Clustered keys; bitmap + arena chunks; chunk-granular iteration for parallel scans. Also varint helpers. |
| `cow_record_store.hpp` | `ShardedCowRecordStore<kShardBits>` | any int64 | byte record | ~13 B slot + 4 B length + payload | COW. Scattered keys, small records. |
| `keyed_string_column.hpp` | `KeyedStringColumn` | any int64 | string | ~12 B + bytes | COW. Parallel SIMD LIKE scan over the whole column. |
| `string_blob.hpp` | `StringBlob`, `StringDict` | id / string | string | 8 B + bytes; dict adds ~9-12 B | Append-only, not COW. |

### Ordered and aggregate structures

| Header | Primitive | Operations | Memory | Notes |
| --- | --- | --- | --- | --- |
| `cow_sorted_blocks.hpp` | `CowSortedBlocks<Key, Less>` | insert / erase / contains / in-order scan with early exit | ~1.35 sizeof(Key) per key | COW ordered set under a custom order. |
| `sorted_count_sum.hpp` | `SortedCountSum` | add(key, n), count and sum of keys > x | 16 B per distinct key | COW multiset with block summaries; exact int128 sums. |
| `radix_range_tree.hpp` | `RadixRangeTree<T>` | point add, range sum over [0, 2^24) | sizeof(T) per touched slot x 4 levels | COW, additive T. |
| `cow_fenwick.hpp` | `CowFenwick<Cell>` | update, prefix visit over int32 positions | <= 32 nodes per populated position | COW Fenwick tree with arbitrary per-node cells. |

### SQL value semantics (stateless)

| Header | Primitive | Use it for |
| --- | --- | --- |
| `sql_date.hpp` | `sql_date::*` | days-since-1970 <-> civil date for any int32 day; month/year interval arithmetic with end-of-month clamping. |
| `sql_like.hpp` | `sql_like::LikeMatcher`, `FindSubstring`, `MatchBlobBitmap/Indices` | LIKE (`%`, `_`, escape, case folding) compiled once, SIMD substring search, bulk match over offsets + blob. Byte-wise: the caller picks collation behavior. |
| `int128_acc.hpp` | `i128::*` | overflow-checked int128 accumulation for exact SUM state. |
| `pg_numeric.hpp`, `pg_numeric_div.hpp` | `pg_numeric::*` | PostgreSQL NUMERIC result scale, avg, division, exact text output. |
| `decimal_text.hpp` | `decimal_text::Parse` | exact decimal text -> int128 coefficient at a fixed scale. |
| `byte_stream.hpp` | little-endian readers/writers | serialized state images at a process boundary. |

### Which one to use

- **Value per integer key, fixed size.** Ids are bounded and non-negative and
  the state is published as versions: `ChunkedCowArray`. Sign or range unknown,
  sparse, or you must scan it: `AnyKeyCowArray` (the default; a far or negative
  key is exact, only slower per key). Never published, single owner:
  `DenseIdArray`. Keys with no locality and fat values: `ShardedCowHashMap`.
- **Variable-length value per key.** Keys cluster (dense or locally dense):
  `ChunkedBlobMap`. Keys scattered, records small: `ShardedCowRecordStore`.
  The column must be filtered by a LIKE pattern: `KeyedStringColumn`. Build-time
  dictionary of distinct strings: `StringDict`.
- **Ordered data.** A set under a custom order with top-k / threshold scans:
  `CowSortedBlocks`. Rank / suffix count-and-sum of numbers: `SortedCountSum`.
  Range sums over a bounded integer domain: `RadixRangeTree`. Prefix aggregates
  of structured cells over int32 positions: `CowFenwick`.
- **Arithmetic and text semantics.** Dates and intervals: `sql_date`. Patterns:
  `sql_like`. Exact sums: `int128_acc`; NUMERIC output and division:
  `pg_numeric*`; decimal text input: `decimal_text`.

Scale guidance: prefer columnar arrays at ~1-16 B per key over node-based maps
with per-row strings; use hash maps only when the key space has no locality; use
the COW variants when a write batch must publish a new immutable state in
O(touched chunks) rather than O(state). Small `kShift` (5-8) suits arrays where
few slots per chunk are used; larger (12-16) suits dense 1-4 byte columns.
