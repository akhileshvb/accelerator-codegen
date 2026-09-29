# All-outcomes strengthening (Q1, Q21)

**Status: Qed for Q1 and Q21.**

## What changed

`FormalSQL_exec D q̂ outs` is existential: it says that **some** Boolean
schedule, and some choice of bag / ORDER BY representatives, evaluates q̂ to
`SqlSuccess outs`. On its own it does not rule out other outcomes (a different
row order, an error, or different values).

The new theorems close that gap. Under the **same premises** as the existing
NormalizeSound theorems, **every** possible FormalSQL outcome of q̂ equals
`SqlSuccess outs`. The embedded accelerator result is therefore *the* SQL
result, not just one legal result.

### Q21 ([`q21/Q21AllOutcomes.v`](q21/Q21AllOutcomes.v))

```coq
Theorem NormalizeSound_Q21_all_outcomes :
  forall db nation_name rows,
    Preconditions_Q21 db nation_name ->
    Q21CountSafe db ->
    Eval_Spec_Q21 nation_name db = Some rows ->
    exists outs D,
      Embed_Q21_Result rows = Some outs /\
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (qhat_Q21 nation_name) outs /\
      (forall sched o,
         eval_query_expr_outcome (basesort D) (instance D) ... sched nil
           (qhat_Q21 nation_name) o ->
         o = SqlSuccess outs).
```

Also:
- `NormalizeSound_Q21_catalog_all_outcomes` (nation = "SAUDI ARABIA", catalog query)
- `Q21_qhat_outcomes_unique` (any two outcomes of q̂ are equal; needs
  `Preconditions_Q21` and `Q21CountSafe`)

### Q1 ([`q1/Q1AllOutcomes.v`](q1/Q1AllOutcomes.v))

```coq
Theorem NormalizeSound_Q1_all_outcomes :
  forall db delta,
    Preconditions_Q1 db delta ->
    Q1NumericSafe db ->
    NumericEmbedFaithful ->
    normalize_q1 (qhat_Q1 delta) = Some LogicalSpec_Q1 /\
    FormalSQL_exec (EmbedDB_Q1 db) delta outs /\
    (forall sched o, eval_query_expr_outcome ... sched nil (qhat_Q1 delta) o ->
                     o = SqlSuccess outs)
  (* outs := EmbedResult_Q1 (Eval_Spec_Q1_outs (cutoff_Q1 delta) db) *)
```

Also:
- `NormalizeSound_Q1_catalog_all_outcomes` (delta = 90, `Queries.source_query_expr`)
- `Q1_qhat_outcomes_unique`, which needs only `Preconditions_Q1` and
  `NumericEmbedFaithful`. It does not use `Q1NumericSafe`: the absence of
  errors is derived, not assumed.

## Premises / TCB

- No new premises and no new axioms.
- Print Assumptions on every new theorem gives the same five stdlib axioms as
  before (see `*/allowed_assumptions.txt`).
- Q1 still carries its existing `NumericEmbedFaithful` representation premise.
  The all-outcomes proof reuses it: it makes SUM/AVG independent of the order
  of rows inside a group (a sum is the embed of a `Z` fold).

## Method

Generic inversion lemmas live in
[`lib/PossibleOutcomeDeterminism.v`](lib/PossibleOutcomeDeterminism.v). There is
one lemma per operator:

| Invariant | Meaning |
|-----------|---------|
| `outcomes_perm q L` | every outcome succeeds; its rows are a Stdlib `Permutation` of `L` |
| `outcomes_bag q L` | every outcome succeeds; its rows are `Oeset.permut` (up to `=t=`) of `L` |

Per-operator lemmas:
- **Table:** `outcomes_bag_table`
- **Filter:** `outcomes_perm_filter` / `outcomes_bag_filter`
- **CrossJoin:** `outcomes_bag_cross_join`
- **Project:** `outcomes_perm_project`; `outcomes_bag_project` collapses a bag back to a permutation
- **Group:** `outcomes_bag_group`. It takes three hypotheses:
  - per-group exactness for the groups of **every** representative
  - group values that ignore the group representative
  - key-runtime safety
- **ORDER BY and the final Project:** `outcomes_order_project`, then
  `pod_order_unique`. If the sort order is transitive and antisymmetric on the
  grouped rows and respects `=t=`, then every ordered representative gives the
  same final list.

How each query supplies these:
- **Q21:**
  - ORDER BY (numwait DESC, s_name ASC) is antisymmetric because s_name is the
    group key (`Q21_order_final_unique`).
  - COUNT-star fits int64 for any group, via `Q21CountSafe`.
- **Q1:**
  - ORDER BY (rf, ls) reuses `q1_group_rows_le_trans_on` and
    `q1_group_rows_order_keys_antisym`.
  - Per-bucket overflow safety is restated over any sub-bag of the inner rows
    (`q1_group_bucket_safe_of_sub`).
  - Group values are rep-independent via `NumericEmbedFaithful`
    (`q1_group_values_respect`).

## Gates

- `scripts/compile_q21_stage2.sh`: compiles the library and `Q21AllOutcomes.v`,
  and scans them for Admitted / admit / give_up / Axiom.
- `scripts/compile_q1_normalize_sound.sh`: compiles the library and
  `Q1AllOutcomes.v`, and runs the same marker scan on both.
- `scripts/check_print_assumptions.sh q1|q21`: covers the new theorems.
