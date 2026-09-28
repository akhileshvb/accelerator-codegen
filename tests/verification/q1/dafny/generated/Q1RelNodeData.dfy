// GENERATED from Rocq serialize_LogicalSpec_Q1_rel_export_json — DO NOT EDIT.
// authority: formalsql/q1/Q1LogicalSpecTree.LogicalSpec_Q1_rel
// serialize: formalsql/q1/Q1LogicalSpecSerialize.v
// dump: tests/verification/specs/q1/logical_spec_relnode.dump.json
// source_sha256=3215b5a8f31183176ac2cf17a5c101a6d5d29f02ec08398cad1759f6cc3016ea
//
// RelNode LogicalSpec DATA only. No evaluator / Adapt.
// Query-specific row/schema/output adapters remain in Q1RelNodeSpec.dfy.

include "../../../logical/dafny/LogicalSpec.dfy"

module Q1RelNodeData {

  import opened LogicalSpec

  const TableLineitem: TableId := "lineitem"

  ghost function Q1PredShipdate(): Pred {
    EBinary(BLe, EInputRef(0), EParam("cutoff", TDate))
  }

  ghost function Q1GroupExprs(): seq<Expr> {
    [
      EInputRef(5),
      EInputRef(6)
    ]
  }

  ghost function Q1AggsRel(): seq<AggSpec> {
    [
      AggSpec(AggSum, SomeExpr(EInputRef(1))),
      AggSpec(AggSum, SomeExpr(EInputRef(2))),
      AggSpec(AggSum, SomeExpr(EBinary(BMul, EInputRef(2), EBinary(BSub, EConst(VInt(100), TNumeric(2)), EInputRef(3))))),
      AggSpec(AggSum, SomeExpr(EBinary(BMul, EBinary(BMul, EInputRef(2), EBinary(BSub, EConst(VInt(100), TNumeric(2)), EInputRef(3))), EBinary(BAdd, EConst(VInt(100), TNumeric(2)), EInputRef(4))))),
      AggSpec(AggAvg, SomeExpr(EInputRef(1))),
      AggSpec(AggAvg, SomeExpr(EInputRef(2))),
      AggSpec(AggAvg, SomeExpr(EInputRef(3))),
      AggSpec(AggCountStar, NoExpr)
    ]
  }

  ghost function Q1ProjectExprs(): seq<Expr> {
    [
      EGroupRef(0),
      EGroupRef(1),
      EAggRef(0),
      EAggRef(1),
      EAggRef(2),
      EAggRef(3),
      EAggRef(4),
      EAggRef(5),
      EAggRef(6),
      EAggRef(7)
    ]
  }

  ghost function Q1OrderKeysRel(): seq<OrderKey> {
    [
      OrderKey(EInputRef(0), SortAsc, NullsLast),
      OrderKey(EInputRef(1), SortAsc, NullsLast)
    ]
  }

  ghost function LogicalSpec_Q1_rel(): LogicalSpec {
    RelSort(RelProject(RelAggregate(RelFilter(RelScan("lineitem"), EBinary(BLe, EInputRef(0), EParam("cutoff", TDate))), [EInputRef(5), EInputRef(6)], [AggSpec(AggSum, SomeExpr(EInputRef(1))), AggSpec(AggSum, SomeExpr(EInputRef(2))), AggSpec(AggSum, SomeExpr(EBinary(BMul, EInputRef(2), EBinary(BSub, EConst(VInt(100), TNumeric(2)), EInputRef(3))))), AggSpec(AggSum, SomeExpr(EBinary(BMul, EBinary(BMul, EInputRef(2), EBinary(BSub, EConst(VInt(100), TNumeric(2)), EInputRef(3))), EBinary(BAdd, EConst(VInt(100), TNumeric(2)), EInputRef(4))))), AggSpec(AggAvg, SomeExpr(EInputRef(1))), AggSpec(AggAvg, SomeExpr(EInputRef(2))), AggSpec(AggAvg, SomeExpr(EInputRef(3))), AggSpec(AggCountStar, NoExpr)]), [EGroupRef(0), EGroupRef(1), EAggRef(0), EAggRef(1), EAggRef(2), EAggRef(3), EAggRef(4), EAggRef(5), EAggRef(6), EAggRef(7)]), [OrderKey(EInputRef(0), SortAsc, NullsLast), OrderKey(EInputRef(1), SortAsc, NullsLast)])
  }
}
