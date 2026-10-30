// The engine's float overflow rule.  DuckDB's SUM saturates to infinity and never errors: every
// check is trivially true.  (postgres/engine.rs, selected by lib_postgres.rs, states Postgres's.)
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_prooflib::wide::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::emit::*;

verus! {

/// A group whose sum of |x| is `a` may be aggregated by the engine.
pub open spec fn grp_ok(a: int) -> bool {
    true
}

/// The query can be aggregated without an engine error.
pub open spec fn safe(db: Db, p: Params) -> bool {
    true
}

pub fn abs_ok(maxv: &Wide, a: &Wide) -> (b: bool)
    requires maxv@ == max_val(),
    ensures b == grp_ok(a@),
{
    true
}

/// The query is safe exactly when every non-empty group of the class is.
pub proof fn lemma_safe_iff(db: Db, p: Params)
    ensures
        safe(db, p) <==> (forall|k: GK| #![trigger cntk(db, k)] k.y == p.p1 && k.uom == p.p2 && cntk(db, k) > 0 ==> grp_ok(absk(db, k))),
{
}

/// An answer exists only if the query is safe.
pub proof fn lemma_fits_safe(db: Db, p: Params)
    ensures answer_fits(db, p) ==> safe(db, p),
{
}

} // verus!
