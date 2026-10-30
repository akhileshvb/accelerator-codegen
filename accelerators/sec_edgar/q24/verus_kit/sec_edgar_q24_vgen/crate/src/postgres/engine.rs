// The engine's float overflow rule.  Postgres's SUM(float8) raises on overflow: an answer exists
// only where float_safe, i.e. every non-empty group's sum of |x| passes float8_sum_safe.
// (engine.rs, the default, is DuckDB's: infinity, no error.)
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_prooflib::wide::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::emit::*;

verus! {

/// A group whose sum of |x| is `a` may be aggregated by the engine.
pub open spec fn grp_ok(a: int) -> bool {
    float8_sum_safe(a)
}

/// The query can be aggregated without an engine error.
pub open spec fn safe(db: Db, p: Params) -> bool {
    float_safe(db, p)
}

pub fn abs_ok(maxv: &Wide, a: &Wide) -> (b: bool)
    requires maxv@ == max_val(),
    ensures b == grp_ok(a@),
{
    let twice = a.add(a);
    !maxv.lt(&twice)
}

/// The query is safe exactly when every non-empty group of the class is.
pub proof fn lemma_safe_iff(db: Db, p: Params)
    ensures
        safe(db, p) <==> (forall|k: GK| #![trigger cntk(db, k)] k.y == p.p1 && k.uom == p.p2 && cntk(db, k) > 0 ==> grp_ok(absk(db, k))),
{
    if safe(db, p) {
        assert forall|k: GK| #![trigger cntk(db, k)] k.y == p.p1 && k.uom == p.p2 && cntk(db, k) > 0 implies grp_ok(absk(db, k)) by {
            lemma_aggs(db, p, k.tag, k.ver);
            assert(k == k_of(p, k.tag, k.ver));
            assert(group_count(db, p, k.tag, k.ver) > 0);
            assert(group_float_safe(db, p, k.tag, k.ver));
        }
    }
    if forall|k: GK| #![trigger cntk(db, k)] k.y == p.p1 && k.uom == p.p2 && cntk(db, k) > 0 ==> grp_ok(absk(db, k)) {
        assert forall|g_tag: Option<Seq<u8>>, g_ver: Option<Seq<u8>>| #[trigger] group_count(db, p, g_tag, g_ver) > 0 implies group_float_safe(db, p, g_tag, g_ver) by {
            lemma_aggs(db, p, g_tag, g_ver);
            let k = k_of(p, g_tag, g_ver);
            assert(cntk(db, k) > 0);
            assert(grp_ok(absk(db, k)));
        }
        assert(groups_float_safe(db, p));
    }
}

/// An answer exists only if the query is safe.
pub proof fn lemma_fits_safe(db: Db, p: Params)
    ensures answer_fits(db, p) ==> safe(db, p),
{
}

} // verus!
