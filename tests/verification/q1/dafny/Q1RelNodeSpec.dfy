// Q1RelNodeSpec — query-specific RelNode adapters + Eval_Spec_Q1_OutRows_rel.
// LogicalSpec tree DATA is generated from Rocq LogicalSpec_Q1_rel
// (see generated/Q1RelNodeData.dfy). Do not hand-duplicate the query tree here.
//
// Dual of formalsql/q1/{Q1LogicalSpecTree,Q1Adapt}.v on logical/dafny Eval_Spec.
// RelNode GroupStore is seq<GroupEntry> (Rocq list dual). Observational bridge
// to flat Eval_Spec_Q1_OutRows / EvalResultSpec is Qed in Q1RelNodeBridge.dfy:
//   Eval_Spec_Q1_OutRows_rel == EvalResultSpec == flat Eval_Spec_Q1_OutRows.
// Public Dafny Exec / runtime Hit theorems target this RelNode endpoint.
//
// No Q1-name branch inside RelNode Eval_Spec.

include "../../logical/dafny/EvalLogicalSpec.dfy"
include "generated/Q1ResultSpec.dfy"
include "generated/Q1RelNodeData.dfy"

module Q1RelNodeSpec {

  import opened LogicalSpec
  import RelEval = RelEvalLogicalSpec
  import R = Q1ResultSpec
  import Data = Q1RelNodeData

  // --- Generated RelNode LogicalSpec DATA (Rocq LogicalSpec_Q1_rel) ---

  const TableLineitem: TableId := Data.TableLineitem

  ghost function Q1PredShipdate(): Pred { Data.Q1PredShipdate() }
  ghost function Q1GroupExprs(): seq<Expr> { Data.Q1GroupExprs() }
  ghost function Q1AggsRel(): seq<AggSpec> { Data.Q1AggsRel() }
  ghost function Q1ProjectExprs(): seq<Expr> { Data.Q1ProjectExprs() }
  ghost function Q1OrderKeysRel(): seq<OrderKey> { Data.Q1OrderKeysRel() }
  ghost function LogicalSpec_Q1_rel(): LogicalSpec { Data.LogicalSpec_Q1_rel() }

  // --- Query-specific row / schema / Params adapters ---

  ghost function RowOfAux(r: R.Row): Row
  {
    [VInt(r.shipdate), VInt(r.qty), VInt(r.price), VInt(r.disc),
     VInt(r.tax), VInt(r.rf), VInt(r.ls)]
  }

  ghost function RowsOfAux(db: seq<R.Row>): seq<Row>
    decreases |db|
  {
    if |db| == 0 then [] else [RowOfAux(db[0])] + RowsOfAux(db[1..])
  }

  ghost function DbOfAux(db: seq<R.Row>): DB
  {
    [(TableLineitem, RowsOfAux(db))]
  }

  ghost function ParamCutoff(c: int): Params
  {
    [("cutoff", VInt(c))]
  }

  // --- Query-specific output Adapt (Pack / SpecOutRow) ---

  ghost function Q1AdaptRowRel(r: Row): seq<R.SpecOutRow>
  {
    if |r| == 10 &&
       r[0].VInt? && r[1].VInt? &&
       r[2].VInt? && r[3].VInt? && r[4].VInt? && r[5].VInt? &&
       r[6].VAvgExact? && r[7].VAvgExact? && r[8].VAvgExact? &&
       r[9].VInt? &&
       0 <= r[0].z < 256 && 0 <= r[1].z < 256 then
      var rf := r[0].z;
      var ls := r[1].z;
      var c := r[9].z;
      if c <= 0 then []
      else [R.SpecOutRow(R.Pack(rf, ls), r[2].z, r[3].z, r[4].z, r[5].z, r[8].sum, c)]
    else
      []
  }

  ghost function Q1AdaptRel(rows: seq<Row>): seq<R.SpecOutRow>
    decreases |rows|
  {
    if |rows| == 0 then []
    else Q1AdaptRowRel(rows[0]) + Q1AdaptRel(rows[1..])
  }

  // RelNode Eval_Spec ∘ Q1Adapt — public Dafny Q1 semantic endpoint.
  // Observational equality to flat Eval_Spec_Q1_OutRows / EvalResultSpec is Qed
  // in Q1RelNodeBridge.dfy (Eval_Spec_Q1_OutRows_rel_eq_flat).
  ghost function Eval_Spec_Q1_OutRows_rel(c: int, db: seq<R.Row>): seq<R.SpecOutRow>
    requires forall i :: 0 <= i < |db| ==> 0 <= db[i].rf < 256 && 0 <= db[i].ls < 256
  {
    var ev := RelEval.Eval_Spec(LogicalSpec_Q1_rel(), ParamCutoff(c), DbOfAux(db));
    if ev.SomeRows? then Q1AdaptRel(ev.rs) else []
  }
}
