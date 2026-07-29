# Auxiliary-state taxonomy

A useful way to classify auxiliary state is by **which expensive operator it removes** and what **read/write tradeoff** it introduces. Maximum speedup usually comes from eliminating the dominant scan, join, aggregation, sort, or correlation while keeping the remaining state lookup **sublinear or constant-time**.

Use this catalog during **analysis candidate search** and when **optimization** considers alternate representations. Map each candidate to one or more families below; prefer designs that remove the dominant base cost without introducing a scan-for-scan swap (see `aux_state_design.md`).

---

## 1. Partitioned sufficient aggregates

Store algebraic statistics by a natural partition and grouping key:

```
partition_key × group_key → sum, count, min, max, conditional sums
```

Examples: daily revenue totals, counts by status, sums by region.

**Best for:** SUM, COUNT, AVG; small fixed group cardinality; range predicates aligned with partitions; rows mapping to one or a few partitions; read-heavy workloads.

**Advantages:** Compact; easy delta maintenance; exact; often eliminates scan, join, and aggregation.

**Risk:** A query may still merge many partitions. Flat partitioned state can replace a large table scan with a smaller but still linear state scan.

**TPC-H-like fits:** Q1, Q6, Q12, Q14.

**Maps to `design_family`:** `flat_partitioned_sufficient_summaries`.

---

## 2. Hierarchical aggregates

Maintain the same sufficient statistics at multiple granularities (e.g. year → month → day). A query combines coarse summaries for complete regions and fine summaries only at boundaries.

**Best for:** Arbitrary range predicates; many historical partitions; queries where flat partition merging is expensive; workloads needing both coarse and fine parameter bindings.

**Advantages:** Much lower read amplification than flat partitions; boundary handling remains exact; point writes update only a bounded path or small number of levels.

**Tradeoff:** More state replication; greater write amplification; more complicated consistency invariants.

**Use for maximum speedup when:** The base scan has already been eliminated, but the flat aux path still spends significant time merging partitions.

**Maps to `design_family`:** `multilevel_hierarchical_summaries`.

---

## 3. Prefix aggregates

Store cumulative sufficient statistics: `prefix[k] = aggregate of partitions 0..k`. A prefix query is one lookup. A range query may use `prefix[right] - prefix[left - 1]` when the aggregate supports subtraction.

**Best for:** Static or append-mostly data; frequent prefix or range queries; additive/invertible aggregates; extremely read-heavy workloads.

**Advantages:** O(1) or near-constant query cost; often the highest possible read speed for range aggregates.

**Tradeoff:** An old-partition update may affect all later prefixes; high write amplification; difficult for non-invertible aggregates.

**Use for maximum speedup when:** Queries vastly outnumber writes and historical updates are rare.

**Maps to `design_family`:** `range_prefix_composable_summaries`.

---

## 4. Tree-based aggregate state

Store aggregates in a compositional tree or range index. Queries combine O(log P) nodes; writes update a logarithmic path.

**Best for:** Dynamic prefix and range queries; arbitrary cutoff or interval parameters; workloads requiring both fast reads and frequent point writes; associative aggregates.

**Advantages:** Typically O(log P) reads and writes; better balanced than flat summaries or fully materialized prefixes; exact and compositional.

**Tradeoff:** More implementation complexity; more state nodes than flat summaries; batch writes may touch many internal nodes.

**Use for maximum speedup when:** Flat summaries are read-bound, while full prefixes are too expensive to maintain.

**Maps to `design_family`:** `tree_or_index_aggregate_organization`.

---

## 5. Sparse range decompositions

Store summaries for selected intervals (e.g. power-of-two blocks) rather than every possible range.

**Best for:** Range aggregation; workloads with many different interval widths; cases where compact state matters more than absolute minimum lookup latency.

**Advantages:** Good storage/query balance; queries combine a small number of canonical intervals; supports dynamic maintenance.

**Tradeoff:** More combination work than exact prefix materialization; requires careful interval selection.

**Use when:** A full tree abstraction is unnecessary but flat partitions are too slow.

---

## 6. Join-derived aggregates

Precompute aggregate state after an expensive join:

```
join-derived partition × group → aggregate statistics
```

Example: revenue grouped by a dimension attribute after joining fact and dimension tables.

**Best for:** Star-schema queries; repeatedly joining a large fact table with small dimensions; stable join keys; queries whose expensive work is both join and aggregation.

**Advantages:** Eliminates repeated join and aggregation; can reduce millions of joined rows to small summaries; usually produces very large speedups.

**Tradeoff:** Dimension-table updates may have high fan-out (one dimension row affects many fact rows).

**Use for maximum speedup when:** The join is on the critical path and dimension updates are relatively uncommon.

**Maps to `design_family`:** `compact_join_intermediate` (when aggregated) or related hybrid forms.

---

## 7. Materialized join intermediates

Store joined tuples or narrow joined projections rather than immediately aggregating:

```
join_key → selected joined columns
```

**Best for:** Multiple downstream predicates or groupings over the same join; queries where no single compact aggregate answers all bindings; expensive joins with relatively selective downstream work.

**Advantages:** Removes join cost; retains flexibility for parameter-dependent filtering and grouping; can serve a broader parameter space.

**Tradeoff:** Much larger than aggregate state; query still scans or indexes joined intermediates; dimension updates may trigger broad repair.

**Use when:** A compact final aggregate would lose information needed for varying query bindings.

**Maps to `design_family`:** `compact_join_intermediate`.

---

## 8. Narrow prefiltered fact subsets

Store only rows satisfying stable predicates, with only columns needed downstream:

```
stable_predicate_matches → narrow projected rows
```

**Best for:** Highly selective stable predicates; expensive decoding of wide base rows; queries whose remaining predicates vary per execution; cases where aggregation cannot be fully precomputed.

**Advantages:** Reduces scanned rows and row width; easier to maintain than a complex derived aggregate; preserves flexibility.

**Tradeoff:** Still requires a scan, aggregation, or sort over the subset; poor when the stable predicate is not selective.

**Use for maximum speedup when:** Most base rows can be permanently excluded but the surviving computation remains parameter-dependent.

**Maps to `design_family`:** `narrow_prefiltered_subset`.

---

## 9. Covering projections

Store a compact projection containing exactly the columns required by the query, often in a query-friendly encoding or order.

**Best for:** Wide tables; queries reading a small subset of columns; expensive base storage decoding; repeated scans that cannot be summarized.

**Advantages:** Smaller memory footprint and bandwidth; better cache locality; can combine with sorting, partitioning, or compression.

**Tradeoff:** Usually less speedup than eliminating the scan entirely; duplicates base columns; must track updates to every projected column.

**Often a fallback** when stronger semantic summaries are not possible.

---

## 10. Hash indexes and lookup maps

Map equality keys directly to rows, row identifiers, aggregates, or state partitions:

```
key → payload or matching row IDs
```

**Best for:** Equality predicates; key-based joins; point lookup queries; sparse access into large state.

**Advantages:** Expected constant-time lookup; eliminates scan and often hash-table construction; straightforward point maintenance.

**Tradeoff:** Weak for ranges, ordering, and prefix queries; high-cardinality keys can consume substantial memory.

**Use for maximum speedup when:** Query selectivity is high and access is dominated by equality probes.

---

## 11. Ordered indexes

Maintain an ordered mapping from a predicate or ordering key to payloads:

```
ordered_key → rows, aggregates, or references
```

**Best for:** Range predicates; ordered retrieval; MIN/MAX; limited result ranges; top-k queries with compatible ordering.

**Advantages:** Avoids full scans; supports range pruning; can avoid sorting.

**Tradeoff:** More expensive writes than hash indexes; may still retrieve many entries for broad ranges.

---

## 12. Bitmap or bitset indexes

Store a bit vector per low-cardinality predicate value:

```
attribute_value → matching-row bitmap
```

Queries combine bitmaps with AND, OR, and NOT.

**Best for:** Low- or moderate-cardinality categorical columns; combinations of many predicates; analytical filtering; relatively stable data.

**Advantages:** Extremely fast predicate composition; compact with compression; efficient vectorized execution.

**Tradeoff:** Not sufficient alone for many aggregations; updates can be less convenient; high-cardinality attributes may produce too many bitmaps.

**Best paired with** narrow payload arrays or aggregate structures.

---

## 13. Inverted indexes

Map values or tokens to matching rows or entities:

```
value → postings list
```

**Best for:** Multi-valued attributes; membership predicates; text-like search; selective categorical conditions.

**Advantages:** Avoids scans for sparse matches; supports intersections and unions.

**Tradeoff:** Broad postings lists can still be expensive; usually needs a second structure for aggregation.

---

## 14. Top-k state

Maintain the highest-ranked candidates under a grouping or predicate:

```
partition/group → ordered top candidates
```

**Best for:** `ORDER BY ... LIMIT K`; small fixed K; repeated rankings over stable partitions; expensive full sorting.

**Advantages:** Eliminates sorting over the full candidate set; query can merge small local top-k lists; often dramatic for large grouped outputs.

**Tradeoff:** Deletions and score reductions may require candidates beyond the current top-k; parameterized filters can make one top-k list insufficient.

**Common solutions:** Extra slack, full ordered indexes, or hierarchical top-k summaries.

**TPC-H-like fits:** Q3, Q10, Q18.

---

## 15. Threshold and candidate-set state

Maintain only rows capable of entering a final result, based on bounds or precomputed thresholds.

**Best for:** Top-k; HAVING predicates; expensive scoring functions; cases where most groups can be safely pruned.

**Advantages:** Much smaller than storing all outputs; can avoid full aggregation or ranking.

**Tradeoff:** Correctness requires sound bounds; threshold changes can trigger repair or expansion.

---

## 16. Per-entity derived state

Maintain one derived record per logical entity (e.g. order → revenue; customer → returned-order revenue).

**Best for:** Queries that aggregate many fact rows into entity-level values; repeated grouping or ranking over entities; entity cardinality much smaller than fact-table cardinality.

**Advantages:** Eliminates repeated fact-level aggregation; useful substrate for additional indexes or top-k structures; natural point updates when fact rows identify their parent entity.

**Tradeoff:** Query may still scan all entities; often needs a secondary partition, range index, or top-k structure for maximum speedup.

**Design mistake:** Stopping here when the remaining entity scan is still large.

---

## 17. Reverse-dependency indexes

Map a dimension or parent key to all derived-state entries affected by its mutation. Typically **maintenance-supporting** state rather than query-serving state.

```
dimension_key → affected fact groups or partitions
```

**Best for:** Join-derived state; dimension updates; high-fan-out dependency tracking; avoiding full rebuilds.

**Advantages:** Makes incremental repair possible; bounds the impact of updates; can turn RebuildAll into targeted delta or partition rebuild.

**Tradeoff:** Additional storage and maintenance; fan-out may still be intrinsically large.

**Can indirectly increase workload speedup** by reducing write costs.

---

## 18. Membership and existence counts

Maintain counts or flags proving whether related rows exist:

```
entity_key → matching_related_row_count
```

**Best for:** EXISTS, NOT EXISTS, anti-joins, correlated subqueries, “at least one” or “none” conditions.

**Advantages:** Converts correlated scans into constant-time lookups; easy to update using count deltas; often much smaller than storing matching rows.

**Tradeoff:** More complex for multiple correlated conditions; updates may need old and new predicate classification.

**TPC-H-like fits:** Q21 and parts of other existence-heavy queries.

---

## 19. Semijoin or antijoin filters

Store keys known to have—or not have—matching rows (`matching_keys` / `nonmatching_keys` sets).

**Best for:** Semijoin and antijoin predicates; eliminating large portions of one input before a join; stable relationship predicates.

**Advantages:** Compact; can remove expensive join work; fast membership checks.

**Tradeoff:** Counts may be required for correct deletion handling; a simple boolean is insufficient when multiple matches exist.

---

## 20. Histograms and distributions

Maintain counts or sums across value buckets:

```
value_bucket → count or aggregate
```

**Best for:** Approximate analytics; selectivity estimation; coarse parameter ranges; queries where bounded approximation is acceptable.

**Advantages:** Very compact; fast range estimates; low maintenance cost.

**Tradeoff:** Usually approximate; poor for exact arbitrary boundaries unless buckets are fine enough.

For **exact** TPC-H results, histograms are normally supporting state rather than the final answer structure.

---

## 21. Quantile and sketch state

Mergeable quantile structures, distinct-count sketches, frequency sketches.

**Best for:** Approximate percentiles; approximate distinct counts; heavy hitters; streaming or high-write workloads.

**Advantages:** Tiny state; mergeable; very fast queries.

**Tradeoff:** Approximate semantics; some structures do not support deletions cleanly.

Usually **not applicable** when exact benchmark equality is required.

---

## 22. Min/max and extrema summaries

Store minimum, maximum, or small ordered candidate sets by partition:

```
partition → min/max or lowest/highest candidates
```

**Best for:** Extrema queries; pruning partitions; range exclusion; ordered limits.

**Advantages:** Very small; cheap reads; can skip large portions of data.

**Tradeoff:** Removing the current min/max may require repair; a single scalar is not enough for robust delete-heavy maintenance.

---

## 23. Bloom filters and probabilistic membership filters

Compact approximate test for whether a key may exist.

**Best for:** Avoiding expensive negative probes; joins with many nonmatching keys; distributed or storage-heavy access; supporting a stronger exact path.

**Advantages:** Tiny and fast; no false negatives.

**Tradeoff:** False positives; cannot produce exact answers alone; deletion requires variants or rebuilds.

These accelerate access but are rarely the main query-answering state.

---

## 24. Denormalized records

Maintain related dimension attributes alongside fact or entity records:

```
fact/entity → copied dimension attributes
```

**Best for:** Repeated joins to slowly changing dimensions; predicates and grouping on dimension attributes; join cost dominating execution.

**Advantages:** Removes joins; improves locality; simplifies downstream filtering.

**Tradeoff:** Dimension updates fan out across many records; storage duplication; potentially expensive transactional maintenance.

**Use when:** Dimension updates are rare relative to reads.

---

## 25. Selectively replicated state

Replicate only especially useful summaries across multiple partitioning dimensions (e.g. by date, category, and region).

**Best for:** Multiple common access paths; workloads concentrated on a few parameter dimensions; read-heavy analytical systems.

**Advantages:** Each query uses an access path aligned with its predicates; avoids expensive reshuffling or scans.

**Tradeoff:** Every write updates multiple copies; selection should be workload-aware; storage can grow quickly.

For maximum speedup, replicate only structures whose expected read savings justify maintenance.

**Maps to `design_family`:** `selectively_replicated_summaries`.

---

## 26. Hybrid state

Combine multiple representations, e.g. coarse aggregate hierarchy + fine-grained boundary partitions + lookup index + top-k summaries.

**Best for:** Queries combining range filtering, grouping, joins, and ranking; broad parameter distributions; workloads with multiple distinct regimes; cases where one representation leaves a clear residual bottleneck.

**Advantages:** Can remove several operators simultaneously; supports specialized fast paths; often achieves the highest absolute query speed.

**Tradeoff:** Highest implementation and correctness complexity; more storage; greater write amplification; requires careful runtime action and access-path selection.

**Most mature accelerator designs eventually become hybrids.**

**Maps to `design_family`:** `hybrid_multi_grain`.

---

## Choosing for maximum speedup

### Step 1: Identify the dominant base cost

| Dominant cost | State families to consider |
|---------------|---------------------------|
| Full-table scan | Partition aggregates, prefiltered subsets, indexes |
| Range scan | Hierarchies, prefixes, range trees, ordered indexes |
| Join | Join-derived aggregate, denormalization, join intermediate |
| Grouped aggregation | Sufficient aggregates, per-entity state |
| Sort / top-k | Ordered state, local top-k, candidate sets |
| Correlated subquery | Existence counts, semijoin/antijoin state |
| Wide-row decoding | Covering projection, narrow subset |
| Repeated parameter combinations | Composable summaries, selective replication |

### Step 2: Check the residual read path

A design is not necessarily good merely because it removes the base scan. Ask:

- How many state entries are read?
- Does that grow with history, range width, groups, or entity count?
- Is another scan, join, aggregate, or sort still present?
- Can a hierarchy, index, or second-level summary eliminate it?

### Step 3: Match the read/write regime

| Workload | Preferred tendency |
|----------|-------------------|
| Nearly read-only | Prefixes, replicated summaries, materialized rankings |
| Read-heavy with point writes | Trees, hierarchical aggregates, join-derived summaries |
| Mixed reads/writes | Flat or hierarchical summaries with bounded deltas |
| Write-heavy | Minimal indexes, compact aggregates, fewer replicas |
| Append-mostly | Prefixes, cumulative summaries, immutable hierarchy |
| Frequent dimension changes | Avoid broad denormalization; use reverse indexes or fallback |
| Large batches | Partition rebuild-friendly organizations |

### Step 4: Evaluate state amplification

The highest query speed is not always the highest **workload** speed. Track:

- **Read amplification** = state units read per query
- **Write amplification** = state units modified per base mutation
- **Storage amplification** = auxiliary bytes / base-data bytes

Approximate workload cost:

```
C ≈ Q·C_read + W_point·C_point_update + W_batch·C_batch_repair + C_storage
```

where Q is query rate and W_* are write rates. The write-frequency sweep (`aux_bench_sweep`) measures end-to-end behavior across read/write mixes; optimization should improve **composite_score**, not read-only speed alone.

### Step 5: Prefer Pareto-optimal designs

A state design is uninteresting when another candidate is: no slower to query; no more expensive to update; no larger; and strictly better in at least one category.

The analysis agent should preserve several **nondominated** candidates when workload assumptions are uncertain.

---

## Practical hierarchy of speedups

Expected speedup generally increases as more dominant work is removed:

1. **Projection or pruning only** — reduces bandwidth but retains much of the query.
2. **Index into base rows** — avoids broad scans but still executes remaining operators.
3. **Materialized join or per-entity intermediate** — removes joins or repeated lower-level aggregation.
4. **Partitioned sufficient aggregate** — removes scans, joins, and aggregation, but may merge many partitions.
5. **Hierarchical / tree-composable aggregate** — also reduces auxiliary read complexity.
6. **Hybrid range + grouping + ranking state** — removes nearly all dominant operators; leaves only bounded finalization.
