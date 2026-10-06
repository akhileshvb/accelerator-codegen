// Small row vectors of the state: pair rows of a part (supplier, count, cost), name-class rows
// (mask of tracked patterns, count) and rollup cells (nation, year, count, profit).  Each has a
// sum-semantics view and a verified in-place "bump".
use vstd::prelude::*;
use verus_common::sums::*;
use crate::values::*;
use verus_common::sqlfns::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

// ── bits of a pattern mask ──

pub open spec fn bit(mask: u128, c: int) -> bool {
    0 <= c < 128 && ((mask >> (c as u128)) & 1u128) == 1u128
}

pub fn has_bit(mask: u128, c: u32) -> (r: bool)
    requires c < 128,
    ensures r == bit(mask, c as int),
{
    ((mask >> c) & 1u128) == 1u128
}

proof fn lemma_set_bit(mask: u128, c: u32, j: u32)
    requires c < 128, j < 128,
    ensures ((mask | (1u128 << c)) >> j) & 1u128 == (if j == c { 1u128 } else { (mask >> j) & 1u128 }),
{
    assert(((mask | (1u128 << c)) >> j) & 1u128 == (if j == c { 1u128 } else { (mask >> j) & 1u128 })) by (bit_vector)
        requires c < 128, j < 128;
}

pub fn set_bit(mask: u128, c: u32) -> (r: u128)
    requires c < 128,
    ensures forall|j: int| #[trigger] bit(r, j) <==> (bit(mask, j) || j == c as int),
{
    let r = mask | (1u128 << c);
    proof {
        assert forall|j: int| #[trigger] bit(r, j) <==> (bit(mask, j) || j == c as int) by {
            if 0 <= j < 128 {
                lemma_set_bit(mask, c, j as u32);
                assert(((r >> (j as u128)) & 1u128) == ((r >> (j as u32)) & 1u128));
                assert(((mask >> (j as u128)) & 1u128) == ((mask >> (j as u32)) & 1u128));
            }
        }
    }
    r
}

// ── pair rows ──

pub open spec fn pair_at(rows: Seq<Pair>, supplier: int, field: int) -> int {
    sum(rows, |row: Pair| if row.supplier == supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0int })
}

pub open spec fn pairs_positive(rows: Seq<Pair>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count > 0
}

pub open spec fn pairs_unique(rows: Seq<Pair>) -> bool {
    rows.map_values(|row: Pair| row.supplier).no_duplicates()
}

pub fn copy_pairs(rows: &Vec<Pair>) -> (result: Vec<Pair>)
    ensures result@ == rows@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), result@ == rows@.take(index as int),
        decreases rows.len() - index,
    { result.push(rows[index]); index += 1; }
    proof { assert(rows@.take(index as int) =~= rows@); }
    result
}

pub fn bump_pair(rows: &mut Vec<Pair>, supplier: i64, count: i128, cost: i128) -> (result: Option<()>)
    ensures
        result.is_some() ==> final(rows).len() > 0,
        result.is_some() && count > 0 && pairs_positive(old(rows)@) ==> pairs_positive(final(rows)@),
        result.is_some() ==> forall|target_supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] pair_at(final(rows)@, target_supplier, field)
            == pair_at(old(rows)@, target_supplier, field) + if target_supplier == supplier as int { if field == 0 { count as int } else { cost as int } } else { 0 },
        pairs_unique(old(rows)@) ==> pairs_unique(final(rows)@),
{
    let ghost before = rows@;
    let ghost increment_count = count as int;
    let ghost increment_cost = cost as int;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> #[trigger] rows@[position as int].supplier != supplier, increment_count == count as int, increment_cost == cost as int,
        decreases rows.len() - index,
    {
        if rows[index].supplier == supplier {
            let count = rows[index].count.checked_add(count)?;
            let cost = rows[index].cost.checked_add(cost)?;
            let entry = Pair { supplier, count, cost };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|target_supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] pair_at(rows@, target_supplier, field)
                    == pair_at(before, target_supplier, field) + if target_supplier == supplier as int { if field == 0 { increment_count } else { increment_cost } } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, |row: Pair| if row.supplier as int == target_supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0 });
                }
                assert(rows@.map_values(|row: Pair| row.supplier) =~= before.map_values(|row: Pair| row.supplier));
            }
            return Some(());
        }
        index += 1;
    }
    let entry = Pair { supplier, count, cost };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|target_supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] pair_at(rows@, target_supplier, field)
            == pair_at(before, target_supplier, field) + if target_supplier == supplier as int { if field == 0 { increment_count } else { increment_cost } } else { 0 } by {
            lemma_sum_push(before, appended, |row: Pair| if row.supplier as int == target_supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0 });
        }
        if pairs_unique(before) {
            let projected = before.map_values(|row: Pair| row.supplier);
            assert(!projected.contains(supplier));
            verus_prooflib::lemma_unique_push(projected, supplier);
            assert(rows@.map_values(|row: Pair| row.supplier) =~= projected.push(supplier));
        }
    }
    Some(())
}

// ── mask rows ──

pub open spec fn mask_green(rows: Seq<MaskRow>) -> int {
    sum(rows, |row: MaskRow| row.count as int)
}

pub open spec fn mask_color(rows: Seq<MaskRow>, c: int) -> int {
    sum(rows, |row: MaskRow| if bit(row.mask, c) { row.count as int } else { 0int })
}

pub open spec fn masks_positive(rows: Seq<MaskRow>) -> bool {
    forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count > 0
}

pub open spec fn masks_unique(rows: Seq<MaskRow>) -> bool {
    rows.map_values(|row: MaskRow| row.mask).no_duplicates()
}

pub fn copy_masks(rows: &Vec<MaskRow>) -> (result: Vec<MaskRow>)
    ensures result@ == rows@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), result@ == rows@.take(index as int),
        decreases rows.len() - index,
    { result.push(rows[index]); index += 1; }
    proof { assert(rows@.take(index as int) =~= rows@); }
    result
}

pub fn bump_mask(rows: &mut Vec<MaskRow>, mask: u128, amount: i128) -> (result: Option<()>)
    ensures
        result.is_some() ==> final(rows).len() > 0,
        result.is_some() && amount > 0 && masks_positive(old(rows)@) ==> masks_positive(final(rows)@),
        result.is_some() ==> mask_green(final(rows)@) == mask_green(old(rows)@) + amount as int,
        result.is_some() ==> forall|c: int| #[trigger] mask_color(final(rows)@, c) == mask_color(old(rows)@, c) + if bit(mask, c) { amount as int } else { 0 },
        masks_unique(old(rows)@) ==> masks_unique(final(rows)@),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> #[trigger] rows@[position as int].mask != mask,
        decreases rows.len() - index,
    {
        if rows[index].mask == mask {
            let count = rows[index].count.checked_add(amount)?;
            let entry = MaskRow { mask, count };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                lemma_sum_update(before, index as int, replacement, |row: MaskRow| row.count as int);
                assert forall|c: int| #[trigger] mask_color(rows@, c) == mask_color(before, c) + if bit(mask, c) { amount as int } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, |row: MaskRow| if bit(row.mask, c) { row.count as int } else { 0int });
                    assert(before[index as int].mask == mask);
                }
                assert(rows@.map_values(|row: MaskRow| row.mask) =~= before.map_values(|row: MaskRow| row.mask));
            }
            return Some(());
        }
        index += 1;
    }
    let entry = MaskRow { mask, count: amount };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        lemma_sum_push(before, appended, |row: MaskRow| row.count as int);
        assert forall|c: int| #[trigger] mask_color(rows@, c) == mask_color(before, c) + if bit(mask, c) { amount as int } else { 0 } by {
            lemma_sum_push(before, appended, |row: MaskRow| if bit(row.mask, c) { row.count as int } else { 0int });
        }
        if masks_unique(before) {
            let projected = before.map_values(|row: MaskRow| row.mask);
            assert(!projected.contains(mask));
            verus_prooflib::lemma_unique_push(projected, mask);
            assert(rows@.map_values(|row: MaskRow| row.mask) =~= projected.push(mask));
        }
    }
    Some(())
}

// ── rollup cells ──

pub open spec fn cell_at(rows: Seq<RCell>, nk: int, year: int, field: int) -> int {
    sum(rows, |row: RCell| if row.nk == nk && row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int })
}

pub open spec fn cells_unique(rows: Seq<RCell>) -> bool {
    rows.map_values(|row: RCell| (row.nk, row.year)).no_duplicates()
}

pub fn copy_cells(rows: &Vec<RCell>) -> (result: Vec<RCell>)
    ensures result@ == rows@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), result@ == rows@.take(index as int),
        decreases rows.len() - index,
    { result.push(rows[index]); index += 1; }
    proof { assert(rows@.take(index as int) =~= rows@); }
    result
}

pub fn bump_cell(rows: &mut Vec<RCell>, nk: i64, year: i64, count: i128, profit: i128) -> (result: Option<()>)
    ensures
        result.is_some() ==> forall|target_nk: int, target_year: int, field: int| 0 <= field <= 1 ==> #[trigger] cell_at(final(rows)@, target_nk, target_year, field)
            == cell_at(old(rows)@, target_nk, target_year, field) + if target_nk == nk as int && target_year == year as int { if field == 0 { count as int } else { profit as int } } else { 0 },
        cells_unique(old(rows)@) ==> cells_unique(final(rows)@),
{
    let ghost before = rows@;
    let ghost increment_count = count as int;
    let ghost increment_profit = profit as int;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> !(#[trigger] rows@[position as int].nk == nk && rows@[position as int].year == year),
            increment_count == count as int, increment_profit == profit as int,
        decreases rows.len() - index,
    {
        if rows[index].nk == nk && rows[index].year == year {
            let c2 = rows[index].count.checked_add(count)?;
            let p2 = rows[index].profit.checked_add(profit)?;
            let entry = RCell { nk, year, count: c2, profit: p2 };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|target_nk: int, target_year: int, field: int| 0 <= field <= 1 ==> #[trigger] cell_at(rows@, target_nk, target_year, field)
                    == cell_at(before, target_nk, target_year, field) + if target_nk == nk as int && target_year == year as int { if field == 0 { increment_count } else { increment_profit } } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, |row: RCell| if row.nk as int == target_nk && row.year as int == target_year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
                }
                assert(rows@.map_values(|row: RCell| (row.nk, row.year)) =~= before.map_values(|row: RCell| (row.nk, row.year)));
            }
            return Some(());
        }
        index += 1;
    }
    let entry = RCell { nk, year, count, profit };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|target_nk: int, target_year: int, field: int| 0 <= field <= 1 ==> #[trigger] cell_at(rows@, target_nk, target_year, field)
            == cell_at(before, target_nk, target_year, field) + if target_nk == nk as int && target_year == year as int { if field == 0 { increment_count } else { increment_profit } } else { 0 } by {
            lemma_sum_push(before, appended, |row: RCell| if row.nk as int == target_nk && row.year as int == target_year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
        }
        if cells_unique(before) {
            let projected = before.map_values(|row: RCell| (row.nk, row.year));
            assert(!projected.contains((nk, year)));
            verus_prooflib::lemma_unique_push(projected, (nk, year));
            assert(rows@.map_values(|row: RCell| (row.nk, row.year)) =~= projected.push((nk, year)));
        }
    }
    Some(())
}

}

verus! {

proof fn lemma_zero_bit(j: u128)
    ensures ((0u128 >> j) & 1u128) == 0u128,
{
    assert(((0u128 >> j) & 1u128) == 0u128) by (bit_vector);
}

pub fn mask_color_exec(rows: &Vec<MaskRow>, c: u32) -> (r: Option<i128>)
    requires c < 128,
    ensures r matches Some(v) ==> v as int == mask_color(rows@, c as int),
{
    let mut total: i128 = 0;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), c < 128, total as int == mask_color(rows@.take(index as int), c as int),
        decreases rows.len() - index,
    {
        let add: i128 = if has_bit(rows[index].mask, c) { rows[index].count } else { 0 };
        proof {
            lemma_sum_take(rows@, index as int, |row: MaskRow| if bit(row.mask, c as int) { row.count as int } else { 0int });
        }
        total = total.checked_add(add)?;
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    Some(total)
}

/// The tracked patterns a part name matches.
pub fn mask_of(name: &Vec<u8>, pats: &Vec<Vec<u8>>) -> (m: u128)
    requires pats.len() <= 127,
    ensures forall|c: int| #[trigger] bit(m, c) <==> (0 <= c < pats.len() && like_match(name@, pats@[c]@)),
{
    let mut m: u128 = 0;
    proof {
        assert forall|c: int| !bit(0u128, c) by {
            if 0 <= c < 128 {
                lemma_zero_bit(c as u128);
            }
        }
    }
    let mut index: usize = 0;
    while index < pats.len()
        invariant index <= pats.len(), pats.len() <= 127,
            forall|c: int| #[trigger] bit(m, c) <==> (0 <= c < index && like_match(name@, pats@[c]@)),
        decreases pats.len() - index,
    {
        if verus_prooflib::like_match_exec(name.as_slice(), pats[index].as_slice()) {
            m = set_bit(m, index as u32);
        }
        index += 1;
    }
    m
}

}
