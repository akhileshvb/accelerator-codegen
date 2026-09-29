# PROOF_LIBRARY_INDEX — reusable lemmas for LLM agents

Format: **USE WHEN / REQUIRES / PROVES / FILE**

Presence in this index does not mean every item is fully extracted from Q1 yet.
Prefer FormalSQLLib facades and `logical/proof/Morphisms.v` patterns; leave Pack/Q1 Stats in Q1.

---

## Bags / lists / permutations

| Name | USE WHEN | REQUIRES | PROVES | FILE |
|------|----------|----------|--------|------|
| ListNoDupFacade | unique keys / order | list props | NoDup preservation | `formalsql/lib/ListNoDupFacade.v` |
| Permutation bags | reorder-invariant claims | Permutation | bag equality | Stdlib + query leaves |
| BagRel (Forall2) | RelNode↔RelNode ordered bag | row relation R | exact multiplicity + order | `logical/proof/SlotMap.v` (re-export Morphisms) |
| bag_correspond (Forall2) | ordered FormalSQL↔RelNode bags | row_correspond | exact multiplicity + order | `logical/proof/SlotMap.v` |
| bag_correspond_unordered | pre-ORDER-BY FormalSQL↔RelNode bags | Permutation + Forall2 row_correspond | exact multiplicity; order not semantic | `logical/proof/SlotMap.v` |

## FormalSQL → RelNode translation (verified fragment)

| Name | USE WHEN | REQUIRES | PROVES | FILE |
|------|----------|----------|--------|------|
| value_correspond | scalar FormalSQL↔SqlValue | supported ty only | value eq | `logical/proof/ValueCorrespond.v` |
| SlotMap_wf / row_correspond | schema-directed slots | name+type WF | slot i ↔ formal attr | `logical/proof/SlotMap.v` |
| bag_correspond | table bags | Forall2 row_correspond | exact multiplicity | `logical/proof/SlotMap.v` |
| FormalSQL_RelNode_ScanCorrespond | table scan bridge | SlotMap_wf + embed + sort | RelScan ≡ FormalSQL Table bag | `logical/proof/ScanCorrespond.v` |

## Filter

| Name | USE WHEN | REQUIRES | PROVES | FILE |
|------|----------|----------|--------|------|
| FilterMorphism | filtered bags correspond | pred correspondence under Rel | BagRel(filter,filter) | `logical/proof/Morphisms.v` |
| FilterAcceptanceFacade | FormalSQL filter acceptance | spine hyps | filter acceptance | `lib/FilterAcceptanceFacade.v` |

## Project

| Name | USE WHEN | REQUIRES | PROVES | FILE |
|------|----------|----------|--------|------|
| ProjectMorphism | projected bags correspond | expr correspondence | BagRel(project,project) | `logical/proof/Morphisms.v` |
| ProjectExactFacade | exact project | FormalSQL project | projected relation | `lib/ProjectExactFacade.v` |

## Join

| Name | USE WHEN | REQUIRES | PROVES | FILE |
|------|----------|----------|--------|------|
| JoinMorphism | join bags correspond | RelL/RelR + cond | BagRel(join,join) | `logical/proof/Morphisms.v` |
| SemiJoinMorphism | EXISTS / semi bags | outer+right cond | BagRel(semi,semi) once-per-outer | `logical/proof/Morphisms.v` |
| AntiJoinMorphism | NOT EXISTS / anti | outer+right cond | BagRel(anti,anti) | `logical/proof/Morphisms.v` |
| (Q1) | Q1 has no join | — | — | — |

## Aggregate / group-by / fold

| Name | USE WHEN | REQUIRES | PROVES | FILE |
|------|----------|----------|--------|------|
| FoldGroupsMorphism | group stores correspond | key+contrib maps | store Rel after fold | `logical/proof/Morphisms.v` |
| AggregateMorphism | finalize results correspond | fold morphism | agg Row bags Rel | `logical/proof/Morphisms.v` |
| GroupsExactFacade | FormalSQL groups | spine | group bags | `lib/GroupsExactFacade.v` |
| GroupBagExactFacade | group bag exactness | — | — | `lib/GroupBagExactFacade.v` |
| AggregateLeafExactFacade | agg leaf | — | — | `lib/AggregateLeafExactFacade.v` |
| NumericFoldFacade | numeric fold | — | — | `lib/NumericFoldFacade.v` |

## Ordering / sort

| Name | USE WHEN | REQUIRES | PROVES | FILE |
|------|----------|----------|--------|------|
| OrderingMorphism | sorted seqs correspond | unique keys + values + sorted | positional eq | `logical/proof/Morphisms.v` |
| KeyedSortFacade | keyed sort | — | — | `lib/KeyedSortFacade.v` |
| SortedGroupResultFacade | sorted group results | — | — | `lib/SortedGroupResultFacade.v` |
| OrderRowsFacade | order rows | — | — | `lib/OrderRowsFacade.v` |

## Maintenance skeleton

| Name | USE WHEN | REQUIRES | PROVES | FILE |
|------|----------|----------|--------|------|
| Inv = Fold | maintenance refine | ApplyDeltaCorrect | Inv preserved | query Dafny Aux |
| BuildCorrect | rebuild from base | oracle | BuildState ≡ Fold | query Twin/Aux |
| ApplyDeltaCorrect | single delta | Inv | Inv' | query Maint |
| ExecCorrect | execute ≡ Spec | Inv | outs ≡ Eval_Spec∘Adapt | query ResultSpec bridge |

## Runtime / snapshot

| Name | USE WHEN | REQUIRES | PROVES | FILE |
|------|----------|----------|--------|------|
| SnapshotHitSafe lift | Hit path | PairInv | ExecCorrect at snapshot | `substrate/dafny/*`, `QNRuntime*.dfy` |

## Q1 instantiation examples

| Pattern | Q1 leaf | Shared owner |
|---------|---------|--------------|
| FilterMorphism | `filter_rows_Q1` (flat) / shipdate≤cutoff | Morphisms + FilterAcceptance |
| FoldGroupsMorphism | Acc↔Stats in `Q1EvalMigration.v` | Morphisms (Pack is Q1-only) |
| OrderingMorphism | `result_row_le_Q1_keys` | KeyedSort / OrderingMorphism |
| Adapt | `Q1Adapt.v` (RelNode) / flat Adapt in EvalLogicalSpec scaffolding | query-specific TCB bridge |

## Explicitly Q1-specific (do not genericize)

- Pack / integer group keys
- Q1 Stats / SpecApply / FinalizeKeys
- AVG over scaled decimals: exact PostgreSQL avg(numeric) — `lib/NumericAggExact.v` (`interp_avg_numeric_fixed_scaled`, `pg_avg_exact`); SUM: `interp_sum_numeric_scaled`
- ModuleTwin SOURCE MAP / extract-diff (`cpp_model`)
