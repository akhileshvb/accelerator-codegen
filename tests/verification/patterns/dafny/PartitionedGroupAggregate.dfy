// PATTERN-GENERIC documentation for PartitionedGroupAggregate Compose.
//
// Full recipe: ../PartitionedGroupAggregate.md
//
// Abstraction (matches Q1Aux Fold/Compose and Q1ResultSpec SpecView):
//   partition(r) : P      — Q1: shipdate
//   key(r)       : K      — Q1: Pack(rf,ls)
//   accept(p,θ)  : bool   — Q1: day <= cutoff
//   contrib      : V⊕V    — Q1: ApplyRow / SpecApply
//   finalize     : V→Out  — Q1: FinalizeOne / SpecFinalize
//   orderKey     : Out→K  — Q1: Pack(rf,ls)
//
// Inv(A,D): A[p,k] = ⊕{contrib(r) | partition(r)=p ∧ key(r)=k}
// Compose from DB: FoldByKey(Filter(Accept(·,θ))) under Inv.
//
// Obligations a new PGA query must supply:
//   KeyMatchesSpec, FilterMatchesSpec, ContribMatchesSpec,
//   FoldOrComposeMatchesSpec, FinalizeMatchesSpec, OrderMatchesSpec,
//   MaintenancePreservesInv.
//
// Dafny abstract-module refinement was abandoned (friction > benefit).
// Reusable proved library: SortedUniqueMap.dfy (SameMapSameSortedSeq).
// PGA structure is a documented recipe; Q1 uses KeysFrom(0,65536) as the
// deterministic KeyStore→sorted-seq construction for ORDERING_PROVED.
//
// Maintenance: InvPreservedByInsert / DeleteLast in Q1Aux.dfy are the generic
// Fold-Inv pattern. Do not rewrite Q1Maintenance for abstraction.
// Copy MaintTwoStepPreservesInv / InvalidateThenRebuild shape per query.

module PartitionedGroupAggregateNotes {
}
