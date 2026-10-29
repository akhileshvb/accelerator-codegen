// The spec's group quantities (sums, count, distinct companies over the query's tuples) as the
// join records' quantities.
use vstd::prelude::*;
use verus_common::order::*;
use verus_common::rel::*;
use verus_common::sums::*;
use verus_prooflib::lemma_sum_ones;
use verus_prooflib::lemma_sum_same_support;
use crate::gen_spec::*;
use crate::groups::*;
use crate::join::*;
use crate::join_spec::*;

verus! {

/// The records stand for the query's tuples: every aggregate over one is the same over the other.
pub open spec fn records_ok(db: Db, p: Params, l: Seq<Rec>) -> bool {
    &&& all_nonneg(l)
    &&& forall|h: spec_fn(JK) -> int| #[trigger] recsum(l, p, h) == sum(tuples(db, p), |tp: Tuple| h(pj(tp)))
}

pub proof fn lemma_group_sum(db: Db, p: Params, l: Seq<Rec>, gs: Option<int>, gt: Option<Seq<u8>>, gst: Option<Seq<u8>>,
    fj: spec_fn(JK) -> int, ft: spec_fn(Tuple) -> int)
    requires
        records_ok(db, p, l),
        forall|t: Tuple| #[trigger] ft(t) == fj(pj(t)),
    ensures
        sum(tuples(db, p), |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { ft(t) } else { 0 })
            == if gst == Some(p.p3) { gsum(l, p, gs, gt, fj) } else { 0 },
{
    let h = |j: JK| if j.sic == gs && j.tlabel == gt && j.stmt == gst { fj(j) } else { 0 };
    let lhs = |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { ft(t) } else { 0 };
    let hp = |tp: Tuple| h(pj(tp));
    assert forall|t: Tuple| #[trigger] lhs(t) == hp(t) by {}
    lemma_sum_ext_all(tuples(db, p), lhs, hp);
    let a = |r: Rec| r.w as int * h(rec_jk(p, r));
    assert(recsum(l, p, h) == sum(tuples(db, p), hp));
    assert(recsum(l, p, h) == sum(l, a));
    if gst == Some(p.p3) {
        let g = |r: Rec| if in_group(p, r, gs, gt) { r.w as int * fj(rec_jk(p, r)) } else { 0 };
        assert forall|r: Rec| #[trigger] a(r) == g(r) by {}
        lemma_sum_ext_all(l, a, g);
        assert(gsum(l, p, gs, gt, fj) == sum(l, g));
    } else {
        assert forall|r: Rec| #[trigger] a(r) == 0int by {}
        lemma_sum_ext_all(l, a, |r: Rec| 0int);
        lemma_sum_zero(l, |r: Rec| 0int);
    }
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

/// A record's `vs` is true, so counting present values counts the weights.
proof fn lemma_vs_one(l: Seq<Rec>, p: Params, gs: Option<int>, gt: Option<Seq<u8>>)
    ensures gsum(l, p, gs, gt, |j: JK| if j.vs { 1int } else { 0int }) == gsum(l, p, gs, gt, one()),
{
    let f = |j: JK| if j.vs { 1int } else { 0int };
    lemma_sum_ext_all(l, |r: Rec| if in_group(p, r, gs, gt) { r.w as int * f(rec_jk(p, r)) } else { 0 },
        |r: Rec| if in_group(p, r, gs, gt) { r.w as int * one()(rec_jk(p, r)) } else { 0 });
}

/// COUNT(*), the present-value count, SUM and sum of magnitudes of a group, as the cell's.
pub proof fn lemma_group_quantities(db: Db, p: Params, l: Seq<Rec>, gs: Option<int>, gt: Option<Seq<u8>>)
    requires records_ok(db, p, l),
    ensures
        group_count(db, p, gs, gt, Some(p.p3)) == gsum(l, p, gs, gt, one()),
        group_sum(db, p, gs, gt, Some(p.p3), tuple_one()) == gsum(l, p, gs, gt, one()),
        group_sum(db, p, gs, gt, Some(p.p3), tuple_value()) == gsum(l, p, gs, gt, value()),
        group_sum(db, p, gs, gt, Some(p.p3), tuple_magnitude()) == gsum(l, p, gs, gt, magnitude()),
{
    let st = Some(p.p3);
    let tup = tuples(db, p);
    let one_t = |t: Tuple| 1int;
    lemma_group_sum(db, p, l, gs, gt, st, one(), one_t);
    lemma_sum_ext_all(tup, |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == st { 1int } else { 0int },
        |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == st { one_t(t) } else { 0 });
    assert(group_count(db, p, gs, gt, st) == sum(tup, |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == st { 1int } else { 0int }));
    let vs_one = |j: JK| if j.vs { 1int } else { 0int };
    lemma_group_sum(db, p, l, gs, gt, st, vs_one, tuple_one());
    lemma_group_sum(db, p, l, gs, gt, st, value(), tuple_value());
    lemma_group_sum(db, p, l, gs, gt, st, magnitude(), tuple_magnitude());
    lemma_vs_one(l, p, gs, gt);
}

/// A group of another statement has no tuples.
pub proof fn lemma_other_statement(db: Db, p: Params, l: Seq<Rec>, gs: Option<int>, gt: Option<Seq<u8>>, gst: Option<Seq<u8>>)
    requires
        records_ok(db, p, l),
        gst != Some(p.p3),
    ensures
        group_count(db, p, gs, gt, gst) == 0,
{
    let tup = tuples(db, p);
    let one_t = |t: Tuple| 1int;
    lemma_group_sum(db, p, l, gs, gt, gst, one(), one_t);
    lemma_sum_ext_all(tup, |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { 1int } else { 0int },
        |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { one_t(t) } else { 0 });
    assert(group_count(db, p, gs, gt, gst) == sum(tup, |t: Tuple| if t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == gst { 1int } else { 0int }));
}

proof fn lemma_cikseq_contains(l: Seq<Rec>, p: Params, gs: Option<int>, gt: Option<Seq<u8>>, c: int)
    ensures
        cikseq(l, p, gs, gt).contains(c)
            == exists|i: int| 0 <= i < l.len() && in_group(p, #[trigger] l[i], gs, gt) && l[i].w > 0 && opt_int(l[i].cik) == Some(c),
    decreases l.len(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    if l.len() > 0 {
        let rest = l.drop_last();
        lemma_cikseq_contains(rest, p, gs, gt, c);
        let last = l.last();
        assert(l[l.len() - 1] == last);
        let cond = in_group(p, last, gs, gt) && last.w > 0 && last.cik is Some;
        let e_rest = exists|i: int| 0 <= i < rest.len() && in_group(p, #[trigger] rest[i], gs, gt) && rest[i].w > 0 && opt_int(rest[i].cik) == Some(c);
        let e_l = exists|i: int| 0 <= i < l.len() && in_group(p, #[trigger] l[i], gs, gt) && l[i].w > 0 && opt_int(l[i].cik) == Some(c);
        let cond_c = cond && opt_int(last.cik) == Some(c);
        if e_rest {
            let i = choose|i: int| 0 <= i < rest.len() && in_group(p, #[trigger] rest[i], gs, gt) && rest[i].w > 0 && opt_int(rest[i].cik) == Some(c);
            assert(l[i] == rest[i]);
        }
        if e_l {
            let i = choose|i: int| 0 <= i < l.len() && in_group(p, #[trigger] l[i], gs, gt) && l[i].w > 0 && opt_int(l[i].cik) == Some(c);
            if i < l.len() - 1 { assert(rest[i] == l[i]); }
        }
        assert(e_l == (e_rest || cond_c));
        let x = cikseq(l, p, gs, gt);
        assert(x == if cond { cikseq(rest, p, gs, gt).push(last.cik->0 as int) } else { cikseq(rest, p, gs, gt) });
        if cond {
            assert(x.contains(c) == (cikseq(rest, p, gs, gt).contains(c) || last.cik->0 as int == c));
        }
        assert(x.contains(c) == (cikseq(rest, p, gs, gt).contains(c) || cond_c));
    }
}

/// The set of companies of a group, as a distinct list of the records' or of the tuples'.
pub proof fn lemma_group_companies(db: Db, p: Params, l: Seq<Rec>, gs: Option<int>, gt: Option<Seq<u8>>)
    requires records_ok(db, p, l),
    ensures
        distinct_count(group_vals_where(db, p, gs, gt, Some(p.p3), |t: Tuple| t.s_.cik is Some, |t: Tuple| t.s_.cik->0))
            == dedup(cikseq(l, p, gs, gt)).len(),
{
    let st = Some(p.p3);
    let keep = |t: Tuple| t.s_.cik is Some;
    let fc = |t: Tuple| t.s_.cik->0;
    let tup = tuples(db, p);
    let spec_seq = group_vals_where(db, p, gs, gt, st, keep, fc);
    let sel = |t: Tuple| t.s_.sic == gs && t.t_.tlabel == gt && t.p.stmt == st && keep(t);
    assert(spec_seq =~= map_where(tup, sel, fc));
    lemma_map_where(tup, sel, fc);
    let ours = cikseq(l, p, gs, gt);
    assert forall|c: int| spec_seq.contains(c) == ours.contains(c) by {
        lemma_cikseq_contains(l, p, gs, gt, c);
        let ind = |t: Tuple| if sel(t) && fc(t) == c { 1int } else { 0int };
        let hc = |j: JK| if j.sic == gs && j.tlabel == gt && j.stmt == st && j.cik == Some(c) { 1int } else { 0int };
        assert forall|t: Tuple| #[trigger] ind(t) == hc(pj(t)) by {}
        lemma_sum_ext_all(tup, ind, |t: Tuple| hc(pj(t)));
        let rec_term = |r: Rec| r.w as int * hc(rec_jk(p, r));
        assert(recsum(l, p, hc) == sum(l, rec_term));
        assert forall|i: int| 0 <= i < l.len() implies rec_term(#[trigger] l[i]) >= 0 by {
            assert(l[i].w >= 0);
        }
        assert forall|i: int| 0 <= i < tup.len() implies ind(#[trigger] tup[i]) >= 0 by {}
        // spec_seq contains c  <=>  some tuple counts  <=>  the records' weighted count is positive  <=>  ours contains c
        if spec_seq.contains(c) {
            let i = choose|i: int| 0 <= i < tup.len() && sel(tup[i]) && fc(tup[i]) == c;
            lemma_sum_pos(tup, ind, i);
            assert(sum(l, rec_term) > 0);
            let k = lemma_sum_nonzero_witness(l, rec_term);
            assert(rec_term(l[k]) != 0);
            assert(l[k].w > 0 && in_group(p, l[k], gs, gt) && opt_int(l[k].cik) == Some(c));
        }
        if ours.contains(c) {
            let k = choose|k: int| 0 <= k < l.len() && in_group(p, l[k], gs, gt) && l[k].w > 0 && opt_int(l[k].cik) == Some(c);
            assert(rec_term(l[k]) > 0);
            lemma_sum_pos(l, rec_term, k);
            assert(sum(tup, ind) > 0);
            let i = lemma_sum_nonzero_witness(tup, ind);
            assert(sel(tup[i]) && fc(tup[i]) == c);
        }
    }
    lemma_dedup(spec_seq);
    lemma_dedup(ours);
    let (a, b) = (dedup(spec_seq), dedup(ours));
    assert forall|x: int| a.contains(x) == b.contains(x) by {}
    assert(a.to_set() =~= b.to_set()) by {
        a.to_set_ensures();
        b.to_set_ensures();
    }
    assert(a.no_duplicates() && b.no_duplicates()) by {
        assert forall|i: int, j: int| 0 <= i < j < a.len() implies a[i] != a[j] by {}
        assert forall|i: int, j: int| 0 <= i < j < b.len() implies b[i] != b[j] by {}
    }
    lemma_sum_same_support(a, b, |x: int| 1int);
    lemma_sum_ones(a);
    lemma_sum_ones(b);
}

}
