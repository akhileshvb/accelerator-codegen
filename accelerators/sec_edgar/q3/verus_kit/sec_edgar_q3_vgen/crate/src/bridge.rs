// The spec's group quantities over the query's tuples as the join records' quantities.
use vstd::prelude::*;
use verus_common::order::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::groups::*;
use crate::join::*;
use crate::join_spec::*;

verus! {

/// The records stand for the query's tuples (of both blocks): every aggregate over one is the same over the other.
pub open spec fn records_ok(db: Db, p: Params, l: Seq<Rec>) -> bool {
    &&& forall|h: spec_fn(JK) -> int| #[trigger] recsum(l, h) == sum(tuples(db, p), |tp: Tuple| h(pj(tp)))
    &&& forall|h: spec_fn(JK) -> int| #[trigger] recsum(l, h) == sum(avg_sub_tuples(db, p), |tp: AvgSubTuple| h(pj2(tp)))
    &&& forall|i: int| 0 <= i < l.len() ==> rec_ok(#[trigger] l[i])
}

/// Σ over the records of one company, whatever the name.
pub open spec fn isum(l: Seq<Rec>, cik: Option<int>, f: spec_fn(JK) -> int) -> int {
    sum(l, |r: Rec| if rec_jk(r).cik == cik { f(rec_jk(r)) } else { 0 })
}

pub open spec fn tup_vs() -> spec_fn(Tuple) -> int {
    |t: Tuple| if t.n.value is Some { 1int } else { 0int }
}

pub open spec fn tup_value() -> spec_fn(Tuple) -> int {
    |t: Tuple| if t.n.value is Some { t.n.value->0 } else { 0int }
}

pub open spec fn tup_mag() -> spec_fn(Tuple) -> int {
    |t: Tuple| if t.n.value is Some { abs(t.n.value->0) } else { 0int }
}

pub open spec fn avg_vs() -> spec_fn(AvgSubTuple) -> int {
    |t: AvgSubTuple| if t.n2.value is Some { 1int } else { 0int }
}

pub open spec fn avg_value() -> spec_fn(AvgSubTuple) -> int {
    |t: AvgSubTuple| if t.n2.value is Some { t.n2.value->0 } else { 0int }
}

pub open spec fn avg_mag() -> spec_fn(AvgSubTuple) -> int {
    |t: AvgSubTuple| if t.n2.value is Some { abs(t.n2.value->0) } else { 0int }
}

pub open spec fn vs_one() -> spec_fn(JK) -> int {
    |j: JK| if j.vs { 1int } else { 0int }
}

/// A record's `vs` is true, so counting present values counts the records.
proof fn lemma_vs_one(l: Seq<Rec>, name: Option<Seq<u8>>, cik: Option<int>)
    ensures
        gsum(l, name, cik, vs_one()) == gsum(l, name, cik, one()),
        isum(l, cik, vs_one()) == isum(l, cik, one()),
{
    lemma_sum_ext_all(l, |r: Rec| if in_group(r, name, cik) { vs_one()(rec_jk(r)) } else { 0 },
        |r: Rec| if in_group(r, name, cik) { one()(rec_jk(r)) } else { 0 });
    lemma_sum_ext_all(l, |r: Rec| if rec_jk(r).cik == cik { vs_one()(rec_jk(r)) } else { 0 },
        |r: Rec| if rec_jk(r).cik == cik { one()(rec_jk(r)) } else { 0 });
}

pub proof fn lemma_outer_quantities(db: Db, p: Params, l: Seq<Rec>, gn: Option<Seq<u8>>, gc: Option<int>)
    requires records_ok(db, p, l),
    ensures
        group_count(db, p, gn, gc) == gsum(l, gn, gc, one()),
        group_sum(db, p, gn, gc, tup_vs()) == gsum(l, gn, gc, one()),
        group_sum(db, p, gn, gc, tup_value()) == gsum(l, gn, gc, value()),
        group_sum(db, p, gn, gc, tup_mag()) == gsum(l, gn, gc, magnitude()),
{
    let tup = tuples(db, p);
    lemma_vs_one(l, gn, gc);
    let fs = [one(), vs_one(), value(), magnitude()];
    // each: the tuple closure is the record closure at pj
    let h1 = |j: JK| if j.name == gn && j.cik == gc { 1int } else { 0int };
    let hv = |j: JK| if j.name == gn && j.cik == gc { vs_one()(j) } else { 0int };
    let hx = |j: JK| if j.name == gn && j.cik == gc { j.v } else { 0int };
    let hm = |j: JK| if j.name == gn && j.cik == gc { abs(j.v) } else { 0int };
    assert(recsum(l, h1) == gsum(l, gn, gc, one())) by {
        lemma_sum_ext_all(l, |r: Rec| h1(rec_jk(r)), |r: Rec| if in_group(r, gn, gc) { one()(rec_jk(r)) } else { 0 });
    }
    assert(recsum(l, hv) == gsum(l, gn, gc, vs_one())) by {
        lemma_sum_ext_all(l, |r: Rec| hv(rec_jk(r)), |r: Rec| if in_group(r, gn, gc) { vs_one()(rec_jk(r)) } else { 0 });
    }
    assert(recsum(l, hx) == gsum(l, gn, gc, value())) by {
        lemma_sum_ext_all(l, |r: Rec| hx(rec_jk(r)), |r: Rec| if in_group(r, gn, gc) { value()(rec_jk(r)) } else { 0 });
    }
    assert(recsum(l, hm) == gsum(l, gn, gc, magnitude())) by {
        lemma_sum_ext_all(l, |r: Rec| hm(rec_jk(r)), |r: Rec| if in_group(r, gn, gc) { magnitude()(rec_jk(r)) } else { 0 });
    }
    lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { 1int } else { 0int }, |t: Tuple| h1(pj(t)));
    lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_vs()(t) } else { 0 }, |t: Tuple| hv(pj(t)));
    lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_value()(t) } else { 0 }, |t: Tuple| hx(pj(t)));
    lemma_sum_ext_all(tup, |t: Tuple| if t.s_.name == gn && t.s_.cik == gc { tup_mag()(t) } else { 0 }, |t: Tuple| hm(pj(t)));
    assert(recsum(l, h1) == sum(tup, |t: Tuple| h1(pj(t))));
    assert(recsum(l, hv) == sum(tup, |t: Tuple| hv(pj(t))));
    assert(recsum(l, hx) == sum(tup, |t: Tuple| hx(pj(t))));
    assert(recsum(l, hm) == sum(tup, |t: Tuple| hm(pj(t))));
}

pub proof fn lemma_inner_quantities(db: Db, p: Params, l: Seq<Rec>, gc: Option<int>)
    requires records_ok(db, p, l),
    ensures
        avg_sub_group_count(db, p, gc) == isum(l, gc, one()),
        avg_sub_group_sum(db, p, gc, avg_vs()) == isum(l, gc, one()),
        avg_sub_group_sum(db, p, gc, avg_value()) == isum(l, gc, value()),
        avg_sub_group_sum(db, p, gc, avg_mag()) == isum(l, gc, magnitude()),
{
    let tup = avg_sub_tuples(db, p);
    lemma_vs_one(l, None, gc);
    let h1 = |j: JK| if j.cik == gc { 1int } else { 0int };
    let hv = |j: JK| if j.cik == gc { vs_one()(j) } else { 0int };
    let hx = |j: JK| if j.cik == gc { j.v } else { 0int };
    let hm = |j: JK| if j.cik == gc { abs(j.v) } else { 0int };
    assert(recsum(l, h1) == isum(l, gc, one())) by {
        lemma_sum_ext_all(l, |r: Rec| h1(rec_jk(r)), |r: Rec| if rec_jk(r).cik == gc { one()(rec_jk(r)) } else { 0 });
    }
    assert(recsum(l, hv) == isum(l, gc, vs_one())) by {
        lemma_sum_ext_all(l, |r: Rec| hv(rec_jk(r)), |r: Rec| if rec_jk(r).cik == gc { vs_one()(rec_jk(r)) } else { 0 });
    }
    assert(recsum(l, hx) == isum(l, gc, value())) by {
        lemma_sum_ext_all(l, |r: Rec| hx(rec_jk(r)), |r: Rec| if rec_jk(r).cik == gc { value()(rec_jk(r)) } else { 0 });
    }
    assert(recsum(l, hm) == isum(l, gc, magnitude())) by {
        lemma_sum_ext_all(l, |r: Rec| hm(rec_jk(r)), |r: Rec| if rec_jk(r).cik == gc { magnitude()(rec_jk(r)) } else { 0 });
    }
    lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { 1int } else { 0int }, |t: AvgSubTuple| h1(pj2(t)));
    lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_vs()(t) } else { 0 }, |t: AvgSubTuple| hv(pj2(t)));
    lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_value()(t) } else { 0 }, |t: AvgSubTuple| hx(pj2(t)));
    lemma_sum_ext_all(tup, |t: AvgSubTuple| if t.s2.cik == gc { avg_mag()(t) } else { 0 }, |t: AvgSubTuple| hm(pj2(t)));
    assert(recsum(l, h1) == sum(tup, |t: AvgSubTuple| h1(pj2(t))));
    assert(recsum(l, hv) == sum(tup, |t: AvgSubTuple| hv(pj2(t))));
    assert(recsum(l, hx) == sum(tup, |t: AvgSubTuple| hx(pj2(t))));
    assert(recsum(l, hm) == sum(tup, |t: AvgSubTuple| hm(pj2(t))));
}

}
