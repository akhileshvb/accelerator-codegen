// Updates of one class record: the persistent operations and what each keeps of the invariant.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::bytes::*;
use verus_prooflib::lemma_ordered_equal;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, lemma_key_order, lemma_key_trans};
use verus_prooflib::wide::*;
use verus_prooflib::float::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::f64bits::*;
use crate::grp::*;
use crate::inv::*;

verus! {

/// The unit's index key: its name.
pub fn nkey_of(name: &Option<Vec<u8>>) -> (k: Key)
    ensures k.view() == nkey_v(ov(*name)),
{
    let mut text = Vec::new();
    append_esc_opt(&mut text, name);
    Key::composite(0, text)
}

/// A shared version of an index with the same lookups.
pub fn sh<V>(ix: &Index<V>) -> (r: Index<V>)
    requires ix.wf(),
    ensures r.wf(), r.entries() == ix.entries(), forall|q: KeyView| #![trigger r.at(q)] r.at(q) == ix.at(q),
{
    let r = ix.share();
    proof { lemma_ordered_equal(&r, ix); }
    r
}

pub fn crec_with(c: &ClassRec, r: Index<Grp>, f: Index<Grp>, fb: Index<Grp>, hp: Index<()>, hn: Index<()>) -> (n: ClassRec)
    ensures n.fy == c.fy, n.stmt@ == c.stmt@, n.uom@ == c.uom@, n.r == r, n.f == f, n.fb == fb, n.hp == hp, n.hn == hn,
{
    ClassRec { fy: c.fy, stmt: copy_bytes(c.stmt.as_slice()), uom: copy_bytes(c.uom.as_slice()), r, f, fb, hp, hn }
}

pub open spec fn ix_same<V>(a: Index<V>, b: Index<V>) -> bool {
    a.wf() && a.entries() == b.entries() && forall|q: KeyView| #![trigger a.at(q)] a.at(q) == b.at(q)
}

pub open spec fn crec_head(c: ClassRec, n: ClassRec) -> bool {
    n.fy == c.fy && n.stmt@ == c.stmt@ && n.uom@ == c.uom@
}

/// The record with the group stored in r.
pub fn crec_r_put(c: &ClassRec, g: Arc<Grp>) -> (n: ClassRec)
    requires c.r.wf() && c.f.wf() && c.fb.wf() && c.hp.wf() && c.hn.wf(),
    ensures
        crec_head(*c, n), n.r.wf(),
        forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) == if q == rkey(*g) { Some(*g) } else { c.r.at(q) },
        ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
{
    let ik = ikey_of(&g.name, g.tag.as_slice(), &g.plabel);
    let k = rkey_of(g.bits, &ik);
    let mut r = c.r.share();
    r.put(k, g);
    crec_with(c, r, sh(&c.f), sh(&c.fb), sh(&c.hp), sh(&c.hn))
}

/// The record without the entry stored under this rank key.
pub fn crec_r_del(c: &ClassRec, rk: &Key) -> (n: ClassRec)
    requires c.r.wf() && c.f.wf() && c.fb.wf() && c.hp.wf() && c.hn.wf(),
    ensures
        crec_head(*c, n), n.r.wf(),
        forall|q: KeyView| #![trigger n.r.at(q)] n.r.at(q) == if q == rk.view() { None } else { c.r.at(q) },
        ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
{
    let mut r = c.r.share();
    r.remove(rk);
    crec_with(c, r, sh(&c.f), sh(&c.fb), sh(&c.hp), sh(&c.hn))
}

/// The record with the group as its unit's frontier group (the unit has none).
pub fn crec_f_put(c: &ClassRec, g: Arc<Grp>) -> (n: ClassRec)
    requires c.r.wf() && c.f.wf() && c.fb.wf() && c.hp.wf() && c.hn.wf(),
    ensures
        crec_head(*c, n), n.f.wf(), n.fb.wf(),
        forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if q == rkey(*g) { Some(*g) } else { c.f.at(q) },
        forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if q == nkey_v(ov(g.name)) { Some(*g) } else { c.fb.at(q) },
        ix_same(n.r, c.r), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
{
    let ik = ikey_of(&g.name, g.tag.as_slice(), &g.plabel);
    let k = rkey_of(g.bits, &ik);
    let nk = nkey_of(&g.name);
    let mut f = c.f.share();
    let mut fb = c.fb.share();
    f.put(k, g.clone());
    fb.put(nk, g);
    crec_with(c, sh(&c.r), f, fb, sh(&c.hp), sh(&c.hn))
}

/// The record without the unit's frontier group.
pub fn crec_f_del(c: &ClassRec, nk: &Key) -> (n: ClassRec)
    requires c.r.wf() && c.f.wf() && c.fb.wf() && c.hp.wf() && c.hn.wf(),
    ensures
        crec_head(*c, n), n.f.wf(), n.fb.wf(),
        c.fb.at(nk.view()) is None ==> ix_same(n.f, c.f) && ix_same(n.fb, c.fb),
        c.fb.at(nk.view()) is Some ==> {
            let b = c.fb.at(nk.view())->0;
            &&& forall|q: KeyView| #![trigger n.f.at(q)] n.f.at(q) == if q == rkey(b) { None } else { c.f.at(q) }
            &&& forall|q: KeyView| #![trigger n.fb.at(q)] n.fb.at(q) == if q == nk.view() { None } else { c.fb.at(q) }
        },
        ix_same(n.r, c.r), ix_same(n.hp, c.hp), ix_same(n.hn, c.hn),
{
    let mut f = c.f.share();
    let mut fb = c.fb.share();
    match c.fb.get(nk) {
        None => {
            proof { lemma_ordered_equal(&f, &c.f); lemma_ordered_equal(&fb, &c.fb); }
        }
        Some(b) => {
            let ik = ikey_of(&b.name, b.tag.as_slice(), &b.plabel);
            let k = rkey_of(b.bits, &ik);
            f.remove(&k);
            fb.remove(nk);
        }
    }
    crec_with(c, sh(&c.r), f, fb, sh(&c.hp), sh(&c.hn))
}

pub fn crec_hp_put(c: &ClassRec, nk: Key) -> (n: ClassRec)
    requires c.r.wf() && c.f.wf() && c.fb.wf() && c.hp.wf() && c.hn.wf(),
    ensures
        crec_head(*c, n), n.hp.wf(),
        forall|q: KeyView| #![trigger n.hp.at(q)] n.hp.at(q) == if q == nk.view() { Some(()) } else { c.hp.at(q) },
        ix_same(n.r, c.r), ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hn, c.hn),
{
    let mut hp = c.hp.share();
    hp.put(nk, Arc::new(()));
    crec_with(c, sh(&c.r), sh(&c.f), sh(&c.fb), hp, sh(&c.hn))
}

pub fn crec_hn_put(c: &ClassRec, nk: Key) -> (n: ClassRec)
    requires c.r.wf() && c.f.wf() && c.fb.wf() && c.hp.wf() && c.hn.wf(),
    ensures
        crec_head(*c, n), n.hn.wf(),
        forall|q: KeyView| #![trigger n.hn.at(q)] n.hn.at(q) == if q == nk.view() { Some(()) } else { c.hn.at(q) },
        ix_same(n.r, c.r), ix_same(n.f, c.f), ix_same(n.fb, c.fb), ix_same(n.hp, c.hp),
{
    let mut hn = c.hn.share();
    hn.put(nk, Arc::new(()));
    crec_with(c, sh(&c.r), sh(&c.f), sh(&c.fb), sh(&c.hp), hn)
}

/// The record without the unit's overflow marks.
pub fn crec_h_del(c: &ClassRec, nk: &Key) -> (n: ClassRec)
    requires c.r.wf() && c.f.wf() && c.fb.wf() && c.hp.wf() && c.hn.wf(),
    ensures
        crec_head(*c, n), n.hp.wf(), n.hn.wf(),
        forall|q: KeyView| #![trigger n.hp.at(q)] n.hp.at(q) == if q == nk.view() { None } else { c.hp.at(q) },
        forall|q: KeyView| #![trigger n.hn.at(q)] n.hn.at(q) == if q == nk.view() { None } else { c.hn.at(q) },
        ix_same(n.r, c.r), ix_same(n.f, c.f), ix_same(n.fb, c.fb),
{
    let mut hp = c.hp.share();
    let mut hn = c.hn.share();
    hp.remove(nk);
    hn.remove(nk);
    crec_with(c, sh(&c.r), sh(&c.f), sh(&c.fb), hp, hn)
}

} // verus!
