// The spec's groups as sums over num rows that have no matching pre row (the anti-join).
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::lemma_sum_ones;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

/// A group of the state: the year bucket and uom of its rows, and the query's group key.
pub struct GK {
    pub y: int,
    pub uom: Seq<u8>,
    pub tag: Option<Seq<u8>>,
    pub ver: Option<Seq<u8>>,
}

/// The row can be counted by a query: its ddate lies in the year y of some parameter p1 = y.
pub open spec fn dq(n: NumRow) -> bool {
    &&& n.value is Some
    &&& n.uom is Some
    &&& n.ddate is Some
    &&& 101 <= n.ddate->0 % 10000
    &&& n.ddate->0 % 10000 <= 1231
}

pub open spec fn gk_of(n: NumRow) -> GK {
    GK { y: n.ddate->0 / 10000, uom: n.uom->0, tag: n.tag, ver: n.version }
}

/// The ON condition of the LEFT JOIN, without the unused db and parameters.
pub open spec fn m3(n: NumRow, p: PreRow) -> bool {
    &&& n.adsh is Some && p.adsh == n.adsh
    &&& n.tag is Some && p.tag == n.tag
    &&& n.version is Some && p.version == n.version
}

pub open spec fn mw(n: NumRow) -> spec_fn(PreRow) -> int {
    |p: PreRow| ind(m3(n, p))
}

/// The number of pre rows n joins with.
pub open spec fn mc(pre: Seq<PreRow>, n: NumRow) -> int {
    sum(pre, mw(n))
}

/// 1 when n has no match (it survives the anti-join).
pub open spec fn um(pre: Seq<PreRow>, n: NumRow) -> int {
    ind(mc(pre, n) == 0)
}

pub type HN = spec_fn(NumRow) -> int;

pub open spec fn ic(k: GK) -> HN {
    |n: NumRow| if dq(n) && gk_of(n) == k { 1int } else { 0 }
}

pub open spec fn iv(k: GK) -> HN {
    |n: NumRow| if dq(n) && gk_of(n) == k { n.value->0 } else { 0 }
}

pub open spec fn ia(k: GK) -> HN {
    |n: NumRow| if dq(n) && gk_of(n) == k { abs(n.value->0) } else { 0 }
}

pub open spec fn uw(pre: Seq<PreRow>, h: HN) -> spec_fn(NumRow) -> int {
    |n: NumRow| um(pre, n) * h(n)
}

/// Σ h over the num rows with no match in pre.
pub open spec fn nsum(num: Seq<NumRow>, pre: Seq<PreRow>, h: HN) -> int {
    sum(num, uw(pre, h))
}

pub open spec fn cntk(db: Db, k: GK) -> int { nsum(db.num, db.pre, ic(k)) }
pub open spec fn sumk(db: Db, k: GK) -> int { nsum(db.num, db.pre, iv(k)) }
pub open spec fn absk(db: Db, k: GK) -> int { nsum(db.num, db.pre, ia(k)) }

pub open spec fn k_params(k: GK) -> Params {
    params(k.y, k.uom)
}

pub open spec fn k_of(p: Params, g_tag: Option<Seq<u8>>, g_ver: Option<Seq<u8>>) -> GK {
    GK { y: p.p1, uom: p.p2, tag: g_tag, ver: g_ver }
}

/// The parameter range test of the spec is the year bucket test.
proof fn lemma_range_year(y: int, d: int)
    ensures
        (y * 10000 + 101 <= d && d <= y * 10000 + 1231) <==> (d / 10000 == y && 101 <= d % 10000 && d % 10000 <= 1231),
{
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(d, 10000);
    vstd::arithmetic::div_mod::lemma_mod_bound(d, 10000);
    let (q, r) = (d / 10000, d % 10000);
    assert(d == 10000 * q + r);
    if y * 10000 + 101 <= d && d <= y * 10000 + 1231 {
        assert(q == y) by (nonlinear_arith)
            requires d == 10000 * q + r, 0 <= r < 10000, y * 10000 + 101 <= d, d <= y * 10000 + 1231;
        assert(r == d - 10000 * y);
    }
    if q == y && 101 <= r && r <= 1231 {
        assert(y * 10000 == 10000 * q);
    }
}

/// A num row passes the WHERE of the query for parameters p, with no pre row: it is counted in the group of k_of.
pub open spec fn passes(p: Params, n: NumRow) -> bool {
    &&& (n.uom is Some && n.uom->0 == p.p2)
    &&& ((n.ddate is Some && p.p1 * 10000 + 101 <= n.ddate->0) && (n.ddate is Some && n.ddate->0 <= p.p1 * 10000 + 1231))
    &&& (n.value is Some)
}

proof fn lemma_passes_gk(p: Params, n: NumRow, g_tag: Option<Seq<u8>>, g_ver: Option<Seq<u8>>)
    ensures
        (passes(p, n) && n.tag == g_tag && n.version == g_ver) <==> (dq(n) && gk_of(n) == k_of(p, g_tag, g_ver)),
{
    if n.ddate is Some { lemma_range_year(p.p1, n.ddate->0); }
}

/// The tuples extending n: the one outer-join tuple when n has no match, none otherwise.
proof fn lemma_tuples_1_sum(db: Db, p: Params, n: NumRow, w: spec_fn(Tuple) -> int)
    ensures
        sum(tuples_1(db, p, n), w) == if mc(db.pre, n) == 0 && where_clause(db, p, n, None) { w(Tuple { n, p: None }) } else { 0 },
{
    reveal(tuples_1);
    let h = |pp: PreRow| if on_p(db, p, n, pp) { seq![Some(pp)] } else { Seq::<Option<PreRow>>::empty() };
    let m = flat(db.pre.map_values(h));
    let one = |x: Option<PreRow>| 1int;
    lemma_sum_flat_map(db.pre, h, one);
    lemma_sum_ones(m);
    assert forall|pp: PreRow| #[trigger] sum(h(pp), one) == mw(n)(pp) by {
        lemma_sum_opt(on_p(db, p, n, pp), Some(pp), one);
        assert(on_p(db, p, n, pp) == m3(n, pp));
    }
    lemma_sum_ext_all(db.pre, |pp: PreRow| sum(h(pp), one), mw(n));
    assert(m.len() == mc(db.pre, n));
    let g = |x: Option<PreRow>| tuples_2(db, p, n, x);
    if m.len() == 0 {
        assert(mc(db.pre, n) == 0);
        let lst = seq![None::<PreRow>];
        lemma_sum_flat_map(lst, g, w);
        lemma_sum_singleton(None::<PreRow>, |x: Option<PreRow>| sum(g(x), w));
        reveal(tuples_2);
        lemma_sum_opt(where_clause(db, p, n, None), Tuple { n, p: None }, w);
    } else {
        lemma_sum_flat_map(m, g, w);
        assert forall|i: int| 0 <= i < m.len() implies sum(g(#[trigger] m[i]), w) == 0 by {
            let x = m[i];
            assert(m.contains(x));
            lemma_flat_contains(db.pre.map_values(h), x);
            let j = choose|j: int| 0 <= j < db.pre.map_values(h).len() && (#[trigger] db.pre.map_values(h)[j]).contains(x);
            let pp = db.pre[j];
            assert(db.pre.map_values(h)[j] == h(pp));
            assert(on_p(db, p, n, pp));
            assert(x == Some(pp));
            reveal(tuples_2);
            assert(!where_clause(db, p, n, x));
            lemma_sum_empty(Seq::<Tuple>::empty(), w);
        }
        lemma_sum_zero(m, |x: Option<PreRow>| sum(g(x), w));
    }
}

/// The spec's group aggregate with weight (hn on the tuple's num row) is the anti-join sum.
pub proof fn lemma_group_nsum(db: Db, p: Params, g_tag: Option<Seq<u8>>, g_ver: Option<Seq<u8>>, hn: HN, f: spec_fn(Tuple) -> int)
    requires
        forall|t: Tuple| #[trigger] f(t) == hn(t.n),
    ensures
        group_sum(db, p, g_tag, g_ver, f) == nsum(db.num, db.pre, |n: NumRow| if dq(n) && gk_of(n) == k_of(p, g_tag, g_ver) { hn(n) } else { 0 }),
{
    reveal(group_sum);
    reveal(tuples);
    let w = |t: Tuple| if t.n.tag == g_tag && t.n.version == g_ver { f(t) } else { 0 };
    let g0 = |n: NumRow| tuples_1(db, p, n);
    lemma_sum_flat_map(db.num, g0, w);
    let k = k_of(p, g_tag, g_ver);
    let hk = |n: NumRow| if dq(n) && gk_of(n) == k { hn(n) } else { 0 };
    assert forall|n: NumRow| #[trigger] sum(g0(n), w) == uw(db.pre, hk)(n) by {
        lemma_tuples_1_sum(db, p, n, w);
        lemma_passes_gk(p, n, g_tag, g_ver);
        reveal(tuples_2);
        assert(where_clause(db, p, n, None) == passes(p, n));
        if mc(db.pre, n) == 0 {
            assert(um(db.pre, n) == 1);
            assert(f(Tuple { n, p: None }) == hn(n));
        } else {
            assert(um(db.pre, n) == 0);
            assert(um(db.pre, n) * hk(n) == 0) by (nonlinear_arith) requires um(db.pre, n) == 0;
        }
    }
    lemma_sum_ext_all(db.num, |n: NumRow| sum(g0(n), w), uw(db.pre, hk));
}

/// The spec's three aggregates of a group, as anti-join sums (k = the group under parameters p).
pub proof fn lemma_aggs(db: Db, p: Params, g_tag: Option<Seq<u8>>, g_ver: Option<Seq<u8>>)
    ensures
        ({ let k = k_of(p, g_tag, g_ver);
            &&& group_count(db, p, g_tag, g_ver) == cntk(db, k)
            &&& group_sum(db, p, g_tag, g_ver, |t: Tuple| if t.n.value is Some { 1int } else { 0 }) == cntk(db, k)
            &&& group_sum(db, p, g_tag, g_ver, |t: Tuple| if t.n.value is Some { t.n.value->0 } else { 0 }) == sumk(db, k)
            &&& group_sum(db, p, g_tag, g_ver, |t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0 }) == absk(db, k)
        }),
{
    let k = k_of(p, g_tag, g_ver);
    let one = |t: Tuple| 1int;
    let vc = |t: Tuple| if t.n.value is Some { 1int } else { 0 };
    let vv = |t: Tuple| if t.n.value is Some { t.n.value->0 } else { 0 };
    let va = |t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0 };
    let (h1, hc, hv, ha) = (|n: NumRow| 1int, |n: NumRow| if n.value is Some { 1int } else { 0 }, |n: NumRow| if n.value is Some { n.value->0 } else { 0 }, |n: NumRow| if n.value is Some { abs(n.value->0) } else { 0 });
    lemma_group_nsum(db, p, g_tag, g_ver, h1, one);
    lemma_group_nsum(db, p, g_tag, g_ver, hc, vc);
    lemma_group_nsum(db, p, g_tag, g_ver, hv, vv);
    lemma_group_nsum(db, p, g_tag, g_ver, ha, va);
    assert(group_count(db, p, g_tag, g_ver) == group_sum(db, p, g_tag, g_ver, one)) by {
        reveal(group_sum);
        reveal(group_count);
        let a = |t: Tuple| if t.n.tag == g_tag && t.n.version == g_ver { 1int } else { 0 };
        let b = |t: Tuple| if t.n.tag == g_tag && t.n.version == g_ver { one(t) } else { 0 };
        assert forall|t: Tuple| #[trigger] a(t) == b(t) by {}
        lemma_sum_ext_all(tuples(db, p), a, b);
    }
    let (x1, xc, xv, xa) = (
        |n: NumRow| if dq(n) && gk_of(n) == k { h1(n) } else { 0 },
        |n: NumRow| if dq(n) && gk_of(n) == k { hc(n) } else { 0 },
        |n: NumRow| if dq(n) && gk_of(n) == k { hv(n) } else { 0 },
        |n: NumRow| if dq(n) && gk_of(n) == k { ha(n) } else { 0 });
    assert forall|n: NumRow| #[trigger] x1(n) == ic(k)(n) && xc(n) == ic(k)(n) && xv(n) == iv(k)(n) && xa(n) == ia(k)(n) by {}
    lemma_sum_ext_all(db.num, uw(db.pre, x1), uw(db.pre, ic(k)));
    lemma_sum_ext_all(db.num, uw(db.pre, xc), uw(db.pre, ic(k)));
    lemma_sum_ext_all(db.num, uw(db.pre, xv), uw(db.pre, iv(k)));
    lemma_sum_ext_all(db.num, uw(db.pre, xa), uw(db.pre, ia(k)));
}

pub proof fn lemma_mc_nonneg(pre: Seq<PreRow>, n: NumRow)
    ensures mc(pre, n) >= 0, um(pre, n) == 0 || um(pre, n) == 1,
{
    assert forall|i: int| 0 <= i < pre.len() implies mw(n)(#[trigger] pre[i]) >= 0 by {}
    lemma_sum_nonneg(pre, mw(n));
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

/// A group has a non-negative count of at most the num rows; no rows, no sum; |sum| <= sum of |x|.
pub proof fn lemma_cnt_facts(db: Db, k: GK)
    ensures
        cntk(db, k) >= 0, cntk(db, k) <= db.num.len(),
        cntk(db, k) == 0 ==> sumk(db, k) == 0,
        abs(sumk(db, k)) <= absk(db, k),
{
    let f = uw(db.pre, ic(k));
    let g = uw(db.pre, iv(k));
    let a = uw(db.pre, ia(k));
    assert forall|i: int| 0 <= i < db.num.len() implies f(#[trigger] db.num[i]) >= 0 by {
        lemma_mc_nonneg(db.pre, db.num[i]);
    }
    assert forall|n: NumRow| #[trigger] f(n) <= (|r: NumRow| 1int)(n) by {
        lemma_mc_nonneg(db.pre, n);
    }
    lemma_sum_le(db.num, f, |r: NumRow| 1int);
    lemma_sum_ones(db.num);
    lemma_sum_nonneg(db.num, f);
    assert forall|i: int| 0 <= i < db.num.len() implies abs(g(#[trigger] db.num[i])) <= a(db.num[i]) by {
        lemma_mc_nonneg(db.pre, db.num[i]);
    }
    lemma_abs_sum(db.num, g, a);
    if cntk(db, k) == 0 {
        assert forall|i: int| 0 <= i < db.num.len() implies g(#[trigger] db.num[i]) == 0 by {
            lemma_sum_ge_term(db.num, f, i);
            lemma_mc_nonneg(db.pre, db.num[i]);
            let n = db.num[i];
            assert(f(n) == 0);
            if um(db.pre, n) == 1 { assert(!(dq(n) && gk_of(n) == k)); }
        }
        lemma_sum_zero(db.num, g);
    }
}

/// A group with more than 10 rows and a total within the bound is a row of the answer.
pub proof fn lemma_row_ok(db: Db, p: Params, g_tag: Option<Seq<u8>>, g_ver: Option<Seq<u8>>, v: int)
    requires
        cntk(db, k_of(p, g_tag, g_ver)) > 10,
        float_within(v, sumk(db, k_of(p, g_tag, g_ver)), absk(db, k_of(p, g_tag, g_ver)), cntk(db, k_of(p, g_tag, g_ver)) - 1),
    ensures
        group_count(db, p, g_tag, g_ver) == cntk(db, k_of(p, g_tag, g_ver)),
        row_ok(db, p, OutRow { tag: g_tag, version: g_ver, cnt: cntk(db, k_of(p, g_tag, g_ver)), total: Some(v) }),
{
    lemma_aggs(db, p, g_tag, g_ver);
}

} // verus!
