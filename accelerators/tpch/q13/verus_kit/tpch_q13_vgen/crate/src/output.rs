use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::order::*;
use verus_common::rel::*;
use crate::facts::*;

verus! {

pub open spec fn better(left: (i64, i64), right: (i64, i64)) -> bool {
    left.1 > right.1 || (left.1 == right.1 && left.0 > right.0)
}

pub open spec fn ordered(rows: Seq<(i64, i64)>) -> bool {
    forall|first: int, second: int| #![trigger rows[first], rows[second]]
        0 <= first < second < rows.len() ==> !better(rows[second], rows[first])
}

pub fn precedes(left: (i64, i64), right: (i64, i64)) -> (result: bool)
    ensures result == better(left, right),
{ left.1 > right.1 || (left.1 == right.1 && left.0 > right.0) }

fn merge(left: &Vec<(i64, i64)>, right: &Vec<(i64, i64)>) -> (rows: Vec<(i64, i64)>)
    requires ordered(left@), ordered(right@),
    ensures ordered(rows@),
        forall|weight: spec_fn((i64, i64)) -> int| #[trigger] sum(rows@, weight)
            == sum(left@, weight) + sum(right@, weight),
{
    let mut rows: Vec<(i64, i64)> = Vec::new();
    let mut first = 0usize;
    let mut second = 0usize;
    while first < left.len() || second < right.len()
        invariant first <= left.len(), second <= right.len(), ordered(left@), ordered(right@), ordered(rows@),
            forall|index: int| 0 <= index < rows@.len() && first < left@.len() ==>
                !better(left@[first as int], #[trigger] rows@[index]),
            forall|index: int| 0 <= index < rows@.len() && second < right@.len() ==>
                !better(right@[second as int], #[trigger] rows@[index]),
            forall|weight: spec_fn((i64, i64)) -> int| #[trigger] sum(rows@, weight)
                == sum(left@.take(first as int), weight) + sum(right@.take(second as int), weight),
        decreases left.len() - first + right.len() - second,
    {
        let take_left = if first < left.len() && second < right.len() {
            precedes(left[first], right[second])
        } else { first < left.len() };
        let ghost previous = rows@;
        if take_left {
            let row = left[first];
            rows.push(row);
            proof {
                assert forall|weight: spec_fn((i64, i64)) -> int| #[trigger] sum(rows@, weight)
                    == sum(left@.take(first as int + 1), weight) + sum(right@.take(second as int), weight) by {
                    lemma_sum_push(previous, row, weight);
                    lemma_sum_take(left@, first as int, weight);
                }
                assert(ordered(rows@)) by {
                    assert forall|earlier: int, later: int| #![trigger rows@[earlier], rows@[later]]
                        0 <= earlier < later < rows@.len() implies !better(rows@[later], rows@[earlier]) by {
                        if later < previous.len() { assert(rows@[later] == previous[later]); }
                        else { assert(rows@[earlier] == previous[earlier]); }
                    }
                }
                if first + 1 < left.len() {
                    assert(!better(left@[first as int + 1], row));
                    assert forall|index: int| 0 <= index < rows@.len() implies
                        !better(left@[first as int + 1], #[trigger] rows@[index]) by {
                        if index < previous.len() { assert(!better(row, previous[index])); }
                    }
                }
                if second < right.len() {
                    assert forall|index: int| 0 <= index < rows@.len() implies
                        !better(right@[second as int], #[trigger] rows@[index]) by {}
                }
            }
            first += 1;
        } else {
            let row = right[second];
            rows.push(row);
            proof {
                assert forall|weight: spec_fn((i64, i64)) -> int| #[trigger] sum(rows@, weight)
                    == sum(left@.take(first as int), weight) + sum(right@.take(second as int + 1), weight) by {
                    lemma_sum_push(previous, row, weight);
                    lemma_sum_take(right@, second as int, weight);
                }
                assert(ordered(rows@)) by {
                    assert forall|earlier: int, later: int| #![trigger rows@[earlier], rows@[later]]
                        0 <= earlier < later < rows@.len() implies !better(rows@[later], rows@[earlier]) by {
                        if later < previous.len() { assert(rows@[later] == previous[later]); }
                        else { assert(rows@[earlier] == previous[earlier]); }
                    }
                }
                if second + 1 < right.len() {
                    assert(!better(right@[second as int + 1], row));
                    assert forall|index: int| 0 <= index < rows@.len() implies
                        !better(right@[second as int + 1], #[trigger] rows@[index]) by {
                        if index < previous.len() { assert(!better(row, previous[index])); }
                    }
                }
                if first < left.len() {
                    assert forall|index: int| 0 <= index < rows@.len() implies
                        !better(left@[first as int], #[trigger] rows@[index]) by {}
                }
            }
            second += 1;
        }
    }
    proof {
        assert(left@.take(left@.len() as int) =~= left@);
        assert(right@.take(right@.len() as int) =~= right@);
    }
    rows
}

fn sort_range(source: &Vec<(i64, i64)>, start: usize, end: usize) -> (rows: Vec<(i64, i64)>)
    requires start <= end <= source.len(),
    ensures ordered(rows@),
        forall|weight: spec_fn((i64, i64)) -> int| #[trigger] sum(rows@, weight)
            == sum(source@.subrange(start as int, end as int), weight),
    decreases end - start,
{
    if end - start <= 1 {
        let mut rows = Vec::new();
        if end > start { rows.push(source[start]); }
        proof { assert(rows@ =~= source@.subrange(start as int, end as int)); }
        return rows;
    }
    let middle = start + (end - start) / 2;
    let left = sort_range(source, start, middle);
    let right = sort_range(source, middle, end);
    let rows = merge(&left, &right);
    proof {
        assert forall|weight: spec_fn((i64, i64)) -> int| #[trigger] sum(rows@, weight)
            == sum(source@.subrange(start as int, end as int), weight) by {
            assert(source@.subrange(start as int, end as int) =~=
                source@.subrange(start as int, middle as int) + source@.subrange(middle as int, end as int));
            lemma_sum_concat(source@.subrange(start as int, middle as int),
                source@.subrange(middle as int, end as int), weight);
        }
    }
    rows
}

pub fn sort(rows: &mut Vec<(i64, i64)>)
    requires no_dups(old(rows)@),
    ensures final(rows)@.to_multiset() == old(rows)@.to_multiset(),
        forall|row: (i64, i64)| #[trigger] final(rows)@.contains(row) == old(rows)@.contains(row),
        sorted_by(final(rows)@, |left: (i64, i64), right: (i64, i64)| better(left, right)),
{
    let ghost original = rows@;
    let sorted = sort_range(rows, 0, rows.len());
    proof {
        assert(rows@.subrange(0, rows@.len() as int) =~= rows@);
        lemma_sums_eq_multiset(sorted@, rows@);
        lemma_no_dups_bag(original, sorted@);
        assert forall|first: int, second: int| #![trigger sorted@[first], sorted@[second]]
            0 <= first < second < sorted@.len() implies better(sorted@[first], sorted@[second]) by {}
        assert forall|row: (i64, i64)| #[trigger] sorted@.contains(row) == original.contains(row) by {
            lemma_bag_contains(sorted@, row);
            lemma_bag_contains(original, row);
        }
    }
    *rows = sorted;
}

}
