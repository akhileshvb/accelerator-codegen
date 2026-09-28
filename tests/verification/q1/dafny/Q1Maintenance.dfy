// Inv-under-maintenance spine for TPC-H Q1 aux.
//
// PATTERN recipe (do not abstract yet): Maint* wrappers → MaintenancePreservesInv
// → MaintTwoStepPreservesInv / InvalidateThenRebuild. See
//   tests/verification/patterns/PartitionedGroupAggregate.md
//
// SOURCE (control flow): StageMaintenance / AccumulateMutation in
//   src/generated/tpch/q1/postgres/aux/module.cpp
// Algebra: Q1Aux Fold/Inv with erase-on-zero ApplyDelta (matches ApplyDelta loop).
//
// Claim: each maintenance action preserves Inv(store, table) when the action
// keeps the store readable; InvalidateAndFallback clears validity so Execute
// uses rebuild/fallback (Compose(Fold(scan))) instead of the stale store.

include "Q1Aux.dfy"

module Q1Maintenance {
  import opened Q1Aux

  ghost predicate TableOk(t: seq<Row>) {
    forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  }

  ghost predicate RowOk(r: Row) {
    0 <= r.rf < 256 && 0 <= r.ls < 256
  }

  // Actions ChooseMaintenance / StageMaintenance can take for Q1.
  datatype MaintAction =
    | NoOp
    | ApplyInsert(r: Row)
    | ApplyDeleteLast(r: Row)       // undo last appended row (suffix delete)
    | ApplyDeleteAt(i: nat)         // delete at arbitrary index (Fold-commutative)
    | ApplyInsertSeq(rows: seq<Row>)
    | ApplyDeleteSuffix(rows: seq<Row>)
    | RebuildPartition              // store := Fold(table)
    | RebuildAll                    // store := Fold(table)
    | InvalidateAndFallback         // valid := false; store unchanged

  datatype AuxState = AuxState(
    store: Store,
    table: seq<Row>,
    valid: bool
  )

  ghost predicate StateOk(s: AuxState) {
    TableOk(s.table)
  }

  ghost predicate StateInv(s: AuxState) {
    StateOk(s) && Inv(s.store, s.table)
  }

  ghost predicate ActionOk(s: AuxState, a: MaintAction) {
    StateOk(s) &&
    match a
      case NoOp => true
      case ApplyInsert(r) => RowOk(r)
      case ApplyDeleteLast(r) =>
        RowOk(r) && |s.table| > 0 && s.table[|s.table| - 1] == r
      case ApplyDeleteAt(i) => i < |s.table|
      case ApplyInsertSeq(rows) =>
        forall i :: 0 <= i < |rows| ==> RowOk(rows[i])
      case ApplyDeleteSuffix(rows) =>
        |s.table| >= |rows| &&
        s.table[|s.table| - |rows|..] == rows &&
        forall i :: 0 <= i < |rows| ==> RowOk(rows[i])
      case RebuildPartition => true
      case RebuildAll => true
      case InvalidateAndFallback => true
  }

  // One maintenance step (model of StageMaintenance outcome + Install).
  ghost function ApplyMaint(s: AuxState, a: MaintAction): AuxState
    requires ActionOk(s, a)
  {
    match a
      case NoOp =>
        s
      case ApplyInsert(r) =>
        AuxState(ApplyDelta(s.store, r, 1), s.table + [r], s.valid)
      case ApplyDeleteLast(r) =>
        AuxState(ApplyDelta(s.store, r, -1), s.table[..|s.table| - 1], s.valid)
      case ApplyDeleteAt(i) =>
        AuxState(ApplyDelta(s.store, s.table[i], -1), RemoveAt(s.table, i), s.valid)
      case ApplyInsertSeq(rows) =>
        AuxState(ApplyDeltaSeq(s.store, rows, 1), s.table + rows, s.valid)
      case ApplyDeleteSuffix(rows) =>
        AuxState(ApplyDeltaSeqRev(s.store, rows, -1),
                 s.table[..|s.table| - |rows|], s.valid)
      case RebuildPartition =>
        AuxState(Fold(s.table), s.table, s.valid)
      case RebuildAll =>
        AuxState(Fold(s.table), s.table, s.valid)
      case InvalidateAndFallback =>
        AuxState(s.store, s.table, false)
  }

  // ----- Per-action Inv preservation -----

  lemma MaintNoOp(s: AuxState)
    requires StateInv(s)
    ensures StateInv(ApplyMaint(s, NoOp))
  {
  }

  lemma MaintInsert(s: AuxState, r: Row)
    requires StateInv(s) && RowOk(r)
    ensures StateInv(ApplyMaint(s, ApplyInsert(r)))
  {
    InvPreservedByInsert(s.store, s.table, r);
  }

  lemma MaintDeleteLast(s: AuxState, r: Row)
    requires StateInv(s) && ActionOk(s, ApplyDeleteLast(r))
    ensures StateInv(ApplyMaint(s, ApplyDeleteLast(r)))
  {
    assert s.table == s.table[..|s.table| - 1] + [r];
    InvPreservedByDeleteLast(s.store, s.table[..|s.table| - 1], r);
  }

  lemma MaintDeleteAt(s: AuxState, i: nat)
    requires StateInv(s) && ActionOk(s, ApplyDeleteAt(i))
    ensures StateInv(ApplyMaint(s, ApplyDeleteAt(i)))
  {
    InvPreservedByDeleteAt(s.store, s.table, i);
  }

  lemma MaintInsertSeq(s: AuxState, rows: seq<Row>)
    requires StateInv(s) && ActionOk(s, ApplyInsertSeq(rows))
    ensures StateInv(ApplyMaint(s, ApplyInsertSeq(rows)))
  {
    ApplyDeltaSeqInsert(s.store, s.table, rows);
  }

  lemma MaintDeleteSuffix(s: AuxState, rows: seq<Row>)
    requires StateInv(s) && ActionOk(s, ApplyDeleteSuffix(rows))
    ensures StateInv(ApplyMaint(s, ApplyDeleteSuffix(rows)))
  {
    var prefix := s.table[..|s.table| - |rows|];
    assert s.table == prefix + rows;
    ApplyDeltaSeqRevDeleteSuffix(s.store, prefix, rows);
  }

  lemma MaintRebuildPartition(s: AuxState)
    requires StateOk(s)
    ensures StateInv(ApplyMaint(s, RebuildPartition))
  {
    RebuildEstablishesInv(s.table);
  }

  lemma MaintRebuildAll(s: AuxState)
    requires StateOk(s)
    ensures StateInv(ApplyMaint(s, RebuildAll))
  {
    RebuildEstablishesInv(s.table);
  }

  lemma MaintInvalidate(s: AuxState)
    requires StateInv(s)
    ensures var s2 := ApplyMaint(s, InvalidateAndFallback);
            s2.store == s.store && s2.table == s.table && !s2.valid &&
            Inv(s2.store, s2.table)
  {
  }

  // Same-day update = delete last matching old row then insert new (AccumulateUpdates).
  lemma MaintUpdateSameDay(s: AuxState, old_r: Row, new_r: Row)
    requires StateInv(s) && RowOk(old_r) && RowOk(new_r)
    requires ActionOk(s, ApplyDeleteLast(old_r))
    ensures StateInv(ApplyMaint(ApplyMaint(s, ApplyDeleteLast(old_r)), ApplyInsert(new_r)))
  {
    MaintDeleteLast(s, old_r);
    var s1 := ApplyMaint(s, ApplyDeleteLast(old_r));
    MaintInsert(s1, new_r);
  }

  // Update of a row at arbitrary index (non-suffix AccumulateUpdates).
  lemma MaintUpdateAt(s: AuxState, i: nat, new_r: Row)
    requires StateInv(s) && RowOk(new_r) && i < |s.table|
    ensures StateInv(ApplyMaint(ApplyMaint(s, ApplyDeleteAt(i)), ApplyInsert(new_r)))
  {
    MaintDeleteAt(s, i);
    var s1 := ApplyMaint(s, ApplyDeleteAt(i));
    MaintInsert(s1, new_r);
  }

  // Top theorem: every StageMaintenance-class action preserves Inv, or
  // Invalidate clears valid while leaving Inv(store, table) intact for rebuild.
  lemma MaintenancePreservesInv(s: AuxState, a: MaintAction)
    requires StateInv(s) && ActionOk(s, a)
    ensures var s2 := ApplyMaint(s, a);
            Inv(s2.store, s2.table) &&
            (a.InvalidateAndFallback? ==> !s2.valid) &&
            (!a.InvalidateAndFallback? ==> s2.valid == s.valid)
  {
    match a
      case NoOp =>
        MaintNoOp(s);
      case ApplyInsert(r) =>
        MaintInsert(s, r);
      case ApplyDeleteLast(r) =>
        MaintDeleteLast(s, r);
      case ApplyDeleteAt(i) =>
        MaintDeleteAt(s, i);
      case ApplyInsertSeq(rows) =>
        MaintInsertSeq(s, rows);
      case ApplyDeleteSuffix(rows) =>
        MaintDeleteSuffix(s, rows);
      case RebuildPartition =>
        MaintRebuildPartition(s);
      case RebuildAll =>
        MaintRebuildAll(s);
      case InvalidateAndFallback =>
        MaintInvalidate(s);
  }

  // Reachability: Inv composes for finite sequences (2-step form; n-step by induction).
  lemma MaintTwoStepPreservesInv(s: AuxState, a1: MaintAction, a2: MaintAction)
    requires StateInv(s)
    requires ActionOk(s, a1)
    requires ActionOk(ApplyMaint(s, a1), a2)
    ensures StateInv(ApplyMaint(ApplyMaint(s, a1), a2))
  {
    MaintenancePreservesInv(s, a1);
    var s1 := ApplyMaint(s, a1);
    assert StateInv(s1);
    MaintenancePreservesInv(s1, a2);
  }

  // When Inv holds, query residual Compose(table) is the accelerator answer
  // Compose/Finalize chain; SQL meaning is FormalSQL (Logos), not a Dafny MathSem.
  lemma InvImpliesComposeStable(s: AuxState, cutoff: int)
    requires StateInv(s)
    ensures Compose(s.table, cutoff) == FoldByGroup(FilterCutoff(s.table, cutoff))
  {
    ComposeEqualsFilteredFold(s.table, cutoff);
  }

  // After Invalidate, Execute must not trust store; rebuild restores Inv.
  lemma InvalidateThenRebuild(s: AuxState)
    requires StateInv(s)
    ensures var s1 := ApplyMaint(s, InvalidateAndFallback);
            var s2 := ApplyMaint(s1, RebuildAll);
            !s1.valid && StateInv(s2) && s2.store == Fold(s.table)
  {
    MaintInvalidate(s);
    var s1 := ApplyMaint(s, InvalidateAndFallback);
    MaintRebuildAll(s1);
  }
}
