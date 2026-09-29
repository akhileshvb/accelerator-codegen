# Stage 2 — Outer ordinary spine (no EXISTS)

**Status: Qed — `Q21_OuterOrdinarySpineCorrespond` proves
`Q21_OuterOrdinarySpineCorrespond_statement`
(= `Q21_CrossJoin_ConjList_JoinInner_Bag`). No new preconditions / TCB.
EXISTS / NOT EXISTS not started.**

File: [`Q21OuterOrdinarySpine.v`](Q21OuterOrdinarySpine.v)

---

## 1. Exact theorem statement

```rocq
Definition Q21_OuterOrdinarySpineCorrespond_statement : Prop :=
  forall (db : DB) (nation_name : string),
    Preconditions_Q21 db nation_name ->
    exists (D : db_state) (fts : list (tuple TNull)) (rows : list Row),
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (q_ordinary_outer nation_name) fts /\
      Eval_Spec filter_outer (Params_Q21 nation_name) db = Some rows /\
      bag_correspond_unordered slot_map_outer_cor fts rows.

(** Alias naming the semantic core: *)
Definition Q21_CrossJoin_ConjList_JoinInner_Bag : Prop :=
  (* definitionally equal to the statement above *)
  ...

Lemma Q21_OuterOrdinarySpineCorrespond_statement_eq_Bag :
  Q21_OuterOrdinarySpineCorrespond_statement =
  Q21_CrossJoin_ConjList_JoinInner_Bag.
(* Qed *)
```

**Body: not Qed.**

---

## 2. Intermediate lemmas

| Lemma | Status |
|-------|--------|
| `bag_correspond_unordered` / `_nil` / `_to_unordered` | Qed (SlotMap) |
| `eval_pred_keep_beq_int_Some` | Qed |
| `eval_pred_keep_beq_text_Some` | Qed |
| `eval_pred_keep_beq_param_text_Some` | Qed |
| `eval_pred_keep_bgt_date_Some` | Qed |
| `q21_empty_{table,cross_join,filter,project}_success` | Qed |
| `query_cross_join_empty_empty` | Qed |
| `q_ordinary_outer_shape_of_Some` / catalog spine | Qed |
| six `VC_*_3vl` bridges | Qed (prior) |
| **Milestone A** `Q21_row_correspond_select4_rename` / `Q21_bag_correspond_select4_rename` | **Qed** |
| **Milestone B** `Q21_ordinary_conjlist_acceptance_iff` / `Q21_ordinary_same_bit` | **Qed** |
| **Milestone C** `bag_correspond_unordered_filter` (+ Q21 instantiations) | **Qed** |
| **Milestone D** `Q21_four_way_cross_product_bag_correspond` | **Qed** |
| **E1** `Q21_FormalSQL_ordinary_outer_exec` | **Qed** |
| **E2** `Q21_filter_outer_eq_ordinary_filter_of_cross4` | **Qed** |
| E3 leaf: `Q21_scan_bags` (supplier/lineitem/orders/nation embeds) | **Qed** |
| E3 leaf: `q21_cross4_present_fresh_of_labels`, `q21_Forall_cross4_labels` | **Qed** |
| E3 leaf: `q21_cross4_member_cor_facts` (labels_ok / text_canonical) | **Qed** |
| **E3** `Q21_OuterOrdinarySpineCorrespond` | **Qed** |

---

## 3. CrossJoin multiplicity ↔ RelNode nested-loop

| FormalSQL | RelNode |
|-----------|---------|
| `query_cross_join_bag` = `Febag.mk_bag (brute_left_join_list …)` | nested `join_inner` (equijoin filter during nest) |
| `Febag.elements` → **canonical** order | left×right **nested-loop** order |
| `filter_brute_left_join_list_as_theta` → theta-join list | `join_inner` = nested-loop theta join |
| `raw_cross_same_rows_as_bag` | — |

Correspondence must be **multiset** (`bag_correspond_unordered`), not Forall2 order.
Cardinality: `|cross| = ∏ |side|`; survivors after join keys ⊆ that product
(same count as nested equijoin when accept flags match).

---

## 4. Project / rename correspondence

`q_ordinary_outer` = `Filter(ordinary_6) ∘ Project(scalar_select_list_4) ∘ Filter(True) ∘ spine`.

`scalar_select_list_4` renames native attrs → `__logos_cor_cor0_*`.
`slot_map_outer_cor` binds those cor attrs to RelNode slots 0..10.

**Open:** `eval_project_rows_exact_map` / `project_rows_success_of_exact_map`
+ per-row `row_correspond` transport native→cor (Q21-specific; not yet Qed).

---

## 5. ConjList ↔ `pred_ordinary_outer`

FormalSQL: `ordinary_outer_conjlist` = 6 `SExpr_Pred` (3 join eqs + status + late + nation).
RelNode: `pred_ordinary_outer` = same six as `BAnd` chain / RelJoin conds + `pred_outer_filters`.

Bridge path (ready, not glued):
- `scalar_expr_conj_list_acceptance_exact` + `pred_acceptance_of_value_list`
- six Qed `VC_*_3vl` under `row_correspond slot_map_outer_cor`
- RelNode `eval_pred_keep_*_Some` (always decides on typed rows)

---

## 6. Print Assumptions

E2, E3, `Q21_scan_bags`, `q21_cross4_member_cor_facts`: exactly the five
stack axioms already on `Preconditions_Q21` and in `q1/allowed_assumptions.txt`
(`ClassicalDedekindReals.sig_not_dec`, `ClassicalDedekindReals.sig_forall_dec`,
`ProofIrrelevance.proof_irrelevance`,
`FunctionalExtensionality.functional_extensionality_dep`,
`Classical_Prop.classic`). No Q21 `Admit`/`Axiom`.

---

## 7. New TCB / preconditions

**None.** E2 uses only `DB_ok_Q21` row widths; E3 uses `Preconditions_Q21`
as-is (DB_ok, NationNameOk, DB_CharCanonical, EmbedDB ≠ None).

E3 does not use `Q21_ordinary_bag_path_of_cross_product`: its per-pair
hypothesis quantifies over *all* `row_correspond` pairs (incl. `labels_ok`,
not implied by `row_correspond`), so it is not dischargeable. E3 composes its
constituents (D + A + generic C + B) with membership-restricted leaves instead.
FormalSQL keep bit is `q21_formal_ordinary_keep` =
`q21_ordinary_rel_keep ∘ rel_row_of slot_map_outer_cor`, which is exact on
every corresponding pair (`row_correspond_rel_row_of`, non-numeric slots).

---

## 8. Next blocker (precise)

Ordinary outer spine is closed; EXISTS(l2) layer is closed in
[`STAGE2_EXISTS_L2.md`](STAGE2_EXISTS_L2.md); NOT EXISTS(l3) is closed in
[`STAGE2_NOT_EXISTS_L3.md`](STAGE2_NOT_EXISTS_L3.md). The 8-conjunct WHERE fusion is closed in
[`STAGE2_WHERE_FUSION.md`](STAGE2_WHERE_FUSION.md). GROUP BY / COUNT is closed in [`STAGE2_GROUP_COUNT.md`](STAGE2_GROUP_COUNT.md). Final Project / ORDER BY and `NormalizeSound_Q21` are closed in [`STAGE2_FINAL.md`](STAGE2_FINAL.md).
