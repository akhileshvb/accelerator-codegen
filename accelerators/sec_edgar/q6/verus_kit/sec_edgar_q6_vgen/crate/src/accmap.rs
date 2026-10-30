// The contributions of one transaction per group, as a flat ordered map.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::*;
use crate::esc::*;
use crate::model::*;

verus! {

pub open spec fn ov(o: Option<Vec<u8>>) -> Option<Seq<u8>> {
    match o { Some(v) => Some(v@), None => None }
}

pub struct AccEnt {
    pub fy: i64,
    pub stmt: Vec<u8>,
    pub uom: Vec<u8>,
    pub name: Option<Vec<u8>>,
    pub tag: Vec<u8>,
    pub plabel: Option<Vec<u8>>,
    pub cnt: Wide,
    pub sum: Wide,
    pub abs: Wide,
}

impl AccEnt {
    pub open spec fn gk(self) -> GK {
        GK { fy: self.fy as int, stmt: self.stmt@, uom: self.uom@, name: ov(self.name), tag: self.tag@, plabel: ov(self.plabel) }
    }
}

pub open spec fn fkey(k: GK) -> KeyView {
    (k.fy, esc(k.stmt) + esc(k.uom) + ikey(k.name, k.tag, k.plabel).1)
}

pub proof fn lemma_fkey_inj(k1: GK, k2: GK)
    requires fkey(k1) == fkey(k2),
    ensures k1 == k2,
{
    let (t1, t2) = (ikey(k1.name, k1.tag, k1.plabel).1, ikey(k2.name, k2.tag, k2.plabel).1);
    assert(esc(k1.stmt) + esc(k1.uom) + t1 =~= esc(k1.stmt) + (esc(k1.uom) + t1));
    assert(esc(k2.stmt) + esc(k2.uom) + t2 =~= esc(k2.stmt) + (esc(k2.uom) + t2));
    lemma_esc_split(k1.stmt, esc(k1.uom) + t1, k2.stmt, esc(k2.uom) + t2);
    lemma_esc_split(k1.uom, ikey(k1.name, k1.tag, k1.plabel).1, k2.uom, ikey(k2.name, k2.tag, k2.plabel).1);
    lemma_ikey_inj(k1.name, k1.tag, k1.plabel, k2.name, k2.tag, k2.plabel);
}

pub open spec fn acc_ok(a: Index<AccEnt>) -> bool {
    a.wf() && forall|kv: KeyView| #![trigger a.at(kv)] a.at(kv) is Some ==> kv == fkey(a.at(kv)->0.gk())
}

pub open spec fn acc_c(a: Index<AccEnt>, k: GK) -> int {
    match a.at(fkey(k)) { Some(e) => e.cnt@, None => 0 }
}

pub open spec fn acc_s(a: Index<AccEnt>, k: GK) -> int {
    match a.at(fkey(k)) { Some(e) => e.sum@, None => 0 }
}

pub open spec fn acc_a(a: Index<AccEnt>, k: GK) -> int {
    match a.at(fkey(k)) { Some(e) => e.abs@, None => 0 }
}

/// The sum stream (sel false) or the sum of absolute values (sel true).
pub open spec fn acc_v(a: Index<AccEnt>, k: GK, sel: bool) -> int {
    if sel { acc_a(a, k) } else { acc_s(a, k) }
}

pub fn copy_bytes(b: &[u8]) -> (r: Vec<u8>)
    ensures r@ == b@,
{
    let mut r = Vec::new();
    let mut i = 0;
    while i < b.len()
        invariant i <= b.len(), r@ == b@.take(i as int),
        decreases b.len() - i,
    {
        r.push(b[i]);
        proof { assert(b@.take(i as int + 1) =~= b@.take(i as int).push(b[i as int])); }
        i += 1;
    }
    proof { assert(b@.take(i as int) =~= b@); }
    r
}

pub fn copy_opt(o: &Option<Vec<u8>>) -> (r: Option<Vec<u8>>)
    ensures ov(r) == ov(*o),
{
    match o { Some(v) => Some(copy_bytes(v.as_slice())), None => None }
}

pub fn fkey_of(fy: i64, stmt: &[u8], uom: &[u8], name: &Option<Vec<u8>>, tag: &[u8], plabel: &Option<Vec<u8>>) -> (k: Key)
    ensures k.view() == fkey(GK { fy: fy as int, stmt: stmt@, uom: uom@, name: ov(*name), tag: tag@, plabel: ov(*plabel) }),
{
    let mut text = Vec::new();
    append_esc(&mut text, stmt);
    append_esc(&mut text, uom);
    append_esc_opt(&mut text, name);
    append_esc(&mut text, tag);
    append_esc_opt(&mut text, plabel);
    proof {
        let k = GK { fy: fy as int, stmt: stmt@, uom: uom@, name: ov(*name), tag: tag@, plabel: ov(*plabel) };
        assert(text@ =~= esc(k.stmt) + esc(k.uom) + ikey(k.name, k.tag, k.plabel).1);
    }
    Key::composite(fy, text)
}

/// Adds (dc, ds) to the group's entry.
pub fn acc_add(acc: &mut Index<AccEnt>, fy: i64, stmt: &[u8], uom: &[u8], name: &Option<Vec<u8>>, tag: &[u8], plabel: &Option<Vec<u8>>, dc: &Wide, ds: &Wide, da: &Wide)
    requires acc_ok(*old(acc)),
    ensures
        acc_ok(*final(acc)),
        ({
            let k = GK { fy: fy as int, stmt: stmt@, uom: uom@, name: ov(*name), tag: tag@, plabel: ov(*plabel) };
            &&& forall|k2: GK| #![trigger acc_c(*final(acc), k2)]
                acc_c(*final(acc), k2) == acc_c(*old(acc), k2) + if k2 == k { dc@ } else { 0 }
            &&& forall|k2: GK| #![trigger acc_s(*final(acc), k2)]
                acc_s(*final(acc), k2) == acc_s(*old(acc), k2) + if k2 == k { ds@ } else { 0 }
            &&& forall|k2: GK| #![trigger acc_a(*final(acc), k2)]
                acc_a(*final(acc), k2) == acc_a(*old(acc), k2) + if k2 == k { da@ } else { 0 }
        }),
{
    let ghost k = GK { fy: fy as int, stmt: stmt@, uom: uom@, name: ov(*name), tag: tag@, plabel: ov(*plabel) };
    let key = fkey_of(fy, stmt, uom, name, tag, plabel);
    let ghost old_acc = *acc;
    let (cnt, sum, abs) = match acc.get(&key) {
        Some(e) => (e.cnt.add(dc), e.sum.add(ds), e.abs.add(da)),
        None => (dc.dup(), ds.dup(), da.dup()),
    };
    proof {
        assert(cnt@ == acc_c(old_acc, k) + dc@ && sum@ == acc_s(old_acc, k) + ds@ && abs@ == acc_a(old_acc, k) + da@);
    }
    let ghost (cv, sv, av) = (cnt@, sum@, abs@);
    let ent = AccEnt { fy, stmt: copy_bytes(stmt), uom: copy_bytes(uom), name: copy_opt(name), tag: copy_bytes(tag), plabel: copy_opt(plabel), cnt, sum, abs };
    proof { assert(ent.gk() == k); }
    let ghost kv = key.view();
    acc.put(key, Arc::new(ent));
    proof {
        assert(kv == fkey(k));
        assert(acc.at(fkey(k)) is Some);
        assert(acc.at(fkey(k))->0.cnt@ == cv && acc.at(fkey(k))->0.sum@ == sv && acc.at(fkey(k))->0.abs@ == av);
        assert forall|kv2: KeyView| #![trigger acc.at(kv2)] acc.at(kv2) is Some implies kv2 == fkey(acc.at(kv2)->0.gk()) by {
            if kv2 != fkey(k) { assert(acc.at(kv2) == old_acc.at(kv2)); }
            else { assert(acc.at(kv2)->0.gk() == k); }
        }
        assert forall|k2: GK| #![trigger acc_c(*acc, k2)]
            acc_c(*acc, k2) == acc_c(old_acc, k2) + if k2 == k { dc@ } else { 0 } by {
            if k2 != k {
                if fkey(k2) == fkey(k) { lemma_fkey_inj(k2, k); }
                assert(acc.at(fkey(k2)) == old_acc.at(fkey(k2)));
            }
        }
        assert forall|k2: GK| #![trigger acc_s(*acc, k2)]
            acc_s(*acc, k2) == acc_s(old_acc, k2) + if k2 == k { ds@ } else { 0 } by {
            if k2 != k {
                if fkey(k2) == fkey(k) { lemma_fkey_inj(k2, k); }
                assert(acc.at(fkey(k2)) == old_acc.at(fkey(k2)));
            }
        }
        assert forall|k2: GK| #![trigger acc_a(*acc, k2)]
            acc_a(*acc, k2) == acc_a(old_acc, k2) + if k2 == k { da@ } else { 0 } by {
            if k2 != k {
                if fkey(k2) == fkey(k) { lemma_fkey_inj(k2, k); }
                assert(acc.at(fkey(k2)) == old_acc.at(fkey(k2)));
            }
        }
    }
}

} // verus!
