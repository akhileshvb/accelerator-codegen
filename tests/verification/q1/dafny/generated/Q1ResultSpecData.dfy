// GENERATED data-only dual of Rocq QueryResultSpec.v (Q1).
// SpecExportCorrect shrink: this module is the trusted AST transcription;
// evaluation lives in Q1ResultSpec.dfy (generic-ish fold/finalize over this data).
// source: formalsql/resultspec/QueryResultSpec.v
// source_sha256=q1-resultspec-poc-v1
//
// Mirrors Rocq:
//   q1_base_date, cutoff_Q1, Q1AggKind, QueryResultSpec_Q1 / S_Q1

module Q1ResultSpecData {

  const BaseDate: int := 10561

  ghost function Cutoff(delta: int): int {
    BaseDate - delta
  }

  datatype AggKind =
    | SumQty | SumBase | SumDiscPrice | SumCharge
    | AvgQty | AvgPrice | AvgDisc
    | CountStar

  // Canonical Q1 result-spec descriptor (parameterized by shipdate cutoff).
  datatype QueryResultSpec = QueryResultSpec(
    cutoff: int,
    group_by_rf_ls: bool,
    aggs: seq<AggKind>,
    order_by_rf_ls: bool
  )

  ghost function S_Q1(cutoff: int): QueryResultSpec {
    QueryResultSpec(
      cutoff,
      true,
      [SumQty, SumBase, SumDiscPrice, SumCharge,
       AvgQty, AvgPrice, AvgDisc, CountStar],
      true
    )
  }

  ghost predicate IsQ1Shape(s: QueryResultSpec) {
    s.group_by_rf_ls &&
    s.order_by_rf_ls &&
    s.aggs == [SumQty, SumBase, SumDiscPrice, SumCharge,
               AvgQty, AvgPrice, AvgDisc, CountStar]
  }

  lemma S_Q1_shape(cutoff: int)
    ensures IsQ1Shape(S_Q1(cutoff))
    ensures S_Q1(cutoff).cutoff == cutoff
  {
  }

  lemma Cutoff_catalog_90()
    ensures Cutoff(90) == 10471
  {
  }
}
