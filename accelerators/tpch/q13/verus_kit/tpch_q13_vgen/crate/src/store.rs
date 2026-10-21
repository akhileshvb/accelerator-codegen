#![verifier::loop_isolation(false)]
// Records per customer key (multiplicity and the distinct order comments with their counts) in a
// persistent ordered table, and histograms of (bucket, customers) as small exact lists.
use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;

pub struct Variant { pub comment: Arc<Vec<u8>>, pub count: Wide }

pub struct Rec { pub mult: Wide, pub orders: Vec<Variant> }

pub struct HEntry { pub bucket: Wide, pub freq: Wide }

pub open spec fn cview(variant: Variant) -> Seq<u8> { (*variant.comment)@ }

pub open spec fn comment_term(comment: Seq<u8>) -> spec_fn(Variant) -> int {
    |variant: Variant| if cview(variant) == comment { variant.count@ } else { 0 }
}

pub open spec fn comment_count(variants: Seq<Variant>, comment: Seq<u8>) -> int {
    sum(variants, comment_term(comment))
}

pub open spec fn distinct_comments(variants: Seq<Variant>) -> bool {
    forall|left: int, right: int| 0 <= left < right < variants.len() ==> cview(#[trigger] variants[left]) != cview(#[trigger] variants[right])
}

pub open spec fn nonzero_counts(variants: Seq<Variant>) -> bool {
    forall|index: int| 0 <= index < variants.len() ==> (#[trigger] variants[index]).count@ != 0
}

pub open spec fn rec_ok(rec: Rec) -> bool {
    &&& distinct_comments(rec.orders@)
    &&& nonzero_counts(rec.orders@)
    &&& !(rec.mult@ == 0 && rec.orders@.len() == 0)
}

pub open spec fn hist_term(bucket: int) -> spec_fn(HEntry) -> int {
    |entry: HEntry| if entry.bucket@ == bucket { entry.freq@ } else { 0 }
}

pub open spec fn hist_val(entries: Seq<HEntry>, bucket: int) -> int { sum(entries, hist_term(bucket)) }

pub open spec fn hist_ok(entries: Seq<HEntry>) -> bool {
    &&& forall|left: int, right: int| 0 <= left < right < entries.len() ==> (#[trigger] entries[left]).bucket@ != (#[trigger] entries[right]).bucket@
    &&& forall|index: int| 0 <= index < entries.len() ==> (#[trigger] entries[index]).freq@ != 0
}

/// Moves `delta` customers into (positive) or out of (negative) a bucket.
#[verifier::spinoff_prover]
#[verifier::rlimit(40)]
pub fn hist_add(entries: &mut Vec<HEntry>, bucket: &Wide, delta: i64)
    requires hist_ok(old(entries)@), delta == 1 || delta == -1,
    ensures hist_ok(final(entries)@),
        forall|other: int| #[trigger] hist_val(final(entries)@, other) == hist_val(old(entries)@, other) + if other == bucket@ { delta as int } else { 0 },
{
    let ghost before = entries@;
    let mut index = 0usize;
    let mut found = false;
    while index < entries.len() && !found
        invariant index <= entries.len(), entries@ == before, hist_ok(before),
            found ==> index < entries.len() && entries@[index as int].bucket@ == bucket@,
            forall|position: int| 0 <= position < index ==> entries@[position].bucket@ != bucket@,
        decreases if found { 0int } else { entries.len() - index + 1 },
    {
        if entries[index].bucket.eq(bucket) { found = true; } else { index += 1; }
    }
    if found {
        let updated = HEntry { bucket: entries[index].bucket.dup(), freq: entries[index].freq.add(&Wide::from_i128(delta as i128)) };
        proof {
            assert forall|other: int| #[trigger] hist_val(before.update(index as int, updated), other) == hist_val(before, other) + if other == bucket@ { delta as int } else { 0 } by {
                lemma_sum_update(before, index as int, updated, hist_term(other));
                if other == bucket@ { assert(hist_term(other)(updated) == updated.freq@); assert(hist_term(other)(before[index as int]) == before[index as int].freq@); }
                else { assert(hist_term(other)(updated) == 0); assert(hist_term(other)(before[index as int]) == 0); }
            }
        }
        if updated.freq.is_zero() {
            let ghost removed = before.remove(index as int);
            entries.remove(index);
            proof {
                assert forall|other: int| #[trigger] hist_val(removed, other) == hist_val(before.update(index as int, updated), other) by {
                    lemma_sum_remove(before.update(index as int, updated), index as int, hist_term(other));
                    assert(removed =~= before.update(index as int, updated).remove(index as int));
                    assert(hist_term(other)(updated) == 0);
                }
                assert forall|left: int, right: int| 0 <= left < right < entries@.len() implies entries@[left].bucket@ != entries@[right].bucket@ by {
                    let source_left = if left < index as int { left } else { left + 1 };
                    let source_right = if right < index as int { right } else { right + 1 };
                    assert(entries@[left] == before[source_left]);
                    assert(entries@[right] == before[source_right]);
                }
                assert forall|position: int| 0 <= position < entries@.len() implies entries@[position].freq@ != 0 by {
                    let source = if position < index as int { position } else { position + 1 };
                    assert(entries@[position] == before[source]);
                }
            }
        } else {
            entries.set(index, updated);
            proof {
                assert forall|left: int, right: int| 0 <= left < right < entries@.len() implies entries@[left].bucket@ != entries@[right].bucket@ by {
                    if left == index as int { assert(entries@[right] == before[right]); }
                    else if right == index as int { assert(entries@[left] == before[left]); }
                    else { assert(entries@[left] == before[left]); assert(entries@[right] == before[right]); }
                }
                assert forall|position: int| 0 <= position < entries@.len() implies entries@[position].freq@ != 0 by {
                    if position != index as int { assert(entries@[position] == before[position]); }
                }
            }
        }
    } else {
        let added = HEntry { bucket: bucket.dup(), freq: Wide::from_i128(delta as i128) };
        entries.push(added);
        proof {
            assert(entries@ =~= before.push(added));
            assert forall|other: int| #[trigger] hist_val(entries@, other) == hist_val(before, other) + if other == bucket@ { delta as int } else { 0 } by {
                lemma_sum_push(before, added, hist_term(other));
            }
            assert forall|left: int, right: int| 0 <= left < right < entries@.len() implies entries@[left].bucket@ != entries@[right].bucket@ by {
                if right == before.len() as int { assert(entries@[left] == before[left]); }
                else { assert(entries@[left] == before[left]); assert(entries@[right] == before[right]); }
            }
            assert forall|position: int| 0 <= position < entries@.len() implies entries@[position].freq@ != 0 by {
                if position < before.len() { assert(entries@[position] == before[position]); }
            }
        }
    }
}


impl Variant {
    pub fn dup(&self) -> (out: Self)
        ensures out == *self,
    { Variant { comment: Arc::clone(&self.comment), count: self.count.dup() } }
}

pub fn bytes_eq(left: &[u8], right: &[u8]) -> (result: bool)
    ensures result == (left@ == right@),
{
    if left.len() != right.len() { return false; }
    let mut index = 0usize;
    while index < left.len()
        invariant index <= left.len(), left.len() == right.len(),
            forall|position: int| 0 <= position < index ==> left@[position] == right@[position],
        decreases left.len() - index,
    {
        if left[index] != right[index] { return false; }
        index += 1;
    }
    proof { assert(left@ =~= right@); }
    true
}

/// Adds `delta` to the count of `comment`, appending a variant when it has none; a count that
/// reaches zero is dropped when `drop_zero`.
#[verifier::spinoff_prover]
#[verifier::rlimit(40)]
pub fn variants_add(variants: &mut Vec<Variant>, comment: &Arc<Vec<u8>>, delta: &Wide, drop_zero: bool)
    requires distinct_comments(old(variants)@), drop_zero ==> nonzero_counts(old(variants)@),
    ensures distinct_comments(final(variants)@), drop_zero ==> nonzero_counts(final(variants)@),
        forall|other: Seq<u8>| #[trigger] comment_count(final(variants)@, other) == comment_count(old(variants)@, other)
            + if other == (*comment)@ { delta@ } else { 0 },
{
    let ghost before = variants@;
    let mut index = 0usize;
    let mut found = false;
    while index < variants.len() && !found
        invariant index <= variants.len(), variants@ == before, distinct_comments(before),
            found ==> index < variants.len() && cview(variants@[index as int]) == (*comment)@,
            forall|position: int| 0 <= position < index ==> cview(variants@[position]) != (*comment)@,
        decreases if found { 0int } else { variants.len() - index + 1 },
    {
        if bytes_eq(variants[index].comment.as_slice(), comment.as_slice()) { found = true; } else { index += 1; }
    }
    if found {
        let updated = Variant { comment: Arc::clone(&variants[index].comment), count: variants[index].count.add(delta) };
        proof {
            assert forall|other: Seq<u8>| #[trigger] comment_count(before.update(index as int, updated), other) == comment_count(before, other)
                + if other == (*comment)@ { delta@ } else { 0 } by {
                lemma_sum_update(before, index as int, updated, comment_term(other));
                if other == (*comment)@ {
                    assert(comment_term(other)(updated) == updated.count@);
                    assert(comment_term(other)(before[index as int]) == before[index as int].count@);
                } else {
                    assert(comment_term(other)(updated) == 0);
                    assert(comment_term(other)(before[index as int]) == 0);
                }
            }
        }
        if drop_zero && updated.count.is_zero() {
            variants.remove(index);
            proof {
                let middle = before.update(index as int, updated);
                assert(variants@ =~= middle.remove(index as int));
                assert forall|other: Seq<u8>| #[trigger] comment_count(variants@, other) == comment_count(middle, other) by {
                    lemma_sum_remove(middle, index as int, comment_term(other));
                    assert(comment_term(other)(updated) == 0);
                }
                assert forall|left: int, right: int| 0 <= left < right < variants@.len() implies cview(variants@[left]) != cview(variants@[right]) by {
                    let source_left = if left < index as int { left } else { left + 1 };
                    let source_right = if right < index as int { right } else { right + 1 };
                    assert(variants@[left] == before[source_left]);
                    assert(variants@[right] == before[source_right]);
                }
                assert forall|position: int| 0 <= position < variants@.len() implies variants@[position].count@ != 0 by {
                    let source = if position < index as int { position } else { position + 1 };
                    assert(variants@[position] == before[source]);
                }
            }
        } else {
            variants.set(index, updated);
            proof {
                assert forall|left: int, right: int| 0 <= left < right < variants@.len() implies cview(variants@[left]) != cview(variants@[right]) by {
                    if left == index as int { assert(variants@[right] == before[right]); }
                    else if right == index as int { assert(variants@[left] == before[left]); }
                    else { assert(variants@[left] == before[left]); assert(variants@[right] == before[right]); }
                }
                assert forall|position: int| 0 <= position < variants@.len() && drop_zero implies variants@[position].count@ != 0 by {
                    if position != index as int { assert(variants@[position] == before[position]); }
                }
            }
        }
    } else {
        let added = Variant { comment: Arc::clone(comment), count: delta.dup() };
        if drop_zero && added.count.is_zero() { return; }
        variants.push(added);
        proof {
            assert(variants@ =~= before.push(added));
            assert forall|other: Seq<u8>| #[trigger] comment_count(variants@, other) == comment_count(before, other)
                + if other == (*comment)@ { delta@ } else { 0 } by {
                lemma_sum_push(before, added, comment_term(other));
            }
            assert forall|left: int, right: int| 0 <= left < right < variants@.len() implies cview(variants@[left]) != cview(variants@[right]) by {
                if right == before.len() as int { assert(variants@[left] == before[left]); }
                else { assert(variants@[left] == before[left]); assert(variants@[right] == before[right]); }
            }
            assert forall|position: int| 0 <= position < variants@.len() && drop_zero implies variants@[position].count@ != 0 by {
                if position < before.len() { assert(variants@[position] == before[position]); }
            }
        }
    }
}

}
