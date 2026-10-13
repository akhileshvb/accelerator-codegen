use vstd::prelude::*;
use verus_common::sums::*;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

/// A reordering of 0..length (as a bag) lists every index exactly once.
pub proof fn lemma_index_permutation(sorted: Seq<usize>, original: Seq<usize>, length: nat)
    requires verus_common::sort::same_bag(sorted, original), original.len() == length,
        forall|index: int| 0 <= index < original.len() ==> (#[trigger] original[index]) as int == index,
        forall|index: int| 0 <= index < sorted.len() ==> (#[trigger] sorted[index]) < length,
    ensures sorted.no_duplicates(),
        forall|index: usize| index < length ==> #[trigger] sorted.contains(index),
{
    assert forall|left: int, right: int| 0 <= left < right < sorted.len() implies
        (#[trigger] sorted[left]) != (#[trigger] sorted[right]) by {
        if sorted[left] == sorted[right] {
            let target = sorted[left];
            let weight = |item: usize| if item == target { 1int } else { 0int };
            lemma_sum_single_match(original, weight, target as int);
            assert(sum(sorted, weight) == sum(original, weight));
            lemma_sum_remove(sorted, left, weight);
            let smaller = sorted.remove(left);
            assert(smaller[right - 1] == target);
            lemma_sum_ge_term(smaller, weight, right - 1);
        }
    }
    assert forall|index: usize| index < length implies #[trigger] sorted.contains(index) by {
        let weight = |item: usize| if item == index { 1int } else { 0int };
        lemma_sum_single_match(original, weight, index as int);
        assert(sum(sorted, weight) == 1);
        if !sorted.contains(index) {
            assert forall|position: int| 0 <= position < sorted.len() implies weight(#[trigger] sorted[position]) == 0 by {}
            lemma_sum_zero(sorted, weight);
        }
    }
}

pub open spec fn slot_key(key: i64, span: usize) -> int {
    ((key as u64) % (span as u64)) as int
}

/// The same keys ordered by their page-store slot (key mod span), so that
/// consecutive store updates walk the page tree along neighbouring paths.
/// Order is a performance hint only; the result is a reordering of `keys`.
pub fn by_slot(keys: &Vec<i64>, span: usize) -> (result: Vec<i64>)
    requires span > 0, keys@.no_duplicates(),
    ensures result@.no_duplicates(),
        forall|key: i64| result@.contains(key) <==> keys@.contains(key),
{
    let mut indices: Vec<usize> = Vec::with_capacity(keys.len());
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), indices.len() == index,
            forall|position: int| 0 <= position < indices.len() ==> (#[trigger] indices@[position]) as int == position,
        decreases keys.len() - index,
    {
        indices.push(index);
        index += 1;
    }
    let ghost valid = |position: usize| position < keys.len();
    let ghost le = |left: usize, right: usize| slot_key(keys@[left as int], span) <= slot_key(keys@[right as int], span);
    let keys_ref = &keys;
    let compare = |left: usize, right: usize| -> (result: bool)
        requires left < keys_ref.len(), right < keys_ref.len(),
        ensures result == (slot_key(keys_ref@[left as int], span) <= slot_key(keys_ref@[right as int], span)),
    { ((keys_ref[left] as u64) % (span as u64)) <= ((keys_ref[right] as u64) % (span as u64)) };
    proof {
        assert forall|left: usize, right: usize, last: usize| valid(left) && valid(right) && valid(last)
            && #[trigger] le(left, right) && #[trigger] le(right, last) implies le(left, last) by {}
        assert forall|left: usize, right: usize| valid(left) && valid(right) implies #[trigger] le(left, right) || le(right, left) by {}
    }
    let sorted = verus_common::sort::sort_idx(&indices, &compare, Ghost(le), Ghost(valid));
    proof { lemma_index_permutation(sorted@, indices@, keys.len() as nat); }
    let mut result: Vec<i64> = Vec::with_capacity(sorted.len());
    index = 0;
    while index < sorted.len()
        invariant index <= sorted.len(), result.len() == index,
            forall|position: int| 0 <= position < sorted.len() ==> (#[trigger] sorted@[position]) < keys.len(),
            forall|position: int| 0 <= position < result.len() ==> (#[trigger] result@[position]) == keys@[sorted@[position] as int],
        decreases sorted.len() - index,
    {
        result.push(keys[sorted[index]]);
        index += 1;
    }
    proof {
        assert forall|left: int, right: int| 0 <= left < right < result.len() implies (#[trigger] result@[left]) != (#[trigger] result@[right]) by {
            assert(sorted@[left] != sorted@[right]);
        }
        assert forall|key: i64| result@.contains(key) implies keys@.contains(key) by {
            let position = choose|position: int| 0 <= position < result.len() && (#[trigger] result@[position]) == key;
            assert(keys@[sorted@[position] as int] == key);
        }
        assert forall|key: i64| keys@.contains(key) implies result@.contains(key) by {
            let position = choose|position: int| 0 <= position < keys.len() && (#[trigger] keys@[position]) == key;
            assert(sorted@.contains(position as usize));
            let slot = choose|slot: int| 0 <= slot < sorted.len() && (#[trigger] sorted@[slot]) == position as usize;
            assert(result@[slot] == key);
        }
    }
    result
}

}
