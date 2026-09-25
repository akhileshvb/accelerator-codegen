// Q12 ResultSpec evaluator — result-only join + CASE counts + ORDER BY.
// No accelerator state. Exact integer outputs (no AVG).

include "Q12ResultSpecData.dfy"

module Q12ResultSpec {

  import D = Q12ResultSpecData

  datatype Priority = High | Low

  datatype OrderRow = OrderRow(orderkey: int, priority: string)
  datatype LineRow = LineRow(
    orderkey: int,
    shipmode: string,
    commitdate: int,
    receiptdate: int,
    shipdate: int
  )

  datatype SpecStats = SpecStats(high: int, low: int)
  datatype SpecOutRow = SpecOutRow(shipmode: string, high: int, low: int)

  ghost function EmptyStats(): SpecStats { SpecStats(0, 0) }

  ghost function ClassifyPriority(p: string): Priority {
    if p == "1-URGENT" || p == "2-HIGH" then High else Low
  }

  ghost predicate LineQualifies(l: LineRow) {
    l.commitdate < l.receiptdate && l.shipdate < l.commitdate
  }

  ghost predicate ModeAccepted(mode: string, m1: string, m2: string) {
    mode == m1 || mode == m2
  }

  ghost predicate InWindow(day: int, start: int, endExclusive: int) {
    start <= day < endExclusive
  }

  // Join: matching orderkey (inner). Spec contributes only when order exists.
  ghost predicate JoinMatches(o: OrderRow, l: LineRow) {
    o.orderkey == l.orderkey
  }

  ghost function SpecApply(s: SpecStats, p: Priority, sign: int): SpecStats {
    if p.High? then SpecStats(s.high + sign, s.low)
    else SpecStats(s.high, s.low + sign)
  }

  type KeyStore = map<string, SpecStats>

  ghost function GetKey(m: KeyStore, k: string): SpecStats {
    if k in m then m[k] else EmptyStats()
  }

  ghost function PriorityMap(orders: seq<OrderRow>): map<int, Priority>
    decreases |orders|
  {
    if |orders| == 0 then map[]
    else
      PriorityMap(orders[..|orders| - 1])[
        orders[|orders| - 1].orderkey := ClassifyPriority(orders[|orders| - 1].priority)]
  }

  ghost function ApplyLine(
    m: KeyStore,
    pri: map<int, Priority>,
    l: LineRow,
    start: int,
    endExclusive: int,
    m1: string,
    m2: string,
    sign: int
  ): KeyStore {
    if !(LineQualifies(l) &&
         InWindow(l.receiptdate, start, endExclusive) &&
         ModeAccepted(l.shipmode, m1, m2) &&
         l.orderkey in pri) then
      m
    else
      var p := pri[l.orderkey];
      var k := l.shipmode;
      m[k := SpecApply(GetKey(m, k), p, sign)]
  }

  ghost function FoldLines(
    lines: seq<LineRow>,
    pri: map<int, Priority>,
    start: int,
    endExclusive: int,
    m1: string,
    m2: string
  ): KeyStore
    decreases |lines|
  {
    if |lines| == 0 then map[]
    else
      ApplyLine(FoldLines(lines[..|lines| - 1], pri, start, endExclusive, m1, m2),
                pri, lines[|lines| - 1], start, endExclusive, m1, m2, 1)
  }

  ghost function SpecView(
    orders: seq<OrderRow>,
    lines: seq<LineRow>,
    start: int,
    endExclusive: int,
    m1: string,
    m2: string
  ): KeyStore {
    FoldLines(lines, PriorityMap(orders), start, endExclusive, m1, m2)
  }

  ghost function SpecFinalizeOne(mode: string, s: SpecStats): seq<SpecOutRow> {
    if s.high == 0 && s.low == 0 then []
    else [SpecOutRow(mode, s.high, s.low)]
  }

  // At most two candidate modes (params); insert-sort by string.
  ghost function InsertSortedStr(rows: seq<SpecOutRow>, row: SpecOutRow): seq<SpecOutRow>
    decreases |rows|
  {
    if |rows| == 0 then [row]
    else if row.shipmode < rows[0].shipmode then [row] + rows
    else [rows[0]] + InsertSortedStr(rows[1..], row)
  }

  ghost function SortByMode(rows: seq<SpecOutRow>): seq<SpecOutRow>
    decreases |rows|
  {
    if |rows| == 0 then []
    else InsertSortedStr(SortByMode(rows[1..]), rows[0])
  }

  ghost function CollectModes(m1: string, m2: string, m: KeyStore): seq<SpecOutRow> {
    var r1 := SpecFinalizeOne(m1, GetKey(m, m1));
    if m1 == m2 then r1
    else r1 + SpecFinalizeOne(m2, GetKey(m, m2))
  }

  ghost function EvalResultSpec(
    orders: seq<OrderRow>,
    lines: seq<LineRow>,
    start: int,
    endExclusive: int,
    m1: string,
    m2: string
  ): seq<SpecOutRow> {
    SortByMode(CollectModes(m1, m2, SpecView(orders, lines, start, endExclusive, m1, m2)))
  }

  ghost function EvalResultSpec_S(s: D.QueryResultSpec, orders: seq<OrderRow>, lines: seq<LineRow>, endExclusive: int)
    : seq<SpecOutRow>
  {
    EvalResultSpec(orders, lines, s.start, endExclusive, s.shipmode1, s.shipmode2)
  }
}
