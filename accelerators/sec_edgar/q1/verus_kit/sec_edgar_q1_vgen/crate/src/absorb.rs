// Absorbing one signed pre row into the group aggregates and the (group, filing) cell map.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::steps::*;

verus! {

pub open spec fn unit_items(items: Items) -> bool {
    forall|k: int| 0 <= k < items.len() ==> (#[trigger] items[k]).1 == 1 || items[k].1 == -1
}

pub open spec fn big() -> int { 85070591730234615865843651857942052864 }

pub fn copy_bytes(source: &[u8]) -> (result: Vec<u8>)
    ensures result@ == source@,
{
    vstd::slice::slice_to_vec(source)
}

pub fn add128(a: i128, b: i128) -> (r: Option<i128>)
    ensures
        r matches Some(v) ==> v as int == a as int + b as int,
        r is None ==> !(i128::MIN <= a as int + b as int <= i128::MAX),
{
    a.checked_add(b)
}

pub open spec fn row_stmt(row: PreInput) -> Option<Seq<u8>> { pre_input_view(row).stmt }

fn rfile_matches(a: &Option<Arc<Vec<u8>>>, row: &PreInput) -> (r: bool)
    ensures
        r == ((match *a { Some(x) => Some((*x)@), None => None }) == pre_input_view(*row).rfile),
{
    match a {
        Some(x) => row.rfile__null == 0 && bytes_eq_exec(x.as_slice(), row.rfile.as_slice()),
        None => row.rfile__null != 0,
    }
}

/// The listed group of the row (its stmt not NULL), if any.
fn find_group(aggs: &Vec<Agg>, row: &PreInput) -> (r: Option<usize>)
    requires
        row.stmt__null == 0,
    ensures
        r matches Some(i) ==> i < aggs@.len() && in_grp(pre_input_view(*row), aggs@[i as int].s(), aggs@[i as int].rf()),
        r is None ==> !has_key(aggs@, row.stmt@, pre_input_view(*row).rfile),
{
    let mut i: usize = 0;
    while i < aggs.len()
        invariant
            i <= aggs@.len(),
            row.stmt__null == 0,
            forall|j: int| 0 <= j < i ==> !((#[trigger] aggs@[j]).s() == row.stmt@ && aggs@[j].rf() == pre_input_view(*row).rfile),
        decreases aggs.len() - i,
    {
        let a = &aggs[i];
        if bytes_eq_exec(a.stmt.as_slice(), row.stmt.as_slice()) && rfile_matches(&a.rfile, row) {
            return Some(i);
        }
        i += 1;
    }
    None
}

proof fn lemma_fits(base: Seq<PreRow>, items: Items, s: Seq<u8>, rf: Option<Seq<u8>>, a: Seq<u8>)
    requires
        unit_items(items),
        total_mass(base, items) < big(),
    ensures
        abs_i(vsum(base, items, w_cnt(s, rf))) < big(),
        abs_i(vsum(base, items, w_lcnt(s, rf))) < big(),
        abs_i(vsum(base, items, w_lsum(s, rf))) < big(),
        abs_i(vsum(base, items, w_cell(s, rf, a))) < big(),
{
    assert forall|r: PreRow| #[trigger] abs_i(w_cnt(s, rf)(r)) <= mass(r) by { lemma_weights_small(r, s, rf, a); }
    assert forall|r: PreRow| #[trigger] abs_i(w_lcnt(s, rf)(r)) <= mass(r) by { lemma_weights_small(r, s, rf, a); }
    assert forall|r: PreRow| #[trigger] abs_i(w_lsum(s, rf)(r)) <= mass(r) by { lemma_weights_small(r, s, rf, a); }
    assert forall|r: PreRow| #[trigger] abs_i(w_cell(s, rf, a)(r)) <= mass(r) by { lemma_weights_small(r, s, rf, a); }
    lemma_vsum_abs_le(base, items, w_cnt(s, rf));
    lemma_vsum_abs_le(base, items, w_lcnt(s, rf));
    lemma_vsum_abs_le(base, items, w_lsum(s, rf));
    lemma_vsum_abs_le(base, items, w_cell(s, rf, a));
}

pub fn group_number(g: usize) -> (r: i64)
    ensures r as int == gkey(g as int),
{
    if g <= 9223372036854775807usize { g as i64 } else { (g - 9223372036854775808usize) as i64 + i64::MIN }
}

pub fn opt_arc(a: &Option<Arc<Vec<u8>>>) -> (r: Option<Arc<Vec<u8>>>)
    ensures
        (match r { Some(x) => Some((*x)@), None => None }) == (match *a { Some(x) => Some((*x)@), None => None }),
{
    match a {
        Some(x) => Some(x.clone()),
        None => None,
    }
}


/// The row's group, listed (a zero group is added when the row opens it).
fn locate_group(
    aggs: &mut Vec<Agg>, cells: &Index<i128>, row: &PreInput,
    Ghost(base): Ghost<Seq<PreRow>>, Ghost(items): Ghost<Items>, Ghost(t): Ghost<nat>,
) -> (idx: usize)
    requires
        parts_ok(old(aggs)@, *cells, base, items),
        row.stmt__null == 0,
        old(aggs)@.len() <= t,
        t + 2 < 18446744073709551615,
    ensures
        parts_ok(final(aggs)@, *cells, base, items),
        idx < final(aggs)@.len(),
        in_grp(pre_input_view(*row), final(aggs)@[idx as int].s(), final(aggs)@[idx as int].rf()),
        final(aggs)@.len() <= t + 1,
        forall|i: int| 0 <= i < old(aggs)@.len() ==> final(aggs)@[i] == old(aggs)@[i],
        final(aggs)@.len() > old(aggs)@.len() ==> final(aggs)@.len() == old(aggs)@.len() + 1 && final(aggs)@.last().filings == 0,
{
    match find_group(aggs, row) {
        Some(i) => i,
        None => {
            let z = Agg {
                stmt: Arc::new(copy_bytes(row.stmt.as_slice())),
                rfile: if row.rfile__null != 0 { None } else { Some(Arc::new(copy_bytes(row.rfile.as_slice()))) },
                cnt: 0, filings: 0, linesum: 0, linecnt: 0,
            };
            proof { lemma_push_group(aggs@, *cells, base, items, z); }
            aggs.push(z);
            aggs.len() - 1
        }
    }
}

/// The group's row count, line count and line sum move by the row.
fn bump_numbers(
    aggs: &mut Vec<Agg>, idx: usize, cells: &Index<i128>, row: &PreInput, sign: i64,
    Ghost(base): Ghost<Seq<PreRow>>, Ghost(items): Ghost<Items>,
) -> (ok: bool)
    requires
        parts_ok(old(aggs)@, *cells, base, items),
        idx < old(aggs)@.len(),
        in_grp(pre_input_view(*row), old(aggs)@[idx as int].s(), old(aggs)@[idx as int].rf()),
        sign == 1 || sign == -1,
        unit_items(items),
    ensures
        ok ==> {
            &&& final(aggs)@.len() == old(aggs)@.len()
            &&& forall|j: int| 0 <= j < old(aggs)@.len() ==> final(aggs)@[j].filings == old(aggs)@[j].filings && same_key(old(aggs)@[j], final(aggs)@[j])
            &&& dir_ok(final(aggs)@)
            &&& cover_ok(final(aggs)@, base, items.push((pre_input_view(*row), sign as int)))
            &&& counts_ok(final(aggs)@, base, items.push((pre_input_view(*row), sign as int)))
            &&& cells_ok(final(aggs)@, *cells, base, items)
            &&& filings_ok(final(aggs)@, *cells)
        },
        total_mass(base, items.push((pre_input_view(*row), sign as int))) < big() ==> ok,
{
    let ghost r = pre_input_view(*row);
    let ghost w = sign as int;
    let ghost items2 = items.push((r, w));
    let ghost aggs1 = aggs@;
    let ghost g = idx as int;
    proof {
        assert forall|k: int| 0 <= k < items2.len() implies (#[trigger] items2[k]).1 == 1 || items2[k].1 == -1 by {
            if k < items.len() { assert(items2[k] == items[k]); }
        }
        let a = aggs1[g];
        lemma_vsum_push(base, items, r, w, w_cnt(a.s(), a.rf()));
        lemma_vsum_push(base, items, r, w, w_lcnt(a.s(), a.rf()));
        lemma_vsum_push(base, items, r, w, w_lsum(a.s(), a.rf()));
        if total_mass(base, items2) < big() { lemma_fits(base, items2, a.s(), a.rf(), Seq::empty()); }
    }
    let a = &aggs[idx];
    let sg = sign as i128;
    assert(sg == 1 || sg == -1);
    let cnt2 = match add128(a.cnt, sg) { Some(v) => v, None => { return false; } };
    let line_some = row.line__null == 0;
    let lc2 = if line_some {
        match add128(a.linecnt, sg) { Some(v) => v, None => { return false; } }
    } else { a.linecnt };
    let ls2 = if line_some {
        match add128(a.linesum, sg * (row.line as i128)) { Some(v) => v, None => { return false; } }
    } else { a.linesum };
    let a2 = Agg { stmt: a.stmt.clone(), rfile: opt_arc(&a.rfile), cnt: cnt2, filings: a.filings, linesum: ls2, linecnt: lc2 };
    aggs.set(idx, a2);
    proof { lemma_bump(aggs1, aggs@, *cells, base, items, r, w, g); }
    true
}

/// The row's filing count in its group moves by the row; the group's filing total follows.
fn bump_cell(
    aggs: &mut Vec<Agg>, idx: usize, cells: &mut Index<i128>, row: &PreInput, sign: i64,
    Ghost(base): Ghost<Seq<PreRow>>, Ghost(items): Ghost<Items>, Ghost(t): Ghost<nat>,
) -> (ok: bool)
    requires
        dir_ok(old(aggs)@),
        cells_ok(old(aggs)@, *old(cells), base, items),
        filings_ok(old(aggs)@, *old(cells)),
        idx < old(aggs)@.len(),
        in_grp(pre_input_view(*row), old(aggs)@[idx as int].s(), old(aggs)@[idx as int].rf()),
        row.adsh__null == 0,
        sign == 1 || sign == -1,
        unit_items(items),
        old(aggs)@.len() <= t + 1,
        forall|i: int| 0 <= i < old(aggs)@.len() ==> (#[trigger] old(aggs)@[i]).filings as int <= t,
        t + 2 < 18446744073709551615,
    ensures
        ok ==> {
            &&& final(aggs)@.len() == old(aggs)@.len()
            &&& forall|j: int| 0 <= j < old(aggs)@.len() ==> {
                &&& same_key(old(aggs)@[j], final(aggs)@[j])
                &&& final(aggs)@[j].cnt == old(aggs)@[j].cnt
                &&& final(aggs)@[j].linecnt == old(aggs)@[j].linecnt
                &&& final(aggs)@[j].linesum == old(aggs)@[j].linesum
            }
            &&& cells_ok(final(aggs)@, *final(cells), base, items.push((pre_input_view(*row), sign as int)))
            &&& filings_ok(final(aggs)@, *final(cells))
            &&& forall|i: int| 0 <= i < final(aggs)@.len() ==> (#[trigger] final(aggs)@[i]).filings as int <= t + 1
        },
        total_mass(base, items.push((pre_input_view(*row), sign as int))) < big() ==> ok,
{
    let ghost r = pre_input_view(*row);
    let ghost w = sign as int;
    let ghost items2 = items.push((r, w));
    let ghost aggs2 = aggs@;
    let ghost g = idx as int;
    let sg = sign as i128;
    assert(sg == 1 || sg == -1);
    let gid = group_number(idx);
    let key = Key::composite(gid, copy_bytes(row.adsh.as_slice()));
    let ghost kv = key.view();
    let ghost adsh = row.adsh@;
    let ghost before = *cells;
    let c0: i128 = match cells.get(&key) { Some(v) => *v, None => 0 };
    proof {
        assert(kv == cell_key(g, adsh));
        assert(c0 != 0 ==> before.at(kv) is Some);
        assert forall|k: int| 0 <= k < items2.len() implies (#[trigger] items2[k]).1 == 1 || items2[k].1 == -1 by {
            if k < items.len() { assert(items2[k] == items[k]); }
        }
        lemma_vsum_push(base, items, r, w, w_cell(aggs2[g].s(), aggs2[g].rf(), adsh));
        if total_mass(base, items2) < big() { lemma_fits(base, items2, aggs2[g].s(), aggs2[g].rf(), adsh); }
    }
    let c1 = match add128(c0, sg) { Some(v) => v, None => { return false; } };
    if c1 != 0 {
        cells.put(key, Arc::new(c1));
    } else {
        cells.remove(&key);
    }
    proof {
        lemma_cells_step(aggs2, before, *cells, base, items, r, w, g, adsh, c1 as int);
        if c0 != 0 { lemma_filings_pos(aggs2, before, g, adsh); }
    }
    let present0 = c0 != 0;
    let present1 = c1 != 0;
    let a = &aggs[idx];
    let f3 = if present1 && !present0 { a.filings + 1 } else if present0 && !present1 { a.filings - 1 } else { a.filings };
    let a3 = Agg { stmt: a.stmt.clone(), rfile: opt_arc(&a.rfile), cnt: a.cnt, filings: f3, linesum: a.linesum, linecnt: a.linecnt };
    aggs.set(idx, a3);
    proof {
        let nv = if c1 != 0 { Some(c1) } else { None };
        lemma_filings_step(aggs2, aggs@, before, *cells, g, adsh, nv);
    }
    true
}

/// Absorb one signed row.  ok is false only when a counter would leave i128 (never for
/// databases within the SQL types and a bounded history).
pub fn absorb(
    aggs: &mut Vec<Agg>, cells: &mut Index<i128>, row: &PreInput, sign: i64,
    Ghost(base): Ghost<Seq<PreRow>>, Ghost(items): Ghost<Items>, Ghost(t): Ghost<nat>,
) -> (ok: bool)
    requires
        parts_ok(old(aggs)@, *old(cells), base, items),
        sign == 1 || sign == -1,
        unit_items(items),
        old(aggs)@.len() <= t,
        forall|i: int| 0 <= i < old(aggs)@.len() ==> (#[trigger] old(aggs)@[i]).filings as int <= t,
        t + 2 < 18446744073709551615,
    ensures
        ok ==> parts_ok(final(aggs)@, *final(cells), base, items.push((pre_input_view(*row), sign as int))),
        ok ==> final(aggs)@.len() <= t + 1,
        ok ==> forall|i: int| 0 <= i < final(aggs)@.len() ==> (#[trigger] final(aggs)@[i]).filings as int <= t + 1,
        total_mass(base, items.push((pre_input_view(*row), sign as int))) < big() ==> ok,
{
    let ghost r = pre_input_view(*row);
    let ghost w = sign as int;
    let ghost items2 = items.push((r, w));
    if row.stmt__null != 0 {
        proof {
            assert forall|s: Seq<u8>, rf: Option<Seq<u8>>| !in_grp(r, s, rf) by {}
            lemma_ignore_item(aggs@, *cells, base, items, r, w);
        }
        return true;
    }
    let idx = locate_group(aggs, cells, row, Ghost(base), Ghost(items), Ghost(t));
    if !bump_numbers(aggs, idx, cells, row, sign, Ghost(base), Ghost(items)) {
        return false;
    }
    if row.adsh__null != 0 {
        proof { lemma_cells_ignore(aggs@, *cells, base, items, r, w); }
        return true;
    }
    let ghost aggs2 = aggs@;
    if !bump_cell(aggs, idx, cells, row, sign, Ghost(base), Ghost(items), Ghost(t)) {
        return false;
    }
    proof {
        lemma_numbers_same(aggs2, aggs@, base, items2);
    }
    true
}

} // verus!
