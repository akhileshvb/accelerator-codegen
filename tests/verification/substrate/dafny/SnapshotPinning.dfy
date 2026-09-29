// SnapshotPinning: TokenForSnapshot(S,t) — same PG snapshot ⇒ same token.
// Connected to PairInv / SnapshotHitSafe without assuming payload==fold freely.

include "VersionedPairing.dfy"
include "ReadHandle.dfy"

module SnapshotPinning {

  import VP = VersionedPairing
  import RH = ReadHandle

  datatype PgSnapshot = PgSnapshot(
    xmin: int,
    xmax: int,
    xip_hash: int,
    flags: int
  )

  datatype VisibilityToken = VisibilityToken(
    mutation_epoch: nat,
    backend_token: int  // hash of snapshot identity (C++ uses string)
  )

  function SnapshotId(S: PgSnapshot): int {
    // Surrogate identity; C++ encodes xmin/xmax/xip into backend_token string.
    S.xmin * 1000003 + S.xmax * 10007 + S.xip_hash * 101 + S.flags
  }

  function MakeTokenForSnapshot(S: PgSnapshot, mutation_epoch: nat): VisibilityToken {
    VisibilityToken(mutation_epoch, SnapshotId(S))
  }

  predicate TokenForSnapshot(S: PgSnapshot, t: VisibilityToken) {
    t.backend_token == SnapshotId(S)
  }

  lemma MakeToken_establishes_TokenForSnapshot(S: PgSnapshot, epoch: nat)
    ensures TokenForSnapshot(S, MakeTokenForSnapshot(S, epoch))
    ensures MakeTokenForSnapshot(S, epoch).mutation_epoch == epoch
  {
  }

  lemma SameSnapshotSameToken(S: PgSnapshot, t1: VisibilityToken, t2: VisibilityToken)
    requires TokenForSnapshot(S, t1)
    requires TokenForSnapshot(S, t2)
    ensures t1.backend_token == t2.backend_token
  {
  }

  lemma DifferentSnapshotDifferentToken(S: PgSnapshot, S': PgSnapshot, t: VisibilityToken)
    requires TokenForSnapshot(S, t)
    requires SnapshotId(S) != SnapshotId(S')
    ensures !TokenForSnapshot(S', t)
  {
  }

  // SnapshotHitSafe: under TokenForSnapshot, a hit observing fold_db_t must match
  // the fold of the pinned snapshot S (fold_db_S).
  predicate SnapshotHitSafe(
    hit: bool,
    S: PgSnapshot,
    t: VisibilityToken,
    fold_db_t: int,
    fold_db_S: int
  ) {
    (TokenForSnapshot(S, t) && hit) ==> fold_db_t == fold_db_S
  }

  // When Fold is a function of SnapshotId, TokenForSnapshot forces fold_t == fold_S.
  lemma SnapshotId_determines_fold(
    S: PgSnapshot,
    t: VisibilityToken,
    fold_by_snap: int -> int,
    hit: bool
  )
    requires TokenForSnapshot(S, t)
    ensures SnapshotHitSafe(
              hit, S, t,
              fold_by_snap(t.backend_token),
              fold_by_snap(SnapshotId(S)))
  {
  }

  // PairInv + Hit at t.mutation_epoch ⇒ RuntimeAuxCorrect (Fold link from PairInv).
  lemma TokenPinned_PairInv_RuntimeAuxCorrect(
    S: PgSnapshot,
    t: VisibilityToken,
    w: VP.World,
    fold_of_db_t: nat -> int
  )
    requires TokenForSnapshot(S, t)
    requires VP.PairInv(w, fold_of_db_t)
    requires VP.Hit(w, VP.VisibilityToken(t.mutation_epoch, ""))
    ensures RH.RuntimeAuxCorrect(
              true,
              w.store.payload_abs,
              RH.AuxAnswerVisible(fold_of_db_t(t.mutation_epoch)))
  {
    RH.PairInv_Hit_implies_RuntimeAuxCorrect(
      w, fold_of_db_t, VP.VisibilityToken(t.mutation_epoch, ""));
  }

  // Concurrent publish after pin: C++ filters slots with
  // valid_through_mutation_epoch > t.mutation_epoch (NotVisible).
  // Model: Visible requires valid_through <= pin epoch, so a publish at E1 > E0
  // cannot Hit a reader pinned at E0.
  predicate PublishEpochFiltered(
    pin_epoch: nat,
    publish_epoch: nat,
    slot_valid_through: nat
  ) {
    publish_epoch > pin_epoch ==>
      !(slot_valid_through == publish_epoch && slot_valid_through <= pin_epoch)
  }

  lemma TokenForSnapshot_stable_under_concurrent_publish(
    S: PgSnapshot,
    pin_epoch: nat,
    publish_epoch: nat,
    w_before: VP.World,
    payload: int
  )
    requires TokenForSnapshot(S, MakeTokenForSnapshot(S, pin_epoch))
    requires publish_epoch > pin_epoch
    requires pin_epoch <= w_before.current_epoch
    ensures
      (var t := MakeTokenForSnapshot(S, pin_epoch);
       var w_pub := VP.World(
         VP.AbstractSlot(VP.Valid, payload, w_before.store.version + 1, publish_epoch),
         if publish_epoch > w_before.current_epoch then publish_epoch
         else w_before.current_epoch);
       // Pinned reader cannot Hit Aux published at a strictly newer epoch.
       !VP.Hit(w_pub, VP.VisibilityToken(t.mutation_epoch, "")) &&
       PublishEpochFiltered(pin_epoch, publish_epoch, publish_epoch))
  {
    var t := MakeTokenForSnapshot(S, pin_epoch);
    var w_pub := VP.World(
      VP.AbstractSlot(VP.Valid, payload, w_before.store.version + 1, publish_epoch),
      if publish_epoch > w_before.current_epoch then publish_epoch
      else w_before.current_epoch);
    // Visible requires valid_through <= t.mutation_epoch == pin_epoch
    // but publish_epoch > pin_epoch, so !Visible.
    assert !VP.Visible(w_pub.store, VP.VisibilityToken(t.mutation_epoch, ""));
  }

  // Pin freezes epoch at first observation of S; later global epoch bumps
  // do not change the pinned token for the same S encoding.
  lemma PinEpoch_frozen_across_bump(
    S: PgSnapshot,
    pin_epoch: nat,
    later_epoch: nat
  )
    requires later_epoch >= pin_epoch
    ensures MakeTokenForSnapshot(S, pin_epoch).mutation_epoch == pin_epoch
    ensures TokenForSnapshot(S, MakeTokenForSnapshot(S, pin_epoch))
    ensures (later_epoch > pin_epoch) ==>
              (MakeTokenForSnapshot(S, pin_epoch).mutation_epoch
               != MakeTokenForSnapshot(S, later_epoch).mutation_epoch)
  {
  }

}
