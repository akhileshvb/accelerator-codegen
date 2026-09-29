# Stage 2 — GROUP BY s_name + COUNT-star

**Status: Qed — `Q21_GroupCountStageCorrespond`. One new, semantically required
premise: `Q21CountSafe`. Final Project / ORDER BY / NormalizeSound: see [`STAGE2_FINAL.md`](STAGE2_FINAL.md).**

File: [`Q21GroupCount.v`](Q21GroupCount.v) (imports `Q21WhereFusion.v`).

| Result | Theorem | Status |
|--------|---------|--------|
| exact grouping shape from q̂ (every nation) | `Q21_catalog_group_shape`, `q21_select_list_1_shape`, `q21_select_list_2_shape`, `q21_group_keys_terms`, `q21_agg_node_in_spec` | Qed |
| group input = Project select_2 ∘ Project select_3 ∘ WHERE | `q21_group_input_success` | Qed |
| group-key correspondence | `Q21_group_key_correspond`, `q21_group_align_Heq` | Qed |
| COUNT-star: RelNode count = FormalSQL group size (lockstep simulation) | `q21_sim_run_aggregate` | Qed |
| group size = occurrences of the key in the input | `Q21_group_count_is_occurrences` | Qed |
| group-env exactness (key leaf, COUNT leaf, HAVING True) | `q21_group_filter_map_exact` | Qed |
| grouped rows ↔ aggregate rows | `Q21_group_rows_correspond` | Qed |
| composed stage | `Q21_GroupCountStageCorrespond` | Qed |

New premise `Q21CountSafe db`: |supplier|·|lineitem|·|orders|·|nation| ≤ int64_max.
FormalSQL COUNT-star is int64 and raises out-of-range on overflow; RelNode counts
in Z. Every group ⊆ WHERE output ⊆ the four-way product, so this bounds every count.
It is a stage-theorem premise, not added to `Preconditions_Q21` (compare Q1's
`Q1NumericSafe`).

Print Assumptions: exactly the five names in `q1/allowed_assumptions.txt`.
