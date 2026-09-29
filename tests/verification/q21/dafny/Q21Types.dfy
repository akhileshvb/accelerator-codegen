// Q21 typed database view + embedding into the generic RelNode DB.
//
// TDB is the typed content of the four tables Q21 reads, restricted to the
// columns the Q21 LogicalSpec references (same column order as the Rocq
// Q21Schema / RelNode scans):
//   supplier(s_suppkey, s_name, s_nationkey)
//   lineitem(l_orderkey, l_suppkey, l_commitdate, l_receiptdate)
//   orders(o_orderkey, o_orderstatus)
//   nation(n_nationkey, n_name)
// ToRel(T) is the RelNode DB the Rocq Stage-2 theorems quantify over
// (row shapes = Rocq row_ok_* ; o_orderstatus a one-character string).

include "../../logical/dafny/LogicalSpec.dfy"

module Q21Types {
  import opened LogicalSpec

  datatype Sup = Sup(sk: int, name: string, nk: int)
  datatype Nat = Nat(nk: int, name: string)
  datatype Ord = Ord(ok: int, st: char)
  datatype Li = Li(ok: int, sk: int, cd: int, rd: int)
  // Per-supplier lineitem counts of one order (C++ SupplierOrderInfo).
  datatype SOI = SOI(total: int, late: int)

  datatype TDB = TDB(sup: seq<Sup>, li: seq<Li>, ords: seq<Ord>, nats: seq<Nat>)

  function SupRow(s: Sup): Row { [VInt(s.sk), VText(s.name), VInt(s.nk)] }
  function LiRow(l: Li): Row { [VInt(l.ok), VInt(l.sk), VInt(l.cd), VInt(l.rd)] }
  function OrdRow(o: Ord): Row { [VInt(o.ok), VText([o.st])] }
  function NatRow(n: Nat): Row { [VInt(n.nk), VText(n.name)] }

  function SupRows(xs: seq<Sup>): seq<Row>
  { if |xs| == 0 then [] else [SupRow(xs[0])] + SupRows(xs[1..]) }
  function LiRows(xs: seq<Li>): seq<Row>
  { if |xs| == 0 then [] else [LiRow(xs[0])] + LiRows(xs[1..]) }
  function OrdRows(xs: seq<Ord>): seq<Row>
  { if |xs| == 0 then [] else [OrdRow(xs[0])] + OrdRows(xs[1..]) }
  function NatRows(xs: seq<Nat>): seq<Row>
  { if |xs| == 0 then [] else [NatRow(xs[0])] + NatRows(xs[1..]) }

  function ToRel(T: TDB): DB
  {
    [("supplier", SupRows(T.sup)), ("lineitem", LiRows(T.li)),
     ("orders", OrdRows(T.ords)), ("nation", NatRows(T.nats))]
  }

  function Q21Params(nation_name: string): Params
  {
    [("nation_name", VText(nation_name))]
  }

  predicate Late(l: Li) { l.rd > l.cd }

  // ----- Accelerator-contract premises on the typed database -----
  //
  // Primary keys of the TPC-H schema (the SQL answer multiplies by duplicate
  // key rows; the accelerator keys its maps by these columns).
  ghost predicate SupKeyUnique(T: TDB)
  { forall i, j :: 0 <= i < j < |T.sup| ==> T.sup[i].sk != T.sup[j].sk }
  ghost predicate NatKeyUnique(T: TDB)
  { forall i, j :: 0 <= i < j < |T.nats| ==> T.nats[i].nk != T.nats[j].nk }
  ghost predicate OrdKeyUnique(T: TDB)
  { forall i, j :: 0 <= i < j < |T.ords| ==> T.ords[i].ok != T.ords[j].ok }
  // Foreign key lineitem.l_orderkey -> orders.o_orderkey.
  ghost predicate LiOrdRI(T: TDB)
  { forall l :: l in T.li ==> exists o :: o in T.ords && o.ok == l.ok }

  ghost predicate KeysOk(T: TDB)
  { SupKeyUnique(T) && NatKeyUnique(T) && OrdKeyUnique(T) && LiOrdRI(T) }
}
