// Generic RelNode LogicalSpec IR (Dafny) — query-independent.
// No shipdate/rf/ls/Pack/Q1 Stats. Q1 catalog/adapt live under tests/verification/q1/.

module LogicalSpec {

  datatype SqlType =
    | TInt | TNumeric(scale: nat) | TBool | TDate | TText | TTyUnknown

  datatype SqlValue =
    | VNull
    | VInt(z: int)
    | VBool(b: bool)
    | VText(s: string)
    | VAvgExact(sum: int, count: int)

  datatype Attr = Attr(name: string, ty: SqlType, nullable: bool)
  type Schema = seq<Attr>
  type Row = seq<SqlValue>
  type TableId = string
  type Params = seq<(string, SqlValue)>
  type DB = seq<(TableId, seq<Row>)>

  datatype UnaryOp = UNeg | UNot | UIsNull | UIsNotNull
  datatype BinaryOp =
    | BAdd | BSub | BMul | BDiv
    | BEq | BNe | BLt | BLe | BGt | BGe
    | BAnd | BOr | BLike | BPrefixMatch

  datatype Expr =
    | EInputRef(slot: nat)
    | EConst(v: SqlValue, ty: SqlType)
    | EParam(id: string, ty: SqlType)
    | EUnary(uop: UnaryOp, e: Expr)
    | EBinary(bop: BinaryOp, lhs: Expr, rhs: Expr)
    | ECase(branches: seq<(Expr, Expr)>, elseExpr: Expr)
    | ECast(e: Expr, ty: SqlType)
    | EGroupRef(i: nat)
    | EAggRef(i: nat)
    | EFunction(id: string, args: seq<Expr>)

  type Pred = Expr

  datatype AggKind = AggCountStar | AggCount | AggSum | AggAvg | AggMin | AggMax
  datatype AggSpec = AggSpec(kind: AggKind, expr: OptionExpr)
  datatype OptionExpr = NoExpr | SomeExpr(e: Expr)

  datatype SortDir = SortAsc | SortDesc
  datatype NullOrder = NullsFirst | NullsLast
  datatype OrderKey = OrderKey(expr: Expr, dir: SortDir, nulls: NullOrder)

  datatype JoinKind = JoinInner | JoinLeft | JoinRight | JoinFull | JoinCross

  datatype RelNode =
    | RelScan(table: TableId)
    | RelFilter(input: RelNode, pred: Pred)
    | RelProject(input: RelNode, exprs: seq<Expr>)
    | RelJoin(kind: JoinKind, lft: RelNode, rgt: RelNode, cond: Pred)
    | RelAggregate(input: RelNode, groupExprs: seq<Expr>, aggs: seq<AggSpec>)
    | RelSort(input: RelNode, orderKeys: seq<OrderKey>)
    | RelDistinct(input: RelNode)
    | RelLimit(input: RelNode, offset: nat, count: nat)

  type LogicalSpec = RelNode
}
