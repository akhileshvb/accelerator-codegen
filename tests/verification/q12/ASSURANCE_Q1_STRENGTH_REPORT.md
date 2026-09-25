# Q12 assurance report (Q1-strength + join2 facade)

Do not start another query.

Gates:

| Gate | Status |
|---|---|
| `RESULTSPEC_NORMALIZE_OK` | **green** |
| `NORMALIZE_SOUND_Q12` | **green** |
| `ORDER_MAINT_PROVED` | **green** |
| `Q12ExecCorrect` / Hit / Compose | **green** |
| `RESULTSPEC_MODEL_EXPORT_OK_Q12` | **pin** |
| `Q12_SQL_SPEC_PROVED` | **pending** — `Q12_GroupBagsPerm` **removed**; residual is isolated `Q12_GroupRowsPerm_CollectModes` (WHERE/CASE/GroupBag blockers) |

---

## Verdict

Reusable **2-table join/group spine** + **SortedGroupResultFacade** are Qed.
**Q1** discharged `Q1_GroupBagsPerm` via the shared sorted-bag theorem.
**Q12** removed `Q12_GroupBagsPerm`; the shared sort theorem does **not** by itself
close Q12 — the missing piece is the **group-row Permutation**
`q12_group_rows_perm_CollectModes_embed` (Q1 twin of
`q1_group_rows_perm_SpecFinalize_embed`), blocked on WHERE + CASE + GroupBagOk.

---

## Shared facades

| Facade | Role |
|---|---|
| `QueryJoin2SpineFacade` | 2-table CrossJoin→Filter→Group→Order→Project |
| `SortedGroupResultFacade` | Permutation + list-local le_trans + antisym ⇒ unique `sort_ordered_rows` |
| `KeyedSortFacade` | bag-local sorted uniqueness (list-local trans) |

---

## Q12 remaining (smallest isolated theorem)

**Name:** `Q12_GroupRowsPerm_CollectModes` (packaging) /
intended `q12_group_rows_perm_CollectModes_embed`:

```text
Permutation
  (q12_group_rows_of orders lines)
  (map q12_group_tuple_of_SpecOutRow (Q12_spec_group_outs orders lines))
```

Then `tnull_sort_ordered_rows_eq_of_perm` closes sorted uniqueness (Q1 pattern).

**Upstream leaf blockers** (to define `q12_group_rows_of`):

1. `Q12_FilterOk` for `scalar_expr_predicate_1`
2. `Q12_ProjectTableOk` for `scalar_select_list_2` (CASE)
3. `Q12_GroupBagOk_holds`

---

## TCB (Q12)

- Logos / FormalSQL semantics
- SpecExportCorrect / toolchain / SPI
- Corresponds_Q12 (C++ ↔ Model)
- `Q12_GroupRowsPerm_CollectModes` (until WHERE/CASE/GroupBag + perm Qed)

No AVG TCB. No maintenance axioms.
