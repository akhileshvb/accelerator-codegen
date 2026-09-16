// Q1 aggregate algebra — derived Fold/Inv/Compose lemmas.
//
// Apply/Merge equations match Q1Finalize structural twins and
// src/generated/tpch/q1/postgres/aux/finalize.cpp.
// Prefer Q1Finalize.dfy as the artifact-facing twin; this module keeps the
// inductive Fold/Inv/Compose development used by early POC lemmas.
//
// Model uses unbounded Dafny `int`. C++ __int128 overflow is outside this
// model; conformance uses TPC-H-ish bounds so observed C++ matches.

module Q1Aux {

  datatype Stats = Stats(
    count: int,
    sum_qty: int,
    sum_base_price: int,
    sum_disc_price: int,
    sum_charge: int,
    sum_discount: int
  )

  ghost function EmptyStats(): Stats {
    Stats(0, 0, 0, 0, 0, 0)
  }

  // Mirrors ApplyQ1RowContribution (finalize.cpp).
  ghost function Apply(
    s: Stats,
    qty: int,
    price: int,
    disc: int,
    tax: int,
    sign: int
  ): Stats {
    var disc_price := price * (100 - disc);
    Stats(
      s.count + sign,
      s.sum_qty + sign * qty,
      s.sum_base_price + sign * price,
      s.sum_disc_price + sign * disc_price,
      s.sum_charge + sign * disc_price * (100 + tax),
      s.sum_discount + sign * disc
    )
  }

  // Mirrors MergeQ1DayGroupStats (day stats into merged accumulator).
  ghost function Merge(a: Stats, b: Stats): Stats {
    Stats(
      a.count + b.count,
      a.sum_qty + b.sum_qty,
      a.sum_base_price + b.sum_base_price,
      a.sum_disc_price + b.sum_disc_price,
      a.sum_charge + b.sum_charge,
      a.sum_discount + b.sum_discount
    )
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

  // PackQ1GroupKey: (rf << 8) | ls with rf, ls as byte codes.
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

  datatype CellKey = CellKey(day: int, gkey: int)

  type Store = map<CellKey, Stats>

  ghost function Get(store: Store, k: CellKey): Stats {
    if k in store then store[k] else EmptyStats()
  }

  ghost function ApplyRow(s: Stats, r: Row, sign: int): Stats
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
  {
    Apply(s, r.qty, r.price, r.disc, r.tax, sign)
  }

  // Matches StageMaintenance ApplyDelta erase-on-zero (module.cpp:~449).
  ghost function ApplyDelta(store: Store, r: Row, sign: int): Store
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
  {
    var k := CellKey(r.shipdate, RowKey(r));
    var s2 := ApplyRow(Get(store, k), r, sign);
    if s2.count == 0 then
      store - {k}
    else
      store[k := s2]
  }

  ghost function ApplyDeltaSeq(store: Store, rows: seq<Row>, sign: int): Store
    requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].rf < 256 && 0 <= rows[i].ls < 256
    decreases |rows|
  {
    if |rows| == 0 then
      store
    else
      ApplyDeltaSeq(ApplyDelta(store, rows[0], sign), rows[1..], sign)
  }

  ghost function Fold(t: seq<Row>): Store
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  {
    if |t| == 0 then
      map[]
    else
      ApplyDelta(Fold(t[..|t| - 1]), t[|t| - 1], 1)
  }

  ghost predicate Inv(store: Store, t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  {
    store == Fold(t)
  }

  // ----- Group-key fold after cutoff filter (Compose / Execute residual) -----

  ghost function FilterCutoff(t: seq<Row>, cutoff: int): seq<Row>
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures forall i :: 0 <= i < |FilterCutoff(t, cutoff)| ==>
              0 <= FilterCutoff(t, cutoff)[i].rf < 256 &&
              0 <= FilterCutoff(t, cutoff)[i].ls < 256
    ensures forall i :: 0 <= i < |FilterCutoff(t, cutoff)| ==>
              FilterCutoff(t, cutoff)[i].shipdate <= cutoff
  {
    if |t| == 0 then
      []
    else if t[|t| - 1].shipdate <= cutoff then
      FilterCutoff(t[..|t| - 1], cutoff) + [t[|t| - 1]]
    else
      FilterCutoff(t[..|t| - 1], cutoff)
  }

  type KeyStore = map<int, Stats>

  ghost function GetKey(m: KeyStore, k: int): Stats {
    if k in m then m[k] else EmptyStats()
  }

  ghost function ApplyDeltaKey(m: KeyStore, r: Row, sign: int): KeyStore
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
  {
    var k := RowKey(r);
    m[k := ApplyRow(GetKey(m, k), r, sign)]
  }

  ghost function FoldByGroup(t: seq<Row>): KeyStore
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  {
    if |t| == 0 then
      map[]
    else
      ApplyDeltaKey(FoldByGroup(t[..|t| - 1]), t[|t| - 1], 1)
  }

  // Compose at the model level: aggregate rows with shipdate <= cutoff by group key.
  ghost function Compose(t: seq<Row>, cutoff: int): KeyStore
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  {
    FoldByGroup(FilterCutoff(t, cutoff))
  }

  // ======================== Lemmas ========================

  lemma ApplyCommutative(
    s: Stats,
    qty1: int, price1: int, disc1: int, tax1: int,
    qty2: int, price2: int, disc2: int, tax2: int
  )
    ensures Apply(Apply(s, qty1, price1, disc1, tax1, 1), qty2, price2, disc2, tax2, 1)
         == Apply(Apply(s, qty2, price2, disc2, tax2, 1), qty1, price1, disc1, tax1, 1)
  {
  }

  // Componentwise signed updates commute for any signs.
  lemma ApplyCommuteAnySign(
    s: Stats,
    qty1: int, price1: int, disc1: int, tax1: int, sign1: int,
    qty2: int, price2: int, disc2: int, tax2: int, sign2: int
  )
    ensures Apply(Apply(s, qty1, price1, disc1, tax1, sign1), qty2, price2, disc2, tax2, sign2)
         == Apply(Apply(s, qty2, price2, disc2, tax2, sign2), qty1, price1, disc1, tax1, sign1)
  {
  }

  lemma ApplyCancel(s: Stats, qty: int, price: int, disc: int, tax: int)
    ensures Apply(Apply(s, qty, price, disc, tax, 1), qty, price, disc, tax, -1) == s
  {
  }

  lemma MergeCommutative(a: Stats, b: Stats)
    ensures Merge(a, b) == Merge(b, a)
  {
  }

  lemma MergeAssociative(a: Stats, b: Stats, c: Stats)
    ensures Merge(Merge(a, b), c) == Merge(a, Merge(b, c))
  {
  }

  lemma FoldEmpty()
    ensures Fold([]) == map[]
  {
  }

  lemma FoldStep(t: seq<Row>, r: Row)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures Fold(t + [r]) == ApplyDelta(Fold(t), r, 1)
  {
    // By definition Fold(t+[r]) == ApplyDelta(Fold((t+[r])[..|t|]), r, 1)
    // and (t+[r])[..|t|] == t.
    assert (t + [r])[..|t| + 1 - 1] == t;
    assert Fold(t + [r]) == ApplyDelta(Fold(t), r, 1);
  }

  lemma InvPreservedByInsert(store: Store, t: seq<Row>, r: Row)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires Inv(store, t)
    ensures Inv(ApplyDelta(store, r, 1), t + [r])
  {
    FoldStep(t, r);
  }

  // Erase-on-zero cancel. Precondition: cell absent or strictly positive count
  // (Fold images satisfy this — only +1 contributions).
  lemma ApplyDeltaCancel(store: Store, r: Row)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires var k := CellKey(r.shipdate, RowKey(r));
             k !in store || store[k].count > 0
    ensures ApplyDelta(ApplyDelta(store, r, 1), r, -1) == store
  {
    var k := CellKey(r.shipdate, RowKey(r));
    var s0 := Get(store, k);
    ApplyCancel(s0, r.qty, r.price, r.disc, r.tax);
    var s1 := ApplyRow(s0, r, 1);
    assert s1.count == s0.count + 1;
    assert ApplyRow(s1, r, -1) == s0;
    if k !in store {
      assert s0 == EmptyStats();
      assert s0.count == 0;
      assert s1.count == 1;
      assert ApplyDelta(store, r, 1) == store[k := s1];
      var mid := store[k := s1];
      assert Get(mid, k) == s1;
      assert ApplyRow(Get(mid, k), r, -1) == s0;
      assert ApplyDelta(mid, r, -1) == mid - {k};
      assert mid - {k} == store;
    } else {
      assert store[k].count > 0;
      assert s0.count > 0;
      assert s1.count > 1;
      assert ApplyDelta(store, r, 1) == store[k := s1];
      var mid := store[k := s1];
      assert ApplyDelta(mid, r, -1) == mid[k := s0];
      assert mid[k := s0] == store;
    }
  }

  lemma FoldHasNoEmptyCell(t: seq<Row>, k: CellKey)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures k !in Fold(t) || Fold(t)[k].count > 0
    decreases |t|
  {
    if |t| == 0 {
    } else {
      var r := t[|t| - 1];
      var k2 := CellKey(r.shipdate, RowKey(r));
      var prev := Fold(t[..|t| - 1]);
      FoldHasNoEmptyCell(t[..|t| - 1], k);
      FoldHasNoEmptyCell(t[..|t| - 1], k2);
      var s2 := ApplyRow(Get(prev, k2), r, 1);
      assert s2.count == Get(prev, k2).count + 1;
      if k2 !in prev {
        assert Get(prev, k2) == EmptyStats();
        assert s2.count == 1;
      } else {
        assert prev[k2].count > 0;
        assert s2.count > 1;
      }
      assert s2.count > 0;
      assert ApplyDelta(prev, r, 1) == prev[k2 := s2];
      assert Fold(t) == ApplyDelta(prev, r, 1);
    }
  }

  lemma InvPreservedByDeleteLast(store: Store, t: seq<Row>, r: Row)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires Inv(store, t + [r])
    ensures Inv(ApplyDelta(store, r, -1), t)
  {
    FoldStep(t, r);
    FoldHasNoEmptyCell(t, CellKey(r.shipdate, RowKey(r)));
    assert store == ApplyDelta(Fold(t), r, 1);
    ApplyDeltaCancel(Fold(t), r);
  }

  lemma InvPreservedByInsertThenDelete(store: Store, t: seq<Row>, r: Row)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires Inv(store, t)
    ensures Inv(ApplyDelta(ApplyDelta(store, r, 1), r, -1), t)
  {
    FoldHasNoEmptyCell(t, CellKey(r.shipdate, RowKey(r)));
    ApplyDeltaCancel(store, r);
  }

  lemma RebuildEstablishesInv(t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures Inv(Fold(t), t)
  {
  }

  lemma NoOpPreservesInv(store: Store, t: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires Inv(store, t)
    ensures Inv(store, t)
  {
  }

  lemma ApplyDeltaSeqInsert(store: Store, t: seq<Row>, rows: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].rf < 256 && 0 <= rows[i].ls < 256
    requires Inv(store, t)
    ensures Inv(ApplyDeltaSeq(store, rows, 1), t + rows)
    decreases |rows|
  {
    if |rows| == 0 {
      assert t + [] == t;
    } else {
      InvPreservedByInsert(store, t, rows[0]);
      ApplyDeltaSeqInsert(ApplyDelta(store, rows[0], 1), t + [rows[0]], rows[1..]);
      assert (t + [rows[0]]) + rows[1..] == t + rows;
    }
  }

  // Undo a suffix: apply -1 to the last row first (inverse of Fold append order).
  ghost function ApplyDeltaSeqRev(store: Store, rows: seq<Row>, sign: int): Store
    requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].rf < 256 && 0 <= rows[i].ls < 256
    decreases |rows|
  {
    if |rows| == 0 then
      store
    else
      ApplyDeltaSeqRev(ApplyDelta(store, rows[|rows| - 1], sign), rows[..|rows| - 1], sign)
  }

  lemma ApplyDeltaSeqRevDeleteSuffix(store: Store, t: seq<Row>, rows: seq<Row>)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires forall i :: 0 <= i < |rows| ==> 0 <= rows[i].rf < 256 && 0 <= rows[i].ls < 256
    requires Inv(store, t + rows)
    ensures Inv(ApplyDeltaSeqRev(store, rows, -1), t)
    decreases |rows|
  {
    if |rows| == 0 {
      assert t + [] == t;
    } else {
      var rows1 := rows[..|rows| - 1];
      var r := rows[|rows| - 1];
      assert t + rows == (t + rows1) + [r];
      InvPreservedByDeleteLast(store, t + rows1, r);
      ApplyDeltaSeqRevDeleteSuffix(ApplyDelta(store, r, -1), t, rows1);
    }
  }

  // ----- Fold commutativity → delete at arbitrary index -----
  // Characterize Fold per cell; RemoveAt subtracts that row's contribution.

  ghost predicate RowsOk(t: seq<Row>) {
    forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
  }

  ghost function RemoveAt(t: seq<Row>, i: nat): seq<Row>
    requires i < |t|
  {
    t[..i] + t[i + 1..]
  }

  ghost function Contrib(t: seq<Row>, k: CellKey): Stats
    requires RowsOk(t)
    decreases |t|
  {
    if |t| == 0 then
      EmptyStats()
    else
      var prev := Contrib(t[..|t| - 1], k);
      var r := t[|t| - 1];
      if CellKey(r.shipdate, RowKey(r)) == k then
        ApplyRow(prev, r, 1)
      else
        prev
  }

  lemma ContribCountNonNeg(t: seq<Row>, k: CellKey)
    requires RowsOk(t)
    ensures Contrib(t, k).count >= 0
    decreases |t|
  {
    if |t| == 0 {
    } else {
      ContribCountNonNeg(t[..|t| - 1], k);
      var r := t[|t| - 1];
      if CellKey(r.shipdate, RowKey(r)) == k {
        assert Contrib(t, k).count == Contrib(t[..|t| - 1], k).count + 1;
      }
    }
  }

  lemma FoldGetEqualsContrib(t: seq<Row>, k: CellKey)
    requires RowsOk(t)
    ensures Get(Fold(t), k) == Contrib(t, k)
    ensures (k in Fold(t) <==> Contrib(t, k).count > 0)
    ensures k in Fold(t) ==> Fold(t)[k] == Contrib(t, k)
    decreases |t|
  {
    if |t| == 0 {
    } else {
      var r := t[|t| - 1];
      var k2 := CellKey(r.shipdate, RowKey(r));
      var prev := Fold(t[..|t| - 1]);
      FoldGetEqualsContrib(t[..|t| - 1], k);
      FoldGetEqualsContrib(t[..|t| - 1], k2);
      ContribCountNonNeg(t[..|t| - 1], k);
      ContribCountNonNeg(t[..|t| - 1], k2);
      var s2 := ApplyRow(Get(prev, k2), r, 1);
      assert s2.count == Get(prev, k2).count + 1;
      assert s2.count > 0;
      assert Fold(t) == prev[k2 := s2];
      if k == k2 {
        assert Contrib(t, k) == ApplyRow(Contrib(t[..|t| - 1], k), r, 1);
        assert Get(Fold(t), k) == s2;
        assert s2 == ApplyRow(Get(prev, k2), r, 1);
        assert Get(prev, k2) == Contrib(t[..|t| - 1], k);
      } else {
        assert Contrib(t, k) == Contrib(t[..|t| - 1], k);
        assert Get(Fold(t), k) == Get(prev, k);
      }
    }
  }

  lemma FoldExtensional(t1: seq<Row>, t2: seq<Row>)
    requires RowsOk(t1) && RowsOk(t2)
    requires forall k: CellKey :: Contrib(t1, k) == Contrib(t2, k)
    ensures Fold(t1) == Fold(t2)
  {
    // Same non-empty cells and payloads ⇒ same map
    forall k: CellKey
      ensures (k in Fold(t1) <==> k in Fold(t2)) &&
              (k in Fold(t1) ==> Fold(t1)[k] == Fold(t2)[k])
    {
      FoldGetEqualsContrib(t1, k);
      FoldGetEqualsContrib(t2, k);
    }
    assert Fold(t1).Keys == Fold(t2).Keys;
    assert forall k :: k in Fold(t1) ==> Fold(t1)[k] == Fold(t2)[k];
  }

  lemma ContribRemoveAt(t: seq<Row>, i: nat, k: CellKey)
    requires RowsOk(t) && i < |t|
    ensures Contrib(RemoveAt(t, i), k)
         == (if CellKey(t[i].shipdate, RowKey(t[i])) == k
             then ApplyRow(Contrib(t, k), t[i], -1)
             else Contrib(t, k))
    decreases |t|
  {
    var r := t[i];
    var kr := CellKey(r.shipdate, RowKey(r));
    if |t| == 1 {
      assert i == 0;
      assert RemoveAt(t, 0) == [];
      if kr == k {
        assert Contrib(t, k) == ApplyRow(EmptyStats(), r, 1);
        ApplyCancel(EmptyStats(), r.qty, r.price, r.disc, r.tax);
      } else {
        assert Contrib(t, k) == EmptyStats();
      }
    } else if i == |t| - 1 {
      assert RemoveAt(t, i) == t[..|t| - 1];
      if kr == k {
        assert Contrib(t, k) == ApplyRow(Contrib(t[..|t| - 1], k), r, 1);
        ApplyCancel(Contrib(t[..|t| - 1], k), r.qty, r.price, r.disc, r.tax);
      } else {
        assert Contrib(t, k) == Contrib(t[..|t| - 1], k);
      }
    } else {
      var last := t[|t| - 1];
      var pref := t[..|t| - 1];
      var klast := CellKey(last.shipdate, RowKey(last));
      assert RemoveAt(t, i) == RemoveAt(pref, i) + [last];
      assert RowsOk(pref) && RowsOk(RemoveAt(pref, i));
      ContribRemoveAt(pref, i, k);
      if klast == k {
        assert Contrib(t, k) == ApplyRow(Contrib(pref, k), last, 1);
        assert Contrib(RemoveAt(t, i), k)
            == ApplyRow(Contrib(RemoveAt(pref, i), k), last, 1);
        if kr == k {
          assert Contrib(RemoveAt(pref, i), k) == ApplyRow(Contrib(pref, k), r, -1);
          ApplyCommuteAnySign(Contrib(pref, k),
            last.qty, last.price, last.disc, last.tax, 1,
            r.qty, r.price, r.disc, r.tax, -1);
          assert ApplyRow(ApplyRow(Contrib(pref, k), last, 1), r, -1)
              == ApplyRow(ApplyRow(Contrib(pref, k), r, -1), last, 1);
        } else {
          assert Contrib(RemoveAt(pref, i), k) == Contrib(pref, k);
        }
      } else {
        assert Contrib(t, k) == Contrib(pref, k);
        assert Contrib(RemoveAt(t, i), k) == Contrib(RemoveAt(pref, i), k);
        if kr == k {
          assert Contrib(RemoveAt(pref, i), k) == ApplyRow(Contrib(pref, k), r, -1);
        } else {
          assert Contrib(RemoveAt(pref, i), k) == Contrib(pref, k);
        }
      }
    }
  }

  lemma FoldFromContrib(t: seq<Row>)
    requires RowsOk(t)
    ensures forall k :: Get(Fold(t), k) == Contrib(t, k)
    ensures forall k :: k in Fold(t) <==> Contrib(t, k).count > 0
  {
    forall k: CellKey
      ensures Get(Fold(t), k) == Contrib(t, k) &&
              (k in Fold(t) <==> Contrib(t, k).count > 0)
    {
      FoldGetEqualsContrib(t, k);
    }
  }

  lemma InvPreservedByDeleteAt(store: Store, t: seq<Row>, i: nat)
    requires RowsOk(t)
    requires i < |t|
    requires Inv(store, t)
    ensures Inv(ApplyDelta(store, t[i], -1), RemoveAt(t, i))
  {
    var r := t[i];
    var kr := CellKey(r.shipdate, RowKey(r));
    FoldFromContrib(t);
    FoldFromContrib(RemoveAt(t, i));
    FoldHasNoEmptyCell(t, kr);
    ContribRemoveAt(t, i, kr);
    ContribCountNonNeg(t, kr);
    // store == Fold(t); after -1 on r, Get matches Contrib(RemoveAt)
    forall k: CellKey
      ensures Get(ApplyDelta(store, r, -1), k) == Contrib(RemoveAt(t, i), k) &&
              (k in ApplyDelta(store, r, -1) <==> Contrib(RemoveAt(t, i), k).count > 0)
    {
      ContribRemoveAt(t, i, k);
      FoldGetEqualsContrib(t, k);
      if k == kr {
        var s0 := Get(store, kr);
        assert s0 == Contrib(t, kr);
        assert s0.count > 0;
        var s1 := ApplyRow(s0, r, -1);
        assert s1 == Contrib(RemoveAt(t, i), kr);
        if s1.count == 0 {
          assert ApplyDelta(store, r, -1) == store - {kr};
          assert Get(ApplyDelta(store, r, -1), kr) == EmptyStats();
        } else {
          assert ApplyDelta(store, r, -1) == store[kr := s1];
        }
      } else {
        assert Contrib(RemoveAt(t, i), k) == Contrib(t, k);
        assert Get(ApplyDelta(store, r, -1), k) == Get(store, k);
      }
    }
    FoldExtensionalFromGet(ApplyDelta(store, r, -1), RemoveAt(t, i));
  }

  lemma FoldExtensionalFromGet(store: Store, t: seq<Row>)
    requires RowsOk(t)
    requires forall k: CellKey :: Get(store, k) == Contrib(t, k)
    requires forall k: CellKey :: k in store <==> Contrib(t, k).count > 0
    ensures store == Fold(t)
  {
    FoldFromContrib(t);
    assert forall k :: k in store <==> k in Fold(t);
    assert forall k :: k in store ==> store[k] == Fold(t)[k];
  }

  lemma FilterCutoffEmpty(cutoff: int)
    ensures FilterCutoff([], cutoff) == []
  {
  }

  lemma FilterCutoffStep(t: seq<Row>, r: Row, cutoff: int)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures r.shipdate <= cutoff ==>
              FilterCutoff(t + [r], cutoff) == FilterCutoff(t, cutoff) + [r]
    ensures r.shipdate > cutoff ==>
              FilterCutoff(t + [r], cutoff) == FilterCutoff(t, cutoff)
  {
    assert (t + [r])[..|t|] == t;
  }

  lemma FoldByGroupStep(t: seq<Row>, r: Row)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures FoldByGroup(t + [r]) == ApplyDeltaKey(FoldByGroup(t), r, 1)
  {
    assert (t + [r])[..|t|] == t;
  }

  // Inductive characterization of Compose: appending a row either updates the
  // group aggregate (if inside the cutoff) or leaves Compose unchanged.
  lemma ComposeStep(t: seq<Row>, r: Row, cutoff: int)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures r.shipdate <= cutoff ==>
              Compose(t + [r], cutoff) == ApplyDeltaKey(Compose(t, cutoff), r, 1)
    ensures r.shipdate > cutoff ==>
              Compose(t + [r], cutoff) == Compose(t, cutoff)
  {
    FilterCutoffStep(t, r, cutoff);
    if r.shipdate <= cutoff {
      FoldByGroupStep(FilterCutoff(t, cutoff), r);
      assert FilterCutoff(t + [r], cutoff) == FilterCutoff(t, cutoff) + [r];
      assert Compose(t + [r], cutoff)
          == FoldByGroup(FilterCutoff(t, cutoff) + [r]);
      assert FoldByGroup(FilterCutoff(t, cutoff) + [r])
          == ApplyDeltaKey(FoldByGroup(FilterCutoff(t, cutoff)), r, 1);
    } else {
      assert FilterCutoff(t + [r], cutoff) == FilterCutoff(t, cutoff);
    }
  }

  // Execute ≡ scan-aggregate at the model level: Compose is exactly FoldByGroup
  // of the cutoff-filtered table (definitional), with a stable empty base.
  lemma ComposeEqualsFilteredFold(t: seq<Row>, cutoff: int)
    requires forall i :: 0 <= i < |t| ==> 0 <= t[i].rf < 256 && 0 <= t[i].ls < 256
    ensures Compose(t, cutoff) == FoldByGroup(FilterCutoff(t, cutoff))
  {
  }

  lemma ComposeEmpty(cutoff: int)
    ensures Compose([], cutoff) == map[]
  {
  }

}
