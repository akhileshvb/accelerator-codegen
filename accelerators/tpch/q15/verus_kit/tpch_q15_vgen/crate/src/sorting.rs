#![verifier::loop_isolation(false)]
// Sorting of exact integers (lifted from the Q18 kernel).
use vstd::prelude::*;
use verus_common::sums::*;

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

pub open spec fn increasing(values: Seq<i128>) -> bool {
    forall|left: int, right: int| 0 <= left < right < values.len() ==>
        #[trigger] values[left] <= #[trigger] values[right]
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

}
