use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::sums::{sum, lemma_sum_take};
use verus_prooflib::wide::Wide;
use verus_prooflib::{lemma_unique_push, lemma_push_membership, lemma_push_to_set};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::profiles::*;
use crate::semantic_proofs::part_capture_weight;

verus! {

broadcast use { vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties };

/// The part keys of the deltas, each once, in order of first appearance.
pub open spec fn first_seen(rows: Seq<PartDelta>) -> Seq<i64>
    decreases rows.len(),
{
    if rows.len() == 0 { Seq::empty() } else {
        let before = first_seen(rows.drop_last());
        let key = rows.last().row.p_partkey;
        if before.contains(key) { before } else { before.push(key) }
    }
}

/// A part that is absent from the state and has a positive net count after the deltas.
pub open spec fn activates(parts: Table<Part>, rows: Seq<PartDelta>, key: i64) -> bool {
    parts.lookup(key).is_none() && sum(rows, part_capture_weight(key as int)) > 0
}

pub open spec fn activated_keys(parts: Table<Part>, rows: Seq<PartDelta>, keys: Seq<i64>) -> Seq<i64>
    decreases keys.len(),
{
    if keys.len() == 0 { Seq::empty() } else {
        let before = activated_keys(parts, rows, keys.drop_last());
        if activates(parts, rows, keys.last()) { before.push(keys.last()) } else { before }
    }
}

pub open spec fn activation_spec(parts: Table<Part>, rows: Seq<PartDelta>) -> Seq<i64> {
    activated_keys(parts, rows, first_seen(rows))
}

pub open spec fn sum_of(map: Map<i64, Wide>, key: i64) -> int {
    if map.contains_key(key) { map[key]@ } else { 0 }
}

proof fn lemma_first_seen_step(rows: Seq<PartDelta>, index: int)
    requires 0 <= index < rows.len(),
    ensures first_seen(rows.take(index + 1)) ==
        if first_seen(rows.take(index)).contains(rows[index].row.p_partkey) { first_seen(rows.take(index)) }
        else { first_seen(rows.take(index)).push(rows[index].row.p_partkey) },
{
    assert(rows.take(index + 1).drop_last() =~= rows.take(index));
}

proof fn lemma_activated_push(parts: Table<Part>, rows: Seq<PartDelta>, keys: Seq<i64>, key: i64)
    ensures activated_keys(parts, rows, keys.push(key)) ==
        if activates(parts, rows, key) { activated_keys(parts, rows, keys).push(key) } else { activated_keys(parts, rows, keys) },
{
    assert(keys.push(key).drop_last() =~= keys);
}

fn delta_sums(rows: &Vec<PartDelta>) -> (result: (HashMap<i64, Wide>, Vec<i64>))
    ensures
        result.1@.no_duplicates() && result.0@.dom() == result.1@.to_set() && result.1@ == first_seen(rows@) &&
        forall|key: i64| #[trigger] sum_of(result.0@, key) == sum(rows@, part_capture_weight(key as int)),
{
    let mut sums: HashMap<i64, Wide> = HashMap::new();
    let mut keys: Vec<i64> = Vec::new();
    let mut index = 0usize;
    while index < rows.len()
        invariant index <= rows.len(), keys@.no_duplicates(), sums@.dom() == keys@.to_set(),
            keys@ == first_seen(rows@.take(index as int)),
            forall|key: i64| #[trigger] sum_of(sums@, key) == sum(rows@.take(index as int), part_capture_weight(key as int)),
        decreases rows.len() - index,
    {
        let previous = Ghost(sums@);
        let previous_keys = Ghost(keys@);
        let key = rows[index].row.p_partkey;
        let old_sum = match sums.get(&key) { Some(value) => value.dup(), None => { keys.push(key); Wide::zero() } };
        let new_sum = old_sum.add(&Wide::from_i128(rows[index].sign as i128));
        sums.insert(key, new_sum);
        proof {
            previous_keys@.to_set_ensures();
            if !previous@.dom().contains(key) { lemma_unique_push(previous_keys@, key); lemma_push_membership(previous_keys@, key); lemma_push_to_set(previous_keys@, key); }
            keys@.to_set_ensures();
            lemma_first_seen_step(rows@, index as int);
            assert(sum_of(previous@, key) == old_sum@);
            assert forall|other: i64| #[trigger] sum_of(sums@, other) == sum(rows@.take(index as int + 1), part_capture_weight(other as int)) by {
                lemma_sum_take(rows@, index as int, part_capture_weight(other as int));
                assert(sum_of(previous@, other) == sum(rows@.take(index as int), part_capture_weight(other as int)));
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(index as int) =~= rows@); }
    (sums, keys)
}

pub fn activation_keys(parts: &Table<Part>, rows: &Vec<PartDelta>) -> (result: Vec<i64>)
    ensures result@ == activation_spec(*parts, rows@),
{
    let (sums, keys) = delta_sums(rows);
    let mut result = Vec::new();
    let mut index = 0usize;
    while index < keys.len()
        invariant index <= keys.len(), sums@.dom() == keys@.to_set(), keys@ == first_seen(rows@),
            forall|key: i64| #[trigger] sum_of(sums@, key) == sum(rows@, part_capture_weight(key as int)),
            result@ == activated_keys(*parts, rows@, keys@.take(index as int)),
        decreases keys.len() - index,
    {
        let key = keys[index];
        proof {
            keys@.to_set_ensures();
            assert(sums@.dom().contains(key));
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            lemma_activated_push(*parts, rows@, keys@.take(index as int), key);
        }
        let found = parts.get(key);
        let absent = found.is_none();
        let positive = match sums.get(&key) { Some(value) => value.is_pos(), None => false };
        proof {
            assert(sum_of(sums@, key) == sum(rows@, part_capture_weight(key as int)));
            assert(absent == parts.lookup(key).is_none());
            assert(positive == (sum(rows@, part_capture_weight(key as int)) > 0));
        }
        if absent && positive { result.push(key); }
        index += 1;
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    result
}

}
