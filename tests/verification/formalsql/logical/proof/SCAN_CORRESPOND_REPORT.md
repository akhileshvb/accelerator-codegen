# Scan correspondence — FormalSQL → RelNode (Stop report)

**Status: `FormalSQL_RelNode_ScanCorrespond` Qed. STOP before next operator.**

Option A incremental bridge. Not a universal FormalSQL↔RelNode equivalence.
Q21 Stage 3 remains workflow-blocked until the full NormalizeSound stack is Qed.

---

## 1. Value correspondence

File: `logical/proof/ValueCorrespond.v`

```rocq
Inductive value_correspond : SqlType -> NullValues.value -> SqlValue -> Prop :=
| VC_null_int32 / int64 / Z / bool / date / text / numeric  (* → VNull *)
| VC_int32  : Value_int32 (Some i) ↔ VInt (int32_value i)   @ TInt
| VC_int64  : Value_int64 (Some i) ↔ VInt (int64_value i)   @ TInt
| VC_Z      : Value_Z (Some z)     ↔ VInt z                 @ TInt
| VC_bool   : Value_bool (Some b)  ↔ VBool b                @ TBool
| VC_date   : Value_date (Some d)  ↔ VInt d                 @ TDate
| VC_text   : Value_string (tm, Some s) ↔ VText s           @ TText
| VC_numeric_scaled :
    Value_numeric (Some (numeric_of_scaled coeff (Z.of_nat scale)))
      ↔ VInt coeff @ TNumeric scale.
```

`formal_attr_sqltype` maps FormalSQL attributes → `option SqlType` (fail closed).

---

## 2. Schema / slot-map relation

File: `logical/proof/SlotMap.v`

```rocq
Record SlotBinding := mkSlotBinding {
  sb_formal : attribute TNull;  (* FormalSQL attribute *)
  sb_rel    : Attr              (* RelNode schema attr *)
}.
Definition SlotMap := list SlotBinding.  (* index = RelNode slot *)

Definition slot_binding_wf sb :=
  formal_attr_name (sb_formal sb) = attr_name (sb_rel sb) /\
  formal_attr_sqltype (sb_formal sb) = Some (attr_type (sb_rel sb)).

Definition SlotMap_wf sm :=
  NoDup (map formal names) /\ NoDup (map RelNode names) /\
  Forall slot_binding_wf sm.
```

---

## 3. RowRel

```rocq
Definition row_correspond (sm : SlotMap) (ft : tuple TNull) (rr : Row) : Prop :=
  length rr = length sm /\
  forall i sb v,
    nth_error sm i = Some sb ->
    nth_error rr i = Some v ->
    value_correspond (attr_type (sb_rel sb))
      (dot TNull ft (sb_formal sb)) v.
```

Concatenated layouts use `sm1 ++ sm2` with `rr1 ++ rr2` (for later Join/Semi).

---

## 4. Bag / table correspondence

```rocq
Definition bag_correspond sm fts rrs := Forall2 (row_correspond sm) fts rrs.
Definition BagRel R a b := Forall2 R a b.  (* RelNode↔RelNode; replaces True stub *)
```

Exact list multiplicity (order-preserving Forall2).

---

## 5. `FormalSQL_RelNode_ScanCorrespond` (exact statement)

File: `logical/proof/ScanCorrespond.v`

```rocq
Theorem FormalSQL_RelNode_ScanCorrespond :
  forall sm tid table expected outputs embed rel_rows p D db,
    SlotMap_wf sm ->
    (forall r, row_correspond sm (embed r) r) ->
    relname_table_id table = tid ->
    @_basesort TNull expected table =S= query_outputs_sort outputs ->
    D = witness_database expected
          [WitnessTable table (map embed rel_rows)] ->
    db = db_singleton tid rel_rows ->
    eval_rel (RelScan tid) p db = Some (RVRows rel_rows) /\
    bag_correspond sm (map embed rel_rows) rel_rows /\
    Febag.cardinal (_instance/_basesort D) (query_table_bag outputs table) =
      N.of_nat (length rel_rows) /\
    forall env schedule outcome,
      eval_query_expr_outcome ... D ...
        (QExpr_Table outputs table) outcome ->
      exists frs,
        outcome = SqlSuccess frs /\
        query_same_rows_as_bag frs (query_table_bag ... D ... table).
```

**Qed.**

---

## 6. Intentionally unsupported / fail closed

| FormalSQL | Status |
|-----------|--------|
| float / double / time / timestamp / timestamptz | no `value_correspond` / `formal_attr_sqltype` |
| `Attr_numeric` (no scale) | None — need `Attr_decimal` with scale |
| Negative decimal scale | None |
| `VAvgExact` as FormalSQL table value | no constructor (RelNode agg carrier only) |
| Type-mismatched pairs | no constructors |

---

## 7. Print Assumptions

```
ClassicalDedekindReals.sig_not_dec
ClassicalDedekindReals.sig_forall_dec
ProofIrrelevance.proof_irrelevance
FunctionalExtensionality.functional_extensionality_dep
Classical_Prop.classic
```

Transitive from FormalSQL/Logos (NUMERIC / reals), **not** new Scan axioms. No Admit.

---

## 8. New shared proof-library files

| File | Role |
|------|------|
| `logical/proof/ValueCorrespond.v` | value + attr→SqlType |
| `logical/proof/SlotMap.v` | SlotMap, row/bag correspond, BagRel |
| `logical/proof/ScanCorrespond.v` | Scan theorem |
| `logical/proof/Morphisms.v` | BagRel re-export (no longer `True`) |
| `scripts/compile_logical_ir.sh` | compiles new files |
| `PROOF_LIBRARY_INDEX.md` | indexed |

---

## 9. TCB assumptions used

- None new for Scan beyond Logos/FormalSQL transitive classical axioms (same surface as Q1 FormalSQL proofs).
- `COLLATION_TCB` not involved (Sort not reached).
- `NumericEmbedFaithful` not used; Scan uses constructive `numeric_of_scaled` embedding only.

---

## 10. Next smallest missing morphism

**FormalSQL CrossJoin / Filter ↔ RelNode `RelJoin JoinInner` + `RelFilter`**
(join condition / predicate correspondence under concatenated `SlotMap`).

Then: Filter alone, EXISTS↔Semi, NOT EXISTS↔Anti, Agg, Project, Sort.

---

## Q1 regression

- `compile_logical_ir.sh` — **OK** (includes `Q1RelNodeBridge.v`)
- `compile_q1_proof.sh` — run in parallel with this report

Q21 Stage 3 still blocked until NormalizeSound_Q21 Qed.
