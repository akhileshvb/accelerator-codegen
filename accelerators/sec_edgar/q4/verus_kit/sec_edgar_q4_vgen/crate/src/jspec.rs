// The query's tuple sums as one nested sum over the four tables (num, sub, tag, pre), and the
// signed records that stand for a change of that sum.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::join_spec::*;

verus! {

pub open spec fn wc(p: Params, n: NumRow, s: SubRow, t: TagRow, r: PreRow) -> bool {
    c_n(p, n) && c_s(p, s) && c_t(t) && m_s(n, s) && m_t(n, t) && c_p(p, n, r)
}

pub open spec fn tup(n: NumRow, s: SubRow, t: TagRow, r: PreRow) -> Tuple {
    Tuple { n, s_: s, t_: t, p: r }
}

pub open spec fn psi(p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow, r: PreRow) -> int {
    if wc(p, n, s, t, r) { h(pj(tup(n, s, t, r))) } else { 0 }
}

pub open spec fn pre_sum(pre: Seq<PreRow>, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow) -> int {
    sum(pre, |r: PreRow| psi(p, h, n, s, t, r))
}

pub open spec fn tag_sum4(tag: Seq<TagRow>, pre: Seq<PreRow>, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow) -> int {
    sum(tag, |t: TagRow| pre_sum(pre, p, h, n, s, t))
}

pub open spec fn sub_sum4(sub: Seq<SubRow>, tag: Seq<TagRow>, pre: Seq<PreRow>, p: Params, h: spec_fn(JK) -> int, n: NumRow) -> int {
    sum(sub, |s: SubRow| tag_sum4(tag, pre, p, h, n, s))
}

/// The sum of h over the tuples of the join of four bags.
pub open spec fn jsum(num: Seq<NumRow>, sub: Seq<SubRow>, tag: Seq<TagRow>, pre: Seq<PreRow>, p: Params, h: spec_fn(JK) -> int) -> int {
    sum(num, |n: NumRow| sub_sum4(sub, tag, pre, p, h, n))
}

proof fn lemma_level4(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow)
    ensures
        sum(tuples_3(db, p, n, s, t), |tp: Tuple| h(pj(tp))) == pre_sum(db.pre, p, h, n, s, t),
{
    let g = |tp: Tuple| h(pj(tp));
    let gg = |r: PreRow| tuples_4(db, p, n, s, t, r);
    lemma_sum_flat_map(db.pre, gg, g);
    assert forall|r: PreRow| #[trigger] sum(gg(r), g) == psi(p, h, n, s, t, r) by {
        lemma_sum_opt(where_clause(db, p, n, s, t, r), tup(n, s, t, r), g);
    }
    lemma_sum_ext_all(db.pre, |r: PreRow| sum(gg(r), g), |r: PreRow| psi(p, h, n, s, t, r));
}

proof fn lemma_level3(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow)
    ensures
        sum(tuples_2(db, p, n, s), |tp: Tuple| h(pj(tp))) == tag_sum4(db.tag, db.pre, p, h, n, s),
{
    let g = |tp: Tuple| h(pj(tp));
    let gg = |t: TagRow| tuples_3(db, p, n, s, t);
    lemma_sum_flat_map(db.tag, gg, g);
    assert forall|t: TagRow| #[trigger] sum(gg(t), g) == pre_sum(db.pre, p, h, n, s, t) by {
        lemma_level4(db, p, h, n, s, t);
    }
    lemma_sum_ext_all(db.tag, |t: TagRow| sum(gg(t), g), |t: TagRow| pre_sum(db.pre, p, h, n, s, t));
}

proof fn lemma_level2(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow)
    ensures
        sum(tuples_1(db, p, n), |tp: Tuple| h(pj(tp))) == sub_sum4(db.sub, db.tag, db.pre, p, h, n),
{
    let g = |tp: Tuple| h(pj(tp));
    let gg = |s: SubRow| tuples_2(db, p, n, s);
    lemma_sum_flat_map(db.sub, gg, g);
    assert forall|s: SubRow| #[trigger] sum(gg(s), g) == tag_sum4(db.tag, db.pre, p, h, n, s) by {
        lemma_level3(db, p, h, n, s);
    }
    lemma_sum_ext_all(db.sub, |s: SubRow| sum(gg(s), g), |s: SubRow| tag_sum4(db.tag, db.pre, p, h, n, s));
}

/// The tuples' sum of anything the group aggregates read is the nested sum over the tables.
pub proof fn lemma_tuple_jsum(db: Db, p: Params, h: spec_fn(JK) -> int)
    ensures
        sum(tuples(db, p), |tp: Tuple| h(pj(tp))) == jsum(db.num, db.sub, db.tag, db.pre, p, h),
{
    let g = |tp: Tuple| h(pj(tp));
    let gg = |n: NumRow| tuples_1(db, p, n);
    lemma_sum_flat_map(db.num, gg, g);
    assert forall|n: NumRow| #[trigger] sum(gg(n), g) == sub_sum4(db.sub, db.tag, db.pre, p, h, n) by {
        lemma_level2(db, p, h, n);
    }
    lemma_sum_ext_all(db.num, |n: NumRow| sum(gg(n), g), |n: NumRow| sub_sum4(db.sub, db.tag, db.pre, p, h, n));
}

}
