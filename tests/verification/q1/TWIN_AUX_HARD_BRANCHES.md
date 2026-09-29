# Twin ↔ Aux hard branches (Q1)

**Status:** see canonical [`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md).
This file is only the ModuleTwin / C++ locus catalog for HB1–HB5.

Named SPI contracts (`AggsFaithful`, `RebuildOracleCorrect`) are **not**
`twin_aux_refinement` TCB. Bookkeeping formerly listed as free contracts
(`FinalizeSortFaithful`, BuiltInv-as-premise, PartsWF/OrderUnique) is **Qed**.

## Proved HB map

| HB | C++ / Twin locus | Status |
|----|------------------|--------|
| HB1 | `StageMaintenance` requires_rebuild | store-NoOp ≡ Aux NoOp Qed (action retag) |
| HB2 | RebuildPartition / BuildState | lineitem Qed; aggs under `AggsFaithful` SPI |
| HB3 | `Execute` hit / fallback / ComposeOutput | map+finalize+fallback Qed; BuiltInv from producers |
| HB4 | delete/insert MaintAction packaging | Fold + Twin deltas Qed |
| HB5 | DayPart keys packaging | Flatten Qed |

Support lemmas (Flatten, ApplyDeltaSeq, PartsWF/OrderExact preservation, closed
`Q1Twin_Execute_*` / `Q1Twin_Maint_*`) live in
[`dafny/Q1TwinAuxRefinement.dfy`](dafny/Q1TwinAuxRefinement.dfy) /
[`dafny/Q1TwinAuxClose.dfy`](dafny/Q1TwinAuxClose.dfy).

## C++ policy

**No C++ change.** HB1 retag is intentional, not proof-hostile.

Do not expand this list into a catch-all `twin_aux_refinement` TCB.
