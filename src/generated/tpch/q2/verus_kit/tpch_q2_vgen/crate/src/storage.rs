use vstd::prelude::*;
use vstd::std_specs::hash::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;

verus! {

pub struct Leaf<Value> {
    pub base: Arc<HashMap<i64, Arc<Value>>>,
    pub keys: Arc<Vec<i64>>,
    pub overlay: Vec<(i64, Option<Arc<Value>>)>,
}

pub struct Page<Value> {
    pub leaves: Vec<Option<Arc<Leaf<Value>>>>,
}

pub struct Directory<Value> {
    pub pages: Arc<Vec<Option<Arc<Page<Value>>>>>,
    pub length: i128,
}

pub open spec fn slot(key: i64) -> int {
    ((key as u64) % 4096) as int
}

pub open spec fn overlay_lookup<Value>(entries: Seq<(i64, Option<Arc<Value>>)>, key: i64) -> Option<Option<Arc<Value>>>
    decreases entries.len(),
{
    if entries.len() == 0 { None }
    else if entries.last().0 == key { Some(entries.last().1) }
    else { overlay_lookup(entries.drop_last(), key) }
}

pub open spec fn entry_keys<Value>(entries: Seq<(i64, Arc<Value>)>) -> Seq<i64> {
    entries.map_values(|entry: (i64, Arc<Value>)| entry.0)
}

pub open spec fn leaf_population<Value>(entry: Option<Arc<Leaf<Value>>>) -> int {
    match entry { None => 0, Some(leaf) => (*leaf).population() }
}

pub open spec fn page_population<Value>(entry: Option<Arc<Page<Value>>>) -> int {
    match entry { None => 0, Some(page) => sum((*page).leaves@, |leaf: Option<Arc<Leaf<Value>>>| leaf_population(leaf)) }
}

pub open spec fn overlay_keys<Value>(entries: Seq<(i64, Option<Arc<Value>>)>) -> Seq<i64> {
    entries.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0)
}

pub proof fn lemma_overlay_absent<Value>(entries: Seq<(i64, Option<Arc<Value>>)>, key: i64)
    requires !overlay_keys(entries).contains(key),
    ensures overlay_lookup(entries, key) == None,
    decreases entries.len(),
{
    if entries.len() > 0 {
        assert(!overlay_keys(entries.drop_last()).contains(key)) by {
            assert forall|index: int| 0 <= index < entries.drop_last().len() implies
                (#[trigger] entries.drop_last()[index]).0 != key by {
                assert(overlay_keys(entries)[index] == entries[index].0);
            }
        }
        assert(overlay_keys(entries)[entries.len() - 1] == entries.last().0);
        lemma_overlay_absent(entries.drop_last(), key);
    }
}

pub proof fn lemma_overlay_unique<Value>(entries: Seq<(i64, Option<Arc<Value>>)>, index: int)
    requires
        0 <= index < entries.len(), overlay_keys(entries).no_duplicates(),
    ensures overlay_lookup(entries, entries[index].0) == Some(entries[index].1),
    decreases entries.len(),
{
    if index < entries.len() - 1 {
        assert(overlay_keys(entries.drop_last()) =~= overlay_keys(entries).drop_last());
        assert(overlay_keys(entries)[index] != overlay_keys(entries)[entries.len() - 1]);
        lemma_overlay_unique(entries.drop_last(), index);
    }
}

pub proof fn lemma_entry_push<Value>(entries: Seq<(i64, Arc<Value>)>, entry: (i64, Arc<Value>))
    ensures entry_keys(entries.push(entry)) == entry_keys(entries).push(entry.0),
{
    assert(entry_keys(entries.push(entry)) =~= entry_keys(entries).push(entry.0));
}

pub open spec fn present<Value>(value: Option<Arc<Value>>) -> int {
    if value is Some { 1 } else { 0 }
}

pub proof fn lemma_overlay_update<Value>(entries: Seq<(i64, Option<Arc<Value>>)>, index: int, value: Option<Arc<Value>>, key: i64)
    requires 0 <= index < entries.len(), overlay_keys(entries).no_duplicates(),
    ensures overlay_lookup(entries.update(index, (entries[index].0, value)), key) ==
        if key == entries[index].0 { Some(value) } else { overlay_lookup(entries, key) },
    decreases entries.len(),
{
    if index < entries.len() - 1 {
        assert(overlay_keys(entries.drop_last()) =~= overlay_keys(entries).drop_last());
        assert(entries.update(index, (entries[index].0, value)).drop_last() =~=
            entries.drop_last().update(index, (entries[index].0, value)));
        lemma_overlay_update(entries.drop_last(), index, value, key);
        if key == entries[index].0 {
            assert(overlay_keys(entries)[index] == key);
            assert(overlay_keys(entries)[entries.len() - 1] == entries.last().0);
            assert(entries.last().0 != key);
        }
    } else if key != entries[index].0 {
        assert(entries.update(index, (entries[index].0, value)).drop_last() =~= entries.drop_last());
    }
}

pub proof fn lemma_population_replace<Value>(before: Leaf<Value>, after: Leaf<Value>, key: i64, value: Option<Arc<Value>>)
    requires
        before.valid(slot(key)), after.valid(slot(key)), before.base == after.base, before.keys == after.keys,
        after.lookup(key) == value,
        forall|probe: i64| probe != key ==> #[trigger] after.lookup(probe) == before.lookup(probe),
        after.overlay@ == before.overlay@.push((key, value)) ||
            exists|index: int| 0 <= index < before.overlay@.len() && (#[trigger] before.overlay@[index]).0 == key &&
                after.overlay@ == before.overlay@.update(index, (key, value)),
    ensures after.population() == before.population() + present(value) - present(before.lookup(key)),
{
    let before_weight = |probe: i64| present(before.lookup(probe));
    let after_weight = |probe: i64| present(after.lookup(probe));
    let difference = |probe: i64| after_weight(probe) - before_weight(probe);
    assert forall|probe: i64| #[trigger] difference(probe) ==
        if probe == key { present(value) - present(before.lookup(key)) } else { 0int } by {}
    lemma_sum_sub((*before.keys)@, after_weight, before_weight, difference);
    if (*before.base)@.contains_key(key) {
        let witness = choose|index: int| 0 <= index < (*before.keys)@.len() && #[trigger] (*before.keys)@[index] == key;
        assert forall|index: int| 0 <= index < (*before.keys)@.len() && index != witness implies difference(#[trigger] (*before.keys)@[index]) == 0 by {
            assert((*before.keys)@[index] != (*before.keys)@[witness]);
        }
        lemma_sum_single_match((*before.keys)@, difference, witness);
    } else {
        lemma_sum_zero((*before.keys)@, difference);
    }
    let overlay_weight = |entry: (i64, Option<Arc<Value>>)| if !(*before.base)@.contains_key(entry.0) { present(entry.1) } else { 0int };
    if after.overlay@ == before.overlay@.push((key, value)) {
        lemma_sum_push(before.overlay@, (key, value), overlay_weight);
        assert(overlay_keys(after.overlay@).no_duplicates());
        assert(overlay_keys(after.overlay@) =~= overlay_keys(before.overlay@).push(key));
        assert(!overlay_keys(before.overlay@).contains(key)) by {
            broadcast use vstd::seq_lib::group_seq_properties;
            if overlay_keys(before.overlay@).contains(key) {
                let witness = choose|index: int| 0 <= index < overlay_keys(before.overlay@).len() && #[trigger] overlay_keys(before.overlay@)[index] == key;
                assert(overlay_keys(after.overlay@)[witness] == overlay_keys(after.overlay@)[after.overlay@.len() - 1]);
            }
        }
        lemma_overlay_absent(before.overlay@, key);
    } else {
        let witness = choose|index: int| 0 <= index < before.overlay@.len() && (#[trigger] before.overlay@[index]).0 == key &&
            after.overlay@ == before.overlay@.update(index, (key, value));
        assert(overlay_keys(before.overlay@).no_duplicates());
        lemma_overlay_unique(before.overlay@, witness);
        lemma_sum_update(before.overlay@, witness, (key, value), overlay_weight);
    }
    lemma_sum_ext_all((*before.keys)@, |probe: i64| if before.lookup(probe) is Some { 1int } else { 0int }, before_weight);
    lemma_sum_ext_all((*after.keys)@, |probe: i64| if after.lookup(probe) is Some { 1int } else { 0int }, after_weight);
    lemma_sum_ext_all(before.overlay@, |entry: (i64, Option<Arc<Value>>)|
        if !(*before.base)@.contains_key(entry.0) && entry.1 is Some { 1int } else { 0int }, overlay_weight);
    lemma_sum_ext_all(after.overlay@, |entry: (i64, Option<Arc<Value>>)|
        if !(*after.base)@.contains_key(entry.0) && entry.1 is Some { 1int } else { 0int }, overlay_weight);
}

impl<Value> Leaf<Value> {
    pub proof fn population_upper(&self)
        ensures self.population() <= (*self.keys)@.len() + self.overlay@.len(),
    {
        let base_weight = |key: i64| if self.lookup(key) is Some { 1int } else { 0int };
        let overlay_weight = |entry: (i64, Option<Arc<Value>>)|
            if !(*self.base)@.contains_key(entry.0) && entry.1 is Some { 1int } else { 0int };
        lemma_sum_le((*self.keys)@, base_weight, |key: i64| 1int);
        lemma_sum_le(self.overlay@, overlay_weight, |entry: (i64, Option<Arc<Value>>)| 1int);
        verus_prooflib::lemma_sum_ones((*self.keys)@);
        verus_prooflib::lemma_sum_ones(self.overlay@);
    }
    pub proof fn lookup_address(&self, key: i64, address: int)
        requires self.valid(address), self.lookup(key) is Some,
        ensures slot(key) == address,
    {
        if !overlay_keys(self.overlay@).contains(key) {
            lemma_overlay_absent(self.overlay@, key);
        } else {
            let witness = choose|index: int| 0 <= index < self.overlay@.len() && (#[trigger] self.overlay@[index]).0 == key;
        }
    }

    pub proof fn population_nonnegative(&self)
        ensures self.population() >= 0,
    {
        lemma_sum_nonneg((*self.keys)@, |key: i64| if self.lookup(key) is Some { 1int } else { 0int });
        lemma_sum_nonneg(self.overlay@, |entry: (i64, Option<Arc<Value>>)|
            if !(*self.base)@.contains_key(entry.0) && entry.1 is Some { 1int } else { 0int });
    }

    pub proof fn population_positive(&self, key: i64)
        requires self.valid(slot_for_leaf(self)), self.lookup(key) is Some,
        ensures self.population() > 0,
    {
        self.population_nonnegative();
        let base_weight = |key: i64| if self.lookup(key) is Some { 1int } else { 0int };
        let overlay_weight = |entry: (i64, Option<Arc<Value>>)|
            if !(*self.base)@.contains_key(entry.0) && entry.1 is Some { 1int } else { 0int };
        lemma_sum_nonneg((*self.keys)@, base_weight);
        lemma_sum_nonneg(self.overlay@, overlay_weight);
        if (*self.base)@.contains_key(key) {
            let witness = choose|index: int| 0 <= index < (*self.keys)@.len() && #[trigger] (*self.keys)@[index] == key;
            lemma_sum_pos((*self.keys)@, base_weight, witness);
        } else {
            if !overlay_keys(self.overlay@).contains(key) { lemma_overlay_absent(self.overlay@, key); }
            let witness = choose|index: int| 0 <= index < self.overlay@.len() && (#[trigger] self.overlay@[index]).0 == key;
            assert(overlay_keys(self.overlay@).no_duplicates());
            lemma_overlay_unique(self.overlay@, witness);
            lemma_sum_pos(self.overlay@, overlay_weight, witness);
        }
    }

    pub open spec fn population(self) -> int {
        sum((*self.keys)@, |key: i64| if self.lookup(key) is Some { 1 } else { 0 }) +
        sum(self.overlay@, |entry: (i64, Option<Arc<Value>>)|
            if !(*self.base)@.contains_key(entry.0) && entry.1 is Some { 1 } else { 0 })
    }

    pub open spec fn lookup(self, key: i64) -> Option<Arc<Value>> {
        match overlay_lookup(self.overlay@, key) {
            Some(value) => value,
            None => if (*self.base)@.contains_key(key) { Some((*self.base)@[key]) } else { None },
        }
    }

    pub open spec fn valid(self, address: int) -> bool {
        &&& (*self.keys)@.len() <= usize::MAX as int
        &&& forall|key: i64| #[trigger] (*self.base)@.contains_key(key)
            ==> slot(key) == address && (*self.keys)@.contains(key)
        &&& forall|index: int| 0 <= index < (*self.keys)@.len()
            ==> (*self.base)@.contains_key(#[trigger] (*self.keys)@[index])
        &&& (*self.keys)@.no_duplicates()
        &&& forall|index: int| 0 <= index < self.overlay@.len()
            ==> slot((#[trigger] self.overlay@[index]).0) == address
        &&& forall|first: int, second: int| 0 <= first < second < self.overlay@.len()
            ==> (#[trigger] self.overlay@[first]).0 != (#[trigger] self.overlay@[second]).0
    }

    pub fn get(&self, key: i64) -> (result: Option<Arc<Value>>)
        ensures result == self.lookup(key),
    {
        let mut index = self.overlay.len();
        proof { assert(self.overlay@.take(index as int) =~= self.overlay@); }
        while index > 0
            invariant index <= self.overlay.len(),
                overlay_lookup(self.overlay@, key) == overlay_lookup(self.overlay@.take(index as int), key),
            decreases index,
        {
            index -= 1;
            proof {
                assert(self.overlay@.take(index as int + 1).last() == self.overlay@[index as int]);
                assert(self.overlay@.take(index as int + 1).drop_last() =~= self.overlay@.take(index as int));
            }
            if self.overlay[index].0 == key {
                return clone_option(&self.overlay[index].1);
            }
        }
        match self.base.get(&key) {
            Some(value) => Some(Arc::clone(value)),
            None => None,
        }
    }

    pub fn entries(&self) -> (result: Vec<(i64, Arc<Value>)>)
        requires self.valid(slot_for_leaf(self)),
        ensures
            forall|index: int| 0 <= index < result@.len()
                ==> self.lookup((#[trigger] result@[index]).0) == Some(result@[index].1),
            forall|key: i64| #[trigger] self.lookup(key) is Some
                ==> result@.map_values(|entry: (i64, Arc<Value>)| entry.0).contains(key),
            result@.map_values(|entry: (i64, Arc<Value>)| entry.0).no_duplicates(),
            result@.len() == self.population(),
    {
        let mut result = Vec::new();
        let mut index = 0;
        while index < self.keys.len()
            invariant
                index <= self.keys.len(), self.valid(slot_for_leaf(self)),
                entry_keys(result@).no_duplicates(),
                forall|offset: int| 0 <= offset < result@.len() ==>
                    self.lookup((#[trigger] result@[offset]).0) == Some(result@[offset].1),
                forall|key: i64| #[trigger] entry_keys(result@).contains(key) ==
                    (self.lookup(key) is Some && (*self.keys)@.take(index as int).contains(key)),
                result@.len() == sum((*self.keys)@.take(index as int), |key: i64| if self.lookup(key) is Some { 1int } else { 0int }),
            decreases self.keys.len() - index,
        {
            let key = self.keys[index];
            let ghost before = result@;
            let ghost was_live = self.lookup(key) is Some;
            if let Some(value) = self.get(key) {
                proof {
                    assert(!(*self.keys)@.take(index as int).contains(key)) by {
                        if (*self.keys)@.take(index as int).contains(key) {
                            let witness = choose|offset: int| 0 <= offset < index && #[trigger] (*self.keys)@[offset] == key;
                            assert((*self.keys)@[witness] != (*self.keys)@[index as int]);
                        }
                    }
                }
                result.push((key, value));
                proof {
                    lemma_entry_push(before, (key, value));
                    verus_prooflib::lemma_unique_push(entry_keys(before), key);
                }
            }
            proof {
                assert((*self.keys)@.take(index as int + 1) =~= (*self.keys)@.take(index as int).push(key));
                lemma_sum_take((*self.keys)@, index as int, |key: i64| if self.lookup(key) is Some { 1int } else { 0int });
                assert forall|probe: i64| #[trigger] entry_keys(result@).contains(probe) ==
                    (self.lookup(probe) is Some && (*self.keys)@.take(index as int + 1).contains(probe)) by {
                    broadcast use vstd::seq_lib::group_seq_properties;
                    assert(entry_keys(before).contains(probe) == (self.lookup(probe) is Some && (*self.keys)@.take(index as int).contains(probe)));
                    assert((*self.keys)@.take(index as int + 1).contains(probe) == ((*self.keys)@.take(index as int).contains(probe) || probe == key));
                    if was_live {
                        assert(entry_keys(result@) =~= entry_keys(before).push(key));
                    } else { assert(result@ == before); }
                }
            }
            index += 1;
        }
        proof { assert((*self.keys)@.take(index as int) =~= (*self.keys)@); }
        let mut offset = 0;
        while offset < self.overlay.len()
            invariant
                offset <= self.overlay.len(), self.valid(slot_for_leaf(self)),
                entry_keys(result@).no_duplicates(),
                forall|index: int| 0 <= index < result@.len() ==>
                    self.lookup((#[trigger] result@[index]).0) == Some(result@[index].1),
                forall|key: i64| #[trigger] entry_keys(result@).contains(key) ==
                    (self.lookup(key) is Some && ((*self.keys)@.contains(key) ||
                        overlay_keys(self.overlay@.take(offset as int)).contains(key))),
                result@.len() == sum((*self.keys)@, |key: i64| if self.lookup(key) is Some { 1int } else { 0int }) +
                    sum(self.overlay@.take(offset as int), |entry: (i64, Option<Arc<Value>>)|
                        if !(*self.base)@.contains_key(entry.0) && entry.1 is Some { 1int } else { 0int }),
            decreases self.overlay.len() - offset,
        {
            let key = self.overlay[offset].0;
            let ghost before = result@;
            proof {
                assert(overlay_keys(self.overlay@).no_duplicates());
                lemma_overlay_unique(self.overlay@, offset as int);
            }
            if !self.base.contains_key(&key) {
                if let Some(value) = self.get(key) {
                    proof {
                        assert(!overlay_keys(self.overlay@.take(offset as int)).contains(key)) by {
                            if overlay_keys(self.overlay@.take(offset as int)).contains(key) {
                                let witness = choose|index: int| 0 <= index < offset && (#[trigger] self.overlay@[index]).0 == key;
                            }
                        }
                        assert(!(*self.keys)@.contains(key)) by {
                            if (*self.keys)@.contains(key) {
                                let witness = choose|index: int| 0 <= index < (*self.keys)@.len() && #[trigger] (*self.keys)@[index] == key;
                            }
                        }
                    }
                    result.push((key, value));
                    proof {
                        lemma_entry_push(before, (key, value));
                        verus_prooflib::lemma_unique_push(entry_keys(before), key);
                    }
                }
            }
            proof {
                assert(overlay_keys(self.overlay@.take(offset as int + 1)) =~= overlay_keys(self.overlay@.take(offset as int)).push(key));
                lemma_sum_take(self.overlay@, offset as int, |entry: (i64, Option<Arc<Value>>)|
                    if !(*self.base)@.contains_key(entry.0) && entry.1 is Some { 1int } else { 0int });
                assert forall|probe: i64| #[trigger] entry_keys(result@).contains(probe) ==
                    (self.lookup(probe) is Some && ((*self.keys)@.contains(probe) ||
                        overlay_keys(self.overlay@.take(offset as int + 1)).contains(probe))) by {
                    broadcast use vstd::seq_lib::group_seq_properties;
                    assert(entry_keys(before).contains(probe) == (self.lookup(probe) is Some && ((*self.keys)@.contains(probe) || overlay_keys(self.overlay@.take(offset as int)).contains(probe))));
                    assert(overlay_keys(self.overlay@.take(offset as int + 1)).contains(probe) == (overlay_keys(self.overlay@.take(offset as int)).contains(probe) || probe == key));
                    if result@.len() > before.len() {
                        assert(entry_keys(result@) =~= entry_keys(before).push(key));
                    } else { assert(result@ == before); }
                    if probe == key && (*self.base)@.contains_key(key) { assert((*self.keys)@.contains(key)); }
                }
            }
            offset += 1;
        }
        proof {
            assert(self.overlay@.take(offset as int) =~= self.overlay@);
            assert forall|key: i64| #[trigger] self.lookup(key) is Some implies entry_keys(result@).contains(key) by {
                broadcast use vstd::seq_lib::group_seq_properties;
                if !overlay_keys(self.overlay@).contains(key) {
                    lemma_overlay_absent(self.overlay@, key);
                }
            }
        }
        result
    }
}

pub open spec fn slot_for_leaf<Value>(leaf: &Leaf<Value>) -> int {
    if (*leaf.keys)@.len() > 0 { slot((*leaf.keys)@[0]) }
    else if leaf.overlay@.len() > 0 { slot(leaf.overlay@[0].0) } else { 0 }
}

pub fn clone_option<Value>(value: &Option<Arc<Value>>) -> (result: Option<Arc<Value>>)
    ensures result == *value,
{
    match value { Some(inner) => Some(Arc::clone(inner)), None => None }
}

impl<Value> Directory<Value> {
    pub proof fn population_upper(&self)
        requires self.valid(),
        ensures self.length < i128::MAX,
    {
        let leaf_bound = usize::MAX as int + 32;
        let page_bound = 64 * leaf_bound;
        assert forall|page_index: int| 0 <= page_index < 64 implies
            page_population(#[trigger] (*self.pages)@[page_index]) <= page_bound by {
            if (*self.pages)@[page_index] is Some {
                let page = (*self.pages)@[page_index]->0;
                assert forall|leaf_index: int| 0 <= leaf_index < 64 implies
                    leaf_population(#[trigger] (*page).leaves@[leaf_index]) <= leaf_bound by {
                    if (*page).leaves@[leaf_index] is Some {
                        let leaf = (*page).leaves@[leaf_index]->0;
                        (*leaf).population_upper();
                        assert((*(*leaf).keys)@.len() <= usize::MAX as int);
                    }
                }
                let capped = |entry: Option<Arc<Leaf<Value>>>| if leaf_population(entry) <= leaf_bound { leaf_population(entry) } else { leaf_bound };
                lemma_sum_ext((*page).leaves@, |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry), capped);
                lemma_sum_le((*page).leaves@, capped, |entry: Option<Arc<Leaf<Value>>>| leaf_bound);
                lemma_sum_scale((*page).leaves@, |entry: Option<Arc<Leaf<Value>>>| 1int, |entry: Option<Arc<Leaf<Value>>>| leaf_bound, leaf_bound);
                verus_prooflib::lemma_sum_ones((*page).leaves@);
                assert((*page).leaves@.len() == 64);
                assert(sum((*page).leaves@, |entry: Option<Arc<Leaf<Value>>>| leaf_bound) == leaf_bound * 64);
                assert(sum((*page).leaves@, |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry)) <= leaf_bound * 64);
                assert(page_population((*self.pages)@[page_index]) == sum((*page).leaves@, |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry)));
            }
        }
        let capped = |entry: Option<Arc<Page<Value>>>| if page_population(entry) <= page_bound { page_population(entry) } else { page_bound };
        lemma_sum_ext((*self.pages)@, |entry: Option<Arc<Page<Value>>>| page_population(entry), capped);
        lemma_sum_le((*self.pages)@, capped, |entry: Option<Arc<Page<Value>>>| page_bound);
        lemma_sum_scale((*self.pages)@, |entry: Option<Arc<Page<Value>>>| 1int, |entry: Option<Arc<Page<Value>>>| page_bound, page_bound);
        verus_prooflib::lemma_sum_ones((*self.pages)@);
        assert(sum((*self.pages)@, |entry: Option<Arc<Page<Value>>>| page_bound) == page_bound * 64);
        assert(self.length as int <= 4096 * (usize::MAX as int + 32));
        assert(usize::MAX as int <= u64::MAX as int);
    }
    pub open spec fn valid(self) -> bool {
        &&& (*self.pages)@.len() == 64
        &&& self.length as int == sum((*self.pages)@, |entry: Option<Arc<Page<Value>>>| page_population(entry))
        &&& self.length >= 0
        &&& forall|page: int| 0 <= page < 64 ==> match #[trigger] (*self.pages)@[page] {
            None => true,
            Some(data) => (*data).leaves@.len() == 64 &&
                forall|leaf: int| 0 <= leaf < 64 ==> match #[trigger] (*data).leaves@[leaf] {
                    None => true,
                    Some(bucket) => (*bucket).valid(page * 64 + leaf) && (*bucket).overlay@.len() <= 32,
                },
        }
    }

    pub open spec fn lookup(self, key: i64) -> Option<Arc<Value>> {
        let address = slot(key);
        match (*self.pages)@[address / 64] {
            None => None,
            Some(page) => match (*page).leaves@[address % 64] {
                None => None,
                Some(leaf) => (*leaf).lookup(key),
            },
        }
    }

    pub fn new() -> (result: Self)
        ensures result.valid(), forall|key: i64| #[trigger] result.lookup(key) == None,
    {
        let mut pages = Vec::new();
        let mut index = 0;
        while index < 64
            invariant pages.len() == index, index <= 64,
                forall|offset: int| 0 <= offset < pages@.len() ==> #[trigger] pages@[offset] == None,
            decreases 64 - index,
        {
            pages.push(None);
            index += 1;
        }
        proof {
            lemma_sum_zero(pages@, |entry: Option<Arc<Page<Value>>>| page_population(entry));
        }
        Directory { pages: Arc::new(pages), length: 0 }
    }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    {
        Directory { pages: Arc::clone(&self.pages), length: self.length }
    }

    pub fn is_empty(&self) -> (result: bool)
        requires self.valid(),
        ensures
            result == (self.length == 0),
            result ==> forall|key: i64| #[trigger] self.lookup(key) == None,
    {
        let result = self.length == 0;
        proof {
            assert forall|key: i64| result implies #[trigger] self.lookup(key) == None by {
                if self.lookup(key) is Some {
                    let address = slot(key);
                    let page = (*self.pages)@[address / 64]->0;
                    let leaf = (*page).leaves@[address % 64]->0;
                    assert((*leaf).valid(slot_for_leaf(&*leaf)));
                    (*leaf).population_positive(key);
                    assert forall|offset: int| 0 <= offset < (*page).leaves@.len() implies
                        leaf_population(#[trigger] (*page).leaves@[offset]) >= 0 by {
                        if (*page).leaves@[offset] is Some { (*(*page).leaves@[offset]->0).population_nonnegative(); }
                    }
                    lemma_sum_ge_term((*page).leaves@, |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry), address % 64);
                    assert forall|offset: int| 0 <= offset < (*self.pages)@.len() implies
                        page_population(#[trigger] (*self.pages)@[offset]) >= 0 by {
                        if (*self.pages)@[offset] is Some {
                            let data = (*self.pages)@[offset]->0;
                            assert forall|index: int| 0 <= index < (*data).leaves@.len() implies
                                leaf_population(#[trigger] (*data).leaves@[index]) >= 0 by {
                                if (*data).leaves@[index] is Some { (*(*data).leaves@[index]->0).population_nonnegative(); }
                            }
                            lemma_sum_nonneg((*data).leaves@, |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry));
                        }
                    }
                    lemma_sum_ge_term((*self.pages)@, |entry: Option<Arc<Page<Value>>>| page_population(entry), address / 64);
                }
            }
        }
        result
    }

    pub fn get(&self, key: i64) -> (result: Option<Arc<Value>>)
        requires self.valid(),
        ensures result == self.lookup(key),
    {
        let address = ((key as u64) % 4096) as usize;
        match &self.pages[address / 64] {
            None => None,
            Some(page) => match &page.leaves[address % 64] {
                None => None,
                Some(leaf) => leaf.get(key),
            },
        }
    }

    pub fn replace(&mut self, key: i64, value: Option<Arc<Value>>)
        requires old(self).valid(),
        ensures
            final(self).valid(), final(self).lookup(key) == value,
            forall|other: i64| other != key ==> #[trigger] final(self).lookup(other) == old(self).lookup(other),
    {
        let ghost original = *self;
        proof { self.population_upper(); }
        let was_present = self.get(key).is_some();
        let was_empty = self.is_empty();
        proof { assert(was_present ==> !was_empty); }
        let is_present = value.is_some();
        let address = ((key as u64) % 4096) as usize;
        let page_index = address / 64;
        let leaf_index = address % 64;
        let mut pages = Vec::new();
        let mut index = 0;
        while index < 64
            invariant index <= 64, pages.len() == index, self.valid(),
                pages@ == (*self.pages)@.take(index as int),
            decreases 64 - index,
        {
            pages.push(clone_option(&self.pages[index]));
            index += 1;
        }
        proof { assert(pages@ =~= (*self.pages)@); }
        let mut leaves = Vec::new();
        let mut offset = 0;
        while offset < 64
            invariant
                offset <= 64, leaves.len() == offset, self.valid(), pages@ == (*self.pages)@,
                page_index < 64, leaf_index < 64,
                forall|index: int| 0 <= index < leaves@.len() ==> #[trigger] leaves@[index] == match (*self.pages)@[page_index as int] {
                    None => None, Some(page) => (*page).leaves@[index],
                },
            decreases 64 - offset,
        {
            leaves.push(match &pages[page_index] {
                None => None,
                Some(page) => clone_option(&page.leaves[offset]),
            });
            offset += 1;
        }
        let empty = Leaf { base: Arc::new(HashMap::new()), keys: Arc::new(Vec::new()), overlay: Vec::new() };
        let before = match &leaves[leaf_index] { None => &empty, Some(leaf) => &**leaf };
        let ghost original_leaf = *before;
        proof {
            assert(before.valid(address as int));
            assert(before.lookup(key) == original.lookup(key));
            assert(overlay_keys(before.overlay@).no_duplicates());
        }
        let mut overlay = Vec::new();
        let mut cursor = 0;
        while cursor < before.overlay.len()
            invariant cursor <= before.overlay.len(), overlay@ == before.overlay@.take(cursor as int),
            decreases before.overlay.len() - cursor,
        {
            overlay.push((before.overlay[cursor].0, clone_option(&before.overlay[cursor].1)));
            cursor += 1;
        }
        proof { assert(overlay@ =~= before.overlay@); }
        let mut position = 0;
        while position < overlay.len() && overlay[position].0 != key
            invariant position <= overlay.len(), overlay@ == before.overlay@,
                forall|index: int| 0 <= index < position ==> (#[trigger] overlay@[index]).0 != key,
            decreases overlay.len() - position,
        {
            position += 1;
        }
        proof { assert(position < overlay.len() ==> before.overlay@[position as int].0 == key); }
        if position < overlay.len() { overlay.set(position, (key, value)); }
        else { overlay.push((key, value)); }
        let base = Arc::clone(&before.base);
        let keys = Arc::clone(&before.keys);
        let mut leaf = Leaf { base, keys, overlay };
        proof {
            if position < before.overlay@.len() {
                assert(before.overlay@[position as int].0 == key);
                assert(overlay_keys(leaf.overlay@) =~= overlay_keys(before.overlay@));
            } else {
                assert(overlay_keys(leaf.overlay@) =~= overlay_keys(before.overlay@).push(key));
                verus_prooflib::lemma_unique_push(overlay_keys(before.overlay@), key);
            }
            assert(leaf.valid(address as int));
            assert(leaf.valid(slot_for_leaf(&leaf)));
            assert forall|probe: i64| #[trigger] leaf.lookup(probe) == if probe == key { value } else { before.lookup(probe) } by {
                if position < before.overlay@.len() {
                    assert(leaf.overlay@ == before.overlay@.update(position as int, (before.overlay@[position as int].0, value)));
                    lemma_overlay_update(before.overlay@, position as int, value, probe);
                } else {
                    assert(leaf.overlay@ == before.overlay@.push((key, value)));
                    assert(leaf.overlay@.drop_last() =~= before.overlay@);
                    assert(leaf.overlay@.last() == (key, value));
                }
                assert(overlay_lookup(leaf.overlay@, probe) == if probe == key { Some(value) } else { overlay_lookup(before.overlay@, probe) });
            }
            lemma_population_replace(*before, leaf, key, value);
        }
        let ghost updated_leaf = leaf;
        if leaf.overlay.len() > 32 {
            let entries = leaf.entries();
            let mut new_base = HashMap::new();
            let mut new_keys = Vec::new();
            let mut cursor = 0;
            while cursor < entries.len()
                invariant cursor <= entries.len(),
                    entry_keys(entries@).no_duplicates(),
                    new_keys@ == entry_keys(entries@.take(cursor as int)),
                    forall|probe: i64| #[trigger] new_base@.contains_key(probe) == entry_keys(entries@.take(cursor as int)).contains(probe),
                    forall|probe: i64| #[trigger] new_base@.contains_key(probe) ==> Some(new_base@[probe]) == updated_leaf.lookup(probe),
                    forall|index: int| 0 <= index < entries@.len() ==>
                        updated_leaf.lookup((#[trigger] entries@[index]).0) == Some(entries@[index].1),
                    forall|probe: i64| (#[trigger] updated_leaf.lookup(probe) is Some) == entry_keys(entries@).contains(probe),
                    updated_leaf.valid(address as int), entries@.len() == updated_leaf.population(),
                decreases entries.len() - cursor,
            {
                let ghost previous_keys = new_keys@;
                new_base.insert(entries[cursor].0, Arc::clone(&entries[cursor].1));
                new_keys.push(entries[cursor].0);
                proof {
                    broadcast use vstd::seq_lib::group_seq_properties;
                    assert(entry_keys(entries@.take(cursor as int + 1)) =~= previous_keys.push(entries@[cursor as int].0));
                }
                cursor += 1;
            }
            proof { assert(entries@.take(cursor as int) =~= entries@); }
            let key_count = new_keys.len();
            leaf = Leaf { base: Arc::new(new_base), keys: Arc::new(new_keys), overlay: Vec::new() };
            proof {
                assert(leaf.valid(address as int)) by {
                    assert forall|probe: i64| #[trigger] (*leaf.base)@.contains_key(probe) implies slot(probe) == address && (*leaf.keys)@.contains(probe) by {
                        updated_leaf.lookup_address(probe, address as int);
                    }
                }
                assert forall|probe: i64| #[trigger] leaf.lookup(probe) == updated_leaf.lookup(probe) by {}
                let weight = |probe: i64| if leaf.lookup(probe) is Some { 1int } else { 0int };
                lemma_sum_ext((*leaf.keys)@, weight, |probe: i64| 1int);
                verus_prooflib::lemma_sum_ones((*leaf.keys)@);
                assert(leaf.population() == updated_leaf.population());
            }
        }
        let ghost final_leaf = leaf;
        let ghost previous_leaves = leaves@;
        leaves.set(leaf_index, Some(Arc::new(leaf)));
        let ghost final_leaves = leaves@;
        let ghost previous_pages = pages@;
        pages.set(page_index, Some(Arc::new(Page { leaves })));
        proof {
            lemma_sum_update(previous_leaves, leaf_index as int, final_leaves[leaf_index as int], |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry));
            lemma_sum_update(previous_pages, page_index as int, pages@[page_index as int], |entry: Option<Arc<Page<Value>>>| page_population(entry));
            if original.lookup(key) is Some { assert(was_present); }
            if (*original.pages)@[page_index as int] is None {
                lemma_sum_zero(previous_leaves, |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry));
            } else {
                assert(previous_leaves =~= (*((*original.pages)@[page_index as int]->0)).leaves@);
            }
            assert(sum(pages@, |entry: Option<Arc<Page<Value>>>| page_population(entry)) == original.length as int +
                present(value) - present(original.lookup(key)));
        }
        self.pages = Arc::new(pages);
        if was_present && !is_present { self.length -= 1; }
        if !was_present && is_present { self.length += 1; }
        proof {
            assert forall|page: int| 0 <= page < 64 implies match #[trigger] (*self.pages)@[page] {
                None => true,
                Some(data) => (*data).leaves@.len() == 64 &&
                    forall|offset: int| 0 <= offset < 64 ==> match #[trigger] (*data).leaves@[offset] {
                        None => true,
                        Some(bucket) => (*bucket).valid(page * 64 + offset) && (*bucket).overlay@.len() <= 32,
                    },
            } by {
                if page == page_index {
                    assert forall|offset: int| 0 <= offset < 64 implies match #[trigger] final_leaves[offset] {
                        None => true,
                        Some(bucket) => (*bucket).valid(page * 64 + offset) && (*bucket).overlay@.len() <= 32,
                    } by {
                        if offset == leaf_index {
                            assert((*final_leaves[offset]->0).valid(address as int));
                        }
                    }
                }
            }
        }
    }

    pub fn entries(&self) -> (result: Vec<(i64, Arc<Value>)>)
        requires self.valid(),
        ensures
            forall|index: int| 0 <= index < result@.len()
                ==> self.lookup((#[trigger] result@[index]).0) == Some(result@[index].1),
            forall|key: i64| #[trigger] self.lookup(key) is Some
                ==> result@.map_values(|entry: (i64, Arc<Value>)| entry.0).contains(key),
            result@.map_values(|entry: (i64, Arc<Value>)| entry.0).no_duplicates(),
            result@.len() == self.length as int,
    {
        let mut result = Vec::new();
        let mut page_index = 0;
        while page_index < 64
            invariant
                page_index <= 64, self.valid(), entry_keys(result@).no_duplicates(),
                forall|index: int| 0 <= index < result@.len() ==>
                    self.lookup((#[trigger] result@[index]).0) == Some(result@[index].1),
                forall|key: i64| #[trigger] entry_keys(result@).contains(key) ==
                    (self.lookup(key) is Some && slot(key) / 64 < page_index),
                result@.len() == sum((*self.pages)@.take(page_index as int), |entry: Option<Arc<Page<Value>>>| page_population(entry)),
            decreases 64 - page_index,
        {
            if let Some(page) = &self.pages[page_index] {
                let mut leaf_index = 0;
                while leaf_index < 64
                    invariant
                        leaf_index <= 64, self.valid(), page_index < 64, page.leaves.len() == 64,
                        (*self.pages)@[page_index as int] == Some(*page),
                        entry_keys(result@).no_duplicates(),
                        forall|index: int| 0 <= index < result@.len() ==>
                            self.lookup((#[trigger] result@[index]).0) == Some(result@[index].1),
                        forall|key: i64| #[trigger] entry_keys(result@).contains(key) == (self.lookup(key) is Some &&
                            (slot(key) / 64 < page_index || slot(key) / 64 == page_index && slot(key) % 64 < leaf_index)),
                        result@.len() == sum((*self.pages)@.take(page_index as int), |entry: Option<Arc<Page<Value>>>| page_population(entry)) +
                            sum(page.leaves@.take(leaf_index as int), |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry)),
                    decreases 64 - leaf_index,
                {
                    if let Some(leaf) = &page.leaves[leaf_index] {
                        proof { assert((*leaf).valid(slot_for_leaf(&**leaf))); }
                        let entries = leaf.entries();
                        let ghost initial = result@;
                        let mut cursor = 0;
                        while cursor < entries.len()
                            invariant
                                cursor <= entries.len(), self.valid(), page_index < 64, leaf_index < 64,
                                (*self.pages)@[page_index as int] == Some(*page), page.leaves@[leaf_index as int] == Some(*leaf),
                                (*leaf).valid(page_index as int * 64 + leaf_index as int),
                                entry_keys(result@).no_duplicates(),
                                result@ == initial + entries@.take(cursor as int),
                                forall|index: int| 0 <= index < result@.len() ==>
                                    self.lookup((#[trigger] result@[index]).0) == Some(result@[index].1),
                                forall|index: int| 0 <= index < entries@.len() ==>
                                    (*leaf).lookup((#[trigger] entries@[index]).0) == Some(entries@[index].1),
                                entry_keys(entries@).no_duplicates(), entries.len() as int == (*leaf).population(),
                                forall|key: i64| (#[trigger] (*leaf).lookup(key) is Some) == entry_keys(entries@).contains(key),
                                forall|key: i64| #[trigger] entry_keys(initial).contains(key) == (self.lookup(key) is Some &&
                                    (slot(key) / 64 < page_index || slot(key) / 64 == page_index && slot(key) % 64 < leaf_index)),
                                initial.len() == sum((*self.pages)@.take(page_index as int), |entry: Option<Arc<Page<Value>>>| page_population(entry)) +
                                    sum(page.leaves@.take(leaf_index as int), |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry)),
                            decreases entries.len() - cursor,
                        {
                            let key = entries[cursor].0;
                            proof {
                                (*leaf).lookup_address(key, page_index as int * 64 + leaf_index as int);
                                assert(!entry_keys(initial).contains(key));
                                verus_prooflib::lemma_prefix_fresh(entry_keys(entries@), cursor as int);
                                assert(entry_keys(result@) =~= entry_keys(initial) + entry_keys(entries@).take(cursor as int));
                                assert(!entry_keys(result@).contains(key)) by {
                                    broadcast use vstd::seq_lib::group_seq_properties;
                                }
                                verus_prooflib::lemma_unique_push(entry_keys(result@), key);
                            }
                            let ghost previous = result@;
                            result.push((entries[cursor].0, Arc::clone(&entries[cursor].1)));
                            proof {
                                lemma_entry_push(previous, entries@[cursor as int]);
                                assert(entries@.take(cursor as int + 1) =~= entries@.take(cursor as int).push(entries@[cursor as int]));
                                assert(result@ =~= initial + entries@.take(cursor as int + 1));
                            }
                            cursor += 1;
                        }
                        proof {
                            assert(entries@.take(cursor as int) =~= entries@);
                            assert forall|key: i64| #[trigger] entry_keys(result@).contains(key) == (self.lookup(key) is Some &&
                                (slot(key) / 64 < page_index || slot(key) / 64 == page_index && slot(key) % 64 < leaf_index + 1)) by {
                                broadcast use vstd::seq_lib::group_seq_properties;
                                assert(entry_keys(result@) =~= entry_keys(initial) + entry_keys(entries@));
                                if entry_keys(entries@).contains(key) { (*leaf).lookup_address(key, page_index as int * 64 + leaf_index as int); }
                                if self.lookup(key) is Some && slot(key) / 64 == page_index && slot(key) % 64 == leaf_index {
                                    assert((*leaf).lookup(key) is Some);
                                }
                            }
                        }
                    }
                    proof {
                        lemma_sum_take(page.leaves@, leaf_index as int, |entry: Option<Arc<Leaf<Value>>>| leaf_population(entry));
                    }
                    leaf_index += 1;
                }
                proof {
                    assert(page.leaves@.take(leaf_index as int) =~= page.leaves@);
                }
            }
            proof {
                lemma_sum_take((*self.pages)@, page_index as int, |entry: Option<Arc<Page<Value>>>| page_population(entry));
            }
            page_index += 1;
        }
        proof { assert((*self.pages)@.take(page_index as int) =~= (*self.pages)@); }
        result
    }
}

}
