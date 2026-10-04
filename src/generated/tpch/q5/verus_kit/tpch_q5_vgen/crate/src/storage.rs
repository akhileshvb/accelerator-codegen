use vstd::prelude::*;
use std::collections::HashMap;
use std::sync::Arc;
use verus_common::sums::*;

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

pub struct Layer {
    pub base: Arc<Bucket>,
    pub replacements: Bucket,
}

impl Layer {
    pub open spec fn rows(self, key: i64) -> Seq<Cell> {
        if self.replacements.groups@.contains_key(key) { self.replacements.groups@[key].rows@ }
        else if self.base.groups@.contains_key(key) { self.base.groups@[key].rows@ }
        else { Seq::empty() }
    }

    pub open spec fn at(self, key: i64, sub: i64, day: i32) -> (int, int) {
        if self.replacements.groups@.contains_key(key) { self.replacements.at(key, sub, day) }
        else { self.base.at(key, sub, day) }
    }

    pub open spec fn wf(self) -> bool { self.base.wf() && self.replacements.wf() && self.replacements.keys@.len() < 64 }

    pub fn group(&self, key: i64) -> (out: Option<&Group>)
        requires self.wf(),
        ensures out matches Some(group) ==> group.wf(),
            match out { Some(group) => group.rows@ == self.rows(key), None => self.rows(key).len() == 0 },
            forall|sub: i64, day: i32| #[trigger] self.at(key, sub, day) ==
                match out { Some(group) => group.at(sub, day), None => (0, 0) },
    {
        match self.replacements.groups.get(&key) {
            Some(group) => Some(group),
            None => self.base.groups.get(&key),
        }
    }

    pub fn changed(&self, key: i64, cell: Cell) -> (out: Option<Self>)
        requires self.wf(),
        ensures out matches Some(layer) ==> layer.wf()
            && layer.at(key, cell.sub, cell.day) ==
                (self.at(key, cell.sub, cell.day).0 + cell.count, self.at(key, cell.sub, cell.day).1 + cell.revenue)
            && forall|other: i64, sub: i64, day: i32| (other, sub, day) != (key, cell.sub, cell.day) ==>
                #[trigger] layer.at(other, sub, day) == self.at(other, sub, day),
    {
        let mut replacements = self.replacements.copy();
        let mut group = match self.group(key) { Some(group) => group.copy(), None => Group::new() };
        if !group.add(cell.sub, cell.day, cell.count, cell.revenue) { return None; }
        replacements.replace(key, group);
        if replacements.keys.len() < 64 { return Some(Self { base: self.base.clone(), replacements }); }
        let mut base = Bucket::new();
        let mut index = 0;
        while index < self.base.keys.len()
            invariant self.wf(), replacements.wf(), base.wf(), index <= self.base.keys.len(),
                forall|key: i64, sub: i64, day: i32| #[trigger] base.at(key, sub, day) ==
                    if self.base.keys@.take(index as int).contains(key) && !replacements.groups@.contains_key(key) {
                        self.base.at(key, sub, day)
                    } else { (0, 0) },
            decreases self.base.keys.len() - index,
        {
            let old_key = self.base.keys[index];
            proof {
                verus_prooflib::lemma_prefix_fresh(self.base.keys@, index as int);
                verus_prooflib::lemma_push_membership(self.base.keys@.take(index as int), old_key);
                assert(self.base.keys@.take(index as int + 1) =~= self.base.keys@.take(index as int).push(old_key));
            }
            if !replacements.groups.contains_key(&old_key) {
                let old_group = self.base.groups.get(&old_key).unwrap();
                if old_group.rows.len() > 0 {
                    base.replace(old_key, old_group.copy());
                }
            }
            index += 1;
        }
        proof { assert(self.base.keys@.take(self.base.keys@.len() as int) =~= self.base.keys@); }
        let ghost old_base = base;
        proof {
            assert forall|key: i64, sub: i64, day: i32| #[trigger] old_base.at(key, sub, day) ==
                if !replacements.groups@.contains_key(key) { self.base.at(key, sub, day) } else { (0, 0) } by {
                if !self.base.keys@.contains(key) { assert(!self.base.groups@.contains_key(key)); }
            }
        }
        index = 0;
        while index < replacements.keys.len()
            invariant self.wf(), replacements.wf(), base.wf(), index <= replacements.keys.len(),
                forall|key: i64, sub: i64, day: i32| #[trigger] base.at(key, sub, day) ==
                    if replacements.keys@.take(index as int).contains(key) { replacements.at(key, sub, day) }
                    else { old_base.at(key, sub, day) },
                forall|key: i64, sub: i64, day: i32| #[trigger] old_base.at(key, sub, day) ==
                    if !replacements.groups@.contains_key(key) { self.base.at(key, sub, day) } else { (0, 0) },
            decreases replacements.keys.len() - index,
        {
            let new_key = replacements.keys[index];
            proof {
                verus_prooflib::lemma_prefix_fresh(replacements.keys@, index as int);
                verus_prooflib::lemma_push_membership(replacements.keys@.take(index as int), new_key);
                assert(replacements.keys@.take(index as int + 1) =~= replacements.keys@.take(index as int).push(new_key));
            }
            let new_group = replacements.groups.get(&new_key).unwrap();
            if new_group.rows.len() > 0 {
                base.replace(new_key, new_group.copy());
            }
            index += 1;
        }
        proof {
            assert(replacements.keys@.take(replacements.keys@.len() as int) =~= replacements.keys@);
        }
        Some(Self { base: Arc::new(base), replacements: Bucket::new() })
    }
}

pub struct Page { pub buckets: Vec<Arc<Layer>> }
pub struct Directory { pub pages: Vec<Arc<Page>> }
pub struct Table { pub pages: Vec<Arc<Directory>> }

pub open spec fn partition_at(parts: Map<i64, Bucket>, key: i64, sub: i64, day: i32) -> (int, int) {
    if parts.contains_key(partition(key) as i64) { parts[partition(key) as i64].at(key, sub, day) } else { (0, 0) }
}

pub open spec fn page_matches(page: Page, parts: Map<i64, Bucket>, leaf: i64) -> bool {
    &&& page.buckets@.len() == 256
    &&& forall|slot: int| 0 <= slot < 256 ==> (#[trigger] page.buckets@[slot]).wf()
    &&& forall|slot: int| 0 <= slot < 256 ==>
        (#[trigger] page.buckets@[slot]).replacements.groups@ == Map::empty()
        && page.buckets@[slot].base.groups@ ==
            if parts.contains_key((leaf as int * 256 + slot) as i64) { parts[(leaf as int * 256 + slot) as i64].groups@ } else { Map::empty() }
}

pub open spec fn partition(key: i64) -> int { (key as u64 % 16777216u64) as int }

pub fn share_arcs<Value>(source: &Vec<Arc<Value>>) -> (out: Vec<Arc<Value>>)
    ensures out@ == source@,
{
    let mut out = Vec::new();
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), out@ == source@.take(index as int),
        decreases source.len() - index,
    { out.push(source[index].clone()); index += 1; }
    out
}

impl Table {
    pub proof fn lemma_rows(self, key: i64, sub: i64, day: i32)
        requires self.wf(),
        ensures self.at(key, sub, day).0 == sum(self.rows(key), |cell: Cell| if cell.sub == sub && cell.day == day { cell.count as int } else { 0 }),
            self.at(key, sub, day).1 == sum(self.rows(key), |cell: Cell| if cell.sub == sub && cell.day == day { cell.revenue as int } else { 0 }),
            forall|position: int| 0 <= position < self.rows(key).len() ==>
                #[trigger] self.at(key, self.rows(key)[position].sub, self.rows(key)[position].day) ==
                    (self.rows(key)[position].count as int, self.rows(key)[position].revenue as int),
    {
        let layer = self.bucket(key);
        assert(layer.wf());
        if layer.replacements.groups@.contains_key(key) {
            layer.replacements.groups@[key].lemma_at(sub, day);
        } else if layer.base.groups@.contains_key(key) {
            layer.base.groups@[key].lemma_at(sub, day);
        }
    }
    pub open spec fn rows(self, key: i64) -> Seq<Cell> { self.bucket(key).rows(key) }

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

    pub open spec fn bucket(self, key: i64) -> Layer {
        *self.pages@[partition(key) / 65536].pages@[(partition(key) / 256) % 256].buckets@[partition(key) % 256]
    }

    pub open spec fn at(self, key: i64, sub: i64, day: i32) -> (int, int) {
        self.bucket(key).at(key, sub, day)
    }

    pub open spec fn wf(self) -> bool {
        &&& self.pages@.len() == 256
        &&& forall|directory: int| 0 <= directory < 256 ==> (#[trigger] self.pages@[directory]).pages@.len() == 256
        &&& forall|directory: int, page: int| 0 <= directory < 256 && 0 <= page < 256 ==>
            (#[trigger] self.pages@[directory].pages@[page]).buckets@.len() == 256
        &&& forall|directory: int, page: int, slot: int| 0 <= directory < 256 && 0 <= page < 256 && 0 <= slot < 256 ==>
            (#[trigger] self.pages@[directory].pages@[page].buckets@[slot]).wf()
    }

    pub fn group(&self, key: i64) -> (out: Option<&Group>)
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
        let code = (key as u64 % 16777216) as usize;
        let out = self.pages[code / 65536].pages[(code / 256) % 256].buckets[code % 256].group(key);
        proof {
            if let Some(group) = out {
                assert forall|index: int| 0 <= index < group.rows@.len() implies
                    #[trigger] self.at(key, group.rows@[index].sub, group.rows@[index].day) ==
                    (group.rows@[index].count as int, group.rows@[index].revenue as int) by {
                    group.lemma_row(index);
                }
                if self.dimension() {
                    assert forall|index: int| 0 <= index < group.rows@.len() implies
                        (#[trigger] group.rows@[index]).day == 0 && group.rows@[index].count > 0 && group.rows@[index].revenue == 0 by {
                        let cell = group.rows@[index];
                        assert(self.at(key, cell.sub, cell.day) == (cell.count as int, cell.revenue as int));
                        assert(cell.revenue == 0);
                        assert(cell.count >= 0);
                        if cell.day != 0 { assert(cell.count == 0); }
                    }
                }
                if self.daily() {
                    assert forall|index: int| 0 <= index < group.rows@.len() implies
                        (#[trigger] group.rows@[index]).sub == 0 && group.rows@[index].count > 0 by {
                        let cell = group.rows@[index];
                        assert(self.at(key, cell.sub, cell.day) == (cell.count as int, cell.revenue as int));
                        assert(cell.count >= 0);
                        if cell.count == 0 { assert(cell.revenue == 0); }
                        if cell.sub != 0 { assert(cell.count == 0); }
                    }
                }
            }
        }
        out
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
        ensures out.wf(), out.pages@ == self.pages@,
            self.counts_nonnegative() ==> out.counts_nonnegative(),
            self.dimension() ==> out.dimension(), self.daily() ==> out.daily(),
            forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == self.at(key, sub, day),
    {
        let out = Self { pages: share_arcs(&self.pages) };
        proof {
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
        let code = (key as u64 % 16777216) as usize;
        let directory_index = code / 65536;
        let page_index = (code / 256) % 256;
        let slot = code % 256;
        let old_directory = &self.pages[directory_index];
        let old_page = &old_directory.pages[page_index];
        let bucket = match old_page.buckets[slot].changed(key, cell) { Some(bucket) => bucket, None => return false };
        let mut buckets = share_arcs(&old_page.buckets);
        buckets.set(slot, Arc::new(bucket));
        let mut pages = share_arcs(&old_directory.pages);
        pages.set(page_index, Arc::new(Page { buckets }));
        self.pages.set(directory_index, Arc::new(Directory { pages }));
        true
    }
}

pub fn freeze(source: Bucket) -> (out: Table)
    requires source.wf(),
    ensures out.wf(),
        forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == source.at(key, sub, day),
{
    let ghost original = source;
    let mut source = source;
    let mut partitions: HashMap<i64, Bucket> = HashMap::new();
    let mut leaf_keys: Vec<i64> = Vec::new();
    let mut leaf_seen: HashMap<i64, bool> = HashMap::new();
    let mut index = 0;
    while index < source.keys.len()
        invariant original.wf(), index <= source.keys.len(), source.keys@ == original.keys@,
            forall|key: i64| #[trigger] source.groups@.contains_key(key) <==>
                original.groups@.contains_key(key) && !original.keys@.take(index as int).contains(key),
            forall|key: i64| source.groups@.contains_key(key) ==> #[trigger] source.groups@[key] == original.groups@[key],
            forall|code: i64| partitions@.contains_key(code) ==> (#[trigger] partitions@[code]).wf() && 0 <= code < 16777216,
            forall|key: i64, sub: i64, day: i32| #[trigger] partition_at(partitions@, key, sub, day) ==
                if original.keys@.take(index as int).contains(key) { original.at(key, sub, day) } else { (0, 0) },
            leaf_keys@.no_duplicates(),
            forall|leaf: i64| #[trigger] leaf_seen@.contains_key(leaf) <==> leaf_keys@.contains(leaf),
            forall|position: int| 0 <= position < leaf_keys@.len() ==> 0 <= #[trigger] leaf_keys@[position] < 65536,
            forall|code: i64| #[trigger] partitions@.contains_key(code) ==> leaf_keys@.contains((code / 256) as i64),
        decreases source.keys.len() - index,
    {
        let key = source.keys[index];
        let ghost before_parts = partitions@;
        proof {
            verus_prooflib::lemma_prefix_fresh(original.keys@, index as int);
            assert(source.groups@.contains_key(key));
            verus_prooflib::lemma_push_membership(original.keys@.take(index as int), key);
            assert(original.keys@.take(index as int + 1) =~= original.keys@.take(index as int).push(key));
        }
        let code = (key as u64 % 16777216) as i64;
        let leaf_key = code / 256;
        if !leaf_seen.contains_key(&leaf_key) {
            proof {
                verus_prooflib::lemma_unique_push(leaf_keys@, leaf_key);
                verus_prooflib::lemma_push_membership(leaf_keys@, leaf_key);
            }
            leaf_seen.insert(leaf_key, true);
            leaf_keys.push(leaf_key);
        }
        let mut bucket = match partitions.remove(&code) { Some(bucket) => bucket, None => Bucket::new() };
        let group = source.groups.remove(&key).unwrap();
        let ghost stored = group;
        bucket.replace(key, group);
        let ghost updated = bucket;
        partitions.insert(code, bucket);
        proof {
            assert(partitions@ =~= before_parts.insert(code, updated));
            assert forall|other: i64, sub: i64, day: i32| #[trigger] partition_at(partitions@, other, sub, day) ==
                if original.keys@.take(index as int + 1).contains(other) { original.at(other, sub, day) } else { (0, 0) } by {
                if other == key {
                    assert(stored == original.groups@[key]);
                    assert(updated.at(key, sub, day) == stored.at(sub, day));
                } else if partition(other) as i64 == code {
                    assert(updated.at(other, sub, day) == partition_at(before_parts, other, sub, day));
                } else {
                    assert(partition_at(partitions@, other, sub, day) == partition_at(before_parts, other, sub, day));
                }
            }
        }
        index += 1;
    }
    proof { assert(original.keys@.take(original.keys@.len() as int) =~= original.keys@); }
    let ghost full_parts = partitions@;
    let empty = Arc::new(Layer { base: Arc::new(Bucket::new()), replacements: Bucket::new() });
    let mut leaf_pages: HashMap<i64, Arc<Page>> = HashMap::new();
    index = 0;
    while index < leaf_keys.len()
        invariant original.wf(), index <= leaf_keys.len(), leaf_keys@.no_duplicates(),
            forall|position: int| 0 <= position < leaf_keys@.len() ==> 0 <= #[trigger] leaf_keys@[position] < 65536,
            forall|code: i64| full_parts.contains_key(code) ==> (#[trigger] full_parts[code]).wf() && 0 <= code < 16777216,
            forall|code: i64| #[trigger] full_parts.contains_key(code) ==> leaf_keys@.contains((code / 256) as i64),
            forall|key: i64, sub: i64, day: i32| #[trigger] partition_at(full_parts, key, sub, day) == original.at(key, sub, day),
            forall|code: i64| #[trigger] partitions@.contains_key(code) <==>
                full_parts.contains_key(code) && !leaf_keys@.take(index as int).contains((code / 256) as i64),
            forall|code: i64| partitions@.contains_key(code) ==> #[trigger] partitions@[code] == full_parts[code],
            forall|leaf: i64| #[trigger] leaf_pages@.contains_key(leaf) <==> leaf_keys@.take(index as int).contains(leaf),
            forall|leaf: i64| leaf_pages@.contains_key(leaf) ==> #[trigger] page_matches(*leaf_pages@[leaf], full_parts, leaf),
            empty.wf(), empty.base.groups@ == Map::empty(), empty.replacements.groups@ == Map::empty(),
        decreases leaf_keys.len() - index,
    {
        let leaf_key = leaf_keys[index];
        proof { verus_prooflib::lemma_prefix_fresh(leaf_keys@, index as int); }
        let mut buckets: Vec<Arc<Layer>> = Vec::new();
        let mut slot = 0usize;
        while slot < 256
            invariant slot <= 256, 0 <= leaf_key < 65536, leaf_key == leaf_keys@[index as int], index < leaf_keys.len(),
                leaf_keys@.no_duplicates(), !leaf_keys@.take(index as int).contains(leaf_key), buckets@.len() == slot,
                forall|code: i64| full_parts.contains_key(code) ==> (#[trigger] full_parts[code]).wf() && 0 <= code < 16777216,
                forall|code: i64| #[trigger] partitions@.contains_key(code) <==>
                    full_parts.contains_key(code) && !leaf_keys@.take(index as int).contains((code / 256) as i64)
                    && (code / 256 != leaf_key || code % 256 >= slot),
                forall|code: i64| partitions@.contains_key(code) ==> #[trigger] partitions@[code] == full_parts[code],
                empty.wf(), empty.base.groups@ == Map::empty(), empty.replacements.groups@ == Map::empty(),
                forall|position: int| 0 <= position < slot ==> (#[trigger] buckets@[position]).wf()
                    && buckets@[position].replacements.groups@ == Map::empty()
                    && buckets@[position].base.groups@ == if full_parts.contains_key((leaf_key as int * 256 + position) as i64) {
                        full_parts[(leaf_key as int * 256 + position) as i64].groups@
                    } else { Map::empty() },
            decreases 256 - slot,
        {
            let code = leaf_key * 256 + slot as i64;
            let bucket = match partitions.remove(&code) {
                Some(bucket) => Arc::new(Layer { base: Arc::new(bucket), replacements: Bucket::new() }),
                None => empty.clone(),
            };
            buckets.push(bucket);
            slot += 1;
        }
        proof {
            verus_prooflib::lemma_push_membership(leaf_keys@.take(index as int), leaf_key);
            assert(leaf_keys@.take(index as int + 1) =~= leaf_keys@.take(index as int).push(leaf_key));
        }
        leaf_pages.insert(leaf_key, Arc::new(Page { buckets }));
        index += 1;
    }
    proof { assert(leaf_keys@.take(leaf_keys@.len() as int) =~= leaf_keys@); }
    let mut empty_buckets: Vec<Arc<Layer>> = Vec::new();
    index = 0;
    while index < 256
        invariant index <= 256, empty_buckets@.len() == index,
            empty.wf(), empty.base.groups@ == Map::empty(), empty.replacements.groups@ == Map::empty(),
            forall|position: int| 0 <= position < index ==> #[trigger] empty_buckets@[position] == empty,
        decreases 256 - index,
    { empty_buckets.push(empty.clone()); index += 1; }
    let empty_page = Arc::new(Page { buckets: empty_buckets });
    let mut pages: Vec<Arc<Directory>> = Vec::new();
    let mut page = 0usize;
    while page < 256
        invariant original.wf(), page <= 256, pages@.len() == page,
            forall|key: i64, sub: i64, day: i32| #[trigger] partition_at(full_parts, key, sub, day) == original.at(key, sub, day),
            forall|leaf: i64| leaf_pages@.contains_key(leaf) ==> #[trigger] page_matches(*leaf_pages@[leaf], full_parts, leaf),
            forall|code: i64| #[trigger] full_parts.contains_key(code) ==> leaf_pages@.contains_key((code / 256) as i64),
            empty_page.buckets@.len() == 256,
            forall|position: int| 0 <= position < 256 ==> (#[trigger] empty_page.buckets@[position]).wf()
                && empty_page.buckets@[position].base.groups@ == Map::empty()
                && empty_page.buckets@[position].replacements.groups@ == Map::empty(),
            forall|directory: int| 0 <= directory < page ==> (#[trigger] pages@[directory]).pages@.len() == 256,
            forall|directory: int, slot: int| 0 <= directory < page && 0 <= slot < 256 ==>
                #[trigger] page_matches(*pages@[directory].pages@[slot], full_parts, (directory * 256 + slot) as i64),
        decreases 256 - page,
    {
        let mut directory_pages: Vec<Arc<Page>> = Vec::new();
        let mut slot = 0usize;
        while slot < 256
            invariant page < 256, slot <= 256, directory_pages@.len() == slot,
                pages@.len() == page,
                forall|directory: int| 0 <= directory < page ==> (#[trigger] pages@[directory]).pages@.len() == 256,
                forall|directory: int, position: int| 0 <= directory < page && 0 <= position < 256 ==>
                    #[trigger] page_matches(*pages@[directory].pages@[position], full_parts, (directory * 256 + position) as i64),
                forall|leaf: i64| leaf_pages@.contains_key(leaf) ==> #[trigger] page_matches(*leaf_pages@[leaf], full_parts, leaf),
                forall|code: i64| #[trigger] full_parts.contains_key(code) ==> leaf_pages@.contains_key((code / 256) as i64),
                empty_page.buckets@.len() == 256,
                forall|position: int| 0 <= position < 256 ==> (#[trigger] empty_page.buckets@[position]).wf()
                    && empty_page.buckets@[position].base.groups@ == Map::empty()
                    && empty_page.buckets@[position].replacements.groups@ == Map::empty(),
                forall|position: int| 0 <= position < slot ==>
                    #[trigger] page_matches(*directory_pages@[position], full_parts, (page as int * 256 + position) as i64),
            decreases 256 - slot,
        {
            let code = (page * 256 + slot) as i64;
            let leaf_page = match leaf_pages.get(&code) { Some(leaf_page) => leaf_page.clone(), None => empty_page.clone() };
            directory_pages.push(leaf_page);
            slot += 1;
        }
        pages.push(Arc::new(Directory { pages: directory_pages }));
        page += 1;
    }
    let out = Table { pages };
    proof {
        assert forall|directory: int, page: int| 0 <= directory < 256 && 0 <= page < 256 implies
            (#[trigger] out.pages@[directory].pages@[page]).buckets@.len() == 256 by {
            assert(page_matches(*out.pages@[directory].pages@[page], full_parts, (directory * 256 + page) as i64));
        }
        assert forall|directory: int, page: int, slot: int| 0 <= directory < 256 && 0 <= page < 256 && 0 <= slot < 256 implies
            (#[trigger] out.pages@[directory].pages@[page].buckets@[slot]).wf() by {
            assert(page_matches(*out.pages@[directory].pages@[page], full_parts, (directory * 256 + page) as i64));
        }
        assert(out.wf());
        assert forall|key: i64, sub: i64, day: i32| #[trigger] out.at(key, sub, day) == original.at(key, sub, day) by {
            let code = partition(key);
            let directory = code / 65536;
            let page = (code / 256) % 256;
            let slot = code % 256;
            assert((directory * 256 + page) * 256 + slot == code);
            assert(page_matches(*out.pages@[directory].pages@[page], full_parts, (code / 256) as i64));
            assert(out.at(key, sub, day) == partition_at(full_parts, key, sub, day));
        }
    }
    out
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
