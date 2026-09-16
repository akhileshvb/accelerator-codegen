// Remaining Q1 maintenance edges: Install/CAS after StageMaintenance, and
// ChooseMaintenance outcomes mapped to Inv-preserving actions.
//
// SOURCE: partitioned_state_store.cpp (Publish/TryPublish/Invalidate)
//         aux_maintenance_policy.cpp / action_selector.cpp (Select*)
//         module.cpp StageMaintenance install of updates / invalidations

include "Q1Maintenance.dfy"
include "../../substrate/dafny/Store.dfy"
include "../../substrate/dafny/MaintenancePolicy.dfy"

module Q1MaintenanceEdges {
  import opened Q1Aux
  import opened Q1Maintenance
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

  ghost function MapPolicyToMaint(a: SubstrateMaintenance.MaintenanceAction): MaintAction
  {
    match a
      case NoOp => NoOp
      case ApplyDelta => ApplyInsertSeq([])
      case RebuildPartition => RebuildPartition
      case RebuildAll => RebuildAll
      case InvalidateAndFallback => InvalidateAndFallback
  }

  lemma MapPolicyActionOk(s: AuxState, a: SubstrateMaintenance.MaintenanceAction)
    requires StateOk(s) && PolicyActionIsSafe(a)
    ensures ActionOk(s, MapPolicyToMaint(a))
  {
  }

  lemma ChosenPolicyPreservesInv(s: AuxState, a: SubstrateMaintenance.MaintenanceAction)
    requires StateInv(s) && PolicyActionIsSafe(a)
    ensures var m := MapPolicyToMaint(a);
            ActionOk(s, m) &&
            (var s2 := ApplyMaint(s, m);
             Inv(s2.store, s2.table))
  {
    MapPolicyActionOk(s, a);
    MaintenancePreservesInv(s, MapPolicyToMaint(a));
  }

  lemma ForcedOrEarlyThenMaintPreservesInv(
      s: AuxState,
      forced: SubstrateMaintenance.Option<SubstrateMaintenance.MaintenanceAction>,
      request: SubstrateMaintenance.MaintenanceRequest)
    requires StateInv(s)
    requires forced.None? || PolicyActionIsSafe(forced.value)
    ensures var early := SubstrateMaintenance.SelectAuxMaintenanceActionEarly(forced, request);
            early.None? ||
            (PolicyActionIsSafe(early.value) &&
             (var m := MapPolicyToMaint(early.value);
              ActionOk(s, m) && Inv(ApplyMaint(s, m).store, ApplyMaint(s, m).table)))
  {
    EarlySelectIsSafe(forced, request);
    var early := SubstrateMaintenance.SelectAuxMaintenanceActionEarly(forced, request);
    if early.Some? {
      ChosenPolicyPreservesInv(s, early.value);
    }
  }

  lemma EmptyApplyDeltaIsNoOp(s: AuxState)
    requires StateInv(s)
    ensures ApplyMaint(s, ApplyInsertSeq([])) == s
    ensures StateInv(ApplyMaint(s, ApplyInsertSeq([])))
  {
    assert ApplyDeltaSeq(s.store, [], 1) == s.store;
    assert s.table + [] == s.table;
    MaintInsertSeq(s, []);
  }
}
