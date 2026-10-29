// A stored group of the answer: its keys, exact count and sum, and the double the answer reports.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::*;
use verus_prooflib::float::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::wideval::*;
use crate::f64bits::*;
use crate::rep::*;

verus! {

pub struct Grp {
    pub name: Option<Vec<u8>>,
    pub tag: Vec<u8>,
    pub plabel: Option<Vec<u8>>,
    pub cnt: Wide,
    pub sum: Wide,
    pub bits: u64,
}

impl Grp {
    pub open spec fn ik(self) -> KeyView {
        ikey(ov(self.name), self.tag@, ov(self.plabel))
    }
}

pub open spec fn max_val() -> int {
    (pow2(53) - 1) * pow2(2045)
}

/// The double `b` is the exact sum (one tuple) or a rounding of it.
pub open spec fn okbits(b: u64, s: int, n: int) -> bool {
    &&& f64_finite(b)
    &&& if n == 1 { f64_val(b) == s } else { f64_rounded(b, s) }
}

pub open spec fn bits_ok(g: Grp) -> bool {
    okbits(g.bits, g.sum@, g.cnt@)
}

/// A group's place in its class's rank order: larger totals first, then the group's key.
pub open spec fn rkey(g: Grp) -> KeyView {
    (f64_desc_key(g.bits), g.ik().1)
}

pub open spec fn rkey_gk(b: u64, k: GK) -> KeyView {
    (f64_desc_key(b), ikey(k.name, k.tag, k.plabel).1)
}

pub fn rkey_of(bits: u64, ik: &Key) -> (k: Key)
    ensures k.view() == (f64_desc_key(bits), ik.view().1),
{
    Key::shared(desc_key(bits), ik.text.clone())
}

/// The double of a group with count c and exact sum s: its bits, None when s is out of the double range.
pub fn grp_bits(tab: &Pow2Tab, maxv: &Wide, minv: &Wide, c: &Wide, s: &Wide, Ghost(db): Ghost<Db>, Ghost(k): Ghost<GK>) -> (r: Option<u64>)
    requires
        tab.ok(),
        maxv@ == max_val(), minv@ == -max_val(),
        rep_db(db),
        c@ == crate::model::cntk(db, k), s@ == crate::model::sumk(db, k), c@ > 0,
    ensures
        r matches Some(b) ==> okbits(b, s@, c@),
        r is None ==> (s@ > max_val() || s@ < -max_val()),
{
    proof { lemma_pow2_consts(); }
    let inr = !s.lt(minv) && !maxv.lt(s);
    if !inr {
        return None;
    }
    proof { assert(f64_in_range(s@)); }
    let one = Wide::from_i128(1);
    if c.eq(&one) {
        proof {
            lemma_single_rep(db, k);
            let x = choose|x: u64| f64_val(x) == crate::model::sumk(db, k);
            lemma_f64_in_range(x);
            assert(f64_finite(x));
        }
        let b = bits_exact(tab, s);
        Some(b)
    } else {
        let b = s.f64_bits();
        proof { lemma_f64_in_range(b); }
        Some(b)
    }
}

pub open spec fn arc_val<T>(a: &Arc<T>) -> T {
    **a
}

pub type OutT = (Option<Vec<u8>>, Option<Vec<u8>>, Option<Vec<u8>>, Option<Vec<u8>>, Option<u64>, i64);

pub fn cnt_i64(c: &Wide) -> (r: Option<i64>)
    ensures
        r matches Some(v) ==> v as int == c@,
        r is None ==> !(-9223372036854775808 <= c@ <= 9223372036854775807),
{
    match c.to_i128() {
        Some(x) => if x >= -9223372036854775808i128 && x <= 9223372036854775807i128 { Some(x as i64) } else { None },
        None => None,
    }
}

} // verus!
