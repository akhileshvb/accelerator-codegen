// Twin <-> Aux refinement lemmas for the per-order contribution computation
// and the per-order lineitem counting, valid for EVERY hash-map iteration order.
//
//   Contrib_refines : ComputeOrderContributions(o, en) == ContribMap(o.orderstatus, o.suppliers)
//   AddLineitem / ApplyLiDeltas : count maps as pointwise sums (SOI algebra)

include "Q21ModuleTwin.dfy"
include "Q21Contrib.dfy"

module Q21Refine {
  import opened Q21Types
  import opened Q21ModuleTwin
  import opened Q21Contrib
  import opened Q21Count

  ghost predicate LateIn(sup: map<int, SOI>, x: int)
  { x in sup && sup[x].total > 0 && sup[x].late > 0 }

  ghost function LateOf(sup: map<int, SOI>, en: seq<int>): set<int>
  { set x | x in en && LateIn(sup, x) }

  lemma LateOf_tail(sup: map<int, SOI>, en: seq<int>)
    requires |en| > 0
    requires forall i, j :: 0 <= i < j < |en| ==> en[i] != en[j]
    ensures LateOf(sup, en) == (if LateIn(sup, en[0]) then {en[0]} else {}) + LateOf(sup, en[1..])
    ensures en[0] !in LateOf(sup, en[1..])
  {
    assert forall x :: x in en <==> (x == en[0] || x in en[1..]);
    if en[0] in en[1..] {
      var k :| 0 <= k < |en[1..]| && en[1..][k] == en[0];
      assert en[0] != en[k + 1];
    }
  }

  lemma ContribLoop_spec(sup: map<int, SOI>, en: seq<int>, s: int, c: int, k: int)
    requires forall x :: x in en ==> x in sup
    requires forall i, j :: 0 <= i < j < |en| ==> en[i] != en[j]
    requires k == 0 || k == 1
    requires k == 1 ==> s !in en
    ensures ContribLoop(sup, en, s, c, k) ==
      (var LS := LateOf(sup, en);
       if k == 0 then
         (if |LS| == 1 then (var x :| x in LS; map[x := sup[x].late]) else map[])
       else
         (if |LS| == 0 then map[s := c] else map[]))
    decreases |en|
  {
    var LS := LateOf(sup, en);
    if |en| == 0 {
      assert LS == {};
    } else {
      LateOf_tail(sup, en);
      var LS1 := LateOf(sup, en[1..]);
      assert forall i, j :: 0 <= i < j < |en[1..]| ==> en[1..][i] != en[1..][j] by {
        forall i, j | 0 <= i < j < |en[1..]| ensures en[1..][i] != en[1..][j] { assert en[i + 1] != en[j + 1]; }
      }
      var x0 := en[0];
      if !LateIn(sup, x0) {
        assert LS == LS1;
        if k == 1 { assert s !in en[1..]; }
        ContribLoop_spec(sup, en[1..], s, c, k);
      } else if k + 1 > 1 {
        assert x0 in LS;
        assert |LS| >= 1;
      } else {
        // k == 0: x0 becomes the sole late candidate.
        assert x0 !in en[1..] by {
          if x0 in en[1..] { var j :| 0 <= j < |en[1..]| && en[1..][j] == x0; assert en[0] != en[j + 1]; }
        }
        ContribLoop_spec(sup, en[1..], x0, sup[x0].late, 1);
        assert LS == {x0} + LS1;
        if |LS1| == 0 {
          assert LS == {x0};
        } else {
          assert |LS| >= 2 by {
            var y :| y in LS1;
            assert {x0, y} <= LS;
            SubsetCard({x0, y}, LS);
          }
        }
      }
    }
  }

  lemma Contrib_refines(o: OrderSummary, en: seq<int>)
    requires IsEnum(en, o.suppliers.Keys)
    ensures ComputeOrderContributions(o, en) == ContribMap(o.orderstatus, o.suppliers)
  {
    if !(o.orderstatus != 'F' || |o.suppliers| <= 1) {
      ContribLoop_spec(o.suppliers, en, 0, 0, 0);
      assert LateOf(o.suppliers, en) == LateSet(o.suppliers);
      if |LateSet(o.suppliers)| == 1 {
        var x :| x in LateSet(o.suppliers);
        Singleton(LateSet(o.suppliers), x);
        assert ContribMap(o.orderstatus, o.suppliers) == map[x := o.suppliers[x].late];
        assert ContribLoop(o.suppliers, en, 0, 0, 0) == map[x := o.suppliers[x].late];
      }
    }
  }

  // ----- Count-map algebra -----

  function GetSOI(m: map<int, SOI>, k: int): SOI { if k in m then m[k] else SOI(0, 0) }

  // Summary of a lineitem sequence restricted to order ok, as a pointwise count.
  lemma SummaryOf_cons(L: seq<Li>, ok: int, sk: int)
    requires |L| > 0
    ensures GetSOI(SummaryOf(L, ok), sk) ==
            SOI(GetSOI(SummaryOf(L[1..], ok), sk).total + (if L[0].ok == ok && L[0].sk == sk then 1 else 0),
                GetSOI(SummaryOf(L[1..], ok), sk).late + (if L[0].ok == ok && L[0].sk == sk && Late(L[0]) then 1 else 0))
  {
    reveal SummaryOf();
    SumF_ind_pos(L, (l: Li) => l.ok == ok && l.sk == sk);
    SumF_ind_pos(L[1..], (l: Li) => l.ok == ok && l.sk == sk);
    SumF_ext(L, (l: Li) => if (l.ok == ok && l.sk == sk) then 1 else 0, (l: Li) => if l.ok == ok && l.sk == sk then 1 else 0);
    assert forall x :: x in L <==> (x == L[0] || x in L[1..]);
    assert sk in SupsOf(L, ok) <==> (L[0].ok == ok && L[0].sk == sk) || sk in SupsOf(L[1..], ok);
    if sk !in SupsOf(L[1..], ok) {
      // Tot / LateC of the tail vanish.
      SumF_ind_pos(L[1..], (l: Li) => l.ok == ok && l.sk == sk && Late(l));
      SumF_ext(L[1..], (l: Li) => if (l.ok == ok && l.sk == sk) then 1 else 0, (l: Li) => if l.ok == ok && l.sk == sk then 1 else 0);
      SumF_ext(L[1..], (l: Li) => if (l.ok == ok && l.sk == sk && Late(l)) then 1 else 0, (l: Li) => if l.ok == ok && l.sk == sk && Late(l) then 1 else 0);
      forall l | l in L[1..] ensures !(l.ok == ok && l.sk == sk) {
        if l.ok == ok && l.sk == sk { assert sk in SupsOf(L[1..], ok); }
      }
      SumF_zero(L[1..], (l: Li) => if l.ok == ok && l.sk == sk then 1 else 0);
      SumF_zero(L[1..], (l: Li) => if l.ok == ok && l.sk == sk && Late(l) then 1 else 0);
      assert Tot(L[1..], ok, sk) == 0;
      assert LateC(L[1..], ok, sk) == 0;
    }
  }

  lemma SummaryOf_late_le(L: seq<Li>, ok: int, sk: int)
    ensures 0 <= GetSOI(SummaryOf(L, ok), sk).late <= GetSOI(SummaryOf(L, ok), sk).total
    ensures sk in SummaryOf(L, ok) ==> GetSOI(SummaryOf(L, ok), sk).total > 0
    ensures sk !in SummaryOf(L, ok) ==> GetSOI(SummaryOf(L, ok), sk) == SOI(0, 0)
    decreases |L|
  {
    reveal SummaryOf();
    if |L| > 0 {
      SummaryOf_late_le(L[1..], ok, sk);
      SummaryOf_cons(L, ok, sk);
      assert forall x :: x in L <==> (x == L[0] || x in L[1..]);
      assert sk in SummaryOf(L, ok) <==> (L[0].ok == ok && L[0].sk == sk) || sk in SummaryOf(L[1..], ok);
    } else {
      assert SupsOf(L, ok) == {};
    }
  }

  // Map equality from pointwise GetSOI + domain = positive totals.
  lemma Summary_ext(m: map<int, SOI>, L: seq<Li>, ok: int)
    requires forall sk :: GetSOI(m, sk) == GetSOI(SummaryOf(L, ok), sk)
    requires forall sk :: sk in m ==> m[sk].total > 0
    ensures m == SummaryOf(L, ok)
  {
    reveal SummaryOf();
    forall sk ensures sk in m <==> sk in SummaryOf(L, ok) {
      SummaryOf_late_le(L, ok, sk);
    }
    forall sk | sk in m ensures m[sk] == SummaryOf(L, ok)[sk] {
      assert GetSOI(m, sk) == m[sk];
      assert GetSOI(SummaryOf(L, ok), sk) == SummaryOf(L, ok)[sk];
    }
  }
}
