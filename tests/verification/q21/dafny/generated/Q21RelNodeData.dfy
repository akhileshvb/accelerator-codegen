// Generated from Rocq LogicalSpec_Q21_rel — DO NOT HAND-EDIT.
// Authority: rocq_serialize_LogicalSpec_Q21_rel_json
// source_sha256=61553bc4d505550edb81642db3975826546f32e72ae6a3ab6c30f5e774545a3b

include "../../../logical/dafny/LogicalSpec.dfy"

module Q21RelNodeData {
  import opened LogicalSpec

  const LogicalSpec_Q21_rel: LogicalSpec :=
    RelSort(RelProject(RelAggregate(RelAntiJoin(RelSemiJoin(RelFilter(RelJoin(JoinInner, RelJoin(JoinInner, RelJoin(JoinInner, RelScan("supplier"), RelScan("lineitem"), EBinary(BEq, EInputRef(0), EInputRef(4))), RelScan("orders"), EBinary(BEq, EInputRef(3), EInputRef(7))), RelScan("nation"), EBinary(BEq, EInputRef(2), EInputRef(9))), EBinary(BAnd, EBinary(BEq, EInputRef(8), EConst(VText("F"), TText)), EBinary(BAnd, EBinary(BGt, EInputRef(6), EInputRef(5)), EBinary(BEq, EInputRef(10), EParam("nation_name", TText))))), RelScan("lineitem"), EBinary(BAnd, EBinary(BEq, EInputRef(3), EInputRef(11)), EBinary(BNe, EInputRef(4), EInputRef(12)))), RelFilter(RelScan("lineitem"), EBinary(BGt, EInputRef(3), EInputRef(2))), EBinary(BAnd, EBinary(BEq, EInputRef(3), EInputRef(11)), EBinary(BNe, EInputRef(4), EInputRef(12)))), [EInputRef(1)], [AggSpec(AggCountStar, NoExpr)]), [EGroupRef(0), EAggRef(0)]), [OrderKey(EInputRef(1), SortDesc, NullsFirst), OrderKey(EInputRef(0), SortAsc, NullsLast)])
}
