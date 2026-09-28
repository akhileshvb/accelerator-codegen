# Q1 FormalSQL proofs

Single status doc: **[STATUS.md](STATUS.md)** (points to
[`../../q1/CURRENT_THEOREM_TCB.md`](../../q1/CURRENT_THEOREM_TCB.md) for TCB).

## Layout

```text
q1/
  STATUS.md                     # Qed table; TCB → q1/CURRENT_THEOREM_TCB.md
  catalog_base_sql.sql          # Cat(Q1) SQL instance (delta=90 filled)
  Q1Catalog.v                   # catalog hash pin
  generated/                    # Logos-lowered Schema/Queries/Witness
  Q1FormalSQLProof.v            # qhat_Q1(delta), Embed, Pre, NoError
  Q1GroupingCorrespondence.v
  Q1AuxCorrectAssembly.v
  Q1Obligations.v
  Q1FormalSQLSpine.v            # leaf Props ∀ delta + of_leaves + thin reductions
../lib/QuerySpineFacade.v
../lib/FilterAcceptanceFacade.v
../lib/TemporalPredAcceptanceFacade.v
../lib/NumericCallExactFacade.v
../lib/ProjectExactFacade.v
../lib/OrderRowsFacade.v
../lib/GroupBagExactFacade.v
../lib/GroupsExactFacade.v
../lib/AggregateLeafExactFacade.v
../generated/Q1AuxSpec.v
```

## Gates

```bash
tests/verification/formalsql/scripts/compile_q1_proof.sh
tests/verification/formalsql/scripts/check_q1_rocq_no_admitted.sh
```

`logos_log/` is **not** kept here — recreated by `scripts/logos_lower_catalog.sh q1`
into `generated/`.
