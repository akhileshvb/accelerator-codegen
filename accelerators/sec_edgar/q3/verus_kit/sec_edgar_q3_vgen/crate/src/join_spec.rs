// Both blocks of the query (the per-company sums and the outer groups) range over the same tuples:
// num rows joined with the sub rows of their filing in the given year.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;

verus! {

/// What the aggregates read of a tuple.
pub struct JK {
    pub name: Option<Seq<u8>>,
    pub cik: Option<int>,
    pub v: int,
    pub vs: bool,
}

pub open spec fn c_n(p: Params, n: NumRow) -> bool {
    n.uom == Some(p.p2) && n.value is Some && n.adsh is Some
}

pub open spec fn c_s(p: Params, s: SubRow) -> bool {
    s.fy == Some(p.p1)
}

pub open spec fn m_s(n: NumRow, s: SubRow) -> bool {
    n.adsh == Some(s.adsh)
}

pub open spec fn val_of(n: NumRow) -> int {
    if n.value is Some { n.value->0 } else { 0 }
}

pub open spec fn jk(n: NumRow, s: SubRow) -> JK {
    JK { name: s.name, cik: s.cik, v: val_of(n), vs: n.value is Some }
}

pub open spec fn pj(tp: Tuple) -> JK {
    jk(tp.n, tp.s_)
}

pub open spec fn pj2(tp: AvgSubTuple) -> JK {
    jk(tp.n2, tp.s2)
}

pub open spec fn sub_term(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow) -> int {
    sum(db.sub, |s: SubRow| if c_s(p, s) && m_s(n, s) { h(jk(n, s)) } else { 0 })
}

pub open spec fn num_term(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow) -> int {
    if c_n(p, n) { sub_term(db, p, h, n) } else { 0 }
}

proof fn lemma_sub_level(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow)
    ensures
        sum(tuples_1(db, p, n), |tp: Tuple| h(pj(tp))) == num_term(db, p, h, n),
{
    let g = |tp: Tuple| h(pj(tp));
    let gg = |s: SubRow| tuples_2(db, p, n, s);
    lemma_sum_flat_map(db.sub, gg, g);
    let term = |s: SubRow| sum(gg(s), g);
    let body = |s: SubRow| if c_s(p, s) && m_s(n, s) { h(jk(n, s)) } else { 0 };
    assert forall|s: SubRow| #[trigger] term(s) == if c_n(p, n) { body(s) } else { 0 } by {
        lemma_sum_opt(where_clause(db, p, n, s), Tuple { n, s_: s }, g);
    }
    assert(sum(tuples_1(db, p, n), g) == sum(db.sub, term));
    if c_n(p, n) {
        lemma_sum_ext_all(db.sub, term, body);
    } else {
        lemma_sum_ext_all(db.sub, term, |s: SubRow| 0int);
        lemma_sum_zero(db.sub, |s: SubRow| 0int);
    }
}

/// The tuples' sum of anything the group aggregates read.
pub proof fn lemma_tuple_sum(db: Db, p: Params, h: spec_fn(JK) -> int)
    ensures
        sum(tuples(db, p), |tp: Tuple| h(pj(tp))) == sum(db.num, |n: NumRow| num_term(db, p, h, n)),
{
    let g = |tp: Tuple| h(pj(tp));
    let gg = |n: NumRow| tuples_1(db, p, n);
    lemma_sum_flat_map(db.num, gg, g);
    assert forall|n: NumRow| #[trigger] sum(gg(n), g) == num_term(db, p, h, n) by {
        lemma_sub_level(db, p, h, n);
    }
    lemma_sum_ext_all(db.num, |n: NumRow| sum(gg(n), g), |n: NumRow| num_term(db, p, h, n));
}

proof fn lemma_avg_sub_level(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow)
    ensures
        sum(avg_sub_tuples_1(db, p, n), |tp: AvgSubTuple| h(pj2(tp))) == num_term(db, p, h, n),
{
    let g = |tp: AvgSubTuple| h(pj2(tp));
    let gg = |s: SubRow| avg_sub_tuples_2(db, p, n, s);
    lemma_sum_flat_map(db.sub, gg, g);
    let term = |s: SubRow| sum(gg(s), g);
    let body = |s: SubRow| if c_s(p, s) && m_s(n, s) { h(jk(n, s)) } else { 0 };
    assert forall|s: SubRow| #[trigger] term(s) == if c_n(p, n) { body(s) } else { 0 } by {
        lemma_sum_opt(avg_sub_where_clause(db, p, n, s), AvgSubTuple { n2: n, s2: s }, g);
    }
    assert(sum(avg_sub_tuples_1(db, p, n), g) == sum(db.sub, term));
    if c_n(p, n) {
        lemma_sum_ext_all(db.sub, term, body);
    } else {
        lemma_sum_ext_all(db.sub, term, |s: SubRow| 0int);
        lemma_sum_zero(db.sub, |s: SubRow| 0int);
    }
}

pub proof fn lemma_avg_tuple_sum(db: Db, p: Params, h: spec_fn(JK) -> int)
    ensures
        sum(avg_sub_tuples(db, p), |tp: AvgSubTuple| h(pj2(tp))) == sum(db.num, |n: NumRow| num_term(db, p, h, n)),
{
    let g = |tp: AvgSubTuple| h(pj2(tp));
    let gg = |n: NumRow| avg_sub_tuples_1(db, p, n);
    lemma_sum_flat_map(db.num, gg, g);
    assert forall|n: NumRow| #[trigger] sum(gg(n), g) == num_term(db, p, h, n) by {
        lemma_avg_sub_level(db, p, h, n);
    }
    lemma_sum_ext_all(db.num, |n: NumRow| sum(gg(n), g), |n: NumRow| num_term(db, p, h, n));
}

}
