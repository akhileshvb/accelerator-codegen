// Structural twin of Postgres generated backend adapters
// SOURCE:
//   src/stateful/postgres/postgres_aux_backend.cpp
//   xact callback order: PreCommit → Commit | Abort
//   mutation capture attnum projection
// PG registration / SPI / xact manager = oracles.

include "../../substrate/dafny/Coordinator.dfy"

module PostgresBackend {
  import opened SubstrateCoordinator
  import opened SubstrateStore

  ghost function CaptureMutation(c: CoordState): CoordState {
    OnMutation(c)
  }

  ghost function OnPreCommit(c: CoordState, outcome: StagedOutcome): CoordState {
    PreCommit(c, outcome)
  }

  ghost function OnCommitApply(c: CoordState, epoch: nat): CoordState
    requires c.staged && c.outcome.action_is_apply_delta && c.outcome.update_payload != 0
  {
    CommitApplyDelta(c, epoch)
  }

  ghost function OnAbort(c: CoordState): CoordState {
    Abort(c)
  }

  datatype XactEvent = EvPreCommit | EvCommit | EvAbort

  ghost function XactStep(c: CoordState, ev: XactEvent, outcome: StagedOutcome, epoch: nat): CoordState
    requires ev != EvCommit || (c.staged && c.outcome.action_is_apply_delta && c.outcome.update_payload != 0)
  {
    match ev
      case EvPreCommit => OnPreCommit(c, outcome)
      case EvCommit => OnCommitApply(c, epoch)
      case EvAbort => OnAbort(c)
  }

  lemma CaptureThenAbortClears(c: CoordState)
    ensures OnAbort(CaptureMutation(c)).pending_mutations == 0
  {
  }

  lemma PreCommitCommitPublish(store0: Store, key: int, payload: int, epoch: nat)
    requires payload != 0
    ensures var outcome := StagedOutcome(true, key, payload, true, false);
            var c0 := CaptureMutation(CoordState(Idle, store0, 0, false, outcome));
            var c1 := OnPreCommit(c0, outcome);
            var c2 := OnCommitApply(c1, epoch);
            GetReadablePayload(c2.store, key) == payload
  {
    var outcome := StagedOutcome(true, key, payload, true, false);
    var c0 := CaptureMutation(CoordState(Idle, store0, 0, false, outcome));
    var c1 := OnPreCommit(c0, outcome);
    assert c1.staged;
    assert c1.outcome.action_is_apply_delta;
    PublishThenReadable(c1.store, key, payload, epoch);
  }

  // Mutation capture: select attnums from a full row (column-index fidelity).
  ghost function ProjectAttnums(attnums: seq<int>, full_row: seq<int>): seq<int>
    requires forall i :: 0 <= i < |attnums| ==> 0 <= attnums[i] < |full_row|
    decreases |attnums|
  {
    if |attnums| == 0 then []
    else [full_row[attnums[0]]] + ProjectAttnums(attnums[1..], full_row)
  }

  lemma ProjectSingleton(row: seq<int>, i: nat)
    requires i < |row|
    ensures ProjectAttnums([i as int], row) == [row[i]]
  {
  }

  // AllowsSynchronousRebuildOnCommit == false (postgres_aux_backend.cpp:~48)
  ghost function AllowsSynchronousRebuildOnCommit(): bool { false }

  lemma PostgresNoSyncRebuild()
    ensures AllowsSynchronousRebuildOnCommit() == false
  {
  }
}
