# Generic data-structure primitives (`stateful/prims/`)

A growing, workload-agnostic library. Accelerator modules build their versioned
state from these instead of hand-rolling copy-on-write containers; when none fits,
the pipeline adds a generic one here (additions only, with tests) through
`primitive_publish` rather than writing a private copy inside a module.

Workload-agnostic building blocks (header-only, namespace
`accelerator_codegen::aux_state::core` unless noted). Every header starts with a
comment giving purpose, complexity, memory per element and COW/thread rules.
Unit and randomized reference tests: `accelerators/prims/tests/test_prims.cpp`.
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
| `sharded_cow_open_map.hpp` | `ShardedCowOpenMap<K, V, kShardBits, Traits>` | integral or small struct (Traits: hash + empty marker) | any V | sizeof({K, V}) per slot, load <= 0.75, + 32 B per directory entry | COW shards. Key stored in the slot, one cache line per lookup; `Prefetch` / `FindBatch` for batched point lookups; `Reserve`, `BulkLoad`. |
| `sharded_cow_map.hpp` | `ShardedCowMap<K, V, Hash>` + `ShardedCowMapTxn` | any hashable K (strings, composites) | any V | std::unordered_map node (~32 B + K + V) + 8 B per bucket | COW shards of `std::unordered_map`; a `Txn` clones each touched shard once per version. Runtime shard count. |
| `chunked_blob_map.hpp` | `ChunkedBlobMap` | any int64 | byte string | 4 B offset + ~1 B length + payload | COW. Clustered keys; bitmap + arena chunks; chunk-granular iteration for parallel scans. Also varint helpers. |
| `cow_record_store.hpp` | `ShardedCowRecordStore<kShardBits>` | any int64 | byte record | ~13 B slot + 4 B length + payload | COW. Scattered keys, small records. |
| `keyed_string_column.hpp` | `KeyedStringColumn` | any int64 | string | ~12 B + bytes | COW. Parallel SIMD LIKE scan over the whole column. |
| `string_blob.hpp` | `StringBlob`, `StringDict` | id / string | string | 8 B + bytes; dict adds ~9-12 B | Append-only, not COW. |

### Composition helpers

| Header | Primitive | Use it for |
| --- | --- | --- |
| `cow_box.hpp` | `CowBox<T>` | Making a state struct that holds containers with an O(directory) copy cheap to copy: the box copies one pointer and clones `T` on the first `Mutable()` while shared. |

### Ordered and aggregate structures

| Header | Primitive | Operations | Memory | Notes |
| --- | --- | --- | --- | --- |
| `sharded_sorted_map.hpp` | `ShardedSortedMap<K, T, KeyOf, ...>`, `ShardedSortedKvMap<K, V>` | find / upsert / erase through a transaction, bulk sorted build, per-shard ordered scan | sizeof(T) per entry + ~24 B per shard | COW hash-sharded map with key-sorted flat shards; a Txn clones each touched shard and directory page once; the shard count is a template cap plus a runtime choice from the expected size. |
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
  Keys with no locality, small values, latency-critical and batchable point
  lookups (probe-side joins, key -> id indexes): `ShardedCowOpenMap` (key in the
  slot, prefetch / `FindBatch`; reserve one key value as the empty marker).
  Strings or composite keys, values that own memory, one write batch per
  version: `ShardedCowMap` + `ShardedCowMapTxn`.
  Millions of records keyed by an id (or any ordered key type with a hash), read
  by point lookup, updated in batches that must publish a new version, and built
  from a scan in any order: `ShardedSortedMap` (records stay in their natural
  layout, one binary search in a ~100-500 entry shard per lookup, a batch clones
  only the touched shards). Choose `ShardedCowHashMap` instead when the key is
  int64, lookups are hot, and nothing needs sorted shards, a bulk sorted build
  or a record that carries its own key.
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
