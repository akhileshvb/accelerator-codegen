// Q12 ResultSpec data — dual of Rocq QueryResultSpecQ12.v
// SpecExportCorrect(S_Q12, evaluator) is TCB (pin-checked dual).

module Q12ResultSpecData {

  const CatalogStart: int := 9131  // DATE '1995-01-01'

  datatype AggKind = HighLineCount | LowLineCount

  datatype QueryResultSpec = QueryResultSpec(
    start: int,
    shipmode1: string,
    shipmode2: string,
    group_by_shipmode: bool,
    aggs: seq<AggKind>,
    order_by_shipmode: bool
  )

  ghost function S_Q12(start: int, m1: string, m2: string): QueryResultSpec {
    QueryResultSpec(start, m1, m2, true, [HighLineCount, LowLineCount], true)
  }

  ghost function S_Q12_catalog(): QueryResultSpec {
    S_Q12(CatalogStart, "MAIL", "SHIP")
  }

  ghost predicate IsQ12Shape(s: QueryResultSpec) {
    s.group_by_shipmode &&
    s.order_by_shipmode &&
    s.aggs == [HighLineCount, LowLineCount]
  }

  lemma S_Q12_shape(start: int, m1: string, m2: string)
    ensures IsQ12Shape(S_Q12(start, m1, m2))
  {
  }
}
