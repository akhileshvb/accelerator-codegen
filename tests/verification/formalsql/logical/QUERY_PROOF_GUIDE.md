# QUERY_PROOF_GUIDE — how to prove a new query against LogicalSpec RelNode IR

## Goal

Instantiate shared lemmas; do **not** copy Q1 inductions wholesale.

## Query-specific obligations (typical)

| Obligation | What to prove | Where it lives |
|------------|---------------|----------------|
| Catalog / schema | `TableId` → `Schema`; slot indices | `qN/QNSchema.v` |
| LogicalSpec tree | `Scan→…` RelNode data | `qN/QNLogicalSpec.v` |
| Exact recognition | `normalize_qN(qhat)=Some(LogicalSpec_QN)` | `qN/QNLogicalNormalize.v` |
| Predicate correspondence | SQL/FormalSQL pred ↔ `eval_pred` | query leaf |
| Key correspondence | group exprs ↔ twin/aux keys | query leaf (Pack stays query-local) |
| Contribution correspondence | row → agg contrib ↔ Aux SpecApply | query leaf |
| Output adapter | RelNode `Row` bag → observed OutRows | `qN/QNAdapt.v` |
| Aux Inv / maintenance | `Inv(A,D) := A represents Fold(D)` | accel Dafny |
| Runtime lift | PairInv + SnapshotHitSafe ⇒ ExecCorrect | runtime Dafny |

## Shared infrastructure (do not fork)

- IR / Eval: `formalsql/logical/{SqlTypes,Schema,Expr,Agg,RelNode,Typing,Eval}.v`
- Dafny Eval: `tests/verification/logical/dafny/`
- Morphisms: `formalsql/logical/proof/Morphisms.v` + `FormalSQLLib/*Facade.v`
- DATA export: Rocq serialize → JSON → generated `LogicalSpecData.dfy`

## Proof recipe (LLM-friendly)

1. Encode `LogicalSpec_QN` as RelNode data; prove `logical_spec_wf catalog_QN = true`.
2. Prove normalize recognition (fail-closed).
3. For each operator in the tree, apply the matching morphism from `PROOF_LIBRARY_INDEX.md`.
4. Prove only the leaf correspondences (pred/key/contrib/adapt).
5. Compose to `Eval_Spec(LogicalSpec_QN) ∘ Adapt = AuxAnswerSpec` (or twin target).
6. Lift through maintenance/runtime using shared skeletons.
7. Update SpecExportCorrect manifest (generic duals) + query bridge pin; never claim presence = equivalence.

## Q1 worked example (major patterns)

- Tree: `Q1LogicalSpecTree.v` — Filter → Aggregate → Project → Sort
- Adapt: `Q1Adapt.v` (RelNode) / flat migration still in `resultspec/EvalLogicalSpec.v` scaffolding
- Filter leaf: `filter_rows_Q1` in legacy flat Eval (RelNode: use FilterMorphism + shipdate≤cutoff)
- Fold/Agg: `Q1EvalMigration.v` Acc↔Stats (query-specific Pack path remains Q1-only)
- Public: `FormalSQLSpecCorrect_Q1`, `NormalizeSound_Q1` → Eval_Spec ∘ Adapt

## Do not

- Put shipdate/rf/ls/Pack into `formalsql/logical/`
- Add Q1 branches to `Eval_Spec`
- Weaken public theorem statements
- Implement Q12/Q14 in this guide’s examples beyond naming
