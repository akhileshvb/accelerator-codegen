# Scale factor contract (iteration vs production)

The pipeline iterates on small datasets; production targets much larger scale factors. A design must stay correct and fast as data grows, not only on the iteration database.

| Tier | DB | Use | Obligation |
|---|---|---|---|
| Iteration (SF1) | the host's dataset env var | correctness, baseline, profile, write-frequency sweeps, A/B | primary signal; fast turnaround |
| Scale check (SF10) | `scale_db` | `sf10` gate after optimization + audit | catches sizing blowups and SF1-only state; a crash, timeout or sharp regression is a scaling bug |
| Design target (SF3000, ~8 TB RAM) | not materialized; approximated by the magnitude databases below | sizing of every key, offset, count, sum, shard, buffer | largest tables ~2e10 rows, ids to ~2e10 (sparse, beyond 2^32), 4 GB+ heaps, sums beyond 2^63, refreshes of 1e5-1e7 rows per commit, all state in memory |

A technique that wins on SF1 but uses SF1-hardcoded capacities, SF1-sized full-domain scans or structures growing superlinearly in table cardinality is not acceptable. Ask whether a change still wins if the largest tables grow x3000 with distinct keys; prefer changes that improve SF1 and remove SF1-specific constants. If SF1 read-path gains stall, redesign the aux structure for cardinality and shape, without caching final answers.

SEC-EDGAR workloads use SF3 as both iteration and production scale (`ACCEL_SEC_EDGAR_DB`); the same rules apply.

## Sizing rules

1. Size from bind-time `row_estimate`, `GetNativeScanTableRowCount` or bind parameters; never hardcode row or distinct counts seen in the iteration data.
2. Dense direct indexing only when the key domain is bounded by bind metadata and keys are known non-negative and small; otherwise chunked/sparse structures (`AnyKeyCowArray`, `FlatHashMap`). Keys are 64-bit; no fixed-width packing or dense caps (`hard_guidelines.md` section 4).
3. No full-domain finalization over slots sized for SF1 maxima when qualifying groups are sparse.
4. Partition count and payload scale with the query window and mutation locality, not total table size; reads stay bounded to the window at any scale.
5. Bulk refresh and point writes scale with touched data, not full rebuilds.
6. No production `getenv`: scale comes from bind context and table statistics.
7. No final-answer aux: redesign intermediates instead.

## Failure classes (design against each; audit and tests check each)

`substrate_primitives.md` lists the tools; `hard_guidelines.md` section 4 holds the exactness rules.

| Failure class | The module must | Checked by |
|---|---|---|
| Sums overflow 64 bits (scaled decimals, their products, running totals, squares) | Accumulate, store, delta and emit in 128-bit (`ScaledDecimal128`, overflow-checked); widen before multiplying; no unchecked narrowing; compare thresholds exactly | "big" magnitude database (money columns x3000): differential vs SQL on build, delta, rebuild |
| Keys, ids, offsets or counts beyond 32 bits (row ids, arena/blob/dictionary offsets, per-key counters, partition ids, row loop indices, hash-combined ids) | Use 64-bit; a lossy narrowing is a checked cast that fails loudly, never wraps, skips the row or falls to per-row SQL | "shift" magnitude database (every key at its SF3000 maximum): differential incl. writes of such keys |
| Arrays or tables sized by the maximum key; dense ranges over sparse keys | Size by distinct or touched keys (paged/sparse COW); dense only when table statistics bound the domain, with a sparse fallback beyond it, not a skip | "shift" database: state bytes and RSS track row counts, not max key |
| Fixed shard, slot or partition counts | Derive counts from expected rows (`DataScaleRows()`, bind-time estimate) so entries per unit stay bounded; per-unit costs (clone, sort, memmove, index merge) do not grow with total rows | per-changed-row maintenance and per-key read time flat from SF1 to larger scale |
| Hard-coded cap forcing `RebuildAll` or a flatten outside the cost model | No `kMax...` rebuild guard; absorb growth incrementally. Every rebuild is a priced `ChooseMaintenance` candidate with honest `rows_touched`, `affected_partition_count`, `partition_rebuild_supported`; the model records `calibrated_scale_rows` | refresh-sized commits on magnitude databases: decision log shows `ApplyDelta`, `ReadAuxExecuteCounts` shows no rebuild or fallback |
| Overlays, tombstones or delta logs that only grow | Fold into the base incrementally (work proportional to recent changes, per touched unit); a delete removes its entry | state size and read time after many insert/delete cycles return to the fresh-build baseline |
| Per-commit work or COW clones proportional to total state (whole-payload copy, whole-shard clone per touched key, per-commit base-table rescan or full-state pass) | Clone per touched unit at cost proportional to rows changed in it; keep in state what maintenance needs; batch lookups into one set-oriented query | maintenance time and bytes copied per changed row at SF1 vs larger scale vs magnitude databases |
| Serial per-row work dominating large refresh or build sizes | Split over the phase cap: `ParallelEach` / `ParallelReduce` in maintenance (after serial engine lookups), `PartitionedBuild` in `Build`, `ParallelReduce` in `Execute`; identical results at any cap | 1-thread vs N-thread differential; maintenance and build time fall with the caps |
| Transient memory peaks (whole-table vector then copy, all partials alive at merge, per-commit buffers sized by state, payloads held past the query) | Stream into the final structure; cheap partials that merge by moving; commit buffers sized by the delta; hold a payload `shared_ptr` only for the query | peak RSS of a build and of a refresh-sized commit vs settled state size (a small multiple, not 2-5x) |
| Node-based maps, per-row strings, scans of all slots or groups | Columnar `accelerators/prims/` primitives; reads bounded to the window | bytes/row estimate; reads flat in total state |

## Verification set for every accelerator

None of this is a gate yet: run it and report it, in addition to the SF1 gates.

1. The host's magnitude-database script (`<iteration dataset> <dir>`) once; then the engine's shared differential (dataset env var or path argument) on the shifted and big datasets: reads, refresh-shaped bulk writes and point writes.
2. The same differential at `ACCEL_AUX_READ_THREADS` / `BUILD_THREADS` / `MAINT_THREADS` = 1 and higher. A phase below its grain runs as one slice; set `ACCEL_AUX_TEST_GRAIN=1` (test-only cap on every slice grain) so even one-row commits and small reads split, with identical results.
3. `ReadAuxExecuteCounts` and the maintenance decision log across the writes: no unexpected rebuild, no fallback.
