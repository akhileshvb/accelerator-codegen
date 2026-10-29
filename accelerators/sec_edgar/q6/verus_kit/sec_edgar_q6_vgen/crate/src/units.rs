// Units (fy, name): which units a filing has sub rows in, and which filings a unit has.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::esc::*;
use crate::accmap::*;
use crate::inv::*;
use crate::crops::*;
use crate::grp::*;

verus! {

pub struct UnitRef {
    pub fy: i64,
    pub name: Option<Vec<u8>>,
}

pub struct ClassRef {
    pub fy: i64,
    pub stmt: Vec<u8>,
    pub uom: Vec<u8>,
}

/// A unit's filings (adsh).
pub struct UnitRec {
    pub filings: Vec<Vec<u8>>,
}

pub open spec fn ukey(fy: int, name: Option<Seq<u8>>) -> KeyView {
    (fy, esc_opt(name))
}

pub open spec fn akey(a: Seq<u8>) -> KeyView {
    (0int, a)
}

pub fn ukey_of(fy: i64, name: &Option<Vec<u8>>) -> (k: Key)
    ensures k.view() == ukey(fy as int, ov(*name)),
{
    let mut text = Vec::new();
    append_esc_opt(&mut text, name);
    Key::composite(fy, text)
}

pub fn akey_of(a: &[u8]) -> (k: Key)
    ensures k.view() == akey(a@),
{
    Key::composite(0, copy_bytes(a))
}

pub open spec fn uref_has(l: Seq<UnitRef>, fy: int, name: Option<Seq<u8>>) -> bool {
    exists|i: int| 0 <= i < l.len() && (#[trigger] l[i]).fy as int == fy && ov(l[i].name) == name
}

pub open spec fn bytes_has(l: Seq<Vec<u8>>, a: Seq<u8>) -> bool {
    exists|i: int| 0 <= i < l.len() && (#[trigger] l[i])@ == a
}

/// Filing a has a sub row of unit (fy, name).
pub open spec fn sub_has(db: Db, a: Seq<u8>, fy: int, name: Option<Seq<u8>>) -> bool {
    exists|i: int| 0 <= i < db.sub.len() && (#[trigger] db.sub[i]).adsh == a && db.sub[i].fy == Some(fy) && db.sub[i].name == name
}

pub open spec fn fm_has(fm: Index<Vec<UnitRef>>, a: Seq<u8>, fy: int, name: Option<Seq<u8>>) -> bool {
    match fm.at(akey(a)) { Some(l) => uref_has(l@, fy, name), None => false }
}

pub open spec fn um_has(um: Index<UnitRec>, fy: int, name: Option<Seq<u8>>, a: Seq<u8>) -> bool {
    match um.at(ukey(fy, name)) { Some(u) => bytes_has(u.filings@, a), None => false }
}

pub open spec fn fm_ok(fm: Index<Vec<UnitRef>>, db: Db) -> bool {
    &&& fm.wf()
    &&& forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| #![trigger fm_has(fm, a, fy, name)] fm_has(fm, a, fy, name) <==> sub_has(db, a, fy, name)
}

pub open spec fn um_ok(um: Index<UnitRec>, db: Db) -> bool {
    &&& um.wf()
    &&& forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| #![trigger um_has(um, fy, name, a)] um_has(um, fy, name, a) <==> sub_has(db, a, fy, name)
}

/// A set of units: keyed by their (fy, name), holding the unit.
pub open spec fn uset_ok(u: Index<UnitRef>) -> bool {
    u.wf() && forall|q: KeyView| #![trigger u.at(q)] u.at(q) is Some ==> q == ukey(u.at(q)->0.fy as int, ov(u.at(q)->0.name))
}

pub open spec fn u_has(u: Index<UnitRef>, fy: int, name: Option<Seq<u8>>) -> bool {
    u.at(ukey(fy, name)) is Some
}

pub fn copy_name(o: &Option<Vec<u8>>) -> (r: Option<Vec<u8>>)
    ensures ov(r) == ov(*o),
{
    copy_opt(o)
}

pub fn uset_add(u: &mut Index<UnitRef>, fy: i64, name: &Option<Vec<u8>>)
    requires uset_ok(*old(u)),
    ensures
        uset_ok(*final(u)),
        forall|f2: int, n2: Option<Seq<u8>>| #![trigger u_has(*final(u), f2, n2)] u_has(*final(u), f2, n2) <==> (u_has(*old(u), f2, n2) || (f2 == fy as int && n2 == ov(*name))),
{
    let k = ukey_of(fy, name);
    let ur = UnitRef { fy, name: copy_name(name) };
    u.put(k, Arc::new(ur));
    proof {
        assert forall|f2: int, n2: Option<Seq<u8>>| u_has(*u, f2, n2) <==> (u_has(*old(u), f2, n2) || (f2 == fy as int && n2 == ov(*name))) by {
            if ukey(f2, n2) == ukey(fy as int, ov(*name)) { lemma_ukey_inj(f2, n2, fy as int, ov(*name)); }
        }
    }
}

pub proof fn lemma_ukey_inj(f1: int, n1: Option<Seq<u8>>, f2: int, n2: Option<Seq<u8>>)
    requires ukey(f1, n1) == ukey(f2, n2),
    ensures f1 == f2, n1 == n2,
{
    lemma_esc_opt_split(n1, Seq::empty(), n2, Seq::empty());
}

} // verus!
