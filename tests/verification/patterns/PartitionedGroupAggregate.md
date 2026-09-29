# PartitionedGroupAggregate (PGA) pattern recipe

Documentation-only pattern interface extracted from the Q1 verification.
**Do not** force a giant abstract Dafny/Rocq module; instantiate these hooks
per query and reuse the shared libraries listed below.

## Interface

```text
partition(row)     : P          — day / date / other partition cell
key(row)           : K          — group key (Pack, shipmode, …)
accept(partition, params) : bool
contrib(row)       : V          — additive contribution (⊕ monoid)
finalize(aggregate): OutRow
orderKey(outrow)   : comparable
Inv(state, db)     — store cells = ⊕ contrib over matching rows
```

## Proof obligations (per query)

```text
KeyMatchesSpec          key(r) = SpecKey(r)
FilterMatchesSpec       accept(p,θ) ⇔ SpecFilter(r,θ)
ContribMatchesSpec      contrib(r) = SpecContrib(r)
FoldOrComposeMatchesSpec
                        FoldByKey(Filter(accept)) = SpecView
FinalizeMatchesSpec     finalize ≈ SpecFinalize (exact or ≈_Q)
OrderMatchesSpec        sorted unique maps equal ⇒ sorted seqs equal
MaintenancePreservesInv each StageMaintenance action preserves Inv
```

## Q1 instantiation

```text
partition = shipdate
key       = Pack(returnflag, linestatus)
accept    = day ≤ cutoff
contrib   = Q1 Stats vector (qty, base, disc_price, charge, discount, count)
finalize  = Q1 OutRow (flags + sums + avgs)
orderKey  = Pack(rf, ls) ascending
Inv       = day×key cells = FoldTwin(db)
```

## Shared libraries to reuse (already proved)

| Component | Provides | New query must supply |
|---|---|---|
| `patterns/dafny/SortedUniqueMap.dfy` | `SameMapSameSortedSeq` | StrictSorted + same keys + same vals |
| `formalsql/lib/KeyedSortFacade.v` | `sorted_perm_eq_in` / `sorted_perm_eq` | le_trans (+ local antisym or global) |
| `formalsql/lib/ListNoDupFacade.v` | NoDup(map), filter uniqueness | key injectivity / ≤1 per key |
| `formalsql/lib/NumericFoldFacade.v` | Z-sum over `rev`/`++` | map of numeric projections |
| `formalsql/lib/NumericScaledFits.v` | of_scaled / digit-budget | typmod bounds for NUMERIC cells |
| `formalsql/lib/FilterAcceptanceFacade.v` + `TemporalPredAcceptanceFacade.v` | DATE≤CAST+INTERVAL filter | predicate AST match |
| `formalsql/resultspec/*` | ResultSpec Data/Eval operators | query Data + Eval instance |
| `substrate/dafny/AuxSyncProtocol.dfy` | Cross-backend sync protocol (Build / own commit / hit) | query store invariant + dbof |

## Maintenance reachability (recipe, not abstracted)

From Q1 `Q1Maintenance.dfy` (keep query-typed; copy structure):

```text
Maint* per action          → thin wrappers on InvPreservedBy*
MaintenancePreservesInv    → match on MaintAction
MaintTwoStepPreservesInv   → compose two steps
InvalidateThenRebuild      → valid:=false then Fold rebuild
```

Do **not** lift `AuxState`/`MaintAction` into a parameterized module unless a
second query proves the friction pays off.

## Fold/Compose morphisms (PATTERN-REUSABLE, typed per query)

Copy the induction shape from `Q1ResultSpecBridge.dfy`:

```text
FilterCutoff_eq_Spec
FoldByGroup_eq_Spec
Compose_eq_SpecView
Compose_positive_counts
```

These stay query-typed (Row/Stats) until a second PGA proves a shared
parameterization is cheaper than copy-paste.

## What stays query-specific

- contrib algebra / Stats vector
- finalize / AVG observation TCB
- key encoding (Pack, string shipmode, …)
- Inv store layout (day×key vs flat key)
- FormalSQL schema/grouping terms
