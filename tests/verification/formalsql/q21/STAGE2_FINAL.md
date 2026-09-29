# Stage 2 — final Project, ORDER BY, NormalizeSound_Q21

**Status: Qed — `NormalizeSound_Q21` and `NormalizeSound_Q21_catalog`.
Stage 3 (Twin/Aux) not started (pending review).**

File: [`Q21NormalizeSoundFinal.v`](Q21NormalizeSoundFinal.v).

**Strengthened (forall outcomes):** `NormalizeSound_Q21_all_outcomes` in
[`Q21AllOutcomes.v`](Q21AllOutcomes.v) proves that EVERY possible FormalSQL outcome of q̂ equals
`SqlSuccess outs`, under the same premises. See [`../ALL_OUTCOMES.md`](../ALL_OUTCOMES.md).

```coq
Theorem NormalizeSound_Q21 :
  forall db nation_name rows,
    Preconditions_Q21 db nation_name ->
    Q21CountSafe db ->
    Eval_Spec_Q21 nation_name db = Some rows ->
    exists outs D,
      Embed_Q21_Result rows = Some outs /\
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (qhat_Q21 nation_name) outs.
```

The earlier `NormalizeSound_Q21_statement` in `Q21NormalizeSound.v` (no
`Q21CountSafe`) is NOT claimed: FormalSQL COUNT-star is int64 and errors on
overflow, so that premise is semantically required.

| Result | Theorem |
|--------|---------|
| top-level shape of q̂ | `Q21_catalog_top_shape`, `q21_select_list_0_shape` |
| RelNode final Project | `q21_project_agg_rows` |
| FormalSQL final Project | `q21_final_project_success` |
| text order bridge (proved, no collation TCB in Rocq) | `Q21_text_c_leb_string_compare` |
| ORDER BY keys never NULL | `q21_final_keys_not_null` |
| two-key order correspondence | `q21_ordered_pair_of_le`, `q21_ordered_rows_of_sorted` |
| RelNode sort = sorting permutation | `q21_result_row_sort_spec` |
| embedded RelNode rows = FormalSQL output tuples | `q21_embed_final` |

Gates:
- `scripts/compile_q21_stage2.sh` — builds proof library + full Q21 chain;
  fails on Admitted / admit / give_up / Axiom.
- `scripts/check_print_assumptions.sh q21` — allowlist `q21/allowed_assumptions.txt`
  (same five stack axioms as Q1).
