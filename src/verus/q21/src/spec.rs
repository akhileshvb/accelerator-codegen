// q_ref: the reference meaning of the catalog SQL for TPC-H Q21.
//
// THIS FILE IS THE TRUSTED SPECIFICATION.  Everything else in the crate is
// proved against it.  Review it against the SQL below; nothing checks that
// correspondence mechanically.
//
//   SELECT s_name, COUNT(*) AS numwait
//   FROM supplier, lineitem l1, orders, nation
//   WHERE s_suppkey = l1.l_suppkey
//     AND o_orderkey = l1.l_orderkey
//     AND o_orderstatus = 'F'
//     AND l1.l_receiptdate > l1.l_commitdate
//     AND EXISTS (SELECT * FROM lineitem l2
//                 WHERE l2.l_orderkey = l1.l_orderkey
//                   AND l2.l_suppkey <> l1.l_suppkey)
//     AND NOT EXISTS (SELECT * FROM lineitem l3
//                     WHERE l3.l_orderkey = l1.l_orderkey
//                       AND l3.l_suppkey <> l1.l_suppkey
//                       AND l3.l_receiptdate > l3.l_commitdate)
//     AND s_nationkey = n_nationkey
//     AND n_name = :nation
//   GROUP BY s_name
//   ORDER BY numwait DESC, s_name
//   LIMIT 100
//
// Semantic decisions (each is part of what a reviewer signs off on):
//  * Tables are bags: a table is a sequence of rows and duplicate rows count
//    separately.  No primary or foreign keys are assumed (the benchmark schema
//    declares none on orders and no foreign keys at all).
//  * Every column Q21 reads is NOT NULL (true of the schema), so rows carry
//    plain values and there is no three-valued logic.
//  * Only the columns Q21 reads are modelled.  `o_orderstatus = 'F'` is kept
//    as the boolean `status_is_f` (exact string equality with "F").
//  * Dates are day numbers; `>` on dates is `>` on day numbers.
//  * Text is a byte string (UTF-8 bytes).  Equality is byte equality.
//    ORDER BY s_name uses the C collation: byte-wise lexicographic, a proper
//    prefix sorts first (the runtime refuses to run unless s_name has a C
//    collation).
//  * s_name and n_name are CHAR(25).  A CHAR value is modelled as the bytes
//    the engine compares, and :nation as the bytes it is compared against.
//    HOST CONTRACT: the scans deliver s_name / n_name, and the host passes
//    :nation, in that form, so that byte equality and bytes_lt agree with the
//    engine's CHAR comparison.  An engine that pads CHAR with spaces but
//    ignores trailing spaces when comparing (PostgreSQL bpchar, MySQL CHAR)
//    is given the values with trailing spaces removed, which is its CHAR ->
//    text cast; :nation is passed unchanged.  DuckDB's CHAR is VARCHAR and is
//    passed unchanged.  Output names are in the same form.
//  * COUNT(*) of a group is the number of FROM-clause tuples satisfying WHERE
//    with that s_name; a group is present iff that number is positive.
//  * The result order is total: groups have distinct names, so (numwait DESC,
//    s_name) never ties and the output sequence is unique.  LIMIT 100 is
//    therefore the first 100 rows of that sequence (all rows if fewer).
use vstd::prelude::*;
pub use verus_common::bytes::bytes_lt;
use verus_common::sums::*;

verus! {

pub struct Supplier {
    pub suppkey: int,
    pub name: Seq<u8>,
    pub nationkey: int,
}

pub struct Nation {
    pub nationkey: int,
    pub name: Seq<u8>,
}

pub struct Order {
    pub orderkey: int,
    pub status_is_f: bool,
}

pub struct Lineitem {
    pub orderkey: int,
    pub suppkey: int,
    pub commitdate: int,
    pub receiptdate: int,
}

/// The four base tables, projected onto the columns Q21 reads.
pub struct Db {
    pub supplier: Seq<Supplier>,
    pub lineitem: Seq<Lineitem>,
    pub orders: Seq<Order>,
    pub nation: Seq<Nation>,
}

/// l_receiptdate > l_commitdate
pub open spec fn late(l: Lineitem) -> bool {
    l.receiptdate > l.commitdate
}

/// EXISTS (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey
///                                     AND l2.l_suppkey <> l1.l_suppkey)
pub open spec fn exists_l2(db: Db, l1: Lineitem) -> bool {
    exists|j: int|
        0 <= j < db.lineitem.len() && (#[trigger] db.lineitem[j]).orderkey == l1.orderkey
            && db.lineitem[j].suppkey != l1.suppkey
}

/// EXISTS (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey
///                                     AND l3.l_suppkey <> l1.l_suppkey
///                                     AND l3.l_receiptdate > l3.l_commitdate)
pub open spec fn exists_l3(db: Db, l1: Lineitem) -> bool {
    exists|j: int|
        0 <= j < db.lineitem.len() && (#[trigger] db.lineitem[j]).orderkey == l1.orderkey
            && db.lineitem[j].suppkey != l1.suppkey && late(db.lineitem[j])
}

/// The WHERE clause for one FROM-clause tuple (s, l1, o, n).
pub open spec fn where_clause(
    db: Db,
    nation_param: Seq<u8>,
    s: Supplier,
    l1: Lineitem,
    o: Order,
    n: Nation,
) -> bool {
    &&& s.suppkey == l1.suppkey
    &&& o.orderkey == l1.orderkey
    &&& o.status_is_f
    &&& late(l1)
    &&& exists_l2(db, l1)
    &&& !exists_l3(db, l1)
    &&& s.nationkey == n.nationkey
    &&& n.name == nation_param
}

/// COUNT(*) for the group s_name = g: the number of tuples of
/// supplier × lineitem × orders × nation (bag product) satisfyingg WHERE whose
/// s_name is g.
pub open spec fn numwait(db: Db, nation_param: Seq<u8>, g: Seq<u8>) -> int {
    sum(
        db.supplier,
        |s: Supplier|
            sum(
                db.lineitem,
                |l1: Lineitem|
                    sum(
                        db.orders,
                        |o: Order|
                            sum(
                                db.nation,
                                |n: Nation|
                                    ind(where_clause(db, nation_param, s, l1, o, n) && s.name == g),
                            ),
                    ),
            ),
    )
}

// bytes_lt (byte-wise lexicographic order = C collation, a proper prefix
// sorts first) is defined in verus_common::bytes and is part of this spec.
/// ORDER BY num wait DESC, s_name: row a sorts strictly before row b.
pub open spec fn row_before(a: (Seq<u8>, int), b: (Seq<u8>, int)) -> bool {
    a.1 > b.1 || (a.1 == b.1 && bytes_lt(a.0, b.0))
}

/// out =  the answer to Q21 without its LIMIT: every group, in ORDER BY
/// order.
pub open spec fn q21_full_result(db: Db, nation_param: Seq<u8>, out: Seq<(Seq<u8>, int)>) -> bool {
    // ORDER BY numwait DESC, s_name (strict: one row per group)
    &&& forall|i: int, j: int|
        0 <= i < j < out.len() ==> row_before(#[trigger] out[i], #[trigger] out[j])
    // each row is a group with its COUNT(*)
    &&& forall|i: int|
        0 <= i < out.len() ==> (#[trigger] out[i]).1 == numwait(db, nation_param, out[i].0)
            && out[i].1 > 0
    // every group appears
    &&& forall|g: Seq<u8>|
        #[trigger] numwait(db, nation_param, g) > 0 ==> exists|i: int|
            0 <= i < out.len() && (#[trigger] out[i]).0 == g
}

/// LIMIT 100
pub open spec fn q21_limit() -> int {
    100
}

/// LIMIT n: the first n rows, or all rows if there are fewer.
pub open spec fn limit_rows(rows: Seq<(Seq<u8>, int)>, n: int) -> Seq<(Seq<u8>, int)> {
    if rows.len() <= n {
        rows
    } else {
        rows.take(n)
    }
}

/// `out` is the answer to Q21 on `db` for :nation = nation_param.
pub open spec fn q21_result(db: Db, nation_param: Seq<u8>, out: Seq<(Seq<u8>, int)>) -> bool {
    exists|full: Seq<(Seq<u8>, int)>|
        #[trigger] q21_full_result(db, nation_param, full) && out == limit_rows(full, q21_limit())
}

} // verus!
