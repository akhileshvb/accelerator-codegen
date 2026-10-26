# State family index

Dominant base cost to candidate `design_family` (details: `aux_state_taxonomy.md`). Candidates must differ materially in family, composition rule or asymptotic read/write behavior; a payload tweak is not a new candidate.

| # | Family | `design_family` | Best when |
|---|---|---|---|
| 1 | Partitioned sufficient aggregates | `flat_partitioned_sufficient_summaries` | SUM/COUNT/AVG over partition-key ranges (exact edge residual for off-grid bounds) |
| 2 | Hierarchical aggregates | `multilevel_hierarchical_summaries` | wide ranges, expensive boundary merges |
| 3 | Prefix aggregates | `range_prefix_composable_summaries` | invertible aggregates, read-heavy |
| 4 | Tree/index aggregates | `tree_or_index_aggregate_organization` | ordered range queries with point writes |
| 5, 9-24 | Sparse ranges, covering projections, indexes, top-k, thresholds, per-entity, reverse-dependency, existence, semijoin, histograms, sketches, extrema, Bloom, denormalized | none (taxonomy section number) | see taxonomy |
| 6, 7 | Join-derived aggregates, materialized join intermediates | `compact_join_intermediate` | join (+agg) folded into compact state |
| 8 | Narrow prefiltered subsets | `narrow_prefiltered_subset` | heavy stable filter, small surviving set |
| 25 | Selectively replicated state | `selectively_replicated_summaries` | several access paths, read-heavy |
| 26 | Hybrid | `hybrid_multi_grain` | no single family removes all dominant cost (e.g. coarse interior plus exact day-level edges) |
