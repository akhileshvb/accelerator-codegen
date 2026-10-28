# Auxiliary-state taxonomy

Classify a candidate by the expensive operator it removes and the read/write trade it adds. The goal is to eliminate the dominant scan, join, aggregation, sort or correlation and leave a sublinear or constant-time state lookup. Use it for candidate search (analysis) and when optimization considers another representation. `taxonomy_index.md` maps each family to its `design_family`; `aux_state_design.md` holds the rejection tests (scan-for-scan swap, full-result cache).

## Families

**1. Partitioned sufficient aggregates.** `partition_key x group_key -> sum, count, min, max, conditional sums`.
Best: SUM/COUNT/AVG, small group cardinality, range predicates on the partition key, read-heavy. Exact, compact, easy deltas.
Cost: a query may still merge many partitions (a linear scan of a smaller state).
Parameter generality: a partition grain is a coarse index, not the parameter domain. A bound off the grid (mid-partition, leap day, partition end plus interval, outside the populated range) combines whole-partition summaries for the interior with an exact residual at the edge partitions (finer-grain summaries or a bounded scan of the edge rows). Never answer only grid-aligned bounds or round to the grid. Accumulators are exact integer/decimal (128-bit), never `double`.

**2. Hierarchical aggregates.** The same statistics at several granularities; coarse summaries for complete regions, fine only at the boundaries.
Best: arbitrary ranges, many partitions, coarse and fine bindings. Low read amplification, exact boundaries via the finest level, point writes touch a bounded path.
Cost: more replication and write amplification, harder invariants. Use when the base scan is gone but flat merging still dominates.

**3. Prefix aggregates.** `prefix[k] = aggregate of partitions 0..k`; a range is `prefix[r] - prefix[l-1]` for invertible aggregates.
Best: static or append-mostly data, extremely read-heavy. O(1) reads.
Cost: an old-partition update changes every later prefix; poor for non-invertible aggregates. Index at the finest exact grain of the bound (day, not month) or add an exact edge residual; clamp out-of-range bounds (empty or full), never index out of bounds.

**4. Tree aggregates.** Aggregates in a tree or range index: O(log P) reads and writes. Best: dynamic prefix/range queries with arbitrary bounds and frequent point writes. Examples: `CowFenwick` over days, `RadixRangeTree` over a bounded integer domain (any bound exact).
Cost: complexity; batch writes touch many internal nodes. Use when flat summaries are read-bound and full prefixes too costly to maintain.

**5. Sparse range decompositions.** Summaries for chosen intervals (e.g. power-of-two blocks); a query combines a few canonical intervals. More combine work than prefixes. Use when a tree is overkill but flat partitions are too slow.

**6. Join-derived aggregates.** Aggregate state after the join (`join-derived partition x group -> statistics`). Best: large fact joined to small stable dimensions. Removes join and aggregation; usually the largest speedups. A dimension update fans out to many fact rows; use when the join is critical-path and dimension updates are rare.

**7. Materialized join intermediates.** Joined tuples or narrow projections (`join_key -> selected columns`). Best: several downstream predicates/groupings over one join, no single compact aggregate fits all bindings. Much larger than aggregates; the query still scans or indexes it; dimension updates may repair broadly.

**8. Narrow prefiltered subsets.** Only rows passing stable predicates, only needed columns. Best: selective stable predicates, wide rows, remaining predicates vary per call. Still a scan/aggregate/sort over the subset; poor if the predicate is not selective.

**9. Covering projections.** Exactly the needed columns in a query-friendly encoding or order. Best: wide tables, repeated scans that cannot be summarized. Duplicates columns and must track their updates; often a fallback.

**10. Hash indexes / lookup maps.** `key -> payload or row ids`. Best: equality predicates, key joins, point lookups; weak for ranges and ordering. Keys are 64-bit, possibly negative or sparse: never index a dense array by raw key without a bind-time bound; use `AnyKeyCowArray` / `FlatHashMap` (`prims/README.md` "Which one to use"). A probe for an absent key returns "no match", not an error.

**11. Ordered indexes.** `ordered_key -> rows, aggregates or references`. Best: range predicates, ordered retrieval, MIN/MAX, top-k with compatible ordering. Writes cost more than hash; broad ranges still return many entries.

**12. Bitmap indexes.** One bitset per low-cardinality value, combined with AND/OR/NOT. Best: categorical columns, many combined predicates, stable data. Values are discovered from data (an unseen value must still be handled). Pair with payload arrays or aggregates; too many bitmaps at high cardinality.

**13. Inverted indexes.** `value -> postings list`. Best: multi-valued attributes, membership, selective categorical conditions. Broad postings stay expensive; needs a second structure to aggregate.

**14. Top-k state.** `partition/group -> ordered top candidates`; merging small local lists removes the full sort. Best: `ORDER BY ... LIMIT K`, small fixed K, stable partitions. Deletes and score decreases may need candidates beyond top-k, and parameterized filters can make one list insufficient (fixes: slack, ordered indexes, hierarchical top-k).

**15. Threshold / candidate-set state.** Only rows that can enter the result, from sound bounds or thresholds. Best: top-k, HAVING, expensive scoring. Threshold changes trigger repair. Parameter-dependent thresholds (`> fraction * total`) compare in exact integer/decimal arithmetic; a parameter that makes the candidate set insufficient takes an exact bounded path, not a full rebuild.

**16. Per-entity derived state.** One record per entity (`account -> total`); entity count far below fact count; point updates via the parent key. Mistake: stopping here while the entity scan is still large; add a partition, range index or top-k.

**17. Reverse-dependency indexes.** `dimension_key -> affected groups/partitions`; maintenance-supporting, not query-serving. Turns RebuildAll into targeted delta or partition rebuild for join-derived state; costs storage, fan-out may stay large.

**18. Existence counts.** `entity_key -> matching_related_row_count`. Best: EXISTS / NOT EXISTS, anti-joins, correlated subqueries. Constant-time lookups, count-delta updates; updates need the old and new predicate classification.

**19. Semijoin / antijoin filters.** Keys with (or without) matches; compact, fast membership. Keep counts: a boolean is wrong when several matches exist and one is deleted.

**20. Histograms; 21. quantile/sketch state.** Approximate. For exact results a histogram is only coarse supporting state with exact edge handling, never the answer structure; sketches are usually not applicable.

**22. Min/max summaries.** `partition -> min/max or small ordered candidate set`. Removing the current extremum needs repair; one scalar is not enough under heavy deletes.

**23. Bloom filters.** No false negatives; avoid expensive negative probes; never the exact answer; deletes need variants or rebuilds.

**24. Denormalized records.** Dimension attributes copied onto fact/entity records; removes joins. Dimension updates fan out and storage duplicates; use when they are rare.

**25. Selectively replicated state.** Summaries replicated across several partitioning dimensions so each query uses an aligned path; every write updates all copies. Replicate only where read savings justify maintenance.

**26. Hybrid state.** Combinations (coarse hierarchy plus fine boundary partitions plus lookup index plus top-k). Removes several operators; highest speed and complexity, storage and write amplification; needs careful access-path selection. Most mature designs end here.

## Choosing

1. **Dominant base cost** to families:

| Cost | Families |
|---|---|
| Full-table scan | partition aggregates, prefiltered subsets, indexes |
| Range scan | hierarchies, prefixes, range trees, ordered indexes |
| Join | join-derived aggregate, denormalization, join intermediate |
| Grouped aggregation | sufficient aggregates, per-entity state |
| Sort / top-k | ordered state, local top-k, candidate sets |
| Correlated subquery | existence counts, semijoin/antijoin |
| Wide-row decoding | covering projection, narrow subset |
| Repeated parameter combinations | composable summaries, selective replication |

2. **Residual read path.** Removing the base scan is not enough. How many state entries are read, and does it grow with history, range width, groups or entities? Is a scan, join, aggregate or sort still present? Is every parameter value (off-grid, NULL, unseen, out of range) answered exactly without a per-call rebuild (`hard_guidelines.md` section 4)?
3. **Read/write regime.**

| Workload | Tendency |
|---|---|
| Nearly read-only | prefixes, replicated summaries, materialized rankings |
| Read-heavy, point writes | trees, hierarchies, join-derived summaries |
| Mixed | flat or hierarchical summaries with bounded deltas |
| Write-heavy | minimal indexes, compact aggregates, few replicas |
| Append-mostly | prefixes, cumulative summaries, immutable hierarchy |
| Frequent dimension changes | reverse indexes or fallback; avoid broad denormalization |
| Large batches | partition-rebuild-friendly organizations |

4. **Amplification.** Track read (state units per query), write (units modified per mutation) and storage (aux bytes / base bytes). Workload cost is `Q*C_read + W_point*C_point_update + W_batch*C_batch_repair + C_storage`; optimize `composite_score` of the write-frequency sweep (`aux_bench_sweep`), not read-only speed.
5. **Pareto.** Drop a candidate another matches on query time, update cost and size and beats strictly on one; keep several nondominated candidates when workload assumptions are uncertain.
