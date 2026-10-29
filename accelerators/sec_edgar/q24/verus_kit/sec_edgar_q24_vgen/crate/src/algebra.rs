// The maintenance identity: the anti-join sums after a transaction are those before plus
// the change in membership on the rescanned filings plus the changed num rows.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;

verus! {

pub open spec fn in_keys(keys: Seq<Seq<u8>>, a: Option<Seq<u8>>) -> bool {
    a is Some && keys.contains(a->0)
}

/// The signed number of changed pre rows n joins with.
pub open spec fn dm(dp: Seq<(PreRow, int)>, n: NumRow) -> int {
    sum(dp, |d: (PreRow, int)| d.1 * mw(n)(d.0))
}

/// Matches of n among the rescanned pre rows, and among the old rows of those filings.
pub open spec fn mc2(rp: Seq<PreRow>, n: NumRow) -> int {
    mc(rp, n)
}

pub open spec fn mc1(rp: Seq<PreRow>, dp: Seq<(PreRow, int)>, n: NumRow) -> int {
    mc(rp, n) - dm(dp, n)
}

pub open spec fn z2(rp: Seq<PreRow>, n: NumRow) -> int {
    ind(mc2(rp, n) == 0)
}

pub open spec fn z1(rp: Seq<PreRow>, dp: Seq<(PreRow, int)>, n: NumRow) -> int {
    ind(mc1(rp, dp, n) == 0)
}

/// Σ over signed rows of sign · f.
pub open spec fn wsum<A>(l: Seq<(A, int)>, f: spec_fn(A) -> int) -> int {
    sum(l, |x: (A, int)| x.1 * f(x.0))
}

pub open spec fn covered_n(dn: Seq<(NumRow, int)>, keys: Seq<Seq<u8>>) -> bool {
    forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).0.adsh is Some ==> keys.contains(dn[i].0.adsh->0)
}

pub open spec fn covered_p(dp: Seq<(PreRow, int)>, keys: Seq<Seq<u8>>) -> bool {
    forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).0.adsh is Some ==> keys.contains(dp[i].0.adsh->0)
}

/// The rescanned pre rows are the pre rows of the keyed filings.
pub open spec fn rp_ok(db2: Db, keys: Seq<Seq<u8>>, rp: Seq<PreRow>) -> bool {
    forall|f: spec_fn(PreRow) -> int| #[trigger] sum(rp, f) == sum(db2.pre, |p: PreRow| if in_keys(keys, p.adsh) { f(p) } else { 0 })
}

pub open spec fn rn_ok(db2: Db, keys: Seq<Seq<u8>>, rn: Seq<NumRow>) -> bool {
    forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rn, f) == sum(db2.num, |n: NumRow| if n.value is Some && in_keys(keys, n.adsh) { f(n) } else { 0 })
}

/// A row of a keyed filing (or without a filing) has its matches among the rescanned pre rows.
proof fn lemma_mc_new(db2: Db, keys: Seq<Seq<u8>>, rp: Seq<PreRow>, n: NumRow)
    requires rp_ok(db2, keys, rp), n.adsh is None || in_keys(keys, n.adsh),
    ensures mc(db2.pre, n) == mc2(rp, n),
{
    let f = mw(n);
    let g = |p: PreRow| if in_keys(keys, p.adsh) { f(p) } else { 0 };
    assert forall|p: PreRow| #[trigger] g(p) == f(p) by {
        if m3(n, p) { assert(in_keys(keys, p.adsh)); }
    }
    lemma_sum_ext_all(db2.pre, g, f);
}

/// The old matches: the new ones less the signed changed pre rows.
proof fn lemma_mc_old(db: Db, db2: Db, dp: Seq<(PreRow, int)>, n: NumRow)
    requires bag_step(db.pre, db2.pre, dp),
    ensures mc(db.pre, n) == mc(db2.pre, n) - dm(dp, n),
{
    assert(sum(db2.pre, mw(n)) == sum(db.pre, mw(n)) + sum(dp, |d: (PreRow, int)| d.1 * mw(n)(d.0)));
}

/// A row outside the keyed filings keeps its matches.
proof fn lemma_dm_outside(dp: Seq<(PreRow, int)>, keys: Seq<Seq<u8>>, n: NumRow)
    requires covered_p(dp, keys), n.adsh is None || !keys.contains(n.adsh->0),
    ensures dm(dp, n) == 0,
{
    let z = |d: (PreRow, int)| d.1 * mw(n)(d.0);
    assert forall|i: int| 0 <= i < dp.len() implies z(#[trigger] dp[i]) == 0 by {
        if m3(n, dp[i].0) { assert(keys.contains(dp[i].0.adsh->0)); }
    }
    lemma_sum_zero(dp, z);
}

/// For the rows of a keyed filing (or without one), um is z on both sides of the transaction.
pub proof fn lemma_um_z(db: Db, db2: Db, dp: Seq<(PreRow, int)>, keys: Seq<Seq<u8>>, rp: Seq<PreRow>, n: NumRow)
    requires
        bag_step(db.pre, db2.pre, dp), rp_ok(db2, keys, rp),
        n.adsh is None || in_keys(keys, n.adsh),
    ensures
        um(db2.pre, n) == z2(rp, n),
        um(db.pre, n) == z1(rp, dp, n),
{
    lemma_mc_new(db2, keys, rp, n);
    lemma_mc_old(db, db2, dp, n);
}

/// Rows outside the keyed filings keep um.
proof fn lemma_um_same(db: Db, db2: Db, dp: Seq<(PreRow, int)>, keys: Seq<Seq<u8>>, n: NumRow)
    requires
        bag_step(db.pre, db2.pre, dp), covered_p(dp, keys),
        n.adsh is None || !keys.contains(n.adsh->0),
    ensures
        um(db2.pre, n) == um(db.pre, n),
{
    lemma_mc_old(db, db2, dp, n);
    lemma_dm_outside(dp, keys, n);
}

/// The anti-join sum after the transaction: before, plus the membership change of the rescanned
/// filings' num rows, plus the signed changed num rows (that had no match before).
pub proof fn lemma_apply_identity(db: Db, db2: Db, dn: Seq<(NumRow, int)>, dp: Seq<(PreRow, int)>,
    keys: Seq<Seq<u8>>, rn: Seq<NumRow>, rp: Seq<PreRow>, h: HN)
    requires
        bag_step(db.num, db2.num, dn), bag_step(db.pre, db2.pre, dp),
        covered_n(dn, keys), covered_p(dp, keys),
        rn_ok(db2, keys, rn), rp_ok(db2, keys, rp),
        forall|n: NumRow| #[trigger] h(n) != 0 ==> n.value is Some,
    ensures
        nsum(db2.num, db2.pre, h) == nsum(db.num, db.pre, h)
            + sum(rn, |n: NumRow| (z2(rp, n) - z1(rp, dp, n)) * h(n))
            + wsum(dn, |n: NumRow| z1(rp, dp, n) * h(n)),
{
    let u1 = uw(db.pre, h);
    let u2 = uw(db2.pre, h);
    // Σ_{num2} u2 = Σ_{num2} u1 + Σ_{num2} (u2 - u1)
    let dif = |n: NumRow| u2(n) - u1(n);
    lemma_sum_sub(db2.num, u2, u1, dif);
    // Σ_{num2} u1 = Σ_{num} u1 + Σ_{dn} sign · u1
    assert(sum(db2.num, u1) == sum(db.num, u1) + sum(dn, |d: (NumRow, int)| d.1 * u1(d.0)));
    // the membership change lives on the rescanned rows
    let tw = |n: NumRow| (z2(rp, n) - z1(rp, dp, n)) * h(n);
    let gd = |n: NumRow| if n.value is Some && in_keys(keys, n.adsh) { tw(n) } else { 0 };
    assert forall|n: NumRow| #[trigger] dif(n) == gd(n) by {
        if n.value is Some && in_keys(keys, n.adsh) {
            lemma_um_z(db, db2, dp, keys, rp, n);
            assert((z2(rp, n) - z1(rp, dp, n)) * h(n) == z2(rp, n) * h(n) - z1(rp, dp, n) * h(n)) by (nonlinear_arith);
        } else if h(n) != 0 {
            assert(n.value is Some);
            lemma_um_same(db, db2, dp, keys, n);
        } else {
            assert(um(db2.pre, n) * h(n) == 0 && um(db.pre, n) * h(n) == 0) by (nonlinear_arith) requires h(n) == 0;
        }
    }
    lemma_sum_ext_all(db2.num, dif, gd);
    assert(sum(rn, tw) == sum(db2.num, gd));
    // the changed rows: um before is z1
    let zh = |n: NumRow| z1(rp, dp, n) * h(n);
    let dw = |d: (NumRow, int)| d.1 * u1(d.0);
    let dz = |d: (NumRow, int)| d.1 * zh(d.0);
    assert forall|i: int| 0 <= i < dn.len() implies dw(#[trigger] dn[i]) == dz(dn[i]) by {
        let d = dn[i];
        if d.0.adsh is Some { assert(keys.contains(d.0.adsh->0)); }
        lemma_um_z(db, db2, dp, keys, rp, d.0);
    }
    lemma_sum_ext(dn, dw, dz);
    assert(wsum(dn, zh) == sum(dn, dz));
}

/// Build: the scanned rows give the anti-join sums of the database.
pub proof fn lemma_build_identity(db: Db, rn: Seq<NumRow>, rp: Seq<PreRow>, h: HN)
    requires
        forall|f: spec_fn(NumRow) -> int| #[trigger] sum(rn, f) == sum(db.num, |n: NumRow| if n.value is Some { f(n) } else { 0 }),
        forall|f: spec_fn(PreRow) -> int| #[trigger] sum(rp, f) == sum(db.pre, f),
        forall|n: NumRow| #[trigger] h(n) != 0 ==> n.value is Some,
    ensures
        nsum(db.num, db.pre, h) == sum(rn, |n: NumRow| z2(rp, n) * h(n)),
{
    let u = uw(db.pre, h);
    let g = |n: NumRow| z2(rp, n) * h(n);
    assert forall|n: NumRow| #[trigger] u(n) == (if n.value is Some { g(n) } else { 0 }) by {
        assert(mc(db.pre, n) == mc2(rp, n));
        if !(n.value is Some) { assert(h(n) == 0); assert(um(db.pre, n) * h(n) == 0) by (nonlinear_arith) requires h(n) == 0; }
    }
    lemma_sum_ext_all(db.num, u, |n: NumRow| if n.value is Some { g(n) } else { 0 });
}

} // verus!
