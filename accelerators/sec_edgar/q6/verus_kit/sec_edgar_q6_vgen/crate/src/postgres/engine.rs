// The engine's float rule, as the kernel uses it.  Postgres float8 SUM raises on overflow: float_safe bounds every group's sum of absolute values.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::wide::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::answer::*;
use crate::cands::*;
use crate::grp::*;

verus! {

/// A group whose sum of absolute values is a makes the engine raise.
pub open spec fn unsafe_a(a: int) -> bool {
    !float8_sum_safe(a)
}

/// No group of the class raises.
pub open spec fn eng_safe(db: Db, p: Params) -> bool {
    float_safe(db, p)
}

pub fn exec_unsafe(maxv: &Wide, a: &Wide) -> (r: bool)
    requires maxv@ == (pow2(53) - 1) * pow2(2045),
    ensures r == unsafe_a(a@),
{
    let twice = a.add(a);
    maxv.lt(&twice)
}

proof fn lemma_count_pos(db: Db, p: Params, k: GK)
    requires is_cand(db, p, k),
    ensures group_count(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel) > 0,
{
    let one = |t: Tuple| 1int;
    lemma_agg(db, p, k, one, icnt(k));
    assert(group_count(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel) == group_sum(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel, one)) by {
        reveal(group_sum);
        reveal(group_count);
        let (gn, gs_, gt, gp) = (k.name, Some(p.p2), Some(k.tag), k.plabel);
        let a = |t: Tuple| if t.s_.name == gn && t.p.stmt == gs_ && t.n.tag == gt && t.p.plabel == gp { 1int } else { 0 };
        let b = |t: Tuple| if t.s_.name == gn && t.p.stmt == gs_ && t.n.tag == gt && t.p.plabel == gp { one(t) } else { 0 };
        assert forall|t: Tuple| #[trigger] a(t) == b(t) by {}
        lemma_sum_ext_all(tuples(db, p), a, b);
    }
}

/// The class's group k, as the spec's group, has absolute sum absk.
proof fn lemma_group_abs(db: Db, p: Params, k: GK)
    requires k_params(k) == p,
    ensures
        group_float_safe(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel) <==> float8_sum_safe(absk(db, k)),
{
    let va = |t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0 };
    lemma_agg(db, p, k, va, iabs(k));
}

pub proof fn lemma_safe_all(db: Db, p: Params)
    requires forall|k: GK| is_cand(db, p, k) ==> !unsafe_a(absk(db, k)),
    ensures eng_safe(db, p),
{
    assert forall|g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>|
        #[trigger] group_count(db, p, g_name, g_stmt, g_tag, g_plabel) > 0 implies group_float_safe(db, p, g_name, g_stmt, g_tag, g_plabel) by {
        lemma_group_nonempty(db, p, g_name, g_stmt, g_tag, g_plabel);
        let k = GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag->0, plabel: g_plabel };
        assert(is_cand(db, p, k));
        lemma_group_abs(db, p, k);
    }
}

pub proof fn lemma_unsafe_undefined(db: Db, p: Params, k: GK)
    requires is_cand(db, p, k), unsafe_a(absk(db, k)),
    ensures !answer_fits(db, p),
{
    lemma_count_pos(db, p, k);
    lemma_group_abs(db, p, k);
    if answer_fits(db, p) {
        assert(float_safe(db, p));
        assert(group_float_safe(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel));
    }
}

pub proof fn lemma_over_range(db: Db, p: Params, k: GK)
    requires is_cand(db, p, k), sumk(db, k) > (pow2(53) - 1) * pow2(2045),
    ensures eng_safe(db, p) || unsafe_a(absk(db, k)),
{
    lemma_abs_le(db, k);
    lemma_max_pos();
}

pub proof fn lemma_safe_each(db: Db, p: Params, k: GK)
    requires eng_safe(db, p), is_cand(db, p, k),
    ensures !unsafe_a(absk(db, k)),
{
    lemma_count_pos(db, p, k);
    lemma_group_abs(db, p, k);
}

} // verus!
