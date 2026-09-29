# Residual generic-interpreter correspondence (RelNode-canonical)

## Two IR generations (do not conflate)

| IR | Location | Role |
|----|----------|------|
| **RelNode tree (canonical)** | `formalsql/logical/` ↔ `tests/verification/logical/dafny/` | Typed relational LogicalSpec; Q1 as Rocq `LogicalSpec_Q1_rel` → generated `Q1RelNodeData.dfy` |
| **Flat expression-IR (migration scaffolding)** | `formalsql/resultspec/` ↔ `q1/dafny/EvalLogicalSpec.dfy` | Rocq observational bridge Qed; Dafny public Exec retargeted to RelNode; flat DATA/pins retained for migration proofs only |

## Canonical SpecExportCorrect surfaces

| Surface | What | Gate artifacts |
|---------|------|----------------|
| **(A) RelNode DATA** | Deterministic transcription Rocq `LogicalSpec_Q1_rel` → JSON → `Q1RelNodeData.dfy` (committed↔regen) | `check_relnode_logical_spec_export.sh`, `specs/q1/logical_spec_relnode.dump.json` |
| **(B) RelNode interpreter TCB** | Name duals + source pins (≠ semantic equivalence) | `interpreter_correspondence_manifest.txt`, complete IR vocab in `spec_export_allowed_leaves.txt`, **`interpreter_source_relnode.pin` (PRIMARY)** covering SqlTypes/Schema/Expr/Agg/RelNode/Typing/Eval + Dafny LogicalSpec/EvalLogicalSpec |
| **(C) Query-specific adapter correspondence** | Separate residual boundary — row/schema/output Adapt | Rocq `Q1Adapt` ↔ Dafny `Q1AdaptRel` (+ `RowOfAux` / `DbOfAux` / `ParamCutoff`) |
| **(D) Flat migration scaffolding** | Flat DATA + flat duals/pins/vocab | `check_logical_spec_export.sh`, `*_migration_flat.*` |

Rocq public meeting point (proved):

```text
Rocq RelNode LogicalSpec_Q1_rel ──► Eval_Spec ──► Q1Adapt (q1/Q1Adapt.v)
        │
        └── Q1RelNodeBridge.v Qed ──► flat Eval_Spec (proof-internal); public statement: Q1FinalTheorem.v (Embed of Eval_Spec rows)
```

Dafny status:

```text
RelNode GroupStore: seq<GroupEntry> list association (Rocq dual; unique-key StoreGet/StorePut)
RelNode DATA: generated/Q1RelNodeData.dfy (from Rocq serialize; sole query-tree authority)
RelNode public Exec: Q1ExecCorrectOrdered / Hit → Eval_Spec_Q1_OutRows_rel (Qed)
Flat Eval_Spec_Q1_OutRows: migration scaffolding only (q1/dafny/EvalLogicalSpec.dfy)
Dafny RelNode↔flat bridge: Q1RelNodeBridge.dfy Qed
  (OutRows_rel == EvalResultSpec == flat OutRows)
```

**Query-specific logical-query semantics** live in RelNode LogicalSpec DATA
(`LogicalSpec_Q1_rel` export). `Q1Adapt` (proof-internal; the Dafny side no longer mirrors it) is
representation / observation bridges — not LogicalSpec DATA.

## SpecExportCorrect (B) — RelNode primary

1. Correspondence manifest duals exist under `formalsql/logical` ↔ `logical/dafny`
2. Fail closed on legacy ResultSpec authority names
3. **`interpreter_source_relnode.pin`** forces TCB review on any edit to
   SqlTypes / Schema / Expr / Agg / RelNode / Typing / Eval (Rocq) or
   LogicalSpec / EvalLogicalSpec (Dafny)
4. Complete currently supported IR constructor vocabulary in
   `spec_export_allowed_leaves.txt` (adding a constructor forces review;
   presence ≠ semantic bound)

Flat duals / `interpreter_source_migration_flat.pin` are migration-only.

## SpecExportCorrect (C) — query-specific adapter correspondence

Section (C) is a **separate** residual correspondence boundary, not a subset
of (B) and not discharged by (A).

| Claim | Status |
|-------|--------|
| Named duals exist (Rocq `Q1Adapt`, Dafny `Q1AdaptRel`, plus `RowOfAux` / `DbOfAux` / `ParamCutoff`) | Gate checks presence |
| Rocq↔Dafny adapter semantic equivalence | **Not proved by SpecExportCorrect** — residual correspondence gap |
| Outside generic-interpreter TCB | True, but **does not** imply adapters are proved |

Do not treat “adapters are query-specific / outside (B)” as a proof of
cross-prover agreement. Observational RelNode↔flat / Aux bridges that *are*
Qed live in `Q1RelNodeBridge.{v,dfy}` and related proof files — those are
separate from the SpecExportCorrect (C) presence gate.

## Explicitly demoted

`EvalSpec_Q1`, `EvalResultSpec`, `SpecApply`, `SpecFinalizeOne`, `FinalizeKeys`, `SortByKey` — scaffolding only.
Use `interpreter_source_relnode.pin` (primary) and `interpreter_source_migration_flat.pin` (flat only);
do not revive a flat `interpreter_source.pin` authority name.
