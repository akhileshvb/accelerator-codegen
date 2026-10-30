// The engine's float-overflow condition on the answer.  DuckDB's float sums go to infinity: no answer is ruled out.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_prooflib::wide::Wide;
use crate::gen_spec::*;
use crate::numeric::*;
use crate::tot::*;

verus! {

/// What the answer's proof needs besides the rows (full_result's overflow part).
#[verifier::opaque]
pub open spec fn engine_safe(db: Db, p: Params) -> bool {
    true
}

/// The check on a class's totals (companies' magnitude, tuple count).
pub open spec fn tot_ok(ia: int, n: int) -> bool {
    true
}

pub fn tot_check(ia: &Wide, n: &Wide, c: &Consts) -> (r: bool)
    requires consts_ok(*c),
    ensures r == tot_ok(ia@, n@),
{
    true
}

pub proof fn lemma_tot_safe(db: Db, p: Params, ia: int, n: int)
    requires totals_bound(db, p, ia, n), tot_ok(ia, n),
    ensures engine_safe(db, p),
{
    reveal(engine_safe);
}

pub proof fn lemma_tot_unsafe(db: Db, p: Params, ia: int, n: int)
    requires totals_bound(db, p, ia, n), !tot_ok(ia, n),
    ensures !defined(db, p),
{
}

}
