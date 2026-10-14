#![verifier::loop_isolation(false)]
use vstd::prelude::*;
use verus_common::bytes::*;
use verus_common::sums::*;
use crate::data::copy_bytes;
use crate::gen_spec::*;
use crate::gen_obligations::out_view;

verus! {

pub proof fn permutation_facts<A>(out: Seq<A>, source: Seq<A>)
    requires forall|weight: spec_fn(A) -> int| #[trigger] sum(out, weight) == sum(source, weight),
    ensures forall|value: A| #[trigger] out.contains(value) <==> source.contains(value),
        source.no_duplicates() ==> out.no_duplicates(),
{
    broadcast use vstd::seq_lib::group_to_multiset_ensures;
    lemma_sums_eq_multiset(out, source);
    assert forall|value: A| #[trigger] out.contains(value) <==> source.contains(value) by {
        let weight = eqv(value);
        if out.contains(value) {
            let found = choose|position: int| 0 <= position < out.len() && #[trigger] out[position] == value;
            lemma_sum_pos(out, weight, found);
            assert(sum(source, weight) == sum(out, weight));
            let witness = lemma_sum_nonzero_witness(source, weight);
            assert(source[witness] == value);
            assert(source.contains(value));
        }
        if source.contains(value) {
            let found = choose|position: int| 0 <= position < source.len() && #[trigger] source[position] == value;
            lemma_sum_pos(source, weight, found);
            assert(sum(source, weight) == sum(out, weight));
            let witness = lemma_sum_nonzero_witness(out, weight);
            assert(out[witness] == value);
            assert(out.contains(value));
        }
    }
    if source.no_duplicates() {
        assert forall|left: int, right: int| 0 <= left < right < out.len() implies
            #[trigger] out[left] != #[trigger] out[right] by {
            if out[left] == out[right] {
                let value = out[left];
                let weight = eqv(value);
                let remaining = out.remove(left);
                assert(remaining[right - 1] == value);
                lemma_sum_pos(remaining, weight, right - 1);
                lemma_sum_remove(out, left, weight);
                assert(out.contains(value));
                assert(source.contains(value));
                let found = choose|position: int| 0 <= position < source.len() && #[trigger] source[position] == value;
                lemma_sum_single_match(source, weight, found);
                assert(sum(out, weight) == sum(source, weight));
            }
        }
    }
}

pub type Row = (Vec<u8>, i64, i64, i32, i128, i128);

pub open spec fn row_view(row: Row) -> OutRow {
    OutRow { c_name: row.0@, c_custkey: row.1 as int, o_orderkey: row.2 as int,
        o_orderdate: row.3 as int, o_totalprice: row.4 as int, sum_qty: row.5 as int }
}

pub open spec fn increasing(values: Seq<i128>) -> bool {
    forall|left: int, right: int| 0 <= left < right < values.len() ==>
        #[trigger] values[left] <= #[trigger] values[right]
}

pub fn earlier(left: &Row, right: &Row) -> (out: bool)
    ensures out == !ext_lt(row_view(*right), row_view(*left)),
        out ==> !row_lt(row_view(*right), row_view(*left)),
        !out ==> !row_lt(row_view(*left), row_view(*right)),
{
    if left.4 != right.4 { return left.4 > right.4; }
    if left.3 != right.3 { return left.3 < right.3; }
    if !bytes_eq_exec(&left.0, &right.0) {
        proof {
            lemma_bytes_lt_asym(left.0@, right.0@);
            lemma_bytes_lt_total(left.0@, right.0@);
        }
        return bytes_lt_exec(&left.0, &right.0);
    }
    proof { lemma_bytes_lt_irrefl(left.0@); }
    if left.1 != right.1 { return left.1 < right.1; }
    left.2 <= right.2
}

pub open spec fn interval(low: usize, high: usize) -> Seq<usize> {
    Seq::new((high as int - low as int) as nat, |position: int| (low as int + position) as usize)
}

pub proof fn interval_members(low: usize, high: usize)
    requires low <= high,
    ensures forall|value: usize| #[trigger] interval(low, high).contains(value) <==> low <= value < high,
{
    assert forall|value: usize| #[trigger] interval(low, high).contains(value) <==> low <= value < high by {
        if interval(low, high).contains(value) {
            let found = choose|position: int| 0 <= position < interval(low, high).len() && #[trigger] interval(low, high)[position] == value;
        }
        if low <= value < high { assert(interval(low, high)[value as int - low as int] == value); }
    }
}

pub open spec fn indices_ordered(rows: Seq<Row>, indices: Seq<usize>) -> bool {
    forall|left: int, right: int| 0 <= left < right < indices.len() ==>
        !row_lt(row_view(rows[#[trigger] indices[right] as int]), row_view(rows[#[trigger] indices[left] as int]))
}

pub fn sort_indices(rows: &Vec<Row>, low: usize, high: usize) -> (out: Vec<usize>)
    requires low <= high <= rows.len(),
    ensures out.len() == high - low,
        forall|position: int| 0 <= position < out.len() ==> low <= #[trigger] out@[position] < high,
        out@.no_duplicates(), indices_ordered(rows@, out@),
        forall|weight: spec_fn(usize) -> int| #[trigger] sum(out@, weight) == sum(interval(low, high), weight),
        forall|value: usize| #[trigger] out@.contains(value) <==> low <= value < high,
    decreases high - low,
{
    proof { interval_members(low, high); }
    let mut out = Vec::new();
    if high - low <= 1 {
        if low < high { out.push(low); }
        proof { assert(out@ =~= interval(low, high)); }
        return out;
    }
    let middle = low + (high - low) / 2;
    let left = sort_indices(rows, low, middle);
    let right = sort_indices(rows, middle, high);
    let mut left_index = 0usize;
    let mut right_index = 0usize;
    while left_index < left.len() || right_index < right.len()
        invariant left_index <= left.len(), right_index <= right.len(),
            out.len() == left_index + right_index,
            indices_ordered(rows@, out@),
            forall|position: int| 0 <= position < out.len() && left_index < left.len() ==>
                !row_lt(row_view(rows@[left@[left_index as int] as int]), row_view(rows@[#[trigger] out@[position] as int])),
            forall|position: int| 0 <= position < out.len() && right_index < right.len() ==>
                !row_lt(row_view(rows@[right@[right_index as int] as int]), row_view(rows@[#[trigger] out@[position] as int])),
            forall|weight: spec_fn(usize) -> int| #[trigger] sum(out@, weight) ==
                sum(left@.take(left_index as int), weight) + sum(right@.take(right_index as int), weight),
            forall|position: int| 0 <= position < out.len() ==> low <= #[trigger] out@[position] < high,
        decreases left.len() - left_index + right.len() - right_index,
    {
        let take_left = if right_index == right.len() { true }
            else if left_index == left.len() { false }
            else { earlier(&rows[left[left_index]], &rows[right[right_index]]) };
        let ghost previous = out@;
        if take_left {
            let value = left[left_index];
            out.push(value);
            proof {
                assert forall|weight: spec_fn(usize) -> int| #[trigger] sum(out@, weight) ==
                    sum(left@.take(left_index as int + 1), weight) + sum(right@.take(right_index as int), weight) by {
                    lemma_sum_push(previous, value, weight);
                    lemma_sum_take(left@, left_index as int, weight);
                }
            }
            left_index += 1;
        } else {
            let value = right[right_index];
            out.push(value);
            proof {
                assert forall|weight: spec_fn(usize) -> int| #[trigger] sum(out@, weight) ==
                    sum(left@.take(left_index as int), weight) + sum(right@.take(right_index as int + 1), weight) by {
                    lemma_sum_push(previous, value, weight);
                    lemma_sum_take(right@, right_index as int, weight);
                }
            }
            right_index += 1;
        }
    }
    proof {
        assert forall|weight: spec_fn(usize) -> int| #[trigger] sum(out@, weight) == sum(interval(low, high), weight) by {
            assert(interval(low, high) =~= interval(low, middle) + interval(middle, high));
            lemma_sum_take_full(left@, weight);
            lemma_sum_take_full(right@, weight);
            lemma_sum_concat(interval(low, middle), interval(middle, high), weight);
        }
        assert(interval(low, high).no_duplicates());
        permutation_facts(out@, interval(low, high));
        assert forall|value: usize| #[trigger] out@.contains(value) <==> low <= value < high by {
            if low <= value < high { assert(interval(low, high)[value as int - low as int] == value); }
        }
    }
    out
}

pub fn sorted(rows: Vec<Row>) -> (out: Vec<Row>)
    ensures
        forall|left: int, right: int| 0 <= left < right < out.len() ==>
            !row_lt(row_view(#[trigger] out@[right]), row_view(#[trigger] out@[left])),
        forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(out_view(rows@), weight),
        forall|value: OutRow| #[trigger] out_view(out@).contains(value) <==> out_view(rows@).contains(value),
        out_view(rows@).no_duplicates() ==> out_view(out@).no_duplicates(),
{
    let indices = sort_indices(&rows, 0, rows.len());
    let ghost project = |position: usize| row_view(rows@[position as int]);
    let mut out = Vec::new();
    let mut index = 0usize;
    while index < indices.len()
        invariant index <= indices.len(), out.len() == index,
            out_view(out@) == indices@.take(index as int).map_values(project),
        decreases indices.len() - index,
    {
        let row = &rows[indices[index]];
        let ghost previous = out_view(out@);
        let ghost prefix = indices@.take(index as int).map_values(project);
        out.push((copy_bytes(&row.0), row.1, row.2, row.3, row.4, row.5));
        proof {
            assert(out_view(out@) =~= previous.push(row_view(*row)));
            assert(indices@.take(index as int + 1).map_values(project) =~=
                prefix.push(row_view(*row)));
        }
        index += 1;
    }
    proof {
        assert(indices@.take(index as int) =~= indices@);
        assert(out_view(out@) =~= indices@.map_values(project));
        assert forall|left: int, right: int| 0 <= left < right < out.len() implies
            !row_lt(row_view(#[trigger] out@[right]), row_view(#[trigger] out@[left])) by {
            assert(row_view(out@[left]) == out_view(out@)[left]);
            assert(row_view(out@[right]) == out_view(out@)[right]);
        }
        assert(interval(0, rows.len()).map_values(project) =~= out_view(rows@));
        assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(out_view(rows@), weight) by {
            let mapped = |position: usize| weight(project(position));
            lemma_sum_map(indices@, project, weight, mapped);
            lemma_sum_map(interval(0, rows.len()), project, weight, mapped);
        }
        permutation_facts(out_view(out@), out_view(rows@));
    }
    out
}

pub fn sort_quantities(values: &Vec<i128>, low: usize, high: usize) -> (out: Vec<i128>)
    requires low <= high <= values.len(),
    ensures increasing(out@), out.len() == high - low,
        values@.subrange(low as int, high as int).no_duplicates() ==> out@.no_duplicates(),
        forall|value: i128| #[trigger] out@.contains(value) <==> values@.subrange(low as int, high as int).contains(value),
        out@.to_multiset() == values@.subrange(low as int, high as int).to_multiset(),
        forall|weight: spec_fn(i128) -> int| #[trigger] sum(out@, weight) == sum(values@.subrange(low as int, high as int), weight),
    decreases high - low,
{
    let mut out = Vec::new();
    if high - low <= 1 {
        if low < high { out.push(values[low]); }
        proof { assert(out@ =~= values@.subrange(low as int, high as int)); }
        return out;
    }
    let middle = low + (high - low) / 2;
    let left = sort_quantities(values, low, middle);
    let right = sort_quantities(values, middle, high);
    let mut left_index = 0usize;
    let mut right_index = 0usize;
    while left_index < left.len() || right_index < right.len()
        invariant left_index <= left.len(), right_index <= right.len(),
            out.len() == left_index + right_index,
            increasing(out@),
            forall|position: int| 0 <= position < out.len() && left_index < left.len() ==>
                #[trigger] out@[position] <= left@[left_index as int],
            forall|position: int| 0 <= position < out.len() && right_index < right.len() ==>
                #[trigger] out@[position] <= right@[right_index as int],
            forall|weight: spec_fn(i128) -> int| #[trigger] sum(out@, weight) ==
                sum(left@.take(left_index as int), weight) + sum(right@.take(right_index as int), weight),
        decreases left.len() - left_index + right.len() - right_index,
    {
        let take_left = if right_index == right.len() { true }
            else if left_index == left.len() { false }
            else { left[left_index] <= right[right_index] };
        let ghost previous = out@;
        if take_left {
            let value = left[left_index];
            out.push(value);
            proof {
                assert forall|weight: spec_fn(i128) -> int| #[trigger] sum(out@, weight) ==
                    sum(left@.take(left_index as int + 1), weight) + sum(right@.take(right_index as int), weight) by {
                    lemma_sum_push(previous, value, weight);
                    lemma_sum_take(left@, left_index as int, weight);
                }
            }
            left_index += 1;
        } else {
            let value = right[right_index];
            out.push(value);
            proof {
                assert forall|weight: spec_fn(i128) -> int| #[trigger] sum(out@, weight) ==
                    sum(left@.take(left_index as int), weight) + sum(right@.take(right_index as int + 1), weight) by {
                    lemma_sum_push(previous, value, weight);
                    lemma_sum_take(right@, right_index as int, weight);
                }
            }
            right_index += 1;
        }
    }
    proof {
        assert forall|weight: spec_fn(i128) -> int| #[trigger] sum(out@, weight) ==
            sum(values@.subrange(low as int, high as int), weight) by {
            assert(values@.subrange(low as int, high as int) =~=
                values@.subrange(low as int, middle as int) + values@.subrange(middle as int, high as int));
            lemma_sum_take_full(left@, weight);
            lemma_sum_take_full(right@, weight);
            lemma_sum_concat(values@.subrange(low as int, middle as int), values@.subrange(middle as int, high as int), weight);
        }
        lemma_sums_eq_multiset(out@, values@.subrange(low as int, high as int));
        permutation_facts(out@, values@.subrange(low as int, high as int));
    }
    out
}

pub fn lower_bound(values: &Vec<i128>, target: i128) -> (out: usize)
    requires increasing(values@),
    ensures out <= values.len(),
        forall|index: int| 0 <= index < out ==> #[trigger] values@[index] < target,
        forall|index: int| out <= index < values.len() ==> #[trigger] values@[index] >= target,
{
    let mut low = 0usize;
    let mut high = values.len();
    while low < high
        invariant low <= high <= values.len(),
            forall|index: int| 0 <= index < low ==> #[trigger] values@[index] < target,
            forall|index: int| high <= index < values.len() ==> #[trigger] values@[index] >= target,
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if values[middle] < target { low = middle + 1; }
        else { high = middle; }
    }
    low
}

pub fn upper_bound(values: &Vec<i128>, target: i128) -> (out: usize)
    requires increasing(values@),
    ensures out <= values.len(),
        forall|index: int| 0 <= index < out ==> #[trigger] values@[index] <= target,
        forall|index: int| out <= index < values.len() ==> #[trigger] values@[index] > target,
{
    let mut low = 0usize;
    let mut high = values.len();
    while low < high
        invariant low <= high <= values.len(),
            forall|index: int| 0 <= index < low ==> #[trigger] values@[index] <= target,
            forall|index: int| high <= index < values.len() ==> #[trigger] values@[index] > target,
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if values[middle] <= target { low = middle + 1; }
        else { high = middle; }
    }
    low
}

pub proof fn insertion_ordered(values: Seq<i128>, position: int, target: i128)
    requires increasing(values), 0 <= position <= values.len(),
        forall|index: int| 0 <= index < position ==> #[trigger] values[index] <= target,
        forall|index: int| position <= index < values.len() ==> #[trigger] values[index] >= target,
    ensures increasing(values.insert(position, target)),
{
    let inserted = values.insert(position, target);
    assert forall|left: int, right: int| 0 <= left < right < inserted.len() implies
        #[trigger] inserted[left] <= #[trigger] inserted[right] by {
        if left != position && right != position {
            let old_left = if left < position { left } else { left - 1 };
            let old_right = if right < position { right } else { right - 1 };
            assert(inserted[left] == values[old_left]);
            assert(inserted[right] == values[old_right]);
        }
    }
}

pub proof fn removal_ordered(values: Seq<i128>, position: int)
    requires increasing(values), 0 <= position < values.len(),
    ensures increasing(values.remove(position)),
{
    let removed = values.remove(position);
    assert forall|left: int, right: int| 0 <= left < right < removed.len() implies
        #[trigger] removed[left] <= #[trigger] removed[right] by {
        let old_left = if left < position { left } else { left + 1 };
        let old_right = if right < position { right } else { right + 1 };
        assert(removed[left] == values[old_left]);
        assert(removed[right] == values[old_right]);
    }
}

}
