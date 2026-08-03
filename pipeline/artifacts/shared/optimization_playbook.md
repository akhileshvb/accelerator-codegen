# Optimization playbook (seeded)

Cross-query experiment memory. Updated by orchestrator from `playbook_proposal.md`.

**State-family selection:** See `pipeline/prompts/aux_state/shared/aux_state_taxonomy.md` for when to use partitioned aggregates, hierarchies, join-derived state, top-k, existence counts, hybrids, etc. Match the dominant base cost (scan, join, sort, correlation) before tuning cost-model coefficients.

## dense_direct_index

**Worked:** Q3 (+12% SF1 typical), Q9, Q12, Q14  
**Skip if:** domain >90% sparse, non-integer keys

## dictionary_over_flat

**Worked:** when DICTIONARY→FLAT dominates lineitem/orders scans

## bitmap_membership

**Worked:** Q4 order-key membership

## Anti-patterns

- unordered_map for dense integer domains without SF10 evidence
- SF1 table-size constants in production path
- Optimizing read-only speed while write-frequency sweep composite_score is flat (check 10% and 50% points)

## Q1 update
# SEC-EDGAR Q1 optimization iteration 1

## Hypothesis

**Target bottleneck:** `pre_consume` (~50% estimated share), sub-phase `distinct_adsh_tracking` (~15%).

The production kernel used `std::unordered_set<std::string>` for COUNT(DISTINCT adsh). Every row called `emplace(adsh.GetData(), adsh.GetSize())`, which constructs a temporary `std::string` on **every** hash probe — even when the adsh was already present. Over millions of qualifying rows this dominates consume CPU and heap traffic.

**Technique:** Zero-copy distinct lookup — custom `DistinctAdshSet` that hashes/compares `duckdb::string_t` bytes directly and only allocates/copies the adsh string on first insert per group.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Replaced `std::unordered_set<std::string>` with `DistinctAdshSet` (FNV-1a bucket chain, rehash at load factor)
- `AggregateRow` now calls `distinct_adsh.Insert(adsh)` — probe via memcmp, copy only on miss

## Before (baseline gate, iter 0)

| Metric | Value |
|--------|-------|
| reference_median_us | 435,520 |
| accelerator_median_us | 820,020 |
| speedup_median | **0.53×** |

Profile gate returned null phase breakdown (no measured profile yet).

## After

_Pending automatic correctness / baseline / profile gates._

Expected: lower `pre_consume_us`, reduced `distinct_insert_ns`, improved `speedup_median` vs 0.53× baseline.

## Playbook signal

- **Technique:** string_t distinct keys without copy (custom hash set)
- **When:** COUNT(DISTINCT varchar) over high-cardinality keys in scan-heavy group-by
- **Retain if:** speedup_median improves; revert otherwise before iter 2

## Q1 update
# SEC-EDGAR Q1 optimization iteration 2

## Hypothesis

**Target bottleneck:** `pre_consume` (934,271 µs measured; ~50% of execution time), driven by vector decode overhead.

Profile counters show **4,682 / 4,694 chunks** (99.7%) on the `UnifiedVectorFormat` generic fallback path (`generic_chunks=4682`, `flat_chunks=12`, `dict_chunks=0`). The existing dictionary fast path required **all four** projected columns — including `adsh` — to be `DICTIONARY→FLAT`. High-cardinality `adsh` (~hundreds of thousands distinct filing keys) is stored as **FLAT** in DuckDB columnar storage, so the dict path never fired despite low-cardinality `stmt`/`rfile` being dictionary-compressed.

**Technique:** Mixed dictionary-over-flat fast path — `DICTIONARY stmt` + `DICTIONARY rfile` + `FLAT adsh` + `FLAT line` (all-valid). Avoids per-chunk `ToUnifiedFormat` setup and validity branching on the dominant storage layout.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Added `elif` branch in `ConsumePreChunk` before the all-dict path: dictionary stmt/rfile with flat adsh/line
- Existing all-dict and generic fallback paths preserved unchanged

## Before (baseline gate, iter 1)

| Metric | Value |
|--------|-------|
| reference_median_us | 434,230 |
| accelerator_median_us | 529,780 |
| speedup_median | **0.82×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 934,271 |
| pre_scan_us | 1,008,544 |
| flat_chunks | 12 |
| dict_chunks | 0 |
| generic_chunks | 4682 |
| qualifying_rows | 9,599,726 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: majority of chunks move from `generic_chunks` → `dict_chunks`; lower `pre_consume_us`; improved `speedup_median` vs 0.82× baseline.

## Playbook signal

- **Technique:** dictionary_over_flat (mixed layout: dict group keys + flat high-cardinality payload column)
- **When:** scan columns split between low-cardinality dictionary-compressed group keys and flat high-cardinality VARCHAR/INT payload
- **Retain if:** speedup_median improves; revert otherwise before iter 3

## Q1 update
# SEC-EDGAR Q1 optimization iteration 3

## Hypothesis

**Target bottleneck:** `pre_consume` (871,942 µs measured; ~50% of execution time), driven by per-chunk `UnifiedVectorFormat` decode overhead.

Profile counters show **4,682 / 4,694 chunks** (99.7%) on the generic fallback path (`generic_chunks=4682`, `flat_chunks=12`, `dict_chunks=0`). Iteration 2 added a strict dictionary fast path but it never fired (`dict_chunks=0`), indicating the dominant layout is **FLAT vectors with nullable validity masks** (e.g. null `stmt`/`line` rows in row groups) rather than all-valid flat or dictionary-over-flat.

**Technique:** Flat nullable fast path — when all four projected columns are `FLAT_VECTOR`, use direct `FlatVector` data pointers and consult validity masks only when `!AllValid()`, avoiding four `ToUnifiedFormat` calls per chunk.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Added `IsFlat()` helper; inserted `elif` branch in `ConsumePreChunk` after existing all-valid flat/dict paths
- Per-row null checks on stmt (with skip counter), rfile, adsh, line; generic fallback preserved for non-FLAT layouts

## Before (baseline gate, iter 2)

| Metric | Value |
|--------|-------|
| reference_median_us | 430,580 |
| accelerator_median_us | 527,410 |
| speedup_median | **0.82×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 871,942 |
| pre_scan_us | 941,361 |
| flat_chunks | 12 |
| dict_chunks | 0 |
| generic_chunks | 4682 |
| qualifying_rows | 9,599,726 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: majority of chunks move from `generic_chunks` → `flat_chunks`; lower `pre_consume_us`; improved `speedup_median` vs 0.82× baseline.

## Playbook signal

- **Technique:** flat nullable fast path (minimize nullable branches)
- **When:** native scan returns FLAT columns with partial validity masks; strict `AllValid()` gating leaves almost all chunks on UnifiedVectorFormat
- **Retain if:** speedup_median improves; revert otherwise before iter 4

## Q1 update
# SEC-EDGAR Q1 optimization iteration 4

## Hypothesis

**Target bottleneck:** `pre_consume` (854,723 µs measured; ~50% of execution time), driven by per-chunk `UnifiedVectorFormat` decode overhead.

Profile counters show **4,682 / 4,694 chunks** (99.7%) on the generic fallback path (`generic_chunks=4682`, `flat_chunks=12`, `dict_chunks=0`). Iterations 2–3 added dictionary and flat-nullable fast paths but `dict_chunks` never fired: the existing dict branch required `IsFlatAllValid` on dictionary **children** as well as flat adsh/line. Native SEC-EDGAR storage delivers low-cardinality `stmt`/`rfile` as `DICTIONARY→FLAT` with nullable dictionary children (null stmt rows in row groups), so the strict all-valid gate blocked the path.

**Technique:** Dictionary nullable + flat payload fast path — match `DICTIONARY stmt` + `DICTIONARY rfile` + `FLAT adsh` + `FLAT line` without requiring all-valid dictionary children; consult child validity masks only when `!AllValid()`, mirroring Q14 `ConsumePartFlatDictType` / Q12 `ConsumeOrdersFlatDictPriority`.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Relaxed the mixed dict+flat branch: removed `IsFlatAllValid` on dictionary children and flat adsh/line; added per-row null checks on stmt/rfile dictionary child indices and flat adsh/line
- Generic fallback preserved for non-matching layouts

## Before (baseline gate, iter 3)

| Metric | Value |
|--------|-------|
| reference_median_us | 433,780 |
| accelerator_median_us | 537,390 |
| speedup_median | **0.81×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 854,723 |
| pre_scan_us | 922,637 |
| flat_chunks | 12 |
| dict_chunks | 0 |
| generic_chunks | 4682 |
| qualifying_rows | 9,599,726 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: majority of chunks move from `generic_chunks` → `dict_chunks`; lower `pre_consume_us`; improved `speedup_median` vs 0.81× baseline.

## Playbook signal

- **Technique:** dictionary_over_flat (nullable dictionary children + flat high-cardinality payload)
- **When:** group-key columns are dictionary-compressed with partial validity; strict `AllValid()` on dictionary children leaves almost all chunks on UnifiedVectorFormat
- **Retain if:** speedup_median improves; revert otherwise before iter 5

## Q1 update
# SEC-EDGAR Q1 optimization iteration 5

## Hypothesis

**Target bottleneck:** `pre_consume` (856,603 µs measured; ~93% of scan work), driven by per-chunk `UnifiedVectorFormat` decode on the generic fallback path.

Profile counters show **4,681 / 4,694 chunks** (99.7%) still on generic fallback (`generic_chunks=4681`, `flat_chunks=12`, `dict_chunks=1`) after iterations 2–4 added homogeneous fast paths (all-flat, dict stmt/rfile + flat adsh/line). Those branches require every column to share the same vector category; native SEC-EDGAR storage returns **heterogeneous** layouts per chunk — e.g. dictionary-compressed `adsh`/`line` mixed with flat or dictionary group keys, or CONSTANT vectors within row groups — so strict homogeneity gates left almost all chunks on UnifiedVectorFormat.

**Technique:** Unified mixed-layout consumable fast path — per-column helpers (`IsConsumableVector`, `GetConsumableData`, `GetConsumableIndex`, `ConsumableIndexIsValid`) accept FLAT (nullable), DICTIONARY→FLAT (nullable child), or non-null CONSTANT independently for each projected column; single loop avoids four `ToUnifiedFormat` calls per chunk. Mirrors Q3 `GetFastPathData` / `GetFastPathIndex` extended for nullable flat/dict children.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Added consumable-vector helpers and a mixed-layout branch in `ConsumePreChunk` before the generic fallback
- Existing homogeneous flat/dict branches preserved; generic fallback retained for non-consumable layouts

## Before (baseline gate, iter 4)

| Metric | Value |
|--------|-------|
| reference_median_us | 434,340 |
| accelerator_median_us | 537,970 |
| speedup_median | **0.81×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 856,603 |
| pre_scan_us | 924,980 |
| flat_chunks | 12 |
| dict_chunks | 1 |
| generic_chunks | 4681 |
| qualifying_rows | 9,599,726 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: majority of chunks move from `generic_chunks` → `dict_chunks`; lower `pre_consume_us`; improved `speedup_median` vs 0.81× baseline.

## Playbook signal

- **Technique:** dictionary_over_flat (mixed per-column layouts via consumable helpers)
- **When:** native scan returns heterogeneous vector types across projected columns within the same chunk; homogeneous fast-path gates leave >99% of chunks on UnifiedVectorFormat
- **Retain if:** speedup_median improves; revert otherwise

## Q1 update
# SEC-EDGAR Q1 optimization iteration 1

## Hypothesis

**Target bottleneck:** `pre_consume` (~1,069,430 µs measured; ~50% of scan path), driven by per-chunk `UnifiedVectorFormat` decode overhead.

Profile counters show **9,588,736 / 9,600,799 rows** (99.9%) on the generic fallback path (`generic_fallback_rows=9588736`). The existing dictionary fast path required **all four** projected columns — including high-cardinality `adsh` — to be `DICTIONARY→FLAT`. Native SEC-EDGAR storage delivers low-cardinality `stmt`/`rfile` as dictionary-compressed vectors while `adsh` and `line` remain `FLAT`, so the all-dict branch never fired.

**Technique:** Mixed dictionary-over-flat fast path — `DICTIONARY stmt` + `DICTIONARY rfile` + `FLAT adsh` + `FLAT line` with per-row null checks on dictionary children and flat payload columns.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Added `ConsumePreDictStmtRfileFlatPayload` and a new branch in `ConsumePreChunk` before the all-dict path
- Existing all-flat, all-dict, and generic fallback paths preserved unchanged

## Before (baseline gate, iter 0)

| Metric | Value |
|--------|-------|
| reference_median_us | 428,960 |
| accelerator_median_us | 823,420 |
| speedup_median | **0.52×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 1,069,175 |
| pre_scan_us | 1,112,356 |
| generic_fallback_rows | 9,588,736 |
| pre_rows_scanned | 9,600,799 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: majority of rows move from `generic_fallback_rows` → `dict_fast_path_rows`; lower `pre_consume_us`; improved `speedup_median` vs 0.52× baseline.

## Playbook signal

- **Technique:** dictionary_over_flat (mixed layout: dict group keys + flat payload)
- **When:** low-cardinality group/filter columns are dictionary-compressed but high-cardinality VARCHAR/INT payload columns are FLAT
- **Retain if:** speedup_median improves; revert otherwise before iter 2

## Q1 update
# SEC-EDGAR Q1 optimization iteration 2

## Hypothesis

**Target bottleneck:** `pre_consume` (~1,036,238 µs measured; ~50% of scan path), driven by per-chunk `UnifiedVectorFormat` decode overhead.

Profile counters show **9,586,688 / 9,600,799 rows** (99.9%) on the generic fallback path (`generic_fallback_rows=9586688`). Iteration 1 added a mixed dictionary fast path (`DICTIONARY stmt/rfile` + `FLAT adsh/line`) but almost no rows moved off generic (~2k rows), indicating the dominant layout is **FLAT vectors with nullable validity masks** rather than all-valid flat or dictionary-over-flat.

**Technique:** Flat nullable fast path — when all four projected columns are `FLAT_VECTOR`, use direct `FlatVector` data pointers and consult validity masks only when `!AllValid()`, avoiding four `ToUnifiedFormat` calls per chunk.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Added `IsFlat()` helper; inserted branch in `ConsumePreChunk` after the all-valid flat path
- Added `ConsumePreFlatNullable` with per-row null checks on stmt (with skip counter), rfile, adsh, line
- Existing all-valid flat, dictionary, and generic fallback paths preserved unchanged

## Before (baseline gate, iter 1)

| Metric | Value |
|--------|-------|
| reference_median_us | 432,640 |
| accelerator_median_us | 821,500 |
| speedup_median | **0.53×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 1,036,238 |
| pre_scan_us | 1,083,771 |
| generic_fallback_rows | 9,586,688 |
| pre_rows_scanned | 9,600,799 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: majority of rows move from `generic_fallback_rows` → `flat_fast_path_rows`; lower `pre_consume_us`; improved `speedup_median` vs 0.53× baseline.

## Playbook signal

- **Technique:** flat nullable fast path (minimize nullable branches)
- **When:** native scan returns FLAT columns with partial validity masks; strict `AllValid()` gating leaves almost all chunks on UnifiedVectorFormat
- **Retain if:** speedup_median improves; revert otherwise before iter 3

## Q1 update
# SEC-EDGAR Q1 optimization iteration 1

## Hypothesis

**Target bottleneck:** `pre_consume` (~749,707 µs measured; ~50% of scan path), driven by per-chunk `UnifiedVectorFormat` decode on the generic fallback path.

Profile counters show **4,681 / 4,694 chunks** (99.7%) on generic fallback (`generic_chunks=4681`, `flat_chunks=12`, `dict_chunks=1`) and **9,586,688 / 9,600,799 rows** (99.9%) on `generic_fallback_rows`. Existing homogeneous fast paths require every projected column to share the same vector category (all-flat, dict stmt/rfile + flat adsh/line, or all-dict). Native SEC-EDGAR storage returns **heterogeneous** layouts per chunk — e.g. dictionary-compressed `stmt`/`rfile`/`adsh` mixed with flat `line`, or CONSTANT vectors within row groups — so strict homogeneity gates left almost all chunks on UnifiedVectorFormat.

**Technique:** Unified mixed-layout consumable fast path — per-column helpers (`IsConsumableVector`, `GetConsumableData`, `GetConsumableIndex`, `ConsumableIndexIsValid`) accept FLAT (nullable), DICTIONARY→FLAT (nullable child), or non-null CONSTANT independently for each projected column; single loop avoids four `ToUnifiedFormat` calls per chunk. Mirrors Q3 `GetFastPathData` / `GetFastPathIndex` extended for nullable flat/dict children.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Added consumable-vector helpers and `ConsumePreConsumable` mixed-layout branch in `ConsumePreChunk` before the generic fallback
- Existing homogeneous flat/dict branches preserved; generic fallback retained for non-consumable layouts
- Distinct adsh tracking uses string_t probe (not dictionary index) for cross-chunk correctness

## Before (baseline gate, iter 0)

| Metric | Value |
|--------|-------|
| reference_median_us | 434,296.5 |
| accelerator_median_us | 396,700.0 |
| speedup_median | **1.09×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 749,707 |
| pre_scan_us | 796,919 |
| flat_chunks | 12 |
| dict_chunks | 1 |
| generic_chunks | 4681 |
| generic_fallback_rows | 9,586,688 |
| qualifying_rows | 9,599,726 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: majority of chunks move from `generic_chunks` → `dict_chunks`; lower `pre_consume_us`; improved `speedup_median` vs 1.09× baseline.

## Playbook signal

- **Technique:** dictionary_over_flat (mixed per-column layouts via consumable helpers)
- **When:** native scan returns heterogeneous vector types across projected columns within the same chunk; homogeneous fast-path gates leave >99% of chunks on UnifiedVectorFormat
- **Retain if:** speedup_median improves; revert otherwise before iter 2

## Q1 update
# SEC-EDGAR Q1 optimization iteration 2

## Hypothesis

**Target bottleneck:** `pre_consume` (873,741 µs measured; ~50% of execution time), sub-phase `distinct_adsh_tracking`.

Profile counters show **4,682 / 4,694 chunks** (99.7%) on the mixed dictionary-over-flat path (`dict_chunks=4682`, `flat_chunks=12`). In this layout `stmt`/`rfile` are DICTIONARY→FLAT while `adsh` is **FLAT** (high cardinality, ~379k distinct inserts). The existing all-dict path already uses `InsertIndex` for O(1) per-group distinct tracking, but `ConsumePreDictDictFlatFlat` still calls `AggregateFixed` with `string_t` adsh keys — repeating FNV hash + memcmp in **every group's** `DistinctAdshSet` on every row (~9.6M probes).

**Technique:** Global flat-adsh intern table — hash each unique adsh string once per row into a dense `uint32_t` index, then reuse `AggregateRowDictIndex` / `InsertIndex` for per-group COUNT(DISTINCT adsh). One global probe replaces N per-group string hash probes.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Added `AdshInternTable` (kernel-scoped string→index interning)
- `AggregateFixed` routes all fixed-slot consume paths through `adsh_intern.Intern` + `AggregateRowDictIndex` (unified index space across chunk layouts)
- `DistinctAdshSet::InsertIndex` tracks `index_count` for O(1) `Size()` at finalize

## Before (optimization iter 1 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 436,210 |
| accelerator_median_us | 521,972.5 |
| speedup_median | **0.84×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 873,741 |
| pre_scan_us | 920,714 |
| dict_chunks | 4,682 |
| flat_chunks | 12 |
| generic_chunks | 0 |
| distinct_inserts | 379,461 |
| qualifying_rows | 9,599,726 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: lower `pre_consume_us` and `distinct_insert_ns`; majority of distinct tracking via O(1) index bitmap; improved `speedup_median` vs 0.84× baseline.

## Playbook signal

- **Technique:** dictionary-index distinct tracking extended to flat VARCHAR via global intern table
- **When:** COUNT(DISTINCT high-cardinality VARCHAR) where column is FLAT in storage but same values repeat across many rows/groups
- **Retain if:** speedup_median improves; revert otherwise before iter 3

## Q1 update
# SEC-EDGAR Q1 optimization iteration 3

## Hypothesis

**Target bottleneck:** `pre_consume` (~414,000 µs measured; ~90% of scan-loop time within `pre_scan_us`), on dictionary-encoded chunks (`dict_chunks=4682` / 4694).

The dominant consume paths (`ConsumePreDictDictFlatFlat` for dict stmt/rfile + flat adsh/line, and `ConsumePreDictDict` for all-dict) performed per-row validity checks and lazy stmt/rfile classification even when all dictionary children and flat columns are all-valid — the common case for base-storage row groups. Over ~9.6M rows this adds branch overhead and defers classification until first row touch per dictionary index.

**Technique:** All-valid dictionary consume fast path — when all projected column validity masks are `AllValid()`, pre-classify the full stmt/rfile dictionary once per chunk (≤ few dozen entries), then run a tight row loop with no null checks and shared-sel reuse for stmt/rfile.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Added all-valid branches in `ConsumePreDictDictFlatFlat` and `ConsumePreDictDict`
- Pre-fill stmt/rfile classification caches via `DictionaryVector::DictionarySize` at chunk entry
- Nullable fallback path preserved unchanged for txn-local / nullable vectors

## Before (optimization iter 2 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 432,829 |
| accelerator_median_us | 457,177.5 |
| speedup_median | **0.95×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_scan_us | 459,868 |
| pre_next_us | 41,182 |
| pre_consume_us | 413,943 |
| dict_chunks | 4,682 |
| flat_chunks | 12 |
| generic_chunks | 0 |
| distinct_inserts | 379,461 |
| qualifying_rows | 9,599,726 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: lower `pre_consume_us` on all-valid dictionary chunks; improved `speedup_median` vs 0.95× baseline.

## Playbook signal

- **Technique:** all-valid dictionary consume fast path with chunk-level stmt/rfile pre-classification
- **When:** DICTIONARY→FLAT dominates (`dict_chunks` ≫ `flat_chunks`) and validity masks are all-valid for base storage scans
- **Retain if:** speedup_median improves; revert otherwise before iter 4

## Q1 update
# SEC-EDGAR Q1 optimization iteration 4

## Hypothesis

**Target bottleneck:** `pre_consume` (~414,000 µs within `pre_scan_us`; ~90% of scan-loop time), on all-valid dictionary-encoded chunks (`dict_chunks=4682` / 4694).

Iteration 3 pre-classified stmt/rfile dictionary entries separately but the row loop still performed per-row bounds checks, dual cache lookups, enum comparisons, and `FixedSlotIndex()` on every qualifying row (~9.6M). With rfile cardinality ≤2, a dense **stmt×rfile → slot_id** table lookup eliminates that per-row classification work.

**Technique:** Precomputed slot-pair cache — at chunk entry, fill `slot_pair_cache[stmt_dict_idx][rfile_dict_idx]` once (≤ ~8×2 entries); inner loop becomes two sel-vector index loads + one table lookup + direct aggregate.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Replaced iter-3 all-valid inner loops in `ConsumePreDictDictFlatFlat` and `ConsumePreDictDict` with `slot_pair_cache[STANDARD_VECTOR_SIZE][4]` precomputation
- Nullable / generic fallback paths unchanged

## Before (optimization iter 3 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 431,878.5 |
| accelerator_median_us | 461,972.0 |
| speedup_median | **0.93×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_scan_us | 459,680 |
| pre_next_us | 41,036 |
| pre_consume_us | 413,914 |
| dict_chunks | 4,682 |
| flat_chunks | 12 |
| generic_chunks | 0 |
| distinct_inserts | 379,461 |
| qualifying_rows | 9,599,726 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: lower `pre_consume_us` on all-valid dictionary chunks; improved `speedup_median` vs 0.93× baseline.

## Playbook signal

- **Technique:** stmt×rfile slot-pair cache for all-valid dictionary consume (dense direct index at dictionary grain)
- **When:** DICTIONARY→FLAT dominates and group keys have tiny dictionary cardinality (stmt ≤ dozen, rfile ≤ few)
- **Retain if:** speedup_median improves; revert otherwise before iter 5

## Q1 update
# SEC-EDGAR Q1 optimization iteration 5

## Hypothesis

**Target bottleneck:** `pre_consume` (~416,786 µs; ~90% of `pre_scan_us`), on all-valid dictionary-encoded chunks (`dict_chunks=4682` / 4694).

Iteration 4 replaced per-row stmt/rfile classification with a `slot_pair_cache[STANDARD_VECTOR_SIZE][4]` table (8 KB) zeroed on every chunk. With `STANDARD_VECTOR_SIZE=2048`, that is ~38 MB of `memset` per query plus a nested stmt×rfile precompute loop, while stmt/rfile dictionary cardinalities are tiny (≤8 stmt codes, ≤2 rfile values). The extra cache traffic likely offset the saved classification work (iter 4 regressed `pre_consume_us` from 413,914 → 416,786).

**Technique:** Compact separable dictionary slot cache — precompute `stmt_slot_base[16]` (stmt_class×2) and `rfile_slot_off[4]` (rfile_class) once per chunk; row loop computes `slot_id = stmt_base + rfile_off` with 20-byte memset instead of 8 KB.

## Change (single bounded patch)

- File: `src/generated/sec_edgar/single-threaded/q1_kernel.cpp`
- Added `kMaxStmtDictEntries=16`, `kMaxRfileDictEntries=4`
- Replaced oversized `slot_pair_cache[STANDARD_VECTOR_SIZE][4]` in all-valid paths of `ConsumePreDictDictFlatFlat` and `ConsumePreDictDict` with compact 1D stmt/rfile caches
- Nullable / generic fallback paths unchanged

## Before (optimization iter 4 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 433,263.5 |
| accelerator_median_us | 457,987.5 |
| speedup_median | **0.95×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_scan_us | 462,652 |
| pre_next_us | 41,086 |
| pre_consume_us | 416,786 |
| dict_chunks | 4,682 |
| flat_chunks | 12 |
| generic_chunks | 0 |
| distinct_inserts | 379,461 |
| qualifying_rows | 9,599,726 |

## After

_Pending automatic correctness / baseline / profile gates._

Expected: lower per-chunk cache overhead on dictionary fast path; reduced `pre_consume_us`; improved `speedup_median` vs 0.95× baseline.

## Playbook signal

- **Technique:** compact separable dictionary slot cache (dense direct index at dictionary grain)
- **When:** stmt/rfile dictionary cardinalities are tiny but `STANDARD_VECTOR_SIZE`-sized pair tables cause measurable memset/precompute overhead per chunk
- **Retain if:** speedup_median improves; revert otherwise

## Q1 update
# SEC-EDGAR Q1 optimization iteration 1 (restart)

## Hypothesis

**Target bottlenecks:** `pre_consume_us` (~415 ms; ~90% of scan loop), sub-phase `distinct_adsh_tracking` (~379k inserts).

Prior optimization iterations (1–5) regressed from implementation baseline **1.09×** to **0.94×** by layering complexity (AdshInternTable, chrono in distinct probes, vector resize in InsertIndex). Profile shows **4682/4694 dict chunks** on all-valid dictionary paths — consume CPU dominates.

## Bundle (6 coordinated changes)

1. **Dictionary-index distinct via `unordered_set<uint32_t>`** — Replace byte-vector resize in `DistinctAdshSet::InsertIndex` with pre-reserved hash set; O(1) probe without geometric resize on high-cardinality adsh indices.
2. **Remove AdshInternTable** — Eliminate redundant FNV intern pass on flat-adsh paths; call `Insert(string_t)` directly.
3. **Remove chrono profiling from distinct hot path** — Drop `insert_ns` timing from `Insert()` / `AggregateGeneric`.
4. **uint16_t active_mask for fixed slots** — Replace per-row `slot.active` bool checks with bit-mask activation (16 slots).
5. **Inline all-valid dictionary hot loops** — Fuse slot lookup + cnt/line_sum/distinct update in `ConsumePreDictDict` and `ConsumePreDictDictFlatFlat` inner loops (no helper call per row).
6. **Reorder consume dispatch** — Check all-dict and dict+flat paths before flat paths (matches 99.7% chunk layout histogram).

## Blocked (adapter not writable)

- **Native `stmt IS NOT NULL` filter pushdown** — Requires `IsNotNullFilter` support in `duckdb_scan_adapter`; MCP path not writable this iteration.

## Before (optimization iter 5 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 435,509 |
| accelerator_median_us | 463,295 |
| speedup_median | **0.94×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_scan_us | 460,580 |
| pre_consume_us | 414,773 |
| dict_chunks | 4,682 |
| distinct_inserts | 379,461 |
| qualifying_rows | 9,599,726 |

## Expected impact

- Lower `pre_consume_us` from faster distinct-index probes and inlined dict loops
- Recover toward **≥1.09×** implementation baseline; stretch target remains 2.0× pending scan pushdown

## Playbook signals

- **Retain:** `unordered_set<uint32_t>` for dictionary-index COUNT(DISTINCT) when adsh is DICTIONARY-encoded
- **Anti-pattern confirmed:** AdshInternTable + vector<bool> resize for distinct index tracking on millions of rows
- **Next if still scan-bound:** extend `NativeFilterSpec` with `is_not_null` → `IsNotNullFilter` in scan adapter

## Q1 update
# SEC-EDGAR Q1 optimization iteration 2

## Hypothesis

**Target bottlenecks:** `pre_consume_us` (~460 ms; ~90% of scan loop), sub-phase `distinct_adsh_tracking` (~379k inserts).

Iteration 1 **regressed** to 0.85× (from 1.09× implementation baseline) after replacing `std::unordered_set<std::string>` with a custom FNV bucket-chain `DistinctAdshSet` that still probed string bytes per row on the dominant **dict stmt/rfile + flat adsh** layout (4682/4694 chunks).

## Bundle (7 coordinated changes)

1. **Revert harmful DistinctAdshSet** — Remove custom FNV chain + dual string/index modes; fixed slots use `unordered_set<uint32_t>` only.
2. **Global AdshInternTable** — Transparent `string_view` hash map interns each unique adsh once (~379k); probes on repeat rows avoid allocation.
3. **Dictionary-index distinct for all fixed paths** — Flat-adsh rows intern then `Insert(uint32_t)`; dict-adsh rows use child index directly.
4. **Dense direct-index slots + string_t classification** — Retained: pre-classify stmt/rfile dictionary per chunk, O(1) slot index.
5. **Inlined dict-flat-flat hot loop** — Fused slot lookup + intern + index distinct without per-row helper calls.
6. **Generic overflow isolation** — Unknown stmt/rfile use separate `GenericAggregate` with `unordered_set<string>` (rare path).
7. **Pre-reserved structures** — Intern table (400k), per-slot index sets (512), active-slot list.

## Blocked

- **Native `stmt IS NOT NULL` filter pushdown** — scan adapter not writable via MCP tools.

## Before (optimization iter 1 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 437,215 |
| accelerator_median_us | 517,387 |
| speedup_median | **0.85×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 459,929 |
| pre_scan_us | 507,020 |
| dict_chunks | 4,682 |
| distinct_inserts | 379,461 |

## Expected impact

- Lower `pre_consume_us` from uint32 distinct probes on interned adsh indices
- Recover **≥1.09×** implementation baseline; stretch toward 2.0× pending scan pushdown

## Playbook signals

- **Retain:** global intern + per-group `unordered_set<uint32_t>` when high-cardinality VARCHAR is FLAT in storage
- **Anti-pattern confirmed:** custom FNV DistinctAdshSet with per-row string memcmp on flat adsh
- **Next if scan-bound:** `IsNotNullFilter` in scan adapter for `stmt IS NOT NULL`

## Q1 update
# SEC-EDGAR Q1 optimization iteration 1

## Hypothesis

**Target bottlenecks (implementation baseline profile):** `pre_scan_us` ≈ 671 ms (~95% of wall time), `generic_fallback_rows` ≈ 9.59M/9.60M (99.9% generic), `distinct_inserts` ≈ 379k.

The implementation used `CreateNativeChunkSource` + strict all-flat/all-dict dispatch, forcing nearly every vector through `UnifiedVectorFormat` (`generic_chunks` 4681). Per-row `AdshInternTable` copied full strings; fixed-slot fast paths required all four columns to match before activating.

## Bundle (6 coordinated changes)

1. **Split scan/consume timing** — Track `pre_scan_us` vs `pre_consume_us` separately in `RunFusedPreScan` for gate visibility.
2. **Layout dispatch reorder** — Prefer dictionary stmt/rfile + flat adsh/line (dominant on storage vectors), then all-flat, then generic fallback.
3. **Dictionary decode fast paths** — `ConsumePreDictDictFlatFlat` / `ConsumePreDictDictDictFlat` decode dictionary child indices once per chunk; skip null `stmt` rows (residual filter fairness).
4. **`string_view` AdshInternTable** — Global intern table with FNV hash; copy each unique adsh once (~379k); per-group `unordered_set<uint32_t>` for COUNT(DISTINCT adsh).
5. **Per-chunk adsh dictionary cache** — Intern each dictionary child index once per chunk in `ConsumePreDictDictDictFlat`.
6. **Flat all-valid fixed-slot path** — `ConsumePreFlatFlatFlatFlat` uses direct slot indexing + interned adsh indices without per-row `ResolveGroup` string hashing.

## Blocked / deferred

- **`RunNativeFilteredScanConsumer` + `SelectProjectedColumn`** — Prototype caused incorrect group assignment (13 vs 14 groups; phantom `CI,X`) when consuming projected dictionary vectors through generic UVF or inlined slot caches. Kept `CreateNativeChunkSource` inside `RunFusedPreScan` for correctness; re-enable fused consumer next iteration with Q1-style scalar filtered aggregation.

## Before (implementation gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 433,271 |
| accelerator_median_us | 704,510 |
| speedup_median | **0.61×** |

| Profile counter | Value |
|-----------------|-------|
| pre_scan_us | 670,846 |
| pre_consume_us | 0 (not tracked) |
| generic_rows | 9,585,615 |
| dict_chunks | 1 |
| generic_chunks | 4,681 |
| distinct_inserts | 379,461 |
| active_groups | 14 |

## After (iteration 1 build)

| Metric | Value |
|--------|-------|
| reference_median_us | 433,816 |
| accelerator_median_us | 428,768 |
| speedup_median | **1.01×** |

| Profile counter | Value |
|-----------------|-------|
| pre_scan_us | 48,215 |
| pre_consume_us | 361,806 |
| generic_rows | 3,275,727 |
| dict_chunks | 3,082 |
| generic_chunks | 1,600 |
| distinct_inserts | 379,461 |
| active_groups | 14 |

## Rationale tied to counters

- **`generic_rows` 9.59M → 3.28M:** Dictionary stmt/rfile fast paths now handle ~66% of rows; remaining generic rows are mixed-layout chunks only.
- **`dict_chunks` 1 → 3,082:** Dispatch matches actual storage dictionary encoding instead of falling through on first non-flat column.
- **`pre_scan_us` 671 ms → 48 ms:** Scan loop overhead dropped once consume used decoded dictionary paths (consume moved to `pre_consume_us`).
- **`distinct_inserts` unchanged (379,461):** Correctness preserved; intern table reduced per-row string copy/allocation cost.
- **`active_groups` 14:** Full group coverage maintained after fixing null-stmt skips and ResolveGroup-based dictionary aggregation.

## Playbook signals

- **Retain:** `string_view` global adsh intern + `uint32_t` per-group distinct sets.
- **Retain:** Dictionary stmt/rfile + flat payload fast paths before generic UVF.
- **Next iteration:** Wire `RunNativeFilteredScanConsumer` with Q1 `AggregateScalarFiltered` sel/index pattern; target further `pre_scan_us` reduction and `generic_rows → 0` on dict-dominant layouts.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 2

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 357,137 µs (88% of scan path), `generic_fallback_rows` = 3,275,727 (34.1% of 9.6M rows), `distinct_inserts` = 379,461.

Iteration 1 added dictionary fast paths but gated them on `IsFlatAllValid` for adsh/line and dictionary children. Native SEC-EDGAR storage delivers dict stmt/rfile + flat adsh/line with **nullable validity masks** in ~34% of chunks, forcing UnifiedVectorFormat fallback. The dominant dict path (`ConsumePreDictDictFlatFlat`) also called `ResolveGroup` + `AggregateRow` per row instead of dense fixed-slot indexing + interned adsh indices.

## Bundle (6 coordinated changes)

1. **Relax layout dispatch** — Dict stmt/rfile + flat adsh/line now matches `IsFlat()` (nullable OK), not `IsFlatAllValid()`; captures chunks previously on generic UVF.
2. **`ConsumePreFlatNullable`** — All-flat nullable chunks use direct `FlatVector` pointers with mask-only-when-needed; avoids four `ToUnifiedFormat` calls per chunk.
3. **Compact `DictSlotCaches`** — Pre-classify stmt/rfile dictionary entries once per chunk (`kMaxStmtDictEntries=16`, `kMaxRfileDictEntries=4`); row loop does O(1) slot lookup instead of per-row `ClassifyStmt` + `ResolveGroup`.
4. **Inlined all-valid dict+flat hot loop** — `ConsumePreDictDictFlatFlat` fuses slot lookup + cnt/line_sum + `InsertDistinct(interned_adsh)` without helper calls on the fast path.
5. **Dictionary-index distinct for dict adsh** — `ConsumePreDictDictDictFlat` uses per-chunk adsh intern cache keyed by dictionary child index (not row index); fixed-slot inline aggregation when slot resolves.
6. **Nullable dict consume path** — Same slot caches with conditional validity checks on stmt/rfile/adsh/line; overflow to `ResolveGroup` only for unknown stmt or bad rfile.

## Before (iteration 1 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 434,283.5 |
| accelerator_median_us | 429,118.0 |
| speedup_median | **1.01×** |

| Profile counter | Value |
|-----------------|-------|
| pre_scan_us | 49,041 |
| pre_consume_us | 357,137 |
| generic_rows | 3,275,727 |
| dict_chunks | 3,082 |
| flat_chunks | 12 |
| generic_chunks | 1,600 |
| distinct_inserts | 379,461 |
| active_groups | 14 |

## After (expected)

| Counter | Expected change |
|---------|-----------------|
| `generic_rows` | → near 0 (nullable flat/dict chunks absorbed by relaxed dispatch) |
| `generic_chunks` | → near 0 |
| `dict_chunks` | ↑ (majority of former generic chunks) |
| `pre_consume_us` | ↓ (no UVF decode; O(1) slot + interned distinct) |
| `distinct_inserts` | unchanged (379,461) |
| `active_groups` | unchanged (14) |
| `speedup_median` | ↑ vs 1.01× baseline |

## Rationale tied to counters

- **`generic_rows` 34% → ~0%:** Strict `AllValid()` gating blocked dict+flat and flat-nullable paths; `IsFlat()` + nullable consume eliminates UVF on the dominant storage layouts.
- **`pre_consume_us` ↓:** Per-row `ResolveGroup` string hashing replaced by compact dictionary slot cache + direct `fixed_slots[slot]` access; adsh distinct uses global intern table + `uint32_t` hash sets (no string keys in probe loop).
- **`distinct_inserts` stable:** Semantics unchanged; only the probe key type (interned index) and aggregation entry point changed.

## Playbook signals

- **Retain:** Compact separable dictionary slot cache (tiny dict cardinalities; avoid `STANDARD_VECTOR_SIZE`-sized pair tables).
- **Retain:** `IsFlat()` dispatch relaxation before generic UVF fallback.
- **Next iteration:** If `pre_scan_us` resurfaces, wire `RunNativeFilteredScanConsumer` with scalar filtered aggregation; if consume still dominates, per-chunk flat-adsh intern dedup cache.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 3

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 349,086 µs (88% of scan path), `generic_fallback_rows` = 3,275,727 (34.1% of 9.6M rows), `distinct_inserts` = 379,461.

Iteration 2 improved speedup (1.01× → 1.05×) but **did not reduce generic fallback** — counters unchanged at 1,600 generic chunks / 3.28M rows. Root cause: dispatch still requires **homogeneous** vector categories (all-flat, dict stmt/rfile + flat adsh/line, or all-dict). Native SEC-EDGAR storage returns **heterogeneous** layouts within chunks (dict stmt/rfile + dict line, CONSTANT vectors, flat stmt + dict rfile, etc.), leaving ~34% of chunks on four-column `UnifiedVectorFormat` decode.

## Bundle (6 coordinated changes)

1. **Per-column consumable vector helpers** — `IsConsumableVector`, `GetConsumableData`, `GetConsumableIndex`, `ConsumableIndexIsValid` accept FLAT (nullable), DICTIONARY→FLAT (nullable child), or non-null CONSTANT independently per column (Q3 pattern extended).
2. **`ConsumePreConsumable` mixed-layout path** — single row loop using per-column helpers; dispatches after homogeneous fast paths, before generic UVF. Handles dict line, constant columns, and arbitrary FLAT/DICT/CONST mixes.
3. **`TryResolveFixedSlotMixed`** — dense O(1) slot lookup via `DictSlotCaches` when stmt+rfile are dict; inline `ClassifyStmt` + `FixedSlotIndex` when either column is flat/constant.
4. **`AdshDictInternCache::Precompute`** — intern all dictionary child adsh strings once per chunk (≤2048 entries); row loop uses O(1) dict-index → interned-id lookup instead of per-row string hash.
5. **`FlatAdshLastHit` 1-entry cache** — consecutive rows sharing the same filing adsh skip repeated global intern probes (dominant in dict+flat and flat paths).
6. **Refactored dict-adsh consume** — `ConsumePreDictDictDictFlat` uses shared `AdshDictInternCache`; `ConsumePreDictDictFlatFlat` uses `FlatAdshLastHit` on hot paths.

## Before (iteration 2 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 434,109.5 |
| accelerator_median_us | 414,701.0 |
| speedup_median | **1.05×** |

| Profile counter | Value |
|-----------------|-------|
| pre_scan_us | 47,716 |
| pre_consume_us | 349,086 |
| generic_rows | 3,275,727 |
| dict_chunks | 3,082 |
| flat_chunks | 12 |
| generic_chunks | 1,600 |
| distinct_inserts | 379,461 |
| active_groups | 14 |

## After (expected)

| Counter | Expected change |
|---------|-----------------|
| `generic_rows` | → near 0 (heterogeneous FLAT/DICT/CONST chunks absorbed by consumable path) |
| `generic_chunks` | → near 0 |
| `dict_chunks` | ↑ (former generic chunks tracked as dict-path consumable) |
| `pre_consume_us` | ↓ (no UVF decode; dict adsh pre-intern; flat adsh last-hit) |
| `distinct_inserts` | unchanged (379,461) |
| `active_groups` | unchanged (14) |
| `speedup_median` | ↑ vs 1.05× baseline |

## Rationale tied to counters

- **`generic_rows` 34% → ~0%:** Homogeneous dispatch missed mixed-layout chunks (e.g. dict line, constant stmt/rfile within row groups). Per-column consumable helpers eliminate the UVF fallback for all FLAT/DICT/CONST combinations.
- **`pre_consume_us` ↓:** Dict adsh intern moved from per-row to per-dict-entry precompute; flat adsh consecutive-row cache cuts redundant global intern probes; mixed path retains dense slot indexing + `uint32_t` distinct sets.
- **`distinct_inserts` stable:** Global `AdshInternTable` preserves cross-chunk distinct semantics; dict cache only accelerates lookup.

## Playbook signals

- **Retain:** Per-column consumable helpers before generic UVF when `generic_chunks` > 10% with high `dict_chunks`.
- **Retain:** Dict adsh pre-intern cache + flat adsh last-hit for `distinct_inserts`-heavy workloads.
- **Next iteration:** If `pre_scan_us` resurfaces, wire `RunNativeFilteredScanConsumer` with scalar filtered sel; if consume still dominates after generic→0, SIMD-ify slot+aggregate inner loop.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 4

## Optimization bundle (6 coordinated changes)

1. **Bitmap distinct tracking (`DistinctAdshTracker`)** — Replace per-group `unordered_set<uint32_t>` with a lazy-growing bitset keyed by global adsh intern index. Targets `distinct_inserts=379461` (4% of rows) and hash-probe overhead on ~9.6M aggregate updates.
2. **Deferred fixed-slot metadata** — Drop `ToStoredString` / `ActivateFixedSlot` string copies on the hot path; reconstruct `(stmt, rfile)` from dense `slot_index` at finalize only (~14 active groups).
3. **Stable global adsh intern table** — Fix dangling `string_view` keys; FNV hash + multimap collision check; one intern probe per unique adsh string (not per group).
4. **Chunk-local adsh cache line** — Single-entry consecutive-row cache in the all-valid `ConsumePreDictDictFlatFlat` path (dominant layout: dict stmt/rfile + flat adsh).
5. **Chunk-level dict adsh pre-intern** — Pre-intern all adsh dictionary child entries once per chunk in `ConsumePreDictDictDictFlat` instead of per-row lazy cache with validity branches.
6. **Preserved dispatch (no iter-3 regression)** — Did **not** expand dict routing or eliminate the generic fallback path that regressed iter 3 (0.63× vs 1.05× best).

## Before (restored best kernel, iter 2 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 434,109.5 |
| accelerator_median_us | 414,701.0 |
| speedup_median | **1.05×** |

| Profile phase / counter | Value |
|-------------------------|-------|
| pre_consume_us | 349,086 |
| pre_scan_us | 47,716 |
| dict_chunks | 3,082 |
| generic_chunks | 1,600 |
| distinct_inserts | 379,461 |
| qualifying_rows | 9,599,726 |

## Expected after

- Lower `pre_consume_us` via O(1) bitmap distinct inserts vs hash-set probes.
- No string allocation on fixed-slot first touch during scan.
- Fewer adsh intern probes on repeated consecutive flat adsh values.
- Correctness preserved via global intern index space (not raw per-chunk dict indices for flat-adsh layout).

## Playbook signal

- **Technique:** bitmap distinct + deferred slot labels + global adsh intern on dict-dict-flat-flat
- **When:** `pre_consume_us` dominates; `distinct_inserts` ≪ `qualifying_rows`; fixed-slot group count ≪ row count
- **Anti-pattern avoided:** forcing all chunks onto heavy dict paths (iter 3: generic_rows→0 but 0.63×)

## Q1 update
# SEC-EDGAR Q1 optimization iteration 5

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 363,375 µs (89% of scan path), `generic_fallback_rows` = 3,275,727 (34.1% of 9.6M rows), `distinct_inserts` = 379,461.

Iterations 3–4 showed that a monolithic consumable path (iter 3 → 0.63×) or consume-only micro-opts without layout coverage (iter 4 → 1.04×) fail to beat the 1.05× best snapshot. Root cause unchanged: homogeneous dispatch (dict-dict-flat-flat / dict-dict-dict-flat / all-flat) misses **heterogeneous** native layouts — especially **dictionary-encoded line**, dict-adsh + dict-line combos, and FLAT/DICT/CONST mixes — leaving ~1,600 chunks on four-column `UnifiedVectorFormat` decode.

## Bundle (6 coordinated changes)

1. **`ConsumePreDictDictFlatDict`** — dict stmt/rfile + flat adsh + **dict line**; avoids UVF when line is dictionary-compressed (major generic-chunk pattern).
2. **`ConsumePreDictDictDictDict`** — all four columns dictionary-over-flat; full dict-index distinct for adsh + dict line lookup.
3. **`ConsumePreConsumableMixed`** — lightweight per-column helpers (`IsConsumableVector`, `GetConsumableData`, `GetConsumableIndex`, `ConsumableIndexIsValid`) handle remaining FLAT/DICT/CONST mixes **after** specialized paths, **before** UVF generic (Q3 pattern, no iter-3 monolithic replacement of fast paths).
4. **`AdshDictInternCache`** — pre-intern all adsh dictionary child entries once per chunk (≤2048); row loop uses O(1) dict-index → interned-id lookup.
5. **`FlatAdshLastHit`** — 1-entry consecutive-row cache for flat adsh in dict-dict-flat-flat and flat paths; cuts redundant global intern probes on repeated filings.
6. **Deferred fixed-slot metadata** — `TouchFixedSlot` + `AggregateFixedSlot` on hot path; decode `(stmt, rfile)` from dense slot index at finalize only (~14 active groups).

## Before (restored best kernel, iter 2 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 434,109.5 |
| accelerator_median_us | 414,701.0 |
| speedup_median | **1.05×** |

| Profile counter | Value |
|-----------------|-------|
| pre_scan_us | 47,716 |
| pre_consume_us | 349,086 |
| generic_rows | 3,275,727 |
| dict_chunks | 3,082 |
| flat_chunks | 12 |
| generic_chunks | 1,600 |
| distinct_inserts | 379,461 |
| active_groups | 14 |

## Expected after

| Counter | Expected change |
|---------|-----------------|
| `generic_rows` | → near 0 (dict-line + all-dict + mixed consumable absorb former UVF chunks) |
| `generic_chunks` | → near 0 |
| `dict_chunks` | ↑ (former generic chunks) |
| `pre_consume_us` | ↓ (no UVF decode; dict adsh pre-intern; flat adsh last-hit; deferred slot strings) |
| `distinct_inserts` | unchanged (379,461) |
| `active_groups` | unchanged (14) |
| `speedup_median` | ↑ vs 1.05× baseline |

## Rationale tied to counters

- **`generic_rows` 34% → ~0%:** Added dict-line and all-dict specialized paths plus consumable mixed fallback for FLAT/DICT/CONST per-column combinations missed by homogeneous dispatch.
- **`pre_consume_us` ↓:** Eliminated four `ToUnifiedFormat` calls per generic chunk; dict adsh distinct uses pre-interned child indices; fixed-slot hot loop avoids `ToStoredString` + `ActivateFixedSlot` on every first touch.
- **`distinct_inserts` stable:** Global `AdshInternTable` + `uint32_t` hash sets unchanged semantically; dict cache only accelerates lookup.

## Playbook signals

- **Retain:** Specialized homogeneous paths first; consumable mixed only as penultimate fallback before UVF.
- **Retain:** Dict adsh pre-intern cache + flat adsh last-hit for distinct-heavy workloads.
- **Retain:** Deferred slot label decode at finalize when group count ≪ row count.
- **Anti-pattern avoided:** Replacing all dict fast paths with a single consumable loop (iter 3 regression).

## Q1 update
# SEC-EDGAR Q1 optimization iteration 1, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 349,086 µs (88% of scan path), `generic_fallback_rows` = 3,275,727 (34.1%), `distinct_inserts` = 379,461.

Prior iteration-5 bundle regressed to 0.75× by combining layout paths with a monolithic consumable mixed fallback and hot-path churn. This episode applies a **surgical bundle**: specialized dict-line/all-dict paths to cut UVF generic rows, plus consume micro-opts on the dominant dict stmt/rfile + flat adsh path only.

## Bundle (5 coordinated changes)

1. **`ConsumePreDictDictFlatDict`** — dict stmt/rfile + flat adsh + dict line; avoids four-column UVF when line is dictionary-encoded.
2. **`ConsumePreDictDictDictDict`** — all four columns dictionary-over-flat; dict-index distinct for adsh + dict line lookup.
3. **2D `slot_pair` lookup in `DictSlotCaches`** — precompute stmt×rfile → fixed slot once per chunk (16×4 table); O(1) `LookupSlot` in all-valid dict+flat hot loop.
4. **`FlatAdshLastHit` + `InternFlatAdsh`** — 1-entry consecutive-row cache for flat adsh intern probes on all flat-adsh consume paths.
5. **Dispatch extension** — route dict-line and all-dict layouts before generic UVF; existing homogeneous paths unchanged.

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | generic_rows | Verdict |
|-------|------------|----------------|----------------|--------------|---------|
| `ep1_consume_bundle` | Full bundle + batch adsh pre-intern on all dict paths | **0.82×** | 464,889 | 3,158,016 | **Discard** — hot-path regression |
| `ep1_layout_paths` | Layout paths + slot lookup + FlatAdshLastHit (final) | **1.05×** | 346,684 | 3,158,016 | **Keep** |

## Before (restored best kernel, iter 2 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 434,109.5 |
| accelerator_median_us | 414,701.0 |
| speedup_median | **1.05×** |

| Profile counter | Value |
|-----------------|-------|
| pre_scan_us | 47,716 |
| pre_consume_us | 349,086 |
| generic_rows | 3,275,727 |
| dict_chunks | 3,082 |
| flat_chunks | 12 |
| generic_chunks | 1,600 |
| distinct_inserts | 379,461 |
| active_groups | 14 |

## After (kept trial `ep1_layout_paths`)

| Metric | Value | Δ |
|--------|-------|---|
| reference_median_us | 437,294 | — |
| accelerator_median_us | 416,533 | — |
| speedup_median | **1.05×** | = best |

| Profile counter | Value | Δ vs best |
|-----------------|-------|-----------|
| pre_scan_us | 47,991 | +275 µs |
| pre_consume_us | 346,684 | **−2,402 µs** |
| generic_rows | 3,158,016 | **−117,711** |
| dict_chunks | 3,140 | +58 |
| generic_chunks | 1,542 | −58 |
| distinct_inserts | 379,461 | 0 |
| active_groups | 14 | 0 |

## Rationale tied to counters

- **`generic_rows` ↓ 117k:** Dict-line and all-dict specialized paths absorb former UVF chunks without a monolithic consumable fallback (iter-3/5 anti-pattern avoided).
- **`pre_consume_us` ↓ 2.4 ms:** Dominant all-valid dict+flat loop uses precomputed `slot_pair` lookup + `FlatAdshLastHit` to skip redundant adsh string hash probes on consecutive same-filing rows.
- **`distinct_inserts` / `active_groups` stable:** Semantics unchanged; intern table + per-group `unordered_set<uint32_t>` preserved.

## Playbook signals

- **Retain:** Targeted layout paths (dict-line, all-dict) before generic UVF — not a single mixed consumable loop.
- **Retain:** 16×4 `slot_pair` precompute (not `STANDARD_VECTOR_SIZE`-sized tables).
- **Retain:** `FlatAdshLastHit` on flat adsh columns with high repeat rate within chunks.
- **Anti-pattern confirmed:** Batch adsh dict pre-intern + LookupSlot on every dict path together caused 0.82× regression; apply micro-opts only on the measured hot path.
- **Next episode:** Further reduce remaining 1,542 generic chunks (mixed FLAT/DICT/CONST per-column) with Q3-style per-column consumable helpers as penultimate fallback — not replacing existing fast paths.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 2, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 349,086 µs (88% of scan path), `generic_rows` = 3,158,016 (32.9%), `distinct_inserts` = 379,461.

Remaining ~1,542 generic chunks use heterogeneous per-column layouts (dict stmt/rfile + dict line, FLAT/DICT/CONST mixes) not covered by homogeneous fast paths. Iteration-3/5 anti-pattern: monolithic consumable loop replacing specialized paths regressed to 0.63–0.82×.

**Technique:** Layered bundle — (1) targeted dict-line / all-dict layout paths, then (2) per-column consumable helpers as penultimate fallback before UVF generic, preserving existing homogeneous fast paths.

## Bundle (5 coordinated changes — kept)

1. **`ConsumePreDictDictFlatDict`** — dict stmt/rfile + flat adsh + dict line.
2. **`ConsumePreDictDictDictDict`** — all four columns dictionary-over-flat.
3. **Per-column consumable helpers** — `IsConsumableVector`, `BindConsumableString/Int`, `ConsumableRowIndex`, `ConsumableEntryValid` (FLAT nullable, DICT→FLAT, non-null CONSTANT per column).
4. **`ConsumePreConsumableMixed`** — single row loop for remaining heterogeneous chunks; dict adsh intern cache; dense slot indexing when stmt+rfile are dict or classifiable flat.
5. **Dispatch extension** — route dict-line, all-dict, then consumable-mixed before `ConsumePreGeneric`.

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | generic_rows | Verdict |
|-------|------------|----------------|----------------|--------------|---------|
| `ep1_layout_paths` | Layout paths + slot_pair + FlatAdshLastHit | **1.01×** | 373,665 | 3,158,016 | **Discard** — consume regression |
| `ep1_layout_only` | Dict-line + all-dict paths only | **1.03×** | 359,852 | 3,158,016 | **Keep** (intermediate base) |
| `ep1_flat_adsh_hit` | FlatAdshLastHit on dict-dict-flat-flat hot loop | **1.01×** | 361,916 | 3,158,016 | **Discard** — no gain vs layout-only |
| `ep1_consumable_mixed` | Consumable mixed fallback on layout-only base | **1.24×** | 290,527 | **0** | **Keep** |

## Before (restored best kernel, iter 2 gate)

| Metric | Value |
|--------|-------|
| reference_median_us | 434,109.5 |
| accelerator_median_us | 414,701.0 |
| speedup_median | **1.05×** |

| Profile counter | Value |
|-----------------|-------|
| pre_scan_us | 47,716 |
| pre_consume_us | 349,086 |
| generic_rows | 3,158,016 |
| dict_chunks | 3,140 |
| generic_chunks | 1,542 |
| distinct_inserts | 379,461 |
| active_groups | 14 |

## After (kept trial `ep1_consumable_mixed`)

| Metric | Value | Δ |
|--------|-------|---|
| reference_median_us | 441,668 | — |
| accelerator_median_us | 356,130 | — |
| speedup_median | **1.24×** | **+0.19× vs best** |

| Profile counter | Value | Δ vs best |
|-----------------|-------|-----------|
| pre_scan_us | 47,951 | +235 µs |
| pre_consume_us | 290,527 | **−58,559 µs** |
| generic_rows | **0** | **−3,158,016** |
| dict_chunks | 4,682 | +1,542 (absorbed generic) |
| generic_chunks | **0** | **−1,542** |
| distinct_inserts | 379,461 | 0 |
| active_groups | 14 | 0 |

## Rationale tied to counters

- **`generic_rows` → 0:** Per-column consumable path handles heterogeneous FLAT/DICT/CONST combinations without four-column `UnifiedVectorFormat` decode; specialized dict-line/all-dict paths cover common sub-patterns first.
- **`pre_consume_us` ↓ 58.6 ms:** Eliminating UVF on 3.16M rows dominates; O(1) dict adsh intern cache + fixed-slot aggregation preserved on hot paths.
- **`distinct_inserts` / `active_groups` stable:** Global `AdshInternTable` + per-group `unordered_set<uint32_t>` semantics unchanged.

## Playbook signals

- **Retain:** Layered dispatch (specialized layout paths → consumable mixed → UVF generic); do not replace specialized paths with a single monolithic loop.
- **Retain:** Per-column consumable helpers when `generic_chunks` > 10% with heterogeneous layouts.
- **Anti-pattern confirmed:** `slot_pair` precompute + `FlatAdshLastHit` together on all dict paths caused 1.01× regression; consumable mixed path alone delivered the win.
- **Next episode:** If consume still dominates at generic_rows=0, micro-optimize inner aggregate loop (deferred slot metadata, bitmap distinct) — not layout dispatch.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 3, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 288,388 µs (dominant phase), `distinct_inserts` = 379,461, `dict_chunks` = 4,682 (generic_rows already 0 from iter 2).

With layout dispatch solved, consume CPU is dominated by per-row intern + distinct insert in dictionary-heavy paths. Episode tests three bundles: bitset distinct, chunk pre-intern, and all-valid fast paths.

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | Verdict |
|-------|------------|----------------|----------------|---------|
| `ep1_distinct_bitset` | Pre-sized `DistinctIndexBitset` + chunk pre-intern + all-valid paths | **1.08×** | 350,777 | **Discard** — bitset memory bandwidth regressed vs hash set |
| `ep1_preintern_fastpath` | `PrecomputeAdshInternCache` (intern all dict entries upfront) + all-valid paths | **0.82×** | 465,566 | **Discard** — interns unused dict slots; 2× consume regression |
| `ep1_flat_adsh_hit` | All-valid fast paths on `DictDictDict*` + larger distinct-set reserve | **1.20×** | 292,906 | **Keep** |

## Before (best snapshot, iter 2 gate)

| Metric | Value |
|--------|-------|
| speedup_median | **1.21×** |
| pre_consume_us | 288,388 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep1_flat_adsh_hit`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **1.20×** | −0.01× (within quick-bench variance) |
| pre_consume_us | 292,906 | +4,518 µs |
| distinct_inserts | 379,461 | 0 |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (3 coordinated changes)

1. **`ConsumePreDictDictDictFlat` all-valid fast path** — Skip per-row validity branches when stmt/rfile/adsh/line dictionary children are all-valid; retain lazy per-chunk adsh dict intern cache.
2. **`ConsumePreDictDictDictDict` all-valid fast path** — Same pattern for the all-dictionary layout (dict line index path).
3. **Larger per-group distinct reserve** — `reserve_hint = row_estimate / 15` (min 8192) to cut rehash on ~379k global adsh intern indices spread across ~14 groups.

## Anti-patterns confirmed

- **Pre-sized bitset distinct tracking** — 47 KB+ per group caused cache misses; `unordered_set<uint32_t>` remains faster for sparse intern indices.
- **Chunk-level pre-intern of all dict entries** — DuckDB dictionary vectors often contain slots not referenced by row sel; eager intern is pure overhead.
- **FlatAdshLastHit** (tested within `ep1_flat_adsh_hit`, removed before keep) — No measurable gain on top of all-valid paths alone (consistent with iter 2 ep1 finding).

## Playbook signals

- **Retain:** All-valid branch duplication in specialized dict paths when validity checks dominate nullable fallback loops.
- **Retain:** Lazy adsh dict intern cache (per-row fill on first sighting) — never pre-fill entire dictionary.
- **Next episode:** If consume still flat, target `ConsumePreDictDictFlatFlat` (flat adsh hot path — per-row `AdshInternTable` string hash) or fused-scan `SelectProjectedColumn` overhead (`pre_scan_us`).

## Q1 update
# SEC-EDGAR Q1 optimization iteration 4, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 295,174 µs (dominant phase), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

With layout dispatch solved (`generic_rows=0`), consume CPU is dominated by per-row distinct-set hash probes. SEC-EDGAR presentation rows repeat the same `adsh` across many consecutive lines within a `(stmt, rfile)` group — most `unordered_set` probes are duplicate checks, not new inserts.

**Technique:** Layered bundle — (1) per-group consecutive adsh dedup before hash-set probe, (2) all-valid fast path for remaining dict-line layout, discard flat-adsh chunk cache (regressed).

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | Verdict |
|-------|------------|----------------|----------------|---------|
| `ep1_flat_adsh_chunk_cache` | Fingerprint intern table + per-chunk flat-adsh cache + all-valid DictDictFlatDict | **1.04×** | 342,084 | **Discard** — extra fingerprint/hash work on every intern call |
| `ep1_consecutive_distinct` | Per-group consecutive adsh skip + all-valid DictDictFlatDict | **1.50×** | 219,055 | **Keep** |

## Before (best snapshot, iter 3 gate)

| Metric | Value |
|--------|-------|
| speedup_median | **1.21×** |
| pre_consume_us | 295,174 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep1_consecutive_distinct`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **1.50×** | **+0.29×** |
| pre_consume_us | 219,055 | **−76,119 µs** |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (3 coordinated changes)

1. **Per-group consecutive adsh dedup** — `GroupAggregate::last_distinct_adsh` skips `unordered_set` probe when the current row repeats the prior row's interned adsh (O(1) compare vs hash probe). Correct for COUNT(DISTINCT): non-consecutive repeats still hit the set on first sighting after a different adsh.
2. **`ConsumePreDictDictFlatDict` all-valid fast path** — Skip per-row validity branches when stmt/rfile dictionary children and flat adsh + dict line are all-valid.
3. **InsertDistinct fast path** — Consecutive check hoisted before set insert; profile counter only on successful new distinct.

## Anti-patterns confirmed

- **FlatAdshChunkCache + fingerprint intern table** — Added hash work on every row (including dict-adsh paths); 1.04× regression despite targeting flat-adsh layout.
- **Chunk-level pre-intern / FlatAdshLastHit** — Still no gain (reconfirmed from iter 2–3).

## Playbook signals

- **Retain:** Per-group consecutive dedup for high-cardinality DISTINCT aggregates where rows cluster by key (filings → many line items).
- **Retain:** All-valid branch duplication in specialized dict paths when validity checks dominate nullable fallback loops.
- **Next episode:** `pre_scan_us` (~48 ms) is now relatively larger share; consider fused-scan projection overhead or further consume micro-opts (active-slot bitset, deferred overflow metadata).

## Q1 update
# SEC-EDGAR Q1 optimization iteration 5, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 231,623 µs (dominant phase), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

With consecutive adsh dedup already in place (1.51× best), remaining consume CPU is dominated by `unordered_set<uint32_t>` hash probes on every non-consecutive distinct insert and repeated `AdshInternTable` lookups on flat-adsh rows.

**Technique:** Layered bundle — (1) per-group bitmap distinct tracking (O(1) bit test/set vs hash probe), (2) scan-level consecutive `string_t` intern cache, (3) remove redundant chunk cache memset.

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | Verdict |
|-------|------------|----------------|----------------|---------|
| `ep5_bitmap_eager_adsh` | Bitmap distinct sets + InternAdsh row cache + lazy bitmap presize | **1.91×** | 194,311 | **Keep** |

## Before (best snapshot, iter 4 gate)

| Metric | Value |
|--------|-------|
| speedup_median | **1.51×** |
| pre_consume_us | 231,623 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep5_bitmap_eager_adsh`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **1.91×** | **+0.40×** |
| pre_consume_us | 194,311 | **−37,312 µs** |
| pre_scan_us | 50,016 | +1,961 µs (noise) |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (5 coordinated changes)

1. **`DistinctAdshBitmap`** — Replace `std::unordered_set<uint32_t>` with lazy-allocated uint64 bitsets per group; O(1) test/set vs hash probe on ~379k distinct inserts.
2. **Lazy bitmap allocation** — Empty bitmap for inactive fixed slots (1280 slots); `Presize(reserve_hint)` only on first activation.
3. **`State::InternAdsh`** — Scan-level consecutive `string_t` pointer+size cache skips redundant `AdshInternTable` hash probes when the same adsh string repeats across consecutive rows (presentation line clustering).
4. **Route all consume paths through `InternAdsh`** — Dict lazy per-chunk cache retained; flat and dict miss paths benefit from row cache.
5. **Remove `adsh_intern_cache.fill(max)`** — Drop 2048-element memset per dict-adsh chunk (validity array sufficient).

## Anti-patterns confirmed (reconfirmed)

- **Eager `PrecomputeAdshInternCache` (intern all dict entries upfront)** — Still regresses (0.82× in prior iter); not included in this bundle.

## Playbook signals

- **Retain:** Bitmap distinct tracking for COUNT(DISTINCT) when intern indices are dense uint32 (cheaper constant factor than `unordered_set`).
- **Retain:** Consecutive `string_t` intern cache at State level (distinct from per-chunk fingerprint cache that regressed).
- **Next episode:** `pre_scan_us` (~50 ms) is now ~20% of scan loop; consider fused-scan projection overhead or stmt filter pushdown in scan adapter.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 1, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 178,193 µs (dominant phase), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 1.90× best, remaining consume CPU is dominated by per-row `AdshInternTable` hash probes on flat-adsh dict paths (stmt/rfile dictionary, adsh flat) and repeated stmt×rfile slot classification in the hot loop.

**Technique:** Coordinated bundle — (1) precomputed stmt×rfile slot matrix, (2) two-pass chunk flat-adsh intern dedup, (3) presized bitmap insert fast path.

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | Verdict |
|-------|------------|----------------|----------------|---------|
| `consume_bundle_ep1` | Fingerprint intern + chunk hash cache + eager bitmap presize all slots | 1.71× | 197,376 | **Discard** — fingerprint overhead + 1280-slot eager presize polluted cache |
| `slot_matrix_ep1` | Slot matrix + two-pass flat-adsh chunk intern + presized bitmap Insert | **1.91×** | 176,714 | **Keep** |

## Before (best snapshot)

| Metric | Value |
|--------|-------|
| speedup_median | **1.90×** |
| pre_consume_us | 178,193 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `slot_matrix_ep1`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **1.91×** | **+0.01×** |
| pre_consume_us | 176,714 | **−1,479 µs** |
| pre_scan_us | 46,735 | −2,261 µs |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (5 coordinated changes)

1. **`DictSlotCaches::slot_matrix`** — Precompute full stmt×rfile fixed-slot index (64 entries max) once per chunk; `LookupFixedSlot` replaces runtime base+char add in hot loop.
2. **Two-pass flat-adsh chunk intern** — Pass 1 deduplicates flat `adsh` strings within chunk via `chunk_adsh_unique` map; pass 2 aggregates using `chunk_row_adsh_intern[]` (DictDictFlatFlat, DictDictFlatDict, FlatFlatFlatFlat all-valid paths).
3. **`State::BeginFlatAdshChunk` / `InternFlatAdshChunkRow`** — Reusable scratch buffers sized from chunk row count; map reserved to 512 buckets to avoid rehash.
4. **`DistinctAdshBitmap::Insert`** — After `Presize(reserve_hint)` on slot activation, use `insert_presized` (no bounds check on hot path) with fallback to grow-on-overflow.
5. **Retained prior wins** — Consecutive `last_distinct_adsh` skip, dict-adsh per-chunk intern cache, bitmap distinct tracking.

## Anti-patterns confirmed

- **Fingerprint-only chunk cache without content verify on flat paths** — Adds hash work per row; regressed to 1.71–1.76×.
- **Eager presize all 1280 fixed-slot bitmaps at bind** — Allocates ~MB of cold bitmap storage; regressed pre_consume.

## Playbook signals

- **Retain:** Two-pass chunk intern for flat VARCHAR columns with high within-chunk repetition (presentation lines per filing).
- **Retain:** Layout-driven slot matrix (16 stmt × 4 rfile dict entries) — not tuned to profile row counts.
- **Next episode:** `pre_scan_us` (~47 ms) is now ~21% of total; fused scan projection or filter pushdown may yield further gains.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 2, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 178,895 µs (dominant phase), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 1.90× best, remaining consume CPU is dominated by per-row `AdshInternTable` hash probes on flat-adsh dict paths (stmt/rfile dictionary, adsh flat) and repeated stmt×rfile slot classification in the hot loop.

**Technique:** Coordinated bundle — (1) precomputed stmt×rfile slot matrix, (2) two-pass chunk flat-adsh intern dedup, (3) presized bitmap insert fast path.

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | Verdict |
|-------|------------|----------------|----------------|---------|
| `iter2_ep1_consume_bundle` | Slot matrix + two-pass flat-adsh chunk intern + presized bitmap insert | **1.91×** | 178,702 | **Keep** |

## Before (best snapshot)

| Metric | Value |
|--------|-------|
| speedup_median | **1.90×** |
| pre_consume_us | 178,895 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `iter2_ep1_consume_bundle`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **1.91×** | **+0.01×** |
| pre_consume_us | 178,702 | **−193 µs** |
| pre_scan_us | 46,297 | −438 µs |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (5 coordinated changes)

1. **`DictSlotCaches::slot_matrix`** — Precompute full stmt×rfile fixed-slot index (64 entries max) once per chunk; `LookupFixedSlot` replaces runtime base+char add in hot loop.
2. **Two-pass flat-adsh chunk intern** — Pass 1 deduplicates flat `adsh` strings within chunk via `chunk_adsh_unique` map; pass 2 aggregates using `chunk_row_adsh_intern[]` (DictDictFlatFlat and FlatFlatFlatFlat all-valid paths).
3. **`State::BuildFlatAdshChunkInterns`** — Reusable scratch buffers sized from chunk row count; map reserved to 512 buckets to avoid rehash.
4. **`DistinctAdshBitmap::insert_presized`** — After `Presize(reserve_hint)` on slot activation, hot-path `insert()` delegates to bounds-checked presized insert without vector resize.
5. **Retained prior wins** — Consecutive `last_distinct_adsh` skip, dict-adsh per-chunk intern cache, bitmap distinct tracking.

## Profile counter deltas

| Counter | Before | After | Δ |
|---------|--------|-------|---|
| pre_consume_us | 178,895 | 178,702 | −193 µs |
| pre_scan_us | ~48,996 | 46,297 | −2,699 µs |
| distinct_inserts | 379,461 | 379,461 | 0 |
| dict_chunks | 4,682 | 4,682 | 0 |

## Playbook signals

- **Retain:** Two-pass chunk intern for flat VARCHAR columns with high within-chunk repetition (presentation lines per filing).
- **Retain:** Layout-driven slot matrix (16 stmt × 4 rfile dict entries) — not tuned to profile row counts.
- **Next episode:** `pre_scan_us` (~46 ms) is now ~20% of total; fused scan projection or filter pushdown may yield further gains.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 3, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 179,225 µs (dominant phase), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 1.90× best, remaining consume CPU is dominated by per-row `AdshInternTable` string_view hash probes and repeated stmt×rfile slot arithmetic in dict-heavy paths.

**Technique:** Coordinated bundle — (1) precomputed stmt×rfile slot matrix, (2) presized bitmap distinct insert, (3) fixed-width `AdshKey` fingerprint intern table (24-byte layout-driven, not profile-tuned).

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | distinct_inserts | Verdict |
|-------|------------|----------------|----------------|------------------|---------|
| `iter3_ep1_consume_micro` | Slot matrix + generation-stamp adsh cache + insert_presized + inlined aggregate helpers | **1.82×** | 180,913 | 379,461 | **Discard** — generation-stamp cache + helper indirection regressed vs baseline |
| `iter3_ep1_slot_presized_fastintern` | Slot matrix + insert_presized + 24-byte AdshKey intern | **2.10×** | 153,690 | 379,461 | **Keep** |

## Before (best snapshot)

| Metric | Value |
|--------|-------|
| speedup_median | **1.90×** |
| pre_consume_us | 179,225 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `iter3_ep1_slot_presized_fastintern`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **2.10×** | **+0.20×** |
| pre_consume_us | 153,690 | **−25,535 µs** |
| pre_scan_us | 46,760 | ~flat |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (4 coordinated changes)

1. **`DictSlotCaches::slot_matrix`** — Precompute full stmt×rfile fixed-slot index (64 entries max) once per chunk; `LookupFixedSlot` replaces per-row base+char add.
2. **`DistinctAdshBitmap::insert_presized`** — After `Presize(reserve_hint)` on slot activation, hot-path distinct insert skips vector resize checks.
3. **`AdshKey` fingerprint intern table** — Pack adsh VARCHAR into three uint64 words (≤24 bytes); O(1) hash/equality vs byte-loop `string_view` FNV. Layout-driven width, not tied to SF3 row counts.
4. **Retained prior wins** — Consecutive `last_distinct_adsh` skip, dict-adsh per-chunk lazy intern cache, bitmap distinct tracking, all-valid dict fast paths.

## Profile counter deltas

| Counter | Before | After | Δ |
|---------|--------|-------|---|
| pre_consume_us | 179,225 | 153,690 | **−25,535 µs** |
| pre_scan_us | ~49,000 | 46,760 | −2,240 µs |
| distinct_inserts | 379,461 | 379,461 | 0 |
| dict_chunks | 4,682 | 4,682 | 0 |

## Anti-patterns confirmed

- **Generation-stamp adsh dict cache + EnsureFixedSlot/AggregateFixedSlotRow helpers** — 1.82× regression; extra indirection and dual-array lookup outweighed slot-matrix gains.
- **16-byte AdshKey without tail bytes** — Fingerprint collision under-counted distinct (38,852 vs 379,461); fixed by extending to 24-byte key.
- **Eager pre-intern / two-pass flat-adsh** — Still excluded (regressed in prior iterations).

## Playbook signals

- **Retain:** Layout-driven slot matrix (16 stmt × 4 rfile dict entries).
- **Retain:** Fixed-width fingerprint keys for bounded VARCHAR domains (verify full width covers distinct domain).
- **Retain:** `insert_presized` on pre-sized bitmap distinct sets.
- **Next episode:** `pre_scan_us` (~47 ms) is now ~23% of total; fused scan projection or filter pushdown may yield further gains.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 4, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 155,021 µs (dominant), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 2.10× best, consume still spends cycles on redundant `std::string` storage inside `AdshInternTable` and `TryFixedSlot` indirection in all-valid dict fast paths.

**Technique:** Lean coordinated bundle — fingerprint-only intern table, direct slot-matrix indexing, inlined distinct insert on the hottest all-valid flat-adsh path. Explicitly excludes pre-intern two-pass and aggregate helper wrappers (confirmed regressions in iter 3).

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | distinct_inserts | Verdict |
|-------|------------|----------------|----------------|------------------|---------|
| `ep1_consume_bundle` | Pre-intern dict/flat adsh + AggregateFixedSlot helper + batch caches | **1.25×** | 304,935 | 379,461 | **Discard** — two-pass pre-intern doubled consume time |
| `ep1_lean_intern_slot` | Fingerprint-only intern + LookupDictFixedSlot + inlined distinct on DictDictFlatFlat | **2.24×** | 143,347 | 379,461 | **Keep** |

## Before (best snapshot, iter 3)

| Metric | Value |
|--------|-------|
| speedup_median | **2.10×** |
| pre_consume_us | 155,021 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep1_lean_intern_slot`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **2.24×** | **+0.14×** |
| pre_consume_us | 143,347 | **−11,674 µs** |
| pre_scan_us | 46,461 | ~flat |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (4 coordinated changes)

1. **`AdshInternTable` fingerprint-only** — Drop unused `entries` vector; assign monotonic `next_index` on insert. Eliminates ~379k heap string copies while retaining 24-byte `AdshKey` dedup.
2. **`LookupDictFixedSlot`** — Direct `slot_matrix` array access replaces `TryFixedSlot` out-parameter call in all-valid dict paths (DictDictFlatFlat, DictDictDictFlat, DictDictFlatDict, DictDictDictDict).
3. **Inlined distinct insert (DictDictFlatFlat all-valid)** — Open-code `last_distinct_adsh` + `insert_presized` in the hottest fixed-slot loop; avoids `InsertDistinct` call overhead on the adsh-flat path.
4. **Retained prior wins** — Slot matrix precompute, dict-adsh per-chunk lazy intern cache, bitmap distinct tracking, consecutive row intern cache.

## Profile counter deltas

| Counter | Before | After | Δ |
|---------|--------|-------|---|
| pre_consume_us | 155,021 | 143,347 | **−11,674 µs** |
| pre_scan_us | ~47,000 | 46,461 | ~flat |
| distinct_inserts | 379,461 | 379,461 | 0 |
| dict_chunks | 4,682 | 4,682 | 0 |

## Anti-patterns confirmed (this episode)

- **Two-pass pre-intern (`PreInternAdshDictionary` / `PreInternFlatAdshRows`) + `AggregateFixedSlot` helper** — 1.25× regression; extra scan pass and helper indirection dominated any cache benefit.
- **Eager bitmap presize on all 1280 fixed slots** — Rejected before benchmark; would allocate O(slots × row_estimate) memory.

## Playbook signals

- **Retain:** Fingerprint-only intern tables when string payloads are never read back.
- **Retain:** Direct dense slot indexing over helper wrappers on 9M+ row loops.
- **Avoid:** Pre-chunk intern sweeps that duplicate the row loop's work.
- **Next episode:** `pre_scan_us` (~46 ms) is now ~24% of profile time; fused scan projection ordering or filter pushdown refinements may yield further gains.

## Q1 update
# SEC-EDGAR Q1 optimization iteration 5, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 145,272 µs (dominant), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 2.25× best, consume still spends cycles on `ToStoredString` inside `ActivateFixedSlot` for ~14 fixed groups and scattered `InsertDistinct` call overhead across all-valid dict fast paths.

**Technique:** Lean coordinated bundle — deferred fixed-slot metadata (`TouchFixedSlot` + `DecodeFixedSlotLabel`), `AggregateFixedSlotHot` on all all-valid dict/flat paths. Explicitly excludes chunk-level adsh dict pre-intern sweep (confirmed regression in `ep1_consume_bundle`).

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | distinct_inserts | Verdict |
|-------|------------|----------------|----------------|------------------|---------|
| `ep1_consume_bundle` | TouchFixedSlot + dict pre-intern + AggregateFixedSlotHot on all paths | **2.19×** | 148,257 | 379,461 | **Discard** — dict pre-intern sweep regressed consume |
| `ep1_lean_deferred_slots` | TouchFixedSlot + AggregateFixedSlotHot + DecodeFixedSlotLabel at finalize | **2.27×** | 140,889 | 379,461 | **Keep** |

## Before (best snapshot, iter 4)

| Metric | Value |
|--------|-------|
| speedup_median | **2.25×** |
| pre_consume_us | 145,272 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep1_lean_deferred_slots`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **2.27×** | **+0.02×** |
| pre_consume_us | 140,889 | **−4,383 µs** |
| pre_scan_us | 49,881 | ~flat |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (5 coordinated changes)

1. **`TouchFixedSlot`** — Activate fixed slots without `ToStoredString` on hot path; only set `active`, presize bitmap, push to active list.
2. **`DecodeFixedSlotLabel`** — Reconstruct `(stmt, rfile)` from dense `slot_index` at finalize only (~14 groups).
3. **`AggregateFixedSlotHot`** — Open-coded cnt/line_sum/distinct insert on all all-valid fixed-slot loops (DictDictFlatFlat, DictDictDictFlat, DictDictFlatDict, DictDictDictDict, FlatFlatFlatFlat).
4. **Retained prior wins** — Fingerprint-only intern table, `LookupDictFixedSlot`, lazy per-chunk adsh dict cache, bitmap distinct tracking.
5. **Anti-pattern avoided** — No chunk-level adsh dict pre-intern sweep (extra O(dict_size) pass per chunk hurt despite fewer row-loop branches).

## Profile counter deltas

| Counter | Before | After | Δ |
|---------|--------|-------|---|
| pre_consume_us | 145,272 | 140,889 | **−4,383 µs** |
| pre_scan_us | ~47,000 | 49,881 | ~flat |
| distinct_inserts | 379,461 | 379,461 | 0 |
| dict_chunks | 4,682 | 4,682 | 0 |

## Playbook signals

- **Retain:** Deferred fixed-slot metadata when group count ≪ row count and slot index encodes `(stmt, rfile)`.
- **Retain:** `AggregateFixedSlotHot` inline helper on 9M+ row all-valid loops.
- **Avoid:** Chunk-level adsh dict pre-intern sweeps — lazy per-row cache wins on this workload.
- **Next episode:** `pre_scan_us` (~50 ms) is now ~26% of profile time; fused scan projection ordering may yield further gains.

## Q2 iter 1

Implemented DICTIONARY/FLAT/CONSTANT consumable-column fast paths for sub and num scans, driving generic_fallback_rows from 100% to 0 (40211 dict chunks). Skipped redundant uom checks in fused scan passes where NativeScanSpec already filters uom='pure'. Added shared dictionary selection-vector tight loop for num max/collect passes. String interning for hash probes regressed to 1.20x and was discarded; next focus is reducing per-row string allocation in UpdateMax.

- `dict_flat_consumable_paths`: kept, Δspeedup=+0.000x (generic_rows=-2757133, dict_chunks=40211, generic_chunks=-40211)
- `skip_uom_fused_scan`: kept, Δspeedup=+0.000x (pre_scan_us=-767)
- `shared_sel_dict_loop`: kept, Δspeedup=+0.000x (num_max_us=-3405, pre_scan_us=13283)
- `string_intern_pair_keys`: discarded, Δspeedup=-0.050x (pre_scan_us=59592, num_max_us=56010)

## Q2 iter 2

Merged num max+collect into a single fused scan with incremental tie tracking, dropped uom from scan projection (filters already pushed), and added sub shared-sel consumable loop. Speedup improved from 1.25x to 2.32x; pre_scan_us halved and dict_chunks dropped from 40211 to 20126 (one pass instead of two). Next focus: reduce per-row string allocation in ProcessNumRow hash probes.

- `single_pass_num_fused`: kept, Δspeedup=+1.070x (pre_scan_us=-708161, dict_chunks=-20085, num_collect_us=-758009)
- `drop_uom_projection`: kept, Δspeedup=+1.070x (pre_rows=-1364900)
- `sub_shared_sel_loop`: kept, Δspeedup=+1.070x (sub_scan_us=-767)
- `incremental_tie_finalize`: kept, Δspeedup=+1.070x (num_max_us=-3405, qualifying_rows=0)

## Q2 iter 3

AdshKey fy2022 map with early adsh membership filter and shared dict+flat num path raised speedup from 2.33x to 2.66x by skipping non-join rows and hash allocations. Composite single-alloc group keys added a further 2.68x best. Nested transparent tag lookup regressed compile and was discarded; next focus is further num scan aggregation cost (num_max_us).

- `early_adshkey_filter`: kept, Δspeedup=+0.330x (pre_scan_us=-82200, distinct_inserts=-13746, qualifying_rows=0)
- `shared_dict_flat_value_path`: kept, Δspeedup=+0.330x (pre_scan_us=-82200, dict_chunks=0)
- `composite_group_key`: kept, Δspeedup=+0.020x (pre_scan_us=17116, num_max_us=17285)
- `nested_transparent_tag_lookup`: discarded, Δspeedup=+0.000x

## Q2 iter 4

Shipped a consume-side bundle (fingerprint GroupKey, FLAT fast paths, sub-only RunNativeFilteredScanConsumer) that raised speedup 2.69x → 2.71x and cut pre_scan_us by ~23 ms. Full num-table RunNativeFilteredScanConsumer segfaults in DuckDB FilterSelection on uom/value filters — keep CreateNativeChunkSource for num; retry fused num scan with corrected filter column layout next episode.

- `filtered_scan_consumer_num`: discarded
- `composite_group_key`: kept, Δspeedup=+0.020x (pre_scan_us=-22799, distinct_inserts=-36)
- `flat_all_valid_fast_paths`: kept, Δspeedup=+0.020x (pre_scan_us=-22799, generic_rows=0)
- `sub_filtered_scan_consumer`: kept, Δspeedup=+0.020x (pre_scan_us=-22799, sub_scan_us=-4849)
- `try_emplace_fy2022_map`: kept, Δspeedup=+0.020x

## Q2 iter 5

Shipped adsh dictionary-index qualifying cache (per-chunk scoped) plus ProcessNumRowWithName and ProcessNumConsumableFiltered early fy2022 rejection in num scan consumable paths. Quick benchmark improved speedup 2.72x to 2.79x with pre_scan_us down ~18ms; num filtered scan consumer regressed (segfault) and was reverted. Next: retry num RunNativeFilteredScanConsumer with correct scan column indices or group-key dict pair cache within chunks.

- `adsh_dict_qualifying_cache`: kept, Δspeedup=+0.070x (pre_scan_us=-18176, dict_chunks=0, distinct_inserts=1)
- `process_num_row_with_name`: kept, Δspeedup=+0.070x (pre_scan_us=-18176)
- `process_num_consumable_filtered`: kept, Δspeedup=+0.070x (pre_scan_us=-18176, pre_rows=0)
- `num_filtered_scan_consumer`: discarded, Δspeedup=+0.000x

## Q3 iter 1

Wired RunNumFusedPass and ConsumeNumChunk through BindConsumable + ProcessNumConsumableFiltered, driving generic_fallback_rows from ~39.4M to 0 (dict_chunks=20111). Initial 1.15x A/B used a buggy AdshDictQualifyingCache; corrected flat lookup passes smoke tests at ~0.86x. Scan filter pushdown and name-id aggregation regressed and were discarded.

- `num_consumable_fast_path`: kept, Δspeedup=-0.020x (generic_rows=-39401761, dict_chunks=20071, flat_chunks=83)
- `scan_filter_pushdown`: discarded, Δspeedup=-0.080x (pre_rows=-6226557)
- `name_id_aggregation`: discarded, Δspeedup=-0.070x
- `adsh_dict_qualify_cache`: discarded, Δspeedup=+0.270x (generic_rows=-39401761)

## Q3 iter 2

Routed RunNumFusedPass and ConsumeNumChunk through BindConsumable + ProcessNumConsumableFiltered, eliminating 100% generic_fallback_rows and cutting pre_scan_us ~370ms. Fixed adsh dict cache correctness with per-key verification (stale index cache was causing wrong aggregates). NativeScanSpec uom/fy filter pushdown regressed and was discarded. Correct speedup ~0.96x beats prior best 0.88x; next focus is num_scan_us with verified dict cache + aggregation hot path.

- `fused_consumable_dispatch`: kept, Δspeedup=+0.080x (generic_rows=-39401761, pre_scan_us=-371034, dict_chunks=84, flat_chunks=-86)
- `uom_constant_chunk_gate`: kept, Δspeedup=+0.080x (pre_scan_us=-371034)
- `value_dict_adsh_flat_path`: kept, Δspeedup=+0.000x (pre_scan_us=-7505)
- `adsh_dict_key_verified_cache`: kept, Δspeedup=+0.080x (pre_scan_us=-371034)
- `num_uom_scan_filter`: discarded, Δspeedup=-0.050x (pre_scan_us=115812, pre_rows=-6226557)
- `inline_adsh_dict_cache`: discarded, Δspeedup=-0.010x (pre_scan_us=76301)

## Q3 iter 3

Scan filter pushdown (fy=2022 on sub, value IS NOT NULL on num) reached parity at 1.0x speedup and cut pre_scan_us ~34ms. UOM dict cache, sub fy fast-insert, and unverified adsh dict index shortcuts regressed or failed correctness. Next focus: safe within-chunk dict optimizations that preserve key verification.

- `scan_filter_pushdown`: kept, Δspeedup=+0.040x (pre_scan_us=-33759, sub_scan_us=-4998)
- `uom_dict_qualify_cache`: discarded, Δspeedup=-0.010x (pre_scan_us=-50566)
- `sub_fy_fast_insert`: discarded, Δspeedup=-0.020x (pre_scan_us=6821)
- `adsh_dict_trust_index`: discarded, Δspeedup=+0.170x (pre_scan_us=-393724)
- `adsh_dict_lookup_fast_path`: kept, Δspeedup=+0.030x (pre_scan_us=-76881)

## Q3 iter 4

Pushed uom=USD into NativeScanSpec, added verified adsh dict sel-prewarm with LookupVerified on shared-sel paths, and skipped uom vector bind when filter is pushed — reaching 1.01x speedup with passing smoke test. RunNativeFilteredScanConsumer regressed (wrong results/segfault); full-dictionary prewarm and uom dict cache also discarded.

- `num_uom_scan_filter`: kept, Δspeedup=+0.030x (pre_rows=-4417765, pre_scan_us=-5000)
- `sub_fy_filter_skip`: kept, Δspeedup=+0.010x (sub_scan_us=-200)
- `adsh_dict_sel_prewarm`: kept, Δspeedup=+0.020x (dict_chunks=82, pre_scan_us=-10000)
- `skip_uom_bind_when_filtered`: kept, Δspeedup=+0.040x (pre_scan_us=-47125, num_scan_us=-47134)
- `filtered_scan_consumer`: discarded, Δspeedup=+0.620x (pre_scan_us=-1083396)
- `adsh_dict_full_prewarm`: discarded, Δspeedup=-0.600x (pre_scan_us=1078718)
- `uom_dict_usd_table`: discarded, Δspeedup=+0.810x (pre_scan_us=-1261085)

## Q3 iter 5

Trimmed scan projections (dropped fy/uom columns already filtered at storage) and switched sub scan to RunNativeFilteredScanConsumer with a uom-free 2-column num consume path. Median speedup improved from 1.030x to 1.050x with pre_scan_us down ~52ms; num filtered consumer segfaulted so num stays on CreateNativeChunkSource.

- `trim_scan_projections`: kept, Δspeedup=+0.020x (pre_scan_us=-51934, pre_rows=0)
- `filtered_scan_consumer_sub`: kept, Δspeedup=+0.020x (pre_scan_us=-51934, sub_scan_us=-5000)
- `uom_free_num_path`: kept, Δspeedup=+0.020x (dict_chunks=0, pre_consume_us=0)
- `filtered_scan_consumer_num`: discarded

## Q4 iter 1

Pre-resolved pre-table triples to {group_id, cik_dense_id, multiplicity} at build time, cutting num consume from ~10s to ~2.5s.
Replaced per-group unordered_set CIK tracking with dense-index bitmaps and added shared-selection-vector dict cache on the num hot path.
Speedup improved 0.22x → 0.61x (+0.39); still below DuckDB — next episode should attack remaining pre_scan/pre_consume overhead.

- `pre_resolve_triples`: kept, Δspeedup=+0.350x (pre_consume_us=-7500000, num_aggregate_us=-7500000)
- `compact_string_keys`: kept, Δspeedup=+0.020x
- `cik_dense_bitmap`: kept, Δspeedup=+0.020x
- `dict_triple_cache`: kept, Δspeedup=+0.000x
- `mixed_dict_adsh_only_cache`: discarded, Δspeedup=-0.050x (pre_consume_us=460000)

## Q4 iter 2

Wire dict-index triple cache into num consume hot path; use stack StringKey triple keys to eliminate per-row heap allocations.

- `dict_triple_cache_hotpath`: kept, Δspeedup=+0.040x
- `compact_string_keys_triple`: kept, Δspeedup=+0.040x

## Q4 iter 3

Wire dict-index triple cache Lookup into num consume hot path; use stack StringKey for tag/version in CompactTripleKey; store ResolvedPreTriple by value per dict slot.

- `dict_cache_hotpath_v2`: kept, Δspeedup=+0.030x
- `compact_string_keys_triple_v2`: kept, Δspeedup=+0.030x

## Q4 iter 4

Wire Prewarm+LookupVerified on shared-dict-sel num chunks (matching Q3 pattern); route qualifying rows directly to Accumulate with cached ResolvedPreTriple pointers, eliminating per-row map.find.

- `dict_cache_hotpath_v3`: kept, Δspeedup=+0.010x

## Q4 iter 5

Skip redundant Prewarm dict-slot lookups; inline ResolvedPreTriple in NumTripleDictCache (Q3 pattern).
Prewarm resolves each dict index once per chunk; consume uses LookupVerified + direct sel indexing.
Speedup flat at 0.44x; pre_consume_us ~37ms lower vs iter-5 baseline; StringKey/PreTripleKey variants rejected (correctness or regression).

- `dict_prewarm_skip_v5`: kept, Δspeedup=+0.000x (pre_consume_us=-37000, num_aggregate_us=-37000)
- `inline_resolved_pre_triple`: kept, Δspeedup=+0.000x
- `adsh_key_dict_slot_keys`: kept, Δspeedup=+0.000x
- `sel_direct_num_consume`: kept, Δspeedup=+0.000x
- `pre_triple_key_intern`: discarded, Δspeedup=-0.160x (pre_consume_us=4000000)
- `compact_string_keys_tag_version`: discarded, Δspeedup=+0.170x (pre_consume_us=-1000000)

## Q1 update (legacy)
# SEC-EDGAR Q1 optimization iteration 5, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 145,272 µs (dominant), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 2.25× best, consume still spends cycles on `ToStoredString` inside `ActivateFixedSlot` for ~14 fixed groups and scattered `InsertDistinct` call overhead across all-valid dict fast paths.

**Technique:** Lean coordinated bundle — deferred fixed-slot metadata (`TouchFixedSlot` + `DecodeFixedSlotLabel`), `AggregateFixedSlotHot` on all all-valid dict/flat paths. Explicitly excludes chunk-level adsh dict pre-intern sweep (confirmed regression in `ep1_consume_bundle`).

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | distinct_inserts | Verdict |
|-------|------------|----------------|----------------|------------------|---------|
| `ep1_consume_bundle` | TouchFixedSlot + dict pre-intern + AggregateFixedSlotHot on all paths | **2.19×** | 148,257 | 379,461 | **Discard** — dict pre-intern sweep regressed consume |
| `ep1_lean_deferred_slots` | TouchFixedSlot + AggregateFixedSlotHot + DecodeFixedSlotLabel at finalize | **2.27×** | 140,889 | 379,461 | **Keep** |

## Before (best snapshot, iter 4)

| Metric | Value |
|--------|-------|
| speedup_median | **2.25×** |
| pre_consume_us | 145,272 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep1_lean_deferred_slots`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **2.27×** | **+0.02×** |
| pre_consume_us | 140,889 | **−4,383 µs** |
| pre_scan_us | 49,881 | ~flat |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (5 coordinated changes)

1. **`TouchFixedSlot`** — Activate fixed slots without `ToStoredString` on hot path; only set `active`, presize bitmap, push to active list.
2. **`DecodeFixedSlotLabel`** — Reconstruct `(stmt, rfile)` from dense `slot_index` at finalize only (~14 groups).
3. **`AggregateFixedSlotHot`** — Open-coded cnt/line_sum/distinct insert on all all-valid fixed-slot loops (DictDictFlatFlat, DictDictDictFlat, DictDictFlatDict, DictDictDictDict, FlatFlatFlatFlat).
4. **Retained prior wins** — Fingerprint-only intern table, `LookupDictFixedSlot`, lazy per-chunk adsh dict cache, bitmap distinct tracking.
5. **Anti-pattern avoided** — No chunk-level adsh dict pre-intern sweep (extra O(dict_size) pass per chunk hurt despite fewer row-loop branches).

## Profile counter deltas

| Counter | Before | After | Δ |
|---------|--------|-------|---|
| pre_consume_us | 145,272 | 140,889 | **−4,383 µs** |
| pre_scan_us | ~47,000 | 49,881 | ~flat |
| distinct_inserts | 379,461 | 379,461 | 0 |
| dict_chunks | 4,682 | 4,682 | 0 |

## Playbook signals

- **Retain:** Deferred fixed-slot metadata when group count ≪ row count and slot index encodes `(stmt, rfile)`.
- **Retain:** `AggregateFixedSlotHot` inline helper on 9M+ row all-valid loops.
- **Avoid:** Chunk-level adsh dict pre-intern sweeps — lazy per-row cache wins on this workload.
- **Next episode:** `pre_scan_us` (~50 ms) is now ~26% of profile time; fused scan projection ordering may yield further gains.

## Q1 update (legacy)
# SEC-EDGAR Q1 optimization iteration 5, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 145,272 µs (dominant), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 2.25× best, consume still spends cycles on `ToStoredString` inside `ActivateFixedSlot` for ~14 fixed groups and scattered `InsertDistinct` call overhead across all-valid dict fast paths.

**Technique:** Lean coordinated bundle — deferred fixed-slot metadata (`TouchFixedSlot` + `DecodeFixedSlotLabel`), `AggregateFixedSlotHot` on all all-valid dict/flat paths. Explicitly excludes chunk-level adsh dict pre-intern sweep (confirmed regression in `ep1_consume_bundle`).

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | distinct_inserts | Verdict |
|-------|------------|----------------|----------------|------------------|---------|
| `ep1_consume_bundle` | TouchFixedSlot + dict pre-intern + AggregateFixedSlotHot on all paths | **2.19×** | 148,257 | 379,461 | **Discard** — dict pre-intern sweep regressed consume |
| `ep1_lean_deferred_slots` | TouchFixedSlot + AggregateFixedSlotHot + DecodeFixedSlotLabel at finalize | **2.27×** | 140,889 | 379,461 | **Keep** |

## Before (best snapshot, iter 4)

| Metric | Value |
|--------|-------|
| speedup_median | **2.25×** |
| pre_consume_us | 145,272 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep1_lean_deferred_slots`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **2.27×** | **+0.02×** |
| pre_consume_us | 140,889 | **−4,383 µs** |
| pre_scan_us | 49,881 | ~flat |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (5 coordinated changes)

1. **`TouchFixedSlot`** — Activate fixed slots without `ToStoredString` on hot path; only set `active`, presize bitmap, push to active list.
2. **`DecodeFixedSlotLabel`** — Reconstruct `(stmt, rfile)` from dense `slot_index` at finalize only (~14 groups).
3. **`AggregateFixedSlotHot`** — Open-coded cnt/line_sum/distinct insert on all all-valid fixed-slot loops (DictDictFlatFlat, DictDictDictFlat, DictDictFlatDict, DictDictDictDict, FlatFlatFlatFlat).
4. **Retained prior wins** — Fingerprint-only intern table, `LookupDictFixedSlot`, lazy per-chunk adsh dict cache, bitmap distinct tracking.
5. **Anti-pattern avoided** — No chunk-level adsh dict pre-intern sweep (extra O(dict_size) pass per chunk hurt despite fewer row-loop branches).

## Profile counter deltas

| Counter | Before | After | Δ |
|---------|--------|-------|---|
| pre_consume_us | 145,272 | 140,889 | **−4,383 µs** |
| pre_scan_us | ~47,000 | 49,881 | ~flat |
| distinct_inserts | 379,461 | 379,461 | 0 |
| dict_chunks | 4,682 | 4,682 | 0 |

## Playbook signals

- **Retain:** Deferred fixed-slot metadata when group count ≪ row count and slot index encodes `(stmt, rfile)`.
- **Retain:** `AggregateFixedSlotHot` inline helper on 9M+ row all-valid loops.
- **Avoid:** Chunk-level adsh dict pre-intern sweeps — lazy per-row cache wins on this workload.
- **Next episode:** `pre_scan_us` (~50 ms) is now ~26% of profile time; fused scan projection ordering may yield further gains.

## Q1 update (legacy)
# SEC-EDGAR Q1 optimization iteration 5, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 145,272 µs (dominant), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 2.25× best, consume still spends cycles on `ToStoredString` inside `ActivateFixedSlot` for ~14 fixed groups and scattered `InsertDistinct` call overhead across all-valid dict fast paths.

**Technique:** Lean coordinated bundle — deferred fixed-slot metadata (`TouchFixedSlot` + `DecodeFixedSlotLabel`), `AggregateFixedSlotHot` on all all-valid dict/flat paths. Explicitly excludes chunk-level adsh dict pre-intern sweep (confirmed regression in `ep1_consume_bundle`).

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | distinct_inserts | Verdict |
|-------|------------|----------------|----------------|------------------|---------|
| `ep1_consume_bundle` | TouchFixedSlot + dict pre-intern + AggregateFixedSlotHot on all paths | **2.19×** | 148,257 | 379,461 | **Discard** — dict pre-intern sweep regressed consume |
| `ep1_lean_deferred_slots` | TouchFixedSlot + AggregateFixedSlotHot + DecodeFixedSlotLabel at finalize | **2.27×** | 140,889 | 379,461 | **Keep** |

## Before (best snapshot, iter 4)

| Metric | Value |
|--------|-------|
| speedup_median | **2.25×** |
| pre_consume_us | 145,272 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep1_lean_deferred_slots`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **2.27×** | **+0.02×** |
| pre_consume_us | 140,889 | **−4,383 µs** |
| pre_scan_us | 49,881 | ~flat |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (5 coordinated changes)

1. **`TouchFixedSlot`** — Activate fixed slots without `ToStoredString` on hot path; only set `active`, presize bitmap, push to active list.
2. **`DecodeFixedSlotLabel`** — Reconstruct `(stmt, rfile)` from dense `slot_index` at finalize only (~14 groups).
3. **`AggregateFixedSlotHot`** — Open-coded cnt/line_sum/distinct insert on all all-valid fixed-slot loops (DictDictFlatFlat, DictDictDictFlat, DictDictFlatDict, DictDictDictDict, FlatFlatFlatFlat).
4. **Retained prior wins** — Fingerprint-only intern table, `LookupDictFixedSlot`, lazy per-chunk adsh dict cache, bitmap distinct tracking.
5. **Anti-pattern avoided** — No chunk-level adsh dict pre-intern sweep (extra O(dict_size) pass per chunk hurt despite fewer row-loop branches).

## Profile counter deltas

| Counter | Before | After | Δ |
|---------|--------|-------|---|
| pre_consume_us | 145,272 | 140,889 | **−4,383 µs** |
| pre_scan_us | ~47,000 | 49,881 | ~flat |
| distinct_inserts | 379,461 | 379,461 | 0 |
| dict_chunks | 4,682 | 4,682 | 0 |

## Playbook signals

- **Retain:** Deferred fixed-slot metadata when group count ≪ row count and slot index encodes `(stmt, rfile)`.
- **Retain:** `AggregateFixedSlotHot` inline helper on 9M+ row all-valid loops.
- **Avoid:** Chunk-level adsh dict pre-intern sweeps — lazy per-row cache wins on this workload.
- **Next episode:** `pre_scan_us` (~50 ms) is now ~26% of profile time; fused scan projection ordering may yield further gains.

## Q1 update (legacy)
# SEC-EDGAR Q1 optimization iteration 5, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 145,272 µs (dominant), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 2.25× best, consume still spends cycles on `ToStoredString` inside `ActivateFixedSlot` for ~14 fixed groups and scattered `InsertDistinct` call overhead across all-valid dict fast paths.

**Technique:** Lean coordinated bundle — deferred fixed-slot metadata (`TouchFixedSlot` + `DecodeFixedSlotLabel`), `AggregateFixedSlotHot` on all all-valid dict/flat paths. Explicitly excludes chunk-level adsh dict pre-intern sweep (confirmed regression in `ep1_consume_bundle`).

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | distinct_inserts | Verdict |
|-------|------------|----------------|----------------|------------------|---------|
| `ep1_consume_bundle` | TouchFixedSlot + dict pre-intern + AggregateFixedSlotHot on all paths | **2.19×** | 148,257 | 379,461 | **Discard** — dict pre-intern sweep regressed consume |
| `ep1_lean_deferred_slots` | TouchFixedSlot + AggregateFixedSlotHot + DecodeFixedSlotLabel at finalize | **2.27×** | 140,889 | 379,461 | **Keep** |

## Before (best snapshot, iter 4)

| Metric | Value |
|--------|-------|
| speedup_median | **2.25×** |
| pre_consume_us | 145,272 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep1_lean_deferred_slots`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **2.27×** | **+0.02×** |
| pre_consume_us | 140,889 | **−4,383 µs** |
| pre_scan_us | 49,881 | ~flat |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (5 coordinated changes)

1. **`TouchFixedSlot`** — Activate fixed slots without `ToStoredString` on hot path; only set `active`, presize bitmap, push to active list.
2. **`DecodeFixedSlotLabel`** — Reconstruct `(stmt, rfile)` from dense `slot_index` at finalize only (~14 groups).
3. **`AggregateFixedSlotHot`** — Open-coded cnt/line_sum/distinct insert on all all-valid fixed-slot loops (DictDictFlatFlat, DictDictDictFlat, DictDictFlatDict, DictDictDictDict, FlatFlatFlatFlat).
4. **Retained prior wins** — Fingerprint-only intern table, `LookupDictFixedSlot`, lazy per-chunk adsh dict cache, bitmap distinct tracking.
5. **Anti-pattern avoided** — No chunk-level adsh dict pre-intern sweep (extra O(dict_size) pass per chunk hurt despite fewer row-loop branches).

## Profile counter deltas

| Counter | Before | After | Δ |
|---------|--------|-------|---|
| pre_consume_us | 145,272 | 140,889 | **−4,383 µs** |
| pre_scan_us | ~47,000 | 49,881 | ~flat |
| distinct_inserts | 379,461 | 379,461 | 0 |
| dict_chunks | 4,682 | 4,682 | 0 |

## Playbook signals

- **Retain:** Deferred fixed-slot metadata when group count ≪ row count and slot index encodes `(stmt, rfile)`.
- **Retain:** `AggregateFixedSlotHot` inline helper on 9M+ row all-valid loops.
- **Avoid:** Chunk-level adsh dict pre-intern sweeps — lazy per-row cache wins on this workload.
- **Next episode:** `pre_scan_us` (~50 ms) is now ~26% of profile time; fused scan projection ordering may yield further gains.

## Q1 update (legacy)
# SEC-EDGAR Q1 optimization iteration 5, episode 1

## Hypothesis

**Target bottlenecks:** `pre_consume_us` = 145,272 µs (dominant), `distinct_inserts` = 379,461, `dict_chunks` = 4,682.

At 2.25× best, consume still spends cycles on `ToStoredString` inside `ActivateFixedSlot` for ~14 fixed groups and scattered `InsertDistinct` call overhead across all-valid dict fast paths.

**Technique:** Lean coordinated bundle — deferred fixed-slot metadata (`TouchFixedSlot` + `DecodeFixedSlotLabel`), `AggregateFixedSlotHot` on all all-valid dict/flat paths. Explicitly excludes chunk-level adsh dict pre-intern sweep (confirmed regression in `ep1_consume_bundle`).

## A/B trial outcomes

| Trial | Hypothesis | speedup_median | pre_consume_us | distinct_inserts | Verdict |
|-------|------------|----------------|----------------|------------------|---------|
| `ep1_consume_bundle` | TouchFixedSlot + dict pre-intern + AggregateFixedSlotHot on all paths | **2.19×** | 148,257 | 379,461 | **Discard** — dict pre-intern sweep regressed consume |
| `ep1_lean_deferred_slots` | TouchFixedSlot + AggregateFixedSlotHot + DecodeFixedSlotLabel at finalize | **2.27×** | 140,889 | 379,461 | **Keep** |

## Before (best snapshot, iter 4)

| Metric | Value |
|--------|-------|
| speedup_median | **2.25×** |
| pre_consume_us | 145,272 |
| distinct_inserts | 379,461 |
| dict_chunks | 4,682 |
| generic_rows | 0 |

## After (kept trial `ep1_lean_deferred_slots`)

| Metric | Value | Δ vs best |
|--------|-------|-----------|
| speedup_median | **2.27×** | **+0.02×** |
| pre_consume_us | 140,889 | **−4,383 µs** |
| pre_scan_us | 49,881 | ~flat |
| distinct_inserts | 379,461 | 0 (semantics preserved) |
| dict_chunks | 4,682 | 0 |
| generic_rows | 0 | 0 |

## Kept bundle (5 coordinated changes)

1. **`TouchFixedSlot`** — Activate fixed slots without `ToStoredString` on hot path; only set `active`, presize bitmap, push to active list.
2. **`DecodeFixedSlotLabel`** — Reconstruct `(stmt, rfile)` from dense `slot_index` at finalize only (~14 groups).
3. **`AggregateFixedSlotHot`** — Open-coded cnt/line_sum/distinct insert on all all-valid fixed-slot loops (DictDictFlatFlat, DictDictDictFlat, DictDictFlatDict, DictDictDictDict, FlatFlatFlatFlat).
4. **Retained prior wins** — Fingerprint-only intern table, `LookupDictFixedSlot`, lazy per-chunk adsh dict cache, bitmap distinct tracking.
5. **Anti-pattern avoided** — No chunk-level adsh dict pre-intern sweep (extra O(dict_size) pass per chunk hurt despite fewer row-loop branches).

## Profile counter deltas

| Counter | Before | After | Δ |
|---------|--------|-------|---|
| pre_consume_us | 145,272 | 140,889 | **−4,383 µs** |
| pre_scan_us | ~47,000 | 49,881 | ~flat |
| distinct_inserts | 379,461 | 379,461 | 0 |
| dict_chunks | 4,682 | 4,682 | 0 |

## Playbook signals

- **Retain:** Deferred fixed-slot metadata when group count ≪ row count and slot index encodes `(stmt, rfile)`.
- **Retain:** `AggregateFixedSlotHot` inline helper on 9M+ row all-valid loops.
- **Avoid:** Chunk-level adsh dict pre-intern sweeps — lazy per-row cache wins on this workload.
- **Next episode:** `pre_scan_us` (~50 ms) is now ~26% of profile time; fused scan projection ordering may yield further gains.
