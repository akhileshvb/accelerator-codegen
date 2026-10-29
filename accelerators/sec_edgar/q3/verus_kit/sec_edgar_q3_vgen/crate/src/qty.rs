// The quantities the state holds per group, as join sums over the database; the ledger of
// contributions a batch of changes makes to them; and the facts about them the kernel needs.
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::exact::*;
use crate::gen_spec::*;
use crate::join_spec::*;

verus! {

pub struct Q {
    pub n: int,
    pub s: int,
    pub a: int,
}

pub open spec fn q_add(x: Q, y: Q) -> Q {
    Q { n: x.n + y.n, s: x.s + y.s, a: x.a + y.a }
}

pub open spec fn fvs() -> spec_fn(JK) -> int {
    |j: JK| if j.vs { 1int } else { 0int }
}

pub open spec fn fv() -> spec_fn(JK) -> int {
    |j: JK| j.v
}

pub open spec fn fm() -> spec_fn(JK) -> int {
    |j: JK| abs(j.v)
}

/// h restricted to a group: a company when `inner`, else a (name, company).
pub open spec fn gh(inner: bool, gn: Option<Seq<u8>>, gc: Option<int>, f: spec_fn(JK) -> int) -> spec_fn(JK) -> int {
    |j: JK| if j.cik == gc && (inner || j.name == gn) { f(j) } else { 0 }
}

/// What a (num, sub) pair adds to h's sum in the parameter set.
pub open spec fn wt(p: Params, h: spec_fn(JK) -> int) -> spec_fn(NumRow, SubRow) -> int {
    |n: NumRow, s: SubRow| if c_n(p, n) && c_s(p, s) && m_s(n, s) { h(jk(n, s)) } else { 0 }
}

pub open spec fn jq(db: Db, p: Params, h: spec_fn(JK) -> int) -> int {
    join_sum(db.num, db.sub, wt(p, h))
}

/// A group's count, sum and absolute sum.
pub open spec fn qj(db: Db, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>) -> Q {
    Q {
        n: jq(db, p, gh(inner, gn, gc, fvs())),
        s: jq(db, p, gh(inner, gn, gc, fv())),
        a: jq(db, p, gh(inner, gn, gc, fm())),
    }
}

/// One tuple's contribution to the groups of its class, with a sign (a multiplicity).
pub struct Ct {
    pub fy: int,
    pub uom: Seq<u8>,
    pub j: JK,
    pub sign: int,
}

pub open spec fn ct_in(c: Ct, p: Params) -> bool {
    c.fy == p.p1 && c.uom == p.p2
}

pub open spec fn lsum(l: Seq<Ct>, p: Params, h: spec_fn(JK) -> int) -> int {
    sum(l, |c: Ct| if ct_in(c, p) { c.sign * h(c.j) } else { 0 })
}

pub open spec fn dq(l: Seq<Ct>, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>) -> Q {
    Q {
        n: lsum(l, p, gh(inner, gn, gc, fvs())),
        s: lsum(l, p, gh(inner, gn, gc, fv())),
        a: lsum(l, p, gh(inner, gn, gc, fm())),
    }
}

/// The ledger is the difference of the joins, for every weight.
pub open spec fn ledger_ok(l: Seq<Ct>, db: Db, db2: Db) -> bool {
    forall|p: Params, h: spec_fn(JK) -> int| #[trigger] jq(db2, p, h) == jq(db, p, h) + lsum(l, p, h)
}

pub proof fn lemma_ledger_q(l: Seq<Ct>, db: Db, db2: Db, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>)
    requires ledger_ok(l, db, db2),
    ensures qj(db2, p, inner, gn, gc) == q_add(qj(db, p, inner, gn, gc), dq(l, p, inner, gn, gc)),
{
    let h1 = gh(inner, gn, gc, fvs());
    let h2 = gh(inner, gn, gc, fv());
    let h3 = gh(inner, gn, gc, fm());
    assert(jq(db2, p, h1) == jq(db, p, h1) + lsum(l, p, h1));
    assert(jq(db2, p, h2) == jq(db, p, h2) + lsum(l, p, h2));
    assert(jq(db2, p, h3) == jq(db, p, h3) + lsum(l, p, h3));
}

// ── the join sums are the spec's group sums ──

pub open spec fn tup_vs() -> spec_fn(Tuple) -> int {
    |t: Tuple| if t.n.value is Some { 1int } else { 0int }
}

pub open spec fn tup_value() -> spec_fn(Tuple) -> int {
    |t: Tuple| if t.n.value is Some { t.n.value->0 } else { 0int }
}

pub open spec fn tup_mag() -> spec_fn(Tuple) -> int {
    |t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0int }
}

pub open spec fn avg_vs() -> spec_fn(AvgSubTuple) -> int {
    |t: AvgSubTuple| if t.n2.value is Some { 1int } else { 0int }
}

pub open spec fn avg_value() -> spec_fn(AvgSubTuple) -> int {
    |t: AvgSubTuple| if t.n2.value is Some { t.n2.value->0 } else { 0int }
}

pub open spec fn avg_mag() -> spec_fn(AvgSubTuple) -> int {
    |t: AvgSubTuple| if t.n2.value is Some { abs(t.n2.value->0) } else { 0int }
}

proof fn lemma_num_term_join(db: Db, p: Params, h: spec_fn(JK) -> int, n: NumRow)
    ensures num_term(db, p, h, n) == sum(db.sub, |s: SubRow| wt(p, h)(n, s)),
{
    let f = |s: SubRow| wt(p, h)(n, s);
    let body = |s: SubRow| if c_s(p, s) && m_s(n, s) { h(jk(n, s)) } else { 0 };
    if c_n(p, n) {
        assert forall|s: SubRow| #[trigger] f(s) == body(s) by {}
        lemma_sum_ext_all(db.sub, f, body);
    } else {
        assert forall|s: SubRow| #[trigger] f(s) == 0 by {}
        lemma_sum_ext_all(db.sub, f, |s: SubRow| 0int);
        lemma_sum_zero(db.sub, |s: SubRow| 0int);
    }
}

/// The tuples' sum of anything the groups read is the join sum.
pub proof fn lemma_jq_tuples(db: Db, p: Params, h: spec_fn(JK) -> int)
    ensures
        sum(tuples(db, p), |tp: Tuple| h(pj(tp))) == jq(db, p, h),
        sum(avg_sub_tuples(db, p), |tp: AvgSubTuple| h(pj2(tp))) == jq(db, p, h),
{
    lemma_tuple_sum(db, p, h);
    lemma_avg_tuple_sum(db, p, h);
    assert forall|n: NumRow| #[trigger] num_term(db, p, h, n) == sum(db.sub, |s: SubRow| wt(p, h)(n, s)) by {
        lemma_num_term_join(db, p, h, n);
    }
    lemma_sum_ext_all(db.num, |n: NumRow| num_term(db, p, h, n), |n: NumRow| sum(db.sub, |s: SubRow| wt(p, h)(n, s)));
}

pub proof fn lemma_outer_join(db: Db, p: Params, gn: Option<Seq<u8>>, gc: Option<int>)
    ensures
        group_sum(db, p, gn, gc, tup_vs()) == qj(db, p, false, gn, gc).n,
        group_sum(db, p, gn, gc, tup_value()) == qj(db, p, false, gn, gc).s,
        group_sum(db, p, gn, gc, tup_mag()) == qj(db, p, false, gn, gc).a,
{
    let tup = tuples(db, p);
    let fs = [fvs(), fv(), fm()];
    let h1 = gh(false, gn, gc, fvs());
    let h2 = gh(false, gn, gc, fv());
    let h3 = gh(false, gn, gc, fm());
    lemma_jq_tuples(db, p, h1);
    lemma_jq_tuples(db, p, h2);
    lemma_jq_tuples(db, p, h3);
    lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_vs()(t) } else { 0 }, |t: Tuple| h1(pj(t)));
    lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_value()(t) } else { 0 }, |t: Tuple| h2(pj(t)));
    lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_mag()(t) } else { 0 }, |t: Tuple| h3(pj(t)));
}

pub proof fn lemma_inner_join(db: Db, p: Params, gc: Option<int>)
    ensures
        avg_sub_group_sum(db, p, gc, avg_vs()) == qj(db, p, true, None, gc).n,
        avg_sub_group_sum(db, p, gc, avg_value()) == qj(db, p, true, None, gc).s,
        avg_sub_group_sum(db, p, gc, avg_mag()) == qj(db, p, true, None, gc).a,
{
    let tup = avg_sub_tuples(db, p);
    let h1 = gh(true, None, gc, fvs());
    let h2 = gh(true, None, gc, fv());
    let h3 = gh(true, None, gc, fm());
    lemma_jq_tuples(db, p, h1);
    lemma_jq_tuples(db, p, h2);
    lemma_jq_tuples(db, p, h3);
    lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_vs()(t) } else { 0 }, |t: AvgSubTuple| h1(pj2(t)));
    lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_value()(t) } else { 0 }, |t: AvgSubTuple| h2(pj2(t)));
    lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_mag()(t) } else { 0 }, |t: AvgSubTuple| h3(pj2(t)));
}

// ── facts about a group's quantities ──

pub open spec fn gcnt<A>(s: Seq<A>, g: spec_fn(A) -> bool, vs: spec_fn(A) -> int) -> int {
    sum(s, |x: A| if g(x) { vs(x) } else { 0 })
}

pub open spec fn gsm<A>(s: Seq<A>, g: spec_fn(A) -> bool, vv: spec_fn(A) -> int) -> int {
    sum(s, |x: A| if g(x) { vv(x) } else { 0 })
}

pub open spec fn gab<A>(s: Seq<A>, g: spec_fn(A) -> bool, vv: spec_fn(A) -> int) -> int {
    sum(s, |x: A| if g(x) { abs(vv(x)) } else { 0 })
}

pub proof fn lemma_gen_group<A>(s: Seq<A>, g: spec_fn(A) -> bool, vs: spec_fn(A) -> int, vv: spec_fn(A) -> int)
    requires
        forall|x: A| 0 <= #[trigger] vs(x) <= 1,
        forall|x: A| #[trigger] vs(x) == 0 ==> vv(x) == 0,
    ensures
        gcnt(s, g, vs) >= 0,
        abs(gsm(s, g, vv)) <= gab(s, g, vv),
        gcnt(s, g, vs) == 0 ==> gsm(s, g, vv) == 0 && gab(s, g, vv) == 0,
        gcnt(s, g, vs) == 1 ==> exists|i: int| 0 <= i < s.len() && g(#[trigger] s[i]) && vs(s[i]) == 1 && gsm(s, g, vv) == vv(s[i]),
    decreases s.len(),
{
    if s.len() > 0 {
        let t = s.drop_last();
        let x = s.last();
        assert(s =~= t.push(x));
        lemma_gen_group(t, g, vs, vv);
        lemma_sum_push(t, x, |y: A| if g(y) { vs(y) } else { 0 });
        lemma_sum_push(t, x, |y: A| if g(y) { vv(y) } else { 0 });
        lemma_sum_push(t, x, |y: A| if g(y) { abs(vv(y)) } else { 0 });
        if gcnt(s, g, vs) == 1 && !(g(x) && vs(x) == 1) {
            assert(gcnt(t, g, vs) == 1);
            let i = choose|i: int| 0 <= i < t.len() && g(#[trigger] t[i]) && vs(t[i]) == 1 && gsm(t, g, vv) == vv(t[i]);
            assert(s[i] == t[i]);
        }
    } else {
        assert(s =~= Seq::<A>::empty());
    }
}

/// A row of num occurs in the tuples.
proof fn lemma_tuple_num(db: Db, p: Params, t: Tuple)
    requires tuples(db, p).contains(t),
    ensures exists|i: int| 0 <= i < db.num.len() && db.num[i] == t.n,
{
    let ms = db.num.map_values(|n: NumRow| tuples_1(db, p, n));
    lemma_flat_contains(ms, t);
    let i = choose|i: int| 0 <= i < ms.len() && (#[trigger] ms[i]).contains(t);
    let n = db.num[i];
    assert(ms[i] == tuples_1(db, p, n));
    let ss = db.sub.map_values(|s: SubRow| tuples_2(db, p, n, s));
    lemma_flat_contains(ss, t);
    let j = choose|j: int| 0 <= j < ss.len() && (#[trigger] ss[j]).contains(t);
    assert(ss[j] == tuples_2(db, p, n, db.sub[j]));
    assert(t.n == n);
}

proof fn lemma_avg_tuple_num(db: Db, p: Params, t: AvgSubTuple)
    requires avg_sub_tuples(db, p).contains(t),
    ensures exists|i: int| 0 <= i < db.num.len() && db.num[i] == t.n2,
{
    let ms = db.num.map_values(|n: NumRow| avg_sub_tuples_1(db, p, n));
    lemma_flat_contains(ms, t);
    let i = choose|i: int| 0 <= i < ms.len() && (#[trigger] ms[i]).contains(t);
    let n = db.num[i];
    assert(ms[i] == avg_sub_tuples_1(db, p, n));
    let ss = db.sub.map_values(|s: SubRow| avg_sub_tuples_2(db, p, n, s));
    lemma_flat_contains(ss, t);
    let j = choose|j: int| 0 <= j < ss.len() && (#[trigger] ss[j]).contains(t);
    assert(ss[j] == avg_sub_tuples_2(db, p, n, db.sub[j]));
    assert(t.n2 == n);
}

/// Every value in num is some bit pattern's.
pub open spec fn nums_dbl(db: Db) -> bool {
    forall|i: int| 0 <= i < db.num.len() ==> ((#[trigger] db.num[i]).value is Some ==> dbl_raw(db.num[i].value->0))
}

/// What the kernel needs of a group's quantities: a count is not negative, a sum is within the sum of magnitudes,
/// an empty group has zero sums, and a group of one tuple sums to a double.
pub proof fn lemma_group_facts(db: Db, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>)
    requires nums_dbl(db),
    ensures ({
        let q = qj(db, p, inner, gn, gc);
        &&& q.n >= 0
        &&& abs(q.s) <= q.a
        &&& q.n == 0 ==> q.s == 0 && q.a == 0
        &&& q.n == 1 ==> dbl_raw(q.s)
    }),
{
    if inner {
        let tup = avg_sub_tuples(db, p);
        let g = |t: AvgSubTuple| t.s2.cik == gc;
        lemma_inner_join(db, p, gc);
        lemma_gen_group(tup, g, avg_vs(), avg_value());
        assert(avg_sub_group_sum(db, p, gc, avg_vs()) == gcnt(tup, g, avg_vs())) by {
            lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_vs()(t) } else { 0 }, |t: AvgSubTuple| if g(t) { avg_vs()(t) } else { 0 });
        }
        assert(avg_sub_group_sum(db, p, gc, avg_value()) == gsm(tup, g, avg_value())) by {
            lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_value()(t) } else { 0 }, |t: AvgSubTuple| if g(t) { avg_value()(t) } else { 0 });
        }
        assert(avg_sub_group_sum(db, p, gc, avg_mag()) == gab(tup, g, avg_value())) by {
            lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_mag()(t) } else { 0 }, |t: AvgSubTuple| if g(t) { abs(avg_value()(t)) } else { 0 });
        }
        if avg_sub_group_sum(db, p, gc, avg_vs()) == 1 {
            let i = choose|i: int| 0 <= i < tup.len() && g(#[trigger] tup[i]) && avg_vs()(tup[i]) == 1 && gsm(tup, g, avg_value()) == avg_value()(tup[i]);
            assert(tup.contains(tup[i]));
            lemma_avg_tuple_num(db, p, tup[i]);
            let k = choose|k: int| 0 <= k < db.num.len() && db.num[k] == tup[i].n2;
            assert(tup[i].n2.value is Some);
            assert(dbl_raw(db.num[k].value->0));
            assert(avg_value()(tup[i]) == tup[i].n2.value->0);
        }
        assert(gh(true, gn, gc, fvs()) =~= gh(true, None, gc, fvs()));
        assert(gh(true, gn, gc, fv()) =~= gh(true, None, gc, fv()));
        assert(gh(true, gn, gc, fm()) =~= gh(true, None, gc, fm()));
    } else {
        let tup = tuples(db, p);
        let g = |t: Tuple| t.s_.name == gn && t.s_.cik == gc;
        lemma_outer_join(db, p, gn, gc);
        lemma_gen_group(tup, g, tup_vs(), tup_value());
        assert(group_sum(db, p, gn, gc, tup_vs()) == gcnt(tup, g, tup_vs())) by {
            lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_vs()(t) } else { 0 }, |t: Tuple| if g(t) { tup_vs()(t) } else { 0 });
        }
        assert(group_sum(db, p, gn, gc, tup_value()) == gsm(tup, g, tup_value())) by {
            lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_value()(t) } else { 0 }, |t: Tuple| if g(t) { tup_value()(t) } else { 0 });
        }
        assert(group_sum(db, p, gn, gc, tup_mag()) == gab(tup, g, tup_value())) by {
            lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_mag()(t) } else { 0 }, |t: Tuple| if g(t) { abs(tup_value()(t)) } else { 0 });
        }
        if group_sum(db, p, gn, gc, tup_vs()) == 1 {
            let i = choose|i: int| 0 <= i < tup.len() && g(#[trigger] tup[i]) && tup_vs()(tup[i]) == 1 && gsm(tup, g, tup_value()) == tup_value()(tup[i]);
            assert(tup.contains(tup[i]));
            lemma_tuple_num(db, p, tup[i]);
            let k = choose|k: int| 0 <= k < db.num.len() && db.num[k] == tup[i].n;
            assert(tup[i].n.value is Some);
            assert(dbl_raw(db.num[k].value->0));
            assert(tup_value()(tup[i]) == tup[i].n.value->0);
        }
    }
}


pub proof fn lemma_gh_ext(inner: bool, gn: Option<Seq<u8>>, gn2: Option<Seq<u8>>, gc: Option<int>, f: spec_fn(JK) -> int)
    requires inner || gn == gn2,
    ensures gh(inner, gn, gc, f) == gh(inner, gn2, gc, f),
{
    assert(gh(inner, gn, gc, f) =~= gh(inner, gn2, gc, f));
}


/// Groups that are the same group have the same quantities.
pub proof fn lemma_qj_norm(db: Db, p: Params, inner: bool, gn: Option<Seq<u8>>, gc: Option<int>, gn2: Option<Seq<u8>>)
    requires inner || gn == gn2,
    ensures qj(db, p, inner, gn, gc) == qj(db, p, inner, gn2, gc),
{
    lemma_gh_ext(inner, gn, gn2, gc, fvs());
    lemma_gh_ext(inner, gn, gn2, gc, fv());
    lemma_gh_ext(inner, gn, gn2, gc, fm());
}


proof fn lemma_tuple_where(db: Db, p: Params, t: Tuple)
    requires tuples(db, p).contains(t),
    ensures where_clause(db, p, t.n, t.s_),
{
    let ms = db.num.map_values(|n: NumRow| tuples_1(db, p, n));
    lemma_flat_contains(ms, t);
    let i = choose|i: int| 0 <= i < ms.len() && (#[trigger] ms[i]).contains(t);
    let n = db.num[i];
    assert(ms[i] == tuples_1(db, p, n));
    let ss = db.sub.map_values(|s: SubRow| tuples_2(db, p, n, s));
    lemma_flat_contains(ss, t);
    let j = choose|j: int| 0 <= j < ss.len() && (#[trigger] ss[j]).contains(t);
    assert(ss[j] == tuples_2(db, p, n, db.sub[j]));
}

proof fn lemma_avg_tuple_where(db: Db, p: Params, t: AvgSubTuple)
    requires avg_sub_tuples(db, p).contains(t),
    ensures avg_sub_where_clause(db, p, t.n2, t.s2),
{
    let ms = db.num.map_values(|n: NumRow| avg_sub_tuples_1(db, p, n));
    lemma_flat_contains(ms, t);
    let i = choose|i: int| 0 <= i < ms.len() && (#[trigger] ms[i]).contains(t);
    let n = db.num[i];
    assert(ms[i] == avg_sub_tuples_1(db, p, n));
    let ss = db.sub.map_values(|s: SubRow| avg_sub_tuples_2(db, p, n, s));
    lemma_flat_contains(ss, t);
    let j = choose|j: int| 0 <= j < ss.len() && (#[trigger] ss[j]).contains(t);
    assert(ss[j] == avg_sub_tuples_2(db, p, n, db.sub[j]));
}

/// Every tuple has a value, so counting tuples counts values.
pub proof fn lemma_count_vs(db: Db, p: Params, gn: Option<Seq<u8>>, gc: Option<int>)
    ensures
        group_count(db, p, gn, gc) == group_sum(db, p, gn, gc, tup_vs()),
        avg_sub_group_count(db, p, gc) == avg_sub_group_sum(db, p, gc, avg_vs()),
{
    let tup = tuples(db, p);
    assert forall|i: int| 0 <= i < tup.len() implies
        (|t: Tuple| if t.s_.name == gn && t.s_.cik == gc { 1int } else { 0int })(#[trigger] tup[i])
        == (|t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_vs()(t) } else { 0int })(tup[i]) by {
        assert(tup.contains(tup[i]));
        lemma_tuple_where(db, p, tup[i]);
    }
    lemma_sum_ext(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { 1int } else { 0int },
        |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_vs()(t) } else { 0int });
    let av = avg_sub_tuples(db, p);
    assert forall|i: int| 0 <= i < av.len() implies
        (|t: AvgSubTuple| if t.s2.cik == gc { 1int } else { 0int })(#[trigger] av[i])
        == (|t: AvgSubTuple| if t.s2.cik == gc { avg_vs()(t) } else { 0int })(av[i]) by {
        assert(av.contains(av[i]));
        lemma_avg_tuple_where(db, p, av[i]);
    }
    lemma_sum_ext(av, |t: AvgSubTuple| if t.s2.cik == gc { 1int } else { 0int },
        |t: AvgSubTuple| if t.s2.cik == gc { avg_vs()(t) } else { 0int });
}

/// A group's count is within its company's.
pub proof fn lemma_group_le_company(db: Db, p: Params, gn: Option<Seq<u8>>, gc: Option<int>)
    ensures qj(db, p, false, gn, gc).n <= qj(db, p, true, None, gc).n,
{
    let hout = gh(false, gn, gc, fvs());
    let hin = gh(true, None, gc, fvs());
    lemma_jq_tuples(db, p, hout);
    lemma_jq_tuples(db, p, hin);
    assert forall|t: Tuple| #[trigger] hout(pj(t)) <= hin(pj(t)) by {}
    lemma_sum_le(tuples(db, p), |tp: Tuple| hout(pj(tp)), |tp: Tuple| hin(pj(tp)));
}

}
