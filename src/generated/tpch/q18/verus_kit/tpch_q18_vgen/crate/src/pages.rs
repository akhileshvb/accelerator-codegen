#![verifier::loop_isolation(false)]
use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;

verus! {

broadcast use vstd::std_specs::hash::group_hash_axioms;

pub proof fn empty_set<A>()
    ensures Seq::<A>::empty().to_set() == Set::<A>::empty(), Seq::<A>::empty().no_duplicates(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    assert(Seq::<A>::empty().to_set() =~= Set::<A>::empty());
}

pub proof fn push_set<A>(values: Seq<A>, value: A)
    ensures values.push(value).to_set() == values.to_set().insert(value),
        values.no_duplicates() && !values.contains(value) ==> values.push(value).no_duplicates(),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    assert(values.push(value).to_set() =~= values.to_set().insert(value));
}

pub proof fn remove_unique<A>(values: Seq<A>, index: int)
    requires values.no_duplicates(), 0 <= index < values.len(),
    ensures values.remove(index).no_duplicates(),
        values.remove(index).to_set() == values.to_set().remove(values[index]),
        forall|value: A| #[trigger] values.remove(index).contains(value) <==> values.contains(value) && value != values[index],
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let remaining = values.remove(index);
    assert forall|left: int, right: int| 0 <= left < right < remaining.len() implies
        #[trigger] remaining[left] != #[trigger] remaining[right] by {
        let old_left = if left < index { left } else { left + 1 };
        let old_right = if right < index { right } else { right + 1 };
        assert(remaining[left] == values[old_left]);
        assert(remaining[right] == values[old_right]);
    }
    assert forall|value: A| #[trigger] remaining.contains(value) <==>
        values.contains(value) && value != values[index] by {
        if remaining.contains(value) {
            let found = choose|found: int| 0 <= found < remaining.len() && #[trigger] remaining[found] == value;
            let old_found = if found < index { found } else { found + 1 };
            assert(remaining[found] == values[old_found]);
            assert(values.contains(value));
        }
        if values.contains(value) && value != values[index] {
            let found = choose|found: int| 0 <= found < values.len() && #[trigger] values[found] == value;
            let new_found = if found < index { found } else { found - 1 };
            assert(remaining[new_found] == value);
            assert(remaining.contains(value));
        }
    }
    assert(remaining.to_set() =~= values.to_set().remove(values[index]));
}

pub proof fn insert_unique<A>(values: Seq<A>, index: int, value: A)
    requires values.no_duplicates(), !values.contains(value), 0 <= index <= values.len(),
    ensures values.insert(index, value).no_duplicates(),
        forall|candidate: A| #[trigger] values.insert(index, value).contains(candidate) <==> values.contains(candidate) || candidate == value,
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let inserted = values.insert(index, value);
    assert forall|left: int, right: int| 0 <= left < right < inserted.len() implies #[trigger] inserted[left] != #[trigger] inserted[right] by {
        let old_left = if left < index { left } else { left - 1 };
        let old_right = if right < index { right } else { right - 1 };
        if left != index && right != index {
            assert(inserted[left] == values[old_left]);
            assert(inserted[right] == values[old_right]);
        } else if left == index { assert(values.contains(values[old_right])); }
        else { assert(values.contains(values[old_left])); }
    }
    assert forall|candidate: A| #[trigger] inserted.contains(candidate) <==> values.contains(candidate) || candidate == value by {
        if inserted.contains(candidate) {
            let found = choose|position: int| 0 <= position < inserted.len() && #[trigger] inserted[position] == candidate;
            if found != index {
                let old_found = if found < index { found } else { found - 1 };
                assert(values[old_found] == candidate);
                assert(values.contains(candidate));
            }
        }
        if values.contains(candidate) {
            let found = choose|position: int| 0 <= position < values.len() && #[trigger] values[position] == candidate;
            let new_found = if found < index { found } else { found + 1 };
            assert(inserted[new_found] == candidate);
            assert(inserted.contains(candidate));
        }
        if candidate == value { assert(inserted[index] == candidate); assert(inserted.contains(candidate)); }
    }
}

pub proof fn swap_remove_unique<A>(values: Seq<A>, index: int)
    requires values.no_duplicates(), 0 <= index < values.len(),
    ensures values.update(index, values.last()).drop_last().no_duplicates(),
        forall|value: A| #[trigger] values.update(index, values.last()).drop_last().contains(value) <==> values.contains(value) && value != values[index],
{
    broadcast use vstd::seq_lib::group_seq_properties;
    let removed = values.update(index, values.last()).drop_last();
    assert forall|left: int, right: int| 0 <= left < right < removed.len() implies #[trigger] removed[left] != #[trigger] removed[right] by {
        let old_left = if left == index { values.len() - 1 } else { left };
        let old_right = if right == index { values.len() - 1 } else { right };
        assert(removed[left] == values[old_left]);
        assert(removed[right] == values[old_right]);
    }
    assert forall|value: A| #[trigger] removed.contains(value) <==> values.contains(value) && value != values[index] by {
        if removed.contains(value) {
            let found = choose|position: int| 0 <= position < removed.len() && #[trigger] removed[position] == value;
            let old_found = if found == index { values.len() - 1 } else { found };
            assert(removed[found] == values[old_found]);
            assert(values.contains(value));
        }
        if values.contains(value) && value != values[index] {
            let found = choose|position: int| 0 <= position < values.len() && #[trigger] values[position] == value;
            let new_found = if found == values.len() - 1 { index } else { found };
            assert(removed[new_found] == value);
            assert(removed.contains(value));
        }
    }
}

pub fn share_vec<T>(source: &Vec<Arc<T>>) -> (out: Vec<Arc<T>>)
    ensures out@ == source@,
{
    let mut out = Vec::new();
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    {
        out.push(Arc::clone(&source[index]));
        index += 1;
    }
    out
}

pub fn copy_keys(source: &Vec<i128>) -> (out: Vec<i128>)
    ensures out@ == source@,
{
    let mut out = Vec::new();
    let mut index = 0usize;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    {
        out.push(source[index]);
        index += 1;
    }
    out
}

pub struct Page<T> {
    pub rows: HashMap<i128, Arc<T>>,
    pub keys: Vec<i128>,
}

impl<T> Page<T> {
    pub open spec fn wf(self) -> bool {
        &&& self.keys@.no_duplicates()
        &&& self.rows@.dom() == self.keys@.to_set()
    }

    pub fn empty() -> (out: Self)
        ensures out.wf(), out.rows@ == Map::<i128, Arc<T>>::empty(), out.keys@ == Seq::<i128>::empty(),
    {
        proof { empty_set::<i128>(); }
        Page { rows: HashMap::new(), keys: Vec::new() }
    }

    pub fn share(&self) -> (out: Self)
        requires self.wf(),
        ensures out.wf(), out.rows@ == self.rows@, out.keys@ == self.keys@,
    {
        broadcast use vstd::seq_lib::group_seq_properties;
        let mut rows = HashMap::new();
        let mut index = 0usize;
        while index < self.keys.len()
            invariant index <= self.keys.len(),
                rows@.dom() == self.keys@.take(index as int).to_set(),
                forall|key: i128| #[trigger] rows@.contains_key(key) ==> rows@[key] == self.rows@[key],
            decreases self.keys.len() - index,
        {
            let ghost prefix = self.keys@.take(index as int);
            let key = self.keys[index];
            proof { assert(self.rows@.contains_key(key)); }
            if let Some(value) = self.rows.get(&key) {
                rows.insert(key, Arc::clone(value));
            }
            proof {
                assert(self.keys@.take(index as int + 1) =~= prefix.push(key));
                push_set(prefix, key);
            }
            index += 1;
        }
        proof { assert(rows@ =~= self.rows@); }
        Page { rows, keys: copy_keys(&self.keys) }
    }

    pub fn put(&mut self, key: i128, value: Arc<T>)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).rows@ == old(self).rows@.insert(key, value),
    {
        let ghost original = self.keys@;
        if !self.rows.contains_key(&key) {
            proof { push_set(self.keys@, key); }
            self.keys.push(key);
        }
        self.rows.insert(key, value);
        proof { assert(self.rows@.dom() =~= self.keys@.to_set()); }
    }

    pub fn erase(&mut self, key: i128)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).rows@ == old(self).rows@.remove(key),
    {
        self.rows.remove(&key);
        let mut index = 0usize;
        while index < self.keys.len()
            invariant index <= self.keys.len(),
                self.keys@ == old(self).keys@,
                self.rows@ == old(self).rows@.remove(key),
                forall|position: int| 0 <= position < index ==> #[trigger] self.keys@[position] != key,
            decreases self.keys.len() - index,
        {
            if self.keys[index] == key {
                proof { remove_unique(self.keys@, index as int); }
                self.keys.remove(index);
                return;
            }
            index += 1;
        }
        proof {
            assert(!self.keys@.contains(key));
            assert(self.rows@.dom() =~= self.keys@.to_set());
        }
    }
}

pub struct Directory<T> { pub pages: Vec<Arc<Page<T>>> }

pub struct Table<T> {
    pub roots: Vec<Arc<Directory<T>>>,
    pub shards: usize,
}

pub open spec fn shard(key: i128, shards: usize) -> usize {
    let bits = key as u128;
    ((bits ^ (bits >> 32) ^ (bits >> 64)) % (shards as u128)) as usize
}

pub fn shard_exec(key: i128, shards: usize) -> (out: usize)
    requires shards > 0,
    ensures out == shard(key, shards), out < shards,
{
    let bits = key as u128;
    ((bits ^ (bits >> 32) ^ (bits >> 64)) % (shards as u128)) as usize
}

impl<T> Table<T> {
    pub proof fn bounds(self, index: usize)
        requires self.wf(), index < self.shards,
        ensures index / 128 < self.roots.len(),
            index % 128 < self.roots@[index as int / 128].pages.len(),
    {
        assert(self.roots@[index as int / 128].pages.len() ==
            if index as int / 128 + 1 < self.roots.len() { 128 } else { self.shards as int - index as int / 128 * 128 });
    }
    pub open spec fn at(self, key: i128) -> Option<T> {
        let index = shard(key, self.shards);
        let page = self.roots@[index as int / 128].pages@[index as int % 128];
        if page.rows@.contains_key(key) { Some(*page.rows@[key]) } else { None }
    }

    pub open spec fn wf(self) -> bool {
        &&& self.shards > 0
        &&& self.roots.len() == (self.shards as int + 127) / 128
        &&& forall|root: int| 0 <= root < self.roots.len() ==>
            #[trigger] self.roots@[root].pages.len() ==
                if root + 1 < self.roots.len() { 128 } else { self.shards as int - root * 128 }
        &&& forall|index: int| 0 <= index < self.shards ==>
            #[trigger] self.roots@[index / 128].pages@[index % 128].wf()
        &&& forall|index: int, key: i128| 0 <= index < self.shards &&
            #[trigger] self.roots@[index / 128].pages@[index % 128].rows@.contains_key(key)
            ==> shard(key, self.shards) == index
    }

    pub fn empty(expected: usize) -> (out: Self)
        ensures out.wf(), forall|key: i128| #[trigger] out.at(key) == None,
    {
        let shards = expected / 128 + 1;
        let blank = Arc::new(Page::empty());
        let mut roots: Vec<Arc<Directory<T>>> = Vec::new();
        let mut index = 0usize;
        while index < shards
            invariant index <= shards,
                index < shards ==> index % 128 == 0,
                roots.len() == (index as int + 127) / 128,
                forall|root: int| 0 <= root < roots.len() ==> #[trigger] roots@[root].pages.len() ==
                    if root + 1 < roots.len() { 128 } else { index as int - root * 128 },
                forall|position: int| 0 <= position < index ==>
                    #[trigger] roots@[position / 128].pages@[position % 128] == blank,
            decreases shards - index,
        {
            let mut pages = Vec::new();
            let mut offset = 0usize;
            let ghost start = index as int;
            while offset < 128 && index < shards
                invariant index <= shards, offset <= 128,
                    index == start + offset, start == roots.len() * 128,
                    pages.len() == offset,
                    forall|position: int| 0 <= position < pages.len() ==> #[trigger] pages@[position] == blank,
                decreases shards - index,
            {
                pages.push(Arc::clone(&blank));
                offset += 1;
                index += 1;
            }
            roots.push(Arc::new(Directory { pages }));
        }
        proof {
            assert forall|position: int| 0 <= position < shards implies
                #[trigger] roots@[position / 128].pages@[position % 128] == blank by {}
        }
        Table { roots, shards }
    }

    pub fn share(&self) -> (out: Self)
        requires self.wf(),
        ensures out.wf(), out.roots@ == self.roots@, out.shards == self.shards,
            forall|key: i128| #[trigger] out.at(key) == self.at(key),
    {
        Table { roots: share_vec(&self.roots), shards: self.shards }
    }

    pub fn get(&self, key: i128) -> (out: Option<Arc<T>>)
        requires self.wf(),
        ensures match out { Some(value) => self.at(key) == Some(*value), None => self.at(key) == None },
    {
        let index = shard_exec(key, self.shards);
        proof { self.bounds(index); }
        let page = &self.roots[index / 128].pages[index % 128];
        match page.rows.get(&key) {
            Some(value) => Some(Arc::clone(value)),
            None => None,
        }
    }

    pub fn replace(&mut self, index: usize, page: Page<T>)
        requires old(self).wf(), index < old(self).shards, page.wf(),
            forall|key: i128| #[trigger] page.rows@.contains_key(key) ==> shard(key, old(self).shards) == index,
        ensures final(self).wf(), final(self).shards == old(self).shards,
            forall|position: int| 0 <= position < final(self).shards ==>
                *#[trigger] final(self).roots@[position / 128].pages@[position % 128] ==
                    if position == index { page } else { *old(self).roots@[position / 128].pages@[position % 128] },
    {
        proof { self.bounds(index); }
        let mut pages = share_vec(&self.roots[index / 128].pages);
        pages.set(index % 128, Arc::new(page));
        self.roots.set(index / 128, Arc::new(Directory { pages }));
    }
}

pub struct Editor<T> {
    pub base: Table<T>,
    pub dirty: HashMap<usize, Page<T>>,
    pub touched: Vec<usize>,
}

impl<T> Editor<T> {
    pub open spec fn wf(self) -> bool {
        &&& self.base.wf()
        &&& self.touched@.no_duplicates()
        &&& self.dirty@.dom() == self.touched@.to_set()
        &&& forall|index: usize| #[trigger] self.dirty@.contains_key(index) ==>
            index < self.base.shards && self.dirty@[index].wf()
        &&& forall|index: usize, key: i128| self.dirty@.contains_key(index) &&
            #[trigger] self.dirty@[index].rows@.contains_key(key) ==> shard(key, self.base.shards) == index
    }

    pub open spec fn at(self, key: i128) -> Option<T> {
        let index = shard(key, self.base.shards);
        if self.dirty@.contains_key(index) {
            if self.dirty@[index].rows@.contains_key(key) {
                Some(*self.dirty@[index].rows@[key])
            } else { None }
        } else { self.base.at(key) }
    }

    pub fn new(base: Table<T>) -> (out: Self)
        requires base.wf(),
        ensures out.wf(), out.base == base,
            forall|key: i128| #[trigger] out.at(key) == base.at(key),
    {
        proof { empty_set::<usize>(); }
        Editor { base, dirty: HashMap::new(), touched: Vec::new() }
    }

    pub fn get(&self, key: i128) -> (out: Option<Arc<T>>)
        requires self.wf(),
        ensures match out { Some(value) => self.at(key) == Some(*value), None => self.at(key) == None },
    {
        let index = shard_exec(key, self.base.shards);
        match self.dirty.get(&index) {
            Some(page) => match page.rows.get(&key) {
                Some(value) => Some(Arc::clone(value)), None => None,
            },
            None => self.base.get(key),
        }
    }

    pub fn prepare(&mut self, key: i128) -> (out: usize)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).base == old(self).base,
            out == shard(key, final(self).base.shards), final(self).dirty@.contains_key(out),
            forall|other: i128| #[trigger] final(self).at(other) == old(self).at(other),
    {
        let index = shard_exec(key, self.base.shards);
        proof { self.base.bounds(index); }
        if !self.dirty.contains_key(&index) {
            let page = self.base.roots[index / 128].pages[index % 128].share();
            self.dirty.insert(index, page);
            proof { push_set(self.touched@, index); }
            self.touched.push(index);
        }
        index
    }

    pub fn put(&mut self, key: i128, value: Arc<T>)
        requires old(self).wf(),
        ensures final(self).wf(),
            forall|other: i128| #[trigger] final(self).at(other) ==
                if other == key { Some(*value) } else { old(self).at(other) },
    {
        let index = self.prepare(key);
        let ghost prepared = *self;
        if let Some(mut page) = self.dirty.remove(&index) {
            page.put(key, value);
            self.dirty.insert(index, page);
        }
        proof {
            assert(self.base == prepared.base);
            assert(self.dirty@.dom() =~= prepared.dirty@.dom());
            assert forall|other: i128| #[trigger] self.at(other) ==
                (if other == key { Some(*value) } else { prepared.at(other) }) by {
                if shard(other, self.base.shards) == index {
                    assert(self.dirty@[index].rows@ == prepared.dirty@[index].rows@.insert(key, value));
                }
            }
        }
    }

    pub fn erase(&mut self, key: i128)
        requires old(self).wf(),
        ensures final(self).wf(),
            forall|other: i128| #[trigger] final(self).at(other) ==
                if other == key { None } else { old(self).at(other) },
    {
        let index = self.prepare(key);
        let ghost prepared = *self;
        if let Some(mut page) = self.dirty.remove(&index) {
            page.erase(key);
            self.dirty.insert(index, page);
        }
        proof {
            assert(self.base == prepared.base);
            assert(self.dirty@.dom() =~= prepared.dirty@.dom());
            assert forall|other: i128| #[trigger] self.at(other) ==
                (if other == key { None } else { prepared.at(other) }) by {
                if shard(other, self.base.shards) == index {
                    assert(self.dirty@[index].rows@ == prepared.dirty@[index].rows@.remove(key));
                }
            }
        }
    }

    pub fn finish(self) -> (out: Table<T>)
        requires self.wf(),
        ensures out.wf(), forall|key: i128| #[trigger] out.at(key) == self.at(key),
    {
        broadcast use vstd::seq_lib::group_seq_properties;
        let mut editor = self;
        let mut index = 0usize;
        while index < editor.touched.len()
            invariant index <= editor.touched.len(),
                editor.touched@ == self.touched@,
                editor.base.wf(), editor.base.shards == self.base.shards,
                forall|position: usize| #[trigger] editor.dirty@.contains_key(position) <==>
                    self.dirty@.contains_key(position) && !self.touched@.take(index as int).contains(position),
                forall|position: usize| #[trigger] editor.dirty@.contains_key(position) ==>
                    editor.dirty@[position] == self.dirty@[position],
                forall|position: int| 0 <= position < self.base.shards ==>
                    *#[trigger] editor.base.roots@[position / 128].pages@[position % 128] ==
                        if self.touched@.take(index as int).contains(position as usize) {
                            self.dirty@[position as usize]
                        } else { *self.base.roots@[position / 128].pages@[position % 128] },
            decreases editor.touched.len() - index,
        {
            let page_index = editor.touched[index];
            proof {
                assert(!self.touched@.take(index as int).contains(page_index));
                assert(editor.dirty@.contains_key(page_index));
            }
            if let Some(page) = editor.dirty.remove(&page_index) {
                editor.base.replace(page_index, page);
            }
            proof {
                assert(self.touched@.take(index as int + 1) =~=
                    self.touched@.take(index as int).push(page_index));
            }
            index += 1;
        }
        proof {
            assert forall|key: i128| #[trigger] editor.base.at(key) == self.at(key) by {
                let position = shard(key, self.base.shards);
                assert(position < self.base.shards);
            }
        }
        editor.base
    }
}

pub struct KeyBucket {
    pub chunks: Vec<Arc<Vec<Arc<Vec<i64>>>>>,
    pub len: usize,
}

pub fn put_valid<T>(editor: &mut Editor<T>, key: i128, value: Arc<T>, Ghost(valid): Ghost<spec_fn(Option<T>) -> bool>)
    requires old(editor).wf(), valid(Some(*value)), forall|other: i128| #[trigger] valid(old(editor).at(other)),
    ensures final(editor).wf(), forall|other: i128| #[trigger] valid(final(editor).at(other)),
        forall|other: i128| #[trigger] final(editor).at(other) ==
            if other == key { Some(*value) } else { old(editor).at(other) },
{
    editor.put(key, value);
    proof {
        assert forall|other: i128| #[trigger] valid(editor.at(other)) by {
            if other != key { assert(valid(old(editor).at(other))); }
        }
    }
}

pub fn erase_valid<T>(editor: &mut Editor<T>, key: i128, Ghost(valid): Ghost<spec_fn(Option<T>) -> bool>)
    requires old(editor).wf(), valid(None), forall|other: i128| #[trigger] valid(old(editor).at(other)),
    ensures final(editor).wf(), forall|other: i128| #[trigger] valid(final(editor).at(other)),
        forall|other: i128| #[trigger] final(editor).at(other) == if other == key { None } else { old(editor).at(other) },
{
    editor.erase(key);
    proof {
        assert forall|other: i128| #[trigger] valid(editor.at(other)) by {
            if other != key { assert(valid(old(editor).at(other))); }
        }
    }
}

impl KeyBucket {
    pub proof fn coordinates(index: int)
        requires index >= 0,
        ensures index / 128 / 128 == index / 16384,
            index == index / 16384 * 16384 + index / 128 % 128 * 128 + index % 128,
    {}
    pub proof fn last_page_size(self)
        requires self.wf(), self.len > 0,
        ensures self.chunks@[(self.len as int - 1) / 16384].len() == (self.len as int - 1) / 128 % 128 + 1,
            self.chunks@[(self.len as int - 1) / 16384]@[(self.len as int - 1) / 128 % 128].len() ==
                (self.len as int - 1) % 128 + 1,
    {
        let page = (self.len as int - 1) / 128;
        let chunk = (self.len as int - 1) / 16384;
        assert(page / 128 == chunk);
        assert(self.chunks@[page / 128]@[page % 128].len() ==
            if (page + 1) * 128 <= self.len { 128 } else { self.len as int - page * 128 });
    }

    pub proof fn bounds(self, index: usize)
        requires self.wf(), index < self.len,
        ensures index / 16384 < self.chunks.len(),
            index / 128 % 128 < self.chunks@[index as int / 16384].len(),
            index % 128 < self.chunks@[index as int / 16384]@[index as int / 128 % 128].len(),
    {
        assert(index as int / 128 / 128 == index as int / 16384);
        assert(self.chunks@[index as int / 16384].len() ==
            if index as int / 16384 + 1 < self.chunks.len() { 128 }
            else { (self.len as int - index as int / 16384 * 16384 + 127) / 128 });
        assert(self.chunks@[index as int / 128 / 128]@[index as int / 128 % 128].len() ==
            if (index as int / 128 + 1) * 128 <= self.len { 128 }
            else { self.len as int - index as int / 128 * 128 });
    }
    pub open spec fn at(self, index: int) -> i64 {
        self.chunks@[index / 16384]@[index / 128 % 128]@[index % 128]
    }

    pub open spec fn view(self) -> Seq<i64> {
        Seq::new(self.len as nat, |index: int| self.at(index))
    }

    pub open spec fn wf(self) -> bool {
        &&& self.chunks.len() == (self.len as int + 16383) / 16384
        &&& forall|chunk: int| 0 <= chunk < self.chunks.len() ==>
            #[trigger] self.chunks@[chunk].len() ==
                if chunk + 1 < self.chunks.len() { 128 }
                else { (self.len as int - chunk * 16384 + 127) / 128 }
        &&& forall|page: int| 0 <= page < (self.len as int + 127) / 128 ==>
            #[trigger] self.chunks@[page / 128]@[page % 128].len() ==
                if (page + 1) * 128 <= self.len { 128 } else { self.len as int - page * 128 }
    }

    pub fn empty() -> (out: Self)
        ensures out.wf(), out.len == 0, out.view() == Seq::<i64>::empty(),
    { KeyBucket { chunks: Vec::new(), len: 0 } }

    pub fn share(&self) -> (out: Self)
        requires self.wf(),
        ensures out.wf(), out.len == self.len, out.view() == self.view(),
    { KeyBucket { chunks: share_vec(&self.chunks), len: self.len } }

    pub fn get(&self, index: usize) -> (out: i64)
        requires self.wf(), index < self.len,
        ensures out == self.at(index as int),
    {
        proof { self.bounds(index); }
        self.chunks[index / 16384][index / 128 % 128][index % 128]
    }

    pub fn push(&mut self, key: i64) -> (out: bool)
        requires old(self).wf(),
        ensures final(self).wf(), out ==> final(self).view() == old(self).view().push(key),
            out <==> old(self).len < usize::MAX,
            out ==> final(self).len == old(self).len + 1,
            !out ==> final(self).view() == old(self).view(),
    {
        if self.len == usize::MAX { return false; }
        proof { if self.len > 0 { self.last_page_size(); } }
        let chunk_index = self.len / 16384;
        let page_index = self.len / 128 % 128;
        proof {
            assert(chunk_index <= self.chunks.len());
            if chunk_index < self.chunks.len() {
                assert(self.chunks@[chunk_index as int].len() == (self.len as int - chunk_index as int * 16384 + 127) / 128);
            }
        }
        if chunk_index == self.chunks.len() { self.chunks.push(Arc::new(Vec::new())); }
        let mut chunk = share_vec(&self.chunks[chunk_index]);
        proof {
            assert(chunk.len() == page_index as int + if self.len % 128 == 0 { 0int } else { 1int });
            if page_index < chunk.len() {
                assert(chunk@[page_index as int].len() == self.len % 128);
            }
        }
        let mut page = Vec::new();
        if page_index < chunk.len() {
            let mut index = 0usize;
            while index < chunk[page_index].len()
                invariant index <= chunk@[page_index as int].len(),
                    self.len == old(self).len,
                    chunk@[page_index as int].len() == old(self).len % 128,
                    page@ == chunk@[page_index as int]@.take(index as int),
                decreases chunk@[page_index as int].len() - index,
            {
                page.push(chunk[page_index][index]);
                index += 1;
            }
        }
        proof {
            if old(self).len > 0 && old(self).len % 128 != 0 {
                assert(page@ == old(self).chunks@[chunk_index as int]@[page_index as int]@);
            }
            assert(page.len() == old(self).len % 128);
        }
        page.push(key);
        if page_index == chunk.len() { chunk.push(Arc::new(page)); }
        else { chunk.set(page_index, Arc::new(page)); }
        self.chunks.set(chunk_index, Arc::new(chunk));
        self.len += 1;
        proof {
            assert forall|position: int| 0 <= position < self.len implies #[trigger] self.at(position) ==
                (if position < old(self).len { old(self).at(position) } else { key }) by {
                Self::coordinates(position);
                Self::coordinates(old(self).len as int);
            }
            assert(self.view() =~= old(self).view().push(key));
        }
        true
    }

    pub fn set(&mut self, slot: usize, key: i64)
        requires old(self).wf(), slot < old(self).len,
        ensures final(self).wf(), final(self).len == old(self).len,
            final(self).view() == old(self).view().update(slot as int, key),
    {
        let chunk_index = slot / 16384;
        proof { self.bounds(slot); }
        let page_index = slot / 128 % 128;
        let mut chunk = share_vec(&self.chunks[chunk_index]);
        let mut page = Vec::new();
        let mut index = 0usize;
        while index < chunk[page_index].len()
            invariant index <= chunk@[page_index as int].len(),
                page@ == chunk@[page_index as int]@.take(index as int),
            decreases chunk@[page_index as int].len() - index,
        {
            page.push(chunk[page_index][index]);
            index += 1;
        }
        page.set(slot % 128, key);
        chunk.set(page_index, Arc::new(page));
        self.chunks.set(chunk_index, Arc::new(chunk));
        proof {
            assert forall|position: int| 0 <= position < self.len implies #[trigger] self.at(position) ==
                (if position == slot { key } else { old(self).at(position) }) by {
                Self::coordinates(position);
                Self::coordinates(slot as int);
            }
            assert(self.view() =~= old(self).view().update(slot as int, key));
        }
    }

    pub fn remove(&mut self, slot: usize) -> (out: i64)
        requires old(self).wf(), slot < old(self).len,
        ensures final(self).wf(), final(self).len == old(self).len - 1,
            out == old(self).view().last(),
            final(self).view() == old(self).view().update(slot as int, out).drop_last(),
    {
        let last = self.get(self.len - 1);
        if slot + 1 < self.len { self.set(slot, last); }
        proof {
            if slot as int + 1 == old(self).len {
                assert(old(self).view().update(slot as int, last) =~= old(self).view());
            }
        }
        self.pop_last();
        last
    }

    pub fn pop_last(&mut self)
        requires old(self).wf(), old(self).len > 0,
        ensures final(self).wf(), final(self).len == old(self).len - 1,
            final(self).view() == old(self).view().drop_last(),
    {
        let ghost original_chunks = self.chunks@;
        let ghost original_length = self.len as int;
        proof { self.last_page_size(); }
        let chunk_index = (self.len - 1) / 16384;
        proof { self.bounds((self.len - 1) as usize); }
        let page_index = (self.len - 1) / 128 % 128;
        let mut chunk = share_vec(&self.chunks[chunk_index]);
        proof {
            assert(chunk.len() == page_index as int + 1);
            assert(chunk@[page_index as int].len() == (original_length - 1) % 128 + 1);
        }
        let mut page = Vec::new();
        let mut index = 0usize;
        while index < chunk[page_index].len() - 1
            invariant index < chunk@[page_index as int].len(),
                page@ == chunk@[page_index as int]@.take(index as int),
            decreases chunk@[page_index as int].len() - index,
        {
            page.push(chunk[page_index][index]);
            index += 1;
        }
        if page.len() == 0 { chunk.pop(); }
        else { chunk.set(page_index, Arc::new(page)); }
        if chunk.len() == 0 { self.chunks.pop(); }
        else { self.chunks.set(chunk_index, Arc::new(chunk)); }
        self.len -= 1;
        proof {
            assert forall|position: int| 0 <= position < self.len implies
                #[trigger] self.at(position) == old(self).at(position) by {
                Self::coordinates(position);
                Self::coordinates(original_length - 1);
                if position / 16384 == chunk_index {
                    if position / 128 % 128 == page_index {
                        assert(position % 128 < page.len());
                    }
                }
            }
            assert(self.view() =~= old(self).view().drop_last());
        }
    }

    pub fn from_keys(keys: &Vec<i64>) -> (out: Self)
        ensures out.wf(), out.len == keys.len(), out.view() == keys@,
    {
        let mut out = KeyBucket::empty();
        let mut index = 0usize;
        while index < keys.len()
            invariant index <= keys.len(), out.wf(), out.len == index, out.view() == keys@.take(index as int),
            decreases keys.len() - index,
        {
            let accepted = out.push(keys[index]);
            proof { assert(accepted); }
            index += 1;
        }
        out
    }
}

}
