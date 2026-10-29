// What the stored groups say about the query: a group's count, sum, sum of magnitudes and number of
// companies are the spec's aggregates over the tuples of the parameter set.
use vstd::prelude::*;
use verus_common::order::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::cells::*;
use crate::gen_spec::*;
use crate::join_spec::*;
use crate::jspec::*;
use crate::rec::*;
use verus_prooflib::lemma_sum_ones;
use verus_prooflib::ordered::{Index, KeyView};

verus! {

/// The total of the tuples: the sum of any group function over a parameter set's tuples.
pub open spec fn tot_db(db: Db) -> Tot {
    |p: Params, h: spec_fn(JK) -> int| sum(tuples(db, p), |tp: Tuple| h(pj(tp)))
}

pub open spec fn tuple_one() -> spec_fn(Tuple) -> int {
    |t: Tuple| if t.n.value is Some { 1int } else { 0int }
}

pub open spec fn tuple_value() -> spec_fn(Tuple) -> int {
    |t: Tuple| if t.n.value is Some { t.n.value->0 } else { 0int }
}

pub open spec fn tuple_magnitude() -> spec_fn(Tuple) -> int {
    |t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0int }
}

/// A group function over the tuples is the total of the group function over their rows, for the groups of a sic.
pub proof fn lemma_group_tot(db: Db, p: Params, sigma: int, tl: Option<Seq<u8>>, f: spec_fn(JK) -> int, ft: spec_fn(Tuple) -> int)
    requires
        p.p1 <= sigma <= p.p2,
        forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| wc(p, n, s, t, r) ==> #[trigger] ft(tup(n, s, t, r)) == f(pj(tup(n, s, t, r))),
    ensures
        group_sum(db, p, Some(sigma), tl, Some(p.p3), ft) == tot_db(db)(gp(sigma, p.p3, p.p4), hf(sigma, tl, f)),
{
    let q = gp(sigma, p.p3, p.p4);
    let g1 = |t: Tuple| if t.s_.sic == Some(sigma) && t.t_.tlabel == tl && t.p.stmt == Some(p.p3) { ft(t) } else { 0 };
    let g2 = |tp: Tuple| hf(sigma, tl, f)(pj(tp));
    assert forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| #[trigger] psi_t(p, g1, n, s, t, r) == psi_t(q, g2, n, s, t, r) by {
        if wc(q, n, s, t, r) {
            assert(wc(p, n, s, t, r));
            assert(ft(tup(n, s, t, r)) == f(pj(tup(n, s, t, r))));
        }
    }
    lemma_tuple_sum_ext(db, p, g1, q, g2);
}

/// A tuple not in the groups' sics or statement is in no group.
pub proof fn lemma_group_outside(db: Db, p: Params, gs: Option<int>, gt: Option<Seq<u8>>, gst: Option<Seq<u8>>, f: spec_fn(Tuple) -> int)
    requires
        !(gs is Some && p.p1 <= gs->0 <= p.p2 && gst == Some(p.p3)),
    ensures
        group_sum(db, p, gs, gt, gst, f) == 0,
{
    let g1 = |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { f(t) } else { 0 };
    let zero = |t: Tuple| 0int;
    assert forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| #[trigger] psi_t(p, g1, n, s, t, r) == psi_t(p, zero, n, s, t, r) by {
        if wc(p, n, s, t, r) {
            assert(g1(tup(n, s, t, r)) == 0);
        }
    }
    lemma_tuple_sum_ext(db, p, g1, p, zero);
    lemma_tuple_gsum(db, p, zero);
    assert forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| #[trigger] psi_t(p, zero, n, s, t, r) == 0 by {}
    lemma_jsum_zero(db, p);
}

proof fn lemma_jsum_zero(db: Db, p: Params)
    ensures jsum_t(db.num, db.sub, db.tag, db.pre, p, |t: Tuple| 0int) == 0,
{
    let zero = |t: Tuple| 0int;
    assert forall|n: NumRow, s: SubRow, t: TagRow| #[trigger] pre_sum_t(db.pre, p, zero, n, s, t) == 0 by {
        assert forall|r: PreRow| #[trigger] psi_t(p, zero, n, s, t, r) == 0 by {}
        lemma_sum_zero(db.pre, |r: PreRow| psi_t(p, zero, n, s, t, r));
    }
    assert forall|n: NumRow, s: SubRow| #[trigger] tag_sum_t(db.tag, db.pre, p, zero, n, s) == 0 by {
        lemma_sum_zero(db.tag, |t: TagRow| pre_sum_t(db.pre, p, zero, n, s, t));
    }
    assert forall|n: NumRow| #[trigger] sub_sum_t(db.sub, db.tag, db.pre, p, zero, n) == 0 by {
        lemma_sum_zero(db.sub, |s: SubRow| tag_sum_t(db.tag, db.pre, p, zero, n, s));
    }
    lemma_sum_zero(db.num, |n: NumRow| sub_sum_t(db.sub, db.tag, db.pre, p, zero, n));
}


pub open spec fn comp_seq(db: Db, p: Params, sigma: int, tl: Option<Seq<u8>>) -> Seq<int> {
    group_vals_where(db, p, Some(sigma), tl, Some(p.p3), |t: Tuple| t.s_.cik is Some, |t: Tuple| t.s_.cik->0)
}

proof fn lemma_map_where_len<A, T>(s: Seq<A>, keep: spec_fn(A) -> bool, f: spec_fn(A) -> T)
    ensures map_where(s, keep, f).len() == sum(s, |x: A| if keep(x) { 1int } else { 0int }),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_map_where_len(s.drop_last(), keep, f);
    }
}

pub proof fn lemma_dedup_len_gen<A>(s: Seq<A>)
    ensures dedup(s).len() <= s.len(),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_dedup_len_gen(s.drop_last());
    }
}

/// A company is in the group's list exactly when its present-count is positive.
proof fn lemma_company_total(db: Db, p: Params, sigma: int, tl: Option<Seq<u8>>, c0: int)
    requires p.p1 <= sigma <= p.p2,
    ensures
        tot_db(db)(gp(sigma, p.p3, p.p4), hf(sigma, tl, q_cik(c0))) >= 0,
        comp_seq(db, p, sigma, tl).contains(c0) == (tot_db(db)(gp(sigma, p.p3, p.p4), hf(sigma, tl, q_cik(c0))) > 0),
{
    let tup_ = tuples(db, p);
    let keep = |t: Tuple| t.s_.cik is Some;
    let sel = |t: Tuple| t.s_.sic == Some(sigma) && t.t_.tlabel == tl && t.p.stmt == Some(p.p3) && keep(t);
    let fc = |t: Tuple| t.s_.cik->0;
    let ind = |t: Tuple| if sel(t) && fc(t) == c0 { 1int } else { 0int };
    let ftc = |t: Tuple| if t.s_.cik is Some && t.s_.cik->0 == c0 { 1int } else { 0int };
    assert forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| wc(p, n, s, t, r) implies #[trigger] ftc(tup(n, s, t, r)) == q_cik(c0)(pj(tup(n, s, t, r))) by {}
    lemma_group_tot(db, p, sigma, tl, q_cik(c0), ftc);
    let g1 = |t: Tuple| if t.s_.sic == Some(sigma) && t.t_.tlabel == tl && t.p.stmt == Some(p.p3) { ftc(t) } else { 0 };
    assert forall|t: Tuple| #[trigger] g1(t) == ind(t) by {}
    lemma_sum_ext_all(tup_, g1, ind);
    assert(group_sum(db, p, Some(sigma), tl, Some(p.p3), ftc) == sum(tup_, g1));
    let total = tot_db(db)(gp(sigma, p.p3, p.p4), hf(sigma, tl, q_cik(c0)));
    assert(total == sum(tup_, ind));
    assert forall|i: int| 0 <= i < tup_.len() implies ind(#[trigger] tup_[i]) >= 0 by {}
    lemma_sum_nonneg(tup_, ind);
    lemma_map_where(tup_, sel, fc);
    let cs_ = comp_seq(db, p, sigma, tl);
    assert(cs_ =~= map_where(tup_, sel, fc));
    if cs_.contains(c0) {
        let i = choose|i: int| 0 <= i < tup_.len() && sel(tup_[i]) && fc(tup_[i]) == c0;
        lemma_sum_pos(tup_, ind, i);
    }
    if total > 0 {
        let i = lemma_sum_nonzero_witness(tup_, ind);
        assert(sel(tup_[i]) && fc(tup_[i]) == c0);
    }
}

/// The number of companies of a group, from the set of companies with nonzero totals.
pub proof fn lemma_companies(db: Db, p: Params, sigma: int, tl: Option<Seq<u8>>, cs: Set<int>)
    requires
        p.p1 <= sigma <= p.p2,
        forall|c0: int| #[trigger] cs.contains(c0) == (tot_db(db)(gp(sigma, p.p3, p.p4), hf(sigma, tl, q_cik(c0))) != 0),
    ensures
        distinct_count(comp_seq(db, p, sigma, tl)) == cs.len(),
{
    let spec_seq = comp_seq(db, p, sigma, tl);
    assert forall|c0: int| #[trigger] spec_seq.contains(c0) == cs.contains(c0) by {
        lemma_company_total(db, p, sigma, tl, c0);
    }
    lemma_dedup(spec_seq);
    let a = dedup(spec_seq);
    assert(a.no_duplicates());
    a.unique_seq_to_set();
    assert(a.to_set() =~= cs) by {
        assert forall|x: int| a.to_set().contains(x) == cs.contains(x) by {
            assert(a.to_set().contains(x) == a.contains(x));
        }
    }
}

/// A group has at least as many tuples as companies.
pub proof fn lemma_count_ge_companies(db: Db, p: Params, sigma: int, tl: Option<Seq<u8>>)
    requires p.p1 <= sigma <= p.p2,
    ensures
        distinct_count(comp_seq(db, p, sigma, tl)) <= group_count(db, p, Some(sigma), tl, Some(p.p3)),
{
    let tup_ = tuples(db, p);
    let keep = |t: Tuple| t.s_.cik is Some;
    let sel = |t: Tuple| t.s_.sic == Some(sigma) && t.t_.tlabel == tl && t.p.stmt == Some(p.p3) && keep(t);
    let fc = |t: Tuple| t.s_.cik->0;
    let cs_ = comp_seq(db, p, sigma, tl);
    assert(cs_ =~= map_where(tup_, sel, fc));
    lemma_map_where_len(tup_, sel, fc);
    lemma_dedup_len_gen(cs_);
    let a = |t: Tuple| if sel(t) { 1int } else { 0int };
    let b = |t: Tuple| if t.s_.sic == Some(sigma) && t.t_.tlabel == tl && t.p.stmt == Some(p.p3) { 1int } else { 0int };
    assert forall|t: Tuple| #[trigger] a(t) <= b(t) by {}
    lemma_sum_le(tup_, a, b);
}

proof fn lemma_abs_sum<A>(s: Seq<A>, f: spec_fn(A) -> int)
    ensures abs(sum(s, f)) <= sum(s, |x: A| abs(f(x))),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_abs_sum(s.drop_last(), f);
    }
}

/// The sum of a group is within its sum of magnitudes.
pub proof fn lemma_sum_within_abs(db: Db, p: Params, gs: Option<int>, gt: Option<Seq<u8>>, gst: Option<Seq<u8>>)
    ensures
        abs(group_sum(db, p, gs, gt, gst, tuple_value())) <= group_sum(db, p, gs, gt, gst, tuple_magnitude()),
{
    let tup_ = tuples(db, p);
    let fv = |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { tuple_value()(t) } else { 0 };
    let fm = |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { tuple_magnitude()(t) } else { 0 };
    lemma_abs_sum(tup_, fv);
    assert forall|t: Tuple| #[trigger] abs(fv(t)) == fm(t) by {}
    lemma_sum_ext_all(tup_, |t: Tuple| abs(fv(t)), fm);
}

pub proof fn lemma_count_nonneg(db: Db, p: Params, gs: Option<int>, gt: Option<Seq<u8>>, gst: Option<Seq<u8>>)
    ensures group_count(db, p, gs, gt, gst) >= 0,
{
    let tup_ = tuples(db, p);
    let b = |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { 1int } else { 0int };
    assert forall|i: int| 0 <= i < tup_.len() implies b(#[trigger] tup_[i]) >= 0 by {}
    lemma_sum_nonneg(tup_, b);
}


pub open spec fn cell_facts(db: Db, p: Params, sigma: int, tl: Option<Seq<u8>>, cnt: int, sm: int, ab: int, nc: int) -> bool {
    &&& cnt == group_count(db, p, Some(sigma), tl, Some(p.p3))
    &&& cnt == group_sum(db, p, Some(sigma), tl, Some(p.p3), tuple_one())
    &&& sm == group_sum(db, p, Some(sigma), tl, Some(p.p3), tuple_value())
    &&& ab == group_sum(db, p, Some(sigma), tl, Some(p.p3), tuple_magnitude())
    &&& nc == distinct_count(comp_seq(db, p, sigma, tl))
    &&& nc <= cnt
    &&& abs(sm) <= ab
}

pub proof fn lemma_count_as_group_sum(db: Db, p: Params, gs: Option<int>, gt: Option<Seq<u8>>, gst: Option<Seq<u8>>)
    ensures group_count(db, p, gs, gt, gst) == group_sum(db, p, gs, gt, gst, |t: Tuple| 1int),
{
    let tup_ = tuples(db, p);
    lemma_sum_ext_all(tup_, |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { 1int } else { 0int },
        |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { (|t2: Tuple| 1int)(t) } else { 0 });
}

/// A stored group is the spec's group.
pub proof fn lemma_cell_sound(db: Db, p: Params, g: Index<Index<Cell>>, c: Cell)
    requires
        rep(g, tot_db(db)),
        cell_at(g, p.p4, p.p3, c.sic as int, opt_bytes(c.tlabel)) == Some(c),
        p.p1 <= c.sic as int <= p.p2,
    ensures
        cell_facts(db, p, c.sic as int, opt_bytes(c.tlabel), c.count@, c.sum@, c.abs@, c.nciks as int),
{
    let (sigma, tl) = (c.sic as int, opt_bytes(c.tlabel));
    let q = gp(sigma, p.p3, p.p4);
    assert(cell_fields(c, tot_db(db), p.p4, p.p3));
    let one_t = |t: Tuple| 1int;
    assert forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| wc(p, n, s, t, r) implies #[trigger] one_t(tup(n, s, t, r)) == q_one()(pj(tup(n, s, t, r))) by {}
    lemma_group_tot(db, p, sigma, tl, q_one(), one_t);
    lemma_count_as_group_sum(db, p, Some(sigma), tl, Some(p.p3));
    assert forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| wc(p, n, s, t, r) implies #[trigger] tuple_one()(tup(n, s, t, r)) == q_one()(pj(tup(n, s, t, r))) by {}
    lemma_group_tot(db, p, sigma, tl, q_one(), tuple_one());
    assert forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| wc(p, n, s, t, r) implies #[trigger] tuple_value()(tup(n, s, t, r)) == q_val()(pj(tup(n, s, t, r))) by {}
    lemma_group_tot(db, p, sigma, tl, q_val(), tuple_value());
    assert forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| wc(p, n, s, t, r) implies #[trigger] tuple_magnitude()(tup(n, s, t, r)) == q_abs()(pj(tup(n, s, t, r))) by {}
    lemma_group_tot(db, p, sigma, tl, q_abs(), tuple_magnitude());
    assert forall|c0: int| #[trigger] c.cs@.contains(c0) == (tot_db(db)(q, hf(sigma, tl, q_cik(c0))) != 0) by {
        if c.ciks.at(ck(c0)) is Some {
            assert(c.cs@.contains(c0));
        } else {
            assert(!c.cs@.contains(c0));
        }
    }
    lemma_companies(db, p, sigma, tl, c.cs@);
    lemma_count_ge_companies(db, p, sigma, tl);
    lemma_sum_within_abs(db, p, Some(sigma), tl, Some(p.p3));
}

/// No stored group: no tuples.
pub proof fn lemma_cell_absent(db: Db, p: Params, g: Index<Index<Cell>>, sigma: int, tl: Option<Seq<u8>>)
    requires
        rep(g, tot_db(db)),
        cell_at(g, p.p4, p.p3, sigma, tl) is None,
        p.p1 <= sigma <= p.p2,
    ensures
        group_count(db, p, Some(sigma), tl, Some(p.p3)) == 0,
{
    let one_t = |t: Tuple| 1int;
    assert forall|n: NumRow, s: SubRow, t: TagRow, r: PreRow| wc(p, n, s, t, r) implies #[trigger] one_t(tup(n, s, t, r)) == q_one()(pj(tup(n, s, t, r))) by {}
    lemma_group_tot(db, p, sigma, tl, q_one(), one_t);
    lemma_count_as_group_sum(db, p, Some(sigma), tl, Some(p.p3));
}

}
