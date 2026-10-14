use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::bytes::*;
use verus_common::rel::*;
use verus_common::sums::*;
use crate::values::*;
use crate::paged::*;
use crate::semantics::*;
use crate::seqfacts::*;

verus! {

pub struct Bucket {
    pub groups: Vec<Group>,
}

impl Bucket {
    pub open spec fn value(self, flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
        aggregate(self.groups@, flag, status, component)
    }
    pub open spec fn wf(self, hash: u64, positive: bool) -> bool {
        &&& forall|index: int| 0 <= index < self.groups.len() ==>
            group_hash((#[trigger] self.groups@[index]).key().0, self.groups@[index].key().1) == hash
            && (positive ==> self.groups@[index].totals.count > 0)
        &&& forall|first: int, second: int| 0 <= first < second < self.groups.len() ==>
            (#[trigger] self.groups@[first]).key() != (#[trigger] self.groups@[second]).key()
    }

    pub fn copy(&self) -> (result: Self)
        ensures result.groups@ == self.groups@,
    {
        let mut groups = Vec::new();
        let mut index = 0usize;
        while index < self.groups.len()
            invariant index <= self.groups.len(), groups@ == self.groups@.take(index as int),
            decreases self.groups.len() - index,
        {
            groups.push(self.groups[index].share());
            index += 1;
        }
        Self { groups }
    }

    pub fn add(&mut self, flag: &Vec<u8>, status: &Vec<u8>, totals: &Totals, positive: bool) -> (result: bool)
        requires old(self).wf(group_hash(flag@, status@), positive),
        ensures result ==> final(self).wf(group_hash(flag@, status@), positive),
            result ==> forall|other_flag: Seq<u8>, other_status: Seq<u8>, component: int|
                #[trigger] final(self).value(other_flag, other_status, component)
                    == old(self).value(other_flag, other_status, component)
                        + if (other_flag, other_status) == (flag@, status@) { totals.at(component) } else { 0 },
    {
        let ghost initial = self.groups@;
        let mut index = 0usize;
        while index < self.groups.len()
            invariant index <= self.groups.len(), self.groups@ == initial, initial == old(self).groups@,
                self.wf(group_hash(flag@, status@), positive),
                forall|position: int| 0 <= position < index ==> (#[trigger] self.groups@[position]).key() != (flag@, status@),
            decreases self.groups.len() - index,
        {
            let group = &self.groups[index];
            if bytes_eq_exec(&group.flag, flag) && bytes_eq_exec(&group.status, status) {
                let next = match group.totals.add(totals) { Some(value) => value, None => return false };
                if positive && next.count <= 0 {
                    if next.count < 0 || !next.is_zero() { return false; }
                    self.groups.remove(index);
                    proof {
                        assert forall|other_flag: Seq<u8>, other_status: Seq<u8>, component: int|
                            #[trigger] self.value(other_flag, other_status, component)
                                == aggregate(initial, other_flag, other_status, component)
                                    + if (other_flag, other_status) == (flag@, status@) { totals.at(component) } else { 0 } by {
                            let f = group_weight(other_flag, other_status, component);
                            lemma_sum_remove(initial, index as int, f);
                            assert(next.at(component) == 0);
                            assert(next.at(component) == initial[index as int].totals.at(component) + totals.at(component));
                        }
                    }
                } else {
                    let replacement = Group { flag: group.flag.clone(), status: group.status.clone(), totals: next };
                    self.groups.set(index, replacement);
                    proof {
                        assert forall|other_flag: Seq<u8>, other_status: Seq<u8>, component: int|
                            #[trigger] self.value(other_flag, other_status, component)
                                == aggregate(initial, other_flag, other_status, component)
                                    + if (other_flag, other_status) == (flag@, status@) { totals.at(component) } else { 0 } by {
                            let f = group_weight(other_flag, other_status, component);
                            lemma_sum_update(initial, index as int, self.groups@[index as int], f);
                            assert(self.groups@[index as int].totals.at(component) == initial[index as int].totals.at(component) + totals.at(component));
                        }
                    }
                }
                return true;
            }
            index += 1;
        }
        if positive && totals.count <= 0 { return totals.is_zero(); }
        self.groups.push(Group { flag: Arc::new(copy_bytes(flag)), status: Arc::new(copy_bytes(status)), totals: totals.copy() });
        proof {
            assert forall|other_flag: Seq<u8>, other_status: Seq<u8>, component: int|
                #[trigger] self.value(other_flag, other_status, component)
                    == aggregate(initial, other_flag, other_status, component)
                        + if (other_flag, other_status) == (flag@, status@) { totals.at(component) } else { 0 } by {
                lemma_sum_push(initial, self.groups@.last(), group_weight(other_flag, other_status, component));
            }
        }
        true
    }
}

pub open spec fn flat_entries(keys: Seq<u64>, buckets: Map<u64, Bucket>) -> Seq<Group> {
    flat(bucket_parts(keys, buckets))
}

pub open spec fn bucket_parts(keys: Seq<u64>, buckets: Map<u64, Bucket>) -> Seq<Seq<Group>> {
    keys.map_values(|key: u64| buckets[key].groups@)
}

pub proof fn lemma_flat_wf_key(keys: Seq<u64>, buckets: Map<u64, Bucket>, key: u64)
    requires flat_wf(keys, buckets), buckets.contains_key(key),
    ensures buckets[key].wf(key, false),
{
    let index = choose|index: int| 0 <= index < keys.len() && #[trigger] keys[index] == key;
}

pub open spec fn group_keys_unique(groups: Seq<Group>) -> bool {
    forall|first: int, second: int| 0 <= first < second < groups.len() ==>
        (#[trigger] groups[first]).key() != (#[trigger] groups[second]).key()
}

pub proof fn lemma_flat_unique(keys: Seq<u64>, buckets: Map<u64, Bucket>)
    requires flat_wf(keys, buckets),
    ensures group_keys_unique(flat_entries(keys, buckets)),
{
    let parts = bucket_parts(keys, buckets);
    assert forall|first: int, second: int| 0 <= first < second < flat(parts).len() implies
        (#[trigger] flat(parts)[first]).key() != (#[trigger] flat(parts)[second]).key() by {
        let left = lemma_flat_index(parts, first);
        let right = lemma_flat_index(parts, second);
        let left_bucket = buckets[keys[left.0]];
        let right_bucket = buckets[keys[right.0]];
        if flat(parts)[first].key() == flat(parts)[second].key() {
            assert(keys[left.0] == group_hash(flat(parts)[first].key().0, flat(parts)[first].key().1));
            assert(keys[right.0] == group_hash(flat(parts)[second].key().0, flat(parts)[second].key().1));
            if left.0 < right.0 { assert(keys[left.0] != keys[right.0]); }
            if right.0 < left.0 { assert(keys[right.0] != keys[left.0]); }
            assert(left.0 == right.0);
            if left.1 < right.1 { assert(left_bucket.groups@[left.1].key() != left_bucket.groups@[right.1].key()); }
            if right.1 < left.1 { assert(left_bucket.groups@[right.1].key() != left_bucket.groups@[left.1].key()); }
            assert(left.1 == right.1);
        }
    }
}

pub open spec fn flat_wf(keys: Seq<u64>, buckets: Map<u64, Bucket>) -> bool {
    &&& forall|key: u64| #[trigger] buckets.contains_key(key) <==> keys.contains(key)
    &&& forall|index: int| 0 <= index < keys.len() ==> buckets[#[trigger] keys[index]].wf(keys[index], false)
    &&& forall|first: int, second: int| 0 <= first < second < keys.len() ==> #[trigger] keys[first] != #[trigger] keys[second]
}

pub proof fn lemma_flat_lookup(keys: Seq<u64>, buckets: Map<u64, Bucket>, flag: Seq<u8>, status: Seq<u8>, component: int)
    requires flat_wf(keys, buckets),
    ensures aggregate(flat_entries(keys, buckets), flag, status, component)
        == if buckets.contains_key(group_hash(flag, status)) { buckets[group_hash(flag, status)].value(flag, status, component) } else { 0 },
{
    let hash = group_hash(flag, status);
    let f = group_weight(flag, status, component);
    let expand = |key: u64| buckets[key].groups@;
    let fold = |key: u64| sum(expand(key), f);
    lemma_sum_flat_map(keys, expand, f);
    assert forall|index: int| 0 <= index < keys.len() && keys[index] != hash implies #[trigger] fold(keys[index]) == 0 by {
        let bucket = buckets[keys[index]];
        assert forall|position: int| 0 <= position < bucket.groups.len() implies #[trigger] f(bucket.groups@[position]) == 0 by {
            assert(group_hash(bucket.groups@[position].key().0, bucket.groups@[position].key().1) == keys[index]);
        }
        lemma_sum_zero(bucket.groups@, f);
    }
    if buckets.contains_key(hash) {
        let index = choose|index: int| 0 <= index < keys.len() && #[trigger] keys[index] == hash;
        assert forall|other: int| 0 <= other < keys.len() && other != index implies #[trigger] fold(keys[other]) == 0 by {
            if other < index { assert(keys[other] != keys[index]); } else { assert(keys[index] != keys[other]); }
        }
        lemma_sum_single_match(keys, fold, index);
    } else { lemma_sum_zero(keys, fold); }
}

pub struct FlatGroups {
    buckets: HashMap<u64, Bucket>,
    keys: Vec<u64>,
}

impl FlatGroups {
    pub closed spec fn keys(self) -> Seq<u64> { self.keys@ }
    pub closed spec fn buckets(self) -> Map<u64, Bucket> { self.buckets@ }
    pub closed spec fn entries(self) -> Seq<Group> {
        flat_entries(self.keys@, self.buckets@)
    }
    pub closed spec fn value(self, flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
        aggregate(self.entries(), flag, status, component)
    }
    pub closed spec fn wf(self) -> bool {
        flat_wf(self.keys@, self.buckets@)
    }

    pub proof fn lemma_value_entries(&self)
        ensures forall|flag: Seq<u8>, status: Seq<u8>, component: int|
            #[trigger] self.value(flag, status, component) == aggregate(self.entries(), flag, status, component),
    {
    }

    pub fn new() -> (result: Self)
        ensures result.wf(), result.entries().len() == 0,
            result.keys().len() == 0, result.buckets() == Map::empty(),
            forall|flag: Seq<u8>, status: Seq<u8>, component: int| #[trigger] result.value(flag, status, component) == 0,
    { Self { buckets: HashMap::new(), keys: Vec::new() } }

    pub fn add(&mut self, flag: &Vec<u8>, status: &Vec<u8>, totals: &Totals) -> (result: bool)
        requires old(self).wf(),
        ensures result ==> final(self).wf(),
            result ==> forall|other_flag: Seq<u8>, other_status: Seq<u8>, component: int|
                #[trigger] final(self).value(other_flag, other_status, component)
                == old(self).value(other_flag, other_status, component)
                    + if (other_flag, other_status) == (flag@, status@) { totals.at(component) } else { 0 },
    {
        let ghost old_buckets = self.buckets@;
        let ghost old_keys = self.keys@;
        let hash = hash_group(flag, status);
        proof { if old_buckets.contains_key(hash) { lemma_flat_wf_key(old_keys, old_buckets, hash); } }
        let mut bucket = match self.buckets.remove(&hash) {
            Some(bucket) => bucket,
            None => { self.keys.push(hash); Bucket { groups: Vec::new() } },
        };
        if !bucket.add(flag, status, totals, false) { return false; }
        self.buckets.insert(hash, bucket);
        proof {
            lemma_push_contains(old_keys, hash);
            assert forall|position: int| 0 <= position < self.keys.len() implies self.buckets@[#[trigger] self.keys@[position]].wf(self.keys@[position], false) by {
                if self.keys@[position] != hash {
                    assert(old_buckets.contains_key(self.keys@[position]));
                    lemma_flat_wf_key(old_keys, old_buckets, self.keys@[position]);
                }
            }
            assert(self.wf());
            assert forall|other_flag: Seq<u8>, other_status: Seq<u8>, component: int|
                #[trigger] self.value(other_flag, other_status, component)
                    == aggregate(flat_entries(old_keys, old_buckets), other_flag, other_status, component)
                        + if (other_flag, other_status) == (flag@, status@) { totals.at(component) } else { 0 } by {
                lemma_flat_lookup(old_keys, old_buckets, other_flag, other_status, component);
                lemma_flat_lookup(self.keys@, self.buckets@, other_flag, other_status, component);
            }
        }
        true
    }

    pub fn collect(&self) -> (result: Vec<Group>)
        requires self.wf(),
        ensures result@ == self.entries(),
            group_keys_unique(result@),
            forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                #[trigger] aggregate(result@, flag, status, component) == self.value(flag, status, component),
    {
        let mut result = Vec::new();
        let ghost parts = bucket_parts(self.keys@, self.buckets@);
        let mut index = 0usize;
        while index < self.keys.len()
            invariant index <= self.keys.len(), self.wf(),
                parts == bucket_parts(self.keys@, self.buckets@), parts.len() == self.keys.len(),
                result@ == flat(parts.take(index as int)),
            decreases self.keys.len() - index,
        {
            if let Some(bucket) = self.buckets.get(&self.keys[index]) {
                let ghost previous = result@;
                let mut group_index = 0usize;
                while group_index < bucket.groups.len()
                    invariant group_index <= bucket.groups.len(), result@ == previous + bucket.groups@.take(group_index as int),
                        previous == flat(parts.take(index as int)),
                        bucket.groups@ == parts[index as int],
                        index < parts.len(),
                    decreases bucket.groups.len() - group_index,
                {
                    result.push(bucket.groups[group_index].share());
                    group_index += 1;
                }
            }
            proof { lemma_flat_take_step(parts, index as int); }
            index += 1;
        }
        proof {
            assert(parts.take(index as int) =~= parts);
            assert(result@ == self.entries());
            lemma_flat_unique(self.keys@, self.buckets@);
        }
        result
    }

    pub fn publish(&self) -> (result: Option<Groups>)
        requires self.wf(),
        ensures result matches Some(groups) ==> groups.wf()
            && forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                #[trigger] groups.value(flag, status, component) == self.value(flag, status, component),
    {
        Groups::new().updated(self)
    }
}

pub struct Groups {
    buckets: Paged<Bucket>,
}

closed spec fn group_parts(buckets: Paged<Bucket>) -> Seq<Seq<Group>> {
    buckets.values().map_values(|bucket: Bucket| bucket.groups@)
}

closed spec fn bucket_weight(flag: Seq<u8>, status: Seq<u8>, component: int) -> spec_fn(Bucket) -> int {
    |bucket: Bucket| bucket.value(flag, status, component)
}

closed spec fn bucket_groups(value: Option<Bucket>) -> Seq<Group> {
    match value { Some(bucket) => bucket.groups@, None => Seq::empty() }
}

closed spec fn persistent_value(buckets: Paged<Bucket>, flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
    aggregate(flat(group_parts(buckets)), flag, status, component)
}

proof fn lemma_persistent_fold(buckets: Paged<Bucket>, flag: Seq<u8>, status: Seq<u8>, component: int)
    ensures persistent_value(buckets, flag, status, component) == sum(buckets.values(), bucket_weight(flag, status, component)),
{
    let expand = |bucket: Bucket| bucket.groups@;
    lemma_sum_flat_map(buckets.values(), expand, group_weight(flag, status, component));
    lemma_sum_ext_all(buckets.values(), |bucket: Bucket| sum(expand(bucket), group_weight(flag, status, component)), bucket_weight(flag, status, component));
}

impl Groups {
    pub closed spec fn entries(self) -> Seq<Group> {
        flat(group_parts(self.buckets))
    }

    pub closed spec fn value(self, flag: Seq<u8>, status: Seq<u8>, component: int) -> int {
        persistent_value(self.buckets, flag, status, component)
    }

    pub closed spec fn wf(self) -> bool {
        &&& self.buckets.wf()
        &&& forall|hash: u64| #[trigger] self.buckets.at(hash) matches Some(bucket) ==>
            bucket.groups.len() > 0 && bucket.wf(hash, true)
    }

    pub fn new() -> (result: Self)
        ensures result.wf(), result.entries().len() == 0,
    { Self { buckets: Paged::new() } }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    { Self { buckets: self.buckets.share() } }

    pub fn updated(&self, delta: &FlatGroups) -> (result: Option<Self>)
        requires self.wf(), delta.wf(),
        ensures result matches Some(groups) ==> groups.wf()
            && forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                #[trigger] groups.value(flag, status, component) == self.value(flag, status, component) + delta.value(flag, status, component),
    {
        let mut buckets = self.buckets.share();
        let mut index = 0usize;
        while index < delta.keys.len()
            invariant index <= delta.keys.len(), self.wf(), delta.wf(), buckets.wf(),
                forall|hash: u64| #[trigger] buckets.at(hash) matches Some(bucket) ==> bucket.groups.len() > 0 && bucket.wf(hash, true),
                forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                    #[trigger] persistent_value(buckets, flag, status, component) == self.value(flag, status, component)
                        + aggregate(flat_entries(delta.keys@.take(index as int), delta.buckets@), flag, status, component),
            decreases delta.keys.len() - index,
        {
            let hash = delta.keys[index];
            let ghost previous_buckets = buckets;
            proof { lemma_flat_wf_key(delta.keys@, delta.buckets@, hash); }
            let mut bucket = match buckets.get(hash) { Some(bucket) => bucket.copy(), None => Bucket { groups: Vec::new() } };
            let ghost original = bucket.groups@;
            let changes = match delta.buckets.get(&hash) { Some(changes) => changes, None => return None };
            {
                let mut group_index = 0usize;
                while group_index < changes.groups.len()
                    invariant group_index <= changes.groups.len(), bucket.wf(hash, true), changes.wf(hash, false),
                        self.wf(), delta.wf(), index < delta.keys.len(), hash == delta.keys@[index as int],
                        buckets == previous_buckets,
                        forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                            #[trigger] persistent_value(previous_buckets, flag, status, component) == self.value(flag, status, component)
                                + aggregate(flat_entries(delta.keys@.take(index as int), delta.buckets@), flag, status, component),
                        original == bucket_groups(previous_buckets.at(hash)),
                        changes.groups@ == delta.buckets@[hash].groups@,
                        forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                            #[trigger] bucket.value(flag, status, component) == aggregate(original, flag, status, component)
                                + aggregate(changes.groups@.take(group_index as int), flag, status, component),
                    decreases changes.groups.len() - group_index,
                {
                    let group = &changes.groups[group_index];
                    if !bucket.add(&group.flag, &group.status, &group.totals, true) { return None; }
                    proof {
                        assert forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                            #[trigger] bucket.value(flag, status, component) == aggregate(original, flag, status, component)
                                + aggregate(changes.groups@.take(group_index as int + 1), flag, status, component) by {
                            lemma_sum_take(changes.groups@, group_index as int, group_weight(flag, status, component));
                        }
                    }
                    group_index += 1;
                }
                proof {
                    assert forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                        #[trigger] bucket.value(flag, status, component) == option_weight(previous_buckets.at(hash), bucket_weight(flag, status, component))
                            + delta.buckets@[hash].value(flag, status, component) by {
                        assert(changes.groups@.take(group_index as int) =~= changes.groups@);
                    }
                }
            }
            let ghost next_bucket = bucket;
            let replacement = if bucket.groups.len() == 0 { None } else { Some(Arc::new(bucket)) };
            let ghost replacement_model = arc_value(replacement);
            proof {
                assert forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                    #[trigger] option_weight(replacement_model, bucket_weight(flag, status, component))
                        == option_weight(previous_buckets.at(hash), bucket_weight(flag, status, component))
                            + delta.buckets@[hash].value(flag, status, component) by {
                    assert(option_weight(replacement_model, bucket_weight(flag, status, component)) == next_bucket.value(flag, status, component));
                }
            }
            buckets = buckets.replace(hash, replacement);
            proof {
                assert forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                    #[trigger] persistent_value(buckets, flag, status, component) == self.value(flag, status, component)
                        + aggregate(flat_entries(delta.keys@.take(index as int + 1), delta.buckets@), flag, status, component) by {
                    let f = group_weight(flag, status, component);
                    let expand = |key: u64| delta.buckets@[key].groups@;
                    let fold = |key: u64| sum(expand(key), f);
                    lemma_persistent_fold(previous_buckets, flag, status, component);
                    lemma_persistent_fold(buckets, flag, status, component);
                    lemma_sum_flat_map(delta.keys@.take(index as int), expand, f);
                    lemma_sum_flat_map(delta.keys@.take(index as int + 1), expand, f);
                    lemma_sum_take(delta.keys@, index as int, fold);
                    assert(sum(buckets.values(), bucket_weight(flag, status, component))
                        == sum(previous_buckets.values(), bucket_weight(flag, status, component))
                            + option_weight(replacement_model, bucket_weight(flag, status, component))
                            - option_weight(previous_buckets.at(hash), bucket_weight(flag, status, component)));
                }
            }
            index += 1;
        }
        proof { assert(delta.keys@.take(index as int) =~= delta.keys@); }
        Some(Self { buckets })
    }

    pub fn collect(&self) -> (result: Vec<Group>)
        requires self.wf(),
        ensures result@ == self.entries(),
            forall|flag: Seq<u8>, status: Seq<u8>, component: int|
                #[trigger] aggregate(result@, flag, status, component) == self.value(flag, status, component),
    {
        let buckets = self.buckets.collect();
        let ghost parts = group_parts(self.buckets);
        let mut result = Vec::new();
        let mut index = 0usize;
        while index < buckets.len()
            invariant index <= buckets.len(),
                buckets@.map_values(|bucket: Arc<Bucket>| *bucket) == self.buckets.values(),
                parts == group_parts(self.buckets), parts.len() == buckets.len(),
                result@ == flat(parts.take(index as int)),
            decreases buckets.len() - index,
        {
            let bucket = &buckets[index];
            let ghost previous = result@;
            let mut group_index = 0usize;
            while group_index < bucket.groups.len()
                invariant group_index <= bucket.groups.len(), index < parts.len(), bucket.groups@ == parts[index as int],
                    result@ == previous + bucket.groups@.take(group_index as int), previous == flat(parts.take(index as int)),
                decreases bucket.groups.len() - group_index,
            {
                result.push(bucket.groups[group_index].share());
                group_index += 1;
            }
            proof { lemma_flat_take_step(parts, index as int); }
            index += 1;
        }
        proof { assert(parts.take(index as int) =~= parts); }
        result
    }

    pub fn is_empty(&self) -> (result: bool)
        requires self.wf(),
        ensures result == (self.entries().len() == 0),
            result ==> forall|flag: Seq<u8>, status: Seq<u8>, component: int| #[trigger] self.value(flag, status, component) == 0,
    {
        proof { self.buckets.lemma_all_values(|bucket: Bucket| bucket.groups.len() > 0); }
        let empty = self.buckets.is_empty();
        proof {
            let parts = group_parts(self.buckets);
            if !empty {
                assert(parts[0].len() > 0);
                let group = parts[0][0];
                assert(parts[0].contains(group));
                lemma_flat_contains(parts, group);
            }
        }
        empty
    }
}

}
