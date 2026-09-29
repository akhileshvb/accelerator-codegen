> **Current public theorem:** `Q1FinalTheorem.NormalizeSound_Q1_rel_all_outcomes`
> (FormalSQL outcome = Embed of the RelNode `Eval_Spec` rows; no numeric
> premise). The AuxAnswerSpec statements below are proof-internal steps.
> Canonical summary: `../../q1/CURRENT_THEOREM_TCB.md`.

# Q1 FormalSQL status

**Canonical theorem / TCB:** [`../../q1/CURRENT_THEOREM_TCB.md`](../../q1/CURRENT_THEOREM_TCB.md).  
Narrative proof flow: [`../../q1/PROOF_FLOW.md`](../../q1/PROOF_FLOW.md).

```text
templatized Cat(Q1)[delta] --Logos shape--> qhat_Q1(delta)
        |
        | Qed obligations + leaf Props (∀ DeltaOk delta)
        v
 QuerySpineFacade → AuxAnswerSpec(DB, EmbedParams(delta))
        |
        | Dafny Twin≡Spec → runtime → C++
```

**Parameter:** `delta` = INTERVAL days in  
`l_shipdate <= DATE '1998-12-01' - INTERVAL '<delta>' DAY`.  
Catalog fill-in: `delta = 90` (`qhat_Q1 90 = Queries.source_query_expr`).  
Claim is **∀ `DeltaOk_Q1 delta`**, not only 90.

## Live Rocq files

| File | Role |
|------|------|
| `../lib/QuerySpineFacade.v` | Generic TFGOP `EQuery_*` once |
| `../lib/FilterAcceptanceFacade.v` | `eval_filter_rows_acceptance_exact` + DATE≤TIMESTAMP↔`Z.leb` |
| `../lib/TemporalPredAcceptanceFacade.v` | DATE ≤ CAST+INTERVAL pred acceptance (leaf/call exact once) |
| `../lib/NumericScaledFits.v` | of_scaled decimal_parts + digit-budget mono for scaled2 mul / charge |
| `../lib/NumericPackagingFacade.v` | Shape-based SUM + `decimal15_2_avg_div_runtime_safe` (AVG finalize) |
| `../lib/NumericCallExactFacade.v` | NUMERIC Mul/Sub/Add/Cast + `$f4`/`$f5` shapes + max→price mono |
| `../lib/ProjectExactFacade.v` | `eval_project_rows_exact_map` wrap |
| `../lib/OrderRowsFacade.v` | Canonical `order_by_rows` / `sort_ordered_rows` |
| `../lib/KeyedSortFacade.v` | Generic sorted-perm uniqueness (D) |
| `../lib/SortedGroupResultFacade.v` | Perm + list-local le_trans + key antisym ⇒ `sort_ordered_rows` eq (discharges `Q1_GroupBagsPerm`; reusable for Q12) |
| `../lib/GroupBagExactFacade.v` | `EGroupBag_Success` from exact groups |
| `../lib/GroupsExactFacade.v` | `eval_groups_outcome_exact_filter_map` + bag combine; HAVING true |
| `../lib/AggregateLeafExactFacade.v` | COUNT-star / SUM(dot) / AVG(fixed)(dot) leaf exact from Logos bridges |
| `Q1FormalSQLProof.v` | Embed, `qhat_Q1`, Pre, NoError, empty AuxCorrect |
| `Q1GroupingCorrespondence.v` | Group/Count/Sum wraps |
| `Q1AuxCorrectAssembly.v` | SpecView↔filter + Embed dots |
| `Q1Obligations.v` | FilterEq … OrderEq |
| `Q1FormalSQLSpine.v` | Leaf Props + `of_leaves` + thin facade reductions |
| `Q1NumericEmbed.v` | `NumericEmbedFaithful` (TCB representation Prop) |
| `Q1GroupsCorrespond.v` | `Q1_GroupsCorrespond` + bag↔key building blocks |
| `Q1TCBAuxCorrect.v` | D outer bridge + restated `FormalSQLAuxCorrect_Q1` |

## Qed

| Item | Status |
|------|--------|
| `qhat_Q1_catalog_instance` (`delta=90`) | **Qed** |
| FilterEq … OrderEq | **Qed** |
| `FormalSQLNoError_Q1` (∀ delta under Pre + Safe) | **Qed** |
| Empty-DB AuxCorrect (∀ delta) | **Qed** |
| `FormalSQLAuxCorrect_Q1_of_leaves` | **Qed** (not TCB) |
| `Q1_FilterRowsOk` / `Q1_FilterRowsOk_holds` | **Qed** |
| `Q1_FilterRowsOk_nil` | **Qed** |
| `Q1_OrderOk_canonical` / `Q1_OrderOk_nil` | **Qed** |
| `Q1_OuterProjectOk_holds` / `Q1_OuterProjectOk_nil` | **Qed** (under group-output labels) |
| `Q1_InnerProjectOk_holds` / `Q1_InnerProjectOk_nil` | **Qed** (digit-budget mono + NumericCallExactFacade) |
| Thin reductions `*_of_exact_map` / `*_of_exact_groups` / `*_of_exact_filter_map` | **Qed** |
| `Q1_GroupBagOk_nil` + key decode / Dot-key runtime None | **Qed** |
| `tnull_avg_numeric_fixed_dot_leaf_exact` (+ closed-group value/runtime) | **Qed** |
| `Q1_GroupBagOk_holds` | **Qed** under per-bucket `q1_group_row_labels_ok` + `q1_group_bucket_safe` |
| Inner/group project labels from Pre-derived rows | **Qed** |
| COUNT packaging of `q1_group_bucket_safe` from Pre | **Qed** (`q1_group_bucket_count_safe_of_pre`) |
| SUM(qty)/SUM(price) from Pre | **Qed** (`q1_group_sum_*_safe_of_pre` + DECIMAL conforms) |
| SUM(`$f4`/`$f5`) from Pre | **Qed** (`q1_group_sum_f4/f5_safe_of_pre` via `NumericPackagingFacade`) |
| AVG(qty/price/disc) from Pre | **Qed** (`q1_group_avg_*_safe_of_pre` via `decimal15_2_avg_div_runtime_safe`) |
| `q1_group_bucket_safe_of_pre` | **Qed** — full Safe incl. AVG, **proved not TCB** |
| Inner-project dots qty/price/disc/`$f4`/`$f5`/rf/ls | **Qed** |
| Group member ↔ AuxSpec row inverse | **Qed** (`q1_group_member_aux_row`) |
| SpecView key **count** / **sum_*** Z bridges | **Qed** |
| Bag↔key blocks (`q1_inner_grouping_key`, `q1_groups_of_aux_bucket`, …) | **Qed** |
| D: `q1_final_rows_eq_of_groups` (outer of group-tuple = EmbedOutRow) | **Qed** |
| `FormalSQLAuxCorrect_Q1_of_holds` | **Qed** under final-list equality (Safe from Pre) |
| `FormalSQLAuxCorrect_Q1` | **Qed** under Pre + `Q1NumericSafe` + `NumericEmbedFaithful` |
| `Q1_GroupsCorrespond_holds` | **Qed** from Pre + Faithful (`Q1_GroupBagsPerm` discharged) |

## Residuals (∀ delta)

| Item | Status | Remaining |
|------|--------|-----------|
| `Q1_GroupBagsPerm` | **Qed** (`Q1_GroupBagsPerm_holds`) | via SortedGroupResultFacade |
| `NumericEmbedFaithful` | **TCB hyp** | Z folds ↔ FormalSQL NUMERIC SUM/AVG cells (representation only) |

Full TCB taxonomy: [`../../q1/CURRENT_THEOREM_TCB.md`](../../q1/CURRENT_THEOREM_TCB.md).

### SpecView / value gap (do not fake)

| Column / bridge | Status |
|-----------------|--------|
| Filter membership / key stats | **Qed** |
| **count** / Z sum columns | **Qed** |
| Bag↔key FormalSQL groups ↔ Aux `RowKey` buckets | **Qed** (`q1_groups_of_aux_bucket`, key-eq under byte Pack) |
| Inner-of-aux dots = embed maps (qty/price/disc/`$f4`/`$f5`) | **Qed** |
| SpecView Z folds = Aux bucket folds (count + sums) | **Qed** (`q1_aux_bucket_*`) |
| Numeric cell embed (scale 2/4/6 + AVG) | **TCB** (`NumericEmbedFaithful`) |
| Per-bucket `q1_group_select_values` = `q1_group_values_of_OutRow` | **Qed** (`q1_group_select_values_of_rev_bucket`) |
| SpecFinalize ↔ FormalSQL group obs (sorted bag) | **Qed** (`Q1_GroupBagsPerm_holds`) |
| `sort_ordered_rows` ↔ `OutRowSort` (order embedding) | **Qed** (`sort_ordered_rows_map_OutRowSort`) |
| Ordered group obs = map of AuxAnswerSpec embeds | **Qed** (`Q1_GroupsCorrespond_holds`) |
| Outer project of those embeds = EmbedResult | **Qed** (D) |

## Next

1. Prefer `embed_avg_from_sum_scale2` ↔ `interp_avg_numeric_fixed` inside NumericEmbed.

## TCB

Canonical: [`../../q1/CURRENT_THEOREM_TCB.md`](../../q1/CURRENT_THEOREM_TCB.md) §4–§5.

> **A (overflow Safe):** `q1_group_bucket_safe_of_pre` — **Qed from Pre** (COUNT + SUM×4 + AVG×3). **Not TCB.**  
> **`Q1NumericSafe DB`:** TPC-H domain + cardinality ⇒ no SqlError (overflow packaging; never values). `DeltaOk_Q1` is Rocq Pre.  
> **B `NumericEmbedFaithful`:** representation only — Z aggregate coefficients ↔ FormalSQL NUMERIC cells (SUM scale 2/4/6; AVG via `embed_avg_from_sum_scale2`). Does **not** choose group membership. Theorem hypothesis (not `Axiom`).  
> **C `Q1_GroupsCorrespond`:** **Qed** from Pre + Faithful (`Q1_GroupBagsPerm` discharged via SortedGroupResultFacade).  
> **D:** generic outer embed of group-tuple OutRow = `EmbedOutRow` — **Qed** (`q1_final_rows_eq_of_groups`).

### Final theorem shape

```coq
Theorem FormalSQLAuxCorrect_Q1 :
  forall db delta,
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    FormalSQL_exec ... (EmbedResult_Q1 (AuxAnswerSpec ...)).
```

**Strengthened (forall outcomes):** `NormalizeSound_Q1_all_outcomes` in
[`Q1AllOutcomes.v`](Q1AllOutcomes.v) proves that EVERY possible FormalSQL outcome of q̂ equals
`SqlSuccess outs`, under the same premises (no new premise, no new axiom).
See [`../ALL_OUTCOMES.md`](../ALL_OUTCOMES.md).

## Gates

`compile_q1_proof.sh` · `compile_q1_normalize_sound.sh` · `check_q1_rocq_no_admitted.sh`
