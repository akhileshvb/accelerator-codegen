# Verification pipeline (Q1 and Q21 follow the same recipe)

```text
catalog SQL ──Logos──► FormalSQL q̂ ──Rocq (Stage 2)──► RelNode LogicalSpec + Eval_Spec
                                                              │ (exported DATA, pinned;
                                                              │  interpreter pinned + reviewed)
generated C++ ──structural twin + extract-diff──► Dafny twin ──Dafny (Stages 3-5)──┘
```

**Stage 2 (Rocq).** For the exact catalog SQL, every FormalSQL outcome of the
query is `SqlSuccess (Embed rows)` where `Eval_Spec(LogicalSpec_rel, params, db)
= Some rows`. `Embed` maps RelNode values to FormalSQL values (scaled decimals,
text, `VAvgExact(sum,count)` as PostgreSQL `avg(numeric)` via
`formalsql/lib/NumericAggExact.avg_value`).

| Query | Public theorem | Premises |
|-------|----------------|----------|
| Q1  | `formalsql/q1/Q1FinalTheorem.v` `NormalizeSound_Q1_rel_all_outcomes` | `Preconditions_Q1`, `Q1NumericSafe` |
| Q21 | `formalsql/q21/Q21AllOutcomes.v` `NormalizeSound_Q21_all_outcomes` | `Preconditions_Q21`, `Q21CountSafe` |

Both depend only on the five standard classical axioms
(`scripts/check_print_assumptions.sh`). Internal proof helpers (e.g. Q1's
`generated/Q1AuxSpec.v` group model) are fully proved and appear in no
statement.

**Stages 3-4 (Dafny).** The Dafny twin of the generated C++ module is proved
against the same RelNode `Eval_Spec` (Dafny interpreter
`logical/dafny/EvalLogicalSpec.dfy`): Build establishes the store invariant,
every maintenance arm preserves it or invalidates, Execute on a hit and the
fallback both return the observed `Eval_Spec` rows exactly
(Q1: `q1/dafny/Q1ExecExact.dfy`; Q21: `q21/dafny/Q21Inv.dfy`, `Q21Build.dfy`).

**Stage 5 (Dafny).** The runtime's cross-backend sync protocol is proved once
(`substrate/dafny/AuxSyncProtocol.dfy`) and instantiated per query
(`q1/dafny/Q1Runtime.dfy`, `q21/dafny/Q21Runtime.dfy`): a hit for a statement
snapshot returns the result on exactly the database that snapshot sees.

**Composed claim.** Rocq (FormalSQL = Embed(Eval_Spec rows)) ⊕ Dafny
(accelerator = Obs(Eval_Spec rows)) ⊕ the trusted edges listed in
`q1/RESIDUALS.txt` / `q21/RESIDUALS.txt` (interpreter correspondence, C++ ↔
twin, PostgreSQL MVCC / callbacks / capture / scan, collation). Not a single
cross-prover theorem.

**Gates.** `scripts/run_verification_all.sh` → `scripts/run_verification_q1.sh`
and `q21/run_q21_verification.sh` (fail closed; the Q21 gate also runs the Q1
regressions and the live Postgres runtime gates).
