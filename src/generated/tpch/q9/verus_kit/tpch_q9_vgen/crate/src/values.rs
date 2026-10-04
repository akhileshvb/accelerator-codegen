use vstd::prelude::*;
use std::sync::Arc;
use crate::storage::*;
use crate::gen_obligations::*;
use verus_common::bytes::*;
use verus_common::sqlfns::*;
use verus_common::sums::*;
use crate::meaning::*;

verus! {

#[derive(Clone, Copy)]
pub struct Cell { pub count: i128, pub revenue: i128, pub quantity: i128 }

pub struct Pair { pub supplier: i64, pub count: i128, pub cost: i128 }
pub struct Moment { pub supplier: i64, pub year: i64, pub cell: Cell }
pub struct Part { pub green: i128, pub pairs: Vec<Pair>, pub moments: Vec<Moment> }
#[derive(Clone, Copy)]
pub struct Year { pub year: i64, pub count: i128 }
pub struct Order { pub refs: i128, pub years: Vec<Year> }
#[derive(Clone, Copy)]
pub struct Number { pub key: i64, pub count: i128 }
pub struct Name { pub bytes: Arc<Vec<u8>>, pub count: i128 }
#[derive(Clone, Copy)]
pub struct Total { pub year: i64, pub count: i128, pub profit: i128 }
pub struct Group { pub name: Arc<Vec<u8>>, pub totals: Vec<Total> }

pub fn zero() -> Cell { Cell { count: 0, revenue: 0, quantity: 0 } }

pub fn copy_bytes(bytes: &Vec<u8>) -> (result: Vec<u8>)
    ensures result@ == bytes@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), result@ == bytes@.take(index as int),
        decreases bytes.len() - index,
    {
        result.push(bytes[index]);
        index += 1;
    }
    result
}

pub fn bytes_equal(left: &Vec<u8>, right: &Vec<u8>) -> (result: bool)
    ensures result == (left@ == right@),
{
    if left.len() != right.len() { return false; }
    let mut index = 0;
    while index < left.len()
        invariant index <= left.len(), left.len() == right.len(),
            forall|position: int| 0 <= position < index ==> #[trigger] left@[position] == right@[position],
        decreases left.len() - index,
    {
        if left[index] != right[index] { return false; }
        index += 1;
    }
    proof { assert(left@ =~= right@); }
    true
}

pub fn bytes_less(left: &Vec<u8>, right: &Vec<u8>) -> (result: bool)
    ensures result == bytes_lt(left@, right@),
{
    proof { assert(left@.skip(0) =~= left@); assert(right@.skip(0) =~= right@); }
    let mut index = 0;
    while index < left.len() && index < right.len()
        invariant index <= left.len(), index <= right.len(),
            bytes_lt(left@, right@) == bytes_lt(left@.skip(index as int), right@.skip(index as int)),
            forall|position: int| 0 <= position < index ==> #[trigger] left@[position] == right@[position],
        decreases left.len() - index,
    {
        if left[index] < right[index] { return true; }
        if left[index] > right[index] { return false; }
        index += 1;
        proof {
            assert(left@.skip(index as int - 1).drop_first() =~= left@.skip(index as int));
            assert(right@.skip(index as int - 1).drop_first() =~= right@.skip(index as int));
        }
    }
    left.len() < right.len()
}

pub open spec fn name_hash(bytes: Seq<u8>) -> int
    decreases bytes.len(),
{
    if bytes.len() == 0 { 0 } else {
        (name_hash(bytes.drop_last()) * 257 + bytes.last() as int + 1) % 9223372036854775783
    }
}

pub fn name_key(bytes: &Vec<u8>) -> (result: i64)
    ensures result as int == name_hash(bytes@),
{
    let mut hash: u128 = 0;
    let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), hash < 9223372036854775783u128,
            hash as int == name_hash(bytes@.take(index as int)),
        decreases bytes.len() - index,
    {
        proof {
            assert(bytes@.take(index as int + 1).drop_last() =~= bytes@.take(index as int));
            assert(bytes@.take(index as int + 1).last() == bytes@[index as int]);
        }
        hash = (hash * 257 + bytes[index] as u128 + 1) % 9223372036854775783u128;
        index += 1;
    }
    proof { assert(bytes@.take(index as int) =~= bytes@); }
    hash as i64
}

#[verifier::spinoff_prover]
pub fn year(date: i32) -> (result: i64)
    ensures result as int == civil_year(date as int),
{
    let shifted = date as i64 + 719468;
    let era = if shifted >= 0 { shifted / 146097 } else { (shifted - 146096) / 146097 };
    proof { assert(era as int == (date as int + 719468) / 146097); }
    let day = shifted - era * 146097;
    proof {
        assert(day as int == civil_doe(date as int));
        assert(0 <= day < 146097);
    }
    let yoe = (day - day / 1460 + day / 36524 - day / 146096) / 365;
    proof { assert(yoe as int == civil_yoe(date as int)); assert(0 <= yoe <= 399); }
    let doy = day - (365 * yoe + yoe / 4 - yoe / 100);
    proof { assert(doy as int == civil_doy(date as int)); assert(0 <= doy <= 365); }
    let month = (5 * doy + 2) / 153;
    proof { assert(month as int == civil_mp(date as int)); assert(0 <= month <= 11); }
    yoe + era * 400 + if month >= 10 { 1 } else { 0 }
}

pub fn copy_years(rows: &Vec<Year>) -> (result: Vec<Year>)
    ensures result@ == rows@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), result@ == rows@.take(index as int),
        decreases rows.len() - index,
    { result.push(rows[index]); index += 1; }
    result
}

pub fn copy_numbers(rows: &Vec<Number>) -> (result: Vec<Number>)
    ensures result@ == rows@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), result@ == rows@.take(index as int),
        decreases rows.len() - index,
    { result.push(rows[index]); index += 1; }
    result
}

pub fn copy_totals(rows: &Vec<Total>) -> (result: Vec<Total>)
    ensures result@ == rows@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), result@ == rows@.take(index as int),
        decreases rows.len() - index,
    { result.push(rows[index]); index += 1; }
    result
}

pub fn copy_names(rows: &Vec<Name>) -> (result: Vec<Name>)
    ensures result@ == rows@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), result@ == rows@.take(index as int),
        decreases rows.len() - index,
    { result.push(Name { bytes: Arc::clone(&rows[index].bytes), count: rows[index].count }); index += 1; }
    result
}

pub fn copy_part(part: &Part) -> (result: Part)
    ensures result.green == part.green, result.pairs@ == part.pairs@, result.moments@ == part.moments@,
{
    let mut pairs = Vec::new();
    let mut index = 0;
    while index < part.pairs.len()
        invariant index <= part.pairs.len(), pairs@ == part.pairs@.take(index as int),
        decreases part.pairs.len() - index,
    {
        pairs.push(Pair { supplier: part.pairs[index].supplier, count: part.pairs[index].count, cost: part.pairs[index].cost });
        index += 1;
    }
    let mut moments = Vec::new();
    index = 0;
    while index < part.moments.len()
        invariant index <= part.moments.len(), moments@ == part.moments@.take(index as int), pairs@ == part.pairs@,
        decreases part.moments.len() - index,
    {
        moments.push(Moment { supplier: part.moments[index].supplier, year: part.moments[index].year, cell: part.moments[index].cell });
        index += 1;
    }
    Part { green: part.green, pairs, moments }
}

pub fn empty_part() -> (result: Part)
    ensures result.green == 0, result.pairs@ == Seq::<Pair>::empty(), result.moments@ == Seq::<Moment>::empty(),
{ Part { green: 0, pairs: Vec::new(), moments: Vec::new() } }

pub fn bump_year(rows: &mut Vec<Year>, target: i64, amount: i128) -> (result: Option<()>)
    ensures
        (forall|position: int| 0 <= position < old(rows).len() && (#[trigger] old(rows)[position]).year == target ==>
            i128::MIN <= old(rows)[position].count as int + amount as int <= i128::MAX) ==> result.is_some(),
        result.is_some() ==> final(rows).len() > 0,
        result.is_some() && amount > 0 && years_positive(old(rows)@) ==> years_positive(final(rows)@),
        result.is_some() ==> forall|target_year: int| #[trigger] year_at(final(rows)@, target_year) == year_at(old(rows)@, target_year) + if target_year == target as int { amount as int } else { 0 },
        old(rows)@.map_values(|row: Year| row.year).no_duplicates() ==> final(rows)@.map_values(|row: Year| row.year).no_duplicates(),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> #[trigger] rows@[position as int].year != target,
        decreases rows.len() - index,
    {
        if rows[index].year == target {
            let count = rows[index].count.checked_add(amount)?;
            let entry = Year { year: target, count };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|target_year: int| #[trigger] year_at(rows@, target_year) == year_at(before, target_year) + if target_year == target as int { amount as int } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, |row: Year| if row.year as int == target_year { row.count as int } else { 0 });
                }
                assert(rows@.map_values(|row: Year| row.year) =~= before.map_values(|row: Year| row.year));
            }
            return Some(());
        }
        index += 1;
    }
    let entry = Year { year: target, count: amount };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|target_year: int| #[trigger] year_at(rows@, target_year) == year_at(before, target_year) + if target_year == target as int { amount as int } else { 0 } by {
            lemma_sum_push(before, appended, |row: Year| if row.year as int == target_year { row.count as int } else { 0 });
        }
        if before.map_values(|row: Year| row.year).no_duplicates() {
            let projected = before.map_values(|row: Year| row.year);
            assert(!projected.contains(target));
            verus_prooflib::lemma_unique_push(projected, target);
            assert(rows@.map_values(|row: Year| row.year) =~= projected.push(target));
        }
    }
    Some(())
}

pub fn bump_number(rows: &mut Vec<Number>, target: i64, amount: i128) -> (result: Option<()>)
    ensures
        result.is_some() ==> final(rows).len() > 0,
        result.is_some() && amount > 0 && numbers_positive(old(rows)@) ==> numbers_positive(final(rows)@),
        result.is_some() ==> forall|target_key: int| #[trigger] number_at(final(rows)@, target_key) == number_at(old(rows)@, target_key) + if target_key == target as int { amount as int } else { 0 },
        old(rows)@.map_values(|row: Number| row.key).no_duplicates() ==> final(rows)@.map_values(|row: Number| row.key).no_duplicates(),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> #[trigger] rows@[position as int].key != target,
        decreases rows.len() - index,
    {
        if rows[index].key == target {
            let count = rows[index].count.checked_add(amount)?;
            let entry = Number { key: target, count };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|target_key: int| #[trigger] number_at(rows@, target_key) == number_at(before, target_key) + if target_key == target as int { amount as int } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, |row: Number| if row.key as int == target_key { row.count as int } else { 0 });
                }
                assert(rows@.map_values(|row: Number| row.key) =~= before.map_values(|row: Number| row.key));
            }
            return Some(());
        }
        index += 1;
    }
    let entry = Number { key: target, count: amount };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|target_key: int| #[trigger] number_at(rows@, target_key) == number_at(before, target_key) + if target_key == target as int { amount as int } else { 0 } by {
            lemma_sum_push(before, appended, |row: Number| if row.key as int == target_key { row.count as int } else { 0 });
        }
        if before.map_values(|row: Number| row.key).no_duplicates() {
            let projected = before.map_values(|row: Number| row.key);
            assert(!projected.contains(target));
            verus_prooflib::lemma_unique_push(projected, target);
            assert(rows@.map_values(|row: Number| row.key) =~= projected.push(target));
        }
    }
    Some(())
}

pub fn bump_name(rows: &mut Vec<Name>, bytes: &Arc<Vec<u8>>, amount: i128) -> (result: Option<()>)
    ensures
        result.is_some() ==> final(rows).len() > 0,
        result.is_some() && amount > 0 && names_positive(old(rows)@) ==> names_positive(final(rows)@),
        result.is_some() ==> forall|target_name: Seq<u8>| #[trigger] name_at(final(rows)@, target_name) == name_at(old(rows)@, target_name) + if target_name == bytes@ { amount as int } else { 0 },
        old(rows)@.map_values(|row: Name| row.bytes@).no_duplicates() ==> final(rows)@.map_values(|row: Name| row.bytes@).no_duplicates(),
{
    let ghost before = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@,
            forall|position: int| 0 <= position < index ==> #[trigger] rows@[position].bytes@ != bytes@,
        decreases rows.len() - index,
    {
        if bytes_equal(&rows[index].bytes, bytes) {
            let count = rows[index].count.checked_add(amount)?;
            let entry = Name { bytes: Arc::clone(bytes), count };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|target_name: Seq<u8>| #[trigger] name_at(rows@, target_name) == name_at(before, target_name) + if target_name == bytes@ { amount as int } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, |row: Name| if row.bytes@ == target_name { row.count as int } else { 0 });
                }
                assert(rows@.map_values(|row: Name| row.bytes@) =~= before.map_values(|row: Name| row.bytes@));
            }
            return Some(());
        }
        index += 1;
    }
    let entry = Name { bytes: Arc::clone(bytes), count: amount };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|target_name: Seq<u8>| #[trigger] name_at(rows@, target_name) == name_at(before, target_name) + if target_name == bytes@ { amount as int } else { 0 } by {
            lemma_sum_push(before, appended, |row: Name| if row.bytes@ == target_name { row.count as int } else { 0 });
        }
        if before.map_values(|row: Name| row.bytes@).no_duplicates() {
            let projected = before.map_values(|row: Name| row.bytes@);
            assert(!projected.contains(bytes@));
            verus_prooflib::lemma_unique_push(projected, bytes@);
            assert(rows@.map_values(|row: Name| row.bytes@) =~= projected.push(bytes@));
        }
    }
    Some(())
}

pub fn bump_pair(part: &mut Part, supplier: i64, count: i128, cost: i128) -> (result: Option<()>)
    ensures
        result.is_some() && count > 0 && pairs_positive(old(part).pairs@) ==> pairs_positive(final(part).pairs@),
        final(part).green == old(part).green, final(part).moments@ == old(part).moments@,
        result.is_some() ==> forall|target_supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] stored_pair(*final(part), target_supplier, field) == stored_pair(*old(part), target_supplier, field) + if target_supplier == supplier as int { if field == 0 { count as int } else { cost as int } } else { 0 },
        old(part).pairs@.map_values(|row: Pair| row.supplier).no_duplicates() ==> final(part).pairs@.map_values(|row: Pair| row.supplier).no_duplicates(),
{
    let ghost before = *part;
    let ghost increment_count = count as int;
    let ghost increment_cost = cost as int;
    let mut index = 0;
    while index < part.pairs.len()
        invariant index <= part.pairs.len(), *part == before, before == *old(part),
            forall|position: int| 0 <= position < index ==> #[trigger] part.pairs@[position].supplier != supplier, increment_count == count as int, increment_cost == cost as int,
        decreases part.pairs.len() - index,
    {
        if part.pairs[index].supplier == supplier {
            let count = part.pairs[index].count.checked_add(count)?;
            let cost = part.pairs[index].cost.checked_add(cost)?;
            let entry = Pair { supplier, count, cost };
            let ghost replacement = entry;
            part.pairs.set(index, entry);
            proof {
                assert forall|target_supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] stored_pair(*part, target_supplier, field) == stored_pair(before, target_supplier, field) + if target_supplier == supplier as int { if field == 0 { increment_count } else { increment_cost } } else { 0 } by {
                    lemma_sum_update(before.pairs@, index as int, replacement, |row: Pair| if row.supplier as int == target_supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0 });
                }
                assert(part.pairs@.map_values(|row: Pair| row.supplier) =~= before.pairs@.map_values(|row: Pair| row.supplier));
            }
            return Some(());
        }
        index += 1;
    }
    let entry = Pair { supplier, count, cost };
    let ghost appended = entry;
    part.pairs.push(entry);
    proof {
        assert forall|target_supplier: int, field: int| 0 <= field <= 1 ==> #[trigger] stored_pair(*part, target_supplier, field) == stored_pair(before, target_supplier, field) + if target_supplier == supplier as int { if field == 0 { increment_count } else { increment_cost } } else { 0 } by {
            lemma_sum_push(before.pairs@, appended, |row: Pair| if row.supplier as int == target_supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0 });
        }
        if before.pairs@.map_values(|row: Pair| row.supplier).no_duplicates() {
            let projected = before.pairs@.map_values(|row: Pair| row.supplier);
            assert(!projected.contains(supplier));
            verus_prooflib::lemma_unique_push(projected, supplier);
            assert(part.pairs@.map_values(|row: Pair| row.supplier) =~= projected.push(supplier));
        }
    }
    Some(())
}

pub fn bump_moment(part: &mut Part, supplier: i64, year: i64, cell: Cell) -> (result: Option<()>)
    ensures
        result.is_some() && cell.count > 0 && moments_positive(old(part).moments@) ==> moments_positive(final(part).moments@),
        final(part).green == old(part).green, final(part).pairs@ == old(part).pairs@,
        result.is_some() ==> forall|target_supplier: int, target_year: int, field: int| 0 <= field <= 2 ==> #[trigger] stored_moment(*final(part), target_supplier, target_year, field) == stored_moment(*old(part), target_supplier, target_year, field) + if target_supplier == supplier as int && target_year == year as int { if field == 0 { cell.count as int } else if field == 1 { cell.revenue as int } else { cell.quantity as int } } else { 0 },
        old(part).moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates() ==> final(part).moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates(),
{
    let ghost before = *part;
    let mut index = 0;
    while index < part.moments.len()
        invariant index <= part.moments.len(), *part == before, before == *old(part),
            forall|position: int| 0 <= position < index ==> #[trigger] part.moments@[position].supplier != supplier || part.moments@[position].year != year,
        decreases part.moments.len() - index,
    {
        if part.moments[index].supplier == supplier && part.moments[index].year == year {
            let count = part.moments[index].cell.count.checked_add(cell.count)?;
            let revenue = part.moments[index].cell.revenue.checked_add(cell.revenue)?;
            let quantity = part.moments[index].cell.quantity.checked_add(cell.quantity)?;
            let entry = Moment { supplier, year, cell: Cell { count, revenue, quantity } };
            let ghost replacement = entry;
            part.moments.set(index, entry);
            proof {
                assert forall|target_supplier: int, target_year: int, field: int| 0 <= field <= 2 ==> #[trigger] stored_moment(*part, target_supplier, target_year, field) == stored_moment(before, target_supplier, target_year, field) + if target_supplier == supplier as int && target_year == year as int { if field == 0 { cell.count as int } else if field == 1 { cell.revenue as int } else { cell.quantity as int } } else { 0 } by {
                    lemma_sum_update(before.moments@, index as int, replacement, |row: Moment| if row.supplier as int == target_supplier && row.year as int == target_year { if field == 0 { row.cell.count as int } else if field == 1 { row.cell.revenue as int } else { row.cell.quantity as int } } else { 0 });
                }
                assert(part.moments@.map_values(|row: Moment| (row.supplier, row.year)) =~= before.moments@.map_values(|row: Moment| (row.supplier, row.year)));
            }
            return Some(());
        }
        index += 1;
    }
    let entry = Moment { supplier, year, cell };
    let ghost appended = entry;
    part.moments.push(entry);
    proof {
        assert forall|target_supplier: int, target_year: int, field: int| 0 <= field <= 2 ==> #[trigger] stored_moment(*part, target_supplier, target_year, field) == stored_moment(before, target_supplier, target_year, field) + if target_supplier == supplier as int && target_year == year as int { if field == 0 { cell.count as int } else if field == 1 { cell.revenue as int } else { cell.quantity as int } } else { 0 } by {
            lemma_sum_push(before.moments@, appended, |row: Moment| if row.supplier as int == target_supplier && row.year as int == target_year { if field == 0 { row.cell.count as int } else if field == 1 { row.cell.revenue as int } else { row.cell.quantity as int } } else { 0 });
        }
        if before.moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates() {
            let projected = before.moments@.map_values(|row: Moment| (row.supplier, row.year));
            assert(!projected.contains((supplier, year)));
            verus_prooflib::lemma_unique_push(projected, (supplier, year));
            assert(part.moments@.map_values(|row: Moment| (row.supplier, row.year)) =~= projected.push((supplier, year)));
        }
    }
    Some(())
}

pub fn bump_total(rows: &mut Vec<Total>, year: i64, count: i128, profit: i128) -> (result: Option<()>)
    ensures
        result.is_some() ==> final(rows).len() > 0,
        result.is_some() && count > 0 && totals_positive(old(rows)@) ==> totals_positive(final(rows)@),
        result.is_some() ==> forall|target_year: int, field: int| 0 <= field <= 1 ==> #[trigger] total_at(final(rows)@, target_year, field) == total_at(old(rows)@, target_year, field) + if target_year == year as int { if field == 0 { count as int } else { profit as int } } else { 0 },
        old(rows)@.map_values(|row: Total| row.year).no_duplicates() ==> final(rows)@.map_values(|row: Total| row.year).no_duplicates(),
{
    let ghost before = rows@;
    let ghost increment_count = count as int;
    let ghost increment_profit = profit as int;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(), rows@ == before, before == old(rows)@, increment_count == count as int, increment_profit == profit as int,
            forall|position: int| 0 <= position < index ==> #[trigger] rows@[position as int].year != year,
        decreases rows.len() - index,
    {
        if rows[index].year == year {
            let count = rows[index].count.checked_add(count)?;
            let profit = rows[index].profit.checked_add(profit)?;
            let entry = Total { year, count, profit };
            let ghost replacement = entry;
            rows.set(index, entry);
            proof {
                assert forall|target_year: int, field: int| 0 <= field <= 1 ==> #[trigger] total_at(rows@, target_year, field) == total_at(before, target_year, field) + if target_year == year as int { if field == 0 { increment_count } else { increment_profit } } else { 0 } by {
                    lemma_sum_update(before, index as int, replacement, |row: Total| if row.year as int == target_year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 });
                }
                assert(rows@.map_values(|row: Total| row.year) =~= before.map_values(|row: Total| row.year));
            }
            return Some(());
        }
        index += 1;
    }
    let entry = Total { year, count, profit };
    let ghost appended = entry;
    rows.push(entry);
    proof {
        assert forall|target_year: int, field: int| 0 <= field <= 1 ==> #[trigger] total_at(rows@, target_year, field) == total_at(before, target_year, field) + if target_year == year as int { if field == 0 { increment_count } else { increment_profit } } else { 0 } by {
            lemma_sum_push(before, appended, |row: Total| if row.year as int == target_year { if field == 0 { row.count as int } else { row.profit as int } } else { 0 });
        }
        if before.map_values(|row: Total| row.year).no_duplicates() {
            let projected = before.map_values(|row: Total| row.year);
            assert(!projected.contains(year));
            verus_prooflib::lemma_unique_push(projected, year);
            assert(rows@.map_values(|row: Total| row.year) =~= projected.push(year));
        }
    }
    Some(())
}

pub fn line_cell(line: LineIn, multiplicity: i128) -> (result: Option<Cell>)
    ensures result matches Some(cell) ==> cell.count == multiplicity
        && cell.revenue as int == line_weight(line_in_view(line), 1) * multiplicity as int
        && cell.quantity as int == line_weight(line_in_view(line), 2) * multiplicity as int,
{
    let revenue = (line.l_extendedprice as i128).checked_mul(100i128.checked_sub(line.l_discount as i128)?)?.checked_mul(multiplicity)?;
    let quantity = (line.l_quantity as i128).checked_mul(multiplicity)?;
    Some(Cell { count: multiplicity, revenue, quantity })
}

pub fn add_line(part: &mut Part, line: LineIn, years: &Vec<Year>, sign: i128) -> (result: Option<()>)
    ensures final(part).green == old(part).green, final(part).pairs@ == old(part).pairs@,
        result.is_some() && sign > 0 && years_positive(years@) && moments_positive(old(part).moments@) ==> moments_positive(final(part).moments@),
        result.is_some() ==> forall|supplier: int, target_year: int, field: int| 0 <= field <= 2 ==>
            #[trigger] stored_moment(*final(part), supplier, target_year, field) == stored_moment(*old(part), supplier, target_year, field)
                + if supplier == line.l_suppkey as int {
                    year_at(years@, target_year) * line_weight(line_in_view(line), field) * sign as int
                } else { 0 },
        old(part).moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates()
            ==> final(part).moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates(),
{
    let ghost before = *part;
    proof {
        assert(years@.take(0) =~= Seq::<Year>::empty());
        assert forall|target_year: int| #[trigger] year_at(years@.take(0), target_year) == 0 by {
            lemma_sum_empty(years@.take(0), |row: Year| if row.year as int == target_year { row.count as int } else { 0 });
        }
        assert forall|supplier: int, target_year: int, field: int| 0 <= field <= 2 implies
            #[trigger] stored_moment(*part, supplier, target_year, field) == stored_moment(before, supplier, target_year, field)
                + if supplier == line.l_suppkey as int {
                    year_at(years@.take(0), target_year) * line_weight(line_in_view(line), field) * sign as int
                } else { 0 } by {
            let weight = line_weight(line_in_view(line), field);
            assert(0int * weight * sign as int == 0) by (nonlinear_arith);
        }
    }
    let mut index = 0;
    let mut processed = Ghost(0int);
    while index < years.len()
        invariant index <= years.len(), processed@ == index as int,
            sign > 0 && years_positive(years@) && moments_positive(before.moments@) ==> moments_positive(part.moments@),
            before == *old(part), part.green == before.green, part.pairs@ == before.pairs@,
            before.moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates()
                ==> part.moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates(),
            forall|supplier: int, target_year: int, field: int| 0 <= field <= 2 ==>
                #[trigger] stored_moment(*part, supplier, target_year, field) == stored_moment(before, supplier, target_year, field)
                    + if supplier == line.l_suppkey as int {
                        year_at(years@.take(index as int), target_year) * line_weight(line_in_view(line), field) * sign as int
                    } else { 0 },
        decreases years.len() - index,
    {
        let ghost previous = *part;
        proof {
            assert forall|supplier: int, target_year: int, field: int| 0 <= field <= 2 implies
                #[trigger] stored_moment(previous, supplier, target_year, field) == stored_moment(before, supplier, target_year, field)
                    + if supplier == line.l_suppkey as int {
                        year_at(years@.take(index as int), target_year) * line_weight(line_in_view(line), field) * sign as int
                    } else { 0 } by {}
        }
        let cell = line_cell(line, years[index].count.checked_mul(sign)?)?;
        proof {
            if sign > 0 && years_positive(years@) {
                assert(years@[index as int].count > 0);
                assert(cell.count as int == years@[index as int].count as int * sign as int);
                let multiplicity = years@[index as int].count as int;
                let scale = sign as int;
                assert(multiplicity > 0 && scale > 0);
                assert(multiplicity * scale > 0) by (nonlinear_arith) requires multiplicity > 0, scale > 0;
                assert(cell.count > 0);
            }
        }
        bump_moment(part, line.l_suppkey, years[index].year, cell)?;
        proof {
            assert forall|supplier: int, target_year: int, field: int| 0 <= field <= 2 implies
                #[trigger] stored_moment(*part, supplier, target_year, field) == stored_moment(before, supplier, target_year, field)
                    + if supplier == line.l_suppkey as int {
                        year_at(years@.take(index as int + 1), target_year) * line_weight(line_in_view(line), field) * sign as int
                    } else { 0 } by {
                lemma_sum_take(years@, index as int, |row: Year| if row.year as int == target_year { row.count as int } else { 0 });
                let prior = year_at(years@.take(index as int), target_year);
                let increment = if years@[index as int].year as int == target_year { years@[index as int].count as int } else { 0 };
                let weight = line_weight(line_in_view(line), field);
                assert((prior + increment) * weight * sign as int == prior * weight * sign as int + increment * weight * sign as int) by (nonlinear_arith);
                assert(cell.count as int == years@[index as int].count as int * sign as int);
                let multiplicity = years@[index as int].count as int;
                let scale = sign as int;
                if field == 1 {
                    assert(cell.revenue as int == weight * (multiplicity * scale));
                    assert(weight * (multiplicity * scale) == multiplicity * weight * scale) by (nonlinear_arith);
                } else if field == 2 {
                    assert(cell.quantity as int == weight * (multiplicity * scale));
                    assert(weight * (multiplicity * scale) == multiplicity * weight * scale) by (nonlinear_arith);
                } else { assert(weight == 1); }
                assert(stored_moment(*part, supplier, target_year, field) == stored_moment(previous, supplier, target_year, field)
                    + if supplier == line.l_suppkey as int && target_year == years@[index as int].year as int {
                        if field == 0 { cell.count as int } else if field == 1 { cell.revenue as int } else { cell.quantity as int }
                    } else { 0 });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof { assert(years@.take(index as int) =~= years@); }
    Some(())
}

pub fn prune_totals(rows: &mut Vec<Total>) -> (result: Option<()>)
    ensures
        forall|year: int, field: int| 0 <= field <= 1 ==>
            #[trigger] total_at(final(rows)@, year, field) == total_at(old(rows)@, year, field),
        old(rows)@.map_values(|row: Total| row.year).no_duplicates() ==> final(rows)@.map_values(|row: Total| row.year).no_duplicates(),
        result.is_some() ==> totals_positive(final(rows)@),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = rows@;
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows.len(),
            initial == old(rows)@,
            forall|year: int, field: int| 0 <= field <= 1 ==>
                #[trigger] total_at(rows@, year, field) == total_at(initial, year, field),
            initial.map_values(|row: Total| row.year).no_duplicates() ==> rows@.map_values(|row: Total| row.year).no_duplicates(),
            forall|position: int| 0 <= position < index ==> (#[trigger] rows[position]).count > 0,
        decreases rows.len() - index,
    {
        if rows[index].count < 0 { return None; }
        if rows[index].count == 0 {
            if rows[index].profit != 0 { return None; }
            let ghost before = rows@;
            rows.remove(index);
            proof {
                assert forall|year: int, field: int| 0 <= field <= 1 implies
                    #[trigger] total_at(rows@, year, field) == total_at(before, year, field) by {
                    lemma_sum_remove(before, index as int, |row: Total| if row.year == year { if field == 0 { row.count as int } else { row.profit as int } } else { 0int });
                }
                if before.map_values(|row: Total| row.year).no_duplicates() {
                    assert(rows@.map_values(|row: Total| row.year) =~= before.map_values(|row: Total| row.year).remove(index as int));
                    verus_prooflib::lemma_unique_remove(before.map_values(|row: Total| row.year), index as int);
                }
            }
        } else { index += 1; }
    }
    Some(())
}

pub fn prune_part(part: &mut Part) -> (result: Option<()>)
    ensures
        final(part).green == old(part).green,
        result.is_some() ==> 0 <= final(part).green <= u64::MAX,
        result.is_some() && final(part).green == 0 ==> final(part).pairs.len() == 0 && final(part).moments.len() == 0,
        result.is_some() && final(part).green > 0 ==> forall|supplier: int, field: int| 0 <= field <= 1 ==>
            #[trigger] stored_pair(*final(part), supplier, field) == stored_pair(*old(part), supplier, field),
        result.is_some() && final(part).green > 0 ==> forall|supplier: int, year: int, field: int| 0 <= field <= 2 ==>
            #[trigger] stored_moment(*final(part), supplier, year, field) == stored_moment(*old(part), supplier, year, field),
        result.is_some() ==> pairs_positive(final(part).pairs@) && moments_positive(final(part).moments@),
        old(part).pairs@.map_values(|row: Pair| row.supplier).no_duplicates() ==> final(part).pairs@.map_values(|row: Pair| row.supplier).no_duplicates(),
        old(part).moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates() ==> final(part).moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let ghost initial = *part;
    if part.green < 0 || part.green > u64::MAX as i128 { return None; }
    if part.green == 0 { part.pairs.clear(); part.moments.clear(); return Some(()); }
    let mut index = 0;
    while index < part.pairs.len()
        invariant index <= part.pairs.len(),
            initial == *old(part), part.green == initial.green, 0 < part.green <= u64::MAX,
            part.moments@ == initial.moments@,
            forall|supplier: int, field: int| 0 <= field <= 1 ==>
                #[trigger] stored_pair(*part, supplier, field) == stored_pair(initial, supplier, field),
            initial.pairs@.map_values(|row: Pair| row.supplier).no_duplicates() ==> part.pairs@.map_values(|row: Pair| row.supplier).no_duplicates(),
            forall|position: int| 0 <= position < index ==> 0 < (#[trigger] part.pairs[position]).count <= u64::MAX,
        decreases part.pairs.len() - index,
    {
        if part.pairs[index].count < 0 || part.pairs[index].count > u64::MAX as i128 { return None; }
        if part.pairs[index].count == 0 {
            if part.pairs[index].cost != 0 { return None; }
            let ghost before = *part;
            part.pairs.remove(index);
            proof {
                assert forall|supplier: int, field: int| 0 <= field <= 1 implies
                    #[trigger] stored_pair(*part, supplier, field) == stored_pair(before, supplier, field) by {
                    lemma_sum_remove(before.pairs@, index as int, |row: Pair| if row.supplier == supplier { if field == 0 { row.count as int } else { row.cost as int } } else { 0int });
                }
                if before.pairs@.map_values(|row: Pair| row.supplier).no_duplicates() {
                    assert(part.pairs@.map_values(|row: Pair| row.supplier) =~= before.pairs@.map_values(|row: Pair| row.supplier).remove(index as int));
                    verus_prooflib::lemma_unique_remove(before.pairs@.map_values(|row: Pair| row.supplier), index as int);
                }
            }
        } else { index += 1; }
    }
    let ghost clean_pairs = part.pairs@;
    index = 0;
    while index < part.moments.len()
        invariant index <= part.moments.len(),
            initial == *old(part), part.green == initial.green, 0 < part.green <= u64::MAX,
            part.pairs@ == clean_pairs, pairs_positive(part.pairs@),
            forall|supplier: int, field: int| 0 <= field <= 1 ==>
                #[trigger] stored_pair(*part, supplier, field) == stored_pair(initial, supplier, field),
            initial.pairs@.map_values(|row: Pair| row.supplier).no_duplicates() ==> part.pairs@.map_values(|row: Pair| row.supplier).no_duplicates(),
            forall|supplier: int, year: int, field: int| 0 <= field <= 2 ==>
                #[trigger] stored_moment(*part, supplier, year, field) == stored_moment(initial, supplier, year, field),
            initial.moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates() ==> part.moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates(),
            forall|position: int| 0 <= position < index ==> (#[trigger] part.moments[position]).cell.count > 0,
        decreases part.moments.len() - index,
    {
        if part.moments[index].cell.count < 0 { return None; }
        if part.moments[index].cell.count == 0 {
            if part.moments[index].cell.revenue != 0 || part.moments[index].cell.quantity != 0 { return None; }
            let ghost before = *part;
            proof {
                assert forall|supplier: int, field: int| 0 <= field <= 1 implies
                    #[trigger] stored_pair(before, supplier, field) == stored_pair(initial, supplier, field) by {}
            }
            part.moments.remove(index);
            proof {
                assert forall|supplier: int, field: int| 0 <= field <= 1 implies
                    #[trigger] stored_pair(*part, supplier, field) == stored_pair(initial, supplier, field) by {
                    assert(stored_pair(*part, supplier, field) == stored_pair(before, supplier, field));
                }
                assert forall|supplier: int, year: int, field: int| 0 <= field <= 2 implies
                    #[trigger] stored_moment(*part, supplier, year, field) == stored_moment(before, supplier, year, field) by {
                    lemma_sum_remove(before.moments@, index as int, |row: Moment| if row.supplier == supplier && row.year == year {
                        if field == 0 { row.cell.count as int } else if field == 1 { row.cell.revenue as int } else { row.cell.quantity as int }
                    } else { 0int });
                }
                if before.moments@.map_values(|row: Moment| (row.supplier, row.year)).no_duplicates() {
                    assert(part.moments@.map_values(|row: Moment| (row.supplier, row.year)) =~= before.moments@.map_values(|row: Moment| (row.supplier, row.year)).remove(index as int));
                    verus_prooflib::lemma_unique_remove(before.moments@.map_values(|row: Moment| (row.supplier, row.year)), index as int);
                }
            }
        } else { index += 1; }
    }
    Some(())
}

}
