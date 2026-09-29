# Stage 2 — exact 8-conjunct WHERE fusion

**Status: Qed — `Q21_catalog_where_fusion`. No new preconditions / TCB.
final Project / Sort not started.** GROUP BY / COUNT: see [`STAGE2_GROUP_COUNT.md`](STAGE2_GROUP_COUNT.md).

File: [`Q21WhereFusion.v`](Q21WhereFusion.v) (imports `Q21NotExistsL3.v`).

| Result | Theorem | Status |
|--------|---------|--------|
| recognition from q̂ (every nation) | `Q21_catalog_where_shape` | Qed |
| ordinary six = conjuncts 1–4, 7–8; EXISTS = 5; NOT EXISTS = 6 | `Q21_catalog_where_ordinary_positions` | Qed |
| row-level exact acceptance | `Q21_catalog_where_row_acceptance` | Qed |
| boolean form | `Q21_catalog_where_accept_true_iff` | Qed |
| Filter(8-way WHERE) over outer rows | `Q21_catalog_where_filter_rows` | Qed |
| stacked filters = fused filter (list equality) | `Q21_stacked_where_filters_eq` | Qed |
| catalog stage ≡ staged query, ↔ AntiJoin rows | `Q21_catalog_where_fusion` | Qed |

Scope of "equivalent": `FormalSQL_exec` is possible-outcome semantics. The
fusion theorem exhibits one `fts` that is a success outcome of BOTH the exact
catalog WHERE stage and the staged query, and that bag-corresponds to the
RelNode anti-join rows; this is the existential shape `NormalizeSound_Q21`
consumes. It does not claim the two queries have identical outcome sets.

Print Assumptions: exactly the five names in `q1/allowed_assumptions.txt`.
