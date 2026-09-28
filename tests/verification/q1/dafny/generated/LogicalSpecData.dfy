// GENERATED from Rocq serialize_LogicalSpec_Q1_export_json — DO NOT EDIT.
// source: formalsql/resultspec/LogicalSpec.v + LogicalSpecSerialize.v
// dump: tests/verification/specs/q1/logical_spec.dump.json
// source_sha256=3289b9672354019e3a1a60edd0c5d1355a2478dfafa48bb760b2d048210c3b25
//
// Expression-IR LogicalSpec DATA only. No evaluator.
// Canonical template is parameterized (EParam); catalog Params are separate.
// Group/order keys are semantic Expr lists (tuples of Values), not a physical int key.
// Scales: Const/Param/Add/Sub/Mul/Sum/Avg follow Rocq column_scale / expr_scale rules.

module LogicalSpecData {

  datatype Scale = Scale0 | Scale2 | Scale4 | Scale6

  datatype ColumnId =
    | ColShipdate | ColQty | ColPrice | ColDisc | ColTax | ColRf | ColLs

  datatype ParamId = ParamCutoff

  datatype Expr =
    | EColumn(c: ColumnId)
    | EConst(z: int, s: Scale)
    | EParam(id: ParamId, s: Scale)
    | EAdd(a: Expr, b: Expr)
    | ESub(a: Expr, b: Expr)
    | EMul(a: Expr, b: Expr)

  datatype Pred = PLe(a: Expr, b: Expr)

  datatype Agg =
    | AggSum(e: Expr)
    | AggAvg(e: Expr)
    | AggCountStar

  datatype SortDir = SortAsc | SortDesc

  datatype OrderKey = OrderKey(expr: Expr, dir: SortDir)

  datatype SpecTable = SpecTableLineitem | SpecTableOrders

  datatype SpecJoin = SpecJoinNone

  datatype LogicalSpec = LogicalSpec(
    inputs: seq<SpecTable>,
    join: SpecJoin,
    filters: seq<Pred>,
    group_keys: seq<Expr>,
    aggs: seq<Agg>,
    order_keys: seq<OrderKey>
  )

  datatype Params = Params(cutoff: int)

  const BaseDate: int := 10561

  const CatalogDelta: int := 90

  const CatalogCutoff: int := 10471

  const CatalogParams: Params := Params(CatalogCutoff)

  // Canonical parameterized Q1 template (no baked cutoff).
  const LogicalSpec_Q1: LogicalSpec := LogicalSpec(
    [SpecTableLineitem],
    SpecJoinNone,
    [
      PLe(EColumn(ColShipdate), EParam(ParamCutoff, Scale0))
    ],
    [
      EColumn(ColRf),
      EColumn(ColLs)
    ],
    [
      AggSum(EColumn(ColQty)),
      AggSum(EColumn(ColPrice)),
      AggSum(EMul(EColumn(ColPrice), ESub(EConst(100, Scale2), EColumn(ColDisc)))),
      AggSum(EMul(EMul(EColumn(ColPrice), ESub(EConst(100, Scale2), EColumn(ColDisc))), EAdd(EConst(100, Scale2), EColumn(ColTax)))),
      AggAvg(EColumn(ColQty)),
      AggAvg(EColumn(ColPrice)),
      AggAvg(EColumn(ColDisc)),
      AggCountStar
    ],
    [
      OrderKey(EColumn(ColRf), SortAsc),
      OrderKey(EColumn(ColLs), SortAsc)
    ]
  )

  ghost function ParamsOfCutoff(c: int): Params {
    Params(c)
  }

  lemma CatalogParams_delta90()
    ensures CatalogParams.cutoff == BaseDate - CatalogDelta
    ensures CatalogParams == ParamsOfCutoff(CatalogCutoff)
  {
  }
}
