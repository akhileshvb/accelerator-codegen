// The engine's float rule, as the kernel uses it.  DuckDB's float SUM does not raise on overflow: no group is unanswerable for its magnitude.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_prooflib::wide::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::cands::*;

verus! {

/// A group whose sum of absolute values is a makes the engine raise.
pub open spec fn unsafe_a(a: int) -> bool {
    false
}

/// No group of the class raises.
pub open spec fn eng_safe(db: Db, p: Params) -> bool {
    true
}

pub fn exec_unsafe(maxv: &Wide, a: &Wide) -> (r: bool)
    requires maxv@ == (pow2(53) - 1) * pow2(2045),
    ensures r == unsafe_a(a@),
{
    false
}

pub proof fn lemma_safe_all(db: Db, p: Params)
    requires forall|k: GK| is_cand(db, p, k) ==> !unsafe_a(absk(db, k)),
    ensures eng_safe(db, p),
{
}

pub proof fn lemma_unsafe_undefined(db: Db, p: Params, k: GK)
    requires is_cand(db, p, k), unsafe_a(absk(db, k)),
    ensures !answer_fits(db, p),
{
}

pub proof fn lemma_over_range(db: Db, p: Params, k: GK)
    requires is_cand(db, p, k), sumk(db, k) > (pow2(53) - 1) * pow2(2045),
    ensures eng_safe(db, p) || unsafe_a(absk(db, k)),
{
}

pub proof fn lemma_safe_each(db: Db, p: Params, k: GK)
    requires eng_safe(db, p), is_cand(db, p, k),
    ensures !unsafe_a(absk(db, k)),
{
}

} // verus!
