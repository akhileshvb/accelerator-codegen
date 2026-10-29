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
use crate::rep::*;
use crate::rank::*;

verus! {

pub open spec fn state_vals(groups: GMap, db: Db) -> bool {
    &&& forall|k: GK| #![trigger gv_c(groups, k)] gv_c(groups, k) == cntk(db, k)
    &&& forall|k: GK| #![trigger gv_s(groups, k)] gv_s(groups, k) == sumk(db, k)
}

pub open spec fn state_ok(groups: GMap, tab: Pow2Tab, db: Db) -> bool {
    &&& gm_ok(groups)
    &&& pos_ok(groups)
    &&& tab.ok()
    &&& rep_db(db)
    &&& state_vals(groups, db)
}

/// The rank index, when kept, lists every group by the answer's order.
pub open spec fn rank_state_ok(groups: GMap, rank: GMap, ranked: bool) -> bool {
    &&& nm_ok(rank)
    &&& ranked ==> rank_ok(groups, rank)
}

pub struct State {
    groups: GMap,
    rank: GMap,
    ranked: bool,
    tab: Arc<Pow2Tab>,
    db: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    closed spec fn valid(self) -> bool {
        &&& state_ok(self.groups, *self.tab, self.db@)
        &&& rank_state_ok(self.groups, self.rank, self.ranked)
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

/// The exec-side predicates of the old-row lists agree with the algebra's on the changed rows.
proof fn lemma_negf_preds(dn: Seq<NumDelta>, dp: Seq<PreDelta>, ds: Seq<SubDelta>, kall: Seq<Seq<u8>>)
    requires kall == all_keys(dn, dp, ds),
    ensures
        negf(num_delta_seq_view(dn), pn()) == negf(num_delta_seq_view(dn), is_one(cnum(kall))),
        negf(pre_delta_seq_view(dp), pp()) == negf(pre_delta_seq_view(dp), is_one(cpre(kall))),
        negf(sub_delta_seq_view(ds), ps()) == negf(sub_delta_seq_view(ds), is_one(csub(kall))),
{
    lemma_keys_cover(dn, dp, ds);
    let (vn, vp, vs) = (num_delta_seq_view(dn), pre_delta_seq_view(dp), sub_delta_seq_view(ds));
    assert forall|i: int| 0 <= i < vn.len() implies pn()(vn[i].0) == is_one(cnum(kall))(vn[i].0) by {
        if dn[i].row.adsh__null == 0 { assert(kall.contains(dn[i].row.adsh@)); }
    }
    assert forall|i: int| 0 <= i < vp.len() implies pp()(vp[i].0) == is_one(cpre(kall))(vp[i].0) by {
        if dp[i].row.adsh__null == 0 { assert(kall.contains(dp[i].row.adsh@)); }
    }
    assert forall|i: int| 0 <= i < vs.len() implies ps()(vs[i].0) == is_one(csub(kall))(vs[i].0) by {
        assert(kall.contains(ds[i].row.adsh@));
    }
    lemma_negf_ext(vn, pn(), is_one(cnum(kall)));
    lemma_negf_ext(vp, pp(), is_one(cpre(kall)));
    lemma_negf_ext(vs, ps(), is_one(csub(kall)));
}

/// The contributions of the two joins are the change in every group.
proof fn lemma_apply_acc(db: Db, db2: Db, dn: Seq<(NumRow, int)>, ds: Seq<(SubRow, int)>, dp: Seq<(PreRow, int)>,
    kall: Seq<Seq<u8>>, rn: Seq<NumRow>, rs: Seq<SubRow>, rp: Seq<PreRow>,
    vn2: Seq<(NumRow, int)>, vs2: Seq<(SubRow, int)>, vp2: Seq<(PreRow, int)>,
    vn1: Seq<(NumRow, int)>, vs1: Seq<(SubRow, int)>, vp1: Seq<(PreRow, int)>,
    vn10: Seq<(NumRow, int)>, vs10: Seq<(SubRow, int)>, vp10: Seq<(PreRow, int)>,
    acc0: Index<AccEnt>, acc1: Index<AccEnt>, acc2: Index<AccEnt>)
    requires
        bag_step(db.num, db2.num, dn), bag_step(db.sub, db2.sub, ds), bag_step(db.pre, db2.pre, dp),
        restr(unit_l(rn), db2.num, cnum(kall)), restr(unit_l(rs), db2.sub, csub(kall)), restr(unit_l(rp), db2.pre, cpre(kall)),
        forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).0.adsh is Some ==> kall.contains(dn[i].0.adsh->0),
        forall|i: int| 0 <= i < ds.len() ==> kall.contains((#[trigger] ds[i]).0.adsh),
        forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).0.adsh is Some ==> kall.contains(dp[i].0.adsh->0),
        vn2 == unit_l(rn), vs2 == unit_l(rs), vp2 == unit_l(rp),
        vn10 == unit_l(rn), vs10 == unit_l(rs), vp10 == unit_l(rp),
        vn1 == vn10 + negf(dn, is_one(cnum(kall))), vs1 == vs10 + negf(ds, is_one(csub(kall))), vp1 == vp10 + negf(dp, is_one(cpre(kall))),
        acc_ok(acc0), forall|k: GK| #[trigger] acc_c(acc0, k) == 0, forall|k: GK| #[trigger] acc_s(acc0, k) == 0,
        acc_delta(acc0, acc1, nn_c(vn2, vs2, vp2, 1), nn_s(vn2, vs2, vp2, 1)),
        acc_delta(acc1, acc2, nn_c(vn1, vs1, vp1, -1), nn_s(vn1, vs1, vp1, -1)),
    ensures
        forall|k: GK| #![trigger acc_c(acc2, k)] acc_c(acc2, k) == cntk(db2, k) - cntk(db, k),
        forall|k: GK| #![trigger acc_s(acc2, k)] acc_s(acc2, k) == sumk(db2, k) - sumk(db, k),
{
    assert forall|k: GK| #![trigger acc_c(acc2, k)] acc_c(acc2, k) == cntk(db2, k) - cntk(db, k) by {
        lemma_diag_k(k);
        lemma_apply_identity(db, db2, dn, ds, dp, kall, rn, rs, rp, icnt(k));
        assert(acc_c(acc1, k) == 0 + nn_c(vn2, vs2, vp2, 1)(k));
        assert(acc_c(acc2, k) == acc_c(acc1, k) + nn_c(vn1, vs1, vp1, -1)(k));
    }
    assert forall|k: GK| #![trigger acc_s(acc2, k)] acc_s(acc2, k) == sumk(db2, k) - sumk(db, k) by {
        lemma_diag_k(k);
        lemma_apply_identity(db, db2, dn, ds, dp, kall, rn, rs, rp, ival(k));
        assert(acc_s(acc1, k) == 0 + nn_s(vn2, vs2, vp2, 1)(k));
        assert(acc_s(acc2, k) == acc_s(acc1, k) + nn_s(vn1, vs1, vp1, -1)(k));
    }
}

#[verifier::spinoff_prover]
#[verifier::rlimit(300)]
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
        assert(state_vals(s.groups, db));
        assert forall|k: GK| #![trigger gv_c(s.groups, k)] gv_c(s.groups, k) == cntk(db, k) by {}
        assert forall|k: GK| #![trigger gv_s(s.groups, k)] gv_s(s.groups, k) == sumk(db, k) by {}
    }
    let ghost kall = all_keys(num_delta@, pre_delta@, sub_delta@);
    let ghost (dn, dp, ds) = (num_delta_seq_view(num_delta@), pre_delta_seq_view(pre_delta@), sub_delta_seq_view(sub_delta@));
    proof {
        lemma_keys_cover(num_delta@, pre_delta@, sub_delta@);
        lemma_rescan_num(db2, nums_by_filing_keys@, nums_by_filing_rows@);
        lemma_rescan_sub(db2, subs_by_filing_keys@, subs_by_filing_rows@);
        lemma_rescan_pre(db2, pres_by_filing_keys@, pres_by_filing_rows@);
        assert(unit_weights(dn) && unit_weights(dp) && unit_weights(ds));
        assert forall|i: int| 0 <= i < dn.len() implies is_rep((#[trigger] dn[i]).0) by {
            let r = num_delta_seq_view(num_delta@)[i].0;
            if r.value is Some { assert(f64_val(num_delta@[i].row.value) == r.value->0); }
        }
        lemma_rep_step(db, db2, dn);
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
    let mut rank = s.rank.share();
    let mut ranked = s.ranked;
    let ghost g0 = groups;
    let ghost r0 = rank;
    proof {
        assert forall|ok: KeyView| #![trigger r0.at(ok)] r0.at(ok) == s.rank.at(ok) by {}
        assert forall|ok: KeyView, rk: KeyView| #![trigger gm_at(r0, ok, rk)] gm_at(r0, ok, rk) == gm_at(s.rank, ok, rk) by {}
        assert forall|ok: KeyView| #![trigger g0.at(ok)] g0.at(ok) == s.groups.at(ok) by {}
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) == gv_c(s.groups, k) && gv_s(g0, k) == gv_s(s.groups, k) by {}
        assert(gm_ok(g0) && pos_ok(g0)) by {
            assert forall|ok: KeyView, ik: KeyView| #![trigger gm_at(g0, ok, ik)] gm_at(g0, ok, ik) == gm_at(s.groups, ok, ik) by {}
        }
        let (rn, rs, rp) = (num_rt_seq_view(nums_by_filing_rows@), sub_rt_seq_view(subs_by_filing_rows@), pre_rt_seq_view(pres_by_filing_rows@));
        assert(seq_view_bytes(nums_by_filing_keys@) == kall && seq_view_bytes(subs_by_filing_keys@) == kall && seq_view_bytes(pres_by_filing_keys@) == kall);
        assert(num_delta_seq_view(l1n0) == unit_l(rn) && sub_delta_seq_view(l1s0) == unit_l(rs) && pre_delta_seq_view(l1p0) == unit_l(rp));
        lemma_negf_preds(num_delta@, pre_delta@, sub_delta@, kall);
        lemma_apply_acc(db, db2, dn, ds, dp, kall, rn, rs, rp,
            num_delta_seq_view(l2n@), sub_delta_seq_view(l2s@), pre_delta_seq_view(l2p@),
            num_delta_seq_view(l1n@), sub_delta_seq_view(l1s@), pre_delta_seq_view(l1p@),
            num_delta_seq_view(l1n0), sub_delta_seq_view(l1s0), pre_delta_seq_view(l1p0), acc0, acc1, acc);
        assert forall|ok: KeyView, ik: KeyView| #![trigger gm_at(g0, ok, ik)] gm_at(g0, ok, ik) == gm_at(s.groups, ok, ik) by {}
        assert(rank_state_ok(g0, r0, s.ranked)) by {
            if s.ranked { lemma_rank_ok_ext(s.groups, g0, s.rank, r0); }
        }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) == cntk(db2, k) by {}
        assert forall|k: GK| #![trigger gv_s(g0, k)] gv_s(g0, k) + acc_s(acc, k) == sumk(db2, k) by {
            assert(gv_s(g0, k) == gv_s(s.groups, k));
            assert(gv_s(s.groups, k) == sumk(db, k));
        }
    }
    merge_acc(&mut groups, &mut rank, &mut ranked, &acc, &s.tab, Ghost(db2));
    let tab = s.tab.clone();
    proof {
        assert forall|k: GK| #![trigger gv_c(groups, k)] gv_c(groups, k) == cntk(db2, k) by {
            assert(gv_c(g0, k) == gv_c(s.groups, k));
        }
        assert forall|k: GK| #![trigger gv_s(groups, k)] gv_s(groups, k) == sumk(db2, k) by {
            assert(gv_s(g0, k) == gv_s(s.groups, k));
        }
        assert(state_vals(groups, db2));
    }
    Ok(State { groups, rank, ranked, tab, db: Ghost(db2) })
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
    let mut rank: GMap = Index::new();
    let mut ranked = true;
    let ghost g0 = groups;
    proof {
        lemma_rep_scan(db, nums@);
        assert forall|k: GK| #![trigger acc_c(acc, k)] acc_c(acc, k) == cntk(db, k) by {
            lemma_diag_k(k);
            lemma_build_identity(db, nums@, subs@, pres@, icnt(k));
            assert(acc_c(acc0, k) == 0);
        }
        assert forall|k: GK| #![trigger acc_s(acc, k)] acc_s(acc, k) == sumk(db, k) by {
            lemma_diag_k(k);
            lemma_build_identity(db, nums@, subs@, pres@, ival(k));
            assert(acc_s(acc0, k) == 0);
        }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) == 0 && gv_s(g0, k) == 0 by {}
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) == cntk(db, k) by {}
        assert forall|k: GK| #![trigger gv_s(g0, k)] gv_s(g0, k) + acc_s(acc, k) == sumk(db, k) by {}
        assert(gm_ok(g0) && pos_ok(g0));
        assert(nm_ok(rank) && rank_ok(g0, rank));
    }
    merge_acc(&mut groups, &mut rank, &mut ranked, &acc, &tab, Ghost(db));
    proof {
        assert forall|k: GK| #![trigger gv_c(groups, k)] gv_c(groups, k) == cntk(db, k) by {}
        assert forall|k: GK| #![trigger gv_s(groups, k)] gv_s(groups, k) == sumk(db, k) by {}
        assert(state_vals(groups, db));
    }
    Some(State { groups, rank, ranked, tab, db: Ghost(db) })
}

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
    proof { use_type_invariant(s); }
    let rows = crate::emit::gather(&s.groups, &s.tab, Ghost(s.db()), p1, p2, p3);
    crate::emit::finish(rows, Ghost(s.db()), p1, p2, p3)
}


} // verus!
