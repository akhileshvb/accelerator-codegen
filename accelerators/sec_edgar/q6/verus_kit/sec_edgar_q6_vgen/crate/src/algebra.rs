// Signed row lists and the maintenance identity: the groups of the database after a transaction
// are those before plus the join of the rescanned filings minus the join of their old rows.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;

verus! {

pub open spec fn wsum<A>(l: Seq<(A, int)>, f: spec_fn(A) -> int) -> int {
    sum(l, |x: (A, int)| x.1 * f(x.0))
}

/// Σ h over the join of three signed lists.
pub open spec fn gs(ln: Seq<(NumRow, int)>, ls: Seq<(SubRow, int)>, lp: Seq<(PreRow, int)>, h: H3) -> int {
    wsum(ln, fs(ls, lp, h))
}

pub open spec fn pf(h: H3, n: NumRow, s: SubRow) -> spec_fn(PreRow) -> int {
    |p: PreRow| h(n, s, p)
}

pub open spec fn fp(lp: Seq<(PreRow, int)>, h: H3, n: NumRow) -> spec_fn(SubRow) -> int {
    |s: SubRow| lvl_p(lp, h, n, s)
}

pub open spec fn fs(ls: Seq<(SubRow, int)>, lp: Seq<(PreRow, int)>, h: H3) -> spec_fn(NumRow) -> int {
    |n: NumRow| lvl_s(ls, lp, h, n)
}

/// l stands for table t restricted to the rows c selects.
pub open spec fn restr<A>(l: Seq<(A, int)>, t: Seq<A>, c: spec_fn(A) -> int) -> bool {
    forall|f: spec_fn(A) -> int| #[trigger] wsum(l, f) == sum(t, |r: A| c(r) * f(r))
}

pub open spec fn is_one<A>(c: spec_fn(A) -> int) -> spec_fn(A) -> bool {
    |r: A| c(r) == 1
}

pub open spec fn unit_l<A>(rows: Seq<A>) -> Seq<(A, int)> {
    rows.map_values(|r: A| (r, 1int))
}

/// The deltas c selects, with opposite signs.
pub open spec fn negf<A>(d: Seq<(A, int)>, c: spec_fn(A) -> bool) -> Seq<(A, int)>
    decreases d.len(),
{
    if d.len() == 0 { Seq::empty() } else {
        negf(d.drop_last(), c) + if c(d.last().0) { seq![(d.last().0, -d.last().1)] } else { Seq::empty() }
    }
}

pub proof fn lemma_wsum_unit<A>(rows: Seq<A>, f: spec_fn(A) -> int)
    ensures wsum(unit_l(rows), f) == sum(rows, f),
    decreases rows.len(),
{
    if rows.len() > 0 {
        let t = rows.drop_last();
        lemma_wsum_unit(t, f);
        assert(unit_l(rows) =~= unit_l(t).push((rows.last(), 1int)));
        lemma_sum_push(unit_l(t), (rows.last(), 1int), |x: (A, int)| x.1 * f(x.0));
        lemma_sum_push(t.push(rows.last()).drop_last(), rows.last(), f);
        assert(rows =~= t.push(rows.last()));
    }
}

pub proof fn lemma_wsum_concat<A>(a: Seq<(A, int)>, b: Seq<(A, int)>, f: spec_fn(A) -> int)
    ensures wsum(a + b, f) == wsum(a, f) + wsum(b, f),
{
    lemma_sum_concat(a, b, |x: (A, int)| x.1 * f(x.0));
}

pub proof fn lemma_wsum_negf<A>(d: Seq<(A, int)>, c: spec_fn(A) -> bool, f: spec_fn(A) -> int)
    ensures wsum(negf(d, c), f) == -sum(d, |x: (A, int)| if c(x.0) { x.1 * f(x.0) } else { 0 }),
    decreases d.len(),
{
    if d.len() > 0 {
        let t = d.drop_last();
        lemma_wsum_negf(t, c, f);
        lemma_wsum_concat(negf(t, c), if c(d.last().0) { seq![(d.last().0, -d.last().1)] } else { Seq::empty() }, f);
        if c(d.last().0) {
            lemma_sum_singleton((d.last().0, -d.last().1), |x: (A, int)| x.1 * f(x.0));
            assert((-d.last().1) * f(d.last().0) == -(d.last().1 * f(d.last().0))) by (nonlinear_arith);
        } else {
            lemma_sum_empty(Seq::<(A, int)>::empty(), |x: (A, int)| x.1 * f(x.0));
        }
        lemma_sum_push(t, d.last(), |x: (A, int)| if c(x.0) { x.1 * f(x.0) } else { 0 });
        assert(d =~= t.push(d.last()));
    }
}

/// Restricting a bag by weights c, then stepping it by deltas.
pub proof fn lemma_restr_step<A>(before: Seq<A>, after: Seq<A>, d: Seq<(A, int)>, c: spec_fn(A) -> int, unit_rows: Seq<A>)
    requires
        bag_step(before, after, d),
        restr(unit_l(unit_rows), after, c),
        forall|r: A| #[trigger] c(r) == 0 || c(r) == 1,
    ensures
        restr(unit_l(unit_rows) + negf(d, is_one(c)), before, c),
{
    assert forall|f: spec_fn(A) -> int| #[trigger] wsum(unit_l(unit_rows) + negf(d, is_one(c)), f) == sum(before, |r: A| c(r) * f(r)) by {
        let cf = |r: A| c(r) * f(r);
        lemma_wsum_concat(unit_l(unit_rows), negf(d, is_one(c)), f);
        lemma_wsum_negf(d, is_one(c), f);
        assert(wsum(unit_l(unit_rows), f) == sum(after, cf));
        assert(sum(after, cf) == sum(before, cf) + sum(d, |x: (A, int)| x.1 * cf(x.0)));
        let g1 = |x: (A, int)| if is_one(c)(x.0) { x.1 * f(x.0) } else { 0 };
        let g2 = |x: (A, int)| x.1 * cf(x.0);
        assert forall|x: (A, int)| #[trigger] g1(x) == g2(x) by {
            assert(c(x.0) == 0 || c(x.0) == 1);
            if c(x.0) == 1 { assert(x.1 * (1 * f(x.0)) == x.1 * f(x.0)) by (nonlinear_arith); }
            else { assert(x.1 * (0 * f(x.0)) == 0) by (nonlinear_arith); }
        }
        lemma_sum_ext_all(d, g1, g2);
        assert(sum(d, g1) == sum(d, g2));
    }
}

pub open spec fn hw(cn: spec_fn(NumRow) -> int, cs: spec_fn(SubRow) -> int, cp: spec_fn(PreRow) -> int, h: H3) -> H3 {
    |n: NumRow, s: SubRow, p: PreRow| cn(n) * (cs(s) * (cp(p) * h(n, s, p)))
}

pub open spec fn lvl_p(lp: Seq<(PreRow, int)>, h: H3, n: NumRow, s: SubRow) -> int {
    wsum(lp, pf(h, n, s))
}

pub open spec fn lvl_s(ls: Seq<(SubRow, int)>, lp: Seq<(PreRow, int)>, h: H3, n: NumRow) -> int {
    wsum(ls, fp(lp, h, n))
}

pub open spec fn pre_w(db: Db, cs_s: int, cp: spec_fn(PreRow) -> int, h: H3, n: NumRow, s: SubRow) -> int {
    sum(db.pre, |p: PreRow| cs_s * (cp(p) * h(n, s, p)))
}

pub open spec fn pre_hw(db: Db, cn: spec_fn(NumRow) -> int, cs: spec_fn(SubRow) -> int, cp: spec_fn(PreRow) -> int, h: H3, n: NumRow, s: SubRow) -> int {
    sum(db.pre, |p: PreRow| hw(cn, cs, cp, h)(n, s, p))
}

pub open spec fn sub_hw(db: Db, cn: spec_fn(NumRow) -> int, cs: spec_fn(SubRow) -> int, cp: spec_fn(PreRow) -> int, h: H3, n: NumRow) -> int {
    sum(db.sub, |s: SubRow| pre_hw(db, cn, cs, cp, h, n, s))
}

pub open spec fn sub_w(db: Db, cs: spec_fn(SubRow) -> int, cp: spec_fn(PreRow) -> int, h: H3, n: NumRow) -> int {
    sum(db.sub, |s: SubRow| pre_w(db, cs(s), cp, h, n, s))
}

/// Lists standing for restrictions of the tables join to the join of the restrictions.
pub proof fn lemma_gs_restr(db: Db, ln: Seq<(NumRow, int)>, ls: Seq<(SubRow, int)>, lp: Seq<(PreRow, int)>,
    cn: spec_fn(NumRow) -> int, cs: spec_fn(SubRow) -> int, cp: spec_fn(PreRow) -> int, h: H3)
    requires
        restr(ln, db.num, cn),
        restr(ls, db.sub, cs),
        restr(lp, db.pre, cp),
    ensures
        gs(ln, ls, lp, h) == gdb(db, hw(cn, cs, cp, h)),
{
    assert forall|n: NumRow, s: SubRow| #[trigger] lvl_p(lp, h, n, s) == sum(db.pre, |p: PreRow| cp(p) * h(n, s, p)) by {
        let f = pf(h, n, s);
        assert(wsum(lp, f) == sum(db.pre, |p: PreRow| cp(p) * f(p)));
        lemma_sum_ext_all(db.pre, |p: PreRow| cp(p) * f(p), |p: PreRow| cp(p) * h(n, s, p));
    }
    assert forall|n: NumRow| #[trigger] lvl_s(ls, lp, h, n) == sub_w(db, cs, cp, h, n) by {
        let f = fp(lp, h, n);
        assert(lvl_s(ls, lp, h, n) == wsum(ls, f));
        assert(wsum(ls, f) == sum(db.sub, |s: SubRow| cs(s) * f(s)));
        assert forall|s: SubRow| #[trigger] cs(s) * f(s) == pre_w(db, cs(s), cp, h, n, s) by {
            let g = |p: PreRow| cp(p) * h(n, s, p);
            assert(f(s) == sum(db.pre, g));
            lemma_sum_scale(db.pre, g, |p: PreRow| cs(s) * (cp(p) * h(n, s, p)), cs(s));
        }
        lemma_sum_ext_all(db.sub, |s: SubRow| cs(s) * f(s), |s: SubRow| pre_w(db, cs(s), cp, h, n, s));
    }
    let f = fs(ls, lp, h);
    assert(wsum(ln, f) == sum(db.num, |n: NumRow| cn(n) * f(n)));
    assert forall|n: NumRow| #[trigger] cn(n) * f(n) == sub_hw(db, cn, cs, cp, h, n) by {
        assert(f(n) == sub_w(db, cs, cp, h, n));
        let g = |s: SubRow| pre_w(db, cs(s), cp, h, n, s);
        lemma_sum_scale(db.sub, g, |s: SubRow| cn(n) * g(s), cn(n));
        assert forall|s: SubRow| #[trigger] pre_hw(db, cn, cs, cp, h, n, s) == cn(n) * g(s) by {
            let gg = |p: PreRow| cs(s) * (cp(p) * h(n, s, p));
            lemma_sum_scale(db.pre, gg, |p: PreRow| cn(n) * gg(p), cn(n));
            lemma_sum_ext_all(db.pre, |p: PreRow| cn(n) * gg(p), |p: PreRow| hw(cn, cs, cp, h)(n, s, p));
        }
        lemma_sum_ext_all(db.sub, |s: SubRow| cn(n) * g(s), |s: SubRow| pre_hw(db, cn, cs, cp, h, n, s));
    }
    lemma_sum_ext_all(db.num, |n: NumRow| cn(n) * f(n), |n: NumRow| sub_hw(db, cn, cs, cp, h, n));
    assert forall|n: NumRow| #[trigger] sub_hw(db, cn, cs, cp, h, n) == sum(db.sub, |s: SubRow| sum(db.pre, |p: PreRow| hw(cn, cs, cp, h)(n, s, p))) by {
        lemma_sum_ext_all(db.sub, |s: SubRow| pre_hw(db, cn, cs, cp, h, n, s), |s: SubRow| sum(db.pre, |p: PreRow| hw(cn, cs, cp, h)(n, s, p)));
    }
    lemma_sum_ext_all(db.num, |n: NumRow| sub_hw(db, cn, cs, cp, h, n), |n: NumRow| sum(db.sub, |s: SubRow| sum(db.pre, |p: PreRow| hw(cn, cs, cp, h)(n, s, p))));
}

pub open spec fn p_lvl(pre: Seq<PreRow>, g: H3, n: NumRow, s: SubRow) -> int {
    sum(pre, |p: PreRow| g(n, s, p))
}

pub open spec fn s_lvl(sub: Seq<SubRow>, pre: Seq<PreRow>, g: H3, n: NumRow) -> int {
    sum(sub, |s: SubRow| p_lvl(pre, g, n, s))
}

pub open spec fn gdb2(num: Seq<NumRow>, sub: Seq<SubRow>, pre: Seq<PreRow>, g: H3) -> int {
    sum(num, |n: NumRow| s_lvl(sub, pre, g, n))
}

pub proof fn lemma_gdb2(db: Db, g: H3)
    ensures gdb(db, g) == gdb2(db.num, db.sub, db.pre, g),
{
    assert forall|n: NumRow| #[trigger] s_lvl(db.sub, db.pre, g, n) == sum(db.sub, |s: SubRow| sum(db.pre, |p: PreRow| g(n, s, p))) by {
        lemma_sum_ext_all(db.sub, |s: SubRow| p_lvl(db.pre, g, n, s), |s: SubRow| sum(db.pre, |p: PreRow| g(n, s, p)));
    }
    lemma_sum_ext_all(db.num, |n: NumRow| s_lvl(db.sub, db.pre, g, n), |n: NumRow| sum(db.sub, |s: SubRow| sum(db.pre, |p: PreRow| g(n, s, p))));
}

pub proof fn lemma_zero_delta<A>(d: Seq<(A, int)>, z: spec_fn(A) -> int)
    requires forall|i: int| 0 <= i < d.len() ==> z((#[trigger] d[i]).0) == 0,
    ensures sum(d, |x: (A, int)| x.1 * z(x.0)) == 0,
{
    assert forall|i: int| 0 <= i < d.len() implies (|x: (A, int)| x.1 * z(x.0))(#[trigger] d[i]) == 0 by {
        assert(z(d[i].0) == 0);
        assert(d[i].1 * 0 == 0) by (nonlinear_arith);
    }
    lemma_sum_zero(d, |x: (A, int)| x.1 * z(x.0));
}

/// A weight that vanishes on every changed row sums the same before and after the changes.
pub proof fn lemma_untouched(db: Db, db2: Db, dn: Seq<(NumRow, int)>, ds: Seq<(SubRow, int)>, dp: Seq<(PreRow, int)>, g: H3)
    requires
        bag_step(db.num, db2.num, dn),
        bag_step(db.sub, db2.sub, ds),
        bag_step(db.pre, db2.pre, dp),
        forall|i: int, s: SubRow, p: PreRow| 0 <= i < dn.len() ==> #[trigger] g(dn[i].0, s, p) == 0,
        forall|i: int, n: NumRow, p: PreRow| 0 <= i < ds.len() ==> #[trigger] g(n, ds[i].0, p) == 0,
        forall|i: int, n: NumRow, s: SubRow| 0 <= i < dp.len() ==> #[trigger] g(n, s, dp[i].0) == 0,
    ensures
        gdb(db2, g) == gdb(db, g),
{
    lemma_gdb2(db, g);
    lemma_gdb2(db2, g);
    // innermost: p-level sums over db2.pre equal those over db.pre
    assert forall|n: NumRow, s: SubRow| #[trigger] p_lvl(db2.pre, g, n, s) == p_lvl(db.pre, g, n, s) by {
        let f = |p: PreRow| g(n, s, p);
        assert(sum(db2.pre, f) == sum(db.pre, f) + sum(dp, |x: (PreRow, int)| x.1 * f(x.0)));
        lemma_zero_delta(dp, f);
    }
    assert forall|n: NumRow| #[trigger] s_lvl(db2.sub, db2.pre, g, n) == s_lvl(db.sub, db.pre, g, n) by {
        let f = |s: SubRow| p_lvl(db2.pre, g, n, s);
        assert(sum(db2.sub, f) == sum(db.sub, f) + sum(ds, |x: (SubRow, int)| x.1 * f(x.0)));
        assert forall|i: int| 0 <= i < ds.len() implies f((#[trigger] ds[i]).0) == 0 by {
            lemma_sum_zero(db2.pre, |p: PreRow| g(n, ds[i].0, p));
        }
        lemma_zero_delta(ds, f);
        lemma_sum_ext_all(db.sub, f, |s: SubRow| p_lvl(db.pre, g, n, s));
    }
    let f = |n: NumRow| s_lvl(db2.sub, db2.pre, g, n);
    assert(sum(db2.num, f) == sum(db.num, f) + sum(dn, |x: (NumRow, int)| x.1 * f(x.0)));
    assert forall|i: int| 0 <= i < dn.len() implies f((#[trigger] dn[i]).0) == 0 by {
        assert forall|s: SubRow| #[trigger] p_lvl(db2.pre, g, dn[i].0, s) == 0 by {
            lemma_sum_zero(db2.pre, |p: PreRow| g(dn[i].0, s, p));
        }
        lemma_sum_zero(db2.sub, |s: SubRow| p_lvl(db2.pre, g, dn[i].0, s));
    }
    lemma_zero_delta(dn, f);
    lemma_sum_ext_all(db.num, f, |n: NumRow| s_lvl(db.sub, db.pre, g, n));
}

pub open spec fn in_keys(keys: Seq<Seq<u8>>, a: Option<Seq<u8>>) -> bool {
    a is Some && keys.contains(a->0)
}

pub open spec fn cnum(keys: Seq<Seq<u8>>) -> spec_fn(NumRow) -> int {
    |n: NumRow| if n.value is Some && in_keys(keys, n.adsh) { 1int } else { 0 }
}

pub open spec fn csub(keys: Seq<Seq<u8>>) -> spec_fn(SubRow) -> int {
    |s: SubRow| if keys.contains(s.adsh) { 1int } else { 0 }
}

pub open spec fn cpre(keys: Seq<Seq<u8>>) -> spec_fn(PreRow) -> int {
    |p: PreRow| if in_keys(keys, p.adsh) { 1int } else { 0 }
}

/// h counts only triples of one filing, with a value.
pub open spec fn diag(h: H3) -> bool {
    forall|n: NumRow, s: SubRow, p: PreRow| #[trigger] h(n, s, p) != 0 ==> n.adsh == Some(s.adsh) && p.adsh == n.adsh && n.value is Some
}

pub proof fn lemma_gdb_add(db: Db, h: H3, h1: H3, h2: H3)
    requires forall|n: NumRow, s: SubRow, p: PreRow| #[trigger] h(n, s, p) == h1(n, s, p) + h2(n, s, p),
    ensures gdb(db, h) == gdb(db, h1) + gdb(db, h2),
{
    lemma_gdb2(db, h);
    lemma_gdb2(db, h1);
    lemma_gdb2(db, h2);
    assert forall|n: NumRow| #[trigger] s_lvl(db.sub, db.pre, h, n) == s_lvl(db.sub, db.pre, h1, n) + s_lvl(db.sub, db.pre, h2, n) by {
        assert forall|s: SubRow| #[trigger] p_lvl(db.pre, h, n, s) == p_lvl(db.pre, h1, n, s) + p_lvl(db.pre, h2, n, s) by {
            lemma_sum_add(db.pre, |p: PreRow| h1(n, s, p), |p: PreRow| h2(n, s, p), |p: PreRow| h(n, s, p));
        }
        lemma_sum_add(db.sub, |s: SubRow| p_lvl(db.pre, h1, n, s), |s: SubRow| p_lvl(db.pre, h2, n, s), |s: SubRow| p_lvl(db.pre, h, n, s));
    }
    lemma_sum_add(db.num, |n: NumRow| s_lvl(db.sub, db.pre, h1, n), |n: NumRow| s_lvl(db.sub, db.pre, h2, n), |n: NumRow| s_lvl(db.sub, db.pre, h, n));
}

/// The maintenance identity for one transaction.
pub proof fn lemma_apply_identity(db: Db, db2: Db, dn: Seq<(NumRow, int)>, ds: Seq<(SubRow, int)>, dp: Seq<(PreRow, int)>,
    keys: Seq<Seq<u8>>, rn: Seq<NumRow>, rs: Seq<SubRow>, rp: Seq<PreRow>, h: H3)
    requires
        bag_step(db.num, db2.num, dn),
        bag_step(db.sub, db2.sub, ds),
        bag_step(db.pre, db2.pre, dp),
        restr(unit_l(rn), db2.num, cnum(keys)),
        restr(unit_l(rs), db2.sub, csub(keys)),
        restr(unit_l(rp), db2.pre, cpre(keys)),
        forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).0.adsh is Some ==> keys.contains(dn[i].0.adsh->0),
        forall|i: int| 0 <= i < ds.len() ==> keys.contains((#[trigger] ds[i]).0.adsh),
        forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).0.adsh is Some ==> keys.contains(dp[i].0.adsh->0),
        diag(h),
    ensures
        gdb(db2, h) == gdb(db, h)
            + gs(unit_l(rn), unit_l(rs), unit_l(rp), h)
            - gs(unit_l(rn) + negf(dn, is_one(cnum(keys))), unit_l(rs) + negf(ds, is_one(csub(keys))), unit_l(rp) + negf(dp, is_one(cpre(keys))), h),
{
    let (cn, cs, cp) = (cnum(keys), csub(keys), cpre(keys));
    assert forall|n: NumRow| #[trigger] cn(n) == 0 || cn(n) == 1 by {}
    assert forall|s: SubRow| #[trigger] cs(s) == 0 || cs(s) == 1 by {}
    assert forall|p: PreRow| #[trigger] cp(p) == 0 || cp(p) == 1 by {}
    lemma_restr_step(db.num, db2.num, dn, cn, rn);
    lemma_restr_step(db.sub, db2.sub, ds, cs, rs);
    lemma_restr_step(db.pre, db2.pre, dp, cp, rp);
    let ha = hw(cn, cs, cp, h);
    lemma_gs_restr(db2, unit_l(rn), unit_l(rs), unit_l(rp), cn, cs, cp, h);
    lemma_gs_restr(db, unit_l(rn) + negf(dn, is_one(cn)), unit_l(rs) + negf(ds, is_one(cs)), unit_l(rp) + negf(dp, is_one(cp)), cn, cs, cp, h);
    let hb = |n: NumRow, s: SubRow, p: PreRow| h(n, s, p) - ha(n, s, p);
    lemma_gdb_add(db, h, ha, hb);
    lemma_gdb_add(db2, h, ha, hb);
    // hb is zero wherever h is zero, and on every triple of a touched filing
    assert forall|n: NumRow, s: SubRow, p: PreRow| h(n, s, p) != 0 && n.adsh is Some && keys.contains(n.adsh->0) implies #[trigger] hb(n, s, p) == 0 by {
        assert(cn(n) == 1 && cs(s) == 1 && cp(p) == 1);
        assert(ha(n, s, p) == h(n, s, p)) by (nonlinear_arith)
            requires ha(n, s, p) == cn(n) * (cs(s) * (cp(p) * h(n, s, p))), cn(n) == 1, cs(s) == 1, cp(p) == 1;
    }
    assert forall|n: NumRow, s: SubRow, p: PreRow| h(n, s, p) == 0 implies #[trigger] hb(n, s, p) == 0 by {
        assert(ha(n, s, p) == 0) by (nonlinear_arith)
            requires ha(n, s, p) == cn(n) * (cs(s) * (cp(p) * h(n, s, p))), h(n, s, p) == 0;
    }
    lemma_untouched(db, db2, dn, ds, dp, hb);
}

} // verus!
