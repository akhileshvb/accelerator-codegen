use vstd::prelude::*;
use verus_common::order::{sorted_by, strict_order, lemma_sorted_unique};
use verus_prooflib::lemma_sorted_insert_at;

verus! {

pub open spec fn strictly_increasing(keys: Seq<i64>) -> bool {
    forall|first: int, last: int| 0 <= first < last < keys.len() ==> keys[first] < keys[last]
}

pub open spec fn increasing_order() -> spec_fn(i64, i64) -> bool { |left: i64, right: i64| left < right }

proof fn lemma_increasing_sorted(keys: Seq<i64>)
    ensures strictly_increasing(keys) == sorted_by(keys, increasing_order()),
{
}

pub proof fn lemma_unique_keys(first: Seq<i64>, second: Seq<i64>)
    requires strictly_increasing(first), strictly_increasing(second),
        forall|key: i64| first.contains(key) <==> second.contains(key),
    ensures first == second,
{
    assert(strict_order(increasing_order()));
    lemma_sorted_unique(first, second, increasing_order());
}

/// Inserts a key into an increasing list, once.
pub fn insert_sorted(keys: &mut Vec<i64>, key: i64)
    requires strictly_increasing(old(keys)@),
    ensures strictly_increasing(final(keys)@),
        forall|other: i64| final(keys)@.contains(other) == (old(keys)@.contains(other) || other == key),
{
    let ghost before = keys@;
    proof { assert(before == old(keys)@); }
    let mut position = 0usize;
    while position < keys.len() && keys[position] < key
        invariant position <= keys.len(), keys@ == before, strictly_increasing(before),
            forall|offset: int| 0 <= offset < position ==> before[offset] < key,
        decreases keys.len() - position,
    {
        position += 1;
    }
    if position < keys.len() && keys[position] == key {
        proof { assert(before[position as int] == key); assert(before.contains(key)); }
        return;
    }
    proof {
        assert forall|offset: int| position <= offset < before.len() implies key < before[offset] by {
            if offset == position { } else { assert(before[position as int] < before[offset]); }
        }
        lemma_sorted_insert_at(before, position as int, key, increasing_order());
        assert(strictly_increasing(before.insert(position as int, key)));
    }
    keys.insert(position, key);
    proof {
        assert(keys@ =~= before.insert(position as int, key));
        assert forall|other: i64| keys@.contains(other) implies (before.contains(other) || other == key) by {
            let spot = choose|spot: int| 0 <= spot < keys@.len() && keys@[spot] == other;
            if spot < position { assert(before[spot] == other); assert(before.contains(other)); }
            else if spot > position { assert(before[spot - 1] == other); assert(before.contains(other)); }
            else { assert(other == key); }
        }
        assert forall|other: i64| (before.contains(other) || other == key) implies keys@.contains(other) by {
            if before.contains(other) {
                let spot = choose|spot: int| 0 <= spot < before.len() && before[spot] == other;
                let moved = if spot < position { spot } else { spot + 1 };
                assert(keys@[moved] == other);
                assert(keys@.contains(other));
            } else {
                assert(keys@[position as int] == key);
                assert(keys@.contains(key));
            }
        }
        assert(old(keys)@ == before);
    }
}

}
