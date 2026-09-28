// EvalLogicalSpec — Dafny Eval_Spec(spec, params, db) over FLAT expression-IR DATA.
// Migration scaffolding only. Public Dafny Q1 Exec / runtime Hit theorems target
// RelNode Eval_Spec_Q1_OutRows_rel (Q1RelNodeSpec.dfy); observational equality is
// Qed in Q1RelNodeBridge.dfy. Rocq public Eval is RelNode
// (formalsql/q1/Q1Adapt.Eval_Spec_Q1_rows_rel). No Q1-name fast path.
// Migration lemmas vs legacy EvalResultSpec remain as scaffolding only.

include "generated/LogicalSpecData.dfy"
include "generated/Q1ResultSpec.dfy"

module EvalLogicalSpec {

  import opened LogicalSpecData
  import R = Q1ResultSpec

  ghost function ColumnValue(c: ColumnId, r: R.Row): int {
    match c
    case ColShipdate => r.shipdate
    case ColQty => r.qty
    case ColPrice => r.price
    case ColDisc => r.disc
    case ColTax => r.tax
    case ColRf => r.rf
    case ColLs => r.ls
  }

  ghost function ParamLookup(p: Params, id: ParamId): int {
    match id
    case ParamCutoff => p.cutoff
  }

  ghost function EvalExpr(e: Expr, p: Params, r: R.Row): int {
    match e
    case EColumn(c) => ColumnValue(c, r)
    case EConst(z, _) => z
    case EParam(id, _) => ParamLookup(p, id)
    case EAdd(a, b) => EvalExpr(a, p, r) + EvalExpr(b, p, r)
    case ESub(a, b) => EvalExpr(a, p, r) - EvalExpr(b, p, r)
    case EMul(a, b) => EvalExpr(a, p, r) * EvalExpr(b, p, r)
  }

  ghost predicate EvalPred(pr: Pred, p: Params, r: R.Row) {
    match pr
    case PLe(a, b) => EvalExpr(a, p, r) <= EvalExpr(b, p, r)
  }

  ghost predicate EvalPreds(ps: seq<Pred>, p: Params, r: R.Row) {
    forall i :: 0 <= i < |ps| ==> EvalPred(ps[i], p, r)
  }

  ghost function FilterRows(ps: seq<Pred>, p: Params, db: seq<R.Row>): seq<R.Row>
    decreases |db|
  {
    if |db| == 0 then
      []
    else if EvalPreds(ps, p, db[|db| - 1]) then
      FilterRows(ps, p, db[..|db| - 1]) + [db[|db| - 1]]
    else
      FilterRows(ps, p, db[..|db| - 1])
  }

  ghost function EvalExprs(es: seq<Expr>, p: Params, r: R.Row): seq<int>
    decreases |es|
  {
    if |es| == 0 then []
    else [EvalExpr(es[0], p, r)] + EvalExprs(es[1..], p, r)
  }

  // Per-agg accumulator: (sum, count)
  type AggAcc = seq<(int, int)>

  ghost function EmptyAcc(n: nat): AggAcc
    ensures |EmptyAcc(n)| == n
    decreases n
  {
    if n == 0 then [] else [(0, 0)] + EmptyAcc(n - 1)
  }

  ghost function AggContrib(a: Agg, p: Params, r: R.Row): int {
    match a
    case AggSum(e) => EvalExpr(e, p, r)
    case AggAvg(e) => EvalExpr(e, p, r)
    case AggCountStar => 1
  }

  ghost function AggsContrib(aggs: seq<Agg>, p: Params, r: R.Row): seq<int>
    ensures |AggsContrib(aggs, p, r)| == |aggs|
    decreases |aggs|
  {
    if |aggs| == 0 then []
    else [AggContrib(aggs[0], p, r)] + AggsContrib(aggs[1..], p, r)
  }

  ghost function AccAddOne(acc: AggAcc, vals: seq<int>): AggAcc
    requires |acc| == |vals|
    ensures |AccAddOne(acc, vals)| == |acc|
    decreases |acc|
  {
    if |acc| == 0 then
      []
    else
      [(acc[0].0 + vals[0], acc[0].1 + 1)] + AccAddOne(acc[1..], vals[1..])
  }

  ghost function AccAddRow(aggs: seq<Agg>, p: Params, r: R.Row, acc: AggAcc): AggAcc
    requires |acc| == |aggs|
    ensures |AccAddRow(aggs, p, r, acc)| == |aggs|
  {
    AccAddOne(acc, AggsContrib(aggs, p, r))
  }

  // Group store: semantic tuple keys (seq<int>), not Pack.
  type GroupStore = map<seq<int>, AggAcc>

  ghost function StoreAddRow(aggs: seq<Agg>, gkeys: seq<Expr>, p: Params,
                             r: R.Row, st: GroupStore): GroupStore
    requires forall k :: k in st ==> |st[k]| == |aggs|
    ensures forall k :: k in StoreAddRow(aggs, gkeys, p, r, st) ==>
              |StoreAddRow(aggs, gkeys, p, r, st)[k]| == |aggs|
  {
    var k := EvalExprs(gkeys, p, r);
    var acc0 := if k in st then st[k] else EmptyAcc(|aggs|);
    st[k := AccAddRow(aggs, p, r, acc0)]
  }

  // Same recursion shape as R.FoldByGroup: fold prefix, then add last row.
  ghost function FoldGroups(aggs: seq<Agg>, gkeys: seq<Expr>, p: Params,
                            rs: seq<R.Row>, st: GroupStore): GroupStore
    requires forall k :: k in st ==> |st[k]| == |aggs|
    ensures forall k :: k in FoldGroups(aggs, gkeys, p, rs, st) ==>
              |FoldGroups(aggs, gkeys, p, rs, st)[k]| == |aggs|
    decreases |rs|
  {
    if |rs| == 0 then
      st
    else
      StoreAddRow(aggs, gkeys, p, rs[|rs| - 1],
                 FoldGroups(aggs, gkeys, p, rs[..|rs| - 1], st))
  }

  datatype SpecResultRow = SpecResultRow(key: seq<int>, acc: AggAcc)

  ghost predicate KeyLexLe(a: seq<int>, b: seq<int>)
    decreases |a| + |b|
  {
    if |a| == 0 then
      true
    else if |b| == 0 then
      false
    else if a[0] < b[0] then
      true
    else if a[0] > b[0] then
      false
    else
      KeyLexLe(a[1..], b[1..])
  }

  lemma KeyLexLe_refl(a: seq<int>)
    ensures KeyLexLe(a, a)
    decreases |a|
  {
    if |a| == 0 {
    } else {
      KeyLexLe_refl(a[1..]);
    }
  }

  lemma KeyLexLe_trans(a: seq<int>, b: seq<int>, c: seq<int>)
    requires KeyLexLe(a, b) && KeyLexLe(b, c)
    ensures KeyLexLe(a, c)
    decreases |a| + |b| + |c|
  {
    if |a| == 0 {
    } else if |b| == 0 {
    } else if |c| == 0 {
    } else if a[0] < b[0] {
      if b[0] < c[0] {
      } else if b[0] == c[0] {
        assert a[0] < c[0];
      }
    } else if a[0] == b[0] {
      if b[0] < c[0] {
        assert a[0] < c[0];
      } else if b[0] == c[0] {
        KeyLexLe_trans(a[1..], b[1..], c[1..]);
      }
    }
  }

  lemma KeyLexLe_total(a: seq<int>, b: seq<int>)
    ensures KeyLexLe(a, b) || KeyLexLe(b, a)
    decreases |a| + |b|
  {
    if |a| == 0 {
    } else if |b| == 0 {
    } else if a[0] < b[0] {
    } else if a[0] > b[0] {
    } else {
      KeyLexLe_total(a[1..], b[1..]);
    }
  }

  lemma KeyLexLe_antisym(a: seq<int>, b: seq<int>)
    requires KeyLexLe(a, b) && KeyLexLe(b, a)
    ensures a == b
    decreases |a| + |b|
  {
    if |a| == 0 {
      assert |b| == 0;
    } else if |b| == 0 {
      assert false;
    } else {
      assert a[0] == b[0];
      KeyLexLe_antisym(a[1..], b[1..]);
    }
  }

  // Unfold KeyLexLe for Q1's length-2 keys.
  lemma KeyLexLe2_unfold(a: seq<int>, b: seq<int>)
    requires |a| == 2 && |b| == 2
    requires KeyLexLe(a, b)
    ensures a[0] < b[0] || (a[0] == b[0] && a[1] <= b[1])
  {
  }

  lemma KeyLexLe2_strict(a: seq<int>, b: seq<int>)
    requires |a| == 2 && |b| == 2
    requires KeyLexLe(a, b)
    requires a != b
    ensures a[0] < b[0] || (a[0] == b[0] && a[1] < b[1])
  {
    KeyLexLe2_unfold(a, b);
    if a[0] == b[0] {
      assert a[1] != b[1];
      assert a[1] < b[1];
    }
  }

  ghost predicate IsLexMinKey(keys: set<seq<int>>, m: seq<int>) {
    m in keys && (forall k :: k in keys ==> KeyLexLe(m, k))
  }

  lemma LexMin_exists(keys: set<seq<int>>)
    requires keys != {}
    ensures exists m :: IsLexMinKey(keys, m)
    decreases |keys|
  {
    var k :| k in keys;
    if keys == {k} {
      KeyLexLe_refl(k);
      assert IsLexMinKey(keys, k);
    } else {
      LexMin_exists(keys - {k});
      var m :| IsLexMinKey(keys - {k}, m);
      if KeyLexLe(k, m) {
        forall kk | kk in keys
          ensures KeyLexLe(k, kk)
        {
          if kk == k {
            KeyLexLe_refl(k);
          } else {
            assert KeyLexLe(m, kk);
            KeyLexLe_trans(k, m, kk);
          }
        }
        assert IsLexMinKey(keys, k);
      } else {
        KeyLexLe_total(k, m);
        assert KeyLexLe(m, k);
        forall kk | kk in keys
          ensures KeyLexLe(m, kk)
        {
          if kk == k {
          } else {
          }
        }
        assert IsLexMinKey(keys, m);
      }
    }
  }

  // Deterministic lex-min via choose-from-IsLexMinKey (no peel Hilbert mismatch).
  ghost function MinKeyOf(keys: set<seq<int>>): seq<int>
    requires keys != {}
    ensures IsLexMinKey(keys, MinKeyOf(keys))
    ensures MinKeyOf(keys) in keys
    ensures forall kk :: kk in keys ==> KeyLexLe(MinKeyOf(keys), kk)
  {
    assert exists m :: IsLexMinKey(keys, m) by {
      LexMin_exists(keys);
    }
    var m :| IsLexMinKey(keys, m);
    m
  }

  lemma MinKeyOf_is_min(keys: set<seq<int>>)
    requires keys != {}
    ensures MinKeyOf(keys) in keys
    ensures forall kk :: kk in keys ==> KeyLexLe(MinKeyOf(keys), kk)
  {
  }

  // Deterministic: emit lex-minimum key first (MinKeyOf is unique lex min).
  ghost function StoreToRows(st: GroupStore): seq<SpecResultRow>
    decreases |st.Keys|
  {
    if st.Keys == {} then
      []
    else
      var k := MinKeyOf(st.Keys);
      [SpecResultRow(k, st[k])] + StoreToRows(st - {k})
  }

  ghost function FilterNonempty(rows: seq<SpecResultRow>): seq<SpecResultRow>
    decreases |rows|
  {
    if |rows| == 0 then
      []
    else if exists i :: 0 <= i < |rows[0].acc| && rows[0].acc[i].1 > 0 then
      [rows[0]] + FilterNonempty(rows[1..])
    else
      FilterNonempty(rows[1..])
  }

  ghost predicate ExprEq(a: Expr, b: Expr) {
    a == b
  }

  ghost function LookupKeyExpr(e: Expr, gkeys: seq<Expr>, kvals: seq<int>): int
    requires |gkeys| == |kvals|
    decreases |gkeys|
  {
    if |gkeys| == 0 then
      0
    else if ExprEq(e, gkeys[0]) then
      kvals[0]
    else
      LookupKeyExpr(e, gkeys[1..], kvals[1..])
  }

  // Lexicographic compare using ls_order_keys (Asc only for Q1).
  ghost predicate ResultRowLe(oks: seq<OrderKey>, gkeys: seq<Expr>,
                              a: SpecResultRow, b: SpecResultRow)
    requires |a.key| == |gkeys| && |b.key| == |gkeys|
    decreases |oks|
  {
    if |oks| == 0 then
      true
    else if |gkeys| == 0 then
      true
    else
      var va := LookupKeyExpr(oks[0].expr, gkeys, a.key);
      var vb := LookupKeyExpr(oks[0].expr, gkeys, b.key);
      if va == vb then
        ResultRowLe(oks[1..], gkeys, a, b)
      else if oks[0].dir.SortAsc? then
        va <= vb
      else
        vb <= va
  }

  ghost function ResultRowInsert(oks: seq<OrderKey>, gkeys: seq<Expr>,
                                 x: SpecResultRow, l: seq<SpecResultRow>): seq<SpecResultRow>
    requires |x.key| == |gkeys|
    requires forall row :: row in l ==> |row.key| == |gkeys|
    ensures forall row :: row in ResultRowInsert(oks, gkeys, x, l) ==>
              |row.key| == |gkeys|
    ensures |ResultRowInsert(oks, gkeys, x, l)| == |l| + 1
    decreases |l|
  {
    if |l| == 0 then
      [x]
    else if ResultRowLe(oks, gkeys, x, l[0]) then
      [x] + l
    else
      [l[0]] + ResultRowInsert(oks, gkeys, x, l[1..])
  }

  ghost function ResultRowSort(oks: seq<OrderKey>, gkeys: seq<Expr>,
                               l: seq<SpecResultRow>): seq<SpecResultRow>
    requires forall row :: row in l ==> |row.key| == |gkeys|
    ensures forall row :: row in ResultRowSort(oks, gkeys, l) ==>
              |row.key| == |gkeys|
    ensures |ResultRowSort(oks, gkeys, l)| == |l|
    decreases |l|
  {
    if |l| == 0 then
      []
    else
      ResultRowInsert(oks, gkeys, l[0], ResultRowSort(oks, gkeys, l[1..]))
  }

  // Pure generic IR interpreter — NO IsLogicalSpec_Q1 branch.
  // Opaque so migration lemmas compose via thin reveal lemmas (no giant VCs).
  ghost function {:opaque} Eval_Spec(s: LogicalSpec, p: Params, db: seq<R.Row>): seq<SpecResultRow>
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
  {
    var filtered := FilterRows(s.filters, p, db);
    var st := FoldGroups(s.aggs, s.group_keys, p, filtered, map[]);
    var rows := FilterNonempty(StoreToRows(st));
    // Q1 group keys are length 2; for general specs require key arity match.
    if forall row :: row in rows ==> |row.key| == |s.group_keys| then
      ResultRowSort(s.order_keys, s.group_keys, rows)
    else
      []
  }

  // Q1-only adapter: semantic [rf;ls] + acc → SpecOutRow with Pack key.
  // Total (no forall requires) so migration lemmas avoid giant WF VCs.
  ghost function Q1AdaptRow(row: SpecResultRow): seq<R.SpecOutRow>
  {
    if |row.key| == 2 && |row.acc| == 8 &&
       0 <= row.key[0] < 256 && 0 <= row.key[1] < 256 then
      var rf := row.key[0];
      var ls := row.key[1];
      var sum_qty := row.acc[0].0;
      var sum_base := row.acc[1].0;
      var sum_disc_price := row.acc[2].0;
      var sum_charge := row.acc[3].0;
      var sum_discount := row.acc[6].0;
      var c := row.acc[7].1;
      if c <= 0 then
        []
      else
        [R.SpecOutRow(R.Pack(rf, ls), sum_qty, sum_base, sum_disc_price,
                      sum_charge, sum_discount, c)]
    else
      []
  }

  ghost function Q1Adapt(rows: seq<SpecResultRow>): seq<R.SpecOutRow>
    decreases |rows|
  {
    if |rows| == 0 then
      []
    else
      Q1AdaptRow(rows[0]) + Q1Adapt(rows[1..])
  }

  // Adapter view of Eval_Spec for Q1. Q1AdaptRow is total (skips ill-formed rows);
  // under StoreRel every IR row is well-formed, so this is the Q1Adapt of Eval_Spec.
  ghost function {:opaque} Eval_Spec_Q1_OutRows(p: Params, db: seq<R.Row>): seq<R.SpecOutRow>
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
  {
    Q1Adapt(Eval_Spec(LogicalSpec_Q1, p, db))
  }

  // ----- Q1 migration: filter / fold / order correspondence -----

  ghost function Q1Filters(): seq<Pred> { LogicalSpec_Q1.filters }
  ghost function Q1GroupKeys(): seq<Expr> { LogicalSpec_Q1.group_keys }
  ghost function Q1Aggs(): seq<Agg> { LogicalSpec_Q1.aggs }
  ghost function Q1OrderKeys(): seq<OrderKey> { LogicalSpec_Q1.order_keys }

  ghost function AccOfStats(s: R.SpecStats): AggAcc {
    [
      (s.sum_qty, s.count),
      (s.sum_base_price, s.count),
      (s.sum_disc_price, s.count),
      (s.sum_charge, s.count),
      (s.sum_qty, s.count),
      (s.sum_base_price, s.count),
      (s.sum_discount, s.count),
      (s.count, s.count)
    ]
  }

  ghost predicate AccWellFormed(acc: AggAcc) {
    |acc| == 8 &&
    acc[0].1 == acc[1].1 && acc[1].1 == acc[2].1 && acc[2].1 == acc[3].1 &&
    acc[3].1 == acc[4].1 && acc[4].1 == acc[5].1 && acc[5].1 == acc[6].1 &&
    acc[6].1 == acc[7].1 &&
    acc[0].0 == acc[4].0 && acc[1].0 == acc[5].0 &&
    acc[7].0 == acc[7].1
  }

  lemma AccOfStats_Empty()
    ensures AccOfStats(R.EmptyStats()) == EmptyAcc(8)
    ensures AccWellFormed(AccOfStats(R.EmptyStats()))
  {
    assert EmptyAcc(8) ==
      [(0, 0), (0, 0), (0, 0), (0, 0), (0, 0), (0, 0), (0, 0), (0, 0)];
  }

  lemma AccOfStats_well(s: R.SpecStats)
    ensures AccWellFormed(AccOfStats(s))
  {
  }

  // (1) FilterRows == FilterCutoff
  lemma FilterRows_eq_FilterCutoff(c: int, db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures FilterRows(Q1Filters(), ParamsOfCutoff(c), db) == R.FilterCutoff(db, c)
    decreases |db|
  {
    if |db| == 0 {
    } else {
      FilterRows_eq_FilterCutoff(c, db[..|db| - 1]);
      EvalPreds_Q1_filter(c, db[|db| - 1]);
    }
  }

  // (2) Acc ↔ SpecStats / FoldGroups ↔ FoldByGroup via Pack
  lemma AggsContrib_Q1(p: Params, r: R.Row)
    ensures AggsContrib(LogicalSpec_Q1.aggs, p, r) ==
            [r.qty, r.price,
             r.price * (100 - r.disc),
             r.price * (100 - r.disc) * (100 + r.tax),
             r.qty, r.price, r.disc, 1]
    ensures AggsContrib(Q1Aggs(), p, r) == AggsContrib(LogicalSpec_Q1.aggs, p, r)
  {
    var aggs := LogicalSpec_Q1.aggs;
    assert |aggs| == 8;

    assert EvalExpr(EColumn(ColPrice), p, r) == r.price;
    assert EvalExpr(EColumn(ColDisc), p, r) == r.disc;
    assert EvalExpr(EColumn(ColTax), p, r) == r.tax;
    assert EvalExpr(EConst(100, Scale2), p, r) == 100;
    assert EvalExpr(ESub(EConst(100, Scale2), EColumn(ColDisc)), p, r) == 100 - r.disc;
    assert EvalExpr(EAdd(EConst(100, Scale2), EColumn(ColTax)), p, r) == 100 + r.tax;

    var e_dp := EMul(EColumn(ColPrice), ESub(EConst(100, Scale2), EColumn(ColDisc)));
    assert EvalExpr(e_dp, p, r)
        == EvalExpr(EColumn(ColPrice), p, r)
         * EvalExpr(ESub(EConst(100, Scale2), EColumn(ColDisc)), p, r);
    assert EvalExpr(e_dp, p, r) == r.price * (100 - r.disc);

    var e_ch := EMul(e_dp, EAdd(EConst(100, Scale2), EColumn(ColTax)));
    assert EvalExpr(e_ch, p, r)
        == EvalExpr(e_dp, p, r)
         * EvalExpr(EAdd(EConst(100, Scale2), EColumn(ColTax)), p, r);
    assert EvalExpr(e_ch, p, r) == r.price * (100 - r.disc) * (100 + r.tax);

    assert aggs[2] == AggSum(e_dp);
    assert aggs[3] == AggSum(e_ch);
    assert AggContrib(aggs[2], p, r) == EvalExpr(e_dp, p, r);
    assert AggContrib(aggs[3], p, r) == EvalExpr(e_ch, p, r);

    assert AggContrib(aggs[0], p, r) == r.qty;
    assert AggContrib(aggs[1], p, r) == r.price;
    assert AggContrib(aggs[2], p, r) == r.price * (100 - r.disc);
    assert AggContrib(aggs[3], p, r) == r.price * (100 - r.disc) * (100 + r.tax);
    assert AggContrib(aggs[4], p, r) == r.qty;
    assert AggContrib(aggs[5], p, r) == r.price;
    assert AggContrib(aggs[6], p, r) == r.disc;
    assert AggContrib(aggs[7], p, r) == 1;
  }

  lemma AccAddOne_head(acc: AggAcc, vals: seq<int>)
    requires |acc| == |vals| && |acc| > 0
    ensures AccAddOne(acc, vals)
         == [(acc[0].0 + vals[0], acc[0].1 + 1)] + AccAddOne(acc[1..], vals[1..])
  {
  }

  lemma AccAddOne_len8(acc: AggAcc, vals: seq<int>)
    requires |acc| == 8 && |vals| == 8
    ensures AccAddOne(acc, vals) == [
      (acc[0].0 + vals[0], acc[0].1 + 1),
      (acc[1].0 + vals[1], acc[1].1 + 1),
      (acc[2].0 + vals[2], acc[2].1 + 1),
      (acc[3].0 + vals[3], acc[3].1 + 1),
      (acc[4].0 + vals[4], acc[4].1 + 1),
      (acc[5].0 + vals[5], acc[5].1 + 1),
      (acc[6].0 + vals[6], acc[6].1 + 1),
      (acc[7].0 + vals[7], acc[7].1 + 1)
    ]
  {
    AccAddOne_head(acc, vals);
    AccAddOne_head(acc[1..], vals[1..]);
    AccAddOne_head(acc[2..], vals[2..]);
    AccAddOne_head(acc[3..], vals[3..]);
    AccAddOne_head(acc[4..], vals[4..]);
    AccAddOne_head(acc[5..], vals[5..]);
    AccAddOne_head(acc[6..], vals[6..]);
    AccAddOne_head(acc[7..], vals[7..]);
    assert AccAddOne(acc[7..], vals[7..])
        == [(acc[7].0 + vals[7], acc[7].1 + 1)];
  }

  lemma AccAddRow_Q1_SpecApply(p: Params, r: R.Row, s: R.SpecStats)
    ensures AccAddRow(Q1Aggs(), p, r, AccOfStats(s))
         == AccOfStats(R.SpecApply(s, r, 1))
  {
    AggsContrib_Q1(p, r);
    var vals := AggsContrib(Q1Aggs(), p, r);
    var acc := AccOfStats(s);
    AccAddOne_len8(acc, vals);
  }

  // =====================================================================
  // Phase-2 migration bridge (A–H). Top-level only composes these lemmas.
  // No Q1 semantic fast path in Eval_Spec.
  // =====================================================================

  // Combined store relation (C+D+E). Opaque so recursive remove proofs stay small.
  ghost predicate {:opaque} StoreRel(st: GroupStore, m: R.KeyStore) {
    (forall k :: k in st ==>
       |k| == 2 && 0 <= k[0] < 256 && 0 <= k[1] < 256 &&
       AccWellFormed(st[k]) &&
       R.Pack(k[0], k[1]) in m &&
       st[k] == AccOfStats(m[R.Pack(k[0], k[1])])) &&
    (forall g :: g in m ==>
       0 <= g < 65536 &&
       [g / 256, g % 256] in st &&
       st[[g / 256, g % 256]] == AccOfStats(m[g]))
  }

  // ----- A. Pack injective on rf,ls ∈ [0,256) -----
  lemma A_Pack_div_mod(rf: int, ls: int)
    requires 0 <= rf < 256 && 0 <= ls < 256
    ensures R.Pack(rf, ls) / 256 == rf
    ensures R.Pack(rf, ls) % 256 == ls
    ensures 0 <= R.Pack(rf, ls) < 65536
  {
  }

  lemma A_Pack_inj(rf1: int, ls1: int, rf2: int, ls2: int)
    requires 0 <= rf1 < 256 && 0 <= ls1 < 256
    requires 0 <= rf2 < 256 && 0 <= ls2 < 256
    ensures R.Pack(rf1, ls1) == R.Pack(rf2, ls2) ==> rf1 == rf2 && ls1 == ls2
  {
    if R.Pack(rf1, ls1) == R.Pack(rf2, ls2) {
      A_Pack_div_mod(rf1, ls1);
      A_Pack_div_mod(rf2, ls2);
    }
  }

  lemma A_Unpack_pack(g: int)
    requires 0 <= g < 65536
    ensures g == (g / 256) * 256 + (g % 256)
    ensures 0 <= g / 256 < 256 && 0 <= g % 256 < 256
    ensures R.Pack(g / 256, g % 256) == g
  {
  }

  // ----- B. Pack order ≡ lex on [rf,ls] -----
  lemma B_Pack_order(rf1: int, ls1: int, rf2: int, ls2: int)
    requires 0 <= rf1 < 256 && 0 <= ls1 < 256
    requires 0 <= rf2 < 256 && 0 <= ls2 < 256
    ensures R.Pack(rf1, ls1) < R.Pack(rf2, ls2)
         <==> (rf1 < rf2 || (rf1 == rf2 && ls1 < ls2))
    ensures R.Pack(rf1, ls1) <= R.Pack(rf2, ls2)
         <==> (rf1 < rf2 || (rf1 == rf2 && ls1 <= ls2))
  {
  }

  lemma B_KeyLexLe_eq_Pack(a: seq<int>, b: seq<int>)
    requires |a| == 2 && |b| == 2
    requires 0 <= a[0] < 256 && 0 <= a[1] < 256
    requires 0 <= b[0] < 256 && 0 <= b[1] < 256
    ensures KeyLexLe(a, b)
         <==> R.Pack(a[0], a[1]) <= R.Pack(b[0], b[1])
  {
    if KeyLexLe(a, b) {
      KeyLexLe2_unfold(a, b);
      B_Pack_order(a[0], a[1], b[0], b[1]);
    } else {
      KeyLexLe_total(a, b);
      assert KeyLexLe(b, a);
      if a == b {
        KeyLexLe_refl(a);
        assert false;
      }
      KeyLexLe2_strict(b, a);
      B_Pack_order(b[0], b[1], a[0], a[1]);
      assert R.Pack(b[0], b[1]) < R.Pack(a[0], a[1]);
    }
  }

  // ----- shared Q1 shape / filter helpers -----
  lemma Q1GroupKeys_shape()
    ensures Q1GroupKeys() == [EColumn(ColRf), EColumn(ColLs)]
    ensures |Q1GroupKeys()| == 2
  {
  }

  lemma Q1OrderKeys_ok()
    ensures forall ok :: ok in Q1OrderKeys() ==>
              exists i :: 0 <= i < |Q1GroupKeys()| && ExprEq(ok.expr, Q1GroupKeys()[i])
    ensures |Q1GroupKeys()| == 2
    ensures |Q1Aggs()| == 8
  {
    Q1GroupKeys_shape();
    assert Q1OrderKeys() == [
      OrderKey(EColumn(ColRf), SortAsc),
      OrderKey(EColumn(ColLs), SortAsc)
    ];
    assert ExprEq(EColumn(ColRf), Q1GroupKeys()[0]);
    assert ExprEq(EColumn(ColLs), Q1GroupKeys()[1]);
  }

  lemma EvalPreds_Q1_filter(c: int, r: R.Row)
    ensures EvalPreds(Q1Filters(), ParamsOfCutoff(c), r) <==> r.shipdate <= c
  {
    assert Q1Filters() == [PLe(EColumn(ColShipdate), EParam(ParamCutoff, Scale0))];
    assert EvalExpr(EColumn(ColShipdate), ParamsOfCutoff(c), r) == r.shipdate;
    assert EvalExpr(EParam(ParamCutoff, Scale0), ParamsOfCutoff(c), r) == c;
    assert EvalPred(Q1Filters()[0], ParamsOfCutoff(c), r) <==> r.shipdate <= c;
  }

  lemma EvalExprs_Q1_group(p: Params, r: R.Row)
    ensures EvalExprs(Q1GroupKeys(), p, r) == [r.rf, r.ls]
  {
    assert Q1GroupKeys() == [EColumn(ColRf), EColumn(ColLs)];
    assert EvalExpr(EColumn(ColRf), p, r) == r.rf;
    assert EvalExpr(EColumn(ColLs), p, r) == r.ls;
  }

  // ----- C/D/E pointwise -----
  lemma C_StoreRel_empty()
    ensures StoreRel(map[], map[])
  {
    reveal StoreRel();
  }

  lemma D_Domain_forward(st: GroupStore, m: R.KeyStore, k: seq<int>)
    requires StoreRel(st, m)
    requires k in st
    ensures |k| == 2 && 0 <= k[0] < 256 && 0 <= k[1] < 256
    ensures R.Pack(k[0], k[1]) in m
  {
    reveal StoreRel();
  }

  lemma D_Domain_backward(st: GroupStore, m: R.KeyStore, g: int)
    requires StoreRel(st, m)
    requires g in m
    ensures 0 <= g < 65536
    ensures [g / 256, g % 256] in st
  {
    reveal StoreRel();
  }

  lemma E_Acc_at_semantic(st: GroupStore, m: R.KeyStore, k: seq<int>)
    requires StoreRel(st, m)
    requires k in st
    ensures |k| == 2 && 0 <= k[0] < 256 && 0 <= k[1] < 256
    ensures AccWellFormed(st[k])
    ensures R.Pack(k[0], k[1]) in m
    ensures st[k] == AccOfStats(m[R.Pack(k[0], k[1])])
  {
    reveal StoreRel();
  }

  lemma StoreRel_empty_keys(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    ensures st.Keys == {} <==> m.Keys == {}
  {
    reveal StoreRel();
    if st.Keys != {} && m.Keys == {} {
      var k :| k in st.Keys;
      assert R.Pack(k[0], k[1]) in m;
      assert false;
    }
    if st.Keys == {} && m.Keys != {} {
      var g :| g in m.Keys;
      assert [g / 256, g % 256] in st;
      assert false;
    }
  }

  lemma StoreAddRow_rel_fwd(p: Params, r: R.Row, st: GroupStore, m: R.KeyStore,
                            st': GroupStore, m': R.KeyStore, k: seq<int>, g: int)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires StoreRel(st, m)
    requires forall kk :: kk in st ==> |st[kk]| == 8
    requires k == [r.rf, r.ls] && g == R.Pack(r.rf, r.ls)
    requires st' == st[k := AccAddRow(Q1Aggs(), p, r, if k in st then st[k] else EmptyAcc(8))]
    requires m' == m[g := R.SpecApply(if g in m then m[g] else R.EmptyStats(), r, 1)]
    ensures forall kk :: kk in st' ==>
              |kk| == 2 && 0 <= kk[0] < 256 && 0 <= kk[1] < 256 &&
              AccWellFormed(st'[kk]) &&
              R.Pack(kk[0], kk[1]) in m' &&
              st'[kk] == AccOfStats(m'[R.Pack(kk[0], kk[1])])
  {
    reveal StoreRel();
    A_Pack_div_mod(r.rf, r.ls);
    var acc0 := if k in st then st[k] else EmptyAcc(8);
    var s0 := if g in m then m[g] else R.EmptyStats();
    if k in st {
      assert acc0 == AccOfStats(s0);
    } else {
      assert g !in m;
      AccOfStats_Empty();
      assert acc0 == AccOfStats(s0);
    }
    AccAddRow_Q1_SpecApply(p, r, s0);
    AccOfStats_well(m'[g]);
    forall kk | kk in st'
      ensures |kk| == 2 && 0 <= kk[0] < 256 && 0 <= kk[1] < 256 &&
              AccWellFormed(st'[kk]) &&
              R.Pack(kk[0], kk[1]) in m' &&
              st'[kk] == AccOfStats(m'[R.Pack(kk[0], kk[1])])
    {
      if kk == k {
      } else {
        assert kk in st;
        assert R.Pack(kk[0], kk[1]) != g by {
          A_Pack_inj(kk[0], kk[1], r.rf, r.ls);
        }
      }
    }
  }

  lemma StoreAddRow_rel_bwd(p: Params, r: R.Row, st: GroupStore, m: R.KeyStore,
                            st': GroupStore, m': R.KeyStore, k: seq<int>, g: int)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires StoreRel(st, m)
    requires forall kk :: kk in st ==> |st[kk]| == 8
    requires k == [r.rf, r.ls] && g == R.Pack(r.rf, r.ls)
    requires st' == st[k := AccAddRow(Q1Aggs(), p, r, if k in st then st[k] else EmptyAcc(8))]
    requires m' == m[g := R.SpecApply(if g in m then m[g] else R.EmptyStats(), r, 1)]
    ensures forall gg :: gg in m' ==>
              0 <= gg < 65536 &&
              [gg / 256, gg % 256] in st' &&
              st'[[gg / 256, gg % 256]] == AccOfStats(m'[gg])
  {
    reveal StoreRel();
    A_Pack_div_mod(r.rf, r.ls);
    var acc0 := if k in st then st[k] else EmptyAcc(8);
    var s0 := if g in m then m[g] else R.EmptyStats();
    if k in st {
      assert acc0 == AccOfStats(s0);
    } else {
      assert g !in m;
      AccOfStats_Empty();
    }
    AccAddRow_Q1_SpecApply(p, r, s0);
    AccOfStats_well(m'[g]);
    forall gg | gg in m'
      ensures 0 <= gg < 65536 &&
              [gg / 256, gg % 256] in st' &&
              st'[[gg / 256, gg % 256]] == AccOfStats(m'[gg])
    {
      if gg == g {
      } else {
        assert gg in m;
      }
    }
  }

  lemma StoreAddRow_rel(p: Params, r: R.Row, st: GroupStore, m: R.KeyStore)
    requires 0 <= r.rf < 256 && 0 <= r.ls < 256
    requires StoreRel(st, m)
    requires forall k :: k in st ==> |st[k]| == 8
    ensures StoreRel(StoreAddRow(Q1Aggs(), Q1GroupKeys(), p, r, st),
                     R.ApplyDeltaKey(m, r, 1))
  {
    reveal StoreRel();
    EvalExprs_Q1_group(p, r);
    var k := [r.rf, r.ls];
    var g := R.Pack(r.rf, r.ls);
    A_Pack_div_mod(r.rf, r.ls);
    var acc0 := if k in st then st[k] else EmptyAcc(8);
    var s0 := if g in m then m[g] else R.EmptyStats();
    var st' := st[k := AccAddRow(Q1Aggs(), p, r, acc0)];
    var m' := m[g := R.SpecApply(s0, r, 1)];
    assert StoreAddRow(Q1Aggs(), Q1GroupKeys(), p, r, st) == st';
    assert R.ApplyDeltaKey(m, r, 1) == m';
    StoreAddRow_rel_fwd(p, r, st, m, st', m', k, g);
    StoreAddRow_rel_bwd(p, r, st, m, st', m', k, g);
  }

  ghost function FoldByGroupFrom(m: R.KeyStore, rs: seq<R.Row>): R.KeyStore
    requires forall i :: 0 <= i < |rs| ==> 0 <= rs[i].rf < 256 && 0 <= rs[i].ls < 256
    decreases |rs|
  {
    if |rs| == 0 then m
    else R.ApplyDeltaKey(FoldByGroupFrom(m, rs[..|rs| - 1]), rs[|rs| - 1], 1)
  }

  lemma FoldByGroupFrom_empty(rs: seq<R.Row>)
    requires forall i :: 0 <= i < |rs| ==> 0 <= rs[i].rf < 256 && 0 <= rs[i].ls < 256
    ensures FoldByGroupFrom(map[], rs) == R.FoldByGroup(rs)
    decreases |rs|
  {
    if |rs| == 0 {
    } else {
      FoldByGroupFrom_empty(rs[..|rs| - 1]);
    }
  }

  lemma FoldGroups_rel(p: Params, rs: seq<R.Row>, st: GroupStore, m: R.KeyStore)
    requires forall i :: 0 <= i < |rs| ==> 0 <= rs[i].rf < 256 && 0 <= rs[i].ls < 256
    requires StoreRel(st, m)
    requires forall k :: k in st ==> |st[k]| == 8
    ensures StoreRel(FoldGroups(Q1Aggs(), Q1GroupKeys(), p, rs, st),
                     FoldByGroupFrom(m, rs))
    decreases |rs|
  {
    if |rs| == 0 {
    } else {
      FoldGroups_rel(p, rs[..|rs| - 1], st, m);
      var st1 := FoldGroups(Q1Aggs(), Q1GroupKeys(), p, rs[..|rs| - 1], st);
      var m1 := FoldByGroupFrom(m, rs[..|rs| - 1]);
      assert forall k :: k in st1 ==> |st1[k]| == 8;
      StoreAddRow_rel(p, rs[|rs| - 1], st1, m1);
    }
  }

  lemma FoldGroups_from_empty(p: Params, rs: seq<R.Row>)
    requires forall i :: 0 <= i < |rs| ==> 0 <= rs[i].rf < 256 && 0 <= rs[i].ls < 256
    ensures StoreRel(FoldGroups(Q1Aggs(), Q1GroupKeys(), p, rs, map[]),
                     R.FoldByGroup(rs))
  {
    C_StoreRel_empty();
    FoldGroups_rel(p, rs, map[], map[]);
    FoldByGroupFrom_empty(rs);
  }

  lemma StoreRel_remove(st: GroupStore, m: R.KeyStore, k: seq<int>, g: int)
    requires StoreRel(st, m)
    requires k in st
    requires |k| == 2 && 0 <= k[0] < 256 && 0 <= k[1] < 256
    requires g == R.Pack(k[0], k[1]) && g in m
    ensures StoreRel(st - {k}, m - {g})
  {
    reveal StoreRel();
    A_Pack_div_mod(k[0], k[1]);
    forall kk | kk in (st - {k})
      ensures |kk| == 2 && 0 <= kk[0] < 256 && 0 <= kk[1] < 256 &&
              AccWellFormed((st - {k})[kk]) &&
              R.Pack(kk[0], kk[1]) in (m - {g}) &&
              (st - {k})[kk] == AccOfStats((m - {g})[R.Pack(kk[0], kk[1])])
    {
      assert kk in st && kk != k;
      A_Pack_inj(kk[0], kk[1], k[0], k[1]);
    }
    forall gg | gg in (m - {g})
      ensures 0 <= gg < 65536 &&
              [gg / 256, gg % 256] in (st - {k}) &&
              (st - {k})[[gg / 256, gg % 256]] == AccOfStats((m - {g})[gg])
    {
      assert gg in m && gg != g;
      A_Unpack_pack(gg);
      assert [gg / 256, gg % 256] != k;
    }
  }

  lemma G_StoreRel_key_bounds(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    ensures forall k :: k in st ==> |k| == 2 && 0 <= k[0] < 256 && 0 <= k[1] < 256
  {
    reveal StoreRel();
  }

  // ----- F. Finalize-one -----
  lemma F_Q1AdaptRow_of_stats(rf: int, ls: int, s: R.SpecStats)
    requires 0 <= rf < 256 && 0 <= ls < 256
    ensures Q1AdaptRow(SpecResultRow([rf, ls], AccOfStats(s)))
         == R.SpecFinalizeOne(R.Pack(rf, ls), s)
  {
  }

  // ----- G. Ordered-key-sequence correspondence -----
  ghost predicate StrictAsc(keys: seq<int>) {
    forall i :: 0 <= i < |keys| - 1 ==> keys[i] < keys[i + 1]
  }

  // Ascending enumeration of a finite int set (no KeysFrom range walk).
  ghost predicate IsIntMin(s: set<int>, x: int) {
    x in s && (forall y :: y in s ==> x <= y)
  }

  lemma IntMin_exists(s: set<int>)
    requires s != {}
    ensures exists x :: IsIntMin(s, x)
    decreases |s|
  {
    var x :| x in s;
    if s == {x} {
      assert IsIntMin(s, x);
    } else {
      IntMin_exists(s - {x});
      var m :| IsIntMin(s - {x}, m);
      if x <= m {
        forall y | y in s
          ensures x <= y
        {
          if y != x {
            assert y in (s - {x});
            assert m <= y;
            assert x <= y;
          }
        }
        assert IsIntMin(s, x);
      } else {
        forall y | y in s
          ensures m <= y
        {
          if y == x {
            assert m < x;
          } else {
            assert y in (s - {x});
            assert m <= y;
          }
        }
        assert IsIntMin(s, m);
      }
    }
  }

  ghost function MinIntOf(s: set<int>): int
    requires s != {}
    ensures IsIntMin(s, MinIntOf(s))
  {
    assert exists x :: IsIntMin(s, x) by { IntMin_exists(s); }
    var x :| IsIntMin(s, x);
    x
  }

  ghost function SortedInts(s: set<int>): seq<int>
    ensures |SortedInts(s)| == |s|
    decreases |s|
  {
    if s == {} then
      []
    else
      var x := MinIntOf(s);
      [x] + SortedInts(s - {x})
  }

  lemma SortedInts_cons(s: set<int>)
    requires s != {}
    ensures SortedInts(s) == [MinIntOf(s)] + SortedInts(s - {MinIntOf(s)})
  {
  }

  lemma SortedInts_head_lt(s: set<int>)
    requires |s| >= 2
    ensures SortedInts(s)[0] < SortedInts(s)[1]
  {
    var x := MinIntOf(s);
    assert x in s;
    assert |s - {x}| == |s| - 1;
    assert |s - {x}| >= 1;
    assert s - {x} != {};
    SortedInts_cons(s);
    var y := MinIntOf(s - {x});
    assert SortedInts(s)[0] == x;
    assert SortedInts(s)[1] == y;
    assert y in (s - {x});
    assert x <= y && x != y;
  }

  lemma SortedInts_strict_asc(s: set<int>)
    ensures StrictAsc(SortedInts(s))
    decreases |s|
  {
    if |s| <= 1 {
    } else {
      SortedInts_head_lt(s);
      var x := MinIntOf(s);
      SortedInts_strict_asc(s - {x});
    }
  }

  lemma SortedInts_in(s: set<int>, x: int)
    requires x in s
    ensures x in SortedInts(s)
    decreases |s|
  {
    var m := MinIntOf(s);
    if x == m {
    } else {
      SortedInts_in(s - {m}, x);
    }
  }

  lemma SortedInts_elem(s: set<int>, i: nat)
    requires 0 <= i < |SortedInts(s)|
    ensures SortedInts(s)[i] in s
    decreases |s|
  {
    if s == {} {
    } else {
      var m := MinIntOf(s);
      if i == 0 {
      } else {
        SortedInts_elem(s - {m}, i - 1);
      }
    }
  }

  // Total: under StoreRel every key has |k|==2 and byte bounds.
  ghost function PackKeysFromStore(st: GroupStore): seq<int>
    decreases |st.Keys|
  {
    if st.Keys == {} then
      []
    else
      var k0 := MinKeyOf(st.Keys);
      if |k0| == 2 && 0 <= k0[0] < 256 && 0 <= k0[1] < 256 then
        [R.Pack(k0[0], k0[1])] + PackKeysFromStore(st - {k0})
      else
        PackKeysFromStore(st - {k0})
  }

  lemma StoreToRows_cons(st: GroupStore)
    requires st.Keys != {}
    ensures StoreToRows(st)
         == [SpecResultRow(MinKeyOf(st.Keys), st[MinKeyOf(st.Keys)])]
          + StoreToRows(st - {MinKeyOf(st.Keys)})
  {
  }

  lemma G_index_lt(keys: seq<int>, i: int, j: int)
    requires StrictAsc(keys)
    requires 0 <= i < j < |keys|
    ensures keys[i] < keys[j]
    decreases j - i
  {
    if j == i + 1 {
    } else {
      G_index_lt(keys, i, j - 1);
    }
  }

  lemma G_PackKeys_cons(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    requires st.Keys != {}
    ensures |MinKeyOf(st.Keys)| == 2
    ensures 0 <= MinKeyOf(st.Keys)[0] < 256 && 0 <= MinKeyOf(st.Keys)[1] < 256
    ensures PackKeysFromStore(st)
         == [R.Pack(MinKeyOf(st.Keys)[0], MinKeyOf(st.Keys)[1])]
          + PackKeysFromStore(st - {MinKeyOf(st.Keys)})
  {
    var k0 := MinKeyOf(st.Keys);
    D_Domain_forward(st, m, k0);
  }

  lemma G_seq_set_card(s: seq<int>, ks: set<int>)
    requires forall i, j :: 0 <= i < j < |s| ==> s[i] != s[j]
    requires forall i :: 0 <= i < |s| ==> s[i] in ks
    requires forall x :: x in ks ==> exists i :: 0 <= i < |s| && s[i] == x
    ensures |s| == |ks|
    decreases |s|
  {
    if |s| == 0 {
      assert ks == {};
    } else {
      var x := s[0];
      forall i | 0 <= i < |s[1..]|
        ensures s[1..][i] in (ks - {x})
      {
        assert s[i + 1] in ks && s[i + 1] != x;
      }
      forall y | y in (ks - {x})
        ensures exists i :: 0 <= i < |s[1..]| && s[1..][i] == y
      {
        var i :| 0 <= i < |s| && s[i] == y;
        assert i != 0;
        assert s[1..][i - 1] == y;
      }
      forall i, j | 0 <= i < j < |s[1..]|
        ensures s[1..][i] != s[1..][j]
      {
        assert s[i + 1] != s[j + 1];
      }
      G_seq_set_card(s[1..], ks - {x});
    }
  }

  lemma G_StrictAsc_nodup(keys: seq<int>)
    requires StrictAsc(keys)
    ensures forall i, j :: 0 <= i < j < |keys| ==> keys[i] != keys[j]
  {
    forall i, j | 0 <= i < j < |keys|
      ensures keys[i] != keys[j]
    {
      G_index_lt(keys, i, j);
    }
  }

  lemma G_AscSeq_unique(a: seq<int>, b: seq<int>)
    requires StrictAsc(a) && StrictAsc(b)
    requires |a| == |b|
    requires forall x :: (x in a) <==> (x in b)
    ensures a == b
    decreases |a|
  {
    if |a| == 0 {
    } else {
      assert a[0] in a;
      assert a[0] in b;
      var jb :| 0 <= jb < |b| && b[jb] == a[0];
      if jb != 0 {
        assert b[0] in b;
        assert b[0] in a;
        var ia :| 0 <= ia < |a| && a[ia] == b[0];
        G_StrictAsc_nodup(a);
        G_StrictAsc_nodup(b);
        if ia == 0 {
          assert b[0] == a[0] && b[jb] == a[0] && 0 < jb;
          assert false;
        } else {
          G_index_lt(a, 0, ia);
          G_index_lt(b, 0, jb);
          assert false;
        }
      }
      assert a[0] == b[0];
      G_StrictAsc_nodup(a);
      G_StrictAsc_nodup(b);
      forall x | true
        ensures (x in a[1..]) <==> (x in b[1..])
      {
        if x in a[1..] {
          assert x in a && x != a[0];
          assert x in b && x != b[0];
          assert x in b[1..];
        }
        if x in b[1..] {
          assert x in b && x != b[0];
          assert x in a && x != a[0];
          assert x in a[1..];
        }
      }
      G_AscSeq_unique(a[1..], b[1..]);
    }
  }

  // KeysFrom(0,65536,·) = SortedInts on the key set. Proved without StoreRel.
  lemma G_KeysFrom_eq_SortedInts(m: R.KeyStore)
    requires forall g :: g in m ==> 0 <= g < 65536
    ensures R.KeysFrom(0, 65536, m) == SortedInts(m.Keys)
  {
    var a := SortedInts(m.Keys);
    var b := R.KeysFrom(0, 65536, m);
    SortedInts_strict_asc(m.Keys);
    R.KeysFrom_all_in_m(0, 65536, m);
    R.KeysFrom_strict_sorted(0, 65536, m);
    G_StrictAsc_nodup(a);
    G_StrictAsc_nodup(b);
    forall i | 0 <= i < |a|
      ensures a[i] in m.Keys
    {
      SortedInts_elem(m.Keys, i);
    }
    forall g | g in m.Keys
      ensures g in a
    {
      SortedInts_in(m.Keys, g);
    }
    forall g | g in m.Keys
      ensures g in b
    {
      R.KeysFrom_contains_fwd(0, 65536, m, g);
    }
    G_seq_set_card(a, m.Keys);
    G_seq_set_card(b, m.Keys);
    forall x | true
      ensures (x in a) <==> (x in b)
    {
      if x in a {
        SortedInts_elem(m.Keys, 0); // help trigger
        var i :| 0 <= i < |a| && a[i] == x;
        SortedInts_elem(m.Keys, i);
        R.KeysFrom_contains_fwd(0, 65536, m, x);
      }
      if x in b {
        assert x in m;
        SortedInts_in(m.Keys, x);
      }
    }
    G_AscSeq_unique(a, b);
  }

  // Lex-min semantic key packs to numeric min of m.Keys.
  lemma G_MinKey_Pack_is_MinInt(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    requires st.Keys != {}
    ensures |MinKeyOf(st.Keys)| == 2
    ensures 0 <= MinKeyOf(st.Keys)[0] < 256 && 0 <= MinKeyOf(st.Keys)[1] < 256
    ensures m.Keys != {}
    ensures R.Pack(MinKeyOf(st.Keys)[0], MinKeyOf(st.Keys)[1]) == MinIntOf(m.Keys)
  {
    StoreRel_empty_keys(st, m);
    var k0 := MinKeyOf(st.Keys);
    D_Domain_forward(st, m, k0);
    var g0 := R.Pack(k0[0], k0[1]);
    A_Pack_div_mod(k0[0], k0[1]);
    var gmin := MinIntOf(m.Keys);
    // g0 <= every g in m via lex-min + Pack order
    forall g | g in m
      ensures g0 <= g
    {
      D_Domain_backward(st, m, g);
      A_Unpack_pack(g);
      var kk := [g / 256, g % 256];
      assert kk in st;
      assert KeyLexLe(k0, kk);
      KeyLexLe2_unfold(k0, kk);
      B_Pack_order(k0[0], k0[1], kk[0], kk[1]);
    }
    assert g0 in m.Keys;
    assert IsIntMin(m.Keys, g0);
    // MinIntOf unique
    assert IsIntMin(m.Keys, gmin);
    assert g0 == gmin;
  }

  // PackKeysFromStore(st) = SortedInts(m.Keys) under StoreRel.
  lemma {:induction false} G_PackKeys_eq_SortedInts(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    ensures PackKeysFromStore(st) == SortedInts(m.Keys)
    decreases |m.Keys|
  {
    StoreRel_empty_keys(st, m);
    if m.Keys == {} {
      assert st.Keys == {};
    } else {
      assert st.Keys != {};
      G_MinKey_Pack_is_MinInt(st, m);
      var k0 := MinKeyOf(st.Keys);
      D_Domain_forward(st, m, k0);
      var g0 := R.Pack(k0[0], k0[1]);
      assert g0 == MinIntOf(m.Keys);
      SortedInts_cons(m.Keys);
      assert SortedInts(m.Keys) == [g0] + SortedInts(m.Keys - {g0});
      G_PackKeys_cons(st, m);
      assert PackKeysFromStore(st) == [g0] + PackKeysFromStore(st - {k0});
      StoreRel_remove(st, m, k0, g0);
      var st1 := st - {k0};
      var m1 := m - {g0};
      assert m1.Keys == m.Keys - {g0};
      assert |m1.Keys| < |m.Keys|;
      G_PackKeys_eq_SortedInts(st1, m1);
      assert PackKeysFromStore(st1) == SortedInts(m1.Keys);
    }
  }

  // Reusable: KeysFrom on packed-Q1 KeyStore = PackKeysFromStore.
  lemma G_KeysFrom_eq_PackKeys(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    requires forall g :: g in m ==> 0 <= g < 65536
    ensures R.KeysFrom(0, 65536, m) == PackKeysFromStore(st)
  {
    G_KeysFrom_eq_SortedInts(m);
    G_PackKeys_eq_SortedInts(st, m);
  }

  // ----- H. map/sequence lifting: Adapt ≡ FinalizeKeys -----
  lemma H_FinalizeKeys_remove(m: R.KeyStore, m1: R.KeyStore, g0: int, keys: seq<int>)
    requires m1 == m - {g0}
    requires forall i :: 0 <= i < |keys| ==> keys[i] in m1
    ensures R.FinalizeKeys(keys, m) == R.FinalizeKeys(keys, m1)
    decreases |keys|
  {
    if |keys| == 0 {
    } else {
      assert R.GetKey(m, keys[0]) == m[keys[0]] == m1[keys[0]] == R.GetKey(m1, keys[0]);
      H_FinalizeKeys_remove(m, m1, g0, keys[1..]);
    }
  }

  lemma H_PackKeys_tail_in_m1(st1: GroupStore, m1: R.KeyStore, i: nat)
    requires StoreRel(st1, m1)
    requires 0 <= i < |PackKeysFromStore(st1)|
    ensures PackKeysFromStore(st1)[i] in m1
  {
    G_PackKeys_eq_SortedInts(st1, m1);
    assert PackKeysFromStore(st1) == SortedInts(m1.Keys);
    SortedInts_elem(m1.Keys, i);
  }

  lemma H_Adapt_eq_FinalizeKeys(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    requires forall g :: g in m ==> m[g].count > 0 && 0 <= g < 65536
    ensures Q1Adapt(StoreToRows(st))
         == R.FinalizeKeys(PackKeysFromStore(st), m)
    decreases |st.Keys|
  {
    G_StoreRel_key_bounds(st, m);
    if st.Keys == {} {
    } else {
      StoreToRows_cons(st);
      G_PackKeys_cons(st, m);
      var k0 := MinKeyOf(st.Keys);
      var g0 := R.Pack(k0[0], k0[1]);
      D_Domain_forward(st, m, k0);
      E_Acc_at_semantic(st, m, k0);
      assert AccWellFormed(st[k0]);
      F_Q1AdaptRow_of_stats(k0[0], k0[1], m[g0]);
      assert R.GetKey(m, g0) == m[g0];
      assert StoreToRows(st)[0] == SpecResultRow(k0, st[k0]);
      assert Q1AdaptRow(StoreToRows(st)[0]) == R.SpecFinalizeOne(g0, m[g0]);
      StoreRel_remove(st, m, k0, g0);
      var st1 := st - {k0};
      var m1 := m - {g0};
      H_Adapt_eq_FinalizeKeys(st1, m1);
      assert Q1Adapt(StoreToRows(st))
          == Q1AdaptRow(StoreToRows(st)[0]) + Q1Adapt(StoreToRows(st1));
      assert Q1Adapt(StoreToRows(st1)) == R.FinalizeKeys(PackKeysFromStore(st1), m1);
      forall i | 0 <= i < |PackKeysFromStore(st1)|
        ensures PackKeysFromStore(st1)[i] in m1
      {
        H_PackKeys_tail_in_m1(st1, m1, i);
      }
      H_FinalizeKeys_remove(m, m1, g0, PackKeysFromStore(st1));
      assert PackKeysFromStore(st) == [g0] + PackKeysFromStore(st1);
      assert R.FinalizeKeys(PackKeysFromStore(st), m)
          == R.SpecFinalizeOne(g0, m[g0])
           + R.FinalizeKeys(PackKeysFromStore(st1), m);
    }
  }

  lemma H_Adapt_eq_Finalize(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    requires forall g :: g in m ==> m[g].count > 0 && 0 <= g < 65536
    ensures Q1Adapt(StoreToRows(st))
         == R.FinalizeKeys(R.KeysFrom(0, 65536, m), m)
  {
    G_KeysFrom_eq_PackKeys(st, m);
    H_Adapt_eq_FinalizeKeys(st, m);
    assert PackKeysFromStore(st) == R.KeysFrom(0, 65536, m);
  }

  // ----- Order-id / filter-id / Eval_Spec reveal composition -----
  // Recursive adjacent-sortedness (avoids large forall VCs at use sites).
  ghost predicate SpecRowsAdjSorted(rows: seq<SpecResultRow>) {
    if |rows| <= 1 then
      true
    else
      |rows[0].key| == 2 && |rows[1].key| == 2 &&
      (rows[0].key[0] < rows[1].key[0] ||
       (rows[0].key[0] == rows[1].key[0] &&
        rows[0].key[1] < rows[1].key[1])) &&
      SpecRowsAdjSorted(rows[1..])
  }

  ghost predicate Q1OrderOk() {
    forall ok :: ok in Q1OrderKeys() ==>
      exists i :: 0 <= i < |Q1GroupKeys()| && ExprEq(ok.expr, Q1GroupKeys()[i])
  }

  lemma Q1OrderOk_holds()
    ensures Q1OrderOk()
  {
    Q1OrderKeys_ok();
  }

  lemma Lookup_Q1(kvals: seq<int>)
    requires |kvals| == 2
    ensures LookupKeyExpr(EColumn(ColRf), Q1GroupKeys(), kvals) == kvals[0]
    ensures LookupKeyExpr(EColumn(ColLs), Q1GroupKeys(), kvals) == kvals[1]
  {
    Q1GroupKeys_shape();
  }

  lemma ResultRowLe_Q1(a: SpecResultRow, b: SpecResultRow)
    requires Q1OrderOk()
    requires |a.key| == 2 && |b.key| == 2
    ensures ResultRowLe(Q1OrderKeys(), Q1GroupKeys(), a, b)
         <==> (a.key[0] < b.key[0] ||
               (a.key[0] == b.key[0] && a.key[1] <= b.key[1]))
  {
    Lookup_Q1(a.key);
    Lookup_Q1(b.key);
  }

  lemma StoreToRows_in(st: GroupStore, row: SpecResultRow)
    requires row in StoreToRows(st)
    ensures row.key in st && row.acc == st[row.key]
    decreases |st.Keys|
  {
    if st.Keys == {} {
      assert false;
    } else {
      StoreToRows_cons(st);
      var k0 := MinKeyOf(st.Keys);
      if row == SpecResultRow(k0, st[k0]) {
      } else {
        assert row in StoreToRows(st - {k0});
        StoreToRows_in(st - {k0}, row);
      }
    }
  }

  lemma StoreToRows_head_lt(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    requires |StoreToRows(st)| > 1
    ensures |StoreToRows(st)[0].key| == 2 && |StoreToRows(st)[1].key| == 2
    ensures StoreToRows(st)[0].key[0] < StoreToRows(st)[1].key[0] ||
            (StoreToRows(st)[0].key[0] == StoreToRows(st)[1].key[0] &&
             StoreToRows(st)[0].key[1] < StoreToRows(st)[1].key[1])
  {
    G_StoreRel_key_bounds(st, m);
    StoreToRows_cons(st);
    var k0 := MinKeyOf(st.Keys);
    var g0 := R.Pack(k0[0], k0[1]);
    D_Domain_forward(st, m, k0);
    var st1 := st - {k0};
    assert st1.Keys != {};
    var k1 := MinKeyOf(st1.Keys);
    assert StoreToRows(st)[1].key == k1;
    assert KeyLexLe(k0, k1) && k0 != k1;
    KeyLexLe2_strict(k0, k1);
  }

  lemma SpecRowsAdjSorted_cons(head: SpecResultRow, tail: seq<SpecResultRow>)
    requires SpecRowsAdjSorted(tail)
    requires |head.key| == 2
    requires |tail| == 0 || (
      |tail[0].key| == 2 &&
      (head.key[0] < tail[0].key[0] ||
       (head.key[0] == tail[0].key[0] && head.key[1] < tail[0].key[1]))
    )
    ensures SpecRowsAdjSorted([head] + tail)
  {
    assert ([head] + tail)[1..] == tail;
  }

  lemma SpecRowsAdjSorted_at(rows: seq<SpecResultRow>, i: int)
    requires SpecRowsAdjSorted(rows)
    requires 0 <= i < |rows| - 1
    ensures |rows[i].key| == 2 && |rows[i+1].key| == 2
    ensures rows[i].key[0] < rows[i+1].key[0] ||
            (rows[i].key[0] == rows[i+1].key[0] &&
             rows[i].key[1] < rows[i+1].key[1])
    decreases i
  {
    if i == 0 {
    } else {
      SpecRowsAdjSorted_at(rows[1..], i - 1);
    }
  }

  lemma StoreToRows_Q1_sorted(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    ensures SpecRowsAdjSorted(StoreToRows(st))
    decreases |st.Keys|
  {
    G_StoreRel_key_bounds(st, m);
    if st.Keys == {} {
    } else if |StoreToRows(st)| <= 1 {
    } else {
      StoreToRows_head_lt(st, m);
      StoreToRows_cons(st);
      var k0 := MinKeyOf(st.Keys);
      D_Domain_forward(st, m, k0);
      var g0 := R.Pack(k0[0], k0[1]);
      StoreRel_remove(st, m, k0, g0);
      var st1 := st - {k0};
      var m1 := m - {g0};
      StoreToRows_Q1_sorted(st1, m1);
      var head := SpecResultRow(k0, st[k0]);
      var tail := StoreToRows(st1);
      assert StoreToRows(st) == [head] + tail;
      assert SpecRowsAdjSorted(tail);
      assert |head.key| == 2;
      assert |tail| > 0;
      assert |tail[0].key| == 2;
      assert head.key[0] < tail[0].key[0] ||
             (head.key[0] == tail[0].key[0] && head.key[1] < tail[0].key[1]);
      SpecRowsAdjSorted_cons(head, tail);
    }
  }

  lemma ResultRowSort_id_on_sorted(l: seq<SpecResultRow>)
    requires Q1OrderOk()
    requires forall row :: row in l ==> |row.key| == 2
    requires SpecRowsAdjSorted(l)
    ensures ResultRowSort(Q1OrderKeys(), Q1GroupKeys(), l) == l
    decreases |l|
  {
    if |l| == 0 {
    } else {
      ResultRowSort_id_on_sorted(l[1..]);
      var tail := ResultRowSort(Q1OrderKeys(), Q1GroupKeys(), l[1..]);
      assert tail == l[1..];
      if |tail| > 0 {
        ResultRowLe_Q1(l[0], tail[0]);
      }
    }
  }

  lemma FilterNonempty_StoreToRows(st: GroupStore, m: R.KeyStore)
    requires StoreRel(st, m)
    requires forall g :: g in m ==> m[g].count > 0
    ensures FilterNonempty(StoreToRows(st)) == StoreToRows(st)
    decreases |st.Keys|
  {
    G_StoreRel_key_bounds(st, m);
    if st.Keys == {} {
    } else {
      StoreToRows_cons(st);
      var k0 := MinKeyOf(st.Keys);
      var g0 := R.Pack(k0[0], k0[1]);
      D_Domain_forward(st, m, k0);
      E_Acc_at_semantic(st, m, k0);
      assert AccWellFormed(st[k0]);
      assert st[k0] == AccOfStats(m[g0]);
      assert m[g0].count > 0;
      assert StoreToRows(st)[0].acc[0].1 > 0;
      StoreRel_remove(st, m, k0, g0);
      FilterNonempty_StoreToRows(st - {k0}, m - {g0});
    }
  }

  lemma Eval_Spec_Q1_fold_rel(c: int, db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures FilterRows(Q1Filters(), ParamsOfCutoff(c), db) == R.FilterCutoff(db, c)
    ensures StoreRel(
              FoldGroups(Q1Aggs(), Q1GroupKeys(), ParamsOfCutoff(c),
                         R.FilterCutoff(db, c), map[]),
              R.FoldByGroup(R.FilterCutoff(db, c)))
    ensures R.FoldByGroup(R.FilterCutoff(db, c)) == R.SpecView(db, c)
    ensures forall g :: g in R.FoldByGroup(R.FilterCutoff(db, c)) ==>
              R.FoldByGroup(R.FilterCutoff(db, c))[g].count > 0 && 0 <= g < 65536
  {
    FilterRows_eq_FilterCutoff(c, db);
    var filtered := R.FilterCutoff(db, c);
    FoldGroups_from_empty(ParamsOfCutoff(c), filtered);
    R.FoldByGroup_positive_counts(filtered);
    R.FoldByGroup_keys_in_pack_range(filtered);
  }

  // Thin reveal: Eval_Spec(LogicalSpec_Q1) equals sorted StoreToRows of the fold.
  lemma Eval_Spec_Q1_eq_StoreToRows(c: int, db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures var p := ParamsOfCutoff(c);
            var filtered := R.FilterCutoff(db, c);
            var st := FoldGroups(Q1Aggs(), Q1GroupKeys(), p, filtered, map[]);
            Eval_Spec(LogicalSpec_Q1, p, db) == StoreToRows(st)
  {
    var p := ParamsOfCutoff(c);
    Eval_Spec_Q1_fold_rel(c, db);
    Q1OrderOk_holds();
    var filtered := R.FilterCutoff(db, c);
    var st := FoldGroups(Q1Aggs(), Q1GroupKeys(), p, filtered, map[]);
    var m := R.FoldByGroup(filtered);
    var raw := StoreToRows(st);

    FilterNonempty_StoreToRows(st, m);
    StoreToRows_Q1_sorted(st, m);
    G_StoreRel_key_bounds(st, m);
    forall row | row in raw
      ensures |row.key| == 2
    {
      StoreToRows_in(st, row);
      D_Domain_forward(st, m, row.key);
    }
    ResultRowSort_id_on_sorted(raw);

    assert LogicalSpec_Q1.filters == Q1Filters();
    assert LogicalSpec_Q1.group_keys == Q1GroupKeys();
    assert LogicalSpec_Q1.aggs == Q1Aggs();
    assert LogicalSpec_Q1.order_keys == Q1OrderKeys();

    reveal Eval_Spec();
    assert FilterRows(LogicalSpec_Q1.filters, p, db) == filtered;
    assert FoldGroups(LogicalSpec_Q1.aggs, LogicalSpec_Q1.group_keys, p, filtered, map[]) == st;
    assert FilterNonempty(StoreToRows(st)) == raw;
    // key-arity guard holds: every StoreToRows key has |key|==|group_keys|==2
    assert forall row :: row in raw ==> |row.key| == |LogicalSpec_Q1.group_keys|;
    assert ResultRowSort(LogicalSpec_Q1.order_keys, LogicalSpec_Q1.group_keys, raw) == raw;
    assert Eval_Spec(LogicalSpec_Q1, p, db) == raw;
  }

  lemma Eval_Spec_Q1_OutRows_eq_Adapt(c: int, db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures var p := ParamsOfCutoff(c);
            Eval_Spec_Q1_OutRows(p, db) == Q1Adapt(Eval_Spec(LogicalSpec_Q1, p, db))
  {
    reveal Eval_Spec_Q1_OutRows();
  }

  // Migration: generic Eval_Spec ∘ Q1Adapt == EvalResultSpec.
  // Body only composes A–H / filter / reveal lemmas — no large recursive VCs.
  lemma Eval_Spec_Q1_eq_EvalResultSpec(c: int, db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures Eval_Spec_Q1_OutRows(ParamsOfCutoff(c), db) == R.EvalResultSpec(db, c)
  {
    var p := ParamsOfCutoff(c);

    // 1. filter + fold correspondence
    Eval_Spec_Q1_fold_rel(c, db);
    var filtered := R.FilterCutoff(db, c);
    var st := FoldGroups(Q1Aggs(), Q1GroupKeys(), p, filtered, map[]);
    var m := R.FoldByGroup(filtered);
    assert StoreRel(st, m);
    assert m == R.SpecView(db, c);

    // 2. Eval_Spec reveals to StoreToRows(st)
    Eval_Spec_Q1_eq_StoreToRows(c, db);
    var raw := StoreToRows(st);
    assert Eval_Spec(LogicalSpec_Q1, p, db) == raw;

    // 3. OutRows = Adapt(raw)
    Eval_Spec_Q1_OutRows_eq_Adapt(c, db);
    assert Eval_Spec_Q1_OutRows(p, db) == Q1Adapt(raw);

    // 4. Adapt = FinalizeKeys(KeysFrom(...)) via G+H
    H_Adapt_eq_Finalize(st, m);
    assert Q1Adapt(raw) == R.FinalizeKeys(R.KeysFrom(0, 65536, m), m);

    // 5. RHS definition
    assert R.EvalResultSpec(db, c)
        == R.FinalizeKeys(R.KeysFrom(0, 65536, m), m);
  }

  lemma Eval_Spec_Q1_catalog(db: seq<R.Row>)
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
    ensures Eval_Spec_Q1_OutRows(CatalogParams, db)
         == R.EvalResultSpec(db, CatalogCutoff)
  {
    Eval_Spec_Q1_eq_EvalResultSpec(CatalogCutoff, db);
  }
}
