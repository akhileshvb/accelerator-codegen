// The two unit maps (filing -> units, unit -> filings) after a rescan of filings.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::esc::*;
use crate::accmap::*;
use crate::crops::*;
use crate::units::*;
use crate::plan::*;
use crate::grp::*;

verus! {

pub fn copy_units(l: &Vec<UnitRef>) -> (r: Vec<UnitRef>)
    ensures r.len() == l.len(), forall|i: int| 0 <= i < l.len() ==> (#[trigger] r@[i]).fy == l@[i].fy && ov(r@[i].name) == ov(l@[i].name),
{
    let mut r: Vec<UnitRef> = Vec::new();
    let mut i = 0;
    while i < l.len()
        invariant i <= l.len(), r.len() == i, forall|j: int| 0 <= j < i ==> (#[trigger] r@[j]).fy == l@[j].fy && ov(r@[j].name) == ov(l@[j].name),
        decreases l.len() - i,
    {
        r.push(UnitRef { fy: l[i].fy, name: copy_opt(&l[i].name) });
        i += 1;
    }
    r
}

pub fn copy_names(l: &Vec<Vec<u8>>) -> (r: Vec<Vec<u8>>)
    ensures r.len() == l.len(), forall|i: int| 0 <= i < l.len() ==> (#[trigger] r@[i])@ == l@[i]@,
{
    let mut r: Vec<Vec<u8>> = Vec::new();
    let mut i = 0;
    while i < l.len()
        invariant i <= l.len(), r.len() == i, forall|j: int| 0 <= j < i ==> (#[trigger] r@[j])@ == l@[j]@,
        decreases l.len() - i,
    {
        r.push(copy_bytes(l[i].as_slice()));
        i += 1;
    }
    r
}

pub proof fn lemma_uref_copy(a: Seq<UnitRef>, b: Seq<UnitRef>, fy: int, name: Option<Seq<u8>>)
    requires a.len() == b.len(), forall|i: int| 0 <= i < a.len() ==> (#[trigger] a[i]).fy == b[i].fy && ov(a[i].name) == ov(b[i].name),
    ensures uref_has(a, fy, name) == uref_has(b, fy, name),
{
    if uref_has(a, fy, name) { let i = choose|i: int| 0 <= i < a.len() && a[i].fy as int == fy && ov(a[i].name) == name; assert(b[i].fy as int == fy); }
    if uref_has(b, fy, name) { let i = choose|i: int| 0 <= i < b.len() && b[i].fy as int == fy && ov(b[i].name) == name; assert(a[i].fy as int == fy); }
}

/// Whether row i of the rescan is a sub row of unit (fy, name).
pub open spec fn rs_unit(rs: Seq<SubRt>, i: int, fy: int, name: Option<Seq<u8>>) -> bool {
    rs[i].fy__null == 0 && rs[i].fy as int == fy && sub_rt_view(rs[i]).name == name
}

/// filing -> units after the rescan of the filings in keys.
pub fn fm_update(fm: &Index<Vec<UnitRef>>, keys: &Vec<Vec<u8>>, rs: &Vec<SubRt>) -> (r: Index<Vec<UnitRef>>)
    requires fm.wf(),
    ensures
        r.wf(),
        forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| #![trigger fm_has(r, a, fy, name)] fm_has(r, a, fy, name) <==>
            ((!seq_view_bytes(keys@).contains(a) && fm_has(*fm, a, fy, name))
                || exists|i: int| 0 <= i < rs.len() && (#[trigger] rs@[i]).adsh@ == a && rs_unit(rs@, i, fy, name)),
{
    let mut r = sh(fm);
    let mut i = 0;
    while i < keys.len()
        invariant
            i <= keys.len(), r.wf(),
            forall|a: Seq<u8>| #![trigger r.at(akey(a))] r.at(akey(a)) == if (exists|j: int| 0 <= j < i && (#[trigger] keys@[j])@ == a) { None } else { fm.at(akey(a)) },
        decreases keys.len() - i,
    {
        r.remove(&akey_of(keys[i].as_slice()));
        i += 1;
        proof {
            assert forall|a: Seq<u8>| r.at(akey(a)) == if (exists|j: int| 0 <= j < i && keys@[j]@ == a) { None } else { fm.at(akey(a)) } by {
                if exists|j: int| 0 <= j < i && keys@[j]@ == a { let j = choose|j: int| 0 <= j < i && keys@[j]@ == a; if j < i - 1 { assert(exists|j2: int| 0 <= j2 < i - 1 && keys@[j2]@ == a); } }
            }
        }
    }
    let ghost r1 = r;
    let mut t = 0;
    while t < rs.len()
        invariant
            t <= rs.len(), r.wf(),
            forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| #![trigger fm_has(r, a, fy, name)] fm_has(r, a, fy, name) <==>
                (fm_has(r1, a, fy, name) || exists|i: int| 0 <= i < t && (#[trigger] rs@[i]).adsh@ == a && rs_unit(rs@, i, fy, name)),
        decreases rs.len() - t,
    {
        if rs[t].fy__null == 0 {
            let nm: Option<Vec<u8>> = if rs[t].name__null != 0 { None } else { Some(copy_bytes(rs[t].name.as_slice())) };
            let ak = akey_of(rs[t].adsh.as_slice());
            let ghost r0 = r;
            let found = r.get(&ak);
            let mut l = match &found { Some(old) => copy_units(old), None => Vec::new() };
            let ghost l0 = l@;
            proof {
                if found is None { assert(l0 =~= Seq::<UnitRef>::empty()); assert(r0.at(ak.view()) is None); }
                if found is Some { assert(r0.at(ak.view()) == Some(arc_val(&found->0))); lemma_uref_copy(l0, r0.at(ak.view())->0@, 0, None); }
            }
            l.push(UnitRef { fy: rs[t].fy, name: nm });
            r.put(ak, Arc::new(l));
            proof {
                assert forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| fm_has(r, a, fy, name) <==>
                    (fm_has(r1, a, fy, name) || exists|i: int| 0 <= i < t + 1 && (#[trigger] rs@[i]).adsh@ == a && rs_unit(rs@, i, fy, name)) by {
                    if a == rs@[t as int].adsh@ {
                        assert(fm_has(r0, a, fy, name) == (fm_has(r1, a, fy, name) || exists|i: int| 0 <= i < t && rs@[i].adsh@ == a && rs_unit(rs@, i, fy, name)));
                        if r0.at(akey(a)) is Some { lemma_uref_copy(l0, r0.at(akey(a))->0@, fy, name); }
                        assert(uref_has(l0, fy, name) == fm_has(r0, a, fy, name));
                        assert(uref_has(l@, fy, name) == (uref_has(l0, fy, name) || (fy == rs@[t as int].fy as int && name == sub_rt_view(rs@[t as int]).name))) by {
                            let x = UnitRef { fy: rs@[t as int].fy, name: nm };
                            assert(l@ == l0.push(x));
                            if uref_has(l@, fy, name) { let i = choose|i: int| 0 <= i < l@.len() && l@[i].fy as int == fy && ov(l@[i].name) == name; if i < l0.len() { assert(l0[i] == l@[i]); } }
                            if uref_has(l0, fy, name) { let i = choose|i: int| 0 <= i < l0.len() && l0[i].fy as int == fy && ov(l0[i].name) == name; assert(l@[i] == l0[i]); }
                            if fy == rs@[t as int].fy as int && name == sub_rt_view(rs@[t as int]).name { assert(l@[l0.len() as int] == x); }
                        }
                        assert(fm_has(r, a, fy, name) == uref_has(l@, fy, name));
                        if fy == rs@[t as int].fy as int && name == sub_rt_view(rs@[t as int]).name { assert(rs_unit(rs@, t as int, fy, name)); }
                        if exists|i: int| 0 <= i < t + 1 && rs@[i].adsh@ == a && rs_unit(rs@, i, fy, name) { let i = choose|i: int| 0 <= i < t + 1 && rs@[i].adsh@ == a && rs_unit(rs@, i, fy, name); if i < t { assert(exists|i2: int| 0 <= i2 < t && rs@[i2].adsh@ == a && rs_unit(rs@, i2, fy, name)); } }
                    } else {
                        assert(akey(a) != akey(rs@[t as int].adsh@));
                        assert(r.at(akey(a)) == r0.at(akey(a)));
                        assert(fm_has(r, a, fy, name) == fm_has(r0, a, fy, name));
                        assert(fm_has(r0, a, fy, name) == (fm_has(r1, a, fy, name) || exists|i: int| 0 <= i < t && rs@[i].adsh@ == a && rs_unit(rs@, i, fy, name)));
                        if exists|i: int| 0 <= i < t + 1 && rs@[i].adsh@ == a && rs_unit(rs@, i, fy, name) { let i = choose|i: int| 0 <= i < t + 1 && rs@[i].adsh@ == a && rs_unit(rs@, i, fy, name); assert(i < t); }
                    }
                }
            }
        }
        proof {
            if rs@[t as int].fy__null != 0 {
                assert forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| fm_has(r, a, fy, name) <==>
                    (fm_has(r1, a, fy, name) || exists|i: int| 0 <= i < t + 1 && (#[trigger] rs@[i]).adsh@ == a && rs_unit(rs@, i, fy, name)) by {
                    if exists|i: int| 0 <= i < t + 1 && rs@[i].adsh@ == a && rs_unit(rs@, i, fy, name) { let i = choose|i: int| 0 <= i < t + 1 && rs@[i].adsh@ == a && rs_unit(rs@, i, fy, name); assert(i < t); }
                }
            }
        }
        t += 1;
    }
    proof {
        assert forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| fm_has(r, a, fy, name) <==>
            ((!seq_view_bytes(keys@).contains(a) && fm_has(*fm, a, fy, name)) || exists|i: int| 0 <= i < rs.len() && (#[trigger] rs@[i]).adsh@ == a && rs_unit(rs@, i, fy, name)) by {
            assert(fm_has(r1, a, fy, name) == (!seq_view_bytes(keys@).contains(a) && fm_has(*fm, a, fy, name))) by {
                if seq_view_bytes(keys@).contains(a) { let j = choose|j: int| 0 <= j < keys.len() && seq_view_bytes(keys@)[j] == a; assert(keys@[j]@ == a); }
                if exists|j: int| 0 <= j < keys.len() && keys@[j]@ == a { assert(seq_view_bytes(keys@)[( choose|j: int| 0 <= j < keys.len() && keys@[j]@ == a)] == a); }
            }
        }
    }
    r
}

/// The filings of the rescan's sub rows of one unit.
pub fn unit_filings(rs: &Vec<SubRt>, fy: i64, name: &Option<Vec<u8>>) -> (r: Vec<Vec<u8>>)
    ensures forall|a: Seq<u8>| #![trigger bytes_has(r@, a)] bytes_has(r@, a) <==> exists|i: int| 0 <= i < rs.len() && rs_unit(rs@, i, fy as int, ov(*name)) && (#[trigger] rs@[i]).adsh@ == a,
{
    let mut r: Vec<Vec<u8>> = Vec::new();
    let mut t = 0;
    while t < rs.len()
        invariant
            t <= rs.len(),
            forall|a: Seq<u8>| #![trigger bytes_has(r@, a)] bytes_has(r@, a) <==> exists|i: int| 0 <= i < t && rs_unit(rs@, i, fy as int, ov(*name)) && (#[trigger] rs@[i]).adsh@ == a,
        decreases rs.len() - t,
    {
        let same = rs[t].fy__null == 0 && rs[t].fy == fy && (match name { None => rs[t].name__null != 0, Some(n) => rs[t].name__null == 0 && verus_prooflib::ordered::compare_bytes(rs[t].name.as_slice(), n.as_slice()) == 0 });
        let ghost r0 = r@;
        if same { r.push(copy_bytes(rs[t].adsh.as_slice())); }
        proof {
            assert(same == rs_unit(rs@, t as int, fy as int, ov(*name)));
            assert forall|a: Seq<u8>| bytes_has(r@, a) <==> exists|i: int| 0 <= i < t + 1 && rs_unit(rs@, i, fy as int, ov(*name)) && (#[trigger] rs@[i]).adsh@ == a by {
                if bytes_has(r@, a) { let k = choose|k: int| 0 <= k < r@.len() && r@[k]@ == a; if k < r0.len() { assert(r0[k] == r@[k]); assert(bytes_has(r0, a)); } else { assert(same); assert(rs@[t as int].adsh@ == a); } }
                if bytes_has(r0, a) { let k = choose|k: int| 0 <= k < r0.len() && r0[k]@ == a; assert(r@[k] == r0[k]); }
                if exists|i: int| 0 <= i < t + 1 && rs_unit(rs@, i, fy as int, ov(*name)) && rs@[i].adsh@ == a {
                    let i = choose|i: int| 0 <= i < t + 1 && rs_unit(rs@, i, fy as int, ov(*name)) && rs@[i].adsh@ == a;
                    if i == t as int { assert(same); assert(r@[r0.len() as int]@ == a); } else { assert(bytes_has(r0, a)); }
                }
            }
        }
        t += 1;
    }
    r
}

/// unit -> filings after the rescan: the units in u have the filings of their sub rows in the rescan.
#[verifier::rlimit(200)]
#[verifier::spinoff_prover]
pub fn um_update(um: &Index<UnitRec>, u: &Index<UnitRef>, rs: &Vec<SubRt>) -> (r: Index<UnitRec>)
    requires um.wf(), uset_ok(*u),
    ensures
        r.wf(),
        forall|fy: int, name: Option<Seq<u8>>, a: Seq<u8>| #![trigger um_has(r, fy, name, a)] um_has(r, fy, name, a) <==>
            (if u_has(*u, fy, name) { exists|i: int| 0 <= i < rs.len() && rs_unit(rs@, i, fy, name) && (#[trigger] rs@[i]).adsh@ == a } else { um_has(*um, fy, name, a) }),
{
    let mut r = sh(um);
    let ents = u.rows();
    let ghost es = u.entries();
    proof { u.model(); }
    let mut i = 0;
    while i < ents.len()
        invariant
            i <= ents.len(), r.wf(), uset_ok(*u), u.wf(),
            ents@.map_values(|row: (Key, Arc<UnitRef>)| (row.0.view(), *row.1)) == es, es == u.entries(),
            forall|fy: int, name: Option<Seq<u8>>, a: Seq<u8>| #![trigger um_has(r, fy, name, a)] um_has(r, fy, name, a) <==>
                (if exists|j: int| 0 <= j < i && (#[trigger] es[j]).1.fy as int == fy && ov(es[j].1.name) == name {
                    exists|k: int| 0 <= k < rs.len() && rs_unit(rs@, k, fy, name) && (#[trigger] rs@[k]).adsh@ == a } else { um_has(*um, fy, name, a) }),
        decreases ents.len() - i,
    {
        let ur = &ents[i].1;
        proof { assert(es[i as int] == (ents@[i as int].0.view(), arc_val(ur))); }
        let fls = unit_filings(rs, ur.fy, &ur.name);
        let ghost r0 = r;
        let ghost flsv = fls@;
        r.put(ukey_of(ur.fy, &ur.name), Arc::new(UnitRec { filings: fls }));
        i += 1;
        proof {
            let ur_i = es[(i - 1) as int].1;
            assert(ur_i == arc_val(ur));
            assert(r.at(ukey(ur_i.fy as int, ov(ur_i.name))) is Some);
            assert forall|fy: int, name: Option<Seq<u8>>, a: Seq<u8>| um_has(r, fy, name, a) <==>
                (if exists|j: int| 0 <= j < i && (#[trigger] es[j]).1.fy as int == fy && ov(es[j].1.name) == name {
                    exists|k: int| 0 <= k < rs.len() && rs_unit(rs@, k, fy, name) && (#[trigger] rs@[k]).adsh@ == a } else { um_has(*um, fy, name, a) }) by {
                if fy == ur_i.fy as int && name == ov(ur_i.name) {
                    assert(ukey(fy, name) == ukey(ur_i.fy as int, ov(ur_i.name)));
                    assert(exists|j: int| 0 <= j < i && es[j].1.fy as int == fy && ov(es[j].1.name) == name) by { assert(es[(i - 1) as int].1.fy as int == fy); }
                    assert(um_has(r, fy, name, a) == bytes_has(flsv, a));
                }
                else {
                    assert(ukey(fy, name) != ukey(ur_i.fy as int, ov(ur_i.name))) by { if ukey(fy, name) == ukey(ur_i.fy as int, ov(ur_i.name)) { lemma_ukey_inj(fy, name, ur_i.fy as int, ov(ur_i.name)); } }
                    assert(r.at(ukey(fy, name)) == r0.at(ukey(fy, name)));
                    assert(um_has(r, fy, name, a) == um_has(r0, fy, name, a));
                    if exists|j: int| 0 <= j < i && es[j].1.fy as int == fy && ov(es[j].1.name) == name { let j = choose|j: int| 0 <= j < i && es[j].1.fy as int == fy && ov(es[j].1.name) == name; assert(j < i - 1); }
                }
            }
        }
    }
    proof {
        assert forall|fy: int, name: Option<Seq<u8>>| u_has(*u, fy, name) <==> exists|j: int| 0 <= j < es.len() && (#[trigger] es[j]).1.fy as int == fy && ov(es[j].1.name) == name by {
            if u_has(*u, fy, name) { let pos = choose|pos: int| #![auto] 0 <= pos < es.len() && es[pos].0 == ukey(fy, name); assert(u.at(es[pos].0) == Some(es[pos].1)); assert(es[pos].0 == ukey(es[pos].1.fy as int, ov(es[pos].1.name))); lemma_ukey_inj(fy, name, es[pos].1.fy as int, ov(es[pos].1.name)); }
            if exists|j: int| 0 <= j < es.len() && es[j].1.fy as int == fy && ov(es[j].1.name) == name { let j = choose|j: int| 0 <= j < es.len() && es[j].1.fy as int == fy && ov(es[j].1.name) == name; assert(u.at(es[j].0) == Some(es[j].1)); assert(es[j].0 == ukey(fy, name)); }
        }
    }
    r
}

} // verus!
