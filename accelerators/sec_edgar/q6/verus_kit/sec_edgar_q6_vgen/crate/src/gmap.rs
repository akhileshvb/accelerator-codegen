// The state's groups: per (fy, stmt, uom) an ordered map of that triple's groups.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::*;
use crate::esc::*;
use crate::accmap::*;
use crate::model::*;

verus! {

pub struct Grp {
    pub name: Option<Vec<u8>>,
    pub tag: Vec<u8>,
    pub plabel: Option<Vec<u8>>,
    pub cnt: Wide,
    pub sum: Wide,
    /// the double the answer reports for this group; meaningful when the state is ranked
    pub bits: u64,
}

impl Grp {
    pub open spec fn ik(self) -> KeyView {
        ikey(ov(self.name), self.tag@, ov(self.plabel))
    }
}

pub type GMap = Index<Index<Grp>>;

pub open spec fn gm_at(m: GMap, ok: KeyView, ik: KeyView) -> Option<Grp> {
    match m.at(ok) { Some(inner) => inner.at(ik), None => None }
}

pub open spec fn gm_ok(m: GMap) -> bool {
    &&& m.wf()
    &&& forall|ok: KeyView| #![trigger m.at(ok)] m.at(ok) is Some ==> {
        let inner = m.at(ok)->0;
        &&& inner.wf()
        &&& forall|ik: KeyView| #![trigger inner.at(ik)] inner.at(ik) is Some ==> ik == inner.at(ik)->0.ik()
    }
}

pub fn gm_get(m: &GMap, fy: i64, stmt: &[u8], uom: &[u8], name: &Option<Vec<u8>>, tag: &[u8], plabel: &Option<Vec<u8>>) -> (r: Option<Arc<Grp>>)
    requires gm_ok(*m),
    ensures
        match r {
            Some(g) => gm_at(*m, okey(fy as int, stmt@, uom@), ikey(ov(*name), tag@, ov(*plabel))) == Some(*g),
            None => gm_at(*m, okey(fy as int, stmt@, uom@), ikey(ov(*name), tag@, ov(*plabel))) is None,
        },
{
    let ok = okey_of(fy, stmt, uom);
    match m.get(&ok) {
        None => None,
        Some(inner) => {
            let ik = ikey_of(name, tag, plabel);
            proof { assert(m.at(ok.view()) == Some(*inner)); }
            inner.get(&ik)
        }
    }
}

/// Stores g in its triple's map (replacing the group with the same key).
pub fn gm_set(m: &mut GMap, fy: i64, stmt: &[u8], uom: &[u8], g: Grp)
    requires gm_ok(*old(m)),
    ensures
        gm_ok(*final(m)),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(m), ok, ik)]
            gm_at(*final(m), ok, ik) == if ok == okey(fy as int, stmt@, uom@) && ik == g.ik() { Some(g) } else { gm_at(*old(m), ok, ik) },
{
    let ghost old_m = *m;
    let ok = okey_of(fy, stmt, uom);
    let ik = ikey_of(&g.name, g.tag.as_slice(), &g.plabel);
    let mut inner = match m.get(&ok) {
        Some(i) => { proof { assert(m.at(ok.view()) == Some(*i)); } i.share() }
        None => Index::new(),
    };
    let ghost inner0 = inner;
    proof {
        assert(inner0.wf());
        assert forall|q: KeyView| #![trigger inner0.at(q)] inner0.at(q) is Some implies q == inner0.at(q)->0.ik() by {
            if m.at(ok.view()) is Some { assert(old_m.at(ok.view())->0.at(q) == inner0.at(q)); }
        }
    }
    let ghost gk = g.ik();
    inner.put(ik, Arc::new(g));
    let ghost ov_ = ok.view();
    let ghost inner1 = inner;
    proof {
        assert forall|q: KeyView| #![trigger inner1.at(q)] inner1.at(q) is Some implies q == inner1.at(q)->0.ik() by {
            if q != ik.view() { assert(inner1.at(q) == inner0.at(q)); }
        }
    }
    m.put(ok, Arc::new(inner));
    proof {
        assert forall|ok2: KeyView| #![trigger m.at(ok2)] m.at(ok2) is Some implies ({
            let inn = m.at(ok2)->0;
            &&& inn.wf()
            &&& forall|ik2: KeyView| #![trigger inn.at(ik2)] inn.at(ik2) is Some ==> ik2 == inn.at(ik2)->0.ik()
        }) by {
            if ok2 != ov_ { assert(m.at(ok2) == old_m.at(ok2)); }
            else { assert(m.at(ok2) == Some(inner1)); }
        }
    }
}

/// Removes the group with this key.
pub fn gm_del(m: &mut GMap, fy: i64, stmt: &[u8], uom: &[u8], name: &Option<Vec<u8>>, tag: &[u8], plabel: &Option<Vec<u8>>)
    requires gm_ok(*old(m)),
    ensures
        gm_ok(*final(m)),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(m), ok, ik)]
            gm_at(*final(m), ok, ik) == if ok == okey(fy as int, stmt@, uom@) && ik == ikey(ov(*name), tag@, ov(*plabel)) { None } else { gm_at(*old(m), ok, ik) },
{
    let ghost old_m = *m;
    let ok = okey_of(fy, stmt, uom);
    let ik = ikey_of(name, tag, plabel);
    let mut inner = match m.get(&ok) {
        Some(i) => { proof { assert(m.at(ok.view()) == Some(*i)); } i.share() }
        None => return,
    };
    let ghost inner0 = inner;
    proof {
        assert forall|q: KeyView| #![trigger inner0.at(q)] inner0.at(q) is Some implies q == inner0.at(q)->0.ik() by {
            assert(old_m.at(ok.view())->0.at(q) == inner0.at(q));
        }
    }
    inner.remove(&ik);
    let ghost ov_ = ok.view();
    let ghost inner1 = inner;
    proof {
        assert forall|q: KeyView| #![trigger inner1.at(q)] inner1.at(q) is Some implies q == inner1.at(q)->0.ik() by {
            assert(inner1.at(q) == if q == ik.view() { None } else { inner0.at(q) });
        }
    }
    m.put(ok, Arc::new(inner));
    proof {
        assert forall|ok2: KeyView| #![trigger m.at(ok2)] m.at(ok2) is Some implies ({
            let inn = m.at(ok2)->0;
            &&& inn.wf()
            &&& forall|ik2: KeyView| #![trigger inn.at(ik2)] inn.at(ik2) is Some ==> ik2 == inn.at(ik2)->0.ik()
        }) by {
            if ok2 != ov_ { assert(m.at(ok2) == old_m.at(ok2)); }
            else { assert(m.at(ok2) == Some(inner1)); }
        }
    }
}


/// A nested map whose inner maps are well formed (the keys are not tied to the values).
pub open spec fn nm_ok(m: GMap) -> bool {
    &&& m.wf()
    &&& forall|ok: KeyView| #![trigger m.at(ok)] m.at(ok) is Some ==> m.at(ok)->0.wf()
}

pub proof fn lemma_gm_ik(m: GMap, ok: KeyView, ik: KeyView)
    requires gm_ok(m), gm_at(m, ok, ik) is Some,
    ensures ik == gm_at(m, ok, ik)->0.ik(),
{
    assert(m.at(ok) is Some);
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

/// Removes the entry under (ok, ik).
pub fn nm_del(m: &mut GMap, ok: &Key, ik: &Key)
    requires nm_ok(*old(m)),
    ensures
        nm_ok(*final(m)),
        forall|o: KeyView, i: KeyView| #![trigger gm_at(*final(m), o, i)]
            gm_at(*final(m), o, i) == if o == ok.view() && i == ik.view() { None } else { gm_at(*old(m), o, i) },
{
    let ghost old_m = *m;
    let mut inner = match m.get(ok) {
        Some(i) => { proof { assert(m.at(ok.view()) == Some(*i)); } i.share() }
        None => return,
    };
    let ghost inner0 = inner;
    proof {
        assert forall|q: KeyView| #![trigger inner0.at(q)] inner0.at(q) == gm_at(old_m, ok.view(), q) by {}
    }
    inner.remove(ik);
    let ghost inner1 = inner;
    m.put(ok.share(), Arc::new(inner));
    proof {
        assert forall|o: KeyView| #![trigger m.at(o)] m.at(o) is Some implies m.at(o)->0.wf() by {
            if o != ok.view() { assert(m.at(o) == old_m.at(o)); }
            else { assert(m.at(o) == Some(inner1)); }
        }
        assert forall|o: KeyView, i: KeyView| #![trigger gm_at(*m, o, i)]
            gm_at(*m, o, i) == if o == ok.view() && i == ik.view() { None } else { gm_at(old_m, o, i) } by {
            if o == ok.view() { assert(m.at(o) == Some(inner1)); }
            else { assert(m.at(o) == old_m.at(o)); }
        }
    }
}

} // verus!
