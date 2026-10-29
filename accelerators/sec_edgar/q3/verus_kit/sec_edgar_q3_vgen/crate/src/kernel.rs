// The kernel: per parameter set (fy, uom), the companies' exact totals with the sum of their rounded totals,
// and the (name, company) groups' exact totals with their rounded totals; the sub table bucketed by filing
// with multiplicities, to join the changed num rows.  A commit is applied per changed row (batch.rs);
// a read walks one class (reply.rs).
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::batch::*;
use crate::bucket::*;
use crate::dmap::*;
use crate::ent::*;
use crate::exact::*;
use crate::fin::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::join_spec::*;
use crate::keys::*;
use crate::merge::*;
use crate::numeric::*;
use crate::qty::*;
use crate::reply::*;
use crate::rows::*;
use crate::stagea::*;
use crate::wbag::*;

verus! {

pub struct State {
    classes: Index<Class>,
    subs: Index<Vec<WRow>>,
    consts: Arc<Consts>,
    db: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    closed spec fn holds(self) -> bool {
        &&& agg_ok(self.classes, self.db@)
        &&& wbag_as(self.subs, phi_tab(self.db@.sub))
        &&& nums_dbl(self.db@)
        &&& consts_ok(*self.consts)
    }
}

pub open spec fn history(s: State) -> nat {
    0
}

/// The filings of the changed sub rows: their num rows meet the changed sub rows.
pub open spec fn rescan_nums_by_adsh_wanted(s: State, num_delta: Seq<NumDelta>, sub_delta: Seq<SubDelta>) -> Seq<Seq<u8>> {
    sub_delta.map_values(|d: SubDelta| d.row.adsh@)
}

proof fn lemma_empty_num_zero(db: Db)
    requires db.num.len() == 0,
    ensures forall|p: Params, h: spec_fn(JK) -> int| #[trigger] jq(db, p, h) == 0,
{
    assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] jq(db, p, h) == 0 by {
        assert(db.num =~= Seq::<NumRow>::empty());
        lemma_sum_empty(db.num, |n: NumRow| sum(db.sub, |s: SubRow| wt(p, h)(n, s)));
    }
}

fn dup_subs(sub: &Vec<SubRt>) -> (sd: Vec<SubDelta>)
    ensures
        sd.len() == sub.len(),
        forall|i: int| 0 <= i < sd.len() ==> (#[trigger] sd[i]).sign == 1 && sub_rt_view(sd[i].row) == sub_rt_view(sub[i]),
{
    let mut sd: Vec<SubDelta> = Vec::new();
    let mut i: usize = 0;
    while i < sub.len()
        invariant
            i <= sub.len(),
            sd.len() == i,
            forall|j: int| 0 <= j < i ==> (#[trigger] sd[j]).sign == 1 && sub_rt_view(sd[j].row) == sub_rt_view(sub[j]),
        decreases sub.len() - i,
    {
        sd.push(SubDelta { row: sub[i].dup(), sign: 1 });
        i += 1;
    }
    sd
}

/// A build is the commit of every num row onto the sub table alone (the num table starts empty).
pub fn build(
    sub: Vec<SubRt>,
    nums: Vec<NumRt>,
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_sub_ok(db, sub@),
        scan_nums_ok(db, nums@),
    ensures
        r matches Some(s) ==> s.db() == db,
        r matches Some(s) ==> crate::kernel::history(s) == 0,
        envelope(db) ==> r is Some,
{
    let consts = Arc::new(consts_new());
    let ghost db1 = Db { num: Seq::empty(), sub: db.sub };
    let sd = dup_subs(&sub);
    let mut subs = wbag_new();
    proof {
        let sv = sub_delta_seq_view(sd@);
        assert forall|f: spec_fn(SubRow) -> int| #[trigger] sum(db.sub, f) == sum(Seq::<SubRow>::empty(), f) + sum(sv, |e: (SubRow, int)| e.1 * f(e.0)) by {
            lemma_sum_empty(Seq::<SubRow>::empty(), f);
            assert(sv =~= sub_rt_seq_view(sub@).map_values(|r: SubRow| (r, 1int))) by {
                assert forall|i: int| 0 <= i < sv.len() implies sv[i] == (sub_rt_seq_view(sub@)[i], 1int) by {
                    assert(sv[i] == (sub_rt_view(sd@[i].row), sd@[i].sign as int));
                }
            }
            lemma_sum_map(sub_rt_seq_view(sub@), |r: SubRow| (r, 1int), |e: (SubRow, int)| e.1 * f(e.0), f);
            assert(sum(sub_rt_seq_view(sub@), f) == sum(db.sub, f));
        }
        assert forall|i: int| 0 <= i < sd.len() implies (#[trigger] sd[i]).sign == 1 || sd[i].sign == -1 by {}
    }
    sub_bag_apply(&mut subs, &sd, Ghost(Seq::empty()));
    proof {
        let sv = sub_delta_seq_view(sd@);
        assert forall|g: spec_fn(SubRow) -> int| #[trigger] phi_acc(Seq::empty(), sv)(g) == phi_tab(db.sub)(g) by {
            assert(sum(db.sub, g) == sum(Seq::<SubRow>::empty(), g) + sum(sv, |e: (SubRow, int)| e.1 * g(e.0)));
        }
        lemma_wbag_ext(subs, phi_acc(Seq::empty(), sv), phi_tab(db.sub));
    }
    let mut bt = batch_new();
    let Ghost(l) = stage_n_rows(&mut bt, &nums, &subs, &consts.pows, Ghost(db.sub));
    let dm = batch_finish(bt);
    let mut classes: Index<Class> = Index::new();
    proof {
        // the nums are doubles
        let pred = |r: NumRow| r.value is Some ==> dbl_raw(r.value->0);
        assert forall|i: int| 0 <= i < num_rt_seq_view(nums@).len() implies pred(#[trigger] num_rt_seq_view(nums@)[i]) by {
            let r = nums@[i];
            assert(num_rt_seq_view(nums@)[i] == num_rt_view(r));
            if r.value__null == 0 { assert(f64_val(r.value) == f64_val(r.value)); }
        }
        lemma_weighted_equal_preserves_predicate(num_rt_seq_view(nums@), db.num, pred);
        assert(nums_dbl(db));
        assert(nums_dbl(db1));
        // the empty state answers for db1
        lemma_empty_num_zero(db1);
        assert(agg_ok(classes, db1)) by {
            assert forall|fy: i64, uom: Seq<u8>| #[trigger] classes.at(ckey(fy as int, uom)) is None implies zero_p(db1, cp(fy, uom)) by {
                assert forall|gn: Option<Seq<u8>>, gc: Option<int>| qj(db1, cp(fy, uom), true, gn, gc).n == 0 && qj(db1, cp(fy, uom), false, gn, gc).n == 0 by {}
            }
        }
        // the num rows as changes
        let nv = nums_unit(nums@);
        assert forall|f: spec_fn(NumRow) -> int| #[trigger] sum(db.num, f) == sum(db1.num, f) + sum(nv, |e: (NumRow, int)| e.1 * f(e.0)) by {
            lemma_sum_empty(db1.num, f);
            assert(nv =~= num_rt_seq_view(nums@).map_values(|r: NumRow| (r, 1int)));
            lemma_sum_map(num_rt_seq_view(nums@), |r: NumRow| (r, 1int), |e: (NumRow, int)| e.1 * f(e.0), f);
            assert(sum(num_rt_seq_view(nums@), f) == sum(db.num, f));
        }
        assert(bag_step(db1.num, db.num, nv));
        let nosubs = Seq::<(SubRow, int)>::empty();
        assert forall|f: spec_fn(SubRow) -> int| #[trigger] sum(db.sub, f) == sum(db1.sub, f) + sum(nosubs, |e: (SubRow, int)| e.1 * f(e.0)) by {
            lemma_sum_empty(nosubs, |e: (SubRow, int)| e.1 * f(e.0));
        }
        assert(bag_step(db1.sub, db.sub, nosubs));
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] lsum(l, p, h)
            == left_join_delta(nv, db1.sub, wt(p, h)) + right_join_delta(db.num, nosubs, wt(p, h)) by {
            lemma_sum_empty(nosubs, |e: (SubRow, int)| e.1 * sum(db.num, |o: NumRow| wt(p, h)(o, e.0)));
        }
        lemma_ledger_from_deltas(l, db1, db, nv, nosubs);
    }
    merge_all(&mut classes, &dm, &*consts, Ghost(db1), Ghost(db), Ghost(l));
    Some(State { classes, subs, consts, db: Ghost(db) })
}

pub fn execute(
    s: &State,
    p1: i64,
    p2: &[u8],
) -> (r: Option<Vec<(Option<Vec<u8>>, Option<i64>, Option<u64>)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1 as int, p2@), out_view(out@)),
        envelope(s.db()) && answer_fits(s.db(), params(p1 as int, p2@)) ==> r is Some,
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
{
    proof { use_type_invariant(s); }
    let ghost db = s.db@;
    let ghost p = params(p1 as int, p2@);
    let key = class_key(p1, p2);
    let found = s.classes.get(&key);
    let cls = match found {
        Some(a) => {
            proof { assert(cls_ok(*a, db, cp(p1, p2@))); }
            a
        },
        None => {
            let cm: Index<Ent> = Index::new();
            let om: Index<Ent> = Index::new();
            proof {
                assert(zero_p(db, cp(p1, p2@)));
                lemma_empty_tmap(true, cm, tf_of(db, cp(p1, p2@), true));
                lemma_empty_tmap(false, om, tf_of(db, cp(p1, p2@), false));
                assert(cm.entries() =~= Seq::<(KeyView, Ent)>::empty());
                lemma_sum_empty(cm.entries(), cvw());
                lemma_sum_empty(cm.entries(), onew());
            }
            let c = Arc::new(Class { cmap: cm, omap: om, sr: Wide::zero(), k: Wide::zero() });
            proof { assert(cls_ok(*c, db, cp(p1, p2@))); }
            c
        },
    };
    proof { assert(cp(p1, p2@) == p); }
    finish(&*cls, &*s.consts, Ghost(db), Ghost(p))
}

pub fn rescan_nums_by_adsh_keys(
    s: &State,
    num_delta: &Vec<NumDelta>,
    sub_delta: &Vec<SubDelta>,
) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == crate::kernel::rescan_nums_by_adsh_wanted(*s, num_delta@, sub_delta@),
{
    let mut r: Vec<Vec<u8>> = Vec::new();
    let mut i: usize = 0;
    while i < sub_delta.len()
        invariant
            i <= sub_delta.len(),
            seq_view_bytes(r@) == sub_delta@.take(i as int).map_values(|d: SubDelta| d.row.adsh@),
        decreases sub_delta.len() - i,
    {
        let ghost old_r = r@;
        r.push(copy_bytes(sub_delta[i].row.adsh.as_slice()));
        proof {
            assert(sub_delta@.take(i as int + 1) =~= sub_delta@.take(i as int).push(sub_delta@[i as int]));
            assert(seq_view_bytes(r@) =~= seq_view_bytes(old_r).push(sub_delta@[i as int].row.adsh@));
            assert(sub_delta@.take(i as int + 1).map_values(|d: SubDelta| d.row.adsh@)
                =~= sub_delta@.take(i as int).map_values(|d: SubDelta| d.row.adsh@).push(sub_delta@[i as int].row.adsh@));
        }
        i += 1;
    }
    proof { assert(sub_delta@.take(sub_delta.len() as int) =~= sub_delta@); }
    r
}

pub fn apply(
    s: &State,
    num_delta: &Vec<NumDelta>,
    sub_delta: &Vec<SubDelta>,
    nums_by_adsh_keys: &Vec<Vec<u8>>,
    nums_by_adsh_rows: &Vec<NumRt>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, num_delta_seq_view(num_delta@), sub_delta_seq_view(sub_delta@)),
        seq_view_bytes(nums_by_adsh_keys@) == crate::kernel::rescan_nums_by_adsh_wanted(*s, num_delta@, sub_delta@),
        rescan_nums_by_adsh_ok(db2, nums_by_adsh_keys@, nums_by_adsh_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        r matches Ok(s2) ==> crate::kernel::history(s2) <= crate::kernel::history(*s) + num_delta@.len() + sub_delta@.len(),
        envelope(s.db()) && envelope(db2) && crate::kernel::history(*s) + num_delta@.len() + sub_delta@.len() <= max_history() ==> r is Ok,
{
    proof { use_type_invariant(s); }
    let ghost db = s.db@;
    let nb = bag_build(nums_by_adsh_rows);
    proof {
        let pred = |r: NumRow| r.value is Some ==> dbl_raw(r.value->0);
        let nv = num_delta_seq_view(num_delta@);
        assert forall|i: int| 0 <= i < nv.len() implies pred(#[trigger] nv[i].0) by {
            let r = num_delta@[i].row;
            assert(nv[i].0 == num_rt_view(r));
            if r.value__null == 0 { assert(f64_val(r.value) == f64_val(r.value)); }
        }
        lemma_bag_step_preserves_predicate(db.num, db2.num, nv, pred);
        assert(nums_dbl(db2));
        assert forall|i: int| 0 <= i < sub_delta.len() implies (#[trigger] sub_delta[i]).sign == 1 || sub_delta[i].sign == -1 by {
            assert(sub_delta_seq_view(sub_delta@)[i].1 == sub_delta[i].sign as int);
        }
        assert forall|i: int| 0 <= i < num_delta.len() implies (#[trigger] num_delta[i]).sign == 1 || num_delta[i].sign == -1 by {
            assert(nv[i].1 == num_delta[i].sign as int);
        }
        assert forall|i: int| 0 <= i < sub_delta@.len() implies seq_view_bytes(nums_by_adsh_keys@).contains((#[trigger] sub_delta@[i]).row.adsh@) by {
            assert(seq_view_bytes(nums_by_adsh_keys@)[i] == sub_delta@[i].row.adsh@);
        }
        lemma_nb_covers(nb, nums_by_adsh_rows@, db2.num, sub_delta@, seq_view_bytes(nums_by_adsh_keys@));
    }
    let (classes, subs) = apply_core(&s.classes, &s.subs, &*s.consts, num_delta, sub_delta, &nb, Ghost(db), Ghost(db2));
    Ok(State { classes, subs, consts: s.consts.clone(), db: Ghost(db2) })
}

}
