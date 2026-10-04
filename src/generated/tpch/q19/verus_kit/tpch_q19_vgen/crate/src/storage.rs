use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_prooflib::*;
use verus_prooflib::{
    bucket_overlay_value as edit_value,
    bucket_overlay_keys as edit_keys,
    lemma_bucket_overlay_push as lemma_edit_push,
    lemma_bucket_overlay_missing as lemma_edit_missing,
};

verus! {

broadcast use { vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties };

pub struct Base<T> {
    pub map: HashMap<u64, Arc<Vec<T>>>,
    pub keys: Vec<u64>,
}

pub struct Page<T> {
    pub base: Arc<Base<T>>,
    pub edits: Vec<(u64, Arc<Vec<T>>)>,
}

pub struct Store<T> {
    pub pages: Vec<Arc<Page<T>>>,
}

pub struct Work<T> {
    pub map: HashMap<u64, Vec<T>>,
    pub keys: Vec<u64>,
}

impl<T> Page<T> {
    pub open spec fn lookup(self, key: u64) -> Seq<T> {
        match edit_value(self.edits@, key) {
            Some(rows) => rows,
            None => if self.base.map@.dom().contains(key) { (*self.base.map@[key])@ } else { Seq::empty() },
        }
    }

    pub open spec fn valid(self) -> bool {
        &&& self.edits.len() <= 64
        &&& self.base.keys@.to_set() == self.base.map@.dom()
    }

    pub fn get(&self, key: u64) -> (result: Option<Arc<Vec<T>>>)
        ensures match result { Some(rows) => (*rows)@ == self.lookup(key), None => self.lookup(key).len() == 0 },
    {
        proof { assert(self.edits@.take(self.edits.len() as int) =~= self.edits@); }
        let mut index = self.edits.len();
        while index > 0
            invariant index <= self.edits.len(),
                edit_value(self.edits@, key) == edit_value(self.edits@.take(index as int), key),
            decreases index,
        {
            index -= 1;
            proof {
                assert(self.edits@.take(index as int + 1).drop_last() =~= self.edits@.take(index as int));
                assert(self.edits@.take(index as int + 1).last() == self.edits@[index as int]);
            }
            if self.edits[index].0 == key { return Some(Arc::clone(&self.edits[index].1)); }
        }
        match self.base.map.get(&key) {
            Some(rows) => Some(Arc::clone(rows)),
            None => None,
        }
    }

    pub fn peek(&self, key: u64) -> (result: Option<&Vec<T>>)
        ensures match result { Some(rows) => (*rows)@ == self.lookup(key), None => self.lookup(key).len() == 0 },
    {
        proof { assert(self.edits@.take(self.edits.len() as int) =~= self.edits@); }
        let mut index = self.edits.len();
        while index > 0
            invariant index <= self.edits.len(),
                edit_value(self.edits@, key) == edit_value(self.edits@.take(index as int), key),
            decreases index,
        {
            index -= 1;
            proof {
                assert(self.edits@.take(index as int + 1).drop_last() =~= self.edits@.take(index as int));
                assert(self.edits@.take(index as int + 1).last() == self.edits@[index as int]);
            }
            if self.edits[index].0 == key { return Some(&self.edits[index].1); }
        }
        match self.base.map.get(&key) {
            Some(rows) => Some(rows),
            None => None,
        }
    }

    pub fn replace(&self, key: u64, rows: Arc<Vec<T>>) -> (result: Page<T>)
        requires self.valid(),
        ensures result.valid(), result.lookup(key) == (*rows)@,
            forall|other: u64| #![trigger result.lookup(other)]
                other != key ==> result.lookup(other) == self.lookup(other),
    {
        let mut edits = Vec::new();
        let mut index = 0usize;
        while index < self.edits.len()
            invariant index <= self.edits.len(), edits.len() <= index, self.valid(),
                edit_value(edits@, key) == None,
                forall|other: u64| #![trigger edit_value(edits@, other)] other != key ==>
                    edit_value(edits@, other) == edit_value(self.edits@.take(index as int), other),
            decreases self.edits.len() - index,
        {
            let ghost previous = edits@;
            if self.edits[index].0 != key {
                edits.push((self.edits[index].0, Arc::clone(&self.edits[index].1)));
            }
            proof {
                assert(self.edits@.take(index as int + 1).drop_last() =~= self.edits@.take(index as int));
                assert(self.edits@.take(index as int + 1).last() == self.edits@[index as int]);
                if self.edits@[index as int].0 != key { lemma_edit_push(previous, edits@.last(), key); }
                assert forall|other: u64| #![trigger edit_value(edits@, other)] other != key ==>
                    edit_value(edits@, other) == edit_value(self.edits@.take(index as int + 1), other) by {
                    if self.edits@[index as int].0 != key { lemma_edit_push(previous, edits@.last(), other); }
                }
            }
            index += 1;
        }
        let ghost previous = edits@;
        edits.push((key, rows));
        proof {
            lemma_edit_push(previous, edits@.last(), key);
            assert forall|other: u64| #![trigger edit_value(edits@, other)] other != key ==>
                edit_value(edits@, other) == edit_value(self.edits@, other) by {
                lemma_edit_push(previous, edits@.last(), other);
            }
        }
        let next = Page { base: Arc::clone(&self.base), edits };
        proof {
            assert forall|other: u64| #![trigger next.lookup(other)] next.lookup(other)
                == if other == key { (*rows)@ } else { self.lookup(other) } by {}
        }
        if next.edits.len() < 64 { return next; }
        let mut map: HashMap<u64, Arc<Vec<T>>> = HashMap::new();
        let mut keys = Vec::new();
        let mut index = 0usize;
        while index < next.base.keys.len()
            invariant index <= next.base.keys.len(), keys@.to_set() == map@.dom(),
                next.base.keys@.to_set() == next.base.map@.dom(),
                forall|other: u64| #![trigger next.lookup(other)] next.lookup(other)
                    == if other == key { (*rows)@ } else { self.lookup(other) },
                map@.dom() == next.base.keys@.take(index as int).to_set().filter(|other: u64| next.lookup(other).len() > 0),
                forall|other: u64| #![trigger map@.dom().contains(other)]
                    map@.dom().contains(other) ==> (*map@[other])@ == next.lookup(other),
            decreases next.base.keys.len() - index,
        {
            let old_key = next.base.keys[index];
            match next.get(old_key) {
                Some(value) => {
                    if value.len() > 0 {
                        proof { lemma_push_to_set(keys@, old_key); }
                        map.insert(old_key, value); keys.push(old_key);
                    }
                },
                None => {},
            }
            proof { lemma_push_to_set(next.base.keys@.take(index as int), old_key); }
            index += 1;
        }
        let mut index = 0usize;
        while index < next.edits.len()
            invariant index <= next.edits.len(), keys@.to_set() == map@.dom(),
                next.base.keys@.to_set() == next.base.map@.dom(),
                forall|other: u64| #![trigger next.lookup(other)] next.lookup(other)
                    == if other == key { (*rows)@ } else { self.lookup(other) },
                map@.dom() == next.base.keys@.to_set().union(edit_keys(next.edits@.take(index as int)).to_set())
                    .filter(|other: u64| next.lookup(other).len() > 0),
                forall|other: u64| #![trigger map@.dom().contains(other)]
                    map@.dom().contains(other) ==> (*map@[other])@ == next.lookup(other),
            decreases next.edits.len() - index,
        {
            let new_key = next.edits[index].0;
            if !next.base.map.contains_key(&new_key) {
                match next.get(new_key) {
                    Some(value) => {
                        if value.len() > 0 {
                            proof { lemma_push_to_set(keys@, new_key); }
                            map.insert(new_key, value);
                            keys.push(new_key);
                        }
                    },
                    None => {},
                }
            }
            proof {
                assert(edit_keys(next.edits@.take(index as int + 1)) =~= edit_keys(next.edits@.take(index as int)).push(new_key));
                lemma_push_to_set(edit_keys(next.edits@.take(index as int)), new_key);
            }
            index += 1;
        }
        proof {
            assert forall|other: u64| #![trigger next.lookup(other)] !map@.dom().contains(other) ==> next.lookup(other).len() == 0 by {
                if !next.base.map@.dom().contains(other) && !edit_keys(next.edits@).contains(other) {
                    lemma_edit_missing(next.edits@, other);
                }
            }
            if !map@.dom().contains(key) { assert(next.lookup(key) =~= Seq::empty()); }
            assert((if map@.dom().contains(key) { (*map@[key])@ } else { Seq::empty() }) =~= (*rows)@);
        }
        let result = Page { base: Arc::new(Base { map, keys }), edits: Vec::new() };
        proof {
            assert forall|other: u64| #[trigger] result.lookup(other) == next.lookup(other) by {
                if !result.base.map@.dom().contains(other) { assert(next.lookup(other) =~= Seq::empty()); }
            }
        }
        result
    }
}

impl<T> Store<T> {
    pub open spec fn lookup(self, key: u64) -> Seq<T> {
        (*self.pages@[(key % 256) as int]).lookup(key)
    }

    pub open spec fn valid(self) -> bool {
        &&& self.pages.len() == 256
        &&& forall|index: int| #![trigger self.pages@[index]]
            0 <= index < 256 ==> (*self.pages@[index]).valid()
    }

    pub fn get(&self, key: u64) -> (result: Option<Arc<Vec<T>>>)
        requires self.valid(),
        ensures match result { Some(rows) => (*rows)@ == self.lookup(key), None => self.lookup(key).len() == 0 },
    {
        self.pages[(key % 256) as usize].get(key)
    }

    pub fn peek(&self, key: u64) -> (result: Option<&Vec<T>>)
        requires self.valid(),
        ensures match result { Some(rows) => (*rows)@ == self.lookup(key), None => self.lookup(key).len() == 0 },
    {
        self.pages[(key % 256) as usize].peek(key)
    }

    pub fn share(&self) -> (result: Store<T>)
        requires self.valid(),
        ensures result.valid(), forall|key: u64| #[trigger] result.lookup(key) == self.lookup(key),
    {
        let mut pages = Vec::new();
        let mut index = 0usize;
        while index < self.pages.len()
            invariant index <= self.pages.len(), pages.len() == index,
                pages@ == self.pages@.take(index as int),
            decreases self.pages.len() - index,
        {
            pages.push(Arc::clone(&self.pages[index]));
            index += 1;
        }
        proof { assert(pages@ =~= self.pages@); }
        Store { pages }
    }

    pub fn replace(&mut self, key: u64, rows: Vec<T>)
        requires old(self).valid(),
        ensures final(self).valid(), final(self).lookup(key) == rows@,
            forall|other: u64| #![trigger final(self).lookup(other)]
                other != key ==> final(self).lookup(other) == old(self).lookup(other),
    {
        let index = (key % 256) as usize;
        let page = self.pages[index].replace(key, Arc::new(rows));
        self.pages.set(index, Arc::new(page));
        proof {
            assert forall|position: int| #![trigger self.pages@[position]]
                0 <= position < 256 ==> (*self.pages@[position]).valid() by {
                if position != index { assert(self.pages@[position] == old(self).pages@[position]); }
            }
        }
    }
}

impl<T> Work<T> {
    pub open spec fn lookup(self, key: u64) -> Seq<T> {
        if self.map@.dom().contains(key) { self.map@[key]@ } else { Seq::empty() }
    }

    pub open spec fn ready(self) -> bool {
        self.keys@.no_duplicates() && self.keys@.to_set() == self.map@.dom()
    }

    pub fn new() -> (result: Work<T>)
        ensures result.ready(), result.map@.dom().is_empty(), result.keys.len() == 0,
    {
        let result = Work { map: HashMap::new(), keys: Vec::new() };
        proof {
            assert(result.keys@.no_duplicates());
            assert(result.keys@.to_set() =~= result.map@.dom());
        }
        result
    }

    pub fn take(&mut self, key: u64) -> (result: Vec<T>)
        requires old(self).ready(),
        ensures
            result@ == old(self).lookup(key),
            final(self).keys@.no_duplicates(),
            final(self).keys@.contains(key),
            final(self).keys@.to_set() == old(self).keys@.to_set().insert(key),
            final(self).map@.dom() == final(self).keys@.to_set().remove(key),
            forall|other: u64| #![trigger final(self).lookup(other)]
                other != key ==> final(self).lookup(other) == old(self).lookup(other),
    {
        let ghost before = *self;
        proof { self.keys@.to_set_ensures(); }
        match self.map.remove(&key) {
            Some(rows) => {
                proof {
                    assert(self.keys@.to_set() =~= before.keys@.to_set().insert(key));
                    assert(self.map@.dom() =~= self.keys@.to_set().remove(key));
                }
                rows
            },
            None => {
                proof { lemma_unique_push(self.keys@, key); }
                self.keys.push(key);
                proof {
                    self.keys@.to_set_ensures();
                    assert(self.keys@ =~= before.keys@.push(key));
                    lemma_push_to_set(before.keys@, key);
                    assert(self.keys@.to_set() =~= before.keys@.to_set().insert(key));
                    assert(self.map@.dom() =~= self.keys@.to_set().remove(key));
                }
                Vec::new()
            },
        }
    }

    pub fn put(&mut self, key: u64, rows: Vec<T>)
        requires old(self).keys@.no_duplicates(), old(self).keys@.contains(key),
            old(self).map@.dom() == old(self).keys@.to_set().remove(key),
        ensures final(self).ready(), final(self).keys@ == old(self).keys@,
            final(self).lookup(key) == rows@,
            forall|other: u64| #![trigger final(self).lookup(other)]
                other != key ==> final(self).lookup(other) == old(self).lookup(other),
    {
        self.map.insert(key, rows);
        proof { self.keys@.to_set_ensures(); }
    }

    pub fn freeze(self) -> (result: Store<T>)
        requires self.ready(),
        ensures result.valid(), forall|key: u64| #[trigger] result.lookup(key) == self.lookup(key),
    {
        let ghost source = self;
        let mut work = self;
        let mut pages: Vec<Arc<Page<T>>> = Vec::new();
        let mut partition = 0usize;
        while partition < 256
            invariant partition <= 256, pages.len() == partition,
                forall|position: int| #![trigger pages@[position]] 0 <= position < pages.len() ==> (*pages@[position]).valid(),
                forall|position: int, key: u64| #![trigger pages@[position].lookup(key)]
                    0 <= position < pages.len() ==> (*pages@[position]).lookup(key).len() == 0,
            decreases 256 - partition,
        {
            let base = Base { map: HashMap::new(), keys: Vec::new() };
            proof { assert(base.keys@.to_set() =~= base.map@.dom()); }
            let page = Page { base: Arc::new(base), edits: Vec::new() };
            proof { assert(page.valid()); }
            pages.push(Arc::new(page));
            partition += 1;
        }
        let mut result = Store { pages };
        let mut index = 0usize;
        while index < work.keys.len()
            invariant index <= work.keys.len(), work.keys@ == source.keys@, source == self, source.ready(), result.valid(),
                work.map@.dom() == source.map@.dom().difference(source.keys@.take(index as int).to_set()),
                forall|key: u64| #![trigger work.lookup(key)] !source.keys@.take(index as int).contains(key) ==> work.lookup(key) == source.lookup(key),
                forall|key: u64| #![trigger result.lookup(key)] result.lookup(key)
                    == if source.keys@.take(index as int).contains(key) { source.lookup(key) } else { Seq::empty() },
            decreases work.keys.len() - index,
        {
            let key = work.keys[index];
            let ghost previous = work;
            proof {
                lemma_prefix_fresh(source.keys@, index as int);
                assert(work.lookup(key) == source.lookup(key));
            }
            let rows = match work.map.remove(&key) { Some(rows) => rows, None => Vec::new() };
            result.replace(key, rows);
            proof {
                assert(source.keys@.take(index as int + 1) =~= source.keys@.take(index as int).push(key));
                lemma_push_to_set(source.keys@.take(index as int), key);
                assert forall|other: u64| #![trigger work.lookup(other)]
                    !source.keys@.take(index as int + 1).contains(other) ==> work.lookup(other) == source.lookup(other) by {
                    if other != key { assert(work.lookup(other) == previous.lookup(other)); }
                }
            }
            index += 1;
        }
        proof {
            assert forall|key: u64| #[trigger] result.lookup(key) == source.lookup(key) by {
                if !source.keys@.contains(key) { assert(!source.map@.dom().contains(key)); }
            }
        }
        result
    }
}

}
