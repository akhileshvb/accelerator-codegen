// The units to recompute and the filings to rescan, as a function of the state and the transaction.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::bytes::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::esc::*;
use crate::accmap::*;
use crate::grp::*;
use crate::inv::*;
use crate::tbl::*;
use crate::units::*;
use crate::plan::*;
use crate::lists::*;
use crate::canon::*;
use crate::core::*;
use crate::evict::*;
use crate::bridge::*;

verus! {

pub open spec fn u_pred_s(fm: Index<Vec<UnitRef>>, ka: Seq<Seq<u8>>, ds: Seq<SubDelta>, fy: int, name: Option<Seq<u8>>) -> bool {
    (exists|i: int| 0 <= i < ka.len() && fm_has(fm, #[trigger] ka[i], fy, name)) || sd_has(ds, fy, name)
}

/// The units a transaction touches, as a set.
pub open spec fn u_spec(fm: Index<Vec<UnitRef>>, ka: Seq<Seq<u8>>, ds: Seq<SubDelta>) -> Index<UnitRef> {
    choose|u: Index<UnitRef>| uset_ok(u) && forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(u, fy, name)] u_has(u, fy, name) <==> u_pred_s(fm, ka, ds, fy, name)
}

pub open spec fn fy_in_u(u: Index<UnitRef>, fy: int) -> bool {
    exists|k: KeyView| #![trigger u.at(k)] u.at(k) is Some && k.0 == fy
}

/// Units whose frontier group is among the first (outside u) of a touched class that keeps few groups outside u.
pub open spec fn vw_pred(cls: Index<ClassRec>, u: Index<UnitRef>, fy: int, name: Option<Seq<u8>>) -> bool {
    exists|ok: KeyView, k: KeyView| #![trigger cls.at(ok), vsel(cls.at(ok)->0.f.entries(), fy, u).contains(k)]
        cls.at(ok) is Some && cls.at(ok)->0.fy as int == fy && fy_in_u(u, fy)
        && notu_keys(cls.at(ok)->0.r.entries(), fy, u).len() < k_limit()
        && vsel(cls.at(ok)->0.f.entries(), fy, u).contains(k) && ov(cls.at(ok)->0.f.at(k)->0.name) == name
}

pub open spec fn w_pred(cls: Index<ClassRec>, fm: Index<Vec<UnitRef>>, ka: Seq<Seq<u8>>, ds: Seq<SubDelta>, fy: int, name: Option<Seq<u8>>) -> bool {
    u_pred_s(fm, ka, ds, fy, name) || vw_pred(cls, u_spec(fm, ka, ds), fy, name)
}

pub open spec fn wanted_pred(cls: Index<ClassRec>, fm: Index<Vec<UnitRef>>, um: Index<UnitRec>, ka: Seq<Seq<u8>>, ds: Seq<SubDelta>) -> spec_fn(Seq<u8>) -> bool {
    |a: Seq<u8>| ka.contains(a) || exists|fy: int, name: Option<Seq<u8>>| #![trigger um_has(um, fy, name, a)] w_pred(cls, fm, ka, ds, fy, name) && um_has(um, fy, name, a)
}

pub open spec fn wanted_def(cls: Index<ClassRec>, fm: Index<Vec<UnitRef>>, um: Index<UnitRec>, dn: Seq<NumDelta>, dp: Seq<PreDelta>, ds: Seq<SubDelta>) -> Seq<Seq<u8>> {
    choose|ks: Seq<Seq<u8>>| canon_keys(ks, wanted_pred(cls, fm, um, all_keys(dn, dp, ds), ds))
}

pub struct Plan {
    pub u: Index<UnitRef>,
    pub ct: Index<ClassRef>,
    pub w: Index<UnitRef>,
    pub cw: Index<ClassRef>,
    pub keys: Vec<Vec<u8>>,
}

/// notu_keys depends on a unit set only through which units it holds.
pub proof fn lemma_notu_congr(es: Seq<(KeyView, Grp)>, fy: int, u1: Index<UnitRef>, u2: Index<UnitRef>)
    requires forall|f: int, n: Option<Seq<u8>>| u_has(u1, f, n) == u_has(u2, f, n),
    ensures notu_keys(es, fy, u1) == notu_keys(es, fy, u2),
    decreases es.len(),
{
    if es.len() > 0 {
        lemma_notu_congr(es.drop_last(), fy, u1, u2);
        assert(u_has(u1, fy, ov(es.last().1.name)) == u_has(u2, fy, ov(es.last().1.name)));
    }
}

pub proof fn lemma_vsel_congr(es: Seq<(KeyView, Grp)>, fy: int, u1: Index<UnitRef>, u2: Index<UnitRef>)
    requires forall|f: int, n: Option<Seq<u8>>| u_has(u1, f, n) == u_has(u2, f, n),
    ensures vsel(es, fy, u1) == vsel(es, fy, u2),
{
    lemma_notu_congr(es, fy, u1, u2);
}

/// The moved groups' units are those the predicate names.
pub proof fn lemma_mv_vw(cls: Index<ClassRec>, ct: Index<ClassRef>, u: Index<UnitRef>, mvs: Seq<Mv>, fy: int, name: Option<Seq<u8>>)
    requires
        tbl_str(cls), uset_ok(u), mv_ok(cls, ct, u, mvs),
        forall|ok: KeyView, k: KeyView| #![trigger cls.at(ok), u.at(k)] cls.at(ok) is Some && u.at(k) is Some && k.0 == cls.at(ok)->0.fy as int ==> ct.at(ok) is Some,
        forall|ok: KeyView| #![trigger ct.at(ok)] ct.at(ok) is Some ==> cls.at(ok) is Some && exists|k: KeyView| #![trigger u.at(k)] u.at(k) is Some && k.0 == cls.at(ok)->0.fy as int,
    ensures mv_has(mvs, fy, name) <==> vw_pred(cls, u, fy, name),
{
    if mv_has(mvs, fy, name) {
        let i = choose|i: int| 0 <= i < mvs.len() && mvs[i].fy as int == fy && ov(mvs[i].g.name) == name;
        let ok = okey(mvs[i].fy as int, mvs[i].stmt@, mvs[i].uom@);
        let c = cls.at(ok)->0;
        assert(cls_key(c) == ok);
        lemma_okey_inj(c.fy as int, c.stmt@, c.uom@, mvs[i].fy as int, mvs[i].stmt@, mvs[i].uom@);
        assert(c.fy as int == fy);
        assert(fy_in_u(u, fy));
        let k = rkey(arc_val(&mvs[i].g));
        assert(c.f.at(k) == Some(*mvs[i].g));
        assert(vsel(c.f.entries(), fy, u).contains(k));
    }
    if vw_pred(cls, u, fy, name) {
        let ok = choose|ok: KeyView| #![trigger cls.at(ok)] cls.at(ok) is Some && cls.at(ok)->0.fy as int == fy && fy_in_u(u, fy)
            && notu_keys(cls.at(ok)->0.r.entries(), fy, u).len() < k_limit() && exists|k: KeyView| vsel(cls.at(ok)->0.f.entries(), fy, u).contains(k) && ov(cls.at(ok)->0.f.at(k)->0.name) == name;
        let c = cls.at(ok)->0;
        let k = choose|k: KeyView| vsel(c.f.entries(), fy, u).contains(k) && ov(c.f.at(k)->0.name) == name;
        let kk = choose|kk: KeyView| #![trigger u.at(kk)] u.at(kk) is Some && kk.0 == fy;
        assert(ct.at(ok) is Some);
        let i = choose|i: int| 0 <= i < mvs.len() && okey((#[trigger] mvs[i]).fy as int, mvs[i].stmt@, mvs[i].uom@) == ok && rkey(*mvs[i].g) == k;
        assert(cls.at(ok)->0.f.at(rkey(*mvs[i].g)) == Some(*mvs[i].g));
        lemma_okey_inj(mvs[i].fy as int, mvs[i].stmt@, mvs[i].uom@, c.fy as int, c.stmt@, c.uom@);
        assert(mvs[i].fy as int == fy);
        assert(ov(mvs[i].g.name) == name);
    }
}

/// The filings of the units in w, and the filings with delta rows, as a sorted list.
#[verifier::rlimit(300)]
#[verifier::spinoff_prover]
pub fn filing_keys(um: &Index<UnitRec>, w: &Index<UnitRef>, keys0: &Vec<Vec<u8>>) -> (r: Vec<Vec<u8>>)
    requires um.wf(), uset_ok(*w),
    ensures
        strictly_sorted(seq_view_bytes(r@)),
        forall|a: Seq<u8>| #![trigger seq_view_bytes(r@).contains(a)] seq_view_bytes(r@).contains(a) <==>
            (seq_view_bytes(keys0@).contains(a) || exists|fy: int, name: Option<Seq<u8>>| #![trigger um_has(*um, fy, name, a)] u_has(*w, fy, name) && um_has(*um, fy, name, a)),
{
    let mut fl: Index<()> = Index::new();
    let mut i = 0;
    while i < keys0.len()
        invariant
            i <= keys0.len(), fl.wf(), forall|q: KeyView| #![trigger fl.at(q)] fl.at(q) is Some ==> q.0 == 0,
            forall|a: Seq<u8>| #![trigger fl.at(akey(a))] fl.at(akey(a)) is Some <==> exists|j: int| 0 <= j < i && (#[trigger] keys0@[j])@ == a,
        decreases keys0.len() - i,
    {
        let ghost fl0 = fl;
        fl.put(akey_of(keys0[i].as_slice()), Arc::new(()));
        i += 1;
        proof {
            assert forall|a: Seq<u8>| fl.at(akey(a)) is Some <==> exists|j: int| 0 <= j < i && (#[trigger] keys0@[j])@ == a by {
                if a == keys0@[(i - 1) as int]@ { assert(keys0@[(i - 1) as int]@ == a); }
                else { assert(akey(a) != akey(keys0@[(i - 1) as int]@)); assert(fl.at(akey(a)) == fl0.at(akey(a))); }
                if exists|j: int| 0 <= j < i && keys0@[j]@ == a { let j = choose|j: int| 0 <= j < i && keys0@[j]@ == a; if j < i - 1 { assert(exists|j2: int| 0 <= j2 < i - 1 && keys0@[j2]@ == a); } }
            }
        }
    }
    let wents = w.rows();
    let ghost wes = w.entries();
    proof { w.model(); }
    let mut iw = 0;
    while iw < wents.len()
        invariant
            iw <= wents.len(), fl.wf(), um.wf(), uset_ok(*w), w.wf(), forall|q: KeyView| #![trigger fl.at(q)] fl.at(q) is Some ==> q.0 == 0,
            wents@.map_values(|row: (Key, Arc<UnitRef>)| (row.0.view(), *row.1)) == wes, wes == w.entries(),
            forall|pos: int| #![trigger wes[pos]] 0 <= pos < wes.len() ==> w.at(wes[pos].0) == Some(wes[pos].1),
            forall|a: Seq<u8>| #![trigger fl.at(akey(a))] fl.at(akey(a)) is Some <==>
                ((exists|j: int| 0 <= j < keys0.len() && (#[trigger] keys0@[j])@ == a) || exists|i2: int| 0 <= i2 < iw && #[trigger] um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a)),
        decreases wents.len() - iw,
    {
        let ur = &wents[iw].1;
        proof { assert(wes[iw as int] == (wents@[iw as int].0.view(), arc_val(ur))); assert(w.at(wes[iw as int].0) == Some(arc_val(ur))); }
        match um.get(&ukey_of(ur.fy, &ur.name)) {
            None => {
                proof {
                    assert forall|a: Seq<u8>| fl.at(akey(a)) is Some <==>
                        ((exists|j: int| 0 <= j < keys0.len() && keys0@[j]@ == a) || exists|i2: int| 0 <= i2 < iw + 1 && #[trigger] um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a)) by {
                        if exists|i2: int| 0 <= i2 < iw + 1 && um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a) {
                            let i2 = choose|i2: int| 0 <= i2 < iw + 1 && um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a);
                            if i2 == iw as int { assert(um.at(ukey(arc_val(ur).fy as int, ov(arc_val(ur).name))) is None); }
                        }
                    }
                }
            }
            Some(rec) => {
                let mut jf = 0;
                proof { assert(um.at(ukey(arc_val(ur).fy as int, ov(arc_val(ur).name))) == Some(arc_val(&rec))); }
                while jf < rec.filings.len()
                    invariant
                        jf <= rec.filings.len(), fl.wf(), iw < wents.len(), forall|q: KeyView| #![trigger fl.at(q)] fl.at(q) is Some ==> q.0 == 0,
                        um.at(ukey(wes[iw as int].1.fy as int, ov(wes[iw as int].1.name))) == Some(arc_val(&rec)),
                        forall|a: Seq<u8>| #![trigger fl.at(akey(a))] fl.at(akey(a)) is Some <==>
                            ((exists|j: int| 0 <= j < keys0.len() && (#[trigger] keys0@[j])@ == a) || (exists|i2: int| 0 <= i2 < iw && #[trigger] um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a))
                                || exists|j2: int| 0 <= j2 < jf && (#[trigger] rec.filings@[j2])@ == a),
                    decreases rec.filings.len() - jf,
                {
                    let ghost fl0 = fl;
                    fl.put(akey_of(rec.filings[jf].as_slice()), Arc::new(()));
                    jf += 1;
                    proof {
                        assert forall|a: Seq<u8>| fl.at(akey(a)) is Some <==>
                            ((exists|j: int| 0 <= j < keys0.len() && keys0@[j]@ == a) || (exists|i2: int| 0 <= i2 < iw && um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a))
                                || exists|j2: int| 0 <= j2 < jf && (#[trigger] rec.filings@[j2])@ == a) by {
                            if a == rec.filings@[(jf - 1) as int]@ {} else { assert(akey(a) != akey(rec.filings@[(jf - 1) as int]@)); assert(fl.at(akey(a)) == fl0.at(akey(a))); }
                            if exists|j2: int| 0 <= j2 < jf && rec.filings@[j2]@ == a { let j2 = choose|j2: int| 0 <= j2 < jf && rec.filings@[j2]@ == a; if j2 < jf - 1 { assert(exists|j3: int| 0 <= j3 < jf - 1 && rec.filings@[j3]@ == a); } }
                        }
                    }
                }
                proof {
                    assert forall|a: Seq<u8>| fl.at(akey(a)) is Some <==>
                        ((exists|j: int| 0 <= j < keys0.len() && keys0@[j]@ == a) || exists|i2: int| 0 <= i2 < iw + 1 && #[trigger] um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a)) by {
                        if exists|j2: int| 0 <= j2 < rec.filings.len() && rec.filings@[j2]@ == a { assert(bytes_has(rec.filings@, a)); assert(um_has(*um, wes[iw as int].1.fy as int, ov(wes[iw as int].1.name), a)); }
                        if exists|i2: int| 0 <= i2 < iw + 1 && um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a) {
                            let i2 = choose|i2: int| 0 <= i2 < iw + 1 && um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a);
                            if i2 == iw as int { assert(bytes_has(rec.filings@, a)); let j2 = choose|j2: int| 0 <= j2 < rec.filings.len() && rec.filings@[j2]@ == a; }
                        }
                    }
                }
            }
        }
        iw += 1;
    }
    let rows = fl.rows();
    let ghost fes = fl.entries();
    proof { fl.model(); }
    let mut out: Vec<Vec<u8>> = Vec::new();
    let mut t = 0;
    while t < rows.len()
        invariant
            t <= rows.len(), fl.wf(), forall|q: KeyView| #![trigger fl.at(q)] fl.at(q) is Some ==> q.0 == 0,
            rows@.map_values(|row: (Key, Arc<()>)| (row.0.view(), *row.1)) == fes, fes == fl.entries(),
            out.len() == t,
            forall|j: int| 0 <= j < t ==> (#[trigger] out@[j])@ == fes[j].0.1,
        decreases rows.len() - t,
    {
        proof { assert(fes[t as int].0 == rows@[t as int].0.view()); }
        out.push(copy_bytes(rows[t].0.text.as_slice()));
        t += 1;
    }
    proof {
        let ov_ = seq_view_bytes(out@);
        assert forall|i: int, j: int| 0 <= i < j < ov_.len() implies bytes_lt(#[trigger] ov_[i], #[trigger] ov_[j]) by {
            assert(key_lt(fes[i].0, fes[j].0));
            assert(fes[i].0.0 == 0 && fes[j].0.0 == 0);
        }
        assert forall|a: Seq<u8>| ov_.contains(a) <==> fl.at(akey(a)) is Some by {
            if ov_.contains(a) { let q = choose|q: int| 0 <= q < ov_.len() && ov_[q] == a; assert(fl.at(fes[q].0) == Some(fes[q].1)); assert(fes[q].0 == akey(a)); }
            if fl.at(akey(a)) is Some { let pos = choose|pos: int| #![auto] 0 <= pos < fes.len() && fes[pos].0 == akey(a); assert(ov_[pos] == a); }
        }
        assert forall|a: Seq<u8>| fl.at(akey(a)) is Some <==> (seq_view_bytes(keys0@).contains(a) || exists|fy: int, name: Option<Seq<u8>>| #![trigger um_has(*um, fy, name, a)] u_has(*w, fy, name) && um_has(*um, fy, name, a)) by {
            if exists|j: int| 0 <= j < keys0.len() && keys0@[j]@ == a { assert(seq_view_bytes(keys0@)[( choose|j: int| 0 <= j < keys0.len() && keys0@[j]@ == a)] == a); }
            if seq_view_bytes(keys0@).contains(a) { let j = choose|j: int| 0 <= j < keys0.len() && seq_view_bytes(keys0@)[j] == a; assert(keys0@[j]@ == a); }
            if exists|i2: int| 0 <= i2 < wes.len() && um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a) {
                let i2 = choose|i2: int| 0 <= i2 < wes.len() && um_has(*um, wes[i2].1.fy as int, ov(wes[i2].1.name), a);
                assert(w.at(wes[i2].0) == Some(wes[i2].1));
                assert(wes[i2].0 == ukey(wes[i2].1.fy as int, ov(wes[i2].1.name)));
                assert(u_has(*w, wes[i2].1.fy as int, ov(wes[i2].1.name)));
            }
            if exists|fy: int, name: Option<Seq<u8>>| u_has(*w, fy, name) && um_has(*um, fy, name, a) {
                let (fy, name) = choose|fy: int, name: Option<Seq<u8>>| #![trigger um_has(*um, fy, name, a)] u_has(*w, fy, name) && um_has(*um, fy, name, a);
                let pos = choose|pos: int| #![auto] 0 <= pos < wes.len() && wes[pos].0 == ukey(fy, name);
                assert(w.at(wes[pos].0) == Some(wes[pos].1));
                assert(wes[pos].0 == ukey(wes[pos].1.fy as int, ov(wes[pos].1.name)));
                lemma_ukey_inj(fy, name, wes[pos].1.fy as int, ov(wes[pos].1.name));
                assert(um_has(*um, wes[pos].1.fy as int, ov(wes[pos].1.name), a));
            }
        }
    }
    out
}

/// The plan of a transaction.
#[verifier::rlimit(300)]
#[verifier::spinoff_prover]
pub fn plan_full(cls: &Index<ClassRec>, fm: &Index<Vec<UnitRef>>, um: &Index<UnitRec>, dn: &Vec<NumDelta>, dp: &Vec<PreDelta>, ds: &Vec<SubDelta>) -> (p: Plan)
    requires tbl_str(*cls), fm.wf(), um.wf(),
    ensures
        plan_ok(*cls, p.u, p.ct, p.w, p.cw),
        ({ let ka = all_keys(dn@, dp@, ds@); 
           &&& forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(p.u, fy, name)] u_has(p.u, fy, name) <==> u_pred_s(*fm, ka, ds@, fy, name)
           &&& forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(p.w, fy, name)] u_has(p.w, fy, name) <==> w_pred(*cls, *fm, ka, ds@, fy, name)
           &&& canon_keys(seq_view_bytes(p.keys@), wanted_pred(*cls, *fm, *um, ka, ds@)) }),
{
    let keys0 = collect_keys(dn, dp, ds);
    let ghost ka = all_keys(dn@, dp@, ds@);
    let u = plan_u(fm, &keys0, ds);
    proof {
        assert forall|fy: int, name: Option<Seq<u8>>| u_has(u, fy, name) <==> u_pred_s(*fm, ka, ds@, fy, name) by {
            assert(u_has(u, fy, name) == u_pred(*fm, keys0@, ds@, fy, name));
            if u_pred(*fm, keys0@, ds@, fy, name) {
                if exists|i: int| 0 <= i < keys0.len() && fm_has(*fm, keys0@[i]@, fy, name) {
                    let i = choose|i: int| 0 <= i < keys0.len() && fm_has(*fm, keys0@[i]@, fy, name);
                    assert(ka[i] == keys0@[i]@);
                }
            }
            if u_pred_s(*fm, ka, ds@, fy, name) {
                if exists|i: int| 0 <= i < ka.len() && fm_has(*fm, ka[i], fy, name) {
                    let i = choose|i: int| 0 <= i < ka.len() && fm_has(*fm, ka[i], fy, name);
                    assert(keys0@[i]@ == ka[i]);
                }
            }
        }
        assert(u_spec(*fm, ka, ds@) == u_spec(*fm, ka, ds@));
        assert(uset_ok(u) && forall|fy: int, name: Option<Seq<u8>>| u_has(u, fy, name) <==> u_pred_s(*fm, ka, ds@, fy, name));
    }
    let ct = plan_classes(cls, &u);
    let mvs = plan_mv(cls, &ct, &u);
    let w = plan_w(&u, &mvs);
    let cw = plan_classes(cls, &w);
    proof {
        let us = u_spec(*fm, ka, ds@);
        assert(uset_ok(us) && forall|fy: int, name: Option<Seq<u8>>| u_has(us, fy, name) <==> u_pred_s(*fm, ka, ds@, fy, name));
        assert forall|f: int, n: Option<Seq<u8>>| u_has(u, f, n) == u_has(us, f, n) by {}
        assert forall|fy: int, name: Option<Seq<u8>>| u_has(w, fy, name) <==> w_pred(*cls, *fm, ka, ds@, fy, name) by {
            lemma_mv_vw(*cls, ct, u, mvs@, fy, name);
            assert(u_has(w, fy, name) == (u_has(u, fy, name) || mv_has(mvs@, fy, name)));
            // vw_pred over u and over u_spec agree
            if vw_pred(*cls, u, fy, name) {
                let ok = choose|ok: KeyView| #![trigger cls.at(ok)] cls.at(ok) is Some && cls.at(ok)->0.fy as int == fy && fy_in_u(u, fy)
                    && notu_keys(cls.at(ok)->0.r.entries(), fy, u).len() < k_limit() && exists|k: KeyView| vsel(cls.at(ok)->0.f.entries(), fy, u).contains(k) && ov(cls.at(ok)->0.f.at(k)->0.name) == name;
                lemma_notu_congr(cls.at(ok)->0.r.entries(), fy, u, us);
                lemma_vsel_congr(cls.at(ok)->0.f.entries(), fy, u, us);
                assert(fy_in_u(us, fy)) by { let kk = choose|kk: KeyView| #![trigger u.at(kk)] u.at(kk) is Some && kk.0 == fy; let ur = u.at(kk)->0; assert(kk == ukey(ur.fy as int, ov(ur.name))); assert(u_has(u, ur.fy as int, ov(ur.name))); assert(u_has(us, ur.fy as int, ov(ur.name))); assert(us.at(kk) is Some); }
                assert(vw_pred(*cls, us, fy, name));
            }
            if vw_pred(*cls, us, fy, name) {
                let ok = choose|ok: KeyView| #![trigger cls.at(ok)] cls.at(ok) is Some && cls.at(ok)->0.fy as int == fy && fy_in_u(us, fy)
                    && notu_keys(cls.at(ok)->0.r.entries(), fy, us).len() < k_limit() && exists|k: KeyView| vsel(cls.at(ok)->0.f.entries(), fy, us).contains(k) && ov(cls.at(ok)->0.f.at(k)->0.name) == name;
                lemma_notu_congr(cls.at(ok)->0.r.entries(), fy, u, us);
                lemma_vsel_congr(cls.at(ok)->0.f.entries(), fy, u, us);
                assert(fy_in_u(u, fy)) by { let kk = choose|kk: KeyView| #![trigger us.at(kk)] us.at(kk) is Some && kk.0 == fy; let ur = us.at(kk)->0; assert(kk == ukey(ur.fy as int, ov(ur.name))); assert(u_has(us, ur.fy as int, ov(ur.name))); assert(u_has(u, ur.fy as int, ov(ur.name))); assert(u.at(kk) is Some); }
                assert(vw_pred(*cls, u, fy, name));
            }
        }
        // plan facts
        assert(subset_u(u, w));
        assert forall|ok: KeyView| ct.at(ok) is Some implies cw.at(ok) is Some by {
            let kk = choose|kk: KeyView| #![trigger u.at(kk)] u.at(kk) is Some && kk.0 == cls.at(ok)->0.fy as int;
            lemma_subset_key(u, w, kk);
        }
        assert(refill_ok(*cls, ct, u, w)) by {
            assert forall|ok: KeyView, k: KeyView| ct.at(ok) is Some && cls.at(ok) is Some && notu_keys(cls.at(ok)->0.r.entries(), cls.at(ok)->0.fy as int, u).len() < k_limit()
                && #[trigger] vsel(cls.at(ok)->0.f.entries(), cls.at(ok)->0.fy as int, u).contains(k) implies wn(w, cls.at(ok)->0.fy as int, nkey_v(ov(cls.at(ok)->0.f.at(k)->0.name))) by {
                let i = choose|i: int| 0 <= i < mvs.len() && okey((#[trigger] mvs@[i]).fy as int, mvs@[i].stmt@, mvs@[i].uom@) == ok && rkey(*mvs@[i].g) == k;
                let c = cls.at(ok)->0;
                assert(c.f.at(rkey(*mvs@[i].g)) == Some(*mvs@[i].g));
                lemma_okey_inj(mvs@[i].fy as int, mvs@[i].stmt@, mvs@[i].uom@, c.fy as int, c.stmt@, c.uom@);
                assert(mv_has(mvs@, c.fy as int, ov(c.f.at(k)->0.name)));
                assert(u_has(w, c.fy as int, ov(c.f.at(k)->0.name)));
                assert(wn(w, c.fy as int, nkey_v(ov(c.f.at(k)->0.name))) == u_has(w, c.fy as int, ov(c.f.at(k)->0.name)));
            }
        }
    }
    let kf = filing_keys(um, &w, &keys0);
    proof {
        let ks = seq_view_bytes(kf@);
        assert(canon_keys(ks, wanted_pred(*cls, *fm, *um, ka, ds@))) by {
            assert forall|a: Seq<u8>| ks.contains(a) <==> wanted_pred(*cls, *fm, *um, ka, ds@)(a) by {
                assert(seq_view_bytes(keys0@) == ka);
                if wanted_pred(*cls, *fm, *um, ka, ds@)(a) && !ka.contains(a) {
                    let (fy, name) = choose|fy: int, name: Option<Seq<u8>>| #![trigger um_has(*um, fy, name, a)] w_pred(*cls, *fm, ka, ds@, fy, name) && um_has(*um, fy, name, a);
                    assert(u_has(w, fy, name));
                }
                if ks.contains(a) && !ka.contains(a) {
                    let (fy, name) = choose|fy: int, name: Option<Seq<u8>>| #![trigger um_has(*um, fy, name, a)] u_has(w, fy, name) && um_has(*um, fy, name, a);
                    assert(w_pred(*cls, *fm, ka, ds@, fy, name));
                }
            }
        }
    }
    Plan { u, ct, w, cw, keys: kf }
}

} // verus!
