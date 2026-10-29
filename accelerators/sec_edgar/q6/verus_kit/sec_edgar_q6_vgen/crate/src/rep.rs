// Every stored value is the value of some double (rows come from doubles); a one-tuple group's sum is such a value.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;

verus! {

pub open spec fn rep_db(db: Db) -> bool {
    forall|i: int| 0 <= i < db.num.len() && (#[trigger] db.num[i]).value is Some ==> exists|x: u64| f64_val(x) == db.num[i].value->0
}

pub open spec fn is_rep(n: NumRow) -> bool {
    n.value is Some ==> exists|x: u64| f64_val(x) == n.value->0
}

pub proof fn lemma_rep_scan(db: Db, nums: Seq<NumRt>)
    requires scan_nums_ok(db, nums),
    ensures rep_db(db),
{
    let bad = |r: NumRow| if is_rep(r) { 0int } else { 1int };
    assert forall|i: int| 0 <= i < num_rt_seq_view(nums).len() implies bad(#[trigger] num_rt_seq_view(nums)[i]) == 0 by {
        let r = nums[i];
        if r.value__null == 0 { assert(f64_val(r.value) == num_rt_view(r).value->0); }
    }
    lemma_sum_zero(num_rt_seq_view(nums), bad);
    let f = bad;
    assert(sum(num_rt_seq_view(nums), f) == sum(db.num, |num: NumRow| if (num.value is Some) { f(num) } else { 0 }));
    let g = |num: NumRow| if (num.value is Some) { f(num) } else { 0 };
    assert forall|r: NumRow| #[trigger] g(r) == bad(r) by {
        if r.value is None { assert(is_rep(r)); }
    }
    lemma_sum_ext_all(db.num, g, bad);
    assert forall|i: int| 0 <= i < db.num.len() && (#[trigger] db.num[i]).value is Some implies exists|x: u64| f64_val(x) == db.num[i].value->0 by {
        if !is_rep(db.num[i]) {
            assert forall|j: int| 0 <= j < db.num.len() implies bad(#[trigger] db.num[j]) >= 0 by {}
            lemma_sum_pos(db.num, bad, i);
        }
    }
}

pub proof fn lemma_rep_step(db: Db, db2: Db, dn: Seq<(NumRow, int)>)
    requires
        rep_db(db),
        bag_step(db.num, db2.num, dn),
        unit_weights(dn),
        forall|i: int| 0 <= i < dn.len() ==> is_rep((#[trigger] dn[i]).0),
    ensures rep_db(db2),
{
    assert forall|i: int| 0 <= i < db2.num.len() && (#[trigger] db2.num[i]).value is Some implies exists|x: u64| f64_val(x) == db2.num[i].value->0 by {
        let r = db2.num[i];
        let f = eqv(r);
        assert forall|j: int| 0 <= j < db2.num.len() implies f(#[trigger] db2.num[j]) >= 0 by {}
        assert(f(db2.num[i]) == 1);
        lemma_sum_pos(db2.num, f, i);
        assert(sum(db2.num, f) == sum(db.num, f) + sum(dn, |d: (NumRow, int)| d.1 * f(d.0)));
        if exists|j: int| 0 <= j < db.num.len() && db.num[j] == r {
            let j = choose|j: int| 0 <= j < db.num.len() && db.num[j] == r;
            assert(is_rep(db.num[j]));
        } else {
            assert forall|j: int| 0 <= j < db.num.len() implies f(#[trigger] db.num[j]) == 0 by {}
            lemma_sum_zero(db.num, f);
            let w = lemma_sum_nonzero_witness(dn, |d: (NumRow, int)| d.1 * f(d.0));
            assert(dn[w].0 == r);
            assert(is_rep(dn[w].0));
        }
    }
}

/// One positive count among non-negative ones: the measure's sum is that row's measure.
pub proof fn lemma_unit_support<A>(rows: Seq<A>, cnt: spec_fn(A) -> int, meas: spec_fn(A) -> int) -> (i: int)
    requires
        forall|j: int| 0 <= j < rows.len() ==> cnt(#[trigger] rows[j]) >= 0,
        forall|j: int| 0 <= j < rows.len() ==> cnt(#[trigger] rows[j]) == 0 ==> meas(rows[j]) == 0,
        sum(rows, cnt) == 1,
    ensures
        0 <= i < rows.len(), cnt(rows[i]) == 1, sum(rows, meas) == meas(rows[i]),
{
    let i = lemma_sum_nonzero_witness(rows, cnt);
    lemma_sum_ge_term(rows, cnt, i);
    assert(cnt(rows[i]) == 1);
    lemma_sum_remove(rows, i, cnt);
    let rest = rows.remove(i);
    assert forall|j: int| 0 <= j < rest.len() implies cnt(#[trigger] rest[j]) >= 0 by {
        if j < i { assert(rest[j] == rows[j]); } else { assert(rest[j] == rows[j + 1]); }
    }
    lemma_sum_nonneg(rest, cnt);
    assert forall|j: int| 0 <= j < rows.len() && j != i implies meas(#[trigger] rows[j]) == 0 by {
        let jj = if j < i { j } else { j - 1 };
        assert(rest[jj] == rows[j]);
        if cnt(rows[j]) > 0 {
            lemma_sum_pos(rest, cnt, jj);
        }
    }
    lemma_sum_single_match(rows, meas, i);
    i
}

/// A group with one tuple has the value of that tuple's double as its sum.
pub proof fn lemma_single_rep(db: Db, k: GK)
    requires rep_db(db), cntk(db, k) == 1,
    ensures exists|x: u64| f64_val(x) == sumk(db, k),
{
    let (hc, hv) = (icnt(k), ival(k));
    lemma_gdb2(db, hc);
    lemma_gdb2(db, hv);
    assert forall|n: NumRow, s: SubRow| #[trigger] p_lvl(db.pre, hc, n, s) >= 0 && (p_lvl(db.pre, hc, n, s) == 0 ==> p_lvl(db.pre, hv, n, s) == 0) by {
        assert forall|i: int| 0 <= i < db.pre.len() implies hc(n, s, #[trigger] db.pre[i]) >= 0 by {}
        verus_prooflib::lemma_sum_measure_support(db.pre, |p: PreRow| hc(n, s, p), |p: PreRow| hv(n, s, p));
    }
    assert forall|n: NumRow| #[trigger] s_lvl(db.sub, db.pre, hc, n) >= 0 && (s_lvl(db.sub, db.pre, hc, n) == 0 ==> s_lvl(db.sub, db.pre, hv, n) == 0) by {
        verus_prooflib::lemma_sum_measure_support(db.sub, |s: SubRow| p_lvl(db.pre, hc, n, s), |s: SubRow| p_lvl(db.pre, hv, n, s));
    }
    let i = lemma_unit_support(db.num, |n: NumRow| s_lvl(db.sub, db.pre, hc, n), |n: NumRow| s_lvl(db.sub, db.pre, hv, n));
    let n0 = db.num[i];
    let j = lemma_unit_support(db.sub, |s: SubRow| p_lvl(db.pre, hc, n0, s), |s: SubRow| p_lvl(db.pre, hv, n0, s));
    let s0 = db.sub[j];
    let q = lemma_unit_support(db.pre, |p: PreRow| hc(n0, s0, p), |p: PreRow| hv(n0, s0, p));
    let p0 = db.pre[q];
    assert(hc(n0, s0, p0) == 1);
    assert(jk(n0, s0, p0) && key_of(n0, s0, p0) == k);
    assert(hv(n0, s0, p0) == n0.value->0);
    assert(n0.value is Some);
}

} // verus!
