// Remaining Q1 maintenance edges: Install/CAS after StageMaintenance, and
// ChooseMaintenance outcomes mapped to Inv-preserving actions.
//
// SOURCE: partitioned_state_store.cpp (Publish/TryPublish/Invalidate)
//         aux_maintenance_policy.cpp / action_selector.cpp (Select*)
//         module.cpp StageMaintenance install of updates / invalidations

include "Q1Maintenance.dfy"
include "Q1ModuleTwin.dfy"
include "../../substrate/dafny/Store.dfy"
include "../../substrate/dafny/MaintenancePolicy.dfy"

module Q1MaintenanceEdges {
  import opened Q1Aux
  import opened Q1Maintenance
  import Twin = Q1ModuleTwin
  import SubstrateStore
  import SubstrateMaintenance

  // Ghost id for an aux DayStore/Fold image installed as a partition payload.
  // Non-zero means "has readable aux state"; 0 means null (store convention).
  ghost function AuxPayloadId(s: AuxState): int {
    if StateInv(s) && s.valid then 1 else 0
  }

  // ----- Install / CAS after a successful Inv-preserving maintenance step -----

  lemma SuccessfulPublishKeepsReadableInv(
      sub: SubstrateStore.Store, key: int, s: AuxState, epoch: nat)
    requires StateInv(s) && s.valid
    requires AuxPayloadId(s) != 0
    ensures var sub2 := SubstrateStore.PublishPartition(sub, key, AuxPayloadId(s), epoch);
            SubstrateStore.GetStatus(sub2, key) == SubstrateStore.Valid &&
            SubstrateStore.GetReadablePayload(sub2, key) == AuxPayloadId(s) &&
            StateInv(s)
  {
    SubstrateStore.PublishThenReadable(sub, key, AuxPayloadId(s), epoch);
  }

  lemma SuccessfulTryPublishKeepsReadableInv(
      sub: SubstrateStore.Store, key: int, s: AuxState, epoch: nat)
    requires StateInv(s) && s.valid
    requires AuxPayloadId(s) != 0
    ensures var r := SubstrateStore.TryPublishPartition(
              sub, key, AuxPayloadId(s), epoch, 0);
            r.1 &&
            SubstrateStore.GetStatus(r.0, key) == SubstrateStore.Valid &&
            SubstrateStore.GetReadablePayload(r.0, key) == AuxPayloadId(s)
  {
    SubstrateStore.TryPublishSuccessReadable(sub, key, AuxPayloadId(s), epoch);
  }

  // CAS failure invalidates the partition; Execute must fall back (not trust store).
  lemma CasFailInvalidatesForcesFallback(
      sub: SubstrateStore.Store, key: int, s: AuxState, epoch: nat, expected: nat)
    requires expected != 0
    requires SubstrateStore.GetPartitionVersion(sub, key) != expected
    ensures var r := SubstrateStore.TryPublishPartition(
              sub, key, AuxPayloadId(s), epoch, expected);
            !r.1 &&
            SubstrateStore.GetStatus(r.0, key) == SubstrateStore.Invalid &&
            SubstrateStore.GetReadablePayload(r.0, key) == 0
  {
    SubstrateStore.TryPublishCasFailInvalidates(sub, key, AuxPayloadId(s), epoch, expected);
  }

  lemma InvalidatePartitionClearsReadable(sub: SubstrateStore.Store, key: int, s: AuxState)
    requires StateInv(s)
    ensures var sub2 := SubstrateStore.InvalidatePartition(sub, key);
            var s2 := ApplyMaint(s, InvalidateAndFallback);
            SubstrateStore.GetStatus(sub2, key) == SubstrateStore.Invalid &&
            SubstrateStore.GetReadablePayload(sub2, key) == 0 &&
            !s2.valid && Inv(s2.store, s2.table)
  {
    SubstrateStore.InvalidateClearsReadable(sub, key);
    MaintInvalidate(s);
  }

  // After StageMaintenance produces an Inv state, installing it (publish) keeps
  // the logical aux Inv; failed CAS is observationally InvalidateAndFallback.
  lemma InstallAfterMaintPreservesInv(
      s: AuxState, a: MaintAction, sub: SubstrateStore.Store, key: int, epoch: nat)
    requires StateInv(s) && ActionOk(s, a)
    requires !a.InvalidateAndFallback?
    ensures var s2 := ApplyMaint(s, a);
            StateInv(s2) && s2.valid == s.valid &&
            (s2.valid ==>
              var sub2 := SubstrateStore.PublishPartition(
                    sub, key, AuxPayloadId(s2), epoch);
              SubstrateStore.GetStatus(sub2, key) == SubstrateStore.Valid &&
              SubstrateStore.GetReadablePayload(sub2, key) == AuxPayloadId(s2))
  {
    MaintenancePreservesInv(s, a);
    var s2 := ApplyMaint(s, a);
    if s2.valid {
      assert AuxPayloadId(s2) != 0;
      SubstrateStore.PublishThenReadable(sub, key, AuxPayloadId(s2), epoch);
    }
  }

  // ----- ChooseMaintenance → Inv-preserving MaintAction -----

  ghost predicate PolicyActionIsSafe(a: SubstrateMaintenance.MaintenanceAction) {
    a == SubstrateMaintenance.NoOp ||
    a == SubstrateMaintenance.ApplyDelta ||
    a == SubstrateMaintenance.RebuildPartition ||
    a == SubstrateMaintenance.RebuildAll ||
    a == SubstrateMaintenance.InvalidateAndFallback
  }

  lemma EarlySelectIsSafe(
      forced: SubstrateMaintenance.Option<SubstrateMaintenance.MaintenanceAction>,
      request: SubstrateMaintenance.MaintenanceRequest)
    ensures var r := SubstrateMaintenance.SelectAuxMaintenanceActionEarly(forced, request);
            r.None? || PolicyActionIsSafe(r.value)
  {
  }

  lemma MinSupportedIsSafe(cands: seq<SubstrateMaintenance.Candidate>)
    requires forall i :: 0 <= i < |cands| ==> PolicyActionIsSafe(cands[i].action)
    ensures PolicyActionIsSafe(SubstrateMaintenance.MinSupported(cands))
    decreases |cands|
  {
    MinSupportedIsSafeRec(cands, SubstrateMaintenance.None);
  }

  lemma MinSupportedIsSafeRec(
      cands: seq<SubstrateMaintenance.Candidate>,
      best: SubstrateMaintenance.Option<(SubstrateMaintenance.MaintenanceAction, int)>)
    requires forall i :: 0 <= i < |cands| ==> PolicyActionIsSafe(cands[i].action)
    requires best.None? || PolicyActionIsSafe(best.value.0)
    ensures PolicyActionIsSafe(SubstrateMaintenance.MinSupportedRec(cands, best))
    decreases |cands|
  {
    if |cands| == 0 {
    } else if !cands[0].supported {
      MinSupportedIsSafeRec(cands[1..], best);
    } else if best.None? || cands[0].cost < best.value.1 {
      MinSupportedIsSafeRec(cands[1..],
        SubstrateMaintenance.Some((cands[0].action, cands[0].cost)));
    } else {
      MinSupportedIsSafeRec(cands[1..], best);
    }
  }

  // Unpack packed group_key from StageMaintenance LineitemDelta into Q1Aux.Row.
  ghost function DeltaToRow(d: Twin.LineitemDelta): Row
    requires 0 <= d.group_key < 65536
  {
    Row(d.shipdate_days, d.quantity_raw, d.extended_price_raw,
        d.discount_raw, d.tax_raw,
        d.group_key / 256, d.group_key % 256)
  }

  ghost function DeltasToRows(ds: seq<Twin.LineitemDelta>): seq<Row>
    requires forall i :: 0 <= i < |ds| ==> 0 <= ds[i].group_key < 65536
    ensures |DeltasToRows(ds)| == |ds|
    ensures forall i :: 0 <= i < |DeltasToRows(ds)| ==>
              0 <= DeltasToRows(ds)[i].rf < 256 && 0 <= DeltasToRows(ds)[i].ls < 256
    decreases |ds|
  {
    if |ds| == 0 then []
    else [DeltaToRow(ds[0])] + DeltasToRows(ds[1..])
  }

  ghost function InsertDeltas(ds: seq<Twin.LineitemDelta>): seq<Twin.LineitemDelta>
    requires forall i :: 0 <= i < |ds| ==> 0 <= ds[i].group_key < 65536
    ensures forall i :: 0 <= i < |InsertDeltas(ds)| ==>
              0 <= InsertDeltas(ds)[i].group_key < 65536
    ensures forall i :: 0 <= i < |InsertDeltas(ds)| ==> InsertDeltas(ds)[i].sign == 1
    decreases |ds|
  {
    if |ds| == 0 then []
    else if ds[0].sign == 1 then [ds[0]] + InsertDeltas(ds[1..])
    else InsertDeltas(ds[1..])
  }

  ghost function DeleteDeltas(ds: seq<Twin.LineitemDelta>): seq<Twin.LineitemDelta>
    requires forall i :: 0 <= i < |ds| ==> 0 <= ds[i].group_key < 65536
    ensures forall i :: 0 <= i < |DeleteDeltas(ds)| ==>
              0 <= DeleteDeltas(ds)[i].group_key < 65536
    ensures forall i :: 0 <= i < |DeleteDeltas(ds)| ==> DeleteDeltas(ds)[i].sign == -1
    decreases |ds|
  {
    if |ds| == 0 then []
    else if ds[0].sign == -1 then [ds[0]] + DeleteDeltas(ds[1..])
    else DeleteDeltas(ds[1..])
  }

  // Map policy action + StageMaintenance delta payload into Inv MaintAction.
  // ApplyDelta with insert-signed deltas → ApplyInsertSeq(rows).
  // ApplyDelta with delete-signed suffix → ApplyDeleteSuffix(rows).
  // Mixed / empty → NoOp when no rows; inserts preferred when both present
  // (updates are modeled as delete+insert via MaintUpdate* separately).
  ghost function MapPolicyToMaint(
      a: SubstrateMaintenance.MaintenanceAction,
      deltas: seq<Twin.LineitemDelta>): MaintAction
    requires forall i :: 0 <= i < |deltas| ==> 0 <= deltas[i].group_key < 65536
  {
    match a
      case NoOp => NoOp
      case ApplyDelta =>
        var ins := InsertDeltas(deltas);
        var del := DeleteDeltas(deltas);
        if |ins| > 0 then
          ApplyInsertSeq(DeltasToRows(ins))
        else if |del| > 0 then
          ApplyDeleteSuffix(DeltasToRows(del))
        else
          ApplyInsertSeq([])
      case RebuildPartition => RebuildPartition
      case RebuildAll => RebuildAll
      case InvalidateAndFallback => InvalidateAndFallback
  }

  lemma MapPolicyActionOk(
      s: AuxState,
      a: SubstrateMaintenance.MaintenanceAction,
      deltas: seq<Twin.LineitemDelta>)
    requires StateOk(s) && PolicyActionIsSafe(a)
    requires forall i :: 0 <= i < |deltas| ==> 0 <= deltas[i].group_key < 65536
    requires a != SubstrateMaintenance.ApplyDelta ||
             |DeleteDeltas(deltas)| == 0 ||
             (|InsertDeltas(deltas)| == 0 &&
              |s.table| >= |DeleteDeltas(deltas)| &&
              s.table[|s.table| - |DeleteDeltas(deltas)|..]
                == DeltasToRows(DeleteDeltas(deltas)))
    ensures ActionOk(s, MapPolicyToMaint(a, deltas))
  {
  }

  lemma ChosenPolicyPreservesInv(
      s: AuxState,
      a: SubstrateMaintenance.MaintenanceAction,
      deltas: seq<Twin.LineitemDelta>)
    requires StateInv(s) && PolicyActionIsSafe(a)
    requires forall i :: 0 <= i < |deltas| ==> 0 <= deltas[i].group_key < 65536
    requires a != SubstrateMaintenance.ApplyDelta ||
             |DeleteDeltas(deltas)| == 0 ||
             (|InsertDeltas(deltas)| == 0 &&
              |s.table| >= |DeleteDeltas(deltas)| &&
              s.table[|s.table| - |DeleteDeltas(deltas)|..]
                == DeltasToRows(DeleteDeltas(deltas)))
    ensures var m := MapPolicyToMaint(a, deltas);
            ActionOk(s, m) &&
            (var s2 := ApplyMaint(s, m);
             Inv(s2.store, s2.table))
  {
    MapPolicyActionOk(s, a, deltas);
    MaintenancePreservesInv(s, MapPolicyToMaint(a, deltas));
  }

  lemma ApplyDeltaInsertsPreserveInv(
      s: AuxState, deltas: seq<Twin.LineitemDelta>)
    requires StateInv(s)
    requires forall i :: 0 <= i < |deltas| ==> 0 <= deltas[i].group_key < 65536
    requires forall i :: 0 <= i < |deltas| ==> deltas[i].sign == 1
    ensures var m := MapPolicyToMaint(SubstrateMaintenance.ApplyDelta, deltas);
            m == ApplyInsertSeq(DeltasToRows(deltas)) &&
            ActionOk(s, m) &&
            StateInv(ApplyMaint(s, m))
  {
    assert InsertDeltas(deltas) == deltas by {
      InsertDeltas_all_plus(deltas);
    }
    assert DeleteDeltas(deltas) == [] by {
      DeleteDeltas_none_when_all_plus(deltas);
    }
    MapPolicyActionOk(s, SubstrateMaintenance.ApplyDelta, deltas);
    MaintInsertSeq(s, DeltasToRows(deltas));
  }

  lemma InsertDeltas_all_plus(ds: seq<Twin.LineitemDelta>)
    requires forall i :: 0 <= i < |ds| ==> 0 <= ds[i].group_key < 65536
    requires forall i :: 0 <= i < |ds| ==> ds[i].sign == 1
    ensures InsertDeltas(ds) == ds
    decreases |ds|
  {
    if |ds| == 0 {
    } else {
      InsertDeltas_all_plus(ds[1..]);
    }
  }

  lemma DeleteDeltas_none_when_all_plus(ds: seq<Twin.LineitemDelta>)
    requires forall i :: 0 <= i < |ds| ==> 0 <= ds[i].group_key < 65536
    requires forall i :: 0 <= i < |ds| ==> ds[i].sign == 1
    ensures DeleteDeltas(ds) == []
    decreases |ds|
  {
    if |ds| == 0 {
    } else {
      DeleteDeltas_none_when_all_plus(ds[1..]);
    }
  }

  lemma ForcedOrEarlyThenMaintPreservesInv(
      s: AuxState,
      forced: SubstrateMaintenance.Option<SubstrateMaintenance.MaintenanceAction>,
      request: SubstrateMaintenance.MaintenanceRequest,
      deltas: seq<Twin.LineitemDelta>)
    requires StateInv(s)
    requires forced.None? || PolicyActionIsSafe(forced.value)
    requires forall i :: 0 <= i < |deltas| ==> 0 <= deltas[i].group_key < 65536
    // Early-selected ApplyDelta must satisfy ActionOk delete-suffix hyps when used.
    requires |DeleteDeltas(deltas)| == 0 ||
             (|InsertDeltas(deltas)| == 0 &&
              |s.table| >= |DeleteDeltas(deltas)| &&
              s.table[|s.table| - |DeleteDeltas(deltas)|..]
                == DeltasToRows(DeleteDeltas(deltas)))
    ensures var early := SubstrateMaintenance.SelectAuxMaintenanceActionEarly(forced, request);
            early.None? ||
            (PolicyActionIsSafe(early.value) &&
             (var m := MapPolicyToMaint(early.value, deltas);
              ActionOk(s, m) && Inv(ApplyMaint(s, m).store, ApplyMaint(s, m).table)))
  {
    EarlySelectIsSafe(forced, request);
    var early := SubstrateMaintenance.SelectAuxMaintenanceActionEarly(forced, request);
    if early.Some? {
      ChosenPolicyPreservesInv(s, early.value, deltas);
    }
  }

  lemma EmptyApplyDeltaIsNoOp(s: AuxState)
    requires StateInv(s)
    ensures ApplyMaint(s, ApplyInsertSeq([])) == s
    ensures StateInv(ApplyMaint(s, ApplyInsertSeq([])))
    ensures MapPolicyToMaint(SubstrateMaintenance.ApplyDelta, []) == ApplyInsertSeq([])
  {
    assert ApplyDeltaSeq(s.store, [], 1) == s.store;
    assert s.table + [] == s.table;
    MaintInsertSeq(s, []);
  }

  // StageMaintenance ApplyDelta path: insert-signed buffer deltas preserve Inv.
  lemma StageMaintApplyDeltaPreservesInv(
      s: AuxState, buf: Twin.TxnBuffer, snapshot_unused: Twin.DayStore,
      rebuilt_unused: Twin.DayStore)
    requires StateInv(s)
    requires !buf.requires_rebuild
    requires |buf.deltas| > 0
    requires forall i :: 0 <= i < |buf.deltas| ==> 0 <= buf.deltas[i].group_key < 65536
    requires forall i :: 0 <= i < |buf.deltas| ==> buf.deltas[i].sign == 1
    ensures Twin.StageMaintenance(buf, Twin.ApplyDelta, snapshot_unused, rebuilt_unused).action
         == Twin.ApplyDelta
    ensures var m := MapPolicyToMaint(SubstrateMaintenance.ApplyDelta, buf.deltas);
            StateInv(ApplyMaint(s, m))
  {
    Twin.StageMaintenanceApplyDelta(buf, snapshot_unused, rebuilt_unused);
    ApplyDeltaInsertsPreserveInv(s, buf.deltas);
  }
}
