# Stage 2 — correlated EXISTS(l2) layer

**Status: Qed — `Q21_ExistsL2StageCorrespond`. No new preconditions / TCB.
GROUP BY / Project / Sort not started.** NOT EXISTS: see [`STAGE2_NOT_EXISTS_L3.md`](STAGE2_NOT_EXISTS_L3.md).

File: [`Q21ExistsL2.v`](Q21ExistsL2.v) (imports [`Q21OuterOrdinarySpine.v`](Q21OuterOrdinarySpine.v)).

| Milestone | Theorem | Status |
|-----------|---------|--------|
| slot / correlation discipline | `Q21_l2_no_capture`, `Q21_l2_inner_resolves`, `Q21_l2_outer_resolves`, `Q21_exists_l2_slot_layout`, `Q21_exists_l2_concat_slots` | Qed |
| A row-level l2 predicate | `Q21_exists_l2_row_correlation` | Qed |
| B EXISTS ↔ ∃ matching l2 | `Q21_exists_l2_acceptance` (via Logos `scalar_expr_correlated_exists_semijoin_acceptance_exact`) | Qed |
| C FormalSQL filter rows | `Q21_exists_l2_filter_rows` (via Logos `eval_filter_rows_correlated_semijoin_sources_exact`) | Qed |
| C RelNode semi-join | `Q21_semi_exists_l2_eval` (via shared `join_semi_rows_eq_filter_existsb`) | Qed |
| C bag / multiplicity | `Q21_exists_l2_bag` | Qed |
| D composed stage | `Q21_ExistsL2StageCorrespond` (on `Q21_OuterOrdinarySpineCorrespond_strong`) | Qed |

Staging note: `q_exists_outer = Filter(EXISTS l2)(q_ordinary_outer)` is a
staged decomposition. The lowered q̂ evaluates all 8 conjuncts in one
`SExpr_ConjList`; `q21_exists_l2_is_catalog_conjunct` pins the EXISTS
conjunct verbatim, and `Q21_exists_l2_acceptance` holds for every projected
product tuple (`labels_ok`), so it plugs into the 8-way ConjList fold.

Print Assumptions: only the five stack axioms of `q1/allowed_assumptions.txt`;
`join_semi_rows_eq_filter_existsb` is closed.
