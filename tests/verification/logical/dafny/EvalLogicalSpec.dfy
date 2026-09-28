// Generic RelNode Eval_Spec — dual of formalsql/logical/Eval.v.
// No Q1-name branch. Presence ≠ semantic equivalence (cross-prover TCB).
// Module name RelEvalLogicalSpec (not EvalLogicalSpec) so q1 flat scaffolding
// can share a compilation unit without colliding with q1/dafny/EvalLogicalSpec.

include "LogicalSpec.dfy"

module RelEvalLogicalSpec {

  import opened LogicalSpec

  datatype Truth3 = TTrue | TFalse | TUnknown
  datatype OptVal = NoVal | SomeVal(v: SqlValue)
  datatype OptVals = NoVals | SomeVals(vs: seq<SqlValue>)
  datatype OptRows = NoRows | SomeRows(rs: seq<Row>)
  datatype OptBool = NoBool | SomeBool(b: bool)
  datatype AccSlot = AccSlot(sum: int, count: int, init: bool)
  type AggAcc = seq<AccSlot>
  datatype OptAcc = NoAcc | SomeAcc(acc: AggAcc)
  datatype OptSlot = NoSlot | SomeSlot(sl: AccSlot)
  // List association store — dual of Rocq Eval.GroupStore (list (keys * Acc)).
  // Unique-key semantics via StorePut; not a physical hash map.
  datatype GroupEntry = GroupEntry(k: seq<SqlValue>, a: AggAcc)
  type GroupStore = seq<GroupEntry>
  datatype OptStore = NoStore | SomeStore(st: GroupStore)
  datatype AggBag = AggBag(keys: seq<SqlValue>, vals: seq<SqlValue>)
  datatype OptBags = NoBags | SomeBags(bags: seq<AggBag>)
  datatype RelVal = RVRows(rs: seq<Row>) | RVAgg(bags: seq<AggBag>)
  datatype OptRel = NoRel | SomeRel(v: RelVal)
  datatype EvalCtx = EvRow(r: Row) | EvAgg(keys: seq<SqlValue>, vals: seq<SqlValue>)

  ghost function SqlValueEq(a: SqlValue, b: SqlValue): bool
  {
    if a.VNull? && b.VNull? then true
    else if a.VInt? && b.VInt? then a.z == b.z
    else if a.VBool? && b.VBool? then a.b == b.b
    else if a.VText? && b.VText? then a.s == b.s
    else if a.VAvgExact? && b.VAvgExact? then a.sum == b.sum && a.count == b.count
    else false
  }

  ghost function ListEq(a: seq<SqlValue>, b: seq<SqlValue>): bool
    decreases |a| + |b|
  {
    if |a| == 0 && |b| == 0 then true
    else if |a| == 0 || |b| == 0 then false
    else SqlValueEq(a[0], b[0]) && ListEq(a[1..], b[1..])
  }

  ghost function ParamsLookup(p: Params, id: string): OptVal
    decreases |p|
  {
    if |p| == 0 then NoVal
    else if p[0].0 == id then SomeVal(p[0].1)
    else ParamsLookup(p[1..], id)
  }

  ghost function DbLookup(db: DB, t: TableId): OptRows
    decreases |db|
  {
    if |db| == 0 then NoRows
    else if db[0].0 == t then SomeRows(db[0].1)
    else DbLookup(db[1..], t)
  }

  ghost function RowNth(r: seq<SqlValue>, i: nat): OptVal
  {
    if i < |r| then SomeVal(r[i]) else NoVal
  }

  ghost function CtxLookupInput(ctx: EvalCtx, i: nat): OptVal
  {
    match ctx
    case EvRow(r) => RowNth(r, i)
    case EvAgg(keys, vals) => RowNth(keys + vals, i)
  }

  ghost function CmpIntEq(a: SqlValue, b: SqlValue): Truth3
  {
    if a.VNull? || b.VNull? then TUnknown
    else if a.VInt? && b.VInt? then (if a.z == b.z then TTrue else TFalse)
    else if a.VBool? && b.VBool? then (if a.b == b.b then TTrue else TFalse)
    else if a.VText? && b.VText? then (if a.s == b.s then TTrue else TFalse)
    else TUnknown
  }

  ghost function CmpIntLeb(a: SqlValue, b: SqlValue): Truth3
  {
    if a.VNull? || b.VNull? then TUnknown
    else if a.VInt? && b.VInt? then (if a.z <= b.z then TTrue else TFalse)
    else TUnknown
  }

  ghost function Truth3Not(t: Truth3): Truth3
  {
    if t == TTrue then TFalse else if t == TFalse then TTrue else TUnknown
  }

  ghost function Truth3And(a: Truth3, b: Truth3): Truth3
  {
    if a == TFalse || b == TFalse then TFalse
    else if a == TUnknown || b == TUnknown then TUnknown
    else TTrue
  }

  ghost function CmpTruth(op: BinaryOp, a: SqlValue, b: SqlValue): Truth3
  {
    if op == BEq then CmpIntEq(a, b)
    else if op == BNe then Truth3Not(CmpIntEq(a, b))
    else if op == BLe then CmpIntLeb(a, b)
    else if op == BLt then Truth3And(CmpIntLeb(a, b), Truth3Not(CmpIntEq(a, b)))
    else if op == BGe then CmpIntLeb(b, a)
    else if op == BGt then Truth3And(CmpIntLeb(b, a), Truth3Not(CmpIntEq(a, b)))
    else TUnknown
  }

  ghost function Arith(op: BinaryOp, x: int, y: int): OptVal
  {
    if op == BAdd then SomeVal(VInt(x + y))
    else if op == BSub then SomeVal(VInt(x - y))
    else if op == BMul then SomeVal(VInt(x * y))
    else if op == BDiv then (if y == 0 then NoVal else SomeVal(VInt(x / y)))
    else NoVal
  }

  ghost function TextPrefix(hay: string, pref: string): bool
    decreases |pref|
  {
    if |pref| == 0 then true
    else if |hay| == 0 then false
    else if hay[0] == pref[0] then TextPrefix(hay[1..], pref[1..])
    else false
  }

  ghost function EvalExpr(fuel: nat, e: Expr, p: Params, ctx: EvalCtx): OptVal
    decreases fuel, 0
  {
    if fuel == 0 then NoVal
    else if e.EInputRef? then CtxLookupInput(ctx, e.slot)
    else if e.EConst? then SomeVal(e.v)
    else if e.EParam? then ParamsLookup(p, e.id)
    else if e.EUnary? then EvalUnary(e.uop, EvalExpr(fuel - 1, e.e, p, ctx))
    else if e.EBinary? then
      EvalBinary(e.bop, EvalExpr(fuel - 1, e.lhs, p, ctx), EvalExpr(fuel - 1, e.rhs, p, ctx))
    else if e.ECase? then EvalCase(fuel - 1, e.branches, e.elseExpr, p, ctx)
    else if e.ECast? then EvalExpr(fuel - 1, e.e, p, ctx)
    else if e.EGroupRef? then
      if ctx.EvAgg? then RowNth(ctx.keys, e.i) else RowNth(ctx.r, e.i)
    else if e.EAggRef? then
      if ctx.EvAgg? then RowNth(ctx.vals, e.i) else RowNth(ctx.r, e.i)
    else NoVal
  }

  ghost function EvalCase(fuel: nat, brs: seq<(Expr, Expr)>, el: Expr, p: Params, ctx: EvalCtx): OptVal
    decreases fuel, 1, |brs|
  {
    if fuel == 0 then NoVal
    else if |brs| == 0 then EvalExpr(fuel, el, p, ctx)
    else
      var c := EvalExpr(fuel, brs[0].0, p, ctx);
      if c.SomeVal? && c.v.VBool? && c.v.b then EvalExpr(fuel, brs[0].1, p, ctx)
      else if c.SomeVal? && (c.v.VBool? || c.v.VNull?) then EvalCase(fuel, brs[1..], el, p, ctx)
      else NoVal
  }

  ghost function EvalUnary(uop: UnaryOp, ov: OptVal): OptVal
  {
    if ov.NoVal? then NoVal
    else if uop == UNeg then
      if ov.v.VInt? then SomeVal(VInt(-ov.v.z)) else NoVal
    else if uop == UNot then
      if ov.v.VBool? then SomeVal(VBool(!ov.v.b))
      else if ov.v.VNull? then SomeVal(VNull)
      else NoVal
    else if uop == UIsNull then SomeVal(VBool(ov.v.VNull?))
    else SomeVal(VBool(!ov.v.VNull?))
  }

  ghost function EvalBinary(bop: BinaryOp, la: OptVal, rb: OptVal): OptVal
  {
    if la.NoVal? || rb.NoVal? then NoVal
    else if bop == BAdd || bop == BSub || bop == BMul || bop == BDiv then
      if la.v.VNull? || rb.v.VNull? then SomeVal(VNull)
      else if la.v.VInt? && rb.v.VInt? then Arith(bop, la.v.z, rb.v.z)
      else NoVal
    else if bop == BAnd then
      if la.v.VBool? && rb.v.VBool? then SomeVal(VBool(la.v.b && rb.v.b))
      else if la.v.VNull? || rb.v.VNull? then SomeVal(VNull)
      else NoVal
    else if bop == BOr then
      if la.v.VBool? && rb.v.VBool? then SomeVal(VBool(la.v.b || rb.v.b))
      else if la.v.VNull? || rb.v.VNull? then SomeVal(VNull)
      else NoVal
    else if bop == BPrefixMatch || bop == BLike then
      if la.v.VNull? || rb.v.VNull? then SomeVal(VNull)
      else if la.v.VText? && rb.v.VText? then SomeVal(VBool(TextPrefix(la.v.s, rb.v.s)))
      else NoVal
    else
      var t := CmpTruth(bop, la.v, rb.v);
      if t == TTrue then SomeVal(VBool(true))
      else if t == TFalse then SomeVal(VBool(false))
      else SomeVal(VNull)
  }

  const EvalFuel: nat := 1024

  ghost function EvalPredKeep(pr: Pred, p: Params, r: Row): OptBool
  {
    var v := EvalExpr(EvalFuel, pr, p, EvRow(r));
    if v.SomeVal? && v.v.VBool? && v.v.b then SomeBool(true)
    else if v.SomeVal? && v.v.VBool? then SomeBool(false)
    else if v.SomeVal? && v.v.VNull? then SomeBool(false)
    else NoBool
  }

  ghost function FilterRows(pr: Pred, p: Params, db: seq<Row>): OptRows
    decreases |db|
  {
    if |db| == 0 then SomeRows([])
    else
      var rest := FilterRows(pr, p, db[1..]);
      var keep := EvalPredKeep(pr, p, db[0]);
      if keep.NoBool? || rest.NoRows? then NoRows
      else if keep.b then SomeRows([db[0]] + rest.rs)
      else SomeRows(rest.rs)
  }

  ghost function EvalExprs(es: seq<Expr>, p: Params, ctx: EvalCtx): OptVals
    decreases |es|
  {
    if |es| == 0 then SomeVals([])
    else
      var head := EvalExpr(EvalFuel, es[0], p, ctx);
      var tail := EvalExprs(es[1..], p, ctx);
      if head.NoVal? || tail.NoVals? then NoVals
      else SomeVals([head.v] + tail.vs)
  }

  ghost function ProjectRows(es: seq<Expr>, p: Params, db: seq<Row>): OptRows
    decreases |db|
  {
    if |db| == 0 then SomeRows([])
    else
      var head := EvalExprs(es, p, EvRow(db[0]));
      var tail := ProjectRows(es, p, db[1..]);
      if head.NoVals? || tail.NoRows? then NoRows
      else SomeRows([head.vs] + tail.rs)
  }

  ghost function ProjectAggRows(es: seq<Expr>, p: Params, bags: seq<AggBag>): OptRows
    decreases |bags|
  {
    if |bags| == 0 then SomeRows([])
    else
      var head := EvalExprs(es, p, EvAgg(bags[0].keys, bags[0].vals));
      var tail := ProjectAggRows(es, p, bags[1..]);
      if head.NoVals? || tail.NoRows? then NoRows
      else SomeRows([head.vs] + tail.rs)
  }

  ghost function JoinAgainst(cond: Pred, p: Params, lrow: Row, right: seq<Row>): OptRows
    decreases |right|
  {
    if |right| == 0 then SomeRows([])
    else
      var combined := lrow + right[0];
      var rest := JoinAgainst(cond, p, lrow, right[1..]);
      var keep := EvalPredKeep(cond, p, combined);
      if keep.NoBool? || rest.NoRows? then NoRows
      else if keep.b then SomeRows([combined] + rest.rs)
      else SomeRows(rest.rs)
  }

  ghost function JoinInnerRows(cond: Pred, p: Params, left: seq<Row>, right: seq<Row>): OptRows
    decreases |left|
  {
    if |left| == 0 then SomeRows([])
    else
      var here := JoinAgainst(cond, p, left[0], right);
      var later := JoinInnerRows(cond, p, left[1..], right);
      if here.NoRows? || later.NoRows? then NoRows
      else SomeRows(here.rs + later.rs)
  }

  ghost function EmptyAcc(n: nat): AggAcc
    decreases n
  {
    if n == 0 then [] else [AccSlot(0, 0, false)] + EmptyAcc(n - 1)
  }

  ghost function AccAddSlot(kind: AggKind, slot: AccSlot, v: OptVal): OptSlot
  {
    if kind == AggCountStar then
      SomeSlot(AccSlot(slot.sum + 1, slot.count + 1, true))
    else if kind == AggCount then
      if v.NoVal? then NoSlot
      else if v.v.VNull? then SomeSlot(slot)
      else SomeSlot(AccSlot(slot.sum + 1, slot.count + 1, true))
    else if kind == AggSum || kind == AggAvg then
      if v.NoVal? then NoSlot
      else if v.v.VNull? then SomeSlot(slot)
      else if v.v.VInt? then SomeSlot(AccSlot(slot.sum + v.v.z, slot.count + 1, true))
      else NoSlot
    else if kind == AggMin then
      if v.NoVal? then NoSlot
      else if v.v.VNull? then SomeSlot(slot)
      else if !v.v.VInt? then NoSlot
      else if !slot.init then SomeSlot(AccSlot(v.v.z, 1, true))
      else if v.v.z < slot.sum then SomeSlot(AccSlot(v.v.z, slot.count + 1, true))
      else SomeSlot(AccSlot(slot.sum, slot.count + 1, true))
    else // AggMax
      if v.NoVal? then NoSlot
      else if v.v.VNull? then SomeSlot(slot)
      else if !v.v.VInt? then NoSlot
      else if !slot.init then SomeSlot(AccSlot(v.v.z, 1, true))
      else if slot.sum < v.v.z then SomeSlot(AccSlot(v.v.z, slot.count + 1, true))
      else SomeSlot(AccSlot(slot.sum, slot.count + 1, true))
  }

  ghost function AggContribOne(a: AggSpec, p: Params, r: Row): OptVal
  {
    if a.kind == AggCountStar then SomeVal(VInt(1))
    else if a.expr.SomeExpr? then EvalExpr(EvalFuel, a.expr.e, p, EvRow(r))
    else NoVal
  }

  ghost function AggsContrib(aggs: seq<AggSpec>, p: Params, r: Row): OptVals
    decreases |aggs|
  {
    // reuse OptVals to carry seq of values (contribs are non-option once SomeVal)
    if |aggs| == 0 then SomeVals([])
    else
      var one := AggContribOne(aggs[0], p, r);
      var rest := AggsContrib(aggs[1..], p, r);
      if one.NoVal? || rest.NoVals? then NoVals
      else SomeVals([one.v] + rest.vs)
  }

  ghost function AccAddOne(aggs: seq<AggSpec>, acc: AggAcc, vals: seq<SqlValue>): OptAcc
    decreases |aggs|
  {
    if |aggs| == 0 && |acc| == 0 && |vals| == 0 then SomeAcc([])
    else if |aggs| > 0 && |acc| > 0 && |vals| > 0 then
      var sl := AccAddSlot(aggs[0].kind, acc[0], SomeVal(vals[0]));
      var rest := AccAddOne(aggs[1..], acc[1..], vals[1..]);
      if sl.NoSlot? || rest.NoAcc? then NoAcc
      else SomeAcc([sl.sl] + rest.acc)
    else NoAcc
  }

  ghost function AccAddRow(aggs: seq<AggSpec>, p: Params, r: Row, acc: AggAcc): OptAcc
  {
    var vals := AggsContrib(aggs, p, r);
    if vals.NoVals? then NoAcc else AccAddOne(aggs, acc, vals.vs)
  }

  // ----- List GroupStore (Rocq dual of Eval.GroupStore) -----

  ghost predicate StoreKeysUnique(st: GroupStore)
  {
    forall i, j :: 0 <= i < j < |st| ==> !ListEq(st[i].k, st[j].k)
  }

  // Dual of Rocq store_find / store_get.
  ghost function StoreGet(st: GroupStore, k: seq<SqlValue>): OptAcc
    decreases |st|
  {
    if |st| == 0 then NoAcc
    else if ListEq(st[0].k, k) then SomeAcc(st[0].a)
    else StoreGet(st[1..], k)
  }

  // Dual of Rocq store_update / store_put.
  ghost function StorePut(st: GroupStore, k: seq<SqlValue>, a: AggAcc): GroupStore
    decreases |st|
  {
    if |st| == 0 then [GroupEntry(k, a)]
    else if ListEq(st[0].k, k) then [GroupEntry(k, a)] + st[1..]
    else [st[0]] + StorePut(st[1..], k, a)
  }

  ghost function StoreAddRow(aggs: seq<AggSpec>, gkeys: seq<Expr>, p: Params, r: Row, st: GroupStore): OptStore
  {
    var keys := EvalExprs(gkeys, p, EvRow(r));
    if keys.NoVals? then NoStore
    else
      var found := StoreGet(st, keys.vs);
      var acc0 := if found.SomeAcc? then found.acc else EmptyAcc(|aggs|);
      var acc1 := AccAddRow(aggs, p, r, acc0);
      if acc1.NoAcc? then NoStore else SomeStore(StorePut(st, keys.vs, acc1.acc))
  }

  ghost function FoldGroups(aggs: seq<AggSpec>, gkeys: seq<Expr>, p: Params, rs: seq<Row>, st: GroupStore): OptStore
    decreases |rs|
  {
    if |rs| == 0 then SomeStore(st)
    else
      var st1 := StoreAddRow(aggs, gkeys, p, rs[0], st);
      if st1.NoStore? then NoStore else FoldGroups(aggs, gkeys, p, rs[1..], st1.st)
  }

  ghost function FinalizeSlot(kind: AggKind, sl: AccSlot): SqlValue
  {
    if kind == AggCountStar || kind == AggCount then VInt(sl.count)
    else if kind == AggSum then (if sl.init then VInt(sl.sum) else VNull)
    else if kind == AggAvg then (if sl.count == 0 then VNull else VAvgExact(sl.sum, sl.count))
    else (if sl.init then VInt(sl.sum) else VNull)
  }

  ghost function FinalizeAcc(aggs: seq<AggSpec>, acc: AggAcc): OptVals
    decreases |aggs|
  {
    if |aggs| == 0 && |acc| == 0 then SomeVals([])
    else if |aggs| > 0 && |acc| > 0 then
      var rest := FinalizeAcc(aggs[1..], acc[1..]);
      if rest.NoVals? then NoVals
      else SomeVals([FinalizeSlot(aggs[0].kind, acc[0])] + rest.vs)
    else NoVals
  }

  ghost predicate AccNonempty(acc: AggAcc)
  {
    exists i :: 0 <= i < |acc| && (acc[i].init || acc[i].count != 0)
  }

  ghost function AggBagsToRows(bags: seq<AggBag>): seq<Row>
    decreases |bags|
  {
    if |bags| == 0 then [] else [bags[0].keys + bags[0].vals] + AggBagsToRows(bags[1..])
  }

  // Emit bags in store order (first-seen key order) — dual of Rocq store_to_agg_bags.
  ghost function StoreToAggBags(aggs: seq<AggSpec>, st: GroupStore): OptBags
    decreases |st|
  {
    if |st| == 0 then SomeBags([])
    else if !AccNonempty(st[0].a) then StoreToAggBags(aggs, st[1..])
    else
      var vals := FinalizeAcc(aggs, st[0].a);
      var rest := StoreToAggBags(aggs, st[1..]);
      if vals.NoVals? || rest.NoBags? then NoBags
      else SomeBags([AggBag(st[0].k, vals.vs)] + rest.bags)
  }

  // Dual of Rocq agg_bags_to_rows (store emission path).
  ghost function StoreToRows(aggs: seq<AggSpec>, st: GroupStore): OptRows
  {
    var bags := StoreToAggBags(aggs, st);
    if bags.NoBags? then NoRows else SomeRows(AggBagsToRows(bags.bags))
  }

  ghost function RunAggregate(aggs: seq<AggSpec>, gkeys: seq<Expr>, p: Params, rs: seq<Row>): OptBags
  {
    var st := FoldGroups(aggs, gkeys, p, rs, []);
    if st.NoStore? then NoBags
    else if |gkeys| == 0 && |st.st| == 0 then
      var vals := FinalizeAcc(aggs, EmptyAcc(|aggs|));
      if vals.NoVals? then NoBags else SomeBags([AggBag([], vals.vs)])
    else
      StoreToAggBags(aggs, st.st)
  }

  lemma ListEq_refl(a: seq<SqlValue>)
    ensures ListEq(a, a)
    decreases |a|
  {
    if |a| == 0 {
    } else {
      ListEq_refl(a[1..]);
    }
  }

  lemma ListEq_sym(a: seq<SqlValue>, b: seq<SqlValue>)
    requires ListEq(a, b)
    ensures ListEq(b, a)
    decreases |a| + |b|
  {
    if |a| == 0 {
    } else {
      ListEq_sym(a[1..], b[1..]);
    }
  }

  lemma ListEq_trans(a: seq<SqlValue>, b: seq<SqlValue>, c: seq<SqlValue>)
    requires ListEq(a, b) && ListEq(b, c)
    ensures ListEq(a, c)
    decreases |a| + |b| + |c|
  {
    if |a| == 0 {
    } else {
      ListEq_trans(a[1..], b[1..], c[1..]);
    }
  }

  lemma StoreGet_Put_hit(st: GroupStore, k: seq<SqlValue>, a: AggAcc)
    ensures StoreGet(StorePut(st, k, a), k) == SomeAcc(a)
    decreases |st|
  {
    if |st| == 0 {
      ListEq_refl(k);
    } else if ListEq(st[0].k, k) {
      ListEq_refl(k);
    } else {
      StoreGet_Put_hit(st[1..], k, a);
    }
  }

  lemma StorePut_length_bound(st: GroupStore, k: seq<SqlValue>, a: AggAcc)
    ensures |StorePut(st, k, a)| <= |st| + 1
    decreases |st|
  {
    if |st| == 0 {
    } else if ListEq(st[0].k, k) {
    } else {
      StorePut_length_bound(st[1..], k, a);
    }
  }

  lemma StoreKeysUnique_tail(st: GroupStore)
    requires |st| > 0
    requires StoreKeysUnique(st)
    ensures StoreKeysUnique(st[1..])
  {
  }

  lemma StorePut_keys_are_old_or_k(
      st: GroupStore, k: seq<SqlValue>, a: AggAcc, idx: nat)
    requires idx < |StorePut(st, k, a)|
    ensures ListEq(StorePut(st, k, a)[idx].k, k) ||
            (exists i :: 0 <= i < |st| && ListEq(StorePut(st, k, a)[idx].k, st[i].k))
    decreases |st|
  {
    if |st| == 0 {
      ListEq_refl(k);
    } else if ListEq(st[0].k, k) {
      if idx == 0 {
        ListEq_refl(k);
      } else {
        ListEq_refl(st[idx].k);
        assert ListEq(StorePut(st, k, a)[idx].k, st[idx].k);
      }
    } else {
      if idx == 0 {
        ListEq_refl(st[0].k);
        assert ListEq(StorePut(st, k, a)[0].k, st[0].k);
      } else {
        StorePut_keys_are_old_or_k(st[1..], k, a, idx - 1);
        if ListEq(StorePut(st, k, a)[idx].k, k) {
        } else {
          var i :| 0 <= i < |st[1..]| &&
            ListEq(StorePut(st[1..], k, a)[idx - 1].k, st[1..][i].k);
          assert ListEq(StorePut(st, k, a)[idx].k, st[i + 1].k);
        }
      }
    }
  }

  lemma StorePut_preserves_unique(st: GroupStore, k: seq<SqlValue>, a: AggAcc)
    requires StoreKeysUnique(st)
    ensures StoreKeysUnique(StorePut(st, k, a))
    decreases |st|
  {
    if |st| == 0 {
    } else if ListEq(st[0].k, k) {
      StoreKeysUnique_tail(st);
      assert StoreKeysUnique([GroupEntry(k, a)] + st[1..]) by {
        forall i, j | 0 <= i < j < |[GroupEntry(k, a)] + st[1..]|
          ensures !ListEq(([GroupEntry(k, a)] + st[1..])[i].k,
                          ([GroupEntry(k, a)] + st[1..])[j].k)
        {
          if i == 0 {
            assert !ListEq(st[0].k, st[j].k);
            ListEq_sym(st[0].k, k);
            // ([GroupEntry(k,a)]+st[1..])[0].k == k, [j].k == st[j].k
            // ListEq(k, st[j].k) && ListEq(st[0].k, k) => ListEq(st[0].k, st[j].k)
            if ListEq(k, st[j].k) {
              ListEq_trans(st[0].k, k, st[j].k);
              assert false;
            }
          }
        }
      }
    } else {
      StoreKeysUnique_tail(st);
      StorePut_preserves_unique(st[1..], k, a);
      var st' := StorePut(st[1..], k, a);
      assert StoreKeysUnique(st');
      assert StoreKeysUnique([st[0]] + st') by {
        forall i, j | 0 <= i < j < |[st[0]] + st'|
          ensures !ListEq(([st[0]] + st')[i].k, ([st[0]] + st')[j].k)
        {
          if i == 0 {
            StorePut_keys_are_old_or_k(st[1..], k, a, j - 1);
            if ListEq(st'[j - 1].k, k) {
              if ListEq(st[0].k, st'[j - 1].k) {
                ListEq_trans(st[0].k, st'[j - 1].k, k);
                assert false;
              }
            } else {
              var t :| 0 <= t < |st[1..]| && ListEq(st'[j - 1].k, st[1..][t].k);
              if ListEq(st[0].k, st'[j - 1].k) {
                ListEq_sym(st'[j - 1].k, st[1..][t].k);
                ListEq_trans(st[0].k, st'[j - 1].k, st[t + 1].k);
                assert !ListEq(st[0].k, st[t + 1].k);
                assert false;
              }
            }
          }
        }
      }
    }
  }

  lemma StoreAddRow_preserves_unique(
      aggs: seq<AggSpec>, gkeys: seq<Expr>, p: Params, r: Row, st: GroupStore)
    requires StoreKeysUnique(st)
    ensures var st1 := StoreAddRow(aggs, gkeys, p, r, st);
            st1.SomeStore? ==> StoreKeysUnique(st1.st)
  {
    var keys := EvalExprs(gkeys, p, EvRow(r));
    if keys.SomeVals? {
      var found := StoreGet(st, keys.vs);
      var acc0 := if found.SomeAcc? then found.acc else EmptyAcc(|aggs|);
      var acc1 := AccAddRow(aggs, p, r, acc0);
      if acc1.SomeAcc? {
        StorePut_preserves_unique(st, keys.vs, acc1.acc);
      }
    }
  }

  lemma FoldGroups_preserves_unique(
      aggs: seq<AggSpec>, gkeys: seq<Expr>, p: Params, rs: seq<Row>, st: GroupStore)
    requires StoreKeysUnique(st)
    ensures var st1 := FoldGroups(aggs, gkeys, p, rs, st);
            st1.SomeStore? ==> StoreKeysUnique(st1.st)
    decreases |rs|
  {
    if |rs| == 0 {
    } else {
      StoreAddRow_preserves_unique(aggs, gkeys, p, rs[0], st);
      var st1 := StoreAddRow(aggs, gkeys, p, rs[0], st);
      if st1.SomeStore? {
        FoldGroups_preserves_unique(aggs, gkeys, p, rs[1..], st1.st);
      }
    }
  }

  lemma FoldGroups_from_empty_unique(
      aggs: seq<AggSpec>, gkeys: seq<Expr>, p: Params, rs: seq<Row>)
    ensures var st1 := FoldGroups(aggs, gkeys, p, rs, []);
            st1.SomeStore? ==> StoreKeysUnique(st1.st)
  {
    FoldGroups_preserves_unique(aggs, gkeys, p, rs, []);
  }

  ghost function ValueLe(nulls: NullOrder, a: SqlValue, b: SqlValue): OptBool
  {
    if a.VNull? && b.VNull? then SomeBool(true)
    else if a.VNull? then SomeBool(nulls == NullsFirst)
    else if b.VNull? then SomeBool(nulls == NullsLast)
    else if a.VInt? && b.VInt? then SomeBool(a.z <= b.z)
    else NoBool
  }

  ghost function OrderKeyLe(p: Params, k: OrderKey, ra: Row, rb: Row): OptBool
  {
    var va := EvalExpr(EvalFuel, k.expr, p, EvRow(ra));
    var vb := EvalExpr(EvalFuel, k.expr, p, EvRow(rb));
    if va.NoVal? || vb.NoVal? then NoBool
    else
      var lea := ValueLe(k.nulls, va.v, vb.v);
      var leb := ValueLe(k.nulls, vb.v, va.v);
      if lea.NoBool? || leb.NoBool? then NoBool
      else if k.dir == SortAsc then SomeBool(lea.b) else SomeBool(leb.b)
  }

  ghost function OrderKeysLe(p: Params, oks: seq<OrderKey>, ra: Row, rb: Row): OptBool
    decreases |oks|
  {
    if |oks| == 0 then SomeBool(true)
    else
      var ab := OrderKeyLe(p, oks[0], ra, rb);
      var ba := OrderKeyLe(p, oks[0], rb, ra);
      if ab.NoBool? || ba.NoBool? then NoBool
      else if ab.b && ba.b then OrderKeysLe(p, oks[1..], ra, rb)
      else if ab.b then SomeBool(true)
      else if ba.b then SomeBool(false)
      else NoBool
  }

  ghost function RowInsert(p: Params, oks: seq<OrderKey>, x: Row, xs: seq<Row>): OptRows
    decreases |xs|
  {
    if |xs| == 0 then SomeRows([x])
    else
      var le := OrderKeysLe(p, oks, x, xs[0]);
      if le.NoBool? then NoRows
      else if le.b then SomeRows([x] + xs)
      else
        var ys := RowInsert(p, oks, x, xs[1..]);
        if ys.NoRows? then NoRows else SomeRows([xs[0]] + ys.rs)
  }

  ghost function ResultRowSort(p: Params, oks: seq<OrderKey>, rs: seq<Row>): OptRows
    decreases |rs|
  {
    if |rs| == 0 then SomeRows([])
    else
      var xs := ResultRowSort(p, oks, rs[1..]);
      if xs.NoRows? then NoRows else RowInsert(p, oks, rs[0], xs.rs)
  }

  ghost function RowIn(r: Row, rs: seq<Row>): bool
    decreases |rs|
  {
    if |rs| == 0 then false else if ListEq(r, rs[0]) then true else RowIn(r, rs[1..])
  }

  ghost function DistinctRows(rs: seq<Row>): seq<Row>
    decreases |rs|
  {
    if |rs| == 0 then []
    else
      var rest := DistinctRows(rs[1..]);
      if RowIn(rs[0], rest) then rest else [rs[0]] + rest
  }

  ghost function SkipN(n: nat, rs: seq<Row>): seq<Row>
    decreases n
  {
    if n == 0 || |rs| == 0 then rs else SkipN(n - 1, rs[1..])
  }

  ghost function TakeN(n: nat, rs: seq<Row>): seq<Row>
    decreases n
  {
    if n == 0 || |rs| == 0 then [] else [rs[0]] + TakeN(n - 1, rs[1..])
  }

  ghost function EvalRel(n: RelNode, p: Params, db: DB): OptRel
    decreases n
  {
    if n.RelScan? then
      var t := DbLookup(db, n.table);
      if t.SomeRows? then SomeRel(RVRows(t.rs)) else NoRel
    else if n.RelFilter? then
      var inn := EvalRel(n.input, p, db);
      if !(inn.SomeRel? && inn.v.RVRows?) then NoRel
      else
        var f := FilterRows(n.pred, p, inn.v.rs);
        if f.SomeRows? then SomeRel(RVRows(f.rs)) else NoRel
    else if n.RelProject? then
      var inn := EvalRel(n.input, p, db);
      if inn.SomeRel? && inn.v.RVRows? then
        var pr := ProjectRows(n.exprs, p, inn.v.rs);
        if pr.SomeRows? then SomeRel(RVRows(pr.rs)) else NoRel
      else if inn.SomeRel? && inn.v.RVAgg? then
        var pr := ProjectAggRows(n.exprs, p, inn.v.bags);
        if pr.SomeRows? then SomeRel(RVRows(pr.rs)) else NoRel
      else NoRel
    else if n.RelJoin? then
      if n.kind == JoinInner || n.kind == JoinCross then
        var L := EvalRel(n.lft, p, db);
        var R := EvalRel(n.rgt, p, db);
        if !(L.SomeRel? && L.v.RVRows? && R.SomeRel? && R.v.RVRows?) then NoRel
        else
          var cond := if n.kind == JoinCross then EConst(VBool(true), TBool) else n.cond;
          var j := JoinInnerRows(cond, p, L.v.rs, R.v.rs);
          if j.SomeRows? then SomeRel(RVRows(j.rs)) else NoRel
      else NoRel
    else if n.RelAggregate? then
      var inn := EvalRel(n.input, p, db);
      if !(inn.SomeRel? && inn.v.RVRows?) then NoRel
      else
        var bags := RunAggregate(n.aggs, n.groupExprs, p, inn.v.rs);
        if bags.SomeBags? then SomeRel(RVAgg(bags.bags)) else NoRel
    else if n.RelSort? then
      var inn := EvalRel(n.input, p, db);
      if inn.SomeRel? && inn.v.RVRows? then
        var s := ResultRowSort(p, n.orderKeys, inn.v.rs);
        if s.SomeRows? then SomeRel(RVRows(s.rs)) else NoRel
      else if inn.SomeRel? && inn.v.RVAgg? then
        var s := ResultRowSort(p, n.orderKeys, AggBagsToRows(inn.v.bags));
        if s.SomeRows? then SomeRel(RVRows(s.rs)) else NoRel
      else NoRel
    else if n.RelDistinct? then
      var inn := EvalRel(n.input, p, db);
      if inn.SomeRel? && inn.v.RVRows? then SomeRel(RVRows(DistinctRows(inn.v.rs))) else NoRel
    else if n.RelLimit? then
      var inn := EvalRel(n.input, p, db);
      if inn.SomeRel? && inn.v.RVRows? then
        SomeRel(RVRows(TakeN(n.count, SkipN(n.offset, inn.v.rs))))
      else NoRel
    else NoRel
  }

  ghost function {:opaque} Eval_Spec(s: LogicalSpec, p: Params, db: DB): OptRows
  {
    var v := EvalRel(s, p, db);
    if v.SomeRel? && v.v.RVRows? then SomeRows(v.v.rs)
    else if v.SomeRel? && v.v.RVAgg? then SomeRows(AggBagsToRows(v.v.bags))
    else NoRows
  }

  // ----- Generic bag / permutation helpers (operator proofs; not GroupStore dual) -----

  ghost predicate IsPermutation<T>(a: seq<T>, b: seq<T>)
  {
    multiset(a) == multiset(b)
  }

  lemma Permutation_refl<T>(a: seq<T>)
    ensures IsPermutation(a, a)
  {
  }

  lemma Permutation_sym<T>(a: seq<T>, b: seq<T>)
    requires IsPermutation(a, b)
    ensures IsPermutation(b, a)
  {
  }

  lemma Permutation_trans<T>(a: seq<T>, b: seq<T>, c: seq<T>)
    requires IsPermutation(a, b) && IsPermutation(b, c)
    ensures IsPermutation(a, c)
  {
  }

  lemma Permutation_cons<T>(x: T, a: seq<T>, b: seq<T>)
    requires IsPermutation(a, b)
    ensures IsPermutation([x] + a, [x] + b)
  {
  }

  lemma Permutation_append<T>(a: seq<T>, b: seq<T>, c: seq<T>, d: seq<T>)
    requires IsPermutation(a, c) && IsPermutation(b, d)
    ensures IsPermutation(a + b, c + d)
  {
  }
}
