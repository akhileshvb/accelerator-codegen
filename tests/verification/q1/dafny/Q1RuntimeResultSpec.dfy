// Q1RuntimeResultSpec — Hit → Inv → ExecSeq ≈ RelNode Eval_Spec OutRows_rel.
//
// Retargets existing PairInv / SnapshotHitSafe / LocalWriteSafe substrate
// (from Q1RuntimeAuxCorrect) to the RelNode ExecCorrectOrdered endpoint.
// Does NOT redesign transaction semantics.
//
// Gates: SUBSTRATE_PROVED (SnapshotInv) + RUNTIME_EXEC_PROVED (Q1RuntimeCorrect).

include "Q1Aux.dfy"
include "Q1Finalize.dfy"
include "Q1ResultSpecBridge.dfy"
include "Q1RuntimeAuxCorrect.dfy"
include "../../substrate/dafny/SnapshotPinning.dfy"
include "../../substrate/dafny/ReadHandle.dfy"

module Q1RuntimeResultSpec {

  import opened Q1Aux
  import F = Q1Finalize
  import Bridge = Q1ResultSpecBridge
  import Spec = Q1ResultSpec
  import RelSpec = Q1RelNodeSpec
  import RT = Q1RuntimeAuxCorrect
  import SP = SnapshotPinning
  import RH = ReadHandle

  // ----- SnapshotInv: Hit(A,S) ⇒ Inv(A, DB(S)) -----

  lemma SnapshotInv(
    fold_pub: Store,
    db_t: seq<Row>,
    db_S: seq<Row>,
    pending: bool,
    local_writes: seq<Row>,
    S: SP.PgSnapshot,
    t: SP.VisibilityToken,
    read_hit_complete: bool
  )
    requires RowsOk(db_t) && RowsOk(db_S)
    requires RT.AcceleratorHit_Q1(pending, S, t, true, read_hit_complete)
    requires RT.PairInv_Q1(fold_pub, db_t)
    requires RT.SnapshotHitSafe_Q1(true, S, t, db_t, db_S)
    ensures Inv(fold_pub, db_S)
    ensures RT.VisibleDB(db_S, pending, local_writes) == db_S
  {
    RT.AcceleratorHit_implies_LocalWriteSafe(pending, S, t, true, read_hit_complete);
    RT.PairInv_SnapshotHitSafe_establishes_Inv_VisibleDB(
      fold_pub, db_t, db_S, pending, local_writes, S, t);
    // PairInv + SnapshotHitSafe ⇒ fold_pub == FoldTwin(db_S) == Inv
    assert fold_pub == FoldTwin(db_S);
  }

  // ----- Hit ⇒ sequence-level ≈_Q1 RelNode Eval_Spec_Q1_OutRows_rel(DB(S)) -----

  lemma Hit_implies_ExecApproxResultSpec(
    fold_pub: Store,
    db_t: seq<Row>,
    db_S: seq<Row>,
    pending: bool,
    local_writes: seq<Row>,
    S: SP.PgSnapshot,
    t: SP.VisibilityToken,
    delta: int,
    read_hit_complete: bool
  )
    requires RowsOk(db_t) && RowsOk(db_S)
    requires forall i :: 0 <= i < |db_S| ==> 0 <= db_S[i].rf < 256 && 0 <= db_S[i].ls < 256
    requires RT.AcceleratorHit_Q1(pending, S, t, true, read_hit_complete)
    requires RT.PairInv_Q1(fold_pub, db_t)
    requires RT.SnapshotHitSafe_Q1(true, S, t, db_t, db_S)
    ensures Bridge.ApproxQ1(
              Bridge.ExecSeqM(db_S, Spec.Cutoff(delta)),
              RelSpec.Eval_Spec_Q1_OutRows_rel(
                Spec.Cutoff(delta),
                Bridge.AuxRowsToSpec(db_S)))
  {
    SnapshotInv(fold_pub, db_t, db_S, pending, local_writes, S, t, read_hit_complete);
    Bridge.Q1ExecCorrectOrdered(fold_pub, db_S, delta);
  }

  // Q1RuntimeCorrect (model endpoint): Hit ⇒ ExecSeq ≈_Q1 RelNode OutRows_rel(DB(S)).
  // Cross-language composition with NormalizeSound_Q1 (Rocq) yields
  //   Hit ⇒ Exec ≈ Eval_FS(qhat, DB(S))
  // under SpecExportCorrect [TCB] + Corresponds_Q1 [TCB].
  ghost predicate Q1RuntimeCorrect(
    hit: bool,
    accel: seq<F.OutRow>,
    spec: seq<Spec.SpecOutRow>
  ) {
    hit ==> Bridge.ApproxQ1(accel, spec)
  }

  lemma Q1RuntimeCorrect_from_Hit(
    fold_pub: Store,
    db_t: seq<Row>,
    db_S: seq<Row>,
    pending: bool,
    local_writes: seq<Row>,
    S: SP.PgSnapshot,
    t: SP.VisibilityToken,
    delta: int,
    read_hit_complete: bool
  )
    requires RowsOk(db_t) && RowsOk(db_S)
    requires forall i :: 0 <= i < |db_S| ==> 0 <= db_S[i].rf < 256 && 0 <= db_S[i].ls < 256
    requires RT.AcceleratorHit_Q1(pending, S, t, true, read_hit_complete)
    requires RT.PairInv_Q1(fold_pub, db_t)
    requires RT.SnapshotHitSafe_Q1(true, S, t, db_t, db_S)
    ensures Q1RuntimeCorrect(
              true,
              Bridge.ExecSeqM(db_S, Spec.Cutoff(delta)),
              RelSpec.Eval_Spec_Q1_OutRows_rel(
                Spec.Cutoff(delta),
                Bridge.AuxRowsToSpec(db_S)))
  {
    Hit_implies_ExecApproxResultSpec(
      fold_pub, db_t, db_S, pending, local_writes, S, t, delta, read_hit_complete);
  }
}
