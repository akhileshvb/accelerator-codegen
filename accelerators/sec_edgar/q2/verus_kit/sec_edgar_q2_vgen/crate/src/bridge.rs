// The generated answer rows, as weighted sums over the tables (class_w).
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::order::*;
use verus_common::perm::*;
use crate::gen_spec::*;
use crate::model::*;

verus! {

pub open spec fn m_in(params: Params, r: NumRow) -> bool {
    r.uom == Some(params.p2) && r.value is Some
}

pub proof fn lemma_m_tuples_sum(db: Db, params: Params, f: spec_fn(MTuple) -> int)
    ensures
        sum(m__tuples(db, params), f) == sum(db.num, |r: NumRow| if m_in(params, r) { f(MTuple { num: r }) } else { 0 }),
{
    let g = |r: NumRow| m__tuples_1(db, params, r);
    lemma_sum_flat_map(db.num, g, f);
    assert(m__tuples(db, params) == flat(db.num.map_values(g)));
    let per = |a: NumRow| sum(g(a), f);
    let target = |r: NumRow| if m_in(params, r) { f(MTuple { num: r }) } else { 0 };
    assert forall|a: NumRow| #[trigger] per(a) == target(a) by {
        lemma_sum_opt(m__where_clause(db, params, a), MTuple { num: a }, f);
        assert(m__where_clause(db, params, a) == m_in(params, a));
    }
    lemma_sum_ext_all(db.num, per, target);
}

pub open spec fn in_grp(params: Params, r: NumRow, ga: Option<Seq<u8>>, gt: Option<Seq<u8>>) -> bool {
    m_in(params, r) && r.adsh == ga && r.tag == gt
}

/// The values of group (ga, gt), in terms of the table's rows.
pub proof fn lemma_vals_contain(db: Db, params: Params, ga: Option<Seq<u8>>, gt: Option<Seq<u8>>, y: int)
    ensures
        m__group_vals_where(db, params, ga, gt, |t: MTuple| t.num.value is Some, |t: MTuple| t.num.value->0).contains(y)
            <==> exists|i: int| 0 <= i < db.num.len() && in_grp(params, #[trigger] db.num[i], ga, gt) && db.num[i].value->0 == y,
{
    let ts = m__tuples(db, params);
    let g = |r: NumRow| m__tuples_1(db, params, r);
    let keep = |t: MTuple| t.num.value is Some;
    let f = |t: MTuple| t.num.value->0;
    let pred = |t: MTuple| t.num.adsh == ga && t.num.tag == gt && keep(t);
    lemma_map_where(ts, pred, f);
    assert(ts == flat(db.num.map_values(g)));
    if m__group_vals_where(db, params, ga, gt, keep, f).contains(y) {
        let k = choose|k: int| 0 <= k < ts.len() && pred(ts[k]) && f(ts[k]) == y;
        assert(ts.contains(ts[k]));
        lemma_flat_contains(db.num.map_values(g), ts[k]);
        let i = choose|i: int| 0 <= i < db.num.map_values(g).len() && (#[trigger] db.num.map_values(g)[i]).contains(ts[k]);
        assert(db.num.map_values(g)[i] == g(db.num[i]));
        assert(m__where_clause(db, params, db.num[i]));
        assert(g(db.num[i]) == seq![MTuple { num: db.num[i] }]);
        assert(ts[k] == MTuple { num: db.num[i] }) by {
            let s1 = seq![MTuple { num: db.num[i] }];
            assert(s1.contains(ts[k]));
            let j = choose|j: int| 0 <= j < s1.len() && s1[j] == ts[k];
            assert(s1[0] == MTuple { num: db.num[i] });
        }
        assert(in_grp(params, db.num[i], ga, gt) && db.num[i].value->0 == y);
    }
    if exists|i: int| 0 <= i < db.num.len() && in_grp(params, #[trigger] db.num[i], ga, gt) && db.num[i].value->0 == y {
        let i = choose|i: int| 0 <= i < db.num.len() && in_grp(params, #[trigger] db.num[i], ga, gt) && db.num[i].value->0 == y;
        let t = MTuple { num: db.num[i] };
        assert(g(db.num[i]) == seq![t]);
        assert(db.num.map_values(g)[i] == seq![t]);
        assert(seq![t].contains(t)) by { assert(seq![t][0] == t); }
        lemma_flat_contains(db.num.map_values(g), t);
        let k = choose|k: int| 0 <= k < ts.len() && ts[k] == t;
        assert(pred(ts[k]) && f(ts[k]) == y);
    }
}

/// n heads its group's maximum row exactly when the generated group maximum is n's value.
pub proof fn lemma_group_max(db: Db, params: Params, n: NumRow)
    requires
        db.num.contains(n),
        is_qual(n),
        n.uom == Some(params.p2),
    ensures
        m__row_of(db, params, n.adsh, n.tag).max_value == Some(n.value->0) <==> is_max(db.num, n),
{
    let (ga, gt) = (n.adsh, n.tag);
    let f1 = |t: MTuple| if t.num.value is Some { 1int } else { 0 };
    let cnt = |t: MTuple| if t.num.adsh == ga && t.num.tag == gt { f1(t) } else { 0 };
    assert(m__group_sum(db, params, ga, gt, f1) == sum(m__tuples(db, params), cnt));
    lemma_m_tuples_sum(db, params, cnt);
    let w = |r: NumRow| if m_in(params, r) { cnt(MTuple { num: r }) } else { 0 };
    let i0 = choose|i: int| 0 <= i < db.num.len() && db.num[i] == n;
    assert forall|i: int| 0 <= i < db.num.len() implies w(#[trigger] db.num[i]) >= 0 by {}
    assert(w(db.num[i0]) == 1);
    lemma_sum_pos(db.num, w, i0);
    let vals = m__group_vals_where(db, params, ga, gt, |t: MTuple| t.num.value is Some, |t: MTuple| t.num.value->0);
    lemma_vals_contain(db, params, ga, gt, n.value->0);
    assert(vals.contains(n.value->0)) by { assert(in_grp(params, db.num[i0], ga, gt)); }
    lemma_max_of_props(vals);
    let mx = max_of(vals);
    assert(m__row_of(db, params, ga, gt).max_value == Some(mx));
    lemma_vals_contain(db, params, ga, gt, mx);
    let j = choose|j: int| 0 <= j < db.num.len() && in_grp(params, #[trigger] db.num[j], ga, gt) && db.num[j].value->0 == mx;
    let vi = vals.index_of(n.value->0);
    assert(vals[vi] == n.value->0);
    if mx == n.value->0 {
        assert forall|r: NumRow| db.num.contains(r) && same_grp(n, r) && r.value is Some implies #[trigger] r.value->0 <= n.value->0 by {
            let k = choose|k: int| 0 <= k < db.num.len() && db.num[k] == r;
            assert(in_grp(params, db.num[k], ga, gt));
            lemma_vals_contain(db, params, ga, gt, r.value->0);
            let p = choose|p: int| 0 <= p < vals.len() && vals[p] == r.value->0;
            assert(vals[p] <= mx);
        }
    }
    if is_max(db.num, n) {
        assert(db.num.contains(db.num[j]));
        assert(same_grp(n, db.num[j]) && db.num[j].value is Some);
        assert(mx <= n.value->0);
        assert(vals[vi] <= mx);
    }
}


pub open spec fn m_keys(db: Db, params: Params) -> Seq<(Option<Seq<u8>>, Option<Seq<u8>>)> {
    dedup(map_where(m__tuples(db, params), |t: MTuple| m__having(db, params, t.num.adsh, t.num.tag), |t: MTuple| (t.num.adsh, t.num.tag)))
}

pub proof fn lemma_m_has_key(db: Db, params: Params, n: NumRow)
    requires
        db.num.contains(n),
        m_in(params, n),
    ensures
        m_keys(db, params).contains((n.adsh, n.tag)),
{
    let ts = m__tuples(db, params);
    let g = |r: NumRow| m__tuples_1(db, params, r);
    let i = choose|i: int| 0 <= i < db.num.len() && db.num[i] == n;
    assert(m__where_clause(db, params, n));
    assert(g(n) == seq![MTuple { num: n }]);
    assert(db.num.map_values(g)[i] == seq![MTuple { num: n }]);
    assert(seq![MTuple { num: n }].contains(MTuple { num: n })) by { assert(seq![MTuple { num: n }][0] == MTuple { num: n }); }
    lemma_flat_contains(db.num.map_values(g), MTuple { num: n });
    assert(ts == flat(db.num.map_values(g)));
    let k = choose|k: int| 0 <= k < ts.len() && ts[k] == MTuple { num: n };
    let keep = |t: MTuple| m__having(db, params, t.num.adsh, t.num.tag);
    let proj = |t: MTuple| (t.num.adsh, t.num.tag);
    lemma_map_where(ts, keep, proj);
    assert(keep(ts[k]) && proj(ts[k]) == (n.adsh, n.tag));
    lemma_dedup(map_where(ts, keep, proj));
}

/// The rows the query yields for a pair (n, s), as a weighted sum.
pub proof fn lemma_pair_sum(db: Db, params: Params, n: NumRow, s: SubRow, f: spec_fn(OutRow) -> int)
    requires
        db.num.contains(n),
    ensures
        ({
            let ff = |t: Tuple| f(OutRow { name: t.s_.name, tag: t.n.tag, value: t.n.value });
            sum(tuples_2(db, params, n, s), ff)
                == if picked(db.num, n, params.p2) && sub_hits(n, s, params.p1) { f(ent_row(n, s)) } else { 0 }
        }),
{
    let ff = |t: Tuple| f(OutRow { name: t.s_.name, tag: t.n.tag, value: t.n.value });
    let g3 = |m_: MRow| tuples_3(db, params, n, s, m_);
    lemma_sum_flat_map(rel_m_(db, params), g3, ff);
    assert(tuples_2(db, params, n, s) == flat(rel_m_(db, params).map_values(g3)));
    let ks = m_keys(db, params);
    let row_of = |k: (Option<Seq<u8>>, Option<Seq<u8>>)| m__row_of(db, params, k.0, k.1);
    assert(rel_m_(db, params) == ks.map_values(row_of));
    let per_m = |m_: MRow| sum(g3(m_), ff);
    let per_k = |k: (Option<Seq<u8>>, Option<Seq<u8>>)| per_m(row_of(k));
    lemma_sum_map(ks, row_of, per_m, per_k);
    lemma_dedup(map_where(m__tuples(db, params), |t: MTuple| m__having(db, params, t.num.adsh, t.num.tag), |t: MTuple| (t.num.adsh, t.num.tag)));
    let want = picked(db.num, n, params.p2) && sub_hits(n, s, params.p1);
    assert forall|m_: MRow| #[trigger] per_m(m_) == if where_clause(db, params, n, s, m_) { f(ent_row(n, s)) } else { 0 } by {
        lemma_sum_opt(where_clause(db, params, n, s, m_), Tuple { n, s_: s, m_ }, ff);
    }
    if want {
        let k0 = (n.adsh, n.tag);
        lemma_m_has_key(db, params, n);
        lemma_group_max(db, params, n);
        let h = |k: (Option<Seq<u8>>, Option<Seq<u8>>)| if k == k0 { f(ent_row(n, s)) } else { 0 };
        assert forall|k: (Option<Seq<u8>>, Option<Seq<u8>>)| #[trigger] per_k(k) == h(k) by {
            let m_ = row_of(k);
            assert(m_.adsh == k.0 && m_.tag == k.1);
            if k == k0 {
                assert(where_clause(db, params, n, s, m_));
            } else {
                assert(!where_clause(db, params, n, s, m_));
            }
        }
        lemma_sum_ext_all(ks, per_k, h);
        lemma_sum_indicator_distinct(ks, k0, f(ent_row(n, s)), h);
    } else {
        assert forall|k: (Option<Seq<u8>>, Option<Seq<u8>>)| #[trigger] per_k(k) == 0 by {
            let m_ = row_of(k);
            assert(m_.adsh == k.0 && m_.tag == k.1);
            if where_clause(db, params, n, s, m_) {
                assert(is_qual(n) && n.uom == Some(params.p2));
                lemma_group_max(db, params, n);
                assert(k == (n.adsh, n.tag));
            }
        }
        lemma_sum_zero(ks, per_k);
    }
}

/// The answer rows, summed with any weight.
pub proof fn lemma_rows_sum(db: Db, params: Params, f: spec_fn(OutRow) -> int)
    ensures
        sum(rows(db, params), f) == class_w(db, params.p1, params.p2, f),
{
    let proj = |t: Tuple| OutRow { name: t.s_.name, tag: t.n.tag, value: t.n.value };
    let ff = |t: Tuple| f(OutRow { name: t.s_.name, tag: t.n.tag, value: t.n.value });
    lemma_sum_map(tuples(db, params), proj, f, ff);
    assert(rows(db, params) == tuples(db, params).map_values(proj));
    let g1 = |n: NumRow| tuples_1(db, params, n);
    lemma_sum_flat_map(db.num, g1, ff);
    assert(tuples(db, params) == flat(db.num.map_values(g1)));
    let target = |n: NumRow| if picked(db.num, n, params.p2) { pair_w(db.sub, n, params.p1, f) } else { 0 };
    let per_n = |n: NumRow| sum(g1(n), ff);
    assert forall|i: int| 0 <= i < db.num.len() implies #[trigger] per_n(db.num[i]) == target(db.num[i]) by {
        let n = db.num[i];
        assert(db.num.contains(n));
        let g2 = |s: SubRow| tuples_2(db, params, n, s);
        lemma_sum_flat_map(db.sub, g2, ff);
        assert(g1(n) == flat(db.sub.map_values(g2)));
        let per_s = |s: SubRow| sum(g2(s), ff);
        let tgt_s = |s: SubRow| if sub_hits(n, s, params.p1) { f(ent_row(n, s)) } else { 0 };
        assert forall|s: SubRow| #[trigger] per_s(s) == if picked(db.num, n, params.p2) { tgt_s(s) } else { 0 } by {
            lemma_pair_sum(db, params, n, s, f);
        }
        if picked(db.num, n, params.p2) {
            lemma_sum_ext_all(db.sub, per_s, tgt_s);
        } else {
            let z = |s: SubRow| 0int;
            lemma_sum_ext_all(db.sub, per_s, z);
            lemma_sum_zero(db.sub, z);
        }
    }
    lemma_sum_ext(db.num, per_n, target);
}

} // verus!
