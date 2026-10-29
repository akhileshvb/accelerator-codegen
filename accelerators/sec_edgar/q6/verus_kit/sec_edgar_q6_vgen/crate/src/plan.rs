// The units a transaction touches, the refills of its classes, and the filings to rescan.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView, Cursor};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::esc::*;
use crate::accmap::*;
use crate::inv::*;
use crate::crops::*;
use crate::grp::*;
use crate::units::*;
use crate::lists::*;

verus! {

/// The sub delta's unit, when its fy is not NULL.
pub open spec fn sd_at(ds: Seq<SubDelta>, j: int, fy: int, name: Option<Seq<u8>>) -> bool {
    ds[j].row.fy__null == 0 && ds[j].row.fy as int == fy && sub_rt_view(ds[j].row).name == name
}

pub open spec fn sd_has(ds: Seq<SubDelta>, fy: int, name: Option<Seq<u8>>) -> bool {
    exists|j: int| 0 <= j < ds.len() && #[trigger] sd_at(ds, j, fy, name)
}

/// Units with a sub row (before the transaction) of a filing with a delta row, and units of sub deltas.
pub open spec fn u_pred(fm: Index<Vec<UnitRef>>, keys: Seq<Vec<u8>>, ds: Seq<SubDelta>, fy: int, name: Option<Seq<u8>>) -> bool {
    (exists|i: int| 0 <= i < keys.len() && fm_has(fm, (#[trigger] keys[i])@, fy, name)) || sd_has(ds, fy, name)
}

pub fn plan_u(fm: &Index<Vec<UnitRef>>, keys: &Vec<Vec<u8>>, ds: &Vec<SubDelta>) -> (u: Index<UnitRef>)
    requires fm.wf(),
    ensures
        uset_ok(u),
        forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(u, fy, name)] u_has(u, fy, name) <==> u_pred(*fm, keys@, ds@, fy, name),
{
    let mut u: Index<UnitRef> = Index::new();
    let mut i = 0;
    while i < keys.len()
        invariant
            i <= keys.len(), fm.wf(), uset_ok(u),
            forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(u, fy, name)] u_has(u, fy, name) <==> (exists|i2: int| 0 <= i2 < i && fm_has(*fm, (#[trigger] keys@[i2])@, fy, name)),
        decreases keys.len() - i,
    {
        let ghost u0 = u;
        match fm.get(&akey_of(keys[i].as_slice())) {
            None => {
                proof {
                    assert forall|fy: int, name: Option<Seq<u8>>| u_has(u, fy, name) <==> (exists|i2: int| 0 <= i2 < i + 1 && fm_has(*fm, (#[trigger] keys@[i2])@, fy, name)) by {
                        if u_has(u, fy, name) {
                            let i2 = choose|i2: int| 0 <= i2 < i && fm_has(*fm, keys@[i2]@, fy, name);
                        }
                        if exists|i2: int| 0 <= i2 < i + 1 && fm_has(*fm, (#[trigger] keys@[i2])@, fy, name) {
                            let i2 = choose|i2: int| 0 <= i2 < i + 1 && fm_has(*fm, keys@[i2]@, fy, name);
                            if i2 == i { assert(fm.at(akey(keys@[i as int]@)) is None); }
                        }
                    }
                }
            }
            Some(l) => {
                let mut j = 0;
                proof { assert(fm.at(akey(keys@[i as int]@)) == Some(arc_val(&l))); }
                while j < l.len()
                    invariant
                        j <= l.len(), uset_ok(u), fm.wf(), i < keys.len(),
                        fm.at(akey(keys@[i as int]@)) == Some(arc_val(&l)),
                        forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(u, fy, name)] u_has(u, fy, name) <==>
                            ((exists|i2: int| 0 <= i2 < i && fm_has(*fm, (#[trigger] keys@[i2])@, fy, name))
                                || (exists|j2: int| 0 <= j2 < j && (#[trigger] l@[j2]).fy as int == fy && ov(l@[j2].name) == name)),
                    decreases l.len() - j,
                {
                    uset_add(&mut u, l[j].fy, &l[j].name);
                    j += 1;
                    proof {
                        assert forall|fy: int, name: Option<Seq<u8>>| u_has(u, fy, name) <==>
                            ((exists|i2: int| 0 <= i2 < i && fm_has(*fm, (#[trigger] keys@[i2])@, fy, name))
                                || (exists|j2: int| 0 <= j2 < j && (#[trigger] l@[j2]).fy as int == fy && ov(l@[j2].name) == name)) by {
                            let jj = (j - 1) as int;
                            if fy == l@[jj].fy as int && name == ov(l@[jj].name) { assert(l@[jj].fy as int == fy); }
                            if exists|j2: int| 0 <= j2 < j - 1 && l@[j2].fy as int == fy && ov(l@[j2].name) == name {
                                let j2 = choose|j2: int| 0 <= j2 < j - 1 && l@[j2].fy as int == fy && ov(l@[j2].name) == name;
                            }
                            if exists|j2: int| 0 <= j2 < j && l@[j2].fy as int == fy && ov(l@[j2].name) == name {
                                let j2 = choose|j2: int| 0 <= j2 < j && l@[j2].fy as int == fy && ov(l@[j2].name) == name;
                            }
                        }
                    }
                }
                proof {
                    assert forall|fy: int, name: Option<Seq<u8>>| u_has(u, fy, name) <==> (exists|i2: int| 0 <= i2 < i + 1 && fm_has(*fm, (#[trigger] keys@[i2])@, fy, name)) by {
                        if u_has(u, fy, name) {
                            if !(exists|i2: int| 0 <= i2 < i && fm_has(*fm, keys@[i2]@, fy, name)) {
                                let j2 = choose|j2: int| 0 <= j2 < l.len() && l@[j2].fy as int == fy && ov(l@[j2].name) == name;
                                assert(uref_has(l@, fy, name));
                                assert(fm_has(*fm, keys@[i as int]@, fy, name));
                            }
                        }
                        if exists|i2: int| 0 <= i2 < i + 1 && fm_has(*fm, (#[trigger] keys@[i2])@, fy, name) {
                            let i2 = choose|i2: int| 0 <= i2 < i + 1 && fm_has(*fm, keys@[i2]@, fy, name);
                            if i2 == i {
                                assert(uref_has(l@, fy, name));
                                let j2 = choose|j2: int| 0 <= j2 < l.len() && l@[j2].fy as int == fy && ov(l@[j2].name) == name;
                            }
                        }
                    }
                }
            }
        }
        i += 1;
    }
    let mut t = 0;
    while t < ds.len()
        invariant
            t <= ds.len(), uset_ok(u),
            forall|fy: int, name: Option<Seq<u8>>| #![trigger u_has(u, fy, name)] u_has(u, fy, name) <==>
                ((exists|i2: int| 0 <= i2 < keys.len() && fm_has(*fm, (#[trigger] keys@[i2])@, fy, name)) || sd_has(ds@.take(t as int), fy, name)),
        decreases ds.len() - t,
    {
        if ds[t].row.fy__null == 0 {
            let nm: Option<Vec<u8>> = if ds[t].row.name__null != 0 { None } else { Some(copy_bytes(ds[t].row.name.as_slice())) };
            uset_add(&mut u, ds[t].row.fy, &nm);
        }
        proof {
            assert forall|fy: int, name: Option<Seq<u8>>| u_has(u, fy, name) <==>
                ((exists|i2: int| 0 <= i2 < keys.len() && fm_has(*fm, (#[trigger] keys@[i2])@, fy, name)) || sd_has(ds@.take(t as int + 1), fy, name)) by {
                let d = ds@[t as int];
                if d.row.fy__null == 0 && ukey(fy, name) == ukey(d.row.fy as int, sub_rt_view(d.row).name) {
                    lemma_ukey_inj(fy, name, d.row.fy as int, sub_rt_view(d.row).name);
                }
                if sd_has(ds@.take(t as int + 1), fy, name) {
                    let j = choose|j: int| 0 <= j < t as int + 1 && #[trigger] sd_at(ds@.take(t as int + 1), j, fy, name);
                    if j < t as int { assert(sd_at(ds@.take(t as int), j, fy, name)); }
                    else { assert(sd_at(ds@, t as int, fy, name)); }
                }
                if sd_has(ds@.take(t as int), fy, name) {
                    let j = choose|j: int| 0 <= j < t as int && #[trigger] sd_at(ds@.take(t as int), j, fy, name);
                    assert(sd_at(ds@.take(t as int + 1), j, fy, name));
                }
                if d.row.fy__null == 0 && ukey(fy, name) == ukey(d.row.fy as int, sub_rt_view(d.row).name) {
                    assert(sd_at(ds@.take(t as int + 1), t as int, fy, name));
                }
            }
        }
        t += 1;
    }
    proof {
        assert(ds@.take(t as int) =~= ds@);
        assert forall|fy: int, name: Option<Seq<u8>>| u_has(u, fy, name) <==> u_pred(*fm, keys@, ds@, fy, name) by {}
    }
    u
}

/// The keys of the entries whose unit is not in u, in order.
pub open spec fn notu_keys(es: Seq<(KeyView, Grp)>, fy: int, u: Index<UnitRef>) -> Seq<KeyView>
    decreases es.len(),
{
    if es.len() == 0 { Seq::empty() } else {
        notu_keys(es.drop_last(), fy, u) + if u_has(u, fy, ov(es.last().1.name)) { Seq::empty() } else { seq![es.last().0] }
    }
}

pub proof fn lemma_notu_take(es: Seq<(KeyView, Grp)>, fy: int, u: Index<UnitRef>, i: int)
    requires 0 <= i < es.len(),
    ensures notu_keys(es.take(i + 1), fy, u) == notu_keys(es.take(i), fy, u) + if u_has(u, fy, ov(es[i].1.name)) { Seq::empty() } else { seq![es[i].0] },
{
    let t = es.take(i + 1);
    assert(t.drop_last() =~= es.take(i));
    assert(t.last() == es[i]);
}

/// How many of the class's kept groups are of units outside u.
pub fn count_notu(c: &ClassRec, u: &Index<UnitRef>) -> (n: usize)
    requires c.r.wf(), uset_ok(*u),
    ensures n as int == notu_keys(c.r.entries(), c.fy as int, *u).len(),
{
    let ents = c.r.rows();
    let ghost es = c.r.entries();
    let mut n: usize = 0;
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(), c.r.wf(), uset_ok(*u),
            ents@.map_values(|row: (Key, Arc<Grp>)| (row.0.view(), *row.1)) == es, es == c.r.entries(),
            n as int == notu_keys(es.take(i as int), c.fy as int, *u).len(), n <= i,
        decreases ents.len() - i,
    {
        let g = &ents[i].1;
        proof {
            assert(es[i as int] == (ents@[i as int].0.view(), arc_val(g)));
            lemma_notu_take(es, c.fy as int, *u, i as int);
        }
        let found = u.get(&ukey_of(c.fy, &g.name));
        if found.is_none() { n += 1; }
        i += 1;
    }
    proof { assert(es.take(i as int) =~= es); }
    n
}

pub open spec fn cset_ok(ct: Index<ClassRef>) -> bool {
    ct.wf() && forall|q: KeyView| #![trigger ct.at(q)] ct.at(q) is Some ==> q == okey(ct.at(q)->0.fy as int, ct.at(q)->0.stmt@, ct.at(q)->0.uom@)
}

pub open spec fn ct_has(ct: Index<ClassRef>, fy: int, stmt: Seq<u8>, uom: Seq<u8>) -> bool {
    ct.at(okey(fy, stmt, uom)) is Some
}

pub fn copy_cref(cr: &ClassRef) -> (r: ClassRef)
    ensures r.fy == cr.fy, r.stmt@ == cr.stmt@, r.uom@ == cr.uom@,
{
    ClassRef { fy: cr.fy, stmt: copy_bytes(cr.stmt.as_slice()), uom: copy_bytes(cr.uom.as_slice()) }
}

pub open spec fn ct_via(um: Index<UnitRec>, ue: UnitRef, fy: int, stmt: Seq<u8>, uom: Seq<u8>) -> bool {
    match um.at(ukey(ue.fy as int, ov(ue.name))) { Some(r) => cref_has(r.classes@, fy, stmt, uom), None => false }
}

/// The classes some unit of u may have groups in.
pub fn plan_ct(um: &Index<UnitRec>, u: &Index<UnitRef>) -> (ct: Index<ClassRef>)
    requires um.wf(), uset_ok(*u),
    ensures
        cset_ok(ct),
        forall|fy: int, stmt: Seq<u8>, uom: Seq<u8>| #![trigger ct_has(ct, fy, stmt, uom)] ct_has(ct, fy, stmt, uom) <==>
            exists|q: KeyView| #![trigger u.at(q)] u.at(q) is Some && ct_via(*um, u.at(q)->0, fy, stmt, uom),
{
    let mut ct: Index<ClassRef> = Index::new();
    let ents = u.rows();
    let ghost es = u.entries();
    proof { u.model(); }
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(), um.wf(), uset_ok(*u), u.wf(), cset_ok(ct),
            ents@.map_values(|row: (Key, Arc<UnitRef>)| (row.0.view(), *row.1)) == es, es == u.entries(),
            forall|fy: int, stmt: Seq<u8>, uom: Seq<u8>| #![trigger ct_has(ct, fy, stmt, uom)] ct_has(ct, fy, stmt, uom) <==>
                exists|i2: int| 0 <= i2 < i && #[trigger] ct_via(*um, es[i2].1, fy, stmt, uom),
        decreases ents.len() - i,
    {
        let ue = &ents[i].1;
        proof { assert(es[i as int] == (ents@[i as int].0.view(), arc_val(ue))); }
        match um.get(&ukey_of(ue.fy, &ue.name)) {
            None => {
                proof {
                    assert forall|fy: int, stmt: Seq<u8>, uom: Seq<u8>| ct_has(ct, fy, stmt, uom) <==> exists|i2: int| 0 <= i2 < i + 1 && #[trigger] ct_via(*um, es[i2].1, fy, stmt, uom) by {
                        if exists|i2: int| 0 <= i2 < i + 1 && ct_via(*um, es[i2].1, fy, stmt, uom) {
                            let i2 = choose|i2: int| 0 <= i2 < i + 1 && ct_via(*um, es[i2].1, fy, stmt, uom);
                            if i2 == i { assert(um.at(ukey(arc_val(ue).fy as int, ov(arc_val(ue).name))) is None); }
                        }
                        if ct_has(ct, fy, stmt, uom) { let i2 = choose|i2: int| 0 <= i2 < i && ct_via(*um, es[i2].1, fy, stmt, uom); }
                    }
                }
            }
            Some(r) => {
                proof { assert(um.at(ukey(arc_val(ue).fy as int, ov(arc_val(ue).name))) == Some(arc_val(&r))); }
                let mut j = 0;
                while j < r.classes.len()
                    invariant
                        j <= r.classes.len(), cset_ok(ct), i < ents.len(),
                        es[i as int].1 == arc_val(ue),
                        um.at(ukey(es[i as int].1.fy as int, ov(es[i as int].1.name))) == Some(arc_val(&r)),
                        forall|fy: int, stmt: Seq<u8>, uom: Seq<u8>| #![trigger ct_has(ct, fy, stmt, uom)] ct_has(ct, fy, stmt, uom) <==>
                            ((exists|i2: int| 0 <= i2 < i && #[trigger] ct_via(*um, es[i2].1, fy, stmt, uom))
                                || (exists|j2: int| 0 <= j2 < j && #[trigger] cref_is(r.classes@[j2], fy, stmt, uom))),
                    decreases r.classes.len() - j,
                {
                    let cr = copy_cref(&r.classes[j]);
                    let k = okey_of(cr.fy, cr.stmt.as_slice(), cr.uom.as_slice());
                    let ghost crv = cr;
                    let ghost ct0 = ct;
                    ct.put(k, Arc::new(cr));
                    j += 1;
                    proof {
                        assert forall|q: KeyView| ct.at(q) is Some implies q == okey(ct.at(q)->0.fy as int, ct.at(q)->0.stmt@, ct.at(q)->0.uom@) by {
                            if q != k.view() { assert(ct0.at(q) is Some); }
                        }
                        assert forall|fy: int, stmt: Seq<u8>, uom: Seq<u8>| ct_has(ct, fy, stmt, uom) <==>
                            ((exists|i2: int| 0 <= i2 < i && #[trigger] ct_via(*um, es[i2].1, fy, stmt, uom))
                                || (exists|j2: int| 0 <= j2 < j && #[trigger] cref_is(r.classes@[j2], fy, stmt, uom))) by {
                            let jj = (j - 1) as int;
                            let kk = okey(crv.fy as int, crv.stmt@, crv.uom@);
                            assert(ct_has(ct0, fy, stmt, uom) <==>
                                ((exists|i2: int| 0 <= i2 < i && #[trigger] ct_via(*um, es[i2].1, fy, stmt, uom))
                                    || (exists|j2: int| 0 <= j2 < jj && #[trigger] cref_is(r.classes@[j2], fy, stmt, uom))));
                            assert(ct.at(okey(fy, stmt, uom)) == if okey(fy, stmt, uom) == kk { Some(crv) } else { ct0.at(okey(fy, stmt, uom)) });
                            assert(cref_is(r.classes@[jj], crv.fy as int, crv.stmt@, crv.uom@));
                            if okey(fy, stmt, uom) == kk {
                                lemma_okey_inj(fy, stmt, uom, crv.fy as int, crv.stmt@, crv.uom@);
                                assert(cref_is(r.classes@[jj], fy, stmt, uom));
                            }
                            if exists|j2: int| 0 <= j2 < j && cref_is(r.classes@[j2], fy, stmt, uom) {
                                let j2 = choose|j2: int| 0 <= j2 < j && cref_is(r.classes@[j2], fy, stmt, uom);
                                if j2 == jj {
                                    assert(okey(fy, stmt, uom) == kk);
                                }
                            }
                        }
                    }
                }
                proof {
                    assert forall|fy: int, stmt: Seq<u8>, uom: Seq<u8>| ct_has(ct, fy, stmt, uom) <==> exists|i2: int| 0 <= i2 < i + 1 && #[trigger] ct_via(*um, es[i2].1, fy, stmt, uom) by {
                        if exists|j2: int| 0 <= j2 < r.classes.len() && cref_is(r.classes@[j2], fy, stmt, uom) {
                            assert(cref_has(r.classes@, fy, stmt, uom));
                            assert(ct_via(*um, es[i as int].1, fy, stmt, uom));
                        }
                        if exists|i2: int| 0 <= i2 < i + 1 && ct_via(*um, es[i2].1, fy, stmt, uom) {
                            let i2 = choose|i2: int| 0 <= i2 < i + 1 && ct_via(*um, es[i2].1, fy, stmt, uom);
                            if i2 == i {
                                let j2 = choose|j2: int| 0 <= j2 < r.classes.len() && cref_is(r.classes@[j2], fy, stmt, uom);
                            }
                        }
                    }
                }
            }
        }
        i += 1;
    }
    proof {
        assert forall|fy: int, stmt: Seq<u8>, uom: Seq<u8>| ct_has(ct, fy, stmt, uom) <==>
            exists|q: KeyView| #![trigger u.at(q)] u.at(q) is Some && ct_via(*um, u.at(q)->0, fy, stmt, uom) by {
            if exists|i2: int| 0 <= i2 < es.len() && ct_via(*um, es[i2].1, fy, stmt, uom) {
                let i2 = choose|i2: int| 0 <= i2 < es.len() && ct_via(*um, es[i2].1, fy, stmt, uom);
                assert(u.at(es[i2].0) == Some(es[i2].1));
            }
            if exists|q: KeyView| u.at(q) is Some && ct_via(*um, u.at(q)->0, fy, stmt, uom) {
                let q = choose|q: KeyView| #![trigger u.at(q)] u.at(q) is Some && ct_via(*um, u.at(q)->0, fy, stmt, uom);
                let pos = choose|pos: int| #![auto] 0 <= pos < es.len() && es[pos].0 == q;
                assert(es[pos].1 == u.at(q)->0);
            }
        }
    }
    ct
}

pub proof fn lemma_notu_nil(fy: int, u: Index<UnitRef>)
    ensures notu_keys(Seq::<(KeyView, Grp)>::empty(), fy, u) =~= Seq::<KeyView>::empty(),
{
    reveal_with_fuel(notu_keys, 2);
}

pub open spec fn take_up<A>(s: Seq<A>, n: int) -> Seq<A> {
    s.take(if s.len() < n { s.len() as int } else { n })
}

pub open spec fn vsel(es: Seq<(KeyView, Grp)>, fy: int, u: Index<UnitRef>) -> Seq<KeyView> {
    take_up(notu_keys(es, fy, u), 512)
}

pub proof fn lemma_notu_prefix(es: Seq<(KeyView, Grp)>, fy: int, u: Index<UnitRef>, t: int)
    requires 0 <= t <= es.len(),
    ensures
        notu_keys(es.take(t), fy, u).len() <= notu_keys(es, fy, u).len(),
        notu_keys(es.take(t), fy, u) =~= notu_keys(es, fy, u).take(notu_keys(es.take(t), fy, u).len() as int),
    decreases es.len() - t,
{
    if t < es.len() {
        lemma_notu_take(es, fy, u, t);
        lemma_notu_prefix(es, fy, u, t + 1);
        let a = notu_keys(es.take(t), fy, u);
        let b = notu_keys(es.take(t + 1), fy, u);
        let full = notu_keys(es, fy, u);
        assert(b =~= full.take(b.len() as int));
        assert(b == a + if u_has(u, fy, ov(es[t].1.name)) { Seq::empty() } else { seq![es[t].0] });
        assert(a =~= b.take(a.len() as int));
        assert(a.len() <= b.len());
        assert(full.take(b.len() as int).take(a.len() as int) =~= full.take(a.len() as int));
        assert(a =~= full.take(a.len() as int));
    } else {
        assert(es.take(t) =~= es);
    }
}

/// The frontier groups of the first units (by rank) that are not in u, at most REFILL of them.
pub fn select_f(c: &ClassRec, u: &Index<UnitRef>) -> (picked: Vec<Arc<Grp>>)
    requires c.f.wf(), uset_ok(*u), forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is Some ==> q == rkey(c.f.at(q)->0),
    ensures
        picked@.map_values(|g: Arc<Grp>| rkey(*g)) == vsel(c.f.entries(), c.fy as int, *u),
        forall|i: int| 0 <= i < picked.len() ==> c.f.at(rkey(*(#[trigger] picked@[i]))) == Some(*picked@[i]),
{
    let ghost es = c.f.entries();
    let ghost fy = c.fy as int;
    let mut picked: Vec<Arc<Grp>> = Vec::new();
    let mut cur = c.f.cursor();
    let ghost mut t: int = 0;
    let mut more = true;
    proof {
        c.f.model();
        assert forall|pos: int| 0 <= pos < es.len() implies es[pos].0 == rkey(es[pos].1) by { assert(c.f.at(es[pos].0) == Some(es[pos].1)); }
        assert(cur.remaining() =~= es.skip(0));
        assert(es.take(0) =~= Seq::<(KeyView, Grp)>::empty());
        lemma_notu_nil(fy, *u);
        assert(notu_keys(es.take(0), fy, *u) =~= Seq::<KeyView>::empty());
        assert(take_up(notu_keys(es.take(0), fy, *u), 512) =~= Seq::<KeyView>::empty());
        assert(picked@.map_values(|g: Arc<Grp>| rkey(*g)) =~= Seq::<KeyView>::empty());
    }
    while picked.len() < 512 && more
        invariant
            c.f.wf(), uset_ok(*u), es == c.f.entries(), fy == c.fy as int,
            0 <= t <= es.len(), cur.remaining() == es.skip(t),
            !more ==> t == es.len(),
            picked.len() <= 512,
            forall|pos: int| #![trigger es[pos]] 0 <= pos < es.len() ==> c.f.at(es[pos].0) == Some(es[pos].1) && es[pos].0 == rkey(es[pos].1),
            picked@.map_values(|g: Arc<Grp>| rkey(*g)) == take_up(notu_keys(es.take(t), fy, *u), 512),
            forall|i: int| 0 <= i < picked.len() ==> c.f.at(rkey(*(#[trigger] picked@[i]))) == Some(*picked@[i]),
        decreases es.len() - t + if more { 1int } else { 0int },
    {
        let ghost before = picked@;
        match cur.next() {
            None => {
                more = false;
                proof { assert(es.skip(t).len() == 0); }
            }
            Some((key, g)) => {
                proof {
                    assert(es.skip(t).len() > 0);
                    assert(t < es.len());
                    assert(es.skip(t)[0] == es[t]);
                    assert(es.skip(t).skip(1) =~= es.skip(t + 1));
                    lemma_notu_take(es, fy, *u, t);
                    assert(c.f.at(es[t].0) == Some(es[t].1));
                    assert(key.view() == es[t].0 && arc_val(&g) == es[t].1);
                }
                let found = u.get(&ukey_of(c.fy, &g.name));
                if found.is_none() {
                    let ghost gv = arc_val(&g);
                    picked.push(g);
                    proof {
                        let nk = notu_keys(es.take(t), fy, *u);
                        assert(picked@ == before.push(g));
                        assert(picked@.map_values(|g: Arc<Grp>| rkey(*g)) =~= before.map_values(|g: Arc<Grp>| rkey(*g)).push(es[t].0));
                        assert(nk.len() < 512) by { assert(before.len() < 512); assert(take_up(nk, 512).len() == before.len()); }
                        assert(take_up(nk, 512) =~= nk);
                        assert(notu_keys(es.take(t + 1), fy, *u) =~= nk.push(es[t].0));
                        assert(take_up(nk.push(es[t].0), 512) =~= nk.push(es[t].0));
                    }
                } else {
                    proof {
                        assert(u_has(*u, fy, ov(es[t].1.name)));
                        assert(notu_keys(es.take(t + 1), fy, *u) =~= notu_keys(es.take(t), fy, *u));
                    }
                }
                proof { t = t + 1; }
            }
        }
    }
    proof {
        lemma_notu_prefix(es, fy, *u, t);
        let a = notu_keys(es.take(t), fy, *u);
        let full = notu_keys(es, fy, *u);
        if picked.len() == 512 {
            assert(a.len() >= 512);
            assert(take_up(a, 512) =~= a.take(512));
            assert(a.take(512) =~= full.take(512));
            assert(take_up(full, 512) =~= full.take(512));
        } else {
            assert(!more && t == es.len());
            assert(es.take(t) =~= es);
        }
    }
    picked
}

} // verus!
