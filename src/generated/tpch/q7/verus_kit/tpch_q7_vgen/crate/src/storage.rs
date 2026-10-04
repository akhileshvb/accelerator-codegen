use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::*;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub trait Bucket {
    spec fn empty(self) -> bool;
    fn is_empty(&self) -> (result: bool)
        ensures result == self.empty();
}

impl Bucket for i128 {
    open spec fn empty(self) -> bool { self == 0 }
    fn is_empty(&self) -> (result: bool)
        ensures result == self.empty(),
    { *self == 0 }
}

pub open spec fn normalize<Value: Bucket>(entry: Option<Value>) -> Option<Value> {
    match entry { Some(value) => if value.empty() { None } else { Some(value) }, None => None }
}

pub closed spec fn raw_prefix<Value: Bucket>(base: Sparse<Value>, tiers: Seq<Arc<Sparse<Value>>>, length: nat, key: i64) -> Option<Value>
    decreases length,
{
    if length == 0 { base.at(key) }
    else if tiers[length as int - 1].at(key).is_some() { tiers[length as int - 1].at(key) }
    else { raw_prefix(base, tiers, (length - 1) as nat, key) }
}

pub closed spec fn raw_keys<Value: Bucket>(base: Sparse<Value>, tiers: Seq<Arc<Sparse<Value>>>, length: nat) -> Set<i64>
    decreases length,
{
    if length == 0 { base.index@.dom() }
    else { raw_keys(base, tiers, (length - 1) as nat).union(tiers[length as int - 1].index@.dom()) }
}

proof fn raw_present_key<Value: Bucket>(base: Sparse<Value>, tiers: Seq<Arc<Sparse<Value>>>, length: nat, key: i64)
    requires length <= tiers.len(),
    ensures raw_prefix(base, tiers, length, key).is_some() ==> raw_keys(base, tiers, length).contains(key),
    decreases length,
{
    if length > 0 { raw_present_key(base, tiers, (length - 1) as nat, key); }
}

proof fn raw_prefix_ext<Value: Bucket>(base: Sparse<Value>, first: Seq<Arc<Sparse<Value>>>, second: Seq<Arc<Sparse<Value>>>, length: nat)
    requires length <= first.len(), length <= second.len(), first.take(length as int) == second.take(length as int),
    ensures forall|key: i64| #![trigger raw_prefix(base, first, length, key)]
        raw_prefix(base, first, length, key) == raw_prefix(base, second, length, key),
    decreases length,
{
    if length > 0 {
        assert(first.take(length as int - 1) =~= second.take(length as int - 1));
        assert(first.take(length as int)[length as int - 1] == first[length as int - 1]);
        assert(second.take(length as int)[length as int - 1] == second[length as int - 1]);
        assert(first[length as int - 1] == second[length as int - 1]);
        raw_prefix_ext(base, first, second, (length - 1) as nat);
    }
    assert forall|key: i64| #![trigger raw_prefix(base, first, length, key)]
        raw_prefix(base, first, length, key) == raw_prefix(base, second, length, key) by {
        reveal_with_fuel(raw_prefix, 2);
        if length > 0 {
            assert(first[length as int - 1].at(key) == second[length as int - 1].at(key));
            assert(raw_prefix(base, first, (length - 1) as nat, key) == raw_prefix(base, second, (length - 1) as nat, key));
        }
    }
}

pub struct Sparse<Value: Bucket> {
    entries: Vec<(i64, Arc<Value>)>,
    index: HashMap<i64, usize>,
}

impl<Value: Bucket> Sparse<Value> {
    pub closed spec fn view(self) -> Seq<(i64, Arc<Value>)> { self.entries@ }

    pub fn rows(&self) -> (result: &Vec<(i64, Arc<Value>)>)
        ensures result@ == self.view(),
    { &self.entries }

    pub closed spec fn at(self, key: i64) -> Option<Value> {
        if self.index@.contains_key(key) {
            Some(*self.entries@[self.index@[key] as int].1)
        } else { None }
    }

    pub closed spec fn valid(self) -> bool {
        &&& forall|key: i64| #![trigger self.index@.contains_key(key)] self.index@.contains_key(key) ==>
            self.index@[key] < self.entries@.len()
            && self.entries@[self.index@[key] as int].0 == key
        &&& forall|offset: int| #![trigger self.entries@[offset]]
            0 <= offset < self.entries@.len() ==>
            self.index@.contains_key(self.entries@[offset].0)
            && self.index@[self.entries@[offset].0] == offset
    }

    pub proof fn row_facts(&self)
        requires self.valid(),
        ensures
            forall|offset: int| #![trigger self.view()[offset]]
                0 <= offset < self.view().len() ==>
                    self.at(self.view()[offset].0) == Some(*self.view()[offset].1),
            forall|key: i64| #![trigger self.at(key)] self.at(key).is_some() ==>
                exists|offset: int| #![trigger self.view()[offset]]
                    0 <= offset < self.view().len()
                        && self.view()[offset].0 == key
                        && self.at(key) == Some(*self.view()[offset].1),
    {
        assert forall|key: i64| #![trigger self.at(key)] self.at(key).is_some() implies
            exists|offset: int| #![trigger self.view()[offset]]
                0 <= offset < self.view().len() && self.view()[offset].0 == key
                    && self.at(key) == Some(*self.view()[offset].1) by {
            let offset = self.index@[key] as int;
            assert(self.view()[offset].0 == key);
        }
    }

    pub proof fn unique_keys(&self)
        requires self.valid(),
        ensures distinct(self.view().map_values(|entry: (i64, Arc<Value>)| entry.0)),
    { self.key_facts(); }

    proof fn key_facts(&self)
        requires self.valid(),
        ensures distinct(self.view().map_values(|entry: (i64, Arc<Value>)| entry.0)),
            self.index@.dom() == self.view().map_values(|entry: (i64, Arc<Value>)| entry.0).to_set(),
    {
        broadcast use vstd::seq_lib::group_seq_properties;
        let keys = self.view().map_values(|entry: (i64, Arc<Value>)| entry.0);
        keys.to_set_ensures();
        assert forall|first: int, second: int| #![trigger keys[first], keys[second]]
            0 <= first < second < keys.len() implies keys[first] != keys[second] by {
            assert(self.index@[keys[first]] == first);
            assert(self.index@[keys[second]] == second);
        }
        assert(self.index@.dom() =~= keys.to_set()) by {
            assert forall|key: i64| #![trigger self.index@.dom().contains(key)]
                self.index@.dom().contains(key) implies keys.to_set().contains(key) by {
                let offset = self.index@[key] as int;
                assert(keys[offset] == key);
                assert(keys.contains(key));
            }
            assert forall|key: i64| #![trigger keys.to_set().contains(key)]
                keys.to_set().contains(key) implies self.index@.dom().contains(key) by {
                let offset = choose|offset: int| 0 <= offset < keys.len() && #[trigger] keys[offset] == key;
                assert(self.entries@[offset].0 == key);
            }
        }
    }

    pub fn new() -> (result: Self)
        ensures result.valid(), result.view().len() == 0,
            forall|key: i64| #![trigger result.at(key)] result.at(key).is_none(),
    { Self { entries: Vec::new(), index: HashMap::new() } }

    pub fn get(&self, key: i64) -> (result: Option<&Value>)
        requires self.valid(),
        ensures result matches Some(value) ==> self.at(key) == Some(*value),
                result.is_none() ==> self.at(key).is_none(),
    {
        match self.index.get(&key) {
            Some(offset) => Some(&*self.entries[*offset].1),
            None => None,
        }
    }

    pub fn shared(&self, key: i64) -> (result: Option<Arc<Value>>)
        requires self.valid(),
        ensures result matches Some(value) ==> self.at(key) == Some(*value),
                result.is_none() ==> self.at(key).is_none(),
    {
        match self.index.get(&key) {
            Some(offset) => Some(Arc::clone(&self.entries[*offset].1)),
            None => None,
        }
    }

    pub fn put(&mut self, key: i64, value: Arc<Value>)
        requires old(self).valid(),
        ensures final(self).valid(), final(self).at(key) == Some(*value),
            forall|other: i64| #![trigger final(self).at(other)]
                other != key ==> final(self).at(other) == old(self).at(other),
    {
        match self.index.get(&key) {
            Some(offset) => { let position = *offset; self.entries.set(position, (key, value)); }
            None => {
                let position = self.entries.len();
                self.entries.push((key, value));
                self.index.insert(key, position);
            }
        }
    }

    fn fill_missing(self, older: &Self) -> (result: Self)
        requires self.valid(), older.valid(),
        ensures result.valid(), forall|key: i64| #![trigger result.at(key)]
            result.at(key) == if self.at(key).is_some() { self.at(key) } else { older.at(key) },
    {
        let ghost before = self;
        let mut result = self;
        let mut offset = 0;
        while offset < older.entries.len()
            invariant offset <= older.entries@.len(), result.valid(), older.valid(),
                forall|key: i64| #![trigger result.at(key)] result.at(key) ==
                    if before.at(key).is_some() { before.at(key) }
                    else if older.index@.contains_key(key) && older.index@[key] < offset { older.at(key) }
                    else { None },
            decreases older.entries@.len() - offset,
        {
            let key = older.entries[offset].0;
            if result.get(key).is_none() { result.put(key, Arc::clone(&older.entries[offset].1)); }
            offset += 1;
        }
        result
    }

    fn compact(self) -> (result: Self)
        requires self.valid(),
        ensures result.valid(), forall|key: i64| #![trigger result.at(key)]
            result.at(key) == normalize(self.at(key)),
    {
        let mut result = Self::new();
        let mut offset = 0;
        while offset < self.entries.len()
            invariant offset <= self.entries@.len(), self.valid(), result.valid(),
                forall|key: i64| #![trigger result.at(key)] result.at(key) ==
                    if self.index@.contains_key(key) && self.index@[key] < offset { normalize(self.at(key)) }
                    else { None },
            decreases self.entries@.len() - offset,
        {
            if !self.entries[offset].1.is_empty() {
                result.put(self.entries[offset].0, Arc::clone(&self.entries[offset].1));
            }
            offset += 1;
        }
        result
    }
}

pub struct Layers<Value: Bucket> {
    base: Arc<Sparse<Value>>,
    tiers: Vec<Arc<Sparse<Value>>>,
    live: usize,
}

impl<Value: Bucket> Layers<Value> {
    pub closed spec fn base_model(self) -> Sparse<Value> { *self.base }
    pub closed spec fn patches(self) -> Seq<Arc<Sparse<Value>>> { self.tiers@ }
    pub closed spec fn live_count(self) -> nat { self.live as nat }
    pub closed spec fn prefix(self, length: nat, key: i64) -> Option<Value>
    {
        raw_prefix(*self.base, self.tiers@, length, key)
    }

    pub closed spec fn at(self, key: i64) -> Option<Value> {
        normalize(self.prefix(self.tiers@.len(), key))
    }

    pub closed spec fn prefix_keys(self, length: nat) -> Set<i64>
    {
        raw_keys(*self.base, self.tiers@, length)
    }

    pub closed spec fn support(self) -> Set<i64> {
        self.prefix_keys(self.tiers@.len()).filter(|key: i64| self.at(key).is_some())
    }

    pub proof fn support_facts(&self)
        ensures forall|key: i64| #![trigger self.support().contains(key)]
            self.support().contains(key) == self.at(key).is_some(),
    {
        assert forall|key: i64| #![trigger self.support().contains(key)]
            self.support().contains(key) == self.at(key).is_some() by {
            raw_present_key(*self.base, self.tiers@, self.tiers@.len(), key);
        }
    }

    pub proof fn empty_lookup(&self, key: i64)
        requires self.valid(), self.empty(),
        ensures self.at(key).is_none(),
    {
        self.support_facts();
        assert(self.support().len() == 0);
        assert(self.support() =~= Set::<i64>::empty());
        assert(!self.support().contains(key));
        assert(self.support().contains(key) == self.at(key).is_some());
    }

    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        self.live as nat == self.support().len()
        && self.support().finite()
        && self.base.valid()
        && forall|offset: int| #![trigger self.tiers@[offset]]
            0 <= offset < self.tiers@.len() ==> self.tiers@[offset].valid()
    }

    pub fn new(base: Sparse<Value>) -> (result: Self)
        requires base.valid(),
        ensures result.valid(), forall|key: i64| #![trigger result.at(key)] result.at(key) == normalize(base.at(key)),
    {
        let mut live = 0;
        let mut offset = 0;
        while offset < base.entries.len()
            invariant offset <= base.entries@.len(), live <= offset, base.valid(),
                live as int == sum(base.entries@.take(offset as int),
                    |entry: (i64, Arc<Value>)| if entry.1.empty() { 0 } else { 1 }),
            decreases base.entries@.len() - offset,
        {
            proof { lemma_sum_take(base.entries@, offset as int,
                |entry: (i64, Arc<Value>)| if entry.1.empty() { 0 } else { 1 }); }
            if !base.entries[offset].1.is_empty() { live += 1; }
            offset += 1;
        }
        let base = Arc::new(base);
        let tiers = Vec::new();
        proof {
            base.key_facts();
            let ghost source = *base;
            let ghost keys = source.view().map_values(|entry: (i64, Arc<Value>)| entry.0);
            let ghost keep = |key: i64| normalize(source.at(key)).is_some();
            lemma_distinct_selected_count(keys, keep);
            let ghost weight = |key: i64| if keep(key) { 1int } else { 0int };
            lemma_sum_map(source.view(), |entry: (i64, Arc<Value>)| entry.0, weight,
                |entry: (i64, Arc<Value>)| weight(entry.0));
            assert forall|position: int| #![trigger source.view()[position]]
                0 <= position < source.view().len() implies
                    weight(source.view()[position].0) == if source.view()[position].1.empty() { 0int } else { 1int } by {
                let key = source.view()[position].0;
                assert(source.index@[key] == position);
            }
            lemma_sum_ext(source.view(), |entry: (i64, Arc<Value>)| weight(entry.0),
                |entry: (i64, Arc<Value>)| if entry.1.empty() { 0int } else { 1int });
            let ghost candidate = Self { base, tiers, live };
            assert(candidate.support() =~= keys.to_set().filter(keep));
            assert(base.view().take(base.view().len() as int) == base.view());
            assert(candidate.valid());
        }
        Self { base, tiers, live }
    }

    proof fn same_prefix(&self, other: Self, length: nat)
        requires length <= self.tiers@.len(), self.tiers@ == other.tiers@,
            *self.base == *other.base,
        ensures self.prefix_keys(length) == other.prefix_keys(length),
            forall|key: i64| #![trigger other.prefix(length, key)]
                self.prefix(length, key) == other.prefix(length, key),
        decreases length,
    {
        assert(self.prefix_keys(length) == other.prefix_keys(length));
    }

    pub fn share(&self) -> (result: Self)
        ensures result.valid(), result.base_model() == self.base_model(), result.patches() == self.patches(),
            result.live_count() == self.live_count(),
            forall|key: i64| #![trigger result.at(key)] result.at(key) == self.at(key),
    {
        proof { use_type_invariant(self); }
        let mut tiers = Vec::new();
        let mut offset = 0;
        while offset < self.tiers.len()
            invariant offset <= self.tiers@.len(), tiers@ == self.tiers@.take(offset as int),
            decreases self.tiers@.len() - offset,
        {
            tiers.push(Arc::clone(&self.tiers[offset]));
            offset += 1;
        }
        let base = Arc::clone(&self.base);
        proof {
            assert(tiers@ =~= self.tiers@);
            let ghost candidate = Self { base, tiers, live: self.live };
            self.same_prefix(candidate, self.tiers@.len());
            assert(candidate.support() =~= self.support());
            assert(candidate.valid());
        }
        Self { base, tiers, live: self.live }
    }

    pub fn get(&self, key: i64) -> (result: Option<&Value>)
        ensures result matches Some(value) ==> self.at(key) == Some(*value),
                result.is_none() ==> self.at(key).is_none(),
    {
        proof { use_type_invariant(self); }
        let mut length = self.tiers.len();
        while length > 0
            invariant length <= self.tiers@.len(), self.valid(),
                self.prefix(self.tiers@.len(), key) == self.prefix(length as nat, key),
            decreases length,
        {
            length -= 1;
            if let Some(value) = self.tiers[length].get(key) {
                return if value.is_empty() { None } else { Some(value) };
            }
        }
        match self.base.get(key) {
            Some(value) => if value.is_empty() { None } else { Some(value) },
            None => None,
        }
    }

    pub fn patch(&self, changes: Sparse<Value>) -> (result: Option<Self>)
        requires changes.valid(),
        ensures result matches Some(updated) ==> updated.valid()
            && forall|key: i64| #![trigger updated.at(key)] updated.at(key) ==
                if changes.at(key).is_some() { normalize(changes.at(key)) } else { self.at(key) },
    {
        proof { use_type_invariant(self); }
        let ghost before_changes = changes;
        let ghost expected = |key: i64| if before_changes.at(key).is_some() {
            normalize(before_changes.at(key))
        } else { self.at(key) };
        let ghost expected_support = self.support().difference(changes.index@.dom())
            .union(changes.index@.dom().filter(|key: i64| normalize(changes.at(key)).is_some()));
        proof {
            self.support_facts();
            changes.key_facts();
            assert forall|key: i64| #![trigger expected_support.contains(key)]
                expected_support.contains(key) == expected(key).is_some() by {}
        }
        let shared = self.share();
        let Self { base, tiers, live } = shared;
        let mut base = base;
        let mut tiers = tiers;
        let mut live = live;
        if changes.entries.len() == 0 { return Some(Self { base, tiers, live }); }
        let mut offset = 0;
        while offset < changes.entries.len()
            invariant offset <= changes.entries@.len(), changes.valid(), self.valid(),
                *base == *self.base, tiers@ == self.tiers@,
                live as int == self.live as int + sum(changes.entries@.take(offset as int),
                    |entry: (i64, Arc<Value>)|
                        (if entry.1.empty() { 0int } else { 1int })
                        - (if self.at(entry.0).is_some() { 1int } else { 0int })),
            decreases changes.entries@.len() - offset,
        {
            proof { lemma_sum_take(changes.entries@, offset as int,
                |entry: (i64, Arc<Value>)|
                    (if entry.1.empty() { 0int } else { 1int })
                    - (if self.at(entry.0).is_some() { 1int } else { 0int })); }
            let key = changes.entries[offset].0;
            let present = !changes.entries[offset].1.is_empty();
            let was_present = self.get(key).is_some();
            if present && !was_present {
                live = match live.checked_add(1) { Some(count) => count, None => return None };
            }
            if !present && was_present {
                live = match live.checked_sub(1) { Some(count) => count, None => return None };
            }
            offset += 1;
        }
        proof {
            let ghost keys = changes.view().map_values(|entry: (i64, Arc<Value>)| entry.0);
            assert(keys.no_duplicates());
            let ghost present = |key: i64| normalize(changes.at(key)).is_some();
            lemma_replacement_support(self.support(), keys, present);
            let ghost weight = |key: i64|
                (if present(key) { 1int } else { 0int })
                - (if self.support().contains(key) { 1int } else { 0int });
            lemma_sum_map(changes.view(), |entry: (i64, Arc<Value>)| entry.0, weight,
                |entry: (i64, Arc<Value>)| weight(entry.0));
            assert forall|position: int| #![trigger changes.view()[position]]
                0 <= position < changes.view().len() implies
                    weight(changes.view()[position].0) ==
                        (if changes.view()[position].1.empty() { 0int } else { 1int })
                        - (if self.at(changes.view()[position].0).is_some() { 1int } else { 0int }) by {
                let key = changes.view()[position].0;
                assert(changes.index@[key] == position);
            }
            lemma_sum_ext(changes.view(), |entry: (i64, Arc<Value>)| weight(entry.0),
                |entry: (i64, Arc<Value>)|
                    (if entry.1.empty() { 0int } else { 1int })
                    - (if self.at(entry.0).is_some() { 1int } else { 0int }));
            assert(changes.view().take(changes.view().len() as int) == changes.view());
            assert(live as int == expected_support.len());
        }
        let mut merged = changes;
        let mut remaining = tiers.len();
        while remaining > 0
            invariant remaining <= self.tiers@.len(), tiers@ == self.tiers@.take(remaining as int),
                *base == *self.base, self.valid(), merged.valid(),
                live as int == expected_support.len(),
                forall|key: i64| #![trigger merged.at(key)]
                    normalize(if merged.at(key).is_some() { merged.at(key) }
                        else { self.prefix(remaining as nat, key) }) == expected(key),
            decreases remaining,
        {
            if tiers[remaining - 1].entries.len() > merged.entries.len() { break; }
            let previous = tiers.pop().unwrap();
            merged = merged.fill_missing(&*previous);
            remaining -= 1;
        }
        if remaining == 0 && merged.entries.len() >= base.entries.len() {
            merged = merged.fill_missing(&*base);
            proof {
                assert forall|key: i64| #![trigger merged.at(key)] normalize(merged.at(key)) == expected(key) by {}
            }
            base = Arc::new(merged.compact());
            proof {
                let ghost candidate = Self { base, tiers, live };
                assert forall|key: i64| #![trigger candidate.at(key)] candidate.at(key) == expected(key) by {
                    assert(candidate.tiers@.len() == 0);
                    assert(normalize(candidate.base.at(key)) == expected(key));
                }
            }
        } else {
            let ghost merged_value = merged;
            tiers.push(Arc::new(merged));
            proof {
                let ghost base_value = *base;
                assert(tiers@.take(remaining as int) =~= self.tiers@.take(remaining as int));
                raw_prefix_ext(base_value, tiers@, self.tiers@, remaining as nat);
                let ghost candidate = Self { base, tiers, live };
                assert forall|key: i64| #![trigger candidate.at(key)] candidate.at(key) == expected(key) by {
                    reveal_with_fuel(raw_prefix, 2);
                    assert(candidate.tiers@.len() == remaining + 1);
                    assert(candidate.tiers@[remaining as int].at(key) == merged_value.at(key));
                    assert(raw_prefix(base_value, candidate.tiers@, remaining as nat, key) == self.prefix(remaining as nat, key));
                }
            }
        }
        proof {
            let ghost candidate = Self { base, tiers, live };
            assert forall|key: i64| #![trigger candidate.at(key)] candidate.at(key) == expected(key) by {
                reveal_with_fuel(raw_prefix, 2);
            }
            candidate.support_facts();
            assert(candidate.support() =~= expected_support);
            assert(candidate.base.valid());
            assert forall|position: int| #![trigger candidate.tiers@[position]]
                0 <= position < candidate.tiers@.len() implies candidate.tiers@[position].valid() by {}
            assert(candidate.live as int == candidate.support().len());
            assert(candidate.valid());
        }
        Some(Self { base, tiers, live })
    }

    pub fn keys(&self) -> (result: Vec<i64>)
        ensures forall|key: i64| #![trigger self.at(key)]
            self.at(key).is_some() <==> result@.contains(key),
    {
        proof { use_type_invariant(self); self.support_facts(); }
        let mut seen: HashMap<i64, usize> = HashMap::new();
        let mut result = Vec::new();
        let mut layer = 0;
        while layer <= self.tiers.len()
            invariant layer <= self.tiers@.len(), self.valid(),
                seen@.dom() == if layer == 0 { Set::<i64>::empty() } else { self.prefix_keys((layer - 1) as nat) },
                forall|key: i64| #![trigger result@.contains(key)]
                    result@.contains(key) == (seen@.contains_key(key) && self.at(key).is_some()),
            decreases self.tiers@.len() + 1 - layer,
        {
            let entries = if layer == 0 { &self.base.entries } else { &self.tiers[layer - 1].entries };
            let ghost previous_seen = seen@.dom();
            let mut offset = 0;
            while offset < entries.len()
                invariant offset <= entries@.len(), layer <= self.tiers@.len(), self.valid(),
                    seen@.dom() == previous_seen.union(entries@.take(offset as int).map_values(|entry: (i64, Arc<Value>)| entry.0).to_set()),
                    forall|key: i64| #![trigger result@.contains(key)]
                        result@.contains(key) == (seen@.contains_key(key) && self.at(key).is_some()),
                decreases entries@.len() - offset,
            {
                let key = entries[offset].0;
                if !seen.contains_key(&key) {
                    seen.insert(key, result.len());
                    if self.get(key).is_some() { result.push(key); }
                }
                proof {
                    broadcast use vstd::seq_lib::group_seq_properties;
                    assert(entries@.take(offset as int + 1) =~= entries@.take(offset as int).push(entries@[offset as int]));
                    assert(entries@.take(offset as int + 1).map_values(|entry: (i64, Arc<Value>)| entry.0)
                        =~= entries@.take(offset as int).map_values(|entry: (i64, Arc<Value>)| entry.0).push(key));
                }
                offset += 1;
            }
            proof {
                if layer == 0 { self.base.key_facts(); } else { self.tiers[layer - 1].key_facts(); }
                assert(entries@.take(entries@.len() as int) =~= entries@);
            }
            if layer == self.tiers.len() {
                proof {
                    assert(seen@.dom() == self.prefix_keys(self.tiers@.len()));
                    assert forall|key: i64| #![trigger self.at(key)]
                        self.at(key).is_some() == result@.contains(key) by {
                        raw_present_key(*self.base, self.tiers@, self.tiers@.len(), key);
                    }
                }
                return result;
            }
            layer += 1;
        }
        result
    }
}

impl<Value: Bucket> Bucket for Layers<Value> {
    closed spec fn empty(self) -> bool { self.live == 0 }
    fn is_empty(&self) -> (result: bool)
        ensures result == self.empty(),
    { self.live == 0 }
}

}
