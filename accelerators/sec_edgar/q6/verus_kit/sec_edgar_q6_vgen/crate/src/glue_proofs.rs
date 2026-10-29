// Scan and rescan contracts as restricted lists; counts are non-negative; empty counts have empty sums.
#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::lemma_sum_measure_support;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;

verus! {

pub proof fn lemma_diag_k(k: GK)
    ensures diag(icnt(k)), diag(ival(k)),
{
    assert forall|n: NumRow, s: SubRow, p: PreRow| #[trigger] icnt(k)(n, s, p) != 0 implies n.adsh == Some(s.adsh) && p.adsh == n.adsh && n.value is Some by {}
    assert forall|n: NumRow, s: SubRow, p: PreRow| #[trigger] ival(k)(n, s, p) != 0 implies n.adsh == Some(s.adsh) && p.adsh == n.adsh && n.value is Some by {}
}

pub open spec fn seq_bytes(keys: Seq<Vec<u8>>) -> Seq<Seq<u8>> {
    keys.map_values(|x: Vec<u8>| x@)
}

pub proof fn lemma_rescan_num(db: Db, keys: Seq<Vec<u8>>, rows: Seq<NumRt>)
    requires rescan_nums_by_filing_ok(db, keys, rows),
    ensures restr(unit_l(num_rt_seq_view(rows)), db.num, cnum(seq_view_bytes(keys))),
{
    assert forall|f: spec_fn(NumRow) -> int| #[trigger] wsum(unit_l(num_rt_seq_view(rows)), f) == sum(db.num, |r: NumRow| cnum(seq_view_bytes(keys))(r) * f(r)) by {
        lemma_wsum_unit(num_rt_seq_view(rows), f);
        let c = |num: NumRow| if ((num.value is Some)) && (num.adsh is Some && seq_view_bytes(keys).contains(num.adsh->0)) { f(num) } else { 0 };
        assert(sum(num_rt_seq_view(rows), f) == sum(db.num, c));
        assert forall|r: NumRow| #[trigger] c(r) == cnum(seq_view_bytes(keys))(r) * f(r) by {
            if r.value is Some && in_keys(seq_view_bytes(keys), r.adsh) {
                assert(1 * f(r) == f(r));
            } else {
                assert(0 * f(r) == 0);
            }
        }
        lemma_sum_ext_all(db.num, c, |r: NumRow| cnum(seq_view_bytes(keys))(r) * f(r));
    }
}

pub proof fn lemma_rescan_sub(db: Db, keys: Seq<Vec<u8>>, rows: Seq<SubRt>)
    requires rescan_subs_by_filing_ok(db, keys, rows),
    ensures restr(unit_l(sub_rt_seq_view(rows)), db.sub, csub(seq_view_bytes(keys))),
{
    assert forall|f: spec_fn(SubRow) -> int| #[trigger] wsum(unit_l(sub_rt_seq_view(rows)), f) == sum(db.sub, |r: SubRow| csub(seq_view_bytes(keys))(r) * f(r)) by {
        lemma_wsum_unit(sub_rt_seq_view(rows), f);
        let c = |sub: SubRow| if seq_view_bytes(keys).contains(sub.adsh) { f(sub) } else { 0 };
        assert(sum(sub_rt_seq_view(rows), f) == sum(db.sub, c));
        assert forall|r: SubRow| #[trigger] c(r) == csub(seq_view_bytes(keys))(r) * f(r) by {
            if seq_view_bytes(keys).contains(r.adsh) { assert(1 * f(r) == f(r)); } else { assert(0 * f(r) == 0); }
        }
        lemma_sum_ext_all(db.sub, c, |r: SubRow| csub(seq_view_bytes(keys))(r) * f(r));
    }
}

pub proof fn lemma_rescan_pre(db: Db, keys: Seq<Vec<u8>>, rows: Seq<PreRt>)
    requires rescan_pres_by_filing_ok(db, keys, rows),
    ensures restr(unit_l(pre_rt_seq_view(rows)), db.pre, cpre(seq_view_bytes(keys))),
{
    assert forall|f: spec_fn(PreRow) -> int| #[trigger] wsum(unit_l(pre_rt_seq_view(rows)), f) == sum(db.pre, |r: PreRow| cpre(seq_view_bytes(keys))(r) * f(r)) by {
        lemma_wsum_unit(pre_rt_seq_view(rows), f);
        let c = |pre: PreRow| if pre.adsh is Some && seq_view_bytes(keys).contains(pre.adsh->0) { f(pre) } else { 0 };
        assert(sum(pre_rt_seq_view(rows), f) == sum(db.pre, c));
        assert forall|r: PreRow| #[trigger] c(r) == cpre(seq_view_bytes(keys))(r) * f(r) by {
            if in_keys(seq_view_bytes(keys), r.adsh) { assert(1 * f(r) == f(r)); } else { assert(0 * f(r) == 0); }
        }
        lemma_sum_ext_all(db.pre, c, |r: PreRow| cpre(seq_view_bytes(keys))(r) * f(r));
    }
}

pub open spec fn c_val() -> spec_fn(NumRow) -> int {
    |n: NumRow| if n.value is Some { 1int } else { 0 }
}

pub open spec fn c_one<R>() -> spec_fn(R) -> int {
    |r: R| 1int
}

/// Build: the scanned rows give the join of the database.
pub proof fn lemma_build_identity(db: Db, nums: Seq<NumRt>, subs: Seq<SubRt>, pres: Seq<PreRt>, h: H3)
    requires
        scan_nums_ok(db, nums), scan_subs_ok(db, subs), scan_pres_ok(db, pres),
        diag(h),
    ensures
        gs(unit_l(num_rt_seq_view(nums)), unit_l(sub_rt_seq_view(subs)), unit_l(pre_rt_seq_view(pres)), h) == gdb(db, h),
{
    assert forall|f: spec_fn(NumRow) -> int| #[trigger] wsum(unit_l(num_rt_seq_view(nums)), f) == sum(db.num, |r: NumRow| c_val()(r) * f(r)) by {
        lemma_wsum_unit(num_rt_seq_view(nums), f);
        let c = |num: NumRow| if (num.value is Some) { f(num) } else { 0 };
        assert(sum(num_rt_seq_view(nums), f) == sum(db.num, c));
        assert forall|r: NumRow| #[trigger] c(r) == c_val()(r) * f(r) by {
            if r.value is Some { assert(1 * f(r) == f(r)); } else { assert(0 * f(r) == 0); }
        }
        lemma_sum_ext_all(db.num, c, |r: NumRow| c_val()(r) * f(r));
    }
    assert forall|f: spec_fn(SubRow) -> int| #[trigger] wsum(unit_l(sub_rt_seq_view(subs)), f) == sum(db.sub, |r: SubRow| c_one::<SubRow>()(r) * f(r)) by {
        lemma_wsum_unit(sub_rt_seq_view(subs), f);
        assert(sum(sub_rt_seq_view(subs), f) == sum(db.sub, f));
        assert forall|r: SubRow| #[trigger] f(r) == c_one::<SubRow>()(r) * f(r) by { assert(1 * f(r) == f(r)); }
        lemma_sum_ext_all(db.sub, f, |r: SubRow| c_one::<SubRow>()(r) * f(r));
    }
    assert forall|f: spec_fn(PreRow) -> int| #[trigger] wsum(unit_l(pre_rt_seq_view(pres)), f) == sum(db.pre, |r: PreRow| c_one::<PreRow>()(r) * f(r)) by {
        lemma_wsum_unit(pre_rt_seq_view(pres), f);
        assert(sum(pre_rt_seq_view(pres), f) == sum(db.pre, f));
        assert forall|r: PreRow| #[trigger] f(r) == c_one::<PreRow>()(r) * f(r) by { assert(1 * f(r) == f(r)); }
        lemma_sum_ext_all(db.pre, f, |r: PreRow| c_one::<PreRow>()(r) * f(r));
    }
    lemma_gs_restr(db, unit_l(num_rt_seq_view(nums)), unit_l(sub_rt_seq_view(subs)), unit_l(pre_rt_seq_view(pres)), c_val(), c_one::<SubRow>(), c_one::<PreRow>(), h);
    let ha = hw(c_val(), c_one::<SubRow>(), c_one::<PreRow>(), h);
    assert forall|n: NumRow, s: SubRow, p: PreRow| #[trigger] ha(n, s, p) == h(n, s, p) by {
        if h(n, s, p) != 0 {
            assert(n.value is Some);
            assert(1 * (1 * (1 * h(n, s, p))) == h(n, s, p));
        } else {
            assert(c_val()(n) * (1 * (1 * 0)) == 0) by (nonlinear_arith);
        }
    }
    lemma_gdb_ext(db, ha, h);
}

pub proof fn lemma_gdb_ext(db: Db, h1: H3, h2: H3)
    requires forall|n: NumRow, s: SubRow, p: PreRow| #[trigger] h1(n, s, p) == h2(n, s, p),
    ensures gdb(db, h1) == gdb(db, h2),
{
    lemma_gdb2(db, h1);
    lemma_gdb2(db, h2);
    assert forall|n: NumRow| #[trigger] s_lvl(db.sub, db.pre, h1, n) == s_lvl(db.sub, db.pre, h2, n) by {
        assert forall|s: SubRow| #[trigger] p_lvl(db.pre, h1, n, s) == p_lvl(db.pre, h2, n, s) by {
            lemma_sum_ext_all(db.pre, |p: PreRow| h1(n, s, p), |p: PreRow| h2(n, s, p));
        }
        lemma_sum_ext_all(db.sub, |s: SubRow| p_lvl(db.pre, h1, n, s), |s: SubRow| p_lvl(db.pre, h2, n, s));
    }
    lemma_sum_ext_all(db.num, |n: NumRow| s_lvl(db.sub, db.pre, h1, n), |n: NumRow| s_lvl(db.sub, db.pre, h2, n));
}

/// A group has a non-negative number of triples; no triple means no sum.
pub proof fn lemma_cnt_facts(db: Db, k: GK)
    ensures cntk(db, k) >= 0, cntk(db, k) == 0 ==> sumk(db, k) == 0,
{
    let (hc, hv) = (icnt(k), ival(k));
    lemma_gdb2(db, hc);
    lemma_gdb2(db, hv);
    // pre level
    assert forall|n: NumRow, s: SubRow| #[trigger] p_lvl(db.pre, hc, n, s) >= 0 && (p_lvl(db.pre, hc, n, s) == 0 ==> p_lvl(db.pre, hv, n, s) == 0) by {
        assert forall|i: int| 0 <= i < db.pre.len() implies hc(n, s, #[trigger] db.pre[i]) >= 0 by {}
        lemma_sum_measure_support(db.pre, |p: PreRow| hc(n, s, p), |p: PreRow| hv(n, s, p));
    }
    assert forall|n: NumRow| #[trigger] s_lvl(db.sub, db.pre, hc, n) >= 0 && (s_lvl(db.sub, db.pre, hc, n) == 0 ==> s_lvl(db.sub, db.pre, hv, n) == 0) by {
        lemma_sum_measure_support(db.sub, |s: SubRow| p_lvl(db.pre, hc, n, s), |s: SubRow| p_lvl(db.pre, hv, n, s));
    }
    lemma_sum_measure_support(db.num, |n: NumRow| s_lvl(db.sub, db.pre, hc, n), |n: NumRow| s_lvl(db.sub, db.pre, hv, n));
}

} // verus!
