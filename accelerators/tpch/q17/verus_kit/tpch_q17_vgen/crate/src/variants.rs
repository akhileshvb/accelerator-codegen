use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use crate::aggregation::*;
use crate::model::*;

verus! {

pub proof fn lemma_variant_replacement(before: Seq<Variant>, after: Seq<Variant>, position: int, replacement: Variant)
    requires variants_wf(before), 0 <= position <= before.len(), replacement.multiplicity >= 0,
        replacement.identity.fingerprint as int == pair_hash(replacement.identity.brand@, replacement.identity.container@),
        forall|index: int| #![trigger before[index]] 0 <= index < before.len() ==>
            (same(*before[index].identity, replacement.identity.brand@, replacement.identity.container@) == (index == position)),
        after == if position < before.len() {
            if replacement.multiplicity == 0 { before.remove(position) } else { before.update(position, replacement) }
        } else { if replacement.multiplicity == 0 { before } else { before.push(replacement) } },
    ensures variants_wf(after),
        forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] variant_total(after, brand, container)
            == variant_total(before, brand, container) + if same(*replacement.identity, brand, container) {
                replacement.multiplicity as int - if position < before.len() { before[position].multiplicity as int } else { 0 }
            } else { 0 },
{
    reveal(variants_wf);
    assert forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] variant_total(after, brand, container)
        == variant_total(before, brand, container) + if same(*replacement.identity, brand, container) {
            replacement.multiplicity as int - if position < before.len() { before[position].multiplicity as int } else { 0 }
        } else { 0 } by {
        reveal(variant_total);
        let weight = |row: Variant| if same(*row.identity, brand, container) { row.multiplicity as int } else { 0int };
        if position < before.len() {
            if replacement.multiplicity == 0 { lemma_sum_remove(before, position, weight); }
            else { lemma_sum_update(before, position, replacement, weight); }
        } else if replacement.multiplicity > 0 { lemma_sum_push(before, replacement, weight); }
    }
    assert forall|index: int| #![trigger after[index]] 0 <= index < after.len() implies
        after[index].multiplicity > 0 && after[index].identity.fingerprint as int == pair_hash(after[index].identity.brand@, after[index].identity.container@) by {
        if position < before.len() && replacement.multiplicity == 0 {
            let source = if index < position { index } else { index + 1 };
            assert(after[index] == before[source]);
        } else if index == position { assert(after[index] == replacement); }
        else { assert(after[index] == before[index]); }
    }
    assert forall|first: int, second: int| #![trigger after[first], after[second]] 0 <= first < second < after.len()
        implies !same(*after[first].identity, after[second].identity.brand@, after[second].identity.container@) by {
        if position < before.len() && replacement.multiplicity == 0 {
            let source_first = if first < position { first } else { first + 1 };
            let source_second = if second < position { second } else { second + 1 };
            assert(source_first < source_second);
            assert(after[first] == before[source_first]); assert(after[second] == before[source_second]);
        } else if first == position { assert(!same(*before[second].identity, replacement.identity.brand@, replacement.identity.container@)); }
        else if second == position { assert(!same(*before[first].identity, replacement.identity.brand@, replacement.identity.container@)); }
        else { assert(after[first] == before[first]); assert(after[second] == before[second]); }
    }
}

#[verifier::rlimit(1)]
pub fn update_variant(rows: &mut Vec<Variant>, identity: Arc<Identity>, delta: i128) -> (result: bool)
    requires variants_wf(old(rows)@), identity.fingerprint as int == pair_hash(identity.brand@, identity.container@),
    ensures variants_wf(final(rows)@),
        result ==> forall|brand: Seq<u8>, container: Seq<u8>| #[trigger] variant_total(final(rows)@, brand, container)
            == variant_total(old(rows)@, brand, container) + if same(*identity, brand, container) { delta as int } else { 0 },
{
    let before = Ghost(rows@);
    let mut index = 0;
    let mut found = false;
    while index < rows.len() && !found
        invariant index <= rows.len(), rows@ == before@, variants_wf(rows@),
            found ==> index < rows.len() && same(*rows@[index as int].identity, identity.brand@, identity.container@),
            forall|position: int| #![trigger rows@[position]] 0 <= position < index ==> !same(*rows@[position].identity, identity.brand@, identity.container@),
        decreases if found { 0int } else { rows.len() - index + 1 },
    {
        if identity_matches(&rows[index].identity, &identity.brand, &identity.container) { found = true; }
        else { index += 1; }
    }
    let previous = if index < rows.len() { rows[index].multiplicity } else { 0 };
    let multiplicity = match previous.checked_add(delta) { Some(value) => value, None => return false };
    if multiplicity < 0 { return false; }
    let replacement = Variant { identity, multiplicity };
    proof {
        reveal(variants_wf);
        assert forall|position: int| #![trigger before@[position]] 0 <= position < before@.len() implies
            (same(*before@[position].identity, replacement.identity.brand@, replacement.identity.container@) == (position == index)) by {
            if position > index && index < before@.len() {
                assert(!same(*before@[index as int].identity, before@[position].identity.brand@, before@[position].identity.container@));
            }
        }
    }
    if index < rows.len() {
        if multiplicity == 0 { rows.remove(index); } else { rows.set(index, replacement); }
    } else if multiplicity > 0 { rows.push(replacement); }
    proof { lemma_variant_replacement(before@, rows@, index as int, replacement); }
    true
}

pub proof fn lemma_variant_prefix(rows: Seq<Variant>, index: int, brand: Seq<u8>, container: Seq<u8>)
    requires 0 <= index < rows.len(),
    ensures variant_total(rows.take(index + 1), brand, container) == variant_total(rows.take(index), brand, container)
        + if same(*rows[index].identity, brand, container) { rows[index].multiplicity as int } else { 0 },
{
    reveal(variant_total);
    assert(rows.take(index + 1) =~= rows.take(index).push(rows[index]));
    lemma_sum_push(rows.take(index), rows[index], |row: Variant| if same(*row.identity, brand, container) { row.multiplicity as int } else { 0int });
}

}
