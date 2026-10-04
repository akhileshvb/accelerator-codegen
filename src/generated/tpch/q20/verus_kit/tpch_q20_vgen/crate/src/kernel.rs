use vstd::prelude::*;
use std::sync::Arc;
use std::collections::HashMap;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::profiles::*;
use crate::profiles::{add, sub, mul};
use crate::dimensions::*;
use crate::model::*;
use crate::output::merge_sort;
use crate::query_proofs::*;
use crate::read_proofs::*;
use crate::update_proofs::*;
use verus_common::sums::sum;
use verus_common::sums::lemma_sum_take;
use verus_prooflib::{lemma_unique_push, lemma_push_membership};

verus! {
pub struct State {
    db: Ghost<Db>,
    data: Data,
}
pub struct Data {
    parts: Table<Part>,
    stock_pairs: Table<Vec<i64>>,
    suppliers: Suppliers,
    nations: Nations,
}
impl State {
    pub closed spec fn db(self) -> Db { self.db@ }
    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        meaning(self.db@, self.data.parts, self.data.stock_pairs, self.data.suppliers, self.data.nations)
            && domain_valid(self.db@)
    }
}
pub fn stock_change(work: &mut HashMap<i64, PartWork>, row: &StockInput, sign: i128) -> (result: bool)
    ensures result ==> final(work)@.dom() == old(work)@.dom(),
        result ==> forall|key: i64| #![trigger final(work)@[key]] final(work)@.contains_key(key) ==> final(work)@[key].count == old(work)@[key].count,
        result ==> forall|key: i64| #[trigger] work_part_count(final(work)@, key) == work_part_count(old(work)@, key),
        result && empty_originals(old(work)@) ==> empty_originals(final(work)@),
        result && work_shape(old(work)@) ==> work_shape(final(work)@),
        result && (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, value: int|
            #[trigger] work_stock_count(final(work)@, part, supplier, value)
                == work_stock_count(old(work)@, part, supplier, value)
                    + if part == row.ps_partkey && supplier == row.ps_suppkey && value == row.ps_availqty { sign as int } else { 0 },
        result && (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, date: int|
            #[trigger] work_day_count(final(work)@, part, supplier, date) == work_day_count(old(work)@, part, supplier, date)
                && work_day_quantity(final(work)@, part, supplier, date) == work_day_quantity(old(work)@, part, supplier, date),
        result && (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, date: int|
            #[trigger] work_day_quantity(final(work)@, part, supplier, date) == work_day_quantity(old(work)@, part, supplier, date),
{
    let ghost before = work@;
    let mut part = match work.remove(&row.ps_partkey) { Some(value) => value, None => return false };
    let mut pair = match part.take_pair(row.ps_suppkey) { Some(value) => value, None => return false };
    let ghost previous_pair = pair;
    if !pair.stock(row.ps_availqty, sign) { return false; }
    let ghost updated_pair = pair;
    part.install_pair(row.ps_suppkey, pair); work.insert(row.ps_partkey, part);
    proof {
        if empty_originals(before) || work_shape(before) {
            assert forall|part_key: i64, supplier: i64, value: int|
                #[trigger] work_stock_count(work@, part_key, supplier, value)
                    == work_stock_count(before, part_key, supplier, value)
                        + if part_key == row.ps_partkey && supplier == row.ps_suppkey && value == row.ps_availqty { sign as int } else { 0 } by {
                if part_key == row.ps_partkey && supplier == row.ps_suppkey {
                    assert(previous_pair.stock_count(value as int) == work_stock_count(before, part_key, supplier, value));
                    assert(work_stock_count(work@, part_key, supplier, value) == updated_pair.stock_count(value as int));
                    assert(updated_pair.stock_count(value as int) == previous_pair.stock_count(value as int)
                        + if value == row.ps_availqty { sign as int } else { 0 });
                }
            }
            assert forall|part_key: i64, supplier: i64, date: int|
                #[trigger] work_day_count(work@, part_key, supplier, date) == work_day_count(before, part_key, supplier, date)
                    && work_day_quantity(work@, part_key, supplier, date) == work_day_quantity(before, part_key, supplier, date) by {
                if part_key == row.ps_partkey && supplier == row.ps_suppkey {
                    assert(crate::profile_proofs::day_measure(previous_pair.days@, date as int, false) == work_day_count(before, part_key, supplier, date));
                    assert(crate::profile_proofs::day_measure(previous_pair.days@, date as int, true) == work_day_quantity(before, part_key, supplier, date));
                }
            }
        }
    }
    true
}
pub fn shipment_change(work: &mut HashMap<i64, PartWork>, row: &ShipmentInput, sign: i128) -> (result: bool)
    ensures result ==> final(work)@.dom() == old(work)@.dom(),
        result ==> forall|key: i64| #![trigger final(work)@[key]] final(work)@.contains_key(key) ==> final(work)@[key].count == old(work)@[key].count,
        result ==> forall|key: i64| #[trigger] work_part_count(final(work)@, key) == work_part_count(old(work)@, key),
        result && empty_originals(old(work)@) ==> empty_originals(final(work)@),
        result && work_shape(old(work)@) ==> work_shape(final(work)@),
        result && (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, date: int|
            #[trigger] work_day_count(final(work)@, part, supplier, date) == work_day_count(old(work)@, part, supplier, date)
                + if part == row.l_partkey && supplier == row.l_suppkey && date == row.l_shipdate { sign as int } else { 0 },
        result && (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, date: int|
            #[trigger] work_day_quantity(final(work)@, part, supplier, date) == work_day_quantity(old(work)@, part, supplier, date)
                + if part == row.l_partkey && supplier == row.l_suppkey && date == row.l_shipdate { (sign as int) * (row.l_quantity as int) } else { 0 },
        result && (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, value: int|
            #[trigger] work_stock_count(final(work)@, part, supplier, value) == work_stock_count(old(work)@, part, supplier, value),
{
    let ghost before = work@;
    let mut part = match work.remove(&row.l_partkey) { Some(value) => value, None => return false };
    let mut pair = match part.take_pair(row.l_suppkey) { Some(value) => value, None => return false };
    let ghost previous_pair = pair;
    if !pair.shipment(row.l_shipdate, row.l_quantity, sign) { return false; }
    let ghost updated_pair = pair;
    part.install_pair(row.l_suppkey, pair); work.insert(row.l_partkey, part);
    proof {
        if empty_originals(before) || work_shape(before) {
            assert forall|part_key: i64, supplier: i64, value: int|
                #[trigger] work_stock_count(work@, part_key, supplier, value)
                    == work_stock_count(before, part_key, supplier, value) by {
                if part_key == row.l_partkey && supplier == row.l_suppkey {
                    assert(previous_pair.stock_count(value) == work_stock_count(before, part_key, supplier, value));
                    assert(updated_pair.stock_count(value) == previous_pair.stock_count(value));
                    assert(work_stock_count(work@, part_key, supplier, value) == updated_pair.stock_count(value));
                }
            }
            assert forall|part_key: i64, supplier: i64, date: int|
                #[trigger] work_day_count(work@, part_key, supplier, date) == work_day_count(before, part_key, supplier, date)
                    + if part_key == row.l_partkey && supplier == row.l_suppkey && date == row.l_shipdate { sign as int } else { 0 } by {
                if part_key == row.l_partkey && supplier == row.l_suppkey {
                    assert(crate::profile_proofs::day_measure(previous_pair.days@, date as int, false) == work_day_count(before, part_key, supplier, date));
                    assert(work_day_count(work@, part_key, supplier, date) == crate::profile_proofs::day_measure(updated_pair.days@, date, false));
                    assert(crate::profile_proofs::day_measure(updated_pair.days@, date, false) == crate::profile_proofs::day_measure(previous_pair.days@, date, false)
                        + if date == row.l_shipdate { sign as int } else { 0 });
                }
            }
            assert forall|part_key: i64, supplier: i64, date: int|
                #[trigger] work_day_quantity(work@, part_key, supplier, date) == work_day_quantity(before, part_key, supplier, date)
                    + if part_key == row.l_partkey && supplier == row.l_suppkey && date == row.l_shipdate { (sign as int) * (row.l_quantity as int) } else { 0 } by {
                if part_key == row.l_partkey && supplier == row.l_suppkey {
                    assert(crate::profile_proofs::day_measure(previous_pair.days@, date as int, true) == work_day_quantity(before, part_key, supplier, date));
                    assert(work_day_quantity(work@, part_key, supplier, date) == crate::profile_proofs::day_measure(updated_pair.days@, date, true));
                    assert(crate::profile_proofs::day_measure(updated_pair.days@, date, true) == crate::profile_proofs::day_measure(previous_pair.days@, date, true)
                        + if date == row.l_shipdate { (row.l_quantity as int) * (sign as int) } else { 0 });
                    assert((sign as int) * (row.l_quantity as int) == (row.l_quantity as int) * (sign as int)) by (nonlinear_arith);
                }
            }
        }
    }
    true
}
pub fn directory_edit(directory: &mut Table<Vec<i64>>, supplier: i64, part: i64, present: bool)
    requires directory_unique(*old(directory)),
    ensures directory_unique(*final(directory)),
        forall|other_supplier: i64, other_part: i64| #![trigger directory_contains(*final(directory), other_supplier, other_part)]
            directory_contains(*final(directory), other_supplier, other_part) ==
                if other_supplier == supplier && other_part == part { present } else { directory_contains(*old(directory), other_supplier, other_part) },
{
    let ghost previous = *directory;
    let mut keys = match directory.get(supplier) { Some(value) => copy_i64(&value), None => Vec::new() };
    let ghost before = keys@;
    let mut index = 0;
    while index < keys.len() && keys[index] != part
        invariant index <= keys.len(), keys@ == before,
            forall|offset: int| #![trigger before[offset]] 0 <= offset < index ==> before[offset] != part,
        decreases keys.len() - index,
    {
        index += 1;
    }
    if present && index == keys.len() {
        proof { assert(!keys@.contains(part)); verus_prooflib::lemma_unique_push(keys@, part); lemma_push_membership(keys@, part); }
        keys.push(part);
    }
    if !present && index < keys.len() {
        proof { verus_prooflib::lemma_unique_remove(keys@, index as int); }
        keys.remove(index);
        proof {
            assert forall|probe: i64| #[trigger] keys@.contains(probe) <==> before.contains(probe) && probe != part by {
                if keys@.contains(probe) {
                    let position = choose|position: int| #![trigger keys@[position]] 0 <= position < keys@.len() && keys@[position] == probe;
                    let source = if position < index { position } else { position + 1 };
                    assert(keys@[position] == before[source]);
                    assert(source != index);
                }
                if before.contains(probe) && probe != part {
                    let source = choose|source: int| #![trigger before[source]] 0 <= source < before.len() && before[source] == probe;
                    let position = if source < index { source } else { source - 1 };
                    assert(keys@[position] == before[source]);
                }
            }
        }
    }
    let ghost updated = keys@;
    if keys.len() == 0 { directory.put(supplier, None); } else { directory.put(supplier, Some(Arc::new(keys))); }
    proof {
        assert forall|other_supplier: i64, other_part: i64| #![trigger directory_contains(*directory, other_supplier, other_part)]
            directory_contains(*directory, other_supplier, other_part) ==
                if other_supplier == supplier && other_part == part { present } else { directory_contains(previous, other_supplier, other_part) } by {
            if other_supplier == supplier { assert(directory_contains(*directory, supplier, other_part) == updated.contains(other_part)); }
        }
    }
}
fn publish_part(state: &mut Data, key: i64, part: Option<Part>)
    requires directory_unique(old(state).stock_pairs),
    ensures final(state).parts.lookup(key) == part,
        forall|other: i64| #![trigger final(state).parts.lookup(other)] other != key ==>
            final(state).parts.lookup(other) == old(state).parts.lookup(other),
        final(state).suppliers == old(state).suppliers, final(state).nations == old(state).nations,
        directory_unique(final(state).stock_pairs),
        directory_matches(old(state).parts, old(state).stock_pairs) ==> directory_matches(final(state).parts, final(state).stock_pairs),
{
    let ghost previous_parts = state.parts;
    let ghost previous_suppliers = state.suppliers;
    let ghost previous_nations = state.nations;
    let ghost previous_directory = state.stock_pairs;
    if let Some(data) = state.parts.get(key) {
        let mut index = 0;
        while index < data.pairs.len()
            invariant index <= data.pairs.len(), state.parts == previous_parts, state.suppliers == previous_suppliers, state.nations == previous_nations,
                directory_unique(state.stock_pairs),
                forall|supplier: i64| #![trigger directory_contains(state.stock_pairs, supplier, key)]
                    directory_contains(state.stock_pairs, supplier, key) ==
                        (directory_contains(previous_directory, supplier, key) && !stock_support(data.pairs@.take(index as int), supplier)),
                forall|supplier: i64, other: i64| #![trigger directory_contains(state.stock_pairs, supplier, other)] other != key ==>
                    directory_contains(state.stock_pairs, supplier, other) == directory_contains(previous_directory, supplier, other),
            decreases data.pairs.len() - index,
        {
            let ghost before = state.stock_pairs;
            if data.pairs[index].1.stocks.len() > 0 { directory_edit(&mut state.stock_pairs, data.pairs[index].0, key, false); }
            proof {
                assert forall|supplier: i64| #![trigger directory_contains(state.stock_pairs, supplier, key)]
                    directory_contains(state.stock_pairs, supplier, key) ==
                        (directory_contains(previous_directory, supplier, key) && !stock_support(data.pairs@.take(index as int + 1), supplier)) by {
                    lemma_stock_support_step(data.pairs@, index as int, supplier);
                    assert(directory_contains(before, supplier, key) == (directory_contains(previous_directory, supplier, key) && !stock_support(data.pairs@.take(index as int), supplier)));
                }
            }
            index += 1;
        }
        proof { assert(data.pairs@.take(data.pairs.len() as int) =~= data.pairs@); }
    }
    proof {
        if directory_matches(previous_parts, previous_directory) {
            assert forall|supplier: i64| !#[trigger] directory_contains(state.stock_pairs, supplier, key) by {
                if let Some(data) = previous_parts.lookup(key) {
                    assert(directory_contains(previous_directory, supplier, key) == stock_support(data.pairs@, supplier));
                }
            }
        }
    }
    match part {
        None => state.parts.put(key, None),
        Some(data) => {
            let mut index = 0;
            while index < data.pairs.len()
                invariant index <= data.pairs.len(), state.parts == previous_parts, state.suppliers == previous_suppliers, state.nations == previous_nations,
                    directory_unique(state.stock_pairs),
                    directory_matches(previous_parts, previous_directory) ==> forall|supplier: i64| #![trigger directory_contains(state.stock_pairs, supplier, key)]
                        directory_contains(state.stock_pairs, supplier, key) == stock_support(data.pairs@.take(index as int), supplier),
                    forall|supplier: i64, other: i64| #![trigger directory_contains(state.stock_pairs, supplier, other)] other != key ==>
                        directory_contains(state.stock_pairs, supplier, other) == directory_contains(previous_directory, supplier, other),
                decreases data.pairs.len() - index,
            {
                let ghost before = state.stock_pairs;
                if data.pairs[index].1.stocks.len() > 0 { directory_edit(&mut state.stock_pairs, data.pairs[index].0, key, true); }
                proof {
                    if directory_matches(previous_parts, previous_directory) {
                        assert forall|supplier: i64| #![trigger directory_contains(state.stock_pairs, supplier, key)]
                            directory_contains(state.stock_pairs, supplier, key) == stock_support(data.pairs@.take(index as int + 1), supplier) by {
                            lemma_stock_support_step(data.pairs@, index as int, supplier);
                            assert(directory_contains(before, supplier, key) == stock_support(data.pairs@.take(index as int), supplier));
                        }
                    }
                }
                index += 1;
            }
            proof { assert(data.pairs@.take(data.pairs.len() as int) =~= data.pairs@); }
            state.parts.put(key, Some(Arc::new(data)));
        },
    }
}
pub fn build(
    qualifying_parts: &[QualifyingPartInput], stocks: &[StockInput], shipments: &[ShipmentInput],
    suppliers: Vec<SupplierInput>, nations: Vec<NationInput>, Ghost(db): Ghost<Db>,
) -> (result: Option<State>)
    requires scan_qualifying_parts_ok(db, qualifying_parts@), scan_stocks_ok(db, stocks@),
        scan_shipments_ok(db, shipments@), scan_suppliers_ok(db, suppliers@), scan_nations_ok(db, nations@),
    ensures result matches Some(state) ==> state.db() == db,
{
    let mut state = Data { parts: Table::new(), stock_pairs: Table::new(), suppliers: Table::new(), nations: Table::new() };
    let ghost initial_parts = state.parts;
    let ghost initial_directory = state.stock_pairs;
    let ghost initial_suppliers = state.suppliers;
    let ghost initial_nations = state.nations;
    let ghost projected = crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@);
    proof {
        crate::state_proofs::lemma_build_domain(db, qualifying_parts@, stocks@, shipments@, suppliers@, nations@);
        assert(dimensions_empty(initial_suppliers, initial_nations));
    }
    let mut work: HashMap<i64, PartWork> = HashMap::new(); let mut keys = Vec::new(); let mut index = 0;
    while index < qualifying_parts.len()
        invariant index <= qualifying_parts.len(), work@.dom() == keys@.to_set(), keys@.no_duplicates(),
            domain_valid(db), dimensions_empty(initial_suppliers, initial_nations),
            work_shape(work@), state.parts == initial_parts, state.stock_pairs == initial_directory,
            projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
            state.suppliers == initial_suppliers, state.nations == initial_nations,
            scan_qualifying_parts_ok(db, qualifying_parts@),
            forall|key: i64| #[trigger] part_count(state.parts, key) == 0,
            forall|key: i64| #![trigger work_part_count(work@, key)]
                work_part_count(work@, key) == sum(qualifying_parts@.take(index as int), |row: QualifyingPartInput| if row.p_partkey == key { 1 } else { 0 }),
            forall|key: i64| #![trigger work@[key]] work@.contains_key(key) ==>
                work@[key].pairs@.is_empty() && work@[key].original@.is_empty(),
        decreases qualifying_parts.len() - index,
    {
        let ghost processed = index as int;
        let ghost before = work@;
        let ghost support = keys@;
        let key = qualifying_parts[index].p_partkey;
        let mut part = match work.remove(&key) {
            Some(value) => value,
            None => {
                proof { lemma_unique_push(keys@, key); lemma_push_membership(keys@, key); }
                keys.push(key); PartWork::new(0)
            },
        };
        proof { assert(part.count as int == work_part_count(before, key)); }
        let count = match add(part.count, 1) { Some(value) => value, None => return None };
        part.count = count;
        let ghost replacement = part;
        let ghost remaining = work@;
        work.insert(key, part);
        proof {
            assert(remaining == before.remove(key));
            assert(work@ == remaining.insert(key, replacement));
            assert(work_part_count(work@, key) == work_part_count(before, key) + 1);
            assert forall|other: i64| #[trigger] work@.contains_key(other) == keys@.contains(other) by {
                assert(work@.contains_key(other) == (before.contains_key(other) || other == key));
                if before.contains_key(key) { assert(keys@ == support); }
                else { assert(keys@ == support.push(key)); }
            }
            assert(work@.dom() =~= keys@.to_set());
            assert forall|other: i64| #![trigger work_part_count(work@, other)]
                work_part_count(work@, other) == sum(qualifying_parts@.take(index as int + 1), |row: QualifyingPartInput| if row.p_partkey == other { 1 } else { 0 }) by {
                lemma_sum_take(qualifying_parts@, index as int, |row: QualifyingPartInput| if row.p_partkey == other { 1 } else { 0 });
                if other != key {
                    assert(work_part_count(work@, other) == work_part_count(before, other));
                    assert(work_part_count(before, other) == sum(qualifying_parts@.take(index as int), |row: QualifyingPartInput| if row.p_partkey == other { 1 } else { 0 }));
                }
            }
        }
        index += 1;
    }
    proof {
        assert(qualifying_parts@.take(qualifying_parts.len() as int) =~= qualifying_parts@);
        assert forall|key: i64| #![trigger work_part_count(work@, key)] work_part_count(work@, key) == eligible(db, key as int) by {
            crate::semantic_proofs::lemma_build_part_count(db, qualifying_parts@, key as int);
        }
    }
    let mut index = 0;
    while index < stocks.len()
        invariant index <= stocks.len(), empty_originals(work@),
            domain_valid(db), dimensions_empty(initial_suppliers, initial_nations),
            work_shape(work@), state.parts == initial_parts, state.stock_pairs == initial_directory,
            projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
            state.suppliers == initial_suppliers, state.nations == initial_nations,
            work@.dom() == keys@.to_set(), keys@.no_duplicates(),
            forall|key: i64| #[trigger] part_count(state.parts, key) == 0,
            forall|key: i64| #![trigger work_part_count(work@, key)] work_part_count(work@, key) == eligible(db, key as int),
            forall|part: i64, supplier: i64, value: int| #![trigger work_stock_count(work@, part, supplier, value)]
                work_stock_count(work@, part, supplier, value) == sum(stocks@.take(index as int), |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }),
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(work@, part, supplier, date)]
                work_day_count(work@, part, supplier, date) == 0 && work_day_quantity(work@, part, supplier, date) == 0,
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_quantity(work@, part, supplier, date)]
                work_day_quantity(work@, part, supplier, date) == 0,
        decreases stocks.len() - index,
    {
        let ghost processed = index as int;
        if !stock_change(&mut work, &stocks[index], 1) { return None; }
        proof {
            assert forall|part: i64, supplier: i64, value: int| #![trigger work_stock_count(work@, part, supplier, value)]
                work_stock_count(work@, part, supplier, value) == sum(stocks@.take(index as int + 1), |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }) by {
                lemma_sum_take(stocks@, index as int, |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 });
            }
        }
        index += 1;
    }
    let mut index = 0;
    proof { assert(stocks@.take(stocks.len() as int) =~= stocks@); }
    while index < shipments.len()
        invariant index <= shipments.len(), empty_originals(work@),
            domain_valid(db), dimensions_empty(initial_suppliers, initial_nations),
            work_shape(work@), state.parts == initial_parts, state.stock_pairs == initial_directory,
            projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
            state.suppliers == initial_suppliers, state.nations == initial_nations,
            forall|part: i64, supplier: i64, value: int| #![trigger work_stock_count(work@, part, supplier, value)]
                work_stock_count(work@, part, supplier, value) == sum(stocks@, |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }),
            work@.dom() == keys@.to_set(), keys@.no_duplicates(),
            forall|key: i64| #[trigger] part_count(state.parts, key) == 0,
            forall|key: i64| #![trigger work_part_count(work@, key)] work_part_count(work@, key) == eligible(db, key as int),
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(work@, part, supplier, date)]
                work_day_count(work@, part, supplier, date) == sum(shipments@.take(index as int), |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 }),
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_quantity(work@, part, supplier, date)]
                work_day_quantity(work@, part, supplier, date) == sum(shipments@.take(index as int), |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }),
        decreases shipments.len() - index,
    {
        let ghost processed = index as int;
        if !shipment_change(&mut work, &shipments[index], 1) { return None; }
        proof {
            assert forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(work@, part, supplier, date)]
                work_day_count(work@, part, supplier, date) == sum(shipments@.take(index as int + 1), |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 }) by {
                lemma_sum_take(shipments@, index as int, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 });
            }
            assert forall|part: i64, supplier: i64, date: int| #![trigger work_day_quantity(work@, part, supplier, date)]
                work_day_quantity(work@, part, supplier, date) == sum(shipments@.take(index as int + 1), |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }) by {
                lemma_sum_take(shipments@, index as int, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 });
            }
        }
        index += 1;
    }
    let mut index = 0;
    let ghost built = work@;
    proof {
        assert(shipments@.take(shipments.len() as int) =~= shipments@);
        assert forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(built, part, supplier, date)]
            work_day_count(built, part, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 })
            && work_day_quantity(built, part, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }) by {
            assert(work_day_quantity(built, part, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }));
        }
        keys@.to_set_ensures();
        assert forall|key: i64| #![trigger keys@.contains(key)] !keys@.contains(key) implies eligible(db, key as int) == 0 by {
            assert(!work@.contains_key(key));
            assert(work_part_count(work@, key) == eligible(db, key as int));
        }
    }
    while index < keys.len()
        invariant index <= keys.len(), keys@.no_duplicates(),
            domain_valid(db), dimensions_empty(initial_suppliers, initial_nations),
            work@ == built.remove_keys(keys@.take(index as int).to_set()), work_shape(built),
            projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
            directory_matches(state.parts, state.stock_pairs),
            state.suppliers == initial_suppliers, state.nations == initial_nations,
            forall|key: i64| #![trigger state.parts.lookup(key)] keys@.take(index as int).contains(key) ==>
                part_correct(projected, key, state.parts.lookup(key)),
            forall|key: i64| #![trigger state.parts.lookup(key)] !keys@.take(index as int).contains(key) ==> state.parts.lookup(key).is_none(),
            forall|part: i64, supplier: i64, value: int| #![trigger work_stock_count(built, part, supplier, value)]
                work_stock_count(built, part, supplier, value) == sum(stocks@, |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }),
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(built, part, supplier, date)]
                work_day_count(built, part, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 })
                && work_day_quantity(built, part, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }),
            forall|key: i64| #![trigger keys@.contains(key)] !keys@.contains(key) ==> eligible(db, key as int) == 0,
            forall|key: i64| #![trigger work_part_count(work@, key)] work_part_count(work@, key)
                == if keys@.take(index as int).contains(key) { 0 } else { eligible(db, key as int) },
            forall|key: i64| #![trigger part_count(state.parts, key)] part_count(state.parts, key)
                == if keys@.take(index as int).contains(key) { eligible(db, key as int) } else { 0 },
        decreases keys.len() - index,
    {
        let key = keys[index];
        let ghost previous_work = work@;
        let ghost previous_parts = state.parts;
        proof {
            verus_prooflib::lemma_prefix_fresh(keys@, index as int);
            assert(work_part_count(previous_work, key) == eligible(db, key as int));
        }
        let raw = match work.remove(&key) { Some(value) => value, None => return None };
        let ghost source = raw;
        proof { assert(raw.count as int == eligible(db, key as int)); }
        let part = match raw.finish() { Some(value) => value, None => return None };
        proof {
            assert(source == built[key]);
            assert forall|supplier: i64, value: int| #![trigger source.stock_for(supplier, value)] source.stock_for(supplier, value) ==
                sum(stocks@, |row: StockInput| if row.ps_partkey == key && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }) by {
                assert(source.stock_for(supplier, value) == work_stock_count(built, key, supplier, value));
                assert(work_stock_count(built, key, supplier, value) == sum(stocks@, |row: StockInput| if row.ps_partkey == key && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }));
            }
            assert forall|supplier: i64, date: int| #![trigger source.day_for(supplier, date, false)]
                source.day_for(supplier, date, false) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 })
                && source.day_for(supplier, date, true) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }) by {
                assert(source.day_for(supplier, date, false) == work_day_count(built, key, supplier, date));
                assert(source.day_for(supplier, date, true) == work_day_quantity(built, key, supplier, date));
                assert(work_day_count(built, key, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 }));
            }
            crate::state_proofs::lemma_build_part(projected, key, source, &part, stocks@, shipments@);
        }
        publish_part(&mut state, key, Some(part));
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            lemma_push_membership(keys@.take(index as int), key);
            verus_prooflib::lemma_push_to_set(keys@.take(index as int), key);
            assert(work@ =~= built.remove_keys(keys@.take(index as int + 1).to_set()));
            assert forall|other: i64| #![trigger work_part_count(work@, other)] work_part_count(work@, other)
                == if keys@.take(index as int + 1).contains(other) { 0 } else { eligible(db, other as int) } by {
                assert(work_part_count(previous_work, other) == if keys@.take(index as int).contains(other) { 0 } else { eligible(db, other as int) });
            }
            assert forall|other: i64| #![trigger part_count(state.parts, other)] part_count(state.parts, other)
                == if keys@.take(index as int + 1).contains(other) { eligible(db, other as int) } else { 0 } by {
                assert(part_count(previous_parts, other) == if keys@.take(index as int).contains(other) { eligible(db, other as int) } else { 0 });
            }
        }
        index += 1;
    }
    proof {
        assert(keys@.take(keys.len() as int) =~= keys@);
        assert forall|key: i64| #![trigger part_count(state.parts, key)] part_count(state.parts, key) == eligible(db, key as int) by {
            if !keys@.contains(key) { assert(eligible(db, key as int) == 0); }
        }
    }
    let ghost finished_parts = state.parts;
    let ghost finished_directory = state.stock_pairs;
    let mut supplier_work = SupplierWork::new(); let mut index = 0;
    while index < suppliers.len() invariant index <= suppliers.len(), state.parts == finished_parts,
        domain_valid(db), dimensions_empty(initial_suppliers, initial_nations),
        projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
        supplier_work.shape(), state.stock_pairs == finished_directory, state.suppliers == initial_suppliers, state.nations == initial_nations,
        directory_matches(state.parts, state.stock_pairs),
        forall|key: i64| #![trigger state.parts.lookup(key)] part_correct(projected, key, state.parts.lookup(key)),
        forall|key: i64| #![trigger part_count(state.parts, key)] part_count(state.parts, key) == eligible(db, key as int),
        forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #![trigger supplier_work.count(initial_suppliers, nation, supplier, name, address)]
            supplier_work.count(initial_suppliers, nation, supplier, name, address) == sum(suppliers@.take(index as int),
                |row: SupplierInput| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name@ == name && row.s_address@ == address { 1 } else { 0 }),
        decreases suppliers.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous = supplier_work;
        if !supplier_work.change(&state.suppliers, &suppliers[index], 1) { return None; }
        proof {
            assert forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #![trigger supplier_work.count(initial_suppliers, nation, supplier, name, address)]
                supplier_work.count(initial_suppliers, nation, supplier, name, address) == sum(suppliers@.take(index as int + 1),
                    |row: SupplierInput| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name@ == name && row.s_address@ == address { 1 } else { 0 }) by {
                let weight = |row: SupplierInput| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name@ == name && row.s_address@ == address { 1int } else { 0int };
                assert(previous.count(initial_suppliers, nation, supplier, name, address) == sum(suppliers@.take(index as int), weight));
                lemma_sum_take(suppliers@, index as int, weight);
            }
        }
        index += 1;
    }
    proof { assert(suppliers@.take(suppliers.len() as int) =~= suppliers@); }
    if !supplier_work.publish(&mut state.suppliers) { return None; }
    proof { crate::state_proofs::lemma_supplier_build(projected, state.suppliers, suppliers@); }
    let ghost finished_suppliers = state.suppliers;
    let mut nation_work = NationWork::new(); let mut index = 0;
    while index < nations.len() invariant index <= nations.len(), state.parts == finished_parts, nation_work.shape(),
        domain_valid(db), dimensions_empty(initial_suppliers, initial_nations), nation_work.hashes(), state.nations == initial_nations,
        nations_positive(state.nations),
        projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
        state.suppliers == finished_suppliers, supplier_correct(projected, state.suppliers),
        state.stock_pairs == finished_directory, directory_matches(state.parts, state.stock_pairs),
        forall|key: i64| #![trigger state.parts.lookup(key)] part_correct(projected, key, state.parts.lookup(key)),
        forall|key: i64| #![trigger part_count(state.parts, key)] part_count(state.parts, key) == eligible(db, key as int),
        forall|name: Seq<u8>, key: i64| #![trigger nation_work.count(initial_nations, name, key)] nation_work.count(initial_nations, name, key) ==
            sum(nations@.take(index as int), |row: NationInput| if row.n_name@ == name && row.n_nationkey == key { 1 } else { 0 }),
        decreases nations.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous = nation_work;
        if !nation_work.change(&state.nations, &nations[index], 1) { return None; }
        proof {
            assert forall|name: Seq<u8>, key: i64| #![trigger nation_work.count(initial_nations, name, key)] nation_work.count(initial_nations, name, key) ==
                sum(nations@.take(index as int + 1), |row: NationInput| if row.n_name@ == name && row.n_nationkey == key { 1 } else { 0 }) by {
                let weight = |row: NationInput| if row.n_name@ == name && row.n_nationkey == key { 1int } else { 0int };
                assert(previous.count(initial_nations, name, key) == sum(nations@.take(index as int), weight));
                lemma_sum_take(nations@, index as int, weight);
            }
        }
        index += 1;
    }
    proof { assert(nations@.take(nations.len() as int) =~= nations@); }
    if !nation_work.publish(&mut state.nations) { return None; }
    proof {
        crate::state_proofs::lemma_nation_build(projected, state.nations, nations@);
        use_type_invariant(&state.parts);
        use_type_invariant(&state.stock_pairs);
        use_type_invariant(&state.suppliers);
        use_type_invariant(&state.nations);
        state.parts.reveal_well_formed();
        state.stock_pairs.reveal_well_formed();
        state.suppliers.reveal_well_formed();
        state.nations.reveal_well_formed();
        crate::state_proofs::lemma_state_meaning(projected, state.parts, state.stock_pairs, state.suppliers, state.nations);
        crate::semantic_proofs::lemma_build_meaning(db, stocks@, shipments@, suppliers@, nations@, state.parts, state.stock_pairs, state.suppliers, state.nations);
    }
    Some(State { db: Ghost(db), data: state })
}
fn qualifies(state: &Data, supplier: i64, start: i32, end: i32) -> (result: Option<bool>)
    ensures read_shape(state.parts) ==> (result matches Some(value) ==>
        value == cached_supplier_passes(state.parts, state.stock_pairs, supplier, start as int, end as int)),
{
    let keys = match state.stock_pairs.get(supplier) { Some(value) => value, None => return Some(false) };
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), state.stock_pairs.lookup(supplier) == Some(*keys),
            read_shape(state.parts) ==> forall|offset: int| #![trigger keys@[offset]] 0 <= offset < index ==>
                !cached_part_passes(state.parts, keys@[offset], supplier, start as int, end as int),
        decreases keys.len() - index,
    {
        let part = match state.parts.get(keys[index]) { Some(value) => value, None => return None };
        let mut pair_index = 0;
        while pair_index < part.pairs.len() && part.pairs[pair_index].0 != supplier
            invariant pair_index <= part.pairs.len(),
                state.parts.lookup(keys@[index as int]) == Some(*part), index < keys.len(),
                state.stock_pairs.lookup(supplier) == Some(*keys),
                forall|offset: int| #![trigger part.pairs@[offset]] 0 <= offset < pair_index ==> part.pairs@[offset].0 != supplier,
            decreases part.pairs.len() - pair_index,
        {
            pair_index += 1;
        }
        if pair_index == part.pairs.len() { return None; }
        let pair = &*part.pairs[pair_index].1;
        proof {
            if read_shape(state.parts) {
                lemma_pair_at(state.parts, keys@[index as int], supplier, &*part, pair_index as int);
                assert(pair_of(state.parts, keys@[index as int], supplier) == Some(*pair));
                assert(pair_valid(*pair));
            }
        }
        if pair.stocks.len() == 0 { return None; }
        let lower = prefix(pair, start); let upper = prefix(pair, end);
        let count = match sub(upper.0, lower.0) { Some(value) => value, None => return None };
        let quantity = match sub(upper.1, lower.1) { Some(value) => value, None => return None };
        let maximum = pair.stocks[pair.stocks.len() - 1].value as i128;
        let threshold = match mul(maximum, 200) { Some(value) => value, None => return None };
        if count > 0 && threshold > quantity {
            proof {
                if read_shape(state.parts) {
                    assert(cached_part_passes(state.parts, keys@[index as int], supplier, start as int, end as int));
                    assert(state.stock_pairs.lookup(supplier) == Some(*keys));
                    assert(keys@.contains(keys@[index as int]));
                    assert(directory_contains(state.stock_pairs, supplier, keys@[index as int]));
                    assert(cached_supplier_passes(state.parts, state.stock_pairs, supplier, start as int, end as int));
                }
            }
            return Some(true);
        }
        proof {
            if read_shape(state.parts) {
                assert(!cached_part_passes(state.parts, keys@[index as int], supplier, start as int, end as int));
            }
        }
        index += 1;
    }
    proof {
        if read_shape(state.parts) && cached_supplier_passes(state.parts, state.stock_pairs, supplier, start as int, end as int) {
            let part = choose|part: i64| #![trigger cached_part_passes(state.parts, part, supplier, start as int, end as int)]
                directory_contains(state.stock_pairs, supplier, part) && cached_part_passes(state.parts, part, supplier, start as int, end as int);
            assert(directory_contains(state.stock_pairs, supplier, part) && cached_part_passes(state.parts, part, supplier, start as int, end as int));
            assert(keys@.contains(part));
            let offset = choose|offset: int| #![trigger keys@[offset]] 0 <= offset < keys@.len() && keys@[offset] == part;
            assert(0 <= offset < keys@.len() && keys@[offset] == part);
            assert(!cached_part_passes(state.parts, part, supplier, start as int, end as int));
        }
    }
    Some(false)
}
pub open spec fn qualification_cache(cache: Map<i64, bool>, parts: Table<Part>, directory: Table<Vec<i64>>, start: int, end: int) -> bool {
    forall|key: i64| #![trigger cache[key]] cache.contains_key(key) ==> cache[key] == cached_supplier_passes(parts, directory, key, start, end)
}
fn emit_rows(out: &mut Vec<(Vec<u8>, Vec<u8>)>, rows: &Vec<SupplierVariant>, factor: i128) -> (result: bool)
    ensures result ==> forall|weight: spec_fn(OutRow) -> int| #[trigger] verus_common::sums::sum(out_view(final(out)@), weight) ==
        verus_common::sums::sum(out_view(old(out)@), weight) + factor as int * variant_weight(rows@, weight),
{
    let ghost initial = out_view(out@);
    let mut variant = 0;
    while variant < rows.len()
        invariant variant <= rows.len(),
            forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight) + factor as int * variant_weight(rows@.take(variant as int), weight),
        decreases rows.len() - variant,
    {
        let copies = match mul(rows[variant].count, factor) { Some(value) => value, None => return false };
        if copies < 0 || copies > usize::MAX as i128 { return false; }
        let copies = copies as usize;
        if out.len().checked_add(copies).is_none() { return false; }
        let ghost before = out_view(out@);
        let ghost row = OutRow { s_name: rows@[variant as int].name@, s_address: rows@[variant as int].address@ };
        proof { assert(copies as int == rows@[variant as int].count as int * factor as int); }
        let mut copy = 0;
        while copy < copies
            invariant copy <= copies, variant < rows.len(),
                row == (OutRow { s_name: rows@[variant as int].name@, s_address: rows@[variant as int].address@ }),
                copies as int == rows@[variant as int].count as int * factor as int,
                forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(before, weight) == sum(initial, weight) + factor as int * variant_weight(rows@.take(variant as int), weight),
                forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(before, weight) + copy as int * weight(row),
            decreases copies - copy,
        {
            let ghost previous = out_view(out@);
            out.push((copy_bytes(&rows[variant].name), copy_bytes(&rows[variant].address)));
            proof {
                assert(out_view(out@) =~= previous.push(row));
                assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(before, weight) + (copy as int + 1) * weight(row) by {
                    verus_common::sums::lemma_sum_push(previous, row, weight);
                    assert(sum(previous, weight) == sum(before, weight) + copy as int * weight(row));
                    verus_prooflib::lemma_scalar_distribute(copy as int, 1, weight(row));
                }
            }
            copy += 1;
        }
        proof {
            assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight) + factor as int * variant_weight(rows@.take(variant as int + 1), weight) by {
                let term = |item: SupplierVariant| item.count as int * weight(OutRow { s_name: item.name@, s_address: item.address@ });
                lemma_sum_take(rows@, variant as int, term);
                lemma_variant_take(rows@, variant as int, weight);
                assert(sum(before, weight) == sum(initial, weight) + factor as int * variant_weight(rows@.take(variant as int), weight));
                assert(sum(out_view(out@), weight) == sum(before, weight) + copies as int * weight(row));
                verus_prooflib::lemma_count_weight_assoc(factor as int, rows@[variant as int].count as int, weight(row));
                let count = rows@[variant as int].count as int;
                let multiplier = factor as int;
                let number = copies as int;
                let mass = weight(row);
                assert(number == count * multiplier);
                assert(number * mass == multiplier * (count * mass)) by (nonlinear_arith)
                    requires number == count * multiplier;
                let previous_total = variant_weight(rows@.take(variant as int), weight);
                let next_total = variant_weight(rows@.take(variant as int + 1), weight);
                assert(weight(row) == weight(OutRow { s_name: rows@[variant as int].name@, s_address: rows@[variant as int].address@ }));
                assert(next_total == previous_total + count * mass);
                assert(multiplier * next_total == multiplier * previous_total + number * mass) by (nonlinear_arith)
                    requires number == count * multiplier, next_total == previous_total + count * mass;
                verus_prooflib::lemma_scalar_distribute(variant_weight(rows@.take(variant as int), weight), term(rows@[variant as int]), factor as int);
            }
        }
        variant += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    true
}
fn emit_bucket(state: &State, bucket: &Table<Vec<SupplierVariant>>, nation: i64, factor: i128, start: i32, end: i32,
    Ghost(parameters): Ghost<Params>, qualified: &mut HashMap<i64, bool>, out: &mut Vec<(Vec<u8>, Vec<u8>)>) -> (result: bool)
    requires parameters.p1 == start, parameters.p3 == end, start < end,
        state.data.suppliers.lookup(nation) == Some(*bucket),
        qualification_cache(old(qualified)@, state.data.parts, state.data.stock_pairs, start as int, end as int),
    ensures qualification_cache(final(qualified)@, state.data.parts, state.data.stock_pairs, start as int, end as int),
        result ==> forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(final(out)@), weight) ==
            sum(out_view(old(out)@), weight) + factor as int * nation_weight(state.db(), parameters, nation as int, weight),
{
    proof { use_type_invariant(state); }
    let ghost db = state.db();
    let ghost initial = out_view(out@);
    let keys = bucket.keys();
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), keys@.no_duplicates(),
            forall|key: i64| #![trigger bucket.lookup(key)] keys@.contains(key) == bucket.lookup(key).is_some(),
            meaning(db, state.data.parts, state.data.stock_pairs, state.data.suppliers, state.data.nations), domain_valid(db), db == state.db(),
            state.data.suppliers.lookup(nation) == Some(*bucket), parameters.p1 == start, parameters.p3 == end, start < end,
            qualification_cache(qualified@, state.data.parts, state.data.stock_pairs, start as int, end as int),
            forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight)
                + factor as int * bucket_weight(state.data.parts, state.data.stock_pairs, *bucket, keys@.take(index as int), parameters, weight),
        decreases keys.len() - index,
    {
        let key = keys[index];
        let valid = match qualified.get(&key) {
            Some(value) => *value,
            None => {
                let value = match qualifies(&state.data, key, start, end) { Some(value) => value, None => return false };
                qualified.insert(key, value); value
            },
        };
        let ghost before = out_view(out@);
        proof { assert(valid == cached_supplier_passes(state.data.parts, state.data.stock_pairs, key, parameters.p1, parameters.p3)); }
        if valid {
            let rows = match bucket.get(key) { Some(value) => value, None => return false };
            if !emit_rows(out, &rows, factor) { return false; }
            proof {
                assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(before, weight) + factor as int * variant_weight(bucket_rows(*bucket, key), weight) by {
                    assert(bucket_rows(*bucket, key) == rows@);
                }
            }
        }
        proof {
            assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight)
                + factor as int * bucket_weight(state.data.parts, state.data.stock_pairs, *bucket, keys@.take(index as int + 1), parameters, weight) by {
                let term = |key: i64| if cached_supplier_passes(state.data.parts, state.data.stock_pairs, key, parameters.p1, parameters.p3) { variant_weight(bucket_rows(*bucket, key), weight) } else { 0int };
                lemma_sum_take(keys@, index as int, term);
                lemma_bucket_take(state.data.parts, state.data.stock_pairs, *bucket, keys@, index as int, parameters, weight);
                assert(sum(out_view(out@), weight) == sum(before, weight) + factor as int * term(key));
                assert(sum(before, weight) == sum(initial, weight) + factor as int * bucket_weight(state.data.parts, state.data.stock_pairs, *bucket, keys@.take(index as int), parameters, weight));
                verus_prooflib::lemma_scalar_distribute(bucket_weight(state.data.parts, state.data.stock_pairs, *bucket, keys@.take(index as int), parameters, weight), term(key), factor as int);
            }
        }
        index += 1;
    }
    proof {
        assert(keys@.take(keys.len() as int) =~= keys@);
        assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight) + factor as int * nation_weight(db, parameters, nation as int, weight) by {
            lemma_bucket(db, parameters, state.data.parts, state.data.stock_pairs, state.data.suppliers, state.data.nations, nation, *bucket, keys@, weight);
            assert(sum(out_view(out@), weight) == sum(initial, weight) + factor as int * bucket_weight(state.data.parts, state.data.stock_pairs, *bucket, keys@, parameters, weight));
        }
    }
    true
}
pub fn execute(state: &State, p1: i32, p2: &[u8], p3: i32) -> (result: Option<Vec<(Vec<u8>, Vec<u8>)>>)
    ensures result matches Some(out) ==> query_result(state.db(), params(p1 as int, p2@, p3 as int), out_view(out@)),
{
    let ghost db = state.db();
    let ghost parameters = params(p1 as int, p2@, p3 as int);
    proof { use_type_invariant(state); }
    let owner = state;
    let state = &owner.data;
    let mut out = Vec::new();
    if p1 >= p3 {
        proof {
            lemma_empty_interval(db, parameters);
            assert(out_view(out@) =~= Seq::<OutRow>::empty());
            assert(query_result(db, parameters, out_view(out@)));
        }
        return Some(out);
    }
    let nations = match state.nations.get(hash(p2)) {
        Some(value) => value,
        None => {
            proof {
                lemma_absent_nation(db, state.nations, parameters);
                assert(out_view(out@) =~= Seq::<OutRow>::empty());
                assert(query_result(db, parameters, out_view(out@)));
            }
            return Some(out);
        },
    };
    let mut qualified: HashMap<i64, bool> = HashMap::new(); let mut nation_index = 0;
    while nation_index < nations.len()
        invariant nation_index <= nations.len(), *state == owner.data, db == owner.db(), parameters == params(p1 as int, p2@, p3 as int), p1 < p3,
            meaning(db, state.parts, state.stock_pairs, state.suppliers, state.nations), domain_valid(db),
            state.nations.lookup(name_hash(p2@) as i64) == Some(*nations),
            qualification_cache(qualified@, state.parts, state.stock_pairs, p1 as int, p3 as int),
            forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == nation_output_weight(db, parameters, nations@.take(nation_index as int), weight),
        decreases nations.len() - nation_index,
    {
        let nation = &nations[nation_index];
        let ghost before = out_view(out@);
        if compare(&nation.name, p2) == 0 {
            if let Some(bucket) = state.suppliers.get(nation.key) {
                if !emit_bucket(owner, &bucket, nation.key, nation.count, p1, p3, Ghost(parameters), &mut qualified, &mut out) { return None; }
            } else {
                proof {
                    assert forall|weight: spec_fn(OutRow) -> int| #[trigger] nation_weight(db, parameters, nation.key as int, weight) == 0 by {
                        lemma_missing_bucket(db, parameters, state.suppliers, nation.key, weight);
                    }
                }
            }
        }
        proof {
            assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == nation_output_weight(db, parameters, nations@.take(nation_index as int + 1), weight) by {
                let term = |row: NationVariant| if row.name@ == parameters.p2 { row.count as int * nation_weight(db, parameters, row.key as int, weight) } else { 0int };
                lemma_sum_take(nations@, nation_index as int, term);
                assert(sum(before, weight) == nation_output_weight(db, parameters, nations@.take(nation_index as int), weight));
                if nation.name@ == p2@ {
                    if state.suppliers.lookup(nation.key).is_none() { assert(nation_weight(db, parameters, nation.key as int, weight) == 0); }
                }
            }
        }
        nation_index += 1;
    }
    proof {
        assert(nations@.take(nations.len() as int) =~= nations@);
        assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(crate::gen_spec::rows(db, parameters), weight) by {
            lemma_nation_rows(db, parameters, state.nations, nations@, weight);
            assert(sum(out_view(out@), weight) == nation_output_weight(db, parameters, nations@, weight));
        }
        verus_prooflib::lemma_weighted_bag_equal(out_view(out@), crate::gen_spec::rows(db, parameters));
    }
    let sorted = merge_sort(out);
    proof { crate::semantic_proofs::lemma_execute_result(db, parameters, out_view(sorted@)); }
    Some(sorted)
}
pub fn contains(keys: &[i64], key: i64) -> (result: bool)
    ensures result == keys@.contains(key),
{
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), forall|offset: int| #![trigger keys@[offset]] 0 <= offset < index ==> keys@[offset] != key,
        decreases keys.len() - index,
    {
        if keys[index] == key { return true; } index += 1;
    }
    false
}
fn part_changes(state: &Data, deltas: &[PartDelta]) -> (result: Option<(HashMap<i64, i128>, Vec<i64>)>)
    ensures result matches Some(data) ==> data.1@.no_duplicates() && data.0@.dom() == data.1@.to_set()
        && data.1@.to_set() == deltas@.map_values(|row: PartDelta| row.p_partkey).to_set()
        && forall|key: i64| #![trigger coalesced_part_count(state.parts, data.0@, key)]
            coalesced_part_count(state.parts, data.0@, key) == part_count(state.parts, key)
                + sum(deltas@, crate::semantic_proofs::part_capture_weight(key as int)),
{
    let mut counts: HashMap<i64, i128> = HashMap::new(); let mut keys: Vec<i64> = Vec::new(); let mut index = 0;
    while index < deltas.len()
        invariant index <= deltas.len(), keys@.no_duplicates(), counts@.dom() == keys@.to_set(),
            keys@.to_set() == deltas@.take(index as int).map_values(|row: PartDelta| row.p_partkey).to_set(),
            forall|key: i64| #![trigger coalesced_part_count(state.parts, counts@, key)]
                coalesced_part_count(state.parts, counts@, key) == part_count(state.parts, key)
                    + sum(deltas@.take(index as int), crate::semantic_proofs::part_capture_weight(key as int)),
        decreases deltas.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous = counts@;
        let ghost support = keys@;
        let row = deltas[index]; let key = row.p_partkey;
        let before = match counts.remove(&key) {
            Some(value) => value,
            None => {
                proof {
                    support.to_set_ensures();
                    lemma_unique_push(keys@, key);
                    lemma_push_membership(keys@, key);
                    verus_prooflib::lemma_push_to_set(keys@, key);
                }
                keys.push(key); match state.parts.get(key) { Some(part) => part.count, None => 0 }
            },
        };
        proof { assert(before as int == coalesced_part_count(state.parts, previous, key)); }
        let change = if row.is_qualifying != 0 { row.sign as i128 } else { 0 };
        let after = match add(before, change) { Some(value) => value, None => return None };
        counts.insert(key, after);
        proof {
            assert(counts@ == previous.insert(key, after));
            if previous.contains_key(key) {
                assert(keys@ == support);
                assert(previous.dom().insert(key) == previous.dom());
            } else { assert(keys@ == support.push(key)); }
            assert(counts@.dom() =~= keys@.to_set());
            let processed_keys = deltas@.take(index as int).map_values(|row: PartDelta| row.p_partkey);
            assert(deltas@.take(index as int + 1).map_values(|row: PartDelta| row.p_partkey) =~= processed_keys.push(key));
            verus_prooflib::lemma_push_to_set(processed_keys, key);
            assert(keys@.to_set() == processed_keys.push(key).to_set());
            assert forall|other: i64| #![trigger coalesced_part_count(state.parts, counts@, other)]
                coalesced_part_count(state.parts, counts@, other) == part_count(state.parts, other)
                    + sum(deltas@.take(index as int + 1), crate::semantic_proofs::part_capture_weight(other as int)) by {
                lemma_sum_take(deltas@, index as int, crate::semantic_proofs::part_capture_weight(other as int));
                assert(coalesced_part_count(state.parts, previous, other) == part_count(state.parts, other)
                    + sum(deltas@.take(index as int), crate::semantic_proofs::part_capture_weight(other as int)));
                if other != key { assert(coalesced_part_count(state.parts, counts@, other) == coalesced_part_count(state.parts, previous, other)); }
            }
        }
        index += 1;
    }
    proof { assert(deltas@.take(deltas.len() as int) =~= deltas@); }
    Some((counts, keys))
}
pub fn activation_keys(state: &Data, part_delta: &[PartDelta]) -> Vec<i64> {
    let (counts, keys) = match part_changes(state, part_delta) { Some(value) => value, None => return Vec::new() };
    let mut result = Vec::new(); let mut index = 0;
    while index < keys.len() decreases keys.len() - index, {
        let key = keys[index];
        if state.parts.get(key).is_none() {
            if let Some(count) = counts.get(&key) { if *count > 0 { result.push(key); } }
        }
        index += 1;
    }
    result
}
pub fn rescan_stocks_for_parts_keys(
    state: &State, lineitem_delta: &[LineitemDelta], nation_delta: &Vec<NationDelta>, part_delta: &[PartDelta],
    partsupp_delta: &[PartsuppDelta], supplier_delta: &Vec<SupplierDelta>,
) -> Vec<i64> { activation_keys(&state.data, part_delta) }
pub fn rescan_shipments_for_parts_keys(
    state: &State, lineitem_delta: &[LineitemDelta], nation_delta: &Vec<NationDelta>, part_delta: &[PartDelta],
    partsupp_delta: &[PartsuppDelta], supplier_delta: &Vec<SupplierDelta>,
) -> Vec<i64> { activation_keys(&state.data, part_delta) }
fn touch(state: &Data, key: i64, counts: &HashMap<i64, i128>, work: &mut HashMap<i64, PartWork>, keys: &mut Vec<i64>) -> (result: bool)
    ensures result && parts_shape(state.parts) && work_ready(old(work)@, old(keys)@, state.parts, counts@) ==>
        work_ready(final(work)@, final(keys)@, state.parts, counts@),
        result ==> final(work)@.contains_key(key) == (old(work)@.contains_key(key) || coalesced_part_count(state.parts, counts@, key) > 0),
        result ==> forall|other: i64| #![trigger final(work)@.contains_key(other)] other != key ==> final(work)@.contains_key(other) == old(work)@.contains_key(other),
        result && parts_shape(state.parts) ==> forall|part: i64, supplier: i64, value: int| #[trigger] combined_stock(final(work)@, state.parts, part, supplier, value)
            == combined_stock(old(work)@, state.parts, part, supplier, value),
        result && parts_shape(state.parts) ==> forall|part: i64, supplier: i64, date: int, quantity: bool| #[trigger] combined_day(final(work)@, state.parts, part, supplier, date, quantity)
            == combined_day(old(work)@, state.parts, part, supplier, date, quantity),
{
    let ghost previous = work@;
    let ghost previous_keys = keys@;
    if work.contains_key(&key) { return true; }
    let before = state.parts.get(key);
    let count = match counts.get(&key) { Some(value) => *value, None => match &before { Some(value) => value.count, None => 0 } };
    if count <= 0 { return true; }
    let mut part = match before {
        Some(value) => {
            proof {
                if parts_shape(state.parts) {
                    assert(state.parts.lookup(key) == Some(*value));
                    assert(crate::profile_proofs::part_shape(*value));
                }
            }
            match PartWork::from_part(&value) { Some(value) => value, None => return false }
        },
        None => PartWork::new(count),
    };
    let ghost loaded = part;
    part.count = count;
    let ghost replacement = part;
    proof {
        if parts_shape(state.parts) && work_ready(previous, previous_keys, state.parts, counts@) {
            previous_keys.to_set_ensures();
            assert(!previous_keys.contains(key));
            verus_prooflib::lemma_unique_push(keys@, key);
            verus_prooflib::lemma_push_to_set(keys@, key);
        }
    }
    work.insert(key, part); keys.push(key);
    proof {
        if parts_shape(state.parts) && work_ready(previous, previous_keys, state.parts, counts@) {
            assert(work@.dom() =~= keys@.to_set());
            assert(replacement.count == coalesced_part_count(state.parts, counts@, key));
        }
        if parts_shape(state.parts) {
            assert forall|part: i64, supplier: i64, value: int| #[trigger] combined_stock(work@, state.parts, part, supplier, value) == combined_stock(previous, state.parts, part, supplier, value) by {
                if part == key {
                    if let Some(source) = state.parts.lookup(key) {
                        assert(loaded.stock_for(supplier, value) == crate::profile_proofs::part_stock(source.pairs@, supplier, value));
                        assert(replacement.stock_for(supplier, value) == loaded.stock_for(supplier, value));
                        assert(replacement.stock_for(supplier, value) == crate::profile_proofs::part_stock(source.pairs@, supplier, value));
                    }
                }
            }
            assert forall|part: i64, supplier: i64, date: int, quantity: bool| #[trigger] combined_day(work@, state.parts, part, supplier, date, quantity) == combined_day(previous, state.parts, part, supplier, date, quantity) by {
                if part == key {
                    if let Some(source) = state.parts.lookup(key) {
                        assert(loaded.day_for(supplier, date, quantity) == crate::profile_proofs::part_day(source.pairs@, supplier, date, quantity));
                        assert(replacement.day_for(supplier, date, quantity) == loaded.day_for(supplier, date, quantity));
                        assert(replacement.day_for(supplier, date, quantity) == crate::profile_proofs::part_day(source.pairs@, supplier, date, quantity));
                    }
                }
            }
        }
    }
    true
}
pub fn apply(
    state: &State, lineitem_delta: &[LineitemDelta], nation_delta: &Vec<NationDelta>, part_delta: &[PartDelta],
    partsupp_delta: &[PartsuppDelta], supplier_delta: &Vec<SupplierDelta>,
    stocks_for_parts_keys: &[i64], stocks_for_parts_rows: &[StockInput],
    shipments_for_parts_keys: &[i64], shipments_for_parts_rows: &[ShipmentInput],
    Ghost(part_full): Ghost<Seq<(PartRow, int)>>, Ghost(db2): Ghost<Db>,
) -> (result: Result<State, u32>)
    requires part_delta_ok(part_delta@, part_full),
        capture_ok(state.db(), db2, lineitem_delta_seq_view(lineitem_delta@), nation_delta_seq_view(nation_delta@), part_full, partsupp_delta_seq_view(partsupp_delta@), supplier_delta_seq_view(supplier_delta@)),
        rescan_stocks_for_parts_ok(db2, stocks_for_parts_keys@, stocks_for_parts_rows@),
        rescan_shipments_for_parts_ok(db2, shipments_for_parts_keys@, shipments_for_parts_rows@),
    ensures result matches Ok(next) ==> next.db() == db2,
{
    let ghost previous_db = state.db();
    proof { use_type_invariant(state); }
    let state = &state.data;
    let (counts, changed) = match part_changes(state, part_delta) { Some(value) => value, None => return Err(1) };
    proof {
        crate::semantic_proofs::lemma_coalesced_after(previous_db, db2, part_delta@, part_full, state.parts, counts@);
        crate::state_proofs::lemma_meaning_components(previous_db, state.parts, state.stock_pairs, state.suppliers, state.nations);
    }
    let mut work = HashMap::new(); let mut keys = Vec::new(); let mut activated: HashMap<i64, bool> = HashMap::new();
    let mut next = Data { parts: state.parts.share(), stock_pairs: state.stock_pairs.share(), suppliers: state.suppliers.share(), nations: state.nations.share() };
    let ghost initial_suppliers = next.suppliers;
    let ghost initial_nations = next.nations;
    proof {
        lemma_dimensions_share(previous_db, state.suppliers, state.nations, initial_suppliers, initial_nations);
        assert forall|supplier: i64, key: i64| #[trigger] directory_contains(next.stock_pairs, supplier, key) == match next.parts.lookup(key) { Some(part) => stock_support(part.pairs@, supplier), None => false } by {
            assert(next.parts.lookup(key) == state.parts.lookup(key));
            assert(next.stock_pairs.lookup(supplier) == state.stock_pairs.lookup(supplier));
            assert(directory_contains(state.stock_pairs, supplier, key) == match state.parts.lookup(key) { Some(part) => stock_support(part.pairs@, supplier), None => false });
        }
    }
    let mut index = 0;
    proof {
        keys@.to_set_ensures();
        assert(work@.dom() =~= keys@.to_set());
        assert(keys@.no_duplicates());
        assert(work_shape(work@));
    }
    while index < changed.len()
        invariant index <= changed.len(), changed@.no_duplicates(), counts@.dom() == changed@.to_set(), directory_unique(next.stock_pairs),
            meaning(previous_db, state.parts, state.stock_pairs, state.suppliers, state.nations), domain_valid(previous_db), parts_shape(state.parts),
            forall|key: i64| #![trigger state.parts.lookup(key)] part_correct(previous_db, key, state.parts.lookup(key)),
            next.suppliers == initial_suppliers, next.nations == initial_nations,
            supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations),
            directory_matches(next.parts, next.stock_pairs),
            forall|key: i64| #[trigger] next.parts.lookup(key) == if changed@.take(index as int).contains(key) && eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
            forall|key: i64| #![trigger work@.contains_key(key)] changed@.take(index as int).contains(key) && eligible(db2, key as int) > 0 ==> work@.contains_key(key),
            forall|part: i64, supplier: i64, value: int| #[trigger] combined_stock(work@, state.parts, part, supplier, value) == combined_stock(Map::empty(), state.parts, part, supplier, value),
            forall|part: i64, supplier: i64, date: int, quantity: bool| #[trigger] combined_day(work@, state.parts, part, supplier, date, quantity) == combined_day(Map::empty(), state.parts, part, supplier, date, quantity),
            part_delta_ok(part_delta@, part_full),
            capture_ok(previous_db, db2, lineitem_delta_seq_view(lineitem_delta@), nation_delta_seq_view(nation_delta@), part_full, partsupp_delta_seq_view(partsupp_delta@), supplier_delta_seq_view(supplier_delta@)),
            rescan_stocks_for_parts_ok(db2, stocks_for_parts_keys@, stocks_for_parts_rows@), rescan_shipments_for_parts_ok(db2, shipments_for_parts_keys@, shipments_for_parts_rows@),
            work_ready(work@, keys@, state.parts, counts@),
            forall|key: i64| #![trigger coalesced_part_count(state.parts, counts@, key)] coalesced_part_count(state.parts, counts@, key) == eligible(db2, key as int),
            forall|key: i64| #[trigger] activated@.contains_key(key) == (changed@.take(index as int).contains(key) && state.parts.lookup(key).is_none() && coalesced_part_count(state.parts, counts@, key) > 0),
            forall|key: i64| #![trigger activated@.contains_key(key)] activated@.contains_key(key) ==> stocks_for_parts_keys@.contains(key) && shipments_for_parts_keys@.contains(key),
        decreases changed.len() - index,
    {
        let ghost previous_activated = activated@;
        let ghost previous_parts = next.parts;
        let ghost previous_work = work@;
        proof { verus_prooflib::lemma_prefix_fresh(changed@, index as int); }
        let key = changed[index]; let count = match counts.get(&key) { Some(value) => *value, None => return Err(2) };
        if count < 0 { return Err(3); }
        if count == 0 { publish_part(&mut next, key, None); }
        else {
            if state.parts.get(key).is_none() {
                if !contains(stocks_for_parts_keys, key) || !contains(shipments_for_parts_keys, key) { return Err(4); }
                activated.insert(key, true);
            }
            if !touch(state, key, &counts, &mut work, &mut keys) { return Err(5); }
        }
        proof {
            assert(changed@.take(index as int + 1) =~= changed@.take(index as int).push(key));
            lemma_push_membership(changed@.take(index as int), key);
            assert(count as int == coalesced_part_count(state.parts, counts@, key));
            assert forall|other: i64| #[trigger] activated@.contains_key(other) == (changed@.take(index as int + 1).contains(other) && state.parts.lookup(other).is_none() && coalesced_part_count(state.parts, counts@, other) > 0) by {
                if other != key { assert(activated@.contains_key(other) == previous_activated.contains_key(other)); }
            }
            assert forall|other: i64| #[trigger] next.parts.lookup(other) == if changed@.take(index as int + 1).contains(other) && eligible(db2, other as int) == 0 { None } else { state.parts.lookup(other) } by {
                assert(previous_parts.lookup(other) == if changed@.take(index as int).contains(other) && eligible(db2, other as int) == 0 { None } else { state.parts.lookup(other) });
            }
            assert forall|other: i64| changed@.take(index as int + 1).contains(other) && eligible(db2, other as int) > 0 implies #[trigger] work@.contains_key(other) by {
                if other != key { assert(work@.contains_key(other) == previous_work.contains_key(other)); }
            }
        }
        index += 1;
    }
    proof {
        assert(changed@.take(changed.len() as int) =~= changed@);
        changed@.to_set_ensures();
        assert forall|key: i64| #![trigger eligible(db2, key as int)] eligible(db2, key as int) > 0 && eligible(previous_db, key as int) == 0 implies
            stocks_for_parts_keys@.contains(key) && shipments_for_parts_keys@.contains(key) by {
            assert(part_count(state.parts, key) == eligible(previous_db, key as int));
            assert(state.parts.lookup(key).is_none());
            assert(coalesced_part_count(state.parts, counts@, key) == eligible(db2, key as int));
            assert(counts@.contains_key(key));
            assert(changed@.contains(key));
            assert(activated@.contains_key(key));
        }
        crate::update_proofs::lemma_apply_domain(previous_db, db2, part_delta@, part_full, partsupp_delta@, lineitem_delta@, supplier_delta@, nation_delta@,
            stocks_for_parts_keys@, stocks_for_parts_rows@, shipments_for_parts_keys@, shipments_for_parts_rows@);
    }
    let ghost facts = FactUpdate { before: previous_db, after: db2, parts: state.parts, counts: counts@, activated: activated@,
        stocks: stocks_for_parts_rows@, ships: shipments_for_parts_rows@, stock_keys: stocks_for_parts_keys@, ship_keys: shipments_for_parts_keys@,
        stock_delta: partsupp_delta@, ship_delta: lineitem_delta@, supplier_delta: supplier_delta@, nation_delta: nation_delta@ };
    proof {
        assert forall|key: i64| #[trigger] activated@.contains_key(key) == (state.parts.lookup(key).is_none() && eligible(db2, key as int) > 0) by {
            if state.parts.lookup(key).is_none() && eligible(db2, key as int) > 0 {
                assert(coalesced_part_count(state.parts, counts@, key) == eligible(db2, key as int));
                assert(counts@.contains_key(key)); assert(changed@.contains(key));
            }
        }
        assert(facts.valid());
        assert forall|key: i64, supplier: i64, value: int| eligible(db2, key as int) > 0 implies #[trigger] combined_stock(work@, state.parts, key, supplier, value) == facts.stock(key, supplier, value, 0, 0) by {
            assert(combined_stock(work@, state.parts, key, supplier, value) == combined_stock(Map::empty(), state.parts, key, supplier, value));
            assert(part_correct(previous_db, key, state.parts.lookup(key)));
            if let Some(part) = state.parts.lookup(key) {
                assert(crate::profile_proofs::part_stock(part.pairs@, supplier, value) == stock_count(previous_db, key as int, supplier as int, value));
            }
        }
        assert forall|key: i64, supplier: i64, date: int, quantity: bool| eligible(db2, key as int) > 0 implies #[trigger] combined_day(work@, state.parts, key, supplier, date, quantity) == facts.day(key, supplier, date, quantity, 0, 0) by {
            assert(combined_day(work@, state.parts, key, supplier, date, quantity) == combined_day(Map::empty(), state.parts, key, supplier, date, quantity));
            assert(part_correct(previous_db, key, state.parts.lookup(key)));
            if let Some(part) = state.parts.lookup(key) {
                assert(crate::profile_proofs::part_day(part.pairs@, supplier, date, false) == daily_count(previous_db, key as int, supplier as int, date));
                assert(crate::profile_proofs::part_day(part.pairs@, supplier, date, true) == daily_quantity(previous_db, key as int, supplier as int, date));
            }
        }
        assert(facts.progress(work@, 0, 0, 0, 0));
        assert(facts.coverage(work@, 0, 0));
        assert forall|key: i64| #[trigger] next.parts.lookup(key) == if eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) } by {
            if !changed@.contains(key) { assert(!counts@.contains_key(key)); assert(coalesced_part_count(state.parts, counts@, key) == eligible(db2, key as int)); assert(eligible(db2, key as int) == part_count(state.parts, key)); }
        }
    }
    let mut index = 0;
    while index < stocks_for_parts_rows.len() invariant index <= stocks_for_parts_rows.len(), directory_unique(next.stock_pairs), domain_valid(db2),
        supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations),
        facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@,
        facts.valid(), facts.parts == state.parts, facts.counts == counts@, facts.activated == activated@, facts.after == db2,
        facts.stocks == stocks_for_parts_rows@, facts.ships == shipments_for_parts_rows@, facts.stock_delta == partsupp_delta@, facts.ship_delta == lineitem_delta@,
        facts.progress(work@, index as int, 0, 0, 0), facts.coverage(work@, 0, 0),
        next.suppliers == initial_suppliers, next.nations == initial_nations, directory_matches(next.parts, next.stock_pairs),
        forall|key: i64| #[trigger] next.parts.lookup(key) == if eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
        parts_shape(state.parts), work_ready(work@, keys@, state.parts, counts@), decreases stocks_for_parts_rows.len() - index, {
        let ghost processed = index as int;
        let ghost before = work@;
        let row = &stocks_for_parts_rows[index];
        if activated.contains_key(&row.ps_partkey) && !stock_change(&mut work, row, 1) { return Err(6); }
        proof {
            lemma_day_frame(before, work@, state.parts);
            assert forall|key: i64, supplier: i64, value: int| eligible(db2, key as int) > 0 implies
                #[trigger] combined_stock(work@, state.parts, key, supplier, value) == combined_stock(before, state.parts, key, supplier, value)
                    + if activated@.contains_key(key) { stock_weight(key, supplier, value)(*row) } else { 0 } by {
                if activated@.contains_key(row.ps_partkey) {
                    assert(before.contains_key(row.ps_partkey));
                    assert(work_stock_count(work@, key, supplier, value) == work_stock_count(before, key, supplier, value)
                        + if key == row.ps_partkey && supplier == row.ps_suppkey && value == row.ps_availqty { 1int } else { 0int });
                }
            }
            facts.stock_step(before, work@, processed, 0, 0, 0, true);
        }
        index += 1;
    }
    let mut index = 0;
    while index < shipments_for_parts_rows.len() invariant index <= shipments_for_parts_rows.len(), directory_unique(next.stock_pairs), domain_valid(db2),
        supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations),
        facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@,
        facts.valid(), facts.parts == state.parts, facts.counts == counts@, facts.activated == activated@, facts.after == db2,
        facts.stocks == stocks_for_parts_rows@, facts.ships == shipments_for_parts_rows@, facts.stock_delta == partsupp_delta@, facts.ship_delta == lineitem_delta@,
        facts.progress(work@, facts.stocks.len() as int, index as int, 0, 0), facts.coverage(work@, 0, 0),
        next.suppliers == initial_suppliers, next.nations == initial_nations, directory_matches(next.parts, next.stock_pairs),
        forall|key: i64| #[trigger] next.parts.lookup(key) == if eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
        parts_shape(state.parts), work_ready(work@, keys@, state.parts, counts@), decreases shipments_for_parts_rows.len() - index, {
        let ghost processed = index as int;
        let ghost before = work@;
        let row = &shipments_for_parts_rows[index];
        if activated.contains_key(&row.l_partkey) && !shipment_change(&mut work, row, 1) { return Err(7); }
        proof {
            lemma_stock_frame(before, work@, state.parts);
            assert forall|key: i64, supplier: i64, date: int, quantity: bool| eligible(db2, key as int) > 0 implies
                #[trigger] combined_day(work@, state.parts, key, supplier, date, quantity) == combined_day(before, state.parts, key, supplier, date, quantity)
                    + if activated@.contains_key(key) { ship_weight(key, supplier, date, quantity)(*row) } else { 0 } by {
                if activated@.contains_key(row.l_partkey) {
                    assert(before.contains_key(row.l_partkey));
                    assert(work_day_count(work@, key, supplier, date) == work_day_count(before, key, supplier, date)
                        + if key == row.l_partkey && supplier == row.l_suppkey && date == row.l_shipdate { 1int } else { 0int });
                    assert(work_day_quantity(work@, key, supplier, date) == work_day_quantity(before, key, supplier, date)
                        + if key == row.l_partkey && supplier == row.l_suppkey && date == row.l_shipdate { row.l_quantity as int } else { 0 });
                }
            }
            facts.ship_step(before, work@, facts.stocks.len() as int, processed, 0, 0, true);
        }
        index += 1;
    }
    let mut index = 0;
    while index < partsupp_delta.len() invariant index <= partsupp_delta.len(), directory_unique(next.stock_pairs), domain_valid(db2),
        supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations),
        facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@,
        facts.valid(), facts.parts == state.parts, facts.counts == counts@, facts.activated == activated@, facts.after == db2,
        facts.stocks == stocks_for_parts_rows@, facts.ships == shipments_for_parts_rows@, facts.stock_delta == partsupp_delta@, facts.ship_delta == lineitem_delta@,
        facts.progress(work@, facts.stocks.len() as int, facts.ships.len() as int, index as int, 0), facts.coverage(work@, index as int, 0),
        next.suppliers == initial_suppliers, next.nations == initial_nations, directory_matches(next.parts, next.stock_pairs),
        forall|key: i64| #[trigger] next.parts.lookup(key) == if eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
        parts_shape(state.parts), work_ready(work@, keys@, state.parts, counts@), decreases partsupp_delta.len() - index, {
        let ghost processed = index as int;
        let ghost before = work@;
        let delta = &partsupp_delta[index]; let key = delta.row.ps_partkey;
        if !activated.contains_key(&key) {
            if !touch(state, key, &counts, &mut work, &mut keys) { return Err(8); }
            let ghost loaded = work@;
            if work.contains_key(&key) && !stock_change(&mut work, &delta.row, delta.sign as i128) { return Err(9); }
            proof {
                lemma_day_frame(loaded, work@, state.parts);
                assert forall|part: i64, supplier: i64, value: int| eligible(db2, part as int) > 0 implies
                    #[trigger] combined_stock(work@, state.parts, part, supplier, value) == combined_stock(before, state.parts, part, supplier, value)
                        + if !activated@.contains_key(part) { stock_delta_weight(part, supplier, value)(*delta) } else { 0 } by {
                    assert(combined_stock(loaded, state.parts, part, supplier, value) == combined_stock(before, state.parts, part, supplier, value));
                    if loaded.contains_key(key) {
                        assert(work_stock_count(work@, part, supplier, value) == work_stock_count(loaded, part, supplier, value)
                            + if part == key && supplier == delta.row.ps_suppkey && value == delta.row.ps_availqty { delta.sign as int } else { 0 });
                    } else { assert(eligible(db2, key as int) <= 0); }
                }
            }
        }
        proof {
            facts.stock_step(before, work@, facts.stocks.len() as int, facts.ships.len() as int, processed, 0, false);
            assert(facts.coverage(work@, processed + 1, 0));
        }
        index += 1;
    }
    let mut index = 0;
    while index < lineitem_delta.len() invariant index <= lineitem_delta.len(), directory_unique(next.stock_pairs), domain_valid(db2),
        supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations),
        facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@,
        facts.valid(), facts.parts == state.parts, facts.counts == counts@, facts.activated == activated@, facts.after == db2,
        facts.stocks == stocks_for_parts_rows@, facts.ships == shipments_for_parts_rows@, facts.stock_delta == partsupp_delta@, facts.ship_delta == lineitem_delta@,
        facts.progress(work@, facts.stocks.len() as int, facts.ships.len() as int, facts.stock_delta.len() as int, index as int), facts.coverage(work@, facts.stock_delta.len() as int, index as int),
        next.suppliers == initial_suppliers, next.nations == initial_nations, directory_matches(next.parts, next.stock_pairs),
        forall|key: i64| #[trigger] next.parts.lookup(key) == if eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
        parts_shape(state.parts), work_ready(work@, keys@, state.parts, counts@), decreases lineitem_delta.len() - index, {
        let ghost processed = index as int;
        let ghost before = work@;
        let delta = &lineitem_delta[index]; let key = delta.row.l_partkey;
        if !activated.contains_key(&key) {
            if !touch(state, key, &counts, &mut work, &mut keys) { return Err(10); }
            let ghost loaded = work@;
            if work.contains_key(&key) && !shipment_change(&mut work, &delta.row, delta.sign as i128) { return Err(11); }
            proof {
                lemma_stock_frame(loaded, work@, state.parts);
                assert forall|part: i64, supplier: i64, date: int, quantity: bool| eligible(db2, part as int) > 0 implies
                    #[trigger] combined_day(work@, state.parts, part, supplier, date, quantity) == combined_day(before, state.parts, part, supplier, date, quantity)
                        + if !activated@.contains_key(part) { ship_delta_weight(part, supplier, date, quantity)(*delta) } else { 0 } by {
                    assert(combined_day(loaded, state.parts, part, supplier, date, quantity) == combined_day(before, state.parts, part, supplier, date, quantity));
                    if loaded.contains_key(key) {
                        assert(work_day_count(work@, part, supplier, date) == work_day_count(loaded, part, supplier, date)
                            + if part == key && supplier == delta.row.l_suppkey && date == delta.row.l_shipdate { delta.sign as int } else { 0 });
                        assert(work_day_quantity(work@, part, supplier, date) == work_day_quantity(loaded, part, supplier, date)
                            + if part == key && supplier == delta.row.l_suppkey && date == delta.row.l_shipdate { delta.sign as int * delta.row.l_quantity as int } else { 0 });
                        let sign = delta.sign as int;
                        let quantity_value = delta.row.l_quantity as int;
                        let contributes = part == key && supplier == delta.row.l_suppkey && date == delta.row.l_shipdate;
                        assert(sign * (if contributes { quantity_value } else { 0int }) == if contributes { sign * quantity_value } else { 0int }) by (nonlinear_arith);
                    } else { assert(eligible(db2, key as int) <= 0); }
                }
            }
        }
        proof {
            facts.ship_step(before, work@, facts.stocks.len() as int, facts.ships.len() as int, facts.stock_delta.len() as int, processed, false);
            assert(facts.coverage(work@, facts.stock_delta.len() as int, processed + 1));
        }
        index += 1;
    }
    let ghost built = work@;
    proof {
        assert forall|key: i64| !built.contains_key(key) implies part_correct(db2, key, #[trigger] next.parts.lookup(key)) by {
            facts.untouched(built, next.parts, key);
        }
    }
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), directory_matches(next.parts, next.stock_pairs), domain_valid(db2),
            facts.valid(), facts.after == db2, facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@, work_ready(built, keys@, facts.parts, facts.counts),
            facts.progress(built, facts.stocks.len() as int, facts.ships.len() as int, facts.stock_delta.len() as int, facts.ship_delta.len() as int),
            supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations),
            next.suppliers == initial_suppliers, next.nations == initial_nations,
            work@ == built.remove_keys(keys@.take(index as int).to_set()),
            forall|key: i64| #![trigger next.parts.lookup(key)] !built.contains_key(key) || keys@.take(index as int).contains(key) ==> part_correct(db2, key, next.parts.lookup(key)),
        decreases keys.len() - index,
    {
        let key = keys[index];
        let ghost previous_parts = next.parts;
        proof {
            verus_prooflib::lemma_prefix_fresh(keys@, index as int);
            keys@.to_set_ensures();
            assert(built.contains_key(key));
            assert(work@.contains_key(key));
        }
        let raw = match work.remove(&key) { Some(value) => value, None => return Err(12) };
        let ghost source = raw;
        let part = match raw.finish() { Some(value) => value, None => return Err(13) };
        proof { facts.finished_part(built, key, source, &part); }
        publish_part(&mut next, key, Some(part));
        proof {
            assert(keys@.take(index as int + 1) =~= keys@.take(index as int).push(key));
            lemma_push_membership(keys@.take(index as int), key);
            verus_prooflib::lemma_push_to_set(keys@.take(index as int), key);
            assert(work@ =~= built.remove_keys(keys@.take(index as int + 1).to_set()));
            assert forall|other: i64| !built.contains_key(other) || keys@.take(index as int + 1).contains(other) implies part_correct(db2, other, #[trigger] next.parts.lookup(other)) by {
                if other != key { assert(part_correct(db2, other, previous_parts.lookup(other))); }
            }
        }
        index += 1;
    }
    proof {
        assert(keys@.take(keys.len() as int) =~= keys@);
        keys@.to_set_ensures();
        assert forall|key: i64| part_correct(db2, key, #[trigger] next.parts.lookup(key)) by {
            if built.contains_key(key) { assert(keys@.contains(key)); }
        }
    }
    let ghost finished_parts = next.parts;
    let ghost finished_directory = next.stock_pairs;
    let mut supplier_work = SupplierWork::new(); let mut index = 0;
    while index < supplier_delta.len()
        invariant index <= supplier_delta.len(), supplier_work.shape(), domain_valid(db2),
            facts.valid(), facts.after == db2, facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@,
            supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations),
            next.parts == finished_parts, next.stock_pairs == finished_directory, directory_matches(next.parts, next.stock_pairs),
            forall|key: i64| #![trigger next.parts.lookup(key)] part_correct(db2, key, next.parts.lookup(key)),
            next.suppliers == initial_suppliers, next.nations == initial_nations,
            forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_work.count(initial_suppliers, nation, supplier, name, address) == supplier_count(initial_suppliers, nation, supplier, name, address)
                + sum(supplier_delta@.take(index as int), supplier_delta_weight(nation, supplier, name, address)),
        decreases supplier_delta.len() - index,
    {
        let ghost previous = supplier_work;
        let delta = &supplier_delta[index];
        if !supplier_work.change(&next.suppliers, &delta.row, delta.sign as i128) { return Err(14); }
        proof {
            assert forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_work.count(initial_suppliers, nation, supplier, name, address) == supplier_count(initial_suppliers, nation, supplier, name, address)
                + sum(supplier_delta@.take(index as int + 1), supplier_delta_weight(nation, supplier, name, address)) by {
                lemma_sum_take(supplier_delta@, index as int, supplier_delta_weight(nation, supplier, name, address));
                assert(previous.count(initial_suppliers, nation, supplier, name, address) == supplier_count(initial_suppliers, nation, supplier, name, address) + sum(supplier_delta@.take(index as int), supplier_delta_weight(nation, supplier, name, address)));
            }
        }
        index += 1;
    }
    let ghost supplier_source = supplier_work;
    proof { assert(supplier_delta@.take(supplier_delta.len() as int) =~= supplier_delta@); }
    if !supplier_work.publish(&mut next.suppliers) { return Err(15); }
    proof {
        lemma_supplier_updated(previous_db, db2, initial_suppliers, next.suppliers, supplier_delta@);
    }
    let ghost finished_suppliers = next.suppliers;
    let mut nation_work = NationWork::new(); let mut index = 0;
    while index < nation_delta.len()
        invariant index <= nation_delta.len(), nation_work.shape(), nation_work.hashes(), domain_valid(db2),
            facts.valid(), facts.after == db2, facts.before == previous_db, facts.nation_delta == nation_delta@,
            nation_correct(previous_db, initial_nations), next.nations == initial_nations,
            next.parts == finished_parts, next.stock_pairs == finished_directory, directory_matches(next.parts, next.stock_pairs),
            forall|key: i64| #![trigger next.parts.lookup(key)] part_correct(db2, key, next.parts.lookup(key)),
            next.suppliers == finished_suppliers, supplier_correct(db2, next.suppliers),
            forall|name: Seq<u8>, key: i64| #[trigger] nation_work.count(initial_nations, name, key) == nation_count(initial_nations, name, key)
                + sum(nation_delta@.take(index as int), nation_delta_weight(name, key)),
        decreases nation_delta.len() - index,
    {
        let ghost previous = nation_work;
        let delta = &nation_delta[index];
        if !nation_work.change(&next.nations, &delta.row, delta.sign as i128) { return Err(16); }
        proof {
            assert forall|name: Seq<u8>, key: i64| #[trigger] nation_work.count(initial_nations, name, key) == nation_count(initial_nations, name, key)
                + sum(nation_delta@.take(index as int + 1), nation_delta_weight(name, key)) by {
                lemma_sum_take(nation_delta@, index as int, nation_delta_weight(name, key));
                assert(previous.count(initial_nations, name, key) == nation_count(initial_nations, name, key) + sum(nation_delta@.take(index as int), nation_delta_weight(name, key)));
            }
        }
        index += 1;
    }
    let ghost nation_source = nation_work;
    proof { assert(nation_delta@.take(nation_delta.len() as int) =~= nation_delta@); }
    if !nation_work.publish(&mut next.nations) { return Err(17); }
    proof {
        lemma_nation_updated(previous_db, db2, initial_nations, next.nations, nation_delta@);
        use_type_invariant(&next.parts); next.parts.reveal_well_formed();
        use_type_invariant(&next.stock_pairs); next.stock_pairs.reveal_well_formed();
        use_type_invariant(&next.suppliers); next.suppliers.reveal_well_formed();
        use_type_invariant(&next.nations); next.nations.reveal_well_formed();
        crate::state_proofs::lemma_state_meaning(db2, next.parts, next.stock_pairs, next.suppliers, next.nations);
    }
    Ok(State { db: Ghost(db2), data: next })
}
}
