// Q1 TwinOp ≡ SpecOp bridge + FoldTwin = FoldSpec.
// Inv/maintenance use FoldTwin; Rocq uses FoldSpec / AuxAnswerSpec only.

include "generated/Q1AuxSpec.dfy"
include "Q1Finalize.dfy"
include "Q1Aux.dfy"
include "Q1ModuleTwin.dfy"

module Q1TwinSpecBridge {

  import Spec = Q1AuxSpec
  import opened Q1Finalize
  import opened Q1Aux
  import Twin = Q1ModuleTwin

  ghost function TwinStatsToSpec(s: Q1Finalize.Stats): Spec.Stats {
    Spec.Stats(s.count, s.sum_qty, s.sum_base_price, s.sum_disc_price,
               s.sum_charge, s.sum_discount)
  }

  ghost function SpecStatsToTwin(s: Spec.Stats): Q1Finalize.Stats {
    Q1Finalize.Stats(s.count, s.sum_qty, s.sum_base_price, s.sum_disc_price,
                     s.sum_charge, s.sum_discount)
  }

  ghost function AuxStatsToFinalize(s: Q1Aux.Stats): Q1Finalize.Stats {
    Q1Finalize.Stats(s.count, s.sum_qty, s.sum_base_price, s.sum_disc_price,
                     s.sum_charge, s.sum_discount)
  }

  ghost function AuxStatsToSpec(s: Q1Aux.Stats): Spec.Stats {
    Spec.Stats(s.count, s.sum_qty, s.sum_base_price, s.sum_disc_price,
               s.sum_charge, s.sum_discount)
  }

  ghost function SpecStatsToAux(s: Spec.Stats): Q1Aux.Stats {
    Q1Aux.Stats(s.count, s.sum_qty, s.sum_base_price, s.sum_disc_price,
                s.sum_charge, s.sum_discount)
  }

  ghost function AuxRowToSpec(r: Q1Aux.Row): Spec.Row
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
  {
    Spec.Row(r.shipdate, r.qty, r.price, r.disc, r.tax, r.rf, r.ls)
  }

  ghost function AuxRowsToSpec(t: seq<Q1Aux.Row>): seq<Spec.Row>
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall i :: 0 <= i < |AuxRowsToSpec(t)| ==>
              0 <= AuxRowsToSpec(t)[i].rf < 256 && 0 <= AuxRowsToSpec(t)[i].ls < 256
    ensures |AuxRowsToSpec(t)| == |t|
    decreases |t|
  {
    if |t| == 0 then []
    else [AuxRowToSpec(t[0])] + AuxRowsToSpec(t[1..])
  }

  lemma AuxRowsToSpec_snoc(t: seq<Q1Aux.Row>, r: Q1Aux.Row)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures AuxRowsToSpec(t + [r]) == AuxRowsToSpec(t) + [AuxRowToSpec(r)]
    decreases |t|
  {
    if |t| == 0 {
    } else {
      AuxRowsToSpec_snoc(t[1..], r);
      assert (t + [r])[0] == t[0];
      assert (t + [r])[1..] == t[1..] + [r];
    }
  }

  ghost function TwinKSToSpec(m: Q1Aux.KeyStore): Spec.KeyStore {
    map g | g in m :: AuxStatsToSpec(m[g])
  }

  lemma TwinApply_eq_SpecApply(
    s: Q1Finalize.Stats,
    qty: int, price: int, disc: int, tax: int, sign: int
  )
    ensures TwinStatsToSpec(ApplyQ1RowContribution(s, qty, price, disc, tax, sign))
         == Spec.SpecApply(TwinStatsToSpec(s),
              Spec.Row(0, qty, price, disc, tax, 0, 0), sign)
  {
  }

  lemma TwinMerge_eq_SpecMerge(a: Q1Finalize.Stats, b: Q1Finalize.Stats)
    ensures TwinStatsToSpec(MergeQ1DayGroupStats(a, b))
         == Spec.SpecMerge(TwinStatsToSpec(a), TwinStatsToSpec(b))
  {
  }

  lemma TwinApplyRow_eq_SpecApply(s: Q1Aux.Stats, r: Q1Aux.Row, sign: int)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures Q1Aux.ApplyRow(s, r, sign)
         == SpecStatsToAux(Spec.SpecApply(AuxStatsToSpec(s), AuxRowToSpec(r), sign))
  {
  }

  lemma FoldTwin_eq_FoldSpec(t: seq<Q1Aux.Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall k: Q1Aux.CellKey ::
              (k in Q1Aux.FoldTwin(t) <==>
                 Spec.CellKey(k.day, k.gkey) in Spec.FoldSpec(AuxRowsToSpec(t))) &&
              (k in Q1Aux.FoldTwin(t) ==>
                 Q1Aux.FoldTwin(t)[k] ==
                 SpecStatsToAux(Spec.FoldSpec(AuxRowsToSpec(t))[Spec.CellKey(k.day, k.gkey)]))
    decreases |t|
  {
    if |t| == 0 {
    } else {
      FoldTwin_eq_FoldSpec(t[..|t| - 1]);
    }
  }

  // ----- B1: Twin Compose ≡ SpecViewFromDB(AuxRowsToSpec) ≡ SpecView semantics -----

  lemma SpecViewFromDB_step(db: seq<Spec.Row>, r: Spec.Row, cutoff: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures (r.shipdate <= cutoff) ==>
              (Spec.SpecViewFromDB(db + [r], cutoff)
           == (var g := Spec.RowKey(r);
               var rest := Spec.SpecViewFromDB(db, cutoff);
               rest[g := Spec.SpecApply(Spec.SpecGetKey(rest, g), r, 1)]))
    ensures (r.shipdate > cutoff) ==>
              (Spec.SpecViewFromDB(db + [r], cutoff) == Spec.SpecViewFromDB(db, cutoff))
  {
    assert (db + [r])[..|db|] == db;
  }

  lemma TwinView_eq_SpecView(t: seq<Q1Aux.Row>, cutoff: int)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures TwinKSToSpec(Compose(t, cutoff))
         == Spec.SpecViewFromDB(AuxRowsToSpec(t), cutoff)
    decreases |t|
  {
    if |t| == 0 {
    } else {
      var pref := t[..|t| - 1];
      var r := t[|t| - 1];
      assert t == pref + [r];
      TwinView_eq_SpecView(pref, cutoff);
      AuxRowsToSpec_snoc(pref, r);
      ComposeStep(pref, r, cutoff);
      SpecViewFromDB_step(AuxRowsToSpec(pref), AuxRowToSpec(r), cutoff);
      if r.shipdate <= cutoff {
        var g := Q1Aux.RowKey(r);
        assert Compose(t, cutoff) == ApplyDeltaKey(Compose(pref, cutoff), r, 1);
        TwinApplyRow_eq_SpecApply(GetKey(Compose(pref, cutoff), g), r, 1);
        // TwinKSToSpec after ApplyDeltaKey matches SpecViewFromDB update.
        assert TwinKSToSpec(Compose(pref, cutoff))
            == Spec.SpecViewFromDB(AuxRowsToSpec(pref), cutoff);
        assert TwinKSToSpec(Compose(t, cutoff))
            == Spec.SpecViewFromDB(AuxRowsToSpec(t), cutoff);
      } else {
        assert Compose(t, cutoff) == Compose(pref, cutoff);
        assert AuxRowsToSpec(t) == AuxRowsToSpec(pref) + [AuxRowToSpec(r)];
      }
    }
  }

  // ----- B2: Twin Finalize ≡ SpecFinalize via OutRowRep -----
  // Spec avgs: AvgRound (PG ties-away-from-zero) at scale-2.
  // Twin: HugeintScaleToDouble. ScaleToRaw(_,2,2) id on Spec integer avgs.

  ghost predicate OutRowRep(twin: Q1Finalize.OutRow, spec: Spec.OutRow, s: Spec.Stats)
  {
    twin.group_key == Spec.Pack(spec.rf, spec.ls) &&
    0 <= spec.rf < 256 && 0 <= spec.ls < 256 &&
    s.count > 0 &&
    twin.sum_qty == spec.sum_qty == s.sum_qty &&
    twin.sum_base_price == spec.sum_base_price == s.sum_base_price &&
    twin.sum_disc_price == spec.sum_disc_price == s.sum_disc_price &&
    twin.sum_charge == spec.sum_charge == s.sum_charge &&
    twin.count_order == spec.count_order == s.count &&
    spec.sum_discount == s.sum_discount &&
    spec.avg_qty == Spec.AvgRound(s.sum_qty, s.count) &&
    spec.avg_price == Spec.AvgRound(s.sum_base_price, s.count) &&
    spec.avg_disc == Spec.AvgRound(s.sum_discount, s.count) &&
    twin.avg_qty == HugeintScaleToDouble(s.sum_qty, kDecimalScale2, s.count as nat) &&
    twin.avg_price == HugeintScaleToDouble(s.sum_base_price, kDecimalScale2, s.count as nat) &&
    twin.avg_disc == HugeintScaleToDouble(s.sum_discount, kDecimalScale2, s.count as nat) &&
    Twin.ScaleToRaw(spec.avg_qty, 2, 2) == spec.avg_qty &&
    Twin.ScaleToRaw(spec.avg_price, 2, 2) == spec.avg_price &&
    Twin.ScaleToRaw(spec.avg_disc, 2, 2) == spec.avg_disc
  }

  lemma TwinFinalizeOne_eq_SpecFinalizeOne(gkey: int, s: Q1Finalize.Stats)
    requires s.count > 0
    requires 0 <= gkey < 65536
    ensures |FinalizeOneEntry(gkey, s)| == 1
    ensures |Spec.SpecFinalizeOne(gkey, TwinStatsToSpec(s))| == 1
    ensures OutRowRep(FinalizeOneEntry(gkey, s)[0],
                      Spec.SpecFinalizeOne(gkey, TwinStatsToSpec(s))[0],
                      TwinStatsToSpec(s))
  {
    var spec_s := TwinStatsToSpec(s);
    Twin.ScaleToRawIdentity(Spec.AvgRound(spec_s.sum_qty, spec_s.count));
    Twin.ScaleToRawIdentity(Spec.AvgRound(spec_s.sum_base_price, spec_s.count));
    Twin.ScaleToRawIdentity(Spec.AvgRound(spec_s.sum_discount, spec_s.count));
  }

  lemma TwinFinalizeOne_empty(gkey: int, s: Q1Finalize.Stats)
    requires s.count <= 0
    ensures FinalizeOneEntry(gkey, s) == []
    ensures Spec.SpecFinalizeOne(gkey, TwinStatsToSpec(s)) == []
  {
  }

  lemma TwinFinalizeOne_sums_counts(gkey: int, s: Q1Finalize.Stats)
    requires s.count > 0
    ensures |FinalizeOneEntry(gkey, s)| == 1
    ensures FinalizeOneEntry(gkey, s)[0].count_order == s.count
    ensures FinalizeOneEntry(gkey, s)[0].sum_qty == s.sum_qty
    ensures FinalizeOneEntry(gkey, s)[0].sum_base_price == s.sum_base_price
    ensures FinalizeOneEntry(gkey, s)[0].sum_disc_price == s.sum_disc_price
    ensures FinalizeOneEntry(gkey, s)[0].sum_charge == s.sum_charge
  {
  }

  ghost function KeyStoreToEntries(m: Q1Aux.KeyStore): seq<(int, Q1Finalize.Stats)>
    decreases |m.Keys|
  {
    if m.Keys == {} then
      []
    else
      var g :| g in m.Keys;
      [(g, AuxStatsToFinalize(m[g]))] + KeyStoreToEntries(m - {g})
  }

  ghost function TwinFinalize(m: Q1Aux.KeyStore): seq<Q1Finalize.OutRow> {
    FinalizeFromEntries(KeyStoreToEntries(m))
  }

  ghost predicate SortedByGroupKey(rows: seq<Q1Finalize.OutRow>) {
    forall i :: 0 <= i < |rows| - 1 ==> rows[i].group_key <= rows[i + 1].group_key
  }

  lemma InsertSorted_sorted(rows: seq<Q1Finalize.OutRow>, row: Q1Finalize.OutRow)
    requires SortedByGroupKey(rows)
    ensures SortedByGroupKey(InsertSorted(rows, row))
    decreases |rows|
  {
    if |rows| == 0 {
    } else if row.group_key < rows[0].group_key {
    } else {
      assert SortedByGroupKey(rows[1..]);
      InsertSorted_sorted(rows[1..], row);
    }
  }

  lemma SortByKey_sorted(rows: seq<Q1Finalize.OutRow>)
    ensures SortedByGroupKey(SortByKey(rows))
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SortByKey_sorted(rows[1..]);
      InsertSorted_sorted(SortByKey(rows[1..]), rows[0]);
    }
  }

  lemma TwinFinalize_sorted(m: Q1Aux.KeyStore)
    ensures SortedByGroupKey(TwinFinalize(m))
  {
    SortByKey_sorted(CollectFinalize(KeyStoreToEntries(m)));
  }

  // Per-key finalize correspondence (ORDER BY: twin SortByKey; Spec bag via keys).
  lemma TwinFinalize_rep_SpecFinalize(m: Q1Aux.KeyStore)
    requires forall g :: g in m ==> m[g].count > 0
    requires forall g :: g in m ==> 0 <= g < 65536
    ensures SortedByGroupKey(TwinFinalize(m))
    ensures forall g :: g in m ==>
              OutRowRep(FinalizeOneEntry(g, AuxStatsToFinalize(m[g]))[0],
                        Spec.SpecFinalizeOne(g, AuxStatsToSpec(m[g]))[0],
                        AuxStatsToSpec(m[g]))
  {
    TwinFinalize_sorted(m);
    forall g | g in m
      ensures OutRowRep(FinalizeOneEntry(g, AuxStatsToFinalize(m[g]))[0],
                        Spec.SpecFinalizeOne(g, AuxStatsToSpec(m[g]))[0],
                        AuxStatsToSpec(m[g]))
    {
      TwinFinalizeOne_eq_SpecFinalizeOne(g, AuxStatsToFinalize(m[g]));
    }
  }

  // ----- B3: AuxAnswerTwin ≡ AuxAnswerSpec via OutRowRep -----

  ghost function AuxAnswerTwin(t: seq<Q1Aux.Row>, cutoff: int): seq<Q1Finalize.OutRow>
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  {
    TwinFinalize(Compose(t, cutoff))
  }

  ghost predicate AuxAnswerRep(view: Q1Aux.KeyStore)
  {
    (forall g :: g in view ==> view[g].count > 0) &&
    (forall g :: g in view ==> 0 <= g < 65536) &&
    SortedByGroupKey(TwinFinalize(view)) &&
    (forall g :: g in view ==>
       OutRowRep(FinalizeOneEntry(g, AuxStatsToFinalize(view[g]))[0],
                 Spec.SpecFinalizeOne(g, AuxStatsToSpec(view[g]))[0],
                 AuxStatsToSpec(view[g])))
  }

  lemma Compose_positive_counts(t: seq<Q1Aux.Row>, cutoff: int)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall g :: g in Compose(t, cutoff) ==> Compose(t, cutoff)[g].count > 0
    decreases |t|
  {
    if |t| == 0 {
    } else {
      var pref := t[..|t| - 1];
      var r := t[|t| - 1];
      Compose_positive_counts(pref, cutoff);
      assert t == pref + [r];
      ComposeStep(pref, r, cutoff);
    }
  }

  lemma AuxAnswerTwin_eq_AuxAnswerSpec(t: seq<Q1Aux.Row>, cutoff: int)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires forall g :: g in Compose(t, cutoff) ==> 0 <= g < 65536
    ensures TwinKSToSpec(Compose(t, cutoff))
         == Spec.SpecViewFromDB(AuxRowsToSpec(t), cutoff)
    ensures AuxAnswerRep(Compose(t, cutoff))
    // AuxAnswerSpec now finalizes with an ORDER BY (l_returnflag, l_linestatus)
    // insertion sort (OutRowSort); the equality is definitional.
    ensures Spec.AuxAnswerSpec(AuxRowsToSpec(t), cutoff)
         == Spec.OutRowSort(Spec.SpecFinalize(Spec.SpecViewFromDB(AuxRowsToSpec(t), cutoff)))
  {
    TwinView_eq_SpecView(t, cutoff);
    Compose_positive_counts(t, cutoff);
    TwinFinalize_rep_SpecFinalize(Compose(t, cutoff));
  }

  // ----- C3: AffectsNoOp_Q1 -----

  ghost predicate Affects_Q1(lineitem_table: int, mutation_table: int) {
    mutation_table == lineitem_table
  }

  ghost function MutIfAffects(t: seq<Q1Aux.Row>, lineitem_table: int,
                              mutation_table: int, delta: seq<Q1Aux.Row>)
    : seq<Q1Aux.Row>
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires forall i :: 0 <= i < |delta| ==> 0 <= delta[i].rf < 256 && 0 <= delta[i].ls < 256
  {
    if Affects_Q1(lineitem_table, mutation_table) then t + delta else t
  }

  lemma AffectsNoOp_Q1(t: seq<Q1Aux.Row>, lineitem_table: int, mutation_table: int,
                       delta: seq<Q1Aux.Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires forall i :: 0 <= i < |delta| ==> 0 <= delta[i].rf < 256 && 0 <= delta[i].ls < 256
    requires !Affects_Q1(lineitem_table, mutation_table)
    ensures Twin.InterpretMutation(lineitem_table, mutation_table) == Twin.ScopeNone
    ensures MutIfAffects(t, lineitem_table, mutation_table, delta) == t
    ensures FoldTwin(MutIfAffects(t, lineitem_table, mutation_table, delta))
         == FoldTwin(t)
  {
    Twin.InterpretIrrelevant(lineitem_table, mutation_table);
  }

}
