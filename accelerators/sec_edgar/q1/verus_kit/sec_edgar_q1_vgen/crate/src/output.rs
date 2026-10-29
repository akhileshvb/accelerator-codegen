// The answer the state gives: one row per listed group of the parameter's stmt, ordered by cnt.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rat::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::steps::*;
use crate::answer::*;

verus! {

pub open spec fn out_row(a: Agg) -> OutRow {
    OutRow {
        stmt: Some(a.s()),
        rfile: a.rf(),
        cnt: a.cnt as int,
        num_filings: a.filings as int,
        avg_line_num: if a.linecnt > 0 { Some(Rat { num: a.linesum as int, den: a.linecnt as int }) } else { None },
    }
}

pub open spec fn qualifies(a: Agg, p1: Seq<u8>) -> bool { a.s() == p1 && a.cnt > 0 }

pub proof fn lemma_vsum_plain(base: Seq<PreRow>, f: spec_fn(PreRow) -> int)
    ensures
        vsum(base, Seq::empty(), f) == sum(base, f),
{
    lemma_sum_empty(Seq::<(PreRow, int)>::empty(), |d: (PreRow, int)| d.1 * f(d.0));
}

/// A listed group with rows is a row of the answer.
pub proof fn lemma_row_ok(db: Db, params: Params, aggs: Seq<Agg>, cells: Index<i128>, i: int)
    requires
        parts_ok(aggs, cells, db.pre, Seq::empty()),
        0 <= i < aggs.len(),
        qualifies(aggs[i], params.p1),
    ensures
        row_ok(db, params, out_row(aggs[i])),
{
    let a = aggs[i];
    let gr = a.rf();
    lemma_group_count_is(db, params, gr);
    lemma_vsum_plain(db.pre, w_cnt(a.s(), a.rf()));
    lemma_vsum_plain(db.pre, w_lcnt(a.s(), a.rf()));
    lemma_vsum_plain(db.pre, w_lsum(a.s(), a.rf()));
    lemma_group_lines(db, params, gr);
    lemma_filings_distinct(db, params, aggs, cells, i);
    assert(cells_ok(aggs, cells, db.pre, Seq::empty()));
    assert(having(db, params, Some(params.p1), gr));
}

/// Every nonempty group of the answer is listed with rows.
pub proof fn lemma_cover(db: Db, params: Params, aggs: Seq<Agg>, cells: Index<i128>, gs: Option<Seq<u8>>, gr: Option<Seq<u8>>) -> (i: int)
    requires
        parts_ok(aggs, cells, db.pre, Seq::empty()),
        group_count(db, params, gs, gr) > 0,
    ensures
        0 <= i < aggs.len(),
        qualifies(aggs[i], params.p1),
        gs == Some(params.p1),
        aggs[i].rf() == gr,
{
    if gs != Some(params.p1) { lemma_group_count_other(db, params, gs, gr); }
    lemma_group_count_is(db, params, gr);
    let w = w_cnt(params.p1, gr);
    let k = lemma_sum_nonzero_witness(db.pre, w);
    assert(in_grp(db.pre[k], params.p1, gr));
    assert(has_key(aggs, params.p1, gr)) by {
        if !has_key(aggs, params.p1, gr) {
            assert(!in_grp(db.pre[k], params.p1, gr));
        }
    }
    let i = choose|i: int| 0 <= i < aggs.len() && (#[trigger] aggs[i]).s() == params.p1 && aggs[i].rf() == gr;
    lemma_vsum_plain(db.pre, w_cnt(aggs[i].s(), aggs[i].rf()));
    assert(aggs[i].cnt as int == sum(db.pre, w));
    i
}

/// The rows (indices into aggs, in order) form a full answer.
pub proof fn lemma_full(db: Db, params: Params, aggs: Seq<Agg>, cells: Index<i128>, g: Seq<int>, out: Seq<OutRow>)
    requires
        parts_ok(aggs, cells, db.pre, Seq::empty()),
        out.len() == g.len(),
        forall|k: int| 0 <= k < g.len() ==> 0 <= #[trigger] g[k] < aggs.len() && qualifies(aggs[g[k]], params.p1),
        forall|k: int| 0 <= k < g.len() ==> #[trigger] out[k] == out_row(aggs[g[k]]),
        forall|k: int, l: int| 0 <= k < l < g.len() ==> g[k] != g[l] && aggs[g[k]].cnt >= aggs[g[l]].cnt,
        forall|i: int| 0 <= i < aggs.len() && qualifies(aggs[i], params.p1) ==> exists|k: int| 0 <= k < g.len() && g[k] == i,
    ensures
        full_result(db, params, out),
{
    assert forall|k: int| 0 <= k < out.len() implies row_ok(db, params, #[trigger] out[k]) by {
        lemma_row_ok(db, params, aggs, cells, g[k]);
    }
    assert forall|k: int, l: int| 0 <= k < l < out.len() implies
        !same_group(#[trigger] out[k], #[trigger] out[l]) && !row_lt(out[l], out[k]) by {
        let (a, b) = (aggs[g[k]], aggs[g[l]]);
        assert(g[k] != g[l]);
        if g[k] < g[l] { assert(!same_key(aggs[g[k]], aggs[g[l]])); } else { assert(!same_key(aggs[g[l]], aggs[g[k]])); }
    }
    assert forall|gs: Option<Seq<u8>>, gr: Option<Seq<u8>>| #[trigger] group_count(db, params, gs, gr) > 0 && having(db, params, gs, gr) implies
        exists|k: int| 0 <= k < out.len() && (#[trigger] out[k]).stmt == gs && out[k].rfile == gr by {
        let i = lemma_cover(db, params, aggs, cells, gs, gr);
        let k = choose|k: int| 0 <= k < g.len() && g[k] == i;
        assert(out[k] == out_row(aggs[i]));
    }
}

/// Under answer_fits, every group's counts fit the result's types.
pub proof fn lemma_group_fits(db: Db, params: Params, aggs: Seq<Agg>, cells: Index<i128>, i: int)
    requires
        parts_ok(aggs, cells, db.pre, Seq::empty()),
        answer_fits(db, params),
        0 <= i < aggs.len(),
        qualifies(aggs[i], params.p1),
    ensures
        out_fits(out_row(aggs[i])),
{
    lemma_row_ok(db, params, aggs, cells, i);
    lemma_answer_ok(db, params);
    let ans = answer(db, params);
    assert(query_result(db, params, ans));
    let gr = aggs[i].rf();
    lemma_group_count_is(db, params, gr);
    lemma_vsum_plain(db.pre, w_cnt(aggs[i].s(), aggs[i].rf()));
    assert(group_count(db, params, Some(params.p1), gr) > 0);
    let k = choose|k: int| 0 <= k < ans.len() && (#[trigger] ans[k]).stmt == Some(params.p1) && ans[k].rfile == gr;
    assert(row_ok(db, params, ans[k]));
    assert(out_fits(ans[k]));
    assert(ans[k] == out_row(aggs[i]));
}

/// Filing totals are counts.
pub proof fn lemma_filings_nonneg(aggs: Seq<Agg>, cells: Index<i128>, i: int)
    requires
        filings_ok(aggs, cells),
        0 <= i < aggs.len(),
    ensures
        aggs[i].filings as int >= 0,
{
    let w = filing_w(i);
    assert forall|p: int| 0 <= p < cells.entries().len() implies w(#[trigger] cells.entries()[p]) >= 0 by {}
    lemma_sum_nonneg(cells.entries(), w);
}

} // verus!
