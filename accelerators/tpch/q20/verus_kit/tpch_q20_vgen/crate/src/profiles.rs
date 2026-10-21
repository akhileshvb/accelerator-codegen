use vstd::prelude::*;
use std::sync::Arc;
use std::collections::HashMap;
use verus_common::sums::{sum, lemma_sum_update};
use verus_prooflib::lemma_sum_insert;
use verus_prooflib::wide::Wide;
use crate::profile_proofs::*;

verus! {
pub struct StockCell { pub value: i64, pub count: Wide }
pub struct Prefix { pub date: i32, pub count: Wide, pub quantity: Wide }
pub struct Daily { pub date: i32, pub count: Wide, pub quantity: Wide }
pub struct Pair { pub stocks: Vec<StockCell>, pub ship: Vec<Prefix> }
pub struct Part { pub count: Wide, pub pairs: Vec<(i64, Pair)> }
pub struct PairWork { pub stocks: Vec<StockCell>, pub days: HashMap<i32, Daily>, pub dates: Vec<i32> }
pub struct PartWork { pub count: Wide, pub pairs: HashMap<i64, PairWork>, pub original: HashMap<i64, Pair>, pub keys: Vec<i64> }

fn sort_dates(source: &Vec<i32>) -> (result: Vec<i32>)
    ensures source@.no_duplicates() ==> result@.no_duplicates()
        && result@.to_set() == source@.to_set()
        && verus_common::order::sorted_by(result@, |left: i32, right: i32| left < right),
{
    let mut sorted: Vec<i32> = Vec::new();
    let mut index = 0;
    while index < source.len()
        invariant index <= source.len(),
            source@.no_duplicates() ==> sorted@.no_duplicates()
                && sorted@.to_set() == source@.take(index as int).to_set()
                && verus_common::order::sorted_by(sorted@, |left: i32, right: i32| left < right),
        decreases source.len() - index,
    {
        let date = source[index];
        let ghost before = sorted@;
        proof {
            if source@.no_duplicates() {
                verus_prooflib::lemma_prefix_fresh(source@, index as int);
                before.to_set_ensures();
                source@.take(index as int).to_set_ensures();
                assert(!source@.take(index as int).to_set().contains(date));
                assert(!before.to_set().contains(date));
                assert(!before.contains(date));
            }
        }
        let mut low = 0; let mut high = sorted.len();
        while low < high
            invariant low <= high <= sorted.len(), sorted@ == before, index < source.len(), date == source@[index as int],
                source@.no_duplicates() ==> !before.contains(date)
                    && before.to_set() == source@.take(index as int).to_set()
                    && verus_common::order::sorted_by(before, |left: i32, right: i32| left < right),
                source@.no_duplicates() ==> forall|offset: int| #![trigger before[offset]] 0 <= offset < low ==> before[offset] < date,
                source@.no_duplicates() ==> forall|offset: int| #![trigger before[offset]] high <= offset < before.len() ==> date < before[offset],
            decreases high - low,
        {
            let middle = low + (high - low) / 2;
            if sorted[middle] < date {
                proof {
                    if source@.no_duplicates() {
                        assert forall|offset: int| #![trigger before[offset]] 0 <= offset < middle implies before[offset] < date by {
                            assert(before[offset] < before[middle as int]);
                        }
                    }
                }
                low = middle + 1;
            } else {
                proof {
                    if source@.no_duplicates() {
                        assert(before[middle as int] != date);
                        assert forall|offset: int| #![trigger before[offset]] middle < offset < before.len() implies date < before[offset] by {
                            assert(before[middle as int] < before[offset]);
                        }
                    }
                }
                high = middle;
            }
        }
        proof {
            if source@.no_duplicates() {
                verus_prooflib::lemma_sorted_insert_at(before, low as int, date, |left: i32, right: i32| left < right);
            }
        }
        sorted.insert(low, date);
        proof {
            if source@.no_duplicates() {
                assert forall|value: i32| #![trigger sorted@.contains(value)] sorted@.contains(value) <==> before.contains(value) || value == date by {
                    assert(sorted@[low as int] == date);
                    if value == date { assert(sorted@.contains(value)); }
                    if sorted@.contains(value) {
                        let offset = choose|offset: int| #![trigger sorted@[offset]] 0 <= offset < sorted@.len() && sorted@[offset] == value;
                        assert(0 <= offset < sorted@.len() && sorted@[offset] == value);
                        if offset < low { assert(before[offset] == value); }
                        else if offset > low { assert(before[offset - 1] == value); }
                    }
                    if before.contains(value) {
                        let offset = choose|offset: int| #![trigger before[offset]] 0 <= offset < before.len() && before[offset] == value;
                        assert(0 <= offset < before.len() && before[offset] == value);
                        assert(sorted@[if offset < low { offset } else { offset + 1 }] == value);
                    }
                }
                assert forall|left: int, right: int| 0 <= left < right < sorted@.len() implies
                    (#[trigger] sorted@[left]) != (#[trigger] sorted@[right]) by {
                    assert(sorted@[left] < sorted@[right]);
                }
                sorted@.to_set_ensures();
                assert(sorted@.to_set() =~= before.to_set().insert(date));
                assert(source@.take(index as int + 1) =~= source@.take(index as int).push(date));
                verus_prooflib::lemma_push_to_set(source@.take(index as int), date);
            }
        }
        index += 1;
    }
    proof { assert(source@.take(source.len() as int) =~= source@); }
    sorted
}

impl PairWork {
    pub open spec fn shape(self) -> bool {
        &&& verus_common::order::sorted_by(self.stocks@, |left: StockCell, right: StockCell| left.value < right.value)
        &&& self.dates@.no_duplicates()
        &&& self.days@.dom() == self.dates@.to_set()
        &&& forall|date: i32| #![trigger self.days@[date]] self.days@.contains_key(date) ==> self.days@[date].date == date
    }
    pub open spec fn stock_count(self, value: int) -> int {
        stock_measure(self.stocks@, value)
    }
    /// What `finish` needs to succeed: a well-shaped work pair whose measures are those of real rows.
    pub open spec fn finish_ready(self) -> bool {
        &&& self.shape()
        &&& forall|value: int| #![trigger self.stock_count(value)] self.stock_count(value) >= 0
        &&& forall|date: int| #![trigger day_measure(self.days@, date, false)]
            day_measure(self.days@, date, false) >= 0
            && (day_measure(self.days@, date, false) == 0 ==> day_measure(self.days@, date, true) == 0)
    }
    pub open spec fn day_count(self, date: i32) -> int {
        if self.days@.contains_key(date) { self.days@[date].count@ } else { 0 }
    }
    pub open spec fn day_quantity(self, date: i32) -> int {
        if self.days@.contains_key(date) { self.days@[date].quantity@ } else { 0 }
    }
    pub fn new() -> (result: Self)
        ensures result.stocks@.len() == 0, result.days@ == Map::<i32, Daily>::empty(), result.dates@.len() == 0,
            result.shape(),
    {
        let stocks: Vec<StockCell> = Vec::new();
        let days: HashMap<i32, Daily> = HashMap::new();
        let dates: Vec<i32> = Vec::new();
        proof { assert(days@.dom() =~= dates@.to_set()); }
        Self { stocks, days, dates }
    }
    pub fn from_pair(pair: &Pair) -> (work: Self)
        ensures work.stocks@ == pair.stocks@
            && (crate::model::pair_valid(*pair) ==> work.shape()
                && (forall|date: int| #![trigger day_measure(work.days@, date, false)] day_measure(work.days@, date, false) == crate::model::stored_day_count(*pair, date))
                && (forall|date: int| #![trigger day_measure(work.days@, date, true)] day_measure(work.days@, date, true) == crate::model::stored_day_quantity(*pair, date))),
    {
        proof { crate::model::lemma_pair_order(pair); }
        let mut result = Self::new(); let mut index = 0;
        while index < pair.stocks.len()
            invariant index <= pair.stocks.len(), result.stocks@ == pair.stocks@.take(index as int),
                result.days@.is_empty(), result.dates@.len() == 0,
            decreases pair.stocks.len() - index,
        {
            result.stocks.push(StockCell { value: pair.stocks[index].value, count: pair.stocks[index].count.dup() }); index += 1;
        }
        proof { assert(pair.stocks@.take(pair.stocks.len() as int) =~= pair.stocks@); }
        let mut index = 0; let mut count = Wide::zero(); let mut quantity = Wide::zero();
        while index < pair.ship.len()
            invariant index <= pair.ship.len(), result.stocks@ == pair.stocks@,
                count@ == if index == 0 { 0 } else { pair.ship@[index as int - 1].count@ },
                quantity@ == if index == 0 { 0 } else { pair.ship@[index as int - 1].quantity@ },
                result.dates@ == pair.ship@.take(index as int).map_values(|cell: Prefix| cell.date),
                crate::model::pair_valid(*pair) ==> result.dates@.no_duplicates()
                    && result.days@.dom() == result.dates@.to_set()
                    && verus_common::order::sorted_by(pair.ship@, |left: Prefix, right: Prefix| left.date < right.date),
                crate::model::pair_valid(*pair) ==> forall|date: i32| #![trigger result.days@[date]] result.days@.contains_key(date) ==> result.days@[date].date == date,
                crate::model::pair_valid(*pair) ==> forall|date: int| #![trigger day_measure(result.days@, date, false)]
                    day_measure(result.days@, date, false) == ship_measure(pair.ship@.take(index as int), date, false),
                crate::model::pair_valid(*pair) ==> forall|date: int| #![trigger day_measure(result.days@, date, true)]
                    day_measure(result.days@, date, true) == ship_measure(pair.ship@.take(index as int), date, true),
            decreases pair.ship.len() - index,
        {
            let cell = &pair.ship[index];
            let ghost cell_value = *cell;
            let daily_count = cell.count.sub(&count);
            let daily_quantity = cell.quantity.sub(&quantity);
            let day = Daily { date: cell.date, count: daily_count, quantity: daily_quantity };
            let ghost previous = result.days@;
            proof {
                if crate::model::pair_valid(*pair) {
                    assert(!result.dates@.contains(cell.date)) by {
                        if result.dates@.contains(cell.date) {
                            let offset = choose|offset: int| #![trigger result.dates@[offset]] 0 <= offset < result.dates@.len() && result.dates@[offset] == cell.date;
                            assert(0 <= offset < result.dates@.len() && result.dates@[offset] == cell.date);
                            assert(pair.ship@[offset].date < cell.date);
                        }
                    }
                    result.dates@.to_set_ensures();
                    assert(!previous.contains_key(cell.date));
                    verus_prooflib::lemma_unique_push(result.dates@, cell.date);
                    verus_prooflib::lemma_push_to_set(result.dates@, cell.date);
                    lemma_day_insert(previous, day, false);
                    lemma_day_insert(previous, day, true);
                }
            }
            result.days.insert(cell.date, day);
            result.dates.push(cell.date); count = cell.count.dup(); quantity = cell.quantity.dup(); index += 1;
            proof {
                assert(pair.ship@.take(index as int) =~= pair.ship@.take(index as int - 1).push(cell_value));
                assert(result.dates@ =~= pair.ship@.take(index as int).map_values(|cell: Prefix| cell.date));
                if crate::model::pair_valid(*pair) {
                    assert(result.days@.dom() =~= result.dates@.to_set());
                    assert forall|date: int| #![trigger day_measure(result.days@, date, false)] day_measure(result.days@, date, false)
                        == ship_measure(pair.ship@.take(index as int), date, false) by {
                        assert(day_measure(previous, date, false) == ship_measure(pair.ship@.take(index as int - 1), date, false));
                        lemma_ship_push(pair.ship@.take(index as int - 1), cell_value, date, false);
                    }
                    assert forall|date: int| #![trigger day_measure(result.days@, date, true)] day_measure(result.days@, date, true)
                        == ship_measure(pair.ship@.take(index as int), date, true) by {
                        assert(day_measure(previous, date, true) == ship_measure(pair.ship@.take(index as int - 1), date, true));
                        lemma_ship_push(pair.ship@.take(index as int - 1), cell_value, date, true);
                    }
                }
            }
        }
        proof { crate::model::lemma_pair_order(pair); assert(pair.ship@.take(pair.ship.len() as int) =~= pair.ship@); }
        result
    }
    pub fn stock(&mut self, value: i64, sign: i128)
        ensures final(self).days@ == old(self).days@, final(self).dates@ == old(self).dates@,
            old(self).shape() ==> final(self).shape(),
            forall|probe: int| #[trigger] final(self).stock_count(probe)
                == old(self).stock_count(probe) + if probe == value { sign as int } else { 0 },
    {
        let ghost before = self.stocks@;
        let mut index = 0;
        while index < self.stocks.len() && self.stocks[index].value < value
            invariant index <= self.stocks.len(),
                self.stocks@ == before,
                forall|offset: int| #![trigger before[offset]] 0 <= offset < index ==> before[offset].value < value,
            decreases self.stocks.len() - index,
        {
            index += 1;
        }
        if index < self.stocks.len() && self.stocks[index].value == value {
            let count = self.stocks[index].count.add(&Wide::from_i128(sign));
            let cell = StockCell { value, count };
            self.stocks.set(index, cell);
            proof {
                assert forall|probe: int| #[trigger] self.stock_count(probe)
                    == sum(before, |cell: StockCell| if cell.value == probe { cell.count@ } else { 0 })
                        + if probe == value { sign as int } else { 0 } by {
                    lemma_sum_update(before, index as int, self.stocks@[index as int], |cell: StockCell| if cell.value == probe { cell.count@ } else { 0 });
                }
                if old(self).shape() {
                    assert forall|left: int, right: int| 0 <= left < right < self.stocks@.len() implies
                        (#[trigger] self.stocks@[left]).value < (#[trigger] self.stocks@[right]).value by {
                        assert(self.stocks@[left].value == before[left].value);
                        assert(self.stocks@[right].value == before[right].value);
                    }
                }
            }
        } else {
            let cell = StockCell { value, count: Wide::from_i128(sign) };
            proof {
                if old(self).shape() {
                    assert forall|offset: int| #![trigger before[offset]] index <= offset < before.len() implies value < before[offset].value by {
                        if index < offset { assert(before[index as int].value < before[offset].value); }
                    }
                    verus_prooflib::lemma_sorted_insert_at(before, index as int, cell, |left: StockCell, right: StockCell| left.value < right.value);
                }
            }
            self.stocks.insert(index, cell);
            proof {
                assert forall|probe: int| #[trigger] self.stock_count(probe)
                    == sum(before, |cell: StockCell| if cell.value == probe { cell.count@ } else { 0 })
                        + if probe == value { sign as int } else { 0 } by {
                    lemma_sum_insert(before, index as int, self.stocks@[index as int], |cell: StockCell| if cell.value == probe { cell.count@ } else { 0 });
                }
            }
        }
    }
    pub fn shipment(&mut self, date: i32, quantity: i64, sign: i128)
        ensures final(self).stocks@ == old(self).stocks@,
            old(self).shape() ==> final(self).shape(),
            forall|probe: i32| #[trigger] final(self).day_count(probe)
                == old(self).day_count(probe) + if probe == date { sign as int } else { 0 },
            forall|probe: i32| #[trigger] final(self).day_quantity(probe)
                == old(self).day_quantity(probe) + if probe == date { (quantity as int) * (sign as int) } else { 0 },
            forall|probe: int| #![trigger day_measure(final(self).days@, probe, false)]
                day_measure(final(self).days@, probe, false) == day_measure(old(self).days@, probe, false)
                    + if probe == date { sign as int } else { 0 },
            forall|probe: int| #![trigger day_measure(final(self).days@, probe, true)]
                day_measure(final(self).days@, probe, true) == day_measure(old(self).days@, probe, true)
                    + if probe == date { (quantity as int) * (sign as int) } else { 0 },
    {
        let ghost before = self.days@;
        let mut day = match self.days.remove(&date) {
            Some(value) => value,
            None => {
                proof {
                    if old(self).shape() {
                        old(self).dates@.to_set_ensures();
                        verus_prooflib::lemma_unique_push(self.dates@, date);
                        verus_prooflib::lemma_push_to_set(self.dates@, date);
                    }
                }
                self.dates.push(date); Daily { date, count: Wide::zero(), quantity: Wide::zero() }
            },
        };
        proof { if old(self).shape() { assert(day.date == date); } }
        day.count = day.count.add(&Wide::from_i128(sign));
        let change = Wide::from_i128(quantity as i128).mul(&Wide::from_i128(sign));
        day.quantity = day.quantity.add(&change);
        let ghost updated = day;
        self.days.insert(date, day);
        proof {
            assert(self.days@ == before.insert(date, updated));
            if old(self).shape() {
                if before.contains_key(date) {
                    assert(self.dates@ == old(self).dates@);
                    assert(before.dom().insert(date) == before.dom());
                } else {
                    assert(self.dates@ == old(self).dates@.push(date));
                    verus_prooflib::lemma_push_to_set(old(self).dates@, date);
                }
                assert(self.days@.dom() =~= self.dates@.to_set());
            }
            assert forall|probe: i32| #[trigger] self.day_count(probe)
                == (if before.contains_key(probe) { before[probe].count@ } else { 0 })
                    + if probe == date { sign as int } else { 0 } by {}
            assert forall|probe: i32| #[trigger] self.day_quantity(probe)
                == (if before.contains_key(probe) { before[probe].quantity@ } else { 0 })
                    + if probe == date { (quantity as int) * (sign as int) } else { 0 } by {}
        }
    }
    #[verifier::rlimit(100)]
    pub fn finish(self) -> (result: Option<Pair>)
        ensures result matches Some(pair) ==>
            (forall|value: int| #![trigger crate::model::stored_stock(pair, value)] crate::model::stored_stock(pair, value) == self.stock_count(value))
            && (self.shape() ==> crate::model::pair_valid(pair)
                && (forall|date: int| #![trigger crate::model::stored_day_count(pair, date)]
                    crate::model::stored_day_count(pair, date) == day_measure(self.days@, date, false))
                && (forall|date: int| #![trigger crate::model::stored_day_quantity(pair, date)]
                    crate::model::stored_day_quantity(pair, date) == day_measure(self.days@, date, true))),
        self.finish_ready() ==> result.is_some(),
    {
        let mut work = self;
        let mut stocks: Vec<StockCell> = Vec::with_capacity(work.stocks.len()); let mut index = 0;
        while index < work.stocks.len()
            invariant index <= work.stocks.len(), work.stocks@ == self.stocks@,
                work.days@ == self.days@, work.dates@ == self.dates@,
                forall|value: int| #![trigger stock_measure(stocks@, value)]
                    stock_measure(stocks@, value) == stock_measure(self.stocks@.take(index as int), value),
                forall|offset: int| #![trigger stocks@[offset]] 0 <= offset < stocks@.len() ==> stocks@[offset].count@ > 0
                    && self.stocks@.take(index as int).contains(stocks@[offset]),
                self.shape() ==> verus_common::order::sorted_by(stocks@, |left: StockCell, right: StockCell| left.value < right.value),
            decreases work.stocks.len() - index,
        {
            let cell_ref = &work.stocks[index];
            let ghost cell = *cell_ref;
            let cell_copy = StockCell { value: cell_ref.value, count: cell_ref.count.dup() };
            let ghost previous = stocks@;
            proof {
                if self.finish_ready() {
                    lemma_stock_cell_count(self.stocks@, index as int);
                    assert(cell == self.stocks@[index as int]);
                    assert(self.stock_count(cell.value as int) == stock_measure(self.stocks@, cell.value as int));
                    assert(cell.count@ >= 0);
                }
            }
            if cell_copy.count.is_neg() { proof { assert(!self.finish_ready()); } return None; }
            if cell_copy.count.is_pos() {
                proof {
                    if self.shape() {
                        assert forall|offset: int| #![trigger stocks@[offset]] 0 <= offset < stocks@.len() implies stocks@[offset].value < cell.value by {
                            let source = choose|source: int| #![trigger self.stocks@[source]] 0 <= source < index && self.stocks@[source] == stocks@[offset];
                            assert(0 <= source < index && self.stocks@[source] == stocks@[offset]);
                            assert(self.stocks@[source].value < self.stocks@[index as int].value);
                        }
                        verus_prooflib::lemma_sorted_insert_at(stocks@, stocks@.len() as int, cell, |left: StockCell, right: StockCell| left.value < right.value);
                        assert(stocks@.insert(stocks@.len() as int, cell) =~= stocks@.push(cell));
                    }
                }
                stocks.push(cell_copy);
            }
            proof {
                assert(self.stocks@.take(index as int + 1) =~= self.stocks@.take(index as int).push(cell));
                verus_prooflib::lemma_push_membership(self.stocks@.take(index as int), cell);
                assert forall|value: int| #![trigger stock_measure(stocks@, value)]
                    stock_measure(stocks@, value) == stock_measure(self.stocks@.take(index as int + 1), value) by {
                    let weight = |cell: StockCell| if cell.value == value { cell.count@ } else { 0 };
                    assert(stock_measure(previous, value) == stock_measure(self.stocks@.take(index as int), value));
                    verus_common::sums::lemma_sum_take(self.stocks@, index as int, weight);
                    if cell.count@ > 0 { verus_common::sums::lemma_sum_push(previous, cell, weight); }
                }
            }
            index += 1;
        }
        proof { assert(self.stocks@.take(self.stocks@.len() as int) =~= self.stocks@); }
        let sorted = sort_dates(&work.dates);
        let mut ship: Vec<Prefix> = Vec::with_capacity(sorted.len()); let mut count = Wide::zero(); let mut quantity = Wide::zero(); let mut index = 0;
        while index < sorted.len()
            invariant index <= sorted.len(),
                forall|value: int| #![trigger stock_measure(stocks@, value)] stock_measure(stocks@, value) == self.stock_count(value),
                forall|offset: int| #![trigger stocks@[offset]] 0 <= offset < stocks@.len() ==> stocks@[offset].count@ > 0,
                self.shape() ==> verus_common::order::sorted_by(stocks@, |left: StockCell, right: StockCell| left.value < right.value)
                    && sorted@.no_duplicates() && sorted@.to_set() == self.days@.dom()
                    && verus_common::order::sorted_by(sorted@, |left: i32, right: i32| left < right),
                self.shape() ==> forall|date: i32| #![trigger work.days@.contains_key(date)] !sorted@.take(index as int).contains(date) ==>
                    work.days@.contains_key(date) == self.days@.contains_key(date)
                        && (work.days@.contains_key(date) ==> work.days@[date] == self.days@[date]),
                self.shape() ==> forall|date: int| #![trigger ship_measure(ship@, date, false)]
                    ship_measure(ship@, date, false) == sum(sorted@.take(index as int), day_weight(self.days@, date, false)),
                self.shape() ==> forall|date: int| #![trigger ship_measure(ship@, date, true)]
                    ship_measure(ship@, date, true) == sum(sorted@.take(index as int), day_weight(self.days@, date, true)),
                count@ == if ship@.len() == 0 { 0 } else { ship@.last().count@ },
                quantity@ == if ship@.len() == 0 { 0 } else { ship@.last().quantity@ },
                self.shape() ==> forall|offset: int| #![trigger ship@[offset]] 0 <= offset < ship@.len() ==>
                    ship@[offset].count@ > if offset == 0 { 0 } else { ship@[offset - 1].count@ }
                        && sorted@.take(index as int).contains(ship@[offset].date),
                self.shape() ==> verus_common::order::sorted_by(ship@, |left: Prefix, right: Prefix| left.date < right.date),
            decreases sorted.len() - index,
        {
            let ghost previous_ship = ship@;
            let ghost previous_days = work.days@;
            proof {
                if self.shape() {
                    verus_prooflib::lemma_prefix_fresh(sorted@, index as int);
                    assert(previous_days.contains_key(sorted@[index as int]));
                }
            }
            let day = match work.days.remove(&sorted[index]) { Some(value) => value, None => { proof { assert(!self.finish_ready()); } return None; } };
            proof {
                if self.shape() {
                    assert(previous_days[sorted@[index as int]] == self.days@[sorted@[index as int]]);
                    assert(day == self.days@[sorted@[index as int]]);
                    assert(day.date == sorted@[index as int]);
                }
            }
            proof { if self.finish_ready() { assert(day_measure(self.days@, day.date as int, false) == day.count@); assert(day_measure(self.days@, day.date as int, true) == day.quantity@); } }
            if day.count.is_neg() || (day.count.is_zero() && !day.quantity.is_zero()) { proof { assert(!self.finish_ready()); } return None; }
            if day.count.is_pos() {
                count = count.add(&day.count);
                quantity = quantity.add(&day.quantity);
                let cell = Prefix { date: day.date, count: count.dup(), quantity: quantity.dup() };
                proof {
                    if self.shape() {
                        assert forall|offset: int| #![trigger ship@[offset]] 0 <= offset < ship@.len() implies ship@[offset].date < day.date by {
                            let source = choose|source: int| #![trigger sorted@[source]] 0 <= source < index && sorted@[source] == ship@[offset].date;
                            assert(0 <= source < index && sorted@[source] == ship@[offset].date);
                            assert(sorted@[source] < sorted@[index as int]);
                        }
                        verus_prooflib::lemma_sorted_insert_at(ship@, ship@.len() as int, cell, |left: Prefix, right: Prefix| left.date < right.date);
                        assert(ship@.insert(ship@.len() as int, cell) =~= ship@.push(cell));
                    }
                }
                ship.push(cell);
            }
            proof {
                if self.shape() {
                    assert(sorted@.take(index as int + 1) =~= sorted@.take(index as int).push(day.date));
                    verus_prooflib::lemma_push_membership(sorted@.take(index as int), day.date);
                    assert forall|date: i32| #![trigger work.days@.contains_key(date)] !sorted@.take(index as int + 1).contains(date) implies
                        work.days@.contains_key(date) == self.days@.contains_key(date)
                            && (work.days@.contains_key(date) ==> work.days@[date] == self.days@[date]) by {
                        assert(date != day.date);
                        assert(!sorted@.take(index as int).contains(date));
                        assert(previous_days.contains_key(date) == self.days@.contains_key(date));
                        if previous_days.contains_key(date) { assert(previous_days[date] == self.days@[date]); }
                    }
                    assert forall|date: int| #![trigger ship_measure(ship@, date, false)] ship_measure(ship@, date, false)
                        == sum(sorted@.take(index as int + 1), day_weight(self.days@, date, false)) by {
                        verus_common::sums::lemma_sum_take(sorted@, index as int, day_weight(self.days@, date, false));
                        assert(ship_measure(previous_ship, date, false) == sum(sorted@.take(index as int), day_weight(self.days@, date, false)));
                        if day.count@ > 0 { lemma_ship_push(previous_ship, ship@.last(), date, false); }
                    }
                    assert forall|date: int| #![trigger ship_measure(ship@, date, true)] ship_measure(ship@, date, true)
                        == sum(sorted@.take(index as int + 1), day_weight(self.days@, date, true)) by {
                        verus_common::sums::lemma_sum_take(sorted@, index as int, day_weight(self.days@, date, true));
                        assert(ship_measure(previous_ship, date, true) == sum(sorted@.take(index as int), day_weight(self.days@, date, true)));
                        if day.count@ > 0 { lemma_ship_push(previous_ship, ship@.last(), date, true); }
                    }
                }
            }
            index += 1;
        }
        proof {
            if self.shape() {
                assert(sorted@.take(sorted.len() as int) =~= sorted@);
                assert forall|date: int| #![trigger ship_measure(ship@, date, false)] ship_measure(ship@, date, false) == day_measure(self.days@, date, false) by {
                    lemma_day_sum(self.days@, sorted@, date, false);
                }
                assert forall|date: int| #![trigger ship_measure(ship@, date, true)] ship_measure(ship@, date, true) == day_measure(self.days@, date, true) by {
                    lemma_day_sum(self.days@, sorted@, date, true);
                }
                reveal(crate::model::pair_valid);
            }
        }
        Some(Pair { stocks, ship })
    }
}
impl PartWork {
    pub open spec fn stock_for(self, key: i64, value: int) -> int {
        if self.pairs@.contains_key(key) { self.pairs@[key].stock_count(value) }
        else if self.original@.contains_key(key) { stock_measure(self.original@[key].stocks@, value) }
        else { 0 }
    }
    pub open spec fn day_for(self, key: i64, date: int, quantity: bool) -> int {
        if self.pairs@.contains_key(key) { day_measure(self.pairs@[key].days@, date, quantity) }
        else if self.original@.contains_key(key) { ship_measure(self.original@[key].ship@, date, quantity) }
        else { 0 }
    }
    pub open spec fn cells_valid(self) -> bool {
        &&& self.pairs@.dom().disjoint(self.original@.dom())
        &&& forall|key: i64| #![trigger self.pairs@[key]] self.pairs@.contains_key(key) ==> self.pairs@[key].shape()
        &&& forall|key: i64| #![trigger self.original@[key]] self.original@.contains_key(key) ==> crate::model::pair_valid(self.original@[key])
            && (self.original@[key].stocks@.len() > 0 || self.original@[key].ship@.len() > 0)
    }
    pub open spec fn shape(self) -> bool {
        self.keys@.no_duplicates() && self.cells_valid()
            && self.keys@.to_set() == self.pairs@.dom().union(self.original@.dom())
    }
    pub open spec fn pending(self, key: i64) -> bool {
        self.keys@.no_duplicates() && self.cells_valid()
            && self.keys@.to_set() == self.pairs@.dom().union(self.original@.dom()).insert(key)
            && !self.pairs@.contains_key(key) && !self.original@.contains_key(key)
    }
    /// What `finish` needs to succeed: a well-shaped work part of real, non-negative rows.
    pub open spec fn finish_ready(self) -> bool {
        &&& self.shape()
        &&& self.count@ > 0
        &&& forall|key: i64| #![trigger self.pairs@[key]] self.pairs@.contains_key(key) ==> self.pairs@[key].finish_ready()
    }
    pub fn new(count: Wide) -> (result: Self)
        ensures result.count == count, result.pairs@ == Map::<i64, PairWork>::empty(),
            result.original@ == Map::<i64, Pair>::empty(), result.keys@.len() == 0,
            result.shape(),
    {
        let result = Self { count, pairs: HashMap::new(), original: HashMap::new(), keys: Vec::new() };
        proof { assert(result.keys@.to_set() =~= result.pairs@.dom().union(result.original@.dom())); }
        result
    }
    pub fn from_part(part: &Part) -> (work: Self)
        ensures work.count == part.count
            && (part_shape(*part) ==> work.shape()
                && (forall|key: i64, value: int| #![trigger work.stock_for(key, value)] work.stock_for(key, value) == part_stock(part.pairs@, key, value))
                && (forall|key: i64, date: int, quantity: bool| #![trigger work.day_for(key, date, quantity)] work.day_for(key, date, quantity) == part_day(part.pairs@, key, date, quantity))),
    {
        let mut result = Self::new(part.count.dup()); let mut index = 0;
        while index < part.pairs.len()
            invariant result.count == part.count, index <= part.pairs.len(), result.pairs@.is_empty(),
                result.keys@ == part.pairs@.take(index as int).map_values(|entry: (i64, Pair)| entry.0),
                part_shape(*part) ==> result.keys@.no_duplicates() && result.original@.dom() == result.keys@.to_set(),
                part_shape(*part) ==> forall|offset: int| #![trigger part.pairs@[offset]] 0 <= offset < index ==>
                    result.original@.contains_key(part.pairs@[offset].0) && crate::model::same_pair(result.original@[part.pairs@[offset].0], part.pairs@[offset].1),
            decreases part.pairs.len() - index,
        {
            proof {
                if part_shape(*part) {
                    let source_keys = part.pairs@.map_values(|entry: (i64, Pair)| entry.0);
                    verus_prooflib::lemma_prefix_fresh(source_keys, index as int);
                    assert(result.keys@ =~= source_keys.take(index as int));
                    verus_prooflib::lemma_unique_push(result.keys@, part.pairs@[index as int].0);
                    verus_prooflib::lemma_push_to_set(result.keys@, part.pairs@[index as int].0);
                }
            }
            result.keys.push(part.pairs[index].0); result.original.insert(part.pairs[index].0, copy_pair(&part.pairs[index].1)); index += 1;
            proof { assert(result.keys@ =~= part.pairs@.take(index as int).map_values(|entry: (i64, Pair)| entry.0)); }
        }
        proof {
            assert(part.pairs@.take(part.pairs.len() as int) =~= part.pairs@);
            if part_shape(*part) {
                result.keys@.to_set_ensures();
                assert forall|key: i64| #![trigger result.original@[key]] result.original@.contains_key(key) implies
                    crate::model::pair_valid(result.original@[key])
                        && (result.original@[key].stocks@.len() > 0 || result.original@[key].ship@.len() > 0) by {
                    assert(result.keys@.contains(key));
                    let offset = choose|offset: int| #![trigger result.keys@[offset]] 0 <= offset < result.keys@.len() && result.keys@[offset] == key;
                    assert(0 <= offset < result.keys@.len() && result.keys@[offset] == key);
                    assert(part.pairs@[offset].0 == key);
                    assert(crate::model::same_pair(result.original@[key], part.pairs@[offset].1)); crate::model::lemma_same_pair(result.original@[key], part.pairs@[offset].1);
                }
                assert(result.pairs@.dom() =~= Set::<i64>::empty());
                assert(result.pairs@.dom().disjoint(result.original@.dom()));
                assert(result.pairs@.dom().union(result.original@.dom()) =~= result.original@.dom());
                assert(result.shape());
                assert forall|key: i64, value: int| #![trigger result.stock_for(key, value)] result.stock_for(key, value) == part_stock(part.pairs@, key, value) by {
                    verus_prooflib::lemma_unique_key_lookup(part.pairs@, key);
                    if result.original@.contains_key(key) {
                        let offset = choose|offset: int| #![trigger part.pairs@[offset]] 0 <= offset < part.pairs@.len() && part.pairs@[offset].0 == key;
                        assert(0 <= offset < part.pairs@.len() && part.pairs@[offset].0 == key);
                        assert(crate::model::same_pair(result.original@[key], part.pairs@[offset].1)); crate::model::lemma_same_pair(result.original@[key], part.pairs@[offset].1);
                    }
                }
                assert forall|key: i64, date: int, quantity: bool| #![trigger result.day_for(key, date, quantity)] result.day_for(key, date, quantity) == part_day(part.pairs@, key, date, quantity) by {
                    verus_prooflib::lemma_unique_key_lookup(part.pairs@, key);
                    if result.original@.contains_key(key) {
                        let offset = choose|offset: int| #![trigger part.pairs@[offset]] 0 <= offset < part.pairs@.len() && part.pairs@[offset].0 == key;
                        assert(0 <= offset < part.pairs@.len() && part.pairs@[offset].0 == key);
                        assert(crate::model::same_pair(result.original@[key], part.pairs@[offset].1)); crate::model::lemma_same_pair(result.original@[key], part.pairs@[offset].1);
                    }
                }
            }
        }
        result
    }
    pub fn take_pair(&mut self, key: i64) -> (result: PairWork)
        ensures final(self).count == old(self).count,
            final(self).pairs@ == old(self).pairs@.remove(key),
            old(self).original@.is_empty() ==> final(self).original@.is_empty(),
            old(self).original@.is_empty() ==> (
                if old(self).pairs@.contains_key(key) { result == old(self).pairs@[key] }
                else { result.stocks@.len() == 0 && result.days@.is_empty() }),
            old(self).shape() ==> final(self).pending(key),
            old(self).shape() ==> (result.shape()
                && (forall|value: int| #![trigger result.stock_count(value)] result.stock_count(value) == old(self).stock_for(key, value))
                && (forall|date: int| #![trigger day_measure(result.days@, date, false)] day_measure(result.days@, date, false) == old(self).day_for(key, date, false))
                && (forall|date: int| #![trigger day_measure(result.days@, date, true)] day_measure(result.days@, date, true) == old(self).day_for(key, date, true))),
            forall|other: i64, value: int| #![trigger final(self).stock_for(other, value)] other != key ==>
                final(self).stock_for(other, value) == old(self).stock_for(other, value),
            forall|other: i64, date: int, quantity: bool| #![trigger final(self).day_for(other, date, quantity)] other != key ==>
                final(self).day_for(other, date, quantity) == old(self).day_for(other, date, quantity),
    {
        let result = match self.pairs.remove(&key) {
            Some(value) => value,
            None => match self.original.remove(&key) {
                Some(value) => PairWork::from_pair(&value),
                None => {
                    proof {
                        if old(self).shape() {
                            old(self).keys@.to_set_ensures();
                            assert(!old(self).pairs@.contains_key(key) && !old(self).original@.contains_key(key));
                            assert(!old(self).keys@.to_set().contains(key));
                            assert(!self.keys@.contains(key));
                            verus_prooflib::lemma_unique_push(self.keys@, key);
                            verus_prooflib::lemma_push_to_set(self.keys@, key);
                        }
                    }
                    self.keys.push(key); PairWork::new()
                },
            },
        };
        proof {
            if old(self).shape() {
                old(self).keys@.to_set_ensures();
                self.keys@.to_set_ensures();
                assert(!self.pairs@.contains_key(key));
                assert(!self.original@.contains_key(key));
                assert(self.keys@.to_set() =~= self.pairs@.dom().union(self.original@.dom()).insert(key));
            }
        }
        result
    }
    pub fn install_pair(&mut self, key: i64, pair: PairWork)
        ensures final(self).count == old(self).count, final(self).keys@ == old(self).keys@,
            final(self).original@ == old(self).original@, final(self).pairs@ == old(self).pairs@.insert(key, pair),
            old(self).pending(key) && pair.shape() ==> final(self).shape(),
            forall|other: i64, value: int| #![trigger final(self).stock_for(other, value)] final(self).stock_for(other, value)
                == if other == key { pair.stock_count(value) } else { old(self).stock_for(other, value) },
            forall|other: i64, date: int, quantity: bool| #![trigger final(self).day_for(other, date, quantity)] final(self).day_for(other, date, quantity)
                == if other == key { day_measure(pair.days@, date, quantity) } else { old(self).day_for(other, date, quantity) },
    {
        self.pairs.insert(key, pair);
        proof {
            if old(self).pending(key) && pair.shape() {
                assert(self.pairs@.dom().disjoint(self.original@.dom()));
                assert(self.pairs@.dom().union(self.original@.dom()) =~= self.keys@.to_set());
            }
        }
    }
    pub fn finish(self) -> (result: Option<Part>)
        ensures result matches Some(part) ==> part.count == self.count && part.count@ > 0,
            self.shape() ==> (result matches Some(part) ==> part_shape(part)
                && (forall|key: i64, value: int| #![trigger part_stock(part.pairs@, key, value)] part_stock(part.pairs@, key, value) == self.stock_for(key, value))
                && (forall|key: i64, date: int, quantity: bool| #![trigger part_day(part.pairs@, key, date, quantity)] part_day(part.pairs@, key, date, quantity) == self.day_for(key, date, quantity))),
        self.finish_ready() ==> result is Some,
    {
        let mut work = self;
        if !work.count.is_pos() { return None; }
        let mut pairs: Vec<(i64, Pair)> = Vec::new(); let mut index = 0;
        while index < work.keys.len()
            invariant work.count == self.count, work.count@ > 0, index <= work.keys.len(), work.keys@ == self.keys@,
                self.shape() ==> work.cells_valid() && self.keys@.no_duplicates()
                    && pairs@.map_values(|entry: (i64, Pair)| entry.0).no_duplicates(),
                self.shape() ==> forall|other: i64| #![trigger work.pairs@.contains_key(other)] !self.keys@.take(index as int).contains(other) ==>
                    work.pairs@.contains_key(other) == self.pairs@.contains_key(other)
                        && (work.pairs@.contains_key(other) ==> work.pairs@[other] == self.pairs@[other])
                        && work.original@.contains_key(other) == self.original@.contains_key(other),
                self.shape() ==> forall|offset: int| #![trigger pairs@[offset]] 0 <= offset < pairs@.len() ==>
                    self.keys@.take(index as int).contains(pairs@[offset].0)
                        && crate::model::pair_valid(pairs@[offset].1)
                        && (pairs@[offset].1.stocks@.len() > 0 || pairs@[offset].1.ship@.len() > 0),
                self.shape() ==> forall|key: i64, value: int| #![trigger work.stock_for(key, value)] work.stock_for(key, value)
                    == if self.keys@.take(index as int).contains(key) { 0 } else { self.stock_for(key, value) },
                self.shape() ==> forall|key: i64, date: int, quantity: bool| #![trigger work.day_for(key, date, quantity)] work.day_for(key, date, quantity)
                    == if self.keys@.take(index as int).contains(key) { 0 } else { self.day_for(key, date, quantity) },
                self.shape() ==> forall|key: i64, value: int| #![trigger part_stock(pairs@, key, value)] part_stock(pairs@, key, value)
                    == if self.keys@.take(index as int).contains(key) { self.stock_for(key, value) } else { 0 },
                self.shape() ==> forall|key: i64, date: int, quantity: bool| #![trigger part_day(pairs@, key, date, quantity)] part_day(pairs@, key, date, quantity)
                    == if self.keys@.take(index as int).contains(key) { self.day_for(key, date, quantity) } else { 0 },
            decreases work.keys.len() - index,
        {
            let key = work.keys[index];
            let ghost previous_work = work;
            let ghost previous_pairs = pairs@;
            proof {
                if self.shape() { verus_prooflib::lemma_prefix_fresh(self.keys@, index as int); }
            }
            proof {
                if self.shape() {
                    self.keys@.to_set_ensures();
                    assert(self.keys@.contains(key));
                    assert(!self.keys@.take(index as int).contains(key));
                }
            }
            let candidate = match work.pairs.remove(&key) {
                Some(raw) => {
                    let ghost source = raw;
                    let pair = match raw.finish() { Some(value) => value, None => {
                        proof { if self.shape() { assert(source == self.pairs@[key]); assert(self.pairs@.contains_key(key)); } assert(!self.finish_ready()); }
                        return None;
                    } };
                    proof {
                        if self.shape() {
                            assert(!work.original@.contains_key(key));
                            assert(source.shape());
                            assert forall|value: int| #![trigger stock_measure(pair.stocks@, value)] stock_measure(pair.stocks@, value) == previous_work.stock_for(key, value) by {
                                assert(crate::model::stored_stock(pair, value) == source.stock_count(value));
                            }
                            assert forall|date: int, quantity: bool| #![trigger ship_measure(pair.ship@, date, quantity)] ship_measure(pair.ship@, date, quantity) == previous_work.day_for(key, date, quantity) by {
                                if quantity { assert(crate::model::stored_day_quantity(pair, date) == day_measure(source.days@, date, true)); }
                                else { assert(crate::model::stored_day_count(pair, date) == day_measure(source.days@, date, false)); }
                            }
                        }
                    }
                    if pair.stocks.len() > 0 || pair.ship.len() > 0 { Some(pair) }
                    else {
                        proof {
                            if self.shape() {
                                assert forall|value: int| #![trigger previous_work.stock_for(key, value)] previous_work.stock_for(key, value) == 0 by {
                                    assert(stock_measure(pair.stocks@, value) == 0);
                                }
                                assert forall|date: int, quantity: bool| #![trigger previous_work.day_for(key, date, quantity)] previous_work.day_for(key, date, quantity) == 0 by {
                                    assert(ship_measure(pair.ship@, date, quantity) == 0);
                                }
                            }
                        }
                        None
                    }
                },
                None => match work.original.remove(&key) { Some(pair) => Some(pair), None => {
                    proof {
                        if self.finish_ready() {
                            self.keys@.to_set_ensures();
                            assert(self.keys@.contains(key));
                            assert(self.keys@.to_set().contains(key));
                            assert(self.pairs@.dom().union(self.original@.dom()).contains(key));
                            assert(self.pairs@.dom().contains(key) || self.original@.dom().contains(key));
                            assert(!previous_work.pairs@.contains_key(key) && !previous_work.original@.contains_key(key));
                            assert(false);
                        }
                    }
                    return None;
                } },
            };
            if let Some(pair) = candidate {
                let ghost retained = pair;
                proof {
                    if self.shape() {
                        assert(crate::model::pair_valid(pair));
                        assert(pair.stocks@.len() > 0 || pair.ship@.len() > 0);
                        assert(!pairs@.map_values(|entry: (i64, Pair)| entry.0).contains(key)) by {
                            let keys = pairs@.map_values(|entry: (i64, Pair)| entry.0);
                            if keys.contains(key) {
                                let offset = choose|offset: int| #![trigger keys[offset]] 0 <= offset < keys.len() && keys[offset] == key;
                                assert(0 <= offset < keys.len() && keys[offset] == key);
                                assert(self.keys@.take(index as int).contains(pairs@[offset].0));
                            }
                        }
                        lemma_pair_push(pairs@, key, retained);
                        assert forall|value: int| #![trigger stock_measure(pair.stocks@, value)] stock_measure(pair.stocks@, value) == previous_work.stock_for(key, value) by {}
                        assert forall|date: int, quantity: bool| #![trigger ship_measure(pair.ship@, date, quantity)] ship_measure(pair.ship@, date, quantity) == previous_work.day_for(key, date, quantity) by {}
                    }
                }
                pairs.push((key, pair));
            }
            proof {
                if self.shape() {
                    assert(self.keys@.take(index as int + 1) =~= self.keys@.take(index as int).push(key));
                    verus_prooflib::lemma_push_membership(self.keys@.take(index as int), key);
                    assert forall|other: i64| #![trigger work.pairs@.contains_key(other)] !self.keys@.take(index as int + 1).contains(other) implies
                        work.pairs@.contains_key(other) == self.pairs@.contains_key(other)
                            && (work.pairs@.contains_key(other) ==> work.pairs@[other] == self.pairs@[other])
                            && work.original@.contains_key(other) == self.original@.contains_key(other) by {
                        assert(other != key);
                        assert(!self.keys@.take(index as int).contains(other));
                    }
                    assert forall|other: i64, value: int| #![trigger work.stock_for(other, value)] work.stock_for(other, value)
                        == if self.keys@.take(index as int + 1).contains(other) { 0 } else { self.stock_for(other, value) } by {
                        assert(previous_work.stock_for(other, value) == if self.keys@.take(index as int).contains(other) { 0 } else { self.stock_for(other, value) });
                    }
                    assert forall|other: i64, date: int, quantity: bool| #![trigger work.day_for(other, date, quantity)] work.day_for(other, date, quantity)
                        == if self.keys@.take(index as int + 1).contains(other) { 0 } else { self.day_for(other, date, quantity) } by {
                        assert(previous_work.day_for(other, date, quantity) == if self.keys@.take(index as int).contains(other) { 0 } else { self.day_for(other, date, quantity) });
                    }
                    assert forall|other: i64, value: int| #![trigger part_stock(pairs@, other, value)] part_stock(pairs@, other, value)
                        == if self.keys@.take(index as int + 1).contains(other) { self.stock_for(other, value) } else { 0 } by {
                        assert(part_stock(previous_pairs, other, value) == if self.keys@.take(index as int).contains(other) { self.stock_for(other, value) } else { 0 });
                        if other == key { assert(previous_work.stock_for(key, value) == self.stock_for(key, value)); }
                    }
                    assert forall|other: i64, date: int, quantity: bool| #![trigger part_day(pairs@, other, date, quantity)] part_day(pairs@, other, date, quantity)
                        == if self.keys@.take(index as int + 1).contains(other) { self.day_for(other, date, quantity) } else { 0 } by {
                        assert(part_day(previous_pairs, other, date, quantity) == if self.keys@.take(index as int).contains(other) { self.day_for(other, date, quantity) } else { 0 });
                        if other == key { assert(previous_work.day_for(key, date, quantity) == self.day_for(key, date, quantity)); }
                    }
                }
            }
            index += 1;
        }
        proof {
            assert(self.keys@.take(self.keys@.len() as int) =~= self.keys@);
            if self.shape() {
                self.keys@.to_set_ensures();
                assert forall|key: i64, value: int| #![trigger part_stock(pairs@, key, value)] part_stock(pairs@, key, value) == self.stock_for(key, value) by {
                    if !self.keys@.contains(key) {
                        assert(!self.keys@.to_set().contains(key));
                        assert(!self.pairs@.contains_key(key) && !self.original@.contains_key(key));
                    }
                }
                assert forall|key: i64, date: int, quantity: bool| #![trigger part_day(pairs@, key, date, quantity)] part_day(pairs@, key, date, quantity) == self.day_for(key, date, quantity) by {
                    if !self.keys@.contains(key) {
                        assert(!self.keys@.to_set().contains(key));
                        assert(!self.pairs@.contains_key(key) && !self.original@.contains_key(key));
                    }
                }
            }
        }
        Some(Part { count: work.count, pairs })
    }
}
pub fn prefix(pair: &Pair, date: i32) -> (result: (Wide, Wide))
    ensures crate::model::pair_valid(*pair) ==> result.0@ == crate::model::stored_before_count(*pair, date as int)
        && result.1@ == crate::model::stored_before_quantity(*pair, date as int),
{
    proof { crate::model::lemma_pair_dates(pair); }
    let mut low = 0; let mut high = pair.ship.len();
    while low < high
        invariant low <= high <= pair.ship.len(),
            crate::model::pair_valid(*pair) ==> forall|index: int| #![trigger pair.ship@[index]]
                0 < index < pair.ship@.len() ==> pair.ship@[index - 1].date < pair.ship@[index].date,
            crate::model::pair_valid(*pair) ==> forall|index: int| #![trigger pair.ship@[index]]
                0 <= index < low ==> pair.ship@[index].date < date,
            crate::model::pair_valid(*pair) ==> forall|index: int| #![trigger pair.ship@[index]]
                high <= index < pair.ship@.len() ==> pair.ship@[index].date >= date,
        decreases high - low,
    {
        let middle = low + (high - low) / 2;
        if pair.ship[middle].date < date {
            proof {
                if crate::model::pair_valid(*pair) {
                    assert forall|index: int| #![trigger pair.ship@[index]] 0 <= index < middle implies pair.ship@[index].date < date by {
                        verus_prooflib::lemma_adjacent_int_order(pair.ship@, |cell: Prefix| cell.date as int, index, middle as int);
                    }
                }
            }
            low = middle + 1;
        } else {
            proof {
                if crate::model::pair_valid(*pair) {
                    assert forall|index: int| #![trigger pair.ship@[index]] middle < index < pair.ship@.len() implies pair.ship@[index].date >= date by {
                        verus_prooflib::lemma_adjacent_int_order(pair.ship@, |cell: Prefix| cell.date as int, middle as int, index);
                    }
                }
            }
            high = middle;
        }
    }
    proof {
        if crate::model::pair_valid(*pair) {
            crate::model::lemma_before_cut(pair, date as int, low as int);
        }
    }
    if low == 0 { (Wide::zero(), Wide::zero()) } else { let cell = &pair.ship[low - 1]; (cell.count.dup(), cell.quantity.dup()) }
}
pub fn copy_pair(source: &Pair) -> (result: Pair)
    ensures result.stocks@ == source.stocks@, result.ship@ == source.ship@,
{
    let mut stocks: Vec<StockCell> = Vec::with_capacity(source.stocks.len()); let mut index = 0;
    while index < source.stocks.len()
        invariant index <= source.stocks.len(), stocks@ == source.stocks@.take(index as int),
        decreases source.stocks.len() - index,
    {
        stocks.push(StockCell { value: source.stocks[index].value, count: source.stocks[index].count.dup() });
        proof { assert(source.stocks@.take(index as int + 1) =~= source.stocks@.take(index as int).push(source.stocks@[index as int])); }
        index += 1;
    }
    proof { assert(source.stocks@.take(source.stocks.len() as int) =~= source.stocks@); }
    let mut ship: Vec<Prefix> = Vec::with_capacity(source.ship.len()); let mut index = 0;
    while index < source.ship.len()
        invariant index <= source.ship.len(), ship@ == source.ship@.take(index as int),
        decreases source.ship.len() - index,
    {
        ship.push(Prefix { date: source.ship[index].date, count: source.ship[index].count.dup(), quantity: source.ship[index].quantity.dup() });
        proof { assert(source.ship@.take(index as int + 1) =~= source.ship@.take(index as int).push(source.ship@[index as int])); }
        index += 1;
    }
    proof { assert(source.ship@.take(source.ship.len() as int) =~= source.ship@); }
    Pair { stocks, ship }
}
pub fn copy_bytes(source: &Vec<u8>) -> (result: Vec<u8>)
    ensures result@ == source@,
{
    let mut result = Vec::with_capacity(source.len()); let mut index = 0;
    while index < source.len()
        invariant index <= source.len(), result@ == source@.take(index as int),
        decreases source.len() - index,
    { result.push(source[index]); index += 1; }
    result
}
pub fn compare(left: &[u8], right: &[u8]) -> (result: i8)
    ensures (result == 0) == (left@ == right@),
        (result < 0) == verus_common::bytes::bytes_lt(left@, right@),
        (result > 0) == verus_common::bytes::bytes_lt(right@, left@),
{
    if verus_common::bytes::bytes_lt_exec(left, right) {
        proof { verus_common::bytes::lemma_bytes_lt_asym(left@, right@); verus_common::bytes::lemma_bytes_lt_irrefl(left@); }
        return -1;
    }
    if verus_common::bytes::bytes_lt_exec(right, left) {
        proof { verus_common::bytes::lemma_bytes_lt_irrefl(left@); }
        return 1;
    }
    proof { if left@ != right@ { verus_common::bytes::lemma_bytes_lt_total(left@, right@); } }
    0
}
pub open spec fn name_hash(bytes: Seq<u8>) -> int
    decreases bytes.len(),
{
    if bytes.len() == 0 { 0 } else { (name_hash(bytes.drop_last()) * 257 + bytes.last() as int) % 2147483647 }
}
pub fn hash(bytes: &[u8]) -> (result: i64)
    ensures result as int == name_hash(bytes@),
{
    let mut value: i64 = 0; let mut index = 0;
    while index < bytes.len()
        invariant index <= bytes.len(), value as int == name_hash(bytes@.take(index as int)), 0 <= value < 2147483647,
        decreases bytes.len() - index,
    {
        proof {
            assert(bytes@.take(index as int + 1).drop_last() =~= bytes@.take(index as int));
            assert(bytes@.take(index as int + 1).last() == bytes@[index as int]);
        }
        value = (value * 257 + bytes[index] as i64) % 2147483647; index += 1;
    }
    proof { assert(bytes@.take(index as int) =~= bytes@); }
    value
}
}
