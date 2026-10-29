// The kernel: groups of the join keyed by (fy, stmt, uom, name, tag, plabel), maintained per filing batch.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::esc::*;
use crate::accmap::*;
use crate::gmap::*;
use crate::merge::*;
use crate::join::*;
use crate::wideval::*;
use crate::glue_proofs::*;
use crate::lists::*;

verus! {

pub open spec fn state_ok(groups: GMap, tab: Pow2Tab, db: Db) -> bool {
    &&& gm_ok(groups)
    &&& pos_ok(groups)
    &&& tab.ok()
    &&& forall|k: GK| #![trigger gv_c(groups, k)] gv_c(groups, k) == cntk(db, k) && gv_s(groups, k) == sumk(db, k)
}

pub struct State {
    groups: GMap,
    tab: Arc<Pow2Tab>,
    db: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    closed spec fn valid(self) -> bool {
        state_ok(self.groups, *self.tab, self.db@)
    }
}

/// The state keeps nothing that grows with past writes.
pub open spec fn history(s: State) -> nat {
    0
}


pub open spec fn rescan_subs_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>) -> Seq<Seq<u8>> {
    all_keys(num_delta, pre_delta, sub_delta)
}

pub open spec fn rescan_nums_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>) -> Seq<Seq<u8>> {
    all_keys(num_delta, pre_delta, sub_delta)
}

pub open spec fn rescan_pres_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>) -> Seq<Seq<u8>> {
    all_keys(num_delta, pre_delta, sub_delta)
}


pub fn rescan_subs_by_filing_keys(s: &State, num_delta: &Vec<NumDelta>, pre_delta: &Vec<PreDelta>, sub_delta: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == rescan_subs_by_filing_wanted(*s, num_delta@, pre_delta@, sub_delta@),
{
    collect_keys(num_delta, pre_delta, sub_delta)
}

pub fn rescan_nums_by_filing_keys(s: &State, num_delta: &Vec<NumDelta>, pre_delta: &Vec<PreDelta>, sub_delta: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == rescan_nums_by_filing_wanted(*s, num_delta@, pre_delta@, sub_delta@),
{
    collect_keys(num_delta, pre_delta, sub_delta)
}

pub fn rescan_pres_by_filing_keys(s: &State, num_delta: &Vec<NumDelta>, pre_delta: &Vec<PreDelta>, sub_delta: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == rescan_pres_by_filing_wanted(*s, num_delta@, pre_delta@, sub_delta@),
{
    collect_keys(num_delta, pre_delta, sub_delta)
}

#[verifier::rlimit(200)]
#[verifier::spinoff_prover]
pub fn apply(
    s: &State,
    num_delta: &Vec<NumDelta>,
    pre_delta: &Vec<PreDelta>,
    sub_delta: &Vec<SubDelta>,
    subs_by_filing_keys: &Vec<Vec<u8>>,
    subs_by_filing_rows: &Vec<SubRt>,
    nums_by_filing_keys: &Vec<Vec<u8>>,
    nums_by_filing_rows: &Vec<NumRt>,
    pres_by_filing_keys: &Vec<Vec<u8>>,
    pres_by_filing_rows: &Vec<PreRt>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, num_delta_seq_view(num_delta@), pre_delta_seq_view(pre_delta@), sub_delta_seq_view(sub_delta@)),
        seq_view_bytes(subs_by_filing_keys@) == rescan_subs_by_filing_wanted(*s, num_delta@, pre_delta@, sub_delta@),
        rescan_subs_by_filing_ok(db2, subs_by_filing_keys@, subs_by_filing_rows@),
        seq_view_bytes(nums_by_filing_keys@) == rescan_nums_by_filing_wanted(*s, num_delta@, pre_delta@, sub_delta@),
        rescan_nums_by_filing_ok(db2, nums_by_filing_keys@, nums_by_filing_rows@),
        seq_view_bytes(pres_by_filing_keys@) == rescan_pres_by_filing_wanted(*s, num_delta@, pre_delta@, sub_delta@),
        rescan_pres_by_filing_ok(db2, pres_by_filing_keys@, pres_by_filing_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        r matches Ok(s2) ==> history(s2) <= history(*s) + num_delta@.len() + pre_delta@.len() + sub_delta@.len(),
        envelope(s.db()) && envelope(db2) && history(*s) + num_delta@.len() + pre_delta@.len() + sub_delta@.len() <= max_history() ==> r is Ok,
{
    proof { use_type_invariant(s); }
    let ghost db = s.db();
    proof {
        assert(state_ok(s.groups, *s.tab, s.db@));
        assert forall|k: GK| #![trigger gv_c(s.groups, k)] gv_c(s.groups, k) == cntk(db, k) && gv_s(s.groups, k) == sumk(db, k) by {}
    }
    let ghost kall = all_keys(num_delta@, pre_delta@, sub_delta@);
    let ghost (dn, dp, ds) = (num_delta_seq_view(num_delta@), pre_delta_seq_view(pre_delta@), sub_delta_seq_view(sub_delta@));
    proof {
        lemma_keys_cover(num_delta@, pre_delta@, sub_delta@);
        lemma_rescan_num(db2, nums_by_filing_keys@, nums_by_filing_rows@);
        lemma_rescan_sub(db2, subs_by_filing_keys@, subs_by_filing_rows@);
        lemma_rescan_pre(db2, pres_by_filing_keys@, pres_by_filing_rows@);
        assert(unit_weights(dn) && unit_weights(dp) && unit_weights(ds));
    }
    let l2n = wrap_num(nums_by_filing_rows);
    let l2s = wrap_sub(subs_by_filing_rows);
    let l2p = wrap_pre(pres_by_filing_rows);
    let mut l1n = wrap_num(nums_by_filing_rows);
    let mut l1s = wrap_sub(subs_by_filing_rows);
    let mut l1p = wrap_pre(pres_by_filing_rows);
    proof {
        assert forall|i: int| 0 <= i < num_delta.len() implies (#[trigger] num_delta@[i]).sign == 1 || num_delta@[i].sign == -1 by { assert(dn[i].1 == num_delta@[i].sign as int); }
        assert forall|i: int| 0 <= i < pre_delta.len() implies (#[trigger] pre_delta@[i]).sign == 1 || pre_delta@[i].sign == -1 by { assert(dp[i].1 == pre_delta@[i].sign as int); }
        assert forall|i: int| 0 <= i < sub_delta.len() implies (#[trigger] sub_delta@[i]).sign == 1 || sub_delta@[i].sign == -1 by { assert(ds[i].1 == sub_delta@[i].sign as int); }
    }
    let ghost (l1n0, l1s0, l1p0) = (l1n@, l1s@, l1p@);
    negate_num(&mut l1n, num_delta);
    negate_sub(&mut l1s, sub_delta);
    negate_pre(&mut l1p, pre_delta);
    let mut acc: Index<AccEnt> = Index::new();
    proof { assert(acc_ok(acc)); }
    let ghost acc0 = acc;
    proof {
        assert forall|i: int| 0 <= i < l1n.len() implies (#[trigger] l1n@[i]).sign == 1 || l1n@[i].sign == -1 by {
            if i < l1n0.len() { assert(l1n@[i] == l1n0[i]); }
        }
        assert forall|i: int| 0 <= i < l1s.len() implies (#[trigger] l1s@[i]).sign == 1 || l1s@[i].sign == -1 by {
            if i < l1s0.len() { assert(l1s@[i] == l1s0[i]); }
        }
        assert forall|i: int| 0 <= i < l1p.len() implies (#[trigger] l1p@[i]).sign == 1 || l1p@[i].sign == -1 by {
            if i < l1p0.len() { assert(l1p@[i] == l1p0[i]); }
        }
    }
    join_lists(&s.tab, &mut acc, &l2n, &l2s, &l2p, 1);
    let ghost acc1 = acc;
    join_lists(&s.tab, &mut acc, &l1n, &l1s, &l1p, -1);
    let mut groups = s.groups.share();
    let ghost g0 = groups;
    proof {
        assert forall|ok: KeyView| #![trigger g0.at(ok)] g0.at(ok) == s.groups.at(ok) by {}
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) == gv_c(s.groups, k) && gv_s(g0, k) == gv_s(s.groups, k) by {}
        assert(gm_ok(g0) && pos_ok(g0)) by {
            assert forall|ok: KeyView, ik: KeyView| #![trigger gm_at(g0, ok, ik)] gm_at(g0, ok, ik) == gm_at(s.groups, ok, ik) by {}
        }
        let (rn, rs, rp) = (num_rt_seq_view(nums_by_filing_rows@), sub_rt_seq_view(subs_by_filing_rows@), pre_rt_seq_view(pres_by_filing_rows@));
        assert(seq_view_bytes(nums_by_filing_keys@) == kall && seq_view_bytes(subs_by_filing_keys@) == kall && seq_view_bytes(pres_by_filing_keys@) == kall);
        // the exec lists against the spec lists
        let (vn2, vs2, vp2) = (num_delta_seq_view(l2n@), sub_delta_seq_view(l2s@), pre_delta_seq_view(l2p@));
        let (vn1, vs1, vp1) = (num_delta_seq_view(l1n@), sub_delta_seq_view(l1s@), pre_delta_seq_view(l1p@));
        assert(vn2 == unit_l(rn) && vs2 == unit_l(rs) && vp2 == unit_l(rp));
        assert(num_delta_seq_view(l1n0) == unit_l(rn) && sub_delta_seq_view(l1s0) == unit_l(rs) && pre_delta_seq_view(l1p0) == unit_l(rp));
                assert forall|i: int| 0 <= i < dn.len() implies pn()(dn[i].0) == is_one(cnum(kall))(dn[i].0) by {
            if num_delta@[i].row.adsh__null == 0 { assert(kall.contains(num_delta@[i].row.adsh@)); }
        }
        assert forall|i: int| 0 <= i < dp.len() implies pp()(dp[i].0) == is_one(cpre(kall))(dp[i].0) by {
            if pre_delta@[i].row.adsh__null == 0 { assert(kall.contains(pre_delta@[i].row.adsh@)); }
        }
        assert forall|i: int| 0 <= i < ds.len() implies ps()(ds[i].0) == is_one(csub(kall))(ds[i].0) by {
            assert(kall.contains(sub_delta@[i].row.adsh@));
        }
        lemma_negf_ext(dn, pn(), is_one(cnum(kall)));
        lemma_negf_ext(dp, pp(), is_one(cpre(kall)));
        lemma_negf_ext(ds, ps(), is_one(csub(kall)));
        assert(vn1 == unit_l(rn) + negf(dn, is_one(cnum(kall))));
        assert(vs1 == unit_l(rs) + negf(ds, is_one(csub(kall))));
        assert(vp1 == unit_l(rp) + negf(dp, is_one(cpre(kall))));
        assert forall|k: GK| #![trigger acc_c(acc, k)] acc_c(acc, k) == cntk(db2, k) - cntk(db, k) && acc_s(acc, k) == sumk(db2, k) - sumk(db, k) by {
            lemma_diag_k(k);
            lemma_apply_identity(db, db2, dn, ds, dp, kall, rn, rs, rp, icnt(k));
            lemma_apply_identity(db, db2, dn, ds, dp, kall, rn, rs, rp, ival(k));
            assert(acc_c(acc0, k) == 0 && acc_s(acc0, k) == 0);
        }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) >= 0 by { lemma_cnt_facts(db2, k); }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) == 0 ==> gv_s(g0, k) + acc_s(acc, k) == 0 by { lemma_cnt_facts(db2, k); }
    }
    merge_acc(&mut groups, &acc);
    let tab = s.tab.clone();
    proof {
        assert(forall|k: GK| #![trigger gv_c(groups, k)] gv_c(groups, k) == cntk(db2, k) && gv_s(groups, k) == sumk(db2, k)) by {
            assert forall|k: GK| #![trigger gv_c(groups, k)] gv_c(groups, k) == cntk(db2, k) && gv_s(groups, k) == sumk(db2, k) by {
                assert(gv_c(g0, k) == gv_c(s.groups, k) && gv_s(g0, k) == gv_s(s.groups, k));
            }
        }
    }
    Ok(State { groups, tab, db: Ghost(db2) })
}


pub fn build(
    subs: Vec<SubRt>,
    nums: Vec<NumRt>,
    pres: Vec<PreRt>,
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_subs_ok(db, subs@),
        scan_nums_ok(db, nums@),
        scan_pres_ok(db, pres@),
    ensures
        r matches Some(s) ==> s.db() == db,
        r matches Some(s) ==> history(s) == 0,
        envelope(db) ==> r is Some,
{
    let tab = Arc::new(pow2_tab());
    let ln = wrap_num(&nums);
    let ls = wrap_sub(&subs);
    let lp = wrap_pre(&pres);
    let mut acc: Index<AccEnt> = Index::new();
    let ghost acc0 = acc;
    proof { assert(acc_ok(acc)); }
    join_lists(&tab, &mut acc, &ln, &ls, &lp, 1);
    let mut groups: GMap = Index::new();
    let ghost g0 = groups;
    proof {
        assert forall|k: GK| #![trigger acc_c(acc, k)] acc_c(acc, k) == cntk(db, k) && acc_s(acc, k) == sumk(db, k) by {
            lemma_diag_k(k);
            lemma_build_identity(db, nums@, subs@, pres@, icnt(k));
            lemma_build_identity(db, nums@, subs@, pres@, ival(k));
            assert(acc_c(acc0, k) == 0 && acc_s(acc0, k) == 0);
        }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) == 0 && gv_s(g0, k) == 0 by {}
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) >= 0 by { lemma_cnt_facts(db, k); }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) == 0 ==> gv_s(g0, k) + acc_s(acc, k) == 0 by { lemma_cnt_facts(db, k); }
        assert(gm_ok(g0) && pos_ok(g0));
    }
    merge_acc(&mut groups, &acc);
    Some(State { groups, tab, db: Ghost(db) })
}

#[verifier::external_body]
pub fn execute(
    s: &State,
    p1: i64,
    p2: &[u8],
    p3: &[u8],
) -> (r: Option<Vec<(Option<Vec<u8>>, Option<Vec<u8>>, Option<Vec<u8>>, Option<Vec<u8>>, Option<u64>, i64)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1 as int, p2@, p3@), out_view(out@)),
        envelope(s.db()) && answer_fits(s.db(), params(p1 as int, p2@, p3@)) ==> r is Some,
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
{
    unimplemented!()
}


} // verus!
