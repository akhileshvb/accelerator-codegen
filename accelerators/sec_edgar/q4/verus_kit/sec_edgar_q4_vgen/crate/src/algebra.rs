// How the join's sum changes with a transaction: the new sum minus the old one is computed from the
// rescanned rows of the affected filings (and tags) alone, as signed passes of those rows through the
// join with the new and the old side tables.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::join_spec::*;
use crate::jspec::*;

verus! {

pub open spec fn kin(keys: Seq<Seq<u8>>, x: Option<Seq<u8>>) -> bool {
    x is Some && keys.contains(x->0)
}

/// A num row of a filing or a tag that changed.
pub open spec fn aff_n(a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, n: NumRow) -> bool {
    kin(a, n.adsh) || kin(g, n.tag)
}

pub open spec fn aff_r(a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, r: PreRow) -> bool {
    kin(a, r.adsh) || kin(g, r.tag)
}

/// Σ sign · f(row) over a signed list.
pub open spec fn wsum<A>(l: Seq<(A, int)>, f: spec_fn(A) -> int) -> int {
    sum(l, |x: (A, int)| x.1 * f(x.0))
}

/// Pre rows with a sign each.
pub type Sides = Seq<(Seq<PreRow>, int)>;

pub open spec fn psum(sides: Sides, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow) -> int {
    sum(sides, |sd: (Seq<PreRow>, int)| sd.1 * pre_sum(sd.0, p, h, n, s, t))
}

pub open spec fn tsum(tag: Seq<TagRow>, sides: Sides, p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow) -> int {
    sum(tag, |t: TagRow| psum(sides, p, h, n, s, t))
}

/// What one num row joins to, over the sub and tag tables and the signed pre rows.
pub open spec fn big_psi(sub: Seq<SubRow>, tag: Seq<TagRow>, sides: Sides, p: Params, h: spec_fn(JK) -> int, n: NumRow) -> int {
    sum(sub, |s: SubRow| tsum(tag, sides, p, h, n, s))
}

/// A pass of signed num rows through the join.
pub open spec fn passv(nl: Seq<(NumRow, int)>, sub: Seq<SubRow>, tag: Seq<TagRow>, sides: Sides, p: Params, h: spec_fn(JK) -> int) -> int {
    sum(nl, |x: (NumRow, int)| x.1 * big_psi(sub, tag, sides, p, h, x.0))
}

pub open spec fn new_sides(rtab: Seq<PreRow>) -> Sides {
    seq![(rtab, 1int)]
}

pub open spec fn old_sides(rtab: Seq<PreRow>, dp: Seq<(PreRow, int)>) -> Sides {
    seq![(rtab, 1int), (plus_rows(dp), -1int), (minus_rows(dp), 1int)]
}

pub open spec fn rtab_ok(a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, pre2: Seq<PreRow>, rtab: Seq<PreRow>) -> bool {
    forall|f: spec_fn(PreRow) -> int| #[trigger] sum(rtab, f) == sum(pre2, |r: PreRow| if aff_r(a, g, r) { f(r) } else { 0 })
}

proof fn lemma_sum3<A>(x: A, y: A, z: A, f: spec_fn(A) -> int)
    ensures sum(seq![x, y, z], f) == f(x) + f(y) + f(z),
{
    assert(seq![x, y, z] =~= seq![x, y].push(z));
    assert(seq![x, y] =~= seq![x].push(y));
    lemma_sum_push(seq![x, y], z, f);
    lemma_sum_push(seq![x], y, f);
    lemma_sum_singleton(x, f);
}

proof fn lemma_sum_all_zero<A>(s: Seq<A>, f: spec_fn(A) -> int)
    requires forall|x: A| #[trigger] f(x) == 0,
    ensures sum(s, f) == 0,
{
    lemma_sum_zero(s, f);
}

/// Changed rows that contribute nothing leave the sum as it was.
proof fn lemma_bag_zero<A>(before: Seq<A>, after: Seq<A>, d: Seq<(A, int)>, f: spec_fn(A) -> int)
    requires
        bag_step(before, after, d),
        forall|i: int| 0 <= i < d.len() ==> f(#[trigger] d[i].0) == 0,
    ensures
        sum(after, f) == sum(before, f),
{
    let w = |x: (A, int)| x.1 * f(x.0);
    assert forall|i: int| 0 <= i < d.len() implies w(#[trigger] d[i]) == 0 by {
        assert(f(d[i].0) == 0);
    }
    lemma_sum_zero(d, w);
}

/// A num row with no adsh joins to nothing.
proof fn lemma_null_adsh(p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow, r: PreRow)
    requires n.adsh is None,
    ensures psi(p, h, n, s, t, r) == 0,
{
}

/// The old pre rows of a num row's join: the rescanned rows without the inserted ones, with the deleted.
proof fn lemma_pre_old(a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, pre: Seq<PreRow>, pre2: Seq<PreRow>, dp: Seq<(PreRow, int)>, rtab: Seq<PreRow>,
    p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow)
    requires
        bag_step(pre, pre2, dp),
        unit_signs(dp),
        rtab_ok(a, g, pre2, rtab),
        aff_n(a, g, n) || n.adsh is None,
    ensures
        pre_sum(pre, p, h, n, s, t) == psum(old_sides(rtab, dp), p, h, n, s, t),
{
    let f = |r: PreRow| psi(p, h, n, s, t, r);
    lemma_delta_split(dp, f);
    assert(sum(pre2, f) == sum(pre, f) + sum(dp, |x: (PreRow, int)| x.1 * f(x.0)));
    let cut = |r: PreRow| if aff_r(a, g, r) { f(r) } else { 0 };
    assert forall|r: PreRow| #[trigger] cut(r) == f(r) by {
        if n.adsh is None {
            lemma_null_adsh(p, h, n, s, t, r);
        } else if f(r) != 0 {
            assert(wc(p, n, s, t, r));
        }
    }
    lemma_sum_ext_all(pre2, cut, f);
    assert(sum(rtab, f) == sum(pre2, cut));
    let sd = |x: (Seq<PreRow>, int)| x.1 * pre_sum(x.0, p, h, n, s, t);
    lemma_sum3((rtab, 1int), (plus_rows(dp), -1int), (minus_rows(dp), 1int), sd);
    assert(sd((rtab, 1int)) == sum(rtab, f));
    assert(sd((plus_rows(dp), -1int)) == -sum(plus_rows(dp), f));
    assert(sd((minus_rows(dp), 1int)) == sum(minus_rows(dp), f));
}

proof fn lemma_pre_new(a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, pre2: Seq<PreRow>, rtab: Seq<PreRow>,
    p: Params, h: spec_fn(JK) -> int, n: NumRow, s: SubRow, t: TagRow)
    requires
        rtab_ok(a, g, pre2, rtab),
        aff_n(a, g, n),
    ensures
        pre_sum(pre2, p, h, n, s, t) == psum(new_sides(rtab), p, h, n, s, t),
{
    let f = |r: PreRow| psi(p, h, n, s, t, r);
    let cut = |r: PreRow| if aff_r(a, g, r) { f(r) } else { 0 };
    assert forall|r: PreRow| #[trigger] cut(r) == f(r) by {
        if f(r) != 0 {
            assert(wc(p, n, s, t, r));
        }
    }
    lemma_sum_ext_all(pre2, cut, f);
    let sd = |x: (Seq<PreRow>, int)| x.1 * pre_sum(x.0, p, h, n, s, t);
    lemma_sum_singleton((rtab, 1int), sd);
}

/// A num row that is in no changed filing and has no changed tag joins as before.
proof fn lemma_unaffected(db: Db, db2: Db, dp: Seq<(PreRow, int)>, ds: Seq<(SubRow, int)>, dt: Seq<(TagRow, int)>,
    a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, p: Params, h: spec_fn(JK) -> int, n: NumRow)
    requires
        bag_step(db.pre, db2.pre, dp),
        bag_step(db.sub, db2.sub, ds),
        bag_step(db.tag, db2.tag, dt),
        forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).0.adsh is Some ==> a.contains(dp[i].0.adsh->0),
        forall|i: int| 0 <= i < ds.len() ==> a.contains((#[trigger] ds[i]).0.adsh),
        forall|i: int| 0 <= i < dt.len() ==> g.contains((#[trigger] dt[i]).0.tag),
        !aff_n(a, g, n),
    ensures
        sub_sum4(db2.sub, db2.tag, db2.pre, p, h, n) == sub_sum4(db.sub, db.tag, db.pre, p, h, n),
{
    assert forall|s: SubRow, t: TagRow| #[trigger] pre_sum(db2.pre, p, h, n, s, t) == pre_sum(db.pre, p, h, n, s, t) by {
        let f = |r: PreRow| psi(p, h, n, s, t, r);
        assert forall|i: int| 0 <= i < dp.len() implies f(#[trigger] dp[i].0) == 0 by {
            if f(dp[i].0) != 0 {
                assert(wc(p, n, s, t, dp[i].0));
                assert(dp[i].0.adsh is Some);
                assert(kin(a, n.adsh));
            }
        }
        lemma_bag_zero(db.pre, db2.pre, dp, f);
    }
    assert forall|s: SubRow| #[trigger] tag_sum4(db2.tag, db2.pre, p, h, n, s) == tag_sum4(db.tag, db.pre, p, h, n, s) by {
        let x = |t: TagRow| pre_sum(db.pre, p, h, n, s, t);
        lemma_sum_ext_all(db2.tag, |t: TagRow| pre_sum(db2.pre, p, h, n, s, t), x);
        assert forall|i: int| 0 <= i < dt.len() implies x(#[trigger] dt[i].0) == 0 by {
            let t = dt[i].0;
            assert forall|r: PreRow| #[trigger] psi(p, h, n, s, t, r) == 0 by {
                if psi(p, h, n, s, t, r) != 0 {
                    assert(wc(p, n, s, t, r));
                    assert(kin(g, n.tag));
                }
            }
            lemma_sum_all_zero(db.pre, |r: PreRow| psi(p, h, n, s, t, r));
        }
        lemma_bag_zero(db.tag, db2.tag, dt, x);
    }
    let y = |s: SubRow| tag_sum4(db.tag, db.pre, p, h, n, s);
    lemma_sum_ext_all(db2.sub, |s: SubRow| tag_sum4(db2.tag, db2.pre, p, h, n, s), y);
    assert forall|i: int| 0 <= i < ds.len() implies y(#[trigger] ds[i].0) == 0 by {
        let s = ds[i].0;
        assert forall|t: TagRow| #[trigger] pre_sum(db.pre, p, h, n, s, t) == 0 by {
            assert forall|r: PreRow| #[trigger] psi(p, h, n, s, t, r) == 0 by {
                if psi(p, h, n, s, t, r) != 0 {
                    assert(wc(p, n, s, t, r));
                    assert(kin(a, n.adsh));
                }
            }
            lemma_sum_all_zero(db.pre, |r: PreRow| psi(p, h, n, s, t, r));
        }
        lemma_sum_all_zero(db.tag, |t: TagRow| pre_sum(db.pre, p, h, n, s, t));
    }
    lemma_bag_zero(db.sub, db2.sub, ds, y);
}

/// From the rows a rescan returns: each one has the key the rescan asked for.
pub proof fn lemma_rescan_rows<A>(rows: Seq<A>, tab: Seq<A>, inkey: spec_fn(A) -> bool)
    requires
        forall|f: spec_fn(A) -> int| #[trigger] sum(rows, f) == sum(tab, |x: A| if inkey(x) { f(x) } else { 0 }),
    ensures
        forall|i: int| 0 <= i < rows.len() ==> inkey(#[trigger] rows[i]),
{
    assert forall|i: int| 0 <= i < rows.len() implies inkey(#[trigger] rows[i]) by {
        if !inkey(rows[i]) {
            let f = |x: A| if x == rows[i] { 1int } else { 0int };
            assert forall|j: int| 0 <= j < rows.len() implies f(#[trigger] rows[j]) >= 0 by {}
            lemma_sum_ge_term(rows, f, i);
            let cut = |x: A| if inkey(x) { f(x) } else { 0 };
            assert forall|x: A| #[trigger] cut(x) == 0 by {
                if inkey(x) { assert(x != rows[i]); }
            }
            lemma_sum_all_zero(tab, cut);
            assert(sum(rows, f) == sum(tab, cut));
        }
    }
}

pub open spec fn psi_of(db: Db, p: Params, h: spec_fn(JK) -> int) -> spec_fn(NumRow) -> int {
    |n: NumRow| sub_sum4(db.sub, db.tag, db.pre, p, h, n)
}

pub open spec fn pdiff(db: Db, db2: Db, p: Params, h: spec_fn(JK) -> int) -> spec_fn(NumRow) -> int {
    |n: NumRow| psi_of(db2, p, h)(n) - psi_of(db, p, h)(n)
}

proof fn lemma_mul_sub(w: int, a: int, b: int)
    ensures w * (a - b) == w * a - w * b,
{
    assert(w * (a - b) == w * a - w * b) by (nonlinear_arith);
}

proof fn lemma_old_join(db: Db, db2: Db, dp: Seq<(PreRow, int)>, a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, rtab: Seq<PreRow>,
    p: Params, h: spec_fn(JK) -> int, n: NumRow)
    requires
        bag_step(db.pre, db2.pre, dp),
        unit_signs(dp),
        rtab_ok(a, g, db2.pre, rtab),
        aff_n(a, g, n) || n.adsh is None,
    ensures
        sub_sum4(db.sub, db.tag, db.pre, p, h, n) == big_psi(db.sub, db.tag, old_sides(rtab, dp), p, h, n),
{
    let old_s = old_sides(rtab, dp);
    assert forall|s: SubRow, t: TagRow| #[trigger] pre_sum(db.pre, p, h, n, s, t) == psum(old_s, p, h, n, s, t) by {
        lemma_pre_old(a, g, db.pre, db2.pre, dp, rtab, p, h, n, s, t);
    }
    assert forall|s: SubRow| #[trigger] tag_sum4(db.tag, db.pre, p, h, n, s) == tsum(db.tag, old_s, p, h, n, s) by {
        lemma_sum_ext_all(db.tag, |t: TagRow| pre_sum(db.pre, p, h, n, s, t), |t: TagRow| psum(old_s, p, h, n, s, t));
    }
    lemma_sum_ext_all(db.sub, |s: SubRow| tag_sum4(db.tag, db.pre, p, h, n, s),
        |s: SubRow| tsum(db.tag, old_s, p, h, n, s));
}

proof fn lemma_new_join(db2: Db, a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, rtab: Seq<PreRow>,
    p: Params, h: spec_fn(JK) -> int, n: NumRow)
    requires
        rtab_ok(a, g, db2.pre, rtab),
        aff_n(a, g, n),
    ensures
        sub_sum4(db2.sub, db2.tag, db2.pre, p, h, n) == big_psi(db2.sub, db2.tag, new_sides(rtab), p, h, n),
{
    let new_s = new_sides(rtab);
    assert forall|s: SubRow, t: TagRow| #[trigger] pre_sum(db2.pre, p, h, n, s, t) == psum(new_s, p, h, n, s, t) by {
        lemma_pre_new(a, g, db2.pre, rtab, p, h, n, s, t);
    }
    assert forall|s: SubRow| #[trigger] tag_sum4(db2.tag, db2.pre, p, h, n, s) == tsum(db2.tag, new_s, p, h, n, s) by {
        lemma_sum_ext_all(db2.tag, |t: TagRow| pre_sum(db2.pre, p, h, n, s, t), |t: TagRow| psum(new_s, p, h, n, s, t));
    }
    lemma_sum_ext_all(db2.sub, |s: SubRow| tag_sum4(db2.tag, db2.pre, p, h, n, s),
        |s: SubRow| tsum(db2.tag, new_s, p, h, n, s));
}

proof fn lemma_delta_num(
    db: Db, db2: Db,
    dn: Seq<(NumRow, int)>, dp: Seq<(PreRow, int)>, ds: Seq<(SubRow, int)>, dt: Seq<(TagRow, int)>,
    a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, nl: Seq<(NumRow, int)>,
    p: Params, h: spec_fn(JK) -> int,
)
    requires
        bag_step(db.num, db2.num, dn),
        bag_step(db.pre, db2.pre, dp),
        bag_step(db.sub, db2.sub, ds),
        bag_step(db.tag, db2.tag, dt),
        forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).0.adsh is Some ==> a.contains(dp[i].0.adsh->0),
        forall|i: int| 0 <= i < ds.len() ==> a.contains((#[trigger] ds[i]).0.adsh),
        forall|i: int| 0 <= i < dt.len() ==> g.contains((#[trigger] dt[i]).0.tag),
        forall|f: spec_fn(NumRow) -> int| #[trigger] wsum(nl, f)
            == sum(db2.num, |n: NumRow| if aff_n(a, g, n) { f(n) } else { 0 }),
    ensures
        jsum(db2.num, db2.sub, db2.tag, db2.pre, p, h) - jsum(db.num, db.sub, db.tag, db.pre, p, h)
            == wsum(nl, pdiff(db, db2, p, h)) + wsum(dn, psi_of(db, p, h)),
{
    let psi_old = psi_of(db, p, h);
    let psi_new = psi_of(db2, p, h);
    let diff = pdiff(db, db2, p, h);
    assert(jsum(db.num, db.sub, db.tag, db.pre, p, h) == sum(db.num, psi_old));
    assert(jsum(db2.num, db2.sub, db2.tag, db2.pre, p, h) == sum(db2.num, psi_new));
    assert(sum(db2.num, psi_old) == sum(db.num, psi_old) + wsum(dn, psi_old));
    lemma_sum_sub(db2.num, psi_new, psi_old, diff);
    let cut = |n: NumRow| if aff_n(a, g, n) { diff(n) } else { 0 };
    assert forall|n: NumRow| #[trigger] diff(n) == cut(n) by {
        if !aff_n(a, g, n) {
            lemma_unaffected(db, db2, dp, ds, dt, a, g, p, h, n);
        }
    }
    lemma_sum_ext_all(db2.num, diff, cut);
    assert(wsum(nl, diff) == sum(db2.num, cut));
    assert(sum(db2.num, diff) == sum(db2.num, cut));
}

proof fn lemma_delta_pass(
    db: Db, db2: Db, dp: Seq<(PreRow, int)>,
    a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, nl: Seq<(NumRow, int)>, rtab: Seq<PreRow>,
    p: Params, h: spec_fn(JK) -> int,
)
    requires
        bag_step(db.pre, db2.pre, dp),
        unit_signs(dp),
        forall|i: int| 0 <= i < nl.len() ==> (#[trigger] nl[i]).1 == 0 || aff_n(a, g, nl[i].0),
        rtab_ok(a, g, db2.pre, rtab),
    ensures
        wsum(nl, pdiff(db, db2, p, h))
            == passv(nl, db2.sub, db2.tag, new_sides(rtab), p, h) - passv(nl, db.sub, db.tag, old_sides(rtab, dp), p, h),
{
    let old_s = old_sides(rtab, dp);
    let new_s = new_sides(rtab);
    let diff = pdiff(db, db2, p, h);
    let wn = |x: (NumRow, int)| x.1 * big_psi(db2.sub, db2.tag, new_s, p, h, x.0);
    let wo = |x: (NumRow, int)| x.1 * big_psi(db.sub, db.tag, old_s, p, h, x.0);
    let wd = |x: (NumRow, int)| x.1 * diff(x.0);
    let wsub = |x: (NumRow, int)| wn(x) - wo(x);
    assert forall|i: int| 0 <= i < nl.len() implies #[trigger] wd(nl[i]) == wsub(nl[i]) by {
        let w = nl[i].1;
        let n = nl[i].0;
        if w != 0 {
            assert(aff_n(a, g, n));
            lemma_old_join(db, db2, dp, a, g, rtab, p, h, n);
            lemma_new_join(db2, a, g, rtab, p, h, n);
        }
        lemma_mul_sub(w, big_psi(db2.sub, db2.tag, new_s, p, h, n), big_psi(db.sub, db.tag, old_s, p, h, n));
        if w == 0 {
            assert(w * diff(n) == 0) by (nonlinear_arith) requires w == 0;
        }
    }
    lemma_sum_ext(nl, wd, wsub);
    lemma_sum_sub(nl, wn, wo, wsub);
}

proof fn lemma_delta_dn(
    db: Db, db2: Db, dn: Seq<(NumRow, int)>, dp: Seq<(PreRow, int)>,
    a: Seq<Seq<u8>>, g: Seq<Seq<u8>>, rtab: Seq<PreRow>,
    p: Params, h: spec_fn(JK) -> int,
)
    requires
        bag_step(db.pre, db2.pre, dp),
        unit_signs(dp),
        forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).0.adsh is Some ==> a.contains(dn[i].0.adsh->0),
        rtab_ok(a, g, db2.pre, rtab),
    ensures
        wsum(dn, psi_of(db, p, h)) == passv(dn, db.sub, db.tag, old_sides(rtab, dp), p, h),
{
    let old_s = old_sides(rtab, dp);
    let psi_old = psi_of(db, p, h);
    let wo = |x: (NumRow, int)| x.1 * big_psi(db.sub, db.tag, old_s, p, h, x.0);
    let wc_ = |x: (NumRow, int)| x.1 * psi_old(x.0);
    assert forall|i: int| 0 <= i < dn.len() implies #[trigger] wc_(dn[i]) == wo(dn[i]) by {
        let n = dn[i].0;
        if n.adsh is Some {
            assert(kin(a, n.adsh));
        }
        lemma_old_join(db, db2, dp, a, g, rtab, p, h, n);
    }
    lemma_sum_ext(dn, wc_, wo);
}

/// The change of the join's sum, from the rescanned rows.
pub proof fn lemma_delta(
    db: Db, db2: Db,
    dn: Seq<(NumRow, int)>, dp: Seq<(PreRow, int)>, ds: Seq<(SubRow, int)>, dt: Seq<(TagRow, int)>,
    a: Seq<Seq<u8>>, g: Seq<Seq<u8>>,
    nl: Seq<(NumRow, int)>, rtab: Seq<PreRow>,
    p: Params, h: spec_fn(JK) -> int,
)
    requires
        bag_step(db.num, db2.num, dn),
        bag_step(db.pre, db2.pre, dp),
        unit_signs(dp),
        bag_step(db.sub, db2.sub, ds),
        bag_step(db.tag, db2.tag, dt),
        forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).0.adsh is Some ==> a.contains(dn[i].0.adsh->0),
        forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).0.adsh is Some ==> a.contains(dp[i].0.adsh->0),
        forall|i: int| 0 <= i < ds.len() ==> a.contains((#[trigger] ds[i]).0.adsh),
        forall|i: int| 0 <= i < dt.len() ==> g.contains((#[trigger] dt[i]).0.tag),
        forall|f: spec_fn(NumRow) -> int| #[trigger] wsum(nl, f)
            == sum(db2.num, |n: NumRow| if aff_n(a, g, n) { f(n) } else { 0 }),
        forall|i: int| 0 <= i < nl.len() ==> (#[trigger] nl[i]).1 == 0 || aff_n(a, g, nl[i].0),
        rtab_ok(a, g, db2.pre, rtab),
    ensures
        jsum(db2.num, db2.sub, db2.tag, db2.pre, p, h) - jsum(db.num, db.sub, db.tag, db.pre, p, h)
            == passv(nl, db2.sub, db2.tag, new_sides(rtab), p, h)
                - passv(nl, db.sub, db.tag, old_sides(rtab, dp), p, h)
                + passv(dn, db.sub, db.tag, old_sides(rtab, dp), p, h),
{
    lemma_delta_num(db, db2, dn, dp, ds, dt, a, g, nl, p, h);
    lemma_delta_pass(db, db2, dp, a, g, nl, rtab, p, h);
    lemma_delta_dn(db, db2, dn, dp, a, g, rtab, p, h);
}


/// The join of a num row over tables with the pre table as the only side.
pub proof fn lemma_big_psi_single(sub: Seq<SubRow>, tag: Seq<TagRow>, pre: Seq<PreRow>, p: Params, h: spec_fn(JK) -> int, n: NumRow)
    ensures big_psi(sub, tag, new_sides(pre), p, h, n) == sub_sum4(sub, tag, pre, p, h, n),
{
    assert forall|s: SubRow, t: TagRow| #[trigger] pre_sum(pre, p, h, n, s, t) == psum(new_sides(pre), p, h, n, s, t) by {
        let sd = |x: (Seq<PreRow>, int)| x.1 * pre_sum(x.0, p, h, n, s, t);
        lemma_sum_singleton((pre, 1int), sd);
    }
    assert forall|s: SubRow| #[trigger] tag_sum4(tag, pre, p, h, n, s) == tsum(tag, new_sides(pre), p, h, n, s) by {
        lemma_sum_ext_all(tag, |t: TagRow| pre_sum(pre, p, h, n, s, t), |t: TagRow| psum(new_sides(pre), p, h, n, s, t));
    }
    lemma_sum_ext_all(sub, |s: SubRow| tag_sum4(tag, pre, p, h, n, s), |s: SubRow| tsum(tag, new_sides(pre), p, h, n, s));
}

}
