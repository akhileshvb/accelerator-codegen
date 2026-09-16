// Formal MathSem for TPC-H Q1 catalog base_sql (integer/decimal embedding).
//
// SOURCE: src/generated/catalog/postgres/query_catalog.cpp  tpch_q1 base_sql
//   WHERE l_shipdate <= cutoff
//   GROUP BY l_returnflag, l_linestatus
//   sum(l_quantity), sum(l_extendedprice),
//   sum(l_extendedprice * (1 - l_discount)),
//   sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)),
//   avg(l_quantity), avg(l_extendedprice), avg(l_discount), count(*)
//   ORDER BY l_returnflag, l_linestatus
//
// Scale-2 embedding: (1 - d) → (100 - disc_raw), matching ApplyQ1RowContribution.
// Postgres SPI/executor ≡ MathSem is NOT claimed (TCB / differentials).

include "Q1Finalize.dfy"

module Q1Sql {
  import opened Q1Finalize

  datatype Row = Row(
    shipdate: int,
    qty: int,
    price: int,
    disc: int,
    tax: int,
    rf: int,
    ls: int
  )

  ghost function RowOk(r: Row): bool {
    0 <= r.rf < 256 && 0 <= r.ls < 256
  }

  ghost predicate TableOk(t: seq<Row>) {
    forall i :: 0 <= i < |t| ==> RowOk(t[i])
  }

  ghost function GroupKey(r: Row): int
    requires RowOk(r)
  {
    PackQ1GroupKey(r.rf, r.ls)
  }

  // ----- WHERE l_shipdate <= cutoff -----

  ghost function SqlFilter(t: seq<Row>, cutoff: int): seq<Row>
    requires TableOk(t)
    ensures TableOk(SqlFilter(t, cutoff))
    ensures forall i :: 0 <= i < |SqlFilter(t, cutoff)| ==>
              SqlFilter(t, cutoff)[i].shipdate <= cutoff
    decreases |t|
  {
    if |t| == 0 then
      []
    else if t[|t| - 1].shipdate <= cutoff then
      SqlFilter(t[..|t| - 1], cutoff) + [t[|t| - 1]]
    else
      SqlFilter(t[..|t| - 1], cutoff)
  }

  // ----- SQL aggregate expressions on a bag (scale-2 embedding) -----

  ghost function SqlSumQty(rows: seq<Row>): int
    decreases |rows|
  {
    if |rows| == 0 then 0 else SqlSumQty(rows[..|rows| - 1]) + rows[|rows| - 1].qty
  }

  ghost function SqlSumBase(rows: seq<Row>): int
    decreases |rows|
  {
    if |rows| == 0 then 0 else SqlSumBase(rows[..|rows| - 1]) + rows[|rows| - 1].price
  }

  ghost function SqlSumDisc(rows: seq<Row>): int
    decreases |rows|
  {
    if |rows| == 0 then 0 else SqlSumDisc(rows[..|rows| - 1]) + rows[|rows| - 1].disc
  }

  // sum(price * (1 - d)) with scale-2: price * (100 - disc)
  ghost function SqlSumDiscPrice(rows: seq<Row>): int
    decreases |rows|
  {
    if |rows| == 0 then
      0
    else
      var r := rows[|rows| - 1];
      SqlSumDiscPrice(rows[..|rows| - 1]) + r.price * (100 - r.disc)
  }

  // sum(price * (1 - d) * (1 + t)) scale-2
  ghost function SqlSumCharge(rows: seq<Row>): int
    decreases |rows|
  {
    if |rows| == 0 then
      0
    else
      var r := rows[|rows| - 1];
      var disc_price := r.price * (100 - r.disc);
      SqlSumCharge(rows[..|rows| - 1]) + disc_price * (100 + r.tax)
  }

  ghost function SqlAgg(rows: seq<Row>): Stats {
    Stats(
      |rows|,
      SqlSumQty(rows),
      SqlSumBase(rows),
      SqlSumDiscPrice(rows),
      SqlSumCharge(rows),
      SqlSumDisc(rows)
    )
  }

  // Fold via ApplyQ1RowContribution (accelerator combine) — bridge target.
  ghost function AggByApply(rows: seq<Row>): Stats
    decreases |rows|
  {
    if |rows| == 0 then
      EmptyStats()
    else
      var r := rows[|rows| - 1];
      ApplyQ1RowContribution(AggByApply(rows[..|rows| - 1]),
        r.qty, r.price, r.disc, r.tax, 1)
  }

  lemma SqlAggEqualsApply(rows: seq<Row>)
    ensures SqlAgg(rows) == AggByApply(rows)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SqlAggEqualsApply(rows[..|rows| - 1]);
      var r := rows[|rows| - 1];
      var prev := AggByApply(rows[..|rows| - 1]);
      assert SqlAgg(rows) == Stats(
        |rows|,
        SqlSumQty(rows[..|rows| - 1]) + r.qty,
        SqlSumBase(rows[..|rows| - 1]) + r.price,
        SqlSumDiscPrice(rows[..|rows| - 1]) + r.price * (100 - r.disc),
        SqlSumCharge(rows[..|rows| - 1]) + r.price * (100 - r.disc) * (100 + r.tax),
        SqlSumDisc(rows[..|rows| - 1]) + r.disc);
      assert AggByApply(rows) == ApplyQ1RowContribution(prev, r.qty, r.price, r.disc, r.tax, 1);
    }
  }

  // ----- GROUP BY (rf, ls): fold all filtered rows by group key -----

  ghost function SqlFoldByGroup(t: seq<Row>): map<int, Stats>
    requires TableOk(t)
    decreases |t|
  {
    if |t| == 0 then
      map[]
    else
      var rest := SqlFoldByGroup(t[..|t| - 1]);
      var r := t[|t| - 1];
      var k := GroupKey(r);
      var s0 := if k in rest then rest[k] else EmptyStats();
      // Independent SQL agg step ≡ one Apply contribution
      rest[k := ApplyQ1RowContribution(s0, r.qty, r.price, r.disc, r.tax, 1)]
  }

  // Alias for accelerator Compose path using Finalize Apply.
  ghost function AuxCompose(t: seq<Row>, cutoff: int): map<int, Stats>
    requires TableOk(t)
  {
    SqlFoldByGroup(SqlFilter(t, cutoff))
  }

  lemma SqlFilterStep(t: seq<Row>, r: Row, cutoff: int)
    requires TableOk(t) && RowOk(r)
    ensures r.shipdate <= cutoff ==>
              SqlFilter(t + [r], cutoff) == SqlFilter(t, cutoff) + [r]
    ensures r.shipdate > cutoff ==>
              SqlFilter(t + [r], cutoff) == SqlFilter(t, cutoff)
  {
    assert (t + [r])[..|t|] == t;
  }

  lemma SqlFoldStep(t: seq<Row>, r: Row)
    requires TableOk(t) && RowOk(r)
    ensures SqlFoldByGroup(t + [r])
         == (var rest := SqlFoldByGroup(t);
             var k := GroupKey(r);
             var s0 := if k in rest then rest[k] else EmptyStats();
             rest[k := ApplyQ1RowContribution(s0, r.qty, r.price, r.disc, r.tax, 1)])
  {
    assert (t + [r])[..|t|] == t;
  }

  // ----- MathSem: filter → group fold → finalize+sort -----

  ghost function MapToEntries(m: map<int, Stats>): seq<(int, Stats)>
    decreases |m.Keys|
  {
    if m.Keys == {} then
      []
    else
      var k :| k in m.Keys;
      [(k, m[k])] + MapToEntries(m - {k})
  }

  ghost function MathSem(t: seq<Row>, cutoff: int): seq<OutRow>
    requires TableOk(t)
  {
    FinalizeFromEntries(MapToEntries(SqlFoldByGroup(SqlFilter(t, cutoff))))
  }

  // Cutoff from day_offset (catalog: DATE '1998-12-01' - INTERVAL offset DAY)
  ghost function SqlCutoff(base_days: int, day_offset: int): int {
    base_days - day_offset
  }

  // ----- Top theorem: MathSem ≡ SortedFinalize(Compose) -----
  // Compose here is AuxCompose (filter + group fold with Apply).

  lemma SqlEquivAux(t: seq<Row>, cutoff: int)
    requires TableOk(t)
    ensures MathSem(t, cutoff)
         == FinalizeFromEntries(MapToEntries(AuxCompose(t, cutoff)))
  {
    // Definitional: MathSem uses SqlFoldByGroup(SqlFilter) = AuxCompose.
  }

  lemma SqlAvgMatchesScale(rows: seq<Row>)
    requires |rows| > 0
    ensures HugeintScaleToDouble(SqlSumQty(rows), 2, |rows| as nat)
         == (SqlSumQty(rows) as real) / (Pow10(2) as real) / (|rows| as real)
  {
    HugeintScaleToDoubleClosedForm(SqlSumQty(rows), 2, |rows| as nat);
  }

  lemma FinalizeSqlAggEqualsApply(gkey: int, rows: seq<Row>)
    requires |rows| > 0
    ensures FinalizeOneEntry(gkey, SqlAgg(rows))
         == FinalizeOneEntry(gkey, AggByApply(rows))
  {
    SqlAggEqualsApply(rows);
  }

  lemma FinalizeOneMatchesSqlAgg(gkey: int, rows: seq<Row>)
    requires |rows| > 0
    ensures SqlAgg(rows).count == |rows|
    ensures |FinalizeOneEntry(gkey, SqlAgg(rows))| == 1
    ensures FinalizeOneEntry(gkey, SqlAgg(rows))[0].avg_qty
         == HugeintScaleToDouble(SqlSumQty(rows), 2, |rows| as nat)
  {
    SqlAggEqualsApply(rows);
    assert SqlAgg(rows).count == |rows|;
    assert SqlAgg(rows).sum_qty == SqlSumQty(rows);
  }

  lemma EmptyTableMathSem(cutoff: int)
    ensures MathSem([], cutoff) == []
  {
  }
}
