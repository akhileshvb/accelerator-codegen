# Q1 proof flow

Current theorem, proof structure and trusted edges:
[`CURRENT_THEOREM_TCB.md`](CURRENT_THEOREM_TCB.md) (canonical) and
[`RESIDUALS.txt`](RESIDUALS.txt). Pipeline overview: [`../PIPELINE.md`](../PIPELINE.md).

```text
Rocq  q1/Q1FinalTheorem.v  NormalizeSound_Q1_rel_all_outcomes
        FormalSQL outcome = Embed(Eval_Spec(LogicalSpec_Q1_rel) rows)
        ├─ Q1AllOutcomes / Q1NormalizeSound / Q1GroupsCorrespond   (FormalSQL side)
        ├─ Q1RelNodeBridge / Q1EvalMigration                         (RelNode side)
        ├─ Q1NumericEmbed.NumericEmbedFaithful_holds  ← lib/NumericAggExact.v
        └─ Eval_Spec_Q1_rel_shape

Dafny q1/dafny/Q1Runtime.dfy  Hit_correct_Q1 / Fallback_correct_Q1
        ├─ substrate/dafny/AuxSyncProtocol.dfy          (shared with Q21)
        ├─ Q1ExecExact.Q1ExecExact: ExecSeqM == Q1Rows(Eval_Spec rows)
        │    ├─ Q1ResultSpecBridge (ExactQ1) ← Q1RelNodeBridge (row shape)
        │    └─ logical/dafny/PgNumeric.dfy
        └─ Q1TwinAuxClose / Q1TwinAuxRefinement / Q1Aux / Q1Maintenance

C++   src/generated/tpch/q1/postgres/aux/{module,finalize}.cpp
        ↔ Q1ModuleTwin / Q1Finalize  (SOURCE MAP + exact extract-diff)
```
