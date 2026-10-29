// The state's entries: per group an exact count, sum and absolute sum, and the rounded total;
// the maps of them per parameter set (a class) and the invariant tying them to the database.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::float::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::exact::*;
use crate::f64bits::*;
use crate::gen_spec::*;
use crate::gkey::*;
use crate::numeric::*;
use crate::qty::*;
use crate::rank::*;

verus! {

pub struct Ent {
    pub name: Option<Vec<u8>>,
    pub cik: Option<i64>,
    pub count: Wide,
    pub sum: Wide,
    pub abs: Wide,
    pub cv: Wide,
    pub bits: Option<u64>,
}

pub fn copy_name(name: &Option<Vec<u8>>) -> (r: Option<Vec<u8>>)
    ensures opt_bytes(r) == opt_bytes(*name),
{
    match name {
        Some(b) => Some(vstd::slice::slice_to_vec(b.as_slice())),
        None => None,
    }
}

/// cv is the double `bits` that nearest-rounds x, or x itself when x is no double.
pub open spec fn rounded_as(x: int, cv: int, bits: Option<u64>) -> bool {
    match bits {
        Some(b) => f64_rounded(b, x) && f64_finite(b) && cv == f64_val(b),
        None => !f64_in_range(x) && cv == x,
    }
}

/// The entry holds the group's quantities and its rounded total (exactly the sum for a single tuple).
pub open spec fn ent_ok(e: Ent, q: Q) -> bool {
    &&& e.count@ == q.n
    &&& q.n >= 1
    &&& e.sum@ == q.s
    &&& e.abs@ == q.a
    &&& rounded_as(e.sum@, e.cv@, e.bits)
    &&& q.n == 1 ==> e.cv@ == e.sum@
}

/// The key of a group's entry: companies (`inner`) have no name.
pub open spec fn ek(inner: bool, gn: Option<Seq<u8>>, gc: Option<int>) -> KeyView {
    gk(if inner { None } else { gn }, gc)
}

pub open spec fn ent_names(inner: bool, e: Ent, gn: Option<Seq<u8>>, gc: Option<int>) -> bool {
    opt_bytes(e.name) == (if inner { None::<Seq<u8>> } else { gn }) && opt_int(e.cik) == gc
}

/// The map holds an entry for every group of the truth tf with a tuple, and for no other.
pub open spec fn tmap_ok_f(inner: bool, m: Index<Ent>, tf: spec_fn(Option<Seq<u8>>, Option<int>) -> Q) -> bool {
    &&& m.wf()
    &&& forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is Some
        ==> ent_ok(m.at(ek(inner, gn, gc))->0, tf(gn, gc)) && ent_names(inner, m.at(ek(inner, gn, gc))->0, gn, gc)
    &&& forall|gn: Option<Seq<u8>>, gc: Option<int>| #[trigger] m.at(ek(inner, gn, gc)) is None ==> tf(gn, gc).n == 0
    &&& forall|k: KeyView| #[trigger] m.at(k) is Some ==>
        k == gk(opt_bytes(m.at(k)->0.name), opt_int(m.at(k)->0.cik)) && (inner ==> m.at(k)->0.name is None)
}

pub open spec fn tf_of(db: Db, p: Params, inner: bool) -> spec_fn(Option<Seq<u8>>, Option<int>) -> Q {
    |gn: Option<Seq<u8>>, gc: Option<int>| qj(db, p, inner, gn, gc)
}

pub open spec fn cvw() -> spec_fn((KeyView, Ent)) -> int {
    |row: (KeyView, Ent)| row.1.cv@
}

pub open spec fn onew() -> spec_fn((KeyView, Ent)) -> int {
    |row: (KeyView, Ent)| 1int
}

/// The state of one parameter set: its companies and groups, and the sum of the companies' rounded totals.
pub struct Class {
    pub cmap: Index<Ent>,
    pub omap: Index<Ent>,
    pub sr: Wide,
    pub k: Wide,
    pub ord: Index<Ent>,
    pub oor: Wide,
}

pub open spec fn cls_ok(c: Class, db: Db, p: Params) -> bool {
    &&& tmap_ok_f(true, c.cmap, tf_of(db, p, true))
    &&& tmap_ok_f(false, c.omap, tf_of(db, p, false))
    &&& c.sr@ == sum(c.cmap.entries(), cvw())
    &&& c.k@ == sum(c.cmap.entries(), onew())
    &&& ord_ok(c.omap, c.ord)
    &&& c.oor@ == sum(c.omap.entries(), oorw())
}

pub open spec fn zero_p(db: Db, p: Params) -> bool {
    forall|gn: Option<Seq<u8>>, gc: Option<int>| qj(db, p, true, gn, gc).n == 0 && qj(db, p, false, gn, gc).n == 0
}

pub open spec fn ckey(fy: int, uom: Seq<u8>) -> KeyView {
    (fy, uom)
}

pub open spec fn cp(fy: i64, uom: Seq<u8>) -> Params {
    Params { p1: fy as int, p2: uom }
}

pub open spec fn agg_ok(cs: Index<Class>, db: Db) -> bool {
    &&& cs.wf()
    &&& forall|fy: i64, uom: Seq<u8>| #[trigger] cs.at(ckey(fy as int, uom)) is Some ==> cls_ok(cs.at(ckey(fy as int, uom))->0, db, cp(fy, uom))
    &&& forall|fy: i64, uom: Seq<u8>| #[trigger] cs.at(ckey(fy as int, uom)) is None ==> zero_p(db, cp(fy, uom))
}

pub fn class_key(fy: i64, uom: &[u8]) -> (k: Key)
    ensures k.view() == ckey(fy as int, uom@),
{
    Key::composite(fy, vstd::slice::slice_to_vec(uom))
}

pub fn ent_key(inner: bool, name: &Option<Vec<u8>>, cik: Option<i64>) -> (k: Key)
    ensures k.view() == gk(if inner { None } else { opt_bytes(*name) }, opt_int(cik)),
{
    if inner { gk_key(&None, cik) } else { gk_key(name, cik) }
}

/// The entry of a group with these exact quantities.
pub fn ent_make(name: Option<Vec<u8>>, cik: Option<i64>, count: Wide, sum: Wide, mags: Wide, c: &Consts, Ghost(q): Ghost<Q>) -> (e: Ent)
    requires
        consts_ok(*c),
        count@ == q.n,
        q.n >= 1,
        sum@ == q.s,
        mags@ == q.a,
        q.n == 1 && f64_in_range(q.s) ==> dbl_raw(q.s),
    ensures
        ent_ok(e, q),
        opt_bytes(e.name) == opt_bytes(name),
        e.cik == cik,
{
    let single = count.eq(&Wide::from_i128(1));
    let (cv, bits) = if single {
        if in_range(&sum, c) {
            let b = exact_bits(&sum, c);
            proof {
                lemma_f64_in_range(b);
                assert(f64_val(b) == sum@);
                assert(abs(f64_val(b) - sum@) == 0);
                assert(pow2(53) * abs(f64_val(b) - sum@) == 0) by (nonlinear_arith) requires abs(f64_val(b) - sum@) == 0;
                assert(f64_rounded(b, sum@));
            }
            (sum.dup(), Some(b))
        } else {
            (sum.dup(), None)
        }
    } else {
        round_value(&sum, c)
    };
    Ent { name, cik, count, sum, abs: mags, cv, bits }
}

/// The rounded total is within the bound of the sum of its group's magnitudes.
pub proof fn lemma_ent_within(e: Ent, q: Q)
    requires ent_ok(e, q), abs(q.s) <= q.a,
    ensures float_within(e.cv@, e.sum@, e.abs@, e.count@ - 1),
{
    if q.n >= 2 {
        match e.bits {
            Some(b) => { lemma_rounded_within(b, e.sum@, e.abs@, e.count@ - 1); },
            None => {},
        }
    }
}


pub proof fn lemma_ent_finite(e: Ent, q: Q)
    requires ent_ok(e, q),
    ensures e.bits matches Some(b) ==> f64_finite(b),
{
}

}
