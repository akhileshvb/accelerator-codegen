// Q1ResultSpecBridge — Q1ExecCorrect (+ ordered sequence form)
//
// Thin Q1 instantiation of PartitionedGroupAggregate morphism:
//   partition=shipdate, key=Pack(rf,ls), accept=day<=cutoff, contrib=Apply
// Pattern-generic induction lives here as Filter/Fold/Compose eqs;
// reusable ordering algebra: SortedUniqueMap.SameMapSameSortedSeq.
// PGA recipe + shared Rocq facades: tests/verification/patterns/PartitionedGroupAggregate.md
//
// Does NOT use AuxAnswerSpec / Q1TwinSpecBridge / FormalSQLAuxCorrect.

include "generated/Q1ResultSpec.dfy"
include "Q1Aux.dfy"
include "Q1Finalize.dfy"
include "EvalLogicalSpec.dfy"
include "Q1RelNodeBridge.dfy"
include "../../patterns/dafny/SortedUniqueMap.dfy"

module Q1ResultSpecBridge {

  import Spec = Q1ResultSpec
  import opened Q1Aux
  import F = Q1Finalize
  import SU = SortedUniqueMap
  import ELS = EvalLogicalSpec
  import LSD = LogicalSpecData
  import RelSpec = Q1RelNodeSpec
  import RelBridge = Q1RelNodeBridge

  // ----- Q1-SPECIFIC morphisms Aux ↔ Spec -----

  ghost function AuxRowToSpec(r: Row): Spec.Row
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
  {
    Spec.Row(r.shipdate, r.qty, r.price, r.disc, r.tax, r.rf, r.ls)
  }

  ghost function AuxRowsToSpec(t: seq<Row>): seq<Spec.Row>
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures |AuxRowsToSpec(t)| == |t|
    ensures forall i :: 0 <= i < |AuxRowsToSpec(t)| ==>
              0 <= AuxRowsToSpec(t)[i].rf < 256 && 0 <= AuxRowsToSpec(t)[i].ls < 256
    decreases |t|
  {
    if |t| == 0 then []
    else [AuxRowToSpec(t[0])] + AuxRowsToSpec(t[1..])
  }

  ghost function AuxStatsToSpec(s: Q1Aux.Stats): Spec.SpecStats {
    Spec.SpecStats(s.count, s.sum_qty, s.sum_base_price, s.sum_disc_price,
                   s.sum_charge, s.sum_discount)
  }

  ghost function AuxStatsToFinalize(s: Q1Aux.Stats): F.Stats {
    F.Stats(s.count, s.sum_qty, s.sum_base_price, s.sum_disc_price,
            s.sum_charge, s.sum_discount)
  }

  // Q1-SPECIFIC / REPRESENTATION: contrib formulas match ($f4/$f5 scaled Apply).
  lemma key_Q1_eq_SpecKey(r: Row)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures RowKey(r) == Spec.RowKey(AuxRowToSpec(r))
  {
  }

  lemma accept_Q1_eq_SpecFilter(r: Row, cutoff: int)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures (r.shipdate <= cutoff) <==>
            (AuxRowToSpec(r).shipdate <= cutoff)
  {
  }

  lemma contrib_Q1_eq_SpecContrib(s: Q1Aux.Stats, r: Row, sign: int)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures AuxStatsToSpec(ApplyRow(s, r, sign))
         == Spec.SpecApply(AuxStatsToSpec(s), AuxRowToSpec(r), sign)
  {
  }

  lemma AuxRowsToSpec_snoc(t: seq<Row>, r: Row)
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

  // PATTERN-GENERIC structure (filter/fold morphism); Q1 Accept/Key/Contrib.
  lemma FilterCutoff_eq_Spec(t: seq<Row>, cutoff: int)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures AuxRowsToSpec(FilterCutoff(t, cutoff))
         == Spec.FilterCutoff(AuxRowsToSpec(t), cutoff)
    decreases |t|
  {
    if |t| == 0 {
    } else {
      var pref := t[..|t| - 1];
      var r := t[|t| - 1];
      assert t == pref + [r];
      FilterCutoff_eq_Spec(pref, cutoff);
      AuxRowsToSpec_snoc(pref, r);
      accept_Q1_eq_SpecFilter(r, cutoff);
      if r.shipdate <= cutoff {
        assert FilterCutoff(t, cutoff) == FilterCutoff(pref, cutoff) + [r];
        AuxRowsToSpec_snoc(FilterCutoff(pref, cutoff), r);
      } else {
        assert FilterCutoff(t, cutoff) == FilterCutoff(pref, cutoff);
      }
    }
  }

  lemma FoldByGroup_eq_Spec(t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall g ::
              (g in FoldByGroup(t) <==> g in Spec.FoldByGroup(AuxRowsToSpec(t))) &&
              (g in FoldByGroup(t) ==>
                (AuxStatsToSpec(FoldByGroup(t)[g]) == Spec.FoldByGroup(AuxRowsToSpec(t))[g]))
    decreases |t|
  {
    if |t| == 0 {
    } else {
      var pref := t[..|t| - 1];
      var r := t[|t| - 1];
      FoldByGroup_eq_Spec(pref);
      AuxRowsToSpec_snoc(pref, r);
      key_Q1_eq_SpecKey(r);
      contrib_Q1_eq_SpecContrib(GetKey(FoldByGroup(pref), RowKey(r)), r, 1);
    }
  }

  lemma Compose_eq_SpecView(t: seq<Row>, cutoff: int)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall g ::
              (g in Compose(t, cutoff) <==> g in Spec.SpecView(AuxRowsToSpec(t), cutoff)) &&
              (g in Compose(t, cutoff) ==>
                (AuxStatsToSpec(Compose(t, cutoff)[g])
                 == Spec.SpecView(AuxRowsToSpec(t), cutoff)[g]))
  {
    FilterCutoff_eq_Spec(t, cutoff);
    FoldByGroup_eq_Spec(FilterCutoff(t, cutoff));
  }

  ghost function ExecGroup(gkey: int, s: Q1Aux.Stats): F.OutRow
    requires s.count > 0
  {
    F.FinalizeOneEntry(gkey, AuxStatsToFinalize(s))[0]
  }

  // REPRESENTATION-SPECIFIC TCB: Spec (sum,count) → accelerator real AVG.
  ghost predicate AvgObservationFaithful(obs: real, sum_coeff: int, count: int) {
    count > 0 &&
    obs == F.HugeintScaleToDouble(sum_coeff, F.kDecimalScale2, count as nat)
  }

  ghost predicate ExactColsMatch(accel: F.OutRow, spec: Spec.SpecOutRow) {
    accel.group_key == spec.group_key &&
    accel.sum_qty == spec.sum_qty &&
    accel.sum_base_price == spec.sum_base_price &&
    accel.sum_disc_price == spec.sum_disc_price &&
    accel.sum_charge == spec.sum_charge &&
    accel.count_order == spec.count
  }

  ghost predicate ApproxQ1Row(accel: F.OutRow, spec: Spec.SpecOutRow) {
    ExactColsMatch(accel, spec) &&
    AvgObservationFaithful(accel.avg_qty, spec.sum_qty, spec.count) &&
    AvgObservationFaithful(accel.avg_price, spec.sum_base_price, spec.count) &&
    AvgObservationFaithful(accel.avg_disc, spec.sum_discount, spec.count)
  }

  lemma FinalizeOne_Approx(gkey: int, s: Q1Aux.Stats)
    requires s.count > 0
    ensures |F.FinalizeOneEntry(gkey, AuxStatsToFinalize(s))| == 1
    ensures |Spec.SpecFinalizeOne(gkey, AuxStatsToSpec(s))| == 1
    ensures ApproxQ1Row(
              F.FinalizeOneEntry(gkey, AuxStatsToFinalize(s))[0],
              Spec.SpecFinalizeOne(gkey, AuxStatsToSpec(s))[0])
  {
  }

  lemma Compose_positive_counts(t: seq<Row>, cutoff: int)
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

  ghost function ExecM_group(db: seq<Row>, cutoff: int, g: int): F.OutRow
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    requires g in Compose(db, cutoff)
    requires Compose(db, cutoff)[g].count > 0
  {
    ExecGroup(g, Compose(db, cutoff)[g])
  }

  ghost function EvalSpec_group(db: seq<Row>, cutoff: int, g: int): Spec.SpecOutRow
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    requires g in Spec.SpecView(AuxRowsToSpec(db), cutoff)
    requires Spec.SpecView(AuxRowsToSpec(db), cutoff)[g].count > 0
  {
    Spec.SpecFinalizeOne(g, Spec.SpecView(AuxRowsToSpec(db), cutoff)[g])[0]
  }

  // Exact column projection for ordering theorem (GENERIC via SortedUniqueMap).
  ghost function ExactEntry(accel: F.OutRow): SU.Entry<(int, int, int, int, int)> {
    SU.Entry(accel.group_key,
      (accel.sum_qty, accel.sum_base_price, accel.sum_disc_price,
       accel.sum_charge, accel.count_order))
  }

  ghost function SpecExactEntry(spec: Spec.SpecOutRow): SU.Entry<(int, int, int, int, int)> {
    SU.Entry(spec.group_key,
      (spec.sum_qty, spec.sum_base_price, spec.sum_disc_price,
       spec.sum_charge, spec.count))
  }

  ghost function ExactEntries(rows: seq<F.OutRow>): seq<SU.Entry<(int, int, int, int, int)>>
    decreases |rows|
  {
    if |rows| == 0 then []
    else [ExactEntry(rows[0])] + ExactEntries(rows[1..])
  }

  ghost function SpecExactEntries(rows: seq<Spec.SpecOutRow>): seq<SU.Entry<(int, int, int, int, int)>>
    decreases |rows|
  {
    if |rows| == 0 then []
    else [SpecExactEntry(rows[0])] + SpecExactEntries(rows[1..])
  }

  lemma ExactEntries_len(rows: seq<F.OutRow>)
    ensures |ExactEntries(rows)| == |rows|
    ensures forall i :: 0 <= i < |rows| ==>
              ExactEntries(rows)[i].key == rows[i].group_key &&
              ExactEntries(rows)[i].val ==
                (rows[i].sum_qty, rows[i].sum_base_price, rows[i].sum_disc_price,
                 rows[i].sum_charge, rows[i].count_order)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      ExactEntries_len(rows[1..]);
    }
  }

  lemma SpecExactEntries_len(rows: seq<Spec.SpecOutRow>)
    ensures |SpecExactEntries(rows)| == |rows|
    ensures forall i :: 0 <= i < |rows| ==>
              SpecExactEntries(rows)[i].key == rows[i].group_key &&
              SpecExactEntries(rows)[i].val ==
                (rows[i].sum_qty, rows[i].sum_base_price, rows[i].sum_disc_price,
                 rows[i].sum_charge, rows[i].count)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SpecExactEntries_len(rows[1..]);
    }
  }

  lemma ExactEntries_HasKey(rows: seq<F.OutRow>, k: int)
    ensures SU.HasKey(ExactEntries(rows), k) <==>
            (exists i :: 0 <= i < |rows| && rows[i].group_key == k)
    decreases |rows|
  {
    ExactEntries_len(rows);
    if |rows| == 0 {
    } else {
      ExactEntries_HasKey(rows[1..], k);
    }
  }

  lemma SpecExactEntries_HasKey(rows: seq<Spec.SpecOutRow>, k: int)
    ensures SU.HasKey(SpecExactEntries(rows), k) <==>
            (exists i :: 0 <= i < |rows| && rows[i].group_key == k)
    decreases |rows|
  {
    SpecExactEntries_len(rows);
    if |rows| == 0 {
    } else {
      SpecExactEntries_HasKey(rows[1..], k);
    }
  }

  ghost predicate ApproxQ1(accel: seq<F.OutRow>, spec: seq<Spec.SpecOutRow>) {
    |accel| == |spec| &&
    (forall i :: 0 <= i < |accel| ==> ApproxQ1Row(accel[i], spec[i]))
  }

  ghost predicate SortedOut(rows: seq<F.OutRow>) {
    forall i :: 0 <= i < |rows| - 1 ==> rows[i].group_key <= rows[i + 1].group_key
  }

  ghost predicate StrictSortedOut(rows: seq<F.OutRow>) {
    forall i :: 0 <= i < |rows| - 1 ==> rows[i].group_key < rows[i + 1].group_key
  }

  // ===== Per-group ExecCorrect (semantic core) =====

  lemma Q1ExecCorrect(store: Store, db: seq<Row>, delta: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    requires Inv(store, db)
    ensures forall g :: g in Compose(db, Spec.Cutoff(delta)) ==>
              g in Spec.SpecView(AuxRowsToSpec(db), Spec.Cutoff(delta)) &&
              Compose(db, Spec.Cutoff(delta))[g].count > 0 &&
              Spec.SpecView(AuxRowsToSpec(db), Spec.Cutoff(delta))[g].count > 0 &&
              ApproxQ1Row(
                ExecM_group(db, Spec.Cutoff(delta), g),
                EvalSpec_group(db, Spec.Cutoff(delta), g))
  {
    var cutoff := Spec.Cutoff(delta);
    Compose_positive_counts(db, cutoff);
    Compose_eq_SpecView(db, cutoff);
    forall g | g in Compose(db, cutoff)
      ensures g in Spec.SpecView(AuxRowsToSpec(db), cutoff) &&
              Compose(db, cutoff)[g].count > 0 &&
              Spec.SpecView(AuxRowsToSpec(db), cutoff)[g].count > 0 &&
              ApproxQ1Row(ExecM_group(db, cutoff, g), EvalSpec_group(db, cutoff, g))
    {
      assert AuxStatsToSpec(Compose(db, cutoff)[g])
          == Spec.SpecView(AuxRowsToSpec(db), cutoff)[g];
      FinalizeOne_Approx(g, Compose(db, cutoff)[g]);
    }
  }

  // ===== Ordering status (explicit) =====
  //
  // ORDERING_PROVED for Spec EvalResultSpec sortedness:
  lemma EvalResultSpec_sorted_ok(db: seq<Row>, delta: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures Spec.SortedByGroupKey(Spec.EvalResultSpec(AuxRowsToSpec(db), Spec.Cutoff(delta)))
  {
    Spec.EvalResultSpec_sorted(AuxRowsToSpec(db), Spec.Cutoff(delta));
  }

  // ORDERING_SEQ: given any two result sequences that are strictly sorted by
  // group_key, carry the same keys, and ApproxQ1Row-match per key, they are
  // position-wise ≈_Q1. Uses GENERIC SortedUniqueMap.SameMapSameSortedSeq on
  // exact projections; AVGs follow from ApproxQ1Row per key.
  ghost predicate HasOutKey(rows: seq<F.OutRow>, k: int) {
    exists i :: 0 <= i < |rows| && rows[i].group_key == k
  }

  ghost predicate HasSpecKey(rows: seq<Spec.SpecOutRow>, k: int) {
    exists i :: 0 <= i < |rows| && rows[i].group_key == k
  }

  lemma ApproxQ1_of_strict_sorted_same_keys(
    accel: seq<F.OutRow>,
    spec: seq<Spec.SpecOutRow>
  )
    requires StrictSortedOut(accel)
    requires forall i :: 0 <= i < |spec| - 1 ==> spec[i].group_key < spec[i + 1].group_key
    requires forall k {:trigger HasOutKey(accel, k)} {:trigger HasSpecKey(spec, k)} ::
               HasOutKey(accel, k) <==> HasSpecKey(spec, k)
    requires forall i, j ::
               (0 <= i < |accel| && 0 <= j < |spec| &&
                accel[i].group_key == spec[j].group_key)
               ==> ApproxQ1Row(accel[i], spec[j])
    ensures ApproxQ1(accel, spec)
  {
    var ae := ExactEntries(accel);
    var se := SpecExactEntries(spec);
    ExactEntries_len(accel);
    SpecExactEntries_len(spec);
    assert SU.StrictSorted(ae) by {
      forall i | 0 <= i < |ae| - 1
        ensures ae[i].key < ae[i + 1].key
      {
        assert 0 <= i < |accel| - 1;
        assert accel[i].group_key < accel[i + 1].group_key;
      }
    }
    assert SU.StrictSorted(se) by {
      forall i | 0 <= i < |se| - 1
        ensures se[i].key < se[i + 1].key
      {
        assert 0 <= i < |spec| - 1;
        assert spec[i].group_key < spec[i + 1].group_key;
      }
    }
    forall k
      ensures SU.HasKey(ae, k) <==> SU.HasKey(se, k)
    {
      ExactEntries_HasKey(accel, k);
      SpecExactEntries_HasKey(spec, k);
      assert HasOutKey(accel, k) <==> HasSpecKey(spec, k);
    }
    forall i, j | 0 <= i < |ae| && 0 <= j < |se| && ae[i].key == se[j].key
      ensures ae[i].val == se[j].val
    {
      assert ApproxQ1Row(accel[i], spec[j]);
      assert ExactColsMatch(accel[i], spec[j]);
    }
    SU.SameMapSameSortedSeq(ae, se);
    assert |accel| == |spec|;
    forall i | 0 <= i < |accel|
      ensures ApproxQ1Row(accel[i], spec[i])
    {
      assert ae[i].key == se[i].key;
      assert accel[i].group_key == spec[i].group_key;
      assert ApproxQ1Row(accel[i], spec[i]);
    }
  }

  // ===== ORDERING_PROVED: sequence-level ExecCorrect =====
  //
  // ExecSeq^M enumerates Pack keys in [0,65536) ascending (same construction as
  // EvalResultSpec). Deterministic model of KeyStore enumeration + sort-by-key.

  ghost function AuxKeysFrom(lo: int, hi: int, m: KeyStore): seq<int>
    decreases hi - lo
  {
    if lo >= hi then
      []
    else if lo in m then
      [lo] + AuxKeysFrom(lo + 1, hi, m)
    else
      AuxKeysFrom(lo + 1, hi, m)
  }

  ghost predicate AuxKeyInKeysFrom(lo: int, hi: int, m: KeyStore, k: int) {
    exists i :: 0 <= i < |AuxKeysFrom(lo, hi, m)| && AuxKeysFrom(lo, hi, m)[i] == k
  }

  lemma AuxKeysFrom_all_in_m(lo: int, hi: int, m: KeyStore)
    ensures forall i :: 0 <= i < |AuxKeysFrom(lo, hi, m)| ==>
              lo <= AuxKeysFrom(lo, hi, m)[i] < hi &&
              AuxKeysFrom(lo, hi, m)[i] in m
    decreases hi - lo
  {
    if lo >= hi {
    } else if lo in m {
      AuxKeysFrom_all_in_m(lo + 1, hi, m);
    } else {
      AuxKeysFrom_all_in_m(lo + 1, hi, m);
    }
  }

  lemma AuxKeysFrom_contains_fwd(lo: int, hi: int, m: KeyStore, k: int)
    requires lo <= k < hi && k in m
    ensures AuxKeyInKeysFrom(lo, hi, m, k)
    decreases hi - lo
  {
    if lo >= hi {
    } else if lo in m {
      if k == lo {
        assert AuxKeysFrom(lo, hi, m)[0] == lo;
      } else {
        assert lo + 1 <= k < hi;
        AuxKeysFrom_contains_fwd(lo + 1, hi, m, k);
        var j :| 0 <= j < |AuxKeysFrom(lo + 1, hi, m)| && AuxKeysFrom(lo + 1, hi, m)[j] == k;
        assert AuxKeysFrom(lo, hi, m) == [lo] + AuxKeysFrom(lo + 1, hi, m);
        assert AuxKeysFrom(lo, hi, m)[j + 1] == k;
      }
    } else {
      assert k != lo;
      assert lo + 1 <= k < hi;
      AuxKeysFrom_contains_fwd(lo + 1, hi, m, k);
      assert AuxKeysFrom(lo, hi, m) == AuxKeysFrom(lo + 1, hi, m);
    }
  }

  lemma AuxKeysFrom_first_ge(lo: int, hi: int, m: KeyStore)
    requires |AuxKeysFrom(lo, hi, m)| > 0
    ensures AuxKeysFrom(lo, hi, m)[0] >= lo
    decreases hi - lo
  {
    if lo >= hi {
    } else if lo in m {
    } else {
      AuxKeysFrom_first_ge(lo + 1, hi, m);
    }
  }

  lemma AuxKeysFrom_strict_sorted(lo: int, hi: int, m: KeyStore)
    ensures forall i :: 0 <= i < |AuxKeysFrom(lo, hi, m)| - 1 ==>
              AuxKeysFrom(lo, hi, m)[i] < AuxKeysFrom(lo, hi, m)[i + 1]
    decreases hi - lo
  {
    if lo >= hi {
    } else if lo in m {
      var tail := AuxKeysFrom(lo + 1, hi, m);
      AuxKeysFrom_strict_sorted(lo + 1, hi, m);
      if |tail| > 0 {
        AuxKeysFrom_first_ge(lo + 1, hi, m);
        assert lo < tail[0];
      }
      forall i | 0 <= i < |AuxKeysFrom(lo, hi, m)| - 1
        ensures AuxKeysFrom(lo, hi, m)[i] < AuxKeysFrom(lo, hi, m)[i + 1]
      {
        if i == 0 {
          if |tail| > 0 {
            assert AuxKeysFrom(lo, hi, m)[1] == tail[0];
          }
        } else {
          assert AuxKeysFrom(lo, hi, m)[i] == tail[i - 1];
          assert AuxKeysFrom(lo, hi, m)[i + 1] == tail[i];
        }
      }
    } else {
      AuxKeysFrom_strict_sorted(lo + 1, hi, m);
    }
  }

  lemma AuxKeysFrom_domain_eq(lo: int, hi: int, m: KeyStore, sm: Spec.KeyStore)
    requires forall k :: lo <= k < hi ==> (k in m <==> k in sm)
    ensures AuxKeysFrom(lo, hi, m) == Spec.KeysFrom(lo, hi, sm)
    decreases hi - lo
  {
    if lo >= hi {
    } else {
      AuxKeysFrom_domain_eq(lo + 1, hi, m, sm);
    }
  }

  ghost function AuxFinalizeKeys(keys: seq<int>, m: KeyStore): seq<F.OutRow>
    decreases |keys|
  {
    if |keys| == 0 then
      []
    else if keys[0] in m && m[keys[0]].count > 0 then
      [ExecGroup(keys[0], m[keys[0]])] + AuxFinalizeKeys(keys[1..], m)
    else
      AuxFinalizeKeys(keys[1..], m)
  }

  lemma Compose_keys_ok(db: seq<Row>, cutoff: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures forall g :: g in Compose(db, cutoff) ==>
              0 <= g < 65536 && Compose(db, cutoff)[g].count > 0
  {
    Compose_positive_counts(db, cutoff);
    FoldByGroup_keys_pack(FilterCutoff(db, cutoff));
  }

  lemma FoldByGroup_keys_pack(t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall g :: g in FoldByGroup(t) ==> 0 <= g < 65536
    decreases |t|
  {
    if |t| == 0 {
    } else {
      FoldByGroup_keys_pack(t[..|t| - 1]);
    }
  }

  lemma AuxKeysFrom_Finalize_pre(lo: int, hi: int, m: KeyStore)
    requires forall g :: g in m ==> m[g].count > 0
    ensures forall i :: 0 <= i < |AuxKeysFrom(lo, hi, m)| ==>
              AuxKeysFrom(lo, hi, m)[i] in m && m[AuxKeysFrom(lo, hi, m)[i]].count > 0
  {
    AuxKeysFrom_all_in_m(lo, hi, m);
  }

  lemma AuxFinalizeKeys_len(keys: seq<int>, m: KeyStore)
    requires forall i :: 0 <= i < |keys| ==> keys[i] in m && m[keys[i]].count > 0
    ensures |AuxFinalizeKeys(keys, m)| == |keys|
    ensures forall i :: 0 <= i < |keys| ==>
              AuxFinalizeKeys(keys, m)[i].group_key == keys[i] &&
              AuxFinalizeKeys(keys, m)[i] == ExecGroup(keys[i], m[keys[i]])
    decreases |keys|
  {
    if |keys| == 0 {
    } else {
      AuxFinalizeKeys_len(keys[1..], m);
    }
  }

  // Model ExecSeq^M(A,θ) under Inv: finalize Compose(D,cutoff) in Pack order.
  ghost function ExecSeqM(db: seq<Row>, cutoff: int): seq<F.OutRow>
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
  {
    AuxFinalizeKeys(AuxKeysFrom(0, 65536, Compose(db, cutoff)), Compose(db, cutoff))
  }

  lemma ExecSeqM_strict_sorted(db: seq<Row>, cutoff: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures StrictSortedOut(ExecSeqM(db, cutoff))
  {
    Compose_keys_ok(db, cutoff);
    var m := Compose(db, cutoff);
    var keys := AuxKeysFrom(0, 65536, m);
    AuxKeysFrom_strict_sorted(0, 65536, m);
    AuxKeysFrom_Finalize_pre(0, 65536, m);
    AuxFinalizeKeys_len(keys, m);
    forall i | 0 <= i < |ExecSeqM(db, cutoff)| - 1
      ensures ExecSeqM(db, cutoff)[i].group_key < ExecSeqM(db, cutoff)[i + 1].group_key
    {
      assert ExecSeqM(db, cutoff)[i].group_key == keys[i];
      assert ExecSeqM(db, cutoff)[i + 1].group_key == keys[i + 1];
    }
  }

  lemma ExecSeqM_HasKey(db: seq<Row>, cutoff: int, k: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures HasOutKey(ExecSeqM(db, cutoff), k) <==> (k in Compose(db, cutoff))
  {
    Compose_keys_ok(db, cutoff);
    var m := Compose(db, cutoff);
    var keys := AuxKeysFrom(0, 65536, m);
    AuxKeysFrom_Finalize_pre(0, 65536, m);
    AuxFinalizeKeys_len(keys, m);
    assert ExecSeqM(db, cutoff) == AuxFinalizeKeys(keys, m);
    if k in m {
      assert 0 <= k < 65536;
      AuxKeysFrom_contains_fwd(0, 65536, m, k);
      var i :| 0 <= i < |keys| && keys[i] == k;
      assert AuxFinalizeKeys(keys, m)[i].group_key == k;
    } else {
      forall i | 0 <= i < |AuxFinalizeKeys(keys, m)|
        ensures AuxFinalizeKeys(keys, m)[i].group_key != k
      {
        assert AuxFinalizeKeys(keys, m)[i].group_key == keys[i];
        AuxKeysFrom_all_in_m(0, 65536, m);
        assert keys[i] in m;
      }
    }
  }

  lemma ExecSeqM_row_at_key(db: seq<Row>, cutoff: int, i: nat)
    requires forall i0 :: 0 <= i0 < |db| ==> 0 <= db[i0].rf < 256 && 0 <= db[i0].ls < 256
    requires 0 <= i < |ExecSeqM(db, cutoff)|
    ensures ExecSeqM(db, cutoff)[i].group_key in Compose(db, cutoff)
    ensures Compose(db, cutoff)[ExecSeqM(db, cutoff)[i].group_key].count > 0
    ensures ExecSeqM(db, cutoff)[i]
         == ExecM_group(db, cutoff, ExecSeqM(db, cutoff)[i].group_key)
  {
    Compose_keys_ok(db, cutoff);
    var m := Compose(db, cutoff);
    var keys := AuxKeysFrom(0, 65536, m);
    AuxKeysFrom_Finalize_pre(0, 65536, m);
    AuxFinalizeKeys_len(keys, m);
    assert ExecSeqM(db, cutoff)[i].group_key == keys[i];
    assert keys[i] in m;
  }

  lemma SpecEval_row_at_key(db: seq<Row>, cutoff: int, j: nat)
    requires forall i0 :: 0 <= i0 < |db| ==> 0 <= db[i0].rf < 256 && 0 <= db[i0].ls < 256
    requires 0 <= j < |Spec.EvalResultSpec(AuxRowsToSpec(db), cutoff)|
    ensures Spec.EvalResultSpec(AuxRowsToSpec(db), cutoff)[j].group_key
              in Spec.SpecView(AuxRowsToSpec(db), cutoff)
    ensures Spec.SpecView(AuxRowsToSpec(db), cutoff)
              [Spec.EvalResultSpec(AuxRowsToSpec(db), cutoff)[j].group_key].count > 0
    ensures Spec.EvalResultSpec(AuxRowsToSpec(db), cutoff)[j]
         == EvalSpec_group(db, cutoff,
              Spec.EvalResultSpec(AuxRowsToSpec(db), cutoff)[j].group_key)
  {
    Spec.SpecView_keys_ok(AuxRowsToSpec(db), cutoff);
    var m := Spec.SpecView(AuxRowsToSpec(db), cutoff);
    var keys := Spec.KeysFrom(0, 65536, m);
    Spec.KeysFrom_Finalize_pre(0, 65536, m);
    Spec.FinalizeKeys_len(keys, m);
    var g := Spec.EvalResultSpec(AuxRowsToSpec(db), cutoff)[j].group_key;
    assert g == keys[j];
    assert keys[j] in m;
    assert Spec.EvalResultSpec(AuxRowsToSpec(db), cutoff)[j]
        == Spec.SpecFinalizeOne(keys[j], m[keys[j]])[0];
  }

  // Full sequence-level theorem (ORDERING_PROVED gate).
  // Public target: RelNode Eval_Spec ∘ Q1Adapt (Eval_Spec_Q1_OutRows_rel).
  // Flat Eval_Spec_Q1_OutRows / EvalResultSpec appear only as migration rewrites.
  lemma Q1ExecCorrectOrdered(store: Store, db: seq<Row>, delta: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    requires Inv(store, db)
    ensures ApproxQ1(
              ExecSeqM(db, Spec.Cutoff(delta)),
              RelSpec.Eval_Spec_Q1_OutRows_rel(
                Spec.Cutoff(delta),
                AuxRowsToSpec(db)))
  {
    var cutoff := Spec.Cutoff(delta);
    var accel := ExecSeqM(db, cutoff);
    var spec_db := AuxRowsToSpec(db);
    var legacy := Spec.EvalResultSpec(spec_db, cutoff);
    var ir := RelSpec.Eval_Spec_Q1_OutRows_rel(cutoff, spec_db);

    Q1ExecCorrect(store, db, delta);
    Compose_eq_SpecView(db, cutoff);
    Compose_keys_ok(db, cutoff);
    Spec.SpecView_keys_ok(spec_db, cutoff);

    // Same ascending Pack-key lists on both sides
    AuxKeysFrom_domain_eq(0, 65536, Compose(db, cutoff),
                          Spec.SpecView(spec_db, cutoff));

    ExecSeqM_strict_sorted(db, cutoff);
    Spec.EvalResultSpec_sorted(spec_db, cutoff);

    forall k
      ensures HasOutKey(accel, k) <==> HasSpecKey(legacy, k)
    {
      ExecSeqM_HasKey(db, cutoff, k);
      Spec.EvalResultSpec_HasKey(spec_db, cutoff, k);
    }

    forall i, j | 0 <= i < |accel| && 0 <= j < |legacy| &&
                  accel[i].group_key == legacy[j].group_key
      ensures ApproxQ1Row(accel[i], legacy[j])
    {
      var g := accel[i].group_key;
      ExecSeqM_row_at_key(db, cutoff, i);
      SpecEval_row_at_key(db, cutoff, j);
      assert ApproxQ1Row(ExecM_group(db, cutoff, g), EvalSpec_group(db, cutoff, g));
      assert accel[i] == ExecM_group(db, cutoff, g);
      assert legacy[j] == EvalSpec_group(db, cutoff, g);
    }

    ApproxQ1_of_strict_sorted_same_keys(accel, legacy);
    // RelNode OutRows_rel == EvalResultSpec (via Q1RelNodeBridge)
    RelBridge.Eval_Spec_Q1_OutRows_rel_eq_EvalResultSpec(cutoff, spec_db);
    assert ir == legacy;
  }
}

