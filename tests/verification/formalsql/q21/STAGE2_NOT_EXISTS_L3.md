# Stage 2 — correlated NOT EXISTS(l3) layer

**Status: Qed — `Q21_NotExistsL3StageCorrespond`. No new preconditions / TCB.
GROUP BY / Project / Sort not started.** 8-conjunct fusion: see [`STAGE2_WHERE_FUSION.md`](STAGE2_WHERE_FUSION.md).

File: [`Q21NotExistsL3.v`](Q21NotExistsL3.v) (imports [`Q21ExistsL2.v`](Q21ExistsL2.v)).

| Milestone | Theorem | Status |
|-----------|---------|--------|
| slot / correlation discipline | `Q21_not_exists_l3_slot_layout`, `Q21_l3_no_capture`, `Q21_l3_inner_resolves`, `Q21_l3_outer_resolves` | Qed |
| A row-level l3 predicate | `Q21_not_exists_l3_row_correlation` | Qed |
| table-EXISTS helper | `q21_lineitem_filter_exists_exact` | Qed |
| B NOT EXISTS ↔ no matching late l3 | `Q21_not_exists_l3_acceptance` (Logos `scalar_expr_correlated_not_exists_antijoin_acceptance_exact`) | Qed |
| C FormalSQL filter rows | `Q21_not_exists_l3_filter_rows` (Logos `eval_filter_rows_correlated_antijoin_sources_exact`) | Qed |
| C RelNode anti-join | `Q21_anti_not_exists_l3_eval` (shared `join_anti_rows_eq_filter_negb_existsb`) | Qed |
| C bag / multiplicity | `Q21_not_exists_l3_bag` | Qed |
| D composed stage | `Q21_NotExistsL3StageCorrespond` (on `Q21_ExistsL2StageCorrespond`) | Qed |

Layout note: RelNode puts `late(l3)` on the anti-join's right input
(`scan_lineitem_late`), and `pred_not_exists_l3` is literally `pred_exists_l2`.
FormalSQL keeps `late` as the third subquery conjunct. The row-level theorem
matches FormalSQL's 3-way conjunct list to `late(bare l3) ∧ pred_not_exists_l3(l1 ++ l3)`,
and `existsb_filter_andb` turns the right-side filter into that conjunction.

Staging note: `q_not_exists_outer = Filter(NOT EXISTS)(Filter(EXISTS)(q_ordinary_outer))`.
Next obligation: prove it equivalent to the lowered single 8-conjunct `SExpr_ConjList`
(`q21_exists_l2_is_catalog_conjunct` and `q21_not_exists_l3_is_catalog_conjunct`
pin conjuncts p4 and p5 verbatim).

Print Assumptions: only the five stack axioms of `q1/allowed_assumptions.txt`;
`join_anti_rows_eq_filter_negb_existsb` and `existsb_filter_andb` are closed.
