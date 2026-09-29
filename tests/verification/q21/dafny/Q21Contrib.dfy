// Q21 per-order contribution algebra.
//
// SummaryOf(L, ok)  : per-supplier (total, late) lineitem counts of order ok —
//                     the mathematical content of C++ OrderSummary::suppliers.
// ContribMap(st, S) : C++ TpchQ21ComputeOrderContributions (math form).
// Cv(T, ok, sk)     : contribution of order ok to supplier sk in T.
//
// Main results:
//   PerOrder     : #qualifying lineitems of (ok, sk) == Cv(T, ok, sk)
//   NW_by_orders : NW(T, sk) == SetSum(OKeys(T), ok => Cv(T, ok, sk))

include "Q21Count.dfy"
include "Q21SetSum.dfy"

module Q21Contrib {
  import opened Q21Types
  import opened Q21SqlCore
  import opened Q21Count
  import opened Q21SetSum

  ghost function Ind(b: bool): int { if b then 1 else 0 }

  ghost function SupsOf(L: seq<Li>, ok: int): set<int>
  { set l | l in L && l.ok == ok :: l.sk }

  ghost function Tot(L: seq<Li>, ok: int, sk: int): int
  { SumF(L, (l: Li) => if l.ok == ok && l.sk == sk then 1 else 0) }

  ghost function LateC(L: seq<Li>, ok: int, sk: int): int
  { SumF(L, (l: Li) => if l.ok == ok && l.sk == sk && Late(l) then 1 else 0) }

  ghost function {:opaque} SummaryOf(L: seq<Li>, ok: int): map<int, SOI>
  { map sk | sk in SupsOf(L, ok) :: SOI(Tot(L, ok, sk), LateC(L, ok, sk)) }

  ghost function LateSet(S: map<int, SOI>): set<int>
  { set sk | sk in S && S[sk].total > 0 && S[sk].late > 0 }

  ghost function ContribMap(st: char, S: map<int, SOI>): map<int, int>
  {
    if st != 'F' || |S| <= 1 then map[]
    else if |LateSet(S)| == 1 then
      var x :| x in LateSet(S);
      map[x := S[x].late]
    else map[]
  }

  ghost function ContribVal(st: char, S: map<int, SOI>, sk: int): int
  { var c := ContribMap(st, S); if sk in c then c[sk] else 0 }

  function StatusOfSeq(O: seq<Ord>, ok: int): char
  { if |O| == 0 then 0 as char else if O[0].ok == ok then O[0].st else StatusOfSeq(O[1..], ok) }

  ghost function StatusOf(T: TDB, ok: int): char { StatusOfSeq(T.ords, ok) }

  ghost function Cv(T: TDB, ok: int, sk: int): int
  { ContribVal(StatusOf(T, ok), SummaryOf(T.li, ok), sk) }

  ghost function OKeys(T: TDB): set<int> { set o | o in T.ords :: o.ok }

  // ----- Basic membership / positivity facts -----

  lemma SumF_ind_pos(L: seq<Li>, P: Li -> bool)
    ensures SumF(L, (l: Li) => if P(l) then 1 else 0) >= 0
    ensures SumF(L, (l: Li) => if P(l) then 1 else 0) > 0 <==> exists l :: l in L && P(l)
  {
    if |L| > 0 {
      SumF_ind_pos(L[1..], P);
      assert forall l :: l in L <==> (l == L[0] || l in L[1..]);
    }
  }

  lemma LateLis_mem(L: seq<Li>, l: Li)
    ensures l in LateLis(L) <==> (l in L && Late(l))
  {
    if |L| > 0 {
      LateLis_mem(L[1..], l);
      assert forall x :: x in L <==> (x == L[0] || x in L[1..]);
    }
  }

  lemma ExOther_iff(l: Li, L: seq<Li>)
    ensures ExOther(l, L) <==> exists l2 :: l2 in L && OtherL(l, l2)
  {
    if |L| > 0 {
      ExOther_iff(l, L[1..]);
      assert forall x :: x in L <==> (x == L[0] || x in L[1..]);
    }
  }

  lemma StatusOf_unique(O: seq<Ord>, ok: int, c: char)
    requires forall i, j :: 0 <= i < j < |O| ==> O[i].ok != O[j].ok
    requires c != 0 as char
    ensures (exists o :: o in O && o.ok == ok && o.st == c) <==> StatusOfSeq(O, ok) == c
  {
    if |O| > 0 {
      StatusOf_unique(O[1..], ok, c);
      assert forall x :: x in O <==> (x == O[0] || x in O[1..]);
      if O[0].ok == ok {
        forall o | o in O[1..] ensures o.ok != ok {
          var j :| 0 <= j < |O[1..]| && O[1..][j] == o;
          assert O[0].ok != O[j + 1].ok;
        }
      }
    }
  }

  lemma OrdF_status(T: TDB, ok: int)
    requires OrdKeyUnique(T)
    ensures OrdF(T, ok) <==> StatusOf(T, ok) == 'F'
  {
    StatusOf_unique(T.ords, ok, 'F');
  }

  lemma LateSet_summary(L: seq<Li>, ok: int, sk: int)
    ensures sk in LateSet(SummaryOf(L, ok)) <==>
            exists l :: l in L && l.ok == ok && l.sk == sk && Late(l)
  {
    reveal SummaryOf();
    var pl: Li -> bool := (l: Li) => l.ok == ok && l.sk == sk && Late(l);
    var pt: Li -> bool := (l: Li) => l.ok == ok && l.sk == sk;
    SumF_ind_pos(L, pl);
    SumF_ind_pos(L, pt);
    SumF_ext(L, (l: Li) => if pl(l) then 1 else 0,
                (l: Li) => if l.ok == ok && l.sk == sk && Late(l) then 1 else 0);
    SumF_ext(L, (l: Li) => if pt(l) then 1 else 0,
                (l: Li) => if l.ok == ok && l.sk == sk then 1 else 0);
    if exists l :: l in L && l.ok == ok && l.sk == sk && Late(l) {
      var l :| l in L && l.ok == ok && l.sk == sk && Late(l);
      assert sk in SupsOf(L, ok);
      assert pt(l) && pl(l);
    }
  }

  lemma Singleton(s: set<int>, x: int)
    requires |s| == 1 && x in s
    ensures s == {x}
  {
    forall y | y in s ensures y == x {
      if y != x { assert {x, y} <= s; SubsetCard({x, y}, s); }
    }
  }

  // ----- The per-order contribution lemma -----

  lemma PerOrder(T: TDB, ok: int, sk: int)
    requires OrdKeyUnique(T)
    ensures SumF(T.li, (l: Li) => if l.ok == ok && l.sk == sk && Qual(T, l) then 1 else 0)
            == Cv(T, ok, sk)
  {
    reveal SummaryOf();
    var L := T.li;
    var S := SummaryOf(L, ok);
    var st := StatusOf(T, ok);
    var LS := LateSet(S);
    OrdF_status(T, ok);
    var Cond := st == 'F' && |SupsOf(L, ok)| > 1 && LS == {sk};
    assert |S| == |SupsOf(L, ok)| by { assert S.Keys == SupsOf(L, ok); }
    var h: Li -> int := (l: Li) => if l.ok == ok && l.sk == sk && Qual(T, l) then 1 else 0;
    if Cond {
      // Every late lineitem of (ok, sk) qualifies.
      forall l | l in L
        ensures h(l) == (if l.ok == ok && l.sk == sk && Late(l) then 1 else 0)
      {
        if l.ok == ok && l.sk == sk && Late(l) {
          // Another supplier exists.
          var other :| other in SupsOf(L, ok) && other != sk by {
            var ss := SupsOf(L, ok);
            assert sk in ss by { assert l in L && l.ok == ok; }
            assert |ss - {sk}| > 0;
            var z :| z in ss - {sk};
          }
          var l2 :| l2 in L && l2.ok == ok && l2.sk == other;
          ExOther_iff(l, L);
          assert OtherL(l, l2);
          // No other supplier is late.
          ExOther_iff(l, LateLis(L));
          if ExOther(l, LateLis(L)) {
            var l3 :| l3 in LateLis(L) && OtherL(l, l3);
            LateLis_mem(L, l3);
            LateSet_summary(L, ok, l3.sk);
            assert l3.sk in LS;
            assert false;
          }
        }
      }
      SumF_ext(L, h, (l: Li) => if l.ok == ok && l.sk == sk && Late(l) then 1 else 0);
      assert sk in LS;
      assert |LS| == 1;
      assert ContribMap(st, S) == map[sk := S[sk].late];
    } else {
      forall l | l in L ensures h(l) == 0 {
        if l.ok == ok && l.sk == sk && Qual(T, l) {
          ExOther_iff(l, L);
          var l2 :| l2 in L && OtherL(l, l2);
          assert l.sk in SupsOf(L, ok) && l2.sk in SupsOf(L, ok);
          assert {l.sk, l2.sk} <= SupsOf(L, ok);
          assert |{l.sk, l2.sk}| == 2;
          SubsetCard({l.sk, l2.sk}, SupsOf(L, ok));
          LateSet_summary(L, ok, sk);
          forall z | z in LS ensures z == sk {
            LateSet_summary(L, ok, z);
            var l3 :| l3 in L && l3.ok == ok && l3.sk == z && Late(l3);
            if z != sk {
              LateLis_mem(L, l3);
              ExOther_iff(l, LateLis(L));
              assert OtherL(l, l3);
            }
          }
          assert LS == {sk};
          assert false;
        }
      }
      SumF_zero(L, h);
      if st == 'F' && |S| > 1 && |LS| == 1 {
        var x :| x in LS;
        assert LS == {x} by {
          forall y | y in LS ensures y == x {
            if y != x { assert {x, y} <= LS; SubsetCard({x, y}, LS); }
          }
        }
        assert x != sk;
      }
    }
  }

  lemma SubsetCard(a: set<int>, b: set<int>)
    requires a <= b
    ensures |a| <= |b|
  {
    assert b == a + (b - a);
  }

  // ----- NW as a sum over order keys -----

  lemma SetSum_point(K: set<int>, y: int, c: int)
    ensures SetSum(K, (ok: int) => if ok == y then c else 0) == (if y in K then c else 0)
  {
    if y in K {
      SetSum_remove(K, (ok: int) => if ok == y then c else 0, y);
      SetSum_zero(K - {y}, (ok: int) => if ok == y then c else 0);
    } else {
      SetSum_zero(K, (ok: int) => if ok == y then c else 0);
    }
  }

  lemma Sum_by_key(L: seq<Li>, K: set<int>, g: Li -> int)
    requires forall l :: l in L && g(l) != 0 ==> l.ok in K
    ensures SumF(L, g) == SetSum(K, (ok: int) => SumF(L, (l: Li) => if l.ok == ok then g(l) else 0))
  {
    if |L| == 0 {
      SetSum_zero(K, (ok: int) => SumF(L, (l: Li) => if l.ok == ok then g(l) else 0));
    } else {
      Sum_by_key(L[1..], K, g);
      var hd: int -> int := (ok: int) => if ok == L[0].ok then g(L[0]) else 0;
      var tl: int -> int := (ok: int) => SumF(L[1..], (l: Li) => if l.ok == ok then g(l) else 0);
      SetSum_ext(K, (ok: int) => SumF(L, (l: Li) => if l.ok == ok then g(l) else 0),
                    (ok: int) => hd(ok) + tl(ok));
      SetSum_plus(K, hd, tl);
      SetSum_point(K, L[0].ok, g(L[0]));
    }
  }

  lemma NW_by_orders(T: TDB, sk: int)
    requires OrdKeyUnique(T)
    ensures NW(T, sk) == SetSum(OKeys(T), (ok: int) => Cv(T, ok, sk))
  {
    var g: Li -> int := (l: Li) => if l.sk == sk && Qual(T, l) then 1 else 0;
    forall l | l in T.li && g(l) != 0 ensures l.ok in OKeys(T) {
      var o :| o in T.ords && o.ok == l.ok && o.st == 'F';
    }
    Sum_by_key(T.li, OKeys(T), g);
    forall ok | ok in OKeys(T)
      ensures SumF(T.li, (l: Li) => if l.ok == ok then g(l) else 0) == Cv(T, ok, sk)
    {
      SumF_ext(T.li, (l: Li) => if l.ok == ok then g(l) else 0,
                     (l: Li) => if l.ok == ok && l.sk == sk && Qual(T, l) then 1 else 0);
      PerOrder(T, ok, sk);
    }
    SetSum_ext(OKeys(T), (ok: int) => SumF(T.li, (l: Li) => if l.ok == ok then g(l) else 0),
                         (ok: int) => Cv(T, ok, sk));
  }

  // Orders absent from the orders table contribute nothing.
  lemma Cv_absent(T: TDB, ok: int, sk: int)
    requires ok !in OKeys(T)
    ensures Cv(T, ok, sk) == 0
  {
    assert StatusOf(T, ok) != 'F' by {
      StatusOf_absent(T.ords, ok);
    }
  }

  lemma StatusOf_absent(O: seq<Ord>, ok: int)
    requires forall o :: o in O ==> o.ok != ok
    ensures StatusOfSeq(O, ok) == 0 as char
  {
    if |O| > 0 { StatusOf_absent(O[1..], ok); }
  }

  lemma Cv_nonneg(T: TDB, ok: int, sk: int)
    ensures Cv(T, ok, sk) >= 0
  {
    var S := SummaryOf(T.li, ok);
    var st := StatusOf(T, ok);
    if st == 'F' && |S| > 1 && |LateSet(S)| == 1 {
      var x :| x in LateSet(S);
      Singleton(LateSet(S), x);
      assert ContribMap(st, S) == map[x := S[x].late];
    }
  }
}
