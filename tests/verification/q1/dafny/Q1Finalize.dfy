// Structural twin of src/generated/tpch/q1/postgres/aux/finalize.cpp
//
// SOURCE MAP (do not edit without updating C++ or conformance):
//   PackQ1GroupKey          finalize.cpp:~51
//   UnpackQ1Returnflag      finalize.cpp:~56
//   UnpackQ1Linestatus      finalize.cpp:~60
//   ApplyQ1RowContribution  finalize.cpp:~64
//   MergeQ1DayGroupStats    finalize.cpp:~77
//   HugeintScaleToDouble    finalize.cpp:~35
//   FinalizeQ1MergedGroups  finalize.cpp:~86
//
// Induction uses Dafny `real` for avg_* (same divide-by-10 loop as C++).
// IEEE double / Hugeint::Cast is bound by conformance bit-pattern checks.

module Q1Finalize {

  datatype Stats = Stats(
    count: int,
    sum_qty: int,
    sum_base_price: int,
    sum_disc_price: int,
    sum_charge: int,
    sum_discount: int
  )

  ghost function EmptyStats(): Stats {
    Stats(0, 0, 0, 0, 0, 0)
  }

  // ----- PackQ1GroupKey / Unpack* (finalize.cpp) -----

  ghost function PackQ1GroupKey(returnflag: int, linestatus: int): int
    requires 0 <= returnflag < 256 && 0 <= linestatus < 256
  {
    returnflag * 256 + linestatus
  }

  ghost function UnpackQ1Returnflag(key: int): int
    requires 0 <= key < 65536
  {
    key / 256
  }

  ghost function UnpackQ1Linestatus(key: int): int
    requires 0 <= key < 65536
  {
    key % 256
  }

  lemma PackUnpack(rf: int, ls: int)
    requires 0 <= rf < 256 && 0 <= ls < 256
    ensures UnpackQ1Returnflag(PackQ1GroupKey(rf, ls)) == rf
    ensures UnpackQ1Linestatus(PackQ1GroupKey(rf, ls)) == ls
  {
  }

  // ----- ApplyQ1RowContribution (finalize.cpp) -----
  // Structural: same field updates and disc_price / charge formula.

  ghost function ApplyQ1RowContribution(
    s: Stats,
    quantity_raw: int,
    extendedprice_raw: int,
    discount_raw: int,
    tax_raw: int,
    sign: int
  ): Stats {
    var one_minus_discount_raw := 100 - discount_raw;
    var disc_price_raw := extendedprice_raw * one_minus_discount_raw;
    var one_plus_tax_raw := 100 + tax_raw;
    Stats(
      s.count + sign,
      s.sum_qty + sign * quantity_raw,
      s.sum_base_price + sign * extendedprice_raw,
      s.sum_disc_price + sign * disc_price_raw,
      s.sum_charge + sign * disc_price_raw * one_plus_tax_raw,
      s.sum_discount + sign * discount_raw
    )
  }

  lemma ApplyCancel(s: Stats, qty: int, price: int, disc: int, tax: int)
    ensures ApplyQ1RowContribution(
              ApplyQ1RowContribution(s, qty, price, disc, tax, 1),
              qty, price, disc, tax, -1) == s
  {
  }

  lemma ApplyCommutative(
    s: Stats,
    qty1: int, price1: int, disc1: int, tax1: int,
    qty2: int, price2: int, disc2: int, tax2: int
  )
    ensures ApplyQ1RowContribution(
              ApplyQ1RowContribution(s, qty1, price1, disc1, tax1, 1),
              qty2, price2, disc2, tax2, 1)
         == ApplyQ1RowContribution(
              ApplyQ1RowContribution(s, qty2, price2, disc2, tax2, 1),
              qty1, price1, disc1, tax1, 1)
  {
  }

  // ----- MergeQ1DayGroupStats (finalize.cpp) -----

  ghost function MergeQ1DayGroupStats(merged: Stats, day_stats: Stats): Stats {
    Stats(
      merged.count + day_stats.count,
      merged.sum_qty + day_stats.sum_qty,
      merged.sum_base_price + day_stats.sum_base_price,
      merged.sum_disc_price + day_stats.sum_disc_price,
      merged.sum_charge + day_stats.sum_charge,
      merged.sum_discount + day_stats.sum_discount
    )
  }

  lemma MergeCommutative(a: Stats, b: Stats)
    ensures MergeQ1DayGroupStats(a, b) == MergeQ1DayGroupStats(b, a)
  {
  }

  lemma MergeAssociative(a: Stats, b: Stats, c: Stats)
    ensures MergeQ1DayGroupStats(MergeQ1DayGroupStats(a, b), c)
         == MergeQ1DayGroupStats(a, MergeQ1DayGroupStats(b, c))
  {
  }

  // ----- HugeintScaleToDouble (finalize.cpp:~35) -----
  // Structural while-scale loop as recursive ScaleDiv; then / count.

  ghost function Pow10(n: nat): int
    ensures Pow10(n) > 0
  {
    if n == 0 then 1 else 10 * Pow10(n - 1)
  }

  // Loop body: while scale > 0 { scaled /= 10; scale--; }
  ghost function ScaleDiv(numeric: real, scale: nat): real
    decreases scale
  {
    if scale == 0 then
      numeric
    else
      ScaleDiv(numeric / 10.0, scale - 1)
  }

  ghost function HugeintScaleToDouble(value: int, scale: nat, count: nat): real {
    if count == 0 then
      0.0
    else
      ScaleDiv(value as real, scale) / (count as real)
  }

  lemma ScaleDivPow10(numeric: real, scale: nat)
    ensures ScaleDiv(numeric, scale) == numeric / (Pow10(scale) as real)
    decreases scale
  {
    if scale == 0 {
    } else {
      ScaleDivPow10(numeric / 10.0, scale - 1);
      assert Pow10(scale) == 10 * Pow10(scale - 1);
    }
  }

  lemma HugeintScaleToDoubleClosedForm(value: int, scale: nat, count: nat)
    requires count > 0
    ensures HugeintScaleToDouble(value, scale, count)
         == (value as real) / (Pow10(scale) as real) / (count as real)
  {
    ScaleDivPow10(value as real, scale);
  }

  lemma HugeintScaleToDoubleZeroCount(value: int, scale: nat)
    ensures HugeintScaleToDouble(value, scale, 0) == 0.0
  {
  }

  // ----- FinalizeQ1MergedGroups (finalize.cpp:~86) -----

  datatype OutRow = OutRow(
    group_key: int,
    sum_qty: int,
    sum_base_price: int,
    sum_disc_price: int,
    sum_charge: int,
    avg_qty: real,
    avg_price: real,
    avg_disc: real,
    count_order: int
  )

  const kDecimalScale2: nat := 2

  // One map entry → optional row (skip if count <= 0), matching the for-loop body.
  ghost function FinalizeOneEntry(gkey: int, stats: Stats): seq<OutRow> {
    if stats.count <= 0 then
      []
    else
      var count := stats.count as nat;
      [OutRow(
        gkey,
        stats.sum_qty,
        stats.sum_base_price,
        stats.sum_disc_price,
        stats.sum_charge,
        HugeintScaleToDouble(stats.sum_qty, kDecimalScale2, count),
        HugeintScaleToDouble(stats.sum_base_price, kDecimalScale2, count),
        HugeintScaleToDouble(stats.sum_discount, kDecimalScale2, count),
        stats.count
      )]
  }

  lemma FinalizeOneAvgs(gkey: int, stats: Stats)
    requires stats.count > 0
    ensures |FinalizeOneEntry(gkey, stats)| == 1
    ensures FinalizeOneEntry(gkey, stats)[0].avg_qty
         == HugeintScaleToDouble(stats.sum_qty, 2, stats.count as nat)
    ensures FinalizeOneEntry(gkey, stats)[0].avg_price
         == HugeintScaleToDouble(stats.sum_base_price, 2, stats.count as nat)
    ensures FinalizeOneEntry(gkey, stats)[0].avg_disc
         == HugeintScaleToDouble(stats.sum_discount, 2, stats.count as nat)
    ensures FinalizeOneEntry(gkey, stats)[0].count_order == stats.count
  {
  }

  lemma FinalizeSkipsNonPositive(gkey: int, stats: Stats)
    requires stats.count <= 0
    ensures FinalizeOneEntry(gkey, stats) == []
  {
  }

  // Collect-then-sort model of FinalizeQ1MergedGroups (finalize.cpp:~86–109).
  // SortByKey is the structural twin of std::sort by group_key; sortedness of
  // the comparator is checked by conformance (order of rows), not re-proved here.

  ghost function CollectFinalize(entries: seq<(int, Stats)>): seq<OutRow>
    decreases |entries|
  {
    if |entries| == 0 then
      []
    else
      FinalizeOneEntry(entries[0].0, entries[0].1) + CollectFinalize(entries[1..])
  }

  ghost function InsertSorted(rows: seq<OutRow>, row: OutRow): seq<OutRow>
    decreases |rows|
  {
    if |rows| == 0 then
      [row]
    else if row.group_key < rows[0].group_key then
      [row] + rows
    else
      [rows[0]] + InsertSorted(rows[1..], row)
  }

  ghost function SortByKey(rows: seq<OutRow>): seq<OutRow>
    decreases |rows|
  {
    if |rows| == 0 then
      []
    else
      InsertSorted(SortByKey(rows[1..]), rows[0])
  }

  ghost function FinalizeFromEntries(entries: seq<(int, Stats)>): seq<OutRow> {
    SortByKey(CollectFinalize(entries))
  }

  lemma CollectPreservesAvgs(entries: seq<(int, Stats)>, i: nat)
    requires i < |entries|
    requires entries[i].1.count > 0
    ensures FinalizeOneEntry(entries[i].0, entries[i].1)[0].avg_qty
         == HugeintScaleToDouble(entries[i].1.sum_qty, 2, entries[i].1.count as nat)
  {
  }

  lemma FinalizeAvgsMatchScale(gkey: int, stats: Stats)
    requires stats.count > 0
    ensures var row := FinalizeOneEntry(gkey, stats)[0];
            row.avg_qty == (stats.sum_qty as real) / (Pow10(2) as real) / (stats.count as real)
  {
    HugeintScaleToDoubleClosedForm(stats.sum_qty, 2, stats.count as nat);
  }
}
