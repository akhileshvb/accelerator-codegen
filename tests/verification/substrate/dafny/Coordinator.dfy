// Structural twin of AuxTransactionCoordinator sequencing
// SOURCE: src/stateful/core/aux_registry.cpp
//   OnMutation → ApplyMutationLocked; PreCommit → Stage; Commit → publish
// Module StageMaintenance / Build are oracles returning staged outcomes.

include "Store.dfy"

module SubstrateCoordinator {
  import opened SubstrateStore

  datatype CoordPhase = Idle | Mutating | Staged | Committed | Aborted

  datatype StagedOutcome = StagedOutcome(
    has_updates: bool,
    update_key: int,
    update_payload: int,
    action_is_apply_delta: bool,
    invalidate_all: bool
  )

  datatype CoordState = CoordState(
    phase: CoordPhase,
    store: Store,
    pending_mutations: nat,
    staged: bool,
    outcome: StagedOutcome
  )

  ghost function InitCoord(): CoordState {
    CoordState(Idle, map[], 0, false, StagedOutcome(false, 0, 0, false, false))
  }

  ghost function OnMutation(c: CoordState): CoordState {
    CoordState(Mutating, c.store, c.pending_mutations + 1, false, c.outcome)
  }

  ghost function PreCommit(c: CoordState, outcome: StagedOutcome): CoordState {
    CoordState(Staged, c.store, c.pending_mutations, true, outcome)
  }

  // Commit ApplyDelta: single-partition publish (structural TryPublish/Publish)
  ghost function CommitApplyDelta(c: CoordState, epoch: nat): CoordState
    requires c.staged
    requires c.outcome.action_is_apply_delta
    requires c.outcome.update_payload != 0
  {
    CoordState(Committed,
      PublishPartition(c.store, c.outcome.update_key, c.outcome.update_payload, epoch),
      0, false, c.outcome)
  }

  ghost function CommitInvalidate(c: CoordState, key: int): CoordState
    requires c.staged
  {
    CoordState(Committed, InvalidatePartition(c.store, key), 0, false, c.outcome)
  }

  ghost function Abort(c: CoordState): CoordState {
    CoordState(Aborted, c.store, 0, false, StagedOutcome(false, 0, 0, false, false))
  }

  lemma AbortClearsPending(c: CoordState)
    ensures Abort(c).pending_mutations == 0
    ensures !Abort(c).staged
  {
  }

  // Plan milestone lemma: StageMaintenance ApplyDelta updates, once TryPublish/
  // Publish succeeds, are what GetReadablePayload returns.
  lemma StagedApplyDeltaThenCommit(store0: Store, key: int, payload: int, epoch: nat)
    requires payload != 0
    ensures var outcome := StagedOutcome(true, key, payload, true, false);
            var c0 := CoordState(Mutating, store0, 1, false, outcome);
            var c1 := PreCommit(c0, outcome);
            var c2 := CommitApplyDelta(c1, epoch);
            GetStatus(c2.store, key) == Valid &&
            GetReadablePayload(c2.store, key) == payload
  {
    var outcome := StagedOutcome(true, key, payload, true, false);
    var c0 := CoordState(Mutating, store0, 1, false, outcome);
    var c1 := PreCommit(c0, outcome);
    PublishThenReadable(c1.store, key, payload, epoch);
  }

  lemma OnMutationIncrementsPending(c: CoordState)
    ensures OnMutation(c).pending_mutations == c.pending_mutations + 1
  {
  }
}
