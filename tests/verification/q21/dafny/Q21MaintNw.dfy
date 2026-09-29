// ApplyContributionDeltas as pointwise arithmetic on nation_numwait:
//   NwVal(after, nk, sk) = NwVal(before, nk, sk)
//                          + [sk has nation nk] * (new(sk) - old(sk))
// with zero entries erased (NoZero preserved).

include "Q21Build.dfy"

module Q21MaintNw {
  import opened Q21ModuleTwin
  import opened Q21Build
  import opened Q21Inv

  function Get0(m: map<int, int>, k: int): int { if k in m then m[k] else 0 }

  ghost predicate NoZero(nw: map<int, map<int, int>>)
  { forall nk, sk :: nk in nw && sk in nw[nk] ==> nw[nk][sk] != 0 }

  lemma ApplyOneDelta_facts(st: TState, sk: int, delta: int)
    requires NoZero(st.nation_numwait)
    ensures var r := ApplyOneDelta(st, sk, delta);
            r.supplier_names == st.supplier_names && r.supplier_nations == st.supplier_nations &&
            r.nation_name_to_keys == st.nation_name_to_keys &&
            r.order_summaries == st.order_summaries && r.order_overlay == st.order_overlay &&
            NoZero(r.nation_numwait) &&
            (forall nk, k :: NwVal(r.nation_numwait, nk, k) == NwVal(st.nation_numwait, nk, k) +
               (if k == sk && sk in st.supplier_nations && st.supplier_nations[sk] == nk then delta else 0)) &&
            (forall nk :: nk in r.nation_numwait ==> (nk in st.nation_numwait ||
               (delta != 0 && sk in st.supplier_nations && st.supplier_nations[sk] == nk)))
  {}

  lemma ApplyDeltasLoop_facts(st: TState, oldc: map<int, int>, newc: map<int, int>, ks: seq<int>)
    requires NoZero(st.nation_numwait)
    requires NoDupSeq(ks)
    ensures var r := ApplyDeltasLoop(st, oldc, newc, ks);
            r.supplier_names == st.supplier_names && r.supplier_nations == st.supplier_nations &&
            r.nation_name_to_keys == st.nation_name_to_keys &&
            r.order_summaries == st.order_summaries && r.order_overlay == st.order_overlay &&
            NoZero(r.nation_numwait) &&
            (forall nk, k :: NwVal(r.nation_numwait, nk, k) == NwVal(st.nation_numwait, nk, k) +
               (if k in ks && k in st.supplier_nations && st.supplier_nations[k] == nk
                then Get0(newc, k) - Get0(oldc, k) else 0)) &&
            (forall nk :: nk in r.nation_numwait ==> (nk in st.nation_numwait ||
               exists k :: k in ks && k in st.supplier_nations && st.supplier_nations[k] == nk &&
                           Get0(newc, k) != Get0(oldc, k)))
    decreases |ks|
  {
    if |ks| > 0 {
      var sk := ks[0];
      var d := Get0(newc, sk) - Get0(oldc, sk);
      var st2 := ApplyOneDelta(st, sk, d);
      ApplyOneDelta_facts(st, sk, d);
      assert NoDupSeq(ks[1..]) by {
        forall i, j | 0 <= i < j < |ks[1..]| ensures ks[1..][i] != ks[1..][j] { assert ks[i + 1] != ks[j + 1]; }
      }
      ApplyDeltasLoop_facts(st2, oldc, newc, ks[1..]);
      assert sk !in ks[1..] by {
        if sk in ks[1..] { var j :| 0 <= j < |ks[1..]| && ks[1..][j] == sk; assert ks[0] != ks[j + 1]; }
      }
      assert forall k :: k in ks <==> (k == sk || k in ks[1..]);
    }
  }

  lemma ApplyContributionDeltas_facts(st: TState, oldc: map<int, int>, newc: map<int, int>)
    requires NoZero(st.nation_numwait)
    ensures var r := ApplyContributionDeltas(st, oldc, newc);
            r.supplier_names == st.supplier_names && r.supplier_nations == st.supplier_nations &&
            r.nation_name_to_keys == st.nation_name_to_keys &&
            r.order_summaries == st.order_summaries && r.order_overlay == st.order_overlay &&
            NoZero(r.nation_numwait) &&
            (forall nk, k :: NwVal(r.nation_numwait, nk, k) == NwVal(st.nation_numwait, nk, k) +
               (if k in st.supplier_nations && st.supplier_nations[k] == nk
                then Get0(newc, k) - Get0(oldc, k) else 0)) &&
            (forall nk :: nk in r.nation_numwait ==> (nk in st.nation_numwait ||
               exists k :: (k in oldc || k in newc) && k in st.supplier_nations && st.supplier_nations[k] == nk))
  {
    var ks := IntKeys(oldc.Keys + newc.Keys);
    IntKeys_enum(oldc.Keys + newc.Keys);
    ApplyDeltasLoop_facts(st, oldc, newc, ks);
  }
}
