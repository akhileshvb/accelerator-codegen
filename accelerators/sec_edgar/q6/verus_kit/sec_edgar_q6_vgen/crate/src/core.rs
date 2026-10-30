// One update of the class table: the recomputed units' groups leave, come back from the contributions, and the classes are trimmed.
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
use crate::tbl::*;
use crate::units::*;
use crate::kept::*;
use crate::plan::*;
use crate::evict::*;
use crate::place::*;
use crate::trim::*;
use crate::bridge::*;
use crate::wideval::*;
use crate::rep::*;

verus! {

/// The plan of an update: which units are recomputed and in which classes.
pub open spec fn plan_ok(cls0: Index<ClassRec>, u: Index<UnitRef>, ct: Index<ClassRef>, w: Index<UnitRef>, cw: Index<ClassRef>) -> bool {
    &&& tbl_str(cls0) && uset_ok(u) && uset_ok(w) && subset_u(u, w) && cset_ok(ct) && cset_ok(cw)
    &&& class_cover_u(cls0, w, cw) && class_cover_u(cls0, u, ct) && refill_ok(cls0, ct, u, w)
}

#[verifier::rlimit(300)]
#[verifier::spinoff_prover]
/// tc: the classes the placed groups can enlarge (trimmed afterwards).
pub fn core_update(cls0: &Index<ClassRec>, u: &Index<UnitRef>, ct: &Index<ClassRef>, w: &Index<UnitRef>, cw: &Index<ClassRef>, tc: &Index<ClassRef>,
    acc: &Index<AccEnt>, tab: &Pow2Tab, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (t: Index<ClassRec>)
    requires
        cls_ok(db, *cls0), plan_ok(*cls0, *u, *ct, *w, *cw), cset_ok(*tc), frame_ok(db, db2, *u),
        tab.ok(), rep_db(db2), acc_ok(*acc),
        forall|k: GK| #![trigger acc_c(*acc, k)] u_has(*w, k.fy, k.name) ==> acc_c(*acc, k) == cntk(db2, k) && acc_s(*acc, k) == sumk(db2, k) && acc_a(*acc, k) == absk(db2, k),
    ensures cls_ok(db2, t),
{
    let (mut t, kept) = evict_all(cls0, cw, ct, u, w);
    proof { lemma_bridge(db, db2, *cls0, t, kept, *u, *w, *ct, *cw); }
    place_all(&mut t, acc, &kept, u, w, tab, Ghost(db2));
    let ghost done = done_of(acc.entries(), acc.entries().len() as int, *w);
    proof { assert(tbl_str(t)) by { assert forall|ok: KeyView| t.at(ok) is Some implies ok == cls_key(t.at(ok)->0) && crec_struct(t.at(ok)->0) by {} } }
    trim_all(&mut t, tc, Ghost(db2), Ghost(mid_cov(*w, done)));
    proof {
        assert forall|k: GK| cntk(db2, k) > 0 implies #[trigger] covered(db2, t, k) by {
            if !u_has(*w, k.fy, k.name) {} else { assert(done(k)); }
        }
        assert(cls_ok(db2, t));
    }
    t
}

} // verus!
