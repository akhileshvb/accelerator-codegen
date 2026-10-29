// The kernel: per class the best groups and, for the rest, each unit's best group; maintained per filing batch.
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::algebra::*;
use crate::accmap::*;
use crate::join::*;
use crate::wideval::*;
use crate::glue_proofs::*;
use crate::lists::*;
use crate::rep::*;
use crate::grp::*;
use crate::inv::*;
use crate::units::*;
use crate::plan::*;
use crate::keys::*;
use crate::canon::*;
use crate::core::*;
use crate::frame::*;
use crate::bridge::*;
use crate::kfacts::*;
use crate::umaps::*;
use crate::tbl::*;
use crate::subs::*;
use crate::kbuild::*;
use crate::evict::*;
use crate::ucls::*;
use crate::esc::*;
use crate::wideval::*;

verus! {

pub struct State {
    cls: Index<ClassRec>,
    uc: Index<ClsSet>,
    fm: Index<Vec<UnitRef>>,
    um: Index<UnitRec>,
    tab: Arc<Pow2Tab>,
    db: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    closed spec fn valid(self) -> bool {
        &&& cls_ok(self.db@, self.cls)
        &&& uc_ok(self.uc, self.db@)
        &&& fm_ok(self.fm, self.db@)
        &&& um_ok(self.um, self.db@)
        &&& rep_db(self.db@)
        &&& self.tab.ok()
    }
}

/// The state keeps nothing that grows with past writes.
pub open spec fn history(s: State) -> nat {
    0
}

pub closed spec fn rescan_subs_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>) -> Seq<Seq<u8>> {
    wanted_def(s.cls, s.fm, s.um, num_delta, pre_delta, sub_delta)
}

pub closed spec fn rescan_nums_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>) -> Seq<Seq<u8>> {
    wanted_def(s.cls, s.fm, s.um, num_delta, pre_delta, sub_delta)
}

pub closed spec fn rescan_pres_by_filing_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>) -> Seq<Seq<u8>> {
    wanted_def(s.cls, s.fm, s.um, num_delta, pre_delta, sub_delta)
}

fn wanted_keys(s: &State, num_delta: &Vec<NumDelta>, pre_delta: &Vec<PreDelta>, sub_delta: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == wanted_def(s.cls, s.fm, s.um, num_delta@, pre_delta@, sub_delta@),
{
    proof { use_type_invariant(s); }
    proof { assert(tbl_str(s.cls)) by { assert forall|ok: KeyView| s.cls.at(ok) is Some implies ok == cls_key(s.cls.at(ok)->0) && crec_struct(s.cls.at(ok)->0) by {} } }
    proof { lemma_unit_cls(s.db(), s.cls, s.uc); }
    let p = plan_full(&s.cls, &s.uc, &s.fm, &s.um, num_delta, pre_delta, sub_delta);
    proof {
        let pred = wanted_pred(s.cls, s.fm, s.um, all_keys(num_delta@, pre_delta@, sub_delta@), sub_delta@);
        let chosen = wanted_def(s.cls, s.fm, s.um, num_delta@, pre_delta@, sub_delta@);
        assert(canon_keys(chosen, pred));
        lemma_canon_unique(seq_view_bytes(p.keys@), chosen, pred);
    }
    p.keys
}

pub fn rescan_subs_by_filing_keys(s: &State, num_delta: &Vec<NumDelta>, pre_delta: &Vec<PreDelta>, sub_delta: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == rescan_subs_by_filing_wanted(*s, num_delta@, pre_delta@, sub_delta@),
{
    wanted_keys(s, num_delta, pre_delta, sub_delta)
}

pub fn rescan_nums_by_filing_keys(s: &State, num_delta: &Vec<NumDelta>, pre_delta: &Vec<PreDelta>, sub_delta: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == rescan_nums_by_filing_wanted(*s, num_delta@, pre_delta@, sub_delta@),
{
    wanted_keys(s, num_delta, pre_delta, sub_delta)
}

pub fn rescan_pres_by_filing_keys(s: &State, num_delta: &Vec<NumDelta>, pre_delta: &Vec<PreDelta>, sub_delta: &Vec<SubDelta>) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == rescan_pres_by_filing_wanted(*s, num_delta@, pre_delta@, sub_delta@),
{
    wanted_keys(s, num_delta, pre_delta, sub_delta)
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
    crate::read::read(&s.cls, Ghost(s.db()), p1, p2, p3)
}

#[verifier::rlimit(500)]
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
    proof { assert(tbl_str(s.cls)) by { assert forall|ok: KeyView| s.cls.at(ok) is Some implies ok == cls_key(s.cls.at(ok)->0) && crec_struct(s.cls.at(ok)->0) by {} } }
    proof { lemma_unit_cls(db, s.cls, s.uc); }
    let plan = plan_full(&s.cls, &s.uc, &s.fm, &s.um, num_delta, pre_delta, sub_delta);
    let ghost ka = all_keys(num_delta@, pre_delta@, sub_delta@);
    let ghost keys = seq_view_bytes(subs_by_filing_keys@);
    let ghost (dn, dp, ds) = (num_delta_seq_view(num_delta@), pre_delta_seq_view(pre_delta@), sub_delta_seq_view(sub_delta@));
    let ghost pred = wanted_pred(s.cls, s.fm, s.um, ka, sub_delta@);
    proof {
        let chosen = wanted_def(s.cls, s.fm, s.um, num_delta@, pre_delta@, sub_delta@);
        assert(canon_keys(chosen, pred));
        assert(keys == chosen && seq_view_bytes(nums_by_filing_keys@) == chosen && seq_view_bytes(pres_by_filing_keys@) == chosen);
        lemma_canon_unique(seq_view_bytes(plan.keys@), chosen, pred);
        lemma_keys_cover(num_delta@, pre_delta@, sub_delta@);
        lemma_rescan_num(db2, nums_by_filing_keys@, nums_by_filing_rows@);
        lemma_rescan_sub(db2, subs_by_filing_keys@, subs_by_filing_rows@);
        lemma_rescan_pre(db2, pres_by_filing_keys@, pres_by_filing_rows@);
        lemma_frame_units(db, db2, num_delta@, pre_delta@, sub_delta@, s.fm, plan.u);
        // the asked filings hold the delta filings
        assert forall|a: Seq<u8>| ka.contains(a) implies keys.contains(a) by { assert(pred(a)); }
        assert forall|i: int| 0 <= i < ds.len() implies keys.contains((#[trigger] ds[i]).0.adsh) by {
            assert(ds[i].0 == sub_rt_view(sub_delta@[i].row));
            assert(ka.contains(sub_delta@[i].row.adsh@));
        }
        // rows
        let (rn, rs, rp) = (num_rt_seq_view(nums_by_filing_rows@), sub_rt_seq_view(subs_by_filing_rows@), pre_rt_seq_view(pres_by_filing_rows@));
        lemma_rep_step(db, db2, dn);
    }
    proof {
        assert forall|i: int| 0 <= i < dn.len() implies is_rep((#[trigger] dn[i]).0) by {
            let r = num_delta_seq_view(num_delta@)[i].0;
            if r.value is Some { assert(f64_val(num_delta@[i].row.value) == r.value->0); }
        }
        lemma_rep_step(db, db2, dn);
    }
    let ls = wrap_sub(subs_by_filing_rows);
    let lp = wrap_pre(pres_by_filing_rows);
    let mut acc: Index<AccEnt> = Index::new();
    proof { assert(acc_ok(acc)); }
    let ghost acc0 = acc;
    join_scan(&s.tab, &mut acc, nums_by_filing_rows, &ls, &lp);
    proof {
        let (rn, rs, rp) = (num_rt_seq_view(nums_by_filing_rows@), sub_rt_seq_view(subs_by_filing_rows@), pre_rt_seq_view(pres_by_filing_rows@));
        assert(sub_delta_seq_view(ls@) == unit_l(rs) && pre_delta_seq_view(lp@) == unit_l(rp));
        assert forall|k: GK| u_has(plan.w, k.fy, k.name) implies #[trigger] acc_c(acc, k) == cntk(db2, k) && acc_s(acc, k) == sumk(db2, k) by {
            assert forall|s2: SubRow| db2.sub.contains(s2) && in_unit(s2, k.fy, k.name) implies keys.contains(s2.adsh) by {
                let q = choose|q: int| 0 <= q < db2.sub.len() && db2.sub[q] == s2;
                assert(sub_has(db2, s2.adsh, k.fy, k.name));
                lemma_unit_filings_asked(db, db2, num_delta@, pre_delta@, sub_delta@, s.cls, s.fm, s.um, keys, plan.w, k.fy, k.name, s2.adsh);
            }
            lemma_unit_abs(db2, keys, rn, rs, rp, k, icnt(k));
            lemma_unit_abs(db2, keys, rn, rs, rp, k, ival(k));
            assert(acc_c(acc, k) == acc_c(acc0, k) + nn_c(unit_l(rn), unit_l(rs), unit_l(rp), 1)(k));
            assert(acc_s(acc, k) == acc_s(acc0, k) + nn_s(unit_l(rn), unit_l(rs), unit_l(rp), 1)(k));
        }
    }
    let uc2 = uc_update(&s.uc, &plan.u, &acc);
    let tc = uc_classes(&uc2, &plan.w);
    let cls2 = core_update(&s.cls, &plan.u, &plan.ct, &plan.w, &plan.cw, &tc, &acc, &s.tab, Ghost(db), Ghost(db2));
    let fm2 = fm_update(&s.fm, &plan.keys, subs_by_filing_rows);
    let um2 = um_update(&s.um, &plan.u, subs_by_filing_rows);
    proof {
        assert(uc_ok(uc2, db2)) by {
            assert forall|k: GK| cntk(db2, k) > 0 implies ucs_has(uc2, ukey(k.fy, k.name), okey(k.fy, k.stmt, k.uom)) by {
                if u_has(plan.u, k.fy, k.name) {
                    assert(u_has(plan.w, k.fy, k.name));
                    assert(acc_c(acc, k) == cntk(db2, k));
                    let e = acc.at(fkey(k))->0;
                    assert(acc.at(fkey(k)) is Some);
                    assert(fkey(e.gk()) == fkey(k));
                    lemma_fkey_inj(e.gk(), k);
                } else {
                    assert(cntk(db, k) == cntk(db2, k));
                }
            }
        }
        let rs = sub_rt_seq_view(subs_by_filing_rows@);
        lemma_fm_ok2(db, db2, ds, keys, subs_by_filing_rows@, s.fm, fm2, plan.keys@);
        assert forall|fy: int, name: Option<Seq<u8>>, a: Seq<u8>| u_has(plan.u, fy, name) && sub_has(db2, a, fy, name) implies keys.contains(a) by {
            lemma_unit_filings_asked(db, db2, num_delta@, pre_delta@, sub_delta@, s.cls, s.fm, s.um, keys, plan.w, fy, name, a);
        }
        assert forall|fy: int, name: Option<Seq<u8>>, a: Seq<u8>| !u_has(plan.u, fy, name) && (sub_has(db, a, fy, name) || sub_has(db2, a, fy, name)) implies !ka.contains(a) by {
            lemma_not_u_not_asked(db, db2, num_delta@, pre_delta@, sub_delta@, s.fm, plan.u, fy, name, a);
        }
        lemma_um_ok2(db, db2, ds, ka, keys, subs_by_filing_rows@, s.um, um2, plan.u);
        assert forall|i: int| 0 <= i < ds.len() implies ka.contains((#[trigger] ds[i]).0.adsh) by { assert(ds[i].0 == sub_rt_view(sub_delta@[i].row)); }
    }
    Ok(State { cls: cls2, uc: uc2, fm: fm2, um: um2, tab: s.tab.clone(), db: Ghost(db2) })
}

#[verifier::rlimit(500)]
#[verifier::spinoff_prover]
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
    let ghost (subs_v, pres_v) = (subs@, pres@);
    let ghost db0 = empty_db();
    let cls0: Index<ClassRec> = Index::new();
    let fm0: Index<Vec<UnitRef>> = Index::new();
    let um0: Index<UnitRec> = Index::new();
    let u = units_of_subs(&subs);
    let no_keys: Vec<Vec<u8>> = Vec::new();
    let fm = fm_update(&fm0, &no_keys, &subs);
    let um = um_update(&um0, &u, &subs);
    let ct: Index<ClassRef> = Index::new();
    let cw: Index<ClassRef> = Index::new();
    let ls = wrap_sub_owned(subs);
    let lp = wrap_pre_owned(pres);
    let mut acc: Index<AccEnt> = Index::new();
    proof { assert(acc_ok(acc)); }
    let ghost acc0 = acc;
    join_scan(&tab, &mut acc, &nums, &ls, &lp);
    proof {
        lemma_rep_scan(db, nums@);
        assert(tbl_str(cls0));
        assert forall|k: GK| cntk(db0, k) > 0 implies covered(db0, cls0, k) by { lemma_empty_groups(k); }
        assert(cls_ok(db0, cls0));
        assert(plan_ok(cls0, u, ct, u, cw)) by {
            assert(subset_u(u, u));
            assert forall|ok: KeyView| ct.at(ok) is Some implies cw.at(ok) is Some by { assert(cls0.at(ok) is Some); }
        }
        assert forall|k: GK| !u_has(u, k.fy, k.name) implies cntk(db, k) == cntk(db0, k) && sumk(db, k) == sumk(db0, k) by {
            assert forall|s2: SubRow| db.sub.contains(s2) implies !in_unit(s2, k.fy, k.name) by {
                if in_unit(s2, k.fy, k.name) {
                    let q = choose|q: int| 0 <= q < db.sub.len() && db.sub[q] == s2;
                    assert(sub_has(db, s2.adsh, k.fy, k.name));
                    lemma_scan_sub_has(db, subs_v, s2.adsh, k.fy, k.name);
                    let i = choose|i: int| 0 <= i < subs_v.len() && rs_unit(subs_v, i, k.fy, k.name) && subs_v[i].adsh@ == s2.adsh;
                    assert(u_has(u, k.fy, k.name));
                }
            }
            lemma_absent_unit(db, k);
            lemma_empty_groups(k);
        }
        assert(frame_ok(db0, db, u));
        assert forall|k: GK| u_has(u, k.fy, k.name) implies #[trigger] acc_c(acc, k) == cntk(db, k) && acc_s(acc, k) == sumk(db, k) by {
            lemma_diag_k(k);
            lemma_build_identity(db, nums@, subs_v, pres_v, icnt(k));
            lemma_build_identity(db, nums@, subs_v, pres_v, ival(k));
            assert(acc_c(acc, k) == acc_c(acc0, k) + nn_c(unit_l(num_rt_seq_view(nums@)), sub_delta_seq_view(ls@), pre_delta_seq_view(lp@), 1)(k));
            assert(acc_s(acc, k) == acc_s(acc0, k) + nn_s(unit_l(num_rt_seq_view(nums@)), sub_delta_seq_view(ls@), pre_delta_seq_view(lp@), 1)(k));
        }
    }
    let uc0: Index<ClsSet> = Index::new();
    let uc = uc_update(&uc0, &u, &acc);
    let tc = uc_classes(&uc, &u);
    let cls = core_update(&cls0, &u, &ct, &u, &cw, &tc, &acc, &tab, Ghost(db0), Ghost(db));
    proof {
        assert(uc_ok(uc, db)) by {
            assert forall|k: GK| cntk(db, k) > 0 implies ucs_has(uc, ukey(k.fy, k.name), okey(k.fy, k.stmt, k.uom)) by {
                if u_has(u, k.fy, k.name) {
                    assert(acc_c(acc, k) == cntk(db, k));
                    let e = acc.at(fkey(k))->0;
                    assert(acc.at(fkey(k)) is Some);
                    assert(fkey(e.gk()) == fkey(k));
                    lemma_fkey_inj(e.gk(), k);
                } else {
                    assert(cntk(db, k) == cntk(db0, k));
                    lemma_empty_groups(k);
                }
            }
        }
        assert(fm_ok(fm0, db0) && um_ok(um0, db0)) by {
            assert forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| fm_has(fm0, a, fy, name) <==> sub_has(db0, a, fy, name) by {}
            assert forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| um_has(um0, fy, name, a) <==> sub_has(db0, a, fy, name) by {}
        }
        assert(fm_ok(fm, db)) by {
            assert forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| fm_has(fm, a, fy, name) <==> sub_has(db, a, fy, name) by {
                lemma_scan_sub_has(db, subs_v, a, fy, name);
            }
        }
        assert(um_ok(um, db)) by {
            assert forall|a: Seq<u8>, fy: int, name: Option<Seq<u8>>| um_has(um, fy, name, a) <==> sub_has(db, a, fy, name) by {
                lemma_scan_sub_has(db, subs_v, a, fy, name);
                if u_has(u, fy, name) {} else { assert(!sub_has(db, a, fy, name)) by { if sub_has(db, a, fy, name) { let i = choose|i: int| 0 <= i < subs_v.len() && rs_unit(subs_v, i, fy, name) && subs_v[i].adsh@ == a; assert(u_has(u, fy, name)); } } }
            }
        }
    }
    Some(State { cls, uc, fm, um, tab, db: Ghost(db) })
}

} // verus!
