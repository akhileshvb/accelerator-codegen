use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::aggregation::*;
use crate::model::*;
use crate::maintenance::Change;
use verus_prooflib::wide::Wide;

verus! {

#[verifier::opaque]
pub open spec fn change_bucket_wf(rows: Seq<Change>) -> bool {
    &&& forall|index: int| #![trigger rows[index]] 0 <= index < rows.len() ==>
        rows[index].identity.fingerprint as int == pair_hash(rows[index].identity.brand@, rows[index].identity.container@)
    &&& forall|first: int, second: int| #![trigger rows[first], rows[second]] 0 <= first < second < rows.len() ==>
        !same(*rows[first].identity, rows[second].identity.brand@, rows[second].identity.container@)
}

#[verifier::opaque]
pub open spec fn change_total(rows: Seq<Change>, brand: Seq<u8>, container: Seq<u8>) -> int {
    sum(rows, |row: Change| if same(*row.identity, brand, container) { row.delta@ } else { 0 })
}

pub proof fn lemma_change_replace(before: Seq<Change>, after: Seq<Change>, position: int, replacement: Change)
    requires change_bucket_wf(before), 0 <= position <= before.len(),
        replacement.identity.fingerprint as int == pair_hash(replacement.identity.brand@, replacement.identity.container@),
        forall|index: int| #![trigger before[index]] 0 <= index < before.len() ==>
            (same(*before[index].identity, replacement.identity.brand@, replacement.identity.container@) == (index == position)),
        after == if position < before.len() { before.update(position, replacement) } else { before.push(replacement) },
    ensures change_bucket_wf(after),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] change_total(after, brand, container)
            == change_total(before, brand, container) + if same(*replacement.identity, brand, container) {
                replacement.delta@ - if position < before.len() { before[position].delta@ } else { 0 }
            } else { 0 },
{
    reveal(change_bucket_wf);
    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] change_total(after, brand, container)
        == change_total(before, brand, container) + if same(*replacement.identity, brand, container) {
            replacement.delta@ - if position < before.len() { before[position].delta@ } else { 0 }
        } else { 0 } by {
        reveal(change_total);
        let weight = |row: Change| if same(*row.identity, brand, container) { row.delta@ } else { 0int };
        if position < before.len() { lemma_sum_update(before, position, replacement, weight); }
        else { lemma_sum_push(before, replacement, weight); }
    }
    assert forall|first: int, second: int| #![trigger after[first], after[second]] 0 <= first < second < after.len()
        implies !same(*after[first].identity, after[second].identity.brand@, after[second].identity.container@) by {
        if first == position { assert(!same(*before[second].identity, replacement.identity.brand@, replacement.identity.container@)); }
        else if second == position { assert(!same(*before[first].identity, replacement.identity.brand@, replacement.identity.container@)); }
        else { assert(after[first] == before[first]); assert(after[second] == before[second]); }
    }
}

#[verifier::rlimit(1)]
pub fn merge_change(rows: &mut Vec<Change>, identity: Arc<Identity>, sign: i64)
    requires change_bucket_wf(old(rows)@), identity.fingerprint as int == pair_hash(identity.brand@, identity.container@),
    ensures change_bucket_wf(final(rows)@),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] change_total(final(rows)@, brand, container)
            == change_total(old(rows)@, brand, container) + if same(*identity, brand, container) { sign as int } else { 0 },
{
    let before = Ghost(rows@);
    let mut index = 0;
    let mut found = false;
    while index < rows.len() && !found
        invariant index <= rows.len(), rows@ == before@, change_bucket_wf(rows@),
            found ==> index < rows.len() && same(*rows@[index as int].identity, identity.brand@, identity.container@),
            forall|position: int| #![trigger rows@[position]] 0 <= position < index ==> !same(*rows@[position].identity, identity.brand@, identity.container@),
        decreases if found { 0int } else { rows.len() - index + 1 },
    {
        if identity_matches(&rows[index].identity, &identity.brand, &identity.container) { found = true; }
        else { index += 1; }
    }
    let step = Wide::from_i128(sign as i128);
    let delta = if index < rows.len() { rows[index].delta.add(&step) } else { step };
    let replacement = Change { identity, delta };
    proof {
        reveal(change_bucket_wf);
        assert forall|position: int| #![trigger before@[position]] 0 <= position < before@.len() implies
            (same(*before@[position].identity, replacement.identity.brand@, replacement.identity.container@) == (position == index)) by {
            if position > index && index < before@.len() {
                assert(!same(*before@[index as int].identity, before@[position].identity.brand@, before@[position].identity.container@));
            }
        }
    }
    if index < rows.len() { rows.set(index, replacement); } else { rows.push(replacement); }
    proof { lemma_change_replace(before@, rows@, index as int, replacement); }
}

pub proof fn lemma_change_prefix(rows: Seq<Change>, index: int, brand: Seq<u8>, container: Seq<u8>)
    requires 0 <= index < rows.len(),
    ensures change_total(rows.take(index + 1), brand, container) == change_total(rows.take(index), brand, container)
        + if same(*rows[index].identity, brand, container) { rows[index].delta@ } else { 0 },
{
    reveal(change_total);
    assert(rows.take(index + 1) =~= rows.take(index).push(rows[index]));
    lemma_sum_push(rows.take(index), rows[index], |row: Change| if same(*row.identity, brand, container) { row.delta@ } else { 0int });
}

pub proof fn lemma_change_entry(rows: Seq<Change>, index: int)
    requires change_bucket_wf(rows), 0 <= index < rows.len(),
    ensures rows[index].identity.fingerprint as int == pair_hash(rows[index].identity.brand@, rows[index].identity.container@),
{ reveal(change_bucket_wf); }

}
