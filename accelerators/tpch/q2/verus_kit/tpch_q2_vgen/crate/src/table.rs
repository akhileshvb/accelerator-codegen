use vstd::prelude::*;
use vstd::hash_map::HashMapWithView as HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::rel::*;

verus! {

broadcast use { vstd::std_specs::hash::group_hash_axioms, vstd::seq_lib::group_seq_properties };

pub type Major = (i64, i64, i64);

pub open spec fn fits_i128(value: int) -> bool {
    -170141183460469231731687303715884105728 <= value <= 170141183460469231731687303715884105727
}

/// What stops an addition to a minor: its total count or volume would leave the i128 range.
pub open spec fn add_overflows(count_total: int, volume_total: int, count: i128, volume: i128) -> bool {
    !(fits_i128(count_total + count as int) && fits_i128(volume_total + volume as int))
}

#[derive(Clone, Copy)]
pub struct Cell {
    pub first: i64,
    pub second: i64,
    pub third: i64,
    pub count: i128,
    pub volume: i128,
}

/// A bucket of a table: a few keys with their rows laid end to end in one flat cell vector
/// (`ends[i]` is where the rows of `keys[i]` stop; they start where the previous key's stop).
/// A persistent version shares whole buckets; a commit copies the buckets it touches.
pub struct Flat {
    pub keys: Vec<Major>,
    pub ends: Vec<usize>,
    pub cells: Vec<Cell>,
}

/// A cell of a dimension row: (first, second, 0) once, volume 0.
#[derive(Clone, Copy)]
pub struct Pair {
    pub first: i64,
    pub second: i64,
}

/// A bucket of a table, shared between versions: a few keys with their rows laid end to end
/// (`ends[i]` is where the rows of `keys[i]` stop; they start where the previous key's stop).
/// The cells are either stored whole or, when every one is (first, second, 0) once with volume 0
/// (a dimension table's), as pairs: 16 bytes a cell instead of 64.
pub struct Bucket {
    pub keys: Vec<Major>,
    pub ends: Vec<usize>,
    pub cells: Vec<Cell>,
    pub pairs: Vec<Pair>,
    pub packed: bool,
}

/// A bucket opened for updates: one row vector per key.  Owned by one `Work`.
pub struct Draft {
    pub keys: Vec<Major>,
    pub rows: Vec<Vec<Cell>>,
}

pub struct Page {
    pub buckets: Vec<Arc<Bucket>>,
}

pub struct PageDirectory {
    entries: Vec<Arc<Page>>,
}

impl View for PageDirectory {
    type V = Seq<Arc<Page>>;
    closed spec fn view(&self) -> Seq<Arc<Page>> { self.entries@ }
}

impl PageDirectory {
    pub fn new() -> (result: Self)
        ensures result@ == Seq::<Arc<Page>>::empty(),
    {
        PageDirectory { entries: Vec::new() }
    }

    pub fn len(&self) -> (result: usize)
        ensures result as int == self@.len(),
    {
        self.entries.len()
    }

    pub fn get(&self, position: usize) -> (result: Arc<Page>)
        requires position < self@.len(),
        ensures result == self@[position as int],
    {
        self.entries[position].clone()
    }

    pub fn share(&self) -> (result: Self)
        ensures result@ == self@,
    {
        let mut entries: Vec<Arc<Page>> = Vec::with_capacity(self.entries.len());
        let mut index = 0;
        while index < self.entries.len()
            invariant index <= self.entries.len(), entries@ == self.entries@.take(index as int),
            decreases self.entries.len() - index,
        {
            entries.push(self.entries[index].clone());
            proof { assert(self.entries@.take(index as int + 1) =~= self.entries@.take(index as int).push(self.entries@[index as int])); }
            index += 1;
        }
        proof { assert(self.entries@.take(index as int) =~= self.entries@); }
        PageDirectory { entries }
    }

    pub fn push(&mut self, page: Arc<Page>)
        requires old(self)@.len() < i64::MAX as int, old(self)@.len() < usize::MAX as int,
        ensures final(self)@ == old(self)@.push(page),
    {
        self.entries.push(page);
    }

    pub fn set(&mut self, position: usize, page: Arc<Page>)
        requires position < old(self)@.len(),
        ensures final(self)@ == old(self)@.update(position as int, page),
    {
        self.entries.set(position, page);
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
    ensures result is None ==> add_overflows(cell_sum(rows@, minor, false), cell_sum(rows@, minor, true), count, volume),
        result matches Some(after) ==> rows_valid(after@)
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
            proof { lemma_cell_lookup(before, minor, index as int); }
            let new_count = match cell.count.checked_add(count) { Some(value) => value, None => return None };
            let new_volume = match cell.volume.checked_add(volume) { Some(value) => value, None => return None };
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

pub open spec fn seq_start(ends: Seq<usize>, i: int) -> int {
    if i <= 0 { 0int } else { ends[i - 1] as int }
}

pub open spec fn seq_row(ends: Seq<usize>, cells: Seq<Cell>, i: int) -> Seq<Cell> {
    cells.subrange(seq_start(ends, i), ends[i] as int)
}

pub open spec fn keys_unique(keys: Seq<Major>) -> bool {
    forall|left: int, right: int| 0 <= left < right < keys.len() ==> #[trigger] keys[left] != #[trigger] keys[right]
}

#[verifier::opaque]
pub open spec fn flat_valid(keys: Seq<Major>, ends: Seq<usize>, cells: Seq<Cell>, position: int, slots: int) -> bool {
    &&& keys.len() == ends.len()
    &&& (ends.len() == 0 ==> cells.len() == 0)
    &&& (ends.len() > 0 ==> ends[ends.len() - 1] as int == cells.len())
    &&& forall|i: int| 0 <= i < ends.len() ==> seq_start(ends, i) < (#[trigger] ends[i]) as int && ends[i] as int <= cells.len()
    &&& forall|i: int| 0 <= i < keys.len() ==> slot(#[trigger] keys[i], slots) == position
    &&& keys_unique(keys)
    &&& forall|i: int| #![trigger seq_row(ends, cells, i)] 0 <= i < ends.len() ==> rows_valid(seq_row(ends, cells, i))
}

pub open spec fn flat_ok(bucket: Flat, position: int, slots: int) -> bool {
    flat_valid(bucket.keys@, bucket.ends@, bucket.cells@, position, slots)
}

pub open spec fn draft_valid(keys: Seq<Major>, rows: Seq<Vec<Cell>>, position: int, slots: int) -> bool {
    &&& keys.len() == rows.len()
    &&& forall|i: int| 0 <= i < keys.len() ==> slot(#[trigger] keys[i], slots) == position
    &&& keys_unique(keys)
    &&& forall|i: int| 0 <= i < rows.len() ==> (#[trigger] rows[i])@.len() > 0 && rows_valid(rows[i]@)
}

pub open spec fn keyed_index(keys: Seq<Major>, key: Major) -> int {
    choose|i: int| 0 <= i < keys.len() && keys[i] == key
}

pub open spec fn rows_in(keys: Seq<Major>, rows: Seq<Vec<Cell>>, key: Major) -> Seq<Cell> {
    if keys.contains(key) { rows[keyed_index(keys, key)]@ } else { Seq::empty() }
}

pub open spec fn draft_rows(draft: Draft, key: Major) -> Seq<Cell> {
    rows_in(draft.keys@, draft.rows@, key)
}

pub open spec fn flat_rows(bucket: Flat, key: Major) -> Seq<Cell> {
    if bucket.keys@.contains(key) { seq_row(bucket.ends@, bucket.cells@, keyed_index(bucket.keys@, key)) } else { Seq::empty() }
}

pub proof fn lemma_keyed_index(keys: Seq<Major>, i: int)
    requires 0 <= i < keys.len(), keys_unique(keys),
    ensures keys.contains(keys[i]), keyed_index(keys, keys[i]) == i,
{
    assert(keys[i] == keys[i]);
    let j = keyed_index(keys, keys[i]);
    assert(0 <= j < keys.len() && keys[j] == keys[i]);
    if j < i { assert(keys[j] != keys[i]); }
    if i < j { assert(keys[i] != keys[j]); }
}

pub open spec fn flat_val(bucket: &Flat) -> Flat {
    *bucket
}

pub open spec fn replacement_flat_spec(bucket: &Flat) -> Flat {
    *bucket
}

pub proof fn lemma_flat_facts(bucket: Flat, position: int, slots: int)
    requires flat_ok(bucket, position, slots),
    ensures bucket.keys@.len() == bucket.ends@.len(), keys_unique(bucket.keys@),
        bucket.ends@.len() == 0 ==> bucket.cells@.len() == 0,
        bucket.ends@.len() > 0 ==> bucket.ends@[bucket.ends@.len() - 1] as int == bucket.cells@.len(),
        forall|i: int| 0 <= i < bucket.ends@.len() ==> seq_start(bucket.ends@, i) < (#[trigger] bucket.ends@[i]) as int && bucket.ends@[i] as int <= bucket.cells@.len(),
        forall|i: int| 0 <= i < bucket.keys@.len() ==> slot(#[trigger] bucket.keys@[i], slots) == position,
        forall|i: int| #![trigger seq_row(bucket.ends@, bucket.cells@, i)] 0 <= i < bucket.ends@.len() ==> rows_valid(seq_row(bucket.ends@, bucket.cells@, i)),
{
    reveal(flat_valid);
}

pub proof fn lemma_flat_rows_valid(bucket: Flat, position: int, slots: int, key: Major)
    requires flat_ok(bucket, position, slots),
    ensures rows_valid(flat_rows(bucket, key)), flat_rows(bucket, key).len() > 0 <==> bucket.keys@.contains(key),
        bucket.keys@.contains(key) ==> slot(key, slots) == position,
{
    reveal(flat_valid);
    if bucket.keys@.contains(key) {
        let i = keyed_index(bucket.keys@, key);
        assert(0 <= i < bucket.keys@.len() && bucket.keys@[i] == key);
        let row = seq_row(bucket.ends@, bucket.cells@, i);
        assert(rows_valid(row));
        assert(seq_start(bucket.ends@, i) < bucket.ends@[i] as int);
        assert(row.len() == bucket.ends@[i] as int - seq_start(bucket.ends@, i));
    } else {
        assert(flat_rows(bucket, key) =~= Seq::<Cell>::empty());
        assert(rows_valid(Seq::<Cell>::empty()));
    }
}

pub proof fn lemma_flat_row_at(bucket: Flat, position: int, slots: int, i: int)
    requires flat_ok(bucket, position, slots), 0 <= i < bucket.keys@.len(),
    ensures flat_rows(bucket, bucket.keys@[i]) == seq_row(bucket.ends@, bucket.cells@, i),
{
    reveal(flat_valid);
    lemma_keyed_index(bucket.keys@, i);
}

pub proof fn lemma_draft_row_at(keys: Seq<Major>, rows: Seq<Vec<Cell>>, i: int)
    requires 0 <= i < keys.len(), keys.len() == rows.len(), keys_unique(keys),
    ensures rows_in(keys, rows, keys[i]) == rows[i]@,
{
    lemma_keyed_index(keys, i);
}

/// A bucket and a draft with the same keys and the same row for each key give every key the same rows.
pub proof fn lemma_rows_agree(bucket: Flat, draft: Draft)
    requires bucket.keys@ == draft.keys@, bucket.ends@.len() == bucket.keys@.len(), draft.rows@.len() == draft.keys@.len(),
        forall|i: int| 0 <= i < bucket.keys@.len() ==> seq_row(bucket.ends@, bucket.cells@, i) == (#[trigger] draft.rows@[i])@,
    ensures forall|key: Major| #[trigger] flat_rows(bucket, key) == draft_rows(draft, key),
{
    assert forall|key: Major| #[trigger] flat_rows(bucket, key) == draft_rows(draft, key) by {
        if bucket.keys@.contains(key) {
            let i = keyed_index(bucket.keys@, key);
            assert(0 <= i < bucket.keys@.len());
        }
    }
}

pub fn copy_range(cells: &Vec<Cell>, start: usize, end: usize) -> (result: Vec<Cell>)
    requires start <= end <= cells.len(),
    ensures result@ == cells@.subrange(start as int, end as int),
{
    let mut result = Vec::with_capacity(end - start);
    let mut offset = start;
    while offset < end
        invariant start <= offset <= end, end <= cells.len(), result@ == cells@.subrange(start as int, offset as int),
        decreases end - offset,
    {
        result.push(cells[offset]);
        proof { assert(cells@.subrange(start as int, offset as int + 1) =~= cells@.subrange(start as int, offset as int).push(cells@[offset as int])); }
        offset += 1;
    }
    result
}

fn row_start(ends: &Vec<usize>, i: usize) -> (result: usize)
    requires i < ends.len(),
    ensures result as int == seq_start(ends@, i as int),
{
    if i == 0 { 0 } else { ends[i - 1] }
}

/// Open a bucket for updates.
pub fn draft_of(bucket: &Flat, Ghost(position): Ghost<int>, Ghost(slots): Ghost<int>) -> (result: Draft)
    requires flat_ok(*bucket, position, slots),
    ensures draft_valid(result.keys@, result.rows@, position, slots), result.keys@ == bucket.keys@,
        forall|i: int| 0 <= i < bucket.keys@.len() ==> (#[trigger] result.rows@[i])@ == seq_row(bucket.ends@, bucket.cells@, i),
{
    proof { reveal(flat_valid); }
    let mut keys: Vec<Major> = Vec::with_capacity(bucket.keys.len());
    let mut rows: Vec<Vec<Cell>> = Vec::with_capacity(bucket.keys.len());
    let mut i = 0;
    while i < bucket.keys.len()
        invariant i <= bucket.keys.len(), flat_ok(*bucket, position, slots), keys@ == bucket.keys@.take(i as int),
            rows@.len() == i,
            forall|j: int| 0 <= j < i ==> (#[trigger] rows@[j])@ == seq_row(bucket.ends@, bucket.cells@, j),
        decreases bucket.keys.len() - i,
    {
        proof { lemma_flat_facts(flat_val(bucket), position, slots); }
        let start = row_start(&bucket.ends, i);
        let end = bucket.ends[i];
        let row = copy_range(&bucket.cells, start, end);
        proof { assert(row@ == seq_row(bucket.ends@, bucket.cells@, i as int)); }
        keys.push(bucket.keys[i]);
        rows.push(row);
        proof { assert(bucket.keys@.take(i as int + 1) =~= bucket.keys@.take(i as int).push(bucket.keys@[i as int])); }
        i += 1;
    }
    proof {
        assert(bucket.keys@.take(bucket.keys@.len() as int) =~= bucket.keys@);
        lemma_flat_facts(flat_val(bucket), position, slots);
        assert forall|j: int| 0 <= j < rows@.len() implies (#[trigger] rows@[j])@.len() > 0 && rows_valid(rows@[j]@) by {
            assert(rows_valid(seq_row(bucket.ends@, bucket.cells@, j)));
            assert(seq_start(bucket.ends@, j) < bucket.ends@[j] as int);
        }
    }
    Draft { keys, rows }
}

/// Close a draft into a bucket (exactly sized).
pub fn flat_of(draft: &Draft, Ghost(position): Ghost<int>, Ghost(slots): Ghost<int>) -> (result: Flat)
    requires draft_valid(draft.keys@, draft.rows@, position, slots),
    ensures flat_ok(result, position, slots), result.keys@ == draft.keys@,
        forall|i: int| 0 <= i < draft.keys@.len() ==> seq_row(result.ends@, result.cells@, i) == (#[trigger] draft.rows@[i])@,
{
    proof { reveal(flat_valid); }
    let n = draft.keys.len();
    let mut hint: usize = 0;
    let mut i = 0;
    while i < n
        invariant i <= n, n == draft.keys.len(), n == draft.rows.len(),
        decreases n - i,
    {
        hint = match hint.checked_add(draft.rows[i].len()) { Some(sum) => sum, None => hint };
        i += 1;
    }
    let mut keys: Vec<Major> = Vec::with_capacity(n);
    let mut ends: Vec<usize> = Vec::with_capacity(n);
    let mut cells: Vec<Cell> = Vec::with_capacity(hint);
    i = 0;
    while i < n
        invariant i <= n, n == draft.keys.len(), n == draft.rows.len(),
            draft_valid(draft.keys@, draft.rows@, position, slots),
            keys@ == draft.keys@.take(i as int), ends@.len() == i, 
            (i == 0 ==> cells@.len() == 0), (i > 0 ==> ends@[i as int - 1] as int == cells@.len()),
            forall|j: int| 0 <= j < i ==> seq_start(ends@, j) < (#[trigger] ends@[j]) as int && ends@[j] as int <= cells@.len(),
            forall|j: int| 0 <= j < i ==> seq_row(ends@, cells@, j) == (#[trigger] draft.rows@[j])@,
        decreases n - i,
    {
        proof { reveal(flat_valid); }
        let ghost old_cells = cells@;
        let ghost old_ends = ends@;
        let row = &draft.rows[i];
        let start = cells.len();
        let mut offset = 0;
        while offset < row.len()
            invariant offset <= row.len(), cells@ == old_cells + row@.take(offset as int),
            decreases row.len() - offset,
        {
            cells.push(row[offset]);
            proof { assert(row@.take(offset as int + 1) =~= row@.take(offset as int).push(row@[offset as int])); }
            offset += 1;
        }
        proof { assert(row@.take(row@.len() as int) =~= row@); }
        ends.push(cells.len());
        keys.push(draft.keys[i]);
        proof {
            assert(draft.keys@.take(i as int + 1) =~= draft.keys@.take(i as int).push(draft.keys@[i as int]));
            assert(cells@ == old_cells + row@);
            assert(row@.len() > 0);
            assert(seq_start(ends@, i as int) == start as int);
            assert forall|j: int| 0 <= j < i implies seq_row(ends@, cells@, j) == (#[trigger] draft.rows@[j])@ by {
                assert(ends@[j] == old_ends[j]);
                assert(seq_start(ends@, j) == seq_start(old_ends, j));
                assert(seq_row(old_ends, old_cells, j) == draft.rows@[j]@);
                assert(cells@.subrange(seq_start(ends@, j), ends@[j] as int) =~= old_cells.subrange(seq_start(old_ends, j), old_ends[j] as int));
            }
            assert(seq_row(ends@, cells@, i as int) =~= row@);
            assert forall|j: int| 0 <= j < i + 1 implies seq_start(ends@, j) < (#[trigger] ends@[j]) as int && ends@[j] as int <= cells@.len() by {
                if j < i { assert(ends@[j] == old_ends[j]); assert(seq_start(ends@, j) == seq_start(old_ends, j)); }
            }
        }
        i += 1;
    }
    proof {
        assert(draft.keys@.take(n as int) =~= draft.keys@);
        assert forall|j: int| 0 <= j < ends@.len() implies rows_valid(seq_row(ends@, cells@, j)) by {
            assert(seq_row(ends@, cells@, j) == draft.rows@[j]@);
        }
    }
    Flat { keys, ends, cells }
}

pub proof fn lemma_draft_remove(keys: Seq<Major>, rows: Seq<Vec<Cell>>, i: int, position: int, slots: int)
    requires draft_valid(keys, rows, position, slots), 0 <= i < keys.len(),
    ensures draft_valid(keys.remove(i), rows.remove(i), position, slots),
        !keys.remove(i).contains(keys[i]),
        rows_in(keys, rows, keys[i]) == rows[i]@,
        forall|other: Major| other != keys[i] ==> #[trigger] rows_in(keys.remove(i), rows.remove(i), other) == rows_in(keys, rows, other),
{
    let nkeys = keys.remove(i);
    let nrows = rows.remove(i);
    assert forall|j: int| 0 <= j < nkeys.len() implies nkeys[j] == keys[if j < i { j } else { j + 1 }] by {}
    assert forall|j: int| 0 <= j < nrows.len() implies nrows[j] == rows[if j < i { j } else { j + 1 }] by {}
    assert(keys_unique(nkeys)) by {
        assert forall|l: int, r: int| 0 <= l < r < nkeys.len() implies nkeys[l] != nkeys[r] by {
            assert(nkeys[l] == keys[if l < i { l } else { l + 1 }]);
            assert(nkeys[r] == keys[if r < i { r } else { r + 1 }]);
        }
    }
    assert forall|j: int| 0 <= j < nkeys.len() implies slot(#[trigger] nkeys[j], slots) == position by {
        assert(nkeys[j] == keys[if j < i { j } else { j + 1 }]);
    }
    assert forall|j: int| 0 <= j < nrows.len() implies (#[trigger] nrows[j])@.len() > 0 && rows_valid(nrows[j]@) by {
        assert(nrows[j] == rows[if j < i { j } else { j + 1 }]);
    }
    lemma_keyed_index(keys, i);
    assert(!nkeys.contains(keys[i])) by {
        if nkeys.contains(keys[i]) {
            let j = choose|j: int| 0 <= j < nkeys.len() && nkeys[j] == keys[i];
            let o = if j < i { j } else { j + 1 };
            assert(keys[o] == keys[i]);
            if o < i { assert(keys[o] != keys[i]); } else { assert(keys[i] != keys[o]); }
        }
    }
    assert forall|other: Major| other != keys[i] implies #[trigger] rows_in(nkeys, nrows, other) == rows_in(keys, rows, other) by {
        if keys.contains(other) {
            let j = keyed_index(keys, other);
            assert(j != i);
            let nj = if j < i { j } else { j - 1 };
            assert(0 <= nj < nkeys.len());
            assert(nkeys[nj] == other);
            lemma_draft_row_at(nkeys, nrows, nj);
            lemma_draft_row_at(keys, rows, j);
            assert(nrows[nj] == rows[j]);
        } else {
            assert(!nkeys.contains(other)) by {
                if nkeys.contains(other) {
                    let j = choose|j: int| 0 <= j < nkeys.len() && nkeys[j] == other;
                    assert(keys[if j < i { j } else { j + 1 }] == other);
                }
            }
        }
    }
}

pub proof fn lemma_draft_push(keys: Seq<Major>, rows: Seq<Vec<Cell>>, key: Major, row: Vec<Cell>, position: int, slots: int)
    requires draft_valid(keys, rows, position, slots), !keys.contains(key), slot(key, slots) == position,
        row@.len() > 0, rows_valid(row@),
    ensures draft_valid(keys.push(key), rows.push(row), position, slots),
        rows_in(keys.push(key), rows.push(row), key) == row@,
        forall|other: Major| other != key ==> #[trigger] rows_in(keys.push(key), rows.push(row), other) == rows_in(keys, rows, other),
{
    let nkeys = keys.push(key);
    let nrows = rows.push(row);
    let last = nkeys.len() - 1;
    assert(keys_unique(nkeys)) by {
        assert forall|l: int, r: int| 0 <= l < r < nkeys.len() implies nkeys[l] != nkeys[r] by {
            if r == last { assert(nkeys[l] == keys[l]); if keys[l] == key { assert(keys.contains(key)); } }
        }
    }
    assert forall|j: int| 0 <= j < nkeys.len() implies slot(#[trigger] nkeys[j], slots) == position by {}
    assert forall|j: int| 0 <= j < nrows.len() implies (#[trigger] nrows[j])@.len() > 0 && rows_valid(nrows[j]@) by {}
    lemma_draft_row_at(nkeys, nrows, last);
    assert forall|other: Major| other != key implies #[trigger] rows_in(nkeys, nrows, other) == rows_in(keys, rows, other) by {
        if keys.contains(other) {
            let j = keyed_index(keys, other);
            lemma_draft_row_at(nkeys, nrows, j);
            lemma_draft_row_at(keys, rows, j);
        } else {
            assert(!nkeys.contains(other)) by {
                if nkeys.contains(other) {
                    let j = choose|j: int| 0 <= j < nkeys.len() && nkeys[j] == other;
                    if j < last { assert(keys[j] == other); }
                }
            }
        }
    }
}

/// Take a key's rows out of a draft (empty if the key is absent).
pub fn draft_remove(draft: &mut Draft, key: Major, Ghost(position): Ghost<int>, Ghost(slots): Ghost<int>) -> (row: Vec<Cell>)
    requires draft_valid(old(draft).keys@, old(draft).rows@, position, slots),
    ensures draft_valid(final(draft).keys@, final(draft).rows@, position, slots),
        row@ == draft_rows(*old(draft), key), rows_valid(row@),
        !final(draft).keys@.contains(key),
        forall|other: Major| other != key ==> #[trigger] draft_rows(*final(draft), other) == draft_rows(*old(draft), other),
{
    let ghost before = *draft;
    let mut i = 0;
    while i < draft.keys.len()
        invariant i <= draft.keys.len(), *draft == before, before == *old(draft), draft_valid(before.keys@, before.rows@, position, slots),
            forall|j: int| 0 <= j < i ==> draft.keys@[j] != key,
        decreases draft.keys.len() - i,
    {
        if draft.keys[i] == key {
            draft.keys.remove(i);
            let row = draft.rows.remove(i);
            proof {
                lemma_draft_remove(before.keys@, before.rows@, i as int, position, slots);
                assert(draft.keys@ == before.keys@.remove(i as int));
                assert(draft.rows@ == before.rows@.remove(i as int));
                assert(before.keys@[i as int] == key);
                assert(row == before.rows@[i as int]);
                assert(draft_rows(before, key) == before.rows@[i as int]@);
                assert(rows_valid(row@));
                assert(before == *old(draft));
                assert(row@ == draft_rows(*old(draft), key));
                assert forall|other: Major| other != key implies #[trigger] draft_rows(*draft, other) == draft_rows(*old(draft), other) by {
                    assert(draft_rows(*draft, other) == rows_in(before.keys@.remove(i as int), before.rows@.remove(i as int), other));
                }
            }
            return row;
        }
        i += 1;
    }
    proof {
        assert(!draft.keys@.contains(key)) by {
            if draft.keys@.contains(key) {
                let j = choose|j: int| 0 <= j < draft.keys@.len() && draft.keys@[j] == key;
                assert(draft.keys@[j] != key);
            }
        }
        assert(Seq::<Cell>::empty() == draft_rows(before, key));
        assert(rows_valid(Seq::<Cell>::empty()));
    }
    Vec::new()
}

/// Put rows under a key that the draft does not hold (nothing if they are empty).
pub fn draft_insert(draft: &mut Draft, key: Major, row: Vec<Cell>, Ghost(position): Ghost<int>, Ghost(slots): Ghost<int>)
    requires draft_valid(old(draft).keys@, old(draft).rows@, position, slots), !old(draft).keys@.contains(key),
        slot(key, slots) == position, rows_valid(row@),
    ensures draft_valid(final(draft).keys@, final(draft).rows@, position, slots),
        forall|other: Major| #[trigger] draft_rows(*final(draft), other) == if other == key { row@ } else { draft_rows(*old(draft), other) },
{
    let ghost before = *draft;
    if row.len() == 0 {
        proof {
            assert forall|other: Major| #[trigger] draft_rows(*draft, other) == if other == key { row@ } else { draft_rows(before, other) } by {
                if other == key { assert(row@ =~= Seq::<Cell>::empty()); }
            }
        }
        return;
    }
    draft.keys.push(key);
    draft.rows.push(row);
    proof {
        lemma_draft_push(before.keys@, before.rows@, key, row, position, slots);
        assert(draft.keys@ == before.keys@.push(key));
        assert(draft.rows@ == before.rows@.push(row));
        assert forall|other: Major| #[trigger] draft_rows(*draft, other) == if other == key { row@ } else { draft_rows(before, other) } by {}
    }
}

/// `ends` never decrease.
pub proof fn lemma_ends_mono(ends: Seq<usize>, a: int, b: int)
    requires forall|i: int| 0 <= i < ends.len() ==> seq_start(ends, i) < (#[trigger] ends[i]) as int,
        0 <= a <= b < ends.len(),
    ensures ends[a] as int <= ends[b] as int,
    decreases b - a,
{
    if a < b {
        lemma_ends_mono(ends, a, b - 1);
        assert(seq_start(ends, b) == ends[b - 1] as int);
    }
}

/// Overwrite one cell of a row (same minor).
#[verifier::rlimit(60)]
pub proof fn lemma_flat_set(keys: Seq<Major>, ends: Seq<usize>, cells: Seq<Cell>, position: int, slots: int, i: int, p: int, cell: Cell)
    requires flat_valid(keys, ends, cells, position, slots), 0 <= i < ends.len(),
        seq_start(ends, i) <= p < ends[i] as int, minor_of(cell) == minor_of(cells[p]), cell.count != 0 || cell.volume != 0,
    ensures flat_valid(keys, ends, cells.update(p, cell), position, slots),
        seq_row(ends, cells.update(p, cell), i) == seq_row(ends, cells, i).update(p - seq_start(ends, i), cell),
        forall|j: int| 0 <= j < ends.len() && j != i ==> seq_row(ends, cells.update(p, cell), j) == seq_row(ends, cells, j),
{
    reveal(flat_valid);
    let cells2 = cells.update(p, cell);
    let st = seq_start(ends, i);
    assert forall|j: int| 0 <= j < ends.len() && j != i implies #[trigger] seq_row(ends, cells2, j) == seq_row(ends, cells, j) by {
        if j < i {
            lemma_ends_mono(ends, j, i - 1);
            assert(ends[j] as int <= ends[i - 1] as int);
        } else {
            lemma_ends_mono(ends, i, j - 1);
            assert(ends[i] as int <= ends[j - 1] as int);
        }
        assert(seq_row(ends, cells2, j) =~= seq_row(ends, cells, j));
    }
    assert(seq_row(ends, cells2, i) =~= seq_row(ends, cells, i).update(p - st, cell));
    assert(seq_row(ends, cells, i)[p - st] == cells[p]);
    lemma_rows_update(seq_row(ends, cells, i), p - st, cell);
    assert forall|j: int| #![trigger seq_row(ends, cells2, j)] 0 <= j < ends.len() implies rows_valid(seq_row(ends, cells2, j)) by {
        if j != i { assert(seq_row(ends, cells2, j) == seq_row(ends, cells, j)); }
    }
}

/// Append a cell to the end of row `i` (every later end moves up by one).
pub proof fn lemma_flat_insert(keys: Seq<Major>, ends: Seq<usize>, cells: Seq<Cell>, ends2: Seq<usize>, position: int, slots: int, i: int, cell: Cell)
    requires flat_valid(keys, ends, cells, position, slots), 0 <= i < ends.len(),
        ends2.len() == ends.len(),
        forall|j: int| 0 <= j < i ==> ends2[j] == ends[j],
        forall|j: int| i <= j < ends.len() ==> ends2[j] as int == ends[j] as int + 1,
        rows_valid(seq_row(ends, cells, i).push(cell)),
    ensures flat_valid(keys, ends2, cells.insert(ends[i] as int, cell), position, slots),
        seq_row(ends2, cells.insert(ends[i] as int, cell), i) == seq_row(ends, cells, i).push(cell),
        forall|j: int| 0 <= j < ends.len() && j != i ==> seq_row(ends2, cells.insert(ends[i] as int, cell), j) == seq_row(ends, cells, j),
{
    reveal(flat_valid);
    let en = ends[i] as int;
    let cells2 = cells.insert(en, cell);
    assert(cells2.len() == cells.len() + 1);
    assert forall|j: int| 0 <= j < ends.len() implies seq_start(ends2, j) == seq_start(ends, j) + (if j > i { 1int } else { 0int }) by {}
    assert forall|j: int| 0 <= j < ends.len() && j != i implies #[trigger] seq_row(ends2, cells2, j) == seq_row(ends, cells, j) by {
        if j < i {
            lemma_ends_mono(ends, j, i - 1);
            assert(seq_row(ends2, cells2, j) =~= seq_row(ends, cells, j));
        } else {
            lemma_ends_mono(ends, i, j - 1);
            assert(seq_row(ends2, cells2, j) =~= seq_row(ends, cells, j));
        }
    }
    assert(seq_row(ends2, cells2, i) =~= seq_row(ends, cells, i).push(cell));
    assert forall|j: int| 0 <= j < ends2.len() implies seq_start(ends2, j) < (#[trigger] ends2[j]) as int && ends2[j] as int <= cells2.len() by {
        assert(seq_start(ends, j) < ends[j] as int && ends[j] as int <= cells.len());
    }
    assert forall|j: int| #![trigger seq_row(ends2, cells2, j)] 0 <= j < ends2.len() implies rows_valid(seq_row(ends2, cells2, j)) by {
        if j != i { assert(seq_row(ends2, cells2, j) == seq_row(ends, cells, j)); }
    }
    if ends2.len() > 0 { assert(ends2[ends2.len() - 1] as int == cells2.len()); }
}

/// Append a new key with a one-cell row.
pub proof fn lemma_flat_push(keys: Seq<Major>, ends: Seq<usize>, cells: Seq<Cell>, position: int, slots: int, key: Major, cell: Cell, end: usize)
    requires flat_valid(keys, ends, cells, position, slots), !keys.contains(key), slot(key, slots) == position,
        cell.count != 0 || cell.volume != 0, end as int == cells.len() + 1,
    ensures flat_valid(keys.push(key), ends.push(end), cells.push(cell), position, slots),
        seq_row(ends.push(end), cells.push(cell), ends.len() as int) == seq![cell],
        forall|j: int| 0 <= j < ends.len() ==> seq_row(ends.push(end), cells.push(cell), j) == seq_row(ends, cells, j),
{
    reveal(flat_valid);
    let keys2 = keys.push(key);
    let ends2 = ends.push(end);
    let cells2 = cells.push(cell);
    let last = ends.len() as int;
    assert(seq_start(ends2, last) == cells.len() as int) by {
        if last > 0 { assert(ends[last - 1] as int == cells.len()); }
    }
    assert forall|j: int| 0 <= j < ends.len() implies #[trigger] seq_row(ends2, cells2, j) == seq_row(ends, cells, j) by {
        assert(seq_row(ends2, cells2, j) =~= seq_row(ends, cells, j));
    }
    assert(seq_row(ends2, cells2, last) =~= seq![cell]);
    assert(rows_valid(seq![cell]));
    assert(keys_unique(keys2)) by {
        assert forall|l: int, r: int| 0 <= l < r < keys2.len() implies keys2[l] != keys2[r] by {
            if r == keys.len() as int { assert(keys2[l] == keys[l]); if keys[l] == key { assert(keys.contains(key)); } }
        }
    }
    assert forall|j: int| 0 <= j < ends2.len() implies seq_start(ends2, j) < (#[trigger] ends2[j]) as int && ends2[j] as int <= cells2.len() by {
        if j < last { assert(seq_start(ends2, j) == seq_start(ends, j)); assert(ends2[j] == ends[j]); }
    }
    assert forall|j: int| 0 <= j < keys2.len() implies slot(#[trigger] keys2[j], slots) == position by {}
    assert forall|j: int| #![trigger seq_row(ends2, cells2, j)] 0 <= j < ends2.len() implies rows_valid(seq_row(ends2, cells2, j)) by {
        if j < last { assert(seq_row(ends2, cells2, j) == seq_row(ends, cells, j)); }
    }
}

/// Flat-level view of rows after an in-place change keeps every other key's rows.
pub proof fn lemma_rows_other(b: Flat, b2: Flat, position: int, slots: int, key: Major)
    requires flat_ok(b, position, slots), flat_ok(b2, position, slots), b2.keys@ == b.keys@, b2.ends@.len() == b.ends@.len(),
        forall|j: int| 0 <= j < b.ends@.len() && b.keys@[j] != key ==> seq_row(b2.ends@, b2.cells@, j) == seq_row(b.ends@, b.cells@, j),
    ensures forall|other: Major| other != key ==> #[trigger] flat_rows(b2, other) == flat_rows(b, other),
{
    lemma_flat_facts(b, position, slots);
    assert forall|other: Major| other != key implies #[trigger] flat_rows(b2, other) == flat_rows(b, other) by {
        if b.keys@.contains(other) {
            let j = keyed_index(b.keys@, other);
            assert(0 <= j < b.keys@.len() && b.keys@[j] == other);
            assert(b2.keys@.contains(other));
            assert(keyed_index(b2.keys@, other) == j);
        }
    }
}

pub proof fn lemma_flat_update_row(orig: Flat, b: Flat, position: int, slots: int, key: Major, i: int)
    requires flat_ok(orig, position, slots), flat_ok(b, position, slots), b.keys@ == orig.keys@, b.ends@.len() == orig.ends@.len(),
        0 <= i < orig.keys@.len(), orig.keys@[i] == key,
        forall|j: int| 0 <= j < orig.ends@.len() && j != i ==> seq_row(b.ends@, b.cells@, j) == seq_row(orig.ends@, orig.cells@, j),
    ensures forall|other: Major| other != key ==> #[trigger] flat_rows(b, other) == flat_rows(orig, other),
        flat_rows(b, key) == seq_row(b.ends@, b.cells@, i), flat_rows(orig, key) == seq_row(orig.ends@, orig.cells@, i),
{
    lemma_flat_facts(orig, position, slots);
    lemma_rows_other(orig, b, position, slots, key);
    lemma_keyed_index(orig.keys@, i);
    lemma_keyed_index(b.keys@, i);
}

pub proof fn lemma_rows_push(row: Seq<Cell>, cell: Cell)
    requires rows_valid(row), cell.count != 0 || cell.volume != 0,
        forall|q: int| 0 <= q < row.len() ==> minor_of(row[q]) != minor_of(cell),
    ensures rows_valid(row.push(cell)),
{
    let after = row.push(cell);
    assert forall|index: int| 0 <= index < after.len() implies (#[trigger] after[index]).count != 0 || after[index].volume != 0 by {
        if index < row.len() { assert(after[index] == row[index]); }
    }
    assert forall|left: int, right: int| 0 <= left < right < after.len() implies minor_of(#[trigger] after[left]) != minor_of(#[trigger] after[right]) by {
        if right < row.len() { assert(after[left] == row[left]); assert(after[right] == row[right]); }
        else { assert(after[left] == row[left]); assert(after[right] == cell); }
    }
}

/// Copy a bucket into fresh vectors with room for `spare` more entries each.
pub fn flat_copy_with(bucket: &Flat, spare: usize) -> (result: Flat)
    ensures result.keys@ == bucket.keys@, result.ends@ == bucket.ends@, result.cells@ == bucket.cells@,
{
    let mut keys: Vec<Major> = Vec::with_capacity(match bucket.keys.len().checked_add(spare) { Some(n) => n, None => bucket.keys.len() });
    let mut i = 0;
    while i < bucket.keys.len()
        invariant i <= bucket.keys.len(), keys@ == bucket.keys@.take(i as int),
        decreases bucket.keys.len() - i,
    {
        keys.push(bucket.keys[i]);
        proof { assert(bucket.keys@.take(i as int + 1) =~= bucket.keys@.take(i as int).push(bucket.keys@[i as int])); }
        i += 1;
    }
    proof { assert(bucket.keys@.take(bucket.keys@.len() as int) =~= bucket.keys@); }
    let mut ends: Vec<usize> = Vec::with_capacity(match bucket.ends.len().checked_add(spare) { Some(n) => n, None => bucket.ends.len() });
    i = 0;
    while i < bucket.ends.len()
        invariant i <= bucket.ends.len(), ends@ == bucket.ends@.take(i as int),
        decreases bucket.ends.len() - i,
    {
        ends.push(bucket.ends[i]);
        proof { assert(bucket.ends@.take(i as int + 1) =~= bucket.ends@.take(i as int).push(bucket.ends@[i as int])); }
        i += 1;
    }
    proof { assert(bucket.ends@.take(bucket.ends@.len() as int) =~= bucket.ends@); }
    let mut cells: Vec<Cell> = Vec::with_capacity(match bucket.cells.len().checked_add(spare) { Some(n) => n, None => bucket.cells.len() });
    i = 0;
    while i < bucket.cells.len()
        invariant i <= bucket.cells.len(), cells@ == bucket.cells@.take(i as int),
        decreases bucket.cells.len() - i,
    {
        cells.push(bucket.cells[i]);
        proof { assert(bucket.cells@.take(i as int + 1) =~= bucket.cells@.take(i as int).push(bucket.cells@[i as int])); }
        i += 1;
    }
    proof { assert(bucket.cells@.take(bucket.cells@.len() as int) =~= bucket.cells@); }
    Flat { keys, ends, cells }
}

/// Add `count` and `volume` to the cell of `key` at `minor` in an owned bucket, in place when it can be.
#[verifier::rlimit(60)]
pub fn flat_add(bucket: Flat, key: Major, minor: Major, count: i128, volume: i128, Ghost(position): Ghost<int>, Ghost(slots): Ghost<int>) -> (result: Option<Flat>)
    requires flat_ok(bucket, position, slots), slot(key, slots) == position,
    ensures result is None ==> add_overflows(cell_sum(flat_rows(bucket, key), minor, false), cell_sum(flat_rows(bucket, key), minor, true), count, volume),
        result matches Some(after) ==> flat_ok(after, position, slots)
        && (forall|other: Major| other != key ==> #[trigger] flat_rows(after, other) == flat_rows(bucket, other))
        && (forall|small: Major, measure: bool| #[trigger] cell_sum(flat_rows(after, key), small, measure) == cell_sum(flat_rows(bucket, key), small, measure)
            + if small == minor { if measure { volume as int } else { count as int } } else { 0 }),
{
    let ghost orig = bucket;
    let mut b = bucket;
    proof { lemma_flat_facts(orig, position, slots); }
    let n = b.keys.len();
    let mut i = 0;
    while i < n && b.keys[i] != key
        invariant orig == bucket, i <= n, n == b.keys.len(), b == orig, flat_ok(orig, position, slots), forall|j: int| 0 <= j < i ==> b.keys@[j] != key,
        decreases n - i,
    {
        i += 1;
    }
    if i == n {
        proof {
            assert(!orig.keys@.contains(key)) by {
                if orig.keys@.contains(key) {
                    let j = choose|j: int| 0 <= j < orig.keys@.len() && orig.keys@[j] == key;
                    assert(orig.keys@[j] != key);
                }
            }
            assert(flat_rows(orig, key) =~= Seq::<Cell>::empty());
            reveal(cell_sum);
        }
        if count == 0 && volume == 0 { return Some(b); }
        let cell = Cell { first: minor.0, second: minor.1, third: minor.2, count, volume };
        b.keys.push(key);
        b.cells.push(cell);
        let end = b.cells.len();
        b.ends.push(end);
        proof {
            lemma_flat_push(orig.keys@, orig.ends@, orig.cells@, position, slots, key, cell, end);
            let last = orig.ends@.len() as int;
            assert(flat_ok(b, position, slots));
            lemma_keyed_index(b.keys@, last);
            assert(flat_rows(b, key) == seq![cell]);
            assert(b.keys@.len() == orig.keys@.len() + 1);
            assert forall|other: Major| other != key implies #[trigger] flat_rows(b, other) == flat_rows(orig, other) by {
                if orig.keys@.contains(other) {
                    let j = keyed_index(orig.keys@, other);
                    lemma_keyed_index(b.keys@, j);
                    assert(b.keys@[j] == other);
                } else {
                    assert(!b.keys@.contains(other)) by {
                        if b.keys@.contains(other) {
                            let j = choose|j: int| 0 <= j < b.keys@.len() && b.keys@[j] == other;
                            if j < last { assert(orig.keys@[j] == other); }
                        }
                    }
                }
            }
            reveal(cell_sum);
            assert forall|small: Major, measure: bool| #[trigger] cell_sum(flat_rows(b, key), small, measure) == cell_sum(flat_rows(orig, key), small, measure)
                + if small == minor { if measure { volume as int } else { count as int } } else { 0 } by {
                lemma_sum_push(Seq::<Cell>::empty(), cell, |entry: Cell| cell_weight(entry, small, measure));
                assert(Seq::<Cell>::empty().push(cell) =~= seq![cell]);
            }
        }
        return Some(b);
    }
    let start = row_start(&b.ends, i);
    let end = b.ends[i];
    proof {
        lemma_keyed_index(orig.keys@, i as int);
        assert(orig.keys@[i as int] == key);
        assert(flat_rows(orig, key) == seq_row(orig.ends@, orig.cells@, i as int));
        assert(seq_start(orig.ends@, i as int) < orig.ends@[i as int] as int && orig.ends@[i as int] as int <= orig.cells@.len());
    }
    let mut p = start;
    while p < end
        invariant orig == bucket, start <= p <= end, end <= b.cells.len(), b == orig, flat_ok(orig, position, slots), i < orig.keys@.len(), orig.keys@.len() == orig.ends@.len(), orig.keys@[i as int] == key, end == orig.ends@[i as int], start as int == seq_start(orig.ends@, i as int),
            forall|q: int| start <= q < p ==> minor_of(#[trigger] orig.cells@[q]) != minor,
        decreases end - p,
    {
        let cell = b.cells[p];
        proof { lemma_flat_facts(orig, position, slots); }
        if (cell.first, cell.second, cell.third) == minor {
            proof {
                let row = seq_row(orig.ends@, orig.cells@, i as int);
                assert(flat_rows(orig, key) == row);
                assert(row[p as int - start as int] == cell);
                assert(rows_valid(row));
                lemma_cell_lookup(row, minor, p as int - start as int);
            }
            let new_count = match cell.count.checked_add(count) { Some(value) => value, None => return None };
            let new_volume = match cell.volume.checked_add(volume) { Some(value) => value, None => return None };
            if new_count == 0 && new_volume == 0 { break; }
            let replacement = Cell { first: minor.0, second: minor.1, third: minor.2, count: new_count, volume: new_volume };
            b.cells.set(p, replacement);
            proof {
                assert(orig.ends@.len() == orig.keys@.len());
                assert(seq_start(orig.ends@, i as int) <= p as int && (p as int) < orig.ends@[i as int] as int);
                assert(minor_of(replacement) == minor_of(orig.cells@[p as int]));
                assert(replacement.count != 0 || replacement.volume != 0);
                assert(flat_ok(orig, position, slots));
                lemma_flat_set(orig.keys@, orig.ends@, orig.cells@, position, slots, i as int, p as int, replacement);
                let row = seq_row(orig.ends@, orig.cells@, i as int);
                let row2 = seq_row(b.ends@, b.cells@, i as int);
                assert(flat_ok(b, position, slots));
                assert(row2 == row.update(p as int - start as int, replacement));
                assert forall|j: int| 0 <= j < orig.ends@.len() && j != i as int implies seq_row(b.ends@, b.cells@, j) == seq_row(orig.ends@, orig.cells@, j) by {}
                lemma_flat_update_row(orig, b, position, slots, key, i as int);
                // the sums: the same minor, the totals move by (count, volume)
                reveal(cell_sum);
                assert(row[p as int - start as int] == cell);
                assert(minor_of(cell) == minor);
                assert(new_count as int == cell.count as int + count as int && new_volume as int == cell.volume as int + volume as int);
                assert forall|small: Major, measure: bool| #[trigger] cell_sum(flat_rows(b, key), small, measure) == cell_sum(flat_rows(orig, key), small, measure)
                    + if small == minor { if measure { volume as int } else { count as int } } else { 0 } by {
                    lemma_sum_update(row, p as int - start as int, replacement, |entry: Cell| cell_weight(entry, small, measure));
                    assert(flat_rows(b, key) == row2);
                    assert(flat_rows(orig, key) == row);
                    assert(cell_weight(cell, small, measure) == if small == minor { if measure { cell.volume as int } else { cell.count as int } } else { 0 });
                    assert(cell_weight(replacement, small, measure) == if small == minor { if measure { new_volume as int } else { new_count as int } } else { 0 });
                }
                assert(flat_ok(b, position, slots));
                assert(orig == bucket);
                assert forall|other: Major| other != key implies #[trigger] flat_rows(b, other) == flat_rows(bucket, other) by {}
            }
            return Some(b);
        }
        proof { assert(minor_of(orig.cells@[p as int]) != minor); }
        p += 1;
    }
    // not in place: the minor is new in this row, or the cell cancels out
    let ghost slow = p < end;
    if p == end {
        if count == 0 && volume == 0 { proof { reveal(cell_sum); } return Some(b); }
        // append a new minor to the row
        let cell = Cell { first: minor.0, second: minor.1, third: minor.2, count, volume };
        b.cells.insert(end, cell);
        let grown = b.cells.len();
        proof { lemma_flat_facts(orig, position, slots); assert(grown as int == orig.cells@.len() + 1); }
        let mut j = i;
        let m = b.ends.len();
        while j < m
            invariant orig == bucket, i <= j <= m, m == b.ends.len(), m == orig.ends@.len(), b.keys@ == orig.keys@, b.cells@ == orig.cells@.insert(end as int, cell),
                flat_ok(orig, position, slots), i < orig.keys@.len(), orig.keys@.len() == orig.ends@.len(), orig.keys@[i as int] == key, end == orig.ends@[i as int],
                forall|q: int| 0 <= q < i ==> b.ends@[q] == orig.ends@[q],
                forall|q: int| i <= q < j ==> b.ends@[q] as int == orig.ends@[q] as int + 1,
                forall|q: int| j <= q < m ==> b.ends@[q] == orig.ends@[q],
                grown as int == orig.cells@.len() + 1,
                forall|q: int| 0 <= q < orig.ends@.len() ==> (#[trigger] orig.ends@[q]) as int <= orig.cells@.len(),
            decreases m - j,
        {
            proof { assert(b.ends@[j as int] == orig.ends@[j as int]); assert(orig.ends@[j as int] as int <= orig.cells@.len()); }
            let e = b.ends[j] + 1;
            b.ends.set(j, e);
            j += 1;
        }
        proof {
            let row = seq_row(orig.ends@, orig.cells@, i as int);
            assert forall|q: int| 0 <= q < row.len() implies minor_of(row[q]) != minor_of(cell) by {
                assert(row[q] == orig.cells@[start as int + q]);
            }
            lemma_rows_push(row, cell);
            lemma_flat_insert(orig.keys@, orig.ends@, orig.cells@, b.ends@, position, slots, i as int, cell);
            assert(flat_ok(b, position, slots));
            assert forall|j: int| 0 <= j < orig.ends@.len() && j != i as int implies seq_row(b.ends@, b.cells@, j) == seq_row(orig.ends@, orig.cells@, j) by {}
            lemma_flat_update_row(orig, b, position, slots, key, i as int);
            reveal(cell_sum);
            assert forall|small: Major, measure: bool| #[trigger] cell_sum(flat_rows(b, key), small, measure) == cell_sum(flat_rows(orig, key), small, measure)
                + if small == minor { if measure { volume as int } else { count as int } } else { 0 } by {
                lemma_sum_push(row, cell, |entry: Cell| cell_weight(entry, small, measure));
            }
        }
        return Some(b);
    }
    // the cell cancels out: rebuild this bucket
    let mut draft = draft_of(&b, Ghost(position), Ghost(slots));
    let ghost d0 = draft;
    proof { lemma_flat_facts(orig, position, slots); lemma_rows_agree(orig, d0); }
    let row = draft_remove(&mut draft, key, Ghost(position), Ghost(slots));
    let ghost d1 = draft;
    let updated = match add_cells(row, minor, count, volume) { Some(value) => value, None => { proof { assert(row@ == flat_rows(orig, key)); } return None; } };
    draft_insert(&mut draft, key, updated, Ghost(position), Ghost(slots));
    let result = flat_of(&draft, Ghost(position), Ghost(slots));
    proof {
        lemma_flat_facts(result, position, slots);
        lemma_rows_agree(result, draft);
        assert forall|other: Major| other != key implies #[trigger] flat_rows(result, other) == flat_rows(orig, other) by {
            assert(draft_rows(d1, other) == draft_rows(d0, other));
        }
        assert(flat_rows(result, key) == updated@);
        assert(row@ == flat_rows(orig, key));
    }
    Some(result)
}

pub open spec fn pair_cell(pair: Pair) -> Cell {
    Cell { first: pair.first, second: pair.second, third: 0, count: 1, volume: 0 }
}

pub open spec fn bucket_cells(bucket: Bucket) -> Seq<Cell> {
    if bucket.packed { bucket.pairs@.map_values(|pair: Pair| pair_cell(pair)) } else { bucket.cells@ }
}

pub open spec fn triple_rows(keys: Seq<Major>, ends: Seq<usize>, cells: Seq<Cell>, key: Major) -> Seq<Cell> {
    if keys.contains(key) { seq_row(ends, cells, keyed_index(keys, key)) } else { Seq::empty() }
}

pub open spec fn bucket_valid(bucket: Bucket, position: int, slots: int) -> bool {
    flat_valid(bucket.keys@, bucket.ends@, bucket_cells(bucket), position, slots)
}

pub open spec fn bucket_rows(bucket: Bucket, key: Major) -> Seq<Cell> {
    triple_rows(bucket.keys@, bucket.ends@, bucket_cells(bucket), key)
}

pub open spec fn arc_bucket(bucket: &Arc<Bucket>) -> Bucket {
    **bucket
}

pub open spec fn bucket_val(bucket: &Bucket) -> Bucket {
    *bucket
}

pub proof fn lemma_bucket_facts(bucket: Bucket, position: int, slots: int)
    requires bucket_valid(bucket, position, slots),
    ensures bucket.keys@.len() == bucket.ends@.len(), keys_unique(bucket.keys@),
        bucket.ends@.len() == 0 ==> bucket_cells(bucket).len() == 0,
        bucket.ends@.len() > 0 ==> bucket.ends@[bucket.ends@.len() - 1] as int == bucket_cells(bucket).len(),
        forall|i: int| 0 <= i < bucket.ends@.len() ==> seq_start(bucket.ends@, i) < (#[trigger] bucket.ends@[i]) as int && bucket.ends@[i] as int <= bucket_cells(bucket).len(),
        forall|i: int| 0 <= i < bucket.keys@.len() ==> slot(#[trigger] bucket.keys@[i], slots) == position,
        forall|i: int| #![trigger seq_row(bucket.ends@, bucket_cells(bucket), i)] 0 <= i < bucket.ends@.len() ==> rows_valid(seq_row(bucket.ends@, bucket_cells(bucket), i)),
{
    reveal(flat_valid);
}

pub proof fn lemma_bucket_rows_valid(bucket: Bucket, position: int, slots: int, key: Major)
    requires bucket_valid(bucket, position, slots),
    ensures rows_valid(bucket_rows(bucket, key)), bucket_rows(bucket, key).len() > 0 <==> bucket.keys@.contains(key),
        bucket.keys@.contains(key) ==> slot(key, slots) == position,
{
    reveal(flat_valid);
    if bucket.keys@.contains(key) {
        let i = keyed_index(bucket.keys@, key);
        assert(0 <= i < bucket.keys@.len() && bucket.keys@[i] == key);
        let row = seq_row(bucket.ends@, bucket_cells(bucket), i);
        assert(rows_valid(row));
        assert(seq_start(bucket.ends@, i) < bucket.ends@[i] as int);
        assert(row.len() == bucket.ends@[i] as int - seq_start(bucket.ends@, i));
    } else {
        assert(bucket_rows(bucket, key) =~= Seq::<Cell>::empty());
        assert(rows_valid(Seq::<Cell>::empty()));
    }
}

/// A flat bucket and a persistent one with the same keys, ends and cells have the same rows.
pub proof fn lemma_bucket_flat_rows(bucket: Bucket, flat: Flat)
    requires bucket.keys@ == flat.keys@, bucket.ends@ == flat.ends@, bucket_cells(bucket) == flat.cells@,
    ensures forall|position: int, slots: int| flat_ok(flat, position, slots) == bucket_valid(bucket, position, slots),
        forall|key: Major| #[trigger] bucket_rows(bucket, key) == flat_rows(flat, key),
{
    assert forall|key: Major| #[trigger] bucket_rows(bucket, key) == flat_rows(flat, key) by {}
    assert forall|position: int, slots: int| flat_ok(flat, position, slots) == bucket_valid(bucket, position, slots) by {}
}

/// The cells `start..end` of a bucket.
pub fn bucket_range(bucket: &Bucket, start: usize, end: usize) -> (result: Vec<Cell>)
    requires start <= end, end as int <= bucket_cells(*bucket).len(),
    ensures result@ == bucket_cells(*bucket).subrange(start as int, end as int),
{
    proof { reveal(bucket_cells); }
    if bucket.packed {
        proof { assert(bucket_cells(*bucket).len() == bucket.pairs@.len()); }
        let mut result = Vec::with_capacity(end - start);
        let mut offset = start;
        while offset < end
            invariant bucket.packed, start <= offset <= end, end <= bucket.pairs.len(),
                bucket_cells(*bucket) == bucket.pairs@.map_values(|pair: Pair| pair_cell(pair)),
                result@ == bucket_cells(*bucket).subrange(start as int, offset as int),
            decreases end - offset,
        {
            let pair = bucket.pairs[offset];
            result.push(Cell { first: pair.first, second: pair.second, third: 0, count: 1, volume: 0 });
            proof {
                assert(bucket_cells(*bucket).subrange(start as int, offset as int + 1) =~= bucket_cells(*bucket).subrange(start as int, offset as int).push(bucket_cells(*bucket)[offset as int]));
                assert(bucket_cells(*bucket)[offset as int] == pair_cell(bucket.pairs@[offset as int]));
            }
            offset += 1;
        }
        result
    } else {
        proof { assert(bucket_cells(*bucket) == bucket.cells@); }
        copy_range(&bucket.cells, start, end)
    }
}

fn copy_keys(source: &Vec<Major>, spare: usize) -> (result: Vec<Major>)
    ensures result@ == source@,
{
    let mut result = Vec::with_capacity(match source.len().checked_add(spare) { Some(n) => n, None => source.len() });
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    {
        result.push(source[index]);
        proof { assert(source@.take(index as int + 1) =~= source@.take(index as int).push(source@[index as int])); }
        index += 1;
    }
    proof { assert(source@.take(source@.len() as int) =~= source@); }
    result
}

fn copy_ends(source: &Vec<usize>, spare: usize) -> (result: Vec<usize>)
    ensures result@ == source@,
{
    let mut result = Vec::with_capacity(match source.len().checked_add(spare) { Some(n) => n, None => source.len() });
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    {
        result.push(source[index]);
        proof { assert(source@.take(index as int + 1) =~= source@.take(index as int).push(source@[index as int])); }
        index += 1;
    }
    proof { assert(source@.take(source@.len() as int) =~= source@); }
    result
}

/// The persistent form of a flat bucket: exactly sized, packed when its cells allow.
pub fn bucket_pack(flat: &Flat) -> (result: Bucket)
    ensures result.keys@ == flat.keys@, result.ends@ == flat.ends@, bucket_cells(result) == flat.cells@,
{
    proof { reveal(bucket_cells); }
    let n = flat.cells.len();
    let mut simple = true;
    let mut index = 0;
    while index < n
        invariant index <= n, n == flat.cells.len(),
            simple ==> forall|j: int| 0 <= j < index ==> pair_cell(Pair { first: flat.cells@[j].first, second: flat.cells@[j].second }) == flat.cells@[j],
        decreases n - index,
    {
        let cell = flat.cells[index];
        if !(cell.third == 0 && cell.count == 1 && cell.volume == 0) { simple = false; }
        index += 1;
    }
    let keys = copy_keys(&flat.keys, 0);
    let ends = copy_ends(&flat.ends, 0);
    if simple {
        let mut pairs: Vec<Pair> = Vec::with_capacity(n);
        let mut offset = 0;
        while offset < n
            invariant offset <= n, n == flat.cells.len(), pairs@.len() == offset,
                forall|j: int| 0 <= j < n ==> pair_cell(Pair { first: flat.cells@[j].first, second: flat.cells@[j].second }) == flat.cells@[j],
                forall|j: int| 0 <= j < offset ==> pairs@[j] == (Pair { first: flat.cells@[j].first, second: flat.cells@[j].second }),
            decreases n - offset,
        {
            let cell = flat.cells[offset];
            pairs.push(Pair { first: cell.first, second: cell.second });
            offset += 1;
        }
        let result = Bucket { keys, ends, cells: Vec::new(), pairs, packed: true };
        proof {
            assert(bucket_cells(result) =~= flat.cells@) by {
                assert(result.pairs@.map_values(|pair: Pair| pair_cell(pair)).len() == n);
                assert forall|j: int| 0 <= j < n implies result.pairs@.map_values(|pair: Pair| pair_cell(pair))[j] == flat.cells@[j] by {
                    assert(result.pairs@.map_values(|pair: Pair| pair_cell(pair))[j] == pair_cell(result.pairs@[j]));
                }
            }
        }
        result
    } else {
        let cells = copy_cells(&flat.cells);
        Bucket { keys, ends, cells, pairs: Vec::new(), packed: false }
    }
}

/// A flat copy of a persistent bucket with room for `spare` more entries.
pub fn bucket_unpack(bucket: &Bucket, spare: usize) -> (result: Flat)
    ensures result.keys@ == bucket.keys@, result.ends@ == bucket.ends@, result.cells@ == bucket_cells(*bucket),
{
    proof { reveal(bucket_cells); }
    let keys = copy_keys(&bucket.keys, spare);
    let ends = copy_ends(&bucket.ends, spare);
    let n = bucket_cells_len(bucket);
    let mut cells: Vec<Cell> = Vec::with_capacity(match n.checked_add(spare) { Some(m) => m, None => n });
    if bucket.packed {
        let mut index = 0;
        while index < bucket.pairs.len()
            invariant bucket.packed, index <= bucket.pairs.len(), cells@ == bucket.pairs@.map_values(|pair: Pair| pair_cell(pair)).take(index as int),
            decreases bucket.pairs.len() - index,
        {
            let pair = bucket.pairs[index];
            cells.push(Cell { first: pair.first, second: pair.second, third: 0, count: 1, volume: 0 });
            proof {
                let mapped = bucket.pairs@.map_values(|pair: Pair| pair_cell(pair));
                assert(mapped.take(index as int + 1) =~= mapped.take(index as int).push(mapped[index as int]));
                assert(mapped[index as int] == pair_cell(bucket.pairs@[index as int]));
            }
            index += 1;
        }
        proof {
            let mapped = bucket.pairs@.map_values(|pair: Pair| pair_cell(pair));
            assert(mapped.take(mapped.len() as int) =~= mapped);
        }
    } else {
        let mut index = 0;
        while index < bucket.cells.len()
            invariant !bucket.packed, index <= bucket.cells.len(), cells@ == bucket.cells@.take(index as int),
            decreases bucket.cells.len() - index,
        {
            cells.push(bucket.cells[index]);
            proof { assert(bucket.cells@.take(index as int + 1) =~= bucket.cells@.take(index as int).push(bucket.cells@[index as int])); }
            index += 1;
        }
        proof { assert(bucket.cells@.take(bucket.cells@.len() as int) =~= bucket.cells@); }
    }
    Flat { keys, ends, cells }
}

fn bucket_cells_len(bucket: &Bucket) -> (result: usize)
    ensures result as int == bucket_cells(*bucket).len(),
{
    proof { reveal(bucket_cells); }
    if bucket.packed {
        proof { assert(bucket_cells(*bucket).len() == bucket.pairs@.len()); }
        bucket.pairs.len()
    } else { bucket.cells.len() }
}

impl Table {
    pub open spec fn keys_view(self) -> Seq<Major> {
        flat((*self.pages)@.map_values(|page: Arc<Page>| page_keys(*page)))
    }

    pub open spec fn rows(self, key: Major) -> Seq<Cell> {
        let position = slot(key, (*self.pages)@.len() as int * 32);
        let page = (*self.pages)@[position / 32];
        let bucket = (*page).buckets@[position % 32];
        bucket_rows(*bucket, key)
    }

    #[verifier::opaque]
    pub open spec fn at(self, key: Major, minor: Major, volume: bool) -> int {
        cell_sum(self.rows(key), minor, volume)
    }

    pub open spec fn shaped(self) -> bool {
        &&& (*self.pages)@.len() > 0
        &&& (*self.pages)@.len() <= usize::MAX as int / 32
        &&& forall|page: int| 0 <= page < (*self.pages)@.len() ==>
            (*#[trigger] (*self.pages)@[page]).buckets@.len() == 32
        &&& forall|page: int, bucket: int| 0 <= page < (*self.pages)@.len() && 0 <= bucket < 32 ==>
            bucket_valid(*#[trigger] (*(*self.pages)@[page]).buckets@[bucket], page * 32 + bucket, (*self.pages)@.len() as int * 32)
    }

    pub open spec fn valid(self) -> bool {
        &&& self.shaped()
        &&& forall|key: Major, index: int| 0 <= index < self.rows(key).len() ==>
            (#[trigger] self.rows(key)[index]).count != 0 || self.rows(key)[index].volume != 0
        &&& forall|key: Major, left: int, right: int| 0 <= left < right < self.rows(key).len() ==>
            (#[trigger] self.rows(key)[left]).first != (#[trigger] self.rows(key)[right]).first
            || self.rows(key)[left].second != self.rows(key)[right].second
            || self.rows(key)[left].third != self.rows(key)[right].third
    }

    pub open spec fn bucket_of_key(self, key: Major) -> Bucket {
        let position = slot(key, (*self.pages)@.len() as int * 32);
        *(*(*self.pages)@[position / 32]).buckets@[position % 32]
    }

    pub proof fn lemma_shaped_valid(self)
        requires self.shaped(),
        ensures self.valid(), forall|key: Major| rows_valid(#[trigger] self.rows(key)),
    {
        assert forall|key: Major| rows_valid(#[trigger] self.rows(key)) by {
            let slots = (*self.pages)@.len() as int * 32;
            let position = slot(key, slots);
            assert(0 <= position < slots);
            assert(0 <= position / 32 < (*self.pages)@.len());
            assert(0 <= position % 32 < 32);
            assert(position == (position / 32) * 32 + position % 32);
            lemma_bucket_rows_valid(*(*(*self.pages)@[position / 32]).buckets@[position % 32], position, slots, key);
        }
        assert forall|key: Major, index: int| 0 <= index < self.rows(key).len() implies
            (#[trigger] self.rows(key)[index]).count != 0 || self.rows(key)[index].volume != 0 by {
            assert(rows_valid(self.rows(key)));
        }
        assert forall|key: Major, left: int, right: int| 0 <= left < right < self.rows(key).len() implies
            (#[trigger] self.rows(key)[left]).first != (#[trigger] self.rows(key)[right]).first
            || self.rows(key)[left].second != self.rows(key)[right].second
            || self.rows(key)[left].third != self.rows(key)[right].third by {
            assert(rows_valid(self.rows(key)));
            assert(minor_of(self.rows(key)[left]) != minor_of(self.rows(key)[right]));
        }
    }

    pub fn empty(size: usize) -> (result: Self)
        ensures result.valid(), forall|key: Major| #[trigger] result.rows(key) == Seq::<Cell>::empty(),
            forall|key: Major, minor: Major, volume: bool| #[trigger] result.at(key, minor, volume) == 0,
    {
        let page_count = size / 512 + 1;
        let shared = Arc::new(Bucket { keys: Vec::new(), ends: Vec::new(), cells: Vec::new(), pairs: Vec::new(), packed: false });
        let mut pages = PageDirectory::new();
        let mut page_index = 0;
        while page_index < page_count
            invariant page_index <= page_count, pages@.len() == page_index,
                0 < page_count <= usize::MAX as int / 32,
                (*shared).keys@ == Seq::<Major>::empty() && (*shared).ends@ == Seq::<usize>::empty() && (*shared).cells@ == Seq::<Cell>::empty(),
                forall|page: int| 0 <= page < pages@.len() ==> (*#[trigger] pages@[page]).buckets@.len() == 32,
                forall|page: int, bucket: int| 0 <= page < pages@.len() && 0 <= bucket < pages@[page].buckets@.len() ==>
                    (*#[trigger] (*pages@[page]).buckets@[bucket]) == *shared,
            decreases page_count - page_index,
        {
            let mut buckets: Vec<Arc<Bucket>> = Vec::with_capacity(32);
            let mut bucket_index = 0;
            while bucket_index < 32
                invariant bucket_index <= 32, buckets.len() == bucket_index,
                    forall|bucket: int| 0 <= bucket < buckets@.len() ==> (*#[trigger] buckets@[bucket]) == *shared,
                decreases 32 - bucket_index,
            {
                buckets.push(shared.clone());
                bucket_index += 1;
            }
            pages.push(Arc::new(Page { buckets }));
            page_index += 1;
        }
        let result = Table { pages: Arc::new(pages) };
        proof {
            reveal(flat_valid);
            lemma_empty_directory((*result.pages)@);
            assert forall|page: int, bucket: int| 0 <= page < (*result.pages)@.len() && 0 <= bucket < 32 implies
                bucket_valid(*#[trigger] (*(*result.pages)@[page]).buckets@[bucket], page * 32 + bucket, (*result.pages)@.len() as int * 32) by {
                let selected = (*(*result.pages)@[page]).buckets@[bucket];
                assert(*selected == *shared);
                assert(flat_valid(Seq::<Major>::empty(), Seq::<usize>::empty(), Seq::<Cell>::empty(), page * 32 + bucket, (*result.pages)@.len() as int * 32));
            }
            result.lemma_shaped_valid();
            assert forall|key: Major| #[trigger] result.rows(key) == Seq::<Cell>::empty() by {
                let slots = (*result.pages)@.len() as int * 32;
                let position = slot(key, slots);
                assert(0 <= position < slots);
                let selected = (*(*result.pages)@[position / 32]).buckets@[position % 32];
                assert(*selected == *shared);
                assert(!(*selected).keys@.contains(key));
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

    /// The bucket that holds slot `position`.
    pub fn bucket_at(&self, position: usize) -> (result: Arc<Bucket>)
        requires self.valid(), position < (*self.pages)@.len() * 32,
        ensures bucket_valid(*result, position as int, (*self.pages)@.len() as int * 32),
            forall|key: Major| slot(key, (*self.pages)@.len() as int * 32) == position as int ==> #[trigger] self.rows(key) == bucket_rows(*result, key),
    {
        proof { reveal(Table::valid); }
        let page = self.pages.get(position / 32);
        let bucket = page.buckets[position % 32].clone();
        proof {
            assert(bucket_valid(*(*(*self.pages)@[(position / 32) as int]).buckets@[(position % 32) as int], ((position / 32) * 32 + position % 32) as int, (*self.pages)@.len() as int * 32));
            assert(position == (position / 32) * 32 + position % 32);
        }
        bucket
    }

    /// The table with slot `position` replaced by `replacement`.
    pub fn with_bucket(&self, position: usize, replacement: Arc<Bucket>) -> (result: Self)
        requires self.valid(), position < (*self.pages)@.len() * 32,
            bucket_valid(*replacement, position as int, (*self.pages)@.len() as int * 32),
        ensures result.valid(), (*result.pages)@.len() == (*self.pages)@.len(),
            forall|key: Major| #[trigger] result.rows(key) ==
                if slot(key, (*self.pages)@.len() as int * 32) == position as int { bucket_rows(*replacement, key) } else { self.rows(key) },
    {
        proof { reveal(Table::valid); }
        let page_index = position / 32;
        let bucket_index = position % 32;
        let original_page = self.pages.get(page_index);
        let mut buckets: Vec<Arc<Bucket>> = Vec::with_capacity(32);
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
            let slots = (*self.pages)@.len() as int * 32;
            assert((*result.pages)@ == (*self.pages)@.update(page_index as int, page));
            assert(position == page_index * 32 + bucket_index);
            assert forall|p: int, b: int| 0 <= p < (*result.pages)@.len() && 0 <= b < 32 implies
                bucket_valid(*#[trigger] (*(*result.pages)@[p]).buckets@[b], p * 32 + b, slots) by {
                if p == page_index as int {
                    assert((*(*result.pages)@[p]).buckets@[b] == buckets@[b]);
                    if b == bucket_index as int { assert(*buckets@[b] == *replacement); }
                    else { assert(*buckets@[b] == *self.pages@[page_index as int].buckets@[b]); }
                }
            }
            assert(result.shaped());
            result.lemma_shaped_valid();
            assert forall|key: Major| #[trigger] result.rows(key) ==
                if slot(key, slots) == position as int { bucket_rows(*replacement, key) } else { self.rows(key) } by {
                let other_position = slot(key, slots);
                assert(0 <= other_position < slots);
                assert(other_position == (other_position / 32) * 32 + other_position % 32);
                if other_position / 32 == page_index as int {
                    let b = other_position % 32;
                    assert((*(*result.pages)@[page_index as int]).buckets@[b] == buckets@[b]);
                    if b == bucket_index as int { assert(*buckets@[b] == *replacement); }
                    else { assert(*buckets@[b] == *self.pages@[page_index as int].buckets@[b]); }
                }
            }
        }
        result
    }

    pub fn replace(&self, key: Major, rows: Arc<Vec<Cell>>) -> (result: Self)
        requires self.valid(), rows_valid((*rows)@),
        ensures result.valid(), forall|other: Major| #[trigger] result.rows(other) ==
            if other == key { (*rows)@ } else { self.rows(other) },
    {
        proof { reveal(Table::valid); }
        let slots = self.pages.len() * 32;
        let position = slot_exec(key, slots);
        let bucket = self.bucket_at(position);
        let ghost position_int = position as int;
        let ghost slots_int = slots as int;
        let flat0 = bucket_unpack(&*bucket, 0);
        let ghost bucket_spec = arc_bucket(&bucket);
        let ghost flat_spec = flat_val(&flat0);
        proof {
            lemma_bucket_facts(bucket_spec, position_int, slots_int);
            lemma_bucket_flat_rows(bucket_spec, flat_spec);
            lemma_flat_facts(flat_spec, position_int, slots_int);
        }
        let mut draft = draft_of(&flat0, Ghost(position_int), Ghost(slots_int));
        let ghost d0 = draft;
        proof { lemma_rows_agree(flat_spec, d0); }
        let _removed = draft_remove(&mut draft, key, Ghost(position_int), Ghost(slots_int));
        let ghost d1 = draft;
        draft_insert(&mut draft, key, copy_cells(&*rows), Ghost(position_int), Ghost(slots_int));
        let replacement_flat = flat_of(&draft, Ghost(position_int), Ghost(slots_int));
        let replacement = bucket_pack(&replacement_flat);
        let ghost replacement_spec = replacement_flat_spec(&replacement_flat);
        proof {
            lemma_flat_facts(replacement_spec, position_int, slots_int);
            lemma_rows_agree(replacement_spec, draft);
            lemma_bucket_flat_rows(replacement, replacement_spec);
        }
        let result = self.with_bucket(position, Arc::new(replacement));
        proof {
            assert forall|other: Major| #[trigger] result.rows(other) == if other == key { (*rows)@ } else { self.rows(other) } by {
                if slot(other, slots_int) == position_int {
                    assert(result.rows(other) == bucket_rows(replacement, other));
                    assert(bucket_rows(replacement, other) == draft_rows(draft, other));
                    assert(self.rows(other) == bucket_rows(*bucket, other));
                    assert(bucket_rows(bucket_spec, other) == flat_rows(flat_spec, other));
                    if other != key {
                        assert(draft_rows(d1, other) == draft_rows(d0, other));
                    }
                }
            }
        }
        result
    }

    #[verifier::rlimit(100)]
    pub fn get(&self, key: Major) -> (result: Arc<Vec<Cell>>)
        requires self.valid(),
        ensures (*result)@ == self.rows(key), rows_valid((*result)@),
    {
        proof { reveal(Table::valid); }
        let position = slot_exec(key, self.pages.len() * 32);
        let page = self.pages.get(position / 32);
        let bucket = &page.buckets[position % 32];
        proof { reveal(flat_valid); }
        let ghost bucket_spec = arc_bucket(bucket);
        proof {
            let slots = (*self.pages)@.len() as int * 32;
            assert(position as int == slot(key, slots));
            assert(position == (position / 32) * 32 + position % 32);
            assert(bucket_valid(**bucket, position as int, slots));
            assert(self.rows(key) == bucket_rows(**bucket, key));
            lemma_bucket_rows_valid(bucket_spec, position as int, slots, key);
            self.lemma_shaped_valid();
        }
        let mut i = 0;
        while i < bucket.keys.len()
            invariant i <= bucket.keys.len(), bucket_valid(**bucket, position as int, (*self.pages)@.len() as int * 32),
                bucket_spec == **bucket, self.rows(key) == bucket_rows(**bucket, key),
                forall|j: int| 0 <= j < i ==> bucket.keys@[j] != key,
            decreases bucket.keys.len() - i,
        {
            proof { lemma_bucket_facts(bucket_spec, position as int, (*self.pages)@.len() as int * 32); }
            if bucket.keys[i] == key {
                let start = row_start(&bucket.ends, i);
                let end = bucket.ends[i];
                proof {
                    lemma_keyed_index(bucket.keys@, i as int);
                    assert(seq_start(bucket.ends@, i as int) < bucket.ends@[i as int] as int && bucket.ends@[i as int] as int <= bucket_cells(bucket_spec).len());
                    lemma_bucket_rows_valid(bucket_spec, position as int, (*self.pages)@.len() as int * 32, key);
                }
                return Arc::new(bucket_range(&**bucket, start, end));
            }
            i += 1;
        }
        proof {
            assert(!bucket.keys@.contains(key)) by {
                if bucket.keys@.contains(key) {
                    let j = choose|j: int| 0 <= j < bucket.keys@.len() && bucket.keys@[j] == key;
                    assert(bucket.keys@[j] != key);
                }
            }
            assert(self.rows(key) =~= Seq::<Cell>::empty());
            assert(rows_valid(Seq::<Cell>::empty()));
        }
        Arc::new(Vec::new())
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

    #[verifier::rlimit(100)]
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

pub proof fn lemma_keys_complete(table: Table)
    requires table.valid(),
    ensures forall|key: Major| #[trigger] table.keys_view().contains(key) <==> table.rows(key).len() > 0,
{
    let pages = (*table.pages)@;
    assert forall|key: Major| #[trigger] table.keys_view().contains(key) <==> table.rows(key).len() > 0 by {
        let page_sets = pages.map_values(|page: Arc<Page>| page_keys(*page));
        lemma_flat_contains(page_sets, key);
        let position = slot(key, pages.len() as int * 32);
        assert(0 <= position < pages.len() as int * 32);
        assert(position == (position / 32) * 32 + position % 32);
        if table.keys_view().contains(key) {
            let page_index = choose|page_index: int| 0 <= page_index < page_sets.len() && (#[trigger] page_sets[page_index]).contains(key);
            let buckets = (*pages[page_index]).buckets@;
            let bucket_sets = buckets.map_values(|bucket: Arc<Bucket>| (*bucket).keys@);
            lemma_flat_contains(bucket_sets, key);
            let bucket_index = choose|bucket_index: int| 0 <= bucket_index < bucket_sets.len() && (#[trigger] bucket_sets[bucket_index]).contains(key);
            let bucket = *buckets[bucket_index];
            let offset = choose|offset: int| 0 <= offset < bucket.keys@.len() && #[trigger] bucket.keys@[offset] == key;
            assert(bucket_valid(bucket, page_index * 32 + bucket_index, pages.len() as int * 32));
            lemma_bucket_facts(bucket, page_index * 32 + bucket_index, pages.len() as int * 32);
            assert(slot(bucket.keys@[offset], pages.len() as int * 32) == page_index * 32 + bucket_index);
            assert(position == page_index * 32 + bucket_index);
            assert(bucket.keys@.contains(key));
            lemma_bucket_rows_valid(bucket, position, pages.len() as int * 32, key);
            assert(table.rows(key) == bucket_rows(bucket, key));
        } else if table.rows(key).len() > 0 {
            let buckets = (*pages[position / 32]).buckets@;
            let bucket = *buckets[position % 32];
            assert(table.rows(key) == bucket_rows(bucket, key));
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
    assert(bucket_valid(bucket, page_index * 32 + bucket_index, (*table.pages)@.len() as int * 32));
    lemma_bucket_facts(bucket, page_index * 32 + bucket_index, (*table.pages)@.len() as int * 32);
    assert(slot(bucket.keys@[offset], (*table.pages)@.len() as int * 32) == page_index * 32 + bucket_index);
    assert(slot(key, (*table.pages)@.len() as int * 32) == page_index * 32 + bucket_index);
}

pub proof fn lemma_keys_unique(table: Table)
    requires table.valid(),
    ensures table.keys_view().no_duplicates(),
{
    reveal(flat_valid);
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
    pub drafts: HashMap<usize, Flat>,
    pub touched: Vec<usize>,
}

pub fn copy_cells(source: &Vec<Cell>) -> (result: Vec<Cell>)
    ensures result@ == source@,
{
    let mut result = Vec::with_capacity(source.len());
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
    pub open spec fn slots(self) -> int {
        (*self.base.pages)@.len() as int * 32
    }

    pub open spec fn valid(self) -> bool {
        &&& self.base.valid()
        &&& forall|s: usize| #[trigger] self.drafts@.contains_key(s) ==> (s as int) < self.slots() && self.touched@.contains(s)
        &&& forall|i: int| 0 <= i < self.touched@.len() ==> self.drafts@.contains_key(#[trigger] self.touched@[i])
        &&& forall|left: int, right: int| 0 <= left < right < self.touched@.len() ==>
            #[trigger] self.touched@[left] != #[trigger] self.touched@[right]
        &&& forall|s: usize| #[trigger] self.drafts@.contains_key(s) ==>
            flat_ok(self.drafts@[s], s as int, self.slots())
    }

    pub open spec fn rows(self, key: Major) -> Seq<Cell> {
        let s = slot(key, self.slots());
        if 0 <= s <= usize::MAX as int && self.drafts@.contains_key(s as usize) {
            flat_rows(self.drafts@[s as usize], key)
        } else { self.base.rows(key) }
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
        proof { reveal(Work::at); reveal(Table::at); }
        let result = Work { base: base.share(), drafts: HashMap::new(), touched: Vec::new() };
        proof {
            assert(result.drafts@ == Map::<usize, Flat>::empty());
            assert forall|key: Major| #[trigger] result.rows(key) == base.rows(key) by {}
        }
        result
    }

    /// Valid except that the draft of slot `hole` is checked out.
    pub open spec fn hollow(self, hole: usize) -> bool {
        &&& self.base.valid()
        &&& (hole as int) < self.slots()
        &&& !self.drafts@.contains_key(hole)
        &&& self.touched@.contains(hole)
        &&& forall|s: usize| #[trigger] self.drafts@.contains_key(s) ==> (s as int) < self.slots() && self.touched@.contains(s)
            && flat_ok(self.drafts@[s], s as int, self.slots())
        &&& forall|i: int| 0 <= i < self.touched@.len() ==>
            (self.drafts@.contains_key(#[trigger] self.touched@[i]) || self.touched@[i] == hole)
        &&& forall|left: int, right: int| 0 <= left < right < self.touched@.len() ==>
            #[trigger] self.touched@[left] != #[trigger] self.touched@[right]
    }

    /// Check out the bucket of slot `position` (copied from the base when untouched).
    fn open_bucket(&mut self, position: usize) -> (draft: Flat)
        requires old(self).valid(), (position as int) < old(self).slots(),
        ensures final(self).hollow(position), final(self).base == old(self).base, final(self).slots() == old(self).slots(),
            final(self).drafts@ == old(self).drafts@.remove(position),
            flat_ok(draft, position as int, old(self).slots()),
            forall|key: Major| slot(key, old(self).slots()) == position as int ==> #[trigger] flat_rows(draft, key) == old(self).rows(key),
    {
        let ghost before = *self;
        match self.drafts.remove(&position) {
            Some(existing) => {
                proof {
                    assert(before.drafts@.contains_key(position));
                    assert forall|key: Major| slot(key, before.slots()) == position as int implies #[trigger] flat_rows(existing, key) == before.rows(key) by {}
                    assert forall|i: int| 0 <= i < self.touched@.len() implies
                        (self.drafts@.contains_key(#[trigger] self.touched@[i]) || self.touched@[i] == position) by {
                        assert(before.drafts@.contains_key(self.touched@[i]));
                    }
                }
                existing
            },
            None => {
                proof {
                    assert(!before.touched@.contains(position)) by {
                        if before.touched@.contains(position) {
                            let i = choose|i: int| 0 <= i < before.touched@.len() && before.touched@[i] == position;
                            assert(before.drafts@.contains_key(before.touched@[i]));
                        }
                    }
                }
                self.touched.push(position);
                let bucket = self.base.bucket_at(position);
                let ghost bucket_spec = arc_bucket(&bucket);
                let draft = bucket_unpack(&*bucket, 8);
                proof {
                    assert(self.touched@ == before.touched@.push(position));
                    assert forall|key: Major| slot(key, before.slots()) == position as int implies #[trigger] flat_rows(draft, key) == before.rows(key) by {
                        assert(0 <= position as int <= usize::MAX as int);
                        assert(!before.drafts@.contains_key(position));
                        assert(draft.keys@ == bucket_spec.keys@ && draft.ends@ == bucket_spec.ends@ && draft.cells@ == bucket_cells(bucket_spec));
                    }
                    assert forall|i: int| 0 <= i < self.touched@.len() implies
                        (self.drafts@.contains_key(#[trigger] self.touched@[i]) || self.touched@[i] == position) by {
                        if i < before.touched@.len() { assert(before.drafts@.contains_key(before.touched@[i])); }
                    }
                    assert forall|left: int, right: int| 0 <= left < right < self.touched@.len() implies
                        #[trigger] self.touched@[left] != #[trigger] self.touched@[right] by {
                        if right == self.touched@.len() - 1 {
                            assert(before.touched@[left] != position) by {
                                if before.touched@[left] == position { assert(before.touched@.contains(position)); }
                            }
                        }
                    }
                }
                draft
            },
        }
    }

    pub fn add(&mut self, key: Major, minor: Major, count: i128, volume: i128) -> (result: Option<()>)
        requires old(self).valid(),
        ensures result is None ==> add_overflows(old(self).at(key, minor, false), old(self).at(key, minor, true), count, volume),
            result is Some ==> final(self).valid(),
            result is Some ==> forall|other: Major, small: Major, measure: bool|
            #[trigger] final(self).at(other, small, measure) == old(self).at(other, small, measure)
                + if other == key && small == minor { if measure { volume as int } else { count as int } } else { 0 },
    {
        if count == 0 && volume == 0 {
            proof {
                reveal(Work::at);
                assert forall|other: Major, small: Major, measure: bool| #[trigger] self.at(other, small, measure) == old(self).at(other, small, measure)
                    + if other == key && small == minor { if measure { volume as int } else { count as int } } else { 0 } by {}
            }
            return Some(());
        }
        let ghost before = *self;
        let slots = self.base.pages.len() * 32;
        let position = slot_exec(key, slots);
        let ghost position_int = position as int;
        let ghost slots_int = slots as int;
        let draft = self.open_bucket(position);
        let ghost mid = *self;
        let ghost d0 = draft;
        let draft = match flat_add(draft, key, minor, count, volume, Ghost(position_int), Ghost(slots_int)) {
            Some(value) => value,
            None => {
                proof {
                    reveal(Work::at);
                    assert(before.rows(key) == flat_rows(d0, key));
                }
                return None;
            },
        };
        let ghost d2 = draft;
        self.drafts.insert(position, draft);
        proof {
            reveal(Work::at);
            assert(self.base == before.base);
            assert(self.slots() == before.slots());
            assert(self.drafts@ == mid.drafts@.insert(position, d2));
            assert(self.touched@ == mid.touched@);
            assert forall|s: usize| #[trigger] self.drafts@.contains_key(s) implies (s as int) < self.slots() && self.touched@.contains(s)
                && flat_ok(self.drafts@[s], s as int, self.slots()) by {
                if s != position { assert(mid.drafts@.contains_key(s)); }
            }
            assert forall|i: int| 0 <= i < self.touched@.len() implies self.drafts@.contains_key(#[trigger] self.touched@[i]) by {
                if self.touched@[i] != position { assert(mid.drafts@.contains_key(self.touched@[i]) || self.touched@[i] == position); }
            }
            assert(self.valid());
            assert forall|other: Major, small: Major, measure: bool| #[trigger] self.at(other, small, measure) == before.at(other, small, measure)
                + if other == key && small == minor { if measure { volume as int } else { count as int } } else { 0 } by {
                let s = slot(other, slots_int);
                if s == position_int {
                    assert(self.rows(other) == flat_rows(d2, other));
                    assert(before.rows(other) == flat_rows(d0, other));
                } else {
                    assert(self.rows(other) == before.rows(other));
                }
            }
        }
        Some(())
    }

    pub fn finish(self) -> (result: Table)
        requires self.valid(),
        ensures result.valid(), forall|key: Major| #[trigger] result.rows(key) == self.rows(key),
            forall|key: Major, minor: Major, measure: bool| #[trigger] result.at(key, minor, measure) == self.at(key, minor, measure),
    {
        let ghost original = self;
        let mut work = self;
        let mut result = work.base.share();
        let ghost slots_int = work.slots();
        let mut index = 0;
        while index < work.touched.len()
            invariant
                original.valid(), work.touched@ == original.touched@, work.base == original.base, work.slots() == original.slots(),
                index <= work.touched.len(), result.valid(), (*result.pages)@.len() == (*original.base.pages)@.len(),
                slots_int == original.slots(),
                forall|s: usize| #[trigger] work.drafts@.contains_key(s) <==>
                    original.drafts@.contains_key(s) && !original.touched@.take(index as int).contains(s),
                forall|s: usize| #[trigger] work.drafts@.contains_key(s) ==> work.drafts@[s] == original.drafts@[s],
                forall|key: Major| #[trigger] result.rows(key) ==
                    if 0 <= slot(key, slots_int) <= usize::MAX as int && original.touched@.take(index as int).contains(slot(key, slots_int) as usize) {
                        original.rows(key)
                    } else { original.base.rows(key) },
            decreases work.touched.len() - index,
        {
            let s = work.touched[index];
            proof { assert(original.drafts@.contains_key(original.touched@[index as int])); assert(work.drafts@.contains_key(s)); }
            let draft = match work.drafts.remove(&s) {
                Some(draft) => draft,
                None => { proof { assert(false); } return result; },
            };
            let bucket = bucket_pack(&draft);
            proof {
                assert(draft == original.drafts@[s]);
                lemma_bucket_flat_rows(bucket, flat_val(&draft));
                assert(bucket_valid(bucket, s as int, slots_int));
                assert(s as int <= (*result.pages)@.len() as int * 32);
            }
            let ghost previous = result;
            result = result.with_bucket(s, Arc::new(bucket));
            proof {
                assert(original.touched@.take(index as int + 1) =~= original.touched@.take(index as int).push(s));
                assert forall|key: Major| #[trigger] result.rows(key) ==
                    if 0 <= slot(key, slots_int) <= usize::MAX as int && original.touched@.take(index as int + 1).contains(slot(key, slots_int) as usize) {
                        original.rows(key)
                    } else { original.base.rows(key) } by {
                    let k = slot(key, slots_int);
                    assert(0 <= k < slots_int);
                    if k == s as int {
                        assert(original.touched@.take(index as int + 1).contains(s));
                        assert(result.rows(key) == bucket_rows(bucket, key));
                        assert(original.rows(key) == flat_rows(original.drafts@[s], key));
                    } else {
                        assert(result.rows(key) == previous.rows(key));
                        assert(original.touched@.take(index as int + 1).contains(k as usize) == original.touched@.take(index as int).contains(k as usize));
                    }
                }
                assert forall|t: usize| #[trigger] work.drafts@.contains_key(t) <==>
                    original.drafts@.contains_key(t) && !original.touched@.take(index as int + 1).contains(t) by {
                    if t == s { assert(original.touched@.take(index as int + 1).contains(s)); }
                    else { assert(original.touched@.take(index as int + 1).contains(t) == original.touched@.take(index as int).contains(t)); }
                }
            }
            index += 1;
        }
        proof {
            assert(original.touched@.take(original.touched@.len() as int) =~= original.touched@);
            assert forall|key: Major| #[trigger] result.rows(key) == original.rows(key) by {
                let k = slot(key, slots_int);
                assert(0 <= k < slots_int);
                if original.drafts@.contains_key(k as usize) {
                    assert(original.touched@.contains(k as usize));
                } else {
                    if original.touched@.contains(k as usize) {
                        let i = choose|i: int| 0 <= i < original.touched@.len() && original.touched@[i] == k as usize;
                        assert(original.drafts@.contains_key(original.touched@[i]));
                    }
                }
            }
            reveal(Work::at);
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] result.at(key, minor, measure) == original.at(key, minor, measure) by {
                reveal(Table::at);
                assert(result.rows(key) == original.rows(key));
            }
        }
        result
    }
}

}
