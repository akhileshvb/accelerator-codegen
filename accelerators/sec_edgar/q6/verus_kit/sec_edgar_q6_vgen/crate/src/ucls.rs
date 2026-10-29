// The classes each unit has groups in: an update touches only the classes of the units it recomputes.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::grp::*;
use crate::inv::*;
use crate::crops::*;
use crate::units::*;
use crate::plan::*;

verus! {

/// A unit's classes, keyed by the class key.
pub struct ClsSet {
    pub s: Index<ClassRef>,
}

/// The unit with key q has groups in the class ok.
pub open spec fn ucs_has(uc: Index<ClsSet>, q: KeyView, ok: KeyView) -> bool {
    match uc.at(q) { Some(c) => c.s.at(ok) is Some, None => false }
}

pub open spec fn uc_str(uc: Index<ClsSet>) -> bool {
    uc.wf() && forall|q: KeyView| #![trigger uc.at(q)] uc.at(q) is Some ==> cset_ok(uc.at(q)->0.s)
}

/// Every group of db is in a class its unit lists.
pub open spec fn uc_ok(uc: Index<ClsSet>, db: Db) -> bool {
    &&& uc_str(uc)
    &&& forall|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 ==> ucs_has(uc, ukey(k.fy, k.name), okey(k.fy, k.stmt, k.uom))
}

/// The unit with key (c.fy, nk.1) has a stored entry in the record.
pub open spec fn unit_in(c: ClassRec, nk: KeyView) -> bool {
    ||| exists|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is Some && nkey_v(ov(c.r.at(q)->0.name)) == nk
    ||| exists|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some && nkey_v(ov(c.f.at(q)->0.name)) == nk
    ||| c.hp.at(nk) is Some
    ||| c.hn.at(nk) is Some
}

/// Every unit with an entry in a class lists the class.
pub open spec fn unit_cls_ok(cls: Index<ClassRec>, uc: Index<ClsSet>) -> bool {
    forall|ok: KeyView, nk: KeyView| #![trigger cls.at(ok), unit_in(cls.at(ok)->0, nk)]
        cls.at(ok) is Some && unit_in(cls.at(ok)->0, nk) ==> ucs_has(uc, (cls.at(ok)->0.fy as int, nk.1), ok)
}

pub proof fn lemma_unit_cls(db: Db, cls: Index<ClassRec>, uc: Index<ClsSet>)
    requires cls_ok(db, cls), uc_ok(uc, db),
    ensures unit_cls_ok(cls, uc),
{
    assert forall|ok: KeyView, nk: KeyView| cls.at(ok) is Some && unit_in(cls.at(ok)->0, nk) implies ucs_has(uc, (cls.at(ok)->0.fy as int, nk.1), ok) by {
        let c = cls.at(ok)->0;
        assert(ok == cls_key(c) && crec_sem(db, c));
        if exists|q: KeyView| c.r.at(q) is Some && nkey_v(ov(c.r.at(q)->0.name)) == nk {
            let q = choose|q: KeyView| c.r.at(q) is Some && nkey_v(ov(c.r.at(q)->0.name)) == nk;
            let k = cls_gk(c, c.r.at(q)->0);
            assert(exact_g(db, c, c.r.at(q)->0));
            assert(cntk(db, k) > 0);
            assert(okey(k.fy, k.stmt, k.uom) == ok);
            assert(ukey(k.fy, k.name) == (c.fy as int, nk.1));
        } else if exists|q: KeyView| c.f.at(q) is Some && nkey_v(ov(c.f.at(q)->0.name)) == nk {
            let q = choose|q: KeyView| c.f.at(q) is Some && nkey_v(ov(c.f.at(q)->0.name)) == nk;
            let k = cls_gk(c, c.f.at(q)->0);
            assert(exact_g(db, c, c.f.at(q)->0));
            assert(cntk(db, k) > 0);
            assert(okey(k.fy, k.stmt, k.uom) == ok);
            assert(ukey(k.fy, k.name) == (c.fy as int, nk.1));
        } else if c.hp.at(nk) is Some {
            assert(has_cand(db, c, nk, true));
            let k = choose|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 && in_class(c, k) && nkey_v(k.name) == nk && sumk(db, k) > max_val();
            assert(okey(k.fy, k.stmt, k.uom) == ok);
            assert(ukey(k.fy, k.name) == (c.fy as int, nk.1));
        } else {
            assert(c.hn.at(nk) is Some);
            assert(has_cand(db, c, nk, false));
            let k = choose|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 && in_class(c, k) && nkey_v(k.name) == nk && sumk(db, k) < -max_val();
            assert(okey(k.fy, k.stmt, k.uom) == ok);
            assert(ukey(k.fy, k.name) == (c.fy as int, nk.1));
        }
    }
}

/// The classes of the units of a set.
pub fn uc_classes(uc: &Index<ClsSet>, s: &Index<UnitRef>) -> (ct: Index<ClassRef>)
    requires uc_str(*uc), uset_ok(*s),
    ensures
        cset_ok(ct),
        forall|q: KeyView, ok: KeyView| #![trigger s.at(q), ucs_has(*uc, q, ok)] s.at(q) is Some && ucs_has(*uc, q, ok) ==> ct.at(ok) is Some,
{
    let ents = s.rows();
    let ghost es = s.entries();
    proof { s.model(); }
    let mut ct: Index<ClassRef> = Index::new();
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(), cset_ok(ct), uc_str(*uc), s.wf(),
            ents@.map_values(|row: (Key, Arc<UnitRef>)| (row.0.view(), *row.1)) == es, es == s.entries(),
            forall|pos: int| #![trigger es[pos]] 0 <= pos < es.len() ==> s.at(es[pos].0) == Some(es[pos].1),
            forall|j: int, ok: KeyView| #![trigger ucs_has(*uc, es[j].0, ok)] 0 <= j < i && ucs_has(*uc, es[j].0, ok) ==> ct.at(ok) is Some,
        decreases ents.len() - i,
    {
        let ur = &ents[i].1;
        let uk = &ents[i].0;
        proof { assert(es[i as int].0 == uk.view()); }
        match uc.get(uk) {
            None => {
                proof { assert(uc.at(uk.view()) is None); }
            }
            Some(cs) => {
                let cents = cs.s.rows();
                let ghost ces = arc_val(&cs).s.entries();
                proof {
                    assert(uc.at(uk.view()) == Some(arc_val(&cs)));
                    assert(cset_ok(cs.s));
                    cs.s.model();
                }
                let ghost ct_before = ct;
                let mut j = 0;
                while j < cents.len()
                    invariant
                        j <= cents.len(), cset_ok(ct), cs.s.wf(), cset_ok(cs.s),
                        cents@.map_values(|row: (Key, Arc<ClassRef>)| (row.0.view(), *row.1)) == ces, ces == cs.s.entries(),
                        forall|pos: int| #![trigger ces[pos]] 0 <= pos < ces.len() ==> cs.s.at(ces[pos].0) == Some(ces[pos].1),
                        forall|ok: KeyView| #![trigger ct_before.at(ok)] ct_before.at(ok) is Some ==> ct.at(ok) is Some,
                        forall|p: int| #![trigger ces[p]] 0 <= p < j ==> ct.at(ces[p].0) is Some,
                    decreases cents.len() - j,
                {
                    let cr = &cents[j].1;
                    let ck = &cents[j].0;
                    proof { assert(ces[j as int].0 == ck.view()); assert(ces[j as int] == (ck.view(), arc_val(cr))); assert(cs.s.at(ck.view()) == Some(arc_val(cr))); }
                    let ghost ct_pre = ct;
                    ct.put(ck.share(), Arc::new(ClassRef { fy: cr.fy, stmt: copy_bytes(cr.stmt.as_slice()), uom: copy_bytes(cr.uom.as_slice()) }));
                    j += 1;
                    proof {
                        assert forall|q: KeyView| ct.at(q) is Some implies q == okey(ct.at(q)->0.fy as int, ct.at(q)->0.stmt@, ct.at(q)->0.uom@) by {
                            if q != ck.view() { assert(ct_pre.at(q) is Some); }
                        }
                    }
                }
                proof {
                    assert forall|ok: KeyView| ucs_has(*uc, es[i as int].0, ok) implies ct.at(ok) is Some by {
                        assert(cs.s.at(ok) is Some);
                        let p = choose|p: int| #![auto] 0 <= p < ces.len() && ces[p].0 == ok;
                    }
                }
            }
        }
        i += 1;
    }
    proof {
        assert forall|q: KeyView, ok: KeyView| s.at(q) is Some && ucs_has(*uc, q, ok) implies ct.at(ok) is Some by {
            let pos = choose|pos: int| #![auto] 0 <= pos < es.len() && es[pos].0 == q;
        }
    }
    ct
}

/// The class sets after a rescan: the units of u list the classes of the contributions they have, the others are unchanged.
#[verifier::rlimit(200)]
#[verifier::spinoff_prover]
pub fn uc_update(uc: &Index<ClsSet>, u: &Index<UnitRef>, acc: &Index<AccEnt>) -> (r: Index<ClsSet>)
    requires uc_str(*uc), uset_ok(*u), acc_ok(*acc),
    ensures
        uc_str(r),
        forall|q: KeyView| #![trigger r.at(q)] u.at(q) is None ==> r.at(q) == uc.at(q),
        forall|kv: KeyView| #![trigger acc.at(kv)] acc.at(kv) is Some && u_has(*u, acc.at(kv)->0.fy as int, ov(acc.at(kv)->0.name)) ==>
            ucs_has(r, ukey(acc.at(kv)->0.fy as int, ov(acc.at(kv)->0.name)), okey(acc.at(kv)->0.fy as int, acc.at(kv)->0.stmt@, acc.at(kv)->0.uom@)),
{
    let mut r = sh(uc);
    let uents = u.rows();
    let ghost ues = u.entries();
    proof { u.model(); }
    let mut i = 0;
    while i < uents.len()
        invariant
            i <= uents.len(), uc_str(r), uset_ok(*u), u.wf(), uc_str(*uc), verus_prooflib::ordered::ordered_rows(ues),
            uents@.map_values(|row: (Key, Arc<UnitRef>)| (row.0.view(), *row.1)) == ues, ues == u.entries(),
            forall|q: KeyView| #![trigger r.at(q)] (forall|j: int| 0 <= j < i ==> (#[trigger] ues[j]).0 != q) ==> r.at(q) == uc.at(q),
            forall|j: int| #![trigger ues[j]] 0 <= j < i ==> r.at(ues[j].0) is Some && (forall|ok: KeyView| #![trigger r.at(ues[j].0)->0.s.at(ok)] r.at(ues[j].0)->0.s.at(ok) is None),
        decreases uents.len() - i,
    {
        let uk = &uents[i].0;
        proof { assert(ues[i as int].0 == uk.view()); }
        let ghost r0 = r;
        r.put(uk.share(), Arc::new(ClsSet { s: Index::new() }));
        i += 1;
        proof {
            assert forall|q: KeyView| r.at(q) is Some && (r.at(q)->0.s.wf() == r.at(q)->0.s.wf()) implies cset_ok(r.at(q)->0.s) by {
                if q != uk.view() { assert(r0.at(q) is Some); }
            }
            assert forall|q: KeyView| (forall|j: int| 0 <= j < i ==> (#[trigger] ues[j]).0 != q) implies r.at(q) == uc.at(q) by {
                assert(forall|j: int| 0 <= j < i - 1 ==> ues[j].0 != q);
                assert(q != uk.view());
            }
            assert forall|j: int| 0 <= j < i implies r.at(ues[j].0) is Some && (forall|ok: KeyView| #![trigger r.at(ues[j].0)->0.s.at(ok)] r.at(ues[j].0)->0.s.at(ok) is None) by {
                if j < i - 1 {
                    verus_prooflib::ordered::lemma_ordered_unique(ues);
                    assert(ues[j].0 != uk.view());
                }
            }
        }
    }
    let ghost r1 = r;
    proof {
        assert forall|q: KeyView| u.at(q) is Some implies r1.at(q) is Some && (forall|ok: KeyView| #![trigger r1.at(q)->0.s.at(ok)] r1.at(q)->0.s.at(ok) is None) by {
            let pos = choose|pos: int| #![auto] 0 <= pos < ues.len() && ues[pos].0 == q;
        }
        assert forall|q: KeyView| u.at(q) is None implies r1.at(q) == uc.at(q) by {
            assert forall|j: int| 0 <= j < ues.len() implies (#[trigger] ues[j]).0 != q by { assert(u.at(ues[j].0) is Some); }
        }
    }
    let aents = acc.rows();
    let ghost aes = acc.entries();
    proof { acc.model(); }
    let mut a = 0;
    while a < aents.len()
        invariant
            a <= aents.len(), uc_str(r), uset_ok(*u), u.wf(), acc.wf(), acc_ok(*acc),
            aents@.map_values(|row: (Key, Arc<AccEnt>)| (row.0.view(), *row.1)) == aes, aes == acc.entries(),
            forall|pos: int| #![trigger aes[pos]] 0 <= pos < aes.len() ==> acc.at(aes[pos].0) == Some(aes[pos].1),
            forall|q: KeyView| #![trigger u.at(q)] u.at(q) is None ==> r.at(q) == uc.at(q),
            forall|q: KeyView| #![trigger u.at(q)] u.at(q) is Some ==> r.at(q) is Some,
            forall|j: int| #![trigger aes[j]] 0 <= j < a && u_has(*u, aes[j].1.fy as int, ov(aes[j].1.name)) ==>
                ucs_has(r, ukey(aes[j].1.fy as int, ov(aes[j].1.name)), okey(aes[j].1.fy as int, aes[j].1.stmt@, aes[j].1.uom@)),
        decreases aents.len() - a,
    {
        let e = &aents[a].1;
        proof { assert(aes[a as int] == (aents@[a as int].0.view(), arc_val(e))); }
        let uk = ukey_of(e.fy, &e.name);
        if u.get(&uk).is_some() {
            let cur = match r.get(&uk) { Some(c) => c, None => { proof { assert(false); } return r; } };
            let ghost r0 = r;
            let mut ns = sh(&cur.s);
            ns.put(okey_of(e.fy, e.stmt.as_slice(), e.uom.as_slice()), Arc::new(ClassRef { fy: e.fy, stmt: copy_bytes(e.stmt.as_slice()), uom: copy_bytes(e.uom.as_slice()) }));
            r.put(uk.share(), Arc::new(ClsSet { s: ns }));
            a += 1;
            proof {
                let cs0 = arc_val(&cur);
                assert(r0.at(uk.view()) == Some(cs0));
                assert(cset_ok(cs0.s));
                assert forall|q: KeyView| r.at(q) is Some implies cset_ok(r.at(q)->0.s) by {
                    if q != uk.view() { assert(r0.at(q) is Some); }
                    else {
                        let nk = okey(e.fy as int, e.stmt@, e.uom@);
                        assert forall|oq: KeyView| ns.at(oq) is Some implies oq == okey(ns.at(oq)->0.fy as int, ns.at(oq)->0.stmt@, ns.at(oq)->0.uom@) by {
                            if oq != nk { assert(cs0.s.at(oq) is Some); }
                        }
                    }
                }
                assert forall|q: KeyView, ok: KeyView| ucs_has(r0, q, ok) implies ucs_has(r, q, ok) by {
                    if q == uk.view() { assert(cs0.s.at(ok) is Some); }
                }
                assert(ucs_has(r, ukey(e.fy as int, ov(e.name)), okey(e.fy as int, e.stmt@, e.uom@)));
                assert forall|q: KeyView| u.at(q) is None implies r.at(q) == uc.at(q) by { assert(q != uk.view()); }
                assert forall|q: KeyView| u.at(q) is Some implies r.at(q) is Some by { if q != uk.view() { assert(r0.at(q) is Some); } }
                assert forall|j: int| 0 <= j < a && u_has(*u, aes[j].1.fy as int, ov(aes[j].1.name)) implies
                    ucs_has(r, ukey(aes[j].1.fy as int, ov(aes[j].1.name)), okey(aes[j].1.fy as int, aes[j].1.stmt@, aes[j].1.uom@)) by {
                    if j < a - 1 { assert(ucs_has(r0, ukey(aes[j].1.fy as int, ov(aes[j].1.name)), okey(aes[j].1.fy as int, aes[j].1.stmt@, aes[j].1.uom@))); }
                }
            }
        } else {
            a += 1;
            proof { assert(aes[(a - 1) as int].1 == arc_val(e)); assert(!u_has(*u, arc_val(e).fy as int, ov(arc_val(e).name))); }
        }
    }
    proof {
        assert forall|kv: KeyView| acc.at(kv) is Some && u_has(*u, acc.at(kv)->0.fy as int, ov(acc.at(kv)->0.name)) implies
            ucs_has(r, ukey(acc.at(kv)->0.fy as int, ov(acc.at(kv)->0.name)), okey(acc.at(kv)->0.fy as int, acc.at(kv)->0.stmt@, acc.at(kv)->0.uom@)) by {
            let pos = choose|pos: int| #![auto] 0 <= pos < aes.len() && aes[pos].0 == kv;
        }
    }
    r
}

} // verus!
