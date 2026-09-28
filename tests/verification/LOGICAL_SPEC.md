# LogicalSpec / verification layer rules

Architecture for JetStream FormalSQL ↔ LogicalSpec ↔ Q1 accelerator verification.
See also `.cursor/plans/logicalspec_architecture_refactor_a6c8724c.plan.md`.

## Right-side chain (accelerator)

```text
Q1 C++ aux
  ↓ CorrespondsCppTwin          [TCB: finite extract-diff / SOURCE MAP]
ModuleTwin                      [Q1ModuleTwin.dfy]
  ↓ Flatten
  ↓ TwinAuxRel(T,A) ≡ Flatten(T.store)=A.store   [representation-only]
  ↓ TwinExecRefinesAux / TwinMaintRefinesAux
AuxModel                        [Inv(A,D) ≡ A.store = FoldTwin(D), separate]
  ↓ ExecCorrect / MaintPreservesInv
LogicalSpec / Eval_Spec
```

**`TwinAuxRel` must not bake in Inv or DB correctness.** Inv stays a separate predicate.

**Current theorem / TCB (canonical):** [`q1/CURRENT_THEOREM_TCB.md`](q1/CURRENT_THEOREM_TCB.md).  
HB loci only: [`q1/TWIN_AUX_HARD_BRANCHES.md`](q1/TWIN_AUX_HARD_BRANCHES.md).  
CorrespondsCppTwin pin: [`q1/SOURCE_MAP.md`](q1/SOURCE_MAP.md).

## Twin ↔ Aux derivation (Phase 0b)

- Prefer **mechanical derivation** of Aux store abstraction / `FoldTwin` / `Inv` from Twin `ApplyDeltaToStore` + `Flatten`.
- If a piece cannot be derived, document why; do not keep duplicated handwritten state/update algebra without that justification.
- **Current status:** closed Twin↔Aux package in `Q1TwinAuxRefinement.dfy` + `Q1TwinAuxClose.dfy` (including ComposeOutput≡ExecSeqM, BuiltInv from BuildState, HB1 NoOp, FoldTwinFromTwin induction). Intentional SPI: `AggsFaithful`, `RebuildOracleCorrect` on free oracle/load. Details: [`q1/CURRENT_THEOREM_TCB.md`](q1/CURRENT_THEOREM_TCB.md). No C++ nips.
- **Spec bridge (Phases 1–4 + RelNode):** RelNode IR is canonical (`formalsql/logical/` ↔ `logical/dafny/`). Rocq `LogicalSpec_Q1_rel` serialize → `logical_spec_relnode_v1` dump → `Q1RelNodeData.dfy`. Public endpoints: Rocq/Dafny RelNode `Eval_Spec`. SpecExportCorrect = RelNode DATA pin + RelNode interpreter correspondence (`interpreter_source_relnode.pin`) + query adapters + flat migration scaffolding ([`formalsql/resultspec/GENERIC_INTERPRETER_TCB.md`](formalsql/resultspec/GENERIC_INTERPRETER_TCB.md)); dual presence is not semantic equivalence. Flat expression-IR (`LogicalSpecData.dfy`, `*_migration_flat.*`) is migration-only. Legacy ResultSpec is migration scaffolding only.

## Left-side chain (SQL)

```text
Catalog SQL → Logos → FormalSQL AST q
  → Rocq normalize (NormalizeCompositional.v) → LogicalSpec S
  → Eval_Spec (EvalLogicalSpec.v) SQL-only
  → NormalizeSound_generic ⇒ FormalSQL = Eval_Spec   [Q1 corollary green]
```

Artifacts:
- [`formalsql/resultspec/LogicalSpec.v`](formalsql/resultspec/LogicalSpec.v)
- [`formalsql/resultspec/NormalizeCompositional.v`](formalsql/resultspec/NormalizeCompositional.v)
- [`formalsql/resultspec/EvalLogicalSpec.v`](formalsql/resultspec/EvalLogicalSpec.v)
- [`formalsql/resultspec/NormalizeSoundGeneric.v`](formalsql/resultspec/NormalizeSoundGeneric.v)
- [`formalsql/resultspec/GENERIC_INTERPRETER_TCB.md`](formalsql/resultspec/GENERIC_INTERPRETER_TCB.md)
- [`formalsql/resultspec/interpreter_correspondence_manifest.txt`](formalsql/resultspec/interpreter_correspondence_manifest.txt) (canonical RelNode duals)
- [`formalsql/resultspec/interpreter_source_relnode.pin`](formalsql/resultspec/interpreter_source_relnode.pin) (PRIMARY RelNode interpreter hashes)
- [`formalsql/resultspec/interpreter_source_migration_flat.pin`](formalsql/resultspec/interpreter_source_migration_flat.pin) (flat migration only)
- [`formalsql/resultspec/spec_export_allowed_leaves.txt`](formalsql/resultspec/spec_export_allowed_leaves.txt) (RelNode constructor vocabulary; presence ≠ semantic bound)

Legacy `NormalizeQuery.v` / `FormalSQLAuxCorrect_*` / `EvalSpec_Q1` remain as internal/migration lemmas;
public names are `FormalSQLSpecCorrect_*` and `NormalizeSound_generic` / `NormalizeSound_*`.

## TCB kinds (honest)

Canonical Q1 listing: [`q1/CURRENT_THEOREM_TCB.md`](q1/CURRENT_THEOREM_TCB.md).

| Kind | Meaning |
|------|---------|
| `cpp_model` | CorrespondsCppTwin (finite / unproved universal) |
| Twin SPI | Exact named contracts only (`AggsFaithful`, free `RebuildOracleCorrect`) — **not** a catch-all `twin_aux_refinement` |
| `rocq_dafny_spec` | SpecExportCorrect: RelNode DATA pin + RelNode interpreter correspondence + adapters + flat migration — **not** SpecApply/FinalizeKeys; presence ≠ semantic equivalence |
| Aux `ExecCorrect` / model Hit | Proves model endpoint ≈ generic Eval_Spec OutRows — **not** ModuleTwin/C++ unless Twin package + `cpp_model` |

## Scripts

- `formalsql/scripts/compile_logical_spec.sh`
- `formalsql/scripts/dump_relnode_logical_spec_json.sh`
- `formalsql/scripts/emit_relnode_logical_spec_dafny.py`
- `formalsql/scripts/check_relnode_logical_spec_export.sh` (canonical RelNode DATA)
- `formalsql/scripts/dump_logical_spec_json.sh`
- `formalsql/scripts/emit_logical_spec_dafny.py`
- `formalsql/scripts/check_logical_spec_export.sh` (flat migration DATA)
- `formalsql/scripts/compile_q1_proof.sh`
- `formalsql/scripts/compile_q1_normalize_sound.sh`
- `formalsql/resultspec/check_spec_export.sh`
