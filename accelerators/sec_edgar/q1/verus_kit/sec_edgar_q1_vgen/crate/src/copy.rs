// Copying the (small) group directory for a new state version.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::model::*;
use crate::steps::*;
use crate::absorb::*;

verus! {

pub open spec fn agg_same(a: Agg, b: Agg) -> bool {
    same_key(a, b) && a.cnt == b.cnt && a.filings == b.filings && a.linesum == b.linesum && a.linecnt == b.linecnt
}

pub fn copy_aggs(src: &Vec<Agg>) -> (r: Vec<Agg>)
    ensures
        r@.len() == src@.len(),
        forall|i: int| 0 <= i < src@.len() ==> agg_same(src@[i], #[trigger] r@[i]),
{
    let mut out: Vec<Agg> = Vec::new();
    let mut i: usize = 0;
    while i < src.len()
        invariant
            i <= src@.len(),
            out@.len() == i,
            forall|j: int| 0 <= j < i ==> agg_same(src@[j], #[trigger] out@[j]),
        decreases src.len() - i,
    {
        let a = &src[i];
        out.push(Agg { stmt: a.stmt.clone(), rfile: opt_arc(&a.rfile), cnt: a.cnt, filings: a.filings, linesum: a.linesum, linecnt: a.linecnt });
        i += 1;
    }
    out
}

/// A copy of the aggregates carries the state's invariant.
pub proof fn lemma_copy_parts(aggs: Seq<Agg>, copy: Seq<Agg>, cells: Index<i128>, base: Seq<PreRow>, items: Items)
    requires
        parts_ok(aggs, cells, base, items),
        copy.len() == aggs.len(),
        forall|i: int| 0 <= i < aggs.len() ==> agg_same(aggs[i], #[trigger] copy[i]),
    ensures
        parts_ok(copy, cells, base, items),
{
    lemma_numbers_same(aggs, copy, base, items);
    lemma_same_keys(aggs, copy, cells, base, items, items);
    assert forall|i: int| 0 <= i < copy.len() implies (#[trigger] copy[i]).filings as int == sum(cells.entries(), filing_w(i)) by {
        assert(copy[i].filings == aggs[i].filings);
    }
}

/// Index versions with the same tree satisfy the same predicates.
pub proof fn lemma_index_congr(aggs: Seq<Agg>, c1: Index<i128>, c2: Index<i128>, base: Seq<PreRow>, items: Items)
    requires
        parts_ok(aggs, c1, base, items),
        c1.tree() == c2.tree(),
    ensures
        parts_ok(aggs, c2, base, items),
{
    assert(c2.wf() == c1.wf());
    assert(c2.entries() == c1.entries());
    assert forall|k: KeyView| c2.at(k) == c1.at(k) by {}
}

} // verus!
