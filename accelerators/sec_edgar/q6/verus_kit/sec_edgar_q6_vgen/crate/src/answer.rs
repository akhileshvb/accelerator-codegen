// A stored group is a row of the answer: the spec's aggregates are the state's counts and sums.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::glue_proofs::*;

verus! {

pub open spec fn row_of_group(p: Params, k: GK, v: int, cnt: int) -> OutRow {
    OutRow { name: k.name, stmt: Some(p.p2), tag: Some(k.tag), plabel: k.plabel, total_value: Some(v), cnt }
}

/// The spec's group aggregate with weight f is the state-side sum when f agrees with it on the group's triples.
pub proof fn lemma_agg(db: Db, p: Params, k: GK, f: spec_fn(Tuple) -> int, hk: H3)
    requires
        k_params(k) == p,
        forall|n: NumRow, s: SubRow, pp: PreRow| jk(n, s, pp) && key_of(n, s, pp) == k ==> f(Tuple { n, s_: s, p: pp }) == hk(n, s, pp),
        forall|n: NumRow, s: SubRow, pp: PreRow| !(jk(n, s, pp) && key_of(n, s, pp) == k) ==> hk(n, s, pp) == 0,
    ensures
        group_sum(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel, f) == gdb(db, hk),
{
    lemma_group_sum_nested(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel, f);
    assert(is_key(p, Some(p.p2), Some(k.tag)));
    assert(k_of(p, k.name, Some(k.tag), k.plabel) == k);
    let h = |n: NumRow, s: SubRow, pp: PreRow| if jk(n, s, pp) && key_of(n, s, pp) == k { f(Tuple { n, s_: s, p: pp }) } else { 0 };
    assert forall|n: NumRow, s: SubRow, pp: PreRow| #[trigger] h(n, s, pp) == hk(n, s, pp) by {}
    lemma_gdb_ext(db, h, hk);
}

pub proof fn lemma_abs_le(db: Db, k: GK)
    ensures abs(sumk(db, k)) <= absk(db, k),
{
    // |sum f| <= sum |f|, level by level
    let (hv, ha) = (ival(k), iabs(k));
    lemma_gdb2(db, hv);
    lemma_gdb2(db, ha);
    assert forall|n: NumRow, s: SubRow| #[trigger] abs(p_lvl(db.pre, hv, n, s)) <= p_lvl(db.pre, ha, n, s) by {
        lemma_abs_sum(db.pre, |p: PreRow| hv(n, s, p), |p: PreRow| ha(n, s, p));
    }
    assert forall|n: NumRow| #[trigger] abs(s_lvl(db.sub, db.pre, hv, n)) <= s_lvl(db.sub, db.pre, ha, n) by {
        lemma_abs_sum(db.sub, |s: SubRow| p_lvl(db.pre, hv, n, s), |s: SubRow| p_lvl(db.pre, ha, n, s));
    }
    lemma_abs_sum(db.num, |n: NumRow| s_lvl(db.sub, db.pre, hv, n), |n: NumRow| s_lvl(db.sub, db.pre, ha, n));
}

/// |Σ f| <= Σ g when |f| <= g pointwise on the sequence.
pub proof fn lemma_abs_sum<A>(s: Seq<A>, f: spec_fn(A) -> int, g: spec_fn(A) -> int)
    requires forall|i: int| 0 <= i < s.len() ==> abs(f(#[trigger] s[i])) <= g(s[i]),
    ensures abs(sum(s, f)) <= sum(s, g),
    decreases s.len(),
{
    if s.len() > 0 {
        let t = s.drop_last();
        assert forall|i: int| 0 <= i < t.len() implies abs(f(#[trigger] t[i])) <= g(t[i]) by { assert(t[i] == s[i]); }
        lemma_abs_sum(t, f, g);
        assert(s =~= t.push(s.last()));
        lemma_sum_push(t, s.last(), f);
        lemma_sum_push(t, s.last(), g);
    }
}

pub proof fn lemma_row_ok(db: Db, p: Params, k: GK, v: int)
    requires
        k_params(k) == p,
        cntk(db, k) > 0,
        float_within(v, sumk(db, k), absk(db, k), cntk(db, k) - 1),
    ensures
        row_ok(db, p, row_of_group(p, k, v, cntk(db, k))),
{
    let one = |t: Tuple| 1int;
    let vc = |t: Tuple| if t.n.value is Some { 1int } else { 0 };
    let vv = |t: Tuple| if t.n.value is Some { t.n.value->0 } else { 0 };
    let va = |t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0 };
    lemma_agg(db, p, k, one, icnt(k));
    lemma_agg(db, p, k, vc, icnt(k));
    lemma_agg(db, p, k, vv, ival(k));
    lemma_agg(db, p, k, va, iabs(k));
    let (gn, gs_, gt, gp) = (k.name, Some(p.p2), Some(k.tag), k.plabel);
    let c = group_count(db, p, gn, gs_, gt, gp);
    assert forall|t: Tuple| #[trigger] one(t) == 1 by {}
    assert(c == group_sum(db, p, gn, gs_, gt, gp, one)) by {
        reveal(group_sum);
        reveal(group_count);
        let a = |t: Tuple| if t.s_.name == gn && t.p.stmt == gs_ && t.n.tag == gt && t.p.plabel == gp { 1int } else { 0 };
        let b = |t: Tuple| if t.s_.name == gn && t.p.stmt == gs_ && t.n.tag == gt && t.p.plabel == gp { one(t) } else { 0 };
        assert forall|t: Tuple| #[trigger] a(t) == b(t) by {}
        lemma_sum_ext_all(tuples(db, p), a, b);
    }
    assert(cntk(db, k) == gdb(db, icnt(k)));
}

} // verus!

verus! {

/// A spec group with rows is a stored group: its stmt and tag are the asked ones.
pub proof fn lemma_group_nonempty(db: Db, p: Params, g_name: Option<Seq<u8>>, g_stmt: Option<Seq<u8>>, g_tag: Option<Seq<u8>>, g_plabel: Option<Seq<u8>>)
    requires group_count(db, p, g_name, g_stmt, g_tag, g_plabel) > 0,
    ensures
        g_stmt == Some(p.p2), g_tag is Some,
        cntk(db, GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag->0, plabel: g_plabel }) > 0,
{
    let one = |t: Tuple| 1int;
    lemma_group_sum_nested(db, p, g_name, g_stmt, g_tag, g_plabel, one);
    assert(group_count(db, p, g_name, g_stmt, g_tag, g_plabel) == group_sum(db, p, g_name, g_stmt, g_tag, g_plabel, one)) by {
        reveal(group_sum);
        reveal(group_count);
        let a = |t: Tuple| if t.s_.name == g_name && t.p.stmt == g_stmt && t.n.tag == g_tag && t.p.plabel == g_plabel { 1int } else { 0 };
        let b = |t: Tuple| if t.s_.name == g_name && t.p.stmt == g_stmt && t.n.tag == g_tag && t.p.plabel == g_plabel { one(t) } else { 0 };
        assert forall|t: Tuple| #[trigger] a(t) == b(t) by {}
        lemma_sum_ext_all(tuples(db, p), a, b);
    }
    assert(is_key(p, g_stmt, g_tag));
    let k = GK { fy: p.p1, stmt: p.p2, uom: p.p3, name: g_name, tag: g_tag->0, plabel: g_plabel };
    assert(k_of(p, g_name, g_tag, g_plabel) == k);
    lemma_agg(db, p, k, one, icnt(k));
    assert(group_sum(db, p, k.name, Some(p.p2), Some(k.tag), k.plabel, one) == gdb(db, icnt(k)));
}

} // verus!
