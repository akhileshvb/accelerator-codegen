// The kernel: per (year, uom) the groups (tag, version) of the num rows that have no pre row,
// with their exact counts and sums, maintained per filing.
#![allow(unused_imports)]
#![verifier::loop_isolation(false)]
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
use crate::cmap::*;
use crate::keys::*;
use crate::loops::*;
use crate::emit::*;
use crate::answer::*;
use crate::rank::*;
use crate::wideval::*;

verus! {

pub open spec fn state_vals(groups: GMap, db: Db) -> bool {
    &&& forall|k: GK| #![trigger gv_c(groups, k)] gv_c(groups, k) == cntk(db, k)
    &&& forall|k: GK| #![trigger gv_s(groups, k)] gv_s(groups, k) == sumk(db, k)
}

pub open spec fn state_ok(groups: GMap, tab: Pow2Tab, db: Db) -> bool {
    &&& gm_ok(groups)
    &&& pos_ok(groups)
    &&& tab.ok()
    &&& state_vals(groups, db)
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

pub open spec fn rescan_nums_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>) -> Seq<Seq<u8>> {
    all_keys(num_delta, pre_delta)
}

pub open spec fn rescan_pres_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>) -> Seq<Seq<u8>> {
    all_keys(num_delta, pre_delta)
}

pub fn rescan_nums_by_filing_keys(s: &State, num_delta: &Vec<NumDelta>, pre_delta: &Vec<PreDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == rescan_nums_by_filing_wanted(*s, num_delta@, pre_delta@),
{
    collect_keys(num_delta, pre_delta)
}

pub fn rescan_pres_by_filing_keys(s: &State, num_delta: &Vec<NumDelta>, pre_delta: &Vec<PreDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == rescan_pres_by_filing_wanted(*s, num_delta@, pre_delta@),
{
    collect_keys(num_delta, pre_delta)
}

proof fn lemma_rn_ok(db2: Db, keys: Seq<Vec<u8>>, rows: Seq<NumRt>)
    requires rescan_nums_by_filing_ok(db2, keys, rows),
    ensures rn_ok(db2, seq_view_bytes(keys), num_rt_seq_view(rows)),
{
    assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(num_rt_seq_view(rows), f) == sum(db2.num, |n: NumRow| if n.value is Some && in_keys(seq_view_bytes(keys), n.adsh) { f(n) } else { 0 }) by {
        let g1 = |num: NumRow| if ((num.value is Some)) && (num.adsh is Some && seq_view_bytes(keys).contains(num.adsh->0)) { f(num) } else { 0 };
        let g2 = |n: NumRow| if n.value is Some && in_keys(seq_view_bytes(keys), n.adsh) { f(n) } else { 0 };
        assert forall|n: NumRow| #[trigger] g1(n) == g2(n) by {}
        lemma_sum_ext_all(db2.num, g1, g2);
    }
}

proof fn lemma_rp_ok(db2: Db, keys: Seq<Vec<u8>>, rows: Seq<PreRt>)
    requires rescan_pres_by_filing_ok(db2, keys, rows),
    ensures rp_ok(db2, seq_view_bytes(keys), pre_rt_seq_view(rows)),
{
    assert forall|f: spec_fn(PreRow) -> int| #[trigger] sum(pre_rt_seq_view(rows), f) == sum(db2.pre, |p: PreRow| if in_keys(seq_view_bytes(keys), p.adsh) { f(p) } else { 0 }) by {
        let g1 = |pre: PreRow| if pre.adsh is Some && seq_view_bytes(keys).contains(pre.adsh->0) { f(pre) } else { 0 };
        let g2 = |p: PreRow| if in_keys(seq_view_bytes(keys), p.adsh) { f(p) } else { 0 };
        assert forall|p: PreRow| #[trigger] g1(p) == g2(p) by {}
        lemma_sum_ext_all(db2.pre, g1, g2);
    }
}

/// The accumulated change is the change of every group.
proof fn lemma_apply_acc(db: Db, db2: Db, dn: Seq<(NumRow, int)>, dp: Seq<(PreRow, int)>,
    keys: Seq<Seq<u8>>, rn: Seq<NumRow>, rp: Seq<PreRow>, k: GK)
    requires
        bag_step(db.num, db2.num, dn), bag_step(db.pre, db2.pre, dp),
        covered_n(dn, keys), covered_p(dp, keys),
        rn_ok(db2, keys, rn), rp_ok(db2, keys, rp),
    ensures
        cntk(db2, k) == cntk(db, k) + sum(rn, |n: NumRow| rescan_w(rp, dp)(n) * ic(k)(n)) + wsum(dn, |n: NumRow| z1(rp, dp, n) * ic(k)(n)),
        sumk(db2, k) == sumk(db, k) + sum(rn, |n: NumRow| rescan_w(rp, dp)(n) * iv(k)(n)) + wsum(dn, |n: NumRow| z1(rp, dp, n) * iv(k)(n)),
{
    lemma_apply_identity(db, db2, dn, dp, keys, rn, rp, ic(k));
    lemma_apply_identity(db, db2, dn, dp, keys, rn, rp, iv(k));
    assert forall|n: NumRow| #[trigger] ((z2(rp, n) - z1(rp, dp, n)) * ic(k)(n)) == rescan_w(rp, dp)(n) * ic(k)(n) by {}
    assert forall|n: NumRow| #[trigger] ((z2(rp, n) - z1(rp, dp, n)) * iv(k)(n)) == rescan_w(rp, dp)(n) * iv(k)(n) by {}
    lemma_sum_ext_all(rn, |n: NumRow| (z2(rp, n) - z1(rp, dp, n)) * ic(k)(n), |n: NumRow| rescan_w(rp, dp)(n) * ic(k)(n));
    lemma_sum_ext_all(rn, |n: NumRow| (z2(rp, n) - z1(rp, dp, n)) * iv(k)(n), |n: NumRow| rescan_w(rp, dp)(n) * iv(k)(n));
}

pub fn apply(
    s: &State,
    num_delta: &Vec<NumDelta>,
    pre_delta: &Vec<PreDelta>,
    nums_by_filing_keys: &Vec<Vec<u8>>,
    nums_by_filing_rows: &Vec<NumRt>,
    pres_by_filing_keys: &Vec<Vec<u8>>,
    pres_by_filing_rows: &Vec<PreRt>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, num_delta_seq_view(num_delta@), pre_delta_seq_view(pre_delta@)),
        seq_view_bytes(nums_by_filing_keys@) == rescan_nums_by_filing_wanted(*s, num_delta@, pre_delta@),
        rescan_nums_by_filing_ok(db2, nums_by_filing_keys@, nums_by_filing_rows@),
        seq_view_bytes(pres_by_filing_keys@) == rescan_pres_by_filing_wanted(*s, num_delta@, pre_delta@),
        rescan_pres_by_filing_ok(db2, pres_by_filing_keys@, pres_by_filing_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        r matches Ok(s2) ==> history(s2) <= history(*s) + num_delta@.len() + pre_delta@.len(),
        envelope(s.db()) && envelope(db2) && history(*s) + num_delta@.len() + pre_delta@.len() <= max_history() ==> r is Ok,
{
    proof { use_type_invariant(s); }
    let ghost db = s.db();
    let ghost kall = all_keys(num_delta@, pre_delta@);
    let ghost (dn, dp) = (num_delta_seq_view(num_delta@), pre_delta_seq_view(pre_delta@));
    let ghost (rn, rp) = (num_rt_seq_view(nums_by_filing_rows@), pre_rt_seq_view(pres_by_filing_rows@));
    proof {
        lemma_keys_cover(num_delta@, pre_delta@);
        lemma_rn_ok(db2, nums_by_filing_keys@, nums_by_filing_rows@);
        lemma_rp_ok(db2, pres_by_filing_keys@, pres_by_filing_rows@);
        assert(seq_view_bytes(nums_by_filing_keys@) == kall && seq_view_bytes(pres_by_filing_keys@) == kall);
        assert(unit_weights(dn) && unit_weights(dp));
        assert forall|i: int| 0 <= i < num_delta.len() implies (#[trigger] num_delta@[i]).sign == 1 || num_delta@[i].sign == -1 by { assert(dn[i].1 == num_delta@[i].sign as int); }
        assert forall|i: int| 0 <= i < pre_delta.len() implies (#[trigger] pre_delta@[i]).sign == 1 || pre_delta@[i].sign == -1 by { assert(dp[i].1 == pre_delta@[i].sign as int); }
        assert forall|i: int| 0 <= i < dn.len() && (#[trigger] dn[i]).0.adsh is Some implies kall.contains(dn[i].0.adsh->0) by {
            assert(num_delta@[i].row.adsh__null == 0);
        }
        assert forall|i: int| 0 <= i < dp.len() && (#[trigger] dp[i]).0.adsh is Some implies kall.contains(dp[i].0.adsh->0) by {
            assert(pre_delta@[i].row.adsh__null == 0);
        }
        assert(rp.len() == pres_by_filing_rows@.len());
        assert(pres_by_filing_rows.len() <= usize::MAX);
    }
    let c2 = count_pres(pres_by_filing_rows);
    let c1 = old_counts(&c2, pre_delta, Ghost(rp));
    let mut acc: Index<AccEnt> = Index::new();
    let ghost acc0 = acc;
    proof { assert(acc_ok(acc)); assert forall|k: GK| #[trigger] acc_c(acc0, k) == 0 && acc_s(acc0, k) == 0 by {} }
    acc_rescan(&s.tab, &mut acc, nums_by_filing_rows, &c2, &c1, Ghost(rp), Ghost(dp));
    acc_changed(&s.tab, &mut acc, num_delta, &c2, &c1, Ghost(rp), Ghost(dp));
    let mut groups = s.groups.share();
    let ghost g0 = groups;
    proof {
        assert forall|ok: KeyView| #![trigger g0.at(ok)] g0.at(ok) == s.groups.at(ok) by {}
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) == gv_c(s.groups, k) && gv_s(g0, k) == gv_s(s.groups, k) by {}
        assert(gm_ok(g0) && pos_ok(g0)) by {
            assert forall|ok: KeyView, ik: KeyView| #![trigger gm_at(g0, ok, ik)] gm_at(g0, ok, ik) == gm_at(s.groups, ok, ik) by {}
        }
        assert(state_ok(s.groups, *s.tab, s.db@));
        assert(state_vals(s.groups, db));
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) == cntk(db2, k) by {
            lemma_apply_acc(db, db2, dn, dp, kall, rn, rp, k);
            assert(gv_c(s.groups, k) == cntk(db, k));
        }
        assert forall|k: GK| #![trigger gv_s(g0, k)] gv_s(g0, k) + acc_s(acc, k) == sumk(db2, k) by {
            lemma_apply_acc(db, db2, dn, dp, kall, rn, rp, k);
            assert(gv_s(s.groups, k) == sumk(db, k));
        }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) >= 0 by { lemma_cnt_facts(db2, k); }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) == 0 ==> gv_s(g0, k) + acc_s(acc, k) == 0 by { lemma_cnt_facts(db2, k); }
    }
    merge_acc(&mut groups, &acc);
    let tab = s.tab.clone();
    proof {
        assert forall|k: GK| #![trigger gv_c(groups, k)] gv_c(groups, k) == cntk(db2, k) by {}
        assert forall|k: GK| #![trigger gv_s(groups, k)] gv_s(groups, k) == sumk(db2, k) by {}
        assert(state_vals(groups, db2));
    }
    Ok(State { groups, tab, db: Ghost(db2) })
}

pub fn build(
    nums: Vec<NumRt>,
    pres: Vec<PreRt>,
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_nums_ok(db, nums@),
        scan_pres_ok(db, pres@),
    ensures
        r matches Some(s) ==> s.db() == db,
        r matches Some(s) ==> history(s) == 0,
        envelope(db) ==> r is Some,
{
    let tab = Arc::new(pow2_tab());
    let ghost (rn, rp) = (num_rt_seq_view(nums@), pre_rt_seq_view(pres@));
    let c = count_pres(&pres);
    let mut acc: Index<AccEnt> = Index::new();
    let ghost acc0 = acc;
    proof { assert(acc_ok(acc)); assert forall|k: GK| #[trigger] acc_c(acc0, k) == 0 && acc_s(acc0, k) == 0 by {} }
    acc_scan(&tab, &mut acc, &nums, &c, Ghost(rp));
    let mut groups: GMap = Index::new();
    let ghost g0 = groups;
    proof {
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) == 0 && gv_s(g0, k) == 0 by {}
        assert(gm_ok(g0) && pos_ok(g0));
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) == cntk(db, k) by { lemma_build_identity(db, rn, rp, ic(k)); }
        assert forall|k: GK| #![trigger gv_s(g0, k)] gv_s(g0, k) + acc_s(acc, k) == sumk(db, k) by { lemma_build_identity(db, rn, rp, iv(k)); }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) >= 0 by { lemma_cnt_facts(db, k); }
        assert forall|k: GK| #![trigger gv_c(g0, k)] gv_c(g0, k) + acc_c(acc, k) == 0 ==> gv_s(g0, k) + acc_s(acc, k) == 0 by { lemma_cnt_facts(db, k); }
    }
    merge_acc(&mut groups, &acc);
    proof {
        assert forall|k: GK| #![trigger gv_c(groups, k)] gv_c(groups, k) == cntk(db, k) by {}
        assert forall|k: GK| #![trigger gv_s(groups, k)] gv_s(groups, k) == sumk(db, k) by {}
        assert(state_vals(groups, db));
    }
    Some(State { groups, tab, db: Ghost(db) })
}

/// The answer rows as the host gets them.
pub fn out_tuple(r: &OutR) -> (o: (Option<Vec<u8>>, Option<Vec<u8>>, i64, Option<u64>))
    requires bits_ok(*r), r.bits is Some, 0 < r.ci <= 9223372036854775807,
    ensures out_view(seq![o])[0] == rview(*r),
{
    let ci64 = r.ci as i64;
    (copy_opt(&r.tag), copy_opt(&r.ver), ci64, r.bits)
}

pub fn execute(
    s: &State,
    p1: i64,
    p2: &[u8],
) -> (r: Option<Vec<(Option<Vec<u8>>, Option<Vec<u8>>, i64, Option<u64>)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1 as int, p2@), out_view(out@)),
        envelope(s.db()) && answer_fits(s.db(), params(p1 as int, p2@)) ==> r is Some,
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
{
    proof { use_type_invariant(s); }
    let ghost db = s.db();
    let ghost p = params(p1 as int, p2@);
    let t = match gather(&s.groups, &s.tab, Ghost(db), p1, p2) {
        Some(t) => t,
        None => return None,
    };
    let rows = t.rows();
    let ghost es = t.entries();
    let ghost full = es.map_values(|e: (KeyView, OutR)| e.1).map_values(|r: OutR| rview(r));
    proof {
        t.model();
        assert(rows@.map_values(|row: (Key, Arc<OutR>)| (row.0.view(), *row.1)) == es);
        assert(full_result(db, p, full));
    }
    let n = if rows.len() < 100 { rows.len() } else { 100 };
    let mut out: Vec<(Option<Vec<u8>>, Option<Vec<u8>>, i64, Option<u64>)> = Vec::new();
    let mut i = 0;
    while i < n
        invariant
            i <= n, n <= rows.len(), n <= 100, n == if rows.len() < 100 { rows.len() } else { 100 },
            rows@.map_values(|row: (Key, Arc<OutR>)| (row.0.view(), *row.1)) == es,
            full == es.map_values(|e: (KeyView, OutR)| e.1).map_values(|r: OutR| rview(r)),
            full_result(db, p, full),
            tmp_ok(db, p, t),
            out.len() == i,
            forall|j: int| 0 <= j < i ==> (#[trigger] out_view(out@)[j]) == full[j],
        decreases n - i,
    {
        let r = &*rows[i].1;
        proof {
            assert(es[i as int].1 == *rows@[i as int].1);
            assert(full[i as int] == rview(*r));
        }
        if r.bits.is_none() || r.ci > 9223372036854775807 {
            proof {
                let o = limit_rows(full, 100);
                assert(query_result(db, p, o));
                assert(o[i as int] == full[i as int]);
                assert(!out_fits(o[i as int]));
                if envelope(db) && answer_fits(db, p) { assert(out_fits(o[i as int])); }
            }
            return None;
        }
        proof {
            let rs = es.map_values(|e: (KeyView, OutR)| e.1);
            assert(rs[i as int] == *r);
            assert(rs[i as int].ci as int == rs[i as int].cnt@);
            assert(row_ok(db, p, full[i as int]));
            assert(full[i as int].cnt > 10);
        }
        let tup = out_tuple(r);
        out.push(tup);
        i += 1;
    }
    proof {
        assert(out_view(out@) =~= limit_rows(full, 100));
        assert(query_result(db, p, out_view(out@)));
    }
    Some(out)
}

} // verus!
