// ReadHandle: ReadHit linearizes check + immutable handle acquisition.
// RuntimeAuxCorrect is proved over this Dafny model only (not generated C++).
// PairInv-style: Hit ⇒ RepAbs(payload, Fold(DB_t)) establishes the Fold link;
// RuntimeAuxCorrect is derived from that, not assumed as a free premise.

include "VersionedPairing.dfy"

module ReadHandle {

  import VP = VersionedPairing

  datatype Handle = Handle(payload_abs: int, generation: nat)

  datatype StoreSlot = StoreSlot(
    valid: bool,
    payload_abs: int,
    generation: nat
  )

  // Single linearization point: validate and return immutable handle, or none.
  datatype ReadResult = Miss | Hit(h: Handle)

  function ReadHit(slot: StoreSlot, t_epoch: nat): ReadResult {
    if slot.valid then Hit(Handle(slot.payload_abs, slot.generation))
    else Miss
  }

  // Execute observation from an immutable handle (abstract payload).
  function Execute(h: Handle): int {
    h.payload_abs
  }

  // AuxAnswer of the visible DB cut for snapshot token t (abstract).
  ghost function AuxAnswerVisible(fold_db_t: int): int {
    fold_db_t
  }

  // ReadLinearizes: a Hit handle is exactly the state validated for t.
  lemma ReadLinearizes(slot: StoreSlot, t_epoch: nat)
    ensures match ReadHit(slot, t_epoch)
      case Miss => true
      case Hit(h) => h.payload_abs == slot.payload_abs && h.generation == slot.generation
  {
  }

  // ConsistentObservation: all required partitions' handles represent Fold_p(DB_t).
  predicate ConsistentObservation(
    handles: seq<Handle>,
    fold_parts: seq<int>
  )
    requires |handles| == |fold_parts|
  {
    forall i :: 0 <= i < |handles| ==> handles[i].payload_abs == fold_parts[i]
  }

  lemma ConsistentObservationHolds(
    handles: seq<Handle>,
    fold_parts: seq<int>
  )
    requires |handles| == |fold_parts|
    requires forall i :: 0 <= i < |handles| ==> handles[i].payload_abs == fold_parts[i]
    ensures ConsistentObservation(handles, fold_parts)
  {
  }

  // LocalWriteSafe model: pending relevant writes ⇒ no accelerator hit.
  predicate LocalWriteSafe(pending_relevant: bool, accelerator_hit: bool) {
    pending_relevant ==> !accelerator_hit
  }

  lemma LocalWriteSafeHolds(pending: bool)
    ensures LocalWriteSafe(pending, !pending || false)
  {
  }

  lemma LocalWriteSafe_dirty_forces_miss(pending: bool, hit: bool)
    requires pending
    requires LocalWriteSafe(pending, hit)
    ensures !hit
  {
  }

  // AffectsNoOp: ¬Affects ⇒ Fold unchanged.
  predicate AffectsNoOp(affects: bool, fold_before: int, fold_after: int) {
    !affects ==> fold_before == fold_after
  }

  lemma AffectsNoOpHolds(affects: bool, fold: int)
    ensures AffectsNoOp(affects, fold, fold)
  {
  }

  // RuntimeAuxCorrect: AcceleratorHit ⇒ Execute(h) = AuxAnswer(VisibleDB).
  predicate RuntimeAuxCorrect(
    hit: bool,
    execute_h: int,
    aux_answer_visible: int
  ) {
    hit ==> execute_h == aux_answer_visible
  }

  // PairInv-style safety: Valid slot payload is Fold(DB_t).
  predicate SnapshotHitSafe(slot: StoreSlot, fold_db_t: int) {
    slot.valid ==> slot.payload_abs == fold_db_t
  }

  // Derive RuntimeAuxCorrect from SnapshotHitSafe (PairInv fragment).
  lemma SnapshotHitSafe_implies_RuntimeAuxCorrect(
    slot: StoreSlot,
    t_epoch: nat,
    fold_db_t: int
  )
    requires SnapshotHitSafe(slot, fold_db_t)
    ensures
      match ReadHit(slot, t_epoch)
        case Miss => RuntimeAuxCorrect(false, 0, AuxAnswerVisible(fold_db_t))
        case Hit(h) => RuntimeAuxCorrect(true, Execute(h), AuxAnswerVisible(fold_db_t))
  {
  }

  // PairInv on a world ⇒ SnapshotHitSafe for a Hit token's fold cut.
  lemma PairInv_implies_SnapshotHitSafe(
    w: VP.World,
    fold_of_db_t: nat -> int,
    t: VP.VisibilityToken
  )
    requires VP.PairInv(w, fold_of_db_t)
    requires VP.Hit(w, t)
    ensures SnapshotHitSafe(
              StoreSlot(true, w.store.payload_abs, w.store.version),
              fold_of_db_t(t.mutation_epoch))
  {
    VP.PairInv_Hit_RepAbs(w, fold_of_db_t, t);
  }

  lemma PairInv_Hit_implies_RuntimeAuxCorrect(
    w: VP.World,
    fold_of_db_t: nat -> int,
    t: VP.VisibilityToken
  )
    requires VP.PairInv(w, fold_of_db_t)
    requires VP.Hit(w, t)
    ensures RuntimeAuxCorrect(
              true,
              w.store.payload_abs,
              AuxAnswerVisible(fold_of_db_t(t.mutation_epoch)))
  {
    VP.PairInv_Hit_RepAbs(w, fold_of_db_t, t);
  }

  lemma RuntimeAuxCorrect_miss_trivial(execute_h: int, aux: int)
    ensures RuntimeAuxCorrect(false, execute_h, aux)
  {
  }

}
