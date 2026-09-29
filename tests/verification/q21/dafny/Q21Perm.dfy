// Row-order independence: every ingredient of Inv_Q21 and of the SQL answer
// depends on each table only as a multiset (under the primary keys).
//
//   StoreInv_perm : TPerm(T, T2) && KeysOk(T) && KeysOk(T2) ==>
//                   (StoreInv(snap, T) <==> StoreInv(snap, T2))
//   SQLRows_perm  : TPerm(T, T2) && KeysOk(T) && KeysOk(T2) ==> SQLRows(T, nn) == SQLRows(T2, nn)

include "Q21Inv.dfy"

module Q21Perm {
  import opened Q21Types
  import opened Q21SqlCore
  import opened Q21Count
  import opened Q21SetSum
  import opened Q21Contrib
  import opened Q21ModuleTwin
  import opened Q21Sorted
  import opened Q21Answer
  import opened Q21Inv

  ghost predicate TPerm(T: TDB, T2: TDB)
  {
    multiset(T.sup) == multiset(T2.sup) && multiset(T.li) == multiset(T2.li) &&
    multiset(T.ords) == multiset(T2.ords) && multiset(T.nats) == multiset(T2.nats)
  }

  lemma SumF_at<X>(b: seq<X>, i: int, f: X -> int)
    requires 0 <= i < |b|
    ensures SumF(b, f) == f(b[i]) + SumF(b[..i] + b[i+1..], f)
  {
    assert b == b[..i] + ([b[i]] + b[i+1..]);
    SumF_app(b[..i], [b[i]] + b[i+1..], f);
    SumF_app(b[..i], b[i+1..], f);
    assert ([b[i]] + b[i+1..])[1..] == b[i+1..];
  }

  lemma Multiset_remove_at<X>(b: seq<X>, i: int)
    requires 0 <= i < |b|
    ensures multiset(b[..i] + b[i+1..]) == multiset(b) - multiset{b[i]}
  {
    assert b == b[..i] + [b[i]] + b[i+1..];
  }

  lemma SumF_perm<X>(a: seq<X>, b: seq<X>, f: X -> int)
    requires multiset(a) == multiset(b)
    ensures SumF(a, f) == SumF(b, f)
    decreases |a|
  {
    if |a| == 0 {
      assert |b| == |multiset(b)| == 0;
    } else {
      var x := a[0];
      assert x in multiset(b);
      var i :| 0 <= i < |b| && b[i] == x;
      var b' := b[..i] + b[i+1..];
      Multiset_remove_at(b, i);
      assert a == [x] + a[1..];
      assert multiset(a[1..]) == multiset(a) - multiset{x};
      SumF_perm(a[1..], b', f);
      SumF_at(b, i, f);
    }
  }

  lemma Mem_perm<X>(a: seq<X>, b: seq<X>, x: X)
    requires multiset(a) == multiset(b)
    ensures x in a <==> x in b
  {
    assert x in a <==> x in multiset(a);
    assert x in b <==> x in multiset(b);
  }

  lemma Views_perm(T: TDB, T2: TDB)
    requires TPerm(T, T2) && KeysOk(T) && KeysOk(T2)
    ensures SKeys(T) == SKeys(T2) && SupNameOf(T) == SupNameOf(T2) && SupNatOf(T) == SupNatOf(T2)
    ensures SupNames(T) == SupNames(T2)
    ensures forall nm :: NationsNamed(T, nm) == NationsNamed(T2, nm)
    ensures OKeys(T) == OKeys(T2)
    ensures forall ok :: StatusOf(T, ok) == StatusOf(T2, ok)
    ensures forall ok :: SummaryOf(T.li, ok) == SummaryOf(T2.li, ok)
    ensures forall ok, sk :: Cv(T, ok, sk) == Cv(T2, ok, sk)
    ensures forall sk :: NW(T, sk) == NW(T2, sk)
  {
    forall s ensures s in T.sup <==> s in T2.sup { Mem_perm(T.sup, T2.sup, s); }
    forall l ensures l in T.li <==> l in T2.li { Mem_perm(T.li, T2.li, l); }
    forall o ensures o in T.ords <==> o in T2.ords { Mem_perm(T.ords, T2.ords, o); }
    forall n ensures n in T.nats <==> n in T2.nats { Mem_perm(T.nats, T2.nats, n); }
    assert SKeys(T) == SKeys(T2);
    forall sk | sk in SKeys(T) ensures SupFirst(T.sup, sk) == SupFirst(T2.sup, sk) {
      var s :| s in T.sup && s.sk == sk;
      SupFirst_unique(T, s);
      SupFirst_unique(T2, s);
    }
    assert SupNames(T) == SupNames(T2);
    forall ok ensures StatusOf(T, ok) == StatusOf(T2, ok) {
      if exists o :: o in T.ords && o.ok == ok {
        var o :| o in T.ords && o.ok == ok;
        StatusOf_row_seq(T.ords, o);
        StatusOf_row_seq(T2.ords, o);
      } else {
        StatusOf_absent(T.ords, ok);
        StatusOf_absent(T2.ords, ok);
      }
    }
    forall ok ensures SummaryOf(T.li, ok) == SummaryOf(T2.li, ok) {
      reveal SummaryOf();
      assert SupsOf(T.li, ok) == SupsOf(T2.li, ok);
      forall sk ensures Tot(T.li, ok, sk) == Tot(T2.li, ok, sk) && LateC(T.li, ok, sk) == LateC(T2.li, ok, sk) {
        SumF_perm(T.li, T2.li, (l: Li) => if l.ok == ok && l.sk == sk then 1 else 0);
        SumF_perm(T.li, T2.li, (l: Li) => if l.ok == ok && l.sk == sk && Late(l) then 1 else 0);
      }
    }
    forall l ensures Qual(T, l) == Qual(T2, l) {
      ExOther_iff(l, T.li); ExOther_iff(l, T2.li);
      ExOther_iff(l, LateLis(T.li)); ExOther_iff(l, LateLis(T2.li));
      forall x ensures x in LateLis(T.li) <==> x in LateLis(T2.li) { LateLis_mem(T.li, x); LateLis_mem(T2.li, x); }
    }
    forall sk ensures NW(T, sk) == NW(T2, sk) {
      SumF_perm(T.li, T2.li, (l: Li) => if l.sk == sk && Qual(T, l) then 1 else 0);
      SumF_ext(T2.li, (l: Li) => if l.sk == sk && Qual(T, l) then 1 else 0, (l: Li) => if l.sk == sk && Qual(T2, l) then 1 else 0);
    }
  }

  lemma StatusOf_row_seq(O: seq<Ord>, o: Ord)
    requires forall i, j :: 0 <= i < j < |O| ==> O[i].ok != O[j].ok
    requires o in O
    ensures StatusOfSeq(O, o.ok) == o.st
  {
    if O[0] != o {
      if O[0].ok == o.ok { var k :| 0 <= k < |O| && O[k] == o; assert k > 0 && O[0].ok != O[k].ok; }
      assert o in O[1..];
      assert forall i, j :: 0 <= i < j < |O[1..]| ==> O[1..][i].ok != O[1..][j].ok by {
        forall i, j | 0 <= i < j < |O[1..]| ensures O[1..][i].ok != O[1..][j].ok { assert O[i + 1].ok != O[j + 1].ok; }
      }
      StatusOf_row_seq(O[1..], o);
    }
  }

  lemma Answer_perm(T: TDB, T2: TDB)
    requires TPerm(T, T2) && KeysOk(T) && KeysOk(T2)
    ensures forall nk :: NWmap(T, nk) == NWmap(T2, nk)
    ensures forall K, nm :: V(T, K, nm) == V(T2, K, nm)
    ensures forall K :: AnsSet(T, K) == AnsSet(T2, K)
  {
    Views_perm(T, T2);
    forall K, nm ensures V(T, K, nm) == V(T2, K, nm) {
      SetSum_ext(SKeys(T), VTerm(T, K, nm), VTerm(T2, K, nm));
    }
  }

  lemma StoreInv_perm(snap: map<int, TImage>, T: TDB, T2: TDB)
    requires TPerm(T, T2) && KeysOk(T) && KeysOk(T2)
    requires StoreInv(snap, T)
    ensures StoreInv(snap, T2)
  {
    Views_perm(T, T2);
    Answer_perm(T, T2);
    forall pid | pid in snap ensures ImageOk(pid, snap[pid], T2) {
      assert ImageOk(pid, snap[pid], T);
    }
  }

  lemma SQLRows_perm(T: TDB, T2: TDB, nn: string)
    requires TPerm(T, T2) && KeysOk(T) && KeysOk(T2)
    ensures SQLRows(T, nn) == SQLRows(T2, nn)
  {
    Views_perm(T, T2);
    Answer_perm(T, T2);
    SQLRows_sorted_answer(T, nn);
    SQLRows_sorted_answer(T2, nn);
    StrictSorted_unique(SQLRows(T, nn), SQLRows(T2, nn));
  }
}
