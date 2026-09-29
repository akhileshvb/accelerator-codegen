// Q21 semantic core, part 2: GROUP BY s_name + COUNT-star, final Project and
// ORDER BY (numwait DESC NULLS FIRST, s_name ASC NULLS LAST) evaluated by the
// generic Dafny RelNode interpreter.  Main result:
//
//   Q21_EvalSpec_Answer :
//     Eval_Spec(LogicalSpec_Q21_rel, Q21Params(nn), ToRel(T))
//       == SomeRows(PairRows(AnswerPairs(T, nn)))
//
// where AnswerPairs = insertion-sort (PairLe) of the first-occurrence
// (s_name, count) fold over the spine rows.  No premises: the characterization
// holds for every typed database.

include "Q21SqlCore.dfy"

module Q21SqlAgg {
  import opened LogicalSpec
  import opened RelEvalLogicalSpec
  import opened Q21Types
  import opened Q21SqlCore
  import Q21RelNodeData

  // ----- (name, count) model of the interpreter's GroupStore -----

  datatype NC = NC(name: string, c: int)

  function Names(X: seq<Q4>): seq<string>
  { if |X| == 0 then [] else [X[0].s.name] + Names(X[1..]) }

  function ModelCnt(m: seq<NC>, nm: string): int
  { if |m| == 0 then 0 else if m[0].name == nm then m[0].c else ModelCnt(m[1..], nm) }

  function ModelAdd(m: seq<NC>, nm: string): seq<NC>
  {
    if |m| == 0 then [NC(nm, 1)]
    else if m[0].name == nm then [NC(nm, m[0].c + 1)] + m[1..]
    else [m[0]] + ModelAdd(m[1..], nm)
  }

  function ModelFold(m: seq<NC>, xs: seq<string>): seq<NC>
    decreases |xs|
  { if |xs| == 0 then m else ModelFold(ModelAdd(m, xs[0]), xs[1..]) }

  function Entry(x: NC): GroupEntry { GroupEntry([VText(x.name)], [AccSlot(x.c, x.c, true)]) }

  function StoreOf(m: seq<NC>): GroupStore
  { if |m| == 0 then [] else [Entry(m[0])] + StoreOf(m[1..]) }

  function PairRow(x: NC): Row { [VText(x.name), VInt(x.c)] }
  function PairRows(m: seq<NC>): seq<Row>
  { if |m| == 0 then [] else [PairRow(m[0])] + PairRows(m[1..]) }

  function BagsOf(m: seq<NC>): seq<AggBag>
  { if |m| == 0 then [] else [AggBag([VText(m[0].name)], [VInt(m[0].c)])] + BagsOf(m[1..]) }

  lemma ListEq_text(a: string, b: string)
    ensures ListEq([VText(a)], [VText(b)]) == (a == b)
  {
    assert [VText(a)][1..] == [];
    assert [VText(b)][1..] == [];
  }

  lemma StoreGet_model(m: seq<NC>, nm: string)
    ensures StoreGet(StoreOf(m), [VText(nm)]) ==
            (if exists i :: 0 <= i < |m| && m[i].name == nm
             then SomeAcc([AccSlot(ModelCnt(m, nm), ModelCnt(m, nm), true)]) else NoAcc)
    decreases |m|
  {
    if |m| > 0 {
      ListEq_text(m[0].name, nm);
      assert StoreOf(m)[1..] == StoreOf(m[1..]);
      StoreGet_model(m[1..], nm);
      if m[0].name != nm {
        assert (exists i :: 0 <= i < |m| && m[i].name == nm) ==
               (exists i :: 0 <= i < |m[1..]| && m[1..][i].name == nm) by {
          if exists i :: 0 <= i < |m| && m[i].name == nm {
            var i :| 0 <= i < |m| && m[i].name == nm;
            assert i > 0 && m[1..][i - 1].name == nm;
          }
        }
      }
    }
  }

  lemma StorePut_model(m: seq<NC>, nm: string)
    ensures StorePut(StoreOf(m), [VText(nm)],
                     [AccSlot(ModelCnt(m, nm) + 1, ModelCnt(m, nm) + 1, true)])
            == StoreOf(ModelAdd(m, nm))
    decreases |m|
  {
    if |m| > 0 {
      ListEq_text(m[0].name, nm);
      assert StoreOf(m)[1..] == StoreOf(m[1..]);
      if m[0].name != nm { StorePut_model(m[1..], nm); }
    }
  }

  lemma ModelCnt_absent(m: seq<NC>, nm: string)
    requires !(exists i :: 0 <= i < |m| && m[i].name == nm)
    ensures ModelCnt(m, nm) == 0
    decreases |m|
  {
    if |m| > 0 {
      assert m[0].name != nm;
      ModelCnt_absent(m[1..], nm);
    }
  }

  lemma StoreAddRow_model(p: Params, m: seq<NC>, x: Q4)
    ensures StoreAddRow(Ag, Gk, p, Q4Row(x), StoreOf(m)) == SomeStore(StoreOf(ModelAdd(m, x.s.name)))
  {
    var nm := x.s.name;
    var r := Q4Row(x);
    assert r[1] == VText(nm);
    assert Gk[1..] == [];
    assert EvalExprs(Gk[1..], p, EvRow(r)) == SomeVals([]);
    assert EvalExprs(Gk, p, EvRow(r)) == SomeVals([VText(nm)]);
    StoreGet_model(m, nm);
    var k := ModelCnt(m, nm);
    var found := StoreGet(StoreOf(m), [VText(nm)]);
    var acc0 := if found.SomeAcc? then found.acc else EmptyAcc(|Ag|);
    if !(exists i :: 0 <= i < |m| && m[i].name == nm) {
      ModelCnt_absent(m, nm);
      assert EmptyAcc(0) == [];
      assert EmptyAcc(|Ag|) == [AccSlot(0, 0, false)];
    }
    assert acc0 == [AccSlot(k, k, k != 0 || (exists i :: 0 <= i < |m| && m[i].name == nm))];
    assert Ag[1..] == [];
    assert AggsContrib(Ag[1..], p, r) == SomeVals([]);
    assert AggsContrib(Ag, p, r) == SomeVals([VInt(1)]);
    assert AccAddOne(Ag[1..], acc0[1..], [VInt(1)][1..]) == SomeAcc([]);
    assert AccAddRow(Ag, p, r, acc0) == SomeAcc([AccSlot(k + 1, k + 1, true)]);
    StorePut_model(m, nm);
  }

  lemma FoldGroups_model(p: Params, X: seq<Q4>, m: seq<NC>)
    ensures FoldGroups(Ag, Gk, p, Q4Rows(X), StoreOf(m)) == SomeStore(StoreOf(ModelFold(m, Names(X))))
    decreases |X|
  {
    if |X| > 0 {
      StoreAddRow_model(p, m, X[0]);
      assert Q4Rows(X)[1..] == Q4Rows(X[1..]);
      FoldGroups_model(p, X[1..], ModelAdd(m, X[0].s.name));
    }
  }

  lemma StoreToAggBags_model(m: seq<NC>)
    ensures StoreToAggBags(Ag, StoreOf(m)) == SomeBags(BagsOf(m))
    decreases |m|
  {
    if |m| > 0 {
      var e := StoreOf(m)[0];
      assert e.a == [AccSlot(m[0].c, m[0].c, true)];
      assert Ag[1..] == [] && e.a[1..] == [];
      assert FinalizeAcc(Ag[1..], e.a[1..]) == SomeVals([]);
      assert FinalizeAcc(Ag, e.a) == SomeVals([VInt(m[0].c)]);
      assert StoreOf(m)[0].a[0].init;
      assert AccNonempty(StoreOf(m)[0].a);
      assert StoreOf(m)[1..] == StoreOf(m[1..]);
      StoreToAggBags_model(m[1..]);
    }
  }

  lemma ProjectAggRows_model(p: Params, m: seq<NC>)
    ensures ProjectAggRows(PE, p, BagsOf(m)) == SomeRows(PairRows(m))
    decreases |m|
  {
    if |m| > 0 {
      var b := BagsOf(m)[0];
      assert PE[1..][1..] == [];
      assert EvalExprs(PE[1..][1..], p, EvAgg(b.keys, b.vals)) == SomeVals([]);
      assert EvalExprs(PE[1..], p, EvAgg(b.keys, b.vals)) == SomeVals([VInt(m[0].c)]);
      assert EvalExprs(PE, p, EvAgg(b.keys, b.vals)) == SomeVals([VText(m[0].name), VInt(m[0].c)]);
      assert BagsOf(m)[1..] == BagsOf(m[1..]);
      ProjectAggRows_model(p, m[1..]);
    }
  }

  // ----- ORDER BY: numwait DESC, s_name ASC (byte order TextCLeb) -----

  lemma TextCLeb_total(a: string, b: string)
    ensures TextCLeb(a, b) || TextCLeb(b, a)
    decreases |a| + |b|
  {
    if |a| > 0 && |b| > 0 && a[0] == b[0] { TextCLeb_total(a[1..], b[1..]); }
  }

  ghost predicate PairLe(a: NC, b: NC)
  { a.c > b.c || (a.c == b.c && TextCLeb(a.name, b.name)) }

  lemma OKs_eval(p: Params, a: NC, b: NC)
    ensures OrderKeysLe(p, OKs, PairRow(a), PairRow(b)) == SomeBool(PairLe(a, b))
  {
    TextCLeb_total(a.name, b.name);
    assert OKs[1..][1..] == [];
    assert OKs[1..][0] == OrderKey(EInputRef(0), SortAsc, NullsLast);
  }

  ghost function InsM(x: NC, xs: seq<NC>): seq<NC>
  { if |xs| == 0 then [x] else if PairLe(x, xs[0]) then [x] + xs else [xs[0]] + InsM(x, xs[1..]) }

  ghost function SortM(xs: seq<NC>): seq<NC>
  { if |xs| == 0 then [] else InsM(xs[0], SortM(xs[1..])) }

  lemma RowInsert_model(p: Params, x: NC, xs: seq<NC>)
    ensures RowInsert(p, OKs, PairRow(x), PairRows(xs)) == SomeRows(PairRows(InsM(x, xs)))
    decreases |xs|
  {
    if |xs| > 0 {
      OKs_eval(p, x, xs[0]);
      assert PairRows(xs)[1..] == PairRows(xs[1..]);
      if !PairLe(x, xs[0]) { RowInsert_model(p, x, xs[1..]); }
    }
  }

  lemma ResultRowSort_model(p: Params, xs: seq<NC>)
    ensures ResultRowSort(p, OKs, PairRows(xs)) == SomeRows(PairRows(SortM(xs)))
    decreases |xs|
  {
    if |xs| > 0 {
      assert PairRows(xs)[1..] == PairRows(xs[1..]);
      ResultRowSort_model(p, xs[1..]);
      RowInsert_model(p, xs[0], SortM(xs[1..]));
    }
  }

  // ----- The SQL answer -----

  ghost function GroupPairs(T: TDB, nn: string): seq<NC>
  { ModelFold([], Names(Spine(T, nn))) }

  ghost function AnswerPairs(T: TDB, nn: string): seq<NC>
  { SortM(GroupPairs(T, nn)) }

  lemma Q21_EvalSpec_Answer(T: TDB, nn: string)
    ensures Eval_Spec(Q21RelNodeData.LogicalSpec_Q21_rel, Q21Params(nn), ToRel(T))
            == SomeRows(PairRows(AnswerPairs(T, nn)))
  {
    var p := Q21Params(nn);
    var db := ToRel(T);
    Q21_spec_shape();
    Spine_rel(T, nn);
    var X := Spine(T, nn);
    FoldGroups_model(p, X, []);
    assert StoreOf([]) == [];
    var M := ModelFold([], Names(X));
    StoreToAggBags_model(M);
    assert RunAggregate(Ag, Gk, p, Q4Rows(X)) == SomeBags(BagsOf(M));
    assert EvalRel(NG, p, db) == SomeRel(RVAgg(BagsOf(M)));
    ProjectAggRows_model(p, M);
    assert EvalRel(NP, p, db) == SomeRel(RVRows(PairRows(M)));
    ResultRowSort_model(p, M);
    assert EvalRel(NTop, p, db) == SomeRel(RVRows(PairRows(SortM(M))));
    reveal Eval_Spec();
  }
}
