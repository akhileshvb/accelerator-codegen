// A table of groups by (class, group key): the kept groups of the units being recomputed.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::grp::*;

verus! {

pub type GMap = Index<Index<Grp>>;

pub open spec fn gm_at(m: GMap, ok: KeyView, ik: KeyView) -> Option<Grp> {
    match m.at(ok) { Some(inner) => inner.at(ik), None => None }
}

/// A nested map whose inner maps are well formed (the keys are not tied to the values).
pub open spec fn nm_ok(m: GMap) -> bool {
    &&& m.wf()
    &&& forall|ok: KeyView| #![trigger m.at(ok)] m.at(ok) is Some ==> m.at(ok)->0.wf()
}

/// Stores g under (ok, ik).
pub fn nm_put(m: &mut GMap, ok: &Key, ik: Key, g: Arc<Grp>)
    requires nm_ok(*old(m)),
    ensures
        nm_ok(*final(m)),
        forall|o: KeyView, i: KeyView| #![trigger gm_at(*final(m), o, i)]
            gm_at(*final(m), o, i) == if o == ok.view() && i == ik.view() { Some(*g) } else { gm_at(*old(m), o, i) },
{
    let ghost old_m = *m;
    let mut inner = match m.get(ok) {
        Some(i) => { proof { assert(m.at(ok.view()) == Some(*i)); } i.share() }
        None => Index::new(),
    };
    let ghost inner0 = inner;
    proof {
        assert forall|q: KeyView| #![trigger inner0.at(q)] inner0.at(q) == gm_at(old_m, ok.view(), q) by {}
    }
    let ghost ikv = ik.view();
    inner.put(ik, g);
    let ghost inner1 = inner;
    m.put(ok.share(), Arc::new(inner));
    proof {
        assert forall|o: KeyView| #![trigger m.at(o)] m.at(o) is Some implies m.at(o)->0.wf() by {
            if o != ok.view() { assert(m.at(o) == old_m.at(o)); }
            else { assert(m.at(o) == Some(inner1)); }
        }
        assert forall|o: KeyView, i: KeyView| #![trigger gm_at(*m, o, i)]
            gm_at(*m, o, i) == if o == ok.view() && i == ikv { Some(*g) } else { gm_at(old_m, o, i) } by {
            if o == ok.view() { assert(m.at(o) == Some(inner1)); }
            else { assert(m.at(o) == old_m.at(o)); }
        }
    }
}

} // verus!
