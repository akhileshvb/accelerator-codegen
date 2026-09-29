// VersionedPairing: PairInv — Hit(W,t) ⇒ RepAbs(W.store, Fold(DB_t))
// Substrate model for JetStream token-world published state.

module VersionedPairing {

  datatype SlotStatus = Valid | Invalid

  datatype AbstractSlot = AbstractSlot(
    status: SlotStatus,
    payload_abs: int,   // abstract Fold fragment id / hash surrogate
    version: nat,
    valid_through: nat
  )

  datatype VisibilityToken = VisibilityToken(
    mutation_epoch: nat,
    backend_token: string
  )

  datatype World = World(
    store: AbstractSlot,
    current_epoch: nat
  )

  predicate Admissible(w: World, t: VisibilityToken) {
    t.mutation_epoch <= w.current_epoch
  }

  predicate Visible(slot: AbstractSlot, t: VisibilityToken) {
    slot.status == Valid && slot.valid_through <= t.mutation_epoch
  }

  predicate Hit(w: World, t: VisibilityToken) {
    Admissible(w, t) && Visible(w.store, t)
  }

  // RepAbs: slot is Valid and its abstract interpretation equals a.
  predicate RepAbs(slot: AbstractSlot, a: int) {
    slot.status == Valid && slot.payload_abs == a
  }

  // PairInv: every successful hit observes Fold(DB_t) via RepAbs.
  ghost predicate PairInv(w: World, fold_of_db_t: nat -> int) {
    forall t: VisibilityToken ::
      Hit(w, t) ==> RepAbs(w.store, fold_of_db_t(t.mutation_epoch))
  }

  lemma InvalidateBreaksHit(w: World, t: VisibilityToken)
    requires Hit(w, t)
    ensures var w' := w.(store := w.store.(status := Invalid, version := w.store.version + 1));
            !Hit(w', t)
  {
  }

  // Establishing PairInv when the published payload equals Fold at valid_through
  // and Fold is stable for all later epochs that can still Hit this slot.
  lemma Publish_establishes_PairInv(
    payload: int,
    epoch: nat,
    current: nat,
    fold_of_db_t: nat -> int
  )
    requires epoch <= current
    requires forall e: nat :: epoch <= e <= current ==> fold_of_db_t(e) == payload
    ensures var w := World(AbstractSlot(Valid, payload, 1, epoch), current);
            PairInv(w, fold_of_db_t)
  {
    var w := World(AbstractSlot(Valid, payload, 1, epoch), current);
    forall t: VisibilityToken
      ensures Hit(w, t) ==> RepAbs(w.store, fold_of_db_t(t.mutation_epoch))
    {
      if Hit(w, t) {
        assert epoch <= t.mutation_epoch <= current;
        assert fold_of_db_t(t.mutation_epoch) == payload;
      }
    }
  }

  lemma PairInv_Hit_RepAbs(w: World, fold_of_db_t: nat -> int, t: VisibilityToken)
    requires PairInv(w, fold_of_db_t)
    requires Hit(w, t)
    ensures RepAbs(w.store, fold_of_db_t(t.mutation_epoch))
    ensures w.store.payload_abs == fold_of_db_t(t.mutation_epoch)
  {
  }

}
