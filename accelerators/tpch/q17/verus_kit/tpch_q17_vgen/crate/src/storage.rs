use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub proof fn lemma_route(span: nat, slot: nat)
    requires span > 1, span % 16 == 0, slot < span,
    ensures span / 16 > 0, slot / (span / 16) < 16, slot % (span / 16) < span / 16,
{
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(span as int, 16);
    assert(span == 16 * (span / 16));
    assert(span / 16 > 0) by (nonlinear_arith) requires span > 1, span == 16 * (span / 16);
    vstd::arithmetic::div_mod::lemma_fundamental_div_mod(slot as int, (span / 16) as int);
    vstd::arithmetic::div_mod::lemma_mod_bound(slot as int, (span / 16) as int);
    assert(span / 16 > 0 && slot / (span / 16) < 16 && slot % (span / 16) < span / 16) by (nonlinear_arith)
        requires span > 1, slot < span, span == 16 * (span / 16);
}

/// Allocation size for an overlay rebuilt from `len` entries plus one (a hint only).
pub fn capacity_hint(len: usize) -> (result: usize)
{ if len < 32 { len + 1 } else { 33 } }

pub fn sum_hint(a: usize, b: usize) -> (result: usize)
{ match a.checked_add(b) { Some(value) => value, None => a } }

pub struct Bucket<Value> {
    pub base: Arc<HashMap<i64, Arc<Value>>>,
    pub keys: Arc<Vec<i64>>,
    pub overlay: HashMap<i64, Option<Arc<Value>>>,
    pub overlay_keys: Vec<i64>,
}

impl<Value> Bucket<Value> {
    pub open spec fn lookup(self, key: i64) -> Option<Value> {
        if self.overlay@.contains_key(key) {
            match self.overlay@[key] { Some(value) => Some(*value), None => None }
        } else if self.base@.contains_key(key) {
            Some(*self.base@[key])
        } else {
            None
        }
    }

    pub open spec fn wf(self) -> bool {
        &&& self.keys@.no_duplicates()
        &&& self.base@.dom() == self.keys@.to_set()
        &&& self.overlay_keys@.no_duplicates()
        &&& self.overlay@.dom() == self.overlay_keys@.to_set()
    }

    pub fn get(&self, key: i64) -> (result: Option<Arc<Value>>)
        requires self.wf(),
        ensures match result { Some(value) => self.lookup(key) == Some(*value), None => self.lookup(key).is_none() },
    {
        if let Some(entry) = self.overlay.get(&key) {
            return match entry { Some(value) => Some(value.clone()), None => None };
        }
        match self.base.get(&key) { Some(value) => Some(value.clone()), None => None }
    }

    pub fn replace(&self, key: i64, value: Option<Arc<Value>>) -> (result: Self)
        requires self.wf(),
        ensures result.wf(),
            result.overlay_keys@.len() < 32,
            result.lookup(key) == match value { Some(payload) => Some(*payload), None => None },
            forall|other: i64| #[trigger] result.lookup(other) == if other == key {
                match value { Some(payload) => Some(*payload), None => None }
            } else { self.lookup(other) },
    {
        let mut overlay: HashMap<i64, Option<Arc<Value>>> = HashMap::with_capacity(capacity_hint(self.overlay_keys.len()));
        let mut overlay_keys: Vec<i64> = Vec::with_capacity(capacity_hint(self.overlay_keys.len()));
        let mut index = 0;
        while index < self.overlay_keys.len()
            invariant index <= self.overlay_keys.len(), self.wf(),
                overlay_keys@.no_duplicates(), overlay@.dom() == overlay_keys@.to_set(),
                overlay@.dom() == self.overlay_keys@.take(index as int).to_set().remove(key),
                forall|other: i64| #![trigger overlay@[other]] overlay@.contains_key(other) ==> overlay@[other] == self.overlay@[other],
            decreases self.overlay_keys.len() - index,
        {
            let other = self.overlay_keys[index];
            proof {
                verus_prooflib::lemma_prefix_fresh(self.overlay_keys@, index as int);
                overlay_keys@.to_set_ensures();
                self.overlay_keys@.to_set_ensures();
                self.overlay_keys@.take(index as int).to_set_ensures();
                assert(self.overlay@.contains_key(other));
                assert(self.overlay_keys@.take(index as int + 1) =~= self.overlay_keys@.take(index as int).push(other));
            }
            if other != key {
                let entry = self.overlay.get(&other).unwrap();
                proof {
                    assert(!overlay@.contains_key(other));
                    assert(!overlay_keys@.contains(other));
                    verus_prooflib::lemma_unique_push(overlay_keys@, other);
                }
                overlay.insert(other, match entry { Some(payload) => Some(payload.clone()), None => None });
                overlay_keys.push(other);
            }
            index += 1;
            proof { overlay_keys@.to_set_ensures(); }
        }
        proof {
            overlay_keys@.to_set_ensures();
            assert(!overlay@.contains_key(key));
            assert(!overlay_keys@.contains(key));
            verus_prooflib::lemma_unique_push(overlay_keys@, key);
        }
        overlay.insert(key, value);
        overlay_keys.push(key);
        let updated = Bucket { base: self.base.clone(), keys: self.keys.clone(), overlay, overlay_keys };
        proof {
            overlay_keys@.to_set_ensures();
            assert(updated.overlay@.dom() =~= updated.overlay_keys@.to_set());
            assert(updated.overlay_keys@.no_duplicates());
            assert(updated.base@.dom() == updated.keys@.to_set());
            assert(updated.wf());
            assert forall|other: i64| #[trigger] updated.lookup(other) == if other == key {
                match value { Some(payload) => Some(*payload), None => None }
            } else { self.lookup(other) } by {}
        }
        if updated.overlay_keys.len() < 32 { return updated; }
        let mut base: HashMap<i64, Arc<Value>> = HashMap::with_capacity(sum_hint(updated.keys.len(), updated.overlay_keys.len()));
        let mut keys: Vec<i64> = Vec::with_capacity(sum_hint(updated.keys.len(), updated.overlay_keys.len()));
        index = 0;
        while index < updated.keys.len()
            invariant index <= updated.keys.len(), updated.wf(),
                keys@.no_duplicates(), base@.dom() == keys@.to_set(),
                forall|other: i64| #[trigger] base@.contains_key(other)
                    <==> updated.keys@.take(index as int).contains(other) && updated.lookup(other).is_some(),
                forall|other: i64| #![trigger base@[other]] base@.contains_key(other) ==> updated.lookup(other) == Some(*base@[other]),
            decreases updated.keys.len() - index,
        {
            let old_key = updated.keys[index];
            proof {
                verus_prooflib::lemma_prefix_fresh(updated.keys@, index as int);
                keys@.to_set_ensures();
                assert(updated.keys@.take(index as int + 1) =~= updated.keys@.take(index as int).push(old_key));
            }
            if let Some(payload) = updated.get(old_key) {
                proof {
                    assert(!base@.contains_key(old_key));
                    assert(!keys@.contains(old_key));
                    verus_prooflib::lemma_unique_push(keys@, old_key);
                }
                base.insert(old_key, payload);
                keys.push(old_key);
            }
            index += 1;
            proof { keys@.to_set_ensures(); }
        }
        index = 0;
        while index < updated.overlay_keys.len()
            invariant index <= updated.overlay_keys.len(), updated.wf(),
                keys@.no_duplicates(), base@.dom() == keys@.to_set(),
                forall|other: i64| #[trigger] base@.contains_key(other)
                    <==> (updated.keys@.contains(other) || updated.overlay_keys@.take(index as int).contains(other)) && updated.lookup(other).is_some(),
                forall|other: i64| #![trigger base@[other]] base@.contains_key(other) ==> updated.lookup(other) == Some(*base@[other]),
            decreases updated.overlay_keys.len() - index,
        {
            let new_key = updated.overlay_keys[index];
            proof {
                verus_prooflib::lemma_prefix_fresh(updated.overlay_keys@, index as int);
                keys@.to_set_ensures();
                updated.keys@.to_set_ensures();
                assert(updated.overlay_keys@.take(index as int + 1) =~= updated.overlay_keys@.take(index as int).push(new_key));
            }
            if !updated.base.contains_key(&new_key) {
                if let Some(payload) = updated.get(new_key) {
                    proof {
                        assert(!base@.contains_key(new_key));
                        assert(!keys@.contains(new_key));
                        verus_prooflib::lemma_unique_push(keys@, new_key);
                    }
                    base.insert(new_key, payload);
                    keys.push(new_key);
                }
            }
            index += 1;
            proof { keys@.to_set_ensures(); }
        }
        let result = Bucket { base: Arc::new(base), keys: Arc::new(keys), overlay: HashMap::new(), overlay_keys: Vec::new() };
        proof {
            assert forall|other: i64| #[trigger] result.lookup(other) == updated.lookup(other) by {
                if updated.lookup(other).is_some() {
                    assert(updated.keys@.contains(other) || updated.overlay_keys@.contains(other));
                }
            }
        }
        result
    }
}

pub enum Page<Value> {
    Leaf(Arc<Bucket<Value>>),
    Directory(Vec<Arc<Page<Value>>>),
}

pub proof fn lemma_directory_lookup<Value>(page: Page<Value>, children: Vec<Arc<Page<Value>>>, span: nat, slot: nat, key: i64)
    requires page == Page::Directory(children), span > 1, span % 16 == 0, children@.len() == 16, slot < span,
    ensures page.lookup(span, slot, key) == children@[(slot / (span / 16)) as int].lookup(span / 16, slot % (span / 16), key),
{ lemma_route(span, slot); reveal(Page::lookup); }

impl<Value> Page<Value> {
    #[verifier::opaque]
    pub open spec fn wf(self, span: nat) -> bool
        decreases span,
    {
        match self {
            Page::Leaf(bucket) => span == 1 && bucket.wf() && bucket.overlay_keys@.len() < 32,
            Page::Directory(children) => span > 1 && span % 16 == 0 && children@.len() == 16
                && forall|index: int| #![trigger children@[index]] 0 <= index < 16 ==> children@[index].wf(span / 16),
        }
    }

    #[verifier::opaque]
    pub open spec fn lookup(self, span: nat, slot: nat, key: i64) -> Option<Value>
        decreases span,
    {
        match self {
            Page::Leaf(bucket) => bucket.lookup(key),
            Page::Directory(children) => if span > 1 {
                children@[(slot / (span / 16)) as int].lookup(span / 16, slot % (span / 16), key)
            } else { None },
        }
    }

    pub fn get(&self, span: usize, slot: usize, key: i64) -> (result: Option<Arc<Value>>)
        requires self.wf(span as nat), slot < span,
        ensures match result { Some(value) => self.lookup(span as nat, slot as nat, key) == Some(*value),
            None => self.lookup(span as nat, slot as nat, key).is_none() },
        decreases span,
    {
        proof { reveal(Page::wf); reveal(Page::lookup); }
        match self {
            Page::Leaf(bucket) => bucket.get(key),
            Page::Directory(children) => {
                proof { lemma_route(span as nat, slot as nat); }
                let width = span / 16;
                proof { assert(children@[(slot / width) as int].wf(width as nat)); }
                children[slot / width].get(width, slot % width, key)
            }
        }
    }

    #[verifier::rlimit(1)]
    pub fn replace(&self, span: usize, slot: usize, key: i64, value: Option<Arc<Value>>) -> (result: Self)
        requires self.wf(span as nat), slot < span,
        ensures result.wf(span as nat),
            forall|position: nat, other: i64| position < span ==> #[trigger] result.lookup(span as nat, position, other)
                == if position == slot && other == key {
                    match value { Some(payload) => Some(*payload), None => None }
                } else { self.lookup(span as nat, position, other) },
        decreases span,
    {
        proof { reveal(Page::wf); reveal(Page::lookup); }
        match self {
            Page::Leaf(bucket) => Page::Leaf(Arc::new(bucket.replace(key, value))),
            Page::Directory(children) => {
                proof { lemma_route(span as nat, slot as nat); }
                let width = span / 16;
                let target = slot / width;
                proof { assert(children@[target as int].wf(width as nat)); }
                let replacement = Arc::new(children[target].replace(width, slot % width, key, value));
                let mut copied = Vec::with_capacity(16);
                let mut index = 0;
                while index < children.len()
                    invariant index <= children.len(), children.len() == 16,
                        copied@.len() == index, width > 0, span == 16 * width, target < 16,
                        replacement.wf(width as nat),
                        forall|position: nat, other: i64| position < width ==> #[trigger] replacement.lookup(width as nat, position, other)
                            == if position == slot % width && other == key { match value { Some(payload) => Some(*payload), None => None } }
                            else { children@[target as int].lookup(width as nat, position, other) },
                        forall|position: int| #![trigger children@[position]] 0 <= position < 16 ==> children@[position].wf(width as nat),
                        forall|position: int| #![trigger copied@[position]] 0 <= position < index ==>
                            copied@[position] == if position == target { replacement } else { children@[position] },
                    decreases children.len() - index,
                {
                    copied.push(if index == target { replacement.clone() } else { children[index].clone() });
                    index += 1;
                }
                let result = Page::Directory(copied);
                proof {
                    reveal(Page::wf); reveal(Page::lookup);
                    assert(result.wf(span as nat));
                    assert forall|position: nat, other: i64| position < span implies #[trigger] result.lookup(span as nat, position, other)
                        == if position == slot && other == key {
                            match value { Some(payload) => Some(*payload), None => None }
                    } else { self.lookup(span as nat, position, other) } by {
                        lemma_route(span as nat, position);
                        lemma_directory_lookup(result, copied, span as nat, position, other);
                        lemma_directory_lookup(*self, *children, span as nat, position, other);
                        assert(span / 16 == width);
                        assert(copied@[(position / (width as nat)) as int] == if position / (width as nat) == target { replacement } else { children@[(position / (width as nat)) as int] });
                        if position / (width as nat) == target {
                            assert((position / (width as nat) == slot / width && position % (width as nat) == slot % width) == (position == slot)) by (nonlinear_arith)
                                requires width > 0;
                        }
                    }
                }
                result
            }
        }
    }
}

pub struct Store<Value> {
    pub root: Arc<Page<Value>>,
    pub span: usize,
}

impl<Value> Store<Value> {
    pub open spec fn wf(self) -> bool { self.span > 0 && self.root.wf(self.span as nat) }

    pub open spec fn lookup(self, key: i64) -> Option<Value> {
        self.root.lookup(self.span as nat, ((key as u64) % (self.span as u64)) as nat, key)
    }

    #[verifier::rlimit(1)]
    pub fn new(expected: usize) -> (store: Self)
        requires expected <= 9223372036854775808,
        ensures store.wf(), forall|key: i64| #[trigger] store.lookup(key).is_none(),
    {
        proof { reveal(Page::wf); reveal(Page::lookup); }
        let empty = Bucket { base: Arc::new(HashMap::new()), keys: Arc::new(Vec::new()), overlay: HashMap::new(), overlay_keys: Vec::new() };
        proof {
            empty.keys@.to_set_ensures(); empty.overlay_keys@.to_set_ensures();
            assert(empty.keys@.to_set() =~= Set::empty());
            assert(empty.overlay_keys@.to_set() =~= Set::empty());
            assert(empty.wf());
        }
        let mut root = Arc::new(Page::Leaf(Arc::new(empty)));
        let mut span: usize = 1;
        let wanted = expected / 128 + 1;
        assert(wanted <= 72057594037927937) by (nonlinear_arith) requires expected <= 9223372036854775808, wanted == expected / 128 + 1;
        while span < wanted
            invariant span > 0, wanted <= 72057594037927937, root.wf(span as nat),
                forall|slot: nat, key: i64| slot < span ==> #[trigger] root.lookup(span as nat, slot, key).is_none(),
            decreases usize::MAX - span,
        {
            let next_span = span * 16;
            proof { assert(next_span > span) by (nonlinear_arith) requires span > 0, next_span == 16 * span; }
            let mut children = Vec::new();
            let mut index = 0;
            while index < 16
                invariant index <= 16, children@.len() == index,
                    span > 0, next_span == span * 16, root.wf(span as nat),
                    forall|slot: nat, key: i64| slot < span ==> #[trigger] root.lookup(span as nat, slot, key).is_none(),
                    forall|position: int| #![trigger children@[position]] 0 <= position < index ==> children@[position] == root,
                decreases 16 - index,
            {
                children.push(root.clone());
                index += 1;
            }
            let next_root = Arc::new(Page::Directory(children));
            proof {
                reveal(Page::wf); reveal(Page::lookup);
                assert(next_root.wf(next_span as nat));
                assert forall|slot: nat, key: i64| slot < next_span implies #[trigger] next_root.lookup(next_span as nat, slot, key).is_none() by {
                    lemma_route(next_span as nat, slot);
                    assert(next_span / 16 == span);
                    lemma_directory_lookup(*next_root, children, next_span as nat, slot, key);
                    assert(children@[(slot / (span as nat)) as int] == root);
                    assert(root.lookup(span as nat, slot % (span as nat), key).is_none());
                }
            }
            root = next_root;
            span = next_span;
        }
        Store { root, span }
    }

    pub fn fork(&self) -> (result: Self)
        requires self.wf(),
        ensures result.wf(), forall|key: i64| #[trigger] result.lookup(key) == self.lookup(key),
    {
        Store { root: self.root.clone(), span: self.span }
    }

    pub fn get(&self, key: i64) -> (result: Option<Arc<Value>>)
        requires self.wf(),
        ensures match result { Some(value) => self.lookup(key) == Some(*value), None => self.lookup(key).is_none() },
    {
        self.root.get(self.span, ((key as u64) % (self.span as u64)) as usize, key)
    }

    pub fn put(&mut self, key: i64, value: Option<Arc<Value>>)
        requires old(self).wf(),
        ensures final(self).wf(),
            forall|other: i64| #[trigger] final(self).lookup(other) == if other == key {
                match value { Some(payload) => Some(*payload), None => None }
            } else { old(self).lookup(other) },
    {
        self.root = Arc::new(self.root.replace(self.span, ((key as u64) % (self.span as u64)) as usize, key, value));
    }
}

}
