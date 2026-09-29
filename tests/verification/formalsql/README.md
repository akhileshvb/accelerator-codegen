# FormalSQL / Logos integration (Q1)

Pin: [PIN.md](PIN.md). Pipeline: [`../PIPELINE.md`](../PIPELINE.md).
Current theorem and TCB: [`../q1/CURRENT_THEOREM_TCB.md`](../q1/CURRENT_THEOREM_TCB.md).

```text
formalsql/
  PIN.md
  lib/                 # TFGOP facades used by the Q1 spine
  resultspec/          # LogicalSpec, normalize, Eval_Spec
  generated/Q1AuxSpec.v
  q1/                  # catalog, Logos AST, Rocq proof
  scripts/             # compile_q1_*.sh
```

Lowering: `scripts/logos_lower_catalog.sh q1` writes
`q1/generated/{Schema,Queries,Witness}.v`. Failure means Q1 is not FormalSQL-verified.

Public Q1 theorem: `q1/Q1FinalTheorem.v` (`NormalizeSound_Q1_rel_all_outcomes`:
FormalSQL outcome = Embed of the RelNode `Eval_Spec` rows; no numeric premise,
`Q1NumericEmbed.NumericEmbedFaithful_holds`). `generated/Q1AuxSpec.v` is a
Rocq-private group model used inside the proof (no longer dual-emitted).
Generic numeric lemmas: `lib/NumericAggExact.v`.

## Q21

Stage 2 (catalog SQL → FormalSQL → `LogicalSpec_Q21_rel`, all outcomes):
`scripts/compile_q21_stage2.sh`; assumptions: `scripts/check_print_assumptions.sh q21`.
Stages 3–5 and the full gate: [`../q21/`](../q21/) (`run_q21_verification.sh`,
`STAGE3_5_REPORT.md`, `RESIDUALS.txt`).
