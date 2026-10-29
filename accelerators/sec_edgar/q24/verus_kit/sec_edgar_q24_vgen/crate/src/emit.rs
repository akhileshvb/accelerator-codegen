// An answer row from a stored group: the exact sum rounded to a double.
#![allow(unused_imports)]
#![verifier::loop_isolation(false)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::*;
use verus_prooflib::float::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::gmap::*;
use crate::wideval::*;
use crate::f64bits::*;
use crate::rank::*;

verus! {

pub struct OutR {
    pub tag: Option<Vec<u8>>,
    pub ver: Option<Vec<u8>>,
    pub cnt: Wide,
    /// cnt, for the sort key
    pub ci: i128,
    /// the chosen total, exactly (the double's value, or the exact sum when out of range)
    pub vw: Wide,
    pub bits: Option<u64>,
}

pub open spec fn rview(r: OutR) -> OutRow {
    OutRow { tag: ov(r.tag), version: ov(r.ver), cnt: r.cnt@, total: Some(r.vw@) }
}

pub open spec fn rik(r: OutR) -> KeyView {
    ikey(ov(r.tag), ov(r.ver))
}

pub open spec fn rk(r: OutR) -> KeyView {
    rkey(r.ci, rik(r))
}

pub open spec fn max_val() -> int {
    (pow2(53) - 1) * pow2(2045)
}

pub open spec fn bits_ok(r: OutR) -> bool {
    match r.bits {
        Some(b) => f64_val(b) == r.vw@ && f64_in_range(r.vw@),
        None => !f64_in_range(r.vw@),
    }
}

/// A row of the answer from a stored group with more than 10 rows.
pub fn make_row(tab: &Pow2Tab, maxv: &Wide, minv: &Wide, g: &Grp, ci: i128, Ghost(db): Ghost<Db>, Ghost(k): Ghost<GK>) -> (r: OutR)
    requires
        tab.ok(),
        maxv@ == max_val(), minv@ == -max_val(),
        g.cnt@ > 10, g.cnt@ == cntk(db, k), g.sum@ == sumk(db, k),
        ci as int == g.cnt@,
        k.tag == ov(g.tag), k.ver == ov(g.ver),
    ensures
        ov(r.tag) == ov(g.tag), ov(r.ver) == ov(g.ver), r.cnt@ == g.cnt@, r.ci == ci,
        float_within(r.vw@, sumk(db, k), absk(db, k), cntk(db, k) - 1),
        bits_ok(r),
{
    proof { lemma_pow2_consts(); lemma_cnt_facts(db, k); }
    let e = &g.sum;
    let inr = !e.lt(minv) && !maxv.lt(e);
    let tag = copy_opt(&g.tag);
    let ver = copy_opt(&g.ver);
    let cnt = g.cnt.dup();
    if inr {
        proof { assert(f64_in_range(e@)); }
        let b0 = e.f64_bits();
        let vw = wide_of_bits(tab, b0);
        proof { lemma_rounded_within(b0, e@, absk(db, k), cntk(db, k) - 1); }
        OutR { tag, ver, cnt, ci, vw, bits: Some(b0) }
    } else {
        proof { assert(!f64_in_range(e@)); }
        OutR { tag, ver, cnt, ci, vw: e.dup(), bits: None }
    }
}

} // verus!
