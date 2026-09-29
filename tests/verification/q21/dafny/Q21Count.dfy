// Q21 counting core: the per-name COUNT of the SQL spine equals the sum of
// per-supplier waits NW(T, sk) over that name's suppliers in nations named nn.
//
//   SpineNameCount :
//     OrdKeyUnique(T) && NatKeyUnique(T) ==>
//       CountNm(Spine(T, nn), nm) == NameSum(T, nn, nm, T.sup)
//
// NW(T, sk) counts lineitems of supplier sk that qualify on their own order
// (order status 'F', late, another supplier exists, no other supplier late).

include "Q21SqlCore.dfy"

module Q21Count {
  import opened Q21Types
  import opened Q21SqlCore

  // ----- Sums over sequences -----

  ghost function SumF<T>(xs: seq<T>, f: T -> int): int
  { if |xs| == 0 then 0 else f(xs[0]) + SumF(xs[1..], f) }

  lemma SumF_app<T>(a: seq<T>, b: seq<T>, f: T -> int)
    ensures SumF(a + b, f) == SumF(a, f) + SumF(b, f)
  {
    if |a| > 0 { assert (a + b)[1..] == a[1..] + b; SumF_app(a[1..], b, f); }
    else { assert a + b == b; }
  }

  lemma SumF_ext<T>(xs: seq<T>, f: T -> int, g: T -> int)
    requires forall x :: x in xs ==> f(x) == g(x)
    ensures SumF(xs, f) == SumF(xs, g)
  {
    if |xs| > 0 { SumF_ext(xs[1..], f, g); }
  }

  lemma SumF_scale<T>(xs: seq<T>, f: T -> int, c: int)
    ensures SumF(xs, (x: T) => c * f(x)) == c * SumF(xs, f)
  {
    if |xs| > 0 { SumF_scale(xs[1..], f, c); }
  }

  lemma SumF_zero<T>(xs: seq<T>, f: T -> int)
    requires forall x :: x in xs ==> f(x) == 0
    ensures SumF(xs, f) == 0
  {
    if |xs| > 0 { SumF_zero(xs[1..], f); }
  }

  lemma SumF_nonneg<T>(xs: seq<T>, f: T -> int)
    requires forall x :: x in xs ==> f(x) >= 0
    ensures SumF(xs, f) >= 0
  {
    if |xs| > 0 { SumF_nonneg(xs[1..], f); }
  }

  // ----- Filters and joins as sums -----

  lemma Sum_FiltWhere(nn: string, X: seq<Q4>, g: Q4 -> int)
    ensures SumF(FiltWhere(nn, X), g) == SumF(X, (x: Q4) => if Where(nn, x) then g(x) else 0)
  {
    if |X| > 0 {
      Sum_FiltWhere(nn, X[1..], g);
      SumF_app(if Where(nn, X[0]) then [X[0]] else [], FiltWhere(nn, X[1..]), g);
    }
  }

  lemma Sum_Semi(X: seq<Q4>, L: seq<Li>, g: Q4 -> int)
    ensures SumF(Semi(X, L), g) == SumF(X, (x: Q4) => if ExistsOther(x, L) then g(x) else 0)
  {
    if |X| > 0 {
      Sum_Semi(X[1..], L, g);
      SumF_app(if ExistsOther(X[0], L) then [X[0]] else [], Semi(X[1..], L), g);
    }
  }

  lemma Sum_Anti(X: seq<Q4>, L: seq<Li>, g: Q4 -> int)
    ensures SumF(Anti(X, L), g) == SumF(X, (x: Q4) => if ExistsOther(x, L) then 0 else g(x))
  {
    if |X| > 0 {
      Sum_Anti(X[1..], L, g);
      SumF_app(if ExistsOther(X[0], L) then [] else [X[0]], Anti(X[1..], L), g);
    }
  }

  lemma Sum_J3a(x: Q3, N: seq<Nat>, g: Q4 -> int)
    ensures SumF(J3a(x, N), g) ==
            SumF(N, (n: Nat) => if x.s.nk == n.nk then g(Q4(x.s, x.l, x.o, n)) else 0)
  {
    if |N| > 0 {
      Sum_J3a(x, N[1..], g);
      SumF_app(if x.s.nk == N[0].nk then [Q4(x.s, x.l, x.o, N[0])] else [], J3a(x, N[1..]), g);
    }
  }

  lemma Sum_J3(X: seq<Q3>, N: seq<Nat>, g: Q4 -> int)
    ensures SumF(J3(X, N), g) ==
            SumF(X, (x: Q3) => SumF(N, (n: Nat) => if x.s.nk == n.nk then g(Q4(x.s, x.l, x.o, n)) else 0))
  {
    if |X| > 0 {
      Sum_J3(X[1..], N, g);
      Sum_J3a(X[0], N, g);
      SumF_app(J3a(X[0], N), J3(X[1..], N), g);
    }
  }

  lemma Sum_J2a(x: Q2, O: seq<Ord>, g: Q3 -> int)
    ensures SumF(J2a(x, O), g) ==
            SumF(O, (o: Ord) => if x.l.ok == o.ok then g(Q3(x.s, x.l, o)) else 0)
  {
    if |O| > 0 {
      Sum_J2a(x, O[1..], g);
      SumF_app(if x.l.ok == O[0].ok then [Q3(x.s, x.l, O[0])] else [], J2a(x, O[1..]), g);
    }
  }

  lemma Sum_J2(X: seq<Q2>, O: seq<Ord>, g: Q3 -> int)
    ensures SumF(J2(X, O), g) ==
            SumF(X, (x: Q2) => SumF(O, (o: Ord) => if x.l.ok == o.ok then g(Q3(x.s, x.l, o)) else 0))
  {
    if |X| > 0 {
      Sum_J2(X[1..], O, g);
      Sum_J2a(X[0], O, g);
      SumF_app(J2a(X[0], O), J2(X[1..], O), g);
    }
  }

  lemma Sum_J1a(s: Sup, L: seq<Li>, g: Q2 -> int)
    ensures SumF(J1a(s, L), g) == SumF(L, (l: Li) => if s.sk == l.sk then g(Q2(s, l)) else 0)
  {
    if |L| > 0 {
      Sum_J1a(s, L[1..], g);
      SumF_app(if s.sk == L[0].sk then [Q2(s, L[0])] else [], J1a(s, L[1..]), g);
    }
  }

  lemma Sum_J1(S: seq<Sup>, L: seq<Li>, g: Q2 -> int)
    ensures SumF(J1(S, L), g) == SumF(S, (s: Sup) => SumF(L, (l: Li) => if s.sk == l.sk then g(Q2(s, l)) else 0))
  {
    if |S| > 0 {
      Sum_J1(S[1..], L, g);
      Sum_J1a(S[0], L, g);
      SumF_app(J1a(S[0], L), J1(S[1..], L), g);
    }
  }

  // ----- Key uniqueness collapses a keyed sum to an existence test -----

  lemma Sum_unique_ord(O: seq<Ord>, ok: int, c: int)
    requires forall i, j :: 0 <= i < j < |O| ==> O[i].ok != O[j].ok
    ensures SumF(O, (o: Ord) => if ok == o.ok && o.st == 'F' then c else 0)
            == (if exists o :: o in O && o.ok == ok && o.st == 'F' then c else 0)
  {
    if |O| > 0 {
      Sum_unique_ord(O[1..], ok, c);
      if O[0].ok == ok {
        assert forall o :: o in O[1..] ==> o.ok != ok by {
          forall o | o in O[1..] ensures o.ok != ok {
            var j :| 0 <= j < |O[1..]| && O[1..][j] == o;
            assert O[0].ok != O[j + 1].ok;
          }
        }
        SumF_zero(O[1..], (o: Ord) => if ok == o.ok && o.st == 'F' then c else 0);
      }
    }
  }

  lemma Sum_unique_nat(N: seq<Nat>, nk: int, nn: string, c: int)
    requires forall i, j :: 0 <= i < j < |N| ==> N[i].nk != N[j].nk
    ensures SumF(N, (n: Nat) => if nk == n.nk && n.name == nn then c else 0)
            == (if exists n :: n in N && n.nk == nk && n.name == nn then c else 0)
  {
    if |N| > 0 {
      Sum_unique_nat(N[1..], nk, nn, c);
      if N[0].nk == nk {
        assert forall n :: n in N[1..] ==> n.nk != nk by {
          forall n | n in N[1..] ensures n.nk != nk {
            var j :| 0 <= j < |N[1..]| && N[1..][j] == n;
            assert N[0].nk != N[j + 1].nk;
          }
        }
        SumF_zero(N[1..], (n: Nat) => if nk == n.nk && n.name == nn then c else 0);
      }
    }
  }

  // ----- Qualification of a single lineitem -----

  ghost predicate OrdF(T: TDB, ok: int)
  { exists o :: o in T.ords && o.ok == ok && o.st == 'F' }

  ghost predicate NatNamed(T: TDB, nk: int, nn: string)
  { exists n :: n in T.nats && n.nk == nk && n.name == nn }

  predicate OtherL(l: Li, l2: Li) { l2.ok == l.ok && l2.sk != l.sk }
  function ExOther(l: Li, L: seq<Li>): bool
  { if |L| == 0 then false else OtherL(l, L[0]) || ExOther(l, L[1..]) }

  lemma ExistsOther_eq(x: Q4, L: seq<Li>)
    ensures ExistsOther(x, L) == ExOther(x.l, L)
  {
    if |L| > 0 { ExistsOther_eq(x, L[1..]); }
  }

  ghost predicate Qual(T: TDB, l: Li)
  { OrdF(T, l.ok) && Late(l) && ExOther(l, T.li) && !ExOther(l, LateLis(T.li)) }

  // Number of qualifying lineitems of supplier sk (with multiplicity).
  ghost function NW(T: TDB, sk: int): int
  { SumF(T.li, (l: Li) => if l.sk == sk && Qual(T, l) then 1 else 0) }

  ghost function NameSum(T: TDB, nn: string, nm: string, S: seq<Sup>): int
  { SumF(S, (s: Sup) => if s.name == nm && NatNamed(T, s.nk, nn) then NW(T, s.sk) else 0) }

  ghost function CountNm(X: seq<Q4>, nm: string): int
  { SumF(X, (x: Q4) => if x.s.name == nm then 1 else 0) }

  lemma SpineNameCount(T: TDB, nn: string, nm: string)
    requires OrdKeyUnique(T) && NatKeyUnique(T)
    ensures CountNm(Spine(T, nn), nm) == NameSum(T, nn, nm, T.sup)
  {
    var L := T.li;
    var L3 := LateLis(L);
    var X1 := J1(T.sup, L);
    var X2 := J2(X1, T.ords);
    var X3 := J3(X2, T.nats);
    var XW := FiltWhere(nn, X3);
    var XS := Semi(XW, L);
    var q: Li -> int := (l: Li) => if Late(l) && ExOther(l, L) && !ExOther(l, L3) then 1 else 0;
    // Tuple-level indicator of the whole WHERE / EXISTS / NOT EXISTS / name test.
    var g3: Q4 -> int := (x: Q4) =>
      if Where(nn, x) && ExistsOther(x, L) && !ExistsOther(x, L3) && x.s.name == nm then 1 else 0;
    // Filters.
    var f0: Q4 -> int := (x: Q4) => if x.s.name == nm then 1 else 0;
    var f1: Q4 -> int := (x: Q4) => if ExistsOther(x, L3) then 0 else f0(x);
    var f2: Q4 -> int := (x: Q4) => if ExistsOther(x, L) then f1(x) else 0;
    var f3: Q4 -> int := (x: Q4) => if Where(nn, x) then f2(x) else 0;
    assert CountNm(Spine(T, nn), nm) == SumF(Anti(XS, L3), f0);
    Sum_Anti(XS, L3, f0);
    SumF_ext(XS, (x: Q4) => if ExistsOther(x, L3) then 0 else f0(x), f1);
    Sum_Semi(XW, L, f1);
    SumF_ext(XW, (x: Q4) => if ExistsOther(x, L) then f1(x) else 0, f2);
    Sum_FiltWhere(nn, X3, f2);
    SumF_ext(X3, (x: Q4) => if Where(nn, x) then f2(x) else 0, f3);
    SumF_ext(X3, f3, g3);
    assert CountNm(Spine(T, nn), nm) == SumF(X3, g3);
    // Nation join.
    Sum_J3(X2, T.nats, g3);
    var g4: Q3 -> int := (x: Q3) => SumF(T.nats, (n: Nat) => if x.s.nk == n.nk then g3(Q4(x.s, x.l, x.o, n)) else 0);
    var g4': Q3 -> int := (x: Q3) =>
      if x.o.st == 'F' && x.s.name == nm && NatNamed(T, x.s.nk, nn) then q(x.l) else 0;
    forall x: Q3 ensures g4(x) == g4'(x) {
      var c := if x.o.st == 'F' && x.s.name == nm then q(x.l) else 0;
      forall n: Nat ensures (if x.s.nk == n.nk then g3(Q4(x.s, x.l, x.o, n)) else 0)
                         == (if x.s.nk == n.nk && n.name == nn then c else 0) {
        var y := Q4(x.s, x.l, x.o, n);
        ExistsOther_eq(y, L);
        ExistsOther_eq(y, L3);
        assert g3(y) == (if x.o.st == 'F' && Late(x.l) && n.name == nn && ExOther(x.l, L)
                            && !ExOther(x.l, L3) && x.s.name == nm then 1 else 0);
      }
      SumF_ext(T.nats, (n: Nat) => if x.s.nk == n.nk then g3(Q4(x.s, x.l, x.o, n)) else 0,
                       (n: Nat) => if x.s.nk == n.nk && n.name == nn then c else 0);
      Sum_unique_nat(T.nats, x.s.nk, nn, c);
    }
    SumF_ext(X2, (x: Q3) => SumF(T.nats, (n: Nat) => if x.s.nk == n.nk then g3(Q4(x.s, x.l, x.o, n)) else 0), g4');
    assert SumF(X3, g3) == SumF(X2, g4');
    // Orders join.
    Sum_J2(X1, T.ords, g4');
    var g5': Q2 -> int := (x: Q2) =>
      if x.s.name == nm && NatNamed(T, x.s.nk, nn) && OrdF(T, x.l.ok) then q(x.l) else 0;
    forall x: Q2 ensures SumF(T.ords, (o: Ord) => if x.l.ok == o.ok then g4'(Q3(x.s, x.l, o)) else 0) == g5'(x) {
      var c := if x.s.name == nm && NatNamed(T, x.s.nk, nn) then q(x.l) else 0;
      SumF_ext(T.ords, (o: Ord) => if x.l.ok == o.ok then g4'(Q3(x.s, x.l, o)) else 0,
                       (o: Ord) => if x.l.ok == o.ok && o.st == 'F' then c else 0);
      Sum_unique_ord(T.ords, x.l.ok, c);
    }
    SumF_ext(X1, (x: Q2) => SumF(T.ords, (o: Ord) => if x.l.ok == o.ok then g4'(Q3(x.s, x.l, o)) else 0), g5');
    assert SumF(X2, g4') == SumF(X1, g5');
    // Supplier/lineitem join.
    Sum_J1(T.sup, L, g5');
    forall s | s in T.sup
      ensures SumF(L, (l: Li) => if s.sk == l.sk then g5'(Q2(s, l)) else 0)
           == (if s.name == nm && NatNamed(T, s.nk, nn) then NW(T, s.sk) else 0)
    {
      if s.name == nm && NatNamed(T, s.nk, nn) {
        SumF_ext(L, (l: Li) => if s.sk == l.sk then g5'(Q2(s, l)) else 0,
                    (l: Li) => if l.sk == s.sk && Qual(T, l) then 1 else 0);
      } else {
        SumF_zero(L, (l: Li) => if s.sk == l.sk then g5'(Q2(s, l)) else 0);
      }
    }
    SumF_ext(T.sup, (s: Sup) => SumF(L, (l: Li) => if s.sk == l.sk then g5'(Q2(s, l)) else 0),
                    (s: Sup) => if s.name == nm && NatNamed(T, s.nk, nn) then NW(T, s.sk) else 0);
  }
}
