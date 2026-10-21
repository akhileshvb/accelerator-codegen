// Compact page storage.  A page keeps its order, shipment and group cells in parallel arrays of
// narrow integers whose view is the wide cell sequence the proofs reason about; a key is stored
// as the offset from the page's first key.  A cell sequence that does not fit the narrow arrays
// is kept as the cells themselves, so packing never fails.
use std::sync::Arc;
use vstd::prelude::*;
use verus_common::bytes::*;
use crate::model::{OrderCell, ShipCell, GroupCell, group_before_id, group_lt};
use crate::wide::{self, Wide};
use crate::columns::{copy_orders, copy_ships, copy_groups};
use crate::values::GroupId;

verus! {

pub open spec fn number_ok(number: int) -> bool {
    -36028797018963968 <= number <= 36028797018963967
}

pub open spec fn key_in_range(number: int, low: u8) -> bool {
    let key = number * 256 + low as int;
    0 <= key && key < 1099511627776
}

pub open spec fn page_key(number: int, low: u8) -> i64 {
    (number * 256 + low as int) as i64
}

// ---- orders ------------------------------------------------------------------------------

pub open spec fn order_cell(number: int, low: u8, customer: u32, date: u16, priority: i8, count: u8) -> OrderCell {
    OrderCell { key: page_key(number, low), customer: customer as i64, date: date as i32, priority: priority as i64, count: count as i64 }
}

pub struct NarrowOrders {
    pub(crate) number: i64,
    pub(crate) lows: Vec<u8>,
    pub(crate) customers: Vec<u32>,
    pub(crate) dates: Vec<u16>,
    pub(crate) priorities: Vec<i8>,
    pub(crate) counts: Vec<u8>,
}

impl View for NarrowOrders {
    type V = Seq<OrderCell>;
    closed spec fn view(&self) -> Seq<OrderCell> {
        Seq::new(self.lows@.len(), |i: int| order_cell(self.number as int, self.lows@[i], self.customers@[i], self.dates@[i],
            self.priorities@[i], self.counts@[i]))
    }
}

impl NarrowOrders {
    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        &&& number_ok(self.number as int)
        &&& self.customers@.len() == self.lows@.len() && self.dates@.len() == self.lows@.len()
        &&& self.priorities@.len() == self.lows@.len() && self.counts@.len() == self.lows@.len()
    }

    pub fn len(&self) -> (result: usize)
        ensures result == self@.len(),
    { self.lows.len() }

    pub fn get(&self, index: usize) -> (result: OrderCell)
        requires index < self@.len(),
        ensures result == self@[index as int],
    {
        proof { use_type_invariant(self); }
        OrderCell { key: self.number * 256 + self.lows[index] as i64, customer: self.customers[index] as i64,
            date: self.dates[index] as i32, priority: self.priorities[index] as i64, count: self.counts[index] as i64 }
    }

    pub fn to_vec(&self) -> (result: Vec<OrderCell>)
        ensures result@ == self@,
    {
        let mut result = Vec::with_capacity(self.lows.len());
        let mut index = 0usize;
        while index < self.lows.len()
            invariant index <= self.lows.len(), self.lows.len() == self@.len(), result@ == self@.take(index as int),
            decreases self.lows.len() - index,
        {
            let cell = self.get(index);
            result.push(cell);
            proof { assert(self@.take(index as int + 1) =~= self@.take(index as int).push(self@[index as int])); }
            index += 1;
        }
        proof { assert(self@.take(index as int) =~= self@); }
        result
    }

    pub fn pack(rows: &Vec<OrderCell>, number: i64) -> (result: Option<NarrowOrders>)
        ensures result matches Some(packed) ==> packed@ == rows@,
    {
        if number < -36028797018963968 || number > 36028797018963967 { return None; }
        let base = number as i128 * 256;
        let mut lows: Vec<u8> = Vec::with_capacity(rows.len());
        let mut customers: Vec<u32> = Vec::with_capacity(rows.len());
        let mut dates: Vec<u16> = Vec::with_capacity(rows.len());
        let mut priorities: Vec<i8> = Vec::with_capacity(rows.len());
        let mut counts: Vec<u8> = Vec::with_capacity(rows.len());
        let mut index = 0usize;
        while index < rows.len()
            invariant index <= rows.len(), number_ok(number as int), base == number as int * 256,
                lows@.len() == index, customers@.len() == index, dates@.len() == index,
                priorities@.len() == index, counts@.len() == index,
                forall|position: int| #![trigger rows@[position]] 0 <= position < index ==>
                    order_cell(number as int, lows@[position], customers@[position], dates@[position], priorities@[position], counts@[position]) == rows@[position],
            decreases rows.len() - index,
        {
            let cell = rows[index];
            let low = cell.key as i128 - base;
            if low < 0 || low > 255 || cell.customer < 0 || cell.customer > u32::MAX as i64
                || cell.date < 0 || cell.date > 65535
                || cell.priority < i8::MIN as i64 || cell.priority > i8::MAX as i64
                || cell.count < 0 || cell.count > 255 { return None; }
            lows.push(low as u8);
            customers.push(cell.customer as u32);
            dates.push(cell.date as u16);
            priorities.push(cell.priority as i8);
            counts.push(cell.count as u8);
            proof {
                assert(order_cell(number as int, lows@[index as int], customers@[index as int], dates@[index as int],
                    priorities@[index as int], counts@[index as int]) == cell);
            }
            index += 1;
        }
        let result = NarrowOrders { number, lows, customers, dates, priorities, counts };
        proof { assert(result@ =~= rows@); }
        Some(result)
    }
}

// ---- shipments ---------------------------------------------------------------------------

/// The revenue stored at `index`: the cell's own word, or for the escape word the first
/// matching entry of the side table (revenues of 2^32 - 1 and more are rare).
pub open spec fn big_revenue(big: Seq<(u32, i64)>, index: int) -> i64
    decreases big.len(),
{
    if big.len() == 0 { 0 }
    else if big[0].0 as int == index { big[0].1 }
    else { big_revenue(big.skip(1), index) }
}

pub open spec fn stored_revenue(word: u32, big: Seq<(u32, i64)>, index: int) -> i64 {
    if word == u32::MAX { big_revenue(big, index) } else { word as i64 }
}

pub open spec fn narrow_wide(revenue: i64) -> Wide {
    Wide { hi: 0, lo: revenue as u128 }
}

pub open spec fn ship_cell(number: int, low: u8, count: u8, date: u16, revenue: i64) -> ShipCell {
    ShipCell { key: page_key(number, low), date: date as i32, count: count as i64,
        revenue: narrow_wide(revenue), positive: narrow_wide(revenue) }
}

pub struct NarrowShips {
    pub(crate) number: i64,
    pub(crate) lows: Vec<u8>,
    pub(crate) counts: Vec<u8>,
    pub(crate) dates: Vec<u16>,
    pub(crate) words: Vec<u32>,
    pub(crate) big: Vec<(u32, i64)>,
}

impl View for NarrowShips {
    type V = Seq<ShipCell>;
    closed spec fn view(&self) -> Seq<ShipCell> {
        Seq::new(self.lows@.len(), |i: int| ship_cell(self.number as int, self.lows@[i], self.counts@[i], self.dates@[i],
            stored_revenue(self.words@[i], self.big@, i)))
    }
}

pub fn lookup_big(big: &Vec<(u32, i64)>, index: usize) -> (result: i64)
    requires index <= u32::MAX as usize,
    ensures result == big_revenue(big@, index as int),
{
    let mut position = 0;
    proof { assert(big@.skip(0) =~= big@); }
    while position < big.len()
        invariant position <= big.len(), index <= u32::MAX as usize,
            forall|offset: int| #![trigger big@[offset]] 0 <= offset < position ==> big@[offset].0 as int != index as int,
            big_revenue(big@, index as int) == big_revenue(big@.skip(position as int), index as int),
        decreases big.len() - position,
    {
        if big[position].0 as usize == index {
            proof { assert(big@.skip(position as int)[0] == big@[position as int]); }
            return big[position].1;
        }
        proof {
            assert(big@.skip(position as int).skip(1) =~= big@.skip(position as int + 1));
        }
        position += 1;
    }
    proof { assert(big@.skip(position as int).len() == 0); }
    0
}

impl NarrowShips {
    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        &&& number_ok(self.number as int)
        &&& self.counts@.len() == self.lows@.len() && self.dates@.len() == self.lows@.len()
        &&& self.words@.len() == self.lows@.len()
        &&& self.lows@.len() <= u32::MAX as int
    }

    pub fn len(&self) -> (result: usize)
        ensures result == self@.len(),
    { self.lows.len() }

    pub fn get(&self, index: usize) -> (result: ShipCell)
        requires index < self@.len(),
        ensures result == self@[index as int],
    {
        proof { use_type_invariant(self); }
        let word = self.words[index];
        let revenue: i64 = if word == u32::MAX { lookup_big(&self.big, index) } else { word as i64 };
        ShipCell { key: self.number * 256 + self.lows[index] as i64, date: self.dates[index] as i32,
            count: self.counts[index] as i64, revenue: Wide { hi: 0, lo: revenue as u128 }, positive: Wide { hi: 0, lo: revenue as u128 } }
    }

    pub fn to_vec(&self) -> (result: Vec<ShipCell>)
        ensures result@ == self@,
    {
        let mut result = Vec::with_capacity(self.lows.len());
        let mut index = 0usize;
        while index < self.lows.len()
            invariant index <= self.lows.len(), self.lows.len() == self@.len(), result@ == self@.take(index as int),
            decreases self.lows.len() - index,
        {
            let cell = self.get(index);
            result.push(cell);
            proof { assert(self@.take(index as int + 1) =~= self@.take(index as int).push(self@[index as int])); }
            index += 1;
        }
        proof { assert(self@.take(index as int) =~= self@); }
        result
    }

    pub fn pack(rows: &Vec<ShipCell>, number: i64) -> (result: Option<NarrowShips>)
        ensures result matches Some(packed) ==> packed@ == rows@,
    {
        if number < -36028797018963968 || number > 36028797018963967 { return None; }
        if rows.len() > u32::MAX as usize { return None; }
        let base = number as i128 * 256;
        let mut lows: Vec<u8> = Vec::with_capacity(rows.len());
        let mut counts: Vec<u8> = Vec::with_capacity(rows.len());
        let mut dates: Vec<u16> = Vec::with_capacity(rows.len());
        let mut words: Vec<u32> = Vec::with_capacity(rows.len());
        let mut big: Vec<(u32, i64)> = Vec::new();
        let mut index = 0usize;
        while index < rows.len()
            invariant index <= rows.len(), number_ok(number as int), base == number as int * 256, rows.len() <= u32::MAX as usize,
                lows@.len() == index, counts@.len() == index, dates@.len() == index, words@.len() == index,
                forall|offset: int| #![trigger big@[offset]] 0 <= offset < big@.len() ==> (big@[offset].0 as int) < index as int,
                forall|position: int| #![trigger rows@[position]] 0 <= position < index ==>
                    ship_cell(number as int, lows@[position], counts@[position], dates@[position], stored_revenue(words@[position], big@, position)) == rows@[position],
            decreases rows.len() - index,
        {
            let cell = rows[index];
            let low = cell.key as i128 - base;
            if low < 0 || low > 255 || cell.count < 0 || cell.count > 255 || cell.date < 0 || cell.date > 65535
                || cell.revenue.hi != 0 || cell.revenue.lo > i64::MAX as u128 || cell.positive.hi != 0 || cell.positive.lo != cell.revenue.lo { return None; }
            let ghost before_big = big@;
            if cell.revenue.lo >= u32::MAX as u128 {
                big.push((index as u32, cell.revenue.lo as i64));
                words.push(u32::MAX);
                proof {
                    // earlier cells keep their value: their indices differ from the new entry's
                    assert forall|position: int| #![trigger rows@[position]] 0 <= position < index implies
                        stored_revenue(words@[position], big@, position) == stored_revenue(words@[position], before_big, position) by {
                        lemma_big_append(before_big, big@.last(), position);
                    }
                    lemma_big_new(before_big, big@.last(), index as int);
                }
            } else {
                words.push(cell.revenue.lo as u32);
            }
            lows.push(low as u8);
            counts.push(cell.count as u8);
            dates.push(cell.date as u16);
            proof {
                assert(ship_cell(number as int, lows@[index as int], counts@[index as int], dates@[index as int],
                    stored_revenue(words@[index as int], big@, index as int)) == cell);
            }
            index += 1;
        }
        let result = NarrowShips { number, lows, counts, dates, words, big };
        proof { assert(result@ =~= rows@); }
        Some(result)
    }
}

/// Appending an entry for another index leaves earlier lookups alone.
proof fn lemma_big_append(before: Seq<(u32, i64)>, entry: (u32, i64), index: int)
    requires entry.0 as int != index,
    ensures big_revenue(before.push(entry), index) == big_revenue(before, index),
    decreases before.len(),
{
    let after = before.push(entry);
    reveal_with_fuel(big_revenue, 2);
    if before.len() > 0 {
        assert(after[0] == before[0]);
        assert(after.skip(1) =~= before.skip(1).push(entry));
        if before[0].0 as int != index { lemma_big_append(before.skip(1), entry, index); }
        assert(big_revenue(after, index) == big_revenue(before, index));
    } else {
        assert(after[0] == entry);
        assert(after.skip(1).len() == 0);
    }
}

/// The new entry answers its own index when no earlier entry has it.
proof fn lemma_big_new(before: Seq<(u32, i64)>, entry: (u32, i64), index: int)
    requires entry.0 as int == index, forall|offset: int| #![trigger before[offset]] 0 <= offset < before.len() ==> before[offset].0 as int != index,
    ensures big_revenue(before.push(entry), index) == entry.1,
    decreases before.len(),
{
    let after = before.push(entry);
    if before.len() > 0 {
        assert(after[0] == before[0]);
        assert(after.skip(1) =~= before.skip(1).push(entry));
        assert forall|offset: int| #![trigger before.skip(1)[offset]] 0 <= offset < before.skip(1).len() implies before.skip(1)[offset].0 as int != index by {
            assert(before.skip(1)[offset] == before[offset + 1]);
        }
        lemma_big_new(before.skip(1), entry, index);
    } else {
        assert(after[0] == entry);
    }
}

// ---- groups ------------------------------------------------------------------------------

/// Two group cells with the same content (their segment buffers may be different allocations).
pub open spec fn group_equiv(left: GroupCell, right: GroupCell) -> bool {
    (*left.segment)@ == (*right.segment)@ && left.id == right.id && left.weight == right.weight
        && left.bound == right.bound && left.last_ship == right.last_ship
}

pub open spec fn groups_equiv(left: Seq<GroupCell>, right: Seq<GroupCell>) -> bool {
    left.len() == right.len() && forall|index: int| #![trigger left[index]] 0 <= index < left.len() ==> group_equiv(left[index], right[index])
}

pub open spec fn none_last() -> u16 { 65535 }

pub open spec fn group_cell(number: int, segment: Arc<Vec<u8>>, low: u8, date: u16, priority: i8, weight: u32, bound: i64, last: u16) -> GroupCell {
    GroupCell { segment, id: GroupId { key: page_key(number, low), date: date as i32, priority: priority as i64 },
        weight: weight as i128, bound,
        last_ship: if last == none_last() { None } else { Some(last as i32) } }
}

pub struct NarrowGroups {
    pub(crate) number: i64,
    pub(crate) names: Vec<Arc<Vec<u8>>>,
    pub(crate) segments: Vec<u8>,
    pub(crate) lows: Vec<u8>,
    pub(crate) dates: Vec<u16>,
    pub(crate) priorities: Vec<i8>,
    pub(crate) weights: Vec<u32>,
    pub(crate) bounds: Vec<i64>,
    pub(crate) lasts: Vec<u16>,
}

impl View for NarrowGroups {
    type V = Seq<GroupCell>;
    closed spec fn view(&self) -> Seq<GroupCell> {
        Seq::new(self.lows@.len(), |i: int| group_cell(self.number as int, self.names@[self.segments@[i] as int], self.lows@[i],
            self.dates@[i], self.priorities@[i], self.weights@[i], self.bounds@[i], self.lasts@[i]))
    }
}

impl NarrowGroups {
    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        &&& number_ok(self.number as int)
        &&& self.segments@.len() == self.lows@.len() && self.dates@.len() == self.lows@.len()
        &&& self.priorities@.len() == self.lows@.len() && self.weights@.len() == self.lows@.len()
        &&& self.bounds@.len() == self.lows@.len() && self.lasts@.len() == self.lows@.len()
        &&& forall|i: int| #![trigger self.segments@[i]] 0 <= i < self.segments@.len() ==> (self.segments@[i] as int) < self.names@.len()
        &&& forall|i: int| #![trigger self.lows@[i]] 0 <= i < self.lows@.len() ==>
            key_in_range(self.number as int, self.lows@[i])
    }

    pub fn len(&self) -> (result: usize)
        ensures result == self@.len(),
    { self.lows.len() }

    pub fn get(&self, index: usize) -> (result: GroupCell)
        requires index < self@.len(),
        ensures result == self@[index as int],
    {
        proof { use_type_invariant(self); }
        let last = self.lasts[index];
        GroupCell { segment: self.names[self.segments[index] as usize].clone(),
            id: GroupId { key: self.number * 256 + self.lows[index] as i64, date: self.dates[index] as i32,
                priority: self.priorities[index] as i64 },
            weight: self.weights[index] as i128, bound: self.bounds[index],
            last_ship: if last == 65535 { None } else { Some(last as i32) } }
    }

    pub fn weight_at(&self, index: usize) -> (result: i128)
        requires index < self@.len(),
        ensures result == self@[index as int].weight,
    {
        proof { use_type_invariant(self); }
        self.weights[index] as i128
    }

    /// The group at `index` sorts before (segment, id).
    pub fn before_id(&self, index: usize, segment: &[u8], id: GroupId) -> (result: bool)
        requires index < self@.len(),
        ensures result == group_before_id(self@[index as int], segment@, id),
    {
        proof { use_type_invariant(self); }
        let name = &self.names[self.segments[index] as usize];
        let key = self.number * 256 + self.lows[index] as i64;
        let date = self.dates[index] as i32;
        let priority = self.priorities[index] as i64;
        bytes_lt_exec(name, segment) || (bytes_eq_exec(name, segment)
            && (key < id.key || (key == id.key && (date < id.date || (date == id.date && priority < id.priority)))))
    }

    /// The group at `index` is exactly (segment, id).
    pub fn is_group(&self, index: usize, segment: &[u8], id: GroupId) -> (result: bool)
        requires index < self@.len(),
        ensures result == (self@[index as int].id.key == id.key && self@[index as int].id.date == id.date
            && self@[index as int].id.priority == id.priority && (*self@[index as int].segment)@ == segment@),
    {
        proof { use_type_invariant(self); }
        let name = &self.names[self.segments[index] as usize];
        self.number * 256 + self.lows[index] as i64 == id.key && self.dates[index] as i32 == id.date
            && self.priorities[index] as i64 == id.priority && bytes_eq_exec(name, segment)
    }

    pub fn to_vec(&self) -> (result: Vec<GroupCell>)
        ensures result@ == self@,
    {
        let mut result = Vec::with_capacity(self.lows.len());
        let mut index = 0usize;
        while index < self.lows.len()
            invariant index <= self.lows.len(), self.lows.len() == self@.len(), result@ == self@.take(index as int),
            decreases self.lows.len() - index,
        {
            let cell = self.get(index);
            result.push(cell);
            proof { assert(self@.take(index as int + 1) =~= self@.take(index as int).push(self@[index as int])); }
            index += 1;
        }
        proof { assert(self@.take(index as int) =~= self@); }
        result
    }

    pub fn pack(rows: &Vec<GroupCell>, number: i64) -> (result: Option<NarrowGroups>)
        ensures result matches Some(packed) ==> groups_equiv(packed@, rows@),
    {
        if number < -36028797018963968 || number > 36028797018963967 { return None; }
        let base = number as i128 * 256;
        let mut names: Vec<Arc<Vec<u8>>> = Vec::new();
        let mut segments: Vec<u8> = Vec::with_capacity(rows.len());
        let mut lows: Vec<u8> = Vec::with_capacity(rows.len());
        let mut dates: Vec<u16> = Vec::with_capacity(rows.len());
        let mut priorities: Vec<i8> = Vec::with_capacity(rows.len());
        let mut weights: Vec<u32> = Vec::with_capacity(rows.len());
        let mut bounds: Vec<i64> = Vec::with_capacity(rows.len());
        let mut lasts: Vec<u16> = Vec::with_capacity(rows.len());
        let mut index = 0usize;
        while index < rows.len()
            invariant index <= rows.len(), number_ok(number as int), base == number as int * 256,
                names@.len() <= 256,
                segments@.len() == index, lows@.len() == index, dates@.len() == index,
                priorities@.len() == index, weights@.len() == index, bounds@.len() == index, lasts@.len() == index,
                forall|position: int| #![trigger segments@[position]] 0 <= position < index ==> (segments@[position] as int) < names@.len(),
                forall|position: int| #![trigger lows@[position]] 0 <= position < index ==>
                    key_in_range(number as int, lows@[position]),
                forall|position: int| #![trigger rows@[position]] 0 <= position < index ==>
                    group_equiv(group_cell(number as int, names@[segments@[position] as int], lows@[position], dates@[position],
                        priorities@[position], weights@[position], bounds@[position], lasts@[position]), rows@[position]),
            decreases rows.len() - index,
        {
            let cell = &rows[index];
            let low = cell.id.key as i128 - base;
            if low < 0 || low > 255 || cell.id.key < 0 || cell.id.key >= 1099511627776 || cell.id.date < 0 || cell.id.date > 65534
                || cell.id.priority < i8::MIN as i64 || cell.id.priority > i8::MAX as i64
                || cell.weight < 0 || cell.weight > u32::MAX as i128 || cell.bound < 0 { return None; }
            let last: u16 = match cell.last_ship {
                Some(value) => { if value < 0 || value > 65534 { return None; } value as u16 },
                None => 65535,
            };
            let mut name = 0usize;
            while name < names.len() && !bytes_eq_exec(&names[name], &cell.segment)
                invariant name <= names.len(),
                    forall|position: int| #![trigger names@[position]] 0 <= position < name ==> (*names@[position])@ != (*cell.segment)@,
                decreases names.len() - name,
            { name += 1; }
            if name == names.len() {
                if names.len() >= 255 { return None; }
                names.push(cell.segment.clone());
            }
            proof { assert(names@.len() <= 255 || name < names@.len()); }
            segments.push(name as u8);
            lows.push(low as u8);
            dates.push(cell.id.date as u16);
            priorities.push(cell.id.priority as i8);
            weights.push(cell.weight as u32);
            bounds.push(cell.bound);
            lasts.push(last);
            proof {
                let built = group_cell(number as int, names@[segments@[index as int] as int], lows@[index as int], dates@[index as int],
                    priorities@[index as int], weights@[index as int], bounds@[index as int], lasts@[index as int]);
                assert((*built.segment)@ == (*cell.segment)@);
                assert(built.id == cell.id);
                assert(built.last_ship == cell.last_ship);
                assert(group_equiv(built, rows@[index as int]));
            }
            index += 1;
        }
        let result = NarrowGroups { number, names, segments, lows, dates, priorities, weights, bounds, lasts };
        proof {
            assert(groups_equiv(result@, rows@));
        }
        Some(result)
    }
}


// ---- narrow or whole ---------------------------------------------------------------------

pub enum PackedOrders { Narrow(NarrowOrders), Wide(Vec<OrderCell>) }

impl View for PackedOrders {
    type V = Seq<OrderCell>;
    closed spec fn view(&self) -> Seq<OrderCell> {
        match self { PackedOrders::Narrow(narrow) => narrow@, PackedOrders::Wide(rows) => rows@ }
    }
}

impl PackedOrders {
    pub fn len(&self) -> (result: usize)
        ensures result == self@.len(),
    { match self { PackedOrders::Narrow(narrow) => narrow.len(), PackedOrders::Wide(rows) => rows.len() } }

    pub fn get(&self, index: usize) -> (result: OrderCell)
        requires index < self@.len(),
        ensures result == self@[index as int],
    { match self { PackedOrders::Narrow(narrow) => narrow.get(index), PackedOrders::Wide(rows) => rows[index] } }

    pub fn to_vec(&self) -> (result: Vec<OrderCell>)
        ensures result@ == self@,
    { match self { PackedOrders::Narrow(narrow) => narrow.to_vec(), PackedOrders::Wide(rows) => copy_orders(rows) } }

    pub fn pack(rows: &Vec<OrderCell>, number: i64) -> (result: PackedOrders)
        ensures result@ == rows@,
    {
        match NarrowOrders::pack(rows, number) {
            Some(narrow) => PackedOrders::Narrow(narrow),
            None => PackedOrders::Wide(copy_orders(rows)),
        }
    }
}

pub enum PackedShips { Narrow(NarrowShips), Wide(Vec<ShipCell>) }

impl View for PackedShips {
    type V = Seq<ShipCell>;
    closed spec fn view(&self) -> Seq<ShipCell> {
        match self { PackedShips::Narrow(narrow) => narrow@, PackedShips::Wide(rows) => rows@ }
    }
}

impl PackedShips {
    pub fn len(&self) -> (result: usize)
        ensures result == self@.len(),
    { match self { PackedShips::Narrow(narrow) => narrow.len(), PackedShips::Wide(rows) => rows.len() } }

    pub fn get(&self, index: usize) -> (result: ShipCell)
        requires index < self@.len(),
        ensures result == self@[index as int],
    { match self { PackedShips::Narrow(narrow) => narrow.get(index), PackedShips::Wide(rows) => rows[index] } }

    pub fn to_vec(&self) -> (result: Vec<ShipCell>)
        ensures result@ == self@,
    { match self { PackedShips::Narrow(narrow) => narrow.to_vec(), PackedShips::Wide(rows) => copy_ships(rows) } }

    pub fn pack(rows: &Vec<ShipCell>, number: i64) -> (result: PackedShips)
        ensures result@ == rows@,
    {
        match NarrowShips::pack(rows, number) {
            Some(narrow) => PackedShips::Narrow(narrow),
            None => PackedShips::Wide(copy_ships(rows)),
        }
    }
}

pub enum PackedGroups { Narrow(NarrowGroups), Wide(Vec<GroupCell>) }

impl View for PackedGroups {
    type V = Seq<GroupCell>;
    closed spec fn view(&self) -> Seq<GroupCell> {
        match self { PackedGroups::Narrow(narrow) => narrow@, PackedGroups::Wide(rows) => rows@ }
    }
}

impl PackedGroups {
    pub fn len(&self) -> (result: usize)
        ensures result == self@.len(),
    { match self { PackedGroups::Narrow(narrow) => narrow.len(), PackedGroups::Wide(rows) => rows.len() } }

    pub fn get(&self, index: usize) -> (result: GroupCell)
        requires index < self@.len(),
        ensures result == self@[index as int],
    {
        match self {
            PackedGroups::Narrow(narrow) => narrow.get(index),
            PackedGroups::Wide(rows) => {
                let group = &rows[index];
                GroupCell { segment: group.segment.clone(), id: group.id, weight: group.weight, bound: group.bound, last_ship: group.last_ship }
            },
        }
    }

    pub fn weight_at(&self, index: usize) -> (result: i128)
        requires index < self@.len(),
        ensures result == self@[index as int].weight,
    { match self { PackedGroups::Narrow(narrow) => narrow.weight_at(index), PackedGroups::Wide(rows) => rows[index].weight } }

    /// The group at `index` sorts before (segment, id).
    pub fn before_id(&self, index: usize, segment: &[u8], id: GroupId) -> (result: bool)
        requires index < self@.len(),
        ensures result == group_before_id(self@[index as int], segment@, id),
    {
        match self {
            PackedGroups::Narrow(narrow) => narrow.before_id(index, segment, id),
            PackedGroups::Wide(rows) => {
                let group = &rows[index];
                bytes_lt_exec(&group.segment, segment) || (bytes_eq_exec(&group.segment, segment)
                    && (group.id.key < id.key || (group.id.key == id.key && (group.id.date < id.date
                    || (group.id.date == id.date && group.id.priority < id.priority)))))
            },
        }
    }

    pub fn to_vec(&self) -> (result: Vec<GroupCell>)
        ensures result@ == self@,
    { match self { PackedGroups::Narrow(narrow) => narrow.to_vec(), PackedGroups::Wide(rows) => copy_groups(rows) } }

    pub fn pack(rows: &Vec<GroupCell>, number: i64) -> (result: PackedGroups)
        ensures groups_equiv(result@, rows@),
    {
        match NarrowGroups::pack(rows, number) {
            Some(narrow) => PackedGroups::Narrow(narrow),
            None => {
                let copy = copy_groups(rows);
                proof { assert(groups_equiv(copy@, rows@)); }
                PackedGroups::Wide(copy)
            },
        }
    }
}

}
