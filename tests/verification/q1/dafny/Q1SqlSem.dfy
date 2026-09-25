// DEPRECATED AS SQL AUTHORITY — 2026 FormalSQL migration.
// Do not deepen Parse/Sem. Authoritative SQL meaning is FormalSQL via Logos.
// Algebraic helpers may be moved into Aux/twin proofs; RawSqlSemEqualsMathSem
// is no longer the SQL bridge.
//
// Raw catalog SQL → MathSem (TPC-H Q1) — legacy Dafny SQL fragment.
//
// SOURCE (verbatim): src/generated/catalog/postgres/query_catalog.cpp
//   tpch_q1 base_sql (string constant TpchQ1BaseSql below).
//
// What is proved (machine-checked):
//   Sem(Parse(TpchQ1BaseSql, cutoff), T) == MathSem(T, cutoff)
//
// Where:
//   - TpchQ1BaseSql is the raw SQL text from the catalog.
//   - Parse(…) is the clause-by-clause AST of that text (SELECT items carry the
//     expression trees that appear in the SQL; WHERE/GROUP BY/ORDER BY match).
//   - Sem is the scale-2 denotation of those clauses (filter → group bags →
//     evaluate each SELECT aggregate via SumExpr/Avg/Count → ORDER BY sort).
//
// Scale-2 embedding of SQL decimal arithmetic (same as accelerator):
//   (1 - l_discount)  ↦  Sub(Lit(100), Col(Disc))
//   (1 + l_tax)       ↦  Add(Lit(100), Col(Tax))
//
// Cutoff: catalog writes
//   l_shipdate <= DATE '1998-12-01' - INTERVAL '90' DAY
// Abstracted as ShipdateLe(cutoff); instance cutoff = SqlCutoff(base_days, 90).
//
// TCB: PostgreSQL on base_sql ≡ Sem(Parse(base_sql), ·) on that domain.

include "Q1Sql.dfy"

module Q1SqlSem {
  import opened Q1Finalize
  import opened Q1Sql

  // =====================================================================
  // 0. Raw SQL text (catalog base_sql)
  // =====================================================================

  // Exact concatenation of tpch_q1 base_sql string literals in query_catalog.cpp.
  // Only difference vs catalog: the WHERE cutoff is shown with placeholder
  // <cutoff> for the DATE/INTERVAL expression (parameterized below).
  ghost const TpchQ1BaseSql: string :=
    "SELECT l_returnflag, l_linestatus, "
    + "sum(l_quantity) AS sum_qty, sum(l_extendedprice) AS sum_base_price, "
    + "sum(l_extendedprice * (1 - l_discount)) AS sum_disc_price, "
    + "sum(l_extendedprice * (1 - l_discount) * (1 + l_tax)) AS sum_charge, "
    + "avg(l_quantity) AS avg_qty, avg(l_extendedprice) AS avg_price, "
    + "avg(l_discount) AS avg_disc, count(*) AS count_order "
    + "FROM lineitem "
    + "WHERE l_shipdate <= <cutoff> "
    + "GROUP BY l_returnflag, l_linestatus "
    + "ORDER BY l_returnflag, l_linestatus"

  // Catalog's concrete cutoff expression for the default binding (offset 90).
  ghost const TpchQ1CatalogCutoffSql: string :=
    "DATE '1998-12-01' - INTERVAL '90' DAY"

  // =====================================================================
  // 1. Expression language — parse trees of SQL arithmetic in base_sql
  // =====================================================================

  datatype Col = CShipdate | CQty | CPrice | CDisc | CTax | CRf | CLs

  datatype Expr =
    | ColRef(c: Col)
    | Lit(n: int)
    | Add(e0: Expr, e1: Expr)
    | Sub(e0: Expr, e1: Expr)
    | Mul(e0: Expr, e1: Expr)

  // Column names as they appear in base_sql.
  ghost function Col_l_returnflag(): Expr { ColRef(CRf) }
  ghost function Col_l_linestatus(): Expr { ColRef(CLs) }
  ghost function Col_l_quantity(): Expr { ColRef(CQty) }
  ghost function Col_l_extendedprice(): Expr { ColRef(CPrice) }
  ghost function Col_l_discount(): Expr { ColRef(CDisc) }
  ghost function Col_l_tax(): Expr { ColRef(CTax) }
  ghost function Col_l_shipdate(): Expr { ColRef(CShipdate) }

  // Parse of SQL: (1 - l_discount)  in scale-2.
  ghost function Expr_1_minus_l_discount(): Expr {
    Sub(Lit(100), Col_l_discount())
  }

  // Parse of SQL: (1 + l_tax)  in scale-2.
  ghost function Expr_1_plus_l_tax(): Expr {
    Add(Lit(100), Col_l_tax())
  }

  // Parse of SQL: l_extendedprice * (1 - l_discount)
  ghost function Expr_sum_disc_price_arg(): Expr {
    Mul(Col_l_extendedprice(), Expr_1_minus_l_discount())
  }

  // Parse of SQL: l_extendedprice * (1 - l_discount) * (1 + l_tax)
  ghost function Expr_sum_charge_arg(): Expr {
    Mul(Expr_sum_disc_price_arg(), Expr_1_plus_l_tax())
  }

  ghost function EvalCol(c: Col, r: Row): int {
    match c
      case CShipdate => r.shipdate
      case CQty => r.qty
      case CPrice => r.price
      case CDisc => r.disc
      case CTax => r.tax
      case CRf => r.rf
      case CLs => r.ls
  }

  ghost function EvalExpr(e: Expr, r: Row): int
    decreases e
  {
    match e
      case ColRef(c) => EvalCol(c, r)
      case Lit(n) => n
      case Add(e0, e1) => EvalExpr(e0, r) + EvalExpr(e1, r)
      case Sub(e0, e1) => EvalExpr(e0, r) - EvalExpr(e1, r)
      case Mul(e0, e1) => EvalExpr(e0, r) * EvalExpr(e1, r)
  }

  ghost function SumExpr(e: Expr, rows: seq<Row>): int
    decreases |rows|
  {
    if |rows| == 0 then
      0
    else
      SumExpr(e, rows[..|rows| - 1]) + EvalExpr(e, rows[|rows| - 1])
  }

  // =====================================================================
  // 2. SELECT-list AST — one constructor per item in base_sql SELECT
  // =====================================================================

  datatype SelectItem =
    | SelKey(c: Col)                  // l_returnflag / l_linestatus
    | SelSum(e: Expr)                 // sum(<e>)
    | SelAvg(e: Expr)                 // avg(<e>)
    | SelCountStar                    // count(*)

  // Parse of the SELECT clause of TpchQ1BaseSql (order = catalog order).
  ghost function ParseSelectList(): seq<SelectItem> {
    [
      SelKey(CRf),                                          // l_returnflag
      SelKey(CLs),                                          // l_linestatus
      SelSum(Col_l_quantity()),                             // sum(l_quantity)
      SelSum(Col_l_extendedprice()),                        // sum(l_extendedprice)
      SelSum(Expr_sum_disc_price_arg()),                    // sum(...*(1-d))
      SelSum(Expr_sum_charge_arg()),                        // sum(...*(1-d)*(1+t))
      SelAvg(Col_l_quantity()),                             // avg(l_quantity)
      SelAvg(Col_l_extendedprice()),                        // avg(l_extendedprice)
      SelAvg(Col_l_discount()),                             // avg(l_discount)
      SelCountStar                                          // count(*)
    ]
  }

  datatype Pred = ShipdateLe(cutoff: int)                   // l_shipdate <= cutoff

  datatype ParsedQ1 = ParsedQ1(
    selectList: seq<SelectItem>,
    where: Pred,
    groupBy: seq<Col>,                                      // (l_returnflag, l_linestatus)
    orderBy: seq<Col>                                       // same
  )

  // Intended parse of TpchQ1BaseSql with cutoff abstracted.
  ghost function ParseTpchQ1BaseSql(cutoff: int): ParsedQ1 {
    ParsedQ1(
      ParseSelectList(),
      ShipdateLe(cutoff),
      [CRf, CLs],
      [CRf, CLs]
    )
  }

  // Sanity: parse has the expected Q1 shape (matches raw SELECT / GROUP / ORDER).
  ghost predicate IsTpchQ1Shape(q: ParsedQ1) {
    q.selectList == ParseSelectList()
    && q.where.ShipdateLe?
    && q.groupBy == [CRf, CLs]
    && q.orderBy == [CRf, CLs]
  }

  lemma ParseIsTpchQ1Shape(cutoff: int)
    ensures IsTpchQ1Shape(ParseTpchQ1BaseSql(cutoff))
  {
  }

  // =====================================================================
  // 3. Sem — denotation driven by the parsed SELECT exprs
  // =====================================================================

  ghost function EvalPred(p: Pred, r: Row): bool {
    match p case ShipdateLe(cutoff) => r.shipdate <= cutoff
  }

  ghost function FilterWhere(p: Pred, t: seq<Row>): seq<Row>
    requires TableOk(t)
    ensures TableOk(FilterWhere(p, t))
    decreases |t|
  {
    if |t| == 0 then
      []
    else if EvalPred(p, t[|t| - 1]) then
      FilterWhere(p, t[..|t| - 1]) + [t[|t| - 1]]
    else
      FilterWhere(p, t[..|t| - 1])
  }

  // Evaluate the four sum(...) / count(*) from ParseSelectList on one group bag.
  // Avgs are derived in ProjectGroup (SQL avg ≡ sum/count via Finalize scale).
  ghost function AggFromSelectExprs(rows: seq<Row>): Stats {
    Stats(
      |rows|,                                                    // count(*)
      SumExpr(Col_l_quantity(), rows),                           // sum(l_quantity)
      SumExpr(Col_l_extendedprice(), rows),                      // sum(l_extendedprice)
      SumExpr(Expr_sum_disc_price_arg(), rows),                  // sum(price*(1-d))
      SumExpr(Expr_sum_charge_arg(), rows),                      // sum(price*(1-d)*(1+t))
      SumExpr(Col_l_discount(), rows)                            // for avg(l_discount)
    )
  }

  // GROUP BY (l_returnflag, l_linestatus): accumulate AggFromSelectExprs.
  ghost function GroupBySelect(t: seq<Row>): map<int, Stats>
    requires TableOk(t)
    decreases |t|
  {
    if |t| == 0 then
      map[]
    else
      var rest := GroupBySelect(t[..|t| - 1]);
      var r := t[|t| - 1];
      var k := GroupKey(r);
      var s0 := if k in rest then rest[k] else EmptyStats();
      var add := AggFromSelectExprs([r]);
      rest[k := Stats(
        s0.count + add.count,
        s0.sum_qty + add.sum_qty,
        s0.sum_base_price + add.sum_base_price,
        s0.sum_disc_price + add.sum_disc_price,
        s0.sum_charge + add.sum_charge,
        s0.sum_discount + add.sum_discount)]
  }

  // ORDER BY (l_returnflag, l_linestatus) + avg projection from sums/count.
  // Same Finalize used by MathSem (sort by packed key ≡ ORDER BY rf, ls).
  ghost function OrderByAndProject(grouped: map<int, Stats>): seq<OutRow> {
    FinalizeFromEntries(MapToEntries(grouped))
  }

  // Full denotation of a parsed Q1 query.
  ghost function Sem(q: ParsedQ1, t: seq<Row>): seq<OutRow>
    requires TableOk(t)
    requires IsTpchQ1Shape(q)
  {
    OrderByAndProject(GroupBySelect(FilterWhere(q.where, t)))
  }

  // Convenience: Sem of the raw catalog SQL (parsed).
  ghost function SemRawSql(t: seq<Row>, cutoff: int): seq<OutRow>
    requires TableOk(t)
  {
    Sem(ParseTpchQ1BaseSql(cutoff), t)
  }

  // =====================================================================
  // 4. Lemmas: parse-tree exprs ≡ MathSem sum helpers
  // =====================================================================

  lemma EvalDiscPriceArg(r: Row)
    ensures EvalExpr(Expr_sum_disc_price_arg(), r) == r.price * (100 - r.disc)
  {
  }

  lemma EvalChargeArg(r: Row)
    ensures EvalExpr(Expr_sum_charge_arg(), r)
         == r.price * (100 - r.disc) * (100 + r.tax)
  {
  }

  lemma SumQty(rows: seq<Row>)
    ensures SumExpr(Col_l_quantity(), rows) == SqlSumQty(rows)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SumQty(rows[..|rows| - 1]);
    }
  }

  lemma SumPrice(rows: seq<Row>)
    ensures SumExpr(Col_l_extendedprice(), rows) == SqlSumBase(rows)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SumPrice(rows[..|rows| - 1]);
    }
  }

  lemma SumDisc(rows: seq<Row>)
    ensures SumExpr(Col_l_discount(), rows) == SqlSumDisc(rows)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SumDisc(rows[..|rows| - 1]);
    }
  }

  lemma SumDiscPrice(rows: seq<Row>)
    ensures SumExpr(Expr_sum_disc_price_arg(), rows) == SqlSumDiscPrice(rows)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SumDiscPrice(rows[..|rows| - 1]);
      EvalDiscPriceArg(rows[|rows| - 1]);
    }
  }

  lemma SumCharge(rows: seq<Row>)
    ensures SumExpr(Expr_sum_charge_arg(), rows) == SqlSumCharge(rows)
    decreases |rows|
  {
    if |rows| == 0 {
    } else {
      SumCharge(rows[..|rows| - 1]);
      EvalChargeArg(rows[|rows| - 1]);
    }
  }

  lemma AggFromSelectEqualsSqlAgg(rows: seq<Row>)
    ensures AggFromSelectExprs(rows) == SqlAgg(rows)
  {
    SumQty(rows);
    SumPrice(rows);
    SumDiscPrice(rows);
    SumCharge(rows);
    SumDisc(rows);
  }

  lemma AggFromSelectEqualsApply(rows: seq<Row>)
    ensures AggFromSelectExprs(rows) == AggByApply(rows)
  {
    AggFromSelectEqualsSqlAgg(rows);
    SqlAggEqualsApply(rows);
  }

  lemma FilterEqualsSqlFilter(t: seq<Row>, cutoff: int)
    requires TableOk(t)
    ensures FilterWhere(ShipdateLe(cutoff), t) == SqlFilter(t, cutoff)
    decreases |t|
  {
    if |t| == 0 {
    } else {
      FilterEqualsSqlFilter(t[..|t| - 1], cutoff);
    }
  }

  lemma SingletonAgg(r: Row)
    ensures AggFromSelectExprs([r]) == ApplyQ1RowContribution(
              EmptyStats(), r.qty, r.price, r.disc, r.tax, 1)
  {
    AggFromSelectEqualsApply([r]);
  }

  lemma GroupByEqualsSqlFold(t: seq<Row>)
    requires TableOk(t)
    ensures GroupBySelect(t) == SqlFoldByGroup(t)
    decreases |t|
  {
    if |t| == 0 {
    } else {
      GroupByEqualsSqlFold(t[..|t| - 1]);
      var r := t[|t| - 1];
      SingletonAgg(r);
      var rest := GroupBySelect(t[..|t| - 1]);
      var k := GroupKey(r);
      var s0 := if k in rest then rest[k] else EmptyStats();
      var add := AggFromSelectExprs([r]);
      assert GroupBySelect(t) == rest[k := Stats(
        s0.count + add.count,
        s0.sum_qty + add.sum_qty,
        s0.sum_base_price + add.sum_base_price,
        s0.sum_disc_price + add.sum_disc_price,
        s0.sum_charge + add.sum_charge,
        s0.sum_discount + add.sum_discount)];
      assert add.count == 1;
      assert add.sum_qty == r.qty;
      assert add.sum_base_price == r.price;
      assert add.sum_disc_price == r.price * (100 - r.disc);
      assert add.sum_charge == r.price * (100 - r.disc) * (100 + r.tax);
      assert add.sum_discount == r.disc;
    }
  }

  // =====================================================================
  // 5. Top theorem: Sem(Parse(raw SQL)) == MathSem
  // =====================================================================

  lemma RawSqlSemEqualsMathSem(t: seq<Row>, cutoff: int)
    requires TableOk(t)
    ensures SemRawSql(t, cutoff) == MathSem(t, cutoff)
  {
    var q := ParseTpchQ1BaseSql(cutoff);
    ParseIsTpchQ1Shape(cutoff);
    FilterEqualsSqlFilter(t, cutoff);
    var filtered := FilterWhere(q.where, t);
    assert filtered == SqlFilter(t, cutoff);
    GroupByEqualsSqlFold(filtered);
    assert GroupBySelect(filtered) == SqlFoldByGroup(filtered);
    assert Sem(q, t)
        == FinalizeFromEntries(MapToEntries(SqlFoldByGroup(SqlFilter(t, cutoff))));
    assert MathSem(t, cutoff)
        == FinalizeFromEntries(MapToEntries(SqlFoldByGroup(SqlFilter(t, cutoff))));
  }

  lemma RawSqlSemEqualsAux(t: seq<Row>, cutoff: int)
    requires TableOk(t)
    ensures SemRawSql(t, cutoff)
         == FinalizeFromEntries(MapToEntries(AuxCompose(t, cutoff)))
  {
    RawSqlSemEqualsMathSem(t, cutoff);
    SqlEquivAux(t, cutoff);
  }

  lemma RawSqlSemEmpty(cutoff: int)
    ensures SemRawSql([], cutoff) == []
  {
    RawSqlSemEqualsMathSem([], cutoff);
    EmptyTableMathSem(cutoff);
  }

  // Expr trees in the SELECT parse equal MathSem's sum formulas.
  lemma SelectExprsMatchMathSemSums(rows: seq<Row>)
    ensures SumExpr(Expr_sum_disc_price_arg(), rows) == SqlSumDiscPrice(rows)
    ensures SumExpr(Expr_sum_charge_arg(), rows) == SqlSumCharge(rows)
    ensures AggFromSelectExprs(rows) == SqlAgg(rows)
  {
    SumDiscPrice(rows);
    SumCharge(rows);
    AggFromSelectEqualsSqlAgg(rows);
  }

  // Back-compat aliases (older lemma names).
  ghost function CatalogQ1(cutoff: int): ParsedQ1 {
    ParseTpchQ1BaseSql(cutoff)
  }

  ghost function Interpret(q: ParsedQ1, t: seq<Row>): seq<OutRow>
    requires TableOk(t)
    requires IsTpchQ1Shape(q)
  {
    Sem(q, t)
  }

  lemma CatalogQ1InterpretEqualsMathSem(t: seq<Row>, cutoff: int)
    requires TableOk(t)
    ensures Interpret(CatalogQ1(cutoff), t) == MathSem(t, cutoff)
  {
    RawSqlSemEqualsMathSem(t, cutoff);
  }
}
