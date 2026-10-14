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
pub struct Pair { pub supplier: i64, pub count: i128, pub cost: i128 }
#[derive(Clone, Copy)]
pub struct Year { pub year: i64, pub count: i128 }
#[derive(Clone, Copy)]
pub struct Number { pub key: i64, pub count: i128 }
pub struct Name { pub bytes: Arc<Vec<u8>>, pub count: i128 }
#[derive(Clone, Copy)]
pub struct Total { pub year: i64, pub count: i128, pub profit: i128 }
pub struct Group { pub name: Arc<Vec<u8>>, pub totals: Vec<Total> }
/// One part-name class: the tracked patterns it matches (bit c of `mask`) and how many rows.
#[derive(Clone, Copy)]
pub struct MaskRow { pub mask: u128, pub count: i128 }
/// One cell of the rollup of a tracked pattern: nation key, year, tuple count, profit sum.
#[derive(Clone, Copy)]
pub struct RCell { pub nk: i64, pub year: i64, pub count: i128, pub profit: i128 }
/// A weight on a year.
#[derive(Clone, Copy)]
pub struct YearW { pub year: i64, pub weight: i128 }

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


}
