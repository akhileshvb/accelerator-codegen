use vstd::prelude::*;
use std::sync::Arc;
use crate::storage::*;
use crate::model::{patched_count, patched_name, sparse_group, patched_answer, patched_counts_unique, patched_names_unique,
    counts_positive, names_positive, patched_counts_positive, patched_names_positive, groups_shape, sparse_groups_shape,
    answers_shape, patched_answers_shape};
use verus_common::sums::*;

macro_rules! value {
    ($expression:expr) => { match $expression { Some(value) => value, None => return None } };
}
pub(crate) use value;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub struct Counts { pub rows: Vec<(i64, i128)> }
pub struct Names { pub rows: Vec<(Vec<u8>, i128)> }
#[derive(Clone, Copy)]
pub struct Cell { pub other: i64, pub year: i64, pub count: i128, pub revenue: i128 }
pub struct Groups { pub rows: Vec<Cell> }
pub struct Answer {
    pub first: Vec<u8>, pub second: Vec<u8>, pub year: i64,
    pub count: i128, pub revenue: i128,
}
pub struct Answers { pub rows: Vec<Answer> }

pub open spec fn count_keys(rows: Seq<(i64, i128)>) -> Seq<i64> {
    rows.map_values(|entry: (i64, i128)| entry.0)
}

pub open spec fn name_keys(rows: Seq<(Vec<u8>, i128)>) -> Seq<Seq<u8>> {
    rows.map_values(|entry: (Vec<u8>, i128)| entry.0@)
}

pub open spec fn group_keys(rows: Seq<Cell>) -> Seq<(i64, i64)> {
    rows.map_values(|entry: Cell| (entry.other, entry.year))
}

pub open spec fn answer_keys(rows: Seq<Answer>) -> Seq<(Seq<u8>, Seq<u8>, i64)> {
    rows.map_values(|entry: Answer| (entry.first@, entry.second@, entry.year))
}

impl Bucket for Counts {
    open spec fn empty(self) -> bool { self.rows@.len() == 0 }
    fn is_empty(&self) -> (result: bool) ensures result == self.empty(), { self.rows.len() == 0 }
}
impl Bucket for Names {
    open spec fn empty(self) -> bool { self.rows@.len() == 0 }
    fn is_empty(&self) -> (result: bool) ensures result == self.empty(), { self.rows.len() == 0 }
}
impl Bucket for Groups {
    open spec fn empty(self) -> bool { self.rows@.len() == 0 }
    fn is_empty(&self) -> (result: bool) ensures result == self.empty(), { self.rows.len() == 0 }
}
impl Bucket for Answers {
    open spec fn empty(self) -> bool { self.rows@.len() == 0 }
    fn is_empty(&self) -> (result: bool) ensures result == self.empty(), { self.rows.len() == 0 }
}

pub open spec fn sparse_count(map: Sparse<Counts>, key: i64, other: i64) -> int {
    match map.at(key) { Some(bucket) => count_at(bucket.rows@, other), None => 0 }
}
pub open spec fn sparse_name(map: Sparse<Names>, key: i64, name: Seq<u8>) -> int {
    match map.at(key) { Some(bucket) => name_at(bucket.rows@, name), None => 0 }
}

pub open spec fn count_weight(key: i64) -> spec_fn((i64, i128)) -> int {
    |entry: (i64, i128)| if entry.0 == key { entry.1 as int } else { 0 }
}
pub open spec fn count_at(rows: Seq<(i64, i128)>, key: i64) -> int {
    verus_common::sums::sum(rows, count_weight(key))
}
pub open spec fn name_at(rows: Seq<(Vec<u8>, i128)>, name: Seq<u8>) -> int {
    verus_common::sums::sum(rows, |entry: (Vec<u8>, i128)| if entry.0@ == name { entry.1 as int } else { 0 })
}
pub open spec fn group_weight(other: i64, year: i64, revenue: bool) -> spec_fn(Cell) -> int {
    |entry: Cell| if entry.other == other && entry.year == year {
        if revenue { entry.revenue as int } else { entry.count as int }
    } else { 0 }
}
pub open spec fn group_at(rows: Seq<Cell>, other: i64, year: i64, revenue: bool) -> int {
    verus_common::sums::sum(rows, group_weight(other, year, revenue))
}
pub open spec fn answer_weight(first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> spec_fn(Answer) -> int {
    |entry: Answer| if entry.first@ == first && entry.second@ == second && entry.year == year {
        if revenue { entry.revenue as int } else { entry.count as int }
    } else { 0 }
}
pub open spec fn answer_at(rows: Seq<Answer>, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int {
    verus_common::sums::sum(rows, answer_weight(first, second, year, revenue))
}

pub fn copy_bytes(bytes: &[u8]) -> (result: Vec<u8>)
    ensures result@ == bytes@,
{
    let mut result = Vec::new();
    let mut offset = 0;
    while offset < bytes.len()
        invariant offset <= bytes@.len(), result@ == bytes@.take(offset as int),
        decreases bytes@.len() - offset,
    { result.push(bytes[offset]); offset += 1; }
    result
}

pub fn bytes_equal(left: &[u8], right: &[u8]) -> (result: bool)
    ensures result == (left@ == right@),
{
    if left.len() != right.len() { return false; }
    let mut offset = 0;
    while offset < left.len()
        invariant offset <= left@.len(), left@.len() == right@.len(),
            left@.take(offset as int) == right@.take(offset as int),
        decreases left@.len() - offset,
    { if left[offset] != right[offset] { return false; } offset += 1; }
    proof { assert(left@.take(left@.len() as int) == left@); assert(right@.take(right@.len() as int) == right@); }
    true
}

#[verifier::opaque]
pub open spec fn hash_prefix(bytes: Seq<u8>) -> int
    decreases bytes.len(),
{
    if bytes.len() == 0 { 0 }
    else { (hash_prefix(bytes.drop_last()) * 257 + bytes.last() as int + 1) % 1000000007 }
}

pub fn name_hash(bytes: &[u8]) -> (result: i64)
    ensures result as int == hash_prefix(bytes@), 0 <= result < 1000000007,
{
    proof { reveal(hash_prefix); }
    let mut result: i64 = 0;
    let mut offset = 0;
    while offset < bytes.len()
        invariant offset <= bytes@.len(), 0 <= result < 1000000007,
            result as int == hash_prefix(bytes@.take(offset as int)),
        decreases bytes@.len() - offset,
    {
        proof {
            reveal(hash_prefix);
            assert(bytes@.take(offset as int + 1).drop_last() == bytes@.take(offset as int));
            assert(bytes@.take(offset as int + 1).last() == bytes@[offset as int]);
        }
        result = (result * 257 + bytes[offset] as i64 + 1) % 1000000007;
        offset += 1;
    }
    proof { assert(bytes@.take(bytes@.len() as int) == bytes@); }
    result
}

pub open spec fn pair_hash_spec(first: Seq<u8>, second: Seq<u8>) -> int {
    (hash_prefix(first) * 257 + hash_prefix(second)) % 1000000007
}

pub fn pair_hash(first: &[u8], second: &[u8]) -> (result: i64)
    ensures result as int == pair_hash_spec(first@, second@), 0 <= result < 1000000007,
{
    let first_hash = name_hash(first);
    let second_hash = name_hash(second);
    (first_hash * 257 + second_hash) % 1000000007
}

pub fn copy_counts(source: &Counts) -> (result: Counts)
    ensures result.rows@ == source.rows@,
{
    let mut rows = Vec::new();
    let mut offset = 0;
    while offset < source.rows.len()
        invariant offset <= source.rows@.len(), rows@ == source.rows@.take(offset as int),
        decreases source.rows@.len() - offset,
    { rows.push(source.rows[offset]); offset += 1; }
    Counts { rows }
}

pub fn adjust_count(bucket: &mut Counts, key: i64, change: i128) -> (result: Option<()>)
    ensures result.is_some() ==> forall|other: i64| #![trigger count_at(final(bucket).rows@, other)]
        count_at(final(bucket).rows@, other) == count_at(old(bucket).rows@, other) + if other == key { change as int } else { 0 },
        count_keys(old(bucket).rows@).no_duplicates() ==> count_keys(final(bucket).rows@).no_duplicates(),
        counts_positive(*old(bucket)) && change > 0 ==> counts_positive(*final(bucket)),
{
    let ghost before = bucket.rows@;
    let mut offset = 0;
    while offset < bucket.rows.len()
        invariant offset <= bucket.rows@.len(), bucket.rows@ == before, before == old(bucket).rows@,
            forall|position: int| #![trigger before[position]] 0 <= position < offset ==> before[position].0 != key,
        decreases bucket.rows@.len() - offset,
    {
        if bucket.rows[offset].0 == key {
            let count = value!(bucket.rows[offset].1.checked_add(change));
            if count == 0 { bucket.rows.remove(offset); } else { bucket.rows.set(offset, (key, count)); }
            proof {
                if count_keys(before).no_duplicates() {
                    if count == 0 {
                        assert(count_keys(bucket.rows@) =~= count_keys(before).remove(offset as int));
                        verus_prooflib::lemma_unique_remove(count_keys(before), offset as int);
                    } else {
                        assert(count_keys(bucket.rows@) =~= count_keys(before));
                    }
                }
                assert forall|other: i64| #![trigger count_at(bucket.rows@, other)]
                    count_at(bucket.rows@, other) == count_at(before, other) + if other == key { change as int } else { 0 } by {
                    let weight = |entry: (i64, i128)| if entry.0 == other { entry.1 as int } else { 0int };
                    if count == 0 { lemma_sum_remove(before, offset as int, weight); }
                    else { lemma_sum_update(before, offset as int, (key, count), weight); }
                }
            }
            return Some(());
        }
        offset += 1;
    }
    if change != 0 { bucket.rows.push((key, change)); }
    proof {
        if count_keys(before).no_duplicates() && change != 0 {
            assert(!count_keys(before).contains(key));
            assert(count_keys(bucket.rows@) =~= count_keys(before).push(key));
            verus_prooflib::lemma_unique_push(count_keys(before), key);
        }
        assert forall|other: i64| #![trigger count_at(bucket.rows@, other)]
            count_at(bucket.rows@, other) == count_at(before, other) + if other == key { change as int } else { 0 } by {
            if change != 0 {
                lemma_sum_push(before, (key, change), |entry: (i64, i128)| if entry.0 == other { entry.1 as int } else { 0int });
            }
        }
    }
    Some(())
}

pub fn count_patch(previous: &Layers<Counts>, patch: &mut Sparse<Counts>, key: i64, other: i64, change: i128) -> (result: Option<()>)
    requires old(patch).valid(),
    ensures final(patch).valid(), result.is_some() ==> forall|target: i64, variant: i64|
        #![trigger patched_count(*previous, *final(patch), target, variant)]
        patched_count(*previous, *final(patch), target, variant) == patched_count(*previous, *old(patch), target, variant)
            + if target == key && variant == other { change as int } else { 0 },
        forall|target: i64| #![trigger patched_counts_unique(*previous, *final(patch), target)]
            patched_counts_unique(*previous, *old(patch), target) ==> patched_counts_unique(*previous, *final(patch), target),
        forall|target: i64| #![trigger patched_counts_positive(*previous, *final(patch), target)]
            patched_counts_positive(*previous, *old(patch), target) && change > 0
                ==> patched_counts_positive(*previous, *final(patch), target),
{
    let mut bucket = match patch.get(key) {
        Some(bucket) => copy_counts(bucket),
        None => match previous.get(key) { Some(bucket) => copy_counts(bucket), None => Counts { rows: Vec::new() } },
    };
    value!(adjust_count(&mut bucket, other, change));
    patch.put(key, Arc::new(bucket));
    Some(())
}

pub fn copy_names(source: &Names) -> (result: Names)
    ensures result.rows@.len() == source.rows@.len(),
        forall|offset: int| #![trigger result.rows@[offset]] 0 <= offset < result.rows@.len() ==>
            result.rows@[offset].0@ == source.rows@[offset].0@ && result.rows@[offset].1 == source.rows@[offset].1,
        forall|name: Seq<u8>| #![trigger name_at(result.rows@, name)] name_at(result.rows@, name) == name_at(source.rows@, name),
        name_keys(result.rows@) == name_keys(source.rows@),
{
    let mut rows: Vec<(Vec<u8>, i128)> = Vec::new();
    let mut offset = 0;
    while offset < source.rows.len()
        invariant offset <= source.rows@.len(), rows@.len() == offset,
            forall|index: int| #![trigger rows@[index]] 0 <= index < offset ==>
                rows@[index].0@ == source.rows@[index].0@ && rows@[index].1 == source.rows@[index].1,
        decreases source.rows@.len() - offset,
    {
        rows.push((copy_bytes(&source.rows[offset].0), source.rows[offset].1));
        offset += 1;
    }
    proof {
        assert(name_keys(rows@) =~= name_keys(source.rows@));
        assert forall|name: Seq<u8>| #![trigger name_at(rows@, name)] name_at(rows@, name) == name_at(source.rows@, name) by {
            verus_prooflib::lemma_sum_aligned(rows@, source.rows@,
                |entry: (Vec<u8>, i128)| if entry.0@ == name { entry.1 as int } else { 0int },
                |entry: (Vec<u8>, i128)| if entry.0@ == name { entry.1 as int } else { 0int });
        }
    }
    Names { rows }
}

pub fn adjust_name(bucket: &mut Names, name: &[u8], change: i128) -> (result: Option<()>)
    ensures result.is_some() ==> forall|variant: Seq<u8>| #![trigger name_at(final(bucket).rows@, variant)]
        name_at(final(bucket).rows@, variant) == name_at(old(bucket).rows@, variant)
            + if variant == name@ { change as int } else { 0 },
        name_keys(old(bucket).rows@).no_duplicates() ==> name_keys(final(bucket).rows@).no_duplicates(),
        names_positive(*old(bucket)) && change > 0 ==> names_positive(*final(bucket)),
{
    let ghost before = bucket.rows@;
    let mut offset = 0;
    while offset < bucket.rows.len()
        invariant offset <= bucket.rows@.len(), bucket.rows@ == before, before == old(bucket).rows@,
            forall|position: int| #![trigger before[position]] 0 <= position < offset ==> before[position].0@ != name@,
        decreases bucket.rows@.len() - offset,
    {
        if bytes_equal(&bucket.rows[offset].0, name) {
            let count = value!(bucket.rows[offset].1.checked_add(change));
            if count == 0 { bucket.rows.remove(offset); }
            else { bucket.rows.set(offset, (copy_bytes(name), count)); }
            proof {
                if name_keys(before).no_duplicates() {
                    if count == 0 {
                        assert(name_keys(bucket.rows@) =~= name_keys(before).remove(offset as int));
                        verus_prooflib::lemma_unique_remove(name_keys(before), offset as int);
                    } else { assert(name_keys(bucket.rows@) =~= name_keys(before)); }
                }
                assert forall|variant: Seq<u8>| #![trigger name_at(bucket.rows@, variant)]
                    name_at(bucket.rows@, variant) == name_at(before, variant) + if variant == name@ { change as int } else { 0 } by {
                    let weight = |entry: (Vec<u8>, i128)| if entry.0@ == variant { entry.1 as int } else { 0int };
                    if count == 0 { lemma_sum_remove(before, offset as int, weight); }
                    else { lemma_sum_update(before, offset as int, bucket.rows@[offset as int], weight); }
                }
            }
            return Some(());
        }
        offset += 1;
    }
    if change != 0 { bucket.rows.push((copy_bytes(name), change)); }
    proof {
        if name_keys(before).no_duplicates() && change != 0 {
            assert(!name_keys(before).contains(name@));
            assert(name_keys(bucket.rows@) =~= name_keys(before).push(name@));
            verus_prooflib::lemma_unique_push(name_keys(before), name@);
        }
        assert forall|variant: Seq<u8>| #![trigger name_at(bucket.rows@, variant)]
            name_at(bucket.rows@, variant) == name_at(before, variant) + if variant == name@ { change as int } else { 0 } by {
            if change != 0 { lemma_sum_push(before, bucket.rows@.last(),
                |entry: (Vec<u8>, i128)| if entry.0@ == variant { entry.1 as int } else { 0int }); }
        }
    }
    Some(())
}

pub fn name_patch(previous: &Layers<Names>, patch: &mut Sparse<Names>, key: i64, name: &[u8], change: i128) -> (result: Option<()>)
    requires old(patch).valid(),
    ensures final(patch).valid(), result.is_some() ==> forall|target: i64, variant: Seq<u8>|
        #![trigger patched_name(*previous, *final(patch), target, variant)]
        patched_name(*previous, *final(patch), target, variant) == patched_name(*previous, *old(patch), target, variant)
            + if target == key && variant == name@ { change as int } else { 0 },
        forall|target: i64| #![trigger patched_names_unique(*previous, *final(patch), target)]
            patched_names_unique(*previous, *old(patch), target) ==> patched_names_unique(*previous, *final(patch), target),
        forall|target: i64| #![trigger patched_names_positive(*previous, *final(patch), target)]
            patched_names_positive(*previous, *old(patch), target) && change > 0
                ==> patched_names_positive(*previous, *final(patch), target),
{
    let mut bucket = match patch.get(key) {
        Some(bucket) => copy_names(bucket),
        None => match previous.get(key) { Some(bucket) => copy_names(bucket), None => Names { rows: Vec::new() } },
    };
    value!(adjust_name(&mut bucket, name, change));
    patch.put(key, Arc::new(bucket));
    Some(())
}

pub fn copy_groups(source: &Groups) -> (result: Groups)
    ensures result.rows@ == source.rows@,
{
    let mut rows = Vec::new();
    let mut offset = 0;
    while offset < source.rows.len()
        invariant offset <= source.rows@.len(), rows@ == source.rows@.take(offset as int),
        decreases source.rows@.len() - offset,
    { rows.push(source.rows[offset]); offset += 1; }
    Groups { rows }
}

pub fn adjust_group(bucket: &mut Groups, other: i64, year: i64, count: i128, revenue: i128) -> (result: Option<()>)
    ensures result.is_some() ==> forall|target: i64, target_year: i64, measure: bool|
        #![trigger group_at(final(bucket).rows@, target, target_year, measure)]
        group_at(final(bucket).rows@, target, target_year, measure)
        == group_at(old(bucket).rows@, target, target_year, measure)
            + if target == other && target_year == year { if measure { revenue as int } else { count as int } } else { 0 },
        groups_shape(*old(bucket)) && (year == 1995 || year == 1996) ==> groups_shape(*final(bucket)),
{
    let ghost before = bucket.rows@;
    let mut offset = 0;
    while offset < bucket.rows.len()
        invariant offset <= bucket.rows@.len(), bucket.rows@ == before, before == old(bucket).rows@,
            forall|position: int| #![trigger before[position]] 0 <= position < offset ==>
                (before[position].other, before[position].year) != (other, year),
        decreases bucket.rows@.len() - offset,
    {
        let cell = bucket.rows[offset];
        if cell.other == other && cell.year == year {
            let total_count = value!(cell.count.checked_add(count));
            let total_revenue = value!(cell.revenue.checked_add(revenue));
            if total_count == 0 && total_revenue == 0 { bucket.rows.remove(offset); }
            else { bucket.rows.set(offset, Cell { other, year, count: total_count, revenue: total_revenue }); }
            proof {
                if group_keys(before).no_duplicates() {
                    if total_count == 0 && total_revenue == 0 {
                        assert(group_keys(bucket.rows@) =~= group_keys(before).remove(offset as int));
                        verus_prooflib::lemma_unique_remove(group_keys(before), offset as int);
                    } else { assert(group_keys(bucket.rows@) =~= group_keys(before)); }
                }
                assert forall|target: i64, target_year: i64, measure: bool|
                    #![trigger group_at(bucket.rows@, target, target_year, measure)]
                    group_at(bucket.rows@, target, target_year, measure)
                    == group_at(before, target, target_year, measure)
                        + if target == other && target_year == year { if measure { revenue as int } else { count as int } } else { 0 } by {
                    let weight = |entry: Cell| if entry.other == target && entry.year == target_year {
                        if measure { entry.revenue as int } else { entry.count as int }
                    } else { 0int };
                    if total_count == 0 && total_revenue == 0 { lemma_sum_remove(before, offset as int, weight); }
                    else { lemma_sum_update(before, offset as int,
                        Cell { other, year, count: total_count, revenue: total_revenue }, weight); }
                }
            }
            return Some(());
        }
        offset += 1;
    }
    if count != 0 || revenue != 0 { bucket.rows.push(Cell { other, year, count, revenue }); }
    proof {
        if group_keys(before).no_duplicates() && (count != 0 || revenue != 0) {
            assert(!group_keys(before).contains((other, year)));
            assert(group_keys(bucket.rows@) =~= group_keys(before).push((other, year)));
            verus_prooflib::lemma_unique_push(group_keys(before), (other, year));
        }
        assert forall|target: i64, target_year: i64, measure: bool|
            #![trigger group_at(bucket.rows@, target, target_year, measure)]
            group_at(bucket.rows@, target, target_year, measure)
            == group_at(before, target, target_year, measure)
                + if target == other && target_year == year { if measure { revenue as int } else { count as int } } else { 0 } by {
            if count != 0 || revenue != 0 { lemma_sum_push(before, Cell { other, year, count, revenue },
                |entry: Cell| if entry.other == target && entry.year == target_year {
                    if measure { entry.revenue as int } else { entry.count as int }
                } else { 0int }); }
        }
    }
    Some(())
}

pub fn group_delta(changes: &mut Sparse<Groups>, first: i64, second: i64, year: i64, count: i128, revenue: i128) -> (result: Option<()>)
    requires old(changes).valid(),
    ensures final(changes).valid(), result.is_some() ==> forall|target: i64, other: i64, target_year: i64, measure: bool|
        #![trigger sparse_group(*final(changes), target, other, target_year, measure)]
        sparse_group(*final(changes), target, other, target_year, measure)
        == sparse_group(*old(changes), target, other, target_year, measure)
            + if target == first && other == second && target_year == year { if measure { revenue as int } else { count as int } } else { 0 },
        sparse_groups_shape(*old(changes)) && (year == 1995 || year == 1996) ==> sparse_groups_shape(*final(changes)),
{
    let mut bucket = match changes.get(first) { Some(bucket) => copy_groups(bucket), None => Groups { rows: Vec::new() } };
    value!(adjust_group(&mut bucket, second, year, count, revenue));
    changes.put(first, Arc::new(bucket));
    Some(())
}

pub fn copy_answers(source: &Answers) -> (result: Answers)
    ensures result.rows@.len() == source.rows@.len(),
        forall|offset: int| #![trigger result.rows@[offset]] 0 <= offset < result.rows@.len() ==> {
            &&& result.rows@[offset].first@ == source.rows@[offset].first@
            &&& result.rows@[offset].second@ == source.rows@[offset].second@
            &&& result.rows@[offset].year == source.rows@[offset].year
            &&& result.rows@[offset].count == source.rows@[offset].count
            &&& result.rows@[offset].revenue == source.rows@[offset].revenue
        },
        forall|first: Seq<u8>, second: Seq<u8>, year: i64, measure: bool|
            #![trigger answer_at(result.rows@, first, second, year, measure)]
            answer_at(result.rows@, first, second, year, measure) == answer_at(source.rows@, first, second, year, measure),
        answer_keys(result.rows@) == answer_keys(source.rows@),
{
    let mut rows: Vec<Answer> = Vec::new();
    let mut offset = 0;
    while offset < source.rows.len()
        invariant offset <= source.rows@.len(), rows@.len() == offset,
            forall|index: int| #![trigger rows@[index]] 0 <= index < offset ==> {
                &&& rows@[index].first@ == source.rows@[index].first@
                &&& rows@[index].second@ == source.rows@[index].second@
                &&& rows@[index].year == source.rows@[index].year
                &&& rows@[index].count == source.rows@[index].count
                &&& rows@[index].revenue == source.rows@[index].revenue
            },
        decreases source.rows@.len() - offset,
    {
        let row = &source.rows[offset];
        rows.push(Answer { first: copy_bytes(&row.first), second: copy_bytes(&row.second),
            year: row.year, count: row.count, revenue: row.revenue });
        offset += 1;
    }
    proof {
        assert(answer_keys(rows@) =~= answer_keys(source.rows@));
        assert forall|first: Seq<u8>, second: Seq<u8>, year: i64, measure: bool|
            #![trigger answer_at(rows@, first, second, year, measure)]
            answer_at(rows@, first, second, year, measure) == answer_at(source.rows@, first, second, year, measure) by {
            let weight = |entry: Answer| if entry.first@ == first && entry.second@ == second && entry.year == year {
                if measure { entry.revenue as int } else { entry.count as int }
            } else { 0int };
            verus_prooflib::lemma_sum_aligned(rows@, source.rows@, weight, weight);
        }
    }
    Answers { rows }
}

pub fn adjust_answer(bucket: &mut Answers, first: &[u8], second: &[u8], year: i64, count: i128, revenue: i128) -> (result: Option<()>)
    ensures result.is_some() ==> forall|left: Seq<u8>, right: Seq<u8>, target_year: i64, measure: bool|
        #![trigger answer_at(final(bucket).rows@, left, right, target_year, measure)]
        answer_at(final(bucket).rows@, left, right, target_year, measure)
            == answer_at(old(bucket).rows@, left, right, target_year, measure)
                + if left == first@ && right == second@ && target_year == year {
                    if measure { revenue as int } else { count as int }
                } else { 0int },
        forall|key: i64| #![trigger answers_shape(*final(bucket), key)]
            answers_shape(*old(bucket), key) && (year == 1995 || year == 1996)
                && pair_hash_spec(first@, second@) == key ==> answers_shape(*final(bucket), key),
{
    let ghost before = bucket.rows@;
    let mut offset = 0;
    while offset < bucket.rows.len()
        invariant offset <= bucket.rows@.len(), bucket.rows@ == before, before == old(bucket).rows@,
            forall|position: int| #![trigger before[position]] 0 <= position < offset ==>
                (before[position].first@, before[position].second@, before[position].year) != (first@, second@, year),
        decreases bucket.rows@.len() - offset,
    {
        let row = &bucket.rows[offset];
        if row.year == year && bytes_equal(&row.first, first) && bytes_equal(&row.second, second) {
            let total_count = value!(row.count.checked_add(count));
            let total_revenue = value!(row.revenue.checked_add(revenue));
            if total_count == 0 && total_revenue == 0 { bucket.rows.remove(offset); }
            else { bucket.rows.set(offset, Answer { first: copy_bytes(first), second: copy_bytes(second), year,
                count: total_count, revenue: total_revenue }); }
            proof {
                if answer_keys(before).no_duplicates() {
                    if total_count == 0 && total_revenue == 0 {
                        assert(answer_keys(bucket.rows@) =~= answer_keys(before).remove(offset as int));
                        verus_prooflib::lemma_unique_remove(answer_keys(before), offset as int);
                    } else { assert(answer_keys(bucket.rows@) =~= answer_keys(before)); }
                }
                assert forall|left: Seq<u8>, right: Seq<u8>, target_year: i64, measure: bool|
                    #![trigger answer_at(bucket.rows@, left, right, target_year, measure)]
                    answer_at(bucket.rows@, left, right, target_year, measure)
                        == answer_at(before, left, right, target_year, measure)
                            + if left == first@ && right == second@ && target_year == year {
                                if measure { revenue as int } else { count as int }
                            } else { 0int } by {
                    let weight = |entry: Answer| if entry.first@ == left && entry.second@ == right && entry.year == target_year {
                        if measure { entry.revenue as int } else { entry.count as int }
                    } else { 0int };
                    if total_count == 0 && total_revenue == 0 { lemma_sum_remove(before, offset as int, weight); }
                    else { lemma_sum_update(before, offset as int, bucket.rows@[offset as int], weight); }
                }
            }
            return Some(());
        }
        offset += 1;
    }
    if count != 0 || revenue != 0 {
        bucket.rows.push(Answer { first: copy_bytes(first), second: copy_bytes(second), year, count, revenue });
    }
    proof {
        if answer_keys(before).no_duplicates() && (count != 0 || revenue != 0) {
            assert(!answer_keys(before).contains((first@, second@, year)));
            assert(answer_keys(bucket.rows@) =~= answer_keys(before).push((first@, second@, year)));
            verus_prooflib::lemma_unique_push(answer_keys(before), (first@, second@, year));
        }
        assert forall|left: Seq<u8>, right: Seq<u8>, target_year: i64, measure: bool|
            #![trigger answer_at(bucket.rows@, left, right, target_year, measure)]
            answer_at(bucket.rows@, left, right, target_year, measure)
                == answer_at(before, left, right, target_year, measure)
                    + if left == first@ && right == second@ && target_year == year {
                        if measure { revenue as int } else { count as int }
                    } else { 0int } by {
            if count != 0 || revenue != 0 {
                lemma_sum_push(before, bucket.rows@.last(), |entry: Answer|
                    if entry.first@ == left && entry.second@ == right && entry.year == target_year {
                        if measure { entry.revenue as int } else { entry.count as int }
                    } else { 0int });
            }
        }
    }
    Some(())
}

pub fn answer_delta(previous: &Layers<Answers>, changes: &mut Sparse<Answers>, first: &[u8], second: &[u8], year: i64, count: i128, revenue: i128) -> (result: Option<()>)
    requires old(changes).valid(),
    ensures final(changes).valid(), result.is_some() ==> forall|left: Seq<u8>, right: Seq<u8>, target_year: i64, measure: bool|
        #![trigger patched_answer(*previous, *final(changes), left, right, target_year, measure)]
        patched_answer(*previous, *final(changes), left, right, target_year, measure)
            == patched_answer(*previous, *old(changes), left, right, target_year, measure)
                + if left == first@ && right == second@ && target_year == year {
                if measure { revenue as int } else { count as int }
            } else { 0int },
        patched_answers_shape(*previous, *old(changes)) && (year == 1995 || year == 1996)
            ==> patched_answers_shape(*previous, *final(changes)),
{
    let key = pair_hash(first, second);
    let mut bucket = match changes.get(key) {
        Some(bucket) => copy_answers(bucket),
        None => match previous.get(key) { Some(bucket) => copy_answers(bucket), None => Answers { rows: Vec::new() } },
    };
    value!(adjust_answer(&mut bucket, first, second, year, count, revenue));
    changes.put(key, Arc::new(bucket));
    Some(())
}

}
