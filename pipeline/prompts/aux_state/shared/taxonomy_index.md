# State family index (read `aux_state_taxonomy.md` for full detail)

Map dominant base cost → candidate `design_family`. Pick **materially different** families per candidate.

| # | Family | `design_family` | Best when |
|---|--------|-----------------|-----------|
| 1 | Partitioned sufficient aggregates | `flat_partitioned_sufficient_summaries` | SUM/COUNT/AVG, partition-key ranges (exact edge residual for off-grid bounds) |
| 2 | Hierarchical aggregates | `multilevel_hierarchical_summaries` | Wide date ranges, boundary merges expensive |
| 3 | Prefix aggregates | `range_prefix_composable_summaries` | Prefix/range on invertible aggregates, read-heavy |
| 4 | Tree/index aggregates | `tree_or_index_aggregate_organization` | Ordered range queries on aggregates |
| 5 | Sparse range decompositions | (see taxonomy §5) | Sparse partition touch sets |
| 6 | Join-derived aggregates | `compact_join_intermediate` | Join+agg folded into compact state |
| 7 | Materialized join intermediates | `compact_join_intermediate` | Reuse join result across queries |
| 8 | Narrow prefiltered subsets | `narrow_prefiltered_subset` | Heavy base filter, small surviving set |
| 9 | Covering projections | (see taxonomy §9) | Column subset serves query without base scan |
| 10–12 | Hash / ordered / bitmap indexes | (see taxonomy §10–12) | Lookup or membership-heavy paths |
| 13 | Inverted indexes | (see taxonomy §13) | Token/attribute → entity sets |
| 14 | Top-k state | (see taxonomy §14) | Bounded output ranking |
| 15–16 | Threshold / per-entity derived | (see taxonomy §15–16) | Entity-scoring bottlenecks |
| 17–19 | Reverse-dep / membership / semijoin | (see taxonomy §17–19) | Existence or dependency pruning |
| 20–22 | Histograms / quantiles / extrema | (see taxonomy §20–22) | Distribution or min-max bottlenecks |
| 23–25 | Bloom / denormalized / replicated | (see taxonomy §23–25) | Probabilistic or replication tradeoffs |
| 26 | Hybrid multi-grain | `hybrid_multi_grain` | No single family removes all dominant cost |

Avoid **scan-for-scan swaps** (aux path still O(n) over state). See `aux_state_design.md`.
