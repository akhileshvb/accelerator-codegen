# Hard guidelines (all agent stages)

Non-negotiable for analysis (planning), implementation, optimization, and audit.

## 1. Native base-table storage

Accelerators read **base tables** through **this engine’s** (`{{engine_name}}`) native storage/scan APIs. Consume engine-local batches, slots, or typed rebuild callbacks — **not** a shared portable chunk IR.

- Do **not** bypass with SQL-over-client “scans” on the hot path when a native adapter exists.
- Do **not** require `ScanNativeBaseTable` → portable `DataChunk` as a cross-engine contract.
- DuckDB may use host-private helpers under `backends/duckdb/engine/native_scan.hpp`; other engines' backends define theirs under `backends/{engine}/engine/` (read-only to query runs).

**Auxiliary state** may and should use **custom storage** (typed in-process images, engine-local aux tables, shared-memory blobs, etc.).

**Forbidden as aux state:** caching or materializing the **final query result** (the exact answer rows for a query binding) and serving it back. Partial aggregates, indexes, eligibility bitmaps, intern tables, membership layouts, and other *intermediate* structures are allowed. If a structure is effectively “precomputed final result for this query,” redesign it.

## 2. Fair benchmarks

No cheating:

- Time the full production query path; consume all results.
- No hidden warmup billed as query latency.
- No SF1-only hardcoded capacities or full-domain work that does not scale.
- No production `getenv` shortcuts that skew timing or skip work.
- Fairness gate is authoritative.

## 3. Aggressive profile-guided optimization that scales

Use measured profile/evidence to drive changes.

### High scale factor is a first-class goal

Iteration benches are often SF1 for speed, but the accelerator must still deliver **strong speedups at high scale factor** (TPC-H **SF10 / SF20+**, with keys/offsets/memory sized for **SF1000+**; SEC at production scale). Treat SF1 wins that blow up memory, scan all groups globally, or hardcode iteration cardinalities as **failures**. Plan with bind-time estimates and data-shape/cardinality reasoning — not SF1 profile snapshots alone.

### When you hit a read-path ceiling

If measured gains stall (micro-opts / emit floors / full-domain filters) and the data shape or cardinality suggests a better family of state:

- You **may redesign the aux-state structure** (partitions, encodings, indexes, dense vs sparse layouts, what is stored vs recomputed).
- Prefer dramatic, scale-aware redesigns over stacking ≤5% tweaks.
- Still forbidden: **precomputed final query results** as aux state (see §1).

### Dual axes: fast reads **and** cheap incremental maintenance

Optimize **both** every attempt — not read-only first with maintenance as an afterthought:

1. **Read axis** — `read_only_query_speedup` / hit latency at 0% writes (weighs more in accept/reject).
2. **Write/maintenance axis** — 10% write-mix `workload_speedup` and point/bulk apply cost from the same quick bench (weighs less, but still gates accepts).

**Accept/reject score** = `read_only^0.75 * write_mix^0.25` (dual-axis weighted geometric mean). A pure read win that deepens the write cliff can lose.

**Maintenance must be cheap:**

- **Bulk / refresh** paths (`Build`, `RebuildAll` / `RebuildPartition`, full resync after cost-model rebuild) — keep them fast, but do **not** use full rebuild as the only response to every point write.
- **Point / synthetic writes** — prefer **ApplyDelta** / touched-key / local-partition updates over `MarkRebuild` + `RebuildAll`.
- When the cost model selects rebuild, **resync/rebuild** so later queries can hit aux again — invalidate-only is not a substitute.
- Prefer maintenance that scales with **touched keys / local partitions**, not full-table rebuilds, when deltas allow.
- Per-attempt quick bench runs **0% reads plus randomly sampled write %s** (fresh sample each attempt): a large drop in write-mix `workload_speedup` (geo-mean across sampled frequencies) is a **first-class failure mode**. If read is already strong and write-mix ≪ 1×, your next hypothesis should attack maintenance cost.

### Read-path ambition

- Expect **large** read-only speedups when aux is valid (often well beyond single-digit percent).
- Prefer structural redesign (layout, encoding, partition pruning, decode elimination) over micro-opts that move <5%.
- Honor exploratory A/B: never ship a change that regresses the **dual-axis** accept score vs best_valid, and do not deepen the write-mix cliff for small read wins.
- Do not game the harness.

## 4. Generality and exactness contract (aux state)

Fast is worthless if the answer is wrong or the state breaks at scale. Spec-value parameters, spec-range data and SF1 sizes are **test inputs, not assumptions**.

**Parameters.** Every parameter of the SQL template must be answered **exactly for any value of its SQL type, including NULL** (SQL semantics: usually an empty result, never a throw/crash). Never enumerate or hardcode a parameter domain (a value list, a month-aligned date, a size range, a fixed set of words). Precompute for the *likely* values only as an optimization; a value the state did not precompute goes to a **fast exact path** (a bounded residual computation over compact state, or a window/partition-local scan), **never a per-call full rebuild**. Specifically:
- Dates/intervals: use the substrate date helpers (`sql_date`) — arbitrary day, Feb 29, month-end + interval, `interval` arithmetic per the engine's rules; no assuming month alignment, no assuming the data's year range.
- LIKE / string predicates: use `sql_like` — partial and mixed-case words, `_` and escapes, empty pattern, pattern chars inside data.
- Enumerated-type params: unknown value, two params equal (e.g. `X1 = X2`), value absent from data. Numeric params: outside the documented range, fractional where the column is integral-looking.
- Thresholds derived from other params (`> fraction * SUM(...)`) compare in exact arithmetic, not doubles.

**Data.** Do not assume the data stays in its documented domain: out-of-range dates, unseen categorical values, off-range numbers, NULLs, negative or huge keys must produce the same result as SQL (and writes of such rows must maintain state correctly).

**Exact numerics.** Decimals/money: integer-scaled or exact-decimal accumulation (`int128_acc`, `decimal_text`, engine numeric helpers), never `double`; never truncate a 128-bit accumulator to 64 bits; match the engine's rounding/scale rules for division and AVG.

**Key width and capacity.** Size every key, id, offset and count for the **top** scale factor (SF1000+: 64-bit keys, billions of rows, > 4 GB string/blob heaps), not the iteration DB. Forbidden: 32-bit or bit-packed keys with a fixed field width (e.g. 20-bit id), fixed dense caps (e.g. 2^28), 32-bit blob offsets, hashing a key into a partition id that can collide (partition by an exact function, or store the full key). Keys may be negative or sparse.

**Memory.** Target ~10-20 B/row of compact columnar state: no node-based maps, no `std::string`/per-row heap object per row, no per-row `shared_ptr`. Build without 2-5x transient copies (stream into chunks; no whole-table vector then re-copy).

**Per-write cost.** O(touched rows/chunks), via copy-on-write chunks that share untouched data. A refresh (~0.1% of rows, keys spread uniformly) touches nearly every chunk/shard/page, so the COW unit must itself cost O(changed rows) (pointer-copied entries, small chunks, hot/cold base+overlay), not a whole chunk per touched key; see `maintenance_framework.md` "Refresh-shaped writes". Never copy the whole state per batch (no `make_shared<State>(*old)` over O(rows) members: the state is versioned copy-on-write, a commit clones only what it touches), and never `RebuildAll` for a routine insert/delete/update that a delta can apply (rebuild only for what a delta truly cannot express). No hard-coded size guard may force a full rebuild outside the cost model; bound overlays by incremental compaction.

**Primitive selection and copy-on-write.** State must be versioned copy-on-write and built from the shared primitives in `accelerators/prims` (catalogue and the "Which one to use" guide: `prims/README.md`: COW arrays/maps/record stores/blob maps, sorted/Fenwick/radix structures, `sql_date`, `sql_like`, exact decimal/int128). Search it first and read the header comments of the candidates. If nothing fits, write a **generic** primitive named by what it is (not by the query), with the structured header comment (PURPOSE / COMPLEXITY / MEMORY / COW/THREADS / USE WHEN), a README row and randomized reference tests, **publish it with `primitive_publish`** (the only way `prims/` changes; additions only, workload-agnostic), then use it. Never hand-roll a private sharded/chunked COW container inside a module, and never deep-copy the state per commit. Workload-specific structures (an inverted index for a LIKE predicate, a query-specific cube layout) stay in the module but must still be COW, assembled from primitives.

**Acceptance evidence.** Beyond the seed binding, when the host has a parameter/data differential harness (e.g. `eval/tests/test_<engine>_<bench>_params.cpp`), run it for this query: plain (classes a/b/c) plus `--data-perturb` (rebuild and delta variants), `--wide-keys`, `--negative-keys`. Where none exists, write equivalent differential cases (off-spec params incl. NULL, off-spec rows, shifted/negated keys) in the unit/differential tests. A mismatch is a correctness failure, not a tolerance issue.

## Edit freedom

Edit allowlisted implementation files freely (coordinated refactors OK) under these constraints. Forbidden: hardcoding answers, semantic shortcuts, or otherwise gaming the benchmark.

Temporary **custom profiling** in editable files is encouraged: invent/split/rename phases and counters as needed, gated on `getenv("ACCEL_AUX_CUSTOM_PROFILE")` (must be a no-op when unset) and emitting `ACCEL_CUSTOM_PROFILE` JSON lines for digests. Ad-hoc shell/scripts for measurement are fine.
