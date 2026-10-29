// The state's groups: per (year, uom) an ordered map of that class's groups.
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
    pub tag: Option<Vec<u8>>,
    pub ver: Option<Vec<u8>>,
    pub cnt: Wide,
    pub sum: Wide,
}

impl Grp {
    pub open spec fn ik(self) -> KeyView {
        ikey(ov(self.tag), ov(self.ver))
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

pub fn gm_get(m: &GMap, y: i64, uom: &[u8], tag: &Option<Vec<u8>>, ver: &Option<Vec<u8>>) -> (r: Option<Arc<Grp>>)
    requires gm_ok(*m),
    ensures
        match r {
            Some(g) => gm_at(*m, okey(y as int, uom@), ikey(ov(*tag), ov(*ver))) == Some(*g),
            None => gm_at(*m, okey(y as int, uom@), ikey(ov(*tag), ov(*ver))) is None,
        },
{
    let ok = okey_of(y, uom);
    match m.get(&ok) {
        None => None,
        Some(inner) => {
            let ik = ikey_of(tag, ver);
            proof { assert(m.at(ok.view()) == Some(*inner)); }
            inner.get(&ik)
        }
    }
}

/// Stores g in its triple's map (replacing the group with the same key).
pub fn gm_set(m: &mut GMap, y: i64, uom: &[u8], g: Grp)
    requires gm_ok(*old(m)),
    ensures
        gm_ok(*final(m)),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(m), ok, ik)]
            gm_at(*final(m), ok, ik) == if ok == okey(y as int, uom@) && ik == g.ik() { Some(g) } else { gm_at(*old(m), ok, ik) },
{
    let ghost old_m = *m;
    let ok = okey_of(y, uom);
    let ik = ikey_of(&g.tag, &g.ver);
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
pub fn gm_del(m: &mut GMap, y: i64, uom: &[u8], tag: &Option<Vec<u8>>, ver: &Option<Vec<u8>>)
    requires gm_ok(*old(m)),
    ensures
        gm_ok(*final(m)),
        forall|ok: KeyView, ik: KeyView| #![trigger gm_at(*final(m), ok, ik)]
            gm_at(*final(m), ok, ik) == if ok == okey(y as int, uom@) && ik == ikey(ov(*tag), ov(*ver)) { None } else { gm_at(*old(m), ok, ik) },
{
    let ghost old_m = *m;
    let ok = okey_of(y, uom);
    let ik = ikey_of(tag, ver);
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

} // verus!
