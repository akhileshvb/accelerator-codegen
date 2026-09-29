// Q21 semantic core, part 1: the RelNode spine of LogicalSpec_Q21_rel
// (3 inner joins, WHERE, EXISTS l2 semi-join, NOT EXISTS l3 anti-join)
// evaluated by the generic Dafny RelNode interpreter on ToRel(T) equals a
// typed nested-loop comprehension (Anti(T, nn)), in interpreter row order.

include "Q21Types.dfy"
include "../../logical/dafny/EvalLogicalSpec.dfy"
include "generated/Q21RelNodeData.dfy"

module Q21SqlCore {
  import opened LogicalSpec
  import opened RelEvalLogicalSpec
  import opened Q21Types
  import Q21RelNodeData

  // ----- Typed join tuples and their RelNode rows -----

  datatype Q2 = Q2(s: Sup, l: Li)
  datatype Q3 = Q3(s: Sup, l: Li, o: Ord)
  datatype Q4 = Q4(s: Sup, l: Li, o: Ord, n: Nat)

  function Q2Row(x: Q2): Row { SupRow(x.s) + LiRow(x.l) }
  function Q3Row(x: Q3): Row { SupRow(x.s) + LiRow(x.l) + OrdRow(x.o) }
  function Q4Row(x: Q4): Row { SupRow(x.s) + LiRow(x.l) + OrdRow(x.o) + NatRow(x.n) }

  function Q2Rows(xs: seq<Q2>): seq<Row>
  { if |xs| == 0 then [] else [Q2Row(xs[0])] + Q2Rows(xs[1..]) }
  function Q3Rows(xs: seq<Q3>): seq<Row>
  { if |xs| == 0 then [] else [Q3Row(xs[0])] + Q3Rows(xs[1..]) }
  function Q4Rows(xs: seq<Q4>): seq<Row>
  { if |xs| == 0 then [] else [Q4Row(xs[0])] + Q4Rows(xs[1..]) }

  lemma Q2Rows_app(a: seq<Q2>, b: seq<Q2>)
    ensures Q2Rows(a + b) == Q2Rows(a) + Q2Rows(b)
  {
    if |a| > 0 { assert (a + b)[1..] == a[1..] + b; Q2Rows_app(a[1..], b); }
    else { assert a + b == b; }
  }
  lemma Q3Rows_app(a: seq<Q3>, b: seq<Q3>)
    ensures Q3Rows(a + b) == Q3Rows(a) + Q3Rows(b)
  {
    if |a| > 0 { assert (a + b)[1..] == a[1..] + b; Q3Rows_app(a[1..], b); }
    else { assert a + b == b; }
  }
  lemma Q4Rows_app(a: seq<Q4>, b: seq<Q4>)
    ensures Q4Rows(a + b) == Q4Rows(a) + Q4Rows(b)
  {
    if |a| > 0 { assert (a + b)[1..] == a[1..] + b; Q4Rows_app(a[1..], b); }
    else { assert a + b == b; }
  }

  // ----- Typed nested-loop spine (interpreter order) -----

  function J1a(s: Sup, L: seq<Li>): seq<Q2>
  { if |L| == 0 then [] else (if s.sk == L[0].sk then [Q2(s, L[0])] else []) + J1a(s, L[1..]) }
  function J1(S: seq<Sup>, L: seq<Li>): seq<Q2>
  { if |S| == 0 then [] else J1a(S[0], L) + J1(S[1..], L) }

  function J2a(x: Q2, O: seq<Ord>): seq<Q3>
  { if |O| == 0 then [] else (if x.l.ok == O[0].ok then [Q3(x.s, x.l, O[0])] else []) + J2a(x, O[1..]) }
  function J2(X: seq<Q2>, O: seq<Ord>): seq<Q3>
  { if |X| == 0 then [] else J2a(X[0], O) + J2(X[1..], O) }

  function J3a(x: Q3, N: seq<Nat>): seq<Q4>
  { if |N| == 0 then [] else (if x.s.nk == N[0].nk then [Q4(x.s, x.l, x.o, N[0])] else []) + J3a(x, N[1..]) }
  function J3(X: seq<Q3>, N: seq<Nat>): seq<Q4>
  { if |X| == 0 then [] else J3a(X[0], N) + J3(X[1..], N) }

  // WHERE o_orderstatus = 'F' AND l_receiptdate > l_commitdate AND n_name = :nation
  predicate Where(nn: string, x: Q4) { x.o.st == 'F' && Late(x.l) && x.n.name == nn }
  function FiltWhere(nn: string, X: seq<Q4>): seq<Q4>
  { if |X| == 0 then [] else (if Where(nn, X[0]) then [X[0]] else []) + FiltWhere(nn, X[1..]) }

  // EXISTS l2: same order, different supplier.
  predicate OtherSupp(x: Q4, l2: Li) { l2.ok == x.l.ok && l2.sk != x.l.sk }
  function ExistsOther(x: Q4, L: seq<Li>): bool
  { if |L| == 0 then false else OtherSupp(x, L[0]) || ExistsOther(x, L[1..]) }
  function Semi(X: seq<Q4>, L: seq<Li>): seq<Q4>
  { if |X| == 0 then [] else (if ExistsOther(X[0], L) then [X[0]] else []) + Semi(X[1..], L) }

  // NOT EXISTS l3: same order, different supplier, late.
  function LateLis(L: seq<Li>): seq<Li>
  { if |L| == 0 then [] else (if Late(L[0]) then [L[0]] else []) + LateLis(L[1..]) }
  function Anti(X: seq<Q4>, L3: seq<Li>): seq<Q4>
  { if |X| == 0 then [] else (if ExistsOther(X[0], L3) then [] else [X[0]]) + Anti(X[1..], L3) }

  function Spine(T: TDB, nn: string): seq<Q4>
  {
    Anti(Semi(FiltWhere(nn, J3(J2(J1(T.sup, T.li), T.ords), T.nats)), T.li), LateLis(T.li))
  }

  // ----- The RelNode subtrees of LogicalSpec_Q21_rel -----

  const C1: Expr := EBinary(BEq, EInputRef(0), EInputRef(4))
  const C2: Expr := EBinary(BEq, EInputRef(3), EInputRef(7))
  const C3: Expr := EBinary(BEq, EInputRef(2), EInputRef(9))
  const W: Expr :=
    EBinary(BAnd, EBinary(BEq, EInputRef(8), EConst(VText("F"), TText)),
      EBinary(BAnd, EBinary(BGt, EInputRef(6), EInputRef(5)),
        EBinary(BEq, EInputRef(10), EParam("nation_name", TText))))
  const SC: Expr :=
    EBinary(BAnd, EBinary(BEq, EInputRef(3), EInputRef(11)),
      EBinary(BNe, EInputRef(4), EInputRef(12)))
  const LateP: Expr := EBinary(BGt, EInputRef(3), EInputRef(2))

  const N1: RelNode := RelJoin(JoinInner, RelScan("supplier"), RelScan("lineitem"), C1)
  const N2: RelNode := RelJoin(JoinInner, N1, RelScan("orders"), C2)
  const N3: RelNode := RelJoin(JoinInner, N2, RelScan("nation"), C3)
  const NF: RelNode := RelFilter(N3, W)
  const NS: RelNode := RelSemiJoin(NF, RelScan("lineitem"), SC)
  const NA: RelNode := RelAntiJoin(NS, RelFilter(RelScan("lineitem"), LateP), SC)
  const Gk: seq<Expr> := [EInputRef(1)]
  const Ag: seq<AggSpec> := [AggSpec(AggCountStar, NoExpr)]
  const NG: RelNode := RelAggregate(NA, Gk, Ag)
  const PE: seq<Expr> := [EGroupRef(0), EAggRef(0)]
  const NP: RelNode := RelProject(NG, PE)
  const OKs: seq<OrderKey> :=
    [OrderKey(EInputRef(1), SortDesc, NullsFirst), OrderKey(EInputRef(0), SortAsc, NullsLast)]
  const NTop: RelNode := RelSort(NP, OKs)

  lemma Q21_spec_shape()
    ensures Q21RelNodeData.LogicalSpec_Q21_rel == NTop
  {
  }

  // ----- Scans -----

  lemma Scan_lookup(T: TDB)
    ensures DbLookup(ToRel(T), "supplier") == SomeRows(SupRows(T.sup))
    ensures DbLookup(ToRel(T), "lineitem") == SomeRows(LiRows(T.li))
    ensures DbLookup(ToRel(T), "orders") == SomeRows(OrdRows(T.ords))
    ensures DbLookup(ToRel(T), "nation") == SomeRows(NatRows(T.nats))
  {
    var db := ToRel(T);
    assert db[1..] == [("lineitem", LiRows(T.li)), ("orders", OrdRows(T.ords)), ("nation", NatRows(T.nats))];
    assert db[1..][1..] == [("orders", OrdRows(T.ords)), ("nation", NatRows(T.nats))];
    assert db[1..][1..][1..] == [("nation", NatRows(T.nats))];
    assert DbLookup(db[1..][1..][1..], "nation") == SomeRows(NatRows(T.nats));
    assert DbLookup(db[1..][1..], "nation") == SomeRows(NatRows(T.nats));
    assert DbLookup(db[1..], "nation") == SomeRows(NatRows(T.nats));
    assert DbLookup(db[1..][1..], "orders") == SomeRows(OrdRows(T.ords));
    assert DbLookup(db[1..], "orders") == SomeRows(OrdRows(T.ords));
  }

  // ----- Predicate evaluation on typed rows -----

  lemma C1_eval(p: Params, s: Sup, l: Li)
    ensures EvalPredKeep(C1, p, SupRow(s) + LiRow(l)) == SomeBool(s.sk == l.sk)
  {}

  lemma C2_eval(p: Params, x: Q2, o: Ord)
    ensures EvalPredKeep(C2, p, Q2Row(x) + OrdRow(o)) == SomeBool(x.l.ok == o.ok)
  {}

  lemma C3_eval(p: Params, x: Q3, n: Nat)
    ensures EvalPredKeep(C3, p, Q3Row(x) + NatRow(n)) == SomeBool(x.s.nk == n.nk)
  {}

  lemma W_eval(nn: string, x: Q4)
    ensures EvalPredKeep(W, Q21Params(nn), Q4Row(x)) == SomeBool(Where(nn, x))
  {
    var r := Q4Row(x);
    assert r[8] == VText([x.o.st]);
    assert ([x.o.st] == "F") == (x.o.st == 'F');
  }

  lemma SC_eval(p: Params, x: Q4, l2: Li)
    ensures EvalPredKeep(SC, p, Q4Row(x) + LiRow(l2)) == SomeBool(OtherSupp(x, l2))
  {}

  lemma LateP_eval(p: Params, l: Li)
    ensures EvalPredKeep(LateP, p, LiRow(l)) == SomeBool(Late(l))
  {}

  // ----- Operator-level characterizations -----

  lemma J1a_rel(p: Params, s: Sup, L: seq<Li>)
    ensures JoinAgainst(C1, p, SupRow(s), LiRows(L)) == SomeRows(Q2Rows(J1a(s, L)))
    decreases |L|
  {
    if |L| > 0 {
      J1a_rel(p, s, L[1..]);
      C1_eval(p, s, L[0]);
      assert LiRows(L)[1..] == LiRows(L[1..]);
      Q2Rows_app(if s.sk == L[0].sk then [Q2(s, L[0])] else [], J1a(s, L[1..]));
    }
  }

  lemma J1_rel(p: Params, S: seq<Sup>, L: seq<Li>)
    ensures JoinInnerRows(C1, p, SupRows(S), LiRows(L)) == SomeRows(Q2Rows(J1(S, L)))
    decreases |S|
  {
    if |S| > 0 {
      J1_rel(p, S[1..], L);
      J1a_rel(p, S[0], L);
      assert SupRows(S)[1..] == SupRows(S[1..]);
      Q2Rows_app(J1a(S[0], L), J1(S[1..], L));
    }
  }

  lemma J2a_rel(p: Params, x: Q2, O: seq<Ord>)
    ensures JoinAgainst(C2, p, Q2Row(x), OrdRows(O)) == SomeRows(Q3Rows(J2a(x, O)))
    decreases |O|
  {
    if |O| > 0 {
      J2a_rel(p, x, O[1..]);
      C2_eval(p, x, O[0]);
      assert OrdRows(O)[1..] == OrdRows(O[1..]);
      Q3Rows_app(if x.l.ok == O[0].ok then [Q3(x.s, x.l, O[0])] else [], J2a(x, O[1..]));
    }
  }

  lemma J2_rel(p: Params, X: seq<Q2>, O: seq<Ord>)
    ensures JoinInnerRows(C2, p, Q2Rows(X), OrdRows(O)) == SomeRows(Q3Rows(J2(X, O)))
    decreases |X|
  {
    if |X| > 0 {
      J2_rel(p, X[1..], O);
      J2a_rel(p, X[0], O);
      assert Q2Rows(X)[1..] == Q2Rows(X[1..]);
      Q3Rows_app(J2a(X[0], O), J2(X[1..], O));
    }
  }

  lemma J3a_rel(p: Params, x: Q3, N: seq<Nat>)
    ensures JoinAgainst(C3, p, Q3Row(x), NatRows(N)) == SomeRows(Q4Rows(J3a(x, N)))
    decreases |N|
  {
    if |N| > 0 {
      J3a_rel(p, x, N[1..]);
      C3_eval(p, x, N[0]);
      assert NatRows(N)[1..] == NatRows(N[1..]);
      Q4Rows_app(if x.s.nk == N[0].nk then [Q4(x.s, x.l, x.o, N[0])] else [], J3a(x, N[1..]));
    }
  }

  lemma J3_rel(p: Params, X: seq<Q3>, N: seq<Nat>)
    ensures JoinInnerRows(C3, p, Q3Rows(X), NatRows(N)) == SomeRows(Q4Rows(J3(X, N)))
    decreases |X|
  {
    if |X| > 0 {
      J3_rel(p, X[1..], N);
      J3a_rel(p, X[0], N);
      assert Q3Rows(X)[1..] == Q3Rows(X[1..]);
      Q4Rows_app(J3a(X[0], N), J3(X[1..], N));
    }
  }

  lemma Where_rel(nn: string, X: seq<Q4>)
    ensures FilterRows(W, Q21Params(nn), Q4Rows(X)) == SomeRows(Q4Rows(FiltWhere(nn, X)))
    decreases |X|
  {
    if |X| > 0 {
      Where_rel(nn, X[1..]);
      W_eval(nn, X[0]);
      assert Q4Rows(X)[1..] == Q4Rows(X[1..]);
      Q4Rows_app(if Where(nn, X[0]) then [X[0]] else [], FiltWhere(nn, X[1..]));
    }
  }

  lemma Exists_rel(p: Params, x: Q4, L: seq<Li>)
    ensures JoinExistsRight(SC, p, Q4Row(x), LiRows(L)) == SomeBool(ExistsOther(x, L))
    decreases |L|
  {
    if |L| > 0 {
      Exists_rel(p, x, L[1..]);
      SC_eval(p, x, L[0]);
      assert LiRows(L)[1..] == LiRows(L[1..]);
    }
  }

  lemma Semi_rel(p: Params, X: seq<Q4>, L: seq<Li>)
    ensures JoinSemiRows(SC, p, Q4Rows(X), LiRows(L)) == SomeRows(Q4Rows(Semi(X, L)))
    decreases |X|
  {
    if |X| > 0 {
      Semi_rel(p, X[1..], L);
      Exists_rel(p, X[0], L);
      assert Q4Rows(X)[1..] == Q4Rows(X[1..]);
      Q4Rows_app(if ExistsOther(X[0], L) then [X[0]] else [], Semi(X[1..], L));
    }
  }

  lemma Anti_rel(p: Params, X: seq<Q4>, L3: seq<Li>)
    ensures JoinAntiRows(SC, p, Q4Rows(X), LiRows(L3)) == SomeRows(Q4Rows(Anti(X, L3)))
    decreases |X|
  {
    if |X| > 0 {
      Anti_rel(p, X[1..], L3);
      Exists_rel(p, X[0], L3);
      assert Q4Rows(X)[1..] == Q4Rows(X[1..]);
      Q4Rows_app(if ExistsOther(X[0], L3) then [] else [X[0]], Anti(X[1..], L3));
    }
  }

  lemma LateLis_rel(p: Params, L: seq<Li>)
    ensures FilterRows(LateP, p, LiRows(L)) == SomeRows(LiRows(LateLis(L)))
    decreases |L|
  {
    if |L| > 0 {
      LateLis_rel(p, L[1..]);
      LateP_eval(p, L[0]);
      assert LiRows(L)[1..] == LiRows(L[1..]);
      LiRows_app(if Late(L[0]) then [L[0]] else [], LateLis(L[1..]));
    }
  }

  lemma LiRows_app(a: seq<Li>, b: seq<Li>)
    ensures LiRows(a + b) == LiRows(a) + LiRows(b)
  {
    if |a| > 0 { assert (a + b)[1..] == a[1..] + b; LiRows_app(a[1..], b); }
    else { assert a + b == b; }
  }

  // ----- The whole spine -----

  lemma Spine_rel(T: TDB, nn: string)
    ensures EvalRel(NA, Q21Params(nn), ToRel(T)) == SomeRel(RVRows(Q4Rows(Spine(T, nn))))
  {
    var p := Q21Params(nn);
    var db := ToRel(T);
    Scan_lookup(T);
    J1_rel(p, T.sup, T.li);
    var X1 := J1(T.sup, T.li);
    assert EvalRel(N1, p, db) == SomeRel(RVRows(Q2Rows(X1)));
    J2_rel(p, X1, T.ords);
    var X2 := J2(X1, T.ords);
    assert EvalRel(N2, p, db) == SomeRel(RVRows(Q3Rows(X2)));
    J3_rel(p, X2, T.nats);
    var X3 := J3(X2, T.nats);
    assert EvalRel(N3, p, db) == SomeRel(RVRows(Q4Rows(X3)));
    Where_rel(nn, X3);
    var X4 := FiltWhere(nn, X3);
    assert EvalRel(NF, p, db) == SomeRel(RVRows(Q4Rows(X4)));
    Semi_rel(p, X4, T.li);
    var X5 := Semi(X4, T.li);
    assert EvalRel(NS, p, db) == SomeRel(RVRows(Q4Rows(X5)));
    LateLis_rel(p, T.li);
    assert EvalRel(RelFilter(RelScan("lineitem"), LateP), p, db)
        == SomeRel(RVRows(LiRows(LateLis(T.li))));
    Anti_rel(p, X5, LateLis(T.li));
  }
}
