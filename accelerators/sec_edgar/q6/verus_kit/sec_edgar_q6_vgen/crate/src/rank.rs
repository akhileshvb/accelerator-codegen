// The rank index: per triple, the groups ordered by the answer's ORDER BY, kept next to the groups.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::*;
use verus_prooflib::float::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::gmap::*;
use crate::merge::*;
use crate::wideval::*;
use crate::f64bits::*;
use crate::glue_proofs::*;
use crate::rep::*;
use crate::answer::*;
use crate::emit::max_val;

verus! {

/// A group's place in its triple's rank index: larger totals first, then the group's key.
pub open spec fn rkey(g: Grp) -> KeyView {
    (f64_desc_key(g.bits), g.ik().1)
}

/// The group's bits are a finite double that is the exact sum (one tuple) or its nearest rounding.
pub open spec fn bits_ok(g: Grp) -> bool {
    &&& f64_finite(g.bits)
    &&& if g.cnt@ == 1 { f64_val(g.bits) == g.sum@ } else { f64_rounded(g.bits, g.sum@) }
}

/// `r` holds every group of `m` exactly once, under its rank key.
pub open spec fn rank_ok(m: GMap, r: GMap) -> bool {
    &&& forall|ok: KeyView, rk: KeyView| #![trigger gm_at(r, ok, rk)] gm_at(r, ok, rk) is Some ==> ({
        let g = gm_at(r, ok, rk)->0;
        &&& rk == rkey(g)
        &&& gm_at(m, ok, g.ik()) == Some(g)
        &&& bits_ok(g)
    })
    &&& forall|ok: KeyView, ik: KeyView| #![trigger gm_at(m, ok, ik)] gm_at(m, ok, ik) is Some ==>
        gm_at(r, ok, rkey(gm_at(m, ok, ik)->0)) == gm_at(m, ok, ik)
}

pub proof fn lemma_rkey_ik(g1: Grp, g2: Grp)
    requires rkey(g1) == rkey(g2),
    ensures g1.ik() == g2.ik(),
{
    assert(g1.ik() == (0int, g1.ik().1));
    assert(g2.ik() == (0int, g2.ik().1));
}

/// Replacing the group at (ok, ik) and its rank entry keeps the rank index complete and exact.
pub proof fn lemma_rank_step(g0: GMap, g1: GMap, r0: GMap, r2: GMap, ok: KeyView, ik: KeyView, og: Option<Grp>, ng: Option<Grp>)
    requires
        gm_ok(g0), gm_ok(g1), rank_ok(g0, r0),
        gm_at(g0, ok, ik) == og, gm_at(g1, ok, ik) == ng,
        forall|o: KeyView, i: KeyView| #![trigger gm_at(g1, o, i)] (o != ok || i != ik) ==> gm_at(g1, o, i) == gm_at(g0, o, i),
        ng is Some ==> bits_ok(ng->0),
        forall|o: KeyView, i: KeyView| #![trigger gm_at(r2, o, i)] gm_at(r2, o, i) ==
            if ng is Some && o == ok && i == rkey(ng->0) { ng }
            else if og is Some && o == ok && i == rkey(og->0) { None }
            else { gm_at(r0, o, i) },
    ensures rank_ok(g1, r2),
{
    if og is Some { lemma_gm_ik(g0, ok, ik); }
    if ng is Some { lemma_gm_ik(g1, ok, ik); }
    assert forall|o: KeyView, rk: KeyView| #![trigger gm_at(r2, o, rk)] gm_at(r2, o, rk) is Some implies ({
        let g = gm_at(r2, o, rk)->0;
        &&& rk == rkey(g)
        &&& gm_at(g1, o, g.ik()) == Some(g)
        &&& bits_ok(g)
    }) by {
        let g = gm_at(r2, o, rk)->0;
        if ng is Some && o == ok && rk == rkey(ng->0) {
            assert(g == ng->0);
        } else {
            assert(gm_at(r0, o, rk) == Some(g));
            if o == ok && g.ik() == ik {
                assert(og == Some(g));
            }
        }
    }
    assert forall|o: KeyView, i: KeyView| #![trigger gm_at(g1, o, i)] gm_at(g1, o, i) is Some implies
        gm_at(r2, o, rkey(gm_at(g1, o, i)->0)) == gm_at(g1, o, i) by {
        let g = gm_at(g1, o, i)->0;
        lemma_gm_ik(g1, o, i);
        if o == ok && i == ik {
            assert(g == ng->0);
        } else {
            assert(gm_at(g0, o, i) == Some(g));
            lemma_gm_ik(g0, o, i);
            if ng is Some && o == ok && rkey(g) == rkey(ng->0) { lemma_rkey_ik(g, ng->0); }
            if og is Some && o == ok && rkey(g) == rkey(og->0) { lemma_rkey_ik(g, og->0); }
        }
    }
}

/// The double of a group with count c and exact sum s: its bits, None when s is out of the double range.
pub fn grp_bits(tab: &Pow2Tab, maxv: &Wide, minv: &Wide, c: &Wide, s: &Wide, Ghost(db): Ghost<Db>, Ghost(k): Ghost<GK>) -> (r: Option<u64>)
    requires
        tab.ok(),
        maxv@ == max_val(), minv@ == -max_val(),
        rep_db(db),
        c@ == cntk(db, k), s@ == sumk(db, k), c@ > 0,
    ensures
        r matches Some(b) ==> f64_finite(b) && if c@ == 1 { f64_val(b) == s@ } else { f64_rounded(b, s@) },
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
            let x = choose|x: u64| f64_val(x) == sumk(db, k);
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

/// Builds the rank key of a group from its stored key and bits.
pub fn rkey_of(bits: u64, ik: &Key) -> (k: Key)
    ensures k.view() == (f64_desc_key(bits), ik.view().1),
{
    Key::shared(desc_key(bits), ik.text.clone())
}

/// Adds the entry's counts to its group (as gm_apply) and re-keys that group in the rank index.
/// The rank index is dropped (ranked = false) once a group's sum leaves the double range.
pub fn gm_apply_r(groups: &mut GMap, rank: &mut GMap, ranked: &mut bool, e: &AccEnt,
    tab: &Pow2Tab, maxv: &Wide, minv: &Wide, Ghost(db): Ghost<Db>)
    requires
        gm_ok(*old(groups)), pos_ok(*old(groups)), nm_ok(*old(rank)),
        *old(ranked) ==> rank_ok(*old(groups), *old(rank)),
        tab.ok(), maxv@ == max_val(), minv@ == -max_val(), rep_db(db),
        gv_c(*old(groups), e.gk()) + e.cnt@ == cntk(db, e.gk()),
        gv_s(*old(groups), e.gk()) + e.sum@ == sumk(db, e.gk()),
    ensures
        gm_ok(*final(groups)), pos_ok(*final(groups)), nm_ok(*final(rank)),
        forall|k: GK| #![trigger gv_c(*final(groups), k)] gv_c(*final(groups), k) == gv_c(*old(groups), k) + if k == e.gk() { e.cnt@ } else { 0 },
        forall|k: GK| #![trigger gv_s(*final(groups), k)] gv_s(*final(groups), k) == gv_s(*old(groups), k) + if k == e.gk() { e.sum@ } else { 0 },
        *final(ranked) ==> rank_ok(*final(groups), *final(rank)),
{
    let ghost g0 = *groups;
    let ghost r0 = *rank;
    let ghost ke = e.gk();
    let ghost okv = okey(e.fy as int, e.stmt@, e.uom@);
    let ghost ikv = ikey(ov(e.name), e.tag@, ov(e.plabel));
    proof { lemma_cnt_facts(db, ke); }
    if !*ranked {
        gm_apply(groups, e, 0);
        return;
    }
    let found = gm_get(groups, e.fy, e.stmt.as_slice(), e.uom.as_slice(), &e.name, e.tag.as_slice(), &e.plabel);
    let ghost og: Option<Grp> = match &found { Some(g) => Some(arc_val(g)), None => None };
    proof { assert(gk_at(g0, ke) == og); assert(og == gm_at(g0, okv, ikv)); }
    let ok = okey_of(e.fy, e.stmt.as_slice(), e.uom.as_slice());
    let (c, s) = match &found {
        Some(g) => (g.cnt.add(&e.cnt), g.sum.add(&e.sum)),
        None => (e.cnt.dup(), e.sum.dup()),
    };
    proof { assert(c@ == cntk(db, ke) && s@ == sumk(db, ke)); }
    if let Some(g) = &found {
        let ik_old = ikey_of(&g.name, g.tag.as_slice(), &g.plabel);
        let rk_old = rkey_of(g.bits, &ik_old);
        nm_del(rank, &ok, &rk_old);
    }
    let ghost r1 = *rank;
    proof {
        assert forall|o: KeyView, i: KeyView| #![trigger gm_at(r1, o, i)] gm_at(r1, o, i) ==
            if og is Some && o == okv && i == rkey(og->0) { None } else { gm_at(r0, o, i) } by {
            if found is None { assert(r1 == r0); }
        }
    }
    if c.is_zero() {
        gm_apply(groups, e, 0);
        proof {
            assert(gm_at(*groups, okv, ikv) is None) by {
                assert(gv_c(*groups, ke) == 0);
            }
            lemma_rank_step(g0, *groups, r0, r1, okv, ikv, og, None);
        }
        return;
    }
    match grp_bits(tab, maxv, minv, &c, &s, Ghost(db), Ghost(ke)) {
        None => {
            *ranked = false;
            gm_apply(groups, e, 0);
        }
        Some(b) => {
            gm_apply(groups, e, b);
            let ng = match gm_get(groups, e.fy, e.stmt.as_slice(), e.uom.as_slice(), &e.name, e.tag.as_slice(), &e.plabel) {
                Some(g) => g,
                None => {
                    proof { assert(gv_c(*groups, ke) > 0); assert(false); }
                    return;
                }
            };
            let ghost ngv = arc_val(&ng);
            let ik = ikey_of(&ng.name, ng.tag.as_slice(), &ng.plabel);
            let rk = rkey_of(b, &ik);
            proof {
                assert(gm_at(*groups, okv, ikv) == Some(ngv));
                assert(ngv.bits == b);
                assert(gk_at(*groups, ke) == Some(ngv));
                assert(gv_c(*groups, ke) == gv_c(g0, ke) + e.cnt@ && gv_s(*groups, ke) == gv_s(g0, ke) + e.sum@);
                assert(ngv.cnt@ == c@ && ngv.sum@ == s@);
            }
            nm_put(rank, &ok, rk, ng);
            proof { lemma_rank_step(g0, *groups, r0, *rank, okv, ikv, og, Some(ngv)); }
        }
    }
}

pub proof fn lemma_rank_ok_ext(m1: GMap, m2: GMap, r1: GMap, r2: GMap)
    requires
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(m2, ok, ik)] gm_at(m2, ok, ik) == gm_at(m1, ok, ik),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(r2, ok, ik)] gm_at(r2, ok, ik) == gm_at(r1, ok, ik),
        rank_ok(m1, r1),
    ensures rank_ok(m2, r2),
{
    assert forall|ok: KeyView, rk: KeyView| #![trigger gm_at(r2, ok, rk)] gm_at(r2, ok, rk) is Some implies ({
        let g = gm_at(r2, ok, rk)->0;
        &&& rk == rkey(g)
        &&& gm_at(m2, ok, g.ik()) == Some(g)
        &&& bits_ok(g)
    }) by {
        assert(gm_at(r1, ok, rk) is Some);
    }
    assert forall|ok: KeyView, ik: KeyView| #![trigger gm_at(m2, ok, ik)] gm_at(m2, ok, ik) is Some implies
        gm_at(r2, ok, rkey(gm_at(m2, ok, ik)->0)) == gm_at(m2, ok, ik) by {
        assert(gm_at(m1, ok, ik) is Some);
    }
}

} // verus!
