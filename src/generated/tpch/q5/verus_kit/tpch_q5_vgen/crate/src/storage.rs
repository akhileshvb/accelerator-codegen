use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;
use verus_prooflib::lemma_sum_insert;

verus! {

pub open spec fn cell_key(sub: i64, day: i32) -> i128 {
    (sub as int * 4294967296 + day as int - i32::MIN as int) as i128
}

pub fn encode(sub: i64, day: i32) -> (out: i128)
    ensures out == cell_key(sub, day),
{ sub as i128 * 4294967296i128 + (day as i128 - i32::MIN as i128) }

pub proof fn lemma_cell_key_injective(left: Cell, right: Cell)
    requires cell_key(left.sub, left.day) == cell_key(right.sub, right.day),
    ensures left.sub == right.sub, left.day == right.day,
{
    let left_offset = left.day as int - i32::MIN as int;
    let right_offset = right.day as int - i32::MIN as int;
    verus_prooflib::lemma_numeric_key_parts(left.sub as int, left_offset, 4294967296);
    verus_prooflib::lemma_numeric_key_parts(right.sub as int, right_offset, 4294967296);
    assert(left.sub as int * 4294967296 + left_offset == right.sub as int * 4294967296 + right_offset);
}

#[derive(Clone, Copy)]
pub struct Cell {
    pub sub: i64,
    pub day: i32,
    pub count: i128,
    pub revenue: i128,
}

pub struct Group {
    pub rows: Vec<Cell>,
    pub positions: HashMap<i128, usize>,
}

impl Group {
    pub proof fn lemma_row(self, index: int)
        requires self.wf(), 0 <= index < self.rows@.len(),
        ensures self.at(self.rows@[index].sub, self.rows@[index].day) ==
            (self.rows@[index].count as int, self.rows@[index].revenue as int),
    {}
    pub proof fn lemma_at(self, sub: i64, day: i32)
        requires self.wf(),
        ensures self.at(sub, day).0 == sum(self.rows@, |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 }),
            self.at(sub, day).1 == sum(self.rows@, |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 }),
    {
        let count = |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 };
        let revenue = |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 };
        if self.positions@.contains_key(cell_key(sub, day)) {
            let position = self.positions@[cell_key(sub, day)] as int;
            lemma_cell_key_injective(self.rows@[position], Cell { sub, day, count: 0, revenue: 0 });
            assert forall|index: int| 0 <= index < self.rows@.len() && index != position implies
                (#[trigger] self.rows@[index]).sub != sub || self.rows@[index].day != day by {
                if self.rows@[index].sub == sub && self.rows@[index].day == day {
                    assert(self.positions@[cell_key(sub, day)] == index);
                }
            }
            lemma_sum_single_match(self.rows@, count, position);
            lemma_sum_single_match(self.rows@, revenue, position);
        } else {
            assert forall|index: int| 0 <= index < self.rows@.len() implies
                (#[trigger] self.rows@[index]).sub != sub || self.rows@[index].day != day by {}
            lemma_sum_zero(self.rows@, count);
            lemma_sum_zero(self.rows@, revenue);
        }
    }
    pub open spec fn at(self, sub: i64, day: i32) -> (int, int) {
        if self.positions@.contains_key(cell_key(sub, day)) {
            let cell = self.rows@[self.positions@[cell_key(sub, day)] as int];
            (cell.count as int, cell.revenue as int)
        } else { (0, 0) }
    }

    pub open spec fn wf(self) -> bool {
        &&& forall|code: i128| self.positions@.contains_key(code) ==>
            #[trigger] self.positions@[code] < self.rows@.len()
            && cell_key(self.rows@[self.positions@[code] as int].sub, self.rows@[self.positions@[code] as int].day) == code
        &&& forall|index: int| 0 <= index < self.rows@.len() ==>
            (#[trigger] self.rows@[index]).count != 0 || self.rows@[index].revenue != 0
        &&& forall|index: int| 0 <= index < self.rows@.len() ==>
            self.positions@.contains_key(cell_key((#[trigger] self.rows@[index]).sub, self.rows@[index].day))
            && self.positions@[cell_key(self.rows@[index].sub, self.rows@[index].day)] == index
    }

    pub fn new() -> (out: Self)
        ensures out.wf(), out.rows@.len() == 0, out.positions@ == Map::empty(),
            forall|sub: i64, day: i32| #[trigger] out.at(sub, day) == (0, 0),
    { Self { rows: Vec::new(), positions: HashMap::new() } }

    pub fn copy(&self) -> (out: Self)
        requires self.wf(),
        ensures out.wf(), out.rows@ == self.rows@, out.positions@ == self.positions@,
    {
        Self { rows: self.rows.clone(), positions: self.positions.clone() }
    }

    pub fn get(&self, sub: i64, day: i32) -> (out: Cell)
        requires self.wf(),
        ensures out.sub == sub, out.day == day,
            (out.count as int, out.revenue as int) == self.at(sub, day),
    {
        match self.positions.get(&(sub as i128 * 4294967296i128 + (day as i128 - i32::MIN as i128))) {
            Some(&position) => {
                proof {
                    assert(self.positions@.contains_key(cell_key(sub, day)));
                    assert(position == self.positions@[cell_key(sub, day)]);
                    assert(position < self.rows@.len());
                    lemma_cell_key_injective(self.rows@[position as int], Cell { sub, day, count: 0, revenue: 0 });
                }
                self.rows[position]
            },
            None => Cell { sub, day, count: 0, revenue: 0 },
        }
    }

    pub fn put(&mut self, cell: Cell)
        requires old(self).wf(),
        ensures final(self).wf(), final(self).at(cell.sub, cell.day) == (cell.count as int, cell.revenue as int),
            forall|sub: i64, day: i32| (sub, day) != (cell.sub, cell.day) ==>
                #[trigger] final(self).at(sub, day) == old(self).at(sub, day),
    {
        let ghost entry = *self;
        match self.positions.get(&encode(cell.sub, cell.day)) {
            Some(position) => {
                let position = *position;
                proof { assert(position < self.rows@.len()); }
                if cell.count == 0 && cell.revenue == 0 {
                    self.positions.remove(&encode(cell.sub, cell.day));
                    let last = self.rows.pop().unwrap();
                    if position < self.rows.len() {
                        self.rows.set(position, last);
                        self.positions.insert(encode(last.sub, last.day), position);
                    }
                } else { self.rows.set(position, cell); }
            },
            None => {
                if cell.count != 0 || cell.revenue != 0 {
                    self.positions.insert(encode(cell.sub, cell.day), self.rows.len());
                    self.rows.push(cell);
                }
            },
        }
        proof {
            assert forall|other: i64, day: i32| (other, day) != (cell.sub, cell.day) implies
                #[trigger] self.at(other, day) == entry.at(other, day) by {
                if cell_key(other, day) == cell_key(cell.sub, cell.day) {
                    lemma_cell_key_injective(Cell { sub: other, day, count: 0, revenue: 0 }, cell);
                }
            }
        }
    }

    pub fn add(&mut self, sub: i64, day: i32, count: i128, revenue: i128) -> (ok: bool)
        requires old(self).wf(),
        ensures final(self).wf(), ok ==> final(self).at(sub, day) ==
            (old(self).at(sub, day).0 + count, old(self).at(sub, day).1 + revenue),
            forall|other: i64, date: i32| (other, date) != (sub, day) ==>
                #[trigger] final(self).at(other, date) == old(self).at(other, date),
    {
        let before = self.get(sub, day);
        let count = match before.count.checked_add(count) { Some(value) => value, None => return false };
        let revenue = match before.revenue.checked_add(revenue) { Some(value) => value, None => return false };
        self.put(Cell { sub, day, count, revenue });
        true
    }
}

pub struct Bucket {
    pub keys: Vec<i64>,
    pub groups: HashMap<i64, Group>,
}

impl Bucket {
    pub open spec fn at(self, key: i64, sub: i64, day: i32) -> (int, int) {
        if self.groups@.contains_key(key) { self.groups@[key].at(sub, day) } else { (0, 0) }
    }

    pub open spec fn wf(self) -> bool {
        &&& forall|key: i64| #[trigger] self.groups@.contains_key(key) <==> self.keys@.contains(key)
        &&& forall|key: i64| self.groups@.contains_key(key) ==> (#[trigger] self.groups@[key]).wf()
        &&& self.keys@.no_duplicates()
    }

    pub fn new() -> (out: Self) ensures out.wf(), out.keys@.len() == 0, out.groups@ == Map::empty(),
        forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == (0, 0),
    { Self { keys: Vec::new(), groups: HashMap::new() } }

    pub fn replace(&mut self, key: i64, group: Group)
        requires old(self).wf(), group.wf(),
        ensures final(self).wf(), final(self).groups@ == old(self).groups@.insert(key, group),
            final(self).keys@ == if old(self).groups@.contains_key(key) { old(self).keys@ } else { old(self).keys@.push(key) },
            forall|other: i64, sub: i64, day: i32| #[trigger] final(self).at(other, sub, day) ==
                if other == key { group.at(sub, day) } else { old(self).at(other, sub, day) },
    {
        if !self.groups.contains_key(&key) {
            proof {
                assert(!self.keys@.contains(key));
                verus_prooflib::lemma_unique_push(self.keys@, key);
                verus_prooflib::lemma_push_membership(self.keys@, key);
            }
            self.keys.push(key);
        }
        self.groups.insert(key, group);
    }

    pub fn copy(&self) -> (out: Self)
        requires self.wf(),
        ensures out.wf(), out.keys@ == self.keys@,
            forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == self.at(key, sub, day),
    {
        let mut out = Self::new();
        let mut index = 0;
        while index < self.keys.len()
            invariant self.wf(), out.wf(), index <= self.keys.len(), out.keys@ == self.keys@.take(index as int),
                forall|key: i64| #[trigger] out.groups@.contains_key(key) <==> self.keys@.take(index as int).contains(key),
                forall|key: i64| out.groups@.contains_key(key) ==>
                    (#[trigger] out.groups@[key]).rows@ == self.groups@[key].rows@
                    && out.groups@[key].positions@ == self.groups@[key].positions@,
            decreases self.keys.len() - index,
        {
            let key = self.keys[index];
            proof {
                assert(self.keys@.no_duplicates());
                verus_prooflib::lemma_prefix_fresh(self.keys@, index as int);
                assert(self.groups@.contains_key(key));
            }
            let group = self.groups.get(&key).unwrap().copy();
            proof {
                verus_prooflib::lemma_push_membership(out.keys@, key);
                assert(self.keys@.take(index as int + 1) =~= out.keys@.push(key));
            }
            out.replace(key, group);
            index += 1;
        }
        proof {
            assert(self.keys@.take(self.keys@.len() as int) =~= self.keys@);
            assert forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == self.at(key, sub, day) by {
                if !out.groups@.contains_key(key) { assert(!self.groups@.contains_key(key)); }
            }
        }
        out
    }

    pub fn add(&mut self, key: i64, sub: i64, day: i32, count: i128, revenue: i128) -> (ok: bool)
        requires old(self).wf(),
        ensures final(self).wf(), ok ==> final(self).at(key, sub, day) ==
            (old(self).at(key, sub, day).0 + count, old(self).at(key, sub, day).1 + revenue),
            forall|other: i64, variant: i64, date: i32| (other, variant, date) != (key, sub, day) ==>
                #[trigger] final(self).at(other, variant, date) == old(self).at(other, variant, date),
    {
        let ghost entry = *self;
        let mut group = match self.groups.remove(&key) {
            Some(group) => group,
            None => {
                proof {
                    assert(!self.keys@.contains(key));
                    verus_prooflib::lemma_unique_push(self.keys@, key);
                    verus_prooflib::lemma_push_membership(self.keys@, key);
                }
                self.keys.push(key);
                Group::new()
            },
        };
        let ok = group.add(sub, day, count, revenue);
        self.groups.insert(key, group);
        proof {
            assert(self.groups@.dom() =~= if entry.groups@.contains_key(key) { entry.groups@.dom() } else { entry.groups@.dom().insert(key) });
        }
        ok
    }
}

// ---------------------------------------------------------------------------
// Compact persistent tables.
//
// A table is a three-level tree of `Arc`s (128 x 128 x 64 slots) over 2^20
// chunks.  A key lives in chunk `partition(key)` (a multiplicative hash, so
// TPC-H's sparse order keys spread evenly).  A chunk is one flat array of
// 32-byte entries (key, sub, day, count, revenue), sorted by key, one entry per
// (key, sub, day) slot with a non-zero value; a key's cells are therefore a
// contiguous run.  There is no per-key allocation, no hash map and no per-entry
// Arc: a version of the table shares every chunk it did not touch, and a
// change copies one chunk and the 3 slot arrays on its path.
//
// Stored counts are i32 and revenues i64 (the in-flight `Cell` stays i128); a
// value that does not fit makes the change fail like any other overflow, which
// every caller already treats as "cannot maintain" (build returns None, apply
// returns an error and the engine falls back to the base query).
// ---------------------------------------------------------------------------

pub open spec fn partition(key: i64) -> int {
    (((key as u64).wrapping_mul(0x9E3779B97F4A7C15u64)) >> 44u64) as int
}

pub proof fn lemma_partition_bound(key: i64)
    ensures 0 <= partition(key) < 1048576,
{
    let x = (key as u64).wrapping_mul(0x9E3779B97F4A7C15u64);
    assert(x >> 44u64 < 1048576u64) by (bit_vector);
}

pub fn part(key: i64) -> (out: usize)
    ensures out == partition(key), out < 1048576,
{
    let x = (key as u64).wrapping_mul(0x9E3779B97F4A7C15u64) >> 44u64;
    proof { lemma_partition_bound(key); }
    x as usize
}

#[derive(Clone, Copy)]
pub struct Entry {
    pub key: i64,
    pub sub: i64,
    pub revenue: i64,
    pub day: i32,
    pub count: i32,
}

impl Entry {
    pub open spec fn cell(self) -> Cell {
        Cell { sub: self.sub, day: self.day, count: self.count as i128, revenue: self.revenue as i128 }
    }
    pub open spec fn live(self) -> bool { self.count != 0 || self.revenue != 0 }
    pub open spec fn same_slot(self, other: Entry) -> bool {
        self.key == other.key && self.sub == other.sub && self.day == other.day
    }
}

pub open spec fn entry_cell(e: Entry) -> Cell { e.cell() }

pub open spec fn sorted_by_key(s: Seq<Entry>) -> bool {
    forall|i: int, j: int| 0 <= i < j < s.len() ==> (#[trigger] s[i]).key <= (#[trigger] s[j]).key
}

pub open spec fn live_all(s: Seq<Entry>) -> bool {
    forall|i: int| 0 <= i < s.len() ==> (#[trigger] s[i]).live()
}

pub open spec fn distinct_slots(s: Seq<Entry>) -> bool {
    forall|i: int, j: int| 0 <= i < j < s.len() ==> !(#[trigger] s[i]).same_slot(#[trigger] s[j])
}

pub open spec fn entries_ok(s: Seq<Entry>) -> bool {
    sorted_by_key(s) && live_all(s) && distinct_slots(s)
}

pub open spec fn homed(s: Seq<Entry>, idx: int) -> bool {
    forall|i: int| 0 <= i < s.len() ==> partition((#[trigger] s[i]).key) == idx
}

pub open spec fn slot_count(e: Entry, key: i64, sub: i64, day: i32) -> int {
    if e.key == key && e.sub == sub && e.day == day { e.count as int } else { 0 }
}

pub open spec fn slot_revenue(e: Entry, key: i64, sub: i64, day: i32) -> int {
    if e.key == key && e.sub == sub && e.day == day { e.revenue as int } else { 0 }
}

pub open spec fn entries_at(s: Seq<Entry>, key: i64, sub: i64, day: i32) -> (int, int) {
    (sum(s, |e: Entry| slot_count(e, key, sub, day)), sum(s, |e: Entry| slot_revenue(e, key, sub, day)))
}

/// The cells of `key`, in entry order.
pub open spec fn rows_of(s: Seq<Entry>, key: i64) -> Seq<Cell>
    decreases s.len(),
{
    if s.len() == 0 { Seq::empty() }
    else {
        let before = rows_of(s.drop_last(), key);
        if s.last().key == key { before.push(s.last().cell()) } else { before }
    }
}

pub open spec fn rows_ok(rows: Seq<Cell>) -> bool {
    &&& forall|i: int| 0 <= i < rows.len() ==> (#[trigger] rows[i]).count != 0 || rows[i].revenue != 0
    &&& forall|i: int, j: int| 0 <= i < j < rows.len() ==>
        !((#[trigger] rows[i]).sub == (#[trigger] rows[j]).sub && rows[i].day == rows[j].day)
}

pub open spec fn rows_count(rows: Seq<Cell>, sub: i64, day: i32) -> int {
    sum(rows, |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 })
}

pub open spec fn rows_revenue(rows: Seq<Cell>, sub: i64, day: i32) -> int {
    sum(rows, |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 })
}

pub open spec fn rows_at(rows: Seq<Cell>, sub: i64, day: i32) -> (int, int) {
    (rows_count(rows, sub, day), rows_revenue(rows, sub, day))
}

pub proof fn lemma_rows_of_concat(a: Seq<Entry>, b: Seq<Entry>, key: i64)
    ensures rows_of(a + b, key) == rows_of(a, key) + rows_of(b, key),
    decreases b.len(),
{
    if b.len() == 0 {
        assert(a + b =~= a);
        assert(rows_of(b, key) =~= Seq::<Cell>::empty());
        assert(rows_of(a, key) + rows_of(b, key) =~= rows_of(a, key));
    } else {
        let c = a + b;
        assert(c.drop_last() =~= a + b.drop_last());
        assert(c.last() == b.last());
        lemma_rows_of_concat(a, b.drop_last(), key);
        if b.last().key == key {
            assert(rows_of(a, key) + rows_of(b, key) =~=
                (rows_of(a, key) + rows_of(b.drop_last(), key)).push(b.last().cell()));
        } else {
            assert(rows_of(a, key) + rows_of(b, key) =~= rows_of(a, key) + rows_of(b.drop_last(), key));
        }
    }
}

pub proof fn lemma_rows_of_none(s: Seq<Entry>, key: i64)
    requires forall|i: int| 0 <= i < s.len() ==> (#[trigger] s[i]).key != key,
    ensures rows_of(s, key).len() == 0,
    decreases s.len(),
{
    if s.len() > 0 {
        assert forall|i: int| 0 <= i < s.drop_last().len() implies (#[trigger] s.drop_last()[i]).key != key by {
            assert(s.drop_last()[i] == s[i]);
        }
        lemma_rows_of_none(s.drop_last(), key);
        assert(s.last() == s[s.len() - 1]);
    }
}

pub proof fn lemma_rows_of_all(s: Seq<Entry>, key: i64)
    requires forall|i: int| 0 <= i < s.len() ==> (#[trigger] s[i]).key == key,
    ensures rows_of(s, key) == s.map_values(|e: Entry| entry_cell(e)),
    decreases s.len(),
{
    if s.len() == 0 {
        assert(s.map_values(|e: Entry| entry_cell(e)) =~= Seq::<Cell>::empty());
    } else {
        assert forall|i: int| 0 <= i < s.drop_last().len() implies (#[trigger] s.drop_last()[i]).key == key by {
            assert(s.drop_last()[i] == s[i]);
        }
        lemma_rows_of_all(s.drop_last(), key);
        assert(s.last() == s[s.len() - 1]);
        assert(s.map_values(|e: Entry| entry_cell(e)) =~=
            s.drop_last().map_values(|e: Entry| entry_cell(e)).push(entry_cell(s.last())));
    }
}

/// A run: entries lo..hi carry `key`, everything before and after does not.
pub proof fn lemma_rows_char(s: Seq<Entry>, key: i64, lo: int, hi: int)
    requires 0 <= lo <= hi <= s.len(),
        forall|m: int| 0 <= m < lo ==> (#[trigger] s[m]).key != key,
        forall|m: int| lo <= m < hi ==> (#[trigger] s[m]).key == key,
        forall|m: int| hi <= m < s.len() ==> (#[trigger] s[m]).key != key,
    ensures rows_of(s, key) == s.subrange(lo, hi).map_values(|e: Entry| entry_cell(e)),
{
    let a = s.subrange(0, lo);
    let b = s.subrange(lo, hi);
    let c = s.subrange(hi, s.len() as int);
    assert(s =~= a + b + c);
    lemma_rows_of_concat(a + b, c, key);
    lemma_rows_of_concat(a, b, key);
    assert forall|i: int| 0 <= i < a.len() implies (#[trigger] a[i]).key != key by { assert(a[i] == s[i]); }
    assert forall|i: int| 0 <= i < b.len() implies (#[trigger] b[i]).key == key by { assert(b[i] == s[lo + i]); }
    assert forall|i: int| 0 <= i < c.len() implies (#[trigger] c[i]).key != key by { assert(c[i] == s[hi + i]); }
    lemma_rows_of_none(a, key);
    lemma_rows_of_none(c, key);
    lemma_rows_of_all(b, key);
    assert(rows_of(a, key) =~= Seq::<Cell>::empty());
    assert(rows_of(c, key) =~= Seq::<Cell>::empty());
    assert(rows_of(s, key) =~= b.map_values(|e: Entry| entry_cell(e)));
}

/// Every cell of `key` comes from an entry of `key`.
pub proof fn lemma_rows_of_elem(s: Seq<Entry>, key: i64, i: int)
    requires 0 <= i < rows_of(s, key).len(),
    ensures exists|m: int| 0 <= m < s.len() && (#[trigger] s[m]).key == key && s[m].cell() == rows_of(s, key)[i],
    decreases s.len(),
{
    let before = rows_of(s.drop_last(), key);
    if s.last().key == key && i == before.len() as int {
        let m = s.len() - 1;
        assert(s[m] == s.last());
        assert(rows_of(s, key)[i] == s.last().cell());
    } else {
        assert(i < before.len());
        assert(rows_of(s, key)[i] == before[i]);
        lemma_rows_of_elem(s.drop_last(), key, i);
        let m = choose|m: int| 0 <= m < s.drop_last().len() && (#[trigger] s.drop_last()[m]).key == key && s.drop_last()[m].cell() == before[i];
        assert(s[m] == s.drop_last()[m]);
    }
}

pub proof fn lemma_rows_of_ok(s: Seq<Entry>, key: i64)
    requires live_all(s), distinct_slots(s),
    ensures rows_ok(rows_of(s, key)),
    decreases s.len(),
{
    if s.len() > 0 {
        let before = rows_of(s.drop_last(), key);
        assert forall|i: int| 0 <= i < s.drop_last().len() implies (#[trigger] s.drop_last()[i]).live() by {
            assert(s.drop_last()[i] == s[i]);
        }
        assert forall|i: int, j: int| 0 <= i < j < s.drop_last().len() implies
            !(#[trigger] s.drop_last()[i]).same_slot(#[trigger] s.drop_last()[j]) by {
            assert(s.drop_last()[i] == s[i]);
            assert(s.drop_last()[j] == s[j]);
        }
        lemma_rows_of_ok(s.drop_last(), key);
        let last = s.last();
        let at_last = s.len() - 1;
        assert(s[at_last] == last);
        if last.key == key {
            let rows = before.push(last.cell());
            assert forall|i: int| 0 <= i < rows.len() implies (#[trigger] rows[i]).count != 0 || rows[i].revenue != 0 by {
                if i < before.len() { assert(rows[i] == before[i]); }
            }
            assert forall|i: int, j: int| 0 <= i < j < rows.len() implies
                !((#[trigger] rows[i]).sub == (#[trigger] rows[j]).sub && rows[i].day == rows[j].day) by {
                if j < before.len() {
                    assert(rows[i] == before[i]);
                    assert(rows[j] == before[j]);
                } else {
                    assert(rows[j] == last.cell());
                    assert(rows[i] == before[i]);
                    lemma_rows_of_elem(s.drop_last(), key, i);
                    let m = choose|m: int| 0 <= m < s.drop_last().len() && (#[trigger] s.drop_last()[m]).key == key && s.drop_last()[m].cell() == before[i];
                    assert(s[m] == s.drop_last()[m]);
                    assert(m < at_last);
                    assert(!s[m].same_slot(s[at_last]));
                }
            }
        }
    }
}

pub proof fn lemma_rows_of_sum(s: Seq<Entry>, key: i64, g: spec_fn(Cell) -> int)
    ensures sum(rows_of(s, key), g) == sum(s, |e: Entry| if e.key == key { g(e.cell()) } else { 0 }),
    decreases s.len(),
{
    if s.len() > 0 {
        lemma_rows_of_sum(s.drop_last(), key, g);
        if s.last().key == key {
            lemma_sum_push(rows_of(s.drop_last(), key), s.last().cell(), g);
        }
    }
}

/// The per-slot sums of a sequence of entries are the sums over the key's cells.
pub proof fn lemma_entries_at_rows(s: Seq<Entry>, key: i64, sub: i64, day: i32)
    ensures entries_at(s, key, sub, day) == rows_at(rows_of(s, key), sub, day),
{
    let count = |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 };
    let revenue = |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 };
    lemma_rows_of_sum(s, key, count);
    lemma_rows_of_sum(s, key, revenue);
    lemma_sum_ext_all(s, |e: Entry| slot_count(e, key, sub, day), |e: Entry| if e.key == key { count(e.cell()) } else { 0 });
    lemma_sum_ext_all(s, |e: Entry| slot_revenue(e, key, sub, day), |e: Entry| if e.key == key { revenue(e.cell()) } else { 0 });
}

/// With distinct slots, a row's slot holds exactly that row.
pub proof fn lemma_rows_at_row(rows: Seq<Cell>, index: int)
    requires rows_ok(rows), 0 <= index < rows.len(),
    ensures rows_at(rows, rows[index].sub, rows[index].day) == (rows[index].count as int, rows[index].revenue as int),
{
    let sub = rows[index].sub;
    let day = rows[index].day;
    let count = |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 };
    let revenue = |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 };
    assert forall|i: int| 0 <= i < rows.len() && i != index implies count(#[trigger] rows[i]) == 0 && revenue(rows[i]) == 0 by {
        if i < index { assert(!(rows[i].sub == rows[index].sub && rows[i].day == rows[index].day)); }
        else { assert(!(rows[index].sub == rows[i].sub && rows[index].day == rows[i].day)); }
    }
    lemma_sum_single_match(rows, count, index);
    lemma_sum_single_match(rows, revenue, index);
}

pub proof fn lemma_rows_at_zero(rows: Seq<Cell>, sub: i64, day: i32)
    requires forall|i: int| 0 <= i < rows.len() ==> !((#[trigger] rows[i]).sub == sub && rows[i].day == day),
    ensures rows_at(rows, sub, day) == (0int, 0int),
{
    let count = |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 };
    let revenue = |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 };
    lemma_sum_zero(rows, count);
    lemma_sum_zero(rows, revenue);
}

pub struct Chunk {
    pub entries: Vec<Entry>,
}

// --- one-entry edits of a chunk's entry sequence -------------------------

pub proof fn lemma_edit_update(old: Seq<Entry>, j: int, ne: Entry, idx: int)
    requires entries_ok(old), homed(old, idx), 0 <= j < old.len(), ne.live(),
        ne.key == old[j].key, ne.sub == old[j].sub, ne.day == old[j].day,
    ensures entries_ok(old.update(j, ne)), homed(old.update(j, ne), idx),
{
    let new = old.update(j, ne);
    assert forall|a: int, b: int| 0 <= a < b < new.len() implies (#[trigger] new[a]).key <= (#[trigger] new[b]).key by {
        assert(new[a].key == old[a].key);
        assert(new[b].key == old[b].key);
    }
    assert forall|a: int| 0 <= a < new.len() implies (#[trigger] new[a]).live() by {
        if a != j { assert(new[a] == old[a]); }
    }
    assert forall|a: int, b: int| 0 <= a < b < new.len() implies !(#[trigger] new[a]).same_slot(#[trigger] new[b]) by {
        assert(new[a].key == old[a].key && new[a].sub == old[a].sub && new[a].day == old[a].day);
        assert(new[b].key == old[b].key && new[b].sub == old[b].sub && new[b].day == old[b].day);
    }
    assert forall|a: int| 0 <= a < new.len() implies partition((#[trigger] new[a]).key) == idx by {
        assert(new[a].key == old[a].key);
    }
}

pub proof fn lemma_edit_remove(old: Seq<Entry>, j: int, idx: int)
    requires entries_ok(old), homed(old, idx), 0 <= j < old.len(),
    ensures entries_ok(old.remove(j)), homed(old.remove(j), idx),
{
    let new = old.remove(j);
    assert forall|a: int, b: int| 0 <= a < b < new.len() implies (#[trigger] new[a]).key <= (#[trigger] new[b]).key by {
        let sa = if a < j { a } else { a + 1 };
        let sb = if b < j { b } else { b + 1 };
        assert(new[a] == old[sa]);
        assert(new[b] == old[sb]);
    }
    assert forall|a: int| 0 <= a < new.len() implies (#[trigger] new[a]).live() by {
        let sa = if a < j { a } else { a + 1 };
        assert(new[a] == old[sa]);
    }
    assert forall|a: int, b: int| 0 <= a < b < new.len() implies !(#[trigger] new[a]).same_slot(#[trigger] new[b]) by {
        let sa = if a < j { a } else { a + 1 };
        let sb = if b < j { b } else { b + 1 };
        assert(new[a] == old[sa]);
        assert(new[b] == old[sb]);
    }
    assert forall|a: int| 0 <= a < new.len() implies partition((#[trigger] new[a]).key) == idx by {
        let sa = if a < j { a } else { a + 1 };
        assert(new[a] == old[sa]);
    }
}

/// Insert `ne` at the end of its key's run: everything before has key <= ne.key
/// (and differs in slot), everything after has key > ne.key.
pub proof fn lemma_edit_insert(old: Seq<Entry>, j: int, ne: Entry, idx: int)
    requires entries_ok(old), homed(old, idx), 0 <= j <= old.len(), ne.live(), partition(ne.key) == idx,
        forall|m: int| 0 <= m < j ==> (#[trigger] old[m]).key <= ne.key,
        forall|m: int| j <= m < old.len() ==> (#[trigger] old[m]).key > ne.key,
        forall|m: int| 0 <= m < j ==> !(#[trigger] old[m]).same_slot(ne),
    ensures entries_ok(old.insert(j, ne)), homed(old.insert(j, ne), idx),
{
    let new = old.insert(j, ne);
    assert forall|a: int, b: int| 0 <= a < b < new.len() implies (#[trigger] new[a]).key <= (#[trigger] new[b]).key by {
        if b < j {
            assert(new[a] == old[a]); assert(new[b] == old[b]);
        } else if a < j {
            assert(new[a] == old[a]);
            if b == j { assert(new[b] == ne); } else { assert(new[b] == old[b - 1]); }
        } else if a == j {
            assert(new[a] == ne); assert(new[b] == old[b - 1]);
        } else {
            assert(new[a] == old[a - 1]); assert(new[b] == old[b - 1]);
        }
    }
    assert forall|a: int| 0 <= a < new.len() implies (#[trigger] new[a]).live() by {
        if a < j { assert(new[a] == old[a]); }
        else if a == j { assert(new[a] == ne); }
        else { assert(new[a] == old[a - 1]); }
    }
    assert forall|a: int, b: int| 0 <= a < b < new.len() implies !(#[trigger] new[a]).same_slot(#[trigger] new[b]) by {
        if b < j {
            assert(new[a] == old[a]); assert(new[b] == old[b]);
        } else if a < j {
            assert(new[a] == old[a]);
            if b == j { assert(new[b] == ne); } else { assert(new[b] == old[b - 1]); }
        } else if a == j {
            assert(new[a] == ne); assert(new[b] == old[b - 1]);
        } else {
            assert(new[a] == old[a - 1]); assert(new[b] == old[b - 1]);
        }
    }
    assert forall|a: int| 0 <= a < new.len() implies partition((#[trigger] new[a]).key) == idx by {
        if a < j { assert(new[a] == old[a]); }
        else if a == j { assert(new[a] == ne); }
        else { assert(new[a] == old[a - 1]); }
    }
}

pub open spec fn bumped(old: (int, int), key: i64, sub: i64, day: i32, count: int, revenue: int, other: i64, s: i64, d: i32) -> (int, int) {
    (old.0 + (if other == key && s == sub && d == day { count } else { 0 }),
     old.1 + (if other == key && s == sub && d == day { revenue } else { 0 }))
}

pub proof fn lemma_at_update(old: Seq<Entry>, j: int, ne: Entry, count: int, revenue: int)
    requires 0 <= j < old.len(),
        ne.key == old[j].key, ne.sub == old[j].sub, ne.day == old[j].day,
        ne.count as int == old[j].count as int + count, ne.revenue as int == old[j].revenue as int + revenue,
    ensures forall|other: i64, s: i64, d: i32| #[trigger] entries_at(old.update(j, ne), other, s, d) ==
        bumped(entries_at(old, other, s, d), old[j].key, old[j].sub, old[j].day, count, revenue, other, s, d),
{
    assert forall|other: i64, s: i64, d: i32| #[trigger] entries_at(old.update(j, ne), other, s, d) ==
        bumped(entries_at(old, other, s, d), old[j].key, old[j].sub, old[j].day, count, revenue, other, s, d) by {
        lemma_sum_update(old, j, ne, |x: Entry| slot_count(x, other, s, d));
        lemma_sum_update(old, j, ne, |x: Entry| slot_revenue(x, other, s, d));
    }
}

pub proof fn lemma_at_remove(old: Seq<Entry>, j: int, count: int, revenue: int)
    requires 0 <= j < old.len(),
        old[j].count as int + count == 0, old[j].revenue as int + revenue == 0,
    ensures forall|other: i64, s: i64, d: i32| #[trigger] entries_at(old.remove(j), other, s, d) ==
        bumped(entries_at(old, other, s, d), old[j].key, old[j].sub, old[j].day, count, revenue, other, s, d),
{
    assert forall|other: i64, s: i64, d: i32| #[trigger] entries_at(old.remove(j), other, s, d) ==
        bumped(entries_at(old, other, s, d), old[j].key, old[j].sub, old[j].day, count, revenue, other, s, d) by {
        lemma_sum_remove(old, j, |x: Entry| slot_count(x, other, s, d));
        lemma_sum_remove(old, j, |x: Entry| slot_revenue(x, other, s, d));
    }
}

pub proof fn lemma_at_insert(old: Seq<Entry>, j: int, ne: Entry)
    requires 0 <= j <= old.len(),
    ensures forall|other: i64, s: i64, d: i32| #[trigger] entries_at(old.insert(j, ne), other, s, d) ==
        bumped(entries_at(old, other, s, d), ne.key, ne.sub, ne.day, ne.count as int, ne.revenue as int, other, s, d),
{
    assert forall|other: i64, s: i64, d: i32| #[trigger] entries_at(old.insert(j, ne), other, s, d) ==
        bumped(entries_at(old, other, s, d), ne.key, ne.sub, ne.day, ne.count as int, ne.revenue as int, other, s, d) by {
        lemma_sum_insert(old, j, ne, |x: Entry| slot_count(x, other, s, d));
        lemma_sum_insert(old, j, ne, |x: Entry| slot_revenue(x, other, s, d));
    }
}

impl Chunk {
    pub open spec fn wf(self, idx: int) -> bool { entries_ok(self.entries@) && homed(self.entries@, idx) }

    pub open spec fn at(self, key: i64, sub: i64, day: i32) -> (int, int) { entries_at(self.entries@, key, sub, day) }

    pub open spec fn rows(self, key: i64) -> Seq<Cell> { rows_of(self.entries@, key) }

    pub fn copy(&self) -> (out: Chunk)
        ensures out.entries@ == self.entries@,
    { Chunk { entries: self.entries.clone() } }

    /// The entries lo..hi are exactly the cells of `key`.
    pub fn run(&self, key: i64) -> (out: (usize, usize))
        requires entries_ok(self.entries@),
        ensures out.0 <= out.1 <= self.entries@.len(),
            self.rows(key) == self.entries@.subrange(out.0 as int, out.1 as int).map_values(|e: Entry| entry_cell(e)),
            forall|m: int| out.0 <= m < out.1 ==> (#[trigger] self.entries@[m]).key == key,
    {
        let n = self.entries.len();
        let mut lo = 0usize;
        while lo < n && self.entries[lo].key < key
            invariant lo <= n, n == self.entries.len(),
                forall|m: int| 0 <= m < lo ==> (#[trigger] self.entries@[m]).key < key,
            decreases n - lo,
        { lo += 1; }
        let mut hi = lo;
        while hi < n && self.entries[hi].key == key
            invariant lo <= hi <= n, n == self.entries.len(),
                forall|m: int| lo <= m < hi ==> (#[trigger] self.entries@[m]).key == key,
            decreases n - hi,
        { hi += 1; }
        proof {
            if hi < n {
                if hi > lo {
                    assert(self.entries@[hi - 1].key == key);
                } else {
                    assert(self.entries@[lo as int].key >= key);
                }
                assert(self.entries@[hi as int].key > key);
            }
            assert forall|m: int| hi <= m < n implies (#[trigger] self.entries@[m]).key != key by {
                if m > hi { assert(self.entries@[hi as int].key <= self.entries@[m].key); }
            }
            lemma_rows_char(self.entries@, key, lo as int, hi as int);
        }
        (lo, hi)
    }

    /// Add `count` and `revenue` to the slot (key, sub, day), in place.
    pub fn add(&mut self, key: i64, sub: i64, day: i32, count: i128, revenue: i128) -> (ok: bool)
        requires old(self).wf(partition(key)),
        ensures final(self).wf(partition(key)),
            !ok ==> final(self).entries@ == old(self).entries@,
            ok ==> forall|other: i64, s: i64, d: i32| #[trigger] final(self).at(other, s, d) ==
                bumped(old(self).at(other, s, d), key, sub, day, count as int, revenue as int, other, s, d),
    {
        if count == 0 && revenue == 0 { return true; }
        let n = self.entries.len();
        let mut lo = 0usize;
        while lo < n && self.entries[lo].key < key
            invariant lo <= n, n == self.entries.len(),
                forall|m: int| 0 <= m < lo ==> (#[trigger] self.entries@[m]).key < key,
            decreases n - lo,
        { lo += 1; }
        let mut j = lo;
        while j < n && self.entries[j].key == key && !(self.entries[j].sub == sub && self.entries[j].day == day)
            invariant lo <= j <= n, n == self.entries.len(),
                forall|m: int| 0 <= m < lo ==> (#[trigger] self.entries@[m]).key < key,
                lo < n ==> self.entries@[lo as int].key >= key,
                forall|m: int| lo <= m < j ==> (#[trigger] self.entries@[m]).key == key
                    && !(self.entries@[m].sub == sub && self.entries@[m].day == day),
            decreases n - j,
        { j += 1; }
        let ghost old_entries = self.entries@;
        if j < n && self.entries[j].key == key && self.entries[j].sub == sub && self.entries[j].day == day {
            let e = self.entries[j];
            let c = match (e.count as i128).checked_add(count) { Some(value) => value, None => return false };
            let r = match (e.revenue as i128).checked_add(revenue) { Some(value) => value, None => return false };
            if c < i32::MIN as i128 || c > i32::MAX as i128 || r < i64::MIN as i128 || r > i64::MAX as i128 { return false; }
            if c == 0 && r == 0 {
                self.entries.remove(j);
                proof {
                    lemma_edit_remove(old_entries, j as int, partition(key));
                    lemma_at_remove(old_entries, j as int, count as int, revenue as int);
                    assert(self.entries@ =~= old_entries.remove(j as int));
                }
            } else {
                let ne = Entry { key, sub, revenue: r as i64, day, count: c as i32 };
                self.entries.set(j, ne);
                proof {
                    lemma_edit_update(old_entries, j as int, ne, partition(key));
                    lemma_at_update(old_entries, j as int, ne, count as int, revenue as int);
                    assert(self.entries@ =~= old_entries.update(j as int, ne));
                }
            }
            true
        } else {
            if count < i32::MIN as i128 || count > i32::MAX as i128 || revenue < i64::MIN as i128 || revenue > i64::MAX as i128 {
                return false;
            }
            let ne = Entry { key, sub, revenue: revenue as i64, day, count: count as i32 };
            proof {
                if j < n {
                    if j > lo { assert(old_entries[j as int - 1].key == key); }
                    assert(old_entries[j as int].key > key);
                }
                assert forall|m: int| j <= m < n implies (#[trigger] old_entries[m]).key > key by {
                    if m > j { assert(old_entries[j as int].key <= old_entries[m].key); }
                }
                assert forall|m: int| 0 <= m < j implies (#[trigger] old_entries[m]).key <= key by {}
                assert forall|m: int| 0 <= m < j implies !(#[trigger] old_entries[m]).same_slot(ne) by {}
            }
            self.entries.insert(j, ne);
            proof {
                lemma_edit_insert(old_entries, j as int, ne, partition(key));
                lemma_at_insert(old_entries, j as int, ne);
                assert(self.entries@ =~= old_entries.insert(j as int, ne));
            }
            true
        }
    }
}

/// A key's cells inside a chunk: a window onto the chunk's entries.
#[derive(Clone, Copy)]
pub struct Run<'a> {
    pub entries: &'a [Entry],
    pub lo: usize,
    pub hi: usize,
    pub rows: Ghost<Seq<Cell>>,
}

impl<'a> Run<'a> {
    pub open spec fn wf(self) -> bool {
        &&& self.lo <= self.hi <= self.entries@.len()
        &&& self.rows@ == self.entries@.subrange(self.lo as int, self.hi as int).map_values(|e: Entry| entry_cell(e))
        &&& rows_ok(self.rows@)
    }

    pub open spec fn at(self, sub: i64, day: i32) -> (int, int) { rows_at(self.rows@, sub, day) }

    pub proof fn lemma_row(self, index: int)
        requires self.wf(), 0 <= index < self.rows@.len(),
        ensures self.at(self.rows@[index].sub, self.rows@[index].day) ==
            (self.rows@[index].count as int, self.rows@[index].revenue as int),
    { lemma_rows_at_row(self.rows@, index); }

    pub proof fn lemma_at(self, sub: i64, day: i32)
        requires self.wf(),
        ensures self.at(sub, day).0 == sum(self.rows@, |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 }),
            self.at(sub, day).1 == sum(self.rows@, |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 }),
    {}

    /// A slot with a non-zero value has a row.
    pub proof fn lemma_at_witness(self, sub: i64, day: i32)
        requires self.wf(), self.at(sub, day).0 != 0 || self.at(sub, day).1 != 0,
        ensures exists|index: int| 0 <= index < self.rows@.len()
            && (#[trigger] self.rows@[index]).sub == sub && self.rows@[index].day == day,
    {
        if !exists|index: int| 0 <= index < self.rows@.len()
            && (#[trigger] self.rows@[index]).sub == sub && self.rows@[index].day == day {
            assert forall|index: int| 0 <= index < self.rows@.len() implies
                !((#[trigger] self.rows@[index]).sub == sub && self.rows@[index].day == day) by {}
            lemma_rows_at_zero(self.rows@, sub, day);
        }
    }

    pub fn len(&self) -> (out: usize)
        requires self.wf(),
        ensures out == self.rows@.len(),
    { self.hi - self.lo }

    pub fn cell(&self, index: usize) -> (out: Cell)
        requires self.wf(), index < self.rows@.len(),
        ensures out == self.rows@[index as int],
    {
        let e = self.entries[self.lo + index];
        Cell { sub: e.sub, day: e.day, count: e.count as i128, revenue: e.revenue as i128 }
    }

    pub fn get(&self, sub: i64, day: i32) -> (out: Cell)
        requires self.wf(),
        ensures out.sub == sub, out.day == day, (out.count as int, out.revenue as int) == self.at(sub, day),
    {
        let mut index = 0usize;
        let length = self.hi - self.lo;
        while index < length
            invariant self.wf(), length == self.rows@.len(), index <= length,
                forall|i: int| 0 <= i < index ==> !((#[trigger] self.rows@[i]).sub == sub && self.rows@[i].day == day),
            decreases length - index,
        {
            let e = self.entries[self.lo + index];
            if e.sub == sub && e.day == day {
                proof { lemma_rows_at_row(self.rows@, index as int); }
                return Cell { sub, day, count: e.count as i128, revenue: e.revenue as i128 };
            }
            index += 1;
        }
        proof { lemma_rows_at_zero(self.rows@, sub, day); }
        Cell { sub, day, count: 0, revenue: 0 }
    }
}

pub struct Page { pub chunks: Vec<Arc<Chunk>> }
pub struct Dir { pub pages: Vec<Arc<Page>> }
pub struct Table { pub dirs: Vec<Arc<Dir>> }

pub fn share_arcs<Value>(source: &Vec<Arc<Value>>) -> (out: Vec<Arc<Value>>)
    ensures out@ == source@,
{
    let mut out = Vec::with_capacity(source.len());
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    { out.push(source[index].clone()); index += 1; }
    out
}

/// `count` consecutive Arcs starting at `lo`.
pub fn clone_range<Value>(source: &Vec<Arc<Value>>, lo: usize, count: usize) -> (out: Vec<Arc<Value>>)
    requires lo + count <= source.len(),
    ensures out@ == source@.subrange(lo as int, (lo + count) as int),
{
    let mut out = Vec::with_capacity(count);
    let mut index = 0;
    while index < count
        invariant index <= count, lo + count <= source.len(),
            out@ == source@.subrange(lo as int, (lo + index) as int),
        decreases count - index,
    {
        out.push(source[lo + index].clone());
        proof {
            assert(source@.subrange(lo as int, (lo + index + 1) as int) =~=
                source@.subrange(lo as int, (lo + index) as int).push(source@[(lo + index) as int]));
        }
        index += 1;
    }
    out
}

/// Tables with the same slots are the same table.
pub proof fn lemma_same_dirs(a: Table, b: Table)
    requires a.wf(), a.dirs@ == b.dirs@,
    ensures b.wf(), forall|idx: int| #[trigger] b.chunk(idx) == a.chunk(idx),
{
    assert forall|idx: int| #[trigger] b.chunk(idx) == a.chunk(idx) by {}
    assert forall|idx: int| 0 <= idx < 1048576 implies (#[trigger] b.chunk(idx)).wf(idx) by {
        assert(a.chunk(idx).wf(idx));
    }
}

/// Setting one chunk changes only that chunk.
pub proof fn lemma_set_chunk(old: Table, new: Table, code: int, chunk: Chunk)
    requires old.wf(), 0 <= code < 1048576, chunk.wf(code),
        new.dirs@.len() == 128,
        new.dirs@ == old.dirs@.update(code / 8192, new.dirs@[code / 8192]),
        new.dirs@[code / 8192].pages@ == old.dirs@[code / 8192].pages@.update((code / 64) % 128, new.dirs@[code / 8192].pages@[(code / 64) % 128]),
        new.dirs@[code / 8192].pages@[(code / 64) % 128].chunks@ ==
            old.dirs@[code / 8192].pages@[(code / 64) % 128].chunks@.update(code % 64, new.dirs@[code / 8192].pages@[(code / 64) % 128].chunks@[code % 64]),
        *new.dirs@[code / 8192].pages@[(code / 64) % 128].chunks@[code % 64] == chunk,
    ensures new.wf(),
        new.chunk(code) == chunk,
        forall|idx: int| 0 <= idx < 1048576 && idx != code ==> #[trigger] new.chunk(idx) == old.chunk(idx),
{
    let d = code / 8192;
    let p = (code / 64) % 128;
    let s = code % 64;
    assert forall|idx: int| 0 <= idx < 1048576 && idx != code implies #[trigger] new.chunk(idx) == old.chunk(idx) by {
        let d2 = idx / 8192;
        let p2 = (idx / 64) % 128;
        let s2 = idx % 64;
        assert(((d2 * 128 + p2) * 64 + s2) == idx);
        assert(((d * 128 + p) * 64 + s) == code);
        if d2 != d {
            assert(new.dirs@[d2] == old.dirs@[d2]);
        } else if p2 != p {
            assert(new.dirs@[d2].pages@[p2] == old.dirs@[d2].pages@[p2]);
        } else {
            assert(s2 != s);
            assert(new.dirs@[d2].pages@[p2].chunks@[s2] == old.dirs@[d2].pages@[p2].chunks@[s2]);
        }
    }
    assert(new.chunk(code) == chunk);
    assert forall|dd: int| 0 <= dd < 128 implies (#[trigger] new.dirs@[dd]).pages@.len() == 128 by {
        if dd == d {} else { assert(new.dirs@[dd] == old.dirs@[dd]); }
    }
    assert forall|dd: int, pp: int| 0 <= dd < 128 && 0 <= pp < 128 implies
        (#[trigger] new.dirs@[dd].pages@[pp]).chunks@.len() == 64 by {
        if dd == d && pp == p {} else if dd == d { assert(new.dirs@[dd].pages@[pp] == old.dirs@[dd].pages@[pp]); }
        else { assert(new.dirs@[dd] == old.dirs@[dd]); }
    }
    assert forall|idx: int| 0 <= idx < 1048576 implies (#[trigger] new.chunk(idx)).wf(idx) by {
        if idx != code { assert(old.chunk(idx).wf(idx)); }
    }
}

impl Table {
    pub open spec fn chunk(self, idx: int) -> Chunk {
        *self.dirs@[idx / 8192].pages@[(idx / 64) % 128].chunks@[idx % 64]
    }

    pub open spec fn rows(self, key: i64) -> Seq<Cell> { self.chunk(partition(key)).rows(key) }

    pub open spec fn at(self, key: i64, sub: i64, day: i32) -> (int, int) {
        self.chunk(partition(key)).at(key, sub, day)
    }

    pub open spec fn wf(self) -> bool {
        &&& self.dirs@.len() == 128
        &&& forall|d: int| 0 <= d < 128 ==> (#[trigger] self.dirs@[d]).pages@.len() == 128
        &&& forall|d: int, p: int| 0 <= d < 128 && 0 <= p < 128 ==>
            (#[trigger] self.dirs@[d].pages@[p]).chunks@.len() == 64
        &&& forall|idx: int| 0 <= idx < 1048576 ==> (#[trigger] self.chunk(idx)).wf(idx)
    }

    pub open spec fn counts_nonnegative(self) -> bool {
        forall|key: i64, sub: i64, day: i32| (#[trigger] self.at(key, sub, day)).0 >= 0
    }

    pub open spec fn dimension(self) -> bool {
        &&& self.counts_nonnegative()
        &&& forall|key: i64, sub: i64, day: i32| (#[trigger] self.at(key, sub, day)).1 == 0
            && (day != 0 ==> self.at(key, sub, day).0 == 0)
    }

    pub open spec fn daily(self) -> bool {
        &&& self.counts_nonnegative()
        &&& forall|key: i64, sub: i64, day: i32| sub != 0 ==> #[trigger] self.at(key, sub, day) == (0, 0)
        &&& forall|key: i64, sub: i64, day: i32| (#[trigger] self.at(key, sub, day)).0 == 0 ==>
            self.at(key, sub, day).1 == 0
    }

    pub proof fn lemma_rows(self, key: i64, sub: i64, day: i32)
        requires self.wf(),
        ensures self.at(key, sub, day).0 == sum(self.rows(key), |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 }),
            self.at(key, sub, day).1 == sum(self.rows(key), |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 }),
            forall|position: int| 0 <= position < self.rows(key).len() ==>
                #[trigger] self.at(key, self.rows(key)[position].sub, self.rows(key)[position].day) ==
                    (self.rows(key)[position].count as int, self.rows(key)[position].revenue as int),
    {
        lemma_partition_bound(key);
        let chunk = self.chunk(partition(key));
        assert(chunk.wf(partition(key)));
        lemma_entries_at_rows(chunk.entries@, key, sub, day);
        lemma_rows_of_ok(chunk.entries@, key);
        assert forall|position: int| 0 <= position < self.rows(key).len() implies
            #[trigger] self.at(key, self.rows(key)[position].sub, self.rows(key)[position].day) ==
                (self.rows(key)[position].count as int, self.rows(key)[position].revenue as int) by {
            lemma_rows_at_row(self.rows(key), position);
            lemma_entries_at_rows(chunk.entries@, key, self.rows(key)[position].sub, self.rows(key)[position].day);
        }
    }

    pub fn group(&self, key: i64) -> (out: Option<Run<'_>>)
        requires self.wf(),
        ensures out matches Some(group) ==> group.wf(),
            match out { Some(group) => group.rows@ == self.rows(key), None => self.rows(key).len() == 0 },
            forall|sub: i64, day: i32| #[trigger] self.at(key, sub, day) ==
                match out { Some(group) => group.at(sub, day), None => (0, 0) },
            out matches Some(group) ==> forall|index: int| 0 <= index < group.rows@.len() ==>
                #[trigger] self.at(key, group.rows@[index].sub, group.rows@[index].day) ==
                    (group.rows@[index].count as int, group.rows@[index].revenue as int),
            out matches Some(group) ==> (self.dimension() ==> forall|index: int| 0 <= index < group.rows@.len() ==>
                (#[trigger] group.rows@[index]).day == 0 && group.rows@[index].count > 0 && group.rows@[index].revenue == 0),
            out matches Some(group) ==> (self.daily() ==> forall|index: int| 0 <= index < group.rows@.len() ==>
                (#[trigger] group.rows@[index]).sub == 0 && group.rows@[index].count > 0),
    {
        let code = part(key);
        let chunk: &Chunk = &*self.dirs[code / 8192].pages[(code / 64) % 128].chunks[code % 64];
        proof {
            lemma_partition_bound(key);
            assert(*chunk == self.chunk(partition(key)));
            assert(chunk.wf(partition(key)));
        }
        let (lo, hi) = chunk.run(key);
        let ghost rows = chunk.rows(key);
        proof {
            lemma_rows_of_ok(chunk.entries@, key);
            self.lemma_rows(key, 0, 0);
            assert forall|sub: i64, day: i32| #[trigger] self.at(key, sub, day) == rows_at(rows, sub, day) by {
                lemma_entries_at_rows(chunk.entries@, key, sub, day);
            }
        }
        if lo == hi {
            proof {
                assert(rows =~= Seq::<Cell>::empty());
                assert forall|sub: i64, day: i32| #[trigger] self.at(key, sub, day) == (0int, 0int) by {
                    lemma_rows_at_zero(rows, sub, day);
                }
            }
            return None;
        }
        let out = Run { entries: chunk.entries.as_slice(), lo, hi, rows: Ghost(rows) };
        proof {
            assert(out.wf());
            assert forall|index: int| 0 <= index < out.rows@.len() implies
                #[trigger] self.at(key, out.rows@[index].sub, out.rows@[index].day) ==
                    (out.rows@[index].count as int, out.rows@[index].revenue as int) by {
                out.lemma_row(index);
            }
            if self.dimension() {
                assert forall|index: int| 0 <= index < out.rows@.len() implies
                    (#[trigger] out.rows@[index]).day == 0 && out.rows@[index].count > 0 && out.rows@[index].revenue == 0 by {
                    let cell = out.rows@[index];
                    assert(self.at(key, cell.sub, cell.day) == (cell.count as int, cell.revenue as int));
                    assert(cell.revenue == 0);
                    assert(cell.count >= 0);
                    if cell.day != 0 { assert(cell.count == 0); }
                }
            }
            if self.daily() {
                assert forall|index: int| 0 <= index < out.rows@.len() implies
                    (#[trigger] out.rows@[index]).sub == 0 && out.rows@[index].count > 0 by {
                    let cell = out.rows@[index];
                    assert(self.at(key, cell.sub, cell.day) == (cell.count as int, cell.revenue as int));
                    assert(cell.count >= 0);
                    if cell.count == 0 { assert(cell.revenue == 0); }
                    if cell.sub != 0 { assert(cell.count == 0); }
                }
            }
        }
        Some(out)
    }

    pub fn get(&self, key: i64, sub: i64, day: i32) -> (out: Cell)
        requires self.wf(),
        ensures (out.count as int, out.revenue as int) == self.at(key, sub, day),
    {
        match self.group(key) {
            Some(group) => group.get(sub, day),
            None => Cell { sub, day, count: 0, revenue: 0 },
        }
    }

    pub fn share(&self) -> (out: Self)
        requires self.wf(),
        ensures out.wf(), out.dirs@ == self.dirs@,
            self.counts_nonnegative() ==> out.counts_nonnegative(),
            self.dimension() ==> out.dimension(), self.daily() ==> out.daily(),
            forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == self.at(key, sub, day),
    {
        let out = Self { dirs: share_arcs(&self.dirs) };
        proof {
            lemma_same_dirs(*self, out);
            assert forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == self.at(key, sub, day) by {}
            if self.counts_nonnegative() {
                assert forall|key: i64, sub: i64, day: i32| (#[trigger] out.at(key, sub, day)).0 >= 0 by {
                    assert(self.at(key, sub, day).0 >= 0);
                }
            }
            if self.dimension() {
                assert forall|key: i64, sub: i64, day: i32| (#[trigger] out.at(key, sub, day)).1 == 0
                    && (day != 0 ==> out.at(key, sub, day).0 == 0) by {
                    assert(self.at(key, sub, day).1 == 0);
                    assert(day != 0 ==> self.at(key, sub, day).0 == 0);
                }
            }
        }
        out
    }

    pub fn add(&mut self, key: i64, cell: Cell) -> (ok: bool)
        requires old(self).wf(),
        ensures final(self).wf(), ok ==> final(self).at(key, cell.sub, cell.day) ==
            (old(self).at(key, cell.sub, cell.day).0 + cell.count,
             old(self).at(key, cell.sub, cell.day).1 + cell.revenue),
            forall|other: i64, sub: i64, day: i32| (other, sub, day) != (key, cell.sub, cell.day) ==>
                #[trigger] final(self).at(other, sub, day) == old(self).at(other, sub, day),
    {
        let code = part(key);
        let directory_index = code / 8192;
        let page_index = (code / 64) % 128;
        let slot = code % 64;
        proof {
            lemma_partition_bound(key);
            assert(self.chunk(code as int).wf(partition(key)));
        }
        let ghost entry = *self;
        let old_directory = &self.dirs[directory_index];
        let old_page = &old_directory.pages[page_index];
        let mut chunk = old_page.chunks[slot].copy();
        proof { assert(*old_page.chunks[slot as int] == entry.chunk(code as int)); }
        if !chunk.add(key, cell.sub, cell.day, cell.count, cell.revenue) { return false; }
        let mut chunks = share_arcs(&old_page.chunks);
        chunks.set(slot, Arc::new(chunk));
        let mut pages = share_arcs(&old_directory.pages);
        pages.set(page_index, Arc::new(Page { chunks }));
        self.dirs.set(directory_index, Arc::new(Dir { pages }));
        proof {
            lemma_set_chunk(entry, *self, code as int, chunk);
            assert forall|other: i64, sub: i64, day: i32| #[trigger] self.at(other, sub, day) ==
                (if partition(other) == code as int {
                    bumped(entry.at(other, sub, day), key, cell.sub, cell.day, cell.count as int, cell.revenue as int, other, sub, day)
                } else { entry.at(other, sub, day) }) by {
                lemma_partition_bound(other);
                if partition(other) == code as int {
                    assert(self.chunk(partition(other)) == chunk);
                } else {
                    assert(self.chunk(partition(other)) == entry.chunk(partition(other)));
                }
            }
            lemma_partition_bound(key);
        }
        true
    }
}

/// Per-chunk row counts, so a builder can size every chunk once.
pub fn count_hints() -> (out: Vec<u32>)
    ensures out@.len() == 1048576,
{ vec![0u32; 1048576] }

pub fn bump_hint(hints: &mut Vec<u32>, key: i64)
    requires old(hints)@.len() == 1048576,
    ensures final(hints)@.len() == 1048576,
{
    let slot = part(key);
    let value = hints[slot];
    if value < u32::MAX { hints.set(slot, value + 1); }
}

/// A table under construction: the same chunks, owned and changed in place.
pub struct Builder {
    pub chunks: Vec<Chunk>,
}

impl Builder {
    pub open spec fn wf(self) -> bool {
        &&& self.chunks@.len() == 1048576
        &&& forall|idx: int| 0 <= idx < 1048576 ==> (#[trigger] self.chunks@[idx]).wf(idx)
    }

    pub open spec fn at(self, key: i64, sub: i64, day: i32) -> (int, int) {
        self.chunks@[partition(key)].at(key, sub, day)
    }

    pub fn with_counts(hints: &Vec<u32>) -> (out: Builder)
        requires hints@.len() == 1048576,
        ensures out.wf(), forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == (0int, 0int),
    {
        let mut chunks: Vec<Chunk> = Vec::with_capacity(1048576);
        let mut index = 0usize;
        while index < 1048576
            invariant index <= 1048576, hints@.len() == 1048576, chunks@.len() == index,
                forall|i: int| 0 <= i < index ==> (#[trigger] chunks@[i]).entries@.len() == 0,
            decreases 1048576 - index,
        {
            chunks.push(Chunk { entries: Vec::with_capacity(hints[index] as usize) });
            index += 1;
        }
        let out = Builder { chunks };
        proof {
            assert forall|idx: int| 0 <= idx < 1048576 implies (#[trigger] out.chunks@[idx]).wf(idx) by {
                assert(out.chunks@[idx].entries@ =~= Seq::<Entry>::empty());
            }
            assert forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == (0int, 0int) by {
                lemma_partition_bound(key);
                assert(out.chunks@[partition(key)].entries@ =~= Seq::<Entry>::empty());
            }
        }
        out
    }

    pub fn new() -> (out: Builder)
        ensures out.wf(), forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == (0int, 0int),
    {
        let hints = count_hints();
        Builder::with_counts(&hints)
    }

    pub fn add(&mut self, key: i64, sub: i64, day: i32, count: i128, revenue: i128) -> (ok: bool)
        requires old(self).wf(),
        ensures final(self).wf(), ok ==> final(self).at(key, sub, day) ==
            (old(self).at(key, sub, day).0 + count, old(self).at(key, sub, day).1 + revenue),
            ok ==> forall|other: i64, s: i64, d: i32| (other, s, d) != (key, sub, day) ==>
                #[trigger] final(self).at(other, s, d) == old(self).at(other, s, d),
    {
        let slot = part(key);
        let ghost entry = *self;
        proof { lemma_partition_bound(key); assert(self.chunks@[slot as int].wf(slot as int)); }
        let ok = self.chunks[slot].add(key, sub, day, count, revenue);
        proof {
            assert forall|idx: int| 0 <= idx < 1048576 implies (#[trigger] self.chunks@[idx]).wf(idx) by {
                if idx != slot as int { assert(self.chunks@[idx] == entry.chunks@[idx]); }
            }
            if ok {
                assert forall|other: i64, s: i64, d: i32| #[trigger] self.at(other, s, d) ==
                    (if partition(other) == slot as int {
                        bumped(entry.at(other, s, d), key, sub, day, count as int, revenue as int, other, s, d)
                    } else { entry.at(other, s, d) }) by {
                    lemma_partition_bound(other);
                    if partition(other) != slot as int { assert(self.chunks@[partition(other)] == entry.chunks@[partition(other)]); }
                }
            }
        }
        ok
    }

    /// Wrap the chunks into the shared, persistent tree.  Nothing is copied.
    pub fn freeze(self) -> (out: Table)
        requires self.wf(),
        ensures out.wf(), forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == self.at(key, sub, day),
    {
        let ghost snapshot = self;
        let mut builder = self;
        let ghost original = builder.chunks@;
        let empty: Arc<Chunk> = Arc::new(Chunk { entries: Vec::new() });
        let mut flat: Vec<Arc<Chunk>> = Vec::with_capacity(1048576);
        let mut index = 0usize;
        while index < 1048576
            invariant index <= 1048576, builder.chunks@.len() == 1048576, original.len() == 1048576,
                flat@.len() == index, (*empty).entries@.len() == 0,
                forall|i: int| 0 <= i < index ==> (*#[trigger] flat@[i]).entries@ == original[i].entries@,
                forall|i: int| index <= i < 1048576 ==> builder.chunks@[i] == original[i],
            decreases 1048576 - index,
        {
            let mut c = Chunk { entries: Vec::new() };
            std::mem::swap(&mut c, &mut builder.chunks[index]);
            if c.entries.len() == 0 {
                flat.push(empty.clone());
                proof { assert(c.entries@ =~= (*empty).entries@); }
            } else {
                flat.push(Arc::new(c));
            }
            index += 1;
        }
        let mut pages: Vec<Arc<Page>> = Vec::with_capacity(16384);
        let mut group = 0usize;
        while group < 16384
            invariant group <= 16384, flat@.len() == 1048576, pages@.len() == group,
                forall|i: int| 0 <= i < group ==> (*#[trigger] pages@[i]).chunks@ == flat@.subrange(i * 64, i * 64 + 64),
            decreases 16384 - group,
        {
            pages.push(Arc::new(Page { chunks: clone_range(&flat, group * 64, 64) }));
            group += 1;
        }
        let mut dirs: Vec<Arc<Dir>> = Vec::with_capacity(128);
        let mut dir = 0usize;
        while dir < 128
            invariant dir <= 128, pages@.len() == 16384, dirs@.len() == dir,
                forall|i: int| 0 <= i < dir ==> (*#[trigger] dirs@[i]).pages@ == pages@.subrange(i * 128, i * 128 + 128),
            decreases 128 - dir,
        {
            dirs.push(Arc::new(Dir { pages: clone_range(&pages, dir * 128, 128) }));
            dir += 1;
        }
        let out = Table { dirs };
        proof {
            assert forall|idx: int| 0 <= idx < 1048576 implies #[trigger] out.chunk(idx).entries@ == original[idx].entries@ by {
                assert(((idx / 8192) * 128 + (idx / 64) % 128) * 64 + idx % 64 == idx);
            }
            assert forall|d: int| 0 <= d < 128 implies (#[trigger] out.dirs@[d]).pages@.len() == 128 by {}
            assert forall|d: int, p: int| 0 <= d < 128 && 0 <= p < 128 implies
                (#[trigger] out.dirs@[d].pages@[p]).chunks@.len() == 64 by {
                assert(out.dirs@[d].pages@[p] == pages@[d * 128 + p]);
            }
            assert forall|idx: int| 0 <= idx < 1048576 implies (#[trigger] out.chunk(idx)).wf(idx) by {
                assert(original[idx].wf(idx));
            }
            assert forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == snapshot.at(key, sub, day) by {
                lemma_partition_bound(key);
            }
        }
        out
    }
}

pub fn empty_table() -> (out: Table)
    ensures out.wf(), forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == (0int, 0int),
{
    Builder::new().freeze()
}

/// A table with the contents of an accumulator.
pub fn freeze(source: Bucket) -> (out: Option<Table>)
    requires source.wf(),
    ensures out matches Some(table) ==> table.wf()
        && forall|key: i64, sub: i64, day: i32| #[trigger] table.at(key, sub, day) == source.at(key, sub, day),
{
    let mut table = empty_table();
    if !merge(&mut table, &source, false) { return None; }
    Some(table)
}

pub fn merge(table: &mut Table, changes: &Bucket, positive: bool) -> (ok: bool)
    requires old(table).wf(), changes.wf(),
    ensures final(table).wf(), ok ==> forall|key: i64, sub: i64, day: i32|
        #[trigger] final(table).at(key, sub, day) ==
        (old(table).at(key, sub, day).0 + changes.at(key, sub, day).0,
         old(table).at(key, sub, day).1 + changes.at(key, sub, day).1),
        ok && positive && old(table).counts_nonnegative() ==> final(table).counts_nonnegative(),
{
    let ghost entry = *table;
    let mut index = 0;
    while index < changes.keys.len()
        invariant table.wf(), changes.wf(), index <= changes.keys.len(),
            entry == *old(table),
            positive && entry.counts_nonnegative() ==> table.counts_nonnegative(),
            forall|key: i64, sub: i64, day: i32| #[trigger] table.at(key, sub, day) ==
                (entry.at(key, sub, day).0 + if changes.keys@.take(index as int).contains(key) { changes.at(key, sub, day).0 } else { 0 },
                 entry.at(key, sub, day).1 + if changes.keys@.take(index as int).contains(key) { changes.at(key, sub, day).1 } else { 0 }),
        decreases changes.keys.len() - index,
    {
        let key = changes.keys[index];
        let group = changes.groups.get(&key).unwrap();
        let ghost before_group = *table;
        proof {
            verus_prooflib::lemma_prefix_fresh(changes.keys@, index as int);
            verus_prooflib::lemma_push_membership(changes.keys@.take(index as int), key);
            assert(changes.keys@.take(index as int + 1) =~= changes.keys@.take(index as int).push(key));
        }
        let mut variant = 0;
        while variant < group.rows.len()
            invariant table.wf(), changes.wf(), group.wf(), variant <= group.rows.len(),
                positive && entry.counts_nonnegative() ==> table.counts_nonnegative(),
                key == changes.keys@[index as int], index < changes.keys.len(),
                group == changes.groups@[key],
                changes.groups@.contains_key(key),
                entry == *old(table),
                forall|other: i64, sub: i64, day: i32| #[trigger] before_group.at(other, sub, day) ==
                    (entry.at(other, sub, day).0 + if changes.keys@.take(index as int).contains(other) { changes.at(other, sub, day).0 } else { 0 },
                     entry.at(other, sub, day).1 + if changes.keys@.take(index as int).contains(other) { changes.at(other, sub, day).1 } else { 0 }),
                !changes.keys@.take(index as int).contains(key),
                forall|other: i64, sub: i64, day: i32| #[trigger] table.at(other, sub, day) ==
                    (before_group.at(other, sub, day).0 + sum(group.rows@.take(variant as int), |cell: Cell|
                        if other == key && cell.sub == sub && cell.day == day { cell.count as int } else { 0 }),
                     before_group.at(other, sub, day).1 + sum(group.rows@.take(variant as int), |cell: Cell|
                        if other == key && cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 })),
            decreases group.rows.len() - variant,
        {
            let delta = group.rows[variant];
            let before = table.get(key, delta.sub, delta.day);
            let count = match before.count.checked_add(delta.count) { Some(value) => value, None => return false };
            let revenue = match before.revenue.checked_add(delta.revenue) { Some(value) => value, None => return false };
            if positive && (count < 0 || (count == 0 && revenue != 0)) { return false; }
            if !table.add(key, delta) { return false; }
            proof {
                assert forall|other: i64, sub: i64, day: i32| #[trigger] table.at(other, sub, day) ==
                    (before_group.at(other, sub, day).0 + sum(group.rows@.take(variant as int + 1), |cell: Cell|
                        if other == key && cell.sub == sub && cell.day == day { cell.count as int } else { 0 }),
                     before_group.at(other, sub, day).1 + sum(group.rows@.take(variant as int + 1), |cell: Cell|
                        if other == key && cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 })) by {
                    lemma_sum_take(group.rows@, variant as int, |cell: Cell| if other == key && cell.sub == sub && cell.day == day { cell.count as int } else { 0 });
                    lemma_sum_take(group.rows@, variant as int, |cell: Cell| if other == key && cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 });
                }
            }
            variant += 1;
        }
        proof {
            assert(group.rows@.take(group.rows@.len() as int) =~= group.rows@);
            assert forall|sub: i64, day: i32| #[trigger] group.at(sub, day) == changes.at(key, sub, day) by {
                group.lemma_at(sub, day);
            }
            assert forall|other: i64, sub: i64, day: i32| #[trigger] table.at(other, sub, day) ==
                (entry.at(other, sub, day).0 + if changes.keys@.take(index as int + 1).contains(other) { changes.at(other, sub, day).0 } else { 0 },
                 entry.at(other, sub, day).1 + if changes.keys@.take(index as int + 1).contains(other) { changes.at(other, sub, day).1 } else { 0 }) by {
                if other == key { group.lemma_at(sub, day); }
                else {
                    lemma_sum_zero(group.rows@, |cell: Cell| if other == key && cell.sub == sub && cell.day == day { cell.count as int } else { 0 });
                    lemma_sum_zero(group.rows@, |cell: Cell| if other == key && cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 });
                }
                assert(changes.keys@.take(index as int + 1) =~= changes.keys@.take(index as int).push(key));
                verus_prooflib::lemma_push_membership(changes.keys@.take(index as int), key);
                assert(table.at(other, sub, day).0 == before_group.at(other, sub, day).0 + sum(group.rows@, |cell: Cell| if other == key && cell.sub == sub && cell.day == day { cell.count as int } else { 0 }));
                assert(before_group.at(other, sub, day).0 == entry.at(other, sub, day).0 + if changes.keys@.take(index as int).contains(other) { changes.at(other, sub, day).0 } else { 0 });
                if other == key {
                    assert(group.at(sub, day) == changes.at(key, sub, day));
                    lemma_sum_ext_all(group.rows@,
                        |cell: Cell| if other == key && cell.sub == sub && cell.day == day { cell.count as int } else { 0 },
                        |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 });
                    lemma_sum_ext_all(group.rows@,
                        |cell: Cell| if other == key && cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 },
                        |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 });
                    assert(sum(group.rows@, |cell: Cell| if other == key && cell.sub == sub && cell.day == day { cell.count as int } else { 0 }) == group.at(sub, day).0);
                }
            }
        }
        index += 1;
    }
    proof {
        assert(changes.keys@.take(changes.keys@.len() as int) =~= changes.keys@);
        assert forall|key: i64, sub: i64, day: i32| #[trigger] table.at(key, sub, day) ==
            (entry.at(key, sub, day).0 + changes.at(key, sub, day).0,
             entry.at(key, sub, day).1 + changes.at(key, sub, day).1) by {
            if !changes.keys@.contains(key) { assert(!changes.groups@.contains_key(key)); }
        }
    }
    true
}

}
