#![verifier::loop_isolation(false)]
// A batch of signed order and customer changes grouped per customer key, and its merge into a record.
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::wide::Wide;
use crate::store::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;

pub struct OChange { pub custkey: i64, pub comment: Arc<Vec<u8>>, pub sign: i64 }

pub struct CChange { pub custkey: i64, pub sign: i64 }

pub struct Group { pub mult: Wide, pub changes: Vec<Variant> }

pub open spec fn group_cc(groups: Map<i64, Group>, key: i64, comment: Seq<u8>) -> int {
    if groups.contains_key(key) { comment_count(groups[key].changes@, comment) } else { 0 }
}

pub open spec fn group_mult(groups: Map<i64, Group>, key: i64) -> int {
    if groups.contains_key(key) { groups[key].mult@ } else { 0 }
}

pub open spec fn groups_ok(groups: Map<i64, Group>) -> bool {
    forall|key: i64| #[trigger] groups.contains_key(key) ==> distinct_comments(groups[key].changes@)
}

pub open spec fn order_term(key: int, comment: Seq<u8>) -> spec_fn(OChange) -> int {
    |change: OChange| change.sign as int * ind(change.custkey as int == key && (*change.comment)@ == comment)
}

pub open spec fn customer_term(key: int) -> spec_fn(CChange) -> int {
    |change: CChange| change.sign as int * ind(change.custkey as int == key)
}

/// Groups the order changes of a batch by customer key.
#[verifier::spinoff_prover]
#[verifier::rlimit(60)]
pub fn group_orders(groups: &mut HashMap<i64, Group>, keys: &mut Vec<i64>, batch: &Vec<OChange>)
    requires groups_ok(old(groups)@), old(groups)@.dom() == old(keys)@.to_set(), old(keys)@.no_duplicates(),
    ensures groups_ok(final(groups)@), final(groups)@.dom() == final(keys)@.to_set(), final(keys)@.no_duplicates(),
        forall|key: i64, comment: Seq<u8>| #[trigger] group_cc(final(groups)@, key, comment)
            == group_cc(old(groups)@, key, comment) + sum(batch@, order_term(key as int, comment)),
        forall|key: i64| #[trigger] group_mult(final(groups)@, key) == group_mult(old(groups)@, key),
        forall|position: int| 0 <= position < batch@.len() ==> final(groups)@.contains_key(#[trigger] batch@[position].custkey),
        forall|key: i64| old(groups)@.contains_key(key) ==> #[trigger] final(groups)@.contains_key(key),
{
    let mut index = 0usize;
    proof {
        assert forall|key: i64, comment: Seq<u8>| #[trigger] group_cc(groups@, key, comment)
            == group_cc(old(groups)@, key, comment) + sum(batch@.take(0), order_term(key as int, comment)) by {
            assert(batch@.take(0) =~= Seq::<OChange>::empty());
            lemma_sum_empty(batch@.take(0), order_term(key as int, comment));
        }
    }
    while index < batch.len()
        invariant index <= batch.len(), groups_ok(groups@), groups@.dom() == keys@.to_set(), keys@.no_duplicates(),
            forall|key: i64, comment: Seq<u8>| #[trigger] group_cc(groups@, key, comment)
                == group_cc(old(groups)@, key, comment) + sum(batch@.take(index as int), order_term(key as int, comment)),
            forall|key: i64| #[trigger] group_mult(groups@, key) == group_mult(old(groups)@, key),
            forall|position: int| 0 <= position < index ==> groups@.contains_key(#[trigger] batch@[position].custkey),
            forall|key: i64| old(groups)@.contains_key(key) ==> #[trigger] groups@.contains_key(key),
        decreases batch.len() - index,
    {
        let change = &batch[index];
        let ghost before = groups@;
        proof { keys@.to_set_ensures(); }
        if !groups.contains_key(&change.custkey) {
            proof { verus_prooflib::lemma_unique_push(keys@, change.custkey); }
            groups.insert(change.custkey, Group { mult: Wide::zero(), changes: Vec::new() });
            keys.push(change.custkey);
        }
        let ghost middle = groups@;
        let mut group = groups.remove(&change.custkey).unwrap();
        let ghost old_changes = group.changes@;
        variants_add(&mut group.changes, &change.comment, &Wide::from_i128(change.sign as i128), false);
        let ghost new_changes = group.changes@;
        groups.insert(change.custkey, group);
        proof {
            keys@.to_set_ensures();
            assert(groups@.dom() =~= keys@.to_set());
            assert forall|key: i64, comment: Seq<u8>| #[trigger] group_cc(groups@, key, comment)
                == group_cc(old(groups)@, key, comment) + sum(batch@.take(index as int + 1), order_term(key as int, comment)) by {
                lemma_sum_take(batch@, index as int, order_term(key as int, comment));
                let term = order_term(key as int, comment)(batch@[index as int]);
                assert(group_cc(before, key, comment) == group_cc(old(groups)@, key, comment) + sum(batch@.take(index as int), order_term(key as int, comment)));
                lemma_sum_empty(Seq::<Variant>::empty(), comment_term(comment));
                if key == change.custkey {
                    assert(group_cc(groups@, key, comment) == comment_count(new_changes, comment));
                    assert(group_cc(middle, key, comment) == comment_count(old_changes, comment));
                    assert(group_cc(before, key, comment) == group_cc(middle, key, comment));
                    if (*change.comment)@ == comment { assert(term == change.sign as int * 1); } else { assert(term == 0); }
                } else {
                    assert(group_cc(groups@, key, comment) == group_cc(middle, key, comment));
                    assert(term == 0);
                }
            }
            assert forall|key: i64| #[trigger] group_mult(groups@, key) == group_mult(old(groups)@, key) by {
                assert(group_mult(before, key) == group_mult(old(groups)@, key));
                if key == change.custkey && !before.contains_key(key) { assert(group_mult(middle, key) == 0); }
            }
            assert forall|key: i64| #[trigger] groups@.contains_key(key) implies distinct_comments(groups@[key].changes@) by {
                if key != change.custkey { assert(middle.contains_key(key)); }
            }
        }
        proof { assert forall|position: int| 0 <= position < index as int + 1 implies groups@.contains_key(#[trigger] batch@[position].custkey) by { if position < index as int { assert(before.contains_key(batch@[position].custkey)); } } }
        index += 1;
    }
    proof { assert(batch@.take(batch@.len() as int) =~= batch@); }
}

/// Groups the customer changes of a batch by customer key.
#[verifier::spinoff_prover]
#[verifier::rlimit(60)]
pub fn group_customers(groups: &mut HashMap<i64, Group>, keys: &mut Vec<i64>, batch: &Vec<CChange>)
    requires groups_ok(old(groups)@), old(groups)@.dom() == old(keys)@.to_set(), old(keys)@.no_duplicates(),
    ensures groups_ok(final(groups)@), final(groups)@.dom() == final(keys)@.to_set(), final(keys)@.no_duplicates(),
        forall|key: i64, comment: Seq<u8>| #[trigger] group_cc(final(groups)@, key, comment) == group_cc(old(groups)@, key, comment),
        forall|key: i64| #[trigger] group_mult(final(groups)@, key) == group_mult(old(groups)@, key) + sum(batch@, customer_term(key as int)),
        forall|position: int| 0 <= position < batch@.len() ==> final(groups)@.contains_key(#[trigger] batch@[position].custkey),
        forall|key: i64| old(groups)@.contains_key(key) ==> #[trigger] final(groups)@.contains_key(key),
{
    let mut index = 0usize;
    proof {
        assert forall|key: i64| #[trigger] group_mult(groups@, key) == group_mult(old(groups)@, key) + sum(batch@.take(0), customer_term(key as int)) by {
            assert(batch@.take(0) =~= Seq::<CChange>::empty());
            lemma_sum_empty(batch@.take(0), customer_term(key as int));
        }
    }
    while index < batch.len()
        invariant index <= batch.len(), groups_ok(groups@), groups@.dom() == keys@.to_set(), keys@.no_duplicates(),
            forall|key: i64, comment: Seq<u8>| #[trigger] group_cc(groups@, key, comment) == group_cc(old(groups)@, key, comment),
            forall|key: i64| #[trigger] group_mult(groups@, key)
                == group_mult(old(groups)@, key) + sum(batch@.take(index as int), customer_term(key as int)),
            forall|position: int| 0 <= position < index ==> groups@.contains_key(#[trigger] batch@[position].custkey),
            forall|key: i64| old(groups)@.contains_key(key) ==> #[trigger] groups@.contains_key(key),
        decreases batch.len() - index,
    {
        let change = &batch[index];
        let ghost before = groups@;
        proof { keys@.to_set_ensures(); }
        if !groups.contains_key(&change.custkey) {
            proof { verus_prooflib::lemma_unique_push(keys@, change.custkey); }
            groups.insert(change.custkey, Group { mult: Wide::zero(), changes: Vec::new() });
            keys.push(change.custkey);
        }
        let ghost middle = groups@;
        let mut group = groups.remove(&change.custkey).unwrap();
        let ghost old_changes = group.changes@;
        group.mult = group.mult.add(&Wide::from_i128(change.sign as i128));
        let ghost new_mult = group.mult@;
        groups.insert(change.custkey, group);
        proof {
            keys@.to_set_ensures();
            assert(groups@.dom() =~= keys@.to_set());
            assert forall|key: i64| #[trigger] group_mult(groups@, key)
                == group_mult(old(groups)@, key) + sum(batch@.take(index as int + 1), customer_term(key as int)) by {
                lemma_sum_take(batch@, index as int, customer_term(key as int));
                let term = customer_term(key as int)(batch@[index as int]);
                assert(group_mult(before, key) == group_mult(old(groups)@, key) + sum(batch@.take(index as int), customer_term(key as int)));
                if key == change.custkey {
                    assert(group_mult(middle, key) == group_mult(before, key));
                    assert(group_mult(groups@, key) == group_mult(middle, key) + change.sign as int);
                    assert(term == change.sign as int * 1);
                } else {
                    assert(group_mult(groups@, key) == group_mult(middle, key));
                    assert(term == 0);
                }
            }
            assert forall|key: i64, comment: Seq<u8>| #[trigger] group_cc(groups@, key, comment) == group_cc(old(groups)@, key, comment) by {
                lemma_sum_empty(Seq::<Variant>::empty(), comment_term(comment));
                assert(group_cc(before, key, comment) == group_cc(old(groups)@, key, comment));
                if key == change.custkey {
                    assert(groups@[key].changes@ == old_changes);
                    assert(middle[key].changes@ == old_changes);
                    assert(group_cc(groups@, key, comment) == comment_count(old_changes, comment));
                    assert(group_cc(middle, key, comment) == comment_count(old_changes, comment));
                    if before.contains_key(key) { assert(middle[key] == before[key]); }
                    else { assert(comment_count(old_changes, comment) == 0); }
                } else { assert(group_cc(groups@, key, comment) == group_cc(middle, key, comment)); if before.contains_key(key) { assert(middle[key] == before[key]); } }
            }
            assert forall|key: i64| #[trigger] groups@.contains_key(key) implies distinct_comments(groups@[key].changes@) by {
                if key != change.custkey { assert(middle.contains_key(key)); }
            }
        }
        proof { assert forall|position: int| 0 <= position < index as int + 1 implies groups@.contains_key(#[trigger] batch@[position].custkey) by { if position < index as int { assert(before.contains_key(batch@[position].custkey)); } } }
        index += 1;
    }
    proof { assert(batch@.take(batch@.len() as int) =~= batch@); }
}

pub open spec fn rec_mult_of(rec: Option<Rec>) -> int {
    match rec { Some(rec) => rec.mult@, None => 0 }
}

pub open spec fn rec_cc_of(rec: Option<Rec>, comment: Seq<u8>) -> int {
    match rec { Some(rec) => comment_count(rec.orders@, comment), None => 0 }
}

/// The record after adding a group's changes; None when nothing is left.
#[verifier::spinoff_prover]
#[verifier::rlimit(60)]
pub fn merge_rec(old: Option<&Rec>, group: &Group) -> (result: Option<Rec>)
    requires old matches Some(rec) ==> rec_ok(*rec), distinct_comments(group.changes@),
    ensures result matches Some(rec) ==> rec_ok(rec),
        rec_mult_of(result) == rec_mult_of(match old { Some(rec) => Some(*rec), None => None }) + group.mult@,
        forall|comment: Seq<u8>| #[trigger] rec_cc_of(result, comment)
            == rec_cc_of(match old { Some(rec) => Some(*rec), None => None }, comment) + comment_count(group.changes@, comment),
{
    let mut variants: Vec<Variant> = Vec::new();
    let ghost old_view = match old { Some(rec) => Some(*rec), None => None };
    let ghost start = match old { Some(rec) => rec.orders@, None => Seq::<Variant>::empty() };
    if let Some(rec) = old {
        let mut index = 0usize;
        while index < rec.orders.len()
            invariant index <= rec.orders.len(), variants@ == rec.orders@.take(index as int),
            decreases rec.orders.len() - index,
        {
            variants.push(rec.orders[index].dup());
            index += 1;
        }
        proof { assert(rec.orders@.take(rec.orders@.len() as int) =~= rec.orders@); assert(variants@ == rec.orders@); }
    }
    proof {
        assert(variants@ == start);
        assert forall|comment: Seq<u8>| #[trigger] comment_count(variants@, comment) == rec_cc_of(old_view, comment) by {
            if old is None { lemma_sum_empty(variants@, comment_term(comment)); }
        }
    }
    let mut index = 0usize;
    while index < group.changes.len()
        invariant index <= group.changes.len(), distinct_comments(variants@), nonzero_counts(variants@),
            distinct_comments(group.changes@),
            forall|comment: Seq<u8>| #[trigger] comment_count(variants@, comment)
                == rec_cc_of(old_view, comment) + comment_count(group.changes@.take(index as int), comment),
        decreases group.changes.len() - index,
    {
        let change = &group.changes[index];
        proof {
            assert forall|comment: Seq<u8>| #[trigger] comment_count(group.changes@.take(index as int + 1), comment)
                == comment_count(group.changes@.take(index as int), comment) + if comment == cview(group.changes@[index as int]) { change.count@ } else { 0 } by {
                lemma_sum_take(group.changes@, index as int, comment_term(comment));
            }
        }
        variants_add(&mut variants, &change.comment, &change.count, true);
        index += 1;
    }
    proof { assert(group.changes@.take(group.changes@.len() as int) =~= group.changes@); }
    let mult = match old { Some(rec) => rec.mult.add(&group.mult), None => group.mult.dup() };
    proof {
        assert(mult@ == rec_mult_of(old_view) + group.mult@);
        assert forall|comment: Seq<u8>| #[trigger] comment_count(variants@, comment) == rec_cc_of(old_view, comment) + comment_count(group.changes@, comment) by {
            assert(group.changes@.take(group.changes@.len() as int) =~= group.changes@);
        }
    }
    if mult.is_zero() && variants.len() == 0 {
        proof {
            assert forall|comment: Seq<u8>| #[trigger] rec_cc_of(None::<Rec>, comment) == rec_cc_of(old_view, comment) + comment_count(group.changes@, comment) by {
                lemma_sum_empty(variants@, comment_term(comment));
                assert(comment_count(variants@, comment) == 0);
            }
        }
        return None;
    }
    Some(Rec { mult, orders: variants })
}

}
