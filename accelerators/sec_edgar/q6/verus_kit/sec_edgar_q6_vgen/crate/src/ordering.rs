// The ORDER BY of the spec on rows with a value and a tag is a strict total order on their keys.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::bytes::*;
use crate::gen_spec::*;

verus! {

pub open spec fn opt_lt(x: Option<Seq<u8>>, y: Option<Seq<u8>>) -> bool {
    (y is None && x is Some) || (x is Some && y is Some && bytes_lt(x->0, y->0))
}

pub proof fn lemma_opt_order(x: Option<Seq<u8>>, y: Option<Seq<u8>>, z: Option<Seq<u8>>)
    ensures
        !opt_lt(x, x),
        opt_lt(x, y) ==> !opt_lt(y, x),
        opt_lt(x, y) && opt_lt(y, z) ==> opt_lt(x, z),
        x != y ==> opt_lt(x, y) || opt_lt(y, x),
{
    if x is Some { lemma_bytes_lt_irrefl(x->0); }
    if x is Some && y is Some {
        lemma_bytes_lt_asym(x->0, y->0);
        if x->0 != y->0 { lemma_bytes_lt_total(x->0, y->0); }
    }
    if x is Some && y is Some && z is Some && bytes_lt(x->0, y->0) && bytes_lt(y->0, z->0) { lemma_bytes_lt_trans(x->0, y->0, z->0); }
}

/// Rows the kernel emits: a value and a tag.
pub open spec fn crow(r: OutRow) -> bool {
    r.total_value is Some && r.tag is Some
}

pub open spec fn keyeq(a: OutRow, b: OutRow) -> bool {
    a.total_value == b.total_value && a.name == b.name && a.tag == b.tag && a.plabel == b.plabel
}

pub proof fn lemma_row_lt_order(a: OutRow, b: OutRow, c: OutRow)
    requires crow(a), crow(b), crow(c),
    ensures
        !row_lt(a, a),
        row_lt(a, b) ==> !row_lt(b, a),
        row_lt(a, b) && row_lt(b, c) ==> row_lt(a, c),
        !keyeq(a, b) ==> row_lt(a, b) || row_lt(b, a),
        keyeq(a, b) ==> (row_lt(a, c) == row_lt(b, c)) && (row_lt(c, a) == row_lt(c, b)),
{
    lemma_opt_order(a.name, b.name, c.name);
    lemma_opt_order(a.plabel, b.plabel, c.plabel);
    lemma_bytes_lt_irrefl(a.tag->0);
    lemma_bytes_lt_asym(a.tag->0, b.tag->0);
    if a.tag->0 != b.tag->0 { lemma_bytes_lt_total(a.tag->0, b.tag->0); }
    if bytes_lt(a.tag->0, b.tag->0) && bytes_lt(b.tag->0, c.tag->0) { lemma_bytes_lt_trans(a.tag->0, b.tag->0, c.tag->0); }
    lemma_bytes_lt_irrefl(b.tag->0);
    lemma_bytes_lt_asym(b.tag->0, a.tag->0);
}

/// Negative transitivity: the incomparability of rows is an equivalence.
pub proof fn lemma_row_le_trans(a: OutRow, b: OutRow, c: OutRow)
    requires crow(a), crow(b), crow(c), !row_lt(b, a), !row_lt(c, b),
    ensures !row_lt(c, a),
{
    lemma_row_lt_order(a, b, c);
    lemma_row_lt_order(b, a, c);
    lemma_row_lt_order(c, a, b);
    lemma_row_lt_order(c, b, a);
    if keyeq(a, b) {
    } else {
        assert(row_lt(a, b));
        if row_lt(c, a) { assert(row_lt(c, b)); }
    }
}

} // verus!
