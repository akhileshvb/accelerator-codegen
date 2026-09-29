# Verification pattern libraries

Shared proof components extracted from Q1 for reuse on the next accelerator.
Prefer the existing layout over inventing a parallel tree.
`CATALOG.json` in this directory is the index the verification driver updates when a proof session finds a lemma later queries can use.

```text
formalsql/lib/                 # Rocq GENERIC / FormalSQL leaf facades
  ListNoDupFacade.v            # NoDup(map), filter uniqueness
  KeyedSortFacade.v            # sorted + perm ⇒ equality
  NumericFoldFacade.v          # Z-sum over rev/app
  NumericScaledFits.v          # scaled NUMERIC algebra
  FilterAcceptanceFacade.v
  TemporalPredAcceptanceFacade.v
  …                            # spine / groups / aggregates / packaging

formalsql/resultspec/          # ResultSpec operators (query-independent core)

patterns/dafny/
  SortedUniqueMap.dfy          # SameMapSameSortedSeq (GENERIC, verified)
  PartitionedGroupAggregate.dfy # notes only

patterns/PartitionedGroupAggregate.md   # recipe + obligations

substrate/dafny/AuxSyncProtocol.dfy     # cross-backend sync: hit ⇒ store matches snapshot
```

See also: [`../q1/CURRENT_THEOREM_TCB.md`](../q1/CURRENT_THEOREM_TCB.md).
