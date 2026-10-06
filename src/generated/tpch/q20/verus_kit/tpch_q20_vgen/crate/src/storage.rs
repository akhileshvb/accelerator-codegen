use vstd::prelude::*;
use std::sync::Arc;
use std::collections::HashMap;
use verus_prooflib::{unique_key_lookup, lemma_unique_key_lookup, lemma_unique_push, lemma_push_membership, lemma_push_to_set};

verus! {

pub struct Leaf<Value> {
    pub base: Arc<HashMap<i64, Arc<Value>>>,
    pub keys: Arc<Vec<i64>>,
    pub overlay: Vec<(i64, Option<Arc<Value>>)>,
}
pub struct Page<Value> { pub leaves: Vec<Option<Arc<Leaf<Value>>>> }
pub struct Directory<Value> { pub pages: Vec<Option<Arc<Page<Value>>>> }
pub struct Table<Value> { roots: Vec<Option<Arc<Directory<Value>>>> }

pub open spec fn slot(key: i64) -> int { (key as u64) as int % 262144 }

pub open spec fn cell_at<Value>(roots: Seq<Option<Arc<Directory<Value>>>>, position: int) -> Option<Leaf<Value>> {
    match roots[position / 4096] {
        None => None,
        Some(directory) => match directory.pages@[position / 64 % 64] {
            None => None,
            Some(page) => match page.leaves@[position % 64] {
                None => None, Some(leaf) => Some(*leaf),
            },
        },
    }
}
pub open spec fn shape_valid<Value>(roots: Seq<Option<Arc<Directory<Value>>>>) -> bool {
    &&& roots.len() == 64
    &&& forall|root: int| #![trigger roots[root]] 0 <= root < 64 ==> match roots[root] {
        None => true,
        Some(directory) => directory.pages@.len() == 64
            && forall|page: int| #![trigger directory.pages@[page]] 0 <= page < 64 ==> match directory.pages@[page] {
                None => true, Some(data) => data.leaves@.len() == 64,
            },
    }
}

impl<Value> Leaf<Value> {
    pub open spec fn content_valid(self) -> bool {
        &&& (*self.keys)@.no_duplicates()
        &&& (*self.base)@.dom() == (*self.keys)@.to_set()
        &&& self.overlay@.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0).no_duplicates()
    }
    pub open spec fn lookup(self, key: i64) -> Option<Value> {
        match unique_key_lookup(self.overlay@, key) {
            Some(value) => match value { Some(item) => Some(*item), None => None },
            None => if (*self.base)@.contains_key(key) { Some(*(*self.base)@[key]) } else { None },
        }
    }
    pub fn get(&self, key: i64) -> (result: Option<Arc<Value>>)
        requires self.content_valid(),
        ensures result matches Some(value) ==> self.lookup(key) == Some(*value),
            result.is_none() ==> self.lookup(key).is_none(),
    {
        proof { lemma_unique_key_lookup(self.overlay@, key); }
        let mut index = 0;
        while index < self.overlay.len()
            invariant index <= self.overlay.len(), self.content_valid(),
                forall|offset: int| #![trigger self.overlay@[offset]] 0 <= offset < index ==> self.overlay@[offset].0 != key,
            decreases self.overlay.len() - index,
        {
            if self.overlay[index].0 == key {
                proof { lemma_unique_key_lookup(self.overlay@, key); }
                return match &self.overlay[index].1 { Some(value) => Some(Arc::clone(value)), None => None };
            }
            index += 1;
        }
        proof {
            assert(!self.overlay@.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0).contains(key)) by {
                let keys = self.overlay@.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0);
                if keys.contains(key) {
                    let offset = choose|offset: int| #![trigger keys[offset]] 0 <= offset < keys.len() && keys[offset] == key;
                    assert(self.overlay@[offset].0 != key);
                }
            }
        }
        match self.base.get(&key) { Some(value) => Some(Arc::clone(value)), None => None }
    }
    /// Like `get`, but borrows the value instead of cloning its `Arc` (no atomic traffic).
    pub fn get_ref(&self, key: i64) -> (result: Option<&Value>)
        requires self.content_valid(),
        ensures result matches Some(value) ==> self.lookup(key) == Some(*value),
            result.is_none() ==> self.lookup(key).is_none(),
    {
        proof { lemma_unique_key_lookup(self.overlay@, key); }
        let mut index = 0;
        while index < self.overlay.len()
            invariant index <= self.overlay.len(), self.content_valid(),
                forall|offset: int| #![trigger self.overlay@[offset]] 0 <= offset < index ==> self.overlay@[offset].0 != key,
            decreases self.overlay.len() - index,
        {
            if self.overlay[index].0 == key {
                proof { lemma_unique_key_lookup(self.overlay@, key); }
                return match &self.overlay[index].1 { Some(value) => Some(&**value), None => None };
            }
            index += 1;
        }
        proof {
            assert(!self.overlay@.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0).contains(key)) by {
                let keys = self.overlay@.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0);
                if keys.contains(key) {
                    let offset = choose|offset: int| #![trigger keys[offset]] 0 <= offset < keys.len() && keys[offset] == key;
                    assert(self.overlay@[offset].0 != key);
                }
            }
        }
        match self.base.get(&key) { Some(value) => Some(&**value), None => None }
    }
    pub fn live_keys(&self) -> (result: Vec<i64>)
        requires self.content_valid(),
        ensures result@.no_duplicates(),
            forall|key: i64| #![trigger self.lookup(key)] result@.contains(key) <==> self.lookup(key).is_some(),
    {
        let mut result = Vec::new();
        let mut index = 0;
        while index < self.keys.len()
            invariant index <= self.keys.len(), result@.no_duplicates(), self.content_valid(),
                forall|key: i64| #![trigger self.lookup(key)] result@.contains(key) <==>
                    (*self.keys)@.take(index as int).contains(key) && self.lookup(key).is_some(),
            decreases self.keys.len() - index,
        {
            let key = self.keys[index];
            if self.get(key).is_some() {
                proof {
                    verus_prooflib::lemma_prefix_fresh((*self.keys)@, index as int);
                    lemma_unique_push(result@, key);
                    lemma_push_membership(result@, key);
                }
                result.push(key);
            }
            proof {
                assert((*self.keys)@.take(index as int + 1) =~= (*self.keys)@.take(index as int).push(key));
                lemma_push_membership((*self.keys)@.take(index as int), key);
            }
            index += 1;
        }
        proof {
            (*self.keys)@.to_set_ensures();
            assert((*self.keys)@.take(self.keys.len() as int) =~= (*self.keys)@);
            assert forall|key: i64| #![trigger self.lookup(key)] result@.contains(key) <==>
                (*self.base)@.contains_key(key) && self.lookup(key).is_some() by {
                assert((*self.keys)@.contains(key) == (*self.base)@.contains_key(key));
            }
        }
        let mut index = 0;
        while index < self.overlay.len()
            invariant index <= self.overlay.len(), result@.no_duplicates(), self.content_valid(),
                forall|key: i64| #![trigger self.lookup(key)] result@.contains(key) <==>
                    ((*self.base)@.contains_key(key) || self.overlay@.take(index as int).map_values(|entry: (i64, Option<Arc<Value>>)| entry.0).contains(key)) && self.lookup(key).is_some(),
            decreases self.overlay.len() - index,
        {
            let key = self.overlay[index].0;
            proof {
                lemma_unique_key_lookup(self.overlay@, key);
                assert(unique_key_lookup(self.overlay@, key) == Some(self.overlay@[index as int].1));
            }
            if !self.base.contains_key(&key) && self.overlay[index].1.is_some() {
                proof {
                    let overlay_keys = self.overlay@.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0);
                    verus_prooflib::lemma_prefix_fresh(overlay_keys, index as int);
                    assert(self.overlay@.take(index as int).map_values(|entry: (i64, Option<Arc<Value>>)| entry.0) =~= overlay_keys.take(index as int));
                    assert(self.lookup(key).is_some());
                    assert(!result@.contains(key));
                    lemma_unique_push(result@, key);
                    lemma_push_membership(result@, key);
                }
                result.push(key);
            }
            proof {
                let before = self.overlay@.take(index as int).map_values(|entry: (i64, Option<Arc<Value>>)| entry.0);
                assert(self.overlay@.take(index as int + 1).map_values(|entry: (i64, Option<Arc<Value>>)| entry.0) =~= before.push(key));
                lemma_push_membership(before, key);
            }
            index += 1;
        }
        proof {
            assert(self.overlay@.take(self.overlay.len() as int) =~= self.overlay@);
            assert forall|key: i64| #![trigger self.lookup(key)] result@.contains(key) <==> self.lookup(key).is_some() by {
                lemma_unique_key_lookup(self.overlay@, key);
            }
        }
        result
    }
    pub fn compact(&self) -> (result: Self)
        requires self.content_valid(),
        ensures result.content_valid(), result.overlay@.len() == 0,
            forall|key: i64| #[trigger] result.lookup(key) == self.lookup(key),
    {
        let keys = self.live_keys();
        let mut base: HashMap<i64, Arc<Value>> = HashMap::new();
        let mut index = 0;
        while index < keys.len()
            invariant index <= keys.len(), self.content_valid(), keys@.no_duplicates(),
                forall|key: i64| #![trigger self.lookup(key)] keys@.contains(key) <==> self.lookup(key).is_some(),
                base@.dom() == keys@.take(index as int).to_set(),
                forall|key: i64| #![trigger base@[key]] base@.contains_key(key) ==> Some(*base@[key]) == self.lookup(key),
            decreases keys.len() - index,
        {
            let key = keys[index];
            if let Some(item) = self.get(key) {
                proof { lemma_push_to_set(keys@.take(index as int), key); }
                base.insert(key, item);
            }
            proof { assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key)); }
            index += 1;
        }
        let overlay: Vec<(i64, Option<Arc<Value>>)> = Vec::new();
        proof {
            assert(overlay@.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0) =~= Seq::<i64>::empty());
            assert(keys@.take(keys.len() as int) =~= keys@);
            keys@.to_set_ensures();
        }
        Self { base: Arc::new(base), keys: Arc::new(keys), overlay }
    }
    pub fn replace(&self, key: i64, value: Option<Arc<Value>>) -> (result: Self)
        requires self.content_valid(), self.overlay@.len() <= 32,
        ensures result.content_valid(), result.overlay@.len() <= 32,
            result.lookup(key) == match value { Some(item) => Some(*item), None => None },
            forall|other: i64| #![trigger result.lookup(other)] other != key ==> result.lookup(other) == self.lookup(other),
    {
        let mut overlay: Vec<(i64, Option<Arc<Value>>)> = Vec::new();
        let mut index = 0;
        while index < self.overlay.len()
            invariant index <= self.overlay.len(), self.content_valid(), self.overlay@.len() <= 32,
                overlay@ == self.overlay@.take(index as int),
            decreases self.overlay.len() - index,
        {
            let copied = match &self.overlay[index].1 { Some(item) => Some(Arc::clone(item)), None => None };
            overlay.push((self.overlay[index].0, copied));
            index += 1;
        }
        let ghost before = overlay@;
        let mut index = 0;
        while index < overlay.len() && overlay[index].0 != key
            invariant index <= overlay.len(), overlay@ == before, before == self.overlay@,
                self.content_valid(), self.overlay@.len() <= 32,
                forall|offset: int| #![trigger before[offset]] 0 <= offset < index ==> before[offset].0 != key,
            decreases overlay.len() - index,
        {
            index += 1;
        }
        let ghost position = index as int;
        if index < overlay.len() { overlay.set(index, (key, value)); }
        else { overlay.push((key, value)); }
        proof {
            let keys = overlay@.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0);
            let old_keys = before.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0);
            if position < before.len() {
                assert(keys =~= old_keys);
            } else {
                assert(!old_keys.contains(key)) by {
                    if old_keys.contains(key) {
                        let offset = choose|offset: int| #![trigger old_keys[offset]] 0 <= offset < old_keys.len() && old_keys[offset] == key;
                        assert(before[offset].0 == key);
                    }
                }
                lemma_unique_push(old_keys, key);
                assert(keys =~= old_keys.push(key));
            }
            lemma_unique_key_lookup(overlay@, key);
            assert(overlay@[position] == (key, value));
            assert(unique_key_lookup(overlay@, key) == Some(value));
            assert forall|other: i64| #![trigger unique_key_lookup(overlay@, other)] other != key implies
                unique_key_lookup(overlay@, other) == unique_key_lookup(before, other) by {
                lemma_unique_key_lookup(overlay@, other);
                lemma_unique_key_lookup(before, other);
                if unique_key_lookup(before, other).is_some() {
                    let offset = choose|offset: int| #![trigger before[offset]] 0 <= offset < before.len() && before[offset].0 == other;
                    assert(0 <= offset < before.len() && before[offset].0 == other);
                    assert(offset != position);
                    assert(overlay@[offset] == before[offset]);
                } else if unique_key_lookup(overlay@, other).is_some() {
                    let offset = choose|offset: int| #![trigger overlay@[offset]] 0 <= offset < overlay@.len() && overlay@[offset].0 == other;
                    assert(0 <= offset < overlay@.len() && overlay@[offset].0 == other);
                    assert(offset != position);
                    assert(before[offset] == overlay@[offset]);
                    assert(false);
                }
            }
        }
        let next = Self { base: Arc::clone(&self.base), keys: Arc::clone(&self.keys), overlay };
        if next.overlay.len() <= 32 { next } else { next.compact() }
    }
}

impl<Value> Table<Value> {
    pub proof fn reveal_well_formed(&self)
        requires self.valid(),
        ensures self.well_formed(),
    {}
    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool { self.well_formed() }
    pub closed spec fn lookup(self, key: i64) -> Option<Value> {
        match cell_at(self.roots@, slot(key)) { None => None, Some(leaf) => leaf.lookup(key) }
    }
    pub closed spec fn well_formed(self) -> bool {
        &&& shape_valid(self.roots@)
        &&& forall|position: int| #![trigger cell_at(self.roots@, position)]
            0 <= position < 262144 ==> match cell_at(self.roots@, position) {
                None => true,
                Some(leaf) => leaf.overlay@.len() <= 32 && leaf.content_valid()
                    && forall|key: i64| #![trigger leaf.lookup(key)] leaf.lookup(key).is_some() ==> slot(key) == position,
            }
    }
    pub fn new() -> (result: Self)
        ensures result.well_formed(), forall|key: i64| #[trigger] result.lookup(key) == None,
    {
        Self { roots: empty_slots() }
    }
    pub fn share(&self) -> (result: Self)
        ensures result.well_formed(),
            forall|key: i64| #[trigger] result.lookup(key) == self.lookup(key),
    {
        proof { use_type_invariant(self); }
        Self { roots: copy_slots(&self.roots) }
    }
    pub fn get(&self, key: i64) -> (result: Option<Arc<Value>>)
        ensures result matches Some(value) ==> self.lookup(key) == Some(*value),
            result.is_none() ==> self.lookup(key).is_none(),
    {
        proof { use_type_invariant(self); }
        let position = (key as u64 % 262144) as usize;
        proof { assert(cell_at(self.roots@, position as int).is_some() ==> cell_at(self.roots@, position as int).unwrap().content_valid()); }
        match &self.roots[position / 4096] {
            None => None,
            Some(directory) => match &directory.pages[position / 64 % 64] {
                None => None,
                Some(page) => match &page.leaves[position % 64] {
                    None => None,
                    Some(leaf) => leaf.get(key),
                },
            },
        }
    }
    pub fn get_ref(&self, key: i64) -> (result: Option<&Value>)
        ensures result matches Some(value) ==> self.lookup(key) == Some(*value),
            result.is_none() ==> self.lookup(key).is_none(),
    {
        proof { use_type_invariant(self); }
        let position = (key as u64 % 262144) as usize;
        proof { assert(cell_at(self.roots@, position as int).is_some() ==> cell_at(self.roots@, position as int).unwrap().content_valid()); }
        match &self.roots[position / 4096] {
            None => None,
            Some(directory) => match &directory.pages[position / 64 % 64] {
                None => None,
                Some(page) => match &page.leaves[position % 64] {
                    None => None,
                    Some(leaf) => leaf.get_ref(key),
                },
            },
        }
    }
    pub fn put(&mut self, key: i64, value: Option<Arc<Value>>)
        ensures final(self).well_formed(),
            final(self).lookup(key) == match value { Some(data) => Some(*data), None => None },
            forall|other: i64| #![trigger final(self).lookup(other)]
                other != key ==> final(self).lookup(other) == old(self).lookup(other),
    {
        proof { use_type_invariant(&*self); }
        let ghost previous = self.roots@;
        let position = (key as u64 % 262144) as usize;
        let root_index = position / 4096;
        let page_index = position / 64 % 64;
        let leaf_index = position % 64;
        let mut directory = match &self.roots[root_index] {
            Some(data) => Directory { pages: copy_slots(&data.pages) },
            None => Directory { pages: empty_slots() },
        };
        let mut page = match &directory.pages[page_index] {
            Some(data) => Page { leaves: copy_slots(&data.leaves) },
            None => Page { leaves: empty_slots() },
        };
        let leaf = match &page.leaves[leaf_index] {
            Some(data) => data.replace(key, value),
            None => {
                let mut overlay: Vec<(i64, Option<Arc<Value>>)> = Vec::new(); overlay.push((key, value));
                let base: HashMap<i64, Arc<Value>> = HashMap::new();
                let keys: Vec<i64> = Vec::new();
                proof {
                    assert(overlay@ == seq![(key, value)]);
                    assert(overlay@[0] == (key, value));
                    assert(overlay@.map_values(|entry: (i64, Option<Arc<Value>>)| entry.0) =~= seq![key]);
                    assert(base@.dom() =~= keys@.to_set());
                    lemma_unique_key_lookup(overlay@, key);
                    assert(unique_key_lookup(overlay@, key) == Some(value));
                }
                Leaf { base: Arc::new(base), keys: Arc::new(keys), overlay }
            },
        };
        let ghost replacement = leaf;
        proof {
            assert(leaf.content_valid());
            assert(leaf.overlay@.len() <= 32);
            assert(leaf.lookup(key) == match value { Some(item) => Some(*item), None => None });
            assert forall|other: i64| #![trigger leaf.lookup(other)] other != key implies
                leaf.lookup(other) == match cell_at(previous, position as int) { Some(data) => data.lookup(other), None => None } by {
                if cell_at(previous, position as int).is_none() {
                    lemma_unique_key_lookup(leaf.overlay@, other);
                }
            }
        }
        page.leaves.set(leaf_index, Some(Arc::new(leaf)));
        directory.pages.set(page_index, Some(Arc::new(page)));
        let mut roots = copy_slots(&self.roots);
        roots.set(root_index, Some(Arc::new(directory)));
        proof {
            assert(shape_valid(roots@));
            assert forall|offset: int| #![trigger cell_at(roots@, offset)] 0 <= offset < 262144 implies
                cell_at(roots@, offset) == if offset == position { Some(replacement) } else { cell_at(previous, offset) } by {
                if offset / 4096 == root_index && offset / 64 % 64 == page_index && offset % 64 == leaf_index {
                    assert(offset == position);
                }
            }
            assert forall|offset: int| #![trigger cell_at(roots@, offset)] 0 <= offset < 262144 implies
                match cell_at(roots@, offset) {
                    None => true,
                    Some(data) => data.overlay@.len() <= 32 && data.content_valid()
                        && forall|other: i64| #![trigger data.lookup(other)] data.lookup(other).is_some() ==> slot(other) == offset,
                } by {
                if offset == position {
                    assert forall|other: i64| #![trigger replacement.lookup(other)] replacement.lookup(other).is_some() implies slot(other) == offset by {
                        if other != key {
                            if let Some(data) = cell_at(previous, offset) {
                                assert(data.lookup(other).is_some());
                            }
                        }
                    }
                }
            }
        }
        self.roots = roots;
    }
    pub fn keys(&self) -> (result: Vec<i64>)
        ensures result@.no_duplicates(),
            forall|key: i64| #![trigger self.lookup(key)]
                result@.contains(key) <==> self.lookup(key).is_some(),
    {
        proof { use_type_invariant(self); }
        let mut result = Vec::new();
        let mut position: usize = 0;
        while position < 262144
            invariant position <= 262144, self.well_formed(), result@.no_duplicates(),
                forall|key: i64| #![trigger self.lookup(key)] result@.contains(key) <==>
                    slot(key) < position && self.lookup(key).is_some(),
            decreases 262144 - position,
        {
            let directory = match &self.roots[position / 4096] {
                Some(value) => value,
                None => {
                    let next = (position / 4096 + 1) * 4096;
                    proof {
                        assert forall|key: i64| #![trigger self.lookup(key)] position <= slot(key) < next implies self.lookup(key).is_none() by {
                            assert(slot(key) / 4096 == position / 4096);
                        }
                    }
                    position = next;
                    continue;
                },
            };
            let page = match &directory.pages[position / 64 % 64] {
                Some(value) => value,
                None => {
                    let next = (position / 64 + 1) * 64;
                    proof {
                        assert forall|key: i64| #![trigger self.lookup(key)] position <= slot(key) < next implies self.lookup(key).is_none() by {
                            assert(slot(key) / 4096 == position / 4096);
                            assert(slot(key) / 64 % 64 == position / 64 % 64);
                        }
                    }
                    position = next;
                    continue;
                },
            };
            if let Some(leaf) = &page.leaves[position % 64] {
                proof { assert(cell_at(self.roots@, position as int) == Some(**leaf)); }
                let keys = leaf.live_keys();
                proof {
                    assert forall|key: i64| #![trigger self.lookup(key)] keys@.contains(key) <==>
                        slot(key) == position && self.lookup(key).is_some() by {
                        if keys@.contains(key) { assert(leaf.lookup(key).is_some()); }
                        if slot(key) == position { assert(self.lookup(key) == leaf.lookup(key)); }
                    }
                }
                let ghost before = result@;
                let mut index = 0;
                while index < keys.len()
                    invariant index <= keys.len(), position < 262144, self.well_formed(), result@.no_duplicates(), keys@.no_duplicates(),
                        forall|key: i64| #![trigger keys@.contains(key)] keys@.contains(key) <==> slot(key) == position && self.lookup(key).is_some(),
                        forall|key: i64| #![trigger before.contains(key)] before.contains(key) <==> slot(key) < position && self.lookup(key).is_some(),
                        forall|key: i64| #![trigger result@.contains(key)] result@.contains(key) <==> before.contains(key) || keys@.take(index as int).contains(key),
                    decreases keys.len() - index,
                {
                    let key = keys[index];
                    let ghost accumulated = result@;
                    proof {
                        assert(keys@.contains(key));
                        assert(keys@.contains(key) <==> slot(key) == position && self.lookup(key).is_some());
                        assert(slot(key) == position);
                        assert(!before.contains(key));
                        verus_prooflib::lemma_prefix_fresh(keys@, index as int);
                        lemma_unique_push(result@, key);
                        lemma_push_membership(result@, key);
                        assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
                        lemma_push_membership(keys@.take(index as int), key);
                    }
                    result.push(key);
                    proof {
                        assert forall|other: i64| #![trigger self.lookup(other)] result@.contains(other) <==>
                            before.contains(other) || keys@.take(index as int + 1).contains(other) by {
                            assert(accumulated.contains(other) <==> before.contains(other) || keys@.take(index as int).contains(other));
                            assert(result@ == accumulated.push(key));
                            lemma_push_membership(accumulated, key);
                            lemma_push_membership(keys@.take(index as int), key);
                            assert(keys@.take(index as int + 1) == keys@.take(index as int).push(key));
                        }
                    }
                    index += 1;
                }
                proof { assert(keys@.take(keys.len() as int) =~= keys@); }
            }
            position += 1;
        }
        result
    }
}

pub fn empty_slots<Value>() -> (result: Vec<Option<Arc<Value>>>)
    ensures result@.len() == 64,
        forall|index: int| #![trigger result@[index]] 0 <= index < 64 ==> result@[index].is_none(),
{
    let mut result: Vec<Option<Arc<Value>>> = Vec::new();
    let mut index = 0;
    while index < 64
        invariant index <= 64, result@.len() == index,
            forall|offset: int| #![trigger result@[offset]] 0 <= offset < index ==> result@[offset].is_none(),
        decreases 64 - index,
    { result.push(None); index += 1; }
    result
}
pub fn copy_slots<Value>(source: &Vec<Option<Arc<Value>>>) -> (result: Vec<Option<Arc<Value>>>)
    ensures result@ == source@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    {
        result.push(match &source[index] { Some(value) => Some(Arc::clone(value)), None => None });
        index += 1;
    }
    result
}
pub fn copy_i64(source: &Vec<i64>) -> (result: Vec<i64>)
    ensures result@ == source@,
{
    let mut result = Vec::new(); let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    { result.push(source[index]); index += 1; }
    result
}
}
