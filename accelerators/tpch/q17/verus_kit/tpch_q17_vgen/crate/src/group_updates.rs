use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::aggregation::*;
use crate::model::*;
use crate::storage::*;
use verus_prooflib::wide::Wide;

verus! {

pub open spec fn is_empty_group(group: Group) -> bool {
    group.references@ == 0 && group.moment.count@ == 0 && group.moment.price@ == 0
}

pub proof fn lemma_group_replacement(before: Seq<Group>, after: Seq<Group>, hash: i64, position: int, replacement: Group)
    requires groups_wf(before, hash), 0 <= position <= before.len(),
        replacement.identity.fingerprint == hash, hash as int == pair_hash(replacement.identity.brand@, replacement.identity.container@),
        forall|index: int| #![trigger before[index]] 0 <= index < before.len() ==> (same(*before[index].identity, replacement.identity.brand@, replacement.identity.container@) == (index == position)),
        after == if position < before.len() {
            if is_empty_group(replacement) { before.remove(position) } else { before.update(position, replacement) }
        } else {
            if is_empty_group(replacement) { before } else { before.push(replacement) }
        },
    ensures groups_wf(after, hash),
        forall|brand: Seq<u8>, container: Seq<u8>, column: nat| #[trigger] group_total(after, brand, container, column)
            == group_total(before, brand, container, column) + if same(*replacement.identity, brand, container) {
                group_value(replacement, column)
                    - if position < before.len() { group_value(before[position], column) } else { 0 }
            } else { 0 },
{
    reveal(groups_wf);
    let identity = *replacement.identity;
    let removed = is_empty_group(replacement);
    assert forall|brand: Seq<u8>, container: Seq<u8>, column: nat| #[trigger] group_total(after, brand, container, column)
        == group_total(before, brand, container, column) + if same(identity, brand, container) {
            group_value(replacement, column)
                - if position < before.len() { group_value(before[position], column) } else { 0 }
        } else { 0 } by {
        reveal(group_total);
        let weight = |group: Group| if same(*group.identity, brand, container) { group_value(group, column) } else { 0int };
        if position < before.len() {
            if removed { lemma_sum_remove(before, position, weight); }
            else { lemma_sum_update(before, position, replacement, weight); }
        } else if !removed { lemma_sum_push(before, replacement, weight); }
    }
    assert forall|index: int| #![trigger after[index]] 0 <= index < after.len() implies {
        &&& after[index].identity.fingerprint == hash
        &&& hash as int == pair_hash(after[index].identity.brand@, after[index].identity.container@)
    } by {
        if position < before.len() && removed {
            let source = if index < position { index } else { index + 1 };
            assert(after[index] == before[source]);
        } else if index == position { assert(after[index] == replacement); }
        else { assert(after[index] == before[index]); }
    }
    assert forall|first: int, second: int| #![trigger after[first], after[second]] 0 <= first < second < after.len()
        implies !same(*after[first].identity, after[second].identity.brand@, after[second].identity.container@) by {
        if position < before.len() && removed {
            let source_first = if first < position { first } else { first + 1 };
            let source_second = if second < position { second } else { second + 1 };
            assert(source_first < source_second);
            assert(after[first] == before[source_first]);
            assert(after[second] == before[source_second]);
        } else if first == position { assert(!same(*before[second].identity, identity.brand@, identity.container@)); }
        else if second == position { assert(!same(*before[first].identity, identity.brand@, identity.container@)); }
        else { assert(after[first] == before[first]); assert(after[second] == before[second]); }
    }
}

#[verifier::rlimit(1)]
pub fn adjust_group(groups: &mut Store<Vec<Group>>, identity: &Arc<Identity>, references_delta: &Wide, count_delta: &Wide, price_delta: &Wide)
    requires group_store_wf(*old(groups)), identity.fingerprint as int == pair_hash(identity.brand@, identity.container@),
    ensures group_store_wf(*final(groups)),
        forall|brand: Seq<u8>, container: Seq<u8>, column: nat| #[trigger] group_total_at(*final(groups), brand, container, column)
            == group_total_at(*old(groups), brand, container, column) + if same(**identity, brand, container) {
                if column == 0 { references_delta@ } else if column == 1 { count_delta@ } else { price_delta@ }
            } else { 0 },
{
    let before_store = Ghost(*groups);
    let old_bucket = groups.get(identity.fingerprint);
    let mut bucket: Vec<Group> = Vec::new();
    if let Some(previous) = old_bucket {
        let mut index = 0;
        while index < previous.len()
            invariant index <= previous.len(), bucket@ == previous@.take(index as int),
                group_store_wf(*groups), *groups == before_store@,
            decreases previous.len() - index,
        {
            bucket.push(Group { identity: previous[index].identity.clone(), references: previous[index].references.dup(), moment: previous[index].moment.dup() });
            index += 1;
        }
    }
    let before = Ghost(bucket@);
    proof { assert(bucket@ == groups_at(*groups, identity.fingerprint)); }
    let mut index = 0;
    let mut found = false;
    while index < bucket.len() && !found
        invariant index <= bucket.len(), bucket@ == before@, group_store_wf(*groups), *groups == before_store@,
            groups_wf(bucket@, identity.fingerprint),
            found ==> index < bucket.len() && same(*bucket@[index as int].identity, identity.brand@, identity.container@),
            forall|position: int| #![trigger bucket@[position]] 0 <= position < index ==> !same(*bucket@[position].identity, identity.brand@, identity.container@),
        decreases if found { 0int } else { bucket.len() - index + 1 },
    {
        if identity_matches(&bucket[index].identity, &identity.brand, &identity.container) { found = true; }
        else { index += 1; }
    }
    let position = index;
    let (references, count, price) = if position < bucket.len() {
        (bucket[position].references.add(references_delta), bucket[position].moment.count.add(count_delta), bucket[position].moment.price.add(price_delta))
    } else { (references_delta.dup(), count_delta.dup(), price_delta.dup()) };
    proof {
        assert forall|offset: int| #![trigger before@[offset]] 0 <= offset < before@.len() implies
            (same(*before@[offset].identity, identity.brand@, identity.container@) == (offset == position)) by {
            if offset < position { }
            else if position < before@.len() && offset != position {
                lemma_group_distinct(before@, identity.fingerprint, position as int, offset);
                assert(!same(*before@[position as int].identity, before@[offset].identity.brand@, before@[offset].identity.container@));
            }
        }
    }
    let replacement = Group { identity: identity.clone(), references, moment: Joined { count, price } };
    let empty = replacement.references.is_zero() && replacement.moment.count.is_zero() && replacement.moment.price.is_zero();
    if position < bucket.len() {
        if empty { bucket.remove(position); } else { bucket.set(position, replacement); }
    } else if !empty { bucket.push(replacement); }
    proof {
        lemma_group_replacement(before@, bucket@, identity.fingerprint, position as int, replacement);
    }
    let after = Ghost(bucket@);
    if bucket.len() == 0 { groups.put(identity.fingerprint, None); }
    else { groups.put(identity.fingerprint, Some(Arc::new(bucket))); }
    proof {
        assert forall|hash: i64| #[trigger] groups_at(*groups, hash) == if hash == identity.fingerprint { after@ } else { groups_at(before_store@, hash) } by {}
        assert forall|hash: i64| #[trigger] groups_wf(groups_at(*groups, hash), hash) by {
            if hash != identity.fingerprint { assert(groups_wf(groups_at(before_store@, hash), hash)); }
        }
        assert forall|brand: Seq<u8>, container: Seq<u8>, column: nat| #[trigger] group_total_at(*groups, brand, container, column)
            == group_total_at(before_store@, brand, container, column) + if same(**identity, brand, container) {
                if column == 0 { references_delta@ } else if column == 1 { count_delta@ } else { price_delta@ }
            } else { 0 } by {
            if same(**identity, brand, container) { assert(pair_hash(brand, container) as i64 == identity.fingerprint); }
        }
    }
}

}
