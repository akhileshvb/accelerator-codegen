// One signed item entering the virtual bag: what each piece of the state must do.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::model::*;

verus! {

pub open spec fn zero_agg(z: Agg, s: Seq<u8>, rf: Option<Seq<u8>>) -> bool {
    z.s() == s && z.rf() == rf && z.cnt == 0 && z.filings == 0 && z.linesum == 0 && z.linecnt == 0
}

/// Listing an unlisted group with zero aggregates keeps the invariant.
pub proof fn lemma_push_group(aggs: Seq<Agg>, cells: Index<i128>, base: Seq<PreRow>, items: Items, z: Agg)
    requires
        parts_ok(aggs, cells, base, items),
        aggs.len() < 18446744073709551615,
        !has_key(aggs, z.s(), z.rf()),
        z.cnt == 0 && z.filings == 0 && z.linesum == 0 && z.linecnt == 0,
    ensures
        parts_ok(aggs.push(z), cells, base, items),
{
    let aggs2 = aggs.push(z);
    let n = aggs.len() as int;
    let (s, rf) = (z.s(), z.rf());
    assert(aggs2[n] == z);
    assert forall|i: int, j: int| 0 <= i < j < aggs2.len() implies !same_key(#[trigger] aggs2[i], #[trigger] aggs2[j]) by {
        if j == n {
            assert(aggs[i].s() == s && aggs[i].rf() == rf ==> has_key(aggs, s, rf));
        } else {
            assert(aggs2[i] == aggs[i] && aggs2[j] == aggs[j]);
        }
    }
    assert forall|s2: Seq<u8>, rf2: Option<Seq<u8>>| !(#[trigger] has_key(aggs2, s2, rf2)) implies {
        &&& forall|k: int| 0 <= k < base.len() ==> !in_grp(#[trigger] base[k], s2, rf2)
        &&& forall|k: int| 0 <= k < items.len() ==> !in_grp(#[trigger] items[k].0, s2, rf2)
    } by {
        if has_key(aggs, s2, rf2) {
            let i = choose|i: int| 0 <= i < aggs.len() && (#[trigger] aggs[i]).s() == s2 && aggs[i].rf() == rf2;
            assert(aggs2[i] == aggs[i]);
        }
    }
    assert forall|s2: Seq<u8>, rf2: Option<Seq<u8>>| !(#[trigger] has_key(aggs, s2, rf2)) implies true by {}
    lemma_vsum_absent(base, items, s, rf, w_cnt(s, rf));
    lemma_vsum_absent(base, items, s, rf, w_lcnt(s, rf));
    lemma_vsum_absent(base, items, s, rf, w_lsum(s, rf));
    assert(counts_ok(aggs2, base, items)) by {
        assert forall|i: int| 0 <= i < aggs2.len() implies {
            let a = #[trigger] aggs2[i];
            &&& a.cnt as int == vsum(base, items, w_cnt(a.s(), a.rf()))
            &&& a.linecnt as int == vsum(base, items, w_lcnt(a.s(), a.rf()))
            &&& a.linesum as int == vsum(base, items, w_lsum(a.s(), a.rf()))
        } by {
            if i < n { assert(aggs2[i] == aggs[i]); }
        }
    }
    cells.model();
    assert forall|k: KeyView| #[trigger] cells.at(k) is Some implies exists|g: int| 0 <= g < aggs2.len() && gkey(g) == k.0 by {
        let g = choose|g: int| 0 <= g < aggs.len() && gkey(g) == k.0;
        assert(aggs2[g] == aggs[g]);
    }
    assert forall|g: int, a: Seq<u8>| 0 <= g < aggs2.len() implies
        cell_ok(#[trigger] cells.at((gkey(g), a)), vsum(base, items, w_cell(aggs2[g].s(), aggs2[g].rf(), a))) by {
        if g < n {
            assert(aggs2[g] == aggs[g]);
        } else {
            lemma_vsum_absent(base, items, s, rf, w_cell(s, rf, a));
            if cells.at((gkey(g), a)) is Some {
                let g0 = choose|g0: int| 0 <= g0 < aggs.len() && gkey(g0) == gkey(g);
                assert(false);
            }
        }
    }
    assert forall|i: int| 0 <= i < aggs2.len() implies (#[trigger] aggs2[i]).filings as int == sum(cells.entries(), filing_w(i)) by {
        if i < n {
            assert(aggs2[i] == aggs[i]);
        } else {
            let w = filing_w(i);
            assert forall|p: int| 0 <= p < cells.entries().len() implies w(#[trigger] cells.entries()[p]) == 0 by {
                let e = cells.entries()[p];
                assert(cells.at(e.0) == Some(e.1));
                let g0 = choose|g0: int| 0 <= g0 < aggs.len() && gkey(g0) == e.0.0;
            }
            lemma_sum_zero(cells.entries(), w);
        }
    }
}

/// A key-preserving update of the aggregates keeps the key-only parts of the invariant.
pub proof fn lemma_same_keys(aggs: Seq<Agg>, aggs2: Seq<Agg>, cells: Index<i128>, base: Seq<PreRow>, items: Items, items2: Items)
    requires
        aggs2.len() == aggs.len(),
        forall|j: int| 0 <= j < aggs.len() ==> same_key(#[trigger] aggs[j], aggs2[j]),
        dir_ok(aggs),
        cells_ok(aggs, cells, base, items),
    ensures
        dir_ok(aggs2),
        cells_ok(aggs2, cells, base, items),
        forall|s: Seq<u8>, rf: Option<Seq<u8>>| has_key(aggs2, s, rf) == has_key(aggs, s, rf),
{
    assert forall|s: Seq<u8>, rf: Option<Seq<u8>>| has_key(aggs2, s, rf) == has_key(aggs, s, rf) by {
        if has_key(aggs2, s, rf) {
            let i = choose|i: int| 0 <= i < aggs2.len() && (#[trigger] aggs2[i]).s() == s && aggs2[i].rf() == rf;
            assert(aggs[i].s() == s && aggs[i].rf() == rf);
        }
        if has_key(aggs, s, rf) {
            let i = choose|i: int| 0 <= i < aggs.len() && (#[trigger] aggs[i]).s() == s && aggs[i].rf() == rf;
            assert(aggs2[i].s() == s && aggs2[i].rf() == rf);
        }
    }
    assert forall|i: int, j: int| 0 <= i < j < aggs2.len() implies !same_key(#[trigger] aggs2[i], #[trigger] aggs2[j]) by {
        assert(same_key(aggs[i], aggs2[i]) && same_key(aggs[j], aggs2[j]));
    }
}

/// At most one listed group holds a row.
pub proof fn lemma_one_group(aggs: Seq<Agg>, i: int, j: int, r: PreRow)
    requires
        dir_ok(aggs),
        0 <= i < aggs.len(),
        0 <= j < aggs.len(),
        i != j,
        in_grp(r, aggs[i].s(), aggs[i].rf()),
    ensures
        !in_grp(r, aggs[j].s(), aggs[j].rf()),
{
    if i < j { assert(!same_key(aggs[i], aggs[j])); } else { assert(!same_key(aggs[j], aggs[i])); }
}

/// The aggregates after item (r, w): each group moves by w times r's weight in it.
pub proof fn lemma_counts_step(aggs: Seq<Agg>, aggs2: Seq<Agg>, base: Seq<PreRow>, items: Items, r: PreRow, w: int)
    requires
        counts_ok(aggs, base, items),
        aggs2.len() == aggs.len(),
        forall|j: int| 0 <= j < aggs.len() ==> {
            &&& same_key(#[trigger] aggs[j], aggs2[j])
            &&& aggs2[j].cnt as int == aggs[j].cnt as int + w * w_cnt(aggs[j].s(), aggs[j].rf())(r)
            &&& aggs2[j].linecnt as int == aggs[j].linecnt as int + w * w_lcnt(aggs[j].s(), aggs[j].rf())(r)
            &&& aggs2[j].linesum as int == aggs[j].linesum as int + w * w_lsum(aggs[j].s(), aggs[j].rf())(r)
        },
    ensures
        counts_ok(aggs2, base, items.push((r, w))),
{
    assert forall|i: int| 0 <= i < aggs2.len() implies {
        let a = #[trigger] aggs2[i];
        &&& a.cnt as int == vsum(base, items.push((r, w)), w_cnt(a.s(), a.rf()))
        &&& a.linecnt as int == vsum(base, items.push((r, w)), w_lcnt(a.s(), a.rf()))
        &&& a.linesum as int == vsum(base, items.push((r, w)), w_lsum(a.s(), a.rf()))
    } by {
        let a = aggs[i];
        assert(aggs2[i].s() == a.s() && aggs2[i].rf() == a.rf());
        lemma_vsum_push(base, items, r, w, w_cnt(a.s(), a.rf()));
        lemma_vsum_push(base, items, r, w, w_lcnt(a.s(), a.rf()));
        lemma_vsum_push(base, items, r, w, w_lsum(a.s(), a.rf()));
    }
}

/// An item whose group is unlisted can only enter after its group is listed.
pub proof fn lemma_cover_step(aggs: Seq<Agg>, base: Seq<PreRow>, items: Items, r: PreRow, w: int)
    requires
        cover_ok(aggs, base, items),
        forall|s: Seq<u8>, rf: Option<Seq<u8>>| !(#[trigger] has_key(aggs, s, rf)) ==> !in_grp(r, s, rf),
    ensures
        cover_ok(aggs, base, items.push((r, w))),
{
    assert forall|s: Seq<u8>, rf: Option<Seq<u8>>| !(#[trigger] has_key(aggs, s, rf)) implies {
        &&& forall|k: int| 0 <= k < base.len() ==> !in_grp(#[trigger] base[k], s, rf)
        &&& forall|k: int| 0 <= k < items.push((r, w)).len() ==> !in_grp(#[trigger] items.push((r, w))[k].0, s, rf)
    } by {
        assert forall|k: int| 0 <= k < items.push((r, w)).len() implies !in_grp(#[trigger] items.push((r, w))[k].0, s, rf) by {
            if k < items.len() { assert(items.push((r, w))[k] == items[k]); }
        }
    }
}

/// The cell map after item (r, w) whose cell is `key`: the new count is the old one plus w.
pub proof fn lemma_cells_step(
    aggs: Seq<Agg>, before: Index<i128>, after: Index<i128>, base: Seq<PreRow>, items: Items,
    r: PreRow, w: int, i: int, a: Seq<u8>, c1: int,
)
    requires
        cells_ok(aggs, before, base, items),
        dir_ok(aggs),
        0 <= i < aggs.len(),
        aggs.len() < 18446744073709551615,
        in_grp(r, aggs[i].s(), aggs[i].rf()),
        r.adsh == Some(a),
        after.wf(),
        forall|q: KeyView| q != (gkey(i), a) ==> #[trigger] after.at(q) == before.at(q),
        cell_ok(after.at((gkey(i), a)), c1),
        c1 == vsum(base, items, w_cell(aggs[i].s(), aggs[i].rf(), a)) + w,
    ensures
        cells_ok(aggs, after, base, items.push((r, w))),
{
    let items2 = items.push((r, w));
    assert forall|k: KeyView| #[trigger] after.at(k) is Some implies exists|g: int| 0 <= g < aggs.len() && gkey(g) == k.0 by {
        if k == (gkey(i), a) {
            assert(gkey(i) == k.0);
        } else {
            assert(before.at(k) is Some);
        }
    }
    assert forall|g: int, b: Seq<u8>| 0 <= g < aggs.len() implies
        cell_ok(#[trigger] after.at((gkey(g), b)), vsum(base, items2, w_cell(aggs[g].s(), aggs[g].rf(), b))) by {
        lemma_vsum_push(base, items, r, w, w_cell(aggs[g].s(), aggs[g].rf(), b));
        if g == i && b == a {
        } else {
            if g != i { lemma_one_group(aggs, i, g, r); }
            assert((gkey(g), b) != (gkey(i), a));
        }
    }
}

/// An item that belongs to no group (its stmt is NULL) changes nothing.
pub proof fn lemma_ignore_item(aggs: Seq<Agg>, cells: Index<i128>, base: Seq<PreRow>, items: Items, r: PreRow, w: int)
    requires
        parts_ok(aggs, cells, base, items),
        forall|s: Seq<u8>, rf: Option<Seq<u8>>| !in_grp(r, s, rf),
    ensures
        parts_ok(aggs, cells, base, items.push((r, w))),
{
    lemma_counts_step(aggs, aggs, base, items, r, w);
    lemma_cover_step(aggs, base, items, r, w);
    assert forall|g: int, a: Seq<u8>| 0 <= g < aggs.len() implies
        cell_ok(#[trigger] cells.at((gkey(g), a)), vsum(base, items.push((r, w)), w_cell(aggs[g].s(), aggs[g].rf(), a))) by {
        lemma_vsum_push(base, items, r, w, w_cell(aggs[g].s(), aggs[g].rf(), a));
    }
}

/// An item with no filing leaves the cell map as it is.
pub proof fn lemma_cells_ignore(aggs: Seq<Agg>, cells: Index<i128>, base: Seq<PreRow>, items: Items, r: PreRow, w: int)
    requires
        cells_ok(aggs, cells, base, items),
        r.adsh is None,
    ensures
        cells_ok(aggs, cells, base, items.push((r, w))),
{
    assert forall|g: int, a: Seq<u8>| 0 <= g < aggs.len() implies
        cell_ok(#[trigger] cells.at((gkey(g), a)), vsum(base, items.push((r, w)), w_cell(aggs[g].s(), aggs[g].rf(), a))) by {
        lemma_vsum_push(base, items, r, w, w_cell(aggs[g].s(), aggs[g].rf(), a));
    }
}

/// The matching group moves by the item; the rest of the state keeps its old items.
pub proof fn lemma_bump(
    aggs: Seq<Agg>, aggs2: Seq<Agg>, cells: Index<i128>, base: Seq<PreRow>, items: Items, r: PreRow, w: int, idx: int,
)
    requires
        parts_ok(aggs, cells, base, items),
        0 <= idx < aggs.len(),
        in_grp(r, aggs[idx].s(), aggs[idx].rf()),
        aggs2.len() == aggs.len(),
        forall|j: int| 0 <= j < aggs.len() && j != idx ==> aggs2[j] == aggs[j],
        same_key(aggs[idx], aggs2[idx]),
        aggs2[idx].filings == aggs[idx].filings,
        aggs2[idx].cnt as int == aggs[idx].cnt as int + w,
        aggs2[idx].linecnt as int == aggs[idx].linecnt as int + w * ind(r.line is Some),
        aggs2[idx].linesum as int == aggs[idx].linesum as int + w * (if r.line is Some { r.line->0 } else { 0 }),
    ensures
        dir_ok(aggs2),
        cover_ok(aggs2, base, items.push((r, w))),
        counts_ok(aggs2, base, items.push((r, w))),
        cells_ok(aggs2, cells, base, items),
        filings_ok(aggs2, cells),
{
    assert forall|j: int| 0 <= j < aggs.len() implies {
        &&& same_key(#[trigger] aggs[j], aggs2[j])
        &&& aggs2[j].cnt as int == aggs[j].cnt as int + w * w_cnt(aggs[j].s(), aggs[j].rf())(r)
        &&& aggs2[j].linecnt as int == aggs[j].linecnt as int + w * w_lcnt(aggs[j].s(), aggs[j].rf())(r)
        &&& aggs2[j].linesum as int == aggs[j].linesum as int + w * w_lsum(aggs[j].s(), aggs[j].rf())(r)
    } by {
        if j != idx { lemma_one_group(aggs, idx, j, r); }
    }
    lemma_counts_step(aggs, aggs2, base, items, r, w);
    lemma_same_keys(aggs, aggs2, cells, base, items, items.push((r, w)));
    assert forall|s: Seq<u8>, rf: Option<Seq<u8>>| !(#[trigger] has_key(aggs, s, rf)) implies !in_grp(r, s, rf) by {
        if in_grp(r, s, rf) {
            assert(aggs[idx].s() == s && aggs[idx].rf() == rf);
        }
    }
    lemma_cover_step(aggs, base, items, r, w);
    assert forall|s: Seq<u8>, rf: Option<Seq<u8>>| has_key(aggs2, s, rf) == has_key(aggs, s, rf) by {}
    assert forall|s: Seq<u8>, rf: Option<Seq<u8>>| !(#[trigger] has_key(aggs2, s, rf)) implies {
        &&& forall|k: int| 0 <= k < base.len() ==> !in_grp(#[trigger] base[k], s, rf)
        &&& forall|k: int| 0 <= k < items.push((r, w)).len() ==> !in_grp(#[trigger] items.push((r, w))[k].0, s, rf)
    } by {
        assert(!has_key(aggs, s, rf));
    }
    assert forall|i: int| 0 <= i < aggs2.len() implies (#[trigger] aggs2[i]).filings as int == sum(cells.entries(), filing_w(i)) by {
        if i == idx { assert(aggs2[i].filings == aggs[i].filings); } else { assert(aggs2[i] == aggs[i]); }
    }
}

pub open spec fn cell_key(idx: int, a: Seq<u8>) -> KeyView { (gkey(idx), a) }

/// A listed filing implies its group lists at least one filing.
pub proof fn lemma_filings_pos(aggs: Seq<Agg>, cells: Index<i128>, idx: int, a: Seq<u8>)
    requires
        filings_ok(aggs, cells),
        cells.wf(),
        0 <= idx < aggs.len(),
        cells.at(cell_key(idx, a)) is Some,
    ensures
        aggs[idx].filings as int >= 1,
{
    cells.model();
    let e = cells.entries();
    let p = choose|p: int| #![auto] 0 <= p < e.len() && e[p].0 == cell_key(idx, a);
    let w = filing_w(idx);
    assert forall|q: int| 0 <= q < e.len() implies w(#[trigger] e[q]) >= 0 by {}
    lemma_sum_ge_term(e, w, p);
}

/// The filing counts after one cell is put (Some) or removed (None).
pub proof fn lemma_filings_step(
    aggs: Seq<Agg>, aggs3: Seq<Agg>, before: Index<i128>, after: Index<i128>, idx: int, a: Seq<u8>, nv: Option<i128>,
)
    requires
        filings_ok(aggs, before),
        before.wf(),
        after.wf(),
        0 <= idx < aggs.len(),
        aggs.len() < 18446744073709551615,
        aggs3.len() == aggs.len(),
        forall|j: int| 0 <= j < aggs.len() && j != idx ==> aggs3[j] == aggs[j],
        aggs3[idx].filings as int == aggs[idx].filings as int
            + (if nv is Some { 1int } else { 0 }) - (if before.at(cell_key(idx, a)) is Some { 1int } else { 0 }),
        after.view() == match nv {
            Some(v) => before.view().insert(cell_key(idx, a), v),
            None => before.view().remove(cell_key(idx, a)),
        },
    ensures
        filings_ok(aggs3, after),
{
    let k = cell_key(idx, a);
    assert forall|j: int| 0 <= j < aggs3.len() implies (#[trigger] aggs3[j]).filings as int == sum(after.entries(), filing_w(j)) by {
        let w = filing_w(j);
        match nv {
            Some(v) => { verus_prooflib::ordered::lemma_sum_put(&before, &after, k, v, w); }
            None => { verus_prooflib::ordered::lemma_sum_remove(&before, &after, k, w); }
        }
        if j != idx {
            assert(aggs3[j] == aggs[j]);
            assert(gkey(j) != gkey(idx));
        }
    }
}

/// Aggregates that differ only in their filing counts have the same keys and counts.
pub proof fn lemma_numbers_same(aggs: Seq<Agg>, aggs3: Seq<Agg>, base: Seq<PreRow>, items: Items)
    requires
        aggs3.len() == aggs.len(),
        forall|j: int| 0 <= j < aggs.len() ==> {
            &&& same_key(#[trigger] aggs[j], aggs3[j])
            &&& aggs3[j].cnt == aggs[j].cnt
            &&& aggs3[j].linecnt == aggs[j].linecnt
            &&& aggs3[j].linesum == aggs[j].linesum
        },
        dir_ok(aggs),
        cover_ok(aggs, base, items),
        counts_ok(aggs, base, items),
    ensures
        dir_ok(aggs3),
        cover_ok(aggs3, base, items),
        counts_ok(aggs3, base, items),
{
    assert forall|s: Seq<u8>, rf: Option<Seq<u8>>| has_key(aggs3, s, rf) == has_key(aggs, s, rf) by {
        if has_key(aggs3, s, rf) {
            let i = choose|i: int| 0 <= i < aggs3.len() && (#[trigger] aggs3[i]).s() == s && aggs3[i].rf() == rf;
            assert(aggs[i].s() == s && aggs[i].rf() == rf);
        }
        if has_key(aggs, s, rf) {
            let i = choose|i: int| 0 <= i < aggs.len() && (#[trigger] aggs[i]).s() == s && aggs[i].rf() == rf;
            assert(aggs3[i].s() == s && aggs3[i].rf() == rf);
        }
    }
    assert forall|i: int, j: int| 0 <= i < j < aggs3.len() implies !same_key(#[trigger] aggs3[i], #[trigger] aggs3[j]) by {
        assert(same_key(aggs[i], aggs3[i]) && same_key(aggs[j], aggs3[j]));
    }
    assert forall|s: Seq<u8>, rf: Option<Seq<u8>>| !(#[trigger] has_key(aggs3, s, rf)) implies {
        &&& forall|k: int| 0 <= k < base.len() ==> !in_grp(#[trigger] base[k], s, rf)
        &&& forall|k: int| 0 <= k < items.len() ==> !in_grp(#[trigger] items[k].0, s, rf)
    } by {
        assert(!has_key(aggs, s, rf));
    }
    assert forall|i: int| 0 <= i < aggs3.len() implies {
        let a = #[trigger] aggs3[i];
        &&& a.cnt as int == vsum(base, items, w_cnt(a.s(), a.rf()))
        &&& a.linecnt as int == vsum(base, items, w_lcnt(a.s(), a.rf()))
        &&& a.linesum as int == vsum(base, items, w_lsum(a.s(), a.rf()))
    } by {
        assert(aggs3[i].s() == aggs[i].s() && aggs3[i].rf() == aggs[i].rf());
    }
}

} // verus!
