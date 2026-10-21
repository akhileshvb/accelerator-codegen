// Compact ordered map from an integer key to a Moment, replacing Index<Moment>.
//
// Memory: an Index<Moment> costs ~190 bytes an entry (an Arc'd AVL node, an Arc'd Moment and
// an Arc'd key text each).  Here the entries live in sorted runs of 24-byte cells
// (key, count, value as i64), one run per bucket `key >> shift`; the buckets sit in an Index
// keyed by bucket number.  A persistent version shares runs; an update copies the one run it
// touches.  The specification mirrors Index<Moment>: entries / at / view / wf / model.
use std::sync::Arc;
use vstd::prelude::*;
use verus_common::rel::{flat, lemma_flat_contains};
use verus_common::sums::*;
use verus_prooflib::ordered::{Index, Key, KeyView, key_lt, ordered_rows, drop_key, lemma_ordered_unique,
    lemma_drop_bindings, lemma_drop_ordered, lemma_drop_sum};
use crate::values::*;
use verus_prooflib::wide::Wide;

verus! {

broadcast use vstd::seq_lib::group_seq_properties;

pub struct Cell {
    pub key: i64,
    pub count: Wide,
    pub value: Wide,
}

impl Cell {
    pub fn dup(&self) -> (result: Cell)
        ensures result == *self,
    { Cell { key: self.key, count: self.count.dup(), value: self.value.dup() } }
}

pub struct Run {
    pub cells: Vec<Cell>,
}

pub open spec fn cell_key(cell: Cell) -> KeyView { (cell.key as int, Seq::<u8>::empty()) }
pub open spec fn cell_moment(cell: Cell) -> Moment { Moment { count: cell.count@, value: cell.value@ } }
pub open spec fn cell_row(cell: Cell) -> (KeyView, Moment) { (cell_key(cell), cell_moment(cell)) }
pub open spec fn cell_rows(cells: Seq<Cell>) -> Seq<(KeyView, Moment)> { cells.map_values(|cell: Cell| cell_row(cell)) }
pub open spec fn run_rows(run: Run) -> Seq<(KeyView, Moment)> { cell_rows(run.cells@) }

pub open spec fn bucket_of(key: i64, shift: u32) -> i64 { key >> shift }

pub proof fn lemma_bucket_monotone(a: i64, b: i64, shift: u32)
    requires a <= b, shift < 64,
    ensures bucket_of(a, shift) <= bucket_of(b, shift),
{
    assert((a >> shift) <= (b >> shift)) by (bit_vector)
        requires a <= b, shift < 64;
}

pub open spec fn cells_sorted(cells: Seq<Cell>) -> bool {
    forall|i: int, j: int| 0 <= i < j < cells.len() ==> (#[trigger] cells[i]).key < (#[trigger] cells[j]).key
}

pub open spec fn run_ok(run: Run, bucket: int, shift: u32) -> bool {
    &&& run.cells@.len() > 0
    &&& cells_sorted(run.cells@)
    &&& forall|i: int| 0 <= i < run.cells@.len() ==> bucket_of((#[trigger] run.cells@[i]).key, shift) as int == bucket
}

pub open spec fn find_cell(cells: Seq<Cell>, key: int) -> Option<Cell>
    decreases cells.len(),
{
    if cells.len() == 0 { None }
    else if cells.last().key as int == key { Some(cells.last()) }
    else { find_cell(cells.drop_last(), key) }
}

pub open spec fn tops_ok(tops: Seq<(KeyView, Run)>, shift: u32) -> bool {
    &&& forall|i: int| 0 <= i < tops.len() ==> {
        let e = #[trigger] tops[i];
        &&& e.0.1 == Seq::<u8>::empty()
        &&& i64::MIN <= e.0.0 <= i64::MAX
        &&& run_ok(e.1, e.0.0, shift)
    }
    &&& ordered_rows(tops)
}

pub open spec fn flat_rows(tops: Seq<(KeyView, Run)>) -> Seq<(KeyView, Moment)> {
    flat(tops.map_values(|e: (KeyView, Run)| run_rows(e.1)))
}

pub proof fn lemma_key_lt_num(a: int, b: int)
    ensures key_lt((a, Seq::<u8>::empty()), (b, Seq::<u8>::empty())) <==> a < b,
{
    verus_prooflib::ordered::lemma_key_order((a, Seq::<u8>::empty()), (a, Seq::<u8>::empty()));
}

pub proof fn lemma_ordered_concat(left: Seq<(KeyView, Moment)>, right: Seq<(KeyView, Moment)>)
    requires ordered_rows(left), ordered_rows(right),
        forall|i: int, j: int| 0 <= i < left.len() && 0 <= j < right.len() ==> key_lt((#[trigger] left[i]).0, (#[trigger] right[j]).0),
    ensures ordered_rows(left + right),
{
    assert forall|first: int, last: int| #![auto] 0 <= first < last < (left + right).len() implies
        key_lt((left + right)[first].0, (left + right)[last].0) by {
        if last < left.len() {
        } else if first >= left.len() {
            assert((left + right)[first] == right[first - left.len()]);
            assert((left + right)[last] == right[last - left.len()]);
        } else {
            assert((left + right)[first] == left[first]);
            assert((left + right)[last] == right[last - left.len()]);
        }
    }
}

pub proof fn lemma_cells_ordered(cells: Seq<Cell>)
    requires cells_sorted(cells),
    ensures ordered_rows(cell_rows(cells)),
{
    let rows = cell_rows(cells);
    assert forall|first: int, last: int| #![auto] 0 <= first < last < rows.len() implies key_lt(rows[first].0, rows[last].0) by {
        assert(rows[first] == cell_row(cells[first]));
        assert(rows[last] == cell_row(cells[last]));
        lemma_key_lt_num(cells[first].key as int, cells[last].key as int);
    }
}

pub proof fn lemma_row_in_run(run: Run, bucket: int, shift: u32, row: (KeyView, Moment))
    requires run_ok(run, bucket, shift), run_rows(run).contains(row),
    ensures row.0.1 == Seq::<u8>::empty(), i64::MIN <= row.0.0 <= i64::MAX,
        bucket_of(row.0.0 as i64, shift) as int == bucket,
        find_cell(run.cells@, row.0.0) is Some,
        cell_moment(find_cell(run.cells@, row.0.0).unwrap()) == row.1,
{
    let rows = run_rows(run);
    let position = choose|position: int| 0 <= position < rows.len() && rows[position] == row;
    assert(rows[position] == cell_row(run.cells@[position]));
    lemma_find_cell(run.cells@, position);
}

// With sorted cells, find_cell finds the cell at a position.
pub proof fn lemma_find_cell(cells: Seq<Cell>, position: int)
    requires cells_sorted(cells), 0 <= position < cells.len(),
    ensures find_cell(cells, cells[position].key as int) == Some(cells[position]),
    decreases cells.len(),
{
    if position < cells.len() - 1 {
        assert(cells[position].key < cells.last().key);
        lemma_find_cell(cells.drop_last(), position);
    }
}

pub proof fn lemma_find_none(cells: Seq<Cell>, key: int)
    requires cells_sorted(cells), find_cell(cells, key) is None,
    ensures forall|i: int| 0 <= i < cells.len() ==> (#[trigger] cells[i]).key as int != key,
    decreases cells.len(),
{
    if cells.len() > 0 {
        lemma_find_none(cells.drop_last(), key);
        assert(cells.drop_last().len() == cells.len() - 1);
        assert forall|i: int| 0 <= i < cells.len() implies (#[trigger] cells[i]).key as int != key by {
            if i < cells.len() - 1 { assert(cells.drop_last()[i] == cells[i]); }
        }
    }
}

pub proof fn lemma_find_some(cells: Seq<Cell>, key: int)
    requires find_cell(cells, key) is Some,
    ensures exists|i: int| 0 <= i < cells.len() && (#[trigger] cells[i]).key as int == key && cells[i] == find_cell(cells, key).unwrap(),
    decreases cells.len(),
{
    if cells.last().key as int != key {
        lemma_find_some(cells.drop_last(), key);
        let i = choose|i: int| 0 <= i < cells.drop_last().len() && (#[trigger] cells.drop_last()[i]).key as int == key
            && cells.drop_last()[i] == find_cell(cells.drop_last(), key).unwrap();
        assert(cells[i] == cells.drop_last()[i]);
    } else {
        assert(cells[cells.len() - 1] == cells.last());
    }
}

// The rows of a run set are ordered, and each row's key sits in its run's bucket.
pub proof fn lemma_flat_ordered(tops: Seq<(KeyView, Run)>, shift: u32)
    requires tops_ok(tops, shift), shift < 64,
    ensures ordered_rows(flat_rows(tops)),
    decreases tops.len(),
{
    let f = |e: (KeyView, Run)| run_rows(e.1);
    if tops.len() > 0 {
        let prefix = tops.drop_last();
        assert(tops_ok(prefix, shift)) by {
            assert forall|first: int, last: int| #![auto] 0 <= first < last < prefix.len() implies key_lt(prefix[first].0, prefix[last].0) by {}
        }
        lemma_flat_ordered(prefix, shift);
        assert(tops.map_values(f).drop_last() =~= prefix.map_values(f));
        let left = flat_rows(prefix);
        let right = run_rows(tops.last().1);
        assert(flat_rows(tops) == left + right);
        lemma_cells_ordered(tops.last().1.cells@);
        assert forall|i: int, j: int| 0 <= i < left.len() && 0 <= j < right.len() implies key_lt((#[trigger] left[i]).0, (#[trigger] right[j]).0) by {
            lemma_flat_contains(prefix.map_values(f), left[i]);
            let t = choose|t: int| 0 <= t < prefix.map_values(f).len() && (#[trigger] prefix.map_values(f)[t]).contains(left[i]);
            assert(prefix.map_values(f)[t] == run_rows(prefix[t].1));
            lemma_row_in_run(prefix[t].1, prefix[t].0.0, shift, left[i]);
            assert(right[j] == cell_row(tops.last().1.cells@[j]));
            assert(run_rows(tops.last().1).contains(right[j]));
            lemma_row_in_run(tops.last().1, tops.last().0.0, shift, right[j]);
            assert(key_lt(prefix[t].0, tops.last().0));
            lemma_key_lt_num(prefix[t].0.0, tops.last().0.0);
            let a = left[i].0.0 as i64;
            let b = right[j].0.0 as i64;
            if a >= b { lemma_bucket_monotone(b, a, shift); }
            lemma_key_lt_num(left[i].0.0, right[j].0.0);
        }
        lemma_ordered_concat(left, right);
    }
}

pub struct Packed {
    top: Index<Run>,
    shift: u32,
}

impl Packed {
    pub closed spec fn shift(self) -> u32 { self.shift }
    pub closed spec fn tops(self) -> Seq<(KeyView, Run)> { self.top.entries() }

    pub closed spec fn entries(self) -> Seq<(KeyView, Moment)> { flat_rows(self.top.entries()) }

    pub closed spec fn wf(self) -> bool {
        &&& self.top.wf()
        &&& self.shift < 64
        &&& tops_ok(self.top.entries(), self.shift)
    }

    pub closed spec fn at(self, key: KeyView) -> Option<Moment> {
        if key.1 == Seq::<u8>::empty() && i64::MIN <= key.0 <= i64::MAX {
            match self.top.at((bucket_of(key.0 as i64, self.shift) as int, Seq::<u8>::empty())) {
                Some(run) => match find_cell(run.cells@, key.0) { Some(cell) => Some(cell_moment(cell)), None => None },
                None => None,
            }
        } else { None }
    }

    pub closed spec fn view(self) -> Map<KeyView, Moment> {
        Map::new(self.entries().map_values(|row: (KeyView, Moment)| row.0).to_set(),
            |key: KeyView| self.at(key).unwrap())
    }

    pub proof fn model(&self)
        requires self.wf(),
        ensures
            self.wf(),
            ordered_rows(self.entries()),
            forall|position: int| #![trigger self.entries()[position]] 0 <= position < self.entries().len() ==>
                self.at(self.entries()[position].0) == Some(self.entries()[position].1),
            forall|query: KeyView| #![trigger self.at(query)] self.at(query).is_some() <==>
                exists|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == query,
    {
        self.top.model();
        let tops = self.top.entries();
        let f = |e: (KeyView, Run)| run_rows(e.1);
        lemma_flat_ordered(tops, self.shift);
        assert forall|position: int| #![trigger self.entries()[position]] 0 <= position < self.entries().len() implies
            self.at(self.entries()[position].0) == Some(self.entries()[position].1) by {
            let row = self.entries()[position];
            assert(self.entries().contains(row));
            lemma_flat_contains(tops.map_values(f), row);
            let t = choose|t: int| 0 <= t < tops.map_values(f).len() && (#[trigger] tops.map_values(f)[t]).contains(row);
            assert(tops.map_values(f)[t] == run_rows(tops[t].1));
            lemma_row_in_run(tops[t].1, tops[t].0.0, self.shift, row);
            assert(self.top.at(tops[t].0) == Some(tops[t].1));
            assert(tops[t].0 == (bucket_of(row.0.0 as i64, self.shift) as int, Seq::<u8>::empty()));
        }
        assert forall|query: KeyView| #![trigger self.at(query)] self.at(query).is_some() <==>
            exists|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == query by {
            if self.at(query).is_some() {
                let b = (bucket_of(query.0 as i64, self.shift) as int, Seq::<u8>::empty());
                let run = self.top.at(b).unwrap();
                let position = choose|position: int| #![auto] 0 <= position < tops.len() && tops[position].0 == b;
                assert(tops[position].1 == run);
                let cell = find_cell(run.cells@, query.0).unwrap();
                lemma_find_some(run.cells@, query.0);
                let i = choose|i: int| 0 <= i < run.cells@.len() && (#[trigger] run.cells@[i]).key as int == query.0 && run.cells@[i] == cell;
                let row = cell_row(cell);
                assert(run_rows(run)[i] == row);
                assert(run_rows(run).contains(row));
                assert(tops.map_values(f)[position] == run_rows(run));
                lemma_flat_contains(tops.map_values(f), row);
                assert(self.entries().contains(row));
                let p = choose|p: int| 0 <= p < self.entries().len() && self.entries()[p] == row;
                assert(self.entries()[p].0 == query);
            }
            if exists|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == query {
                let position = choose|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == query;
                assert(self.at(self.entries()[position].0) == Some(self.entries()[position].1));
            }
        }
    }
}

pub proof fn lemma_find_iff(cells: Seq<Cell>, key: int)
    requires cells_sorted(cells),
    ensures (find_cell(cells, key) is Some) == (exists|i: int| 0 <= i < cells.len() && (#[trigger] cells[i]).key as int == key),
        forall|i: int| 0 <= i < cells.len() && (#[trigger] cells[i]).key as int == key ==> find_cell(cells, key) == Some(cells[i]),
{
    if find_cell(cells, key) is Some { lemma_find_some(cells, key); }
    else { lemma_find_none(cells, key); }
    assert forall|i: int| 0 <= i < cells.len() && (#[trigger] cells[i]).key as int == key implies find_cell(cells, key) == Some(cells[i]) by {
        lemma_find_cell(cells, i);
    }
}

pub fn copy_into(target: &mut Vec<Cell>, cells: &Vec<Cell>, start: usize, end: usize)
    requires start <= end <= cells.len(),
    ensures final(target)@ == old(target)@ + cells@.subrange(start as int, end as int),
{
    let ghost original = target@;
    let mut i = start;
    while i < end
        invariant start <= i <= end <= cells.len(), target@ == original + cells@.subrange(start as int, i as int),
        decreases end - i,
    {
        target.push(cells[i].dup());
        proof { assert(cells@.subrange(start as int, i as int + 1) =~= cells@.subrange(start as int, i as int).push(cells@[i as int])); }
        i += 1;
    }
}

// First position whose key is at least `key`.
pub fn lower(cells: &Vec<Cell>, key: i64) -> (position: usize)
    requires cells_sorted(cells@),
    ensures position <= cells.len(),
        forall|j: int| 0 <= j < position ==> (#[trigger] cells@[j]).key < key,
        forall|j: int| position <= j < cells.len() ==> (#[trigger] cells@[j]).key >= key,
{
    let mut low: usize = 0;
    let mut high: usize = cells.len();
    while low < high
        invariant low <= high <= cells.len(), cells_sorted(cells@),
            forall|j: int| 0 <= j < low ==> (#[trigger] cells@[j]).key < key,
            forall|j: int| high <= j < cells.len() ==> (#[trigger] cells@[j]).key >= key,
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if cells[middle].key < key {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    low
}

pub fn run_get(cells: &Vec<Cell>, key: i64) -> (result: Option<Cell>)
    requires cells_sorted(cells@),
    ensures result == find_cell(cells@, key as int),
{
    let position = lower(cells, key);
    if position < cells.len() && cells[position].key == key {
        proof { lemma_find_cell(cells@, position as int); }
        Some(cells[position].dup())
    } else {
        proof {
            lemma_find_iff(cells@, key as int);
            assert(forall|i: int| 0 <= i < cells@.len() ==> (#[trigger] cells@[i]).key as int != key as int);
        }
        None
    }
}

#[verifier::rlimit(200)]
pub fn run_put(cells: &Vec<Cell>, cell: Cell) -> (result: Vec<Cell>)
    requires cells_sorted(cells@),
    ensures cells_sorted(result@), result@.len() > 0,
        forall|i: int| 0 <= i < result@.len() ==> result@[i] == cell || cells@.contains(result@[i]),
        forall|q: int| find_cell(result@, q) == if q == cell.key as int { Some(cell) } else { find_cell(cells@, q) },
{
    let p = lower(cells, cell.key);
    let replaced = p < cells.len() && cells[p].key == cell.key;
    let q = if replaced { p + 1 } else { p };
    let mut result = Vec::new();
    copy_into(&mut result, cells, 0, p);
    result.push(cell);
    copy_into(&mut result, cells, q, cells.len());
    proof {
        let s = result@;
        let n = cells@.len() as int;
        let (p, q) = (p as int, q as int);
        assert(s == cells@.subrange(0, p).push(cell) + cells@.subrange(q, n));
        assert(s.len() == p + 1 + (n - q));
        assert forall|i: int| 0 <= i < s.len() implies {
            &&& (i < p ==> s[i] == cells@[i])
            &&& (i == p ==> s[i] == cell)
            &&& (i > p ==> s[i] == cells@[i - p - 1 + q])
        } by {}
        assert(cells_sorted(s)) by {
            assert forall|i: int, j: int| 0 <= i < j < s.len() implies (#[trigger] s[i]).key < (#[trigger] s[j]).key by {
                if i < p { if j < p {} else if j == p { } else { assert(cells@[i].key < cells@[j - p - 1 + q].key); } }
                else if i == p { if replaced { assert(cells@[p].key == cell.key); assert(cells@[p].key < cells@[j - p - 1 + q].key); } else { assert(cell.key <= cells@[q].key); assert(cells@[q].key <= cells@[j - p - 1 + q].key); } }
                else { assert(cells@[i - p - 1 + q].key < cells@[j - p - 1 + q].key); }
            }
        }
        assert forall|i: int| 0 <= i < s.len() implies s[i] == cell || cells@.contains(s[i]) by {
            if i < p { assert(cells@[i] == s[i]); }
            else if i > p { assert(cells@[i - p - 1 + q] == s[i]); }
        }
        assert forall|k: int| find_cell(s, k) == if k == cell.key as int { Some(cell) } else { find_cell(cells@, k) } by {
            lemma_find_iff(s, k);
            lemma_find_iff(cells@, k);
            if k == cell.key as int {
                assert(s[p] == cell);
            } else if find_cell(cells@, k) is Some {
                let i = choose|i: int| 0 <= i < cells@.len() && (#[trigger] cells@[i]).key as int == k;
                assert(i < p || i >= q) by { if replaced && i == p { assert(cells@[p].key == cell.key); } }
                if i < p { assert(s[i] == cells@[i]); } else { assert(s[i - q + p + 1] == cells@[i]); }
            } else {
                assert forall|i: int| 0 <= i < s.len() implies (#[trigger] s[i]).key as int != k by {
                    if i < p { assert(s[i] == cells@[i]); } else if i > p { assert(s[i] == cells@[i - p - 1 + q]); }
                }
            }
        }
    }
    result
}

#[verifier::rlimit(200)]
pub fn run_drop(cells: &Vec<Cell>, key: i64) -> (result: Vec<Cell>)
    requires cells_sorted(cells@), exists|i: int| 0 <= i < cells@.len() && (#[trigger] cells@[i]).key == key,
    ensures cells_sorted(result@), result@.len() + 1 == cells@.len(),
        forall|i: int| 0 <= i < result@.len() ==> cells@.contains(result@[i]),
        forall|q: int| find_cell(result@, q) == if q == key as int { None } else { find_cell(cells@, q) },
{
    let p = lower(cells, key);
    proof {
        let i = choose|i: int| 0 <= i < cells@.len() && (#[trigger] cells@[i]).key == key;
        if p as int > i { assert(cells@[i].key < key); }
        if (p as int) < i { assert(cells@[p as int].key < cells@[i].key); }
    }
    let mut result = Vec::new();
    copy_into(&mut result, cells, 0, p);
    copy_into(&mut result, cells, p + 1, cells.len());
    proof {
        let s = result@;
        let n = cells@.len() as int;
        let p = p as int;
        assert(cells@[p].key == key);
        assert(s == cells@.subrange(0, p) + cells@.subrange(p + 1, n));
        assert forall|i: int| 0 <= i < s.len() implies ((i < p ==> s[i] == cells@[i]) && (i >= p ==> s[i] == cells@[i + 1])) by {}
        assert(cells_sorted(s)) by {
            assert forall|i: int, j: int| 0 <= i < j < s.len() implies (#[trigger] s[i]).key < (#[trigger] s[j]).key by {
                let ci = if i < p { i } else { i + 1 };
                let cj = if j < p { j } else { j + 1 };
                assert(s[i] == cells@[ci]);
                assert(s[j] == cells@[cj]);
            }
        }
        assert forall|i: int| 0 <= i < s.len() implies cells@.contains(s[i]) by {
            if i < p { assert(cells@[i] == s[i]); } else { assert(cells@[i + 1] == s[i]); }
        }
        assert forall|k: int| find_cell(s, k) == if k == key as int { None } else { find_cell(cells@, k) } by {
            lemma_find_iff(s, k);
            lemma_find_iff(cells@, k);
            if k == key as int {
                assert forall|i: int| 0 <= i < s.len() implies (#[trigger] s[i]).key as int != k by {
                    if i < p { assert(s[i] == cells@[i]); assert(cells@[i].key < cells@[p].key); }
                    else { assert(s[i] == cells@[i + 1]); assert(cells@[p].key < cells@[i + 1].key); }
                }
            } else if find_cell(cells@, k) is Some {
                let i = choose|i: int| 0 <= i < cells@.len() && (#[trigger] cells@[i]).key as int == k;
                assert(i != p);
                if i < p { assert(s[i] == cells@[i]); } else { assert(s[i - 1] == cells@[i]); }
            } else {
                assert forall|i: int| 0 <= i < s.len() implies (#[trigger] s[i]).key as int != k by {
                    if i < p { assert(s[i] == cells@[i]); } else { assert(s[i] == cells@[i + 1]); }
                }
            }
        }
    }
    result
}

impl Packed {
    #[verifier::type_invariant]
    spec fn valid(self) -> bool { self.wf() }

    pub proof fn map_model(&self)
        requires self.wf(),
        ensures
            ordered_rows(self.entries()),
            self.view().dom().finite(),
            forall|key: KeyView| #![trigger self.at(key)] self.at(key) ==
                if self.view().dom().contains(key) { Some(self.view()[key]) } else { None },
            forall|key: KeyView| #![trigger self.view().dom().contains(key)]
                self.view().dom().contains(key) <==> self.at(key).is_some(),
            forall|row: (KeyView, Moment)| #![trigger self.entries().contains(row)]
                self.entries().contains(row) <==> self.view().dom().contains(row.0) && self.view()[row.0] == row.1,
    {
        self.model();
        let keys = self.entries().map_values(|row: (KeyView, Moment)| row.0);
        keys.to_set_ensures();
        assert forall|key: KeyView| #![trigger self.view().dom().contains(key)]
            self.view().dom().contains(key) <==> self.at(key).is_some() by {
            if keys.contains(key) {
                let position = choose|position: int| 0 <= position < keys.len() && keys[position] == key;
                assert(self.entries()[position].0 == key);
            }
            if self.at(key).is_some() {
                let position = choose|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == key;
                assert(keys[position] == key);
            }
        }
        assert forall|row: (KeyView, Moment)| #![trigger self.entries().contains(row)]
            self.entries().contains(row) <==> self.view().dom().contains(row.0) && self.view()[row.0] == row.1 by {
            if self.entries().contains(row) {
                let position = choose|position: int| 0 <= position < self.entries().len() && self.entries()[position] == row;
            }
            if self.view().dom().contains(row.0) && self.view()[row.0] == row.1 {
                let position = choose|position: int| #![auto] 0 <= position < self.entries().len() && self.entries()[position].0 == row.0;
                assert(self.entries()[position] == row);
            }
        }
    }

    pub fn new(shift: u32) -> (result: Self)
        requires shift < 64,
        ensures result.wf(), result.entries() == Seq::<(KeyView, Moment)>::empty(), result.shift() == shift,
            result.view() == Map::<KeyView, Moment>::empty(),
            forall|query: KeyView| #![trigger result.at(query)] result.at(query).is_none(),
    {
        let top = Index::new();
        let result = Packed { top, shift };
        proof {
            assert(result.top.entries() =~= Seq::<(KeyView, Run)>::empty());
            assert(result.entries() =~= Seq::<(KeyView, Moment)>::empty());
            assert(result.view() =~= Map::<KeyView, Moment>::empty());
        }
        result
    }

    pub fn share(&self) -> (result: Self)
        ensures self.wf(), result.wf(), result.entries() == self.entries(), result.shift() == self.shift(),
            result.tops() == self.tops(),
            forall|query: KeyView| #![trigger result.at(query)] result.at(query) == self.at(query),
            result.view() == self.view(),
    {
        proof { use_type_invariant(self); }
        let result = Packed { top: self.top.share(), shift: self.shift };
        proof {
            assert(forall|query: KeyView| #![trigger result.at(query)] result.at(query) == self.at(query));
            assert(result.view() =~= self.view());
        }
        result
    }

    pub fn is_empty(&self) -> (result: bool)
        ensures result == (self.entries().len() == 0),
    {
        proof { use_type_invariant(self); }
        let result = self.top.is_empty();
        proof {
            if self.top.entries().len() == 0 {
                assert(self.top.entries() =~= Seq::<(KeyView, Run)>::empty());
            } else {
                let tops = self.top.entries();
                let f = |e: (KeyView, Run)| run_rows(e.1);
                assert(tops[0].1.cells@.len() > 0);
                lemma_flat_nonempty(tops);
            }
        }
        result
    }

    pub fn get(&self, key: i64) -> (result: Option<Mom>)
        ensures self.wf(), match result {
            Some(moment) => self.at((key as int, Seq::<u8>::empty())) == Some(moment@),
            None => self.at((key as int, Seq::<u8>::empty())) is None,
        },
    {
        proof { use_type_invariant(self); }
        let bucket = key >> self.shift;
        match self.top.get(&Key::number(bucket)) {
            None => None,
            Some(run) => {
                proof { assert(self.top.entries().len() > 0 || true); }
                // the run is the one the index holds under this bucket
                proof {
                    self.top.model();
                    let b = (bucket as int, Seq::<u8>::empty());
                    let position = choose|position: int| #![auto] 0 <= position < self.top.entries().len() && self.top.entries()[position].0 == b;
                    assert(self.top.entries()[position].1 == *run);
                }
                match run_get(&run.cells, key) {
                    Some(cell) => Some(Mom { count: cell.count.dup(), value: cell.value.dup() }),
                    None => None,
                }
            },
        }
    }
}

pub proof fn lemma_flat_rows_push(tops: Seq<(KeyView, Run)>, n: int)
    requires 0 <= n < tops.len(),
    ensures flat_rows(tops.take(n + 1)) == flat_rows(tops.take(n)) + run_rows(tops[n].1),
{
    let f = |e: (KeyView, Run)| run_rows(e.1);
    let prefix = tops.take(n + 1);
    assert(prefix.map_values(f).drop_last() =~= tops.take(n).map_values(f));
    assert(prefix.last() == tops[n]);
    assert(prefix.map_values(f).last() == run_rows(tops[n].1));
    assert(prefix.map_values(f).len() > 0);
}

pub proof fn lemma_flat_nonempty(tops: Seq<(KeyView, Run)>)
    requires tops.len() > 0, forall|i: int| 0 <= i < tops.len() ==> (#[trigger] tops[i]).1.cells@.len() > 0,
    ensures flat_rows(tops).len() > 0,
    decreases tops.len(),
{
    let f = |e: (KeyView, Run)| run_rows(e.1);
    assert(tops.map_values(f).drop_last() =~= tops.drop_last().map_values(f));
    assert(flat_rows(tops) == flat_rows(tops.drop_last()) + run_rows(tops.last().1));
}

// The run under a bucket of a tops_ok sequence is a valid run of that bucket.
pub proof fn lemma_run_of(top: Index<Run>, shift: u32, bucket: KeyView, run: &Run)
    requires top.wf(), tops_ok(top.entries(), shift), top.at(bucket) == Some(*run),
    ensures bucket.1 == Seq::<u8>::empty(), i64::MIN <= bucket.0 <= i64::MAX, run_ok(*run, bucket.0, shift),
{
    top.model();
    let position = choose|position: int| #![auto] 0 <= position < top.entries().len() && top.entries()[position].0 == bucket;
    assert(top.at(top.entries()[position].0) == Some(top.entries()[position].1));
    assert(top.entries()[position].1 == *run);
}

// A new index that differs from a tops_ok one only at one bucket (a valid run or none) is tops_ok.
pub proof fn lemma_tops_change(old_top: Index<Run>, new_top: Index<Run>, shift: u32, bucket: KeyView, replacement: Option<Run>)
    requires old_top.wf(), new_top.wf(), tops_ok(old_top.entries(), shift),
        bucket.1 == Seq::<u8>::empty(), i64::MIN <= bucket.0 <= i64::MAX,
        replacement is Some ==> run_ok(replacement.unwrap(), bucket.0, shift),
        forall|query: KeyView| #![trigger new_top.at(query)] new_top.at(query) == if query == bucket { replacement } else { old_top.at(query) },
    ensures tops_ok(new_top.entries(), shift),
{
    old_top.model();
    new_top.model();
    assert forall|i: int| 0 <= i < new_top.entries().len() implies {
        let e = #[trigger] new_top.entries()[i];
        &&& e.0.1 == Seq::<u8>::empty()
        &&& i64::MIN <= e.0.0 <= i64::MAX
        &&& run_ok(e.1, e.0.0, shift)
    } by {
        let e = new_top.entries()[i];
        assert(new_top.at(e.0) == Some(e.1));
        if e.0 == bucket {
            assert(replacement == Some(e.1));
        } else {
            assert(old_top.at(e.0) == Some(e.1));
            lemma_run_of(old_top, shift, e.0, &e.1);
        }
    }
}

pub proof fn lemma_singleton_run(cell: Cell, shift: u32)
    ensures cells_sorted(seq![cell]), find_cell(seq![cell], cell.key as int) == Some(cell),
        forall|q: int| q != cell.key as int ==> find_cell(seq![cell], q) is None,
{
    assert(seq![cell].drop_last() =~= Seq::<Cell>::empty());
    assert forall|q: int| q != cell.key as int implies find_cell(seq![cell], q) is None by {
        assert(seq![cell].last() == cell);
        reveal_with_fuel(find_cell, 2);
    }
}

impl Packed {
    pub fn put(&mut self, key: i64, moment: &Mom)
        ensures final(self).wf(), final(self).shift() == old(self).shift(),
            forall|query: KeyView| #![trigger final(self).at(query)] final(self).at(query)
                == if query == (key as int, Seq::<u8>::empty()) { Some(moment@) } else { old(self).at(query) },
    {
        proof { use_type_invariant(&*self); }
        let ghost old_top = self.top;
        let ghost shift = self.shift;
        let bucket = key >> self.shift;
        let bkey = Key::number(bucket);
        let cell = Cell { key, count: moment.count.dup(), value: moment.value.dup() };
        let run = match self.top.get(&bkey) {
            Some(existing) => {
                proof { lemma_run_of(old_top, shift, bkey.view(), &*existing); }
                let cells = run_put(&existing.cells, cell);
                proof {
                    assert forall|i: int| 0 <= i < cells@.len() implies bucket_of((#[trigger] cells@[i]).key, shift) as int == bucket as int by {
                        if cells@[i] != cell { assert(existing.cells@.contains(cells@[i])); 
                            let j = choose|j: int| 0 <= j < existing.cells@.len() && existing.cells@[j] == cells@[i];
                            assert(bucket_of(existing.cells@[j].key, shift) as int == bucket as int);
                        }
                    }
                }
                Run { cells }
            },
            None => {
                let mut cells = Vec::new();
                cells.push(cell);
                proof { lemma_singleton_run(cell, shift); assert(cells@ =~= seq![cell]); }
                Run { cells }
            },
        };
        let ghost run_view = run;
        proof {
            assert(run_ok(run_view, bucket as int, shift)) by {
                assert(bucket_of(cell.key, shift) == bucket);
            }
        }
        let mut top = self.top.share();
        top.put(bkey, Arc::new(run));
        proof { lemma_tops_change(old_top, top, shift, (bucket as int, Seq::<u8>::empty()), Some(run_view)); }
        self.top = top;
        proof {
            assert forall|query: KeyView| #![trigger self.at(query)] self.at(query)
                == if query == (key as int, Seq::<u8>::empty()) { Some(moment@) } else { Packed { top: old_top, shift }.at(query) } by {
                if query.1 == Seq::<u8>::empty() && i64::MIN <= query.0 <= i64::MAX {
                    let b = (bucket_of(query.0 as i64, shift) as int, Seq::<u8>::empty());
                    if query == (key as int, Seq::<u8>::empty()) {
                        assert(b == (bucket as int, Seq::<u8>::empty()));
                    } else if b == (bucket as int, Seq::<u8>::empty()) {
                        if old_top.at(b).is_none() { lemma_singleton_run(cell, shift); }
                    }
                }
            }
        }
    }

    pub fn remove(&mut self, key: i64)
        ensures final(self).wf(), final(self).shift() == old(self).shift(),
            forall|query: KeyView| #![trigger final(self).at(query)] final(self).at(query)
                == if query == (key as int, Seq::<u8>::empty()) { None } else { old(self).at(query) },
    {
        proof { use_type_invariant(&*self); }
        let ghost old_top = self.top;
        let ghost shift = self.shift;
        let ghost before = *self;
        let bucket = key >> self.shift;
        let bkey = Key::number(bucket);
        match self.top.get(&bkey) {
            None => { return; },
            Some(existing) => {
                proof { lemma_run_of(old_top, shift, bkey.view(), &*existing); }
                if run_get(&existing.cells, key).is_none() { return; }
                proof {
                    lemma_find_some(existing.cells@, key as int);
                }
                let cells = run_drop(&existing.cells, key);
                let ghost dropped = Run { cells };
                proof {
                    assert forall|i: int| 0 <= i < cells@.len() implies bucket_of((#[trigger] cells@[i]).key, shift) as int == bucket as int by {
                        assert(existing.cells@.contains(cells@[i]));
                        let j = choose|j: int| 0 <= j < existing.cells@.len() && existing.cells@[j] == cells@[i];
                        assert(bucket_of(existing.cells@[j].key, shift) as int == bucket as int);
                    }
                }
                if cells.len() == 0 {
                    let mut top = self.top.share();
                    top.remove(&bkey);
                    proof { lemma_tops_change(old_top, top, shift, bkey.view(), None); }
                    self.top = top;
                } else {
                    let mut top = self.top.share();
                    top.put(bkey, Arc::new(Run { cells }));
                    proof { lemma_tops_change(old_top, top, shift, bkey.view(), Some(dropped)); }
                    self.top = top;
                }
                proof {
                    assert forall|query: KeyView| #![trigger self.at(query)] self.at(query)
                        == if query == (key as int, Seq::<u8>::empty()) { None } else { before.at(query) } by {
                        if query.1 == Seq::<u8>::empty() && i64::MIN <= query.0 <= i64::MAX {
                            let b = (bucket_of(query.0 as i64, shift) as int, Seq::<u8>::empty());
                            if query == (key as int, Seq::<u8>::empty()) {
                                assert(b == bkey.view());
                            } else if b == bkey.view() {
                                if dropped.cells@.len() == 0 {
                                    assert(find_cell(dropped.cells@, query.0) is None);
                                    // the old run holds nothing but the removed key
                                    lemma_find_iff(existing.cells@, query.0);
                                    assert(find_cell(existing.cells@, query.0) == find_cell(dropped.cells@, query.0));
                                }
                            }
                        }
                    }
                }
            },
        }
    }
}

// A snapshot cursor: the rows are copied out once, 24 bytes each, for the duration of a scan.
pub struct PackedCursor {
    rows: Vec<Cell>,
    position: usize,
}

impl PackedCursor {
    pub closed spec fn remaining(self) -> Seq<(KeyView, Moment)> { cell_rows(self.rows@).skip(self.position as int) }
    pub closed spec fn work(self) -> int { self.remaining().len() as int }
    #[verifier::type_invariant]
    spec fn valid(self) -> bool { self.position <= self.rows@.len() }

    pub fn next(&mut self) -> (result: Option<(Key, Arc<Mom>)>)
        ensures
            final(self).work() >= 0,
            match result {
                Some(row) => {
                    &&& old(self).remaining().len() > 0
                    &&& (row.0.view(), (*row.1)@) == old(self).remaining()[0]
                    &&& final(self).remaining() == old(self).remaining().skip(1)
                    &&& final(self).work() < old(self).work()
                },
                None => old(self).remaining().len() == 0 && final(self).remaining().len() == 0,
            },
    {
        proof { use_type_invariant(&*self); }
        if self.position < self.rows.len() {
            let cell = self.rows[self.position].dup();
            self.position += 1;
            proof { assert(cell_rows(self.rows@).skip(self.position as int) =~= cell_rows(self.rows@).skip(self.position as int - 1).skip(1)); }
            Some((Key::number(cell.key), Arc::new(Mom { count: cell.count, value: cell.value })))
        } else {
            None
        }
    }
}

impl Packed {
    pub fn cursor(&self) -> (result: PackedCursor)
        ensures result.remaining() == self.entries(), result.work() >= 0,
    {
        proof { use_type_invariant(self); }
        let tops = self.top.rows();
        let ghost entries = self.top.entries();
        let ghost f = |e: (KeyView, Run)| run_rows(e.1);
        let mut cells: Vec<Cell> = Vec::new();
        let mut i: usize = 0;
        proof { assert(cell_rows(cells@) =~= Seq::<(KeyView, Moment)>::empty()); assert(entries.take(0) =~= Seq::<(KeyView, Run)>::empty()); }
        while i < tops.len()
            invariant 0 <= i <= tops.len(), tops@.map_values(|row: (Key, Arc<Run>)| (row.0.view(), *row.1)) == entries,
                cell_rows(cells@) == flat_rows(entries.take(i as int)),
            decreases tops.len() - i,
        {
            let run = &tops[i].1;
            let ghost before = cells@;
            copy_into(&mut cells, &run.cells, 0, run.cells.len());
            proof {
                assert(entries[i as int].1 == *tops@[i as int].1);
                assert(run.cells@.subrange(0, run.cells@.len() as int) =~= run.cells@);
                lemma_flat_rows_push(entries, i as int);
                assert(cell_rows(cells@) =~= cell_rows(before) + run_rows(entries[i as int].1)) by {
                    assert(cells@ == before + run.cells@);
                }
            }
            i += 1;
        }
        proof { assert(entries.take(tops@.len() as int) =~= entries); }
        let result = PackedCursor { rows: cells, position: 0 };
        proof { assert(result.remaining() =~= self.entries()); }
        result
    }
}

pub open spec fn contribution(p: Packed, key: KeyView, weight: spec_fn((KeyView, Moment)) -> int) -> int {
    match p.at(key) { Some(value) => weight((key, value)), None => 0 }
}

pub proof fn lemma_key_contribution(p: &Packed, key: KeyView, weight: spec_fn((KeyView, Moment)) -> int)
    requires p.wf(),
    ensures sum(p.entries(), |row: (KeyView, Moment)| if row.0 == key { weight(row) } else { 0 }) == contribution(*p, key, weight),
{
    p.model();
    lemma_ordered_unique(p.entries());
    let rows = p.entries();
    let selected = |row: (KeyView, Moment)| if row.0 == key { weight(row) } else { 0 };
    if p.at(key).is_some() {
        let position = choose|position: int| #![auto] 0 <= position < rows.len() && rows[position].0 == key;
        assert forall|other: int| #![trigger rows[other]]
            0 <= other < rows.len() && other != position implies selected(rows[other]) == 0 by {
            if other < position { assert(rows[other].0 != rows[position].0); }
            else { assert(rows[position].0 != rows[other].0); }
        }
        lemma_sum_single_match(rows, selected, position);
    } else {
        assert forall|position: int| #![trigger rows[position]] 0 <= position < rows.len() implies selected(rows[position]) == 0 by {};
        lemma_sum_zero(rows, selected);
    }
}

pub proof fn lemma_sum_unchanged_except(before: &Packed, after: &Packed, key: KeyView, weight: spec_fn((KeyView, Moment)) -> int)
    requires before.wf(), after.wf(),
        forall|query: KeyView| #![trigger before.at(query), after.at(query)] query != key ==> before.at(query) == after.at(query),
    ensures sum(after.entries(), weight) - contribution(*after, key, weight)
        == sum(before.entries(), weight) - contribution(*before, key, weight),
{
    before.map_model();
    after.map_model();
    let low = drop_key(before.entries(), key);
    let high = drop_key(after.entries(), key);
    lemma_drop_bindings(before.entries(), key);
    lemma_drop_bindings(after.entries(), key);
    assert forall|row: (KeyView, Moment)| #![trigger low.contains(row), high.contains(row)]
        low.contains(row) <==> high.contains(row) by {
        if row.0 != key {
            assert(before.at(row.0) == after.at(row.0));
            assert(before.entries().contains(row) <==> before.at(row.0) == Some(row.1));
            assert(after.entries().contains(row) <==> after.at(row.0) == Some(row.1));
        }
    }
    low.to_set_ensures();
    high.to_set_ensures();
    assert(low.to_set() =~= high.to_set());
    lemma_drop_ordered(before.entries(), key);
    lemma_drop_ordered(after.entries(), key);
    lemma_ordered_unique(low);
    lemma_ordered_unique(high);
    verus_prooflib::lemma_sum_same_support(low, high, weight);
    lemma_drop_sum(before.entries(), key, weight);
    lemma_drop_sum(after.entries(), key, weight);
    lemma_key_contribution(before, key, weight);
    lemma_key_contribution(after, key, weight);
}

// After put: the sum moves by the new binding minus the old one.
pub proof fn lemma_sum_put(before: &Packed, after: &Packed, key: KeyView, value: Moment, weight: spec_fn((KeyView, Moment)) -> int)
    requires before.wf(), after.wf(),
        forall|query: KeyView| #![trigger after.at(query)] after.at(query) == if query == key { Some(value) } else { before.at(query) },
    ensures sum(after.entries(), weight) == sum(before.entries(), weight) - contribution(*before, key, weight) + weight((key, value)),
{
    lemma_sum_unchanged_except(before, after, key, weight);
    assert(after.at(key) == Some(value));
}

pub proof fn lemma_sum_remove(before: &Packed, after: &Packed, key: KeyView, weight: spec_fn((KeyView, Moment)) -> int)
    requires before.wf(), after.wf(),
        forall|query: KeyView| #![trigger after.at(query)] after.at(query) == if query == key { None } else { before.at(query) },
    ensures sum(after.entries(), weight) == sum(before.entries(), weight) - contribution(*before, key, weight),
{
    lemma_sum_unchanged_except(before, after, key, weight);
    assert(after.at(key).is_none());
}

pub proof fn lemma_selected_weight(p: &Packed, key: KeyView, weight: spec_fn((KeyView, Moment)) -> int, factor: int, selected: spec_fn((KeyView, Moment)) -> int)
    requires p.wf(), forall|row: (KeyView, Moment)| #[trigger] selected(row) == if row.0 == key { weight(row) * factor } else { 0 },
    ensures sum(p.entries(), selected) == contribution(*p, key, weight) * factor,
{
    let scaled = |row: (KeyView, Moment)| weight(row) * factor;
    let filtered = |row: (KeyView, Moment)| if row.0 == key { scaled(row) } else { 0 };
    lemma_key_contribution(p, key, scaled);
    lemma_sum_ext_all(p.entries(), selected, filtered);
}

}
