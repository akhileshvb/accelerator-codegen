// Stage 3.4: Build / Rebuild establishes Inv_Q21, and ExecuteFallback is exact.
//
//   Build_establishes_Inv :
//     KeysOk(T) && DomOk(T) && EnumOracle(ei, es) ==>
//       StoreInv(InstallMap(BuildImages(T, ei, es)), T)
//   ExecuteFallback_correct :
//     KeysOk(T) && DomOk(T) && EnumOracle(ei, es) ==>
//       ExecuteFallback(T, ArgText(nn), ei, es) == SQLRows(T, nn)

include "Q21Inv.dfy"
include "Q21Refine.dfy"

module Q21Build {
  import opened Q21Types
  import opened Q21Count
  import opened Q21SetSum
  import opened Q21Contrib
  import opened Q21ModuleTwin
  import opened Q21Sorted
  import opened Q21Answer
  import opened Q21Emit
  import opened Q21Inv
  import opened Q21Refine

  // ===================== Install =====================

  function InstallMap(imgs: seq<(int, TImage)>): map<int, TImage>
  { if |imgs| == 0 then map[] else InstallMap(imgs[..|imgs| - 1])[imgs[|imgs| - 1].0 := imgs[|imgs| - 1].1] }

  lemma InstallMap_facts(imgs: seq<(int, TImage)>)
    ensures forall pid :: pid in InstallMap(imgs) ==> (pid, InstallMap(imgs)[pid]) in imgs
    ensures forall p :: p in imgs ==> p.0 in InstallMap(imgs)
  {
    if |imgs| > 0 {
      InstallMap_facts(imgs[..|imgs| - 1]);
      assert imgs == imgs[..|imgs| - 1] + [imgs[|imgs| - 1]];
    }
  }

  // ===================== Scans =====================

  lemma ScanSuppliers_facts(st: TState, S: seq<Sup>)
    requires forall i, j :: 0 <= i < j < |S| ==> S[i].sk != S[j].sk
    ensures var r := ScanSuppliers(st, S);
            r.nation_name_to_keys == st.nation_name_to_keys && r.order_summaries == st.order_summaries &&
            r.order_overlay == st.order_overlay && r.nation_numwait == st.nation_numwait &&
            (forall sk :: sk in r.supplier_names <==> sk in st.supplier_names || exists s :: s in S && s.sk == sk) &&
            (forall sk :: sk in r.supplier_nations <==> sk in st.supplier_nations || exists s :: s in S && s.sk == sk) &&
            (forall s :: s in S ==> r.supplier_names[s.sk] == s.name && r.supplier_nations[s.sk] == s.nk) &&
            (forall sk :: sk in st.supplier_names && !(exists s :: s in S && s.sk == sk) ==> r.supplier_names[sk] == st.supplier_names[sk]) &&
            (forall sk :: sk in st.supplier_nations && !(exists s :: s in S && s.sk == sk) ==> r.supplier_nations[sk] == st.supplier_nations[sk])
    decreases |S|
  {
    if |S| > 0 {
      var st2 := st.(supplier_names := st.supplier_names[S[0].sk := S[0].name],
                     supplier_nations := st.supplier_nations[S[0].sk := S[0].nk]);
      assert forall i, j :: 0 <= i < j < |S[1..]| ==> S[1..][i].sk != S[1..][j].sk by {
        forall i, j | 0 <= i < j < |S[1..]| ensures S[1..][i].sk != S[1..][j].sk { assert S[i + 1].sk != S[j + 1].sk; }
      }
      ScanSuppliers_facts(st2, S[1..]);
      assert forall s :: s in S[1..] ==> s.sk != S[0].sk by {
        forall s | s in S[1..] ensures s.sk != S[0].sk {
          var k :| 0 <= k < |S[1..]| && S[1..][k] == s; assert S[0].sk != S[k + 1].sk;
        }
      }
      assert forall s :: s in S <==> (s == S[0] || s in S[1..]);
    }
  }

  lemma Sup_maps(T: TDB, st: TState)
    requires SupKeyUnique(T)
    requires st == ScanSuppliers(EmptyState(), T.sup)
    ensures st.supplier_names == SupNameOf(T) && st.supplier_nations == SupNatOf(T)
  {
    ScanSuppliers_facts(EmptyState(), T.sup);
    forall sk ensures sk in st.supplier_names <==> sk in SKeys(T) {}
    forall sk | sk in SKeys(T) ensures st.supplier_names[sk] == SupNameOf(T)[sk] && st.supplier_nations[sk] == SupNatOf(T)[sk] {
      var s :| s in T.sup && s.sk == sk;
      SupFirst_unique(T, s);
    }
  }

  ghost function NksNamed(N: seq<Nat>, nm: string): seq<int>
  { if |N| == 0 then [] else (if N[0].name == nm then [N[0].nk] else []) + NksNamed(N[1..], nm) }

  lemma ScanNations_facts(st: TState, N: seq<Nat>)
    ensures var r := ScanNations(st, N);
            r.supplier_names == st.supplier_names && r.supplier_nations == st.supplier_nations &&
            r.order_summaries == st.order_summaries && r.order_overlay == st.order_overlay &&
            r.nation_numwait == st.nation_numwait &&
            (forall nm :: nm in r.nation_name_to_keys <==>
               nm in st.nation_name_to_keys || exists n :: n in N && n.name == nm) &&
            (forall nm :: nm in r.nation_name_to_keys ==>
               r.nation_name_to_keys[nm] ==
                 (if nm in st.nation_name_to_keys then st.nation_name_to_keys[nm] else []) + NksNamed(N, nm))
    decreases |N|
  {
    if |N| > 0 {
      var nm0 := N[0].name;
      var ks := if nm0 in st.nation_name_to_keys then st.nation_name_to_keys[nm0] else [];
      var st2 := st.(nation_name_to_keys := st.nation_name_to_keys[nm0 := ks + [N[0].nk]]);
      ScanNations_facts(st2, N[1..]);
      assert forall n :: n in N <==> (n == N[0] || n in N[1..]);
      forall nm | nm in ScanNations(st, N).nation_name_to_keys
        ensures ScanNations(st, N).nation_name_to_keys[nm] ==
                (if nm in st.nation_name_to_keys then st.nation_name_to_keys[nm] else []) + NksNamed(N, nm)
      {
        if nm == nm0 { assert ks + [N[0].nk] + NksNamed(N[1..], nm) == ks + NksNamed(N, nm); }
      }
    }
  }

  lemma NksNamed_mem(N: seq<Nat>, nm: string, x: int)
    ensures x in NksNamed(N, nm) <==> exists n :: n in N && n.name == nm && n.nk == x
  {
    if |N| > 0 {
      NksNamed_mem(N[1..], nm, x);
      assert forall n :: n in N <==> (n == N[0] || n in N[1..]);
    }
  }

  lemma NksNamed_nodup(N: seq<Nat>, nm: string)
    requires forall i, j :: 0 <= i < j < |N| ==> N[i].nk != N[j].nk
    ensures NoDupSeq(NksNamed(N, nm))
  {
    if |N| > 0 {
      assert forall i, j :: 0 <= i < j < |N[1..]| ==> N[1..][i].nk != N[1..][j].nk by {
        forall i, j | 0 <= i < j < |N[1..]| ensures N[1..][i].nk != N[1..][j].nk { assert N[i + 1].nk != N[j + 1].nk; }
      }
      NksNamed_nodup(N[1..], nm);
      if N[0].name == nm {
        var rest := NksNamed(N[1..], nm);
        forall y | y in rest ensures y != N[0].nk {
          NksNamed_mem(N[1..], nm, y);
          var n :| n in N[1..] && n.name == nm && n.nk == y;
          var k :| 0 <= k < |N[1..]| && N[1..][k] == n;
          assert N[0].nk != N[k + 1].nk;
        }
        var res := NksNamed(N, nm);
        assert res == [N[0].nk] + rest;
        forall i, j | 0 <= i < j < |res| ensures res[i] != res[j] {
          if i == 0 { assert res[j] in rest; } else { assert res[i] == rest[i - 1] && res[j] == rest[j - 1]; }
        }
      }
    }
  }

  lemma Idx_built(T: TDB, st: TState)
    requires NatKeyUnique(T)
    requires st.nation_name_to_keys == ScanNations(ScanSuppliers(EmptyState(), T.sup), T.nats).nation_name_to_keys
    requires forall i, j :: 0 <= i < j < |T.sup| ==> T.sup[i].sk != T.sup[j].sk
    ensures IdxOk(st, T)
  {
    var s1 := ScanSuppliers(EmptyState(), T.sup);
    ScanSuppliers_facts(EmptyState(), T.sup);
    ScanNations_facts(s1, T.nats);
    forall nm ensures (nm in st.nation_name_to_keys <==> NationsNamed(T, nm) != {}) &&
                      (nm in st.nation_name_to_keys ==>
                         NoDupSeq(st.nation_name_to_keys[nm]) &&
                         (forall x :: x in st.nation_name_to_keys[nm] <==> x in NationsNamed(T, nm)))
    {
      if exists n :: n in T.nats && n.name == nm {
        var n :| n in T.nats && n.name == nm;
        assert n.nk in NationsNamed(T, nm);
      }
      if NationsNamed(T, nm) != {} {
        var x :| x in NationsNamed(T, nm);
        var n :| n in T.nats && n.name == nm && n.nk == x;
      }
      if nm in st.nation_name_to_keys {
        assert st.nation_name_to_keys[nm] == [] + NksNamed(T.nats, nm);
        assert [] + NksNamed(T.nats, nm) == NksNamed(T.nats, nm);
        NksNamed_nodup(T.nats, nm);
        forall x ensures x in st.nation_name_to_keys[nm] <==> x in NationsNamed(T, nm) {
          NksNamed_mem(T.nats, nm, x);
        }
      }
    }
  }

  lemma ScanOrders_facts(st: TState, O: seq<Ord>)
    requires forall i, j :: 0 <= i < j < |O| ==> O[i].ok != O[j].ok
    requires forall o :: o in O ==> o.ok !in st.order_summaries
    ensures var r := ScanOrders(st, O);
            r.supplier_names == st.supplier_names && r.supplier_nations == st.supplier_nations &&
            r.nation_name_to_keys == st.nation_name_to_keys && r.order_overlay == st.order_overlay &&
            r.nation_numwait == st.nation_numwait &&
            (forall ok :: ok in r.order_summaries <==> ok in st.order_summaries || exists o :: o in O && o.ok == ok) &&
            (forall o :: o in O ==> r.order_summaries[o.ok] == OrderSummary(o.st, map[])) &&
            (forall ok :: ok in st.order_summaries ==> r.order_summaries[ok] == st.order_summaries[ok])
    decreases |O|
  {
    if |O| > 0 {
      var o0 := O[0];
      var st2 := st.(order_summaries := st.order_summaries[o0.ok := OrderSummary(0 as char, map[]).(orderstatus := o0.st)]);
      assert forall i, j :: 0 <= i < j < |O[1..]| ==> O[1..][i].ok != O[1..][j].ok by {
        forall i, j | 0 <= i < j < |O[1..]| ensures O[1..][i].ok != O[1..][j].ok { assert O[i + 1].ok != O[j + 1].ok; }
      }
      forall o | o in O[1..] ensures o.ok !in st2.order_summaries {
        var k :| 0 <= k < |O[1..]| && O[1..][k] == o; assert O[0].ok != O[k + 1].ok;
        assert o in O;
      }
      assert o0 in O;
      ScanOrders_facts(st2, O[1..]);
      assert forall o :: o in O <==> (o == O[0] || o in O[1..]);
    }
  }

  lemma StatusOf_row(T: TDB, o: Ord)
    requires OrdKeyUnique(T) && o in T.ords
    ensures StatusOf(T, o.ok) == o.st
  {
    StatusRowSeq(T.ords, o);
  }

  lemma StatusRowSeq(O: seq<Ord>, o: Ord)
    requires forall i, j :: 0 <= i < j < |O| ==> O[i].ok != O[j].ok
    requires o in O
    ensures StatusOfSeq(O, o.ok) == o.st
  {
    if O[0] != o {
      if O[0].ok == o.ok {
        var k :| 0 <= k < |O| && O[k] == o; assert k > 0 && O[0].ok != O[k].ok;
      }
      assert o in O[1..];
      assert forall i, j :: 0 <= i < j < |O[1..]| ==> O[1..][i].ok != O[1..][j].ok by {
        forall i, j | 0 <= i < j < |O[1..]| ensures O[1..][i].ok != O[1..][j].ok { assert O[i + 1].ok != O[j + 1].ok; }
      }
      StatusRowSeq(O[1..], o);
    }
  }

  function AddSOI(a: SOI, b: SOI): SOI { SOI(a.total + b.total, a.late + b.late) }

  lemma ScanLineitems_frame(st: TState, L: seq<Li>)
    ensures var r := ScanLineitems(st, L);
            r.supplier_names == st.supplier_names && r.supplier_nations == st.supplier_nations &&
            r.nation_name_to_keys == st.nation_name_to_keys && r.order_overlay == st.order_overlay &&
            r.nation_numwait == st.nation_numwait &&
            r.order_summaries.Keys == st.order_summaries.Keys &&
            (forall ok :: ok in st.order_summaries ==>
               r.order_summaries[ok].orderstatus == st.order_summaries[ok].orderstatus)
    decreases |L|
  {
    if |L| > 0 {
      var ok0 := L[0].ok;
      if ok0 !in st.order_summaries || st.order_summaries[ok0].orderstatus != 'F' {
        ScanLineitems_frame(st, L[1..]);
      } else {
        var s := st.order_summaries[ok0];
        ScanLineitems_frame(st.(order_summaries := st.order_summaries[ok0 := s.(suppliers := AddLineitem(s.suppliers, L[0]))]), L[1..]);
      }
    }
  }

  function OneSOI(l: Li, ok: int, sk: int): SOI
  { if l.ok == ok && l.sk == sk then SOI(1, if l.rd > l.cd then 1 else 0) else SOI(0, 0) }

  lemma AddLineitem_get(sup: map<int, SOI>, l: Li, sk: int)
    ensures GetSOI(AddLineitem(sup, l), sk) == AddSOI(GetSOI(sup, sk), OneSOI(l, l.ok, sk))
  {}

  ghost function {:opaque} SumAt(L: seq<Li>, ok: int, sk: int): SOI { GetSOI(SummaryOf(L, ok), sk) }

  lemma SumAt_step(L: seq<Li>, ok: int, sk: int)
    requires |L| > 0
    ensures SumAt(L, ok, sk) == AddSOI(OneSOI(L[0], ok, sk), SumAt(L[1..], ok, sk))
  {
    reveal SumAt();
    SummaryOf_cons(L, ok, sk);
  }

  lemma SumAt_nil(L: seq<Li>, ok: int, sk: int)
    requires |L| == 0
    ensures SumAt(L, ok, sk) == SOI(0, 0)
  {
    reveal SumAt();
    reveal SummaryOf();
    assert SupsOf(L, ok) == {};
  }

  lemma {:isolate_assertions} ScanLineitems_counts(st: TState, L: seq<Li>, ok: int, sk: int)
    requires ok in st.order_summaries && st.order_summaries[ok].orderstatus == 'F'
    ensures ok in ScanLineitems(st, L).order_summaries
    ensures GetSOI(ScanLineitems(st, L).order_summaries[ok].suppliers, sk) ==
            AddSOI(GetSOI(st.order_summaries[ok].suppliers, sk), SumAt(L, ok, sk))
    decreases |L|
  {
    ScanLineitems_frame(st, L);
    if |L| == 0 {
      SumAt_nil(L, ok, sk);
    } else {
      SumAt_step(L, ok, sk);
      var ok0 := L[0].ok;
      if ok0 !in st.order_summaries || st.order_summaries[ok0].orderstatus != 'F' {
        assert OneSOI(L[0], ok, sk) == SOI(0, 0);
        assert ScanLineitems(st, L) == ScanLineitems(st, L[1..]);
        ScanLineitems_counts(st, L[1..], ok, sk);
      } else {
        var s := st.order_summaries[ok0];
        var sup2 := AddLineitem(s.suppliers, L[0]);
        var st2 := st.(order_summaries := st.order_summaries[ok0 := s.(suppliers := sup2)]);
        assert ScanLineitems(st, L) == ScanLineitems(st2, L[1..]);
        assert ok in st2.order_summaries && st2.order_summaries[ok].orderstatus == 'F';
        ScanLineitems_counts(st2, L[1..], ok, sk);
        if ok0 == ok {
          AddLineitem_get(s.suppliers, L[0], sk);
        } else {
          assert OneSOI(L[0], ok, sk) == SOI(0, 0);
        }
      }
    }
  }

  lemma ScanLineitems_unchanged(st: TState, L: seq<Li>, ok: int)
    requires ok in st.order_summaries && st.order_summaries[ok].orderstatus != 'F'
    ensures ok in ScanLineitems(st, L).order_summaries
    ensures ScanLineitems(st, L).order_summaries[ok] == st.order_summaries[ok]
    decreases |L|
  {
    ScanLineitems_frame(st, L);
    if |L| > 0 {
      var ok0 := L[0].ok;
      if ok0 !in st.order_summaries || st.order_summaries[ok0].orderstatus != 'F' {
        ScanLineitems_unchanged(st, L[1..], ok);
      } else {
        var s := st.order_summaries[ok0];
        ScanLineitems_unchanged(st.(order_summaries := st.order_summaries[ok0 := s.(suppliers := AddLineitem(s.suppliers, L[0]))]), L[1..], ok);
      }
    }
  }

  lemma ScanLineitems_pos(st: TState, L: seq<Li>, ok: int, sk: int)
    requires ok in st.order_summaries
    requires forall k :: k in st.order_summaries[ok].suppliers ==> st.order_summaries[ok].suppliers[k].total > 0
    ensures ok in ScanLineitems(st, L).order_summaries
    ensures sk in ScanLineitems(st, L).order_summaries[ok].suppliers ==>
            ScanLineitems(st, L).order_summaries[ok].suppliers[sk].total > 0
    decreases |L|
  {
    ScanLineitems_frame(st, L);
    if |L| > 0 {
      var ok0 := L[0].ok;
      if ok0 !in st.order_summaries || st.order_summaries[ok0].orderstatus != 'F' {
        ScanLineitems_pos(st, L[1..], ok, sk);
      } else {
        var s := st.order_summaries[ok0];
        var st2 := st.(order_summaries := st.order_summaries[ok0 := s.(suppliers := AddLineitem(s.suppliers, L[0]))]);
        ScanLineitems_pos(st2, L[1..], ok, sk);
      }
    }
  }

  // The scanned summaries: for every order key, status and (F-orders) exact counts.
  lemma Scanned(T: TDB)
    requires KeysOk(T)
    ensures var st := ScanAll(T);
            st.supplier_names == SupNameOf(T) && st.supplier_nations == SupNatOf(T) &&
            IdxOk(st, T) && st.order_overlay == map[] && st.nation_numwait == map[] &&
            st.order_summaries.Keys == OKeys(T) &&
            (forall ok :: ok in OKeys(T) ==>
               st.order_summaries[ok].orderstatus == StatusOf(T, ok) &&
               st.order_summaries[ok].suppliers ==
                 (if StatusOf(T, ok) == 'F' then SummaryOf(T.li, ok) else map[]))
  {
    var s1 := ScanSuppliers(EmptyState(), T.sup);
    var s2 := ScanNations(s1, T.nats);
    var s3 := ScanOrders(s2, T.ords);
    var s4 := ScanLineitems(s3, T.li);
    ScanSuppliers_facts(EmptyState(), T.sup);
    Sup_maps(T, s1);
    ScanNations_facts(s1, T.nats);
    ScanOrders_facts(s2, T.ords);
    ScanLineitems_frame(s3, T.li);
    Idx_built(T, s4);
    forall ok | ok in OKeys(T)
      ensures s4.order_summaries[ok].orderstatus == StatusOf(T, ok) &&
              s4.order_summaries[ok].suppliers == (if StatusOf(T, ok) == 'F' then SummaryOf(T.li, ok) else map[])
    {
      var o :| o in T.ords && o.ok == ok;
      StatusOf_row(T, o);
      assert s3.order_summaries[ok] == OrderSummary(o.st, map[]);
      if o.st == 'F' {
        var m := s4.order_summaries[ok].suppliers;
        forall sk ensures GetSOI(m, sk) == GetSOI(SummaryOf(T.li, ok), sk) {
          ScanLineitems_counts(s3, T.li, ok, sk);
          reveal SumAt();
        }
        forall sk | sk in m ensures m[sk].total > 0 {
          ScanLineitems_pos(s3, T.li, ok, sk);
        }
        Summary_ext(m, T.li, ok);
      } else {
        ScanLineitems_unchanged(s3, T.li, ok);
      }
    }
  }

  // ===================== Contribution accumulation =====================

  function NwVal(nw: map<int, map<int, int>>, nk: int, sk: int): int
  { if nk in nw && sk in nw[nk] then nw[nk][sk] else 0 }

  ghost predicate NwWF(nw: map<int, map<int, int>>)
  { forall nk :: nk in nw ==> |nw[nk]| > 0 && forall sk :: sk in nw[nk] ==> nw[nk][sk] > 0 }

  lemma AddContribs_facts(st: TState, c: map<int, int>, cen: seq<int>)
    requires IsEnum(cen, c.Keys)
    requires NwWF(st.nation_numwait)
    requires forall sk :: sk in c ==> c[sk] >= 0
    ensures var r := AddContribs(st, c, cen);
            NwWF(r.nation_numwait) &&
            (forall nk, sk :: NwVal(r.nation_numwait, nk, sk) == NwVal(st.nation_numwait, nk, sk) +
               (if sk in c && sk in st.supplier_nations && st.supplier_nations[sk] == nk then c[sk] else 0))
    decreases |cen|
  {
    if |cen| == 0 {
      assert c.Keys == {} by { forall x | x in c.Keys ensures false { assert x in cen; } }
    } else {
      var sk0 := cen[0];
      var c' := c - {sk0};
      assert IsEnum(cen[1..], c'.Keys) by {
        forall x ensures x in cen[1..] <==> x in c'.Keys {
          if x in cen[1..] { var k :| 0 <= k < |cen[1..]| && cen[1..][k] == x; assert cen[0] != cen[k + 1]; }
          if x in c'.Keys { assert x in cen; if x != cen[0] { assert x in cen[1..]; } }
        }
      }
      AddContribs_restrict(st, c, c', cen[1..]);
      if c[sk0] == 0 || sk0 !in st.supplier_nations {
        AddContribs_facts(st, c', cen[1..]);
      } else {
        var nk0 := st.supplier_nations[sk0];
        var st2 := st.(nation_numwait := AddNumwait(st.nation_numwait, nk0, sk0, c[sk0]));
        AddContribs_restrict(st2, c, c', cen[1..]);
        AddContribs_facts(st2, c', cen[1..]);
      }
    }
  }

  lemma AddContribs_restrict(st: TState, c: map<int, int>, c': map<int, int>, cen: seq<int>)
    requires forall x :: x in cen ==> x in c && x in c' && c[x] == c'[x]
    ensures AddContribs(st, c, cen) == AddContribs(st, c', cen)
    decreases |cen|
  {
    if |cen| > 0 {
      var sk := cen[0];
      if c[sk] == 0 || sk !in st.supplier_nations {
        AddContribs_restrict(st, c, c', cen[1..]);
      } else {
        AddContribs_restrict(st.(nation_numwait := AddNumwait(st.nation_numwait, st.supplier_nations[sk], sk, c[sk])), c, c', cen[1..]);
      }
    }
  }

  ghost function OrdTerm(m: map<int, OrderSummary>, natm: map<int, int>, nk: int, sk: int): int -> int
  {
    (ok: int) =>
      if ok in m && m[ok].orderstatus == 'F' && sk in natm && natm[sk] == nk
      then ContribVal(m[ok].orderstatus, m[ok].suppliers, sk) else 0
  }

  lemma BuildContribs_facts(st: TState, oen: seq<int>, sen: map<int, seq<int>>, cen: map<int, seq<int>>)
    requires NwWF(st.nation_numwait)
    requires forall x :: x in oen ==> x in st.order_summaries && x in sen && x in cen
    requires forall x :: x in oen ==> IsEnum(sen[x], st.order_summaries[x].suppliers.Keys)
    requires forall x :: x in oen ==> forall y :: y in sen[x] ==> y in st.order_summaries[x].suppliers
    requires forall x :: x in oen ==> IsEnum(cen[x], ComputeOrderContributions(st.order_summaries[x], sen[x]).Keys)
    requires forall x :: x in oen ==> forall y :: y in cen[x] ==> y in ComputeOrderContributions(st.order_summaries[x], sen[x])
    requires NoDupSeq(oen)
    ensures var r := BuildContribs(st, oen, sen, cen);
            NwWF(r.nation_numwait) &&
            (forall nk, sk :: NwVal(r.nation_numwait, nk, sk) == NwVal(st.nation_numwait, nk, sk) +
               SumF(oen, OrdTerm(st.order_summaries, st.supplier_nations, nk, sk)))
    decreases |oen|
  {
    if |oen| > 0 {
      var ok := oen[0];
      var s := st.order_summaries[ok];
      assert NoDupSeq(oen[1..]) by {
        forall i, j | 0 <= i < j < |oen[1..]| ensures oen[1..][i] != oen[1..][j] { assert oen[i + 1] != oen[j + 1]; }
      }
      if s.orderstatus != 'F' {
        BuildContribs_facts(st, oen[1..], sen, cen);
      } else {
        var c := ComputeOrderContributions(s, sen[ok]);
        Contrib_refines(s, sen[ok]);
        forall sk | sk in c ensures c[sk] >= 0 {
          var x :| x in LateSet(s.suppliers);
          Singleton(LateSet(s.suppliers), x);
        }
        AddContribs_facts(st, c, cen[ok]);
        var st2 := AddContribs(st, c, cen[ok]);
        BuildContribs_facts(st2, oen[1..], sen, cen);
      }
    }
  }

  // Sum over an enumeration equals the set sum.
  lemma SumF_enum(en: seq<int>, s: set<int>, f: int -> int)
    requires IsEnum(en, s)
    ensures SumF(en, f) == SetSum(s, f)
    decreases |en|
  {
    if |en| == 0 {
      assert s == {} by { forall x | x in s ensures false { assert x in en; } }
    } else {
      var s' := s - {en[0]};
      assert IsEnum(en[1..], s') by {
        forall x ensures x in en[1..] <==> x in s' {
          if x in en[1..] { var k :| 0 <= k < |en[1..]| && en[1..][k] == x; assert en[0] != en[k + 1]; }
          if x in s' { assert x in en; if x != en[0] { assert x in en[1..]; } }
        }
      }
      SumF_enum(en[1..], s', f);
      SetSum_remove(s, f, en[0]);
    }
  }

  // ===================== BuildState: Inv facts =====================

  lemma BuildState_facts(T: TDB, ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires KeysOk(T) && EnumOracle(ei, es)
    ensures var st := BuildState(T, ei, es);
            SupOk(st, T) && IdxOk(st, T) && OrdersOk(st, T) && NwWF(st.nation_numwait) &&
            (forall nk :: NwOf(st, nk) == NWmap(T, nk)) &&
            st.order_overlay == map[]
  {
    var st4 := ScanAll(T);
    Scanned(T);
    var m := st4.order_summaries;
    var sen := SupEnums(m, ei);
    var cen := ContribEnums(m, sen, ei);
    var oen := ei(m.Keys);
    var st5 := BuildContribs(st4, oen, sen, cen);
    var st := BuildState(T, ei, es);
    assert NwWF(st4.nation_numwait);
    BuildContribs_facts(st4, oen, sen, cen);
    // numwait == NWmap
    forall nk, sk ensures NwVal(st.nation_numwait, nk, sk) ==
                          (if sk in SKeys(T) && SupNatOf(T)[sk] == nk then NW(T, sk) else 0)
    {
      var f := OrdTerm(m, st4.supplier_nations, nk, sk);
      SumF_enum(oen, m.Keys, f);
      if sk in SKeys(T) && SupNatOf(T)[sk] == nk {
        forall ok | ok in OKeys(T) ensures f(ok) == Cv(T, ok, sk) {
          if StatusOf(T, ok) != 'F' {
            assert ContribMap(StatusOf(T, ok), SummaryOf(T.li, ok)) == map[];
          }
        }
        SetSum_ext(OKeys(T), f, (ok: int) => Cv(T, ok, sk));
        NW_by_orders(T, sk);
      } else {
        SetSum_zero(m.Keys, f);
      }
    }
    assert st.nation_numwait == st5.nation_numwait;
    forall nk ensures NwOf(st, nk) == NWmap(T, nk) {
      forall sk ensures sk in NwOf(st, nk) <==> sk in NWmap(T, nk) {
        if sk in SKeys(T) { NW_nonneg(T, sk); }
        var v := NwVal(st.nation_numwait, nk, sk);
        assert v == (if sk in SKeys(T) && SupNatOf(T)[sk] == nk then NW(T, sk) else 0);
        if sk in NwOf(st, nk) {
          assert nk in st.nation_numwait && sk in st.nation_numwait[nk];
          assert v > 0;
        }
        if sk in NWmap(T, nk) {
          assert v != 0;
        }
      }
      forall sk | sk in NwOf(st, nk) ensures NwOf(st, nk)[sk] == NWmap(T, nk)[sk] {
        var v := NwVal(st.nation_numwait, nk, sk);
        assert v == (if sk in SKeys(T) && SupNatOf(T)[sk] == nk then NW(T, sk) else 0);
      }
    }
    // Orders after pruning.
    forall ok ensures (FindOrder(st, ok).SomeSummary? ==>
                         FindOrder(st, ok).s == OrderSummary(StatusOf(T, ok), SummaryOf(T.li, ok))) &&
                      (FindOrder(st, ok).NoSummary? ==> forall sk :: Cv(T, ok, sk) == 0)
    {
      assert st.order_overlay == map[];
      assert st.order_summaries == PruneSummaries(m, sen);
      if ok in OKeys(T) {
        var s := m[ok];
        Contrib_refines(s, sen[ok]);
        if KeepSummary(s, sen[ok]) {
          assert s.orderstatus == 'F';
          assert s == OrderSummary(StatusOf(T, ok), SummaryOf(T.li, ok));
          assert FindOrder(st, ok) == SomeSummary(s);
        }
        if !KeepSummary(s, sen[ok]) {
          forall sk ensures Cv(T, ok, sk) == 0 {
            if StatusOf(T, ok) == 'F' {
              assert s.suppliers == SummaryOf(T.li, ok);
            }
          }
        }
      } else {
        assert ok !in st.order_summaries;
        forall sk ensures Cv(T, ok, sk) == 0 { Cv_absent(T, ok, sk); }
      }
    }
  }

  // ===================== Build establishes Inv =====================

  lemma NationImgs_ok(st: TState, T: TDB, nks: seq<int>, ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es)
    requires SupOk(st, T) && forall nk :: NwOf(st, nk) == NWmap(T, nk)
    requires forall nk :: nk in nks ==> NkRange(nk)
    ensures forall p :: p in NationImgs(st, nks, ei, es) ==> ImageOk(p.0, p.1, T) && p.0 == NationPartition(p.1.nationkey)
    ensures forall nk :: nk in nks ==> exists p :: p in NationImgs(st, nks, ei, es) && p.0 == NationPartition(nk)
    decreases |nks|
  {
    if |nks| > 0 {
      NationImgs_ok(st, T, nks[1..], ei, es);
      var nk := nks[0];
      assert NkRange(nk);
      var en := if nk in st.nation_numwait then ei(st.nation_numwait[nk].Keys) else [];
      NationRows_ok(st, T, nk, en);
      var p := (NationPartition(nk), MakeImage(NationK, nk, st, "", en));
      assert IsNatPid(p.0) && p.0 - kNationBase == nk;
      assert ImageOk(p.0, p.1, T);
    }
  }

  lemma NameIdxImgs_ok(st: TState, T: TDB, names: seq<string>, ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es)
    requires SupOk(st, T) && IdxOk(st, T) && forall nk :: NwOf(st, nk) == NWmap(T, nk)
    requires forall x :: x in names ==> x in st.nation_name_to_keys
    ensures forall p :: p in NameIdxImgs(st, names, ei, es) ==> ImageOk(p.0, p.1, T)
    decreases |names|
  {
    if |names| > 0 {
      NameIdxImgs_ok(st, T, names[1..], ei, es);
      var nm := names[0];
      var ks := st.nation_name_to_keys[nm];
      if |ks| == 1 {
        var nk := ks[0];
        assert NationsNamed(T, nm) == {nk} by {
          forall x ensures x in NationsNamed(T, nm) <==> x == nk {
            if x in NationsNamed(T, nm) { assert x in ks; assert ks == [nk]; }
            if x == nk { assert ks[0] in ks; }
          }
        }
        var en := if nk in st.nation_numwait then ei(st.nation_numwait[nk].Keys) else [];
        NationRows_ok(st, T, nk, en);
        var pid := NationNamePartition(nm);
        assert IsNamePid(pid);
        assert ImageOk(pid, MakeImage(NationK, nk, st, nm, en), T);
      }
    }
  }

  lemma Build_establishes_Inv(T: TDB, ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires KeysOk(T) && DomOk(T) && EnumOracle(ei, es)
    ensures StoreInv(InstallMap(BuildImages(T, ei, es)), T)
  {
    var st := BuildState(T, ei, es);
    BuildState_facts(T, ei, es);
    var nks := ei(st.nation_numwait.Keys);
    forall nk | nk in nks ensures NkRange(nk) {
      assert nk in st.nation_numwait;
      assert |st.nation_numwait[nk]| > 0;
      var sk :| sk in st.nation_numwait[nk];
      assert sk in NwOf(st, nk);
      assert sk in NWmap(T, nk);
      var s :| s in T.sup && s.sk == sk;
      SupFirst_unique(T, s);
    }
    NationImgs_ok(st, T, nks, ei, es);
    var names := es(st.nation_name_to_keys.Keys);
    NameIdxImgs_ok(st, T, names, ei, es);
    var imgs := BuildImages(T, ei, es);
    var fixed := [(kSupplierMetadata, MakeImage(SupplierMetadata, 0, st, "", [])),
                  (kOrderSummaries, MakeImage(OrderSummaries, 0, st, "", [])),
                  (kNationNameIndex, MakeImage(NationNameIndex, 0, st, "", []))];
    assert imgs == fixed + NationImgs(st, nks, ei, es) + NameIdxImgs(st, names, ei, es);
    forall p | p in imgs ensures ImageOk(p.0, p.1, T) {
      if p in fixed { } else if p in NationImgs(st, nks, ei, es) { } else { }
    }
    InstallMap_facts(imgs);
    var S := InstallMap(imgs);
    forall pid | pid in S ensures ImageOk(pid, S[pid], T) { assert (pid, S[pid]) in imgs; }
    forall nk | NkRange(nk) && NationPartition(nk) !in S ensures NWmap(T, nk) == map[] {
      if NWmap(T, nk) != map[] {
        assert NwOf(st, nk) != map[];
        assert nk in st.nation_numwait;
        assert nk in nks;
        var p :| p in NationImgs(st, nks, ei, es) && p.0 == NationPartition(nk);
        assert p in imgs;
      }
    }
  }

  // ===================== ExecuteFallback =====================

  lemma ExecuteFallback_correct(T: TDB, nn: string, ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires KeysOk(T) && DomOk(T) && EnumOracle(ei, es)
    ensures ExecuteFallback(T, ArgText(nn), ei, es) == SQLRows(T, nn)
  {
    var full := BuildState(T, ei, es);
    BuildState_facts(T, ei, es);
    var K := NationsNamed(T, nn);
    var ks := if nn in full.nation_name_to_keys then full.nation_name_to_keys[nn] else [];
    var nw := map nk | nk in ks && nk in full.nation_numwait :: full.nation_numwait[nk];
    var st := EmptyState().(nation_numwait := nw, supplier_names := full.supplier_names);
    forall nk | nk in st.nation_numwait ensures nk in K && st.nation_numwait[nk] == NWmap(T, nk) {
      assert nk in ks;
      assert NwOf(full, nk) == NWmap(T, nk);
    }
    forall nk | nk in K && nk !in st.nation_numwait ensures NWmap(T, nk) == map[] {
      assert nn in full.nation_name_to_keys;
      assert nk in ks;
      assert NwOf(full, nk) == NWmap(T, nk);
    }
    EmitResult_ok(st, T, K, ei);
    Rows_eq_SQL(EmitResult(st, ei), T, nn);
  }
}
