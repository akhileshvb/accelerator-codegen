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
use verus_prooflib::wide::Wide;
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

/// How many signed rows a batch of records is built from at a time (bounds the records held at once).
pub const CHUNK: usize = 65536;

pub open spec fn pass_tot(tot0: Tot, nl: Seq<(NumRow, int)>, mult: int, st: Seq<SubRow>, tt: Seq<TagRow>, sides: Sides) -> Tot {
    |p: Params, h: spec_fn(JK) -> int| tot0(p, h) + mult * passv(nl, st, tt, sides, p, h)
}

/// The groups with a pass of signed num rows applied, a chunk of rows at a time.
pub fn run_pass(g0: Index<Index<Cell>>, nl: &Vec<(NumRt, i64)>, mult: i64, sub_bag: &Index<Vec<SubRt>>, tag_bag: &Index<Vec<TagRt>>,
    sides: &Vec<Side>, pows: &Vec<Wide>, Ghost(st): Ghost<Seq<SubRow>>, Ghost(tt): Ghost<Seq<TagRow>>, Ghost(tot0): Ghost<Tot>)
    -> (g: Index<Index<Cell>>)
    requires
        rep(g0, tot0),
        -1 <= mult <= 1,
        bag_holds::<SubRt>(*sub_bag, st),
        bag_holds::<TagRt>(*tag_bag, tt),
        sides_ok(sides@),
        pows_ok(pows@),
        nl_ok(nl@),
    ensures
        rep(g, pass_tot(tot0, nl_view(nl@), mult as int, st, tt, sides_spec(sides@))),
{
    let mut g = g0;
    let mut i: usize = 0;
    proof {
        assert(nl_view(nl@.take(0)) =~= Seq::<(NumRow, int)>::empty());
        lemma_rep_ext(g, tot0, pass_tot(tot0, nl_view(nl@.take(0)), mult as int, st, tt, sides_spec(sides@)));
    }
    while i < nl.len()
        invariant
            i <= nl.len(),
            -1 <= mult <= 1,
            bag_holds::<SubRt>(*sub_bag, st),
            bag_holds::<TagRt>(*tag_bag, tt),
            sides_ok(sides@),
            pows_ok(pows@),
            nl_ok(nl@),
            rep(g, pass_tot(tot0, nl_view(nl@.take(i as int)), mult as int, st, tt, sides_spec(sides@))),
        decreases nl.len() - i,
    {
        let end = if nl.len() - i > CHUNK { i + CHUNK } else { nl.len() };
        let mut recs: Vec<Rec> = Vec::new();
        emit_pass(&mut recs, nl, i, end, mult, sub_bag, tag_bag, sides, pows, Ghost(st), Ghost(tt));
        let ghost cur = pass_tot(tot0, nl_view(nl@.take(i as int)), mult as int, st, tt, sides_spec(sides@));
        let g1 = apply_recs(g, &recs, Ghost(cur));
        proof {
            assert(nl_view(nl@.take(end as int)) =~= nl_view(nl@.take(i as int)) + nl_view(nl@.subrange(i as int, end as int)));
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] tot_recs(cur, recs@)(p, h)
                == pass_tot(tot0, nl_view(nl@.take(end as int)), mult as int, st, tt, sides_spec(sides@))(p, h) by {
                let f = |x: (NumRow, int)| x.1 * big_psi(st, tt, sides_spec(sides@), p, h, x.0);
                lemma_sum_concat(nl_view(nl@.take(i as int)), nl_view(nl@.subrange(i as int, end as int)), f);
                assert(mult as int * (passv(nl_view(nl@.take(i as int)), st, tt, sides_spec(sides@), p, h)
                    + passv(nl_view(nl@.subrange(i as int, end as int)), st, tt, sides_spec(sides@), p, h))
                    == mult as int * passv(nl_view(nl@.take(i as int)), st, tt, sides_spec(sides@), p, h)
                        + mult as int * passv(nl_view(nl@.subrange(i as int, end as int)), st, tt, sides_spec(sides@), p, h)) by (nonlinear_arith);
            }
            lemma_rep_ext(g1, tot_recs(cur, recs@), pass_tot(tot0, nl_view(nl@.take(end as int)), mult as int, st, tt, sides_spec(sides@)));
        }
        g = g1;
        i = end;
    }
    proof { assert(nl@.take(nl.len() as int) =~= nl@); }
    g
}

pub open spec fn build_tot(rows: Seq<NumRow>, st: Seq<SubRow>, tt: Seq<TagRow>, sides: Sides) -> Tot {
    |p: Params, h: spec_fn(JK) -> int| sum(rows, |n: NumRow| big_psi(st, tt, sides, p, h, n))
}

/// The groups of a whole database: its num rows passed through the join a chunk at a time.
fn build_groups(nums: &Vec<NumRt>, sub_bag: &Index<Vec<SubRt>>, tag_bag: &Index<Vec<TagRt>>,
    sides: &Vec<Side>, pows: &Vec<Wide>, Ghost(st): Ghost<Seq<SubRow>>, Ghost(tt): Ghost<Seq<TagRow>>) -> (g: Index<Index<Cell>>)
    requires
        bag_holds::<SubRt>(*sub_bag, st),
        bag_holds::<TagRt>(*tag_bag, tt),
        sides_ok(sides@),
        pows_ok(pows@),
    ensures
        rep(g, build_tot(rows_v(nums@), st, tt, sides_spec(sides@))),
{
    let mut g: Index<Index<Cell>> = Index::new();
    let mut i: usize = 0;
    proof {
        assert(rows_v(nums@.take(0)) =~= Seq::<NumRow>::empty());
        assert forall|u: Seq<u8>, stm: Seq<u8>, sc: int, tl: Option<Seq<u8>>| #[trigger] cell_at(g, u, stm, sc, tl) is None implies
            zero_rep(tot_zero(), u, stm, sc, tl) by {}
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] build_tot(rows_v(nums@.take(0)), st, tt, sides_spec(sides@))(p, h) == tot_zero()(p, h) by {}
        lemma_rep_ext(g, tot_zero(), build_tot(rows_v(nums@.take(0)), st, tt, sides_spec(sides@)));
    }
    while i < nums.len()
        invariant
            i <= nums.len(),
            bag_holds::<SubRt>(*sub_bag, st),
            bag_holds::<TagRt>(*tag_bag, tt),
            sides_ok(sides@),
            pows_ok(pows@),
            rep(g, build_tot(rows_v(nums@.take(i as int)), st, tt, sides_spec(sides@))),
        decreases nums.len() - i,
    {
        let end = if nums.len() - i > CHUNK { i + CHUNK } else { nums.len() };
        let mut recs: Vec<Rec> = Vec::new();
        emit_rows(&mut recs, nums, i, end, sub_bag, tag_bag, sides, pows, Ghost(st), Ghost(tt));
        let ghost cur = build_tot(rows_v(nums@.take(i as int)), st, tt, sides_spec(sides@));
        let g1 = apply_recs(g, &recs, Ghost(cur));
        proof {
            assert(rows_v(nums@.take(end as int)) =~= rows_v(nums@.take(i as int)) + rows_v(nums@.subrange(i as int, end as int)));
            assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] tot_recs(cur, recs@)(p, h)
                == build_tot(rows_v(nums@.take(end as int)), st, tt, sides_spec(sides@))(p, h) by {
                lemma_sum_concat(rows_v(nums@.take(i as int)), rows_v(nums@.subrange(i as int, end as int)),
                    |n: NumRow| big_psi(st, tt, sides_spec(sides@), p, h, n));
            }
            lemma_rep_ext(g1, tot_recs(cur, recs@), build_tot(rows_v(nums@.take(end as int)), st, tt, sides_spec(sides@)));
        }
        g = g1;
        i = end;
    }
    proof { assert(nums@.take(nums.len() as int) =~= nums@); }
    g
}

/// The build's total is the tuples' total.
proof fn lemma_build_total(db: Db, nums: Seq<NumRt>)
    requires scan_nums_ok(db, nums),
    ensures
        forall|p: Params, h: spec_fn(JK) -> int| #[trigger] build_tot(rows_v(nums), db.sub, db.tag, new_sides(db.pre))(p, h) == tot_db(db)(p, h),
{
    assert(rows_v(nums) =~= num_rt_seq_view(nums));
    assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] build_tot(rows_v(nums), db.sub, db.tag, new_sides(db.pre))(p, h) == tot_db(db)(p, h) by {
        let f = |n: NumRow| big_psi(db.sub, db.tag, new_sides(db.pre), p, h, n);
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
    let mut sides: Vec<Side> = Vec::new();
    sides.push(Side { idx: pre_bag, b: 1, tab: Ghost(db.pre) });
    proof { assert(sides_spec(sides@) =~= new_sides(db.pre)); }
    let groups = build_groups(&nums, &sub_bag, &tag_bag, &sides, &consts.pows, Ghost(db.sub), Ghost(db.tag));
    proof {
        lemma_build_total(db, nums@);
        lemma_rep_ext(groups, build_tot(rows_v(nums@), db.sub, db.tag, sides_spec(sides@)), tot_db(db));
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
    let shared = s.groups.share();
    proof { lemma_rep_share(s.groups, shared, tot_db(db)); }
    let ghost nlv = nl_view(nl@);
    let ghost dnv = nl_view(dn@);
    let ghost ssn = sides_spec(sides_new@);
    let ghost sso = sides_spec(sides_old@);
    let g1 = run_pass(shared, &nl, 1, &subs2, &tags2, &sides_new, &s.consts.pows, Ghost(db2.sub), Ghost(db2.tag), Ghost(tot_db(db)));
    let ghost t1 = pass_tot(tot_db(db), nlv, 1, db2.sub, db2.tag, ssn);
    let g2 = run_pass(g1, &nl, -1, &s.subs, &s.tags, &sides_old, &s.consts.pows, Ghost(db.sub), Ghost(db.tag), Ghost(t1));
    let ghost t2 = pass_tot(t1, nlv, -1, db.sub, db.tag, sso);
    let groups = run_pass(g2, &dn, 1, &s.subs, &s.tags, &sides_old, &s.consts.pows, Ghost(db.sub), Ghost(db.tag), Ghost(t2));
    proof {
        lemma_wanted_covers(num_delta@, pre_delta@, sub_delta@, tag_delta@);
        lemma_nums_cover(db2, nums_by_adsh_keys@, nums_by_adsh_rows@, nums_by_tag_keys@, nums_by_tag_rows@, nums_all@);
        lemma_pres_cover(db2, pres_by_adsh_keys@, pres_by_adsh_rows@, pres_by_tag_keys@, pres_by_tag_rows@, pres_all@);
        lemma_nums_aff(db2, nums_by_adsh_keys@, nums_by_adsh_rows@, nums_by_tag_keys@, nums_by_tag_rows@, nums_all@);
        lemma_apply_total(db, db2, dview(dn@), dview(dp@), dview(ds@), dview(dt@), a, g, nlv, rtab);
        let t3 = pass_tot(t2, dnv, 1, db.sub, db.tag, sso);
        assert forall|p: Params, h: spec_fn(JK) -> int| #[trigger] t3(p, h) == tot_db(db2)(p, h) by {}
        lemma_rep_ext(groups, t3, tot_db(db2));
    }
    Ok(State { subs: subs2, tags: tags2, groups, consts: s.consts.clone(), db: Ghost(db2) })
}

}
