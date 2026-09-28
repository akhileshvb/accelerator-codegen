// Q1RelNodeBridge — Dafny RelNode ↔ flat observational bridge for Q1.
//
// Qed (under byte-range keys, no Assume/Axiom):
//   Eval_Spec_Q1_OutRows_rel(c, db)
//     == R.EvalResultSpec(db, c)
//     == FlatEval.Eval_Spec_Q1_OutRows(ParamsOfCutoff(c), db)
//
// Path: filter → ListStoreRel fold (== FoldByGroup) → StoreToAggBags/Project
// → ResultRowSort → Adapt == FinalizeKeys(KeysFrom(0,65536, SpecView), SpecView).
// RelNode GroupStore is seq<GroupEntry>; keys remain [VInt(rf), VInt(ls)].
// Pack only in Adapt / KeysFrom meeting point. No Assume/Axiom.

include "Q1RelNodeSpec.dfy"
include "EvalLogicalSpec.dfy"

module Q1RelNodeBridge {

  import opened LogicalSpec
  import RelEval = RelEvalLogicalSpec
  import FlatEval = EvalLogicalSpec
  import LSD = LogicalSpecData
  import opened Q1RelNodeSpec
  import R = Q1ResultSpec

  // =====================================================================
  // AccSlot ↔ SpecStats
  // =====================================================================

  ghost function AccSlotOfStats(s: R.SpecStats): RelEval.AggAcc
  {
    [
      RelEval.AccSlot(s.sum_qty, s.count, s.count != 0),
      RelEval.AccSlot(s.sum_base_price, s.count, s.count != 0),
      RelEval.AccSlot(s.sum_disc_price, s.count, s.count != 0),
      RelEval.AccSlot(s.sum_charge, s.count, s.count != 0),
      RelEval.AccSlot(s.sum_qty, s.count, s.count != 0),
      RelEval.AccSlot(s.sum_base_price, s.count, s.count != 0),
      RelEval.AccSlot(s.sum_discount, s.count, s.count != 0),
      RelEval.AccSlot(s.count, s.count, s.count != 0)
    ]
  }

  lemma AccSlotOfStats_Empty()
    ensures AccSlotOfStats(R.EmptyStats()) == RelEval.EmptyAcc(8)
  {
    assert RelEval.EmptyAcc(8) == [
      RelEval.AccSlot(0, 0, false), RelEval.AccSlot(0, 0, false),
      RelEval.AccSlot(0, 0, false), RelEval.AccSlot(0, 0, false),
      RelEval.AccSlot(0, 0, false), RelEval.AccSlot(0, 0, false),
      RelEval.AccSlot(0, 0, false), RelEval.AccSlot(0, 0, false)
    ];
  }

  ghost function KeysOfRow(r: R.Row): seq<SqlValue>
  {
    [VInt(r.rf), VInt(r.ls)]
  }

  ghost function PackOfKeys(k: seq<SqlValue>): int
    requires |k| == 2 && k[0].VInt? && k[1].VInt?
    requires 0 <= k[0].z < 256 && 0 <= k[1].z < 256
  {
    R.Pack(k[0].z, k[1].z)
  }

  // Domain/content via StoreGet — independent of first-seen index order.
  ghost predicate {:opaque} ListStoreRel(st: RelEval.GroupStore, m: R.KeyStore)
  {
    RelEval.StoreKeysUnique(st) &&
    (forall i :: 0 <= i < |st| ==>
       |st[i].k| == 2 && st[i].k[0].VInt? && st[i].k[1].VInt? &&
       0 <= st[i].k[0].z < 256 && 0 <= st[i].k[1].z < 256) &&
    (forall g :: g in m ==> 0 <= g < 65536 && m[g].count >= 0) &&
    (forall rf, ls :: 0 <= rf < 256 && 0 <= ls < 256 ==>
       (R.Pack(rf, ls) in m <==>
          RelEval.StoreGet(st, [VInt(rf), VInt(ls)]).SomeAcc?) &&
       (R.Pack(rf, ls) in m ==>
          RelEval.StoreGet(st, [VInt(rf), VInt(ls)]).acc
            == AccSlotOfStats(m[R.Pack(rf, ls)])))
  }

  lemma Pack_div_mod(rf: int, ls: int)
    requires 0 <= rf < 256 && 0 <= ls < 256
    ensures R.Pack(rf, ls) / 256 == rf
    ensures R.Pack(rf, ls) % 256 == ls
    ensures 0 <= R.Pack(rf, ls) < 65536
  {
  }

  lemma Pack_inj(rf1: int, ls1: int, rf2: int, ls2: int)
    requires 0 <= rf1 < 256 && 0 <= ls1 < 256
    requires 0 <= rf2 < 256 && 0 <= ls2 < 256
    ensures R.Pack(rf1, ls1) == R.Pack(rf2, ls2) ==> rf1 == rf2 && ls1 == ls2
  {
    if R.Pack(rf1, ls1) == R.Pack(rf2, ls2) {
      Pack_div_mod(rf1, ls1);
      Pack_div_mod(rf2, ls2);
    }
  }

  lemma Unpack_pack(g: int)
    requires 0 <= g < 65536
    ensures g == (g / 256) * 256 + (g % 256)
    ensures 0 <= g / 256 < 256 && 0 <= g % 256 < 256
    ensures R.Pack(g / 256, g % 256) == g
  {
  }

  lemma ListStoreRel_count_nonneg(st: RelEval.GroupStore, m: R.KeyStore, g: int)
    requires ListStoreRel(st, m)
    requires g in m
    ensures m[g].count >= 0
  {
    reveal ListStoreRel();
  }

  lemma ListStoreRel_empty()
    ensures ListStoreRel([], map[])
  {
    reveal ListStoreRel();
  }

  // =====================================================================
  // A. Filter
  // =====================================================================

  lemma EvalPredKeep_Q1_shipdate(c: int, r: R.Row)
    ensures RelEval.EvalPredKeep(Q1PredShipdate(), ParamCutoff(c), RowOfAux(r))
         == RelEval.SomeBool(r.shipdate <= c)
  {
    // shipdate slot 0, cutoff param — unfolds by computation
  }

  lemma RowsOfAux_len(db: seq<R.Row>)
    ensures |RowsOfAux(db)| == |db|
    decreases |db|
  {
    if |db| == 0 {
    } else {
      RowsOfAux_len(db[1..]);
    }
  }

  lemma RowsOfAux_cons(db: seq<R.Row>)
    requires |db| > 0
    ensures RowsOfAux(db) == [RowOfAux(db[0])] + RowsOfAux(db[1..])
  {
  }

  lemma FilterCutoff_cons_front(db: seq<R.Row>, c: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    requires |db| > 0
    ensures db[0].shipdate <= c ==>
            R.FilterCutoff(db, c) == [db[0]] + R.FilterCutoff(db[1..], c)
    ensures db[0].shipdate > c ==>
            R.FilterCutoff(db, c) == R.FilterCutoff(db[1..], c)
    decreases |db|
  {
    if |db| == 1 {
    } else {
      // FilterCutoff peels from the back; prove by induction on the suffix shape.
      FilterCutoff_cons_front_aux(db, c);
    }
  }

  // Helper: characterize FilterCutoff by front-cons recursion.
  lemma FilterCutoff_cons_front_aux(db: seq<R.Row>, c: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures R.FilterCutoff(db, c) ==
            (if |db| == 0 then []
             else if db[0].shipdate <= c then [db[0]] + R.FilterCutoff(db[1..], c)
             else R.FilterCutoff(db[1..], c))
    decreases |db|
  {
    if |db| == 0 {
    } else if |db| == 1 {
    } else {
      FilterCutoff_cons_front_aux(db[..|db| - 1], c);
      // Unfold last-peel definition and reassociate.
      var pref := db[..|db| - 1];
      var last := db[|db| - 1];
      assert db == pref + [last];
      assert db[1..] == pref[1..] + [last];
      FilterCutoff_cons_front_aux(db[1..], c);
      if last.shipdate <= c {
        assert R.FilterCutoff(db, c) == R.FilterCutoff(pref, c) + [last];
        if db[0].shipdate <= c {
          assert R.FilterCutoff(pref, c) == [db[0]] + R.FilterCutoff(pref[1..], c);
          assert R.FilterCutoff(db[1..], c) == R.FilterCutoff(pref[1..], c) + [last];
        } else {
          assert R.FilterCutoff(pref, c) == R.FilterCutoff(pref[1..], c);
          assert R.FilterCutoff(db[1..], c) == R.FilterCutoff(pref[1..], c) + [last];
        }
      } else {
        assert R.FilterCutoff(db, c) == R.FilterCutoff(pref, c);
        if db[0].shipdate <= c {
          assert R.FilterCutoff(pref, c) == [db[0]] + R.FilterCutoff(pref[1..], c);
          assert R.FilterCutoff(db[1..], c) == R.FilterCutoff(pref[1..], c);
        } else {
          assert R.FilterCutoff(pref, c) == R.FilterCutoff(pref[1..], c);
          assert R.FilterCutoff(db[1..], c) == R.FilterCutoff(pref[1..], c);
        }
      }
    }
  }

  lemma FilterRows_rel_eq_FilterCutoff(c: int, db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures RelEval.FilterRows(Q1PredShipdate(), ParamCutoff(c), RowsOfAux(db))
         == RelEval.SomeRows(RowsOfAux(R.FilterCutoff(db, c)))
    decreases |db|
  {
    if |db| == 0 {
    } else {
      FilterCutoff_cons_front_aux(db, c);
      FilterRows_rel_eq_FilterCutoff(c, db[1..]);
      EvalPredKeep_Q1_shipdate(c, db[0]);
      RowsOfAux_cons(db);
      if db[0].shipdate <= c {
        assert RowsOfAux(R.FilterCutoff(db, c))
            == [RowOfAux(db[0])] + RowsOfAux(R.FilterCutoff(db[1..], c));
      } else {
        assert RowsOfAux(R.FilterCutoff(db, c))
            == RowsOfAux(R.FilterCutoff(db[1..], c));
      }
    }
  }

  // =====================================================================
  // B. Contrib / AccAdd / StoreAdd / Fold via ListStoreRel
  // =====================================================================

  lemma EvalExprs_Q1_group(c: int, r: R.Row)
    ensures RelEval.EvalExprs(Q1GroupExprs(), ParamCutoff(c), RelEval.EvRow(RowOfAux(r)))
         == RelEval.SomeVals(KeysOfRow(r))
  {
  }

  lemma AggsContrib_Q1_rel(c: int, r: R.Row)
    ensures RelEval.AggsContrib(Q1AggsRel(), ParamCutoff(c), RowOfAux(r))
         == RelEval.SomeVals([
              VInt(r.qty), VInt(r.price),
              VInt(r.price * (100 - r.disc)),
              VInt(r.price * (100 - r.disc) * (100 + r.tax)),
              VInt(r.qty), VInt(r.price), VInt(r.disc), VInt(1)
            ])
  {
  }

  lemma AccAddSlot_Sum(slot: RelEval.AccSlot, z: int)
    ensures RelEval.AccAddSlot(AggSum, slot, RelEval.SomeVal(VInt(z)))
         == RelEval.SomeSlot(RelEval.AccSlot(slot.sum + z, slot.count + 1, true))
  {
  }

  lemma AccAddSlot_Avg(slot: RelEval.AccSlot, z: int)
    ensures RelEval.AccAddSlot(AggAvg, slot, RelEval.SomeVal(VInt(z)))
         == RelEval.SomeSlot(RelEval.AccSlot(slot.sum + z, slot.count + 1, true))
  {
  }

  lemma AccAddSlot_CountStar(slot: RelEval.AccSlot)
    ensures RelEval.AccAddSlot(AggCountStar, slot, RelEval.SomeVal(VInt(1)))
         == RelEval.SomeSlot(RelEval.AccSlot(slot.sum + 1, slot.count + 1, true))
  {
  }

  lemma Q1AggsRel_shape()
    ensures |Q1AggsRel()| == 8
    ensures Q1AggsRel()[0].kind == AggSum
    ensures Q1AggsRel()[1].kind == AggSum
    ensures Q1AggsRel()[2].kind == AggSum
    ensures Q1AggsRel()[3].kind == AggSum
    ensures Q1AggsRel()[4].kind == AggAvg
    ensures Q1AggsRel()[5].kind == AggAvg
    ensures Q1AggsRel()[6].kind == AggAvg
    ensures Q1AggsRel()[7].kind == AggCountStar
  {
  }

  lemma AccAddOne_Q1_suffix7(acc: RelEval.AggAcc)
    requires |acc| == 1
    ensures RelEval.AccAddOne(Q1AggsRel()[7..], acc, [VInt(1)])
         == RelEval.SomeAcc([RelEval.AccSlot(acc[0].sum + 1, acc[0].count + 1, true)])
  {
    Q1AggsRel_shape();
    AccAddSlot_CountStar(acc[0]);
  }

  lemma AccAddOne_Q1_from6(acc: RelEval.AggAcc, v6: int)
    requires |acc| == 2
    ensures RelEval.AccAddOne(Q1AggsRel()[6..], acc, [VInt(v6), VInt(1)])
         == RelEval.SomeAcc([
              RelEval.AccSlot(acc[0].sum + v6, acc[0].count + 1, true),
              RelEval.AccSlot(acc[1].sum + 1, acc[1].count + 1, true)
            ])
  {
    Q1AggsRel_shape();
    AccAddSlot_Avg(acc[0], v6);
    AccAddOne_Q1_suffix7(acc[1..]);
  }

  lemma AccAddOne_Q1_from5(acc: RelEval.AggAcc, v5: int, v6: int)
    requires |acc| == 3
    ensures RelEval.AccAddOne(Q1AggsRel()[5..], acc, [VInt(v5), VInt(v6), VInt(1)])
         == RelEval.SomeAcc([
              RelEval.AccSlot(acc[0].sum + v5, acc[0].count + 1, true),
              RelEval.AccSlot(acc[1].sum + v6, acc[1].count + 1, true),
              RelEval.AccSlot(acc[2].sum + 1, acc[2].count + 1, true)
            ])
  {
    Q1AggsRel_shape();
    AccAddSlot_Avg(acc[0], v5);
    AccAddOne_Q1_from6(acc[1..], v6);
  }

  lemma AccAddOne_Q1_from4(acc: RelEval.AggAcc, v4: int, v5: int, v6: int)
    requires |acc| == 4
    ensures RelEval.AccAddOne(Q1AggsRel()[4..], acc, [VInt(v4), VInt(v5), VInt(v6), VInt(1)])
         == RelEval.SomeAcc([
              RelEval.AccSlot(acc[0].sum + v4, acc[0].count + 1, true),
              RelEval.AccSlot(acc[1].sum + v5, acc[1].count + 1, true),
              RelEval.AccSlot(acc[2].sum + v6, acc[2].count + 1, true),
              RelEval.AccSlot(acc[3].sum + 1, acc[3].count + 1, true)
            ])
  {
    Q1AggsRel_shape();
    AccAddSlot_Avg(acc[0], v4);
    AccAddOne_Q1_from5(acc[1..], v5, v6);
  }

  lemma AccAddOne_Q1_from3(acc: RelEval.AggAcc, v3: int, v4: int, v5: int, v6: int)
    requires |acc| == 5
    ensures RelEval.AccAddOne(Q1AggsRel()[3..], acc,
              [VInt(v3), VInt(v4), VInt(v5), VInt(v6), VInt(1)])
         == RelEval.SomeAcc([
              RelEval.AccSlot(acc[0].sum + v3, acc[0].count + 1, true),
              RelEval.AccSlot(acc[1].sum + v4, acc[1].count + 1, true),
              RelEval.AccSlot(acc[2].sum + v5, acc[2].count + 1, true),
              RelEval.AccSlot(acc[3].sum + v6, acc[3].count + 1, true),
              RelEval.AccSlot(acc[4].sum + 1, acc[4].count + 1, true)
            ])
  {
    Q1AggsRel_shape();
    AccAddSlot_Sum(acc[0], v3);
    AccAddOne_Q1_from4(acc[1..], v4, v5, v6);
  }

  lemma AccAddOne_Q1_from2(acc: RelEval.AggAcc, v2: int, v3: int, v4: int, v5: int, v6: int)
    requires |acc| == 6
    ensures RelEval.AccAddOne(Q1AggsRel()[2..], acc,
              [VInt(v2), VInt(v3), VInt(v4), VInt(v5), VInt(v6), VInt(1)])
         == RelEval.SomeAcc([
              RelEval.AccSlot(acc[0].sum + v2, acc[0].count + 1, true),
              RelEval.AccSlot(acc[1].sum + v3, acc[1].count + 1, true),
              RelEval.AccSlot(acc[2].sum + v4, acc[2].count + 1, true),
              RelEval.AccSlot(acc[3].sum + v5, acc[3].count + 1, true),
              RelEval.AccSlot(acc[4].sum + v6, acc[4].count + 1, true),
              RelEval.AccSlot(acc[5].sum + 1, acc[5].count + 1, true)
            ])
  {
    Q1AggsRel_shape();
    AccAddSlot_Sum(acc[0], v2);
    AccAddOne_Q1_from3(acc[1..], v3, v4, v5, v6);
  }

  lemma AccAddOne_Q1_from1(acc: RelEval.AggAcc, v1: int, v2: int, v3: int, v4: int, v5: int, v6: int)
    requires |acc| == 7
    ensures RelEval.AccAddOne(Q1AggsRel()[1..], acc,
              [VInt(v1), VInt(v2), VInt(v3), VInt(v4), VInt(v5), VInt(v6), VInt(1)])
         == RelEval.SomeAcc([
              RelEval.AccSlot(acc[0].sum + v1, acc[0].count + 1, true),
              RelEval.AccSlot(acc[1].sum + v2, acc[1].count + 1, true),
              RelEval.AccSlot(acc[2].sum + v3, acc[2].count + 1, true),
              RelEval.AccSlot(acc[3].sum + v4, acc[3].count + 1, true),
              RelEval.AccSlot(acc[4].sum + v5, acc[4].count + 1, true),
              RelEval.AccSlot(acc[5].sum + v6, acc[5].count + 1, true),
              RelEval.AccSlot(acc[6].sum + 1, acc[6].count + 1, true)
            ])
  {
    Q1AggsRel_shape();
    AccAddSlot_Sum(acc[0], v1);
    AccAddOne_Q1_from2(acc[1..], v2, v3, v4, v5, v6);
  }

  lemma AccAddOne_Q1_full(acc: RelEval.AggAcc, v0: int, v1: int, v2: int, v3: int, v4: int, v5: int, v6: int)
    requires |acc| == 8
    ensures RelEval.AccAddOne(Q1AggsRel(), acc,
              [VInt(v0), VInt(v1), VInt(v2), VInt(v3), VInt(v4), VInt(v5), VInt(v6), VInt(1)])
         == RelEval.SomeAcc([
              RelEval.AccSlot(acc[0].sum + v0, acc[0].count + 1, true),
              RelEval.AccSlot(acc[1].sum + v1, acc[1].count + 1, true),
              RelEval.AccSlot(acc[2].sum + v2, acc[2].count + 1, true),
              RelEval.AccSlot(acc[3].sum + v3, acc[3].count + 1, true),
              RelEval.AccSlot(acc[4].sum + v4, acc[4].count + 1, true),
              RelEval.AccSlot(acc[5].sum + v5, acc[5].count + 1, true),
              RelEval.AccSlot(acc[6].sum + v6, acc[6].count + 1, true),
              RelEval.AccSlot(acc[7].sum + 1, acc[7].count + 1, true)
            ])
  {
    Q1AggsRel_shape();
    AccAddSlot_Sum(acc[0], v0);
    AccAddOne_Q1_from1(acc[1..], v1, v2, v3, v4, v5, v6);
  }

  lemma AccAddRow_Q1_SpecApply(c: int, r: R.Row, s: R.SpecStats)
    requires s.count >= 0
    ensures RelEval.AccAddRow(Q1AggsRel(), ParamCutoff(c), RowOfAux(r), AccSlotOfStats(s))
         == RelEval.SomeAcc(AccSlotOfStats(R.SpecApply(s, r, 1)))
  {
    AggsContrib_Q1_rel(c, r);
    var acc := AccSlotOfStats(s);
    var vals := [
      VInt(r.qty), VInt(r.price),
      VInt(r.price * (100 - r.disc)),
      VInt(r.price * (100 - r.disc) * (100 + r.tax)),
      VInt(r.qty), VInt(r.price), VInt(r.disc), VInt(1)
    ];
    assert RelEval.AggsContrib(Q1AggsRel(), ParamCutoff(c), RowOfAux(r))
        == RelEval.SomeVals(vals);
    assert RelEval.AccAddRow(Q1AggsRel(), ParamCutoff(c), RowOfAux(r), acc)
        == RelEval.AccAddOne(Q1AggsRel(), acc, vals);
    AccAddOne_Q1_full(acc, r.qty, r.price,
      r.price * (100 - r.disc),
      r.price * (100 - r.disc) * (100 + r.tax),
      r.qty, r.price, r.disc);
    var s1 := R.SpecApply(s, r, 1);
    assert s1.count == s.count + 1;
    assert s1.count != 0;
    assert acc[0].sum == s.sum_qty && acc[0].count == s.count;
    assert acc[1].sum == s.sum_base_price && acc[1].count == s.count;
    assert acc[2].sum == s.sum_disc_price && acc[2].count == s.count;
    assert acc[3].sum == s.sum_charge && acc[3].count == s.count;
    assert acc[4].sum == s.sum_qty && acc[4].count == s.count;
    assert acc[5].sum == s.sum_base_price && acc[5].count == s.count;
    assert acc[6].sum == s.sum_discount && acc[6].count == s.count;
    assert acc[7].sum == s.count && acc[7].count == s.count;
    assert s1.sum_qty == s.sum_qty + r.qty;
    assert s1.sum_base_price == s.sum_base_price + r.price;
    assert s1.sum_disc_price == s.sum_disc_price + r.price * (100 - r.disc);
    assert s1.sum_charge == s.sum_charge + r.price * (100 - r.disc) * (100 + r.tax);
    assert s1.sum_discount == s.sum_discount + r.disc;
    assert AccSlotOfStats(s1) == [
      RelEval.AccSlot(s.sum_qty + r.qty, s.count + 1, true),
      RelEval.AccSlot(s.sum_base_price + r.price, s.count + 1, true),
      RelEval.AccSlot(s.sum_disc_price + r.price * (100 - r.disc), s.count + 1, true),
      RelEval.AccSlot(s.sum_charge + r.price * (100 - r.disc) * (100 + r.tax), s.count + 1, true),
      RelEval.AccSlot(s.sum_qty + r.qty, s.count + 1, true),
      RelEval.AccSlot(s.sum_base_price + r.price, s.count + 1, true),
      RelEval.AccSlot(s.sum_discount + r.disc, s.count + 1, true),
      RelEval.AccSlot(s.count + 1, s.count + 1, true)
    ];
  }

  lemma AccAddRow_Q1_Empty(c: int, r: R.Row)
    ensures RelEval.AccAddRow(Q1AggsRel(), ParamCutoff(c), RowOfAux(r), RelEval.EmptyAcc(8))
         == RelEval.SomeAcc(AccSlotOfStats(R.SpecApply(R.EmptyStats(), r, 1)))
  {
    AccSlotOfStats_Empty();
    AccAddRow_Q1_SpecApply(c, r, R.EmptyStats());
  }


  lemma ListEq_KeysOfRow(r: R.Row)
    ensures RelEval.ListEq(KeysOfRow(r), KeysOfRow(r))
  {
    RelEval.ListEq_refl(KeysOfRow(r));
  }

  lemma ListEq_VInt2(a0: int, a1: int, b0: int, b1: int)
    requires RelEval.ListEq([VInt(a0), VInt(a1)], [VInt(b0), VInt(b1)])
    ensures a0 == b0 && a1 == b1
  {
  }

  lemma StoreGet_ListStoreRel(st: RelEval.GroupStore, m: R.KeyStore, r: R.Row)
    requires ListStoreRel(st, m)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures var g := R.Pack(r.rf, r.ls);
            var found := RelEval.StoreGet(st, KeysOfRow(r));
            (g in m <==> found.SomeAcc?) &&
            (found.SomeAcc? ==> found.acc == AccSlotOfStats(m[g]))
  {
    reveal ListStoreRel();
  }

  lemma ListEq_unique_right(a: seq<SqlValue>, b: seq<SqlValue>, c: seq<SqlValue>)
    requires RelEval.ListEq(a, b) && RelEval.ListEq(a, c)
    ensures RelEval.ListEq(b, c)
  {
    RelEval.ListEq_sym(a, b);
    RelEval.ListEq_trans(b, a, c);
  }

  lemma StoreGet_Put_miss(st: RelEval.GroupStore, k: seq<SqlValue>, a: RelEval.AggAcc, k2: seq<SqlValue>)
    requires !RelEval.ListEq(k, k2)
    ensures RelEval.StoreGet(RelEval.StorePut(st, k, a), k2) == RelEval.StoreGet(st, k2)
    decreases |st|
  {
    if |st| == 0 {
    } else if RelEval.ListEq(st[0].k, k) {
      // Head matched k; StorePut replaces it. k2 ≠ k so Get skips new head to st[1..].
      // Old Get also skipped head: ListEq(st[0].k, k2) would imply ListEq(k, k2).
      assert !RelEval.ListEq(st[0].k, k2) by {
        if RelEval.ListEq(st[0].k, k2) {
          ListEq_unique_right(st[0].k, k, k2);
          assert RelEval.ListEq(k, k2);
          assert false;
        }
      }
      assert RelEval.StorePut(st, k, a) == [RelEval.GroupEntry(k, a)] + st[1..];
      assert RelEval.StoreGet(RelEval.StorePut(st, k, a), k2)
          == RelEval.StoreGet(st[1..], k2);
      assert RelEval.StoreGet(st, k2) == RelEval.StoreGet(st[1..], k2);
    } else {
      if RelEval.ListEq(st[0].k, k2) {
      } else {
        StoreGet_Put_miss(st[1..], k, a, k2);
      }
    }
  }

  lemma StorePut_entries_wf(st: RelEval.GroupStore, k: seq<SqlValue>, a: RelEval.AggAcc)
    requires forall i :: 0 <= i < |st| ==>
               |st[i].k| == 2 && st[i].k[0].VInt? && st[i].k[1].VInt? &&
               0 <= st[i].k[0].z < 256 && 0 <= st[i].k[1].z < 256
    requires |k| == 2 && k[0].VInt? && k[1].VInt? &&
             0 <= k[0].z < 256 && 0 <= k[1].z < 256
    ensures forall i :: 0 <= i < |RelEval.StorePut(st, k, a)| ==>
              |RelEval.StorePut(st, k, a)[i].k| == 2 &&
              RelEval.StorePut(st, k, a)[i].k[0].VInt? &&
              RelEval.StorePut(st, k, a)[i].k[1].VInt? &&
              0 <= RelEval.StorePut(st, k, a)[i].k[0].z < 256 &&
              0 <= RelEval.StorePut(st, k, a)[i].k[1].z < 256
    decreases |st|
  {
    if |st| == 0 {
    } else if RelEval.ListEq(st[0].k, k) {
    } else {
      StorePut_entries_wf(st[1..], k, a);
    }
  }

  lemma ListStoreRel_after_Put(
      st: RelEval.GroupStore, m: R.KeyStore, r: R.Row, s1: R.SpecStats)
    requires ListStoreRel(st, m)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires s1.count >= 0
    requires s1 == (if R.Pack(r.rf, r.ls) in m
                    then R.SpecApply(m[R.Pack(r.rf, r.ls)], r, 1)
                    else R.SpecApply(R.EmptyStats(), r, 1))
    ensures ListStoreRel(
              RelEval.StorePut(st, KeysOfRow(r), AccSlotOfStats(s1)),
              m[R.Pack(r.rf, r.ls) := s1])
  {
    reveal ListStoreRel();
    var g := R.Pack(r.rf, r.ls);
    var k := KeysOfRow(r);
    var a := AccSlotOfStats(s1);
    var st1 := RelEval.StorePut(st, k, a);
    var m1 := m[g := s1];
    Pack_div_mod(r.rf, r.ls);
    RelEval.StorePut_preserves_unique(st, k, a);
    StorePut_entries_wf(st, k, a);
    RelEval.StoreGet_Put_hit(st, k, a);
    forall rf, ls | 0 <= rf < 256 && 0 <= ls < 256
      ensures (R.Pack(rf, ls) in m1 <==>
                 RelEval.StoreGet(st1, [VInt(rf), VInt(ls)]).SomeAcc?)
      ensures R.Pack(rf, ls) in m1 ==>
              RelEval.StoreGet(st1, [VInt(rf), VInt(ls)]).acc
                == AccSlotOfStats(m1[R.Pack(rf, ls)])
    {
      var g2 := R.Pack(rf, ls);
      var k2 := [VInt(rf), VInt(ls)];
      if g2 == g {
        Pack_div_mod(rf, ls);
        assert rf == r.rf && ls == r.ls;
        assert RelEval.ListEq(k, k2) by { RelEval.ListEq_refl(k); }
        assert RelEval.StoreGet(st1, k2) == RelEval.SomeAcc(a);
        assert g2 in m1;
        assert m1[g2] == s1;
      } else {
        assert !RelEval.ListEq(k, k2) by {
          if RelEval.ListEq(k, k2) {
            ListEq_VInt2(r.rf, r.ls, rf, ls);
            assert false;
          }
        }
        StoreGet_Put_miss(st, k, a, k2);
        assert RelEval.StoreGet(st1, k2) == RelEval.StoreGet(st, k2);
        if g2 in m {
          assert g2 in m1 && m1[g2] == m[g2];
        } else {
          assert g2 !in m1;
        }
      }
    }
    forall gg | gg in m1
      ensures 0 <= gg < 65536 && m1[gg].count >= 0
    {
      if gg == g {
        Pack_div_mod(r.rf, r.ls);
      } else {
        assert gg in m;
      }
    }
  }

  lemma StoreAddRow_ListStoreRel(c: int, r: R.Row, st: RelEval.GroupStore, m: R.KeyStore)
    requires ListStoreRel(st, m)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures var st1 := RelEval.StoreAddRow(
              Q1AggsRel(), Q1GroupExprs(), ParamCutoff(c), RowOfAux(r), st);
            st1.SomeStore? && ListStoreRel(st1.st, R.ApplyDeltaKey(m, r, 1))
  {
    reveal ListStoreRel();
    EvalExprs_Q1_group(c, r);
    var g := R.Pack(r.rf, r.ls);
    StoreGet_ListStoreRel(st, m, r);
    var found := RelEval.StoreGet(st, KeysOfRow(r));
    if g in m {
      assert found.SomeAcc? && found.acc == AccSlotOfStats(m[g]);
      assert m[g].count >= 0;
      AccAddRow_Q1_SpecApply(c, r, m[g]);
      var s1 := R.SpecApply(m[g], r, 1);
      assert s1.count >= 0;
      ListStoreRel_after_Put(st, m, r, s1);
      assert R.ApplyDeltaKey(m, r, 1) == m[g := s1];
    } else {
      assert found.NoAcc?;
      AccAddRow_Q1_Empty(c, r);
      var s1 := R.SpecApply(R.EmptyStats(), r, 1);
      assert s1.count >= 0;
      ListStoreRel_after_Put(st, m, r, s1);
      assert R.GetKey(m, g) == R.EmptyStats();
      assert R.ApplyDeltaKey(m, r, 1) == m[g := s1];
    }
  }

  ghost function FoldApplyFront(rs: seq<R.Row>, m: R.KeyStore): R.KeyStore
    requires forall i :: 0 <= i < |rs| ==> 0 <= rs[i].rf < 256 && 0 <= rs[i].ls < 256
    decreases |rs|
  {
    if |rs| == 0 then m
    else FoldApplyFront(rs[1..], R.ApplyDeltaKey(m, rs[0], 1))
  }

  lemma FoldGroups_ListStoreRel(c: int, rs: seq<R.Row>, st: RelEval.GroupStore, m: R.KeyStore)
    requires ListStoreRel(st, m)
    requires forall i :: 0 <= i < |rs| ==> 0 <= rs[i].rf < 256 && 0 <= rs[i].ls < 256
    ensures var st1 := RelEval.FoldGroups(
              Q1AggsRel(), Q1GroupExprs(), ParamCutoff(c), RowsOfAux(rs), st);
            st1.SomeStore? && ListStoreRel(st1.st, FoldApplyFront(rs, m))
    decreases |rs|
  {
    if |rs| == 0 {
    } else {
      RowsOfAux_cons(rs);
      StoreAddRow_ListStoreRel(c, rs[0], st, m);
      var st1 := RelEval.StoreAddRow(
        Q1AggsRel(), Q1GroupExprs(), ParamCutoff(c), RowOfAux(rs[0]), st);
      FoldGroups_ListStoreRel(c, rs[1..], st1.st, R.ApplyDeltaKey(m, rs[0], 1));
    }
  }

  // SpecApply is commutative for distinct-then-same key bags: front fold == back fold.
  // Prove by relating both to the unique pointwise sum of contributions (via induction
  // on |rs| and ApplyDeltaKey fusion).
  lemma ApplyDeltaKey_comm(m: R.KeyStore, r1: R.Row, r2: R.Row)
    requires 0 <= r1.rf < 256 && 0 <= r1.ls < 256
    requires 0 <= r2.rf < 256 && 0 <= r2.ls < 256
    ensures R.ApplyDeltaKey(R.ApplyDeltaKey(m, r1, 1), r2, 1)
         == R.ApplyDeltaKey(R.ApplyDeltaKey(m, r2, 1), r1, 1)
  {
    var g1 := R.Pack(r1.rf, r1.ls);
    var g2 := R.Pack(r2.rf, r2.ls);
    if g1 == g2 {
      // same key: SpecApply(SpecApply(s,r1),r2) == SpecApply(SpecApply(s,r2),r1)
      var s := R.GetKey(m, g1);
      assert R.SpecApply(R.SpecApply(s, r1, 1), r2, 1)
          == R.SpecApply(R.SpecApply(s, r2, 1), r1, 1);
    } else {
      // distinct keys: map updates commute
    }
  }

  lemma FoldApplyFront_snoc(rs: seq<R.Row>, r: R.Row, m: R.KeyStore)
    requires forall i :: 0 <= i < |rs| ==> 0 <= rs[i].rf < 256 && 0 <= rs[i].ls < 256
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    ensures FoldApplyFront(rs + [r], m) == R.ApplyDeltaKey(FoldApplyFront(rs, m), r, 1)
    decreases |rs|
  {
    if |rs| == 0 {
    } else {
      FoldApplyFront_snoc(rs[1..], r, R.ApplyDeltaKey(m, rs[0], 1));
      assert (rs + [r])[1..] == rs[1..] + [r];
    }
  }

  lemma FoldApplyFront_eq_FoldByGroup(rs: seq<R.Row>)
    requires forall i :: 0 <= i < |rs| ==> 0 <= rs[i].rf < 256 && 0 <= rs[i].ls < 256
    ensures FoldApplyFront(rs, map[]) == R.FoldByGroup(rs)
    decreases |rs|
  {
    if |rs| == 0 {
    } else {
      FoldApplyFront_eq_FoldByGroup(rs[..|rs| - 1]);
      FoldApplyFront_snoc(rs[..|rs| - 1], rs[|rs| - 1], map[]);
      assert rs == rs[..|rs| - 1] + [rs[|rs| - 1]];
    }
  }

  lemma FoldGroups_from_empty_ListStoreRel(c: int, rs: seq<R.Row>)
    requires forall i :: 0 <= i < |rs| ==> 0 <= rs[i].rf < 256 && 0 <= rs[i].ls < 256
    ensures var st1 := RelEval.FoldGroups(
              Q1AggsRel(), Q1GroupExprs(), ParamCutoff(c), RowsOfAux(rs), []);
            st1.SomeStore? && ListStoreRel(st1.st, R.FoldByGroup(rs))
  {
    ListStoreRel_empty();
    FoldGroups_ListStoreRel(c, rs, [], map[]);
    FoldApplyFront_eq_FoldByGroup(rs);
  }

  // =====================================================================
  // D. Finalize / Project / one-group Adapt
  // =====================================================================

  ghost function FinalizedValsOfStats(s: R.SpecStats): seq<SqlValue>
    requires s.count > 0
  {
    [
      VInt(s.sum_qty), VInt(s.sum_base_price),
      VInt(s.sum_disc_price), VInt(s.sum_charge),
      VAvgExact(s.sum_qty, s.count),
      VAvgExact(s.sum_base_price, s.count),
      VAvgExact(s.sum_discount, s.count),
      VInt(s.count)
    ]
  }

  ghost function ProjectedRowOfStats(rf: int, ls: int, s: R.SpecStats): Row
    requires 0 <= rf < 256 && 0 <= ls < 256
    requires s.count > 0
  {
    [VInt(rf), VInt(ls)] + FinalizedValsOfStats(s)
  }

  lemma FinalizeAcc_of_stats(s: R.SpecStats)
    requires s.count > 0
    ensures RelEval.FinalizeAcc(Q1AggsRel(), AccSlotOfStats(s))
         == RelEval.SomeVals(FinalizedValsOfStats(s))
  {
    Q1AggsRel_shape();
  }

  lemma AccNonempty_of_stats(s: R.SpecStats)
    requires s.count > 0
    ensures RelEval.AccNonempty(AccSlotOfStats(s))
  {
    var acc := AccSlotOfStats(s);
    assert acc[0].init || acc[0].count != 0;
  }

  lemma ProjectAgg_Q1_row(c: int, rf: int, ls: int, vals: seq<SqlValue>)
    requires |vals| == 8
    ensures RelEval.EvalExprs(Q1ProjectExprs(), ParamCutoff(c),
              RelEval.EvAgg([VInt(rf), VInt(ls)], vals))
         == RelEval.SomeVals([VInt(rf), VInt(ls)] + vals)
  {
  }

  lemma Q1AdaptRowRel_of_finalized(rf: int, ls: int, s: R.SpecStats)
    requires 0 <= rf < 256 && 0 <= ls < 256
    requires s.count > 0
    ensures Q1AdaptRowRel(ProjectedRowOfStats(rf, ls, s))
         == R.SpecFinalizeOne(R.Pack(rf, ls), s)
  {
  }

  lemma Pack_order(rf1: int, ls1: int, rf2: int, ls2: int)
    requires 0 <= rf1 < 256 && 0 <= ls1 < 256
    requires 0 <= rf2 < 256 && 0 <= ls2 < 256
    ensures R.Pack(rf1, ls1) <= R.Pack(rf2, ls2)
         <==> (rf1 < rf2 || (rf1 == rf2 && ls1 <= ls2))
    ensures R.Pack(rf1, ls1) < R.Pack(rf2, ls2)
         <==> (rf1 < rf2 || (rf1 == rf2 && ls1 < ls2))
  {
  }

  // =====================================================================
  // D2. Store entry helpers + projected-row view of ListStoreRel
  // =====================================================================

  lemma StoreGet_at_index(st: RelEval.GroupStore, i: nat)
    requires RelEval.StoreKeysUnique(st)
    requires 0 <= i < |st|
    ensures RelEval.StoreGet(st, st[i].k) == RelEval.SomeAcc(st[i].a)
    decreases i
  {
    if i == 0 {
      RelEval.ListEq_refl(st[0].k);
    } else {
      assert !RelEval.ListEq(st[0].k, st[i].k);
      RelEval.StoreKeysUnique_tail(st);
      StoreGet_at_index(st[1..], i - 1);
      assert RelEval.StoreGet(st[1..], st[i].k) == RelEval.SomeAcc(st[i].a);
      assert st[1..][i - 1].k == st[i].k;
      assert RelEval.StoreGet(st, st[i].k) == RelEval.StoreGet(st[1..], st[i].k);
    }
  }

  lemma ListStoreRel_entry(st: RelEval.GroupStore, m: R.KeyStore, i: nat)
    requires ListStoreRel(st, m)
    requires 0 <= i < |st|
    ensures |st[i].k| == 2 && st[i].k[0].VInt? && st[i].k[1].VInt?
    ensures 0 <= st[i].k[0].z < 256 && 0 <= st[i].k[1].z < 256
    ensures R.Pack(st[i].k[0].z, st[i].k[1].z) in m
    ensures st[i].a == AccSlotOfStats(m[R.Pack(st[i].k[0].z, st[i].k[1].z)])
    ensures m[R.Pack(st[i].k[0].z, st[i].k[1].z)].count >= 0
  {
    reveal ListStoreRel();
    assert |st[i].k| == 2 && st[i].k[0].VInt? && st[i].k[1].VInt?;
    assert 0 <= st[i].k[0].z < 256 && 0 <= st[i].k[1].z < 256;
    StoreGet_at_index(st, i);
    var rf := st[i].k[0].z;
    var ls := st[i].k[1].z;
    var g := R.Pack(rf, ls);
    var k := [VInt(rf), VInt(ls)];
    assert st[i].k == k;
    RelEval.ListEq_refl(k);
    assert RelEval.StoreGet(st, k) == RelEval.StoreGet(st, st[i].k);
    assert RelEval.StoreGet(st, k).SomeAcc?;
    assert g in m;
    assert RelEval.StoreGet(st, k).acc == AccSlotOfStats(m[g]);
    assert st[i].a == AccSlotOfStats(m[g]);
    assert m[g].count >= 0;
  }

  lemma StoreGet_absent(st: RelEval.GroupStore, k: seq<SqlValue>)
    requires forall i :: 0 <= i < |st| ==> !RelEval.ListEq(st[i].k, k)
    ensures RelEval.StoreGet(st, k).NoAcc?
    decreases |st|
  {
    if |st| == 0 {
    } else {
      assert !RelEval.ListEq(st[0].k, k);
      StoreGet_absent(st[1..], k);
    }
  }

  lemma Head_absent_in_tail(st: RelEval.GroupStore)
    requires RelEval.StoreKeysUnique(st)
    requires |st| > 0
    ensures RelEval.StoreGet(st[1..], st[0].k).NoAcc?
  {
    forall i | 0 <= i < |st[1..]|
      ensures !RelEval.ListEq(st[1..][i].k, st[0].k)
    {
      assert !RelEval.ListEq(st[0].k, st[i + 1].k);
      if RelEval.ListEq(st[1..][i].k, st[0].k) {
        RelEval.ListEq_sym(st[1..][i].k, st[0].k);
        assert RelEval.ListEq(st[0].k, st[i + 1].k);
        assert false;
      }
    }
    StoreGet_absent(st[1..], st[0].k);
  }

  ghost predicate StoreProjectedPre(st: RelEval.GroupStore, m: R.KeyStore)
  {
    RelEval.StoreKeysUnique(st) &&
    (forall i :: 0 <= i < |st| ==>
       |st[i].k| == 2 && st[i].k[0].VInt? && st[i].k[1].VInt? &&
       0 <= st[i].k[0].z < 256 && 0 <= st[i].k[1].z < 256 &&
       R.Pack(st[i].k[0].z, st[i].k[1].z) in m &&
       m[R.Pack(st[i].k[0].z, st[i].k[1].z)].count > 0 &&
       st[i].a == AccSlotOfStats(m[R.Pack(st[i].k[0].z, st[i].k[1].z)]))
  }

  lemma ListStoreRel_to_ProjectedPre(st: RelEval.GroupStore, m: R.KeyStore)
    requires ListStoreRel(st, m)
    requires forall g :: g in m ==> m[g].count > 0
    ensures StoreProjectedPre(st, m)
  {
    reveal ListStoreRel();
    forall i | 0 <= i < |st|
      ensures |st[i].k| == 2 && st[i].k[0].VInt? && st[i].k[1].VInt?
      ensures 0 <= st[i].k[0].z < 256 && 0 <= st[i].k[1].z < 256
      ensures R.Pack(st[i].k[0].z, st[i].k[1].z) in m
      ensures m[R.Pack(st[i].k[0].z, st[i].k[1].z)].count > 0
      ensures st[i].a == AccSlotOfStats(m[R.Pack(st[i].k[0].z, st[i].k[1].z)])
    {
      ListStoreRel_entry(st, m, i);
      var g := R.Pack(st[i].k[0].z, st[i].k[1].z);
      assert g in m;
      assert m[g].count > 0;
    }
  }

  ghost function ProjectedRowsOfStore(st: RelEval.GroupStore, m: R.KeyStore): seq<Row>
    requires StoreProjectedPre(st, m)
    decreases |st|
  {
    if |st| == 0 then
      []
    else
      var rf := st[0].k[0].z;
      var ls := st[0].k[1].z;
      var g := R.Pack(rf, ls);
      [ProjectedRowOfStats(rf, ls, m[g])] + ProjectedRowsOfStore(st[1..], m)
  }

  lemma StoreToAggBags_Project_ListStoreRel(
      c: int, st: RelEval.GroupStore, m: R.KeyStore)
    requires StoreProjectedPre(st, m)
    ensures RelEval.StoreToAggBags(Q1AggsRel(), st).SomeBags?
    ensures var bags := RelEval.StoreToAggBags(Q1AggsRel(), st).bags;
            RelEval.ProjectAggRows(Q1ProjectExprs(), ParamCutoff(c), bags)
              == RelEval.SomeRows(ProjectedRowsOfStore(st, m))
    decreases |st|
  {
    if |st| == 0 {
    } else {
      var rf := st[0].k[0].z;
      var ls := st[0].k[1].z;
      var g := R.Pack(rf, ls);
      var s := m[g];
      assert st[0].a == AccSlotOfStats(s);
      AccNonempty_of_stats(s);
      FinalizeAcc_of_stats(s);
      StoreToAggBags_Project_ListStoreRel(c, st[1..], m);
      var restBags := RelEval.StoreToAggBags(Q1AggsRel(), st[1..]).bags;
      assert RelEval.StoreToAggBags(Q1AggsRel(), st)
          == RelEval.SomeBags(
               [RelEval.AggBag(st[0].k, FinalizedValsOfStats(s))] + restBags);
      ProjectAgg_Q1_row(c, rf, ls, FinalizedValsOfStats(s));
      assert RelEval.EvalExprs(Q1ProjectExprs(), ParamCutoff(c),
               RelEval.EvAgg(st[0].k, FinalizedValsOfStats(s)))
          == RelEval.SomeVals(ProjectedRowOfStats(rf, ls, s));
    }
  }

  // =====================================================================
  // D3. Pack keys + Adapt ≡ FinalizeKeys on projected rows
  // =====================================================================

  ghost function PackKeysOfProjected(rows: seq<Row>): seq<int>
    requires forall i :: 0 <= i < |rows| ==>
               |rows[i]| >= 2 && rows[i][0].VInt? && rows[i][1].VInt? &&
               0 <= rows[i][0].z < 256 && 0 <= rows[i][1].z < 256
    decreases |rows|
  {
    if |rows| == 0 then []
    else [R.Pack(rows[0][0].z, rows[0][1].z)] + PackKeysOfProjected(rows[1..])
  }

  lemma PackKeysOfProjected_len(rows: seq<Row>)
    requires forall i :: 0 <= i < |rows| ==>
               |rows[i]| >= 2 && rows[i][0].VInt? && rows[i][1].VInt? &&
               0 <= rows[i][0].z < 256 && 0 <= rows[i][1].z < 256
    ensures |PackKeysOfProjected(rows)| == |rows|
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      PackKeysOfProjected_len(rows[1..]);
    }
  }

  ghost predicate ProjectedRowsOk(rows: seq<Row>)
  {
    forall i :: 0 <= i < |rows| ==>
      |rows[i]| == 10 &&
      rows[i][0].VInt? && rows[i][1].VInt? &&
      rows[i][2].VInt? && rows[i][3].VInt? &&
      rows[i][4].VInt? && rows[i][5].VInt? &&
      rows[i][6].VAvgExact? && rows[i][7].VAvgExact? &&
      rows[i][8].VAvgExact? && rows[i][9].VInt? &&
      0 <= rows[i][0].z < 256 && 0 <= rows[i][1].z < 256 &&
      rows[i][9].z > 0
  }

  lemma ProjectedRowsOk_implies_PackKeysPre(rows: seq<Row>)
    requires ProjectedRowsOk(rows)
    ensures forall i :: 0 <= i < |rows| ==>
              |rows[i]| >= 2 && rows[i][0].VInt? && rows[i][1].VInt? &&
              0 <= rows[i][0].z < 256 && 0 <= rows[i][1].z < 256
  {
  }

  lemma ProjectedRowsOfStore_ok(st: RelEval.GroupStore, m: R.KeyStore)
    requires StoreProjectedPre(st, m)
    ensures ProjectedRowsOk(ProjectedRowsOfStore(st, m))
    ensures |ProjectedRowsOfStore(st, m)| == |st|
    decreases |st|
  {
    if |st| == 0 {
    } else {
      ProjectedRowsOfStore_ok(st[1..], m);
    }
  }

  lemma StoreProjectedPre_implies_PackKeysPre(st: RelEval.GroupStore, m: R.KeyStore)
    requires StoreProjectedPre(st, m)
    ensures ProjectedRowsOk(ProjectedRowsOfStore(st, m))
    ensures |ProjectedRowsOfStore(st, m)| == |st|
    ensures forall i :: 0 <= i < |ProjectedRowsOfStore(st, m)| ==>
              |ProjectedRowsOfStore(st, m)[i]| >= 2 &&
              ProjectedRowsOfStore(st, m)[i][0].VInt? &&
              ProjectedRowsOfStore(st, m)[i][1].VInt? &&
              0 <= ProjectedRowsOfStore(st, m)[i][0].z < 256 &&
              0 <= ProjectedRowsOfStore(st, m)[i][1].z < 256
    ensures |PackKeysOfProjected(ProjectedRowsOfStore(st, m))| == |st|
  {
    ProjectedRowsOfStore_ok(st, m);
    ProjectedRowsOk_implies_PackKeysPre(ProjectedRowsOfStore(st, m));
    PackKeysOfProjected_len(ProjectedRowsOfStore(st, m));
  }

  lemma Projected_row_at(st: RelEval.GroupStore, m: R.KeyStore, i: nat)
    requires StoreProjectedPre(st, m)
    requires 0 <= i < |st|
    ensures |ProjectedRowsOfStore(st, m)| == |st|
    ensures ProjectedRowsOfStore(st, m)[i]
         == ProjectedRowOfStats(st[i].k[0].z, st[i].k[1].z,
              m[R.Pack(st[i].k[0].z, st[i].k[1].z)])
    decreases i
  {
    ProjectedRowsOfStore_ok(st, m);
    if i == 0 {
    } else {
      Projected_row_at(st[1..], m, i - 1);
    }
  }

  lemma Projected_Pack_index_in_m(st: RelEval.GroupStore, m: R.KeyStore, i: nat)
    requires StoreProjectedPre(st, m)
    requires 0 <= i < |st|
    ensures ProjectedRowsOk(ProjectedRowsOfStore(st, m))
    ensures |ProjectedRowsOfStore(st, m)| == |st|
    ensures forall j :: 0 <= j < |st| ==>
              |ProjectedRowsOfStore(st, m)[j]| >= 2 &&
              ProjectedRowsOfStore(st, m)[j][0].VInt? &&
              ProjectedRowsOfStore(st, m)[j][1].VInt? &&
              0 <= ProjectedRowsOfStore(st, m)[j][0].z < 256 &&
              0 <= ProjectedRowsOfStore(st, m)[j][1].z < 256
    ensures |PackKeysOfProjected(ProjectedRowsOfStore(st, m))| == |st|
    ensures PackKeysOfProjected(ProjectedRowsOfStore(st, m))[i]
         == R.Pack(st[i].k[0].z, st[i].k[1].z)
    ensures PackKeysOfProjected(ProjectedRowsOfStore(st, m))[i] in m
    decreases i
  {
    ProjectedRowsOfStore_ok(st, m);
    ProjectedRowsOk_implies_PackKeysPre(ProjectedRowsOfStore(st, m));
    PackKeysOfProjected_len(ProjectedRowsOfStore(st, m));
    Projected_row_at(st, m, i);
    if i == 0 {
      assert ProjectedRowsOfStore(st, m)[0][0].z == st[0].k[0].z;
      assert ProjectedRowsOfStore(st, m)[0][1].z == st[0].k[1].z;
    } else {
      Projected_Pack_index_in_m(st[1..], m, i - 1);
    }
  }

  lemma Projected_Pack_nodup(st: RelEval.GroupStore, m: R.KeyStore)
    requires StoreProjectedPre(st, m)
    ensures ProjectedRowsOk(ProjectedRowsOfStore(st, m))
    ensures |ProjectedRowsOfStore(st, m)| == |st|
    ensures forall i :: 0 <= i < |st| ==>
              |ProjectedRowsOfStore(st, m)[i]| >= 2 &&
              ProjectedRowsOfStore(st, m)[i][0].VInt? &&
              ProjectedRowsOfStore(st, m)[i][1].VInt? &&
              0 <= ProjectedRowsOfStore(st, m)[i][0].z < 256 &&
              0 <= ProjectedRowsOfStore(st, m)[i][1].z < 256
    ensures |PackKeysOfProjected(ProjectedRowsOfStore(st, m))| == |st|
    ensures var keys := PackKeysOfProjected(ProjectedRowsOfStore(st, m));
            forall i, j :: 0 <= i < j < |keys| ==> keys[i] != keys[j]
    decreases |st|
  {
    StoreProjectedPre_implies_PackKeysPre(st, m);
    if |st| == 0 {
    } else {
      var g0 := R.Pack(st[0].k[0].z, st[0].k[1].z);
      Projected_Pack_nodup(st[1..], m);
      forall i | 0 <= i < |st[1..]|
        ensures PackKeysOfProjected(ProjectedRowsOfStore(st[1..], m))[i] != g0
      {
        Projected_Pack_index_in_m(st[1..], m, i);
        var gi := R.Pack(st[i + 1].k[0].z, st[i + 1].k[1].z);
        assert PackKeysOfProjected(ProjectedRowsOfStore(st[1..], m))[i] == gi;
        assert !RelEval.ListEq(st[0].k, st[i + 1].k);
        assert !(st[0].k[0].z == st[i + 1].k[0].z &&
                 st[0].k[1].z == st[i + 1].k[1].z) by {
          if st[0].k[0].z == st[i + 1].k[0].z &&
             st[0].k[1].z == st[i + 1].k[1].z {
            assert st[0].k == st[i + 1].k;
            RelEval.ListEq_refl(st[0].k);
            assert RelEval.ListEq(st[0].k, st[i + 1].k);
            assert false;
          }
        }
        Pack_inj(st[0].k[0].z, st[0].k[1].z,
                 st[i + 1].k[0].z, st[i + 1].k[1].z);
        assert g0 != gi;
      }
    }
  }

  lemma Projected_Pack_find(st: RelEval.GroupStore, m: R.KeyStore, g: int)
    requires ListStoreRel(st, m)
    requires StoreProjectedPre(st, m)
    requires forall gg :: gg in m ==> 0 <= gg < 65536
    requires g in m
    ensures ProjectedRowsOk(ProjectedRowsOfStore(st, m))
    ensures |PackKeysOfProjected(ProjectedRowsOfStore(st, m))| == |st|
    ensures exists i :: 0 <= i < |st| &&
              PackKeysOfProjected(ProjectedRowsOfStore(st, m))[i] == g
    decreases |st|
  {
    reveal ListStoreRel();
    StoreProjectedPre_implies_PackKeysPre(st, m);
    Unpack_pack(g);
    var rf := g / 256;
    var ls := g % 256;
    assert RelEval.StoreGet(st, [VInt(rf), VInt(ls)]).SomeAcc?;
    Projected_Pack_find_walk(st, m, rf, ls);
  }

  lemma Projected_Pack_find_walk(
      st: RelEval.GroupStore, m: R.KeyStore, rf: int, ls: int)
    requires StoreProjectedPre(st, m)
    requires 0 <= rf < 256 && 0 <= ls < 256
    requires RelEval.StoreGet(st, [VInt(rf), VInt(ls)]).SomeAcc?
    ensures ProjectedRowsOk(ProjectedRowsOfStore(st, m))
    ensures |PackKeysOfProjected(ProjectedRowsOfStore(st, m))| == |st|
    ensures exists i :: 0 <= i < |st| &&
              PackKeysOfProjected(ProjectedRowsOfStore(st, m))[i]
                == R.Pack(rf, ls)
    decreases |st|
  {
    StoreProjectedPre_implies_PackKeysPre(st, m);
    var g := R.Pack(rf, ls);
    var k := [VInt(rf), VInt(ls)];
    if |st| == 0 {
      assert RelEval.StoreGet(st, k).NoAcc?;
      assert false;
    } else if RelEval.ListEq(st[0].k, k) {
      assert st[0].k == [VInt(st[0].k[0].z), VInt(st[0].k[1].z)];
      ListEq_VInt2(st[0].k[0].z, st[0].k[1].z, rf, ls);
      assert st[0].k[0].z == rf && st[0].k[1].z == ls;
      Projected_Pack_index_in_m(st, m, 0);
      assert PackKeysOfProjected(ProjectedRowsOfStore(st, m))[0] == g;
    } else {
      assert RelEval.StoreGet(st, k) == RelEval.StoreGet(st[1..], k);
      Projected_Pack_find_walk(st[1..], m, rf, ls);
      var j :| 0 <= j < |st[1..]| &&
               PackKeysOfProjected(ProjectedRowsOfStore(st[1..], m))[j] == g;
      assert PackKeysOfProjected(ProjectedRowsOfStore(st, m))[j + 1] == g;
    }
  }

  lemma H_AdaptRel_eq_FinalizeKeys(rows: seq<Row>, m: R.KeyStore)
    requires ProjectedRowsOk(rows)
    requires forall i :: 0 <= i < |rows| ==>
               var g := R.Pack(rows[i][0].z, rows[i][1].z);
               g in m && m[g].count > 0 &&
               rows[i] == ProjectedRowOfStats(rows[i][0].z, rows[i][1].z, m[g])
    ensures Q1AdaptRel(rows)
         == R.FinalizeKeys(PackKeysOfProjected(rows), m)
    decreases |rows|
  {
    ProjectedRowsOk_implies_PackKeysPre(rows);
    if |rows| == 0 {
    } else {
      var rf := rows[0][0].z;
      var ls := rows[0][1].z;
      var g := R.Pack(rf, ls);
      assert m[g].count > 0;
      Q1AdaptRowRel_of_finalized(rf, ls, m[g]);
      assert R.GetKey(m, g) == m[g];
      assert R.SpecFinalizeOne(g, R.GetKey(m, g)) == R.SpecFinalizeOne(g, m[g]);
      assert ProjectedRowsOk(rows[1..]);
      H_AdaptRel_eq_FinalizeKeys(rows[1..], m);
    }
  }

  // =====================================================================
  // D4. ResultRowSort by (rf,ls) ≡ Pack ascending
  // =====================================================================

  lemma OrderKeyLe_Q1_0(c: int, ra: Row, rb: Row)
    requires |ra| >= 1 && |rb| >= 1 && ra[0].VInt? && rb[0].VInt?
    ensures RelEval.OrderKeyLe(ParamCutoff(c), Q1OrderKeysRel()[0], ra, rb)
         == RelEval.SomeBool(ra[0].z <= rb[0].z)
  {
  }

  lemma OrderKeyLe_Q1_1(c: int, ra: Row, rb: Row)
    requires |ra| >= 2 && |rb| >= 2 && ra[1].VInt? && rb[1].VInt?
    ensures RelEval.OrderKeyLe(ParamCutoff(c), Q1OrderKeysRel()[1], ra, rb)
         == RelEval.SomeBool(ra[1].z <= rb[1].z)
  {
  }

  lemma OrderKeysLe_Q1_eq_Pack(c: int, ra: Row, rb: Row)
    requires |ra| >= 2 && |rb| >= 2
    requires ra[0].VInt? && ra[1].VInt? && rb[0].VInt? && rb[1].VInt?
    requires 0 <= ra[0].z < 256 && 0 <= ra[1].z < 256
    requires 0 <= rb[0].z < 256 && 0 <= rb[1].z < 256
    ensures RelEval.OrderKeysLe(ParamCutoff(c), Q1OrderKeysRel(), ra, rb)
         == RelEval.SomeBool(
              R.Pack(ra[0].z, ra[1].z) <= R.Pack(rb[0].z, rb[1].z))
  {
    OrderKeyLe_Q1_0(c, ra, rb);
    OrderKeyLe_Q1_0(c, rb, ra);
    OrderKeyLe_Q1_1(c, ra, rb);
    OrderKeyLe_Q1_1(c, rb, ra);
    Pack_order(ra[0].z, ra[1].z, rb[0].z, rb[1].z);
  }

  ghost predicate PackKeysStrictAsc(rows: seq<Row>)
    requires ProjectedRowsOk(rows)
  {
    forall i :: 0 <= i < |rows| - 1 ==>
      R.Pack(rows[i][0].z, rows[i][1].z)
        < R.Pack(rows[i + 1][0].z, rows[i + 1][1].z)
  }

  lemma PackKeysStrictAsc_head_lt(rows: seq<Row>, k: nat)
    requires ProjectedRowsOk(rows)
    requires PackKeysStrictAsc(rows)
    requires 0 < k < |rows|
    ensures R.Pack(rows[0][0].z, rows[0][1].z)
          < R.Pack(rows[k][0].z, rows[k][1].z)
    decreases k
  {
    if k == 1 {
    } else {
      PackKeysStrictAsc_head_lt(rows, k - 1);
      assert R.Pack(rows[k - 1][0].z, rows[k - 1][1].z)
           < R.Pack(rows[k][0].z, rows[k][1].z);
    }
  }

  lemma Multiset_cancel_singleton<T>(a: multiset<T>, b: multiset<T>, x: T)
    requires a + multiset{x} == b + multiset{x}
    ensures a == b
  {
    forall y
      ensures a[y] == b[y]
    {
      assert (a + multiset{x})[y] == (b + multiset{x})[y];
      if y == x {
        assert (a + multiset{x})[x] == a[x] + 1;
        assert (b + multiset{x})[x] == b[x] + 1;
      } else {
        assert (a + multiset{x})[y] == a[y];
        assert (b + multiset{x})[y] == b[y];
      }
    }
  }

  lemma RowInsert_Q1(
      c: int, x: Row, xs: seq<Row>)
    requires ProjectedRowsOk([x])
    requires ProjectedRowsOk(xs)
    requires PackKeysStrictAsc(xs)
    requires |x| == 10 && x[0].VInt? && x[1].VInt?
    requires 0 <= x[0].z < 256 && 0 <= x[1].z < 256
    requires forall i :: 0 <= i < |xs| ==>
               |xs[i]| == 10 && xs[i][0].VInt? && xs[i][1].VInt? &&
               0 <= xs[i][0].z < 256 && 0 <= xs[i][1].z < 256 &&
               R.Pack(x[0].z, x[1].z) != R.Pack(xs[i][0].z, xs[i][1].z)
    ensures RelEval.RowInsert(ParamCutoff(c), Q1OrderKeysRel(), x, xs).SomeRows?
    ensures var ys := RelEval.RowInsert(ParamCutoff(c), Q1OrderKeysRel(), x, xs).rs;
            ProjectedRowsOk(ys) &&
            |ys| == |xs| + 1 &&
            PackKeysStrictAsc(ys) &&
            multiset(ys) == multiset([x] + xs)
    decreases |xs|
  {
    ProjectedRowsOk_implies_PackKeysPre([x]);
    ProjectedRowsOk_implies_PackKeysPre(xs);
    if |xs| == 0 {
      assert RelEval.RowInsert(ParamCutoff(c), Q1OrderKeysRel(), x, xs)
          == RelEval.SomeRows([x]);
      assert ProjectedRowsOk([x]);
      assert PackKeysStrictAsc([x]);
    } else {
      OrderKeysLe_Q1_eq_Pack(c, x, xs[0]);
      var px := R.Pack(x[0].z, x[1].z);
      var p0 := R.Pack(xs[0][0].z, xs[0][1].z);
      var le := RelEval.OrderKeysLe(ParamCutoff(c), Q1OrderKeysRel(), x, xs[0]);
      assert le == RelEval.SomeBool(px <= p0);
      if le.b {
        Pack_order(x[0].z, x[1].z, xs[0][0].z, xs[0][1].z);
        assert px != p0;
        assert px < p0;
        assert RelEval.RowInsert(ParamCutoff(c), Q1OrderKeysRel(), x, xs)
            == RelEval.SomeRows([x] + xs);
        assert ProjectedRowsOk([x] + xs);
        assert PackKeysStrictAsc([x] + xs) by {
          assert px < p0;
          forall i | 0 <= i < |xs| - 1
            ensures R.Pack(xs[i][0].z, xs[i][1].z)
                  < R.Pack(xs[i + 1][0].z, xs[i + 1][1].z)
          {
          }
        }
        assert multiset([x] + xs) == multiset([x] + xs);
      } else {
        Pack_order(x[0].z, x[1].z, xs[0][0].z, xs[0][1].z);
        assert px != p0;
        assert p0 < px;
        assert ProjectedRowsOk(xs[1..]);
        assert PackKeysStrictAsc(xs[1..]);
        RowInsert_Q1(c, x, xs[1..]);
        var ys1 := RelEval.RowInsert(ParamCutoff(c), Q1OrderKeysRel(), x, xs[1..]).rs;
        assert RelEval.RowInsert(ParamCutoff(c), Q1OrderKeysRel(), x, xs)
            == RelEval.SomeRows([xs[0]] + ys1);
        assert ProjectedRowsOk([xs[0]] + ys1);
        assert PackKeysStrictAsc([xs[0]] + ys1) by {
          forall i | 0 <= i < |ys1|
            ensures p0 < R.Pack(ys1[i][0].z, ys1[i][1].z)
          {
            assert ys1[i] in multiset([x] + xs[1..]);
            if ys1[i] == x {
              assert p0 < px;
            } else {
              var t :| 0 <= t < |xs[1..]| && xs[1..][t] == ys1[i];
              assert xs[t + 1] == xs[1..][t] == ys1[i];
              PackKeysStrictAsc_head_lt(xs, t + 1);
              assert R.Pack(xs[0][0].z, xs[0][1].z)
                   < R.Pack(xs[t + 1][0].z, xs[t + 1][1].z);
            }
          }
        }
        assert multiset(ys1) == multiset([x] + xs[1..]);
        assert [xs[0]] + ys1 == [xs[0]] + ys1;
        assert ([xs[0]] + ys1) == [xs[0]] + ys1;
        assert multiset([xs[0]] + ys1) == multiset([xs[0]]) + multiset(ys1);
        assert multiset([xs[0]]) == multiset{xs[0]};
        assert xs == [xs[0]] + xs[1..];
        assert multiset(xs) == multiset([xs[0]]) + multiset(xs[1..]);
        assert [x] + xs == [x] + xs;
        assert multiset([x] + xs) == multiset([x]) + multiset(xs);
        assert multiset([x]) == multiset{x};
        assert multiset([x] + xs[1..]) == multiset([x]) + multiset(xs[1..]);
        assert multiset([x] + xs)
            == multiset{x} + multiset{xs[0]} + multiset(xs[1..]);
        assert multiset([xs[0]] + ys1)
            == multiset{xs[0]} + multiset{x} + multiset(xs[1..]);
        assert multiset([xs[0]] + ys1) == multiset([x] + xs);
      }
    }
  }

  lemma ResultRowSort_Q1(c: int, rows: seq<Row>)
    requires ProjectedRowsOk(rows)
    requires forall i, j :: 0 <= i < j < |rows| ==>
               (R.Pack(rows[i][0].z, rows[i][1].z)
                != R.Pack(rows[j][0].z, rows[j][1].z))
    ensures RelEval.ResultRowSort(ParamCutoff(c), Q1OrderKeysRel(), rows).SomeRows?
    ensures var sorted := RelEval.ResultRowSort(ParamCutoff(c), Q1OrderKeysRel(), rows).rs;
            ProjectedRowsOk(sorted) &&
            |sorted| == |rows| &&
            PackKeysStrictAsc(sorted) &&
            multiset(sorted) == multiset(rows)
    decreases |rows|
  {
    ProjectedRowsOk_implies_PackKeysPre(rows);
    if |rows| == 0 {
      assert RelEval.ResultRowSort(ParamCutoff(c), Q1OrderKeysRel(), rows)
          == RelEval.SomeRows([]);
    } else {
      assert ProjectedRowsOk(rows[1..]);
      assert forall i, j :: 0 <= i < j < |rows[1..]| ==>
               (R.Pack(rows[1..][i][0].z, rows[1..][i][1].z)
                != R.Pack(rows[1..][j][0].z, rows[1..][j][1].z));
      ResultRowSort_Q1(c, rows[1..]);
      var xs := RelEval.ResultRowSort(ParamCutoff(c), Q1OrderKeysRel(), rows[1..]).rs;
      assert ProjectedRowsOk(xs);
      assert multiset(xs) == multiset(rows[1..]);
      var p0 := R.Pack(rows[0][0].z, rows[0][1].z);
      forall i | 0 <= i < |xs|
        ensures |xs[i]| == 10 && xs[i][0].VInt? && xs[i][1].VInt?
        ensures 0 <= xs[i][0].z < 256 && 0 <= xs[i][1].z < 256
        ensures p0 != R.Pack(xs[i][0].z, xs[i][1].z)
      {
        assert xs[i] in multiset(rows[1..]);
        var j :| 0 <= j < |rows[1..]| && rows[1..][j] == xs[i];
        assert rows[j + 1] == xs[i];
        assert p0 != R.Pack(rows[j + 1][0].z, rows[j + 1][1].z);
      }
      assert ProjectedRowsOk([rows[0]]);
      RowInsert_Q1(c, rows[0], xs);
      var sorted := RelEval.ResultRowSort(ParamCutoff(c), Q1OrderKeysRel(), rows).rs;
      assert sorted
          == RelEval.RowInsert(ParamCutoff(c), Q1OrderKeysRel(), rows[0], xs).rs;
      assert multiset(sorted) == multiset([rows[0]] + xs);
      assert multiset([rows[0]] + xs) == multiset([rows[0]]) + multiset(xs);
      assert multiset([rows[0]]) == multiset{rows[0]};
      assert rows == [rows[0]] + rows[1..];
      assert multiset(rows) == multiset([rows[0]]) + multiset(rows[1..]);
      assert multiset(rows) == multiset{rows[0]} + multiset(rows[1..]);
      assert multiset(sorted) == multiset(rows);
    }
  }

  lemma Multiset_seq_cons<T>(s: seq<T>)
    requires |s| > 0
    ensures multiset(s) == multiset{s[0]} + multiset(s[1..])
  {
    assert s == [s[0]] + s[1..];
    assert multiset(s) == multiset([s[0]] + s[1..]);
    assert multiset([s[0]] + s[1..]) == multiset([s[0]]) + multiset(s[1..]);
    assert multiset([s[0]]) == multiset{s[0]};
  }

  lemma Multiset_seq_remove_at<T>(t: seq<T>, j: nat)
    requires j < |t|
    ensures multiset(t) == multiset{t[j]} + multiset(t[..j] + t[j + 1..])
  {
    assert t == t[..j] + [t[j]] + t[j + 1..];
    assert multiset(t)
        == multiset(t[..j]) + multiset([t[j]]) + multiset(t[j + 1..]);
    assert multiset([t[j]]) == multiset{t[j]};
    assert multiset(t[..j] + t[j + 1..])
        == multiset(t[..j]) + multiset(t[j + 1..]);
  }

  lemma PackKeys_index(rows: seq<Row>, i: nat)
    requires ProjectedRowsOk(rows)
    requires i < |rows|
    ensures |PackKeysOfProjected(rows)| == |rows|
    ensures PackKeysOfProjected(rows)[i]
         == R.Pack(rows[i][0].z, rows[i][1].z)
    decreases i
  {
    ProjectedRowsOk_implies_PackKeysPre(rows);
    PackKeysOfProjected_len(rows);
    if i == 0 {
    } else {
      assert ProjectedRowsOk(rows[1..]);
      PackKeys_index(rows[1..], i - 1);
    }
  }

  lemma PackKeys_multiset_remove_at(rows: seq<Row>, j: nat)
    requires ProjectedRowsOk(rows)
    requires j < |rows|
    ensures var rest := rows[..j] + rows[j + 1..];
            ProjectedRowsOk(rest) &&
            multiset(PackKeysOfProjected(rows))
              == multiset{R.Pack(rows[j][0].z, rows[j][1].z)}
                 + multiset(PackKeysOfProjected(rest))
    decreases |rows|
  {
    ProjectedRowsOk_implies_PackKeysPre(rows);
    PackKeysOfProjected_len(rows);
    var rest := rows[..j] + rows[j + 1..];
    assert ProjectedRowsOk(rest) by {
      forall i | 0 <= i < |rest|
        ensures |rest[i]| == 10 && rest[i][0].VInt? && rest[i][1].VInt?
                && rest[i][2].VInt? && rest[i][3].VInt?
                && rest[i][4].VInt? && rest[i][5].VInt?
                && rest[i][6].VAvgExact? && rest[i][7].VAvgExact?
                && rest[i][8].VAvgExact? && rest[i][9].VInt?
                && 0 <= rest[i][0].z < 256 && 0 <= rest[i][1].z < 256
                && rest[i][9].z > 0
      {
        if i < j {
          assert rest[i] == rows[i];
        } else {
          assert rest[i] == rows[i + 1];
        }
      }
    }
    if j == 0 {
      Multiset_seq_cons(PackKeysOfProjected(rows));
      assert PackKeysOfProjected(rows)
          == [R.Pack(rows[0][0].z, rows[0][1].z)] + PackKeysOfProjected(rows[1..]);
      assert rest == rows[1..];
    } else {
      // PackKeys(rows) = [pack(rows[0])] + PackKeys(rows[1..])
      // remove at j>0 <=> remove at j-1 in tail
      PackKeys_multiset_remove_at(rows[1..], j - 1);
      var rest1 := rows[1..][..(j - 1)] + rows[1..][(j - 1) + 1..];
      assert rest == [rows[0]] + rest1;
      ProjectedRowsOk_implies_PackKeysPre(rest);
      Multiset_seq_cons(PackKeysOfProjected(rows));
      Multiset_seq_cons(PackKeysOfProjected(rest));
    }
  }

  lemma PackKeys_multiset_of_rows_perm(a: seq<Row>, b: seq<Row>)
    requires ProjectedRowsOk(a) && ProjectedRowsOk(b)
    requires multiset(a) == multiset(b)
    ensures multiset(PackKeysOfProjected(a)) == multiset(PackKeysOfProjected(b))
    decreases |a|
  {
    ProjectedRowsOk_implies_PackKeysPre(a);
    ProjectedRowsOk_implies_PackKeysPre(b);
    if |a| == 0 {
      assert |b| == 0;
    } else {
      assert a == [a[0]] + a[1..];
      Multiset_seq_cons(a);
      assert a[0] in multiset(b);
      var j :| 0 <= j < |b| && b[j] == a[0];
      Multiset_seq_remove_at(b, j);
      var b2 := b[..j] + b[j + 1..];
      assert a[0] == b[j];
      calc {
        multiset(a[1..]) + multiset{a[0]};
      ==
        multiset(a);
      ==
        multiset(b);
      ==
        multiset{b[j]} + multiset(b2);
      ==
        multiset{a[0]} + multiset(b2);
      ==
        multiset(b2) + multiset{a[0]};
      }
      Multiset_cancel_singleton(multiset(a[1..]), multiset(b2), a[0]);
      assert ProjectedRowsOk(a[1..]);
      PackKeys_multiset_remove_at(b, j);
      assert ProjectedRowsOk(b2);
      PackKeys_multiset_of_rows_perm(a[1..], b2);
      Multiset_seq_cons(PackKeysOfProjected(a));
      assert R.Pack(a[0][0].z, a[0][1].z) == R.Pack(b[j][0].z, b[j][1].z);
      assert multiset(PackKeysOfProjected(a))
          == multiset{R.Pack(a[0][0].z, a[0][1].z)}
             + multiset(PackKeysOfProjected(a[1..]));
      assert multiset(PackKeysOfProjected(b))
          == multiset{R.Pack(b[j][0].z, b[j][1].z)}
             + multiset(PackKeysOfProjected(b2));
    }
  }

  lemma PackKeysStrictAsc_StrictAsc(rows: seq<Row>)
    requires ProjectedRowsOk(rows)
    requires PackKeysStrictAsc(rows)
    ensures FlatEval.StrictAsc(PackKeysOfProjected(rows))
  {
    ProjectedRowsOk_implies_PackKeysPre(rows);
    PackKeysOfProjected_len(rows);
    var keys := PackKeysOfProjected(rows);
    forall i | 0 <= i < |keys| - 1
      ensures keys[i] < keys[i + 1]
    {
      PackKeys_index(rows, i);
      PackKeys_index(rows, i + 1);
      assert keys[i] == R.Pack(rows[i][0].z, rows[i][1].z);
      assert keys[i + 1] == R.Pack(rows[i + 1][0].z, rows[i + 1][1].z);
    }
  }

  lemma Sorted_Pack_eq_KeysFrom(
      st: RelEval.GroupStore, m: R.KeyStore, rows: seq<Row>, sorted: seq<Row>)
    requires ListStoreRel(st, m)
    requires StoreProjectedPre(st, m)
    requires forall g :: g in m ==> 0 <= g < 65536
    requires rows == ProjectedRowsOfStore(st, m)
    requires ProjectedRowsOk(rows)
    requires ProjectedRowsOk(sorted)
    requires PackKeysStrictAsc(sorted)
    requires multiset(sorted) == multiset(rows)
    ensures PackKeysOfProjected(sorted) == R.KeysFrom(0, 65536, m)
  {
    ProjectedRowsOfStore_ok(st, m);
    ProjectedRowsOk_implies_PackKeysPre(rows);
    ProjectedRowsOk_implies_PackKeysPre(sorted);
    PackKeysOfProjected_len(rows);
    PackKeysOfProjected_len(sorted);
    Projected_Pack_nodup(st, m);
    PackKeys_multiset_of_rows_perm(sorted, rows);
    var a := PackKeysOfProjected(sorted);
    var b := R.KeysFrom(0, 65536, m);
    PackKeysStrictAsc_StrictAsc(sorted);
    FlatEval.G_KeysFrom_eq_SortedInts(m);
    assert b == FlatEval.SortedInts(m.Keys);
    FlatEval.SortedInts_strict_asc(m.Keys);
    FlatEval.G_StrictAsc_nodup(a);
    FlatEval.G_StrictAsc_nodup(b);

    forall i | 0 <= i < |a|
      ensures a[i] in m.Keys
    {
      assert a[i] in multiset(PackKeysOfProjected(rows));
      var j :| 0 <= j < |PackKeysOfProjected(rows)| &&
               PackKeysOfProjected(rows)[j] == a[i];
      assert j < |st|;
      Projected_Pack_index_in_m(st, m, j);
    }
    forall g | g in m.Keys
      ensures g in a
    {
      Projected_Pack_find(st, m, g);
      var j :| 0 <= j < |st| && PackKeysOfProjected(rows)[j] == g;
      assert j < |PackKeysOfProjected(rows)|;
      assert g in multiset(PackKeysOfProjected(rows));
      assert g in multiset(a);
    }
    forall g | g in m.Keys
      ensures exists i :: 0 <= i < |b| && b[i] == g
    {
      R.KeysFrom_contains_fwd(0, 65536, m, g);
      assert R.KeyInKeysFrom(0, 65536, m, g);
    }
    R.KeysFrom_all_in_m(0, 65536, m);
    forall i | 0 <= i < |b|
      ensures b[i] in m.Keys
    {
    }
    // seq-membership vs exists-index: connect for G_seq_set_card
    forall i | 0 <= i < |b|
      ensures b[i] in m.Keys
    {
      assert b[i] in m;
    }
    FlatEval.G_seq_set_card(a, m.Keys);
    FlatEval.G_seq_set_card(b, m.Keys);
    assert forall x :: (x in a) <==> (x in m.Keys);
    assert forall x :: (x in b) <==> (x in m.Keys) by {
      forall x
        ensures (x in b) <==> (x in m.Keys)
      {
        if x in m.Keys {
          R.KeysFrom_contains_fwd(0, 65536, m, x);
          assert R.KeyInKeysFrom(0, 65536, m, x);
          // KeyInKeysFrom unfolds to exists i
        }
        if x in b {
          assert x in m;
        }
      }
    }
    FlatEval.G_AscSeq_unique(a, b);
  }

  lemma H_Adapt_sorted_eq_Finalize(
      st: RelEval.GroupStore, m: R.KeyStore, rows: seq<Row>, sorted: seq<Row>)
    requires ListStoreRel(st, m)
    requires StoreProjectedPre(st, m)
    requires forall g :: g in m ==> 0 <= g < 65536 && m[g].count > 0
    requires rows == ProjectedRowsOfStore(st, m)
    requires ProjectedRowsOk(rows)
    requires ProjectedRowsOk(sorted)
    requires PackKeysStrictAsc(sorted)
    requires multiset(sorted) == multiset(rows)
    ensures Q1AdaptRel(sorted)
         == R.FinalizeKeys(R.KeysFrom(0, 65536, m), m)
  {
    ProjectedRowsOfStore_ok(st, m);
    Sorted_Pack_eq_KeysFrom(st, m, rows, sorted);
    forall i | 0 <= i < |sorted|
      ensures var g := R.Pack(sorted[i][0].z, sorted[i][1].z);
              g in m && m[g].count > 0 &&
              sorted[i] == ProjectedRowOfStats(sorted[i][0].z, sorted[i][1].z, m[g])
    {
      assert sorted[i] in multiset(rows);
      var j :| 0 <= j < |rows| && rows[j] == sorted[i];
      assert j < |st|;
      Projected_row_at(st, m, j);
      var g := R.Pack(sorted[i][0].z, sorted[i][1].z);
      assert g == R.Pack(st[j].k[0].z, st[j].k[1].z);
      assert g in m && m[g].count > 0;
    }
    H_AdaptRel_eq_FinalizeKeys(sorted, m);
    assert PackKeysOfProjected(sorted) == R.KeysFrom(0, 65536, m);
  }

  // =====================================================================
  // E. Top-level: OutRows_rel == EvalResultSpec == flat OutRows
  // =====================================================================

  lemma FilterCutoff_byte_keys(db: seq<R.Row>, c: int)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures forall i :: 0 <= i < |R.FilterCutoff(db, c)| ==>
              0 <= R.FilterCutoff(db, c)[i].rf < 256 &&
              0 <= R.FilterCutoff(db, c)[i].ls < 256
    decreases |db|
  {
    if |db| == 0 {
    } else {
      FilterCutoff_byte_keys(db[..|db| - 1], c);
    }
  }

  lemma EvalRel_scan_lineitem(c: int, db: seq<R.Row>)
    ensures RelEval.EvalRel(RelScan(TableLineitem), ParamCutoff(c), DbOfAux(db))
         == RelEval.SomeRel(RelEval.RVRows(RowsOfAux(db)))
  {
  }

  lemma EvalRel_filter_Q1(c: int, db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures RelEval.EvalRel(
              RelFilter(RelScan(TableLineitem), Q1PredShipdate()),
              ParamCutoff(c), DbOfAux(db))
         == RelEval.SomeRel(RelEval.RVRows(RowsOfAux(R.FilterCutoff(db, c))))
  {
    EvalRel_scan_lineitem(c, db);
    FilterRows_rel_eq_FilterCutoff(c, db);
  }

  lemma Eval_Spec_Q1_rel_unfold_to_sorted(
      c: int, db: seq<R.Row>, st: RelEval.GroupStore, m: R.KeyStore, sorted: seq<Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    requires var filtered := R.FilterCutoff(db, c);
             RelEval.FoldGroups(Q1AggsRel(), Q1GroupExprs(), ParamCutoff(c),
               RowsOfAux(filtered), []).SomeStore? &&
             RelEval.FoldGroups(Q1AggsRel(), Q1GroupExprs(), ParamCutoff(c),
               RowsOfAux(filtered), []).st == st &&
             m == R.FoldByGroup(filtered) &&
             StoreProjectedPre(st, m) &&
             var rows := ProjectedRowsOfStore(st, m);
             RelEval.ResultRowSort(ParamCutoff(c), Q1OrderKeysRel(), rows)
               == RelEval.SomeRows(sorted)
    ensures RelEval.Eval_Spec(LogicalSpec_Q1_rel(), ParamCutoff(c), DbOfAux(db))
         == RelEval.SomeRows(sorted)
  {
    FilterCutoff_byte_keys(db, c);
    EvalRel_filter_Q1(c, db);
    var filtered := R.FilterCutoff(db, c);
    StoreToAggBags_Project_ListStoreRel(c, st, m);
    var bags := RelEval.StoreToAggBags(Q1AggsRel(), st).bags;
    var rows := ProjectedRowsOfStore(st, m);
    reveal RelEval.Eval_Spec();
    // EvalRel path: filter → aggregate → project → sort
    assert RelEval.RunAggregate(Q1AggsRel(), Q1GroupExprs(), ParamCutoff(c),
             RowsOfAux(filtered))
        == RelEval.StoreToAggBags(Q1AggsRel(), st);
    assert RelEval.EvalRel(
             RelAggregate(
               RelFilter(RelScan(TableLineitem), Q1PredShipdate()),
               Q1GroupExprs(), Q1AggsRel()),
             ParamCutoff(c), DbOfAux(db))
        == RelEval.SomeRel(RelEval.RVAgg(bags));
    assert RelEval.EvalRel(
             RelProject(
               RelAggregate(
                 RelFilter(RelScan(TableLineitem), Q1PredShipdate()),
                 Q1GroupExprs(), Q1AggsRel()),
               Q1ProjectExprs()),
             ParamCutoff(c), DbOfAux(db))
        == RelEval.SomeRel(RelEval.RVRows(rows));
    assert RelEval.EvalRel(LogicalSpec_Q1_rel(), ParamCutoff(c), DbOfAux(db))
        == RelEval.SomeRel(RelEval.RVRows(sorted));
  }

  lemma Eval_Spec_Q1_OutRows_rel_eq_EvalResultSpec(c: int, db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures Eval_Spec_Q1_OutRows_rel(c, db) == R.EvalResultSpec(db, c)
  {
    FilterCutoff_byte_keys(db, c);
    var filtered := R.FilterCutoff(db, c);
    FoldGroups_from_empty_ListStoreRel(c, filtered);
    var stOpt := RelEval.FoldGroups(
      Q1AggsRel(), Q1GroupExprs(), ParamCutoff(c), RowsOfAux(filtered), []);
    var st := stOpt.st;
    var m := R.FoldByGroup(filtered);
    assert ListStoreRel(st, m);
    assert m == R.SpecView(db, c);
    R.FoldByGroup_positive_counts(filtered);
    R.FoldByGroup_keys_in_pack_range(filtered);
    assert forall g :: g in m ==> m[g].count > 0 && 0 <= g < 65536;
    ListStoreRel_to_ProjectedPre(st, m);
    assert StoreProjectedPre(st, m);

    var rows := ProjectedRowsOfStore(st, m);
    ProjectedRowsOfStore_ok(st, m);
    Projected_Pack_nodup(st, m);
    assert ProjectedRowsOk(rows);
    ProjectedRowsOk_implies_PackKeysPre(rows);
    PackKeysOfProjected_len(rows);
    Projected_Pack_nodup(st, m);
    forall i, j | 0 <= i < j < |rows|
      ensures R.Pack(rows[i][0].z, rows[i][1].z)
           != R.Pack(rows[j][0].z, rows[j][1].z)
    {
      PackKeys_index(rows, i);
      PackKeys_index(rows, j);
      Projected_Pack_index_in_m(st, m, i);
      Projected_Pack_index_in_m(st, m, j);
      assert PackKeysOfProjected(rows)[i]
          == R.Pack(st[i].k[0].z, st[i].k[1].z);
      assert PackKeysOfProjected(rows)[j]
          == R.Pack(st[j].k[0].z, st[j].k[1].z);
      assert PackKeysOfProjected(rows)[i] != PackKeysOfProjected(rows)[j];
    }
    ResultRowSort_Q1(c, rows);
    var sorted := RelEval.ResultRowSort(ParamCutoff(c), Q1OrderKeysRel(), rows).rs;

    Eval_Spec_Q1_rel_unfold_to_sorted(c, db, st, m, sorted);
    assert RelEval.Eval_Spec(LogicalSpec_Q1_rel(), ParamCutoff(c), DbOfAux(db))
        == RelEval.SomeRows(sorted);
    assert Eval_Spec_Q1_OutRows_rel(c, db) == Q1AdaptRel(sorted);

    H_Adapt_sorted_eq_Finalize(st, m, rows, sorted);
    assert Q1AdaptRel(sorted)
        == R.FinalizeKeys(R.KeysFrom(0, 65536, m), m);
    assert R.EvalResultSpec(db, c)
        == R.FinalizeKeys(R.KeysFrom(0, 65536, m), m);
  }

  lemma Eval_Spec_Q1_OutRows_rel_eq_flat(c: int, db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures Eval_Spec_Q1_OutRows_rel(c, db)
         == FlatEval.Eval_Spec_Q1_OutRows(LSD.ParamsOfCutoff(c), db)
  {
    Eval_Spec_Q1_OutRows_rel_eq_EvalResultSpec(c, db);
    FlatEval.Eval_Spec_Q1_eq_EvalResultSpec(c, db);
  }
}
