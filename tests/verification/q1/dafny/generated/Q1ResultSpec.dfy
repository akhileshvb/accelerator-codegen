// GENERATED companion for Rocq QueryResultSpec / NormalizeQuery — Q1 PoC.
// SpecExportCorrect(S_Q1, this module) is TCB (hash-checked dual of Rocq Spec).
// source: formalsql/resultspec/QueryResultSpec.v + NormalizeQuery.v
// source_sha256=q1-resultspec-poc-v1
//
// Trusted surface (shrunk):
//   Q1ResultSpecData.dfy  — AST / constants only (dual of Rocq QueryResultSpec.v)
//   this file             — evaluator over that AST (filter/group/agg/order)
// Pin still checks both; semantic TCB is now "data transcription + that this
// evaluator matches Rocq EvalSpec_Q1", not an opaque monolithic dual.

include "Q1ResultSpecData.dfy"

module Q1ResultSpec {

  import D = Q1ResultSpecData

  const BaseDate: int := D.BaseDate

  ghost function Cutoff(delta: int): int {
    D.Cutoff(delta)
  }

  datatype Row = Row(
    shipdate: int,
    qty: int,
    price: int,
    disc: int,
    tax: int,
    rf: int,
    ls: int
  )

  // Aggregate cell after GROUP BY (sums are FormalSQL scaled integers).
  datatype SpecStats = SpecStats(
    count: int,
    sum_qty: int,
    sum_base_price: int,
    sum_disc_price: int,
    sum_charge: int,
    sum_discount: int
  )

  ghost function EmptyStats(): SpecStats {
    SpecStats(0, 0, 0, 0, 0, 0)
  }

  // Contrib matches FormalSQL $f4/$f5 at scale 2/4/6 and Q1 Apply formulas.
  // Still query-specific (not yet a generic AggKind interpreter).
  ghost function SpecApply(s: SpecStats, r: Row, sign: int): SpecStats {
    var disc_price := r.price * (100 - r.disc);
    SpecStats(
      s.count + sign,
      s.sum_qty + sign * r.qty,
      s.sum_base_price + sign * r.price,
      s.sum_disc_price + sign * disc_price,
      s.sum_charge + sign * disc_price * (100 + r.tax),
      s.sum_discount + sign * r.disc
    )
  }

  ghost function Pack(rf: int, ls: int): int
    requires 0 <= rf < 256 && 0 <= ls < 256
  {
    rf * 256 + ls
  }

  ghost function RowKey(r: Row): int
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
  {
    Pack(r.rf, r.ls)
  }

  type KeyStore = map<int, SpecStats>

  ghost function GetKey(m: KeyStore, g: int): SpecStats {
    if g in m then m[g] else EmptyStats()
  }

  ghost function ApplyDeltaKey(m: KeyStore, r: Row, sign: int): KeyStore
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
  {
    var g := RowKey(r);
    m[g := SpecApply(GetKey(m, g), r, sign)]
  }

  ghost function FilterCutoff(t: seq<Row>, cutoff: int): seq<Row>
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall i :: 0 <= i < |FilterCutoff(t, cutoff)| ==>
              0 <= FilterCutoff(t, cutoff)[i].rf < 256 &&
              0 <= FilterCutoff(t, cutoff)[i].ls < 256
  {
    if |t| == 0 then
      []
    else if t[|t| - 1].shipdate <= cutoff then
      FilterCutoff(t[..|t| - 1], cutoff) + [t[|t| - 1]]
    else
      FilterCutoff(t[..|t| - 1], cutoff)
  }

  ghost function FoldByGroup(t: seq<Row>): KeyStore
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  {
    if |t| == 0 then
      map[]
    else
      ApplyDeltaKey(FoldByGroup(t[..|t| - 1]), t[|t| - 1], 1)
  }

  // Spec view: rows with shipdate <= cutoff, grouped by (rf, ls).
  ghost function SpecView(db: seq<Row>, cutoff: int): KeyStore
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
  {
    FoldByGroup(FilterCutoff(db, cutoff))
  }

  // Result row: EXACT columns + AVG numerators (sum_*, count). No float yet.
  datatype SpecOutRow = SpecOutRow(
    group_key: int,
    sum_qty: int,
    sum_base_price: int,
    sum_disc_price: int,
    sum_charge: int,
    sum_discount: int,
    count: int
  )

  ghost function SpecFinalizeOne(gkey: int, s: SpecStats): seq<SpecOutRow> {
    if s.count <= 0 then
      []
    else
      [SpecOutRow(gkey, s.sum_qty, s.sum_base_price, s.sum_disc_price,
                  s.sum_charge, s.sum_discount, s.count)]
  }

  // Deterministic ascending enumeration of keys in [lo, hi) that are in m.
  // Q1 Pack(rf,ls) ∈ [0, 65536); used instead of nondet KeyStoreToEntries+Sort.
  ghost function KeysFrom(lo: int, hi: int, m: KeyStore): seq<int>
    decreases hi - lo
  {
    if lo >= hi then
      []
    else if lo in m then
      [lo] + KeysFrom(lo + 1, hi, m)
    else
      KeysFrom(lo + 1, hi, m)
  }

  ghost function FinalizeKeys(keys: seq<int>, m: KeyStore): seq<SpecOutRow>
    decreases |keys|
  {
    if |keys| == 0 then
      []
    else
      SpecFinalizeOne(keys[0], GetKey(m, keys[0])) + FinalizeKeys(keys[1..], m)
  }

  ghost function KeyStoreToEntries(m: KeyStore): seq<(int, SpecStats)>
    decreases |m.Keys|
  {
    if m.Keys == {} then
      []
    else
      var g :| g in m.Keys;
      [(g, m[g])] + KeyStoreToEntries(m - {g})
  }

  ghost function CollectFinalize(entries: seq<(int, SpecStats)>): seq<SpecOutRow>
    decreases |entries|
  {
    if |entries| == 0 then
      []
    else
      SpecFinalizeOne(entries[0].0, entries[0].1) + CollectFinalize(entries[1..])
  }

  ghost function InsertSorted(rows: seq<SpecOutRow>, row: SpecOutRow): seq<SpecOutRow>
    decreases |rows|
  {
    if |rows| == 0 then
      [row]
    else if row.group_key < rows[0].group_key then
      [row] + rows
    else
      [rows[0]] + InsertSorted(rows[1..], row)
  }

  ghost function SortByKey(rows: seq<SpecOutRow>): seq<SpecOutRow>
    decreases |rows|
  {
    if |rows| == 0 then
      []
    else
      InsertSorted(SortByKey(rows[1..]), rows[0])
  }

  lemma FoldByGroup_keys_in_pack_range(t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall g :: g in FoldByGroup(t) ==> 0 <= g < 65536
    decreases |t|
  {
    if |t| == 0 {
    } else {
      FoldByGroup_keys_in_pack_range(t[..|t| - 1]);
    }
  }

  lemma FoldByGroup_positive_counts(t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall g :: g in FoldByGroup(t) ==> FoldByGroup(t)[g].count > 0
    decreases |t|
  {
    if |t| == 0 {
    } else {
      FoldByGroup_positive_counts(t[..|t| - 1]);
    }
  }

  lemma SpecView_keys_ok(db: seq<Row>, cutoff: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures forall g :: g in SpecView(db, cutoff) ==>
              0 <= g < 65536 && SpecView(db, cutoff)[g].count > 0
  {
    FoldByGroup_keys_in_pack_range(FilterCutoff(db, cutoff));
    FoldByGroup_positive_counts(FilterCutoff(db, cutoff));
  }

  ghost predicate KeyInKeysFrom(lo: int, hi: int, m: KeyStore, k: int) {
    exists i :: 0 <= i < |KeysFrom(lo, hi, m)| && KeysFrom(lo, hi, m)[i] == k
  }

  lemma KeysFrom_all_in_m(lo: int, hi: int, m: KeyStore)
    ensures forall i :: 0 <= i < |KeysFrom(lo, hi, m)| ==>
              lo <= KeysFrom(lo, hi, m)[i] < hi &&
              KeysFrom(lo, hi, m)[i] in m
    decreases hi - lo
  {
    if lo >= hi {
    } else if lo in m {
      KeysFrom_all_in_m(lo + 1, hi, m);
    } else {
      KeysFrom_all_in_m(lo + 1, hi, m);
    }
  }

  lemma KeysFrom_contains_fwd(lo: int, hi: int, m: KeyStore, k: int)
    requires lo <= k < hi && k in m
    ensures KeyInKeysFrom(lo, hi, m, k)
    decreases hi - lo
  {
    if lo >= hi {
    } else if lo in m {
      if k == lo {
        assert KeysFrom(lo, hi, m)[0] == lo;
      } else {
        assert lo + 1 <= k < hi;
        KeysFrom_contains_fwd(lo + 1, hi, m, k);
        var j :| 0 <= j < |KeysFrom(lo + 1, hi, m)| && KeysFrom(lo + 1, hi, m)[j] == k;
        assert KeysFrom(lo, hi, m) == [lo] + KeysFrom(lo + 1, hi, m);
        assert KeysFrom(lo, hi, m)[j + 1] == k;
      }
    } else {
      assert k != lo;
      assert lo + 1 <= k < hi;
      KeysFrom_contains_fwd(lo + 1, hi, m, k);
      assert KeysFrom(lo, hi, m) == KeysFrom(lo + 1, hi, m);
    }
  }

  lemma KeysFrom_contains_bwd(lo: int, hi: int, m: KeyStore, k: int)
    requires KeyInKeysFrom(lo, hi, m, k)
    ensures lo <= k < hi && k in m
    decreases hi - lo
  {
    KeysFrom_all_in_m(lo, hi, m);
    var i :| 0 <= i < |KeysFrom(lo, hi, m)| && KeysFrom(lo, hi, m)[i] == k;
  }

  lemma KeysFrom_membership(lo: int, hi: int, m: KeyStore, k: int)
    ensures KeyInKeysFrom(lo, hi, m, k) <==> (lo <= k < hi && k in m)
  {
    if lo <= k < hi && k in m {
      KeysFrom_contains_fwd(lo, hi, m, k);
    }
    if KeyInKeysFrom(lo, hi, m, k) {
      KeysFrom_contains_bwd(lo, hi, m, k);
    }
  }

  lemma KeysFrom_first_ge(lo: int, hi: int, m: KeyStore)
    requires |KeysFrom(lo, hi, m)| > 0
    ensures KeysFrom(lo, hi, m)[0] >= lo
    decreases hi - lo
  {
    if lo >= hi {
    } else if lo in m {
      assert KeysFrom(lo, hi, m)[0] == lo;
    } else {
      KeysFrom_first_ge(lo + 1, hi, m);
    }
  }

  lemma KeysFrom_strict_sorted(lo: int, hi: int, m: KeyStore)
    ensures forall i :: 0 <= i < |KeysFrom(lo, hi, m)| - 1 ==>
              KeysFrom(lo, hi, m)[i] < KeysFrom(lo, hi, m)[i + 1]
    decreases hi - lo
  {
    if lo >= hi {
    } else if lo in m {
      var tail := KeysFrom(lo + 1, hi, m);
      KeysFrom_strict_sorted(lo + 1, hi, m);
      if |tail| > 0 {
        KeysFrom_first_ge(lo + 1, hi, m);
        assert KeysFrom(lo, hi, m)[0] == lo;
        assert KeysFrom(lo, hi, m)[1] == tail[0];
        assert lo < tail[0];
      }
      forall i | 0 <= i < |KeysFrom(lo, hi, m)| - 1
        ensures KeysFrom(lo, hi, m)[i] < KeysFrom(lo, hi, m)[i + 1]
      {
        if i == 0 {
          if |tail| > 0 {
            assert KeysFrom(lo, hi, m)[1] == tail[0];
            assert lo < tail[0];
          }
        } else {
          assert KeysFrom(lo, hi, m)[i] == tail[i - 1];
          assert KeysFrom(lo, hi, m)[i + 1] == tail[i];
        }
      }
    } else {
      KeysFrom_strict_sorted(lo + 1, hi, m);
    }
  }

  lemma KeysFrom_domain_eq(lo: int, hi: int, m1: KeyStore, m2: KeyStore)
    requires forall k :: lo <= k < hi ==> (k in m1 <==> k in m2)
    ensures KeysFrom(lo, hi, m1) == KeysFrom(lo, hi, m2)
    decreases hi - lo
  {
    if lo >= hi {
    } else {
      KeysFrom_domain_eq(lo + 1, hi, m1, m2);
    }
  }

  lemma KeysFrom_Finalize_pre(lo: int, hi: int, m: KeyStore)
    requires forall g :: g in m ==> m[g].count > 0
    ensures forall i :: 0 <= i < |KeysFrom(lo, hi, m)| ==>
              KeysFrom(lo, hi, m)[i] in m && m[KeysFrom(lo, hi, m)[i]].count > 0
  {
    KeysFrom_all_in_m(lo, hi, m);
  }

  lemma FinalizeKeys_len(keys: seq<int>, m: KeyStore)
    requires forall i :: 0 <= i < |keys| ==> keys[i] in m && m[keys[i]].count > 0
    ensures |FinalizeKeys(keys, m)| == |keys|
    ensures forall i :: 0 <= i < |keys| ==>
              FinalizeKeys(keys, m)[i].group_key == keys[i] &&
              FinalizeKeys(keys, m)[i] == SpecFinalizeOne(keys[i], m[keys[i]])[0]
    decreases |keys|
  {
    if |keys| == 0 {
    } else {
      assert SpecFinalizeOne(keys[0], GetKey(m, keys[0]))
          == [SpecOutRow(keys[0], m[keys[0]].sum_qty, m[keys[0]].sum_base_price,
                         m[keys[0]].sum_disc_price, m[keys[0]].sum_charge,
                         m[keys[0]].sum_discount, m[keys[0]].count)];
      FinalizeKeys_len(keys[1..], m);
    }
  }

  // Canonical EvalResultSpec(S_Q1(cutoff), D): ascending Pack keys in SpecView.
  ghost function EvalResultSpec(db: seq<Row>, cutoff: int): seq<SpecOutRow>
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
  {
    FinalizeKeys(KeysFrom(0, 65536, SpecView(db, cutoff)), SpecView(db, cutoff))
  }

  ghost predicate SortedByGroupKey(rows: seq<SpecOutRow>) {
    forall i :: 0 <= i < |rows| - 1 ==> rows[i].group_key <= rows[i + 1].group_key
  }

  ghost predicate StrictSortedByGroupKey(rows: seq<SpecOutRow>) {
    forall i :: 0 <= i < |rows| - 1 ==> rows[i].group_key < rows[i + 1].group_key
  }

  lemma InsertSorted_sorted(rows: seq<SpecOutRow>, row: SpecOutRow)
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

  lemma SortByKey_sorted(rows: seq<SpecOutRow>)
    ensures SortedByGroupKey(SortByKey(rows))
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SortByKey_sorted(rows[1..]);
      InsertSorted_sorted(SortByKey(rows[1..]), rows[0]);
    }
  }

  lemma EvalResultSpec_sorted(db: seq<Row>, cutoff: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures SortedByGroupKey(EvalResultSpec(db, cutoff))
    ensures StrictSortedByGroupKey(EvalResultSpec(db, cutoff))
  {
    SpecView_keys_ok(db, cutoff);
    var m := SpecView(db, cutoff);
    var keys := KeysFrom(0, 65536, m);
    KeysFrom_strict_sorted(0, 65536, m);
    KeysFrom_Finalize_pre(0, 65536, m);
    FinalizeKeys_len(keys, m);
    forall i | 0 <= i < |EvalResultSpec(db, cutoff)| - 1
      ensures EvalResultSpec(db, cutoff)[i].group_key < EvalResultSpec(db, cutoff)[i + 1].group_key
    {
      assert EvalResultSpec(db, cutoff)[i].group_key == keys[i];
      assert EvalResultSpec(db, cutoff)[i + 1].group_key == keys[i + 1];
    }
  }

  lemma EvalResultSpec_HasKey(db: seq<Row>, cutoff: int, k: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures (exists i :: 0 <= i < |EvalResultSpec(db, cutoff)| &&
               EvalResultSpec(db, cutoff)[i].group_key == k) <==>
            (k in SpecView(db, cutoff))
  {
    SpecView_keys_ok(db, cutoff);
    var m := SpecView(db, cutoff);
    var keys := KeysFrom(0, 65536, m);
    KeysFrom_Finalize_pre(0, 65536, m);
    FinalizeKeys_len(keys, m);
    assert EvalResultSpec(db, cutoff) == FinalizeKeys(keys, m);
    if k in m {
      assert 0 <= k < 65536;
      KeysFrom_contains_fwd(0, 65536, m, k);
      var i :| 0 <= i < |keys| && keys[i] == k;
      assert FinalizeKeys(keys, m)[i].group_key == keys[i] == k;
    } else {
      forall i | 0 <= i < |FinalizeKeys(keys, m)|
        ensures FinalizeKeys(keys, m)[i].group_key != k
      {
        assert FinalizeKeys(keys, m)[i].group_key == keys[i];
        KeysFrom_all_in_m(0, 65536, m);
        assert keys[i] in m;
        assert keys[i] != k;
      }
    }
  }
}
