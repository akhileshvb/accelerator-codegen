use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct Run<Value> {
    data: HashMap<i64, Option<Arc<Value>>>,
    keys: Vec<i64>,
}

pub struct Index<Value> {
    runs: Vec<Arc<Run<Value>>>,
    size: usize,
}

pub struct Work<Value> {
    pub data: HashMap<i64, Value>,
    pub keys: Vec<i64>,
    pub listed: HashMap<i64, bool>,
}

pub open spec fn work_directory<Value>(work: Work<Value>) -> bool {
    &&& work.keys@.no_duplicates()
    &&& work.keys@.to_set() == work.listed@.dom()
    &&& work.data@.dom().subset_of(work.listed@.dom())
}

pub open spec fn work_complete<Value>(work: Work<Value>) -> bool {
    work_directory(work) && work.data@.dom() == work.keys@.to_set()
}

impl<Value> Work<Value> {
    pub fn new() -> (result: Self)
        ensures result.data@ == Map::<i64, Value>::empty(), result.keys@ == Seq::<i64>::empty(),
            result.listed@ == Map::<i64, bool>::empty(),
            work_complete(result),
    {
        proof { assert(Seq::<i64>::empty().to_set() =~= Set::<i64>::empty()); }
        Work { data: HashMap::new(), keys: Vec::new(), listed: HashMap::new() }
    }

    pub fn take(&mut self, key: i64) -> (result: Option<Value>)
        ensures
            final(self).keys@ == old(self).keys@,
            final(self).listed@ == old(self).listed@,
            final(self).data@ == old(self).data@.remove(key),
            result == if old(self).data@.contains_key(key) { Some(old(self).data@[key]) } else { None },
            work_directory(*old(self)) ==> work_directory(*final(self)),
    {
        self.data.remove(&key)
    }

    pub fn put(&mut self, key: i64, value: Value)
        ensures
            final(self).data@ == old(self).data@.insert(key, value),
            final(self).keys@ == if old(self).listed@.contains_key(key) { old(self).keys@ } else { old(self).keys@.push(key) },
            final(self).listed@ == if old(self).listed@.contains_key(key) { old(self).listed@ } else { old(self).listed@.insert(key, true) },
            work_directory(*old(self)) ==> work_directory(*final(self)),
            work_complete(*old(self)) ==> work_complete(*final(self)),
    {
        let ghost original = *self;
        if !self.listed.contains_key(&key) {
            proof {
                if work_directory(original) { assert(!self.keys@.contains(key)); }
            }
            self.keys.push(key);
            self.listed.insert(key, true);
        }
        self.data.insert(key, value);
        proof {
            if work_directory(original) {
                assert(self.keys@.to_set() =~= self.listed@.dom());
                assert(self.data@.dom().subset_of(self.listed@.dom())) by {
                    assert forall|entry: i64| (#[trigger] self.data@.dom().contains(entry)) implies
                        self.listed@.dom().contains(entry) by {}
                }
            }
            if work_complete(original) {
                assert(self.keys@.to_set() =~= original.keys@.to_set().insert(key));
                assert(self.data@.dom() =~= original.data@.dom().insert(key));
                assert(work_complete(*self));
            }
        }
    }
}

pub open spec fn lookup<Value>(runs: Seq<Arc<Run<Value>>>, key: i64) -> Option<Arc<Value>>
    decreases runs.len(),
{
    if runs.len() == 0 { None }
    else if runs[0].model().contains_key(key) { runs[0].model()[key] }
    else { lookup(runs.drop_first(), key) }
}

pub open spec fn support<Value>(runs: Seq<Arc<Run<Value>>>) -> Set<i64>
    decreases runs.len(),
{
    if runs.len() == 0 { Set::empty() }
    else {
        let domain = runs[0].model().dom();
        support(runs.drop_first()).difference(domain).union(
            domain.filter(|key: i64| runs[0].model()[key].is_some()))
    }
}

pub closed spec fn run_ok<Value>(run: Run<Value>) -> bool {
    &&& run.keys@.no_duplicates()
    &&& run.keys@.to_set() == run.data@.dom()
}

pub open spec fn overlay_at<Value>(base: Index<Value>, delta: Run<Value>, key: i64) -> Option<Arc<Value>> {
    if delta.model().contains_key(key) { delta.model()[key] } else { base.at(key) }
}

pub open spec fn overlay_lookup<Value>(data: Map<i64, Option<Arc<Value>>>, runs: Seq<Arc<Run<Value>>>, key: i64) -> Option<Arc<Value>> {
    if data.contains_key(key) { data[key] } else { lookup(runs, key) }
}

pub open spec fn overlay_support<Value>(data: Map<i64, Option<Arc<Value>>>, runs: Seq<Arc<Run<Value>>>) -> Set<i64> {
    support(runs).difference(data.dom()).union(data.dom().filter(|key: i64| data[key].is_some()))
}

pub proof fn lemma_support_lookup<Value>(runs: Seq<Arc<Run<Value>>>, key: i64)
    ensures support(runs).contains(key) <==> lookup(runs, key).is_some(),
    decreases runs.len(),
{
    if runs.len() > 0 { lemma_support_lookup(runs.drop_first(), key); }
}

pub proof fn lemma_overlay_support<Value>(data: Map<i64, Option<Arc<Value>>>, runs: Seq<Arc<Run<Value>>>, key: i64)
    ensures overlay_support(data, runs).contains(key) <==> overlay_lookup(data, runs, key).is_some(),
{
    lemma_support_lookup(runs, key);
}

pub proof fn lemma_lookup_skip<Value>(runs: Seq<Arc<Run<Value>>>, key: i64, position: int)
    requires 0 <= position <= runs.len(),
        forall|entry: int| 0 <= entry < position ==> !(#[trigger] runs[entry]).model().contains_key(key),
    ensures lookup(runs, key) == lookup(runs.subrange(position, runs.len() as int), key),
    decreases position,
{
    if position > 0 {
        let tail = runs.drop_first();
        assert forall|entry: int| 0 <= entry < position - 1 implies !(#[trigger] tail[entry]).model().contains_key(key) by {
            assert(tail[entry] == runs[entry + 1]);
        }
        lemma_lookup_skip(tail, key, position - 1);
        assert(tail.subrange(position - 1, tail.len() as int) =~= runs.subrange(position, runs.len() as int));
    } else { assert(runs.subrange(0, runs.len() as int) =~= runs); }
}

pub proof fn lemma_lookup_witness<Value>(runs: Seq<Arc<Run<Value>>>, key: i64)
    ensures lookup(runs, key).is_some() ==>
        exists|entry: int| 0 <= entry < runs.len() && (#[trigger] runs[entry]).model().contains_key(key),
    decreases runs.len(),
{
    if runs.len() > 0 && !runs[0].model().contains_key(key) {
        lemma_lookup_witness(runs.drop_first(), key);
        if lookup(runs, key).is_some() {
            let tail = runs.drop_first();
            let entry = choose|entry: int| 0 <= entry < tail.len() && (#[trigger] tail[entry]).model().contains_key(key);
            assert(runs[entry + 1] == tail[entry]);
        }
    }
}

fn put_run_parts<Value>(data: &mut HashMap<i64, Option<Arc<Value>>>, keys: &mut Vec<i64>, key: i64, value: Option<Arc<Value>>)
    requires old(keys)@.no_duplicates(), old(keys)@.to_set() == old(data)@.dom(),
    ensures
        final(data)@ == old(data)@.insert(key, value),
        final(keys)@ == if old(data)@.contains_key(key) { old(keys)@ } else { old(keys)@.push(key) },
        final(keys)@.no_duplicates(), final(keys)@.to_set() == final(data)@.dom(),
{
    if !data.contains_key(&key) { keys.push(key); }
    data.insert(key, value);
}

impl<Value> Run<Value> {
    pub closed spec fn model(&self) -> Map<i64, Option<Arc<Value>>> { self.data@ }
    pub closed spec fn key_seq(&self) -> Seq<i64> { self.keys@ }
    pub fn new() -> (result: Self)
        ensures result.model() == Map::<i64, Option<Arc<Value>>>::empty(), result.key_seq() == Seq::<i64>::empty(), run_ok(result),
    {
        proof { assert(Seq::<i64>::empty().to_set() =~= Set::<i64>::empty()); }
        Run { data: HashMap::new(), keys: Vec::new() }
    }

    pub fn put(&mut self, key: i64, value: Option<Arc<Value>>)
        requires run_ok(*old(self)),
        ensures
            run_ok(*final(self)),
            final(self).model() == old(self).model().insert(key, value),
            final(self).key_seq() == if old(self).model().contains_key(key) { old(self).key_seq() } else { old(self).key_seq().push(key) },
    {
        put_run_parts(&mut self.data, &mut self.keys, key, value);
    }

    pub fn get(&self, key: i64) -> (result: Option<Arc<Value>>)
        ensures result == if self.model().contains_key(key) { self.model()[key] } else { None },
    {
        match self.data.get(&key) {
            Some(Some(value)) => Some(Arc::clone(value)),
            _ => None,
        }
    }
}

impl<Value> Index<Value> {
    pub closed spec fn at(&self, key: i64) -> Option<Arc<Value>> { lookup(self.runs@, key) }
    pub closed spec fn size(self) -> nat { self.size as nat }
    pub closed spec fn domain(self) -> Set<i64> { support(self.runs@) }

    pub proof fn lemma_empty(self)
        requires self.valid(), self.size() == 0,
        ensures forall|key: i64| #[trigger] self.at(key) == None,
    {
    }

    pub proof fn lemma_nonempty(&self)
        requires self.valid(), self.size() > 0,
        ensures exists|key: i64| (#[trigger] self.at(key)).is_some(),
    {
        if !(exists|key: i64| #[trigger] support(self.runs@).contains(key)) {
            assert(support(self.runs@) =~= Set::<i64>::empty());
            assert(support(self.runs@).len() == 0);
        }
        let key = choose|key: i64| #[trigger] support(self.runs@).contains(key);
        assert(self.at(key).is_some());
    }

    pub fn len(&self) -> (result: usize)
        ensures result == self.size(),
    { self.size }

    pub closed spec fn valid(self) -> bool {
        &&& forall|position: int| 0 <= position < self.runs.len() ==>
            run_ok(*(#[trigger] self.runs@[position]))
        &&& support(self.runs@).len() == self.size
        &&& (self.runs.len() == 0 <==> self.size == 0)
        &&& forall|key: i64| (#[trigger] self.at(key)).is_some() <==> support(self.runs@).contains(key)
    }

    #[verifier::type_invariant]
    pub closed spec fn invariant(self) -> bool { self.valid() }

    pub fn new() -> (result: Self)
        ensures result.valid(), result.size() == 0, result.domain() == Set::<i64>::empty(),
            forall|key: i64| #[trigger] result.at(key) == None,
    {
        Index { runs: Vec::new(), size: 0 }
    }

    pub fn get(&self, key: i64) -> (result: Option<Arc<Value>>)
        ensures result == self.at(key),
    {
        proof { assert(self.runs@.subrange(0, self.runs.len() as int) =~= self.runs@); }
        let mut position = 0;
        while position < self.runs.len()
            invariant position <= self.runs.len(),
                lookup(self.runs@.subrange(position as int, self.runs.len() as int), key) == self.at(key),
            decreases self.runs.len() - position,
        {
            if self.runs[position].data.contains_key(&key) {
                proof {
                    let suffix = self.runs@.subrange(position as int, self.runs.len() as int);
                    assert(suffix[0] == self.runs@[position as int]);
                }
                return self.runs[position].get(key);
            }
            proof {
                let suffix = self.runs@.subrange(position as int, self.runs.len() as int);
                assert(suffix[0] == self.runs@[position as int]);
                assert(suffix.drop_first() =~= self.runs@.subrange(position as int + 1, self.runs.len() as int));
            }
            position += 1;
        }
        None
    }

    pub fn share(&self) -> (result: Self)
        ensures result.valid(), result.size() == self.size(), result.domain() == self.domain(),
            forall|key: i64| #[trigger] result.at(key) == self.at(key),
    {
        proof { use_type_invariant(self); }
        let mut runs = Vec::new();
        let mut position = 0;
        while position < self.runs.len()
            invariant position <= self.runs.len(), runs@ == self.runs@.take(position as int), self.valid(),
            decreases self.runs.len() - position,
        {
            runs.push(Arc::clone(&self.runs[position]));
            position += 1;
        }
        proof {
            assert(self.runs@.take(position as int) =~= self.runs@);
            assert forall|key: i64| #[trigger] lookup(runs@, key).is_some() <==> support(runs@).contains(key) by {
                lemma_support_lookup(runs@, key);
            }
            reveal(Index::invariant);
            reveal(Index::valid);
        }
        Index { runs, size: self.size }
    }

    pub fn with(&self, mut newer: Run<Value>) -> (result: Option<Self>)
        requires run_ok(newer),
        ensures
            result matches Some(index) ==> index.valid() &&
                forall|key: i64| #[trigger] index.at(key) ==
                    if newer.model().contains_key(key) { newer.model()[key] } else { self.at(key) },
    {
        proof { use_type_invariant(self); }
        if newer.keys.len() == 0 { return Some(self.share()); }
        let ghost input = newer.model();
        let ghost input_keys = newer.keys@;
        let ghost mut domain = self.domain();
        let mut size = self.size;
        let mut changed = 0;
        while changed < newer.keys.len()
            invariant
                self.valid(), run_ok(newer), newer.model() == input, newer.keys@ == input_keys,
                changed <= newer.keys.len(), size == domain.len(),
                domain == self.domain().difference(input_keys.take(changed as int).to_set()).union(
                    input_keys.take(changed as int).to_set().filter(|key: i64| input[key].is_some())),
            decreases newer.keys.len() - changed,
        {
            let key = newer.keys[changed];
            proof {
                assert(!input_keys.take(changed as int).contains(key));
                assert(domain.contains(key) == self.domain().contains(key));
            }
            if self.get(key).is_some() && newer.get(key).is_none() {
                size = match size.checked_sub(1) { Some(value) => value, None => return None };
            }
            if self.get(key).is_none() && newer.get(key).is_some() {
                size = match size.checked_add(1) { Some(value) => value, None => return None };
            }
            proof {
                let prior = domain;
                if input[key].is_some() { domain = domain.insert(key); }
                else { domain = domain.remove(key); }
                assert(domain.len() == size);
                assert(input_keys.take(changed as int + 1) =~= input_keys.take(changed as int).push(key));
                assert(input_keys.take(changed as int + 1).to_set() =~= input_keys.take(changed as int).to_set().insert(key));
                assert(domain =~= self.domain().difference(input_keys.take(changed as int + 1).to_set()).union(
                    input_keys.take(changed as int + 1).to_set().filter(|entry: i64| input[entry].is_some())));
            }
            changed += 1;
        }
        proof {
            assert(input_keys.take(changed as int) =~= input_keys);
            assert(domain == overlay_support(input, self.runs@));
        }
        if size == 0 {
            proof {
                assert(domain =~= Set::<i64>::empty());
                assert forall|key: i64| #[trigger] overlay_lookup(input, self.runs@, key) == None by {
                    lemma_overlay_support(input, self.runs@, key);
                }
            }
            return Some(Index::new());
        }
        let mut position = 0;
        proof { assert(self.runs@.subrange(0, self.runs.len() as int) =~= self.runs@); }
        while position < self.runs.len()
            invariant
                self.valid(), run_ok(newer), position <= self.runs.len(), size == domain.len(), size > 0,
                forall|key: i64| #[trigger] overlay_lookup(newer.model(), self.runs@.subrange(position as int, self.runs.len() as int), key)
                    == overlay_lookup(input, self.runs@, key),
            decreases self.runs.len() - position,
        {
            let older = &self.runs[position];
            if newer.keys.len() < older.keys.len() / 2 { break; }
            let ghost before = newer.model();
            let mut entry = 0;
            while entry < older.keys.len()
                invariant
                    entry <= older.keys.len(), run_ok(newer), run_ok(**older),
                    newer.model().dom() == before.dom().union(older.keys@.take(entry as int).to_set()),
                    forall|key: i64| #[trigger] newer.model().contains_key(key) ==>
                        newer.model()[key] == if before.contains_key(key) { before[key] } else { older.model()[key] },
                decreases older.keys.len() - entry,
            {
                let key = older.keys[entry];
                if !newer.data.contains_key(&key) {
                    newer.put(key, older.get(key));
                }
                entry += 1;
            }
            proof {
                assert(older.keys@.take(entry as int) =~= older.keys@);
                let tail = self.runs@.subrange(position as int, self.runs.len() as int);
                assert(tail[0] == *older);
                assert(tail.drop_first() =~= self.runs@.subrange(position as int + 1, self.runs.len() as int));
                assert(newer.model().dom() == before.dom().union(older.model().dom()));
                assert forall|key: i64| #[trigger] overlay_lookup(newer.model(), tail.drop_first(), key)
                    == overlay_lookup(before, tail, key) by {}
            }
            position += 1;
        }
        if position == self.runs.len() {
            let ghost before = newer.model();
            let mut live = Run::new();
            let mut entry = 0;
            while entry < newer.keys.len()
                invariant
                    entry <= newer.keys.len(), run_ok(live), run_ok(newer), newer.model() == before,
                    live.model().dom() == newer.keys@.take(entry as int).to_set().filter(|key: i64| before[key].is_some()),
                    forall|key: i64| #[trigger] live.model().contains_key(key) ==> live.model()[key] == before[key],
                decreases newer.keys.len() - entry,
            {
                let key = newer.keys[entry];
                if let Some(value) = newer.get(key) { live.put(key, Some(value)); }
                entry += 1;
            }
            proof {
                assert(newer.keys@.take(entry as int) =~= newer.keys@);
                assert(live.model().dom() == before.dom().filter(|key: i64| before[key].is_some()));
                assert forall|key: i64| #[trigger] overlay_lookup(live.model(), Seq::empty(), key)
                    == overlay_lookup(before, Seq::empty(), key) by {}
            }
            newer = live;
        }
        let ghost merged = newer.model();
        let ghost prepared = newer;
        let ghost start = position;
        proof {
            assert forall|key: i64| #[trigger] overlay_lookup(merged, self.runs@.subrange(start as int, self.runs.len() as int), key)
                == overlay_lookup(input, self.runs@, key) by {}
        }
        let mut runs = Vec::new();
        if newer.keys.len() > 0 { runs.push(Arc::new(newer)); }
        let ghost head = runs@;
        while position < self.runs.len()
            invariant
                start <= position <= self.runs.len(),
                self.valid(),
                size == domain.len(), size > 0,
                forall|key: i64| #[trigger] overlay_lookup(merged, self.runs@.subrange(start as int, self.runs.len() as int), key)
                    == overlay_lookup(input, self.runs@, key),
                runs@ == head + self.runs@.subrange(start as int, position as int),
                forall|entry: int| 0 <= entry < runs.len() ==> run_ok(*(#[trigger] runs@[entry])),
            decreases self.runs.len() - position,
        {
            runs.push(Arc::clone(&self.runs[position]));
            position += 1;
        }
        proof {
            let tail = self.runs@.subrange(start as int, self.runs.len() as int);
            assert forall|key: i64| #[trigger] lookup(runs@, key) == overlay_lookup(input, self.runs@, key) by {
                assert(lookup(runs@, key) == overlay_lookup(merged, tail, key));
                lemma_support_lookup(runs@, key);
                lemma_overlay_support(input, self.runs@, key);
            }
            assert(support(runs@) =~= domain) by {
                assert forall|key: i64| #[trigger] support(runs@).contains(key) == domain.contains(key) by {
                    lemma_support_lookup(runs@, key);
                    lemma_overlay_support(input, self.runs@, key);
                }
            }
            assert(runs.len() > 0) by {
                if runs.len() == 0 { assert(support(runs@) == Set::<i64>::empty()); }
            }
            assert forall|key: i64| #[trigger] lookup(runs@, key).is_some() <==> support(runs@).contains(key) by {
                lemma_support_lookup(runs@, key);
            }
            reveal(Index::invariant);
            reveal(Index::valid);
        }
        Some(Index { runs, size })
    }

    pub fn keys(&self) -> (result: Vec<i64>)
        ensures result@.no_duplicates(), result@.to_set() == self.domain(),
            forall|key: i64| (#[trigger] self.at(key)).is_some() <==> result@.contains(key),
    {
        proof { use_type_invariant(self); }
        let mut seen: HashMap<i64, bool> = HashMap::new();
        let mut keys = Vec::new();
        let mut position = 0;
        while position < self.runs.len()
            invariant
                position <= self.runs.len(), self.valid(), keys@.no_duplicates(),
                keys@.to_set() == seen@.dom().filter(|key: i64| self.at(key).is_some()),
                forall|key: i64| #[trigger] seen@.contains_key(key) <==>
                    exists|entry: int| 0 <= entry < position && (#[trigger] self.runs@[entry]).model().contains_key(key),
            decreases self.runs.len() - position,
        {
            let run = &self.runs[position];
            proof { assert(run_ok(**run)); }
            let mut entry = 0;
            while entry < run.keys.len()
                invariant
                    entry <= run.keys.len(), position < self.runs.len(), *run == self.runs@[position as int], run_ok(**run),
                    self.valid(),
                    keys@.no_duplicates(),
                    keys@.to_set() == seen@.dom().filter(|key: i64| self.at(key).is_some()),
                    forall|key: i64| #[trigger] seen@.contains_key(key) <==>
                        run.keys@.take(entry as int).contains(key) ||
                        exists|prior: int| 0 <= prior < position && (#[trigger] self.runs@[prior]).model().contains_key(key),
                decreases run.keys.len() - entry,
            {
                let key = run.keys[entry];
                let ghost prior_seen = seen@;
                let ghost prior_keys = keys@;
                if !seen.contains_key(&key) {
                    proof {
                        assert(run.keys@.contains(key));
                        assert(run.model().contains_key(key));
                        assert forall|prior: int| 0 <= prior < position implies !(#[trigger] self.runs@[prior]).model().contains_key(key) by {}
                        lemma_lookup_skip(self.runs@, key, position as int);
                        let tail = self.runs@.subrange(position as int, self.runs.len() as int);
                        assert(tail[0] == *run);
                        assert(self.at(key) == run.model()[key]);
                        assert(!keys@.to_set().contains(key));
                        assert(!keys@.contains(key));
                    }
                    seen.insert(key, true);
                    if let Some(_) = run.get(key) { keys.push(key); }
                    proof {
                        assert(keys@.to_set() =~= seen@.dom().filter(|item: i64| self.at(item).is_some())) by {
                            assert forall|item: i64| #[trigger] keys@.to_set().contains(item) ==
                                seen@.dom().filter(|item: i64| self.at(item).is_some()).contains(item) by {
                                if item != key {
                                    assert(keys@.to_set().contains(item) == prior_keys.to_set().contains(item));
                                    assert(seen@.contains_key(item) == prior_seen.contains_key(item));
                                }
                            }
                        }
                    }
                }
                proof {
                    assert(run.keys@.take(entry as int + 1) =~= run.keys@.take(entry as int).push(key));
                }
                entry += 1;
            }
            proof {
                assert(run.keys@.take(entry as int) =~= run.keys@);
                assert forall|key: i64| #[trigger] seen@.contains_key(key) <==>
                    exists|prior: int| 0 <= prior < position + 1 && (#[trigger] self.runs@[prior]).model().contains_key(key) by {
                    if run.model().contains_key(key) {
                        assert(self.runs@[position as int].model().contains_key(key));
                    }
                    if exists|prior: int| 0 <= prior < position + 1 && (#[trigger] self.runs@[prior]).model().contains_key(key) {
                        let prior = choose|prior: int| 0 <= prior < position + 1 && (#[trigger] self.runs@[prior]).model().contains_key(key);
                        if prior == position { assert(run.model().contains_key(key)); }
                    }
                }
            }
            position += 1;
        }
        proof {
            assert(keys@.to_set() =~= self.domain()) by {
                assert forall|key: i64| #[trigger] keys@.to_set().contains(key) == self.domain().contains(key) by {
                    lemma_support_lookup(self.runs@, key);
                    lemma_lookup_witness(self.runs@, key);
                }
            }
        }
        keys
    }
}

pub fn current<Value>(base: &Index<Value>, delta: &Run<Value>, key: i64) -> (result: Option<Arc<Value>>)
    ensures result == overlay_at(*base, *delta, key),
{
    if delta.data.contains_key(&key) { delta.get(key) } else { base.get(key) }
}

}
