// The kernel's state shape and its meaning: per-group aggregates plus a persistent map of
// per-(group, filing) row counts, all stated against a virtual bag base + signed items.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

pub struct Agg {
    pub stmt: Arc<Vec<u8>>,
    pub rfile: Option<Arc<Vec<u8>>>,
    pub cnt: i128,
    pub filings: i128,
    pub linesum: i128,
    pub linecnt: i128,
}

impl Agg {
    pub open spec fn s(self) -> Seq<u8> { (*self.stmt)@ }

    pub open spec fn rf(self) -> Option<Seq<u8>> {
        match self.rfile { Some(a) => Some((*a)@), None => None }
    }
}

pub type Items = Seq<(PreRow, int)>;

pub open spec fn in_grp(r: PreRow, s: Seq<u8>, rf: Option<Seq<u8>>) -> bool {
    r.stmt == Some(s) && r.rfile == rf
}

pub open spec fn w_cnt(s: Seq<u8>, rf: Option<Seq<u8>>) -> spec_fn(PreRow) -> int {
    |r: PreRow| ind(in_grp(r, s, rf))
}

pub open spec fn w_lcnt(s: Seq<u8>, rf: Option<Seq<u8>>) -> spec_fn(PreRow) -> int {
    |r: PreRow| ind(in_grp(r, s, rf) && r.line is Some)
}

pub open spec fn w_lsum(s: Seq<u8>, rf: Option<Seq<u8>>) -> spec_fn(PreRow) -> int {
    |r: PreRow| if in_grp(r, s, rf) && r.line is Some { r.line->0 } else { 0 }
}

pub open spec fn w_cell(s: Seq<u8>, rf: Option<Seq<u8>>, a: Seq<u8>) -> spec_fn(PreRow) -> int {
    |r: PreRow| ind(in_grp(r, s, rf) && r.adsh == Some(a))
}

pub open spec fn dsum(items: Items, f: spec_fn(PreRow) -> int) -> int {
    sum(items, |d: (PreRow, int)| d.1 * f(d.0))
}

/// The weight of f over base plus the signed items.
pub open spec fn vsum(base: Seq<PreRow>, items: Items, f: spec_fn(PreRow) -> int) -> int {
    sum(base, f) + dsum(items, f)
}

pub proof fn lemma_vsum_push(base: Seq<PreRow>, items: Items, r: PreRow, w: int, f: spec_fn(PreRow) -> int)
    ensures
        vsum(base, items.push((r, w)), f) == vsum(base, items, f) + w * f(r),
{
    lemma_sum_push(items, (r, w), |d: (PreRow, int)| d.1 * f(d.0));
}

/// Rows outside the group weigh nothing in any of the group's weights.
pub proof fn lemma_vsum_absent(base: Seq<PreRow>, items: Items, s: Seq<u8>, rf: Option<Seq<u8>>, f: spec_fn(PreRow) -> int)
    requires
        forall|k: int| 0 <= k < base.len() ==> !in_grp(#[trigger] base[k], s, rf),
        forall|k: int| 0 <= k < items.len() ==> !in_grp(#[trigger] items[k].0, s, rf),
        forall|r: PreRow| !in_grp(r, s, rf) ==> #[trigger] f(r) == 0,
    ensures
        vsum(base, items, f) == 0,
{
    lemma_sum_zero(base, f);
    let g = |d: (PreRow, int)| d.1 * f(d.0);
    assert forall|k: int| 0 <= k < items.len() implies g(#[trigger] items[k]) == 0 by {
        assert(f(items[k].0) == 0);
    }
    lemma_sum_zero(items, g);
}

// ── Magnitudes: a row weighs at most 1 + |line| in every weight above ──

pub open spec fn mass(r: PreRow) -> int {
    1 + match r.line { Some(l) => if l < 0 { -l } else { l }, None => 0 }
}

pub open spec fn imass(d: (PreRow, int)) -> int { mass(d.0) }

pub open spec fn total_mass(base: Seq<PreRow>, items: Items) -> int {
    sum(base, |r: PreRow| mass(r)) + sum(items, |d: (PreRow, int)| imass(d))
}

pub open spec fn abs_i(x: int) -> int { if x < 0 { -x } else { x } }

pub proof fn lemma_sum_abs_le(s: Seq<PreRow>, f: spec_fn(PreRow) -> int)
    requires
        forall|r: PreRow| #[trigger] abs_i(f(r)) <= mass(r),
    ensures
        abs_i(sum(s, f)) <= sum(s, |r: PreRow| mass(r)),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_sum_abs_le(s.drop_last(), f);
        assert(abs_i(f(s.last())) <= mass(s.last()));
    }
}

pub proof fn lemma_dsum_abs_le(items: Items, f: spec_fn(PreRow) -> int)
    requires
        forall|r: PreRow| #[trigger] abs_i(f(r)) <= mass(r),
        forall|k: int| 0 <= k < items.len() ==> (#[trigger] items[k]).1 == 1 || items[k].1 == -1,
    ensures
        abs_i(dsum(items, f)) <= sum(items, |d: (PreRow, int)| imass(d)),
    decreases items.len(),
{
    if items.len() > 0 {
        let t = items.drop_last();
        assert forall|k: int| 0 <= k < t.len() implies (#[trigger] t[k]).1 == 1 || t[k].1 == -1 by {
            assert(t[k] == items[k]);
        }
        lemma_dsum_abs_le(t, f);
        assert(items.last() == items[items.len() - 1]);
        assert(abs_i(f(items.last().0)) <= mass(items.last().0));
    }
}

pub proof fn lemma_vsum_abs_le(base: Seq<PreRow>, items: Items, f: spec_fn(PreRow) -> int)
    requires
        forall|r: PreRow| #[trigger] abs_i(f(r)) <= mass(r),
        forall|k: int| 0 <= k < items.len() ==> (#[trigger] items[k]).1 == 1 || items[k].1 == -1,
    ensures
        abs_i(vsum(base, items, f)) <= total_mass(base, items),
{
    lemma_sum_abs_le(base, f);
    lemma_dsum_abs_le(items, f);
}

pub proof fn lemma_mass_prefix(items: Items, k: int)
    requires
        0 <= k <= items.len(),
    ensures
        sum(items.take(k), |d: (PreRow, int)| imass(d)) <= sum(items, |d: (PreRow, int)| imass(d)),
{
    let g = |d: (PreRow, int)| imass(d);
    lemma_sum_concat(items.take(k), items.skip(k), g);
    assert(items.take(k) + items.skip(k) =~= items);
    assert forall|i: int| 0 <= i < items.skip(k).len() implies g(#[trigger] items.skip(k)[i]) >= 0 by {}
    lemma_sum_nonneg(items.skip(k), g);
}

pub proof fn lemma_weights_small(r: PreRow, s: Seq<u8>, rf: Option<Seq<u8>>, a: Seq<u8>)
    ensures
        abs_i(w_cnt(s, rf)(r)) <= mass(r),
        abs_i(w_lcnt(s, rf)(r)) <= mass(r),
        abs_i(w_lsum(s, rf)(r)) <= mass(r),
        abs_i(w_cell(s, rf, a)(r)) <= mass(r),
{
}

// ── The state invariant pieces ──

pub open spec fn same_key(a: Agg, b: Agg) -> bool { a.s() == b.s() && a.rf() == b.rf() }

pub open spec fn has_key(aggs: Seq<Agg>, s: Seq<u8>, rf: Option<Seq<u8>>) -> bool {
    exists|i: int| 0 <= i < aggs.len() && (#[trigger] aggs[i]).s() == s && aggs[i].rf() == rf
}

/// Groups are listed once.
pub open spec fn dir_ok(aggs: Seq<Agg>) -> bool {
    forall|i: int, j: int| 0 <= i < j < aggs.len() ==> !same_key(#[trigger] aggs[i], #[trigger] aggs[j])
}

/// A group without a listing has no rows, in the base or in any item.
pub open spec fn cover_ok(aggs: Seq<Agg>, base: Seq<PreRow>, items: Items) -> bool {
    forall|s: Seq<u8>, rf: Option<Seq<u8>>| !(#[trigger] has_key(aggs, s, rf)) ==> {
        &&& forall|k: int| 0 <= k < base.len() ==> !in_grp(#[trigger] base[k], s, rf)
        &&& forall|k: int| 0 <= k < items.len() ==> !in_grp(#[trigger] items[k].0, s, rf)
    }
}

pub open spec fn counts_ok(aggs: Seq<Agg>, base: Seq<PreRow>, items: Items) -> bool {
    forall|i: int| 0 <= i < aggs.len() ==> {
        let a = #[trigger] aggs[i];
        &&& a.cnt as int == vsum(base, items, w_cnt(a.s(), a.rf()))
        &&& a.linecnt as int == vsum(base, items, w_lcnt(a.s(), a.rf()))
        &&& a.linesum as int == vsum(base, items, w_lsum(a.s(), a.rf()))
    }
}

/// The number the cell map uses for group g.
pub open spec fn gkey(g: int) -> int {
    if g < 9223372036854775808 { g } else { g - 18446744073709551616 }
}

pub open spec fn cell_ok(v: Option<i128>, c: int) -> bool {
    match v { Some(x) => x as int == c && c != 0, None => c == 0 }
}

/// cells maps (group, filing) to the nonzero number of rows of that filing in that group.
pub open spec fn cells_ok(aggs: Seq<Agg>, cells: Index<i128>, base: Seq<PreRow>, items: Items) -> bool {
    &&& cells.wf()
    &&& forall|k: KeyView| #[trigger] cells.at(k) is Some ==> exists|g: int| 0 <= g < aggs.len() && gkey(g) == k.0
    &&& forall|g: int, a: Seq<u8>| 0 <= g < aggs.len() ==>
        cell_ok(#[trigger] cells.at((gkey(g), a)), vsum(base, items, w_cell(aggs[g].s(), aggs[g].rf(), a)))
}

pub open spec fn filing_w(g: int) -> spec_fn((KeyView, i128)) -> int {
    |e: (KeyView, i128)| if e.0.0 == gkey(g) { 1int } else { 0 }
}

pub open spec fn filings_ok(aggs: Seq<Agg>, cells: Index<i128>) -> bool {
    forall|i: int| 0 <= i < aggs.len() ==> (#[trigger] aggs[i]).filings as int == sum(cells.entries(), filing_w(i))
}

pub open spec fn parts_ok(aggs: Seq<Agg>, cells: Index<i128>, base: Seq<PreRow>, items: Items) -> bool {
    &&& dir_ok(aggs)
    &&& cover_ok(aggs, base, items)
    &&& counts_ok(aggs, base, items)
    &&& cells_ok(aggs, cells, base, items)
    &&& filings_ok(aggs, cells)
}

} // verus!
