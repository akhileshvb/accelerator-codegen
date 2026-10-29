// The query's tuples summed as nested sums over num, sub, tag with the pre rows folded into a
// multiplicity: what the join code computes.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;

verus! {

/// What a group's aggregates read of a tuple.
pub struct JK {
    pub sic: Option<int>,
    pub tlabel: Option<Seq<u8>>,
    pub cik: Option<int>,
    pub v: int,
    pub vs: bool,
    pub stmt: Option<Seq<u8>>,
}

pub open spec fn c_n(p: Params, n: NumRow) -> bool {
    n.uom == Some(p.p4) && n.value is Some && n.adsh is Some && n.tag is Some && n.version is Some
}

pub open spec fn c_s(p: Params, s: SubRow) -> bool {
    s.sic is Some && p.p1 <= s.sic->0 && s.sic->0 <= p.p2
}

pub open spec fn c_t(t: TagRow) -> bool {
    t.abstract_ == Some(0int)
}

pub open spec fn m_s(n: NumRow, s: SubRow) -> bool {
    n.adsh == Some(s.adsh)
}

pub open spec fn m_t(n: NumRow, t: TagRow) -> bool {
    n.tag == Some(t.tag) && n.version == Some(t.version)
}

pub open spec fn c_p(p: Params, n: NumRow, r: PreRow) -> bool {
    r.stmt == Some(p.p3) && r.adsh == n.adsh && r.tag == n.tag && r.version == n.version
}

pub open spec fn val_of(n: NumRow) -> int {
    if n.value is Some { n.value->0 } else { 0 }
}

pub open spec fn jk(p: Params, n: NumRow, s: SubRow, t: TagRow) -> JK {
    JK { sic: s.sic, tlabel: t.tlabel, cik: s.cik, v: val_of(n), vs: n.value is Some, stmt: Some(p.p3) }
}

pub open spec fn pj(tp: Tuple) -> JK {
    JK { sic: tp.s_.sic, tlabel: tp.t_.tlabel, cik: tp.s_.cik, v: val_of(tp.n), vs: tp.n.value is Some, stmt: tp.p.stmt }
}

pub open spec fn prew(db: Db, p: Params, n: NumRow) -> int {
    sum(db.pre, |r: PreRow| if c_p(p, n, r) { 1int } else { 0int })
}

pub open spec fn tag_term(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow) -> int {
    sum(db.tag, |t: TagRow| if c_t(t) && m_t(n, t) { prew(db, p, n) * h(jk(p, n, s, t)) } else { 0 })
}

pub open spec fn sub_term(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow) -> int {
    sum(db.sub, |s: SubRow| if c_s(p, s) && m_s(n, s) { tag_term(db, p, h, n, s) } else { 0 })
}

pub open spec fn num_term(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow) -> int {
    if c_n(p, n) { sub_term(db, p, h, n) } else { 0 }
}

proof fn lemma_where(db: Db, p: Params, n: NumRow, s: SubRow, t: TagRow, r: PreRow)
    ensures
        where_clause(db, p, n, s, t, r) == (c_n(p, n) && c_s(p, s) && c_t(t) && m_s(n, s) && m_t(n, t) && c_p(p, n, r)),
{
}

proof fn lemma_pre_level(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow)
    ensures
        sum(tuples_3(db, p, n, s, t), |tp: Tuple| h(pj(tp)))
            == if c_n(p, n) && c_s(p, s) && c_t(t) && m_s(n, s) && m_t(n, t) { prew(db, p, n) * h(jk(p, n, s, t)) } else { 0 },
{
    let g = |tp: Tuple| h(pj(tp));
    let gg = |r: PreRow| tuples_4(db, p, n, s, t, r);
    lemma_sum_flat_map(db.pre, gg, g);
    let a = c_n(p, n) && c_s(p, s) && c_t(t) && m_s(n, s) && m_t(n, t);
    let hv = h(jk(p, n, s, t));
    let ind = |r: PreRow| if c_p(p, n, r) { 1int } else { 0int };
    let term = |r: PreRow| sum(gg(r), g);
    assert forall|r: PreRow| #[trigger] term(r) == if a { hv * ind(r) } else { 0 } by {
        lemma_where(db, p, n, s, t, r);
        lemma_sum_opt(where_clause(db, p, n, s, t, r), Tuple { n, s_: s, t_: t, p: r }, g);
        if where_clause(db, p, n, s, t, r) {
            assert(pj(Tuple { n, s_: s, t_: t, p: r }) == jk(p, n, s, t));
        }
    }
    assert(sum(tuples_3(db, p, n, s, t), g) == sum(db.pre, term));
    if a {
        let scaled = |r: PreRow| hv * ind(r);
        lemma_sum_ext_all(db.pre, term, scaled);
        lemma_sum_scale(db.pre, ind, scaled, hv);
        assert(sum(db.pre, term) == hv * sum(db.pre, ind));
        assert(prew(db, p, n) == sum(db.pre, ind));
    } else {
        lemma_sum_ext_all(db.pre, term, |r: PreRow| 0int);
        lemma_sum_zero(db.pre, |r: PreRow| 0int);
    }
}

proof fn lemma_tag_level(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow)
    ensures
        sum(tuples_2(db, p, n, s), |tp: Tuple| h(pj(tp)))
            == if c_n(p, n) && c_s(p, s) && m_s(n, s) { tag_term(db, p, h, n, s) } else { 0 },
{
    let g = |tp: Tuple| h(pj(tp));
    let gg = |t: TagRow| tuples_3(db, p, n, s, t);
    lemma_sum_flat_map(db.tag, gg, g);
    let a = c_n(p, n) && c_s(p, s) && m_s(n, s);
    let term = |t: TagRow| sum(gg(t), g);
    let body = |t: TagRow| if c_t(t) && m_t(n, t) { prew(db, p, n) * h(jk(p, n, s, t)) } else { 0 };
    assert forall|t: TagRow| #[trigger] term(t) == if a { body(t) } else { 0 } by {
        lemma_pre_level(db, p, h, n, s, t);
    }
    if a {
        lemma_sum_ext_all(db.tag, term, body);
    } else {
        lemma_sum_ext_all(db.tag, term, |t: TagRow| 0int);
        lemma_sum_zero(db.tag, |t: TagRow| 0int);
    }
}

proof fn lemma_sub_level(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow)
    ensures
        sum(tuples_1(db, p, n), |tp: Tuple| h(pj(tp))) == num_term(db, p, h, n),
{
    let g = |tp: Tuple| h(pj(tp));
    let gg = |s: SubRow| tuples_2(db, p, n, s);
    lemma_sum_flat_map(db.sub, gg, g);
    let term = |s: SubRow| sum(gg(s), g);
    let body = |s: SubRow| if c_s(p, s) && m_s(n, s) { tag_term(db, p, h, n, s) } else { 0 };
    if c_n(p, n) {
        assert forall|s: SubRow| #[trigger] term(s) == body(s) by {
            lemma_tag_level(db, p, h, n, s);
        }
        lemma_sum_ext_all(db.sub, term, body);
    } else {
        assert forall|s: SubRow| #[trigger] term(s) == 0int by {
            lemma_tag_level(db, p, h, n, s);
        }
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

}
