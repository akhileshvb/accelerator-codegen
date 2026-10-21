use vstd::prelude::*;
use crate::profiles::{Prefix, Daily, StockCell};
use crate::profiles::{Pair, Part};
use std::sync::Arc;
use verus_common::sums::*;

verus! {
pub open spec fn pair_lookup(rows: Seq<(i64, Pair)>, key: i64) -> Option<Pair> {
    match verus_prooflib::unique_key_lookup(rows, key) { Some(pair) => Some(pair), None => None }
}
pub open spec fn part_stock(rows: Seq<(i64, Pair)>, key: i64, value: int) -> int {
    match pair_lookup(rows, key) { Some(pair) => stock_measure(pair.stocks@, value), None => 0 }
}
pub open spec fn part_day(rows: Seq<(i64, Pair)>, key: i64, date: int, quantity: bool) -> int {
    match pair_lookup(rows, key) { Some(pair) => ship_measure(pair.ship@, date, quantity), None => 0 }
}
pub open spec fn part_shape(part: Part) -> bool {
    &&& part.pairs@.map_values(|entry: (i64, Pair)| entry.0).no_duplicates()
    &&& forall|index: int| #![trigger part.pairs@[index]] 0 <= index < part.pairs@.len() ==>
        crate::model::pair_valid(part.pairs@[index].1)
            && (part.pairs@[index].1.stocks@.len() > 0 || part.pairs@[index].1.ship@.len() > 0)
}
pub proof fn lemma_pair_push(rows: Seq<(i64, Pair)>, key: i64, pair: Pair)
    requires rows.map_values(|entry: (i64, Pair)| entry.0).no_duplicates(),
        !rows.map_values(|entry: (i64, Pair)| entry.0).contains(key),
    ensures rows.push((key, pair)).map_values(|entry: (i64, Pair)| entry.0).no_duplicates(),
        pair_lookup(rows.push((key, pair)), key) == Some(pair),
        forall|other: i64| #![trigger pair_lookup(rows.push((key, pair)), other)] other != key ==>
            pair_lookup(rows.push((key, pair)), other) == pair_lookup(rows, other),
{
    let next = rows.push((key, pair));
    let keys = rows.map_values(|entry: (i64, Pair)| entry.0);
    verus_prooflib::lemma_unique_push(keys, key);
    assert(next.map_values(|entry: (i64, Pair)| entry.0) =~= keys.push(key));
    verus_prooflib::lemma_unique_key_lookup(next, key);
    assert(next[rows.len() as int] == (key, pair));
    assert forall|other: i64| #![trigger pair_lookup(next, other)] other != key implies pair_lookup(next, other) == pair_lookup(rows, other) by {
        verus_prooflib::lemma_unique_key_lookup(rows, other);
        verus_prooflib::lemma_unique_key_lookup(next, other);
        if verus_prooflib::unique_key_lookup(rows, other).is_some() {
            let offset = choose|offset: int| #![trigger rows[offset]] 0 <= offset < rows.len() && rows[offset].0 == other;
            assert(0 <= offset < rows.len() && rows[offset].0 == other);
            assert(next[offset] == rows[offset]);
        } else if verus_prooflib::unique_key_lookup(next, other).is_some() {
            let offset = choose|offset: int| #![trigger next[offset]] 0 <= offset < next.len() && next[offset].0 == other;
            assert(0 <= offset < next.len() && next[offset].0 == other);
            assert(offset < rows.len());
            assert(rows[offset] == next[offset]);
            assert(false);
        }
    }
}
pub open spec fn stock_measure(stocks: Seq<StockCell>, value: int) -> int {
    sum(stocks, |cell: StockCell| if cell.value == value { cell.count@ } else { 0 })
}
pub open spec fn ship_measure(ship: Seq<Prefix>, date: int, quantity: bool) -> int {
    sum(rng(ship.len() as int), |index: int| if ship[index].date == date {
        (if quantity { ship[index].quantity@ } else { ship[index].count@ })
            - if index == 0 { 0 } else if quantity { ship[index - 1].quantity@ } else { ship[index - 1].count@ }
    } else { 0 })
}
pub proof fn lemma_ship_push(ship: Seq<Prefix>, cell: Prefix, date: int, quantity: bool)
    ensures ship_measure(ship.push(cell), date, quantity) == ship_measure(ship, date, quantity)
        + if cell.date == date {
            (if quantity { cell.quantity@ } else { cell.count@ })
                - if ship.len() == 0 { 0 } else if quantity { ship.last().quantity@ } else { ship.last().count@ }
        } else { 0 },
{
    let next = ship.push(cell);
    let after = |index: int| if next[index].date == date {
        (if quantity { next[index].quantity@ } else { next[index].count@ })
            - if index == 0 { 0 } else if quantity { next[index - 1].quantity@ } else { next[index - 1].count@ }
    } else { 0 };
    let before = |index: int| if ship[index].date == date {
        (if quantity { ship[index].quantity@ } else { ship[index].count@ })
            - if index == 0 { 0 } else if quantity { ship[index - 1].quantity@ } else { ship[index - 1].count@ }
    } else { 0 };
    verus_prooflib::lemma_range_step(ship.len() as int, after);
    assert forall|index: int| #![trigger after(index)] 0 <= index < ship.len() implies after(index) == before(index) by {
        assert(next[index] == ship[index]);
        if index > 0 { assert(next[index - 1] == ship[index - 1]); }
    }
    lemma_sum_ext(rng(ship.len() as int), after, before);
}
pub open spec fn day_measure(days: Map<i32, Daily>, date: int, quantity: bool) -> int {
    if i32::MIN as int <= date <= i32::MAX as int && days.contains_key(date as i32) {
        if quantity { days[date as i32].quantity@ } else { days[date as i32].count@ }
    } else { 0 }
}
pub open spec fn day_weight(days: Map<i32, Daily>, date: int, quantity: bool) -> spec_fn(i32) -> int {
    |key: i32| if key == date && days.contains_key(key) {
        if quantity { days[key].quantity@ } else { days[key].count@ }
    } else { 0 }
}
pub proof fn lemma_day_insert(days: Map<i32, Daily>, day: Daily, quantity: bool)
    requires !days.contains_key(day.date),
    ensures forall|date: int| #![trigger day_measure(days.insert(day.date, day), date, quantity)]
        day_measure(days.insert(day.date, day), date, quantity) == day_measure(days, date, quantity)
            + if day.date == date { if quantity { day.quantity@ } else { day.count@ } } else { 0 },
{
    assert forall|date: int| #![trigger day_measure(days.insert(day.date, day), date, quantity)]
        day_measure(days.insert(day.date, day), date, quantity) == day_measure(days, date, quantity)
            + if day.date == date { if quantity { day.quantity@ } else { day.count@ } } else { 0 } by {
        if day.date == date { assert(date as i32 == day.date); }
        else if i32::MIN as int <= date <= i32::MAX as int { assert(date as i32 != day.date); }
    }
}
pub proof fn lemma_day_sum(days: Map<i32, Daily>, dates: Seq<i32>, date: int, quantity: bool)
    requires dates.no_duplicates(), days.dom() == dates.to_set(),
    ensures sum(dates, day_weight(days, date, quantity)) == day_measure(days, date, quantity),
{
    dates.to_set_ensures();
    let weight = day_weight(days, date, quantity);
    if i32::MIN as int <= date <= i32::MAX as int && days.contains_key(date as i32) {
        let key = date as i32;
        assert(dates.contains(key));
        let selected = choose|index: int| #![trigger dates[index]] 0 <= index < dates.len() && dates[index] == key;
        assert(0 <= selected < dates.len() && dates[selected] == key);
        assert forall|index: int| #![trigger weight(dates[index])] 0 <= index < dates.len() && index != selected implies weight(dates[index]) == 0 by {
            assert(dates[index] != dates[selected]);
        }
        lemma_sum_single_match(dates, weight, selected);
    } else {
        assert forall|index: int| #![trigger weight(dates[index])] 0 <= index < dates.len() implies weight(dates[index]) == 0 by {
            if dates[index] == date { assert(i32::MIN as int <= date <= i32::MAX as int); }
        }
        lemma_sum_zero(dates, weight);
    }
}
pub proof fn lemma_stock_cell_count(stocks: Seq<StockCell>, index: int)
    requires verus_common::order::sorted_by(stocks, |left: StockCell, right: StockCell| left.value < right.value),
        0 <= index < stocks.len(),
    ensures stock_measure(stocks, stocks[index].value as int) == stocks[index].count@,
{
    let value = stocks[index].value as int;
    let weight = |cell: StockCell| if cell.value == value { cell.count@ } else { 0 };
    assert forall|other: int| #![trigger weight(stocks[other])] 0 <= other < stocks.len() && other != index implies weight(stocks[other]) == 0 by {
        if other < index { assert(stocks[other].value < stocks[index].value); }
        else { assert(stocks[index].value < stocks[other].value); }
    }
    lemma_sum_single_match(stocks, weight, index);
}
}
