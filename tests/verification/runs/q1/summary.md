# q1 verification summary

Canonical status: tests/verification/q1/CURRENT_THEOREM_TCB.md. Under Preconditions_Q1 and Q1NumericSafe, and assuming NumericEmbedFaithful, FormalSQL_exec(EmbedDB_Q1(db), delta) observes EmbedResult(Eval_Spec_Q1_outs(cutoff(delta), db)) (= Q1Adapt ∘ Eval_Spec(LogicalSpec_Q1, …); Q1ByteKeys discharged from Pre). Q1_GroupBagsPerm is Qed. Dafny proves Inv ⇒ Exec ≈_Q1 Eval_Spec_Q1_OutRows (via Eval_Spec_Q1_eq_EvalResultSpec), maintenance preserves Inv, Hit ⇒ Aux execution, and the Twin↔Aux closed package (ComposeOutput≡ExecSeqM, BuiltInv from BuildState, HB1–HB4 bookkeeping). Intentional Twin SPI: AggsFaithful (agg BuildState only) and RebuildOracleCorrect on free Stage/load oracles (lineitem BuildState discharges RebuildOracleCorrect). C++ correspondence is cpp_model (finite extract-diff). Q1ExecCorrect alone does not cover ModuleTwin/C++.

## Gates

- LOGOS_OK: pass
- CANONICAL_SPEC_OK: pass
- SQL_SPEC_PROVED: pass
- NO_QUERY_SEMANTIC_RESIDUAL: pass
- NO_ADMITTED: pass
- PRINT_ASSUMPTIONS: pass
- SPEC_EXPORT_CHECKED: pass
- CRITIQUE_SQL: skipped
- ACCEL_MODEL_PROVED: pass
- MAINTENANCE_PROVED: pass
- SNAPSHOT_HIT_PROVED: pass
- CPP_MODEL_CORRESPONDENCE: finite_test
- CRITIQUE_ACCEL: skipped
- PATTERN_LIBRARY_UPDATED: pass

## Preconditions

- Preconditions_Q1 db delta
- Q1NumericSafe db

## TCB

- Logos lowering (sql_lowering)
- FormalSQL reference semantics (sql_lowering)
- NumericEmbedFaithful (numeric_representation)
- AvgObservationFaithful (numeric_representation)
- SpecExportCorrect (rocq_dafny_spec)
- Corresponds_Q1 finite extract-diff (cpp_model)
- AggsFaithful (twin_spi)
- RebuildOracleCorrect (free Stage/load oracle) (twin_spi)
- snapshot/SPI abstraction (runtime_spi)
- Rocq/Dafny/Logos toolchain pins (toolchain)

## Semantic residuals

- none

## Pattern library additions

- none
