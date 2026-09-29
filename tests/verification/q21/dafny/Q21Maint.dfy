// Stage 3.5: every maintenance arm of StageMaintenance preserves Inv_Q21.
//
//   Maint_arms :
//     outcome is ApplyDelta  ==> StoreInv(Publish(snap, updates), T')
//     otherwise              ==> invalidate_all  (store fully invalidated; next
//                                read falls back / rebuilds), or NoOp with no
//                                Q21 deltas (database unchanged).
//
// Premises: StoreInv(snap, T); KeysOk(T), KeysOk(T'), DomOk(T); the
// transaction's lineitem / order deltas are valid and T' is their result;
// no supplier / nation writes (those set requires_rebuild).

include "Q21Delta.dfy"
include "Q21MaintNw.dfy"

module Q21Maint {
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
  import opened Q21Build
  import opened Q21Delta
  import opened Q21MaintNw

  // ===================== Context =====================

  ghost function DeltaSupps(b: TxnBuffer): set<int> { set d | d in b.lineitem_deltas :: d.suppkey }

  ghost function SummarySupps(st: TState, ks: set<int>): set<int>
  { set ok, sk | ok in ks && FindOrder(st, ok).SomeSummary? && sk in FindOrder(st, ok).s.suppliers :: sk }

  ghost predicate MCtx(T: TDB, T2: TDB, b: TxnBuffer, st1: TState, needed0: set<int>, NS: set<int>, U: set<int>)
  {
    KeysOk(T) && KeysOk(T2) && DomOk(T) &&
    T2 == TDB(T.sup, ApplyLi(T.li, b.lineitem_deltas), ApplyOrd(T.ords, b.order_deltas), T.nats) &&
    LiValid(T.li, b.lineitem_deltas) && OrdValid(T.ords, b.order_deltas) &&
    st1.supplier_names == SupNameOf(T) && st1.supplier_nations == SupNatOf(T) &&
    IdxOk(st1, T) && OrdersOk(st1, T) &&
    NS == SummarySupps(st1, AffectedOrderKeys(b)) + DeltaSupps(b) &&
    needed0 == NationsOf(st1, NS) &&
    U == OKeys(T) + OKeys(T2) + AffectedOrderKeys(b)
  }

  ghost function Cmix(T: TDB, T2: TDB, P: set<int>, ok: int, sk: int): int
  { if ok in P then Cv(T2, ok, sk) else Cv(T, ok, sk) }

  ghost function MixF(T: TDB, T2: TDB, P: set<int>, sk: int): int -> int
  { (ok: int) => Cmix(T, T2, P, ok, sk) }

  ghost function Mix(T: TDB, T2: TDB, P: set<int>, U: set<int>, sk: int): int
  { SetSum(U, MixF(T, T2, P, sk)) }

  ghost predicate OrdT2(st: TState, T2: TDB, ok: int)
  {
    (FindOrder(st, ok).SomeSummary? ==>
       FindOrder(st, ok).s == OrderSummary(StatusOf(T2, ok), SummaryOf(T2.li, ok))) &&
    (FindOrder(st, ok).NoSummary? ==> forall sk :: Cv(T2, ok, sk) == 0)
  }

  ghost predicate MInv(st: TState, st1: TState, T: TDB, T2: TDB, P: set<int>, U: set<int>,
                       needed0: set<int>, NS: set<int>)
  {
    st.supplier_names == st1.supplier_names && st.supplier_nations == st1.supplier_nations &&
    st.nation_name_to_keys == st1.nation_name_to_keys &&
    NoZero(st.nation_numwait) &&
    (forall ok :: ok in P ==> OrdT2(st, T2, ok)) &&
    (forall ok :: ok !in P ==> FindOrder(st, ok) == FindOrder(st1, ok)) &&
    (forall nk, sk :: nk in needed0 ==>
       NwVal(st.nation_numwait, nk, sk) ==
         (if sk in SKeys(T) && SupNatOf(T)[sk] == nk then Mix(T, T2, P, U, sk) else 0)) &&
    (forall nk :: nk !in needed0 ==> nk !in st.nation_numwait) &&
    (forall ok, sk :: ok in P && Cv(T2, ok, sk) != Cv(T, ok, sk) ==> sk in NS)
  }

  // ===================== Small facts =====================

  lemma ContribMap_keys(st: char, S: map<int, SOI>)
    ensures forall k :: k in ContribMap(st, S) ==> k in S
    ensures forall k :: k in ContribMap(st, S) ==> ContribMap(st, S)[k] > 0
  {
    if !(st != 'F' || |S| <= 1) && |LateSet(S)| == 1 {
      var x :| x in LateSet(S);
      Singleton(LateSet(S), x);
      assert ContribMap(st, S) == map[x := S[x].late];
    }
  }

  lemma Cv_eq_get(T: TDB, ok: int, sk: int)
    ensures Cv(T, ok, sk) == Get0(ContribMap(StatusOf(T, ok), SummaryOf(T.li, ok)), sk)
  {}

  lemma SummaryOf_none(L: seq<Li>, ok: int)
    requires forall l :: l in L ==> l.ok != ok
    ensures SummaryOf(L, ok) == map[]
  {
    reveal SummaryOf();
    assert SupsOf(L, ok) == {};
  }

  lemma Mix_step(T: TDB, T2: TDB, P: set<int>, U: set<int>, ok: int, sk: int)
    requires ok in U && ok !in P
    ensures Mix(T, T2, P + {ok}, U, sk) == Mix(T, T2, P, U, sk) + (Cv(T2, ok, sk) - Cv(T, ok, sk))
  {
    var f := MixF(T, T2, P, sk);
    var g := MixF(T, T2, P + {ok}, sk);
    SetSum_local_change(U, {ok}, f, g);
    SetSum_single(ok, (x: int) => g(x) - f(x));
  }

  lemma Unaffected_cv(T: TDB, T2: TDB, b: TxnBuffer, ok: int, sk: int)
    requires KeysOk(T)
    requires T2 == TDB(T.sup, ApplyLi(T.li, b.lineitem_deltas), ApplyOrd(T.ords, b.order_deltas), T.nats)
    requires LiValid(T.li, b.lineitem_deltas) && OrdValid(T.ords, b.order_deltas)
    requires ok !in AffectedOrderKeys(b)
    ensures StatusOf(T2, ok) == StatusOf(T, ok)
    ensures SummaryOf(T2.li, ok) == SummaryOf(T.li, ok)
    ensures Cv(T2, ok, sk) == Cv(T, ok, sk)
  {
    forall d | d in b.order_deltas ensures d.orderkey != ok { assert d.orderkey in AffectedOrderKeys(b); }
    forall d | d in b.lineitem_deltas ensures d.orderkey != ok { assert d.orderkey in AffectedOrderKeys(b); }
    Unaffected_status(T.ords, b.order_deltas, ok);
    Summary_after(T.li, b.lineitem_deltas, ok);
    DeltasFor_empty(b.lineitem_deltas, ok);
  }

  lemma Mix_init(T: TDB, T2: TDB, U: set<int>, sk: int)
    requires KeysOk(T) && OKeys(T) <= U
    ensures Mix(T, T2, {}, U, sk) == NW(T, sk)
  {
    SetSum_ext(U, MixF(T, T2, {}, sk), (ok: int) => Cv(T, ok, sk));
    forall ok | ok in U && ok !in OKeys(T) ensures Cv(T, ok, sk) == 0 { Cv_absent(T, ok, sk); }
    SetSum_superset(OKeys(T), U, (ok: int) => Cv(T, ok, sk));
    NW_by_orders(T, sk);
  }

  lemma Mix_final(T: TDB, T2: TDB, b: TxnBuffer, U: set<int>, sk: int)
    requires KeysOk(T) && KeysOk(T2)
    requires T2 == TDB(T.sup, ApplyLi(T.li, b.lineitem_deltas), ApplyOrd(T.ords, b.order_deltas), T.nats)
    requires LiValid(T.li, b.lineitem_deltas) && OrdValid(T.ords, b.order_deltas)
    requires OKeys(T2) <= U && AffectedOrderKeys(b) <= U
    ensures Mix(T, T2, AffectedOrderKeys(b), U, sk) == NW(T2, sk)
  {
    var A := AffectedOrderKeys(b);
    forall ok | ok in U ensures MixF(T, T2, A, sk)(ok) == Cv(T2, ok, sk) {
      if ok !in A { Unaffected_cv(T, T2, b, ok, sk); }
    }
    SetSum_ext(U, MixF(T, T2, A, sk), (ok: int) => Cv(T2, ok, sk));
    forall ok | ok in U && ok !in OKeys(T2) ensures Cv(T2, ok, sk) == 0 { Cv_absent(T2, ok, sk); }
    SetSum_superset(OKeys(T2), U, (ok: int) => Cv(T2, ok, sk));
    NW_by_orders(T2, sk);
  }

  // ===================== One affected order =====================

  lemma {:isolate_assertions} ProcessOrder_step(T: TDB, T2: TDB, b: TxnBuffer, st1: TState, needed0: set<int>, NS: set<int>,
                                                U: set<int>, st: TState, P: set<int>, ok: int, sen0: seq<int>, sen1: seq<int>)
    requires MCtx(T, T2, b, st1, needed0, NS, U)
    requires MInv(st, st1, T, T2, P, U, needed0, NS)
    requires ok in AffectedOrderKeys(b) && ok !in P
    requires IsEnum(sen0, MutableOrderGet(st, ok).suppliers.Keys)
    requires var s0 := MutableOrderGet(st, ok);
             var chg := StatusChange(b.order_deltas, ok);
             var s1 := if chg.Change? then s0.(orderstatus := chg.new_status) else s0;
             IsEnum(sen1, ApplyLiDeltas(s1.suppliers, DeltasFor(b.lineitem_deltas, ok)).Keys)
    ensures var r := ProcessOrder(st, b, ok, sen0, sen1);
            r.SomeState? ==>
              MInv(r.st, st1, T, T2, P + {ok}, U, needed0, NS) &&
              (var s0 := MutableOrderGet(st, ok);
               var chg := StatusChange(b.order_deltas, ok);
               var s1 := if chg.Change? then s0.(orderstatus := chg.new_status) else s0;
               var sup2 := ApplyLiDeltas(s1.suppliers, DeltasFor(b.lineitem_deltas, ok));
               ComputeOrderContributions(s0, sen0).Keys + ComputeOrderContributions(s1.(suppliers := sup2), sen1).Keys <= NS)
  {
    var LD := b.lineitem_deltas;
    var OD := b.order_deltas;
    var chg := StatusChange(OD, ok);
    var first := FirstOldStatus(OD, ok);
    var r := ProcessOrder(st, b, ok, sen0, sen1);
    Status_after(T.ords, OD, ok);
    Summary_after(T.li, LD, ok);
    assert FindOrder(st, ok) == FindOrder(st1, ok);
    var s0 := MutableOrderGet(st, ok);
    var s1 := if chg.Change? then s0.(orderstatus := chg.new_status) else s0;
    var sup2 := ApplyLiDeltas(s1.suppliers, DeltasFor(LD, ok));
    var s2 := s1.(suppliers := sup2);
    Contrib_refines(s0, sen0);
    Contrib_refines(s2, sen1);
    if r.SomeState? {
      if FindOrder(st, ok).NoSummary? && chg.Change? && chg.new_status == 0 as char {
        // Deleted by this transaction: contributes nothing before and after.
        assert r.st == st;
        assert StatusOf(T2, ok) == 0 as char;
        forall sk ensures Cv(T2, ok, sk) == 0 && Cv(T, ok, sk) == 0 {
          assert ContribMap(StatusOf(T2, ok), SummaryOf(T2.li, ok)) == map[];
        }
        forall sk ensures Mix(T, T2, P + {ok}, U, sk) == Mix(T, T2, P, U, sk) {
          assert ok in U;
          Mix_step(T, T2, P, U, ok, sk);
        }
        assert s0 == OrderSummary(0 as char, map[]);
        assert ComputeOrderContributions(s0, sen0) == map[];
        assert ComputeOrderContributions(s2, sen1) == map[];
      } else {
        // Old summary content and old contributions.
        var present := FindOrder(st, ok).SomeSummary?;
        if present {
          assert s0 == OrderSummary(StatusOf(T, ok), SummaryOf(T.li, ok));
        } else {
          assert first.SomeChar? && first.c == 0 as char && chg.Change?;
          Insert_first(T.ords, OD, ok);
          assert ok !in OKeys(T);
          forall l | l in T.li ensures l.ok != ok {
            if l.ok == ok { var o :| o in T.ords && o.ok == l.ok; assert ok in OKeys(T); }
          }
          SummaryOf_none(T.li, ok);
          assert s0 == OrderSummary(0 as char, map[]);
        }
        var oldc := ComputeOrderContributions(s0, sen0);
        forall sk ensures Get0(oldc, sk) == Cv(T, ok, sk) {
          if !present { assert oldc == map[]; }
        }
        // New summary == T2-exact.
        assert s1.orderstatus == StatusOf(T2, ok);
        assert s1.suppliers == SummaryOf(T.li, ok);
        assert sup2 == SummaryOf(T2.li, ok);
        assert s2 == OrderSummary(StatusOf(T2, ok), SummaryOf(T2.li, ok));
        var newc := ComputeOrderContributions(s2, sen1);
        forall sk ensures Get0(newc, sk) == Cv(T2, ok, sk) {}
        // Contribution keys lie in NS.
        ContribMap_keys(s0.orderstatus, s0.suppliers);
        ContribMap_keys(s2.orderstatus, s2.suppliers);
        ApplyLiDeltas_keys(s1.suppliers, DeltasFor(LD, ok));
        forall k | k in oldc ensures k in NS {
          assert k in s0.suppliers;
          assert k in SummarySupps(st1, AffectedOrderKeys(b));
        }
        forall k | k in newc ensures k in NS {
          assert k in sup2;
          if k !in s1.suppliers {
            var d :| d in DeltasFor(LD, ok) && d.suppkey == k;
            DeltasFor_mem(LD, ok, d);
            assert k in DeltaSupps(b);
          } else {
            assert k in SummarySupps(st1, AffectedOrderKeys(b));
          }
        }
        // State after overlay write + contribution deltas.
        var st2 := PutOverlay(st, ok, s2);
        assert NoZero(st2.nation_numwait);
        ApplyContributionDeltas_facts(st2, oldc, newc);
        assert r.st == ApplyContributionDeltas(st2, oldc, newc);
        forall nk, sk | nk in needed0
          ensures NwVal(r.st.nation_numwait, nk, sk) ==
                  (if sk in SKeys(T) && SupNatOf(T)[sk] == nk then Mix(T, T2, P + {ok}, U, sk) else 0)
        {
          Mix_step(T, T2, P, U, ok, sk);
          if sk in st.supplier_nations && st.supplier_nations[sk] == nk && Get0(newc, sk) != Get0(oldc, sk) {
            assert sk in SKeys(T);
          }
        }
        forall nk | nk !in needed0 ensures nk !in r.st.nation_numwait {
          if nk in r.st.nation_numwait {
            var k :| (k in oldc || k in newc) && k in st2.supplier_nations && st2.supplier_nations[k] == nk;
            assert k in NS;
            assert nk in NationsOf(st1, NS);
          }
        }
        forall o | o in P + {ok} ensures OrdT2(r.st, T2, o) {
          if o == ok { assert FindOrder(r.st, ok) == SomeSummary(s2); }
          else { assert FindOrder(r.st, o) == FindOrder(st, o); }
        }
        forall o | o !in P + {ok} ensures FindOrder(r.st, o) == FindOrder(st1, o) {
          assert FindOrder(r.st, o) == FindOrder(st, o);
        }
        forall o, sk | o in P + {ok} && Cv(T2, o, sk) != Cv(T, o, sk) ensures sk in NS {
          if o == ok {
            if Cv(T2, ok, sk) != 0 { assert sk in newc; } else { assert sk in oldc; }
          }
        }
      }
    }
  }

  lemma ProcessOrders_facts(T: TDB, T2: TDB, b: TxnBuffer, st1: TState, needed0: set<int>, NS: set<int>,
                            U: set<int>, st: TState, P: set<int>, ks: seq<int>,
                            ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es)
    requires MCtx(T, T2, b, st1, needed0, NS, U)
    requires MInv(st, st1, T, T2, P, U, needed0, NS)
    requires NoDupSeq(ks)
    requires forall k :: k in ks ==> k in AffectedOrderKeys(b) && k !in P
    ensures var r := ProcessOrders(st, b, ks, ei, es);
            r.SomeStateSet? ==>
              MInv(r.st, st1, T, T2, P + (set k | k in ks), U, needed0, NS) && r.contrib_supps <= NS
    decreases |ks|
  {
    if |ks| == 0 {
      assert P + (set k | k in ks) == P;
    } else {
      var ok := ks[0];
      var s0 := MutableOrderGet(st, ok);
      var chg := StatusChange(b.order_deltas, ok);
      var s1 := if chg.Change? then s0.(orderstatus := chg.new_status) else s0;
      var sup2 := ApplyLiDeltas(s1.suppliers, DeltasFor(b.lineitem_deltas, ok));
      var sen0 := ei(s0.suppliers.Keys);
      var sen1 := ei(sup2.Keys);
      ProcessOrder_step(T, T2, b, st1, needed0, NS, U, st, P, ok, sen0, sen1);
      var r := ProcessOrder(st, b, ok, sen0, sen1);
      if r.SomeState? {
        assert NoDupSeq(ks[1..]) by {
          forall i, j | 0 <= i < j < |ks[1..]| ensures ks[1..][i] != ks[1..][j] { assert ks[i + 1] != ks[j + 1]; }
        }
        forall k | k in ks[1..] ensures k in AffectedOrderKeys(b) && k !in P + {ok} {
          var j :| 0 <= j < |ks[1..]| && ks[1..][j] == k; assert ks[0] != ks[j + 1];
        }
        ProcessOrders_facts(T, T2, b, st1, needed0, NS, U, r.st, P + {ok}, ks[1..], ei, es);
        assert P + {ok} + (set k | k in ks[1..]) == P + (set k | k in ks) by {
          assert forall k :: k in ks <==> (k == ok || k in ks[1..]);
        }
      }
    }
  }

  // ===================== Publication =====================

  function PublishMap(snap: map<int, TImage>, ups: seq<(int, TImage)>): map<int, TImage>
    decreases |ups|
  { if |ups| == 0 then snap else PublishMap(snap[ups[0].0 := ups[0].1], ups[1..]) }

  lemma PublishMap_facts(snap: map<int, TImage>, ups: seq<(int, TImage)>)
    ensures forall pid :: pid in PublishMap(snap, ups) ==>
              (pid, PublishMap(snap, ups)[pid]) in ups ||
              (pid in snap && PublishMap(snap, ups)[pid] == snap[pid] && forall p :: p in ups ==> p.0 != pid)
    ensures forall p :: p in ups ==> p.0 in PublishMap(snap, ups)
    ensures snap.Keys <= PublishMap(snap, ups).Keys
    decreases |ups|
  {
    if |ups| > 0 {
      PublishMap_facts(snap[ups[0].0 := ups[0].1], ups[1..]);
      assert forall p :: p in ups <==> (p == ups[0] || p in ups[1..]);
    }
  }

  lemma NWmap_same(T: TDB, T2: TDB, nk: int)
    requires T2.sup == T.sup && T2.nats == T.nats
    requires forall sk :: sk in SKeys(T) && SupNatOf(T)[sk] == nk ==> NW(T2, sk) == NW(T, sk)
    ensures NWmap(T2, nk) == NWmap(T, nk)
    ensures AnsSet(T2, {nk}) == AnsSet(T, {nk})
    ensures forall nm :: NationsNamed(T2, nm) == NationsNamed(T, nm)
  {
    assert SKeys(T2) == SKeys(T) && SupNameOf(T2) == SupNameOf(T) && SupNatOf(T2) == SupNatOf(T);
    forall nm ensures V(T2, {nk}, nm) == V(T, {nk}, nm) {
      SetSum_ext(SKeys(T), VTerm(T2, {nk}, nm), VTerm(T, {nk}, nm));
    }
    assert SupNames(T2) == SupNames(T);
  }

  lemma FirstName_unique(m: map<string, seq<int>>, names: seq<string>, nk: int, nm: string)
    requires forall x :: x in names ==> x in m
    requires nm in names && nk in m[nm]
    requires forall nm2 :: nm2 in m && nk in m[nm2] ==> nm2 == nm
    ensures FirstNameWith(m, names, nk) == SomeName(nm)
    decreases |names|
  {
    if names[0] != nm {
      assert nm in names[1..];
      FirstName_unique(m, names[1..], nk, nm);
    }
  }

  lemma Name_of_nk(T2: TDB, st: TState, nk: int, nm: string, nm2: string)
    requires KeysOk(T2) && IdxOk(st, T2)
    requires nm in st.nation_name_to_keys && nk in st.nation_name_to_keys[nm]
    requires nm2 in st.nation_name_to_keys && nk in st.nation_name_to_keys[nm2]
    ensures nm2 == nm
  {
    assert nk in NationsNamed(T2, nm) && nk in NationsNamed(T2, nm2);
    var n :| n in T2.nats && n.name == nm && n.nk == nk;
    var n2 :| n2 in T2.nats && n2.name == nm2 && n2.nk == nk;
    var i :| 0 <= i < |T2.nats| && T2.nats[i] == n;
    var j :| 0 <= j < |T2.nats| && T2.nats[j] == n2;
    if i != j { if i < j { assert T2.nats[i].nk != T2.nats[j].nk; } else { assert T2.nats[j].nk != T2.nats[i].nk; } }
  }

  lemma PublishImages_ok(T2: TDB, st: TState, pub: seq<int>, nen: map<int, seq<int>>, names: seq<string>)
    requires KeysOk(T2) && DomOk(T2)
    requires SupOk(st, T2) && IdxOk(st, T2)
    requires forall x :: x in pub ==> NkRange(x) && x in nen && NwOf(st, x) == NWmap(T2, x)
    requires forall x :: x in pub && x in st.nation_numwait ==> IsEnum(nen[x], st.nation_numwait[x].Keys)
    requires forall x :: x in pub && x in st.nation_numwait ==> forall y :: y in nen[x] ==> y in st.nation_numwait[x]
    requires forall x :: x in names ==> x in st.nation_name_to_keys
    ensures forall p :: p in PublishImages(st, pub, nen, names) ==> ImageOk(p.0, p.1, T2)
    decreases |pub|
  {
    if |pub| > 0 {
      PublishImages_ok(T2, st, pub[1..], nen, names);
      var nk := pub[0];
      assert nk in pub;
      NationRows_ok(st, T2, nk, nen[nk]);
      assert IsNatPid(NationPartition(nk));
      var ni := NameImages(st, nk, nen[nk], names);
      forall p | p in ni ensures ImageOk(p.0, p.1, T2) {
        var r := FirstNameWith(st.nation_name_to_keys, names, nk);
        var nm := r.nm;
        var ks := st.nation_name_to_keys[nm];
        assert |ks| == 1 && nk in ks;
        assert NationsNamed(T2, nm) == {nk} by {
          forall x ensures x in NationsNamed(T2, nm) <==> x == nk {
            if x in NationsNamed(T2, nm) { assert x in ks; assert ks == [ks[0]]; assert ks[0] == nk; }
          }
        }
        assert IsNamePid(NationNamePartition(nm));
      }
      assert PublishImages(st, pub, nen, names) ==
        [(NationPartition(nk), MakeImage(NationK, nk, st, "", nen[nk]))] + ni + PublishImages(st, pub[1..], nen, names);
    }
  }

  lemma NameImage_published(st: TState, pub: seq<int>, nen: map<int, seq<int>>, names: seq<string>, nk: int, nm: string)
    requires forall x :: x in pub ==> x in nen
    requires forall x :: x in pub && x in st.nation_numwait ==> forall y :: y in nen[x] ==> y in st.nation_numwait[x]
    requires forall x :: x in names ==> x in st.nation_name_to_keys
    requires nk in pub
    requires FirstNameWith(st.nation_name_to_keys, names, nk) == SomeName(nm) && |st.nation_name_to_keys[nm]| == 1
    ensures exists p :: p in PublishImages(st, pub, nen, names) && p.0 == NationNamePartition(nm)
    decreases |pub|
  {
    if pub[0] != nk { NameImage_published(st, pub[1..], nen, names, nk, nm); }
    else {
      var ni := NameImages(st, nk, nen[nk], names);
      assert ni[0].0 == NationNamePartition(nm);
      assert ni[0] in PublishImages(st, pub, nen, names);
    }
  }

  lemma Publish_ok(T: TDB, T2: TDB, st1: TState, st2: TState, needed0: set<int>, pub: seq<int>,
                   nen: map<int, seq<int>>, names: seq<string>,
                   ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es)
    requires KeysOk(T2) && DomOk(T) && T2.sup == T.sup && T2.nats == T.nats
    requires SupOk(st2, T2) && IdxOk(st2, T2) && OrdersOk(st2, T2)
    requires IsEnum(pub, needed0)
    requires forall x :: x in needed0 ==> NkRange(x) && NwOf(st2, x) == NWmap(T2, x)
    requires nen == NationEnums(st2, pub, ei)
    requires names == es(st2.nation_name_to_keys.Keys)
    ensures forall p :: p in ([(kSupplierMetadata, MakeImage(SupplierMetadata, 0, st2, "", [])),
                              (kOrderSummaries, MakeImage(OrderSummaries, 0, st2, "", [])),
                              (kNationNameIndex, MakeImage(NationNameIndex, 0, st2, "", []))]
                                + PublishImages(st2, pub, nen, names)) ==>
              ImageOk(p.0, p.1, T2)
  {
    assert DomOk(T2);
    forall x | x in pub && x in st2.nation_numwait ensures IsEnum(nen[x], st2.nation_numwait[x].Keys) {}
    PublishImages_ok(T2, st2, pub, nen, names);
  }

  lemma Untouched_ok(T: TDB, T2: TDB, snap: map<int, TImage>, pid: int, ups: seq<(int, TImage)>,
                     st2: TState, needed0: set<int>, pub: seq<int>, nen: map<int, seq<int>>, names: seq<string>)
    requires KeysOk(T) && KeysOk(T2) && T2.sup == T.sup && T2.nats == T.nats
    requires StoreInv(snap, T) && pid in snap
    requires IdxOk(st2, T2)
    requires IsEnum(pub, needed0)
    requires forall x :: x in pub ==> x in nen
    requires forall x :: x in pub && x in st2.nation_numwait ==> forall y :: y in nen[x] ==> y in st2.nation_numwait[x]
    requires IsEnum(names, st2.nation_name_to_keys.Keys)
    requires forall p :: p in PublishImages(st2, pub, nen, names) ==> p in ups
    requires kSupplierMetadata in (set p | p in ups :: p.0) && kOrderSummaries in (set p | p in ups :: p.0) &&
             kNationNameIndex in (set p | p in ups :: p.0)
    requires forall p :: p in ups ==> p.0 != pid
    requires forall nk :: nk !in needed0 ==> NWmap(T2, nk) == NWmap(T, nk) && AnsSet(T2, {nk}) == AnsSet(T, {nk})
    requires forall nm :: NationsNamed(T2, nm) == NationsNamed(T, nm)
    ensures ImageOk(pid, snap[pid], T2)
  {
    var img := snap[pid];
    assert ImageOk(pid, img, T);
    if pid == kSupplierMetadata || pid == kOrderSummaries || pid == kNationNameIndex {
      var p :| p in ups && p.0 == pid;
      assert false;
    } else if IsNatPid(pid) {
      var nk := pid - kNationBase;
      if nk in needed0 {
        assert nk in pub;
        NationPartition_published(st2, pub, nen, names, nk);
        var p :| p in PublishImages(st2, pub, nen, names) && p.0 == NationPartition(nk);
        assert p in ups;
      }
    } else {
      assert IsNamePid(pid);
      var nm := img.nation_name;
      var nk := img.nationkey;
      if nk in needed0 {
        assert nk in NationsNamed(T2, nm);
        assert nm in st2.nation_name_to_keys;
        var ks := st2.nation_name_to_keys[nm];
        assert nk in ks;
        assert |ks| == 1 by {
          if |ks| != 1 {
            assert |ks| >= 2;
            assert ks[0] in NationsNamed(T2, nm) && ks[1] in NationsNamed(T2, nm);
            assert ks[0] != ks[1];
          }
        }
        forall nm2 | nm2 in st2.nation_name_to_keys && nk in st2.nation_name_to_keys[nm2] ensures nm2 == nm {
          Name_of_nk(T2, st2, nk, nm, nm2);
        }
        assert nm in names;
        FirstName_unique(st2.nation_name_to_keys, names, nk, nm);
        assert nk in pub;
        NameImage_published(st2, pub, nen, names, nk, nm);
        var p :| p in PublishImages(st2, pub, nen, names) && p.0 == NationNamePartition(nm);
        assert p in ups;
      }
    }
  }

  lemma Final_nations(T: TDB, T2: TDB, b: TxnBuffer, st1: TState, st2: TState, needed0: set<int>, NS: set<int>, U: set<int>)
    requires MCtx(T, T2, b, st1, needed0, NS, U)
    requires MInv(st2, st1, T, T2, AffectedOrderKeys(b), U, needed0, NS)
    ensures forall nk :: nk in needed0 ==> NwOf(st2, nk) == NWmap(T2, nk)
  {
    forall nk | nk in needed0 ensures NwOf(st2, nk) == NWmap(T2, nk) {
      forall sk ensures NwVal(st2.nation_numwait, nk, sk) ==
                        (if sk in SKeys(T2) && SupNatOf(T2)[sk] == nk then NW(T2, sk) else 0) {
        Mix_final(T, T2, b, U, sk);
      }
      forall sk ensures sk in NwOf(st2, nk) <==> sk in NWmap(T2, nk) {
        if sk in SKeys(T2) { NW_nonneg(T2, sk); }
        var v := NwVal(st2.nation_numwait, nk, sk);
        assert v == (if sk in SKeys(T2) && SupNatOf(T2)[sk] == nk then NW(T2, sk) else 0);
        if sk in NwOf(st2, nk) { assert nk in st2.nation_numwait && sk in st2.nation_numwait[nk]; assert v != 0; }
      }
      forall sk | sk in NwOf(st2, nk) ensures NwOf(st2, nk)[sk] == NWmap(T2, nk)[sk] {
        var v := NwVal(st2.nation_numwait, nk, sk);
        assert v == (if sk in SKeys(T2) && SupNatOf(T2)[sk] == nk then NW(T2, sk) else 0);
      }
    }
  }

  lemma Final_orders(T: TDB, T2: TDB, b: TxnBuffer, st1: TState, st2: TState, needed0: set<int>, NS: set<int>, U: set<int>)
    requires MCtx(T, T2, b, st1, needed0, NS, U)
    requires MInv(st2, st1, T, T2, AffectedOrderKeys(b), U, needed0, NS)
    ensures OrdersOk(st2, T2)
  {
    var A := AffectedOrderKeys(b);
    forall ok ensures OrdT2(st2, T2, ok) {
      if ok !in A {
        assert FindOrder(st2, ok) == FindOrder(st1, ok);
        assert OrdersOk(st1, T);
        forall sk ensures Cv(T2, ok, sk) == Cv(T, ok, sk) { Unaffected_cv(T, T2, b, ok, sk); }
        Unaffected_cv(T, T2, b, ok, 0);
        if FindOrder(st1, ok).SomeSummary? {
          assert FindOrder(st1, ok).s == OrderSummary(StatusOf(T, ok), SummaryOf(T.li, ok));
        } else {
          assert forall sk :: Cv(T, ok, sk) == 0;
        }
      } else {
        assert ok in A;
      }
    }
    forall ok ensures (FindOrder(st2, ok).SomeSummary? ==>
                         FindOrder(st2, ok).s == OrderSummary(StatusOf(T2, ok), SummaryOf(T2.li, ok))) &&
                      (FindOrder(st2, ok).NoSummary? ==> forall sk :: Cv(T2, ok, sk) == 0)
    {
      assert OrdT2(st2, T2, ok);
    }
  }

  lemma Outside_nations(T: TDB, T2: TDB, b: TxnBuffer, st1: TState, st2: TState, needed0: set<int>, NS: set<int>, U: set<int>)
    requires MCtx(T, T2, b, st1, needed0, NS, U)
    requires MInv(st2, st1, T, T2, AffectedOrderKeys(b), U, needed0, NS)
    ensures forall nk :: nk !in needed0 ==> NWmap(T2, nk) == NWmap(T, nk) && AnsSet(T2, {nk}) == AnsSet(T, {nk})
    ensures forall nm :: NationsNamed(T2, nm) == NationsNamed(T, nm)
  {
    var A := AffectedOrderKeys(b);
    forall nk | nk !in needed0 ensures NWmap(T2, nk) == NWmap(T, nk) && AnsSet(T2, {nk}) == AnsSet(T, {nk}) {
      forall sk | sk in SKeys(T) && SupNatOf(T)[sk] == nk ensures NW(T2, sk) == NW(T, sk) {
        Mix_init(T, T2, U, sk);
        Mix_final(T, T2, b, U, sk);
        SetSum_local_change(U, A, MixF(T, T2, {}, sk), MixF(T, T2, A, sk));
        forall ok | ok in A ensures MixF(T, T2, A, sk)(ok) - MixF(T, T2, {}, sk)(ok) == 0 {
          if Cv(T2, ok, sk) != Cv(T, ok, sk) {
            assert sk in NS;
            assert nk in NationsOf(st1, NS);
          }
        }
        SetSum_zero(A, (x: int) => MixF(T, T2, A, sk)(x) - MixF(T, T2, {}, sk)(x));
      }
      NWmap_same(T, T2, nk);
    }
    forall nm ensures NationsNamed(T2, nm) == NationsNamed(T, nm) { assert T2.nats == T.nats; }
  }

  // ===================== Main maintenance theorem =====================

  ghost predicate TxnOk(T: TDB, T2: TDB, b: TxnBuffer)
  {
    KeysOk(T) && KeysOk(T2) && DomOk(T) && StatusNZ(T.ords) &&
    !b.requires_rebuild && b.supplier_delta_count == 0 && b.nation_delta_count == 0 &&
    LiValid(T.li, b.lineitem_deltas) && OrdValid(T.ords, b.order_deltas) &&
    T2 == TDB(T.sup, ApplyLi(T.li, b.lineitem_deltas), ApplyOrd(T.ords, b.order_deltas), T.nats)
  }

  lemma {:isolate_assertions} Maint_ApplyDelta(T: TDB, T2: TDB, b: TxnBuffer, dec1: Action, dec2: Action,
                                               snap: map<int, TImage>,
                                               ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es)
    requires TxnOk(T, T2, b)
    requires StoreInv(snap, T)
    ensures var out := StageMaintenance(b, dec1, dec2, snap, snap, ei, es);
            out.action == ApplyDelta && !out.invalidate_all ==> StoreInv(PublishMap(snap, out.updates), T2)
  {
    var out := StageMaintenance(b, dec1, dec2, snap, snap, ei, es);
    if out.action == ApplyDelta && !out.invalidate_all {
      assert !(|b.lineitem_deltas| == 0 && |b.order_deltas| == 0);
      assert dec2 == ApplyDelta;
      var ls := LoadSide(snap);
      assert ls.SomeState?;
      assert ImageOk(kSupplierMetadata, snap[kSupplierMetadata], T);
      assert ImageOk(kOrderSummaries, snap[kOrderSummaries], T);
      assert ImageOk(kNationNameIndex, snap[kNationNameIndex], T);
      var st := ls.st;
      var needed0 := NeededBeforeLoad(st, b);
      var nks := IntKeys(needed0);
      IntKeys_enum(needed0);
      forall nk | nk in nks ensures NkRange(nk) {
        assert nk in needed0;
        var sk :| sk in (set ok, sk | ok in AffectedOrderKeys(b) && FindOrder(st, ok).SomeSummary? &&
                                     sk in FindOrder(st, ok).s.suppliers :: sk) + (set d | d in b.lineitem_deltas :: d.suppkey)
                  && sk in st.supplier_nations && st.supplier_nations[sk] == nk;
        var s :| s in T.sup && s.sk == sk;
        SupFirst_unique(T, s);
      }
      LoadNations_some(st, snap, nks, T);
      LoadNations_nw(st, snap, nks, T);
      var st1 := LoadNations(st, snap, nks).st;
      var A := AffectedOrderKeys(b);
      var NS := SummarySupps(st1, A) + DeltaSupps(b);
      var U := OKeys(T) + OKeys(T2) + A;
      assert FindOrder(st1, 0) == FindOrder(st, 0);
      assert forall ok :: FindOrder(st1, ok) == FindOrder(st, ok);
      assert NS == (set ok, sk | ok in A && FindOrder(st, ok).SomeSummary? && sk in FindOrder(st, ok).s.suppliers :: sk)
                   + (set d | d in b.lineitem_deltas :: d.suppkey);
      assert needed0 == NationsOf(st1, NS);
      assert MCtx(T, T2, b, st1, needed0, NS, U);
      // Initial invariant (P = {}).
      forall nk, sk | nk in needed0
        ensures NwVal(st1.nation_numwait, nk, sk) ==
                (if sk in SKeys(T) && SupNatOf(T)[sk] == nk then Mix(T, T2, {}, U, sk) else 0)
      {
        Mix_init(T, T2, U, sk);
        if sk in SKeys(T) { NW_nonneg(T, sk); }
        assert nk in nks;
        if nk in st1.nation_numwait { assert st1.nation_numwait[nk] == NWmap(T, nk); }
        else { assert NWmap(T, nk) == map[]; }
      }
      assert NoZero(st1.nation_numwait) by {
        forall nk, sk | nk in st1.nation_numwait && sk in st1.nation_numwait[nk] ensures st1.nation_numwait[nk][sk] != 0 {
          assert st1.nation_numwait[nk] == NWmap(T, nk);
        }
      }
      assert MInv(st1, st1, T, T2, {}, U, needed0, NS);
      var aks := IntKeys(A);
      IntKeys_enum(A);
      ProcessOrders_facts(T, T2, b, st1, needed0, NS, U, st1, {}, aks, ei, es);
      var po := ProcessOrders(st1, b, aks, ei, es);
      assert po.SomeStateSet?;
      assert {} + (set k | k in aks) == A;
      var st2 := po.st;
      assert MInv(st2, st1, T, T2, A, U, needed0, NS);
      // Final orders and nations are exact for T2.
      Final_nations(T, T2, b, st1, st2, needed0, NS, U);
      Final_orders(T, T2, b, st1, st2, needed0, NS, U);
      Outside_nations(T, T2, b, st1, st2, needed0, NS, U);
      // Published nations == needed0.
      var deltaSupps := set d | d in b.lineitem_deltas :: d.suppkey;
      var published := needed0 + NationsOf(st2, deltaSupps) + NationsOf(st2, po.contrib_supps);
      assert published == needed0;
      var pub := IntKeys(published);
      IntKeys_enum(published);
      var nen := NationEnums(st2, pub, ei);
      var names := es(st2.nation_name_to_keys.Keys);
      assert out.updates ==
        [(kSupplierMetadata, MakeImage(SupplierMetadata, 0, st2, "", [])),
         (kOrderSummaries, MakeImage(OrderSummaries, 0, st2, "", [])),
         (kNationNameIndex, MakeImage(NationNameIndex, 0, st2, "", []))]
        + PublishImages(st2, pub, nen, names);
      assert SupOk(st2, T2) && IdxOk(st2, T2);
      forall x | x in needed0 ensures NkRange(x) { assert x in nks; }
      Publish_ok(T, T2, st1, st2, needed0, pub, nen, names, ei, es);
      var ups := out.updates;
      assert forall p :: p in ups ==> ImageOk(p.0, p.1, T2);
      PublishMap_facts(snap, ups);
      var S2 := PublishMap(snap, ups);
      assert ups[0].0 == kSupplierMetadata && ups[1].0 == kOrderSummaries && ups[2].0 == kNationNameIndex;
      assert forall p :: p in PublishImages(st2, pub, nen, names) ==> p in ups;
      forall pid | pid in S2 ensures ImageOk(pid, S2[pid], T2) {
        if (pid, S2[pid]) !in ups {
          assert pid in snap && S2[pid] == snap[pid];
          Untouched_ok(T, T2, snap, pid, ups, st2, needed0, pub, nen, names);
        }
      }
      forall nk | NkRange(nk) && NationPartition(nk) !in S2 ensures NWmap(T2, nk) == map[] {
        assert NationPartition(nk) !in snap;
        if nk in needed0 {
          NationPartition_published(st2, pub, nen, names, nk);
          var p :| p in PublishImages(st2, pub, nen, names) && p.0 == NationPartition(nk);
          assert p in ups;
          assert false;
        }
      }
    }
  }

  lemma NationPartition_published(st: TState, pub: seq<int>, nen: map<int, seq<int>>, names: seq<string>, nk: int)
    requires forall x :: x in pub ==> x in nen
    requires forall x :: x in pub && x in st.nation_numwait ==> forall y :: y in nen[x] ==> y in st.nation_numwait[x]
    requires forall x :: x in names ==> x in st.nation_name_to_keys
    requires nk in pub
    ensures exists p :: p in PublishImages(st, pub, nen, names) && p.0 == NationPartition(nk)
    decreases |pub|
  {
    if pub[0] != nk { NationPartition_published(st, pub[1..], nen, names, nk); }
    else { assert PublishImages(st, pub, nen, names)[0].0 == NationPartition(nk); }
  }

  // Every other arm is fail-closed: it invalidates all partitions (the next
  // read falls back to a full scan / lazy rebuild), or it is a NoOp with no
  // Q21-relevant deltas (the database is unchanged).
  lemma Maint_other_arms(b: TxnBuffer, dec1: Action, dec2: Action, snap: map<int, TImage>,
                         ei: set<int> -> seq<int>, es: set<string> -> seq<string>)
    requires EnumOracle(ei, es)
    ensures var out := StageMaintenance(b, dec1, dec2, snap, snap, ei, es);
            (out.action == ApplyDelta && !out.invalidate_all) ||
            out.invalidate_all ||
            (out == Outcome(NoOp, false, []) && !b.requires_rebuild &&
             |b.lineitem_deltas| == 0 && |b.order_deltas| == 0)
  {}

  // A buffer that never saw a supplier / nation mutation or a parse failure
  // (the only ways MarkRebuild is reached) has requires_rebuild == false and
  // zero supplier / nation delta counts.
  lemma Accumulate_counts(ms: seq<Mutation>, b: TxnBuffer)
    requires b.supplier_delta_count == 0 && b.nation_delta_count == 0
    ensures var r := AccumulateAll(ms, b); r.supplier_delta_count == 0 && r.nation_delta_count == 0
    decreases |ms|
  {
    if |ms| > 0 { AccumulateOne_counts(ms[0], b); Accumulate_counts(ms[1..], AccumulateMutation(ms[0], b)); }
  }

  function AccumulateAll(ms: seq<Mutation>, b: TxnBuffer): TxnBuffer
    decreases |ms|
  { if |ms| == 0 then b else AccumulateAll(ms[1..], AccumulateMutation(ms[0], b)) }

  lemma AccumulateOne_counts(m: Mutation, b: TxnBuffer)
    requires b.supplier_delta_count == 0 && b.nation_delta_count == 0
    ensures AccumulateMutation(m, b).supplier_delta_count == 0 && AccumulateMutation(m, b).nation_delta_count == 0
  {
    match m {
      case LiMut(kind, olds, news) =>
        match kind {
          case Insert => LiRows_counts(b, news, 1);
          case Delete => LiRows_counts(b, olds, -1);
          case Update => if |olds| == |news| { LiPairs_counts(b, olds, news); }
        }
      case OrdMut(kind, olds, news) =>
        match kind {
          case Insert => OrdRows_counts(b, news, 1);
          case Delete => OrdRows_counts(b, olds, -1);
          case Update => if |olds| == |news| { OrdPairs_counts(b, olds, news); }
        }
      case _ =>
    }
  }

  lemma LiRows_counts(b: TxnBuffer, rows: seq<OptLi>, sign: int)
    requires b.supplier_delta_count == 0 && b.nation_delta_count == 0
    ensures AddLiRows(b, rows, sign).supplier_delta_count == 0 && AddLiRows(b, rows, sign).nation_delta_count == 0
    decreases |rows|
  {
    if |rows| > 0 {
      var b2 := match rows[0] case LiBad => MarkRebuild(b) case LiOk(l) => AddLineitemDelta(b, LineitemDelta(l.ok, l.sk, l.cd, l.rd, sign));
      LiRows_counts(b2, rows[1..], sign);
    }
  }

  lemma LiPairs_counts(b: TxnBuffer, olds: seq<OptLi>, news: seq<OptLi>)
    requires |olds| == |news|
    requires b.supplier_delta_count == 0 && b.nation_delta_count == 0
    ensures AddLiPairs(b, olds, news).supplier_delta_count == 0 && AddLiPairs(b, olds, news).nation_delta_count == 0
    decreases |olds|
  {
    if |olds| > 0 {
      LiRows_counts(b, [olds[0]], -1);
      LiRows_counts(AddLiRows(b, [olds[0]], -1), [news[0]], 1);
      LiPairs_counts(AddLiRows(AddLiRows(b, [olds[0]], -1), [news[0]], 1), olds[1..], news[1..]);
    }
  }

  lemma OrdRows_counts(b: TxnBuffer, rows: seq<OptOrd>, sign: int)
    requires b.supplier_delta_count == 0 && b.nation_delta_count == 0
    ensures AddOrdRows(b, rows, sign).supplier_delta_count == 0 && AddOrdRows(b, rows, sign).nation_delta_count == 0
    decreases |rows|
  {
    if |rows| > 0 {
      var b2 := match rows[0]
        case OrdBad => MarkRebuild(b)
        case OrdOk(o) => AddOrderDelta(b, OrderDelta(o.ok, if sign > 0 then 0 as char else o.st, if sign > 0 then o.st else 0 as char));
      OrdRows_counts(b2, rows[1..], sign);
    }
  }

  lemma OrdPairs_counts(b: TxnBuffer, olds: seq<OptOrd>, news: seq<OptOrd>)
    requires |olds| == |news|
    requires b.supplier_delta_count == 0 && b.nation_delta_count == 0
    ensures AddOrdPairs(b, olds, news).supplier_delta_count == 0 && AddOrdPairs(b, olds, news).nation_delta_count == 0
    decreases |olds|
  {
    if |olds| > 0 {
      match (olds[0], news[0])
      case (OrdOk(o1), OrdOk(o2)) =>
        if o1.ok == o2.ok {
          OrdPairs_counts(AddOrderDelta(b, OrderDelta(o1.ok, o1.st, o2.st)), olds[1..], news[1..]);
        }
      case _ =>
    }
  }
}
