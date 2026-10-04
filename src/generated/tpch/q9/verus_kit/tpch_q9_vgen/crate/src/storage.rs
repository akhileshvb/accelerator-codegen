use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::rel::{flat, lemma_flat_contains};

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct Work<Value> {
    pub rows: HashMap<i64, Value>,
    pub keys: Vec<i64>,
    pub seen: HashMap<i64, bool>,
}

impl<Value> Work<Value> {
    pub open spec fn valid(self) -> bool {
        self.keys@.no_duplicates() && self.seen@.dom() == self.keys@.to_set()
            && self.rows@.dom().subset_of(self.seen@.dom())
    }

    pub fn new() -> (result: Self)
        ensures result.valid(), result.rows@ == Map::<i64, Value>::empty(), result.keys@ == Seq::<i64>::empty(),
    {
        let result = Self { rows: HashMap::new(), keys: Vec::new(), seen: HashMap::new() };
        proof {
            assert(result.keys@.to_set() =~= Set::<i64>::empty());
            assert(result.rows@.dom() =~= Set::<i64>::empty());
            assert(result.seen@.dom() =~= Set::<i64>::empty());
        }
        result
    }

    pub fn put(&mut self, key: i64, value: Value)
        requires old(self).valid(),
        ensures final(self).valid(), final(self).rows@ == old(self).rows@.insert(key, value),
            final(self).keys@ == if old(self).seen@.contains_key(key) { old(self).keys@ } else { old(self).keys@.push(key) },
    {
        let ghost previous = self.keys@;
        if !self.seen.contains_key(&key) {
            proof { verus_prooflib::lemma_unique_push(previous, key); }
            self.seen.insert(key, true);
            self.keys.push(key);
            proof {
                verus_prooflib::lemma_push_to_set(previous, key);
                assert(self.keys@.to_set() =~= previous.to_set().insert(key));
                assert(self.seen@.dom() =~= self.keys@.to_set());
            }
        }
        self.rows.insert(key, value);
        proof { assert(self.rows@.dom().subset_of(self.seen@.dom())); }
    }

    pub fn take(&mut self, key: i64) -> (result: Option<Value>)
        requires old(self).valid(),
        ensures final(self).valid(), final(self).keys@ == old(self).keys@,
            final(self).seen@ == old(self).seen@,
            final(self).rows@ == old(self).rows@.remove(key),
            result == if old(self).rows@.contains_key(key) { Some(old(self).rows@[key]) } else { None },
    {
        self.rows.remove(&key)
    }
}

pub enum Directory<Value> {
    Page { rows: HashMap<i64, Arc<Value>>, keys: Vec<i64> },
    Branch { children: Vec<Option<Arc<Directory<Value>>>> },
}

pub struct Pages<Value> {
    root: Arc<Directory<Value>>,
    depth: u32,
}

pub open spec fn slot(key: i64, shift: u32) -> int {
    routed_bits(key, shift as nat) % 32
}

pub open spec fn routed_bits(key: i64, shift: nat) -> int
    decreases shift,
{
    if shift == 0 { (key as u64) as int } else { routed_bits(key, (shift - 1) as nat) / 32 }
}

pub open spec fn pow32(depth: nat) -> int
    decreases depth,
{
    if depth == 0 { 1 } else { 32 * pow32((depth - 1) as nat) }
}

impl<Value> Directory<Value> {
    pub open spec fn support(&self, depth: nat) -> Seq<i64>
        decreases depth, 0nat,
    {
        match self {
            Directory::Page { keys, .. } => keys@,
            Directory::Branch { children } => if depth == 0 { Seq::empty() } else {
                flat(child_supports(children@, (depth - 1) as nat))
            },
        }
    }

    pub open spec fn at(self, key: i64, depth: nat, shift: nat) -> Option<Value>
        decreases depth,
    {
        match self {
            Directory::Page { rows, .. } => if rows@.contains_key(key) { Some(*rows@[key]) } else { None },
            Directory::Branch { children } => {
                let index = routed_bits(key, shift) % 32;
                if depth > 0 && index < children.len() && children[index].is_some() {
                    children[index].unwrap().at(key, (depth - 1) as nat, shift + 1)
                } else { None }
            },
        }
    }

    pub open spec fn valid(self, depth: nat, shift: nat) -> bool
        decreases depth,
    {
        match self {
            Directory::Page { rows, keys } => keys@.no_duplicates()
                && forall|key: i64| #[trigger] rows@.contains_key(key) <==> keys@.contains(key),
            Directory::Branch { children } => depth > 0 && children.len() == 32
                && (forall|index: int| 0 <= index < children.len() && #[trigger] children[index].is_some()
                    ==> children[index].unwrap().valid((depth - 1) as nat, shift + 1)
                        && children[index].unwrap().support((depth - 1) as nat).len() > 0)
                && (forall|index: int, key: i64| 0 <= index < children.len() && children[index].is_some()
                    && #[trigger] children[index].unwrap().support((depth - 1) as nat).contains(key)
                    ==> routed_bits(key, shift) % 32 == index),
        }
    }
}

pub open spec fn child_supports<Value>(children: Seq<Option<Arc<Directory<Value>>>>, depth: nat) -> Seq<Seq<i64>>
    decreases depth, 1nat,
{
    Seq::new(children.len(), |position: int| match children[position] {
        Some(child) => child.support(depth), None => Seq::empty(),
    })
}

pub proof fn lemma_directory_support<Value>(node: &Directory<Value>, depth: nat, shift: nat)
    requires node.valid(depth, shift),
    ensures node.support(depth).no_duplicates(),
        forall|key: i64| #[trigger] node.support(depth).contains(key) <==> node.at(key, depth, shift).is_some(),
    decreases depth,
{
    match node {
        Directory::Page { rows, keys } => {},
        Directory::Branch { children } => {
            let supports = child_supports(children@, (depth - 1) as nat);
            assert forall|position: int| 0 <= position < supports.len() implies (#[trigger] supports[position]).no_duplicates() by {
                if children[position].is_some() {
                    lemma_directory_support(&*children[position].unwrap(), (depth - 1) as nat, shift + 1);
                }
            }
            assert forall|left: int, right: int, key: i64| #![trigger supports[left].contains(key), supports[right].contains(key)]
                0 <= left < right < supports.len() implies !(supports[left].contains(key) && supports[right].contains(key)) by {
                if supports[left].contains(key) && supports[right].contains(key) {
                    assert(children[left].is_some() && children[right].is_some());
                    assert(routed_bits(key, shift) % 32 == left);
                    assert(routed_bits(key, shift) % 32 == right);
                }
            }
            verus_prooflib::lemma_flat_unique(supports);
            assert forall|key: i64| #[trigger] node.support(depth).contains(key) <==> node.at(key, depth, shift).is_some() by {
                lemma_flat_contains(supports, key);
                if node.support(depth).contains(key) {
                    let position = choose|position: int| 0 <= position < supports.len() && (#[trigger] supports[position]).contains(key);
                    assert(supports.len() == children.len());
                    assert(supports[position] == match children@[position] {
                        Some(child) => child.support((depth - 1) as nat), None => Seq::<i64>::empty(),
                    });
                    assert(!Seq::<i64>::empty().contains(key));
                    assert(children[position].is_some());
                    let child = children[position].unwrap();
                    lemma_directory_support(&*child, (depth - 1) as nat, shift + 1);
                    assert(routed_bits(key, shift) % 32 == position);
                } else if node.at(key, depth, shift).is_some() {
                    let position = routed_bits(key, shift) % 32;
                    assert(0 <= position < 32);
                    assert(children[position].is_some());
                    lemma_directory_support(&*children[position].unwrap(), (depth - 1) as nat, shift + 1);
                    assert(supports[position].contains(key));
                }
            }
        },
    }
}

pub fn route(key: i64, shift: u32) -> (result: usize)
    requires shift <= 12,
    ensures result < 32, result as int == slot(key, shift),
{
    let mut bits = key as u64;
    let mut index: u32 = 0;
    while index < shift
        invariant index <= shift, shift <= 12, bits as int == routed_bits(key, index as nat),
        decreases shift - index,
    {
        bits = bits / 32;
        index += 1;
    }
    (bits % 32) as usize
}

pub open spec fn value_at<Value>(rows: Map<i64, Value>, key: i64) -> Option<Value> {
    if rows.contains_key(key) { Some(rows[key]) } else { None }
}

pub open spec fn arc_at<Value>(rows: Map<i64, Arc<Value>>, key: i64) -> Option<Value> {
    if rows.contains_key(key) { Some(*rows[key]) } else { None }
}

pub fn freeze<Value>(work: Work<Arc<Value>>, depth: u32, shift: u32) -> (result: Arc<Directory<Value>>)
    requires depth + shift <= 12, work.valid(),
    ensures result.valid(depth as nat, shift as nat),
        result.support(depth as nat).to_set() == work.rows@.dom(),
        forall|key: i64| #[trigger] result.at(key, depth as nat, shift as nat) == arc_at(work.rows@, key),
    decreases depth,
{
    let ghost initial_rows = work.rows@;
    let ghost initial_keys = work.keys@;
    let mut pending = work;
    if depth == 0 || pending.rows.len() <= 256 {
        let mut rows = HashMap::new();
        let mut keys = Vec::new();
        let mut index = 0;
        let mut processed = Ghost(0int);
        while index < pending.keys.len()
            invariant index <= pending.keys.len(), processed@ == index as int,
                initial_rows == work.rows@, initial_keys == work.keys@,
                pending.valid(), pending.keys@ == initial_keys, initial_keys.no_duplicates(),
                initial_rows.dom().subset_of(initial_keys.to_set()),
                keys@.no_duplicates(), keys@.to_set() == rows@.dom(),
                rows@.dom() == initial_rows.dom().intersect(initial_keys.take(index as int).to_set()),
                forall|key: i64| #[trigger] arc_at(rows@, key) == if initial_keys.take(index as int).contains(key) { arc_at(initial_rows, key) } else { None },
                forall|key: i64| #[trigger] arc_at(pending.rows@, key) == if initial_keys.take(index as int).contains(key) { None } else { arc_at(initial_rows, key) },
            decreases pending.keys.len() - index,
        {
            let key = pending.keys[index];
            let ghost prior_pending = pending.rows@;
            let ghost prior_rows = rows@;
            proof {
                verus_prooflib::lemma_prefix_fresh(initial_keys, index as int);
                assert forall|target: i64| #[trigger] arc_at(prior_pending, target) == if initial_keys.take(index as int).contains(target) { None } else { arc_at(initial_rows, target) } by {}
                assert forall|target: i64| #[trigger] arc_at(prior_rows, target) == if initial_keys.take(index as int).contains(target) { arc_at(initial_rows, target) } else { None } by {}
                assert(!rows@.contains_key(key));
                assert(!keys@.contains(key));
            }
            let entry = pending.take(key);
            proof {
                assert(pending.rows@ == prior_pending.remove(key));
                assert(entry == if prior_pending.contains_key(key) { Some(prior_pending[key]) } else { None });
            }
            if let Some(value) = entry {
                proof { verus_prooflib::lemma_unique_push(keys@, key); }
                rows.insert(key, value);
                keys.push(key);
            }
            proof {
                assert(initial_keys.take(index as int + 1) =~= initial_keys.take(index as int).push(key));
                initial_keys.take(index as int + 1).to_set_ensures();
                keys@.to_set_ensures();
                assert(keys@.to_set() =~= rows@.dom());
                assert forall|target: i64| #[trigger] rows@.contains_key(target) <==> initial_rows.contains_key(target) && initial_keys.take(index as int + 1).contains(target) by {
                    assert(initial_keys.take(index as int + 1).contains(target) <==> initial_keys.take(index as int).contains(target) || target == key);
                    assert(arc_at(prior_pending, key) == arc_at(initial_rows, key));
                }
                assert(rows@.dom() =~= initial_rows.dom().intersect(initial_keys.take(index as int + 1).to_set()));
                assert forall|target: i64| #[trigger] arc_at(pending.rows@, target) == if initial_keys.take(index as int + 1).contains(target) { None } else { arc_at(initial_rows, target) } by {
                    assert(arc_at(prior_pending, target) == if initial_keys.take(index as int).contains(target) { None } else { arc_at(initial_rows, target) });
                    assert(arc_at(pending.rows@, target) == if target == key { None } else { arc_at(prior_pending, target) });
                    assert(initial_keys.take(index as int + 1).contains(target) <==> initial_keys.take(index as int).contains(target) || target == key);
                }
                assert forall|target: i64| #[trigger] arc_at(rows@, target) == if initial_keys.take(index as int + 1).contains(target) { arc_at(initial_rows, target) } else { None } by {
                    assert(arc_at(prior_rows, target) == if initial_keys.take(index as int).contains(target) { arc_at(initial_rows, target) } else { None });
                    assert(arc_at(prior_pending, key) == arc_at(initial_rows, key));
                    assert(rows@ == if entry.is_some() { prior_rows.insert(key, entry.unwrap()) } else { prior_rows });
                    assert(initial_keys.take(index as int + 1).contains(target) <==> initial_keys.take(index as int).contains(target) || target == key);
                }
            }
            index += 1;
            processed = Ghost(index as int);
        }
        proof {
            assert(initial_keys.take(index as int) =~= initial_keys);
            assert(rows@.dom() =~= initial_rows.dom());
        }
        let result = Arc::new(Directory::Page { rows, keys });
        proof {
            assert forall|key: i64| #[trigger] result.at(key, depth as nat, shift as nat) == arc_at(work.rows@, key) by {
                assert(arc_at(rows@, key) == arc_at(initial_rows, key));
            }
        }
        result
    } else {
        let mut buckets: Vec<Work<Arc<Value>>> = Vec::new();
        let mut index = 0;
        while index < 32
            invariant index <= 32, buckets.len() == index,
                initial_rows == work.rows@, initial_keys == work.keys@,
                pending.valid(), pending.rows@ == initial_rows, pending.keys@ == initial_keys,
                initial_rows.dom().subset_of(initial_keys.to_set()),
                forall|position: int| 0 <= position < buckets.len() ==> #[trigger] buckets@[position].valid()
                    && buckets@[position].rows@ == Map::<i64, Arc<Value>>::empty() && buckets@[position].keys@ == Seq::<i64>::empty()
                    && buckets@[position].rows@.dom() == buckets@[position].keys@.to_set(),
            decreases 32 - index,
        {
            let bucket = Work::new();
            proof { assert(bucket.rows@.dom() =~= bucket.keys@.to_set()); }
            buckets.push(bucket);
            index += 1;
        }
        index = 0;
        proof {
            assert(initial_keys.take(0) =~= Seq::<i64>::empty());
            assert forall|position: int, key: i64| 0 <= position < 32 implies
                #[trigger] arc_at(buckets@[position].rows@, key) == None by {
                assert(buckets@[position].valid());
                assert(buckets@[position].rows@ == Map::<i64, Arc<Value>>::empty());
            }
        }
        let mut processed = Ghost(0int);
        while index < pending.keys.len()
            invariant index <= pending.keys.len(), buckets.len() == 32, processed@ == index as int,
                initial_rows == work.rows@, initial_keys == work.keys@,
                0 < depth, depth + shift <= 12, pending.valid(), pending.keys@ == initial_keys, initial_keys.no_duplicates(),
                initial_rows.dom().subset_of(initial_keys.to_set()),
                forall|position: int| 0 <= position < 32 ==> #[trigger] buckets@[position].valid()
                    && buckets@[position].rows@.dom() == buckets@[position].keys@.to_set(),
                forall|position: int, key: i64| 0 <= position < 32 ==>
                    #[trigger] arc_at(buckets@[position].rows@, key) == if initial_keys.take(index as int).contains(key) && slot(key, shift) == position { arc_at(initial_rows, key) } else { None },
                forall|key: i64| #[trigger] arc_at(pending.rows@, key) == if initial_keys.take(index as int).contains(key) { None } else { arc_at(initial_rows, key) },
            decreases pending.keys.len() - index,
        {
            let key = pending.keys[index];
            let ghost prior_pending = pending.rows@;
            let ghost prior_buckets = buckets@;
            proof {
                verus_prooflib::lemma_prefix_fresh(initial_keys, index as int);
                assert forall|target: i64| #[trigger] arc_at(prior_pending, target) == if initial_keys.take(index as int).contains(target) { None } else { arc_at(initial_rows, target) } by {}
                assert forall|position: int, target: i64| 0 <= position < 32 implies
                    #[trigger] arc_at(prior_buckets[position].rows@, target) == if initial_keys.take(index as int).contains(target) && slot(target, shift) == position { arc_at(initial_rows, target) } else { None } by {}
            }
            let entry = pending.take(key);
            proof {
                assert(pending.rows@ == prior_pending.remove(key));
                assert(entry == if prior_pending.contains_key(key) { Some(prior_pending[key]) } else { None });
            }
            if let Some(value) = entry {
                let target = route(key, shift);
                let ghost previous = buckets@;
                proof {
                    assert(previous == prior_buckets);
                    assert(arc_at(previous[target as int].rows@, key) == None);
                    assert(!previous[target as int].rows@.contains_key(key));
                    assert(previous[target as int].valid());
                    assert(previous[target as int].rows@.dom() == previous[target as int].keys@.to_set());
                }
                let mut bucket = buckets.remove(target);
                proof { assert(!bucket.rows@.contains_key(key)); assert(!bucket.keys@.contains(key)); }
                bucket.put(key, value);
                proof {
                    verus_prooflib::lemma_push_to_set(previous[target as int].keys@, key);
                    assert(bucket.rows@.dom() =~= bucket.keys@.to_set());
                }
                let ghost replacement = bucket;
                buckets.insert(target, bucket);
                proof { assert(buckets@ =~= previous.update(target as int, replacement)); }
            }
            proof {
                assert(initial_keys.take(index as int + 1) =~= initial_keys.take(index as int).push(key));
                assert forall|target: i64| #[trigger] arc_at(pending.rows@, target) == if initial_keys.take(index as int + 1).contains(target) { None } else { arc_at(initial_rows, target) } by {
                    assert(arc_at(prior_pending, target) == if initial_keys.take(index as int).contains(target) { None } else { arc_at(initial_rows, target) });
                    assert(arc_at(pending.rows@, target) == if target == key { None } else { arc_at(prior_pending, target) });
                    assert(initial_keys.take(index as int + 1).contains(target) <==> initial_keys.take(index as int).contains(target) || target == key);
                }
                assert forall|position: int, target: i64| 0 <= position < 32 implies
                    #[trigger] arc_at(buckets@[position].rows@, target) == if initial_keys.take(index as int + 1).contains(target) && slot(target, shift) == position { arc_at(initial_rows, target) } else { None } by {
                    assert(arc_at(prior_buckets[position].rows@, target) == if initial_keys.take(index as int).contains(target) && slot(target, shift) == position { arc_at(initial_rows, target) } else { None });
                    assert(arc_at(prior_pending, key) == arc_at(initial_rows, key));
                    if entry.is_some() && slot(key, shift) == position {
                        assert(buckets@[position].rows@ == prior_buckets[position].rows@.insert(key, entry.unwrap()));
                    } else { assert(buckets@[position].rows@ == prior_buckets[position].rows@); }
                    assert(initial_keys.take(index as int + 1).contains(target) <==> initial_keys.take(index as int).contains(target) || target == key);
                }
            }
            index += 1;
            processed = Ghost(index as int);
        }
        let mut children: Vec<Option<Arc<Directory<Value>>>> = Vec::new();
        let ghost partitioned = buckets@;
        index = 0;
        while index < 32
            invariant index <= 32, buckets.len() == 32 - index, children.len() == index,
                initial_rows == work.rows@, initial_keys == work.keys@,
                0 < depth, depth + shift <= 12, initial_rows.dom().subset_of(initial_keys.to_set()),
                partitioned.len() == 32, buckets@ == partitioned.skip(index as int),
                forall|position: int| 0 <= position < 32 ==> #[trigger] partitioned[position].valid()
                    && partitioned[position].rows@.dom() == partitioned[position].keys@.to_set(),
                forall|position: int, key: i64| 0 <= position < 32 ==>
                    #[trigger] arc_at(partitioned[position].rows@, key) == if slot(key, shift) == position { arc_at(initial_rows, key) } else { None },
                forall|position: int| 0 <= position < children.len() ==> {
                    let child = #[trigger] children@[position];
                    match child {
                        Some(child) => child.valid((depth - 1) as nat, (shift + 1) as nat)
                            && child.support((depth - 1) as nat).len() > 0
                            && child.support((depth - 1) as nat).to_set() == partitioned[position].rows@.dom(),
                        None => partitioned[position].rows@.dom() == Set::<i64>::empty(),
                    }
                },
                forall|position: int, key: i64| 0 <= position < children.len() ==>
                    #[trigger] child_at(children@[position], key, (depth - 1) as nat, (shift + 1) as nat) == arc_at(partitioned[position].rows@, key),
            decreases 32 - index,
        {
            proof {
                assert(buckets@[0] == partitioned[index as int]);
                assert(partitioned[index as int].valid());
                assert(buckets@[0].rows@.dom() == buckets@[0].keys@.to_set());
            }
            let bucket = buckets.remove(0);
            proof { assert(bucket.rows@.dom() == bucket.keys@.to_set()); }
            if bucket.keys.len() == 0 { children.push(None); }
            else {
                let ghost witness = bucket.keys@[0];
                proof {
                    bucket.keys@.to_set_ensures();
                    assert(bucket.rows@.contains_key(witness));
                }
                let child = freeze(bucket, depth - 1, shift + 1);
                proof {
                    let ghost child_support = child.support((depth - 1) as nat);
                    child_support.to_set_ensures();
                    assert(child.support((depth - 1) as nat).contains(witness));
                }
                children.push(Some(child));
            }
            index += 1;
        }
        let result = Arc::new(Directory::Branch { children });
        proof {
            assert forall|position: int, key: i64| 0 <= position < children.len() && children@[position].is_some()
                && #[trigger] children@[position].unwrap().support((depth - 1) as nat).contains(key)
                implies routed_bits(key, shift as nat) % 32 == position by {
                children@[position].unwrap().support((depth - 1) as nat).to_set_ensures();
                assert(partitioned[position].rows@.contains_key(key));
                assert(arc_at(partitioned[position].rows@, key).is_some());
            }
            assert(result.valid(depth as nat, shift as nat));
            lemma_directory_support(&*result, depth as nat, shift as nat);
            let ghost result_support = result.support(depth as nat);
            result_support.to_set_ensures();
            assert forall|key: i64| #[trigger] result.at(key, depth as nat, shift as nat) == arc_at(initial_rows, key) by {
                let position = slot(key, shift);
                assert(0 <= position < 32);
                assert(child_at(children@[position], key, (depth - 1) as nat, (shift + 1) as nat) == arc_at(partitioned[position].rows@, key));
                assert(arc_at(partitioned[position].rows@, key) == arc_at(initial_rows, key));
            }
            assert forall|key: i64| #[trigger] result_support.to_set().contains(key) <==> initial_rows.contains_key(key) by {
                assert(result.at(key, depth as nat, shift as nat) == arc_at(initial_rows, key));
            }
            assert(result.support(depth as nat).to_set() =~= initial_rows.dom());
        }
        result
    }
}

pub open spec fn child_at<Value>(child: Option<Arc<Directory<Value>>>, key: i64, depth: nat, shift: nat) -> Option<Value> {
    match child { Some(child) => child.at(key, depth, shift), None => None }
}

pub fn lookup<Value>(node: &Arc<Directory<Value>>, key: i64, depth: u32, shift: u32) -> (result: Option<Arc<Value>>)
    requires depth + shift <= 12, node.valid(depth as nat, shift as nat),
    ensures node.at(key, depth as nat, shift as nat) == match result { Some(value) => Some(*value), None => None },
    decreases depth,
{
    match &**node {
        Directory::Page { rows, .. } => match rows.get(&key) {
            Some(value) => Some(Arc::clone(value)),
            None => None,
        },
        Directory::Branch { children } => {
            let index = route(key, shift);
            match &children[index] {
                Some(child) => lookup(child, key, depth - 1, shift + 1),
                None => None,
            }
        },
    }
}

pub fn occupied<Value>(node: &Arc<Directory<Value>>, depth: u32, shift: u32) -> (result: bool)
    requires node.valid(depth as nat, shift as nat),
    ensures result <==> node.support(depth as nat).len() > 0,
{
    match &**node {
        Directory::Page { keys, .. } => keys.len() > 0,
        Directory::Branch { children } => {
            let mut index = 0;
            while index < children.len()
                invariant index <= children.len(),
                    **node == (Directory::Branch { children: *children }),
                    0 < depth, children.len() == 32,
                    node.valid(depth as nat, shift as nat),
                    forall|position: int| 0 <= position < index ==> #[trigger] children@[position].is_none(),
                decreases children.len() - index,
            {
                if children[index].is_some() {
                    proof {
                        let supports = child_supports(children@, (depth - 1) as nat);
                        assert(supports[index as int].len() > 0);
                        let witness = supports[index as int][0];
                        lemma_flat_contains(supports, witness);
                        assert(flat(supports).contains(witness));
                    }
                    return true;
                }
                index += 1;
            }
            proof {
                let supports = child_supports(children@, (depth - 1) as nat);
                assert forall|position: int| 0 <= position < supports.len() implies #[trigger] supports[position] == Seq::<i64>::empty() by {
                    assert(children@[position].is_none());
                }
                if flat(supports).len() > 0 {
                    let witness = flat(supports)[0];
                    assert(flat(supports).contains(witness));
                    lemma_flat_contains(supports, witness);
                    let position = choose|position: int| 0 <= position < supports.len() && (#[trigger] supports[position]).contains(witness);
                    assert(supports[position] == Seq::<i64>::empty());
                }
            }
            false
        },
    }
}

pub fn replace<Value>(node: &Arc<Directory<Value>>, key: i64, value: Option<Arc<Value>>, depth: u32, shift: u32) -> (result: Arc<Directory<Value>>)
    requires depth + shift <= 12, node.valid(depth as nat, shift as nat),
    ensures result.valid(depth as nat, shift as nat),
        forall|target: i64| #[trigger] result.at(target, depth as nat, shift as nat) == if target == key {
            match value { Some(entry) => Some(*entry), None => None }
        } else { node.at(target, depth as nat, shift as nat) },
    decreases depth,
{
    match &**node {
        Directory::Page { rows, keys } => {
            let mut work = Work::new();
            let mut index = 0;
            while index < keys.len()
                invariant index <= keys.len(),
                    node.valid(depth as nat, shift as nat), work.valid(),
                    **node == (Directory::Page { rows: *rows, keys: *keys }),
                    forall|target: i64| #[trigger] arc_at(work.rows@, target) == if target != key && keys@.take(index as int).contains(target) { arc_at(rows@, target) } else { None },
                decreases keys.len() - index,
            {
                let current = keys[index];
                let ghost before = work.rows@;
                proof {
                    assert forall|target: i64| #[trigger] arc_at(before, target) == if target != key && keys@.take(index as int).contains(target) { arc_at(rows@, target) } else { None } by {}
                }
                if current != key {
                    if let Some(entry) = rows.get(&current) {
                        work.put(current, Arc::clone(entry));
                    }
                }
                proof {
                    assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(current));
                    assert forall|target: i64| #[trigger] arc_at(work.rows@, target) == if target != key && keys@.take(index as int + 1).contains(target) { arc_at(rows@, target) } else { None } by {
                        assert(arc_at(before, target) == if target != key && keys@.take(index as int).contains(target) { arc_at(rows@, target) } else { None });
                        assert(keys@.take(index as int + 1).contains(target) <==> keys@.take(index as int).contains(target) || target == current);
                    }
                }
                index += 1;
            }
            let ghost before = work.rows@;
            proof {
                assert(keys@.take(index as int) =~= keys@);
                assert forall|target: i64| #[trigger] arc_at(before, target) == if target != key { arc_at(rows@, target) } else { None } by {
                    assert(rows@.contains_key(target) <==> keys@.contains(target));
                }
            }
            if let Some(entry) = value {
                work.put(key, entry);
            }
            proof {
                assert forall|target: i64| #[trigger] arc_at(work.rows@, target) == if target == key { match value { Some(entry) => Some(*entry), None => None } } else { node.at(target, depth as nat, shift as nat) } by {
                    assert(arc_at(before, target) == if target != key { arc_at(rows@, target) } else { None });
                }
            }
            freeze(work, depth, shift)
        },
        Directory::Branch { children } => {
            let target = route(key, shift);
            let mut new_children: Vec<Option<Arc<Directory<Value>>>> = Vec::new();
            let mut index = 0;
            while index < 32
                invariant index <= 32, new_children.len() == index, 0 < depth, depth + shift <= 12,
                    node.valid(depth as nat, shift as nat), children.len() == 32, target < 32,
                    target as int == slot(key, shift),
                    **node == (Directory::Branch { children: *children }),
                    forall|position: int| 0 <= position < children.len() && #[trigger] children@[position].is_some()
                        ==> children@[position].unwrap().valid((depth - 1) as nat, shift as nat + 1),
                    forall|position: int| 0 <= position < new_children.len() && #[trigger] new_children@[position].is_some()
                        ==> new_children@[position].unwrap().valid((depth - 1) as nat, shift as nat + 1)
                            && new_children@[position].unwrap().support((depth - 1) as nat).len() > 0,
                    forall|position: int, current: i64| 0 <= position < new_children.len() && new_children@[position].is_some()
                        && #[trigger] new_children@[position].unwrap().support((depth - 1) as nat).contains(current)
                        ==> slot(current, shift) == position,
                    forall|position: int, current: i64| 0 <= position < new_children.len() ==>
                        #[trigger] child_at(new_children@[position], current, (depth - 1) as nat, shift as nat + 1)
                            == if position == target as int && current == key { match value { Some(entry) => Some(*entry), None => None } }
                            else { child_at(children@[position], current, (depth - 1) as nat, shift as nat + 1) },
                decreases 32 - index,
            {
                if index == target {
                    proof {
                        if children@[index as int].is_some() { assert(children@[index as int].unwrap().valid((depth - 1) as nat, shift as nat + 1)); }
                    }
                    let child = match &children[index] {
                        Some(child) => Arc::clone(child),
                        None => freeze(Work::new(), depth - 1, shift + 1),
                    };
                    proof {
                        assert forall|current: i64| #[trigger] child.at(current, (depth - 1) as nat, shift as nat + 1)
                            == child_at(children@[index as int], current, (depth - 1) as nat, shift as nat + 1) by {}
                    }
                    let entry = match &value { Some(entry) => Some(Arc::clone(entry)), None => None };
                    proof { assert(entry == value); }
                    let updated = replace(&child, key, entry, depth - 1, shift + 1);
                    proof { lemma_directory_support(&*updated, (depth - 1) as nat, shift as nat + 1); }
                    let present = occupied(&updated, depth - 1, shift + 1);
                    let ghost selected = if present { Some(updated) } else { None };
                    proof {
                        assert forall|current: i64| #[trigger] child_at(selected, current, (depth - 1) as nat, shift as nat + 1)
                            == if current == key { match value { Some(entry) => Some(*entry), None => None } }
                            else { child_at(children@[index as int], current, (depth - 1) as nat, shift as nat + 1) } by {
                            assert(updated.at(current, (depth - 1) as nat, shift as nat + 1) == if current == key { match value { Some(entry) => Some(*entry), None => None } } else { child.at(current, (depth - 1) as nat, shift as nat + 1) });
                            assert(child.at(current, (depth - 1) as nat, shift as nat + 1) == child_at(children@[index as int], current, (depth - 1) as nat, shift as nat + 1));
                            if !present { assert(!updated.support((depth - 1) as nat).contains(current)); }
                        }
                        assert forall|current: i64| present && #[trigger] updated.support((depth - 1) as nat).contains(current) implies slot(current, shift) == index as int by {
                            if current != key {
                                assert(child.at(current, (depth - 1) as nat, shift as nat + 1).is_some());
                                assert(children@[index as int].is_some());
                                if children@[index as int].is_some() {
                                    lemma_directory_support(&*children@[index as int].unwrap(), (depth - 1) as nat, shift as nat + 1);
                                    assert(children@[index as int].unwrap().support((depth - 1) as nat).contains(current));
                                    assert(slot(current, shift) == index as int);
                                }
                            }
                        }
                    }
                    new_children.push(if present { Some(updated) } else { None });
                } else {
                    new_children.push(match &children[index] {
                        Some(child) => Some(Arc::clone(child)),
                        None => None,
                    });
                }
                index += 1;
            }
            let result = Arc::new(Directory::Branch { children: new_children });
            proof {
                assert forall|current: i64| #[trigger] result.at(current, depth as nat, shift as nat) == if current == key {
                    match value { Some(entry) => Some(*entry), None => None }
                } else { node.at(current, depth as nat, shift as nat) } by {
                    let position = slot(current, shift);
                    assert(0 <= position < 32);
                    assert(child_at(new_children@[position], current, (depth - 1) as nat, shift as nat + 1)
                        == if position == target as int && current == key { match value { Some(entry) => Some(*entry), None => None } }
                        else { child_at(children@[position], current, (depth - 1) as nat, shift as nat + 1) });
                }
            }
            result
        },
    }
}

pub fn collect<Value>(node: &Arc<Directory<Value>>, depth: u32, keys: &mut Vec<i64>)
    requires depth <= 12,
        exists|shift: nat| #[trigger] node.valid(depth as nat, shift),
    ensures final(keys)@ == old(keys)@ + node.support(depth as nat),
    decreases depth,
{
    let ghost initial = keys@;
    match &**node {
        Directory::Page { keys: page_keys, .. } => {
            let mut index = 0;
            while index < page_keys.len()
                invariant index <= page_keys.len(), initial == old(keys)@,
                    keys@ == initial + page_keys@.take(index as int),
                decreases page_keys.len() - index,
            {
                keys.push(page_keys[index]);
                index += 1;
            }
        },
        Directory::Branch { children } => {
            let ghost shift = choose|shift: nat| #[trigger] node.valid(depth as nat, shift);
            let ghost supports = child_supports(children@, (depth - 1) as nat);
            let mut index = 0;
            while index < children.len()
                invariant index <= children.len(), 0 < depth <= 12, node.valid(depth as nat, shift),
                    initial == old(keys)@, keys@ == initial + flat(supports.take(index as int)),
                    supports == child_supports(children@, (depth - 1) as nat),
                    node.support(depth as nat) == flat(supports),
                    forall|position: int| 0 <= position < children.len() && #[trigger] children@[position].is_some()
                        ==> children@[position].unwrap().valid((depth - 1) as nat, shift + 1),
                decreases children.len() - index,
            {
                let ghost previous = keys@;
                proof {
                    if children[index as int].is_some() {
                        assert(children[index as int].unwrap().valid((depth - 1) as nat, shift + 1));
                        assert(exists|offset: nat| #[trigger] children[index as int].unwrap().valid((depth - 1) as nat, offset));
                    }
                }
                if let Some(child) = &children[index] { collect(child, depth - 1, keys); }
                proof {
                    assert(supports.take(index as int + 1).drop_last() =~= supports.take(index as int));
                    assert(supports.take(index as int + 1).last() == supports[index as int]);
                    assert(flat(supports.take(index as int + 1)) =~= flat(supports.take(index as int)) + supports[index as int]);
                }
                index += 1;
            }
        },
    }
}

impl<Value> Pages<Value> {
    pub closed spec fn at(self, key: i64) -> Option<Value> {
        self.root.at(key, self.depth as nat, 0)
    }

    pub closed spec fn valid(self) -> bool {
        self.depth <= 12 && self.root.valid(self.depth as nat, 0)
    }

    pub fn from_work(work: Work<Value>) -> (result: Self)
        requires work.valid(),
        ensures result.valid(),
            forall|key: i64| #[trigger] result.at(key) == if work.rows@.contains_key(key) { Some(work.rows@[key]) } else { None },
    {
        let mut pending = work;
        let mut shared = Work::new();
        let mut index = 0;
        while index < pending.keys.len()
            invariant index <= pending.keys.len(),
                pending.valid(), shared.valid(), pending.keys@ == work.keys@,
                forall|key: i64| #[trigger] value_at(pending.rows@, key) == if work.keys@.take(index as int).contains(key) { None } else { value_at(work.rows@, key) },
                forall|key: i64| #[trigger] arc_at(shared.rows@, key) == if work.keys@.take(index as int).contains(key) { value_at(work.rows@, key) } else { None },
            decreases pending.keys.len() - index,
        {
            let key = pending.keys[index];
            let ghost prior_pending = pending.rows@;
            let ghost prior_shared = shared.rows@;
            proof {
                verus_prooflib::lemma_prefix_fresh(work.keys@, index as int);
                assert forall|target: i64| #[trigger] value_at(prior_pending, target) == if work.keys@.take(index as int).contains(target) { None } else { value_at(work.rows@, target) } by {}
                assert forall|target: i64| #[trigger] arc_at(prior_shared, target) == if work.keys@.take(index as int).contains(target) { value_at(work.rows@, target) } else { None } by {}
            }
            let entry = pending.take(key);
            proof {
                assert(pending.rows@ == prior_pending.remove(key));
                assert(entry == value_at(prior_pending, key));
                assert(value_at(prior_pending, key) == value_at(work.rows@, key));
            }
            if let Some(value) = entry { shared.put(key, Arc::new(value)); }
            proof {
                assert(work.keys@.take(index as int + 1) =~= work.keys@.take(index as int).push(key));
                assert forall|target: i64| #[trigger] value_at(pending.rows@, target) == if work.keys@.take(index as int + 1).contains(target) { None } else { value_at(work.rows@, target) } by {
                    assert(value_at(prior_pending, target) == if work.keys@.take(index as int).contains(target) { None } else { value_at(work.rows@, target) });
                    assert(value_at(pending.rows@, target) == if target == key { None } else { value_at(prior_pending, target) });
                    assert(work.keys@.take(index as int + 1).contains(target) <==> work.keys@.take(index as int).contains(target) || target == key);
                }
                assert forall|target: i64| #[trigger] arc_at(shared.rows@, target) == if work.keys@.take(index as int + 1).contains(target) { value_at(work.rows@, target) } else { None } by {
                    assert(arc_at(prior_shared, target) == if work.keys@.take(index as int).contains(target) { value_at(work.rows@, target) } else { None });
                    if target == key {
                        assert(arc_at(shared.rows@, target) == value_at(work.rows@, target));
                    } else { assert(arc_at(shared.rows@, target) == arc_at(prior_shared, target)); }
                    assert(work.keys@.take(index as int + 1).contains(target) <==> work.keys@.take(index as int).contains(target) || target == key);
                }
            }
            index += 1;
        }
        Self { root: freeze(shared, 12, 0), depth: 12 }
    }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    {
        Self { root: Arc::clone(&self.root), depth: self.depth }
    }

    pub fn get(&self, key: i64) -> (result: Option<Arc<Value>>)
        requires self.valid(),
        ensures self.at(key) == match result { Some(value) => Some(*value), None => None },
    {
        lookup(&self.root, key, self.depth, 0)
    }

    pub fn put(&mut self, key: i64, value: Option<Value>)
        requires old(self).valid(),
        ensures final(self).valid(),
            forall|target: i64| #[trigger] final(self).at(target) == if target == key { value } else { old(self).at(target) },
    {
        let entry = match value { Some(value) => Some(Arc::new(value)), None => None };
        self.root = replace(&self.root, key, entry, self.depth, 0);
    }

    pub fn keys(&self) -> (result: Vec<i64>)
        requires self.valid(),
        ensures result@.no_duplicates(),
            forall|key: i64| #[trigger] self.at(key).is_some() <==> result@.contains(key),
    {
        proof { lemma_directory_support(&*self.root, self.depth as nat, 0); }
        let mut keys = Vec::new();
        collect(&self.root, self.depth, &mut keys);
        keys
    }
}

}
