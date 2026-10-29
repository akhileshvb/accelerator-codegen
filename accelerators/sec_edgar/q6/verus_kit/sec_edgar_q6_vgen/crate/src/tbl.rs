// The table of class records: loading a class (empty when new) and storing its updated record.
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

verus! {

pub open spec fn tbl_keyed(cls: Index<ClassRec>) -> bool {
    cls.wf() && forall|ok: KeyView| #![trigger cls.at(ok)] cls.at(ok) is Some ==> ok == cls_key(cls.at(ok)->0)
}

/// Every record of the table is well formed and exact for db (kept groups are in the class's order, not yet their number).
pub open spec fn tbl_wf(db: Db, cls: Index<ClassRec>) -> bool {
    &&& cls.wf()
    &&& forall|ok: KeyView| #![trigger cls.at(ok)] cls.at(ok) is Some ==> {
        let c = cls.at(ok)->0;
        &&& ok == cls_key(c)
        &&& crec_struct(c)
        &&& crec_sem(db, c)
    }
}

/// Every group of db that p holds for is accounted for.
pub open spec fn cov_where(db: Db, cls: Index<ClassRec>, p: spec_fn(GK) -> bool) -> bool {
    forall|k: GK| #![trigger cntk(db, k)] cntk(db, k) > 0 && p(k) ==> covered(db, cls, k)
}

pub open spec fn crec_is_empty(c: ClassRec) -> bool {
    &&& c.r.wf() && c.f.wf() && c.fb.wf() && c.hp.wf() && c.hn.wf()
    &&& c.r.entries().len() == 0 && c.f.entries().len() == 0 && c.hp.entries().len() == 0 && c.hn.entries().len() == 0
    &&& forall|q: KeyView| #![trigger c.r.at(q)] c.r.at(q) is None
    &&& forall|q: KeyView| #![trigger c.f.at(q)] c.f.at(q) is None
    &&& forall|q: KeyView| #![trigger c.fb.at(q)] c.fb.at(q) is None
    &&& forall|q: KeyView| #![trigger c.hp.at(q)] c.hp.at(q) is None
    &&& forall|q: KeyView| #![trigger c.hn.at(q)] c.hn.at(q) is None
}

pub proof fn lemma_empty_ok(db: Db, c: ClassRec)
    requires crec_is_empty(c),
    ensures crec_struct(c), crec_sem(db, c), crec_full(c),
{}

pub fn crec_empty(fy: i64, stmt: &[u8], uom: &[u8]) -> (c: ClassRec)
    ensures c.fy == fy, c.stmt@ == stmt@, c.uom@ == uom@, crec_is_empty(c),
{
    ClassRec { fy, stmt: copy_bytes(stmt), uom: copy_bytes(uom), r: Index::new(), f: Index::new(), fb: Index::new(), hp: Index::new(), hn: Index::new() }
}

/// The class's record, or an empty one.
pub fn cls_load(cls: &Index<ClassRec>, fy: i64, stmt: &[u8], uom: &[u8]) -> (r: (Arc<ClassRec>, bool))
    requires tbl_keyed(*cls),
    ensures
        r.0.fy == fy, r.0.stmt@ == stmt@, r.0.uom@ == uom@,
        r.1 ==> cls.at(okey(fy as int, stmt@, uom@)) == Some(*r.0),
        !r.1 ==> cls.at(okey(fy as int, stmt@, uom@)) is None && crec_is_empty(*r.0),
{
    let ok = okey_of(fy, stmt, uom);
    match cls.get(&ok) {
        Some(c) => {
            proof {
                assert(cls.at(ok.view()) == Some(*c));
                lemma_okey_inj(c.fy as int, c.stmt@, c.uom@, fy as int, stmt@, uom@);
            }
            (c, true)
        }
        None => (Arc::new(crec_empty(fy, stmt, uom)), false),
    }
}

/// The table with the class's record replaced.
pub fn cls_store(cls: &mut Index<ClassRec>, c: ClassRec)
    requires old(cls).wf(),
    ensures
        final(cls).wf(),
        forall|q: KeyView| #![trigger final(cls).at(q)] final(cls).at(q) == if q == cls_key(c) { Some(c) } else { old(cls).at(q) },
{
    let ok = okey_of(c.fy, c.stmt.as_slice(), c.uom.as_slice());
    cls.put(ok, Arc::new(c));
}

} // verus!
