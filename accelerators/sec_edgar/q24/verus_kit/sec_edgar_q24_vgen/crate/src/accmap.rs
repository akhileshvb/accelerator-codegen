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
    pub y: i64,
    pub uom: Vec<u8>,
    pub tag: Option<Vec<u8>>,
    pub ver: Option<Vec<u8>>,
    pub cnt: Wide,
    pub sum: Wide,
}

impl AccEnt {
    pub open spec fn gk(self) -> GK {
        GK { y: self.y as int, uom: self.uom@, tag: ov(self.tag), ver: ov(self.ver) }
    }
}

pub open spec fn fkey(k: GK) -> KeyView {
    (k.y, esc(k.uom) + ikey(k.tag, k.ver).1)
}

pub proof fn lemma_fkey_inj(k1: GK, k2: GK)
    requires fkey(k1) == fkey(k2),
    ensures k1 == k2,
{
    lemma_esc_split(k1.uom, ikey(k1.tag, k1.ver).1, k2.uom, ikey(k2.tag, k2.ver).1);
    lemma_ikey_inj(k1.tag, k1.ver, k2.tag, k2.ver);
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

pub fn fkey_of(y: i64, uom: &[u8], tag: &Option<Vec<u8>>, ver: &Option<Vec<u8>>) -> (k: Key)
    ensures k.view() == fkey(GK { y: y as int, uom: uom@, tag: ov(*tag), ver: ov(*ver) }),
{
    let mut text = Vec::new();
    append_esc(&mut text, uom);
    append_esc_opt(&mut text, tag);
    append_esc_opt(&mut text, ver);
    proof {
        let k = GK { y: y as int, uom: uom@, tag: ov(*tag), ver: ov(*ver) };
        assert(text@ =~= esc(k.uom) + ikey(k.tag, k.ver).1);
    }
    Key::composite(y, text)
}

/// Adds (dc, ds) to the group's entry.
pub fn acc_add(acc: &mut Index<AccEnt>, y: i64, uom: &[u8], tag: &Option<Vec<u8>>, ver: &Option<Vec<u8>>, dc: &Wide, ds: &Wide)
    requires acc_ok(*old(acc)),
    ensures
        acc_ok(*final(acc)),
        ({
            let k = GK { y: y as int, uom: uom@, tag: ov(*tag), ver: ov(*ver) };
            &&& forall|k2: GK| #![trigger acc_c(*final(acc), k2)]
                acc_c(*final(acc), k2) == acc_c(*old(acc), k2) + if k2 == k { dc@ } else { 0 }
            &&& forall|k2: GK| #![trigger acc_s(*final(acc), k2)]
                acc_s(*final(acc), k2) == acc_s(*old(acc), k2) + if k2 == k { ds@ } else { 0 }
        }),
{
    let ghost k = GK { y: y as int, uom: uom@, tag: ov(*tag), ver: ov(*ver) };
    let key = fkey_of(y, uom, tag, ver);
    let ghost old_acc = *acc;
    let (cnt, sum) = match acc.get(&key) {
        Some(e) => (e.cnt.add(dc), e.sum.add(ds)),
        None => (dc.dup(), ds.dup()),
    };
    proof {
        assert(cnt@ == acc_c(old_acc, k) + dc@ && sum@ == acc_s(old_acc, k) + ds@);
    }
    let ghost (cv, sv) = (cnt@, sum@);
    let ent = AccEnt { y, uom: copy_bytes(uom), tag: copy_opt(tag), ver: copy_opt(ver), cnt, sum };
    proof { assert(ent.gk() == k); }
    let ghost kv = key.view();
    acc.put(key, Arc::new(ent));
    proof {
        assert(kv == fkey(k));
        assert(acc.at(fkey(k)) is Some);
        assert(acc.at(fkey(k))->0.cnt@ == cv && acc.at(fkey(k))->0.sum@ == sv);
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
    }
}

} // verus!
