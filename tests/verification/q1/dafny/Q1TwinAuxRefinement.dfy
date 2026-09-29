// Q1 Twin ↔ Aux refinement boundary (Phase 0b).
//
// TwinAuxRel is representation-only:
//   TwinAuxRel(twin_store, aux_store)  ≡  Flatten(twin_store) == aux_store
// It does NOT bake in Inv or DB correctness.
//
// Inv stays separate (Q1Aux):
//   Inv(aux_store, D)  ≡  aux_store == FoldTwin(D)
//
// Derivation: FoldTwinFromTwin := Flatten ∘ Twin.ApplyDeltaSeq(Empty, RowsToDeltas).
// Q1Aux keeps local Stats/Apply (isomorphic to Q1Finalize) so Aux-only proofs need
// not import ModuleTwin — justified by Apply_eq_Finalize here. FoldTwinFromTwin_eq
// links Aux.FoldTwin to Twin fold (Qed, including induction).
//
// Closed here: ApplyDeltaSeq, StageApplyDelta, RebuildOracleCorrect,
// BuildStateForDays(lineitem)⇒RebuildOracleCorrect, hit Execute=ComposeOutput,
// Compose_GetKey=FilteredContrib. Stronger all-path / finalize / BuiltInv / closed
// package theorems: Q1TwinAuxClose.dfy. Canonical status:
//   tests/verification/q1/CURRENT_THEOREM_TCB.md
// Intentional SPI: AggsFaithful; RebuildOracleCorrect on free oracle/load.
// No twin_aux_refinement catch-all TCB.
include "Q1ModuleTwin.dfy"
include "Q1Aux.dfy"

module Q1TwinAuxRefinement {

  import Twin = Q1ModuleTwin
  import F = Q1Finalize
  import opened Q1Aux

  ghost function StatsToAux(s: F.Stats): Stats {
    Stats(s.count, s.sum_qty, s.sum_base_price, s.sum_disc_price,
          s.sum_charge, s.sum_discount)
  }

  ghost function StatsFromAux(s: Stats): F.Stats {
    F.Stats(s.count, s.sum_qty, s.sum_base_price, s.sum_disc_price,
            s.sum_charge, s.sum_discount)
  }

  lemma StatsRoundTrip(s: F.Stats)
    ensures StatsFromAux(StatsToAux(s)) == s
  {
  }

  lemma Apply_eq_Finalize(s: F.Stats, qty: int, price: int, disc: int, tax: int, sign: int)
    ensures StatsToAux(F.ApplyQ1RowContribution(s, qty, price, disc, tax, sign))
         == Apply(StatsToAux(s), qty, price, disc, tax, sign)
  {
  }

  // ----- Flatten (representation only; ignores DayPart.keys / DayStore.order) -----

  ghost function CellKeysOf(state: Twin.DayStore): set<CellKey> {
    set day, gkey |
      day in state.parts && gkey in state.parts[day].groups
      :: CellKey(day, gkey)
  }

  ghost function Flatten(state: Twin.DayStore): Store {
    map ck | ck in CellKeysOf(state) :: StatsToAux(state.parts[ck.day].groups[ck.gkey])
  }

  ghost predicate TwinAuxRel(twin_store: Twin.DayStore, aux_store: Store) {
    Flatten(twin_store) == aux_store
  }

  lemma FlattenEmpty()
    ensures Flatten(Twin.EmptyStore()) == map[]
    ensures CellKeysOf(Twin.EmptyStore()) == {}
  {
  }

  lemma Flatten_lookup(state: Twin.DayStore, day: int, gkey: int)
    requires day in state.parts && gkey in state.parts[day].groups
    ensures CellKey(day, gkey) in Flatten(state)
    ensures Flatten(state)[CellKey(day, gkey)]
         == StatsToAux(state.parts[day].groups[gkey])
  {
  }

  lemma Flatten_absent(state: Twin.DayStore, day: int, gkey: int)
    requires !(day in state.parts && gkey in state.parts[day].groups)
    ensures CellKey(day, gkey) !in Flatten(state)
  {
  }

  // ----- Row ↔ Twin delta + Twin-derived fold -----

  ghost function RowToDelta(r: Row, sign: int): Twin.LineitemDelta
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
  {
    Twin.LineitemDelta(r.shipdate, Pack(r.rf, r.ls), r.qty, r.price, r.disc, r.tax, sign)
  }

  ghost function RowsToInsertDeltas(t: seq<Row>): seq<Twin.LineitemDelta>
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    decreases |t|
  {
    if |t| == 0 then []
    else [RowToDelta(t[0], 1)] + RowsToInsertDeltas(t[1..])
  }

  ghost function TwinFoldRows(t: seq<Row>): Twin.DayStore
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  {
    Twin.ApplyDeltaSeq(Twin.EmptyStore(), RowsToInsertDeltas(t))
  }

  ghost function FoldTwinFromTwin(t: seq<Row>): Store
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  {
    Flatten(TwinFoldRows(t))
  }

  lemma RowsToInsertDeltas_snoc(t: seq<Row>, r: Row)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures RowsToInsertDeltas(t + [r]) == RowsToInsertDeltas(t) + [RowToDelta(r, 1)]
    decreases |t|
  {
    if |t| == 0 {
    } else {
      RowsToInsertDeltas_snoc(t[1..], r);
      assert (t + [r])[0] == t[0];
      assert (t + [r])[1..] == t[1..] + [r];
    }
  }

  lemma TwinApplyDeltaSeq_snoc(state: Twin.DayStore, ds: seq<Twin.LineitemDelta>, d: Twin.LineitemDelta)
    ensures Twin.ApplyDeltaSeq(state, ds + [d])
         == Twin.ApplyDeltaToStore(Twin.ApplyDeltaSeq(state, ds), d)
    decreases |ds|
  {
    if |ds| == 0 {
      assert ds + [d] == [d];
    } else {
      assert (ds + [d])[0] == ds[0];
      assert (ds + [d])[1..] == ds[1..] + [d];
      TwinApplyDeltaSeq_snoc(Twin.ApplyDeltaToStore(state, ds[0]), ds[1..], d);
    }
  }

  lemma TwinFoldRows_snoc(t: seq<Row>, r: Row)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures TwinFoldRows(t + [r])
         == Twin.ApplyDeltaToStore(TwinFoldRows(t), RowToDelta(r, 1))
  {
    RowsToInsertDeltas_snoc(t, r);
    TwinApplyDeltaSeq_snoc(Twin.EmptyStore(), RowsToInsertDeltas(t), RowToDelta(r, 1));
  }

  ghost predicate TwinStoreCountsNonNeg(state: Twin.DayStore) {
    forall day, gkey ::
      day in state.parts && gkey in state.parts[day].groups ==>
      state.parts[day].groups[gkey].count >= 0
  }

  lemma Empty_TwinStoreCountsNonNeg()
    ensures TwinStoreCountsNonNeg(Twin.EmptyStore())
  {
  }

  lemma ApplyDeltaToStore_insert_preserves_nonneg(state: Twin.DayStore, r: Row)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires TwinStoreCountsNonNeg(state)
    ensures TwinStoreCountsNonNeg(Twin.ApplyDeltaToStore(state, RowToDelta(r, 1)))
  {
    var d := RowToDelta(r, 1);
    var g := Twin.GetGroup(Twin.GetPart(state, d.shipdate_days), d.group_key);
    assert g.count >= 0;
    var g2 := F.ApplyQ1RowContribution(g, d.quantity_raw, d.extended_price_raw,
                                       d.discount_raw, d.tax_raw, 1);
    assert g2.count == g.count + 1 >= 1;
  }

  lemma TwinFoldRows_counts_nonneg(t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures TwinStoreCountsNonNeg(TwinFoldRows(t))
    decreases |t|
  {
    if |t| == 0 {
      Empty_TwinStoreCountsNonNeg();
    } else {
      var pref := t[..|t| - 1];
      var r := t[|t| - 1];
      assert t == pref + [r];
      TwinFoldRows_counts_nonneg(pref);
      TwinFoldRows_snoc(pref, r);
      ApplyDeltaToStore_insert_preserves_nonneg(TwinFoldRows(pref), r);
    }
  }

  // Twin ApplyDeltaToStore (+1) matches Aux ApplyDelta on Flattened stores.
  lemma {:isolate_assertions} Flatten_ApplyDeltaToStore_insert(state: Twin.DayStore, r: Row)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires TwinStoreCountsNonNeg(state)
    ensures Flatten(Twin.ApplyDeltaToStore(state, RowToDelta(r, 1)))
         == ApplyDelta(Flatten(state), r, 1)
  {
    var d := RowToDelta(r, 1);
    var day := d.shipdate_days;
    var gkey := d.group_key;
    var ck := CellKey(day, gkey);
    var part := Twin.GetPart(state, day);
    var g := Twin.GetGroup(part, gkey);
    assert g.count >= 0;
    var g2 := F.ApplyQ1RowContribution(g, d.quantity_raw, d.extended_price_raw,
                                       d.discount_raw, d.tax_raw, 1);
    Apply_eq_Finalize(g, d.quantity_raw, d.extended_price_raw, d.discount_raw, d.tax_raw, 1);
    assert g2.count == g.count + 1;
    assert g2.count != 0;
    var state1 := Twin.ApplyDeltaToStore(state, d);
    var aux0 := Flatten(state);
    var aux1 := ApplyDelta(aux0, r, 1);
    assert Get(aux0, ck) == (if ck in aux0 then aux0[ck] else EmptyStats());
    if day in state.parts && gkey in state.parts[day].groups {
      Flatten_lookup(state, day, gkey);
      assert Get(aux0, ck) == StatsToAux(g);
    } else {
      Flatten_absent(state, day, gkey);
      assert Get(aux0, ck) == EmptyStats();
      assert StatsToAux(g) == EmptyStats();
    }
    assert ApplyRow(Get(aux0, ck), r, 1) == StatsToAux(g2);
    assert aux1 == aux0[ck := StatsToAux(g2)];
    assert day in state1.parts && gkey in state1.parts[day].groups;
    assert state1.parts[day].groups[gkey] == g2;
    Flatten_lookup(state1, day, gkey);
    // Extensionality: Flatten(state1) and aux1 agree on all keys.
    assert forall ck2 :: ck2 in Flatten(state1) <==> ck2 in aux1;
    assert forall ck2 :: ck2 in aux1 ==> Flatten(state1)[ck2] == aux1[ck2];
  }

  lemma FoldTwinFromTwin_empty()
    ensures FoldTwinFromTwin([]) == FoldTwin([])
  {
    FlattenEmpty();
    FoldEmpty();
  }

  lemma FoldTwinFromTwin_eq_AuxFoldTwin(t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures FoldTwinFromTwin(t) == FoldTwin(t)
    decreases |t|
  {
    if |t| == 0 {
      FoldTwinFromTwin_empty();
    } else {
      var pref := t[..|t| - 1];
      var r := t[|t| - 1];
      assert t == pref + [r];
      FoldTwinFromTwin_eq_AuxFoldTwin(pref);
      TwinFoldRows_counts_nonneg(pref);
      TwinFoldRows_snoc(pref, r);
      FoldStep(pref, r);
      Flatten_ApplyDeltaToStore_insert(TwinFoldRows(pref), r);
    }
  }

  lemma TwinAuxRel_of_derived_fold(t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures TwinAuxRel(TwinFoldRows(t), FoldTwinFromTwin(t))
  {
  }

  // TwinMaintRefinesAux — insert-one slice under TwinAuxRel.
  lemma TwinMaintRefinesAux_one_insert(
    twin_store: Twin.DayStore,
    aux_store: Store,
    r: Row
  )
    requires TwinAuxRel(twin_store, aux_store)
    requires TwinStoreCountsNonNeg(twin_store)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures TwinAuxRel(
              Twin.ApplyDeltaToStore(twin_store, RowToDelta(r, 1)),
              ApplyDelta(aux_store, r, 1))
  {
    Flatten_ApplyDeltaToStore_insert(twin_store, r);
  }

  // ----- Arbitrary ApplyDeltaSeq (any sign) under TwinAuxRel -----

  ghost function ApplyTwinDelta(store: Store, d: Twin.LineitemDelta): Store {
    var ck := CellKey(d.shipdate_days, d.group_key);
    var s2 := Apply(Get(store, ck), d.quantity_raw, d.extended_price_raw,
                    d.discount_raw, d.tax_raw, d.sign);
    if s2.count == 0 then
      store - {ck}
    else
      store[ck := s2]
  }

  ghost function ApplyTwinDeltaSeq(store: Store, ds: seq<Twin.LineitemDelta>): Store
    decreases |ds|
  {
    if |ds| == 0 then
      store
    else
      ApplyTwinDeltaSeq(ApplyTwinDelta(store, ds[0]), ds[1..])
  }

  lemma ApplyTwinDeltaSeq_empty(store: Store)
    ensures ApplyTwinDeltaSeq(store, []) == store
  {
  }

  lemma ApplyTwinDeltaSeq_step(store: Store, d: Twin.LineitemDelta, ds: seq<Twin.LineitemDelta>)
    ensures ApplyTwinDeltaSeq(store, [d] + ds)
         == ApplyTwinDeltaSeq(ApplyTwinDelta(store, d), ds)
  {
  }

  lemma ApplyTwinDelta_eq_ApplyDelta_insert(store: Store, r: Row)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures ApplyTwinDelta(store, RowToDelta(r, 1)) == ApplyDelta(store, r, 1)
  {
  }

  // Twin ApplyDeltaToStore (any sign) matches Aux ApplyTwinDelta on Flattened stores.
  lemma {:isolate_assertions} Flatten_ApplyDeltaToStore(state: Twin.DayStore, d: Twin.LineitemDelta)
    ensures Flatten(Twin.ApplyDeltaToStore(state, d))
         == ApplyTwinDelta(Flatten(state), d)
  {
    var day := d.shipdate_days;
    var gkey := d.group_key;
    var ck := CellKey(day, gkey);
    var part := Twin.GetPart(state, day);
    var g := Twin.GetGroup(part, gkey);
    var g2 := F.ApplyQ1RowContribution(g, d.quantity_raw, d.extended_price_raw,
                                       d.discount_raw, d.tax_raw, d.sign);
    Apply_eq_Finalize(g, d.quantity_raw, d.extended_price_raw, d.discount_raw, d.tax_raw, d.sign);
    var state1 := Twin.ApplyDeltaToStore(state, d);
    var aux0 := Flatten(state);
    var aux1 := ApplyTwinDelta(aux0, d);
    assert Get(aux0, ck) == (if ck in aux0 then aux0[ck] else EmptyStats());
    if day in state.parts && gkey in state.parts[day].groups {
      Flatten_lookup(state, day, gkey);
      assert Get(aux0, ck) == StatsToAux(g);
    } else {
      Flatten_absent(state, day, gkey);
      assert Get(aux0, ck) == EmptyStats();
      assert StatsToAux(g) == EmptyStats();
    }
    assert Apply(Get(aux0, ck), d.quantity_raw, d.extended_price_raw,
                 d.discount_raw, d.tax_raw, d.sign) == StatsToAux(g2);
    if g2.count == 0 {
      assert aux1 == aux0 - {ck};
      assert !(day in state1.parts && gkey in state1.parts[day].groups) ||
             (day in state1.parts && gkey !in state1.parts[day].groups);
      // After erase, CellKey(day,gkey) is absent from Flatten(state1).
      assert CellKey(day, gkey) !in Flatten(state1);
      assert forall ck2 :: ck2 in Flatten(state1) <==> ck2 in aux1;
      assert forall ck2 :: ck2 in aux1 ==> Flatten(state1)[ck2] == aux1[ck2];
    } else {
      assert aux1 == aux0[ck := StatsToAux(g2)];
      assert day in state1.parts && gkey in state1.parts[day].groups;
      assert state1.parts[day].groups[gkey] == g2;
      Flatten_lookup(state1, day, gkey);
      assert forall ck2 :: ck2 in Flatten(state1) <==> ck2 in aux1;
      assert forall ck2 :: ck2 in aux1 ==> Flatten(state1)[ck2] == aux1[ck2];
    }
  }

  lemma Flatten_ApplyDeltaSeq(state: Twin.DayStore, ds: seq<Twin.LineitemDelta>)
    ensures Flatten(Twin.ApplyDeltaSeq(state, ds))
         == ApplyTwinDeltaSeq(Flatten(state), ds)
    decreases |ds|
  {
    if |ds| == 0 {
    } else {
      Flatten_ApplyDeltaToStore(state, ds[0]);
      Flatten_ApplyDeltaSeq(Twin.ApplyDeltaToStore(state, ds[0]), ds[1..]);
    }
  }

  lemma TwinMaintRefinesAux_ApplyDeltaSeq(
    twin_store: Twin.DayStore,
    aux_store: Store,
    ds: seq<Twin.LineitemDelta>
  )
    requires TwinAuxRel(twin_store, aux_store)
    ensures TwinAuxRel(
              Twin.ApplyDeltaSeq(twin_store, ds),
              ApplyTwinDeltaSeq(aux_store, ds))
  {
    Flatten_ApplyDeltaSeq(twin_store, ds);
  }

  // StageMaintenance ApplyDelta branch under TwinAuxRel (no requires_rebuild).
  lemma TwinMaintRefinesAux_StageApplyDelta(
    buf: Twin.TxnBuffer,
    snapshot: Twin.DayStore,
    aux_store: Store,
    rebuilt: Twin.DayStore
  )
    requires TwinAuxRel(snapshot, aux_store)
    requires !buf.requires_rebuild
    requires |buf.deltas| > 0
    ensures Twin.StageMaintenance(buf, Twin.ApplyDelta, snapshot, rebuilt).action
         == Twin.ApplyDelta
    ensures TwinAuxRel(
              Twin.StageMaintenance(buf, Twin.ApplyDelta, snapshot, rebuilt).state,
              ApplyTwinDeltaSeq(aux_store, buf.deltas))
  {
    Twin.StageMaintenanceApplyDelta(buf, snapshot, rebuilt);
    TwinMaintRefinesAux_ApplyDeltaSeq(snapshot, aux_store, buf.deltas);
  }

  // ----- Rebuild under explicit rebuild-correctness contract (HB2) -----
  // The Twin StageMaintenance RebuildPartition arm takes a free `rebuilt` oracle.
  // Correctness is not "whatever the oracle returns"; it is the named contract below.
  // This is Twin underspecification closed by a contract — not a C++ change.

  ghost predicate RebuildOracleCorrect(rebuilt: Twin.DayStore, expected_aux: Store) {
    Flatten(rebuilt) == expected_aux
  }

  // Typical instantiation: expected_aux == FoldTwin(db) (full rebuild), or the
  // Aux store after applying the same logical maintenance as the rebuild target.
  lemma TwinMaintRefinesAux_RebuildPartition(
    buf: Twin.TxnBuffer,
    snapshot: Twin.DayStore,
    rebuilt: Twin.DayStore,
    expected_aux: Store
  )
    requires !buf.requires_rebuild
    requires |buf.deltas| > 0
    requires RebuildOracleCorrect(rebuilt, expected_aux)
    ensures Twin.StageMaintenance(buf, Twin.RebuildPartition, snapshot, rebuilt).action
         == Twin.RebuildPartition
    ensures Twin.StageMaintenance(buf, Twin.RebuildPartition, snapshot, rebuilt).state
         == rebuilt
    ensures TwinAuxRel(rebuilt, expected_aux)
  {
    Twin.StageMaintenanceRebuildPartitionUsesOracle(buf, snapshot, rebuilt);
  }

  // RebuildAll / requires_rebuild upgrade (HB1): Twin keeps snapshot and only
  // changes the action tag. Store refine ≡ Aux NoOp is Qed in Q1TwinAuxClose
  // (TwinMaintRefinesAux_RequiresRebuild_*). Intentional action retag, not a
  // C++ nip — see CURRENT_THEOREM_TCB.md / TWIN_AUX_HARD_BRANCHES.md.

  lemma TwinMaint_RequiresRebuild_preserves_snapshot(
    buf: Twin.TxnBuffer,
    decision: Twin.MaintenanceAction,
    snapshot: Twin.DayStore,
    rebuilt: Twin.DayStore,
    aux_store: Store
  )
    requires buf.requires_rebuild
    requires TwinAuxRel(snapshot, aux_store)
    ensures Twin.StageMaintenance(buf, decision, snapshot, rebuilt).state == snapshot
    ensures TwinAuxRel(
              Twin.StageMaintenance(buf, decision, snapshot, rebuilt).state,
              aux_store)
  {
  }

  // ----- Hit-only Execute (HB3 slice) -----

  lemma TwinExec_hit_eq_ComposeOutput(
    args: Twin.QueryArgs,
    base_days: int,
    store_days: Twin.DayStore,
    built: Twin.DayStore
  )
    requires args.has_offset
    ensures Twin.Execute(args, base_days, true, true, store_days, built)
         == Twin.ComposeOutput(store_days, Twin.TpchQ1CutoffDays(base_days, args.day_offset))
  {
  }

  // Cutoff view of a flattened Aux store: merge all cells with day <= cutoff by gkey.
  // Used as the representation-level target of ComposeOutput under TwinAuxRel.
  ghost function AuxMergeCell(m: map<int, Stats>, gkey: int, s: Stats): map<int, Stats> {
    if gkey in m then
      m[gkey := Merge(m[gkey], s)]
    else
      m[gkey := s]
  }

  ghost function AuxCutoffMergeFromKeys(store: Store, cutoff: int, keys: seq<CellKey>): map<int, Stats>
    decreases |keys|
  {
    if |keys| == 0 then
      map[]
    else
      var ck := keys[0];
      var rest := AuxCutoffMergeFromKeys(store, cutoff, keys[1..]);
      if ck.day <= cutoff && ck in store then
        AuxMergeCell(rest, ck.gkey, store[ck])
      else
        rest
  }

  // Well-formed DayStore: every day in parts appears in order (PutPart invariant).
  ghost predicate WellFormedDayStore(state: Twin.DayStore) {
    forall day :: day in state.parts ==> day in state.order
  }

  lemma Empty_WellFormed()
    ensures WellFormedDayStore(Twin.EmptyStore())
  {
  }

  lemma ApplyDeltaToStore_preserves_WellFormed(state: Twin.DayStore, d: Twin.LineitemDelta)
    requires WellFormedDayStore(state)
    ensures WellFormedDayStore(Twin.ApplyDeltaToStore(state, d))
  {
  }

  lemma ApplyDeltaSeq_preserves_WellFormed(state: Twin.DayStore, ds: seq<Twin.LineitemDelta>)
    requires WellFormedDayStore(state)
    ensures WellFormedDayStore(Twin.ApplyDeltaSeq(state, ds))
    decreases |ds|
  {
    if |ds| == 0 {
    } else {
      ApplyDeltaToStore_preserves_WellFormed(state, ds[0]);
      ApplyDeltaSeq_preserves_WellFormed(Twin.ApplyDeltaToStore(state, ds[0]), ds[1..]);
    }
  }

  // TwinExecRefinesAux hit path — representation slice:
  // Execute hit = ComposeOutput(store_days). Full ComposeOutput ≡ ExecSeqM under
  // TwinAuxRel+Inv+PartsWF+OrderUnique is proved in Q1TwinAuxClose
  // (FinalizeSortFaithful_from_map + TwinExecRefinesAux_ComposeEqAux).
  // Premises package TwinAuxRel+Inv.
  ghost predicate TwinExecRefinesAux_Premises(
    days: Twin.DayStore,
    store: Store,
    db: seq<Row>
  )
  {
    TwinAuxRel(days, store) &&
    WellFormedDayStore(days) &&
    (forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256) &&
    Inv(store, db)
  }

  lemma TwinExecRefinesAux_hit_ComposeOnly(
    args: Twin.QueryArgs,
    base_days: int,
    days: Twin.DayStore,
    store: Store,
    db: seq<Row>,
    built: Twin.DayStore
  )
    requires TwinExecRefinesAux_Premises(days, store, db)
    requires args.has_offset
    ensures Twin.Execute(args, base_days, true, true, days, built)
         == Twin.ComposeOutput(days, Twin.TpchQ1CutoffDays(base_days, args.day_offset))
    ensures TwinAuxRel(days, store)
    ensures Inv(store, db)
  {
    TwinExec_hit_eq_ComposeOutput(args, base_days, days, built);
  }

  // ----- ComposeOutput ≡ Aux Finalize∘Compose (HB3b) -----
  // Cell bridge: Compose_GetKey_eq_FilteredContrib (Q1Aux).
  // Map + finalize + fallback closed in Q1TwinAuxClose (see CURRENT_THEOREM_TCB.md).
  // No C++ change.

  ghost function DayPartToKeyStore(part: Twin.DayPart): KeyStore {
    map g | g in part.groups :: StatsToAux(part.groups[g])
  }

  ghost function TwinMergedAtCutoff(days: Twin.DayStore, cutoff: int): Twin.DayPart {
    Twin.ComposeMergeDays(days, cutoff, 0, Twin.EmptyPart())
  }

  // Target equality (proved in Q1TwinAuxClose via map + FinalizeSortFaithful_from_map).
  ghost predicate ComposeOutput_eq_AuxFinalizeCompose(
    days: Twin.DayStore,
    db: seq<Row>,
    cutoff: int
  )
    requires RowsOk(db)
  {
    DayPartToKeyStore(TwinMergedAtCutoff(days, cutoff)) == Compose(db, cutoff)
  }

  // Under TwinAuxRel+Inv, per-key Compose cells equal FilteredContrib(db).
  // Full DayPartToKeyStore(TwinMerged) == Compose remains (a).
  lemma TwinExec_ComposeCells_FilteredContrib(
    days: Twin.DayStore,
    store: Store,
    db: seq<Row>,
    cutoff: int,
    g: int
  )
    requires TwinExecRefinesAux_Premises(days, store, db)
    ensures GetKey(Compose(db, cutoff), g) == FilteredContrib(db, cutoff, g)
  {
    Compose_GetKey_eq_FilteredContrib(db, cutoff, g);
  }

  // ----- BuildStateForDays ⇒ RebuildOracleCorrect (lineitem path) -----

  ghost function InputToRow(inp: Twin.LineitemInput): Row
    requires 0 <= inp.returnflag < 256 && 0 <= inp.linestatus < 256
    requires inp.quantity_scale >= 0 && inp.extended_price_scale >= 0
    requires inp.discount_scale >= 0 && inp.tax_scale >= 0
  {
    Row(inp.shipdate_days,
        Twin.ScaleToRaw(inp.quantity_raw, inp.quantity_scale, 2),
        Twin.ScaleToRaw(inp.extended_price_raw, inp.extended_price_scale, 2),
        Twin.ScaleToRaw(inp.discount_raw, inp.discount_scale, 2),
        Twin.ScaleToRaw(inp.tax_raw, inp.tax_scale, 2),
        inp.returnflag, inp.linestatus)
  }

  ghost function InputsToRows(inputs: seq<Twin.LineitemInput>): seq<Row>
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    ensures |InputsToRows(inputs)| == |inputs|
    ensures forall i :: 0 <= i < |InputsToRows(inputs)| ==>
              0 <= InputsToRows(inputs)[i].rf < 256 &&
              0 <= InputsToRows(inputs)[i].ls < 256
    decreases |inputs|
  {
    if |inputs| == 0 then []
    else [InputToRow(inputs[0])] + InputsToRows(inputs[1..])
  }

  ghost function InputToDelta(inp: Twin.LineitemInput): Twin.LineitemDelta
    requires 0 <= inp.returnflag < 256 && 0 <= inp.linestatus < 256
    requires inp.quantity_scale >= 0 && inp.extended_price_scale >= 0
    requires inp.discount_scale >= 0 && inp.tax_scale >= 0
  {
    Twin.LineitemDelta(
      inp.shipdate_days,
      F.PackQ1GroupKey(inp.returnflag, inp.linestatus),
      Twin.ScaleToRaw(inp.quantity_raw, inp.quantity_scale, 2),
      Twin.ScaleToRaw(inp.extended_price_raw, inp.extended_price_scale, 2),
      Twin.ScaleToRaw(inp.discount_raw, inp.discount_scale, 2),
      Twin.ScaleToRaw(inp.tax_raw, inp.tax_scale, 2),
      1)
  }

  ghost function InputsToDeltas(inputs: seq<Twin.LineitemInput>): seq<Twin.LineitemDelta>
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    decreases |inputs|
  {
    if |inputs| == 0 then []
    else [InputToDelta(inputs[0])] + InputsToDeltas(inputs[1..])
  }

  lemma InputToDelta_eq_RowToDelta(inp: Twin.LineitemInput)
    requires 0 <= inp.returnflag < 256 && 0 <= inp.linestatus < 256
    requires inp.quantity_scale >= 0 && inp.extended_price_scale >= 0
    requires inp.discount_scale >= 0 && inp.tax_scale >= 0
    ensures InputToDelta(inp) == RowToDelta(InputToRow(inp), 1)
  {
  }

  lemma {:isolate_assertions} Flatten_ApplyLineitemInput(
    state: Twin.DayStore, inp: Twin.LineitemInput
  )
    requires 0 <= inp.returnflag < 256 && 0 <= inp.linestatus < 256
    requires inp.quantity_scale >= 0 && inp.extended_price_scale >= 0
    requires inp.discount_scale >= 0 && inp.tax_scale >= 0
    requires TwinStoreCountsNonNeg(state)
    ensures Twin.PutPart(state, inp.shipdate_days,
              Twin.ApplyLineitemInput(inp, Twin.GetPart(state, inp.shipdate_days)))
         == Twin.ApplyDeltaToStore(state, InputToDelta(inp))
    ensures Flatten(
              Twin.PutPart(state, inp.shipdate_days,
                Twin.ApplyLineitemInput(inp, Twin.GetPart(state, inp.shipdate_days))))
         == ApplyTwinDelta(Flatten(state), InputToDelta(inp))
    ensures TwinStoreCountsNonNeg(
              Twin.PutPart(state, inp.shipdate_days,
                Twin.ApplyLineitemInput(inp, Twin.GetPart(state, inp.shipdate_days))))
  {
    var d := InputToDelta(inp);
    var day := d.shipdate_days;
    var part := Twin.GetPart(state, day);
    var g := Twin.GetGroup(part, d.group_key);
    assert g.count >= 0;
    var g2 := F.ApplyQ1RowContribution(g, d.quantity_raw, d.extended_price_raw,
                                       d.discount_raw, d.tax_raw, 1);
    assert g2.count == g.count + 1 != 0;
    assert Twin.ApplyLineitemInput(inp, part)
        == Twin.PutGroup(part, d.group_key, g2);
    assert Twin.ApplyDeltaToStore(state, d)
        == Twin.PutPart(state, day, Twin.PutGroup(part, d.group_key, g2));
    Flatten_ApplyDeltaToStore(state, d);
    InputToDelta_eq_RowToDelta(inp);
    ApplyDeltaToStore_insert_preserves_nonneg(state, InputToRow(inp));
  }

  lemma {:isolate_assertions} Flatten_ConsumeLineitems(
    state: Twin.DayStore, inputs: seq<Twin.LineitemInput>
  )
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    requires TwinStoreCountsNonNeg(state)
    ensures Flatten(Twin.ConsumeLineitems(state, inputs, {}))
         == ApplyTwinDeltaSeq(Flatten(state), InputsToDeltas(inputs))
    ensures TwinStoreCountsNonNeg(Twin.ConsumeLineitems(state, inputs, {}))
    decreases |inputs|
  {
    if |inputs| == 0 {
    } else {
      var inp := inputs[0];
      var state1 := Twin.PutPart(state, inp.shipdate_days,
        Twin.ApplyLineitemInput(inp, Twin.GetPart(state, inp.shipdate_days)));
      Flatten_ApplyLineitemInput(state, inp);
      Flatten_ConsumeLineitems(state1, inputs[1..]);
    }
  }

  lemma InputsToRows_ok(inputs: seq<Twin.LineitemInput>)
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    ensures RowsOk(InputsToRows(inputs))
  {
  }

  lemma RowsToInsertDeltas_eq_InputsToDeltas(inputs: seq<Twin.LineitemInput>)
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    ensures RowsToInsertDeltas(InputsToRows(inputs)) == InputsToDeltas(inputs)
    decreases |inputs|
  {
    if |inputs| == 0 {
    } else {
      InputToDelta_eq_RowToDelta(inputs[0]);
      RowsToInsertDeltas_eq_InputsToDeltas(inputs[1..]);
    }
  }

  lemma BuildStateForDays_lineitems_RebuildOracleCorrect(
    inputs: seq<Twin.LineitemInput>
  )
    requires forall i :: 0 <= i < |inputs| ==>
              0 <= inputs[i].returnflag < 256 && 0 <= inputs[i].linestatus < 256 &&
              inputs[i].quantity_scale >= 0 && inputs[i].extended_price_scale >= 0 &&
              inputs[i].discount_scale >= 0 && inputs[i].tax_scale >= 0
    ensures var rebuilt := Twin.BuildStateForDays({}, [], false, [], false, inputs);
            var rows := InputsToRows(inputs);
            RebuildOracleCorrect(rebuilt, FoldTwin(rows))
  {
    InputsToRows_ok(inputs);
    Twin.BuildStateLineitemFold(inputs);
    assert Twin.BuildStateForDays({}, [], false, [], false, inputs)
        == Twin.ConsumeLineitems(Twin.EmptyStore(), inputs, {});
    FlattenEmpty();
    Empty_TwinStoreCountsNonNeg();
    Flatten_ConsumeLineitems(Twin.EmptyStore(), inputs);
    RowsToInsertDeltas_eq_InputsToDeltas(inputs);
    FoldTwinFromTwin_eq_AuxFoldTwin(InputsToRows(inputs));
    Flatten_ApplyDeltaSeq(Twin.EmptyStore(), RowsToInsertDeltas(InputsToRows(inputs)));
  }

  // Aggregates-for-days / aggregates-all BuildState arms: RebuildOracleCorrect
  // under intentional SPI AggsFaithful (Q1TwinAuxClose). Not unfinished bookkeeping.
}
