// The kernel: the aggregate state of the query.  Per (uom, stmt) an ordered index of the groups
// (sic, tlabel) with their exact count, sum, sum of magnitudes and companies (cells.rs), plus the
// sub rows by adsh and the tag rows by (tag, version) that writes join against.  A read walks the
// groups of the parameter set's sic range (answer.rs); a write passes the rows of the changed
// filings and tags through the join before and after (emit.rs) and applies the signed records to the
// groups (algebra.rs says why that is the new aggregate).
#![allow(unused_imports)]
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bag::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::algebra::*;
use crate::answer::*;
use crate::applylem::*;
use crate::bucket::*;
use crate::cells::*;
use crate::emit::*;
use crate::facts::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::join_spec::*;
use crate::jspec::*;
use crate::keyset::*;
use crate::lists::*;
use crate::numeric::*;
use crate::pairs::*;
use crate::rec::*;
use crate::rows::*;

verus! {

pub struct State {
    subs: Index<Vec<SubRt>>,
    tags: Index<Vec<TagRt>>,
    groups: Index<Index<Cell>>,
    consts: Arc<Consts>,
    db: Ghost<Db>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    #[verifier::type_invariant]
    closed spec fn holds(self) -> bool {
        &&& bag_holds::<SubRt>(self.subs, self.db@.sub)
        &&& bag_holds::<TagRt>(self.tags, self.db@.tag)
        &&& rep(self.groups, tot_db(self.db@))
        &&& consts_ok(*self.consts)
    }
}

pub open spec fn history(s: State) -> nat {
    0
}

pub open spec fn rescan_nums_by_adsh_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>, tag_delta: Seq<TagDelta>) -> Seq<Seq<u8>> {
    adsh_wanted(num_delta, pre_delta, sub_delta)
}

pub open spec fn rescan_pres_by_adsh_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>, tag_delta: Seq<TagDelta>) -> Seq<Seq<u8>> {
    adsh_wanted(num_delta, pre_delta, sub_delta)
}

pub open spec fn rescan_nums_by_tag_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>, tag_delta: Seq<TagDelta>) -> Seq<Seq<u8>> {
    tag_wanted(tag_delta)
}

pub open spec fn rescan_pres_by_tag_wanted(s: State, num_delta: Seq<NumDelta>, pre_delta: Seq<PreDelta>, sub_delta: Seq<SubDelta>, tag_delta: Seq<TagDelta>) -> Seq<Seq<u8>> {
    tag_wanted(tag_delta)
}

pub open spec fn tot_zero() -> Tot {
    |p: Params, h: spec_fn(JK) -> int| 0int
}

/// The total of the records of a pass over the whole database is the tuples' total.
proof fn lemma_build_total(db: Db, nums: Seq<NumRt>, recs: Seq<Rec>, nl: Seq<(NumRt, i64)>, sides: Seq<Side>)
    requires
        scan_nums_ok(db, nums),
        nl_view(nl) == ones(rows_v(nums)),
        sides_spec(sides) == new_sides(db.pre),
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] rsum(recs, p, h)
            == 0 + 1int * passv(nl_view(nl), db.sub, db.tag, sides_spec(sides), p, h),
    ensures
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] tot_recs(tot_zero(), recs)(p, h) == tot_db(db)(p, h),
{
    assert(rows_v(nums) =~= num_rt_seq_view(nums));
    assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] tot_recs(tot_zero(), recs)(p, h) == tot_db(db)(p, h) by {
        let f = |n: NumRow| big_psi(db.sub, db.tag, new_sides(db.pre), p, h, n);
        lemma_ones_wsum(rows_v(nums), f);
        assert(passv(nl_view(nl), db.sub, db.tag, sides_spec(sides), p, h) == wsum(ones(rows_v(nums)), f)) by {
            lemma_sum_ext_all(ones(rows_v(nums)), |x: (NumRow, int)| x.1 * big_psi(db.sub, db.tag, new_sides(db.pre), p, h, x.0),
                |x: (NumRow, int)| x.1 * f(x.0));
        }
        assert(sum(num_rt_seq_view(nums), f) == sum(db.num, f));
        assert forall|n: NumRow| #[trigger] f(n) == sub_sum4(db.sub, db.tag, db.pre, p, h, n) by {
            lemma_big_psi_single(db.sub, db.tag, db.pre, p, h, n);
        }
        lemma_sum_ext_all(db.num, f, |n: NumRow| sub_sum4(db.sub, db.tag, db.pre, p, h, n));
        lemma_tuple_jsum(db, p, h);
    }
}

pub fn build(
    sub: Vec<SubRt>,
    tag: Vec<TagRt>,
    nums: Vec<NumRt>,
    pres: Vec<PreRt>,
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_sub_ok(db, sub@),
        scan_tag_ok(db, tag@),
        scan_nums_ok(db, nums@),
        scan_pres_ok(db, pres@),
    ensures
        r matches Some(s) ==> s.db() == db,
        r matches Some(s) ==> crate::kernel::history(s) == 0,
        envelope(db) ==> r is Some,
{
    proof {
        assert(rows_v(nums@) =~= num_rt_seq_view(nums@));
        assert(rows_v(sub@) =~= sub_rt_seq_view(sub@));
        assert(rows_v(tag@) =~= tag_rt_seq_view(tag@));
        assert(rows_v(pres@) =~= pre_rt_seq_view(pres@));
    }
    let sub_bag = bag_build(&sub);
    let tag_bag = bag_build(&tag);
    let pre_bag = bag_build(&pres);
    proof {
        lemma_bag_holds_transfer::<SubRt>(sub_bag, rows_v(sub@), db.sub);
        lemma_bag_holds_transfer::<TagRt>(tag_bag, rows_v(tag@), db.tag);
        lemma_bag_holds_transfer::<PreRt>(pre_bag, rows_v(pres@), db.pre);
    }
    let consts = Arc::new(consts_new());
    let nl = with_ones(&nums);
    let mut sides: Vec<Side> = Vec::new();
    sides.push(Side { idx: pre_bag, b: 1, tab: Ghost(db.pre) });
    let mut recs: Vec<Rec> = Vec::new();
    emit_pass(&mut recs, &nl, 1, &sub_bag, &tag_bag, &sides, &consts.pows, Ghost(db.sub), Ghost(db.tag));
    proof {
        assert(sides_spec(sides@) =~= new_sides(db.pre));
        lemma_build_total(db, nums@, recs@, nl@, sides@);
    }
    let g0: Index<Index<Cell>> = Index::new();
    proof {
        assert forall|u: Seq<u8>, st: Seq<u8>, sc: int, tl: Option<Seq<u8>>| #[trigger] cell_at(g0, u, st, sc, tl) is None implies
            zero_rep(tot_zero(), u, st, sc, tl) by {}
    }
    let groups = apply_recs(g0, &recs, Ghost(tot_zero()));
    proof {
        lemma_rep_ext(groups, tot_recs(tot_zero(), recs@), tot_db(db));
    }
    Some(State { subs: sub_bag, tags: tag_bag, groups, consts, db: Ghost(db) })
}

pub fn execute(
    s: &State,
    p1: i64,
    p2: i64,
    p3: &[u8],
    p4: &[u8],
) -> (r: Option<Vec<(Option<i64>, Option<Vec<u8>>, Option<Vec<u8>>, i64, Option<u64>, Option<u64>)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1 as int, p2 as int, p3@, p4@), out_view(out@)),
        envelope(s.db()) && answer_fits(s.db(), params(p1 as int, p2 as int, p3@, p4@)) ==> r is Some,
        r matches Some(out) ==> forall|i: int| 0 <= i < out.len() ==> out_fits(#[trigger] out_view(out@)[i]),
{
    proof { use_type_invariant(s); }
    answer(&s.groups, p1, p2, p3, p4, &*s.consts, Ghost(s.db@))
}

pub fn rescan_nums_by_adsh_keys(
    s: &State,
    num_delta: &Vec<NumDelta>,
    pre_delta: &Vec<PreDelta>,
    sub_delta: &Vec<SubDelta>,
    tag_delta: &Vec<TagDelta>,
) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == crate::kernel::rescan_nums_by_adsh_wanted(*s, num_delta@, pre_delta@, sub_delta@, tag_delta@),
{
    adsh_keys(num_delta, pre_delta, sub_delta)
}

pub fn rescan_pres_by_adsh_keys(
    s: &State,
    num_delta: &Vec<NumDelta>,
    pre_delta: &Vec<PreDelta>,
    sub_delta: &Vec<SubDelta>,
    tag_delta: &Vec<TagDelta>,
) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == crate::kernel::rescan_pres_by_adsh_wanted(*s, num_delta@, pre_delta@, sub_delta@, tag_delta@),
{
    adsh_keys(num_delta, pre_delta, sub_delta)
}

pub fn rescan_nums_by_tag_keys(
    s: &State,
    num_delta: &Vec<NumDelta>,
    pre_delta: &Vec<PreDelta>,
    sub_delta: &Vec<SubDelta>,
    tag_delta: &Vec<TagDelta>,
) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == crate::kernel::rescan_nums_by_tag_wanted(*s, num_delta@, pre_delta@, sub_delta@, tag_delta@),
{
    tag_keys(tag_delta)
}

pub fn rescan_pres_by_tag_keys(
    s: &State,
    num_delta: &Vec<NumDelta>,
    pre_delta: &Vec<PreDelta>,
    sub_delta: &Vec<SubDelta>,
    tag_delta: &Vec<TagDelta>,
) -> (r: Vec<Vec<u8>>)
    ensures seq_view_bytes(r@) == crate::kernel::rescan_pres_by_tag_wanted(*s, num_delta@, pre_delta@, sub_delta@, tag_delta@),
{
    tag_keys(tag_delta)
}


pub fn apply(
    s: &State,
    num_delta: &Vec<NumDelta>,
    pre_delta: &Vec<PreDelta>,
    sub_delta: &Vec<SubDelta>,
    tag_delta: &Vec<TagDelta>,
    nums_by_adsh_keys: &Vec<Vec<u8>>,
    nums_by_adsh_rows: &Vec<NumRt>,
    pres_by_adsh_keys: &Vec<Vec<u8>>,
    pres_by_adsh_rows: &Vec<PreRt>,
    nums_by_tag_keys: &Vec<Vec<u8>>,
    nums_by_tag_rows: &Vec<NumRt>,
    pres_by_tag_keys: &Vec<Vec<u8>>,
    pres_by_tag_rows: &Vec<PreRt>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, num_delta_seq_view(num_delta@), pre_delta_seq_view(pre_delta@), sub_delta_seq_view(sub_delta@), tag_delta_seq_view(tag_delta@)),
        seq_view_bytes(nums_by_adsh_keys@) == crate::kernel::rescan_nums_by_adsh_wanted(*s, num_delta@, pre_delta@, sub_delta@, tag_delta@),
        rescan_nums_by_adsh_ok(db2, nums_by_adsh_keys@, nums_by_adsh_rows@),
        seq_view_bytes(pres_by_adsh_keys@) == crate::kernel::rescan_pres_by_adsh_wanted(*s, num_delta@, pre_delta@, sub_delta@, tag_delta@),
        rescan_pres_by_adsh_ok(db2, pres_by_adsh_keys@, pres_by_adsh_rows@),
        seq_view_bytes(nums_by_tag_keys@) == crate::kernel::rescan_nums_by_tag_wanted(*s, num_delta@, pre_delta@, sub_delta@, tag_delta@),
        rescan_nums_by_tag_ok(db2, nums_by_tag_keys@, nums_by_tag_rows@),
        seq_view_bytes(pres_by_tag_keys@) == crate::kernel::rescan_pres_by_tag_wanted(*s, num_delta@, pre_delta@, sub_delta@, tag_delta@),
        rescan_pres_by_tag_ok(db2, pres_by_tag_keys@, pres_by_tag_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
        r matches Ok(s2) ==> crate::kernel::history(s2) <= crate::kernel::history(*s) + num_delta@.len() + pre_delta@.len() + sub_delta@.len() + tag_delta@.len(),
        envelope(s.db()) && envelope(db2) && crate::kernel::history(*s) + num_delta@.len() + pre_delta@.len() + sub_delta@.len() + tag_delta@.len() <= max_history() ==> r is Ok,
{
    proof { use_type_invariant(s); }
    let ghost db = s.db@;
    let dn = pairs_num(num_delta);
    let dp = pairs_pre(pre_delta);
    let ds = pairs_sub(sub_delta);
    let dt = pairs_tag(tag_delta);
    // the new join sides
    let subs2 = bag_apply(&s.subs, &ds, Ghost(db.sub), Ghost(db2.sub));
    let tags2 = bag_apply(&s.tags, &dt, Ghost(db.tag), Ghost(db2.tag));
    // the rows of the changed filings and tags, each once
    let aset = key_set(nums_by_adsh_keys);
    let ghost a = seq_view_bytes(nums_by_adsh_keys@);
    let ghost g = seq_view_bytes(nums_by_tag_keys@);
    let nums_all = merge_rows(nums_by_adsh_rows, nums_by_tag_rows, &aset, Ghost(a));
    let pres_all = merge_rows(pres_by_adsh_rows, pres_by_tag_rows, &aset, Ghost(a));
    let nl = with_ones(&nums_all);
    let (plus, minus) = split_signs(&dp);
    let idx_all = bag_build(&pres_all);
    let idx_plus = bag_build(&plus);
    let idx_minus = bag_build(&minus);
    let ghost rtab = rows_v(pres_all@);
    let mut sides_new: Vec<Side> = Vec::new();
    let idx_shared = idx_all.share();
    proof {
        assert forall|k: KeyView| rows_at::<PreRt>(idx_shared, k) == rows_at::<PreRt>(idx_all, k) by {
            assert(idx_shared.at(k) == idx_all.at(k));
        }
        assert(bag_holds::<PreRt>(idx_shared, rtab));
    }
    sides_new.push(Side { idx: idx_shared, b: 1, tab: Ghost(rtab) });
    let mut sides_old: Vec<Side> = Vec::new();
    sides_old.push(Side { idx: idx_all, b: 1, tab: Ghost(rtab) });
    sides_old.push(Side { idx: idx_plus, b: -1, tab: Ghost(plus_rows(dview(dp@))) });
    sides_old.push(Side { idx: idx_minus, b: 1, tab: Ghost(minus_rows(dview(dp@))) });
    proof {
        assert(rows_v(pres_by_adsh_rows@) =~= pre_rt_seq_view(pres_by_adsh_rows@));
        assert(rows_v(pres_by_tag_rows@) =~= pre_rt_seq_view(pres_by_tag_rows@));
        assert(rows_v(plus@) == plus_rows(dview(dp@)));
        assert(sides_spec(sides_new@) =~= new_sides(rtab));
        assert(sides_spec(sides_old@) =~= old_sides(rtab, dview(dp@)));
        assert(sides_ok(sides_new@)) by {
            assert forall|i: int| 0 <= i < sides_new.len() implies bag_holds::<PreRt>((#[trigger] sides_new@[i]).idx, sides_new@[i].tab@) && (sides_new@[i].b == 1 || sides_new@[i].b == -1) by {}
        }
        assert(sides_ok(sides_old@)) by {
            assert forall|i: int| 0 <= i < sides_old.len() implies bag_holds::<PreRt>((#[trigger] sides_old@[i]).idx, sides_old@[i].tab@) && (sides_old@[i].b == 1 || sides_old@[i].b == -1) by {}
        }
    }
    proof {
        assert(num_delta_seq_view(num_delta@) == dview(dn@));
        assert(nl_ok(dn@)) by {
            assert forall|i: int| 0 <= i < dn.len() implies (#[trigger] dn@[i]).1 == 1 || dn@[i].1 == -1 by {
                assert(num_delta_seq_view(num_delta@)[i].1 == 1 || num_delta_seq_view(num_delta@)[i].1 == -1);
                assert(dview(dn@)[i].1 == dn@[i].1 as int);
            }
        }
    }
    let mut recs: Vec<Rec> = Vec::new();
    emit_pass(&mut recs, &nl, 1, &subs2, &tags2, &sides_new, &s.consts.pows, Ghost(db2.sub), Ghost(db2.tag));
    emit_pass(&mut recs, &nl, -1, &s.subs, &s.tags, &sides_old, &s.consts.pows, Ghost(db.sub), Ghost(db.tag));
    emit_pass(&mut recs, &dn, 1, &s.subs, &s.tags, &sides_old, &s.consts.pows, Ghost(db.sub), Ghost(db.tag));
    proof {
        lemma_wanted_covers(num_delta@, pre_delta@, sub_delta@, tag_delta@);
        lemma_nums_cover(db2, nums_by_adsh_keys@, nums_by_adsh_rows@, nums_by_tag_keys@, nums_by_tag_rows@, nums_all@);
        lemma_pres_cover(db2, pres_by_adsh_keys@, pres_by_adsh_rows@, pres_by_tag_keys@, pres_by_tag_rows@, pres_all@);
        lemma_nums_aff(db2, nums_by_adsh_keys@, nums_by_adsh_rows@, nums_by_tag_keys@, nums_by_tag_rows@, nums_all@);
        assert(num_delta_seq_view(num_delta@) == dview(dn@));
        lemma_apply_total(db, db2, dview(dn@), dview(dp@), dview(ds@), dview(dt@), a, g, nl_view(nl@), rtab, recs@);
    }
    let shared = s.groups.share();
    proof { lemma_rep_share(s.groups, shared, tot_db(db)); }
    let groups = apply_recs(shared, &recs, Ghost(tot_db(db)));
    proof { lemma_rep_ext(groups, tot_recs(tot_db(db), recs@), tot_db(db2)); }
    Ok(State { subs: subs2, tags: tags2, groups, consts: s.consts.clone(), db: Ghost(db2) })
}

}
