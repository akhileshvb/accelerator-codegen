// q_ref: the reference meaning of the catalog SQL for TPC-H Q1.
//
// THIS FILE IS THE TRUSTED SPECIFICATION.  Everything else in the crate is
// proved against it.  Review it against the SQL below; nothing checks that
// correspondence mechanically.
//
//   SELECT l_returnflag, l_linestatus,
//          SUM(l_quantity)                                        AS sum_qty,
//          SUM(l_extendedprice)                                   AS sum_base_price,
//          SUM(l_extendedprice * (1 - l_discount))                AS sum_disc_price,
//          SUM(l_extendedprice * (1 - l_discount) * (1 + l_tax))  AS sum_charge,
//          AVG(l_quantity)                                        AS avg_qty,
//          AVG(l_extendedprice)                                   AS avg_price,
//          AVG(l_discount)                                        AS avg_disc,
//          COUNT(*)                                               AS count_order
//   FROM lineitem
//   WHERE l_shipdate <= DATE '1998-12-01' - INTERVAL ':1' DAY
//   GROUP BY l_returnflag, l_linestatus
//   ORDER BY l_returnflag, l_linestatus
//
// Semantic decisions (each is part of what a reviewer signs off on):
//  * The table is a bag: a sequence of rows, duplicates count separately.
//  * Every column Q1 reads is NOT NULL (true of the schema), so there is no
//    three-valued logic.
//  * DECIMAL(15,2) values are their exact integer number of hundredths
//    (l_quantity = 17.00 is 1700).  Hence, exactly:
//      l_extendedprice * (1 - l_discount)              = disc_price(l) / 10^4
//      l_extendedprice * (1 - l_discount) * (1 + l_tax) = charge(l)     / 10^6
//    and the SUM columns are the group sums of these integers at scales
//    2, 2, 4 and 6.
//  * The date bound is a day number `cutoff` (the host computes
//    1998-12-01 - :1 days in its own day encoding and passes shipdates in the
//    same encoding); `<=` on dates is `<=` on day numbers.
//  * Text is a byte string; GROUP BY equality is byte equality and ORDER BY is
//    byte-wise lexicographic (C collation), l_returnflag first.
//  * The AVG columns are exact rationals sum/count of a group's
//    sum_qty / sum_base_price / sum_disc and count.  Rounding them to the
//    engine's AVG result type is host formatting (trusted, per engine).
//  * A group is present iff its COUNT(*) is positive.
use vstd::prelude::*;
pub use verus_common::bytes::bytes_lt;
use verus_common::sums::*;

verus! {

pub struct Lineitem {
    pub shipdate: int,
    pub quantity: int,
    pub extendedprice: int,
    pub discount: int,
    pub tax: int,
    pub returnflag: Seq<u8>,
    pub linestatus: Seq<u8>,
}

pub open spec fn qualifies(l: Lineitem, cutoff: int) -> bool {
    l.shipdate <= cutoff
}

pub open spec fn in_group(l: Lineitem, cutoff: int, rf: Seq<u8>, ls: Seq<u8>) -> bool {
    qualifies(l, cutoff) && l.returnflag == rf && l.linestatus == ls
}

/// l_extendedprice * (1 - l_discount), in units of 10^-4.
pub open spec fn disc_price(l: Lineitem) -> int {
    l.extendedprice * (100 - l.discount)
}

/// l_extendedprice * (1 - l_discount) * (1 + l_tax), in units of 10^-6.
pub open spec fn charge(l: Lineitem) -> int {
    l.extendedprice * (100 - l.discount) * (100 + l.tax)
}

pub open spec fn f_qty() -> spec_fn(Lineitem) -> int {
    |l: Lineitem| l.quantity
}

pub open spec fn f_base() -> spec_fn(Lineitem) -> int {
    |l: Lineitem| l.extendedprice
}

pub open spec fn f_disc_price() -> spec_fn(Lineitem) -> int {
    |l: Lineitem| disc_price(l)
}

pub open spec fn f_charge() -> spec_fn(Lineitem) -> int {
    |l: Lineitem| charge(l)
}

pub open spec fn f_disc() -> spec_fn(Lineitem) -> int {
    |l: Lineitem| l.discount
}

pub open spec fn f_one() -> spec_fn(Lineitem) -> int {
    |l: Lineitem| 1int
}

/// Σ f(l) over the rows of group (rf, ls) that pass the WHERE clause.
pub open spec fn gsum(
    db: Seq<Lineitem>,
    cutoff: int,
    rf: Seq<u8>,
    ls: Seq<u8>,
    f: spec_fn(Lineitem) -> int,
) -> int {
    sum(db, |l: Lineitem| if in_group(l, cutoff, rf, ls) { f(l) } else { 0 })
}

/// One output row, before the AVG columns are formatted (avg_x = sum_x / count).
pub struct Group {
    pub rf: Seq<u8>,
    pub ls: Seq<u8>,
    pub sum_qty: int,
    pub sum_base_price: int,
    pub sum_disc_price: int,
    pub sum_charge: int,
    pub sum_disc: int,
    pub count: int,
}

pub open spec fn group_ok(db: Seq<Lineitem>, cutoff: int, g: Group) -> bool {
    &&& g.count > 0
    &&& g.count == gsum(db, cutoff, g.rf, g.ls, f_one())
    &&& g.sum_qty == gsum(db, cutoff, g.rf, g.ls, f_qty())
    &&& g.sum_base_price == gsum(db, cutoff, g.rf, g.ls, f_base())
    &&& g.sum_disc_price == gsum(db, cutoff, g.rf, g.ls, f_disc_price())
    &&& g.sum_charge == gsum(db, cutoff, g.rf, g.ls, f_charge())
    &&& g.sum_disc == gsum(db, cutoff, g.rf, g.ls, f_disc())
}

/// ORDER BY l_returnflag, l_linestatus.
pub open spec fn key_lt(a: Group, b: Group) -> bool {
    bytes_lt(a.rf, b.rf) || (a.rf == b.rf && bytes_lt(a.ls, b.ls))
}

/// `out` is the answer of Q1 on table `db` with date bound `cutoff`:
/// every row is a correct group, rows are in strictly increasing ORDER BY
/// order (so groups are distinct), and every non-empty group is present.
pub open spec fn q1_result(db: Seq<Lineitem>, cutoff: int, out: Seq<Group>) -> bool {
    &&& forall|i: int| 0 <= i < out.len() ==> group_ok(db, cutoff, #[trigger] out[i])
    &&& forall|i: int, j: int| 0 <= i < j < out.len() ==> key_lt(#[trigger] out[i], #[trigger] out[j])
    &&& forall|rf: Seq<u8>, ls: Seq<u8>|
        #[trigger] gsum(db, cutoff, rf, ls, f_one()) > 0 ==> exists|i: int|
            0 <= i < out.len() && (#[trigger] out[i]).rf == rf && out[i].ls == ls
}

} // verus!
