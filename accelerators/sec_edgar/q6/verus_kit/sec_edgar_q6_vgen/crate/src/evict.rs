// Stage A of an update: the recomputed units' stored groups leave their classes.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_key_order, lemma_key_trans};
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::grp::*;
use crate::inv::*;
use crate::crops::*;
use crate::units::*;
use crate::kept::*;
use crate::tbl::*;
use crate::plan::*;
use crate::readp::*;
use crate::ordx::*;

verus! {

/// The unit named by nk, of fy, is in the set.
pub open spec fn wn(w: Index<UnitRef>, fy: int, nk: KeyView) -> bool {
    w.at((fy, nk.1)) is Some
}

/// Some unit among the first i of the set, of the class's fy, has this unit key.
pub open spec fn fside_hit(wes: Seq<(KeyView, UnitRef)>, fy: int, i: int, nk: KeyView) -> bool {
    exists|j: int| 0 <= j < i && j < wes.len() && (#[trigger] wes[j]).1.fy as int == fy && nkey_v(ov(wes[j].1.name)) == nk
}

/// The record without the unit's frontier group and overflow marks.
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn crec_evict_unit(c: &ClassRec, nk: &Key) -> (n: ClassRec)
    requires crec_struct(*c),
    ensures
        crec_head(*c, n), n.r.wf(), n.f.wf(), n.fb.wf(), n.hp.wf(), n.hn.wf(),
        ix_same(n.r, c.r),
        forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if c.f.at(q) is Some && nkey_v(ov(c.f.at(q)->0.name)) == nk.view() { None } else { c.f.at(q) },
        forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if q == nk.view() { None } else { c.fb.at(q) },
        forall|q: KeyView| #![trigger n.hp.at(q)] n.hp.at(q) == if q == nk.view() { None } else { c.hp.at(q) },
        forall|q: KeyView| #![trigger n.hn.at(q)] n.hn.at(q) == if q == nk.view() { None } else { c.hn.at(q) },
{
    let n1 = crec_f_del(c, nk);
    let n2 = crec_h_del(&n1, nk);
    proof {
        assert forall|q: KeyView| n2.f.at(q) == if c.f.at(q) is Some && nkey_v(ov(c.f.at(q)->0.name)) == nk.view() { None } else { c.f.at(q) } by {
            if c.fb.at(nk.view()) is Some {
                let b = c.fb.at(nk.view())->0;
                assert(c.f.at(rkey(b)) == c.fb.at(nk.view()));
                if c.f.at(q) is Some {
                    let h = c.f.at(q)->0;
                    assert(c.fb.at(nkey_v(ov(h.name))) == c.f.at(q));
                    if nkey_v(ov(h.name)) == nk.view() { assert(c.f.at(q) == c.fb.at(nk.view())); assert(q == rkey(b)); }
                    if q == rkey(b) { assert(c.f.at(q) == c.fb.at(nk.view())); assert(nkey_v(ov(b.name)) == nk.view()); }
                }
            } else {
                if c.f.at(q) is Some {
                    let h = c.f.at(q)->0;
                    assert(c.fb.at(nkey_v(ov(h.name))) == c.f.at(q));
                }
            }
        }
    }
    n2
}

/// The record without the frontier side of every unit in w that is of the class's fy.
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn evict_fside(c: &ClassRec, w: &Index<UnitRef>) -> (n: ClassRec)
    requires crec_struct(*c), uset_ok(*w),
    ensures
        crec_head(*c, n), n.r.wf(), n.f.wf(), n.fb.wf(), n.hp.wf(), n.hn.wf(),
        ix_same(n.r, c.r),
        forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if c.f.at(q) is Some && wn(*w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name))) { None } else { c.f.at(q) },
        forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if c.fb.at(q) is Some && wn(*w, c.fy as int, q) { None } else { c.fb.at(q) },
        forall|q: KeyView| #![trigger n.hp.at(q)] n.hp.at(q) == if c.hp.at(q) is Some && wn(*w, c.fy as int, q) { None } else { c.hp.at(q) },
        forall|q: KeyView| #![trigger n.hn.at(q)] n.hn.at(q) == if c.hn.at(q) is Some && wn(*w, c.fy as int, q) { None } else { c.hn.at(q) },
{
    let wl = w.rows();
    let ghost wes = w.entries();
    proof { w.model(); }
    let mut n = crec_with(c, sh(&c.r), sh(&c.f), sh(&c.fb), sh(&c.hp), sh(&c.hn));
    let mut i = 0;
    while i < wl.len()
        invariant
            i <= wl.len(), crec_struct(*c), crec_head(*c, n), uset_ok(*w), w.wf(),
            wl@.map_values(|row: (Key, Arc<UnitRef>)| (row.0.view(), *row.1)) == wes, wes == w.entries(),
            n.r.wf(), n.f.wf(), n.fb.wf(), n.hp.wf(), n.hn.wf(),
            ix_same(n.r, c.r),
            forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if c.f.at(q) is Some && fside_hit(wes, c.fy as int, i as int, nkey_v(ov(c.f.at(q)->0.name))) { None } else { c.f.at(q) },
            forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if c.fb.at(q) is Some && fside_hit(wes, c.fy as int, i as int, q) { None } else { c.fb.at(q) },
            forall|q: KeyView| #![trigger n.hp.at(q)] n.hp.at(q) == if c.hp.at(q) is Some && fside_hit(wes, c.fy as int, i as int, q) { None } else { c.hp.at(q) },
            forall|q: KeyView| #![trigger n.hn.at(q)] n.hn.at(q) == if c.hn.at(q) is Some && fside_hit(wes, c.fy as int, i as int, q) { None } else { c.hn.at(q) },
        decreases wl.len() - i,
    {
        let ur = &wl[i].1;
        proof { assert(wes[i as int] == (wl@[i as int].0.view(), arc_val(ur))); }
        if ur.fy == c.fy {
            let nk = nkey_of(&ur.name);
            let n0 = n;
            n = crec_evict_unit(&n0, &nk);
            proof {
                assert(nk.view() == nkey_v(ov(arc_val(ur).name)));
                assert forall|q: KeyView| n.f.at(q) == if c.f.at(q) is Some && fside_hit(wes, c.fy as int, i as int + 1, nkey_v(ov(c.f.at(q)->0.name))) { None } else { c.f.at(q) } by {
                    if c.f.at(q) is Some { let nq = nkey_v(ov(c.f.at(q)->0.name)); assert(fside_hit(wes, c.fy as int, i as int + 1, nq) == (fside_hit(wes, c.fy as int, i as int, nq) || nq == nk.view())); }
                }
            }
        }
        i += 1;
    }
    n
}

/// Entries before position i of the kept groups whose unit is in the set.
pub open spec fn rside_hit(es: Seq<(KeyView, Grp)>, fy: int, u: Index<UnitRef>, i: int, q: KeyView) -> bool {
    exists|j: int| 0 <= j < i && j < es.len() && (#[trigger] es[j]).0 == q && wn(u, fy, nkey_v(ov(es[j].1.name)))
}

/// The record without the kept groups of the units in u; the kept groups of the other units in w are registered in `kept`.
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn evict_rside(c: &ClassRec, u: &Index<UnitRef>, w: &Index<UnitRef>, kept: &mut GMap) -> (n: ClassRec)
    requires c.r.wf(), r_uniq(c.r), c.f.wf(), c.fb.wf(), c.hp.wf(), c.hn.wf(), uset_ok(*u), uset_ok(*w), nm_ok(*old(kept)),
    ensures
        crec_head(*c, n), n.r.wf(), ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
        forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) == if c.r.at(q) is Some && wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) { None } else { c.r.at(q) },
        nm_ok(*final(kept)),
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && wn(*w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) && !wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) ==>
            gm_at(*final(kept), cls_key(*c), c.r.at(q)->0.ik()) == Some(c.r.at(q)->0),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(kept), ok, ik)] gm_at(*final(kept), ok, ik) is Some ==>
            gm_at(*old(kept), ok, ik) is Some || (ok == cls_key(*c) && exists|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && c.r.at(q)->0.ik() == ik
                && gm_at(*final(kept), ok, ik) == c.r.at(q) && n.r.at(q) == c.r.at(q)),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(kept), ok, ik)] ok != cls_key(*c) ==> gm_at(*final(kept), ok, ik) == gm_at(*old(kept), ok, ik),
{
    let ents = c.r.rows();
    let ghost es = c.r.entries();
    proof { c.r.model(); }
    let ghost kept0 = *kept;
    let mut n = crec_with(c, sh(&c.r), sh(&c.f), sh(&c.fb), sh(&c.hp), sh(&c.hn));
    let ok = okey_of(c.fy, c.stmt.as_slice(), c.uom.as_slice());
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(), r_uniq(c.r), crec_head(*c, n), uset_ok(*u), uset_ok(*w), c.r.wf(),
            ents@.map_values(|row: (Key, Arc<Grp>)| (row.0.view(), *row.1)) == es, es == c.r.entries(),
            ok.view() == cls_key(*c),
            n.r.wf(), ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
            forall|pos: int| #![trigger es[pos]] 0 <= pos < es.len() ==> c.r.at(es[pos].0) == Some(es[pos].1),
            forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) == if c.r.at(q) is Some && rside_hit(es, c.fy as int, *u, i as int, q) { None } else { c.r.at(q) },
            nm_ok(*kept),
            forall|j: int| 0 <= j < i ==> (wn(*w, c.fy as int, nkey_v(ov((#[trigger] es[j]).1.name))) && !wn(*u, c.fy as int, nkey_v(ov(es[j].1.name)))) ==>
                gm_at(*kept, ok.view(), es[j].1.ik()) == Some(es[j].1),
            forall|ok2: KeyView, ik: KeyView| #![trigger gm_at(*kept, ok2, ik)] gm_at(*kept, ok2, ik) is Some ==>
                gm_at(kept0, ok2, ik) is Some || (ok2 == ok.view() && exists|j: int| 0 <= j < i && #[trigger] es[j].1.ik() == ik && gm_at(*kept, ok2, ik) == Some(es[j].1) && !wn(*u, c.fy as int, nkey_v(ov(es[j].1.name)))),
            forall|ok2: KeyView, ik: KeyView| #![trigger gm_at(*kept, ok2, ik)] ok2 != ok.view() ==> gm_at(*kept, ok2, ik) == gm_at(kept0, ok2, ik),
        decreases ents.len() - i,
    {
        let key = &ents[i].0;
        let g = &ents[i].1;
        proof { assert(es[i as int] == (key.view(), arc_val(g))); assert(c.r.at(key.view()) == Some(arc_val(g))); }
        let nm = ukey_of(c.fy, &g.name);
        let in_w = w.get(&nm).is_some();
        let in_u = u.get(&nm).is_some();
        proof { assert(nm.view() == (c.fy as int, nkey_v(ov(arc_val(g).name)).1)); }
        if in_w && !in_u {
            let ik = ikey_of(&g.name, g.tag.as_slice(), &g.plabel);
            let ghost kept1 = *kept;
            nm_put(kept, &ok, ik, g.clone());
            proof {
                assert forall|j: int| 0 <= j < i + 1 implies ((wn(*w, c.fy as int, nkey_v(ov(es[j].1.name))) && !wn(*u, c.fy as int, nkey_v(ov(es[j].1.name)))) ==>
                    gm_at(*kept, ok.view(), es[j].1.ik()) == Some(es[j].1)) by {
                    if j < i {
                        if es[j].1.ik() != arc_val(g).ik() { assert(gm_at(*kept, ok.view(), es[j].1.ik()) == gm_at(kept1, ok.view(), es[j].1.ik())); }
                        else {
                            assert(c.r.at(es[j].0) == Some(es[j].1));
                            lemma_ik_same(*c, es[j].0, key.view());
                        }
                    }
                }
                assert forall|ok2: KeyView, ik2: KeyView| gm_at(*kept, ok2, ik2) is Some implies
                    gm_at(kept0, ok2, ik2) is Some || (ok2 == ok.view() && exists|j: int| 0 <= j < i + 1 && #[trigger] es[j].1.ik() == ik2 && gm_at(*kept, ok2, ik2) == Some(es[j].1) && !wn(*u, c.fy as int, nkey_v(ov(es[j].1.name)))) by {
                    if ok2 == ok.view() && ik2 == arc_val(g).ik() { assert(es[i as int].1.ik() == ik2); assert(!wn(*u, c.fy as int, nkey_v(ov(es[i as int].1.name))) == !in_u); }
                }
            }
        } else {
            proof {
                assert forall|j: int| 0 <= j < i + 1 implies ((wn(*w, c.fy as int, nkey_v(ov(es[j].1.name))) && !wn(*u, c.fy as int, nkey_v(ov(es[j].1.name)))) ==>
                    gm_at(*kept, ok.view(), es[j].1.ik()) == Some(es[j].1)) by {
                    if j == i { assert(wn(*w, c.fy as int, nkey_v(ov(es[j].1.name))) == in_w); assert(wn(*u, c.fy as int, nkey_v(ov(es[j].1.name))) == in_u); }
                }
                assert forall|ok2: KeyView, ik2: KeyView| gm_at(*kept, ok2, ik2) is Some implies
                    gm_at(kept0, ok2, ik2) is Some || (ok2 == ok.view() && exists|j: int| 0 <= j < i + 1 && #[trigger] es[j].1.ik() == ik2 && gm_at(*kept, ok2, ik2) == Some(es[j].1) && !wn(*u, c.fy as int, nkey_v(ov(es[j].1.name)))) by {
                    if gm_at(*kept, ok2, ik2) is Some && !(gm_at(kept0, ok2, ik2) is Some) {
                        let j = choose|j: int| 0 <= j < i && es[j].1.ik() == ik2 && gm_at(*kept, ok2, ik2) == Some(es[j].1);
                    }
                }
            }
        }
        if in_u {
            n = crec_r_del(&n, key);
            proof {
                assert forall|q: KeyView| n.r.at(q) == if c.r.at(q) is Some && rside_hit(es, c.fy as int, *u, i as int + 1, q) { None } else { c.r.at(q) } by {
                    if q == key.view() { assert(rside_hit(es, c.fy as int, *u, i as int + 1, q)); }
                    else if c.r.at(q) is Some { assert(rside_hit(es, c.fy as int, *u, i as int + 1, q) == rside_hit(es, c.fy as int, *u, i as int, q)); }
                }
            }
        } else {
            proof {
                assert forall|q: KeyView| n.r.at(q) == if c.r.at(q) is Some && rside_hit(es, c.fy as int, *u, i as int + 1, q) { None } else { c.r.at(q) } by {
                    if c.r.at(q) is Some && rside_hit(es, c.fy as int, *u, i as int + 1, q) && !rside_hit(es, c.fy as int, *u, i as int, q) {
                        assert(wn(*u, c.fy as int, nkey_v(ov(es[i as int].1.name))) == in_u);
                        assert(es[i as int].0 == q);
                    }
                }
            }
        }
        i += 1;
    }
    proof {
        assert forall|q: KeyView| n.r.at(q) == if c.r.at(q) is Some && wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) { None } else { c.r.at(q) } by {
            if c.r.at(q) is Some {
                let pos = choose|pos: int| #![auto] 0 <= pos < es.len() && es[pos].0 == q;
                assert(es[pos].1 == c.r.at(q)->0);
                if wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) { assert(rside_hit(es, c.fy as int, *u, es.len() as int, q)); }
                if rside_hit(es, c.fy as int, *u, es.len() as int, q) {
                    let j = choose|j: int| 0 <= j < es.len() && es[j].0 == q && wn(*u, c.fy as int, nkey_v(ov(es[j].1.name)));
                    assert(es[j].1 == c.r.at(q)->0);
                }
            }
        }
        assert forall|q: KeyView| c.r.at(q) is Some && wn(*w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) && !wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) implies
            gm_at(*kept, cls_key(*c), c.r.at(q)->0.ik()) == Some(c.r.at(q)->0) by {
            let pos = choose|pos: int| #![auto] 0 <= pos < es.len() && es[pos].0 == q;
            assert(es[pos].1 == c.r.at(q)->0);
        }
        assert forall|ok2: KeyView, ik: KeyView| gm_at(*kept, ok2, ik) is Some implies
            gm_at(kept0, ok2, ik) is Some || (ok2 == cls_key(*c) && exists|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && c.r.at(q)->0.ik() == ik
                && gm_at(*kept, ok2, ik) == c.r.at(q) && n.r.at(q) == c.r.at(q)) by {
            if gm_at(*kept, ok2, ik) is Some && !(gm_at(kept0, ok2, ik) is Some) {
                let j = choose|j: int| 0 <= j < es.len() && es[j].1.ik() == ik && gm_at(*kept, ok2, ik) == Some(es[j].1);
                assert(c.r.at(es[j].0) == Some(es[j].1));
                assert(!wn(*u, c.fy as int, nkey_v(ov(es[j].1.name))));
            }
        }
    }
    n
}

pub open spec fn r_uniq(r: Index<Grp>) -> bool {
    forall|r1: KeyView, r2: KeyView| #![trigger r.at(r1), r.at(r2)] r.at(r1) is Some && r.at(r2) is Some && r.at(r1)->0.ik() == r.at(r2)->0.ik() ==> r1 == r2
}

pub proof fn lemma_ik_same(c: ClassRec, q1: KeyView, q2: KeyView)
    requires r_uniq(c.r), c.r.at(q1) is Some, c.r.at(q2) is Some, c.r.at(q1)->0.ik() == c.r.at(q2)->0.ik(),
    ensures q1 == q2,
{}

/// The groups moved from the frontier into the kept groups of the class: the first of its units outside u.
pub open spec fn mvd(pk: Seq<Grp>, q: KeyView) -> bool {
    exists|i: int| 0 <= i < pk.len() && rkey(#[trigger] pk[i]) == q
}

/// n is c without the frontier side of the units in w and the kept groups of the units in u, plus the groups pk.
pub open spec fn evict_rel(c: ClassRec, n: ClassRec, u: Index<UnitRef>, w: Index<UnitRef>, pk: Seq<Grp>) -> bool {
    &&& crec_head(c, n)
    &&& n.r.wf() && n.f.wf() && n.fb.wf() && n.hp.wf() && n.hn.wf()
    &&& forall|i: int| 0 <= i < pk.len() ==> n.r.at(rkey(#[trigger] pk[i])) == Some(pk[i])
    &&& forall|q: KeyView| #![trigger n.r.at(q)] !mvd(pk, q) ==> n.r.at(q) == if c.r.at(q) is Some && wn(u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) { None } else { c.r.at(q) }
    &&& forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if c.f.at(q) is Some && wn(w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name))) { None } else { c.f.at(q) }
    &&& forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if c.fb.at(q) is Some && wn(w, c.fy as int, q) { None } else { c.fb.at(q) }
    &&& forall|q: KeyView| #![trigger n.hp.at(q)] n.hp.at(q) == if c.hp.at(q) is Some && wn(w, c.fy as int, q) { None } else { c.hp.at(q) }
    &&& forall|q: KeyView| #![trigger n.hn.at(q)] n.hn.at(q) == if c.hn.at(q) is Some && wn(w, c.fy as int, q) { None } else { c.hn.at(q) }
}

/// The kept groups of the units in w but not in u, as n stores them, are registered in `kept`.
pub open spec fn kept_rel(c: ClassRec, n: ClassRec, u: Index<UnitRef>, w: Index<UnitRef>, pk: Seq<Grp>, k0: GMap, k1: GMap) -> bool {
    &&& nm_ok(k1)
    &&& forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && wn(w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) && !wn(u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) ==>
        gm_at(k1, cls_key(c), c.r.at(q)->0.ik()) == Some(c.r.at(q)->0)
    &&& forall|i: int| 0 <= i < pk.len() ==> gm_at(k1, cls_key(c), (#[trigger] pk[i]).ik()) == Some(pk[i])
    &&& forall|ok: KeyView, ik: KeyView| #![trigger gm_at(k1, ok, ik)] gm_at(k1, ok, ik) is Some ==>
        gm_at(k0, ok, ik) is Some || (ok == cls_key(c) && (
            (exists|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && c.r.at(q)->0.ik() == ik && gm_at(k1, ok, ik) == c.r.at(q) && n.r.at(q) == c.r.at(q))
            || (exists|i: int| 0 <= i < pk.len() && (#[trigger] pk[i]).ik() == ik && gm_at(k1, ok, ik) == Some(pk[i]))))
    &&& forall|ok: KeyView, ik: KeyView| #![trigger gm_at(k1, ok, ik)] ok != cls_key(c) ==> gm_at(k1, ok, ik) == gm_at(k0, ok, ik)
}

/// Stores the groups in the class's kept groups, and registers them in `kept`.
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn move_class(n: &ClassRec, picked: &Vec<Arc<Grp>>, kept: &mut GMap) -> (m: ClassRec)
    requires n.r.wf(), n.f.wf(), n.fb.wf(), n.hp.wf(), n.hn.wf(), nm_ok(*old(kept)),
        forall|i: int, j: int| #![trigger picked@[i], picked@[j]] 0 <= i < j < picked.len() ==> picked@[i].ik() != picked@[j].ik(),
    ensures
        crec_head(*n, m), m.r.wf(), ix_same(m.f, n.f), ix_same(m.fb, n.fb), ix_same(m.hp, n.hp), ix_same(m.hn, n.hn),
        forall|i: int| 0 <= i < picked.len() ==> m.r.at(rkey(*(#[trigger] picked@[i]))) == Some(*picked@[i]),
        forall|q: KeyView| #![trigger m.r.at(q)] !mvd(picked@.map_values(|g: Arc<Grp>| *g), q) ==> m.r.at(q) == n.r.at(q),
        nm_ok(*final(kept)),
        forall|i: int| 0 <= i < picked.len() ==> gm_at(*final(kept), cls_key(*n), (#[trigger] picked@[i]).ik()) == Some(*picked@[i]),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(kept), ok, ik)] gm_at(*final(kept), ok, ik) is Some ==>
            gm_at(*old(kept), ok, ik) is Some || (ok == cls_key(*n) && exists|i: int| 0 <= i < picked.len() && (#[trigger] picked@[i]).ik() == ik && gm_at(*final(kept), ok, ik) == Some(*picked@[i])),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(kept), ok, ik)] ok != cls_key(*n) ==> gm_at(*final(kept), ok, ik) == gm_at(*old(kept), ok, ik),
        forall|ik: KeyView| #![trigger gm_at(*final(kept), cls_key(*n), ik)] (forall|i: int| 0 <= i < picked.len() ==> (#[trigger] picked@[i]).ik() != ik) ==> gm_at(*final(kept), cls_key(*n), ik) == gm_at(*old(kept), cls_key(*n), ik),
{
    let ghost pk = picked@.map_values(|g: Arc<Grp>| *g);
    let ghost kept0 = *kept;
    proof { assert forall|a: int, b: int| 0 <= a < b < picked.len() implies picked@[a].ik() != picked@[b].ik() by {} }
    let ok = okey_of(n.fy, n.stmt.as_slice(), n.uom.as_slice());
    let mut m = crec_with(n, sh(&n.r), sh(&n.f), sh(&n.fb), sh(&n.hp), sh(&n.hn));
    let mut i = 0;
    while i < picked.len()
        invariant
            i <= picked.len(), crec_head(*n, m), m.r.wf(), n.r.wf(), n.f.wf(), n.fb.wf(), n.hp.wf(), n.hn.wf(),
            ix_same(m.f, n.f), ix_same(m.fb, n.fb), ix_same(m.hp, n.hp), ix_same(m.hn, n.hn),
            pk == picked@.map_values(|g: Arc<Grp>| *g),
            forall|a: int, b: int| 0 <= a < b < picked.len() ==> (#[trigger] picked@[a]).ik() != (#[trigger] picked@[b]).ik(),
            ok.view() == cls_key(*n), nm_ok(*kept),
            forall|j: int| 0 <= j < i ==> m.r.at(rkey(*(#[trigger] picked@[j]))) == Some(*picked@[j]),
            forall|q: KeyView| #![trigger m.r.at(q)] !(exists|j: int| 0 <= j < i && rkey(*(#[trigger] picked@[j])) == q) ==> m.r.at(q) == n.r.at(q),
            forall|j: int| 0 <= j < i ==> gm_at(*kept, ok.view(), (#[trigger] picked@[j]).ik()) == Some(*picked@[j]),
            forall|ok2: KeyView, ik: KeyView| #![trigger gm_at(*kept, ok2, ik)] gm_at(*kept, ok2, ik) is Some ==>
                gm_at(kept0, ok2, ik) is Some || (ok2 == ok.view() && exists|j: int| 0 <= j < i && (#[trigger] picked@[j]).ik() == ik && gm_at(*kept, ok2, ik) == Some(*picked@[j])),
            forall|ok2: KeyView, ik: KeyView| #![trigger gm_at(*kept, ok2, ik)] ok2 != ok.view() ==> gm_at(*kept, ok2, ik) == gm_at(kept0, ok2, ik),
            forall|ik: KeyView| #![trigger gm_at(*kept, ok.view(), ik)] (forall|j: int| 0 <= j < i ==> (#[trigger] picked@[j]).ik() != ik) ==> gm_at(*kept, ok.view(), ik) == gm_at(kept0, ok.view(), ik),
        decreases picked.len() - i,
    {
        let g = &picked[i];
        let ghost m0 = m;
        let ghost kept1 = *kept;
        m = crec_r_put(&m, g.clone());
        let ik = ikey_of(&g.name, g.tag.as_slice(), &g.plabel);
        nm_put(kept, &ok, ik, g.clone());
        proof {
            let gv = **g;
            assert forall|j: int| 0 <= j < i + 1 implies m.r.at(rkey(*(#[trigger] picked@[j]))) == Some(*picked@[j]) by {
                if j < i {
                    if rkey(*picked@[j]) != rkey(gv) { assert(m.r.at(rkey(*picked@[j])) == m0.r.at(rkey(*picked@[j]))); }
                    else { lemma_rkey_fields(*picked@[j], gv); assert(gv == *picked@[i as int]); assert(picked@[j].ik() != picked@[i as int].ik()); assert(false); }
                }
            }
            assert forall|j: int| 0 <= j < i + 1 implies gm_at(*kept, ok.view(), (#[trigger] picked@[j]).ik()) == Some(*picked@[j]) by {
                if j < i {
                    assert(gv == *picked@[i as int]);
                    assert(picked@[j].ik() != picked@[i as int].ik());
                    assert(gm_at(*kept, ok.view(), picked@[j].ik()) == gm_at(kept1, ok.view(), picked@[j].ik()));
                }
            }
        }
        i += 1;
    }
    proof {
        assert forall|q: KeyView| !mvd(pk, q) implies m.r.at(q) == n.r.at(q) by {
            if exists|j: int| 0 <= j < picked.len() && rkey(*(#[trigger] picked@[j])) == q {
                let j = choose|j: int| 0 <= j < picked.len() && rkey(*picked@[j]) == q;
                assert(pk[j] == *picked@[j]);
                assert(mvd(pk, q));
            }
        }
    }
    m
}

} // verus!

verus! {

/// The groups moved from the frontier into the class's kept groups: its first units outside u, when it has few kept groups outside u.
pub open spec fn pick_of(c: ClassRec, u: Index<UnitRef>, refill: bool) -> Seq<Grp> {
    if refill && notu_keys(c.r.entries(), c.fy as int, u).len() < k_limit() {
        vsel(c.f.entries(), c.fy as int, u).map_values(|k: KeyView| c.f.at(k)->0)
    } else { Seq::empty() }
}

proof fn lemma_picked_distinct(c: ClassRec, u: Index<UnitRef>, picked: Seq<Arc<Grp>>)
    requires
        crec_struct(c),
        picked.map_values(|g: Arc<Grp>| rkey(*g)) == vsel(c.f.entries(), c.fy as int, u),
        forall|i: int| 0 <= i < picked.len() ==> c.f.at(rkey(*(#[trigger] picked[i]))) == Some(*picked[i]),
    ensures
        forall|i: int, j: int| #![trigger picked[i], picked[j]] 0 <= i < j < picked.len() ==> picked[i].ik() != picked[j].ik(),
{
    c.f.model();
    assert(ordered_es(c.f.entries()));
    lemma_vsel_prefix(c.f.entries(), c.fy as int, u);
    let vs = vsel(c.f.entries(), c.fy as int, u);
    assert forall|i: int, j: int| 0 <= i < j < picked.len() implies picked[i].ik() != picked[j].ik() by {
        let (gi, gj) = (*picked[i], *picked[j]);
        assert(vs[i] == rkey(gi) && vs[j] == rkey(gj));
        assert(key_lt(vs[i], vs[j]));
        lemma_key_order(vs[i], vs[j]);
        if gi.ik() == gj.ik() {
            lemma_ikey_inj(ov(gi.name), gi.tag@, ov(gi.plabel), ov(gj.name), gj.tag@, ov(gj.plabel));
            assert(c.fb.at(nkey_v(ov(gi.name))) == c.f.at(rkey(gi)));
            assert(c.fb.at(nkey_v(ov(gj.name))) == c.f.at(rkey(gj)));
            assert(gi == gj);
        }
    }
}

/// The class's record without the recomputed units' groups: the frontier side of every unit in w, the kept groups of the units in u;
/// with refill, the frontier groups of the first units outside u move into the kept groups when few remain.
/// The kept groups of the units in w but not in u are registered in `kept`.
#[verifier::rlimit(200)]
#[verifier::spinoff_prover]
pub fn evict_class(c: &ClassRec, u: &Index<UnitRef>, w: &Index<UnitRef>, kept: &mut GMap, refill: bool) -> (n: ClassRec)
    requires
        crec_struct(*c), uset_ok(*u), uset_ok(*w), nm_ok(*old(kept)),
        refill && notu_keys(c.r.entries(), c.fy as int, *u).len() < k_limit() ==>
            forall|k: KeyView| vsel(c.f.entries(), c.fy as int, *u).contains(k) ==> wn(*w, c.fy as int, nkey_v(ov(c.f.at(k)->0.name))),
        forall|fy: int, name: Option<Seq<u8>>| u_has(*u, fy, name) ==> u_has(*w, fy, name),
    ensures
        evict_rel(*c, n, *u, *w, pick_of(*c, *u, refill)),
        kept_rel(*c, n, *u, *w, pick_of(*c, *u, refill), *old(kept), *final(kept)),
{
    let ghost kept0 = *kept;
    let n1 = evict_fside(c, w);
    proof {
        assert(cls_key(n1) == cls_key(*c));
        assert forall|q: KeyView| c.r.at(q) is Some implies n1.r.at(q) == c.r.at(q) by {}
        assert forall|q1: KeyView, q2: KeyView| n1.r.at(q1) is Some && n1.r.at(q2) is Some && n1.r.at(q1)->0.ik() == n1.r.at(q2)->0.ik() implies q1 == q2 by {}
    }
    let n2 = evict_rside(&n1, u, w, kept);
    let ghost kept1 = *kept;
    let cnt = count_notu(c, u);
    if refill && cnt < K_LIMIT {
        let picked = select_f(c, u);
        proof {
            lemma_picked_distinct(*c, *u, picked@);
            assert(picked@.map_values(|g: Arc<Grp>| *g) =~= pick_of(*c, *u, refill)) by {
                assert forall|i: int| 0 <= i < picked.len() implies picked@.map_values(|g: Arc<Grp>| *g)[i] == pick_of(*c, *u, refill)[i] by {
                    assert(c.f.at(rkey(*picked@[i])) == Some(*picked@[i]));
                }
            }
        }
        let n3 = move_class(&n2, &picked, kept);
        proof {
            let pk = pick_of(*c, *u, refill);
            assert(pk =~= picked@.map_values(|g: Arc<Grp>| *g));
            assert forall|i: int| 0 <= i < pk.len() implies pk[i] == *picked@[i] by {}
            // evict_rel
            assert forall|i: int| 0 <= i < pk.len() implies n3.r.at(rkey(#[trigger] pk[i])) == Some(pk[i]) by { assert(pk[i] == *picked@[i]); }
            assert forall|q: KeyView| !mvd(pk, q) implies n3.r.at(q) == if c.r.at(q) is Some && wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) { None } else { c.r.at(q) } by {
                assert(!mvd(picked@.map_values(|g: Arc<Grp>| *g), q));
                assert(n3.r.at(q) == n2.r.at(q));
            }
            assert(cls_key(n3) == cls_key(*c));
            // kept_rel
            assert forall|i: int| 0 <= i < pk.len() implies gm_at(*kept, cls_key(*c), (#[trigger] pk[i]).ik()) == Some(pk[i]) by { assert(pk[i] == *picked@[i]); }
            assert forall|q: KeyView| c.r.at(q) is Some && wn(*w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) && !wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) implies
                gm_at(*kept, cls_key(*c), c.r.at(q)->0.ik()) == Some(c.r.at(q)->0) by {
                assert(gm_at(kept1, cls_key(*c), c.r.at(q)->0.ik()) == Some(c.r.at(q)->0));
                let ik = c.r.at(q)->0.ik();
                assert forall|i: int| 0 <= i < picked.len() implies (#[trigger] picked@[i]).ik() != ik by {
                    assert(c.f.at(rkey(*picked@[i])) == Some(*picked@[i]));
                }
            }
            assert forall|ok: KeyView, ik: KeyView| gm_at(*kept, ok, ik) is Some implies
                gm_at(kept0, ok, ik) is Some || (ok == cls_key(*c) && (
                    (exists|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && c.r.at(q)->0.ik() == ik && gm_at(*kept, ok, ik) == c.r.at(q) && n3.r.at(q) == c.r.at(q))
                    || (exists|i: int| 0 <= i < pk.len() && (#[trigger] pk[i]).ik() == ik && gm_at(*kept, ok, ik) == Some(pk[i])))) by {
                if gm_at(kept0, ok, ik) is None {
                    if gm_at(kept1, ok, ik) is Some {
                        let q = choose|q: KeyView| #![trigger n1.r.at(q)] n1.r.at(q) is Some && n1.r.at(q)->0.ik() == ik && gm_at(kept1, ok, ik) == n1.r.at(q) && n2.r.at(q) == n1.r.at(q);
                        assert(c.r.at(q) == n1.r.at(q));
                        assert forall|i: int| 0 <= i < picked.len() implies (#[trigger] picked@[i]).ik() != ik by { assert(c.f.at(rkey(*picked@[i])) == Some(*picked@[i])); }
                        assert(gm_at(*kept, ok, ik) == gm_at(kept1, ok, ik));
                        assert(!mvd(pk, q)) by {
                            if mvd(pk, q) { let i = choose|i: int| 0 <= i < pk.len() && rkey(pk[i]) == q; assert(c.f.at(rkey(*picked@[i])) == Some(*picked@[i])); lemma_rkey_ik_eq(pk[i], c.r.at(q)->0); }
                        }
                        assert(n3.r.at(q) == n2.r.at(q));
                    } else {
                        assert(ok == cls_key(*c));
                        let i = choose|i: int| 0 <= i < picked.len() && picked@[i].ik() == ik && gm_at(*kept, ok, ik) == Some(*picked@[i]);
                        assert(pk[i] == *picked@[i]);
                    }
                }
            }
        }
        n3
    } else {
        proof {
            assert(pick_of(*c, *u, refill) =~= Seq::<Grp>::empty());
            assert(!mvd(pick_of(*c, *u, refill), arbitrary_key()));
            assert forall|q: KeyView| !mvd(pick_of(*c, *u, refill), q) by {}
        }
        n2
    }
}

pub open spec fn arbitrary_key() -> KeyView { (0int, Seq::<u8>::empty()) }

proof fn lemma_rkey_ik_eq(a: Grp, b: Grp)
    requires rkey(a) == rkey(b),
    ensures a.ik() == b.ik(),
{
    lemma_rkey_fields(a, b);
}

} // verus!

verus! {

pub open spec fn subset_u(u: Index<UnitRef>, w: Index<UnitRef>) -> bool {
    forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(u, fy, name)] u_has(u, fy, name) ==> u_has(w, fy, name)
}

/// The moved groups are the frontier groups of the first units outside u, which are all recomputed; every other frontier group ranks after them.
pub proof fn lemma_pick_facts(c: ClassRec, u: Index<UnitRef>, w: Index<UnitRef>, refill: bool)
    requires
        crec_struct(c), uset_ok(u), subset_u(u, w),
        refill && notu_keys(c.r.entries(), c.fy as int, u).len() < k_limit() ==>
            forall|k: KeyView| vsel(c.f.entries(), c.fy as int, u).contains(k) ==> wn(w, c.fy as int, nkey_v(ov(c.f.at(k)->0.name))),
    ensures ({
        let pk = pick_of(c, u, refill);
        &&& forall|i: int| 0 <= i < pk.len() ==> c.f.at(rkey(#[trigger] pk[i])) == Some(pk[i])
        &&& forall|i: int| 0 <= i < pk.len() ==> i < vsel(c.f.entries(), c.fy as int, u).len() && rkey(#[trigger] pk[i]) == vsel(c.f.entries(), c.fy as int, u)[i]
        &&& forall|i: int| 0 <= i < pk.len() ==> wn(w, c.fy as int, nkey_v(ov((#[trigger] pk[i]).name))) && !wn(u, c.fy as int, nkey_v(ov(pk[i].name)))
        &&& forall|i: int, q: KeyView| #![trigger pk[i], c.f.at(q)] 0 <= i < pk.len() && c.f.at(q) is Some && !wn(w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name))) ==> key_lt(rkey(pk[i]), q)
    }),
{
    let pk = pick_of(c, u, refill);
    if refill && notu_keys(c.r.entries(), c.fy as int, u).len() < k_limit() {
        c.f.model();
        let es = c.f.entries();
        assert(ordered_es(es));
        lemma_notu_sorted(es, c.fy as int, u);
        lemma_vsel_prefix(es, c.fy as int, u);
        let vs = vsel(es, c.fy as int, u);
        let nk = notu_keys(es, c.fy as int, u);
        assert(pk.len() == vs.len());
        assert forall|i: int| 0 <= i < pk.len() implies c.f.at(rkey(#[trigger] pk[i])) == Some(pk[i]) by {
            let k = vs[i];
            assert(nk.contains(k)) by { assert(nk.take(if nk.len() < k_refill() { nk.len() as int } else { k_refill() })[i] == nk[i]); }
            let j = choose|j: int| 0 <= j < es.len() && es[j].0 == k && !u_has(u, c.fy as int, ov(es[j].1.name));
            assert(c.f.at(es[j].0) == Some(es[j].1));
            assert(pk[i] == c.f.at(k)->0);
            assert(rkey(pk[i]) == k) by { assert(c.f.at(k) is Some); }
        }
        assert forall|i: int| 0 <= i < pk.len() implies i < vs.len() && rkey(#[trigger] pk[i]) == vs[i] by {
            let k = vs[i];
            assert(nk.contains(k)) by { assert(nk.take(if nk.len() < k_refill() { nk.len() as int } else { k_refill() })[i] == nk[i]); }
            let j = choose|j: int| 0 <= j < es.len() && es[j].0 == k && !u_has(u, c.fy as int, ov(es[j].1.name));
            assert(c.f.at(es[j].0) == Some(es[j].1));
            assert(pk[i] == c.f.at(k)->0);
            assert(rkey(pk[i]) == k);
        }
        assert forall|i: int| 0 <= i < pk.len() implies wn(w, c.fy as int, nkey_v(ov((#[trigger] pk[i]).name))) && !wn(u, c.fy as int, nkey_v(ov(pk[i].name))) by {
            let k = vs[i];
            assert(vs.contains(k));
            assert(nk.contains(k));
            let j = choose|j: int| 0 <= j < es.len() && es[j].0 == k && !u_has(u, c.fy as int, ov(es[j].1.name));
            assert(c.f.at(es[j].0) == Some(es[j].1));
            assert(pk[i] == es[j].1);
        }
        assert forall|i: int, q: KeyView| #![trigger pk[i], c.f.at(q)] 0 <= i < pk.len() && c.f.at(q) is Some && !wn(w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name))) implies key_lt(rkey(pk[i]), q) by {
            let h = c.f.at(q)->0;
            assert(!u_has(u, c.fy as int, ov(h.name)));
            let pos = choose|pos: int| #![auto] 0 <= pos < es.len() && es[pos].0 == q;
            assert(es[pos].1 == h);
            assert(nk.contains(q));
            if vs.contains(q) {
                assert(wn(w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name))));
            }
            assert(!vs.contains(q));
            let k0 = vs[i];
            assert(vs.contains(k0));
            assert(nk.contains(k0));
            let j0 = choose|j: int| 0 <= j < es.len() && es[j].0 == k0 && !u_has(u, c.fy as int, ov(es[j].1.name));
            assert(c.f.at(es[j0].0) == Some(es[j0].1));
            assert(pk[i] == c.f.at(k0)->0);
            assert(rkey(pk[i]) == k0);
        }
    } else {
        assert(pk =~= Seq::<Grp>::empty());
    }
}

/// Evicting units and moving the refill groups keeps the record well formed and exact, and the groups of the other units accounted for.
#[verifier::rlimit(200)]
#[verifier::spinoff_prover]
pub proof fn lemma_evict_ok(db: Db, c: ClassRec, n: ClassRec, u: Index<UnitRef>, w: Index<UnitRef>, refill: bool)
    requires
        crec_struct(c), crec_sem(db, c), uset_ok(u), subset_u(u, w),
        evict_rel(c, n, u, w, pick_of(c, u, refill)),
        refill && notu_keys(c.r.entries(), c.fy as int, u).len() < k_limit() ==>
            forall|k: KeyView| vsel(c.f.entries(), c.fy as int, u).contains(k) ==> wn(w, c.fy as int, nkey_v(ov(c.f.at(k)->0.name))),
    ensures
        crec_struct(n), crec_sem(db, n),
        forall|k: GK, s: int, m: int| #![trigger covered_in(n, k, s, m)]
            in_class(c, k) && covered_in(c, k, s, m) && !u_has(w, k.fy, k.name) ==> covered_in(n, k, s, m),
{
    lemma_pick_facts(c, u, w, refill);
    let pk = pick_of(c, u, refill);
    // a moved group has the unit's frontier slot: two groups with the same key are the same group
    assert forall|a: Grp, b: Grp| c.f.at(rkey(a)) == Some(a) && c.f.at(rkey(b)) == Some(b) && a.ik() == b.ik() implies a == b by {
        lemma_ikey_inj(ov(a.name), a.tag@, ov(a.plabel), ov(b.name), b.tag@, ov(b.plabel));
        assert(c.fb.at(nkey_v(ov(a.name))) == c.f.at(rkey(a)));
        assert(c.fb.at(nkey_v(ov(b.name))) == c.f.at(rkey(b)));
    }
    assert forall|q: KeyView| n.r.at(q) is Some implies q == rkey(n.r.at(q)->0) by {
        if mvd(pk, q) { let i = choose|i: int| 0 <= i < pk.len() && rkey(pk[i]) == q; assert(n.r.at(rkey(pk[i])) == Some(pk[i])); }
        else { assert(c.r.at(q) is Some); }
    }
    assert forall|q: KeyView| n.f.at(q) is Some implies {
        &&& q == rkey(n.f.at(q)->0)
        &&& n.fb.at(nkey_v(ov(n.f.at(q)->0.name))) == n.f.at(q)
    } by {
        let h = c.f.at(q)->0;
        assert(c.fb.at(nkey_v(ov(h.name))) == c.f.at(q));
    }
    assert forall|nk: KeyView| n.fb.at(nk) is Some implies {
        &&& nk == nkey_v(ov(n.fb.at(nk)->0.name))
        &&& n.f.at(rkey(n.fb.at(nk)->0)) == n.fb.at(nk)
    } by {
        let h = c.fb.at(nk)->0;
        assert(c.f.at(rkey(h)) == c.fb.at(nk));
    }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.f.at(r2) is Some implies key_lt(r1, r2) && n.r.at(r1)->0.ik() != n.f.at(r2)->0.ik() by {
        let h = c.f.at(r2)->0;
        if mvd(pk, r1) {
            let i = choose|i: int| 0 <= i < pk.len() && rkey(pk[i]) == r1;
            assert(n.r.at(rkey(pk[i])) == Some(pk[i]));
            assert(key_lt(rkey(pk[i]), r2));
            if pk[i].ik() == h.ik() { assert(pk[i] == h); assert(wn(w, c.fy as int, nkey_v(ov(h.name)))); assert(false); }
        } else {
            assert(c.r.at(r1) is Some);
        }
    }
    assert forall|r1: KeyView, r2: KeyView| n.r.at(r1) is Some && n.r.at(r2) is Some && n.r.at(r1)->0.ik() == n.r.at(r2)->0.ik() implies r1 == r2 by {
        if mvd(pk, r1) && mvd(pk, r2) {
            let i = choose|i: int| 0 <= i < pk.len() && rkey(pk[i]) == r1;
            let j = choose|j: int| 0 <= j < pk.len() && rkey(pk[j]) == r2;
            assert(n.r.at(rkey(pk[i])) == Some(pk[i]) && n.r.at(rkey(pk[j])) == Some(pk[j]));
            assert(pk[i] == pk[j]);
        } else if mvd(pk, r1) {
            let i = choose|i: int| 0 <= i < pk.len() && rkey(pk[i]) == r1;
            assert(n.r.at(rkey(pk[i])) == Some(pk[i]));
            assert(c.r.at(r2) is Some);
            assert(c.f.at(rkey(pk[i])) == Some(pk[i]));
        } else if mvd(pk, r2) {
            let j = choose|j: int| 0 <= j < pk.len() && rkey(pk[j]) == r2;
            assert(n.r.at(rkey(pk[j])) == Some(pk[j]));
            assert(c.r.at(r1) is Some);
            assert(c.f.at(rkey(pk[j])) == Some(pk[j]));
        } else {
            assert(c.r.at(r1) is Some && c.r.at(r2) is Some);
        }
    }
    assert forall|nk: KeyView| n.hp.at(nk) is Some implies nk.0 == 0 by { assert(c.hp.at(nk) is Some); }
    assert forall|nk: KeyView| n.hn.at(nk) is Some implies nk.0 == 0 by { assert(c.hn.at(nk) is Some); }
    assert forall|q: KeyView| n.r.at(q) is Some implies exact_g(db, n, n.r.at(q)->0) by {
        if mvd(pk, q) { let i = choose|i: int| 0 <= i < pk.len() && rkey(pk[i]) == q; assert(n.r.at(rkey(pk[i])) == Some(pk[i])); assert(c.f.at(rkey(pk[i])) == Some(pk[i])); }
        else { assert(c.r.at(q) is Some); }
    }
    assert forall|q: KeyView| n.f.at(q) is Some implies exact_g(db, n, n.f.at(q)->0) by { assert(c.f.at(q) is Some); }
    assert forall|nk: KeyView| n.hp.at(nk) is Some implies has_cand(db, n, nk, true) by { assert(c.hp.at(nk) is Some); }
    assert forall|nk: KeyView| n.hn.at(nk) is Some implies has_cand(db, n, nk, false) by { assert(c.hn.at(nk) is Some); }
    assert forall|k: GK, s: int, m: int| in_class(c, k) && covered_in(c, k, s, m) && !u_has(w, k.fy, k.name) implies #[trigger] covered_in(n, k, s, m) by {
        assert(wn(w, c.fy as int, nkey_v(k.name)) == u_has(w, k.fy, k.name));
        assert(!u_has(u, k.fy, k.name));
        if s <= max_val() && s >= -max_val() && r_has(c, k) {
            let q = choose|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && cls_gk(c, c.r.at(q)->0) == k;
            assert(!mvd(pk, q)) by {
                if mvd(pk, q) { let i = choose|i: int| 0 <= i < pk.len() && rkey(pk[i]) == q; assert(c.f.at(rkey(pk[i])) == Some(pk[i])); lemma_rkey_ik_eq(pk[i], c.r.at(q)->0); }
            }
            assert(n.r.at(q) == c.r.at(q));
            assert(cls_gk(n, n.r.at(q)->0) == k);
        }
    }
}

} // verus!

verus! {

/// What the loop over the classes does to the table.
pub open spec fn evict_tbl(cls: Index<ClassRec>, t: Index<ClassRec>, cw: Index<ClassRef>, ct: Index<ClassRef>, u: Index<UnitRef>, w: Index<UnitRef>) -> bool {
    &&& t.wf()
    &&& forall|ok: KeyView| #![trigger t.at(ok)] cls.at(ok) is None ==> t.at(ok) is None
    &&& forall|ok: KeyView| #![trigger cls.at(ok)] cls.at(ok) is Some ==> t.at(ok) is Some && (
        if cw.at(ok) is Some { evict_rel(cls.at(ok)->0, t.at(ok)->0, u, w, pick_of(cls.at(ok)->0, u, ct.at(ok) is Some)) } else { t.at(ok) == cls.at(ok) })
}

/// The refill groups are recomputed units.
pub open spec fn refill_ok(cls: Index<ClassRec>, ct: Index<ClassRef>, u: Index<UnitRef>, w: Index<UnitRef>) -> bool {
    forall|ok: KeyView, k: KeyView| #![trigger ct.at(ok), cls.at(ok), vsel(cls.at(ok)->0.f.entries(), cls.at(ok)->0.fy as int, u).contains(k)]
        ct.at(ok) is Some && cls.at(ok) is Some && notu_keys(cls.at(ok)->0.r.entries(), cls.at(ok)->0.fy as int, u).len() < k_limit()
        && vsel(cls.at(ok)->0.f.entries(), cls.at(ok)->0.fy as int, u).contains(k) ==>
        wn(w, cls.at(ok)->0.fy as int, nkey_v(ov(cls.at(ok)->0.f.at(k)->0.name)))
}

pub open spec fn kept_tbl(cls: Index<ClassRec>, t: Index<ClassRec>, cw: Index<ClassRef>, u: Index<UnitRef>, w: Index<UnitRef>, kept: GMap) -> bool {
    &&& nm_ok(kept)
    &&& forall|ok: KeyView, ik: KeyView| #![trigger gm_at(kept, ok, ik)] gm_at(kept, ok, ik) is Some ==> {
        let g = gm_at(kept, ok, ik)->0;
        &&& cw.at(ok) is Some && t.at(ok) is Some
        &&& g.ik() == ik
        &&& t.at(ok)->0.r.at(rkey(g)) == Some(g)
    }
    &&& forall|ok: KeyView, q: KeyView| #![trigger t.at(ok), t.at(ok)->0.r.at(q)] cw.at(ok) is Some && t.at(ok) is Some && t.at(ok)->0.r.at(q) is Some ==> {
        let c = t.at(ok)->0;
        let g = c.r.at(q)->0;
        wn(w, c.fy as int, nkey_v(ov(g.name))) && !wn(u, c.fy as int, nkey_v(ov(g.name))) ==> gm_at(kept, ok, g.ik()) == Some(g)
    }
}

/// Stage A over the classes of cw: evict, class by class.
#[verifier::rlimit(200)]
#[verifier::spinoff_prover]
pub fn evict_all(cls: &Index<ClassRec>, cw: &Index<ClassRef>, ct: &Index<ClassRef>, u: &Index<UnitRef>, w: &Index<UnitRef>) -> (r: (Index<ClassRec>, GMap))
    requires
        tbl_str(*cls), cset_ok(*cw), cset_ok(*ct), uset_ok(*u), uset_ok(*w), subset_u(*u, *w), refill_ok(*cls, *ct, *u, *w),
    ensures
        evict_tbl(*cls, r.0, *cw, *ct, *u, *w),
        kept_tbl(*cls, r.0, *cw, *u, *w, r.1),
{
    let mut t = sh(cls);
    let mut kept: GMap = Index::new();
    let ents = cw.rows();
    let ghost es = cw.entries();
    proof { cw.model(); verus_prooflib::ordered::lemma_ordered_unique(es); }
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(), tbl_str(*cls), cset_ok(*cw), cset_ok(*ct), uset_ok(*u), uset_ok(*w), subset_u(*u, *w), refill_ok(*cls, *ct, *u, *w), cw.wf(),
            ents@.map_values(|row: (Key, Arc<ClassRef>)| (row.0.view(), *row.1)) == es, es == cw.entries(),
            forall|pos: int| #![trigger es[pos]] 0 <= pos < es.len() ==> cw.at(es[pos].0) == Some(es[pos].1),
            forall|a: int, b: int| 0 <= a < b < es.len() ==> (#[trigger] es[a]).0 != (#[trigger] es[b]).0,
            t.wf(), nm_ok(kept),
            forall|ok: KeyView| #![trigger t.at(ok)] cls.at(ok) is None ==> t.at(ok) is None,
            forall|ok: KeyView| #![trigger cls.at(ok)] cls.at(ok) is Some ==> t.at(ok) is Some && (
                if (exists|j: int| 0 <= j < i && #[trigger] es[j].0 == ok) { evict_rel(cls.at(ok)->0, t.at(ok)->0, *u, *w, pick_of(cls.at(ok)->0, *u, ct.at(ok) is Some)) } else { t.at(ok) == cls.at(ok) }),
            forall|ok: KeyView, ik: KeyView| #![trigger gm_at(kept, ok, ik)] gm_at(kept, ok, ik) is Some ==> {
                let g = gm_at(kept, ok, ik)->0;
                &&& (exists|j: int| 0 <= j < i && #[trigger] es[j].0 == ok) && t.at(ok) is Some
                &&& g.ik() == ik
                &&& t.at(ok)->0.r.at(rkey(g)) == Some(g)
            },
            forall|ok: KeyView, q: KeyView| #![trigger t.at(ok), t.at(ok)->0.r.at(q)] (exists|j: int| 0 <= j < i && #[trigger] es[j].0 == ok) && t.at(ok) is Some && t.at(ok)->0.r.at(q) is Some ==> {
                let c = t.at(ok)->0;
                let g = c.r.at(q)->0;
                wn(*w, c.fy as int, nkey_v(ov(g.name))) && !wn(*u, c.fy as int, nkey_v(ov(g.name))) ==> gm_at(kept, ok, g.ik()) == Some(g)
            },
        decreases ents.len() - i,
    {
        let key = &ents[i].0;
        proof { assert(es[i as int].0 == key.view()); }
        match t.get(key) {
            None => {}
            Some(c) => {
                let ghost kept0 = kept;
                let ghost t0 = t;
                let refill = ct.get(key).is_some();
                proof {
                    assert(t.at(key.view()) == Some(arc_val(&c)));
                    assert(cls.at(key.view()) is Some);
                    assert(!(exists|j: int| 0 <= j < i && es[j].0 == key.view())) by {
                        if exists|j: int| 0 <= j < i && es[j].0 == key.view() { let j = choose|j: int| 0 <= j < i && es[j].0 == key.view(); assert(es[j].0 != es[i as int].0); }
                    }
                    assert(arc_val(&c) == cls.at(key.view())->0);
                    assert(refill == (ct.at(key.view()) is Some));
                }
                let n = evict_class(&c, u, w, &mut kept, refill);
                cls_store(&mut t, n);
                proof {
                    let c0 = arc_val(&c);
                    assert(cls_key(n) == cls_key(c0));
                    assert(cls_key(c0) == key.view());
                    assert(refill ==> ct.at(key.view()) is Some);
                    assert(refill && notu_keys(c0.r.entries(), c0.fy as int, *u).len() < k_limit() ==>
                        forall|k: KeyView| vsel(c0.f.entries(), c0.fy as int, *u).contains(k) ==> wn(*w, c0.fy as int, nkey_v(ov(c0.f.at(k)->0.name))));
                }
            }
        }
        i += 1;
    }
    (t, kept)
}

} // verus!
