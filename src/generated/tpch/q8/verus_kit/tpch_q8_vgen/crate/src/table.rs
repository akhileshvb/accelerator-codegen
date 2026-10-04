use vstd::prelude::*;
use vstd::hash_map::HashMapWithView as HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::compound_map::CompoundMap;
use verus_prooflib::ordered::{Index, Key};

verus! {

broadcast use { vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties };

pub type Major = (i64, i64, i64);

#[derive(Clone, Copy)]
pub struct Cell {
    pub first: i64,
    pub second: i64,
    pub third: i64,
    pub count: i128,
    pub volume: i128,
}

pub struct Bucket {
    pub values: CompoundMap<Arc<Vec<Cell>>>,
    pub keys: Vec<Major>,
}

pub struct Page {
    pub buckets: Vec<Arc<Bucket>>,
}

pub struct PageDirectory {
    entries: Index<Arc<Page>>,
    length: usize,
}

impl View for PageDirectory {
    type V = Seq<Arc<Page>>;
    closed spec fn view(&self) -> Seq<Arc<Page>> {
        Seq::new(self.length as nat, |position: int| self.entries.view()[(position, Seq::<u8>::empty())])
    }
}

impl PageDirectory {
    #[verifier::type_invariant]
    spec fn valid(self) -> bool {
        self.entries.wf() && self.length <= i64::MAX as int
            && forall|position: int| 0 <= position < self.length ==>
                #[trigger] self.entries.view().dom().contains((position, Seq::<u8>::empty()))
    }

    pub fn new() -> (result: Self)
        ensures result@ == Seq::<Arc<Page>>::empty(),
    {
        let result = PageDirectory { entries: Index::new(), length: 0 };
        proof { assert(result@ =~= Seq::<Arc<Page>>::empty()); }
        result
    }

    pub fn len(&self) -> (result: usize)
        ensures result as int == self@.len(), result <= i64::MAX as int,
    {
        proof { use_type_invariant(self); }
        self.length
    }

    pub fn get(&self, position: usize) -> (result: Arc<Page>)
        requires position < self@.len(),
        ensures result == self@[position as int],
    {
        proof { use_type_invariant(self); }
        let key = Key::number(position as i64);
        match self.entries.get(&key) {
            Some(page) => (*page).clone(),
            None => {
                proof { assert(false); }
                Arc::new(Page { buckets: Vec::new() })
            },
        }
    }

    pub fn share(&self) -> (result: Self)
        ensures result@ == self@,
    {
        proof { use_type_invariant(self); }
        let result = PageDirectory { entries: self.entries.share(), length: self.length };
        proof { assert(result@ =~= self@); }
        result
    }

    pub fn push(&mut self, page: Arc<Page>)
        requires old(self)@.len() < i64::MAX as int, old(self)@.len() < usize::MAX as int,
        ensures final(self)@ == old(self)@.push(page),
    {
        proof { use_type_invariant(&*self); }
        let ghost before = self@;
        let mut entries = self.entries.share();
        entries.put(Key::number(self.length as i64), Arc::new(page));
        self.entries = entries;
        self.length += 1;
        proof { assert(self@ =~= before.push(page)); }
    }

    pub fn set(&mut self, position: usize, page: Arc<Page>)
        requires position < old(self)@.len(),
        ensures final(self)@ == old(self)@.update(position as int, page),
    {
        proof { use_type_invariant(&*self); }
        let ghost before = self@;
        let mut entries = self.entries.share();
        entries.put(Key::number(position as i64), Arc::new(page));
        self.entries = entries;
        proof { assert(self@ =~= before.update(position as int, page)); }
    }
}

pub struct Table {
    pub pages: Arc<PageDirectory>,
}

pub open spec fn slot(key: Major, slots: int) -> int {
    (1208925819614629174706176 + key.0 as int + key.1 as int * 65537 + key.2 as int * 4099) % slots
}

pub fn slot_exec(key: Major, slots: usize) -> (result: usize)
    requires slots > 0,
    ensures result as int == slot(key, slots as int), result < slots,
{
    let hash = 1208925819614629174706176i128 + key.0 as i128 + key.1 as i128 * 65537i128 + key.2 as i128 * 4099i128;
    (hash % slots as i128) as usize
}

#[verifier::opaque]
pub open spec fn cell_sum(rows: Seq<Cell>, minor: Major, volume: bool) -> int {
    sum(rows, |cell: Cell| cell_weight(cell, minor, volume))
}

pub open spec fn minor_of(cell: Cell) -> Major {
    (cell.first, cell.second, cell.third)
}

pub open spec fn cell_weight(cell: Cell, minor: Major, volume: bool) -> int {
    if minor_of(cell) == minor { if volume { cell.volume as int } else { cell.count as int } } else { 0 }
}

fn add_cells(rows: Vec<Cell>, minor: Major, count: i128, volume: i128) -> (result: Option<Vec<Cell>>)
    requires rows_valid(rows@),
    ensures result matches Some(after) ==> rows_valid(after@)
        && forall|small: Major, measure: bool| #[trigger] cell_sum(after@, small, measure) == cell_sum(rows@, small, measure)
            + if small == minor { if measure { volume as int } else { count as int } } else { 0 },
{
    proof { reveal(cell_sum); }
    let ghost before = rows@;
    let mut current = rows;
    let mut index = 0;
    while index < current.len()
        invariant index <= current.len(), current@ == before, before == rows@, rows_valid(before),
            forall|offset: int| 0 <= offset < index ==> minor_of(#[trigger] before[offset]) != minor,
        decreases current.len() - index,
    {
        let cell = current[index];
        if minor_of_exec(cell) == minor {
            let new_count = cell.count.checked_add(count)?;
            let new_volume = cell.volume.checked_add(volume)?;
            if new_count == 0 && new_volume == 0 {
                current.remove(index);
                proof {
                    lemma_rows_remove(before, index as int);
                    assert forall|small: Major, measure: bool| #[trigger] cell_sum(current@, small, measure) == cell_sum(before, small, measure)
                    + if small == minor { if measure { volume as int } else { count as int } } else { 0 } by {
                        reveal(cell_sum);
                        lemma_sum_remove(before, index as int, |entry: Cell| cell_weight(entry, small, measure));
                    }
                }
            } else {
                let replacement = Cell { first: minor.0, second: minor.1, third: minor.2, count: new_count, volume: new_volume };
                current.set(index, replacement);
                proof {
                    lemma_rows_update(before, index as int, replacement);
                    assert forall|small: Major, measure: bool| #[trigger] cell_sum(current@, small, measure) == cell_sum(before, small, measure)
                    + if small == minor { if measure { volume as int } else { count as int } } else { 0 } by {
                        reveal(cell_sum);
                        lemma_sum_update(before, index as int, replacement, |entry: Cell| cell_weight(entry, small, measure));
                    }
                }
            }
            return Some(current);
        }
        index += 1;
    }
    let cell = Cell { first: minor.0, second: minor.1, third: minor.2, count, volume };
    if count == 0 && volume == 0 { return Some(current); }
    current.push(cell);
    proof {
        assert(rows_valid(current@));
        assert forall|small: Major, measure: bool| #[trigger] cell_sum(current@, small, measure) == cell_sum(before, small, measure)
            + if small == minor { if measure { volume as int } else { count as int } } else { 0 } by {
            reveal(cell_sum);
            lemma_sum_push(before, cell, |entry: Cell| cell_weight(entry, small, measure));
        }
    }
    Some(current)
}

fn minor_of_exec(cell: Cell) -> (result: Major)
    ensures result == minor_of(cell),
{
    (cell.first, cell.second, cell.third)
}

proof fn lemma_rows_remove(rows: Seq<Cell>, index: int)
    requires rows_valid(rows), 0 <= index < rows.len(),
    ensures rows_valid(rows.remove(index)),
{
    assert forall|offset: int| 0 <= offset < rows.remove(index).len() implies
        (#[trigger] rows.remove(index)[offset]).count != 0 || rows.remove(index)[offset].volume != 0 by {
        assert(rows.remove(index)[offset] == rows[if offset < index { offset } else { offset + 1 }]);
    }
    assert forall|left: int, right: int| 0 <= left < right < rows.remove(index).len() implies
        minor_of(#[trigger] rows.remove(index)[left]) != minor_of(#[trigger] rows.remove(index)[right]) by {
        assert(rows.remove(index)[left] == rows[if left < index { left } else { left + 1 }]);
        assert(rows.remove(index)[right] == rows[if right < index { right } else { right + 1 }]);
    }
}

proof fn lemma_rows_update(rows: Seq<Cell>, index: int, cell: Cell)
    requires rows_valid(rows), 0 <= index < rows.len(), minor_of(cell) == minor_of(rows[index]), cell.count != 0 || cell.volume != 0,
    ensures rows_valid(rows.update(index, cell)),
{
    assert forall|offset: int| 0 <= offset < rows.update(index, cell).len() implies
        (#[trigger] rows.update(index, cell)[offset]).count != 0 || rows.update(index, cell)[offset].volume != 0 by {}
    assert forall|left: int, right: int| 0 <= left < right < rows.update(index, cell).len() implies
        minor_of(#[trigger] rows.update(index, cell)[left]) != minor_of(#[trigger] rows.update(index, cell)[right]) by {}
}

pub open spec fn rows_valid(rows: Seq<Cell>) -> bool {
    &&& forall|index: int| 0 <= index < rows.len() ==>
        (#[trigger] rows[index]).count != 0 || rows[index].volume != 0
    &&& forall|left: int, right: int| 0 <= left < right < rows.len() ==>
        minor_of(#[trigger] rows[left]) != minor_of(#[trigger] rows[right])
}

impl Table {
    pub open spec fn keys_view(self) -> Seq<Major> {
        flat((*self.pages)@.map_values(|page: Arc<Page>| page_keys(*page)))
    }

    pub open spec fn rows(self, key: Major) -> Seq<Cell> {
        let position = slot(key, (*self.pages)@.len() as int * 32);
        let page = (*self.pages)@[position / 32];
        let bucket = (*page).buckets@[position % 32];
        if (*bucket).values@.contains_key(key) {
            (*(*bucket).values@[key])@
        } else { Seq::empty() }
    }

    #[verifier::opaque]
    pub open spec fn at(self, key: Major, minor: Major, volume: bool) -> int {
        cell_sum(self.rows(key), minor, volume)
    }

    pub open spec fn valid(self) -> bool {
        &&& (*self.pages)@.len() > 0
        &&& (*self.pages)@.len() <= usize::MAX as int / 32
        &&& forall|page: int| 0 <= page < (*self.pages)@.len() ==>
            (*#[trigger] (*self.pages)@[page]).buckets@.len() == 32
        &&& forall|page: int, bucket: int| 0 <= page < (*self.pages)@.len() && 0 <= bucket < 32 ==>
            bucket_valid(*#[trigger] (*(*self.pages)@[page]).buckets@[bucket], page * 32 + bucket, (*self.pages)@.len() as int * 32)
        &&& forall|key: Major, index: int| 0 <= index < self.rows(key).len() ==>
            (#[trigger] self.rows(key)[index]).count != 0 || self.rows(key)[index].volume != 0
        &&& forall|key: Major, left: int, right: int| 0 <= left < right < self.rows(key).len() ==>
            (#[trigger] self.rows(key)[left]).first != (#[trigger] self.rows(key)[right]).first
            || self.rows(key)[left].second != self.rows(key)[right].second
            || self.rows(key)[left].third != self.rows(key)[right].third
    }

    pub fn empty(size: usize) -> (result: Self)
        ensures result.valid(), forall|key: Major| #[trigger] result.rows(key) == Seq::<Cell>::empty(),
            forall|key: Major, minor: Major, volume: bool| #[trigger] result.at(key, minor, volume) == 0,
    {
        proof { reveal(Table::valid); }
        let page_count = size / 256 + 1;
        let mut pages = PageDirectory::new();
        let mut page_index = 0;
        while page_index < page_count
            invariant page_index <= page_count, pages@.len() == page_index,
                0 < page_count <= usize::MAX as int / 32,
                forall|page: int| 0 <= page < pages@.len() ==> (*#[trigger] pages@[page]).buckets@.len() == 32,
                forall|page: int, bucket: int| 0 <= page < pages@.len() && 0 <= bucket < 32 ==>
                    (*#[trigger] (*pages@[page]).buckets@[bucket]).values@ == IMap::<Major, Arc<Vec<Cell>>>::empty()
                    && (*(*pages@[page]).buckets@[bucket]).keys@ == Seq::<Major>::empty(),
            decreases page_count - page_index,
        {
            let mut buckets: Vec<Arc<Bucket>> = Vec::new();
            let mut bucket_index = 0;
            while bucket_index < 32
                invariant bucket_index <= 32, buckets.len() == bucket_index,
                    forall|bucket: int| 0 <= bucket < buckets@.len() ==>
                        (*#[trigger] buckets@[bucket]).values@ == IMap::<Major, Arc<Vec<Cell>>>::empty()
                        && (*buckets@[bucket]).keys@ == Seq::<Major>::empty(),
                decreases 32 - bucket_index,
            {
                buckets.push(Arc::new(Bucket { values: CompoundMap::new(), keys: Vec::new() }));
                bucket_index += 1;
            }
            pages.push(Arc::new(Page { buckets }));
            page_index += 1;
        }
        let result = Table { pages: Arc::new(pages) };
        proof {
            lemma_empty_directory((*result.pages)@);
            assert forall|key: Major| #[trigger] result.rows(key) == Seq::<Cell>::empty() by {
                let position = slot(key, pages@.len() as int * 32);
                assert(0 <= position < pages@.len() * 32);
            }
            assert forall|page: int, bucket: int| 0 <= page < (*result.pages)@.len() && 0 <= bucket < 32 implies
                bucket_valid(*#[trigger] (*(*result.pages)@[page]).buckets@[bucket], page * 32 + bucket, (*result.pages)@.len() as int * 32) by {
                let selected = (*(*result.pages)@[page]).buckets@[bucket];
                assert((*selected).values@ == IMap::<Major, Arc<Vec<Cell>>>::empty());
                assert((*selected).keys@ == Seq::<Major>::empty());
            }
            assert(result.keys_view() == Seq::<Major>::empty());
            reveal(Table::at);
            reveal(cell_sum);
        }
        result
    }

    pub fn share(&self) -> (result: Self)
        ensures result == *self,
    {
        Table { pages: self.pages.clone() }
    }

    pub fn replace(&self, key: Major, rows: Arc<Vec<Cell>>) -> (result: Self)
        requires self.valid(), rows_valid((*rows)@),
        ensures result.valid(), forall|other: Major| #[trigger] result.rows(other) ==
            if other == key { (*rows)@ } else { self.rows(other) },
    {
        proof { reveal(Table::valid); }
        let position = slot_exec(key, self.pages.len() * 32);
        let page_index = position / 32;
        let bucket_index = position % 32;
        let original_page = self.pages.get(page_index);
        let replacement = Arc::new(replace_bucket(&original_page.buckets[bucket_index], position, self.pages.len() * 32, key, rows));
        let mut buckets: Vec<Arc<Bucket>> = Vec::new();
        let mut index = 0;
        while index < 32
            invariant index <= 32, buckets.len() == index, page_index < (*self.pages)@.len(), bucket_index < 32,
                original_page == self.pages@[page_index as int], original_page.buckets@.len() == 32,
                self.pages@[page_index as int].buckets@.len() == 32,
                forall|offset: int| 0 <= offset < index ==> (*#[trigger] buckets@[offset]) ==
                    if offset == bucket_index { *replacement } else { *self.pages@[page_index as int].buckets@[offset] },
            decreases 32 - index,
        {
            if index == bucket_index { buckets.push(replacement.clone()); }
            else { buckets.push(original_page.buckets[index].clone()); }
            index += 1;
        }
        let page = Arc::new(Page { buckets });
        let mut pages = self.pages.share();
        pages.set(page_index, page);
        let result = Table { pages: Arc::new(pages) };
        proof {
            assert forall|other: Major| #[trigger] result.rows(other) ==
                if other == key { (*rows)@ } else { self.rows(other) } by {
                let other_position = slot(other, (*self.pages)@.len() as int * 32);
                if other_position == position {
                    assert((*result.pages)@[page_index as int].buckets@[bucket_index as int].values@ == (*replacement).values@);
                }
            }
        }
        result
    }

    pub fn get(&self, key: Major) -> (result: Arc<Vec<Cell>>)
        requires self.valid(),
        ensures (*result)@ == self.rows(key), rows_valid((*result)@),
    {
        proof { reveal(Table::valid); }
        let position = slot_exec(key, self.pages.len() * 32);
        let page = self.pages.get(position / 32);
        let bucket = &page.buckets[position % 32];
        proof {
            assert(position as int == slot(key, (*self.pages)@.len() as int * 32));
            assert(self.rows(key) == if bucket.values@.contains_key(key) { (*bucket.values@[key])@ } else { Seq::empty() });
            assert(rows_valid(self.rows(key)));
        }
        let result = match bucket.values.get(&key) {
            Some(rows) => {
                let result = rows.clone();
                proof { assert(*rows == bucket.values@[key]); assert((*result)@ == (*bucket.values@[key])@); }
                result
            },
            None => Arc::new(Vec::new()),
        };
        proof { assert((*result)@ == self.rows(key)); }
        result
    }

    pub fn lookup(&self, key: Major, minor: Major) -> (result: (i128, i128))
        requires self.valid(),
        ensures result.0 as int == self.at(key, minor, false), result.1 as int == self.at(key, minor, true),
    {
        proof { reveal(Table::at); reveal(cell_sum); }
        let rows = self.get(key);
        let mut index = 0;
        while index < rows.len()
            invariant index <= rows.len(), (*rows)@ == self.rows(key), rows_valid((*rows)@),
                forall|offset: int| 0 <= offset < index ==> minor_of(#[trigger] (*rows)@[offset]) != minor,
            decreases rows.len() - index,
        {
            let cell = rows[index];
            if (cell.first, cell.second, cell.third) == minor {
                proof { reveal(Table::at); lemma_cell_lookup((*rows)@, minor, index as int); }
                return (cell.count, cell.volume);
            }
            index += 1;
        }
        proof {
            reveal(Table::at);
            reveal(cell_sum);
            lemma_sum_zero((*rows)@, |cell: Cell| cell_weight(cell, minor, false));
            lemma_sum_zero((*rows)@, |cell: Cell| cell_weight(cell, minor, true));
        }
        (0, 0)
    }

    pub fn keys(&self) -> (result: Vec<Major>)
        requires self.valid(),
        ensures result@ == self.keys_view(), result@.no_duplicates(),
            forall|key: Major| #[trigger] result@.contains(key) <==> self.rows(key).len() > 0,
    {
        proof { reveal(Table::valid); }
        let mut result = Vec::new();
        let mut page_index = 0;
        while page_index < self.pages.len()
            invariant self.valid(), page_index <= (*self.pages)@.len(),
                result@ == flat((*self.pages)@.take(page_index as int).map_values(|page: Arc<Page>| page_keys(*page))),
            decreases (*self.pages)@.len() - page_index,
        {
            let mut bucket_index = 0;
            let ghost preceding_pages = result@;
            while bucket_index < 32
                invariant self.valid(), page_index < (*self.pages)@.len(), bucket_index <= 32,
                    self.pages@[page_index as int].buckets@.len() == 32,
                    preceding_pages == flat((*self.pages)@.take(page_index as int).map_values(|page: Arc<Page>| page_keys(*page))),
                    result@ == preceding_pages + flat(self.pages@[page_index as int].buckets@.take(bucket_index as int).map_values(|bucket: Arc<Bucket>| (*bucket).keys@)),
                decreases 32 - bucket_index,
            {
                let page = self.pages.get(page_index);
                let bucket = &page.buckets[bucket_index];
                let mut index = 0;
                let ghost preceding_buckets = result@;
                while index < bucket.keys.len()
                    invariant self.valid(), page_index < (*self.pages)@.len(), bucket_index < 32,
                        self.pages@[page_index as int].buckets@.len() == 32,
                        *bucket == self.pages@[page_index as int].buckets@[bucket_index as int],
                        preceding_pages == flat((*self.pages)@.take(page_index as int).map_values(|page: Arc<Page>| page_keys(*page))),
                        preceding_buckets == preceding_pages + flat(self.pages@[page_index as int].buckets@.take(bucket_index as int).map_values(|bucket: Arc<Bucket>| (*bucket).keys@)),
                        index <= bucket.keys.len(), result@ == preceding_buckets + bucket.keys@.take(index as int),
                    decreases bucket.keys.len() - index,
                {
                    result.push(bucket.keys[index]);
                    proof {
                        assert(bucket.keys@.take(index as int + 1) =~= bucket.keys@.take(index as int).push(bucket.keys@[index as int]));
                    }
                    index += 1;
                }
                proof {
                    let buckets = self.pages@[page_index as int].buckets@;
                    assert(buckets.take(bucket_index as int + 1) =~= buckets.take(bucket_index as int).push(buckets[bucket_index as int]));
                    assert(buckets.take(bucket_index as int + 1).map_values(|bucket: Arc<Bucket>| (*bucket).keys@)
                        =~= buckets.take(bucket_index as int).map_values(|bucket: Arc<Bucket>| (*bucket).keys@).push(bucket.keys@));
                    assert(buckets.take(bucket_index as int + 1).map_values(|bucket: Arc<Bucket>| (*bucket).keys@).drop_last()
                        =~= buckets.take(bucket_index as int).map_values(|bucket: Arc<Bucket>| (*bucket).keys@));
                }
                bucket_index += 1;
            }
            proof {
                assert((*self.pages)@.take(page_index as int + 1) =~= (*self.pages)@.take(page_index as int).push((*self.pages)@[page_index as int]));
                assert((*self.pages)@.take(page_index as int + 1).map_values(|page: Arc<Page>| page_keys(*page))
                    =~= (*self.pages)@.take(page_index as int).map_values(|page: Arc<Page>| page_keys(*page)).push(page_keys(*(*self.pages)@[page_index as int])));
                assert((*self.pages)@.take(page_index as int + 1).map_values(|page: Arc<Page>| page_keys(*page)).drop_last()
                    =~= (*self.pages)@.take(page_index as int).map_values(|page: Arc<Page>| page_keys(*page)));
            }
            page_index += 1;
        }
        proof {
            assert((*self.pages)@.take((*self.pages)@.len() as int) =~= (*self.pages)@);
            assert(result@ == self.keys_view());
            lemma_keys_complete(*self);
            lemma_keys_unique(*self);
            assert forall|key: Major| #[trigger] result@.contains(key) <==> self.rows(key).len() > 0 by {
                assert(self.keys_view().contains(key) == result@.contains(key));
            }
        }
        result
    }
}

fn replace_bucket(bucket: &Bucket, position: usize, slots: usize, key: Major, rows: Arc<Vec<Cell>>) -> (result: Bucket)
    requires bucket_valid(*bucket, position as int, slots as int), slot(key, slots as int) == position as int, rows_valid((*rows)@),
    ensures bucket_valid(result, position as int, slots as int),
        result.values@ == if (*rows)@.len() == 0 { bucket.values@.remove(key) } else { bucket.values@.insert(key, rows) },
{
    let mut values = CompoundMap::new();
    let mut keys = Vec::new();
    let mut index = 0;
    while index < bucket.keys.len()
        invariant index <= bucket.keys.len(), bucket_valid(*bucket, position as int, slots as int),
            forall|other: Major| #[trigger] values@.contains_key(other) <==> other != key && bucket.keys@.take(index as int).contains(other),
            forall|other: Major| #[trigger] values@.contains_key(other) ==> values@[other] == bucket.values@[other],
            forall|other: Major| #[trigger] values@.contains_key(other) <==> keys@.contains(other),
            forall|left: int, right: int| 0 <= left < right < keys@.len() ==> #[trigger] keys@[left] != #[trigger] keys@[right],
            forall|offset: int| 0 <= offset < keys@.len() ==> slot(#[trigger] keys@[offset], slots as int) == position as int,
        decreases bucket.keys.len() - index,
    {
        let other = bucket.keys[index];
        let ghost previous_values = values@;
        let ghost previous_keys = keys@;
        if other != key {
            proof { assert(bucket.values@.contains_key(other)); }
            if let Some(value) = bucket.values.get(&other) { values.insert(other, value.clone()); keys.push(other); }
        }
        proof {
            assert(bucket.keys@.take(index as int + 1) =~= bucket.keys@.take(index as int).push(other));
            assert forall|candidate: Major| #[trigger] values@.contains_key(candidate) <==>
                candidate != key && bucket.keys@.take(index as int + 1).contains(candidate) by {
                if candidate == other {
                    assert(bucket.keys@.take(index as int + 1).contains(candidate));
                } else {
                    assert(values@.contains_key(candidate) == previous_values.contains_key(candidate));
                    assert(bucket.keys@.take(index as int + 1).contains(candidate) == bucket.keys@.take(index as int).contains(candidate));
                }
            }
            assert forall|candidate: Major| #[trigger] values@.contains_key(candidate) <==> keys@.contains(candidate) by {
                if candidate != other {
                    assert(values@.contains_key(candidate) == previous_values.contains_key(candidate));
                    assert(keys@.contains(candidate) == previous_keys.contains(candidate));
                }
            }
        }
        index += 1;
    }
    if rows.len() > 0 { values.insert(key, rows); keys.push(key); }
    let result = Bucket { values, keys };
    proof {
        assert(result.values@ =~= if (*rows)@.len() == 0 { bucket.values@.remove(key) } else { bucket.values@.insert(key, rows) });
    }
    result
}

pub proof fn lemma_keys_complete(table: Table)
    requires table.valid(),
    ensures forall|key: Major| #[trigger] table.keys_view().contains(key) <==> table.rows(key).len() > 0,
{
    reveal(Table::valid);
    let pages = (*table.pages)@;
    assert forall|key: Major| #[trigger] table.keys_view().contains(key) <==> table.rows(key).len() > 0 by {
        let page_sets = pages.map_values(|page: Arc<Page>| page_keys(*page));
        lemma_flat_contains(page_sets, key);
        let position = slot(key, pages.len() as int * 32);
        if table.keys_view().contains(key) {
            let page_index = choose|page_index: int| 0 <= page_index < page_sets.len() && (#[trigger] page_sets[page_index]).contains(key);
            let buckets = (*pages[page_index]).buckets@;
            let bucket_sets = buckets.map_values(|bucket: Arc<Bucket>| (*bucket).keys@);
            lemma_flat_contains(bucket_sets, key);
            let bucket_index = choose|bucket_index: int| 0 <= bucket_index < bucket_sets.len() && (#[trigger] bucket_sets[bucket_index]).contains(key);
            let bucket = *buckets[bucket_index];
            let offset = choose|offset: int| 0 <= offset < bucket.keys@.len() && #[trigger] bucket.keys@[offset] == key;
            assert(position == page_index * 32 + bucket_index);
            assert(bucket.values@.contains_key(key));
        } else if table.rows(key).len() > 0 {
            let buckets = (*pages[position / 32]).buckets@;
            let bucket = *buckets[position % 32];
            assert(bucket.keys@.contains(key));
            let bucket_sets = buckets.map_values(|bucket: Arc<Bucket>| (*bucket).keys@);
            lemma_flat_contains(bucket_sets, key);
            assert(bucket_sets[position % 32].contains(key));
            assert(page_sets[position / 32].contains(key));
        }
    }
}

proof fn lemma_page_key_position(table: Table, page_index: int, key: Major)
    requires table.valid(), 0 <= page_index < (*table.pages)@.len(), page_keys(*(*table.pages)@[page_index]).contains(key),
    ensures slot(key, (*table.pages)@.len() as int * 32) / 32 == page_index,
{
    let buckets = (*(*table.pages)@[page_index]).buckets@;
    let bucket_sets = buckets.map_values(|bucket: Arc<Bucket>| (*bucket).keys@);
    lemma_flat_contains(bucket_sets, key);
    let bucket_index = choose|bucket_index: int| 0 <= bucket_index < bucket_sets.len() && (#[trigger] bucket_sets[bucket_index]).contains(key);
    let bucket = *buckets[bucket_index];
    let offset = choose|offset: int| 0 <= offset < bucket.keys@.len() && #[trigger] bucket.keys@[offset] == key;
    assert(slot(key, (*table.pages)@.len() as int * 32) == page_index * 32 + bucket_index);
}

pub proof fn lemma_keys_unique(table: Table)
    requires table.valid(),
    ensures table.keys_view().no_duplicates(),
{
    let pages = (*table.pages)@;
    let page_sets = pages.map_values(|page: Arc<Page>| page_keys(*page));
    assert forall|page_index: int| 0 <= page_index < page_sets.len() implies (#[trigger] page_sets[page_index]).no_duplicates() by {
        let buckets = (*pages[page_index]).buckets@;
        let bucket_sets = buckets.map_values(|bucket: Arc<Bucket>| (*bucket).keys@);
        assert forall|bucket_index: int| 0 <= bucket_index < bucket_sets.len() implies (#[trigger] bucket_sets[bucket_index]).no_duplicates() by {
            assert(bucket_valid(*buckets[bucket_index], page_index * 32 + bucket_index, pages.len() as int * 32));
        }
        assert forall|left: int, right: int, value: Major| #![trigger bucket_sets[left].contains(value), bucket_sets[right].contains(value)]
            0 <= left < right < bucket_sets.len() implies !(bucket_sets[left].contains(value) && bucket_sets[right].contains(value)) by {
            if bucket_sets[left].contains(value) && bucket_sets[right].contains(value) {
                let left_keys = (*buckets[left]).keys@;
                let right_keys = (*buckets[right]).keys@;
                let left_offset = choose|offset: int| 0 <= offset < left_keys.len() && #[trigger] left_keys[offset] == value;
                let right_offset = choose|offset: int| 0 <= offset < right_keys.len() && #[trigger] right_keys[offset] == value;
                assert(slot(value, pages.len() as int * 32) == page_index * 32 + left);
                assert(slot(value, pages.len() as int * 32) == page_index * 32 + right);
            }
        }
        verus_prooflib::lemma_flat_unique(bucket_sets);
    }
    assert forall|left: int, right: int, value: Major| #![trigger page_sets[left].contains(value), page_sets[right].contains(value)]
        0 <= left < right < page_sets.len() implies !(page_sets[left].contains(value) && page_sets[right].contains(value)) by {
        if page_sets[left].contains(value) && page_sets[right].contains(value) {
            lemma_page_key_position(table, left, value);
            lemma_page_key_position(table, right, value);
        }
    }
    verus_prooflib::lemma_flat_unique(page_sets);
}

pub open spec fn page_keys(page: Page) -> Seq<Major> {
    flat(page.buckets@.map_values(|bucket: Arc<Bucket>| (*bucket).keys@))
}

proof fn lemma_empty_buckets(buckets: Seq<Arc<Bucket>>)
    requires forall|index: int| 0 <= index < buckets.len() ==> (*#[trigger] buckets[index]).keys@ == Seq::<Major>::empty(),
    ensures flat(buckets.map_values(|bucket: Arc<Bucket>| (*bucket).keys@)) == Seq::<Major>::empty(),
    decreases buckets.len(),
{
    if buckets.len() > 0 {
        lemma_empty_buckets(buckets.drop_last());
        assert(buckets.map_values(|bucket: Arc<Bucket>| (*bucket).keys@).drop_last() =~= buckets.drop_last().map_values(|bucket: Arc<Bucket>| (*bucket).keys@));
    }
}

proof fn lemma_empty_directory(pages: Seq<Arc<Page>>)
    requires forall|page: int, bucket: int| 0 <= page < pages.len() && 0 <= bucket < (*pages[page]).buckets@.len() ==>
        (*#[trigger] (*pages[page]).buckets@[bucket]).keys@ == Seq::<Major>::empty(),
    ensures flat(pages.map_values(|page: Arc<Page>| page_keys(*page))) == Seq::<Major>::empty(),
    decreases pages.len(),
{
    if pages.len() > 0 {
        lemma_empty_directory(pages.drop_last());
        lemma_empty_buckets((*pages.last()).buckets@);
        assert(pages.map_values(|page: Arc<Page>| page_keys(*page)).drop_last() =~= pages.drop_last().map_values(|page: Arc<Page>| page_keys(*page)));
    }
}

pub proof fn lemma_cell_lookup(rows: Seq<Cell>, minor: Major, index: int)
    requires rows_valid(rows), 0 <= index < rows.len(), minor_of(rows[index]) == minor,
    ensures cell_sum(rows, minor, false) == rows[index].count as int, cell_sum(rows, minor, true) == rows[index].volume as int,
{
    reveal(cell_sum);
    assert forall|offset: int| 0 <= offset < rows.len() && offset != index implies minor_of(#[trigger] rows[offset]) != minor by {
        if offset < index { assert(minor_of(rows[offset]) != minor_of(rows[index])); }
        else { assert(minor_of(rows[index]) != minor_of(rows[offset])); }
    }
    lemma_sum_single_match(rows, |cell: Cell| cell_weight(cell, minor, false), index);
    lemma_sum_single_match(rows, |cell: Cell| cell_weight(cell, minor, true), index);
}

pub struct Work {
    pub base: Table,
    pub changes: CompoundMap<Vec<Cell>>,
    pub keys: Vec<Major>,
}

pub fn copy_cells(source: &Vec<Cell>) -> (result: Vec<Cell>)
    ensures result@ == source@,
{
    let mut result = Vec::new();
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    {
        result.push(source[index]);
        index += 1;
    }
    result
}

impl Work {
    pub open spec fn valid(self) -> bool {
        &&& self.base.valid()
        &&& forall|key: Major| #[trigger] self.changes@.contains_key(key) <==> self.keys@.contains(key)
        &&& forall|key: Major| #[trigger] self.changes@.contains_key(key) ==> rows_valid(self.changes@[key]@)
        &&& forall|left: int, right: int| 0 <= left < right < self.keys@.len() ==>
            #[trigger] self.keys@[left] != #[trigger] self.keys@[right]
    }

    pub open spec fn rows(self, key: Major) -> Seq<Cell> {
        if self.changes@.contains_key(key) { self.changes@[key]@ } else { self.base.rows(key) }
    }

    #[verifier::opaque]
    pub open spec fn at(self, key: Major, minor: Major, volume: bool) -> int {
        cell_sum(self.rows(key), minor, volume)
    }

    pub fn new(base: &Table) -> (result: Self)
        requires base.valid(),
        ensures result.valid(), forall|key: Major| #[trigger] result.rows(key) == base.rows(key),
            forall|key: Major, minor: Major, volume: bool| #[trigger] result.at(key, minor, volume) == base.at(key, minor, volume),
    {
        proof { reveal(Work::valid); reveal(Work::at); }
        let result = Work { base: base.share(), changes: CompoundMap::new(), keys: Vec::new() };
        proof { reveal(Work::at); reveal(Table::at); }
        result
    }

    pub fn add(&mut self, key: Major, minor: Major, count: i128, volume: i128) -> (result: Option<()>)
        requires old(self).valid(),
        ensures result is Some ==> final(self).valid(),
            result is Some ==> forall|other: Major, small: Major, measure: bool|
            #[trigger] final(self).at(other, small, measure) == old(self).at(other, small, measure)
                + if other == key && small == minor { if measure { volume as int } else { count as int } } else { 0 },
    {
        proof { reveal(Work::valid); }
        if count == 0 && volume == 0 { return Some(()); }
        let ghost before = *self;
        let ghost present = self.changes@.contains_key(key);
        let rows = match self.changes.remove(&key) {
            Some(rows) => rows,
            None => { self.keys.push(key); copy_cells(&self.base.get(key)) },
        };
        proof {
            assert(self.keys@ == if present { before.keys@ } else { before.keys@.push(key) });
            if present { assert(before.keys@.contains(key)); }
            assert(self.keys@.contains(key));
        }
        let rows = add_cells(rows, minor, count, volume)?;
        self.changes.insert(key, rows);
        proof {
            assert forall|other: Major| #[trigger] self.changes@.contains_key(other) <==> self.keys@.contains(other) by {
                if other != key {
                    assert(self.changes@.contains_key(other) == before.changes@.contains_key(other));
                    assert(self.keys@.contains(other) == before.keys@.contains(other));
                }
            }
            assert forall|other: Major, small: Major, measure: bool|
                #[trigger] self.at(other, small, measure) == before.at(other, small, measure)
                    + if other == key && small == minor { if measure { volume as int } else { count as int } } else { 0 } by {
                reveal(Work::at);
                if other != key { assert(self.rows(other) == before.rows(other)); }
            }
        }
        Some(())
    }

    pub fn finish(self) -> (result: Table)
        requires self.valid(),
        ensures result.valid(), forall|key: Major| #[trigger] result.rows(key) == self.rows(key),
            forall|key: Major, minor: Major, measure: bool| #[trigger] result.at(key, minor, measure) == self.at(key, minor, measure),
    {
        proof { reveal(Work::valid); }
        let ghost original = self;
        let mut work = self;
        let mut result = work.base.share();
        let mut index = 0;
        while index < work.keys.len()
            invariant
                index <= work.keys.len(), result.valid(), original.valid(), work.keys@ == original.keys@,
                forall|key: Major| #[trigger] work.changes@.contains_key(key) <==>
                    original.changes@.contains_key(key) && !original.keys@.take(index as int).contains(key),
                forall|key: Major| #[trigger] work.changes@.contains_key(key) ==> work.changes@[key]@ == original.changes@[key]@,
                forall|key: Major| #[trigger] result.rows(key) == if original.keys@.take(index as int).contains(key) {
                    original.rows(key)
                } else { original.base.rows(key) },
            decreases work.keys.len() - index,
        {
            let key = work.keys[index];
            proof {
                assert(original.changes@.contains_key(key));
                assert(!original.keys@.take(index as int).contains(key));
            }
            let rows = match work.changes.remove(&key) {
                Some(rows) => rows,
                None => { proof { assert(false); } return result; },
            };
            result = result.replace(key, Arc::new(rows));
            proof {
                assert(original.keys@.take(index as int + 1) =~= original.keys@.take(index as int).push(key));
                assert forall|other: Major| #[trigger] work.changes@.contains_key(other) <==>
                    original.changes@.contains_key(other) && !original.keys@.take(index as int + 1).contains(other) by {
                    if other == key { assert(!work.changes@.contains_key(other)); }
                }
                assert forall|other: Major| #[trigger] result.rows(other) == if original.keys@.take(index as int + 1).contains(other) {
                    original.rows(other)
                } else { original.base.rows(other) } by {
                    if other == key { assert(result.rows(other) == original.rows(other)); }
                }
            }
            index += 1;
        }
        proof {
            assert forall|key: Major| #[trigger] result.rows(key) == original.rows(key) by {
                if original.changes@.contains_key(key) { assert(original.keys@.contains(key)); }
            }
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] result.at(key, minor, measure) == original.at(key, minor, measure) by {
                reveal(Table::at); reveal(Work::at);
                assert(result.rows(key) == original.rows(key));
            }
        }
        result
    }
}

pub open spec fn bucket_valid(bucket: Bucket, position: int, slots: int) -> bool {
    &&& forall|key: Major| #[trigger] bucket.values@.contains_key(key) <==> bucket.keys@.contains(key)
    &&& forall|index: int| 0 <= index < bucket.keys@.len() ==>
        slot(#[trigger] bucket.keys@[index], slots) == position
    &&& forall|left: int, right: int| 0 <= left < right < bucket.keys@.len() ==>
        #[trigger] bucket.keys@[left] != #[trigger] bucket.keys@[right]
    &&& forall|key: Major| #[trigger] bucket.values@.contains_key(key) ==> (*bucket.values@[key])@.len() > 0
}

}
