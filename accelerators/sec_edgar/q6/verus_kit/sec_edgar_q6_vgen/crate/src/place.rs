// Stage C of an update: each recomputed group takes its place in its class.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_key_order, lemma_key_trans, compare};
use verus_prooflib::wide::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::f64bits::*;
use crate::grp::*;
use crate::inv::*;
use crate::crops::*;
use crate::crlem::*;
use crate::tbl::*;
use crate::units::*;
use crate::kept::*;
use crate::evict::*;
use crate::cnt::*;
use crate::wideval::*;
use crate::rep::*;

verus! {

/// The kept groups registered in `kept` are still in their class's kept groups.
pub open spec fn ks_ok(kept: GMap, t: Index<ClassRec>) -> bool {
    forall|ok: KeyView, ik: KeyView| #![trigger gm_at(kept, ok, ik)] gm_at(kept, ok, ik) is Some ==> {
        let g = gm_at(kept, ok, ik)->0;
        t.at(ok) is Some && g.ik() == ik && t.at(ok)->0.r.at(rkey(g)) == Some(g)
    }
}

pub open spec fn full_all(t: Index<ClassRec>) -> bool {
    forall|ok: KeyView| #![trigger t.at(ok)] t.at(ok) is Some ==> crec_full(t.at(ok)->0)
}

/// What is left of a recomputed unit in the table is a group placed already (or, for a unit that only refills, a kept group).
pub open spec fn purge_ok(t: Index<ClassRec>, kept: GMap, u: Index<UnitRef>, w: Index<UnitRef>, done: spec_fn(GK) -> bool) -> bool {
    forall|ok: KeyView| #![trigger t.at(ok)] t.at(ok) is Some ==> {
        let c = t.at(ok)->0;
        &&& forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some && wn(w, c.fy as int, nkey_v(ov(c.f.at(q)->0.name))) ==> done(cls_gk(c, c.f.at(q)->0))
        &&& forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && wn(u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) ==> done(cls_gk(c, c.r.at(q)->0))
        &&& forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && wn(w, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) && !wn(u, c.fy as int, nkey_v(ov(c.r.at(q)->0.name))) ==>
            done(cls_gk(c, c.r.at(q)->0)) || gm_at(kept, ok, c.r.at(q)->0.ik()) == c.r.at(q)
    }
}

pub open spec fn mid_cov(w: Index<UnitRef>, done: spec_fn(GK) -> bool) -> spec_fn(GK) -> bool {
    |k: GK| !u_has(w, k.fy, k.name) || done(k)
}

pub open spec fn mid_ok(db2: Db, t: Index<ClassRec>, kept: GMap, u: Index<UnitRef>, w: Index<UnitRef>, done: spec_fn(GK) -> bool) -> bool {
    &&& tbl_wf(db2, t)
    &&& cov_where(db2, t, mid_cov(w, done))
    &&& ks_ok(kept, t)
    &&& purge_ok(t, kept, u, w, done)
    &&& full_all(t)
}

/// What placing the group k does to the class record c, giving n.
pub open spec fn step_ok(db2: Db, c: ClassRec, n: ClassRec, k: GK, s: int, m: int) -> bool {
    &&& crec_struct(n) && crec_sem(db2, n) && crec_full(n) && crec_head(c, n) && cov_mono(c, n)
    &&& covered_in(n, k, s, m)
    &&& forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) is Some ==> c.f.at(q) == n.f.at(q) || cls_gk(c, n.f.at(q)->0) == k
    &&& forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) is Some ==> c.r.at(q) == n.r.at(q) || cls_gk(c, n.r.at(q)->0) == k
    &&& forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> n.r.at(q) == c.r.at(q)
}

/// A step on a class record gives a step on the table.
pub proof fn lemma_place_mid(db2: Db, t: Index<ClassRec>, t2: Index<ClassRec>, c: ClassRec, n: ClassRec, kept: GMap,
    u: Index<UnitRef>, w: Index<UnitRef>, done: spec_fn(GK) -> bool, k: GK)
    requires
        mid_ok(db2, t, kept, u, w, done),
        cntk(db2, k) > 0, k.fy == c.fy as int && k.stmt == c.stmt@ && k.uom == c.uom@,
        t.at(cls_key(c)) is Some ==> t.at(cls_key(c))->0 == c,
        t.at(cls_key(c)) is None ==> crec_is_empty(c),
        step_ok(db2, c, n, k, sumk(db2, k), cntk(db2, k)),
        t2.wf(), forall|q: KeyView| #![trigger t2.at(q)] t2.at(q) == if q == cls_key(c) { Some(n) } else { t.at(q) },
        cls_key(n) == cls_key(c),
        wn(w, c.fy as int, nkey_v(k.name)),
    ensures mid_ok(db2, t2, kept, u, w, ins(done, k)),
{
    let ok = cls_key(c);
    if t.at(ok) is None { lemma_empty_ok(db2, c); }
    assert forall|o: KeyView| t2.at(o) is Some implies ({
        let cc = t2.at(o)->0;
        &&& o == cls_key(cc) && crec_struct(cc) && crec_sem(db2, cc)
    }) by { if o != ok { assert(t.at(o) is Some); } }
    assert(tbl_wf(db2, t2));
    // coverage
    assert forall|k2: GK| cntk(db2, k2) > 0 && mid_cov(w, ins(done, k))(k2) implies #[trigger] covered(db2, t2, k2) by {
        let ok2 = okey(k2.fy, k2.stmt, k2.uom);
        if ok2 == ok {
            lemma_okey_inj(k2.fy, k2.stmt, k2.uom, c.fy as int, c.stmt@, c.uom@);
            assert(in_class(c, k2));
            if k2 == k {
                assert(covered_in(n, k2, sumk(db2, k2), cntk(db2, k2)));
            } else {
                assert(mid_cov(w, done)(k2));
                assert(covered(db2, t, k2));
                assert(t.at(ok) is Some);
                assert(covered_in(c, k2, sumk(db2, k2), cntk(db2, k2)));
            }
        } else {
            assert(k2 != k);
            assert(t2.at(ok2) == t.at(ok2));
            assert(mid_cov(w, done)(k2));
        }
    }
    // kept groups stay
    assert forall|o: KeyView, ik: KeyView| gm_at(kept, o, ik) is Some implies ({
        let g = gm_at(kept, o, ik)->0;
        t2.at(o) is Some && g.ik() == ik && t2.at(o)->0.r.at(rkey(g)) == Some(g)
    }) by {
        let g = gm_at(kept, o, ik)->0;
        if o == ok { assert(t.at(ok) is Some); assert(c.r.at(rkey(g)) == Some(g)); assert(n.r.at(rkey(g)) == c.r.at(rkey(g))); }
    }
    // what is left of recomputed units
    assert forall|o: KeyView| t2.at(o) is Some implies ({
        let cc = t2.at(o)->0;
        &&& forall|q: KeyView| #![trigger cc.f.at(q)] cc.f.at(q) is Some && wn(w, cc.fy as int, nkey_v(ov(cc.f.at(q)->0.name))) ==> ins(done, k)(cls_gk(cc, cc.f.at(q)->0))
        &&& forall|q: KeyView| #![trigger cc.r.at(q)] cc.r.at(q) is Some && wn(u, cc.fy as int, nkey_v(ov(cc.r.at(q)->0.name))) ==> ins(done, k)(cls_gk(cc, cc.r.at(q)->0))
        &&& forall|q: KeyView| #![trigger cc.r.at(q)] cc.r.at(q) is Some && wn(w, cc.fy as int, nkey_v(ov(cc.r.at(q)->0.name))) && !wn(u, cc.fy as int, nkey_v(ov(cc.r.at(q)->0.name))) ==>
            ins(done, k)(cls_gk(cc, cc.r.at(q)->0)) || gm_at(kept, o, cc.r.at(q)->0.ik()) == cc.r.at(q)
    }) by {
        if o != ok {
            assert(t2.at(o) == t.at(o));
        } else {
            assert(cls_key(c) == o);
            assert forall|q: KeyView| n.f.at(q) is Some && wn(w, n.fy as int, nkey_v(ov(n.f.at(q)->0.name))) implies ins(done, k)(cls_gk(n, n.f.at(q)->0)) by {
                if c.f.at(q) == n.f.at(q) { assert(t.at(ok) is Some); assert(cls_gk(n, n.f.at(q)->0) == cls_gk(c, c.f.at(q)->0)); }
                else { assert(cls_gk(c, n.f.at(q)->0) == k); assert(cls_gk(n, n.f.at(q)->0) == k); }
            }
            assert forall|q: KeyView| n.r.at(q) is Some && wn(u, n.fy as int, nkey_v(ov(n.r.at(q)->0.name))) implies ins(done, k)(cls_gk(n, n.r.at(q)->0)) by {
                if c.r.at(q) == n.r.at(q) { assert(t.at(ok) is Some); assert(cls_gk(n, n.r.at(q)->0) == cls_gk(c, c.r.at(q)->0)); }
                else { assert(cls_gk(c, n.r.at(q)->0) == k); assert(cls_gk(n, n.r.at(q)->0) == k); }
            }
            assert forall|q: KeyView| n.r.at(q) is Some && wn(w, n.fy as int, nkey_v(ov(n.r.at(q)->0.name))) && !wn(u, n.fy as int, nkey_v(ov(n.r.at(q)->0.name))) implies
                (ins(done, k)(cls_gk(n, n.r.at(q)->0)) || gm_at(kept, o, n.r.at(q)->0.ik()) == n.r.at(q)) by {
                if c.r.at(q) == n.r.at(q) { assert(t.at(ok) is Some); assert(cls_gk(n, n.r.at(q)->0) == cls_gk(c, c.r.at(q)->0)); }
                else { assert(cls_gk(n, n.r.at(q)->0) == k); }
            }
        }
    }
    assert forall|o: KeyView| t2.at(o) is Some implies crec_full(t2.at(o)->0) by { if o != ok { assert(t.at(o) is Some); } }
}

} // verus!

verus! {

pub proof fn lemma_ix_same_len<V>(a: Index<V>, b: Index<V>)
    requires ix_same(a, b),
    ensures a.entries().len() == b.entries().len(),
{}

/// A group above or below the double range is marked for its unit.
pub fn step_h(c: &ClassRec, e: &AccEnt, above: bool, Ghost(db2): Ghost<Db>) -> (n: ClassRec)
    requires
        crec_struct(*c), crec_sem(db2, *c), crec_full(*c),
        cntk(db2, e.gk()) > 0, in_class(*c, e.gk()),
        above ==> sumk(db2, e.gk()) > max_val(), !above ==> sumk(db2, e.gk()) < -max_val(),
    ensures step_ok(db2, *c, n, e.gk(), sumk(db2, e.gk()), cntk(db2, e.gk())),
{
    let nk = nkey_of(&e.name);
    proof { assert(has_cand(db2, *c, nk.view(), above)) by { let k = e.gk(); assert(cntk(db2, k) > 0 && in_class(*c, k) && nkey_v(k.name) == nk.view()); } }
    if above {
        let n = crec_hp_put(c, nk);
        proof {
            lemma_hp_put_ok(db2, *c, n, nk.view());
            lemma_ix_same_len(n.r, c.r); lemma_ix_same_len(n.f, c.f);
            assert(covered_in(n, e.gk(), sumk(db2, e.gk()), cntk(db2, e.gk())));
        }
        n
    } else {
        let n = crec_hn_put(c, nk);
        proof {
            lemma_hn_put_ok(db2, *c, n, nk.view());
            lemma_ix_same_len(n.r, c.r); lemma_ix_same_len(n.f, c.f);
            lemma_max_pos();
            assert(covered_in(n, e.gk(), sumk(db2, e.gk()), cntk(db2, e.gk())));
        }
        n
    }
}

/// A group better than the whole frontier joins the kept groups.
pub fn step_r(c: &ClassRec, x: Grp, Ghost(db2): Ghost<Db>) -> (n: ClassRec)
    requires
        crec_struct(*c), crec_sem(db2, *c), crec_full(*c), exact_g(db2, *c, x),
        forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some ==> key_lt(rkey(x), q) && c.f.at(q)->0.ik() != x.ik(),
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> c.r.at(q)->0.ik() != x.ik(),
    ensures step_ok(db2, *c, n, cls_gk(*c, x), x.sum@, x.cnt@),
{
    let ghost xv = x;
    let n = crec_r_put(c, Arc::new(x));
    proof {
        lemma_r_put_ok(db2, *c, n, xv);
        lemma_len_le(c.r, n.r);
        lemma_ix_same_len(n.f, c.f);
        lemma_max_pos();
        assert(covered_in(n, cls_gk(*c, xv), xv.sum@, xv.cnt@));
        assert(cls_gk(n, xv) == cls_gk(*c, xv));
    }
    n
}

/// A group worse than the whole frontier becomes its unit's frontier group (the unit has none).
pub fn step_f_new(c: &ClassRec, x: Grp, Ghost(db2): Ghost<Db>) -> (n: ClassRec)
    requires
        crec_struct(*c), crec_sem(db2, *c), crec_full(*c), exact_g(db2, *c, x),
        c.f.entries().len() > 0,
        c.fb.at(nkey_v(ov(x.name))) is None,
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> key_lt(q, rkey(x)) && c.r.at(q)->0.ik() != x.ik(),
    ensures step_ok(db2, *c, n, cls_gk(*c, x), x.sum@, x.cnt@),
{
    let ghost xv = x;
    let n = crec_f_put(c, Arc::new(x));
    proof {
        lemma_f_put_ok(db2, *c, n, xv);
        lemma_ix_same_len(n.r, c.r);
        assert(cls_gk(n, xv) == cls_gk(*c, xv));
        assert(n.f.entries().len() > 0) by { lemma_len_le(c.f, n.f); }
    }
    n
}

/// A group better than its unit's frontier group (but worse than the whole frontier's first) replaces it.
pub fn step_f_swap(c: &ClassRec, x: Grp, b: &Arc<Grp>, Ghost(db2): Ghost<Db>) -> (n: ClassRec)
    requires
        crec_struct(*c), crec_sem(db2, *c), crec_full(*c), exact_g(db2, *c, x),
        c.fb.at(nkey_v(ov(x.name))) == Some(**b), key_lt(rkey(x), rkey(**b)),
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> key_lt(q, rkey(x)) && c.r.at(q)->0.ik() != x.ik(),
    ensures step_ok(db2, *c, n, cls_gk(*c, x), x.sum@, x.cnt@),
{
    let ghost xv = x;
    let ghost bv = arc_val(b);
    let nk = nkey_of(&x.name);
    let n1 = crec_f_del(c, &nk);
    let n = crec_f_put(&n1, Arc::new(x));
    proof {
        assert(nk.view() == nkey_v(ov(x.name)));
        lemma_ikey_name_eq(xv, bv);
        lemma_key_order(rkey(xv), rkey(bv));
        assert(c.f.at(rkey(bv)) == c.fb.at(nkey_v(ov(bv.name))));
        lemma_f_swap_ok(db2, *c, n, bv, xv);
        lemma_ix_same_len(n.r, c.r);
        assert(n.f.entries().len() > 0) by { lemma_len_le(c.f, c.f); }
        assert(c.f.entries().len() > 0) by { c.f.model(); }
        assert(cls_gk(n, xv) == cls_gk(*c, xv));
    }
    n
}

pub proof fn lemma_ikey_name_eq(a: Grp, b: Grp)
    requires nkey_v(ov(a.name)) == nkey_v(ov(b.name)),
    ensures ov(a.name) == ov(b.name),
{
    lemma_esc_opt_split(ov(a.name), Seq::empty(), ov(b.name), Seq::empty());
}

} // verus!

verus! {

pub fn kept_has(kept: &GMap, ok: &Key, ik: &Key) -> (r: bool)
    requires nm_ok(*kept),
    ensures r == (gm_at(*kept, ok.view(), ik.view()) is Some),
{
    match kept.get(ok) {
        None => false,
        Some(inner) => {
            proof { assert(kept.at(ok.view()) == Some(*inner)); }
            inner.get(ik).is_some()
        }
    }
}

/// The first entry of an index is its least.
pub proof fn lemma_first_min(f: Index<Grp>)
    requires f.wf(), f.entries().len() > 0,
    ensures
        f.at(f.entries()[0].0) == Some(f.entries()[0].1),
        forall|q: KeyView| #![trigger f.at(q)] f.at(q) is Some ==> q == f.entries()[0].0 || key_lt(f.entries()[0].0, q),
{
    f.model();
    assert forall|q: KeyView| f.at(q) is Some implies q == f.entries()[0].0 || key_lt(f.entries()[0].0, q) by {
        let pos = choose|pos: int| #![auto] 0 <= pos < f.entries().len() && f.entries()[pos].0 == q;
        if pos > 0 { assert(key_lt(f.entries()[0].0, f.entries()[pos].0)); }
    }
}

/// A group not placed yet and not kept has no stored group with its key in its class.
pub proof fn lemma_fresh(db2: Db, t: Index<ClassRec>, c: ClassRec, kept: GMap, u: Index<UnitRef>, w: Index<UnitRef>, done: spec_fn(GK) -> bool, k: GK, x: Grp)
    requires
        mid_ok(db2, t, kept, u, w, done), t.at(cls_key(c)) == Some(c), in_class(c, k), cls_gk(c, x) == k,
        wn(w, c.fy as int, nkey_v(k.name)), !done(k), gm_at(kept, cls_key(c), x.ik()) is None, subset_u(u, w),
    ensures
        forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some ==> c.f.at(q)->0.ik() != x.ik(),
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> c.r.at(q)->0.ik() != x.ik(),
{
    assert forall|q: KeyView| c.f.at(q) is Some implies c.f.at(q)->0.ik() != x.ik() by {
        let h = c.f.at(q)->0;
        if h.ik() == x.ik() {
            lemma_ikey_inj(ov(h.name), h.tag@, ov(h.plabel), ov(x.name), x.tag@, ov(x.plabel));
            assert(cls_gk(c, h) == k);
            assert(wn(w, c.fy as int, nkey_v(ov(h.name))));
            assert(done(cls_gk(c, h)));
        }
    }
    assert forall|q: KeyView| c.r.at(q) is Some implies c.r.at(q)->0.ik() != x.ik() by {
        let g = c.r.at(q)->0;
        if g.ik() == x.ik() {
            lemma_ikey_inj(ov(g.name), g.tag@, ov(g.plabel), ov(x.name), x.tag@, ov(x.plabel));
            assert(cls_gk(c, g) == k);
            assert(wn(w, c.fy as int, nkey_v(ov(g.name))));
            if wn(u, c.fy as int, nkey_v(ov(g.name))) { assert(done(cls_gk(c, g))); }
            else { assert(done(cls_gk(c, g)) || gm_at(kept, cls_key(c), g.ik()) == c.r.at(q)); }
        }
    }
}

proof fn lemma_empty_fresh(c: ClassRec, x: Grp)
    requires crec_is_empty(c),
    ensures
        forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some ==> c.f.at(q)->0.ik() != x.ik(),
        forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some ==> c.r.at(q)->0.ik() != x.ik(),
{}

} // verus!

verus! {

/// Places the recomputed group e in its class: kept, or frontier, or marked above / below the double range.
#[verifier::rlimit(300)]
#[verifier::spinoff_prover]
pub fn place(t: &mut Index<ClassRec>, e: &AccEnt, kept: &GMap, u: &Index<UnitRef>, w: &Index<UnitRef>,
    tab: &Pow2Tab, maxv: &Wide, minv: &Wide, Ghost(db2): Ghost<Db>, Ghost(done): Ghost<spec_fn(GK) -> bool>)
    requires
        mid_ok(db2, *old(t), *kept, *u, *w, done), uset_ok(*u), uset_ok(*w), subset_u(*u, *w), nm_ok(*kept),
        tab.ok(), maxv@ == max_val(), minv@ == -max_val(), rep_db(db2),
        e.cnt@ == cntk(db2, e.gk()), e.sum@ == sumk(db2, e.gk()), e.cnt@ > 0,
        wn(*w, e.fy as int, nkey_v(ov(e.name))), !done(e.gk()),
    ensures mid_ok(db2, *final(t), *kept, *u, *w, ins(done, e.gk())),
{
    let ghost k = e.gk();
    let ghost t0 = *t;
    let ok_key = okey_of(e.fy, e.stmt.as_slice(), e.uom.as_slice());
    let ik = ikey_of(&e.name, e.tag.as_slice(), &e.plabel);
    proof { lemma_max_pos(); }
    if kept_has(kept, &ok_key, &ik) {
        proof {
            let g = gm_at(*kept, ok_key.view(), ik.view())->0;
            let c = t0.at(ok_key.view())->0;
            assert(c.r.at(rkey(g)) == Some(g));
            assert(ok_key.view() == cls_key(c));
            lemma_okey_inj(c.fy as int, c.stmt@, c.uom@, e.fy as int, e.stmt@, e.uom@);
            lemma_ikey_inj(ov(g.name), g.tag@, ov(g.plabel), ov(e.name), e.tag@, ov(e.plabel));
            assert(cls_gk(c, g) == k);
            assert(exact_g(db2, c, g));
            assert(r_has(c, k));
            assert(covered_in(c, k, sumk(db2, k), cntk(db2, k)));
            assert forall|k2: GK| cntk(db2, k2) > 0 && mid_cov(*w, ins(done, k))(k2) implies #[trigger] covered(db2, t0, k2) by {
                if k2 == k { assert(covered(db2, t0, k)) by { assert(okey(k.fy, k.stmt, k.uom) == ok_key.view()); } }
                else { assert(mid_cov(*w, done)(k2)); }
            }
            assert(purge_ok(t0, *kept, *u, *w, ins(done, k)));
        }
        return;
    }
    let (c, existed) = cls_load(t, e.fy, e.stmt.as_slice(), e.uom.as_slice());
    let ghost cv = arc_val(&c);
    proof {
        assert(ok_key.view() == okey(e.fy as int, e.stmt@, e.uom@));
        if existed { assert(t0.at(cls_key(cv)) == Some(cv)); assert(cls_key(cv) == ok_key.view()); }
        else { lemma_empty_ok(db2, cv); assert(cls_key(cv) == ok_key.view()); }
        assert(in_class(cv, k));
    }
    let b = grp_bits(tab, maxv, minv, &e.cnt, &e.sum, Ghost(db2), Ghost(k));
    let n: ClassRec;
    match b {
        None => {
            let above = maxv.lt(&e.sum);
            proof { assert(e.sum@ > max_val() || e.sum@ < -max_val()); }
            n = step_h(&c, e, above, Ghost(db2));
        }
        Some(bits) => {
            let x = Grp { name: copy_opt(&e.name), tag: copy_bytes(e.tag.as_slice()), plabel: copy_opt(&e.plabel), cnt: e.cnt.dup(), sum: e.sum.dup(), bits };
            let ghost xv = x;
            proof {
                assert(cls_gk(cv, xv) == k);
                assert(exact_g(db2, cv, xv)) by { assert(f64_in_range(e.sum@)); }
                if existed { lemma_fresh(db2, t0, cv, *kept, *u, *w, done, k, xv); } else { lemma_empty_fresh(cv, xv); }
                assert(crec_struct(cv) && crec_sem(db2, cv) && crec_full(cv));
            }
            let rx = rkey_of(bits, &ik);
            let mut cur = c.f.cursor();
            let first = cur.next();
            proof { c.f.model(); }
            let to_r = match &first {
                None => true,
                Some((fk, fg)) => compare(&rx, fk) < 0,
            };
            if to_r {
                proof {
                    if cv.f.entries().len() > 0 {
                        lemma_first_min(cv.f);
                        let f0 = cv.f.entries()[0].0;
                        assert(first matches Some(row) && row.0.view() == f0);
                        assert forall|q: KeyView| cv.f.at(q) is Some implies key_lt(rkey(xv), q) by {
                            if q != f0 { lemma_key_trans(rkey(xv), f0, q); }
                        }
                    }
                }
                n = step_r(&c, x, Ghost(db2));
            } else {
                proof {
                    assert(cv.f.entries().len() > 0);
                    lemma_first_min(cv.f);
                    let f0 = cv.f.entries()[0].0;
                    assert(!key_lt(rkey(xv), f0));
                    // every kept group ranks before x
                    assert forall|q: KeyView| cv.r.at(q) is Some implies key_lt(q, rkey(xv)) by {
                        assert(key_lt(q, f0));
                        lemma_key_order(rkey(xv), f0);
                        let h0 = cv.f.at(f0)->0;
                        if rkey(xv) == f0 { lemma_rkey_fields(xv, h0); assert(xv.ik() == h0.ik()); assert(cv.f.at(f0) is Some); assert(false); }
                        assert(key_lt(f0, rkey(xv)));
                        lemma_key_trans(q, f0, rkey(xv));
                    }
                }
                let nk = nkey_of(&e.name);
                match c.fb.get(&nk) {
                    None => {
                        n = step_f_new(&c, x, Ghost(db2));
                    }
                    Some(bg) => {
                        let rb = {
                            let ikb = ikey_of(&bg.name, bg.tag.as_slice(), &bg.plabel);
                            rkey_of(bg.bits, &ikb)
                        };
                        if compare(&rx, &rb) < 0 {
                            proof { assert(cv.fb.at(nkey_v(ov(xv.name))) == Some(arc_val(&bg))); }
                            n = step_f_swap(&c, x, &bg, Ghost(db2));
                        } else {
                            proof {
                                let bv = arc_val(&bg);
                                assert(!key_lt(rkey(xv), rkey(bv)));
                                assert(cv.fb.at(nkey_v(k.name)) == Some(bv));
                                assert(okbits(xv.bits, xv.sum@, xv.cnt@));
                                assert(rkey_gk(xv.bits, k) == rkey(xv));
                                assert(covered_in(cv, k, xv.sum@, xv.cnt@));
                            }
                            proof {
                                assert(step_ok(db2, cv, cv, k, sumk(db2, k), cntk(db2, k))) by {
                                    assert(covered_in(cv, k, xv.sum@, xv.cnt@));
                                    assert(crec_head(cv, cv)) ;
                                    assert(forall|k1: GK, s1: int, m1: int| covered_in(cv, k1, s1, m1) ==> covered_in(cv, k1, s1, m1));
                                }
                                lemma_place_mid(db2, t0, t0, cv, cv, *kept, *u, *w, done, k);
                                assert(*t == t0);
                            }
                            return;
                        }
                    }
                }
            }
        }
    }
    proof { assert(step_ok(db2, cv, n, k, sumk(db2, k), cntk(db2, k))); }
    let ghost n0 = n;
    cls_store(t, n);
    proof { lemma_place_mid(db2, t0, *t, cv, n0, *kept, *u, *w, done, k); }
}

} // verus!

verus! {

/// The groups of the first i entries of the contributions that belong to recomputed units, with a positive count.
pub open spec fn done_of(es: Seq<(KeyView, AccEnt)>, i: int, w: Index<UnitRef>) -> spec_fn(GK) -> bool {
    (|k: GK| exists|j: int| 0 <= j < i && j < es.len() && (#[trigger] es[j]).0 == fkey(k) && u_has(w, k.fy, k.name) && es[j].1.cnt@ > 0)
}

pub open spec fn ins(done: spec_fn(GK) -> bool, k: GK) -> spec_fn(GK) -> bool {
    |x: GK| done(x) || x == k
}

pub open spec fn no_done() -> spec_fn(GK) -> bool {
    |x: GK| false
}

/// Stage C: places every recomputed group of the contributions.
#[verifier::rlimit(300)]
#[verifier::spinoff_prover]
pub fn place_all(t: &mut Index<ClassRec>, acc: &Index<AccEnt>, kept: &GMap, u: &Index<UnitRef>, w: &Index<UnitRef>,
    tab: &Pow2Tab, Ghost(db2): Ghost<Db>)
    requires
        mid_ok(db2, *old(t), *kept, *u, *w, no_done()), uset_ok(*u), uset_ok(*w), subset_u(*u, *w), nm_ok(*kept),
        tab.ok(), rep_db(db2), acc_ok(*acc),
        forall|k: GK| #![trigger acc_c(*acc, k)] u_has(*w, k.fy, k.name) ==> acc_c(*acc, k) == cntk(db2, k) && acc_s(*acc, k) == sumk(db2, k),
    ensures
        ({ let es = acc.entries(); mid_ok(db2, *final(t), *kept, *u, *w, done_of(es, es.len() as int, *w)) }),
        forall|k: GK| u_has(*w, k.fy, k.name) && cntk(db2, k) > 0 ==> done_of(acc.entries(), acc.entries().len() as int, *w)(k),
{
    proof { lemma_pow2_consts(); }
    let m53 = Wide::from_i128(9007199254740991);
    let maxv = m53.mul(&tab.t[2045]);
    let minv = maxv.neg();
    let ents = acc.rows();
    let ghost es = acc.entries();
    proof {
        acc.model();
        verus_prooflib::ordered::lemma_ordered_unique(es);
        assert(done_of(es, 0, *w) =~= no_done());
    }
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(), uset_ok(*u), uset_ok(*w), subset_u(*u, *w), nm_ok(*kept), tab.ok(), rep_db(db2), acc_ok(*acc), acc.wf(),
            maxv@ == max_val(), minv@ == -max_val(),
            ents@.map_values(|row: (Key, Arc<AccEnt>)| (row.0.view(), *row.1)) == es, es == acc.entries(),
            forall|pos: int| #![trigger es[pos]] 0 <= pos < es.len() ==> acc.at(es[pos].0) == Some(es[pos].1),
            forall|a: int, b: int| 0 <= a < b < es.len() ==> (#[trigger] es[a]).0 != (#[trigger] es[b]).0,
            forall|k: GK| #![trigger acc_c(*acc, k)] u_has(*w, k.fy, k.name) ==> acc_c(*acc, k) == cntk(db2, k) && acc_s(*acc, k) == sumk(db2, k),
            mid_ok(db2, *t, *kept, *u, *w, done_of(es, i as int, *w)),
        decreases ents.len() - i,
    {
        let ent = &ents[i].1;
        let ghost e = arc_val(ent);
        proof {
            assert(es[i as int] == (ents@[i as int].0.view(), e));
            assert(acc.at(es[i as int].0) == Some(e));
            assert(es[i as int].0 == fkey(e.gk()));
        }
        let nm = ukey_of(ent.fy, &ent.name);
        let in_w = w.get(&nm).is_some();
        let pos_cnt = ent.cnt.is_pos();
        proof { assert(nm.view() == ukey(e.fy as int, ov(e.name))); }
        if in_w && pos_cnt {
            let ghost done = done_of(es, i as int, *w);
            proof {
                let k = e.gk();
                assert(acc_c(*acc, k) == e.cnt@);
                assert(!done(k)) by {
                    if done(k) {
                        let j = choose|j: int| 0 <= j < i && j < es.len() && es[j].0 == fkey(k) && u_has(*w, k.fy, k.name) && es[j].1.cnt@ > 0;
                        assert(es[j].0 == es[i as int].0);
                    }
                }
                assert(u_has(*w, k.fy, k.name));
                assert(wn(*w, e.fy as int, nkey_v(ov(e.name))));
            }
            place(t, ent, kept, u, w, tab, &maxv, &minv, Ghost(db2), Ghost(done));
            proof {
                let k = e.gk();
                assert(ins(done, k) =~= done_of(es, i as int + 1, *w)) by {
                    assert forall|k2: GK| #![trigger done_of(es, i as int + 1, *w)(k2)] ins(done, k)(k2) == done_of(es, i as int + 1, *w)(k2) by {
                        if done_of(es, i as int + 1, *w)(k2) {
                            let j = choose|j: int| 0 <= j < i + 1 && j < es.len() && es[j].0 == fkey(k2) && u_has(*w, k2.fy, k2.name) && es[j].1.cnt@ > 0;
                            if j == i as int { lemma_fkey_inj(k2, k); }
                        }
                        if k2 == k { assert(es[i as int].0 == fkey(k2)); }
                        if done(k2) { let j = choose|j: int| 0 <= j < i && j < es.len() && es[j].0 == fkey(k2) && u_has(*w, k2.fy, k2.name) && es[j].1.cnt@ > 0; }
                    }
                }
            }
        } else {
            proof {
                assert(done_of(es, i as int + 1, *w) =~= done_of(es, i as int, *w)) by {
                    assert forall|k2: GK| #![trigger done_of(es, i as int + 1, *w)(k2)] done_of(es, i as int + 1, *w)(k2) == done_of(es, i as int, *w)(k2) by {
                        if done_of(es, i as int + 1, *w)(k2) {
                            let j = choose|j: int| 0 <= j < i + 1 && j < es.len() && es[j].0 == fkey(k2) && u_has(*w, k2.fy, k2.name) && es[j].1.cnt@ > 0;
                            if j == i as int { lemma_fkey_inj(k2, e.gk()); assert(u_has(*w, e.fy as int, ov(e.name))); assert(false); }
                        }
                        if done_of(es, i as int, *w)(k2) { let j = choose|j: int| 0 <= j < i && j < es.len() && es[j].0 == fkey(k2) && u_has(*w, k2.fy, k2.name) && es[j].1.cnt@ > 0; }
                    }
                }
            }
        }
        i += 1;
    }
    proof {
        assert forall|k: GK| u_has(*w, k.fy, k.name) && cntk(db2, k) > 0 implies done_of(es, es.len() as int, *w)(k) by {
            assert(acc_c(*acc, k) == cntk(db2, k));
            assert(acc.at(fkey(k)) is Some);
            let pos = choose|pos: int| #![auto] 0 <= pos < es.len() && es[pos].0 == fkey(k);
            assert(es[pos].1 == acc.at(fkey(k))->0);
        }
    }
}

} // verus!
