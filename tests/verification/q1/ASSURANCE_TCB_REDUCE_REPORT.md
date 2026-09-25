# Q1 TCB reduce report (final stop)

Gates rechecked this stop: Rocq `KeyedSortFacade.vo`, `Q1GroupsCorrespond.vo`,
`Q1TCBAuxCorrect.vo`, `Q1NormalizeSound.vo`.

`GROUP_BAGS_PROVED` — **no**. Q1 stopped with `Q1_GroupBagsPerm` still a hyp.

---

## 1. `Q1_GroupBagsPerm` → `GROUP_BAGS_PROVED`?

**Not closed.** Explicit narrow TCB hypothesis retained.

### Qed retained (do not reopen)

| Lemma / artifact | Role |
|------------------|------|
| `q1_group_rows_In_iff_SpecFinalize` | Same key→row **set** (bidirectional `In`) |
| `SpecFinalize_NoDup`, `SpecFinalize_embed_NoDup`, Pack / key recovery | Spec-side uniqueness |
| `q1_groups_head_key_NoDup`, `length_q1_group_rows_eq_SpecFinalize` | FormalSQL key uniqueness + length |
| `q1_group_rows_perm_SpecFinalize_embed` | `In_iff` + NoDup → `Permutation` |
| `ordered_rows` ↔ `Sorted Q1TupleLe`, `sort_ordered_rows_Sorted_Q1` | FormalSQL order ↔ Stdlib `Sorted` |
| `KeyedSortFacade.sorted_perm_eq_in` | Generic local-antisym sort uniqueness (**shared**) |

### Exact remaining blocker

```text
Q1_GroupBagsPerm db delta :=
  sort_ordered_rows q1_order_keys (q1_group_rows_of db delta)
  =
  sort_ordered_rows q1_order_keys
    (map q1_group_tuple_of_OutRow
      (SpecFinalize (SpecView (FoldSpec db) (EmbedParams_Q1 delta))))
```

Close shape (not pursued further): `Permutation` (Qed) + both sides
`Sorted Q1TupleLe` (Qed) + local key antisym → `sorted_perm_eq_in`.
Missing piece stopped on: **`q1_tuple_le` transitivity** (needed by the
facade’s `Sorted_le_In`), not FormalSQL leaf/numeric work.

---

## 2. Shared extraction this stop

`tests/verification/formalsql/lib/KeyedSortFacade.v` (compiling):

- `sorted_perm_eq_in` — bag-local antisym only (no global antisym hyp)
- `sorted_perm_eq` / `map_sorted_perm_eq` — under optional global antisym section

No further unfinished lemmas were generalized.

---

## 3. Other TCB (unchanged)

| Assumption | Status |
|------------|--------|
| `NumericEmbedFaithful` | Still TCB (`NUMERIC_EMBED_PROVED` no) |
| `SpecExportCorrect` | Still TCB; Data AST pin shrink kept |
| `Q1_GroupBagsPerm` | **Still hyp** (`GROUP_BAGS_PROVED` no) |

---

## Final explicit Q1 TCB

```text
Logos lowering
FormalSQL-as-reference-semantics
NumericEmbedFaithful
Q1_GroupBagsPerm              ← narrow: sorted bag eq (In/NoDup/Perm Qed)
SpecExportCorrect             (Data AST pin + EvalResultSpec ↔ EvalSpec_Q1)
Corresponds_Q1(C++, Model)
AvgObservationFaithful
proof checker / SPI substrate cuts
```

Q1 stopped. Q6 not started.
