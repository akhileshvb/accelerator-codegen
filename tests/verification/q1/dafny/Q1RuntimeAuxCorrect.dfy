// Q1 RuntimeAuxCorrect: AcceleratorHit ⇒ Execute(h) = AuxAnswerTwin(VisibleDB(S,T), θ)
//
// Derives the guarantee from PairInv + SnapshotHitSafe + ReadHit + LocalWriteSafe.
// Does NOT assume payload == Fold(VisibleDB) as a free premise: PairInv gives
// fold_pub == FoldTwin(DB_t); SnapshotHitSafe lifts that to FoldTwin(DB(S));
// LocalWriteSafe collapses VisibleDB(S,T) to DB(S) on a hit.
//
// Observation is AuxAnswerTwin = TwinFinalize ∘ Compose (Q1TwinSpecBridge),
// connected to FoldTwin via Inv(fold, db_witness).

include "Q1Aux.dfy"
include "Q1TwinSpecBridge.dfy"
include "../../substrate/dafny/ReadHandle.dfy"
include "../../substrate/dafny/SnapshotPinning.dfy"

module Q1RuntimeAuxCorrect {

  import opened Q1Aux
  import Bridge = Q1TwinSpecBridge
  import opened Q1Finalize
  import RH = ReadHandle
  import SP = SnapshotPinning

  // ----- VisibleDB(S, T): snapshot S plus T's own uncommitted writes -----

  ghost function VisibleDB(
    db_S: seq<Row>,
    pending_relevant: bool,
    local_writes: seq<Row>
  ): seq<Row>
  {
    if pending_relevant then db_S + local_writes else db_S
  }

  lemma VisibleDB_collapses_when_clean(
    db_S: seq<Row>,
    local_writes: seq<Row>
  )
    ensures VisibleDB(db_S, false, local_writes) == db_S
  {
  }

  // ----- Hit path model -----

  // AcceleratorHit_Q1: LocalWriteSafe clean + TokenForSnapshot + ReadHit complete.
  predicate AcceleratorHit_Q1(
    pending_relevant: bool,
    S: SP.PgSnapshot,
    t: SP.VisibilityToken,
    store_hit: bool,
    read_hit_complete: bool
  ) {
    !pending_relevant &&
    SP.TokenForSnapshot(S, t) &&
    store_hit &&
    read_hit_complete
  }

  lemma AcceleratorHit_implies_LocalWriteSafe(
    pending: bool,
    S: SP.PgSnapshot,
    t: SP.VisibilityToken,
    store_hit: bool,
    read_ok: bool
  )
    requires AcceleratorHit_Q1(pending, S, t, store_hit, read_ok)
    ensures RH.LocalWriteSafe(pending, true)
  {
  }

  // PairInv fragment for Q1: published fold equals FoldTwin of the publish-cut DB.
  ghost predicate PairInv_Q1(fold_pub: Store, db_t: seq<Row>)
    requires RowsOk(db_t)
  {
    fold_pub == FoldTwin(db_t)
  }

  // SnapshotHitSafe for Q1 folds under a pinned snapshot.
  ghost predicate SnapshotHitSafe_Q1(
    hit: bool,
    S: SP.PgSnapshot,
    t: SP.VisibilityToken,
    db_t: seq<Row>,
    db_S: seq<Row>
  )
    requires RowsOk(db_t) && RowsOk(db_S)
  {
    (SP.TokenForSnapshot(S, t) && hit) ==> FoldTwin(db_t) == FoldTwin(db_S)
  }

  // ReadHit: immutable handle RepAbs's the published FoldTwin cut.
  predicate ReadHit_Q1(fold_handle: Store, fold_pub: Store) {
    fold_handle == fold_pub
  }

  // Execute observation from a ReadHit handle. Witness db must satisfy Inv
  // (fold == FoldTwin(db)); PairInv + SnapshotHitSafe establish that for VisibleDB.
  ghost function Execute_Q1(
    fold_handle: Store,
    db_witness: seq<Row>,
    theta: int
  ): seq<OutRow>
    requires RowsOk(db_witness)
    requires fold_handle == FoldTwin(db_witness)
  {
    Bridge.AuxAnswerTwin(db_witness, theta)
  }

  // RuntimeAuxCorrect_Q1 predicate (PIPELINE Guarantee B).
  predicate RuntimeAuxCorrect_Q1(
    hit: bool,
    execute_obs: seq<OutRow>,
    aux_visible: seq<OutRow>
  ) {
    hit ==> execute_obs == aux_visible
  }

  // ----- Main lemma: derive Execute = AuxAnswerTwin(VisibleDB, θ) -----

  lemma PairInv_SnapshotHitSafe_establishes_Inv_VisibleDB(
    fold_pub: Store,
    db_t: seq<Row>,
    db_S: seq<Row>,
    pending: bool,
    local_writes: seq<Row>,
    S: SP.PgSnapshot,
    t: SP.VisibilityToken
  )
    requires RowsOk(db_t) && RowsOk(db_S)
    requires PairInv_Q1(fold_pub, db_t)
    requires SnapshotHitSafe_Q1(true, S, t, db_t, db_S)
    requires SP.TokenForSnapshot(S, t)
    requires RH.LocalWriteSafe(pending, true)
    ensures !pending
    ensures VisibleDB(db_S, pending, local_writes) == db_S
    ensures fold_pub == FoldTwin(VisibleDB(db_S, pending, local_writes))
  {
    // LocalWriteSafe(pending, true) ⇒ pending ==> false ⇒ !pending
    assert !pending;
    VisibleDB_collapses_when_clean(db_S, local_writes);
    assert FoldTwin(db_t) == FoldTwin(db_S);
    assert fold_pub == FoldTwin(db_t);
  }

  lemma RuntimeAuxCorrect_Q1_from_PairInv(
    fold_pub: Store,
    db_t: seq<Row>,
    db_S: seq<Row>,
    pending: bool,
    local_writes: seq<Row>,
    S: SP.PgSnapshot,
    t: SP.VisibilityToken,
    theta: int,
    read_hit_complete: bool
  )
    requires RowsOk(db_t) && RowsOk(db_S)
    requires AcceleratorHit_Q1(pending, S, t, true, read_hit_complete)
    requires PairInv_Q1(fold_pub, db_t)
    requires SnapshotHitSafe_Q1(true, S, t, db_t, db_S)
    requires ReadHit_Q1(fold_pub, fold_pub)
    ensures
      (var vis := VisibleDB(db_S, pending, local_writes);
       vis == db_S &&
       fold_pub == FoldTwin(vis) &&
       RuntimeAuxCorrect_Q1(
         true,
         Execute_Q1(fold_pub, vis, theta),
         Bridge.AuxAnswerTwin(vis, theta)))
  {
    AcceleratorHit_implies_LocalWriteSafe(pending, S, t, true, read_hit_complete);
    PairInv_SnapshotHitSafe_establishes_Inv_VisibleDB(
      fold_pub, db_t, db_S, pending, local_writes, S, t);
    var vis := VisibleDB(db_S, pending, local_writes);
    assert Execute_Q1(fold_pub, vis, theta) == Bridge.AuxAnswerTwin(vis, theta);
  }

  // Miss path is trivial (same as substrate ReadHandle).
  lemma RuntimeAuxCorrect_Q1_miss(
    execute_obs: seq<OutRow>,
    aux_visible: seq<OutRow>
  )
    ensures RuntimeAuxCorrect_Q1(false, execute_obs, aux_visible)
  {
  }

  // Compose-level form (View before TwinFinalize); equality lifts through Finalize.
  lemma RuntimeAuxCorrect_Q1_Compose_form(
    fold_pub: Store,
    db_t: seq<Row>,
    db_S: seq<Row>,
    pending: bool,
    local_writes: seq<Row>,
    S: SP.PgSnapshot,
    t: SP.VisibilityToken,
    theta: int,
    read_hit_complete: bool
  )
    requires RowsOk(db_t) && RowsOk(db_S)
    requires AcceleratorHit_Q1(pending, S, t, true, read_hit_complete)
    requires PairInv_Q1(fold_pub, db_t)
    requires SnapshotHitSafe_Q1(true, S, t, db_t, db_S)
    ensures
      (var vis := VisibleDB(db_S, pending, local_writes);
       Compose(vis, theta) == Compose(db_S, theta) &&
       Bridge.AuxAnswerTwin(vis, theta) == Bridge.AuxAnswerTwin(db_S, theta))
  {
    AcceleratorHit_implies_LocalWriteSafe(pending, S, t, true, read_hit_complete);
    PairInv_SnapshotHitSafe_establishes_Inv_VisibleDB(
      fold_pub, db_t, db_S, pending, local_writes, S, t);
  }

}
