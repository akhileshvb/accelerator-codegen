#![verifier::loop_isolation(false)]
// A window's per-supplier counts and exact revenues, tallied from the bucket profiles.
use vstd::prelude::*;
use std::collections::HashMap;
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use verus_prooflib::wide::Wide;
use crate::model::*;
use crate::values::bin;
use crate::query_proofs::*;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties;

/// A supplier's row count and exact revenue inside the window.
pub type Cell = (Wide, Wide);

pub open spec fn cnt_at(map: Map<i64, Cell>, key: i64) -> int {
    if map.contains_key(key) { map[key].0@ } else { 0 }
}

pub open spec fn rev_at(map: Map<i64, Cell>, key: i64) -> int {
    if map.contains_key(key) { map[key].1@ } else { 0 }
}

/// Adds a count and a revenue to a supplier's cell, noting the supplier when it is new.
#[verifier::spinoff_prover]
pub fn add_cell(cells: &mut HashMap<i64, Cell>, keys: &mut Vec<i64>, supplier: i64, count: &Wide, revenue: &Wide)
    requires old(cells)@.dom() == old(keys)@.to_set(), old(keys)@.no_duplicates(),
    ensures final(cells)@.dom() == final(keys)@.to_set(), final(keys)@.no_duplicates(),
        forall|key: i64| #[trigger] cnt_at(final(cells)@, key) == cnt_at(old(cells)@, key) + if key == supplier { count@ } else { 0int },
        forall|key: i64| #[trigger] rev_at(final(cells)@, key) == rev_at(old(cells)@, key) + if key == supplier { revenue@ } else { 0int },
{
    proof { keys@.to_set_ensures(); }
    if !cells.contains_key(&supplier) {
        proof { verus_prooflib::lemma_unique_push(keys@, supplier); }
        keys.push(supplier);
    }
    let (held_count, held_revenue) = match cells.get(&supplier) { Some(cell) => (cell.0.dup(), cell.1.dup()), None => (Wide::zero(), Wide::zero()) };
    cells.insert(supplier, (held_count.add(count), held_revenue.add(revenue)));
    proof { keys@.to_set_ensures(); assert(cells@.dom() =~= keys@.to_set()); }
}

/// The part of a bucket profile that lies in [lower, upper).
#[verifier::spinoff_prover]
pub fn profile_part(profile: &BucketProfile, lower: i32, upper: i32, full: bool, Ghost(bin): Ghost<int>) -> (part: (Wide, Wide))
    requires profile_shape(*profile, bin), full ==> lower as int <= bin * 256 && upper as int >= (bin + 1) * 256,
    ensures part.0@ == selected_count(*profile, lower as int, upper as int), part.1@ == selected_revenue(*profile, lower as int, upper as int),
{
    if full {
        proof { lemma_profile_full(*profile, bin, lower as int, upper as int); }
        return (Wide::from_i128(profile.count as i128), profile.revenue.dup());
    }
    let mut count = Wide::zero();
    let mut revenue = Wide::zero();
    let mut index = 0usize;
    proof {
        assert(profile.days@.take(0) =~= Seq::<Day>::empty());
        lemma_sum_empty(profile.days@.take(0), |day: Day| if lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int { day.count as int } else { 0int });
        lemma_sum_empty(profile.days@.take(0), |day: Day| if lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int { day.revenue@ } else { 0int });
    }
    while index < profile.days.len()
        invariant index <= profile.days.len(),
            count@ == sum(profile.days@.take(index as int), |day: Day| if lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int { day.count as int } else { 0int }),
            revenue@ == sum(profile.days@.take(index as int), |day: Day| if lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int { day.revenue@ } else { 0int }),
        decreases profile.days.len() - index,
    {
        let day = &profile.days[index];
        proof {
            lemma_sum_take(profile.days@, index as int, |day: Day| if lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int { day.count as int } else { 0int });
            lemma_sum_take(profile.days@, index as int, |day: Day| if lower as int <= day.shipdate as int && (day.shipdate as int) < upper as int { day.revenue@ } else { 0int });
        }
        if lower <= day.shipdate && day.shipdate < upper {
            count = count.add(&Wide::from_i128(day.count as i128));
            revenue = revenue.add(&day.revenue);
        }
        index += 1;
    }
    proof { assert(profile.days@.take(profile.days.len() as int) =~= profile.days@); }
    (count, revenue)
}


pub proof fn lemma_facts_window_outside(facts: &Facts, supplier: int, lower: int, upper: int, revenue: bool)
    requires facts.wf(), facts_shape(*facts), !(i64::MIN <= supplier <= i64::MAX),
    ensures facts_window(*facts, supplier, lower, upper, revenue) == 0,
{
    facts.model();
    let weight = facts_window_weight(supplier, lower, upper, revenue);
    assert forall|index: int| 0 <= index < facts.entries().len() implies weight(#[trigger] facts.entries()[index]) == 0 by {
        let entry = facts.entries()[index];
        assert(facts.at(entry.0) == Some(entry.1));
        lemma_profile_lookup(&entry.1, supplier, lower, upper, revenue);
        assert(entry.1.at(number_key(supplier)) is None);
    }
    lemma_sum_zero(facts.entries(), weight);
}

/// Every supplier's count and exact revenue in [lower, upper), from the profiles of the buckets it touches.
#[verifier::spinoff_prover]
#[verifier::rlimit(200)]
pub fn tally(facts: &Facts, lower: i32, upper: i32) -> (result: (HashMap<i64, Cell>, Vec<i64>))
    requires facts_shape(*facts), lower < upper,
    ensures ({ let (cells, keys) = result;
        &&& forall|key: i64| #[trigger] cnt_at(cells@, key) == facts_window(*facts, key as int, lower as int, upper as int, false)
        &&& forall|key: i64| #[trigger] rev_at(cells@, key) == facts_window(*facts, key as int, lower as int, upper as int, true)
        &&& cells@.dom() == keys@.to_set()
        &&& keys@.no_duplicates() }),
{
    let known = facts.share();
    proof { facts.model(); }
    let first_bin = bin(lower);
    let last_bin = bin(upper - 1);
    let mut cells: HashMap<i64, Cell> = HashMap::new();
    let mut keys: Vec<i64> = Vec::new();
    let mut cursor = facts.cursor();
    let ghost entries = facts.entries();
    let ghost mut position = 0int;
    proof {
        keys@.to_set_ensures();
        assert(cells@.dom() =~= keys@.to_set());
        assert(entries.take(0) =~= Seq::<(KeyView, Index<BucketProfile>)>::empty());
        assert forall|key: i64| cnt_at(cells@, key) == sum(entries.take(0), facts_window_weight(key as int, lower as int, upper as int, false)) by {
            lemma_sum_empty(entries.take(0), facts_window_weight(key as int, lower as int, upper as int, false));
        }
        assert forall|key: i64| rev_at(cells@, key) == sum(entries.take(0), facts_window_weight(key as int, lower as int, upper as int, true)) by {
            lemma_sum_empty(entries.take(0), facts_window_weight(key as int, lower as int, upper as int, true));
        }
    }
    loop
        invariant facts.wf(), facts_shape(*facts), lower < upper, entries == facts.entries(), 0 <= position <= entries.len(),
            cursor.remaining() == entries.skip(position), cursor.work() >= 0,
            first_bin as int == bucket(lower as int), last_bin as int == bucket(upper as int - 1),
            forall|index: int| 0 <= index < entries.len() ==> facts.at((#[trigger] entries[index]).0) == Some(entries[index].1),
            cells@.dom() == keys@.to_set(), keys@.no_duplicates(),
            forall|key: i64| #[trigger] cnt_at(cells@, key) == sum(entries.take(position), facts_window_weight(key as int, lower as int, upper as int, false)),
            forall|key: i64| #[trigger] rev_at(cells@, key) == sum(entries.take(position), facts_window_weight(key as int, lower as int, upper as int, true)),
        decreases cursor.work(),
    {
        let ghost pending = cursor.remaining();
        let (bucket_key, suppliers) = match cursor.next() {
            Some(value) => value,
            None => {
                proof {
                    assert(position == entries.len());
                    assert(entries.take(position) =~= entries);
                    assert forall|key: i64| #[trigger] cnt_at(cells@, key) == facts_window(*facts, key as int, lower as int, upper as int, false) by {}
                    assert forall|key: i64| #[trigger] rev_at(cells@, key) == facts_window(*facts, key as int, lower as int, upper as int, true) by {}
                }
                return (cells, keys);
            },
        };
        let ghost entry = entries[position];
        proof {
            assert(entry == (bucket_key.view(), *suppliers));
            assert(facts.at(entry.0) == Some(entry.1));
            assert(entry.0 == number_key(bucket_key.number as int));
        }
        let ghost before = cells@;
        if bucket_key.number >= first_bin && bucket_key.number <= last_bin {
            let full = lower as i128 <= (bucket_key.number as i128) * 256 && upper as i128 >= (bucket_key.number as i128 + 1) * 256;
            let mut inner = suppliers.cursor();
            let ghost inner_entries = entry.1.entries();
            let ghost mut slot = 0int;
            proof {
                assert(inner_entries == entry.1.entries());
                assert(facts_shape(*facts));
                assert(suppliers.wf());
                suppliers.model();
                assert(inner_entries.take(0) =~= Seq::<(KeyView, BucketProfile)>::empty());
                assert forall|key: i64| cnt_at(cells@, key) == cnt_at(before, key) + sum(inner_entries.take(0), pending_weight(key as int, lower as int, upper as int, false)) by {
                    lemma_sum_empty(inner_entries.take(0), pending_weight(key as int, lower as int, upper as int, false));
                }
                assert forall|key: i64| rev_at(cells@, key) == rev_at(before, key) + sum(inner_entries.take(0), pending_weight(key as int, lower as int, upper as int, true)) by {
                    lemma_sum_empty(inner_entries.take(0), pending_weight(key as int, lower as int, upper as int, true));
                }
            }
            loop
                invariant facts_shape(*facts), suppliers.wf(), inner_entries == suppliers.entries(), 0 <= slot <= inner_entries.len(),
                    inner.remaining() == inner_entries.skip(slot), inner.work() >= 0, bucket_key.number as int >= first_bin as int,
                    entry == (bucket_key.view(), *suppliers), facts.at(entry.0) == Some(entry.1),
                    full ==> lower as int <= bucket_key.number as int * 256 && upper as int >= (bucket_key.number as int + 1) * 256,
                    forall|index: int| 0 <= index < inner_entries.len() ==> suppliers.at((#[trigger] inner_entries[index]).0) == Some(inner_entries[index].1),
                    cells@.dom() == keys@.to_set(), keys@.no_duplicates(),
                    forall|key: i64| #[trigger] cnt_at(cells@, key) == cnt_at(before, key) + sum(inner_entries.take(slot), pending_weight(key as int, lower as int, upper as int, false)),
                    forall|key: i64| #[trigger] rev_at(cells@, key) == rev_at(before, key) + sum(inner_entries.take(slot), pending_weight(key as int, lower as int, upper as int, true)),
                decreases inner.work(),
            {
                let (supplier_key, shared) = match inner.next() {
                    Some(value) => value,
                    None => {
                        proof {
                            assert(slot == inner_entries.len());
                            assert(inner_entries.take(slot) =~= inner_entries);
                        }
                        break;
                    },
                };
                let ghost slot_entry = inner_entries[slot];
                proof {
                    assert(slot_entry == (supplier_key.view(), *shared));
                    assert(suppliers.at(slot_entry.0) == Some(slot_entry.1));
                    assert(profile_at(*facts, bucket_key.number as int, supplier_key.number as int) == Some(*shared));
                    assert(slot_entry.0 == number_key(supplier_key.number as int));
                    assert(profile_shape(*shared, bucket_key.number as int));
                }
                let part = profile_part(&*shared, lower, upper, full, Ghost(bucket_key.number as int));
                let ghost previous = cells@;
                add_cell(&mut cells, &mut keys, supplier_key.number, &part.0, &part.1);
                proof {
                    assert forall|key: i64| #[trigger] cnt_at(cells@, key) == cnt_at(before, key) + sum(inner_entries.take(slot + 1), pending_weight(key as int, lower as int, upper as int, false)) by {
                        lemma_sum_take(inner_entries, slot, pending_weight(key as int, lower as int, upper as int, false));
                    }
                    assert forall|key: i64| #[trigger] rev_at(cells@, key) == rev_at(before, key) + sum(inner_entries.take(slot + 1), pending_weight(key as int, lower as int, upper as int, true)) by {
                        lemma_sum_take(inner_entries, slot, pending_weight(key as int, lower as int, upper as int, true));
                    }
                    slot = slot + 1;
                }
            }
            proof {
                assert forall|key: i64| #[trigger] cnt_at(cells@, key) == sum(entries.take(position + 1), facts_window_weight(key as int, lower as int, upper as int, false)) by {
                    lemma_sum_take(entries, position, facts_window_weight(key as int, lower as int, upper as int, false));
                    assert(facts_window_weight(key as int, lower as int, upper as int, false)(entry) == sum(inner_entries, pending_weight(key as int, lower as int, upper as int, false)));
                }
                assert forall|key: i64| #[trigger] rev_at(cells@, key) == sum(entries.take(position + 1), facts_window_weight(key as int, lower as int, upper as int, true)) by {
                    lemma_sum_take(entries, position, facts_window_weight(key as int, lower as int, upper as int, true));
                    assert(facts_window_weight(key as int, lower as int, upper as int, true)(entry) == sum(inner_entries, pending_weight(key as int, lower as int, upper as int, true)));
                }
            }
        } else {
            proof {
                assert(bucket_key.number as int > last_bin as int || (bucket_key.number as int) < first_bin as int);
                assert forall|key: i64| #[trigger] cnt_at(cells@, key) == sum(entries.take(position + 1), facts_window_weight(key as int, lower as int, upper as int, false)) by {
                    lemma_sum_take(entries, position, facts_window_weight(key as int, lower as int, upper as int, false));
                    lemma_bucket_range_zero(facts, entry, key as int, lower as int, upper as int, false);
                }
                assert forall|key: i64| #[trigger] rev_at(cells@, key) == sum(entries.take(position + 1), facts_window_weight(key as int, lower as int, upper as int, true)) by {
                    lemma_sum_take(entries, position, facts_window_weight(key as int, lower as int, upper as int, true));
                    lemma_bucket_range_zero(facts, entry, key as int, lower as int, upper as int, true);
                }
            }
        }
        proof {
            assert(cursor.remaining() == entries.skip(position + 1));
            position = position + 1;
        }
    }
}

}
