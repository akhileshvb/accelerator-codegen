use vstd::prelude::*;
use std::collections::HashMap;
use verus_prooflib::wide::Wide;
use crate::pages::*;
use crate::moments::*;
use crate::algebra::*;

verus! {

broadcast use { vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties };

/// A signed change to a page entry; exact, since a transaction may carry any number of rows.
pub struct Change {
    pub count: Wide,
    pub total: Wide,
}

impl Change {
    pub open spec fn model(self) -> (int, int) { (self.count@, self.total@) }

    pub fn zero() -> (result: Change)
        ensures result.model() == (0int, 0int),
    { Change { count: Wide::zero(), total: Wide::zero() } }

    pub fn of(sign: i128, balance: i64) -> (result: Change)
        ensures result.model() == (sign as int, sign * balance),
    {
        Change { count: Wide::from_i128(sign), total: Wide::from_i128(sign).mul(&Wide::from_i128(balance as i128)) }
    }

    pub fn dup(&self) -> (result: Change)
        ensures result.model() == self.model(),
    { Change { count: self.count.dup(), total: self.total.dup() } }

    pub fn add(&self, other: &Change) -> (result: Change)
        ensures result.model() == plus(self.model(), other.model()),
    { Change { count: self.count.add(&other.count), total: self.total.add(&other.total) } }
}

pub open spec fn scratch_value(values: Map<u128, Change>, key: u128) -> (int, int) {
    if values.dom().contains(key) { values[key].model() } else { (0, 0) }
}

pub open spec fn hist_key(key: u128) -> bool {
    exists|code: u32, depth: u32, interval: u64|
        code <= 65792 && depth <= 50 && interval < (1u64 << depth) && key == #[trigger] histogram_key(code, depth, interval)
}

pub open spec fn finish_fits(target: Pages, changes: Map<u128, Change>) -> bool {
    forall|key: u128| #[trigger] changes.dom().contains(key) ==>
        fits(target.value(key).0 + changes[key].count@) && fits(target.value(key).1 + changes[key].total@)
}

pub fn finish_changes(target: &mut Pages, changes: &HashMap<u128, Change>, keys: &Vec<u128>) -> (result: bool)
    requires keys@.no_duplicates(), changes@.dom() == keys@.to_set(),
        forall|index: int| 0 <= index < keys.len() ==> (#[trigger] keys@[index]) < KEY_LIMIT,
    ensures result ==> forall|key: u128| key < KEY_LIMIT ==>
        #[trigger] final(target).value(key) == plus(old(target).value(key), scratch_value(changes@, key)),
        finish_fits(*old(target), changes@) ==> result,
{
    let initial = Ghost(*target);
    let mut index = 0usize;
    while index < keys.len()
        invariant index <= keys.len(), initial@ == *old(target),
            keys@.no_duplicates(), changes@.dom() == keys@.to_set(),
            forall|position: int| 0 <= position < keys.len() ==> (#[trigger] keys@[position]) < KEY_LIMIT,
            forall|key: u128| key < KEY_LIMIT ==> #[trigger] target.value(key) ==
                if keys@.take(index as int).contains(key) { plus(initial@.value(key), scratch_value(changes@, key)) }
                else { initial@.value(key) },
        decreases keys.len() - index,
    {
        let key = keys[index];
        proof { assert(!keys@.take(index as int).contains(key)); assert(keys@.to_set().contains(key)); }
        let change = match changes.get(&key) { Some(value) => value.dup(), None => Change::zero() };
        let current = target.get(key);
        let count = Wide::from_i128(current.count).add(&change.count).to_i128();
        let total = Wide::from_i128(current.total).add(&change.total).to_i128();
        let (count, total) = match (count, total) { (Some(count), Some(total)) => (count, total), _ => {
            proof {
                assert(!finish_fits(initial@, changes@)) by {
                    if finish_fits(initial@, changes@) {
                        assert(changes@.dom().contains(key));
                        assert(change.model() == changes@[key].model());
                        assert(current.model() == initial@.value(key));
                        assert(false);
                    }
                }
            }
            return false;
        } };
        let touched = !change.count.is_zero() || !change.total.is_zero();
        if touched { target.set(key, Moment { count, total }); }
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            assert forall|other: u128| other < KEY_LIMIT implies #[trigger] target.value(other) ==
                if keys@.take(index as int + 1).contains(other) { plus(initial@.value(other), scratch_value(changes@, other)) }
                else { initial@.value(other) } by {
                if other == key { assert(scratch_value(changes@, other) == change.model()); }
            }
        }
        index += 1;
    }
    proof { assert(keys@.take(index as int) =~= keys@); }
    true
}

pub fn scratch_add(changes: &mut HashMap<u128, Change>, keys: &mut Vec<u128>, key: u128, change: Change)
    requires old(keys)@.no_duplicates(), old(changes)@.dom() == old(keys)@.to_set(), key < KEY_LIMIT,
        forall|position: int| 0 <= position < old(keys)@.len() ==> (#[trigger] old(keys)@[position]) < KEY_LIMIT,
    ensures
        final(keys)@.no_duplicates() && final(changes)@.dom() == final(keys)@.to_set() &&
        (forall|position: int| 0 <= position < final(keys)@.len() ==> (#[trigger] final(keys)@[position]) < KEY_LIMIT) &&
        final(changes)@.dom() == old(changes)@.dom().insert(key) &&
        forall|other: u128| #[trigger] scratch_value(final(changes)@, other) ==
            if other == key { plus(scratch_value(old(changes)@, other), change.model()) }
            else { scratch_value(old(changes)@, other) },
{
    let original = Ghost(changes@);
    let original_keys = Ghost(keys@);
    let before = match changes.get(&key) { Some(value) => value.dup(), None => { keys.push(key); Change::zero() } };
    let value = before.add(&change);
    changes.insert(key, value);
    proof {
        if !original@.dom().contains(key) {
            lemma_push_set(original_keys@, key);
            assert(keys@.no_duplicates());
            assert(keys@.to_set() =~= original_keys@.to_set().insert(key));
        }
        assert forall|other: u128| #[trigger] scratch_value(changes@, other) ==
            if other == key { plus(scratch_value(original@, other), change.model()) }
            else { scratch_value(original@, other) } by { }
    }
}

pub fn scratch_histogram(changes: &mut HashMap<u128, Change>, keys: &mut Vec<u128>, code: u32, balance: i64, sign: i128)
    requires code <= 65792, 0 < balance < LIMIT,
        old(keys)@.no_duplicates(), old(changes)@.dom() == old(keys)@.to_set(),
        forall|position: int| 0 <= position < old(keys)@.len() ==> (#[trigger] old(keys)@[position]) < KEY_LIMIT,
    ensures
        final(keys)@.no_duplicates() && final(changes)@.dom() == final(keys)@.to_set() &&
        (forall|position: int| 0 <= position < final(keys)@.len() ==> (#[trigger] final(keys)@[position]) < KEY_LIMIT) &&
        (forall|key: u128| final(changes)@.dom().contains(key) ==> old(changes)@.dom().contains(key) || hist_key(key)) &&
        forall|key: u128| #[trigger] scratch_value(final(changes)@, key) ==
            plus(scratch_value(old(changes)@, key), ancestor_moment(code, balance, sign, key, 51)),
{
    let initial = Ghost(changes@);
    let mut depth = 0u32;
    while depth <= 50
        invariant depth <= 51, keys@.no_duplicates(), changes@.dom() == keys@.to_set(),
            initial@ == old(changes)@, code <= 65792, 0 < balance < LIMIT,
            forall|position: int| 0 <= position < keys@.len() ==> (#[trigger] keys@[position]) < KEY_LIMIT,
            forall|key: u128| changes@.dom().contains(key) ==> initial@.dom().contains(key) || hist_key(key),
            forall|key: u128| #[trigger] scratch_value(changes@, key) ==
                plus(scratch_value(initial@, key), ancestor_moment(code, balance, sign, key, depth as int)),
        decreases 51 - depth,
    {
        proof { lemma_interval_bound(balance, depth); }
        let key = hkey(code, depth, (balance as u64) >> (50 - depth));
        proof { assert(hist_key(key)); }
        scratch_add(changes, keys, key, Change::of(sign, balance));
        proof {
            assert forall|other: u128| #[trigger] scratch_value(changes@, other) ==
                plus(scratch_value(initial@, other), ancestor_moment(code, balance, sign, other, depth as int + 1)) by {
                lemma_ancestor_step(code, balance, sign, other, depth);
            }
        }
        depth += 1;
    }
}

}
