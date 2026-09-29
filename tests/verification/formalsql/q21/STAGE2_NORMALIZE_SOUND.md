# Stage 2 — NormalizeSound_Q21 (Q1-style strategy)

**Status: superseded — `NormalizeSound_Q21` is Qed (with `Q21CountSafe`); see [`STAGE2_FINAL.md`](STAGE2_FINAL.md). Stage 3 pending review.**

Strategy: Q1-style query-specific NormalizeSound
(see [`Q21_STRATEGY.md`](Q21_STRATEGY.md)). Generic FormalSQL→RelNode compiler
stack is **not** a prerequisite. Shared ScanCorrespond / ValueCorrespond /
SlotMap are retained as optional helpers.

---

## 1. Exact target theorem (tightened, fail-closed)

```rocq
Theorem NormalizeSound_Q21 :
  forall db nation_name rows,
    Preconditions_Q21 db nation_name ->
    Eval_Spec_Q21 nation_name db = Some rows ->
    exists outs D,
      Embed_Q21_Result rows = Some outs /\
      EmbedDB_Q21 db = Some D /\
      FormalSQL_exec D (qhat_Q21 nation_name) outs.
```

- **No** `Eval_Spec = None → []` fallback.
- **No** empty-only `Preconditions_Q21` (`db = db_empty_Q21` rejected).
- Empty DB may appear only as a local sanity lemma.

Scaffolding now in place:
- `qhat_Q21 nation_name` (catalog short-circuit + `subst_nation_query`)
- nonempty `EmbedDB_Q21` / `Preconditions_Q21` (`DB_ok ∧ NationNameOk ∧ Embed ≠ None`)
- first nonempty leaf Qed: `EmbedRow_supplier_row_correspond`

---

## 2. Proof decomposition (intended, Q1-style)

```text
NormalizeSound_Q21
  ├─ normalize_q21 (recognition) — Qed separately
  ├─ RelNode Eval_Spec LogicalSpec_Q21_rel
  │    spine: Join/Filter → Semi → Anti → Agg COUNT(*) → Project → Sort
  ├─ Embed_Q21_Result (RelNode rows → FormalSQL output tuples)
  └─ FormalSQL_exec qhat on EmbedDB
       Q21-specific leaves (not generic morphism stack):
         CrossJoin spine / outer ConjList filter
         EXISTS ↔ Semi acceptance (Logos lemmas + Q21 ConjList glue)
         NOT EXISTS ↔ Anti acceptance
         GROUP BY s_name + COUNT(*)
         Project + ORDER BY numwait DESC, s_name ASC
```

---

## 3. Generic vs Q21-specific

| Item | Kind | Status |
|------|------|--------|
| `normalize_q21_source` / `_shared` | Q21 | Qed (recognition) |
| `LogicalSpec_Q21_rel` | Q21 DATA | Qed WF |
| `qhat_Q21 nation_name` | Q21 | defined; catalog instance Qed |
| `ValueCorrespond` / `SlotMap` / `ScanCorrespond` | generic (retained) | Qed; optional helper |
| nonempty `EmbedDB_Q21` / `Preconditions_Q21` | Q21 | defined (+ nonempty example Qed) |
| `EmbedRow_supplier_row_correspond` | Q21 leaf | **Qed** |
| FormalSQL CrossJoin/EXISTS/Group/Order spine | Q21 | **open** |
| Full `NormalizeSound_Q21` | Q21 | **open** |

---

## 4. ScanCorrespond usage

**Not required** for the Q1-style plan. Available if table-leaf proofs want it.
No further generic Join/Semi/Anti morphisms will be built as Stage-2 gates.

---

## 5. Print Assumptions

N/A for `NormalizeSound_Q21` (no Qed proof object).
Qed lemmas in `Q21Qhat` / `Q21FormalSQLProof` / recognition have no new axioms.

---

## 6. Remaining TCB

- Logos FormalSQL semantics (same as Q1)
- RelNode `Eval_Spec` definition
- `COLLATION_TCB` (Sort not yet reached in NormalizeSound)
- No `NumericEmbedFaithful` for Q21 COUNT(*)/text path yet

---

## 7. Stage 2 gate status

| Gate | Status |
|------|--------|
| DATA + recognition | **PASS** |
| Strategy = Q1-style query-specific | **PASS** |
| Theorem tightened (fail-closed, nonempty Pre) | **PASS** |
| `qhat_Q21` + nonempty Embed/Pre | **PASS** |
| First nonempty semantic leaf Qed | **PASS** (`EmbedRow_supplier_row_correspond`) |
| `NormalizeSound_Q21` Qed | **FAIL** |
| Stage 3 | **BLOCKED** |

---

## 8. Smallest remaining blocker

**FormalSQL CrossJoin + Project + ordinary 6-conjunct ConjList
`FormalSQL_exec` on nonempty `EmbedDB_Q21`, bag-corresponding to
`Eval_Spec filter_outer`.**

See [`STAGE2_OUTER_ORDINARY.md`](STAGE2_OUTER_ORDINARY.md).
Secondary: CHAR/Bpchar text 3VL for status/`n_name`.
EXISTS / NOT EXISTS not started.

Empty-spine success is only a local sanity check (do not build a general
empty-spine framework).
