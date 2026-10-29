// From the loops' virtual bag (base + signed items) to the database the obligations talk about.
use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::steps::*;
use crate::absorb::*;
use crate::answer::lemma_sum_ones;

verus! {

/// Scanned rows as +1 items.
pub open spec fn scan_items(rows: Seq<PreInput>) -> Items {
    rows.map_values(|r: PreInput| (pre_input_view(r), 1int))
}

pub proof fn lemma_scan_items_unit(rows: Seq<PreInput>)
    ensures
        unit_items(scan_items(rows)),
        scan_items(rows).len() == rows.len(),
{
}

pub proof fn lemma_scan_vsum(rows: Seq<PreInput>, db: Db, f: spec_fn(PreRow) -> int)
    requires
        scan_pre_build_ok(db, rows),
    ensures
        vsum(Seq::empty(), scan_items(rows), f) == sum(db.pre, f),
{
    let items = scan_items(rows);
    lemma_sum_empty(Seq::<PreRow>::empty(), f);
    lemma_sum_map(rows, |r: PreInput| pre_input_view(r), f, |r: PreInput| f(pre_input_view(r)));
    let ones = |d: (PreRow, int)| f(d.0);
    let signed = |d: (PreRow, int)| d.1 * f(d.0);
    assert forall|i: int| 0 <= i < items.len() implies signed(#[trigger] items[i]) == ones(items[i]) by {}
    lemma_sum_ext(items, signed, ones);
    lemma_sum_map(rows, |r: PreInput| (pre_input_view(r), 1int), ones, |r: PreInput| f(pre_input_view(r)));
    assert(sum(pre_input_seq_view(rows), f) == sum(db.pre, f));
}

/// The state's parts, valid for a virtual bag, are valid for any plain bag of the same weights.
pub proof fn lemma_parts_transfer(aggs: Seq<Agg>, cells: Index<i128>, base: Seq<PreRow>, items: Items, base2: Seq<PreRow>)
    requires
        parts_ok(aggs, cells, base, items),
        forall|f: spec_fn(PreRow) -> int| #[trigger] vsum(base, items, f) == sum(base2, f),
    ensures
        parts_ok(aggs, cells, base2, Seq::empty()),
{
    assert forall|f: spec_fn(PreRow) -> int| #[trigger] vsum(base2, Seq::empty(), f) == sum(base2, f) by {
        assert(dsum(Seq::<(PreRow, int)>::empty(), f) == 0) by { lemma_sum_empty(Seq::<(PreRow, int)>::empty(), |d: (PreRow, int)| d.1 * f(d.0)); }
    }
    assert forall|s: Seq<u8>, rf: Option<Seq<u8>>| !(#[trigger] has_key(aggs, s, rf)) implies {
        &&& forall|k: int| 0 <= k < base2.len() ==> !in_grp(#[trigger] base2[k], s, rf)
        &&& forall|k: int| 0 <= k < Seq::<(PreRow, int)>::empty().len() ==> !in_grp(#[trigger] Seq::<(PreRow, int)>::empty()[k].0, s, rf)
    } by {
        let f = w_cnt(s, rf);
        lemma_vsum_absent(base, items, s, rf, f);
        assert forall|k: int| 0 <= k < base2.len() implies !in_grp(#[trigger] base2[k], s, rf) by {
            if in_grp(base2[k], s, rf) {
                assert forall|i: int| 0 <= i < base2.len() implies f(#[trigger] base2[i]) >= 0 by {}
                assert(f(base2[k]) == 1);
                lemma_sum_pos(base2, f, k);
            }
        }
    }
    assert forall|i: int| 0 <= i < aggs.len() implies {
        let a = #[trigger] aggs[i];
        &&& a.cnt as int == vsum(base2, Seq::empty(), w_cnt(a.s(), a.rf()))
        &&& a.linecnt as int == vsum(base2, Seq::empty(), w_lcnt(a.s(), a.rf()))
        &&& a.linesum as int == vsum(base2, Seq::empty(), w_lsum(a.s(), a.rf()))
    } by {}
    assert forall|g: int, a: Seq<u8>| 0 <= g < aggs.len() implies
        cell_ok(#[trigger] cells.at((gkey(g), a)), vsum(base2, Seq::empty(), w_cell(aggs[g].s(), aggs[g].rf(), a))) by {}
}

pub proof fn lemma_sum_mass_le(s: Seq<PreRow>, m: int)
    requires
        forall|k: int| 0 <= k < s.len() ==> mass(#[trigger] s[k]) <= m,
    ensures
        sum(s, |r: PreRow| mass(r)) <= s.len() * m,
    decreases s.len(),
{
    if s.len() > 0 {
        let t = s.drop_last();
        assert forall|k: int| 0 <= k < t.len() implies mass(#[trigger] t[k]) <= m by { assert(t[k] == s[k]); }
        lemma_sum_mass_le(t, m);
        assert(mass(s.last()) <= m);
        assert(s.last() == s[s.len() - 1]);
        assert((s.len() - 1) * m + m == s.len() * m) by (nonlinear_arith);
        assert(t.len() * m <= (s.len() - 1) * m);
    }
}

pub proof fn lemma_imass_le(items: Items, m: int)
    requires
        forall|k: int| 0 <= k < items.len() ==> mass((#[trigger] items[k]).0) <= m,
    ensures
        sum(items, |d: (PreRow, int)| imass(d)) <= items.len() * m,
    decreases items.len(),
{
    if items.len() > 0 {
        let t = items.drop_last();
        assert forall|k: int| 0 <= k < t.len() implies mass((#[trigger] t[k]).0) <= m by { assert(t[k] == items[k]); }
        lemma_imass_le(t, m);
        assert(items.last() == items[items.len() - 1]);
        assert((items.len() - 1) * m + m == items.len() * m) by (nonlinear_arith);
    }
}

/// Mass is nonnegative: a prefix weighs no more than the whole.
pub proof fn lemma_total_mass_prefix(base: Seq<PreRow>, items: Items, k: int)
    requires
        0 <= k <= items.len(),
    ensures
        total_mass(base, items.take(k)) <= total_mass(base, items),
{
    lemma_mass_prefix(items, k);
}

pub open spec fn in_type_row(r: PreRow) -> bool { pre_in_type(r) }

pub open spec fn mass_cap() -> int { 2147483649 }

/// Rows of the SQL type weigh at most 2^31 + 1.
pub proof fn lemma_type_mass(r: PreRow)
    requires
        pre_in_type(r),
    ensures
        mass(r) <= mass_cap(),
{
}

/// A database within the envelope: its rows weigh at most mass_cap each.
pub proof fn lemma_env_mass(db: Db)
    requires
        envelope(db),
    ensures
        sum(db.pre, |r: PreRow| mass(r)) <= db.pre.len() * mass_cap(),
{
    assert forall|k: int| 0 <= k < db.pre.len() implies mass(#[trigger] db.pre[k]) <= mass_cap() by {
        lemma_type_mass(db.pre[k]);
    }
    lemma_sum_mass_le(db.pre, mass_cap());
}

/// Scanned rows of a database within the envelope are within the SQL type too.
pub proof fn lemma_scan_in_type(rows: Seq<PreInput>, db: Db)
    requires
        scan_pre_build_ok(db, rows),
        envelope(db),
    ensures
        forall|k: int| 0 <= k < rows.len() ==> pre_in_type(pre_input_view(#[trigger] rows[k])),
{
    let bad = |r: PreRow| if pre_in_type(r) { 0int } else { 1int };
    assert forall|k: int| 0 <= k < db.pre.len() implies bad(#[trigger] db.pre[k]) == 0 by {}
    lemma_sum_zero(db.pre, bad);
    let view = pre_input_seq_view(rows);
    assert(sum(view, bad) == 0);
    assert forall|k: int| 0 <= k < view.len() implies bad(#[trigger] view[k]) >= 0 by {}
    assert forall|k: int| 0 <= k < rows.len() implies pre_in_type(pre_input_view(#[trigger] rows[k])) by {
        assert(view[k] == pre_input_view(rows[k]));
        if !pre_in_type(view[k]) { lemma_sum_pos(view, bad, k); }
    }
}

pub proof fn lemma_scan_mass(rows: Seq<PreInput>, db: Db)
    requires
        scan_pre_build_ok(db, rows),
        envelope(db),
    ensures
        total_mass(Seq::empty(), scan_items(rows)) < big(),
{
    lemma_scan_in_type(rows, db);
    let items = scan_items(rows);
    assert forall|k: int| 0 <= k < items.len() implies mass((#[trigger] items[k]).0) <= mass_cap() by {
        lemma_type_mass(pre_input_view(rows[k]));
    }
    lemma_imass_le(items, mass_cap());
    lemma_sum_empty(Seq::<PreRow>::empty(), |r: PreRow| mass(r));
    // len <= 2^63 follows from the scan: its +1 count equals the database's
    let one = |r: PreRow| 1int;
    lemma_sum_ones(db.pre);
    lemma_sum_ones(pre_input_seq_view(rows));
    assert(sum(pre_input_seq_view(rows), one) == sum(db.pre, one));
    assert(pre_input_seq_view(rows).len() == rows.len());
    assert((rows.len() as int) * mass_cap() <= 9223372036854775808 * mass_cap()) by (nonlinear_arith)
        requires rows.len() <= 9223372036854775808;
}

/// The mass of a database plus a bounded delta.
pub proof fn lemma_apply_mass(db: Db, items: Items, n: int)
    requires
        envelope(db),
        items.len() <= n,
        n <= 4611686018427387904,
        forall|k: int| 0 <= k < items.len() ==> mass((#[trigger] items[k]).0) <= 9223372036854775809,
    ensures
        total_mass(db.pre, items) < big(),
{
    lemma_env_mass(db);
    lemma_imass_le(items, 9223372036854775809);
    assert((db.pre.len() as int) * mass_cap() <= 9223372036854775808 * mass_cap()) by (nonlinear_arith)
        requires db.pre.len() <= 9223372036854775808;
    assert((items.len() as int) * 9223372036854775809 <= 4611686018427387904 * 9223372036854775809) by (nonlinear_arith)
        requires items.len() <= 4611686018427387904;
}

pub proof fn lemma_parts_transfer_scan(aggs: Seq<Agg>, cells: Index<i128>, rows: Seq<PreInput>, items: Items, db: Db)
    requires
        items == scan_items(rows),
        scan_pre_build_ok(db, rows),
        parts_ok(aggs, cells, Seq::empty(), items),
    ensures
        parts_ok(aggs, cells, db.pre, Seq::empty()),
{
    assert forall|f: spec_fn(PreRow) -> int| #[trigger] vsum(Seq::empty(), items, f) == sum(db.pre, f) by {
        lemma_scan_vsum(rows, db, f);
    }
    lemma_parts_transfer(aggs, cells, Seq::empty(), items, db.pre);
}

} // verus!
