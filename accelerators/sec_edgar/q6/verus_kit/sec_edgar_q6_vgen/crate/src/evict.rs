// Stage A of an update: the recomputed units' stored groups leave their classes.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt};
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::grp::*;
use crate::inv::*;
use crate::crops::*;
use crate::units::*;
use crate::kept::*;

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

/// The class's record without the recomputed units' groups: the frontier side of every unit in w, the kept groups of
/// the units in u.  The kept groups of the other units in w stay, and are registered in `kept`.
#[verifier::rlimit(100)]
#[verifier::spinoff_prover]
pub fn evict_class(c: &ClassRec, u: &Index<UnitRef>, w: &Index<UnitRef>, kept: &mut GMap) -> (n: ClassRec)
    requires crec_struct(*c), uset_ok(*u), uset_ok(*w), nm_ok(*old(kept)),
    ensures
        crec_head(*c, n), n.r.wf(), n.f.wf(), n.fb.wf(), n.hp.wf(), n.hn.wf(),
        forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) == if c.r.at(q) is Some && wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) { None } else { c.r.at(q) },
        forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if c.f.at(q) is Some && wn(*w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name))) { None } else { c.f.at(q) },
        forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if c.fb.at(q) is Some && wn(*w, c.fy as int, q) { None } else { c.fb.at(q) },
        forall|q: KeyView| #![trigger n.hp.at(q)] n.hp.at(q) == if c.hp.at(q) is Some && wn(*w, c.fy as int, q) { None } else { c.hp.at(q) },
        forall|q: KeyView| #![trigger n.hn.at(q)] n.hn.at(q) == if c.hn.at(q) is Some && wn(*w, c.fy as int, q) { None } else { c.hn.at(q) },
        nm_ok(*final(kept)),
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && wn(*w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) && !wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) ==>
            gm_at(*final(kept), cls_key(*c), c.r.at(q)->0.ik()) == Some(c.r.at(q)->0),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(kept), ok, ik)] gm_at(*final(kept), ok, ik) is Some ==>
            gm_at(*old(kept), ok, ik) is Some || (ok == cls_key(*c) && exists|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && c.r.at(q)->0.ik() == ik
                && gm_at(*final(kept), ok, ik) == c.r.at(q) && n.r.at(q) == c.r.at(q)),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(kept), ok, ik)] ok != cls_key(*c) ==> gm_at(*final(kept), ok, ik) == gm_at(*old(kept), ok, ik),
{
    let n1 = evict_fside(c, w);
    proof {
        assert(cls_key(n1) == cls_key(*c));
        assert forall|q: KeyView| c.r.at(q) is Some implies n1.r.at(q) == c.r.at(q) by {}
        assert forall|q1: KeyView, q2: KeyView| n1.r.at(q1) is Some && n1.r.at(q2) is Some && n1.r.at(q1)->0.ik() == n1.r.at(q2)->0.ik() implies q1 == q2 by {}
    }
    let n2 = evict_rside(&n1, u, w, kept);
    proof {
        assert forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && wn(*w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) && !wn(*u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) implies
            gm_at(*kept, cls_key(*c), c.r.at(q)->0.ik()) == Some(c.r.at(q)->0) by {
            assert(n1.r.at(q) == c.r.at(q));
        }
    }
    n2
}

} // verus!
