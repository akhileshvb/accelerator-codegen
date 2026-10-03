// The proof library: lemmas shared by Verus kernels, written by the pipeline's
// agents (seeded empty).  Rules: a lemma is general (no query names; stated
// over the spec vocabulary of verus_common or general structures), verifies
// on its own without assume, says when to use it in a doc comment, and
// records where it came from.  Adding one re-verifies every kernel that uses
// the library (pipeline tool prooflib_publish).
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::bytes::*;

verus! {

pub open spec fn int_bytes_lt(left: (int, Seq<u8>), right: (int, Seq<u8>)) -> bool {
    left.0 < right.0 || (left.0 == right.0 && bytes_lt(left.1, right.1))
}

/// Use for ordered indexes whose composite keys contain an integer and C-collated bytes.
pub proof fn lemma_int_bytes_order(left: (int, Seq<u8>), right: (int, Seq<u8>))
    ensures
        !int_bytes_lt(left, left),
        !(int_bytes_lt(left, right) && int_bytes_lt(right, left)),
        left != right ==> int_bytes_lt(left, right) || int_bytes_lt(right, left),
{
    lemma_bytes_lt_irrefl(left.1);
    lemma_bytes_lt_asym(left.1, right.1);
    if left.0 == right.0 && left != right {
        if left.1 == right.1 { assert(left == right); }
        lemma_bytes_lt_total(left.1, right.1);
    }
}

/// Use when an ordered-tree rotation or a range bound needs composite-key transitivity.
pub proof fn lemma_int_bytes_trans(first: (int, Seq<u8>), middle: (int, Seq<u8>), last: (int, Seq<u8>))
    requires int_bytes_lt(first, middle), int_bytes_lt(middle, last),
    ensures int_bytes_lt(first, last),
{
    if first.0 == middle.0 && middle.0 == last.0 {
        lemma_bytes_lt_trans(first.1, middle.1, last.1);
    }
}

} // verus!
