// The bounded state of the kernel: per class (fy, stmt, uom) the best groups in answer order (r),
// and for the rest each unit's (fy, name) best group (f, fb), with the invariant the answer is read from.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt};
use verus_prooflib::wide::*;
use verus_prooflib::float::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::f64bits::*;
use crate::grp::*;
use crate::engine::*;

verus! {

/// Answer entries kept per class at most; below LIMIT a class is refilled to REFILL units.
/// K_LIMIT is the query's LIMIT.  K_MAX and K_REFILL are tuning constants; the proofs hold for any K_MAX >= K_LIMIT and
/// K_REFILL >= K_LIMIT (a refill must bring a class back to K_LIMIT groups).  The kit tests also run a build with the smallest
/// values (K_MAX 203, K_REFILL 200) so that trim and refill happen on most classes.
pub const K_MAX: usize = 1024;
pub const K_LIMIT: usize = 200;
pub const K_REFILL: usize = 512;
pub open spec fn k_max() -> int { K_MAX as int }
pub open spec fn k_limit() -> int { K_LIMIT as int }
pub open spec fn k_refill() -> int { K_REFILL as int }

pub struct ClassRec {
    pub fy: i64,
    pub stmt: Vec<u8>,
    pub uom: Vec<u8>,
    /// the class's best groups in answer order, by rkey
    pub r: Index<Grp>,
    /// each unit's best group outside r, by rkey
    pub f: Index<Grp>,
    /// the same groups by unit
    pub fb: Index<Grp>,
    /// units with a group whose sum is above / below the double range
    pub hp: Index<()>,
    pub hn: Index<()>,
}

pub open spec fn cls_key(c: ClassRec) -> KeyView {
    okey(c.fy as int, c.stmt@, c.uom@)
}

pub open spec fn cls_gk(c: ClassRec, g: Grp) -> GK {
    GK { fy: c.fy as int, stmt: c.stmt@, uom: c.uom@, name: ov(g.name), tag: g.tag@, plabel: ov(g.plabel) }
}

pub open spec fn in_class(c: ClassRec, k: GK) -> bool {
    k.fy == c.fy as int && k.stmt == c.stmt@ && k.uom == c.uom@
}

pub open spec fn nkey_v(name: Option<Seq<u8>>) -> KeyView {
    (0int, esc_opt(name))
}

/// The group's sum and count are the database's, inside the double range, and its double is a rounding of the sum.
pub open spec fn exact_g(db: Db, c: ClassRec, g: Grp) -> bool {
    let k = cls_gk(c, g);
    &&& g.cnt@ == cntk(db, k)
    &&& g.sum@ == sumk(db, k)
    &&& g.cnt@ > 0
    &&& f64_in_range(g.sum@)
    &&& bits_ok(g)
    &&& !unsafe_a(absk(db, k))
}

pub open spec fn crec_struct(c: ClassRec) -> bool {
    &&& c.r.wf() && c.f.wf() && c.fb.wf() && c.hp.wf() && c.hn.wf()
    &&& forall|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some ==> rk == rkey(c.r.at(rk)->0)
    &&& forall|rk: KeyView| #![trigger c.f.at(rk)] c.f.at(rk) is Some ==> {
        &&& rk == rkey(c.f.at(rk)->0)
        &&& c.fb.at(nkey_v(ov(c.f.at(rk)->0.name))) == c.f.at(rk)
    }
    &&& forall|nk: KeyView| #![trigger c.fb.at(nk)] c.fb.at(nk) is Some ==> {
        &&& nk == nkey_v(ov(c.fb.at(nk)->0.name))
        &&& c.f.at(rkey(c.fb.at(nk)->0)) == c.fb.at(nk)
    }
    &&& forall|r1: KeyView, r2: KeyView| #![trigger c.r.at(r1), c.f.at(r2)] c.r.at(r1) is Some && c.f.at(r2) is Some ==> key_lt(r1, r2)
    &&& forall|r1: KeyView, r2: KeyView| #![trigger c.r.at(r1), c.r.at(r2)]
        c.r.at(r1) is Some && c.r.at(r2) is Some && c.r.at(r1)->0.ik() == c.r.at(r2)->0.ik() ==> r1 == r2
    &&& forall|r1: KeyView, r2: KeyView| #![trigger c.r.at(r1), c.f.at(r2)]
        c.r.at(r1) is Some && c.f.at(r2) is Some ==> c.r.at(r1)->0.ik() != c.f.at(r2)->0.ik()
    &&& forall|nk: KeyView| #![trigger c.hp.at(nk)] c.hp.at(nk) is Some ==> nk.0 == 0
    &&& forall|nk: KeyView| #![trigger c.hn.at(nk)] c.hn.at(nk) is Some ==> nk.0 == 0
}

/// A group the answer cannot have: its sum is above the double range, or the engine would raise on it.
pub open spec fn bad_g(s: int, a: int) -> bool {
    s > max_val() || unsafe_a(a)
}

pub open spec fn has_cand(db: Db, c: ClassRec, nk: KeyView, above: bool) -> bool {
    exists|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 && in_class(c, k) && nkey_v(k.name) == nk
        && (if above { bad_g(sumk(db, k), absk(db, k)) } else { sumk(db, k) < -max_val() })
}

pub open spec fn crec_sem(db: Db, c: ClassRec) -> bool {
    &&& forall|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some ==> exact_g(db, c, c.r.at(rk)->0)
    &&& forall|rk: KeyView| #![trigger c.f.at(rk)] c.f.at(rk) is Some ==> exact_g(db, c, c.f.at(rk)->0)
    &&& forall|nk: KeyView| #![trigger c.hp.at(nk)] c.hp.at(nk) is Some ==> has_cand(db, c, nk, true)
    &&& forall|nk: KeyView| #![trigger c.hn.at(nk)] c.hn.at(nk) is Some ==> has_cand(db, c, nk, false)
}

/// The class keeps LIMIT groups, or all of its groups.
pub open spec fn crec_full(c: ClassRec) -> bool {
    c.r.entries().len() >= k_limit() || c.f.entries().len() == 0
}

pub open spec fn r_has(c: ClassRec, k: GK) -> bool {
    exists|rk: KeyView| #![trigger c.r.at(rk)] c.r.at(rk) is Some && cls_gk(c, c.r.at(rk)->0) == k
}

/// Group k (sum s, count n) is accounted for in its class: kept, or bounded by its unit's frontier group.
pub open spec fn covered_in(c: ClassRec, k: GK, s: int, n: int, a: int) -> bool {
    if bad_g(s, a) { c.hp.at(nkey_v(k.name)) is Some }
    else if s < -max_val() { c.hn.at(nkey_v(k.name)) is Some }
    else {
        r_has(c, k) || exists|b: u64| #![trigger okbits(b, s, n)] okbits(b, s, n) && c.fb.at(nkey_v(k.name)) is Some
            && !key_lt(rkey_gk(b, k), rkey(c.fb.at(nkey_v(k.name))->0))
    }
}

pub open spec fn covered(db: Db, cls: Index<ClassRec>, k: GK) -> bool {
    match cls.at(okey(k.fy, k.stmt, k.uom)) {
        Some(c) => covered_in(c, k, sumk(db, k), cntk(db, k), absk(db, k)),
        None => false,
    }
}

pub open spec fn cls_ok(db: Db, cls: Index<ClassRec>) -> bool {
    &&& cls.wf()
    &&& forall|ok: KeyView| #![trigger cls.at(ok)] cls.at(ok) is Some ==> {
        let c = cls.at(ok)->0;
        &&& ok == cls_key(c)
        &&& crec_struct(c)
        &&& crec_sem(db, c)
        &&& crec_full(c)
    }
    &&& forall|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 ==> covered(db, cls, k)
}

} // verus!
