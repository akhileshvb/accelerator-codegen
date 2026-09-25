# Verification pattern libraries

Shared proof components extracted from Q1 for reuse on the next accelerator.
Prefer the existing layout over inventing a parallel tree.

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

substrate/dafny/SnapshotPinning.dfy     # Hit → Inv skeleton
```

See also: `../REUSE_LIBRARY_REPORT.md` (inventory, metrics, next-query pick);
`../q12/Q12_REUSE_REPORT.md` (Q12 reuse measurement).
