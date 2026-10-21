use vstd::prelude::*;
use std::sync::Arc;
use std::collections::HashMap;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::activation::*;
use crate::profiles::*;
use crate::dimensions::*;
use crate::model::*;
use crate::output::merge_sort;
use crate::query_proofs::*;
use crate::read_proofs::*;
use crate::update_proofs::*;
use crate::ready_proofs::*;
use verus_common::sums::sum;
use verus_prooflib::wide::Wide;
use verus_common::sums::lemma_sum_nonzero_witness;
use verus_common::sums::lemma_sum_pos;
use verus_common::sums::lemma_sum_nonneg;
use crate::profile_proofs::pair_lookup;
use verus_common::sums::bag_step;
use verus_common::sqlfns::like_match;
use verus_common::sums::lemma_sum_take;
use verus_prooflib::{lemma_unique_push, lemma_push_membership};

verus! {

pub open spec fn history(s: State) -> nat { 0 }

pub struct State {
    db: Ghost<Db>,
    data: Data,
}
pub struct Data {
    parts: Table<Part>,
    stock_pairs: Table<Vec<i64>>,
    suppliers: Suppliers,
    nations: Nations,
    names: PartNames,
}
impl State {
    pub closed spec fn db(self) -> Db { self.db@ }
    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        meaning(self.db@, self.data.parts, self.data.stock_pairs, self.data.suppliers, self.data.nations, self.data.names)
            && domain_valid(self.db@)
    }
}
pub fn stock_change(work: &mut HashMap<i64, PartWork>, row: &StockInput, sign: i128)
    requires old(work)@.contains_key(row.ps_partkey),
    ensures final(work)@.dom() == old(work)@.dom(),
        forall|key: i64| #![trigger final(work)@[key]] final(work)@.contains_key(key) ==> final(work)@[key].count == old(work)@[key].count,
        forall|key: i64| #[trigger] work_part_count(final(work)@, key) == work_part_count(old(work)@, key),
        empty_originals(old(work)@) ==> empty_originals(final(work)@),
        work_shape(old(work)@) ==> work_shape(final(work)@),
        (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, value: int|
            #[trigger] work_stock_count(final(work)@, part, supplier, value)
                == work_stock_count(old(work)@, part, supplier, value)
                    + if part == row.ps_partkey && supplier == row.ps_suppkey && value == row.ps_availqty { sign as int } else { 0 },
        (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, date: int|
            #[trigger] work_day_count(final(work)@, part, supplier, date) == work_day_count(old(work)@, part, supplier, date)
                && work_day_quantity(final(work)@, part, supplier, date) == work_day_quantity(old(work)@, part, supplier, date),
        (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, date: int|
            #[trigger] work_day_quantity(final(work)@, part, supplier, date) == work_day_quantity(old(work)@, part, supplier, date),
{
    let ghost before = work@;
    let mut part = match work.remove(&row.ps_partkey) { Some(value) => value, None => { proof { assert(false); } return; } };
    let mut pair = part.take_pair(row.ps_suppkey);
    let ghost previous_pair = pair;
    pair.stock(row.ps_availqty, sign);
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
}
pub fn shipment_change(work: &mut HashMap<i64, PartWork>, row: &ShipmentInput, sign: i128)
    requires old(work)@.contains_key(row.l_partkey),
    ensures final(work)@.dom() == old(work)@.dom(),
        forall|key: i64| #![trigger final(work)@[key]] final(work)@.contains_key(key) ==> final(work)@[key].count == old(work)@[key].count,
        forall|key: i64| #[trigger] work_part_count(final(work)@, key) == work_part_count(old(work)@, key),
        empty_originals(old(work)@) ==> empty_originals(final(work)@),
        work_shape(old(work)@) ==> work_shape(final(work)@),
        (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, date: int|
            #[trigger] work_day_count(final(work)@, part, supplier, date) == work_day_count(old(work)@, part, supplier, date)
                + if part == row.l_partkey && supplier == row.l_suppkey && date == row.l_shipdate { sign as int } else { 0 },
        (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, date: int|
            #[trigger] work_day_quantity(final(work)@, part, supplier, date) == work_day_quantity(old(work)@, part, supplier, date)
                + if part == row.l_partkey && supplier == row.l_suppkey && date == row.l_shipdate { (sign as int) * (row.l_quantity as int) } else { 0 },
        (empty_originals(old(work)@) || work_shape(old(work)@)) ==> forall|part: i64, supplier: i64, value: int|
            #[trigger] work_stock_count(final(work)@, part, supplier, value) == work_stock_count(old(work)@, part, supplier, value),
{
    let ghost before = work@;
    let mut part = match work.remove(&row.l_partkey) { Some(value) => value, None => { proof { assert(false); } return; } };
    let mut pair = part.take_pair(row.l_suppkey);
    let ghost previous_pair = pair;
    pair.shipment(row.l_shipdate, row.l_quantity, sign);
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
}
fn publish_part(state: &mut Data, key: i64, part: Option<Part>)
    ensures final(state).parts.lookup(key) == part,
        forall|other: i64| #![trigger final(state).parts.lookup(other)] other != key ==>
            final(state).parts.lookup(other) == old(state).parts.lookup(other),
        final(state).suppliers == old(state).suppliers, final(state).nations == old(state).nations, final(state).names == old(state).names,
        final(state).stock_pairs == old(state).stock_pairs,
{
    match part {
        None => state.parts.put(key, None),
        Some(data) => state.parts.put(key, Some(Arc::new(data))),
    }
}
pub fn build(
    parts: Vec<PartInput>, stocks: &[StockInput], shipments: &[ShipmentInput],
    suppliers: Vec<SupplierInput>, nations: Vec<NationInput>, Ghost(db): Ghost<Db>,
) -> (result: Option<State>)
    requires scan_parts_ok(db, parts@), scan_stocks_ok(db, stocks@),
        scan_shipments_ok(db, shipments@), scan_suppliers_ok(db, suppliers@), scan_nations_ok(db, nations@),
    ensures result matches Some(state) ==> state.db() == db,
        result is Some,
{
    let mut state = Data { parts: Table::new(), stock_pairs: Table::new(), suppliers: Table::new(), nations: Table::new(), names: Table::new() };
    let ghost initial_parts = state.parts;
    let ghost initial_directory = state.stock_pairs;
    let ghost initial_suppliers = state.suppliers;
    let ghost initial_nations = state.nations;
    let ghost initial_names = state.names;
    let ghost projected = crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@);
    proof {
        crate::state_proofs::lemma_build_domain(db, parts@, stocks@, shipments@, suppliers@, nations@);
        assert(dimensions_empty(initial_suppliers, initial_nations, initial_names));
    }
    let mut chunk: u64 = 0;
    while chunk < 16
        invariant chunk <= 16, domain_valid(db), dimensions_empty(initial_suppliers, initial_nations, initial_names),
            projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
            scan_parts_ok(db, parts@),
            directory_matches(state.parts, state.stock_pairs),
            state.suppliers == initial_suppliers, state.nations == initial_nations, state.names == initial_names,
            forall|key: i64| #![trigger state.parts.lookup(key)] chunk_of(key) < chunk as int ==>
                part_correct(projected, key, state.parts.lookup(key)),
            forall|key: i64| #![trigger state.parts.lookup(key)] chunk_of(key) >= chunk as int ==> state.parts.lookup(key).is_none(),
            forall|key: i64| #![trigger part_count(state.parts, key)] part_count(state.parts, key)
                == if chunk_of(key) < chunk as int { eligible(db, key as int) } else { 0 },
        decreases 16 - chunk,
    {
    let ghost chunk_parts = state.parts;
    let ghost chunk_directory = state.stock_pairs;
    let mut work: HashMap<i64, PartWork> = HashMap::new(); let mut keys = Vec::new(); let mut index = 0;
    while index < parts.len()
        invariant index <= parts.len(), work@.dom() == keys@.to_set(), keys@.no_duplicates(), chunk < 16,
            forall|key: i64| #![trigger work@[key]] work@.contains_key(key) ==> work@[key].count@ > 0,
            domain_valid(db), dimensions_empty(initial_suppliers, initial_nations, initial_names),
            work_shape(work@), state.parts == chunk_parts, state.stock_pairs == chunk_directory,
            projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
            state.suppliers == initial_suppliers, state.nations == initial_nations, state.names == initial_names,
            scan_parts_ok(db, parts@),
            forall|key: i64| #![trigger work_part_count(work@, key)]
                work_part_count(work@, key) == if chunk_of(key) == chunk as int { sum(parts@.take(index as int), |row: PartInput| if row.p_partkey == key { 1 } else { 0 }) } else { 0 },
            forall|key: i64| #![trigger work@[key]] work@.contains_key(key) ==>
                work@[key].pairs@.is_empty() && work@[key].original@.is_empty(),
            forall|key: i64| #![trigger keys@.contains(key)] keys@.contains(key) ==> chunk_of(key) == chunk as int,
        decreases parts.len() - index,
    {
        let ghost processed = index as int;
        let ghost before = work@;
        let ghost support = keys@;
        let key = parts[index].p_partkey;
        if (key as u64) % 16 == chunk {
            let mut part = match work.remove(&key) {
                Some(value) => value,
                None => {
                    proof { lemma_unique_push(keys@, key); lemma_push_membership(keys@, key); }
                    keys.push(key); PartWork::new(Wide::zero())
                },
            };
            proof { assert(part.count@ == work_part_count(before, key)); assert(chunk_of(key) == chunk as int); }
            proof { assert(part.count@ >= 0); }
            part.count = part.count.add(&Wide::from_i128(1));
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
                assert forall|other: i64| #![trigger keys@.contains(other)] keys@.contains(other) implies chunk_of(other) == chunk as int by {
                    if before.contains_key(key) { assert(keys@ == support); } else { assert(keys@ == support.push(key)); }
                }
                assert forall|other: i64| #![trigger work_part_count(work@, other)]
                    work_part_count(work@, other) == if chunk_of(other) == chunk as int { sum(parts@.take(index as int + 1), |row: PartInput| if row.p_partkey == other { 1 } else { 0 }) } else { 0 } by {
                    lemma_sum_take(parts@, index as int, |row: PartInput| if row.p_partkey == other { 1 } else { 0 });
                    if other != key {
                        assert(work_part_count(work@, other) == work_part_count(before, other));
                    }
                }
            }
        } else {
            proof {
                assert forall|other: i64| #![trigger work_part_count(work@, other)]
                    work_part_count(work@, other) == if chunk_of(other) == chunk as int { sum(parts@.take(index as int + 1), |row: PartInput| if row.p_partkey == other { 1 } else { 0 }) } else { 0 } by {
                    lemma_sum_take(parts@, index as int, |row: PartInput| if row.p_partkey == other { 1 } else { 0 });
                    if chunk_of(other) == chunk as int { assert(other != key); }
                }
            }
        }
        index += 1;
    }
    proof {
        assert(parts@.take(parts.len() as int) =~= parts@);
        assert forall|key: i64| #![trigger work_part_count(work@, key)] work_part_count(work@, key) == if chunk_of(key) == chunk as int { eligible(db, key as int) } else { 0 } by {
            crate::semantic_proofs::lemma_build_part_count(db, parts@, key as int);
        }
    }
    let mut index = 0;
    while index < stocks.len()
        invariant index <= stocks.len(), empty_originals(work@), chunk < 16,
            forall|key: i64| #![trigger work@[key]] work@.contains_key(key) ==> work@[key].count@ > 0,
            forall|key: i64| #![trigger keys@.contains(key)] keys@.contains(key) ==> chunk_of(key) == chunk as int,
            domain_valid(db), dimensions_empty(initial_suppliers, initial_nations, initial_names),
            work_shape(work@), state.parts == chunk_parts, state.stock_pairs == chunk_directory,
            projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
            state.suppliers == initial_suppliers, state.nations == initial_nations, state.names == initial_names,
            work@.dom() == keys@.to_set(), keys@.no_duplicates(),
            forall|key: i64| #![trigger work_part_count(work@, key)] work_part_count(work@, key) == if chunk_of(key) == chunk as int { eligible(db, key as int) } else { 0 },
            forall|part: i64, supplier: i64, value: int| #![trigger work_stock_count(work@, part, supplier, value)]
                chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_stock_count(work@, part, supplier, value) == sum(stocks@.take(index as int), |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }),
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(work@, part, supplier, date)]
                work_day_count(work@, part, supplier, date) == 0 && work_day_quantity(work@, part, supplier, date) == 0,
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_quantity(work@, part, supplier, date)]
                work_day_quantity(work@, part, supplier, date) == 0,
        decreases stocks.len() - index,
    {
        let ghost processed = index as int;
        let present = (stocks[index].ps_partkey as u64) % 16 == chunk && work.contains_key(&stocks[index].ps_partkey);
        if present { stock_change(&mut work, &stocks[index], 1); }
        proof {
            if !present {
                assert(work_part_count(work@, stocks@[index as int].ps_partkey) == 0) by {
                    if chunk_of(stocks@[index as int].ps_partkey) == chunk as int { assert(!work@.contains_key(stocks@[index as int].ps_partkey)); }
                }
            }
            assert forall|part: i64, supplier: i64, value: int| #![trigger work_stock_count(work@, part, supplier, value)]
                chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_stock_count(work@, part, supplier, value) == sum(stocks@.take(index as int + 1), |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }) by {
                lemma_sum_take(stocks@, index as int, |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 });
                if !present && chunk_of(part) == chunk as int && eligible(db, part as int) > 0 {
                    assert(work_part_count(work@, part) == eligible(db, part as int));
                    assert(part != stocks@[index as int].ps_partkey);
                }
            }
        }
        index += 1;
    }
    let mut index = 0;
    proof { assert(stocks@.take(stocks.len() as int) =~= stocks@); }
    while index < shipments.len()
        invariant index <= shipments.len(), empty_originals(work@), chunk < 16,
            forall|key: i64| #![trigger work@[key]] work@.contains_key(key) ==> work@[key].count@ > 0,
            forall|key: i64| #![trigger keys@.contains(key)] keys@.contains(key) ==> chunk_of(key) == chunk as int,
            domain_valid(db), dimensions_empty(initial_suppliers, initial_nations, initial_names),
            work_shape(work@), state.parts == chunk_parts, state.stock_pairs == chunk_directory,
            projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
            state.suppliers == initial_suppliers, state.nations == initial_nations, state.names == initial_names,
            forall|part: i64, supplier: i64, value: int| #![trigger work_stock_count(work@, part, supplier, value)]
                chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_stock_count(work@, part, supplier, value) == sum(stocks@, |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }),
            work@.dom() == keys@.to_set(), keys@.no_duplicates(),
            forall|key: i64| #![trigger work_part_count(work@, key)] work_part_count(work@, key) == if chunk_of(key) == chunk as int { eligible(db, key as int) } else { 0 },
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(work@, part, supplier, date)]
                chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_day_count(work@, part, supplier, date) == sum(shipments@.take(index as int), |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 }),
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_quantity(work@, part, supplier, date)]
                chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_day_quantity(work@, part, supplier, date) == sum(shipments@.take(index as int), |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }),
        decreases shipments.len() - index,
    {
        let ghost processed = index as int;
        let present = (shipments[index].l_partkey as u64) % 16 == chunk && work.contains_key(&shipments[index].l_partkey);
        if present { shipment_change(&mut work, &shipments[index], 1); }
        proof {
            if !present {
                assert(work_part_count(work@, shipments@[index as int].l_partkey) == 0) by {
                    if chunk_of(shipments@[index as int].l_partkey) == chunk as int { assert(!work@.contains_key(shipments@[index as int].l_partkey)); }
                }
            }
            assert forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(work@, part, supplier, date)]
                chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_day_count(work@, part, supplier, date) == sum(shipments@.take(index as int + 1), |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 }) by {
                lemma_sum_take(shipments@, index as int, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 });
                if !present && chunk_of(part) == chunk as int && eligible(db, part as int) > 0 {
                    assert(work_part_count(work@, part) == eligible(db, part as int));
                    assert(part != shipments@[index as int].l_partkey);
                }
            }
            assert forall|part: i64, supplier: i64, date: int| #![trigger work_day_quantity(work@, part, supplier, date)]
                chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_day_quantity(work@, part, supplier, date) == sum(shipments@.take(index as int + 1), |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }) by {
                lemma_sum_take(shipments@, index as int, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 });
                if !present && chunk_of(part) == chunk as int && eligible(db, part as int) > 0 {
                    assert(work_part_count(work@, part) == eligible(db, part as int));
                    assert(part != shipments@[index as int].l_partkey);
                }
            }
        }
        index += 1;
    }
    let mut index = 0;
    let ghost built = work@;
    proof {
        assert(shipments@.take(shipments.len() as int) =~= shipments@);
        assert forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(built, part, supplier, date)]
            chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_day_count(built, part, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 })
            && work_day_quantity(built, part, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }) by {
        }
        keys@.to_set_ensures();
        assert forall|key: i64| #![trigger keys@.contains(key)] !keys@.contains(key) && chunk_of(key) == chunk as int implies eligible(db, key as int) == 0 by {
            assert(!work@.contains_key(key));
            assert(work_part_count(work@, key) == eligible(db, key as int));
        }
    }
    while index < keys.len()
        invariant index <= keys.len(), keys@.no_duplicates(), chunk < 16, built.dom() == keys@.to_set(),
            forall|key: i64| #![trigger built[key]] built.contains_key(key) ==> built[key].count@ > 0,
            domain_valid(db), dimensions_empty(initial_suppliers, initial_nations, initial_names),
            work@ == built.remove_keys(keys@.take(index as int).to_set()), work_shape(built),
            projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
            directory_matches(state.parts, state.stock_pairs),
            state.suppliers == initial_suppliers, state.nations == initial_nations, state.names == initial_names,
            forall|key: i64| #![trigger keys@.contains(key)] keys@.contains(key) ==> chunk_of(key) == chunk as int,
            forall|key: i64| #![trigger state.parts.lookup(key)] chunk_of(key) < chunk as int || keys@.take(index as int).contains(key) ==>
                part_correct(projected, key, state.parts.lookup(key)),
            forall|key: i64| #![trigger state.parts.lookup(key)] chunk_of(key) >= chunk as int && !keys@.take(index as int).contains(key) ==> state.parts.lookup(key).is_none(),
            forall|part: i64, supplier: i64, value: int| #![trigger work_stock_count(built, part, supplier, value)]
                chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_stock_count(built, part, supplier, value) == sum(stocks@, |row: StockInput| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }),
            forall|part: i64, supplier: i64, date: int| #![trigger work_day_count(built, part, supplier, date)]
                chunk_of(part) == chunk as int && eligible(db, part as int) > 0 ==> work_day_count(built, part, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 })
                && work_day_quantity(built, part, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }),
            forall|key: i64| #![trigger keys@.contains(key)] !keys@.contains(key) && chunk_of(key) == chunk as int ==> eligible(db, key as int) == 0,
            forall|key: i64| #![trigger work_part_count(work@, key)] work_part_count(work@, key)
                == if keys@.take(index as int).contains(key) { 0 } else if chunk_of(key) == chunk as int { eligible(db, key as int) } else { 0 },
            forall|key: i64| #![trigger part_count(state.parts, key)] part_count(state.parts, key)
                == if keys@.take(index as int).contains(key) || chunk_of(key) < chunk as int { eligible(db, key as int) } else { 0 },
        decreases keys.len() - index,
    {
        let key = keys[index];
        let ghost previous_work = work@;
        let ghost previous_parts = state.parts;
        proof {
            verus_prooflib::lemma_prefix_fresh(keys@, index as int);
            assert(keys@.contains(key));
            assert(chunk_of(key) == chunk as int);
            assert(work_part_count(previous_work, key) == eligible(db, key as int));
        }
        proof {
            keys@.to_set_ensures();
            keys@.take(index as int).to_set_ensures();
            assert(built.contains_key(key));
            assert(!keys@.take(index as int).to_set().contains(key));
            assert(work@.contains_key(key));
        }
        let raw = match work.remove(&key) { Some(value) => value, None => { proof { assert(false); } return None; } };
        let ghost source = raw;
        proof {
            assert(raw.count@ == eligible(db, key as int));
            assert(source == built[key]);
            assert(source.count@ > 0);
            assert forall|supplier: i64, value: int| #[trigger] source.stock_for(supplier, value) >= 0 by {
                assert(source.stock_for(supplier, value) == work_stock_count(built, key, supplier, value));
                let weight = |row: StockInput| if row.ps_partkey == key && row.ps_suppkey == supplier && row.ps_availqty == value { 1int } else { 0int };
                assert forall|i: int| 0 <= i < stocks@.len() implies weight(#[trigger] stocks@[i]) >= 0 by {}
                lemma_sum_nonneg(stocks@, weight);
                assert(work_stock_count(built, key, supplier, value) == sum(stocks@, weight));
            }
            assert forall|supplier: i64, date: int| #![trigger source.day_for(supplier, date, false)]
                source.day_for(supplier, date, false) >= 0 && (source.day_for(supplier, date, false) == 0 ==> source.day_for(supplier, date, true) == 0) by {
                assert(source.day_for(supplier, date, false) == work_day_count(built, key, supplier, date));
                assert(source.day_for(supplier, date, true) == work_day_quantity(built, key, supplier, date));
                let count = |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0int };
                let quantity = |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0int };
                assert forall|row: ShipmentInput| #[trigger] count(row) >= 0 by {}
                assert forall|row: ShipmentInput| #![trigger quantity(row)] count(row) == 0 ==> quantity(row) == 0 by {}
                lemma_sum_nonneg(shipments@, count);
                assert(work_day_count(built, key, supplier, date) == sum(shipments@, count));
                assert(work_day_quantity(built, key, supplier, date) == sum(shipments@, quantity));
                if sum(shipments@, count) == 0 { crate::ready_proofs::lemma_empty_indicator(shipments@, count, quantity); }
            }
            crate::ready_proofs::lemma_part_finish_ready(source);
        }
        let part = match raw.finish() { Some(value) => value, None => { proof { assert(false); } return None; } };
        proof {
            assert(part.count@ > 0 && part.count == source.count);
            assert(eligible(db, key as int) > 0);
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
                assert(work_day_quantity(built, key, supplier, date) == sum(shipments@, |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }));
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
                == if keys@.take(index as int + 1).contains(other) { 0 } else if chunk_of(other) == chunk as int { eligible(db, other as int) } else { 0 } by {
                assert(work_part_count(previous_work, other) == if keys@.take(index as int).contains(other) { 0 } else if chunk_of(other) == chunk as int { eligible(db, other as int) } else { 0 });
            }
            assert forall|other: i64| #![trigger part_count(state.parts, other)] part_count(state.parts, other)
                == if keys@.take(index as int + 1).contains(other) || chunk_of(other) < chunk as int { eligible(db, other as int) } else { 0 } by {
                assert(part_count(previous_parts, other) == if keys@.take(index as int).contains(other) || chunk_of(other) < chunk as int { eligible(db, other as int) } else { 0 });
            }
            assert forall|other: i64| #![trigger state.parts.lookup(other)] chunk_of(other) < chunk as int || keys@.take(index as int + 1).contains(other) implies
                part_correct(projected, other, state.parts.lookup(other)) by {
                if other != key { assert(state.parts.lookup(other) == previous_parts.lookup(other)); }
            }
            assert forall|other: i64| #![trigger state.parts.lookup(other)] chunk_of(other) >= chunk as int && !keys@.take(index as int + 1).contains(other) implies
                state.parts.lookup(other).is_none() by {
                assert(other != key);
                assert(state.parts.lookup(other) == previous_parts.lookup(other));
            }
        }
        index += 1;
    }
    proof {
        assert(keys@.take(keys.len() as int) =~= keys@);
        assert forall|key: i64| #![trigger state.parts.lookup(key)] chunk_of(key) < chunk as int + 1 implies
            part_correct(projected, key, state.parts.lookup(key)) by {
            if chunk_of(key) == chunk as int && !keys@.contains(key) {
                assert(eligible(db, key as int) == 0);
                assert(state.parts.lookup(key).is_none());
            }
        }
        assert forall|key: i64| #![trigger state.parts.lookup(key)] chunk_of(key) >= chunk as int + 1 implies state.parts.lookup(key).is_none() by {
            assert(!keys@.contains(key));
        }
        assert forall|key: i64| #![trigger part_count(state.parts, key)] part_count(state.parts, key)
            == if chunk_of(key) < chunk as int + 1 { eligible(db, key as int) } else { 0 } by {
            if chunk_of(key) == chunk as int && !keys@.contains(key) { assert(eligible(db, key as int) == 0); }
        }
    }
    chunk += 1;
    }
    proof {
        assert forall|key: i64| #![trigger part_count(state.parts, key)] part_count(state.parts, key) == eligible(db, key as int) by {
            assert(chunk_of(key) < 16);
        }
        assert forall|key: i64| #![trigger state.parts.lookup(key)] part_correct(projected, key, state.parts.lookup(key)) by {
            assert(chunk_of(key) < 16);
        }
    }
    let ghost finished_parts = state.parts;
    let ghost finished_directory = state.stock_pairs;
    let mut supplier_work = SupplierWork::new(); let mut index = 0;
    while index < suppliers.len() invariant index <= suppliers.len(), state.parts == finished_parts,
        domain_valid(db), dimensions_empty(initial_suppliers, initial_nations, initial_names),
        projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
        supplier_work.shape(), state.stock_pairs == finished_directory, state.suppliers == initial_suppliers, state.nations == initial_nations, state.names == initial_names,
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
        supplier_work.change(&state.suppliers, &suppliers[index], 1);
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
    proof {
        assert(suppliers@.take(suppliers.len() as int) =~= suppliers@);
        assert forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_work.count(initial_suppliers, nation, supplier, name, address) >= 0 by {
            let weight = |row: SupplierInput| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name@ == name && row.s_address@ == address { 1int } else { 0int };
            assert forall|i: int| 0 <= i < suppliers@.len() implies weight(#[trigger] suppliers@[i]) >= 0 by {}
            lemma_sum_nonneg(suppliers@, weight);
        }
    }
    if !supplier_work.publish(&mut state.suppliers) { proof { assert(false); } return None; }
    proof { crate::state_proofs::lemma_supplier_build(projected, state.suppliers, suppliers@); }
    let ghost finished_suppliers = state.suppliers;
    let mut nation_work = NationWork::new(); let mut index = 0;
    while index < nations.len() invariant index <= nations.len(), state.parts == finished_parts, nation_work.shape(),
        domain_valid(db), dimensions_empty(initial_suppliers, initial_nations, initial_names), nation_work.hashes(), state.nations == initial_nations, state.names == initial_names,
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
        nation_work.change(&state.nations, &nations[index], 1);
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
    proof {
        assert(nations@.take(nations.len() as int) =~= nations@);
        assert forall|name: Seq<u8>, key: i64| #[trigger] nation_work.count(initial_nations, name, key) >= 0 by {
            let weight = |row: NationInput| if row.n_name@ == name && row.n_nationkey == key { 1int } else { 0int };
            assert forall|i: int| 0 <= i < nations@.len() implies weight(#[trigger] nations@[i]) >= 0 by {}
            lemma_sum_nonneg(nations@, weight);
        }
    }
    if !nation_work.publish(&mut state.nations) { proof { assert(false); } return None; }
    proof { crate::state_proofs::lemma_nation_build(projected, state.nations, nations@); }
    let ghost finished_nations = state.nations;
    let mut name_work = NameWork::new(); let mut index = 0;
    proof { assert(names_positive(initial_names)); }
    while index < parts.len() invariant index <= parts.len(), state.parts == finished_parts, name_work.shape(), names_positive(state.names),
        domain_valid(db), dimensions_empty(initial_suppliers, initial_nations, initial_names), state.names == initial_names,
        nations_positive(state.nations), state.nations == finished_nations, nation_correct(projected, state.nations),
        projected == crate::semantic_proofs::scan_db(db, stocks@, shipments@, suppliers@, nations@),
        state.suppliers == finished_suppliers, supplier_correct(projected, state.suppliers),
        state.stock_pairs == finished_directory, directory_matches(state.parts, state.stock_pairs),
        forall|key: i64| #![trigger state.parts.lookup(key)] part_correct(projected, key, state.parts.lookup(key)),
        forall|key: i64| #![trigger part_count(state.parts, key)] part_count(state.parts, key) == eligible(db, key as int),
        forall|key: i64, name: Seq<u8>| #![trigger name_work.count(initial_names, key, name)] name_work.count(initial_names, key, name) ==
            sum(parts@.take(index as int), |row: PartInput| if row.p_partkey == key && row.p_name@ == name { 1 } else { 0 }),
        decreases parts.len() - index,
    {
        let ghost previous = name_work;
        name_work.change(&state.names, &parts[index], 1);
        proof {
            assert forall|key: i64, name: Seq<u8>| #![trigger name_work.count(initial_names, key, name)] name_work.count(initial_names, key, name) ==
                sum(parts@.take(index as int + 1), |row: PartInput| if row.p_partkey == key && row.p_name@ == name { 1 } else { 0 }) by {
                let weight = |row: PartInput| if row.p_partkey == key && row.p_name@ == name { 1int } else { 0int };
                assert(previous.count(initial_names, key, name) == sum(parts@.take(index as int), weight));
                lemma_sum_take(parts@, index as int, weight);
            }
        }
        index += 1;
    }
    proof {
        assert(parts@.take(parts.len() as int) =~= parts@);
        assert forall|key: i64, name: Seq<u8>| #[trigger] name_work.count(initial_names, key, name) >= 0 by {
            let weight = |row: PartInput| if row.p_partkey == key && row.p_name@ == name { 1int } else { 0int };
            assert forall|i: int| 0 <= i < parts@.len() implies weight(#[trigger] parts@[i]) >= 0 by {}
            lemma_sum_nonneg(parts@, weight);
        }
    }
    if !name_work.publish(&mut state.names) { proof { assert(false); } return None; }
    proof {
        crate::state_proofs::lemma_names_build(db, state.names, parts@);
        use_type_invariant(&state.parts);
        use_type_invariant(&state.stock_pairs);
        use_type_invariant(&state.suppliers);
        use_type_invariant(&state.nations);
        use_type_invariant(&state.names);
        state.parts.reveal_well_formed();
        state.stock_pairs.reveal_well_formed();
        state.suppliers.reveal_well_formed();
        state.nations.reveal_well_formed();
        state.names.reveal_well_formed();
        crate::state_proofs::lemma_state_meaning(projected, state.parts, state.stock_pairs, state.suppliers, state.nations, state.names);
        crate::semantic_proofs::lemma_build_meaning(db, stocks@, shipments@, suppliers@, nations@, state.parts, state.stock_pairs, state.suppliers, state.nations, state.names);
    }
    Some(State { db: Ghost(db), data: state })
}
fn is_prefix_pattern(pattern: &[u8]) -> (result: bool)
    ensures result == prefix_pattern(pattern@),
{
    if pattern.len() == 0 || pattern[pattern.len() - 1] != 37 { return false; }
    let mut index = 0;
    while index + 1 < pattern.len()
        invariant index + 1 <= pattern.len() || index == 0, pattern.len() >= 1, pattern[pattern.len() - 1 as int] == 37u8,
            forall|i: int| #![trigger pattern@[i]] 0 <= i < index ==> pattern@[i] != 37u8 && pattern@[i] != 95u8,
        decreases pattern.len() - index,
    {
        if pattern[index] == 37 || pattern[index] == 95 { return false; }
        index += 1;
    }
    true
}
fn row_matches(name: &[u8], pattern: &[u8], literal_prefix: bool) -> (result: bool)
    requires literal_prefix ==> prefix_pattern(pattern@),
    ensures result == like_match(name@, pattern@),
{
    if !literal_prefix { return verus_prooflib::like_match_exec(name, pattern); }
    proof { lemma_like_prefix_pattern(name@, pattern@); }
    let n = pattern.len() - 1;
    if name.len() < n { return false; }
    let mut index = 0;
    while index < n
        invariant index <= n, n + 1 == pattern.len(), name.len() >= n, prefix_pattern(pattern@),
            like_match(name@, pattern@) == (name@.len() + 1 >= pattern@.len() && name@.subrange(0, n as int) =~= pattern@.subrange(0, n as int)),
            forall|i: int| #![trigger name@[i]] 0 <= i < index ==> name@[i] == pattern@[i],
        decreases n - index,
    {
        if name[index] != pattern[index] {
            proof {
                assert(name@.subrange(0, n as int)[index as int] == name@[index as int]);
                assert(pattern@.subrange(0, n as int)[index as int] == pattern@[index as int]);
                assert(name@.subrange(0, n as int) != pattern@.subrange(0, n as int)) by {
                    if name@.subrange(0, n as int) == pattern@.subrange(0, n as int) {
                        assert(name@.subrange(0, n as int)[index as int] == pattern@.subrange(0, n as int)[index as int]);
                    }
                }
            }
            return false;
        }
        index += 1;
    }
    proof { assert(name@.subrange(0, n as int) =~= pattern@.subrange(0, n as int)); }
    true
}
/// Appends to `cands` the key of every row of one names slot whose name matches the pattern.
fn scan_slot(names: &PartNames, slot: i64, pattern: &[u8], literal_prefix: bool, cands: &mut Vec<i64>)
    requires names_positive(*names), literal_prefix ==> prefix_pattern(pattern@),
        forall|key: i64| #![trigger old(cands)@.contains(key)] old(cands)@.contains(key) ==> names_match(*names, key, pattern@),
    ensures forall|key: i64| #![trigger final(cands)@.contains(key)] final(cands)@.contains(key) ==> names_match(*names, key, pattern@),
        forall|key: i64| #![trigger old(cands)@.contains(key)] old(cands)@.contains(key) ==> final(cands)@.contains(key),
        forall|key: i64, name: Seq<u8>| #![trigger name_count(*names, key, name)]
            name_slot(key, name) == slot && name_count(*names, key, name) > 0 && like_match(name, pattern@) ==> final(cands)@.contains(key),
{
    let ghost before = cands@;
    if let Some(rows) = names.get_ref(slot) {
        let mut index = 0;
        while index < rows.len()
            invariant index <= rows.len(), names.lookup(slot) == Some(*rows), names_positive(*names), literal_prefix ==> prefix_pattern(pattern@),
                forall|key: i64| #![trigger cands@.contains(key)] cands@.contains(key) ==> names_match(*names, key, pattern@),
                forall|offset: int| #![trigger rows@[offset]] 0 <= offset < index && like_match(rows@[offset].name@, pattern@) ==> cands@.contains(rows@[offset].key),
                forall|key: i64| #![trigger cands@.contains(key)] before.contains(key) ==> cands@.contains(key),
            decreases rows.len() - index,
        {
            if row_matches(rows[index].name.as_slice(), pattern, literal_prefix) {
                let ghost row = rows@[index as int];
                proof {
                    let whole = *rows;
                    assert(names.lookup(slot) == Some(whole));
                    assert(rows@ == whole@);
                    lemma_names_row(*names, slot, whole, index as int);
                    assert(name_slot(row.key, row.name@) == slot);
                    assert(name_count(*names, row.key, row.name@) > 0) by {
                        let name = row.name@;
                        let w = |item: NationVariant| if item.name@ == name && item.key == row.key { item.count@ } else { 0int };
                        assert forall|j: int| 0 <= j < whole@.len() implies w(#[trigger] whole@[j]) >= 0 by { lemma_names_row(*names, slot, whole, j); }
                        lemma_sum_pos(whole@, w, index as int);
                        lemma_nation_measure_weight(whole@, name, row.key, w);
                    }
                    assert(names_match(*names, row.key, pattern@));
                }
                let ghost previous = cands@;
                cands.push(rows[index].key);
                proof {
                    assert(cands@ =~= previous.push(row.key));
                    assert(cands@ == previous.push(row.key));
                    verus_prooflib::lemma_push_membership(previous, row.key);
                    assert forall|key: i64| #![trigger cands@.contains(key)] cands@.contains(key) implies names_match(*names, key, pattern@) by {
                        if key != row.key { assert(previous.contains(key)); }
                    }
                    assert(cands@.contains(row.key));
                    assert forall|key: i64| #![trigger cands@.contains(key)] before.contains(key) implies cands@.contains(key) by {
                        assert(previous.contains(key) ==> cands@.contains(key));
                    }
                }
            }
            index += 1;
        }
        proof {
            assert forall|key: i64, name: Seq<u8>| #![trigger name_count(*names, key, name)]
                name_slot(key, name) == slot && name_count(*names, key, name) > 0 && like_match(name, pattern@) implies cands@.contains(key) by {
                let w = |item: NationVariant| if item.name@ == name && item.key == key { item.count@ } else { 0int };
                lemma_nation_measure_weight(rows@, name, key, w);
                let j = lemma_sum_nonzero_witness(rows@, w);
                assert(rows@[j].name@ == name && rows@[j].key == key);
                assert(like_match(rows@[j].name@, pattern@));
            }
        }
    } else {
        proof {
            assert forall|key: i64, name: Seq<u8>| #![trigger name_count(*names, key, name)]
                name_slot(key, name) == slot && name_count(*names, key, name) > 0 && like_match(name, pattern@) implies cands@.contains(key) by {
                assert(names.lookup(slot).is_none());
                assert(false);
            }
        }
    }
}
/// Candidate part keys for a pattern: every part whose stored name matches.  A pattern that starts with three
/// literal bytes only needs the 1024 slots that share those bytes; any other pattern scans every slot.
fn candidates(names: &PartNames, pattern: &[u8]) -> (result: Vec<i64>)
    requires names_positive(*names),
    ensures
        (forall|key: i64| #![trigger names_match(*names, key, pattern@)] names_match(*names, key, pattern@) ==> result@.contains(key))
        && (forall|key: i64| #![trigger result@.contains(key)] result@.contains(key) ==> names_match(*names, key, pattern@)),
{
    let mut cands: Vec<i64> = Vec::new();
    let literal_prefix = is_prefix_pattern(pattern);
    if pattern.len() >= 3 && pattern[0] != 37 && pattern[0] != 95 && pattern[1] != 37 && pattern[1] != 95
        && pattern[2] != 37 && pattern[2] != 95 {
        proof { assert(pattern_literal3(pattern@)); }
        let base = name_slot_exec(0, pattern);
        proof { assert(base as int == name_code(pattern@) * 1024 + key_low(0)); assert(key_low(0) == 0); lemma_slot_split(0, pattern@); }
        let mut low: i64 = 0;
        while low < 1024
            invariant 0 <= low <= 1024, base as int == name_code(pattern@) * 1024, pattern_literal3(pattern@), names_positive(*names),
                literal_prefix == prefix_pattern(pattern@),
                forall|key: i64| #![trigger cands@.contains(key)] cands@.contains(key) ==> names_match(*names, key, pattern@),
                forall|key: i64, name: Seq<u8>| #![trigger name_count(*names, key, name)]
                    key_low(key) < low && name_count(*names, key, name) > 0 && like_match(name, pattern@) ==> cands@.contains(key),
            decreases 1024 - low,
        {
            let ghost before = cands@;
            let slot = base + low;
            scan_slot(names, slot, pattern, literal_prefix, &mut cands);
            proof {
                assert forall|key: i64, name: Seq<u8>| #![trigger name_count(*names, key, name)]
                    key_low(key) < low + 1 && name_count(*names, key, name) > 0 && like_match(name, pattern@) implies cands@.contains(key) by {
                    if key_low(key) == low {
                        lemma_like_prefix(name, pattern@);
                        lemma_slot_split(key, name);
                        assert(name_slot(key, name) == slot);
                    } else {
                        assert(before.contains(key));
                    }
                }
            }
            low += 1;
        }
        proof {
            assert forall|key: i64| #![trigger names_match(*names, key, pattern@)] names_match(*names, key, pattern@) implies cands@.contains(key) by {
                let name = choose|name: Seq<u8>| #![trigger name_count(*names, key, name)] name_count(*names, key, name) > 0 && like_match(name, pattern@);
                lemma_slot_split(key, name);
                assert(key_low(key) < 1024);
            }
        }
    } else {
        let slots = names.keys();
        let mut position = 0;
        while position < slots.len()
            invariant position <= slots.len(), names_positive(*names), literal_prefix == prefix_pattern(pattern@),
                forall|slot: i64| #![trigger names.lookup(slot)] slots@.contains(slot) == names.lookup(slot).is_some(),
                forall|key: i64| #![trigger cands@.contains(key)] cands@.contains(key) ==> names_match(*names, key, pattern@),
                forall|key: i64, name: Seq<u8>| #![trigger name_count(*names, key, name)]
                    slots@.take(position as int).contains(name_slot(key, name)) && name_count(*names, key, name) > 0 && like_match(name, pattern@) ==> cands@.contains(key),
            decreases slots.len() - position,
        {
            let ghost before = cands@;
            scan_slot(names, slots[position], pattern, literal_prefix, &mut cands);
            proof {
                assert(slots@.take(position as int + 1) =~= slots@.take(position as int).push(slots@[position as int]));
                verus_prooflib::lemma_push_membership(slots@.take(position as int), slots@[position as int]);
                assert forall|key: i64, name: Seq<u8>| #![trigger name_count(*names, key, name)]
                    slots@.take(position as int + 1).contains(name_slot(key, name)) && name_count(*names, key, name) > 0 && like_match(name, pattern@) implies cands@.contains(key) by {
                    if name_slot(key, name) != slots@[position as int] { assert(slots@.take(position as int).contains(name_slot(key, name))); assert(before.contains(key)); }
                }
            }
            position += 1;
        }
        proof {
            assert(slots@.take(slots.len() as int) =~= slots@);
            assert forall|key: i64| #![trigger names_match(*names, key, pattern@)] names_match(*names, key, pattern@) implies cands@.contains(key) by {
                let name = choose|name: Seq<u8>| #![trigger name_count(*names, key, name)] name_count(*names, key, name) > 0 && like_match(name, pattern@);
                assert(names.lookup(name_slot(key, name)).is_some());
                assert(slots@.contains(name_slot(key, name)));
            }
        }
    }
    cands
}
fn pair_passes(pair: &Pair, start: i32, end: i32) -> (result: bool)
    requires pair_valid(*pair),
    ensures result == cached_pair_passes(*pair, start as int, end as int),
{
    if pair.stocks.len() == 0 { return false; }
    let lower = prefix(pair, start); let upper = prefix(pair, end);
    let count = upper.0.sub(&lower.0);
    let quantity = upper.1.sub(&lower.1);
    let threshold = Wide::from_i128(pair.stocks[pair.stocks.len() - 1].value as i128).mul(&Wide::from_i128(200));
    count.is_pos() && quantity.lt(&threshold)
}
/// For each supplier key of the bucket: does any candidate part have a qualifying (part, supplier) pair?
fn bucket_qualified(state: &Data, keys: &Vec<i64>, cands: &Vec<i64>, start: i32, end: i32) -> (found: HashMap<i64, bool>)
    requires read_shape(state.parts),
    ensures found@.dom() == keys@.to_set()
        && forall|supplier: i64| #![trigger found@[supplier]] keys@.contains(supplier) ==>
            found@[supplier] == any_cand_passes(state.parts, cands@, supplier, start as int, end as int),
{
    let mut found: HashMap<i64, bool> = HashMap::new();
    let mut k = 0;
    while k < keys.len()
        invariant k <= keys.len(), found@.dom() == keys@.take(k as int).to_set(),
            forall|supplier: i64| #![trigger found@[supplier]] found@.contains_key(supplier) ==> !found@[supplier],
        decreases keys.len() - k,
    {
        proof {
            verus_prooflib::lemma_push_to_set(keys@.take(k as int), keys@[k as int]);
            assert(keys@.take(k as int + 1) =~= keys@.take(k as int).push(keys@[k as int]));
        }
        found.insert(keys[k], false);
        k += 1;
    }
    proof { assert(keys@.take(keys.len() as int) =~= keys@); keys@.to_set_ensures(); }
    let mut ci = 0;
    while ci < cands.len()
        invariant ci <= cands.len(), read_shape(state.parts), found@.dom() == keys@.to_set(),
            forall|supplier: i64| #![trigger found@[supplier]] found@.contains_key(supplier) ==>
                found@[supplier] == exists|j: int| #![trigger cands@[j]] 0 <= j < ci
                    && cached_part_passes(state.parts, cands@[j], supplier, start as int, end as int),
        decreases cands.len() - ci,
    {
        let part_key = cands[ci];
        let ghost previous = found@;
        if let Some(part) = state.parts.get_ref(part_key) {
            let mut pi = 0;
            while pi < part.pairs.len()
                invariant pi <= part.pairs.len(), read_shape(state.parts), state.parts.lookup(part_key) == Some(*part),
                    found@.dom() == previous.dom(), previous.dom() == keys@.to_set(),
                    forall|supplier: i64| #![trigger found@[supplier]] found@.contains_key(supplier) ==>
                        (found@[supplier] ==> previous[supplier] || cached_part_passes(state.parts, part_key, supplier, start as int, end as int)),
                    forall|supplier: i64| #![trigger found@[supplier]] found@.contains_key(supplier) ==> (previous[supplier] ==> found@[supplier]),
                    forall|supplier: i64, idx: int| #![trigger part.pairs@[idx], found@[supplier]] found@.contains_key(supplier) && 0 <= idx < pi
                        && part.pairs@[idx].0 == supplier && cached_part_passes(state.parts, part_key, supplier, start as int, end as int) ==> found@[supplier],
                decreases part.pairs.len() - pi,
            {
                let supplier = part.pairs[pi].0;
                let open = match found.get(&supplier) { Some(value) => !*value, None => false };
                if open {
                    let pair = &part.pairs[pi].1;
                    proof {
                        lemma_pair_at(state.parts, part_key, supplier, &*part, pi as int);
                        assert(pair_of(state.parts, part_key, supplier) == Some(*pair));
                        assert(pair_valid(*pair));
                    }
                    let passes = pair_passes(pair, start, end);
                    if passes { found.insert(supplier, true); }
                }
                pi += 1;
            }
            proof {
                assert forall|supplier: i64| #![trigger found@[supplier]] found@.contains_key(supplier) implies
                    found@[supplier] == (previous[supplier] || cached_part_passes(state.parts, part_key, supplier, start as int, end as int)) by {
                    if cached_part_passes(state.parts, part_key, supplier, start as int, end as int) {
                        let data = state.parts.lookup(part_key).unwrap();
                        assert(pair_lookup(data.pairs@, supplier).is_some());
                        let idx = choose|idx: int| #![trigger data.pairs@[idx]] 0 <= idx < data.pairs@.len() && data.pairs@[idx].0 == supplier;
                        assert(part.pairs@[idx].0 == supplier);
                    }
                }
            }
        } else {
            proof {
                assert forall|supplier: i64| #![trigger found@[supplier]] found@.contains_key(supplier) implies
                    found@[supplier] == (previous[supplier] || cached_part_passes(state.parts, part_key, supplier, start as int, end as int)) by {
                    assert(!cached_part_passes(state.parts, part_key, supplier, start as int, end as int));
                }
            }
        }
        proof {
            assert forall|supplier: i64| #![trigger found@[supplier]] found@.contains_key(supplier) implies
                found@[supplier] == exists|j: int| #![trigger cands@[j]] 0 <= j < ci + 1
                    && cached_part_passes(state.parts, cands@[j], supplier, start as int, end as int) by {
                assert(previous.contains_key(supplier));
                if cached_part_passes(state.parts, part_key, supplier, start as int, end as int) { assert(cands@[ci as int] == part_key); }
            }
        }
        ci += 1;
    }
    found
}
fn emit_rows(out: &mut Vec<(Vec<u8>, Vec<u8>)>, rows: &Vec<SupplierVariant>, factor: &Wide) -> (result: bool)
    requires factor@ > 0, forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==> rows@[index].count@ > 0,
    ensures result ==> forall|weight: spec_fn(OutRow) -> int| #[trigger] verus_common::sums::sum(out_view(final(out)@), weight) ==
            verus_common::sums::sum(out_view(old(out)@), weight) + factor@ * variant_weight(rows@, weight),
        !result ==> sum(out_view(old(out)@), one_row()) + factor@ * variant_weight(rows@, one_row()) > max_rows(),
{
    let ghost initial = out_view(out@);
    let mut variant = 0;
    while variant < rows.len()
        invariant variant <= rows.len(), factor@ > 0, initial == out_view(old(out)@),
            forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==> rows@[index].count@ > 0,
            forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight) + factor@ * variant_weight(rows@.take(variant as int), weight),
        decreases rows.len() - variant,
    {
        let copies_wide = rows[variant].count.mul(factor);
        let total = Wide::from_i128(out.len() as i128).add(&copies_wide);
        proof {
            let count = rows@[variant as int].count@;
            assert(copies_wide@ == count * factor@);
            assert(copies_wide@ >= 0) by (nonlinear_arith) requires count > 0, factor@ > 0, copies_wide@ == count * factor@;
        }
        if Wide::from_i128(9223372036854775808).lt(&total) {
            proof {
                let weight = one_row();
                lemma_out_len(out@);
                lemma_variant_take(rows@, variant as int, weight);
                lemma_variant_prefix_le(rows@, variant as int + 1, weight);
                let length = out@.len() as int;
                let before = variant_weight(rows@.take(variant as int), weight);
                let count = rows@[variant as int].count@;
                let whole = variant_weight(rows@, weight);
                assert(variant_weight(rows@.take(variant as int + 1), weight) == before + count * weight(OutRow { s_name: rows@[variant as int].name@, s_address: rows@[variant as int].address@ }));
                assert(weight(OutRow { s_name: rows@[variant as int].name@, s_address: rows@[variant as int].address@ }) == 1int);
                assert(whole >= before + count);
                assert(factor@ * whole >= factor@ * (before + count)) by (nonlinear_arith) requires factor@ > 0, whole >= before + count;
                assert(factor@ * (before + count) == factor@ * before + count * factor@) by (nonlinear_arith);
                assert(length == sum(initial, weight) + factor@ * before);
                assert(total@ == length + count * factor@);
                assert(sum(initial, weight) + factor@ * whole > max_rows());
            }
            return false;
        }
        proof { assert(total@ == out@.len() as int + copies_wide@); assert(copies_wide@ <= max_rows()); }
        let copies_signed = match copies_wide.to_i128() { Some(value) => value, None => { proof { assert(false); } return false; } };
        proof { assert(copies_signed as int == copies_wide@); }
        let copies = copies_signed as u64;
        let ghost before = out_view(out@);
        let ghost row = OutRow { s_name: rows@[variant as int].name@, s_address: rows@[variant as int].address@ };
        proof { assert(copies as int == copies_wide@); assert(copies as int == rows@[variant as int].count@ * factor@); }
        let mut copy: u64 = 0;
        while copy < copies
            invariant copy <= copies, variant < rows.len(),
                row == (OutRow { s_name: rows@[variant as int].name@, s_address: rows@[variant as int].address@ }),
                copies as int == rows@[variant as int].count@ * factor@,
                forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(before, weight) == sum(initial, weight) + factor@ * variant_weight(rows@.take(variant as int), weight),
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
            assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight) + factor@ * variant_weight(rows@.take(variant as int + 1), weight) by {
                let term = |item: SupplierVariant| item.count@ * weight(OutRow { s_name: item.name@, s_address: item.address@ });
                lemma_sum_take(rows@, variant as int, term);
                lemma_variant_take(rows@, variant as int, weight);
                assert(sum(before, weight) == sum(initial, weight) + factor@ * variant_weight(rows@.take(variant as int), weight));
                assert(sum(out_view(out@), weight) == sum(before, weight) + copies as int * weight(row));
                verus_prooflib::lemma_count_weight_assoc(factor@, rows@[variant as int].count@, weight(row));
                let count = rows@[variant as int].count@;
                let multiplier = factor@;
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
                verus_prooflib::lemma_scalar_distribute(variant_weight(rows@.take(variant as int), weight), term(rows@[variant as int]), factor@);
            }
        }
        variant += 1;
    }
    proof { assert(rows@.take(rows.len() as int) =~= rows@); }
    true
}
fn emit_bucket(state: &State, bucket: &Table<Vec<SupplierVariant>>, nation: i64, factor: &Wide, start: i32, end: i32, pattern: &[u8],
    Ghost(parameters): Ghost<Params>, cands: &Vec<i64>, out: &mut Vec<(Vec<u8>, Vec<u8>)>) -> (result: bool)
    requires parameters.p1 == start, parameters.p4 == end, parameters.p2 == pattern@, start < end, factor@ > 0,
        state.data.suppliers.lookup(nation) == Some(*bucket),
        forall|key: i64| #![trigger names_match(state.data.names, key, pattern@)] names_match(state.data.names, key, pattern@) ==> cands@.contains(key),
        forall|key: i64| #![trigger cands@.contains(key)] cands@.contains(key) ==> names_match(state.data.names, key, pattern@),
    ensures
        result ==> forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(final(out)@), weight) ==
            sum(out_view(old(out)@), weight) + factor@ * nation_weight(state.db(), parameters, nation as int, weight),
        !result ==> sum(out_view(old(out)@), one_row()) + factor@ * nation_weight(state.db(), parameters, nation as int, one_row()) > max_rows(),
{
    proof { use_type_invariant(state); }
    let ghost db = state.db();
    let ghost initial = out_view(out@);
    let keys = bucket.keys();
    proof {
        assert(read_shape(state.data.parts));
        assert(state.data.suppliers.lookup(nation) == Some(*bucket));
        assert forall|key: i64| #![trigger bucket.lookup(key)] bucket.lookup(key) matches Some(rows) ==>
            forall|offset: int| #![trigger rows@[offset]] 0 <= offset < rows@.len() ==> rows@[offset].count@ > 0 by {}
    }
    let qualified = bucket_qualified(&state.data, &keys, cands, start, end);
    let mut index = 0;
    while index < keys.len()
        invariant index <= keys.len(), keys@.no_duplicates(), initial == out_view(old(out)@),
            qualified@.dom() == keys@.to_set(),
            forall|supplier: i64| #![trigger qualified@[supplier]] keys@.contains(supplier) ==>
                qualified@[supplier] == any_cand_passes(state.data.parts, cands@, supplier, start as int, end as int),
            forall|key: i64| #![trigger names_match(state.data.names, key, pattern@)] names_match(state.data.names, key, pattern@) ==> cands@.contains(key),
            forall|key: i64| #![trigger cands@.contains(key)] cands@.contains(key) ==> names_match(state.data.names, key, pattern@),
            forall|key: i64| #![trigger bucket.lookup(key)] keys@.contains(key) == bucket.lookup(key).is_some(), factor@ > 0,
            forall|key: i64| #![trigger bucket.lookup(key)] bucket.lookup(key) matches Some(rows) ==> forall|offset: int| #![trigger rows@[offset]] 0 <= offset < rows@.len() ==> rows@[offset].count@ > 0,
            meaning(db, state.data.parts, state.data.stock_pairs, state.data.suppliers, state.data.nations, state.data.names), domain_valid(db), db == state.db(),
            state.data.suppliers.lookup(nation) == Some(*bucket), parameters.p1 == start, parameters.p4 == end, parameters.p2 == pattern@, start < end,
            forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight)
                + factor@ * bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@.take(index as int), parameters, weight),
        decreases keys.len() - index,
    {
        let key = keys[index];
        proof { keys@.to_set_ensures(); assert(keys@.contains(key)); }
        let valid = match qualified.get(&key) { Some(value) => *value, None => { proof { assert(false); } return false; } };
        let ghost before = out_view(out@);
        proof {
            lemma_cands_equiv(db, state.data.parts, state.data.stock_pairs, state.data.suppliers, state.data.nations, state.data.names, cands@, key, start as int, end as int, pattern@);
            assert(valid == cached_supplier_passes(state.data.parts, state.data.stock_pairs, state.data.names, key, parameters.p1, parameters.p4, parameters.p2)); }
        if valid {
            let rows = match bucket.get(key) { Some(value) => value, None => { proof { assert(false); } return false; } };
            proof { assert(bucket.lookup(key) == Some(*rows)); }
            if !emit_rows(out, &rows, factor) {
                proof {
                    let weight = one_row();
                    assert(bucket_rows(*bucket, key) == rows@);
                    lemma_bucket_take(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@, index as int, parameters, weight);
                    lemma_bucket_prefix_le(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@, index as int + 1, parameters, weight);
                    lemma_bucket(db, parameters, state.data.parts, state.data.stock_pairs, state.data.suppliers, state.data.nations, state.data.names, nation, *bucket, keys@, weight);
                    let whole = bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@, parameters, weight);
                    let upto = bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@.take(index as int + 1), parameters, weight);
                    assert(sum(before, weight) == sum(initial, weight) + factor@ * bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@.take(index as int), parameters, weight));
                    assert(upto == bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@.take(index as int), parameters, weight) + variant_weight(rows@, weight));
                    assert(factor@ * whole >= factor@ * upto) by (nonlinear_arith) requires factor@ > 0, whole >= upto;
                    assert(factor@ * upto == factor@ * bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@.take(index as int), parameters, weight) + factor@ * variant_weight(rows@, weight)) by (nonlinear_arith)
                        requires upto == bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@.take(index as int), parameters, weight) + variant_weight(rows@, weight);
                    assert(whole == nation_weight(db, parameters, nation as int, weight));
                }
                return false;
            }
            proof {
                assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(before, weight) + factor@ * variant_weight(bucket_rows(*bucket, key), weight) by {
                    assert(bucket_rows(*bucket, key) == rows@);
                }
            }
        }
        proof {
            assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight)
                + factor@ * bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@.take(index as int + 1), parameters, weight) by {
                let term = |key: i64| if cached_supplier_passes(state.data.parts, state.data.stock_pairs, state.data.names, key, parameters.p1, parameters.p4, parameters.p2) { variant_weight(bucket_rows(*bucket, key), weight) } else { 0int };
                lemma_sum_take(keys@, index as int, term);
                lemma_bucket_take(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@, index as int, parameters, weight);
                assert(sum(out_view(out@), weight) == sum(before, weight) + factor@ * term(key));
                assert(sum(before, weight) == sum(initial, weight) + factor@ * bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@.take(index as int), parameters, weight));
                verus_prooflib::lemma_scalar_distribute(bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@.take(index as int), parameters, weight), term(key), factor@);
            }
        }
        index += 1;
    }
    proof {
        assert(keys@.take(keys.len() as int) =~= keys@);
        assert forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == sum(initial, weight) + factor@ * nation_weight(db, parameters, nation as int, weight) by {
            lemma_bucket(db, parameters, state.data.parts, state.data.stock_pairs, state.data.suppliers, state.data.nations, state.data.names, nation, *bucket, keys@, weight);
            assert(sum(out_view(out@), weight) == sum(initial, weight) + factor@ * bucket_weight(state.data.parts, state.data.stock_pairs, state.data.names, *bucket, keys@, parameters, weight));
        }
    }
    true
}
pub fn execute(owner: &State, p1: i32, p2: &[u8], p3: &[u8], p4: i32) -> (result: Option<Vec<(Vec<u8>, Vec<u8>)>>)
    ensures result matches Some(out) ==> query_result(owner.db(), params(p1 as int, p2@, p3@, p4 as int), out_view(out@)),
        result is None ==> !answer_fits(owner.db(), params(p1 as int, p2@, p3@, p4 as int)),
{
    let ghost db = owner.db();
    let ghost parameters = params(p1 as int, p2@, p3@, p4 as int);
    proof { use_type_invariant(owner); }
    let state = &owner.data;
    let mut out = Vec::new();
    if p1 >= p4 {
        proof {
            lemma_empty_interval(db, parameters);
            assert(out_view(out@) =~= Seq::<OutRow>::empty());
            assert(query_result(db, parameters, out_view(out@)));
        }
        return Some(out);
    }
    let nations = match state.nations.get(hash(p3)) {
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
    proof { assert(names_correct(db, state.names)); }
    let cands = candidates(&state.names, p2);
    let mut nation_index = 0;
    proof {
        assert(state.nations.lookup(name_hash(p3@) as i64) == Some(*nations));
        assert(meaning(db, state.parts, state.stock_pairs, state.suppliers, state.nations, state.names));
        assert forall|index: int| #![trigger nations@[index]] 0 <= index < nations@.len() implies nations@[index].count@ > 0 by {
            lemma_meaning_nation_row(db, state.parts, state.stock_pairs, state.suppliers, state.nations, state.names, name_hash(p3@) as i64, *nations, index);
        }
    }
    while nation_index < nations.len()
        invariant nation_index <= nations.len(), *state == owner.data,
            forall|index: int| #![trigger nations@[index]] 0 <= index < nations@.len() ==> nations@[index].count@ > 0, db == owner.db(), parameters == params(p1 as int, p2@, p3@, p4 as int), p1 < p4,
            meaning(db, state.parts, state.stock_pairs, state.suppliers, state.nations, state.names), domain_valid(db),
            state.nations.lookup(name_hash(p3@) as i64) == Some(*nations),
            forall|key: i64| #![trigger names_match(state.names, key, p2@)] names_match(state.names, key, p2@) ==> cands@.contains(key),
            forall|key: i64| #![trigger cands@.contains(key)] cands@.contains(key) ==> names_match(state.names, key, p2@),
            forall|weight: spec_fn(OutRow) -> int| #[trigger] sum(out_view(out@), weight) == nation_output_weight(db, parameters, nations@.take(nation_index as int), weight),
        decreases nations.len() - nation_index,
    {
        let nation = &nations[nation_index];
        let ghost before = out_view(out@);
        if compare(&nation.name, p3) == 0 {
            if let Some(bucket) = state.suppliers.get(nation.key) {
                if !emit_bucket(owner, &bucket, nation.key, &nation.count, p1, p4, p2, Ghost(parameters), &cands, &mut out) {
                    proof {
                        let weight = one_row();
                        let term = |row: NationVariant| if row.name@ == parameters.p3 { row.count@ * nation_weight(db, parameters, row.key as int, weight) } else { 0int };
                        lemma_sum_take(nations@, nation_index as int, term);
                        assert(nations@[nation_index as int] == *nation);
                        assert(nation.name@ == parameters.p3);
                        lemma_nation_prefix_le(db, parameters, nations@, nation_index as int + 1, weight);
                        lemma_nation_rows(db, parameters, state.nations, nations@, weight);
                        lemma_too_long(db, parameters);
                    }
                    return None;
                }
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
                let term = |row: NationVariant| if row.name@ == parameters.p3 { row.count@ * nation_weight(db, parameters, row.key as int, weight) } else { 0int };
                lemma_sum_take(nations@, nation_index as int, term);
                assert(sum(before, weight) == nation_output_weight(db, parameters, nations@.take(nation_index as int), weight));
                if nation.name@ == p3@ {
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
fn part_changes(state: &Data, deltas: &Vec<PartDelta>) -> (result: (HashMap<i64, Wide>, Vec<i64>))
    ensures result.1@.no_duplicates() && result.0@.dom() == result.1@.to_set()
        && result.1@.to_set() == deltas@.map_values(|row: PartDelta| row.row.p_partkey).to_set()
        && forall|key: i64| #![trigger coalesced_part_count(state.parts, result.0@, key)]
            coalesced_part_count(state.parts, result.0@, key) == part_count(state.parts, key)
                + sum(deltas@, crate::semantic_proofs::part_capture_weight(key as int)),
{
    let mut counts: HashMap<i64, Wide> = HashMap::new(); let mut keys: Vec<i64> = Vec::new(); let mut index = 0;
    while index < deltas.len()
        invariant index <= deltas.len(), keys@.no_duplicates(), counts@.dom() == keys@.to_set(),
            keys@.to_set() == deltas@.take(index as int).map_values(|row: PartDelta| row.row.p_partkey).to_set(),
            forall|key: i64| #![trigger coalesced_part_count(state.parts, counts@, key)]
                coalesced_part_count(state.parts, counts@, key) == part_count(state.parts, key)
                    + sum(deltas@.take(index as int), crate::semantic_proofs::part_capture_weight(key as int)),
        decreases deltas.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous = counts@;
        let ghost support = keys@;
        let row = &deltas[index]; let key = row.row.p_partkey;
        let before = match counts.remove(&key) {
            Some(value) => value,
            None => {
                proof {
                    support.to_set_ensures();
                    lemma_unique_push(keys@, key);
                    lemma_push_membership(keys@, key);
                    verus_prooflib::lemma_push_to_set(keys@, key);
                }
                keys.push(key); match state.parts.get(key) { Some(part) => part.count.dup(), None => Wide::zero() }
            },
        };
        proof { assert(before@ == coalesced_part_count(state.parts, previous, key)); }
        let after = before.add(&Wide::from_i128(row.sign as i128));
        counts.insert(key, after);
        proof {
            assert(counts@ == previous.insert(key, after));
            if previous.contains_key(key) {
                assert(keys@ == support);
                assert(previous.dom().insert(key) == previous.dom());
            } else { assert(keys@ == support.push(key)); }
            assert(counts@.dom() =~= keys@.to_set());
            let processed_keys = deltas@.take(index as int).map_values(|row: PartDelta| row.row.p_partkey);
            assert(deltas@.take(index as int + 1).map_values(|row: PartDelta| row.row.p_partkey) =~= processed_keys.push(key));
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
    (counts, keys)
}
pub closed spec fn rescan_stocks_for_parts_wanted(state: State, lineitem_delta: Seq<LineitemDelta>, nation_delta: Seq<NationDelta>,
    part_delta: Seq<PartDelta>, partsupp_delta: Seq<PartsuppDelta>, supplier_delta: Seq<SupplierDelta>) -> Seq<i64>
{
    activation_spec(state.data.parts, part_delta)
}
pub closed spec fn rescan_shipments_for_parts_wanted(state: State, lineitem_delta: Seq<LineitemDelta>, nation_delta: Seq<NationDelta>,
    part_delta: Seq<PartDelta>, partsupp_delta: Seq<PartsuppDelta>, supplier_delta: Seq<SupplierDelta>) -> Seq<i64>
{
    activation_spec(state.data.parts, part_delta)
}
pub fn rescan_stocks_for_parts_keys(
    state: &State, lineitem_delta: &[LineitemDelta], nation_delta: &Vec<NationDelta>, part_delta: &Vec<PartDelta>,
    partsupp_delta: &[PartsuppDelta], supplier_delta: &Vec<SupplierDelta>,
) -> (result: Vec<i64>)
    ensures result@ == rescan_stocks_for_parts_wanted(*state, lineitem_delta@, nation_delta@, part_delta@, partsupp_delta@, supplier_delta@),
{ activation_keys(&state.data.parts, part_delta) }
pub fn rescan_shipments_for_parts_keys(
    state: &State, lineitem_delta: &[LineitemDelta], nation_delta: &Vec<NationDelta>, part_delta: &Vec<PartDelta>,
    partsupp_delta: &[PartsuppDelta], supplier_delta: &Vec<SupplierDelta>,
) -> (result: Vec<i64>)
    ensures result@ == rescan_shipments_for_parts_wanted(*state, lineitem_delta@, nation_delta@, part_delta@, partsupp_delta@, supplier_delta@),
{ activation_keys(&state.data.parts, part_delta) }
fn touch(state: &Data, key: i64, counts: &HashMap<i64, Wide>, work: &mut HashMap<i64, PartWork>, keys: &mut Vec<i64>)
    ensures parts_shape(state.parts) && work_ready(old(work)@, old(keys)@, state.parts, counts@) ==>
        work_ready(final(work)@, final(keys)@, state.parts, counts@),
        final(work)@.contains_key(key) == (old(work)@.contains_key(key) || coalesced_part_count(state.parts, counts@, key) > 0),
        forall|other: i64| #![trigger final(work)@.contains_key(other)] other != key ==> final(work)@.contains_key(other) == old(work)@.contains_key(other),
        parts_shape(state.parts) ==> forall|part: i64, supplier: i64, value: int| #[trigger] combined_stock(final(work)@, state.parts, part, supplier, value)
            == combined_stock(old(work)@, state.parts, part, supplier, value),
        parts_shape(state.parts) ==> forall|part: i64, supplier: i64, date: int, quantity: bool| #[trigger] combined_day(final(work)@, state.parts, part, supplier, date, quantity)
            == combined_day(old(work)@, state.parts, part, supplier, date, quantity),
{
    let ghost previous = work@;
    let ghost previous_keys = keys@;
    if work.contains_key(&key) { return; }
    let before = state.parts.get(key);
    let count = match counts.get(&key) { Some(value) => value.dup(), None => match &before { Some(value) => value.count.dup(), None => Wide::zero() } };
    if !count.is_pos() { return; }
    let mut part = match before {
        Some(value) => {
            proof {
                if parts_shape(state.parts) {
                    assert(state.parts.lookup(key) == Some(*value));
                    assert(crate::profile_proofs::part_shape(*value));
                }
            }
            PartWork::from_part(&value)
        },
        None => PartWork::new(count.dup()),
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
            assert(replacement.count@ == coalesced_part_count(state.parts, counts@, key));
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
}
pub fn apply(
    state: &State, lineitem_delta: &[LineitemDelta], nation_delta: &Vec<NationDelta>, part_delta: &Vec<PartDelta>,
    partsupp_delta: &[PartsuppDelta], supplier_delta: &Vec<SupplierDelta>,
    stocks_for_parts_keys: &[i64], stocks_for_parts_rows: &[StockInput],
    shipments_for_parts_keys: &[i64], shipments_for_parts_rows: &[ShipmentInput],
    Ghost(db2): Ghost<Db>,
) -> (result: Result<State, u32>)
    requires capture_ok(state.db(), db2, lineitem_delta_seq_view(lineitem_delta@), nation_delta_seq_view(nation_delta@), part_delta_seq_view(part_delta@), partsupp_delta_seq_view(partsupp_delta@), supplier_delta_seq_view(supplier_delta@)),
        stocks_for_parts_keys@ == rescan_stocks_for_parts_wanted(*state, lineitem_delta@, nation_delta@, part_delta@, partsupp_delta@, supplier_delta@),
        rescan_stocks_for_parts_ok(db2, stocks_for_parts_keys@, stocks_for_parts_rows@),
        shipments_for_parts_keys@ == rescan_shipments_for_parts_wanted(*state, lineitem_delta@, nation_delta@, part_delta@, partsupp_delta@, supplier_delta@),
        rescan_shipments_for_parts_ok(db2, shipments_for_parts_keys@, shipments_for_parts_rows@),
    ensures result matches Ok(next) ==> next.db() == db2,
        result is Ok,
{
    let ghost previous_db = state.db();
    proof { use_type_invariant(state); }
    let ghost previous_state = *state;
    let state = &state.data;
    let (counts, changed) = part_changes(state, part_delta);
    proof {
        crate::semantic_proofs::lemma_coalesced_after(previous_db, db2, part_delta@, state.parts, counts@);
        crate::state_proofs::lemma_meaning_components(previous_db, state.parts, state.stock_pairs, state.suppliers, state.nations, state.names);
    }
    let mut work = HashMap::new(); let mut keys = Vec::new(); let mut activated: HashMap<i64, bool> = HashMap::new();
    let mut next = Data { parts: state.parts.share(), stock_pairs: state.stock_pairs.share(), suppliers: state.suppliers.share(), nations: state.nations.share(), names: state.names.share() };
    let ghost initial_suppliers = next.suppliers;
    let ghost initial_nations = next.nations;
    let ghost initial_names = next.names;
    proof {
        lemma_dimensions_share(previous_db, state.suppliers, state.nations, state.names, initial_suppliers, initial_nations, initial_names);
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
            changed@.to_set() == part_delta@.map_values(|row: PartDelta| row.row.p_partkey).to_set(),
            forall|key: i64| #![trigger coalesced_part_count(state.parts, counts@, key)] coalesced_part_count(state.parts, counts@, key) == part_count(state.parts, key)
                + sum(part_delta@, crate::semantic_proofs::part_capture_weight(key as int)),
            *state == previous_state.data,
            stocks_for_parts_keys@ == rescan_stocks_for_parts_wanted(previous_state, lineitem_delta@, nation_delta@, part_delta@, partsupp_delta@, supplier_delta@),
            shipments_for_parts_keys@ == rescan_shipments_for_parts_wanted(previous_state, lineitem_delta@, nation_delta@, part_delta@, partsupp_delta@, supplier_delta@),
            meaning(previous_db, state.parts, state.stock_pairs, state.suppliers, state.nations, state.names), domain_valid(previous_db), parts_shape(state.parts),
            forall|key: i64| #![trigger state.parts.lookup(key)] part_correct(previous_db, key, state.parts.lookup(key)),
            next.suppliers == initial_suppliers, next.nations == initial_nations, next.names == initial_names,
            supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations), names_correct(previous_db, initial_names),
            directory_matches(next.parts, next.stock_pairs),
            forall|key: i64| #[trigger] next.parts.lookup(key) == if changed@.take(index as int).contains(key) && eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
            forall|key: i64| #![trigger work@.contains_key(key)] changed@.take(index as int).contains(key) && eligible(db2, key as int) > 0 ==> work@.contains_key(key),
            forall|part: i64, supplier: i64, value: int| #[trigger] combined_stock(work@, state.parts, part, supplier, value) == combined_stock(Map::empty(), state.parts, part, supplier, value),
            forall|part: i64, supplier: i64, date: int, quantity: bool| #[trigger] combined_day(work@, state.parts, part, supplier, date, quantity) == combined_day(Map::empty(), state.parts, part, supplier, date, quantity),
            capture_ok(previous_db, db2, lineitem_delta_seq_view(lineitem_delta@), nation_delta_seq_view(nation_delta@), part_delta_seq_view(part_delta@), partsupp_delta_seq_view(partsupp_delta@), supplier_delta_seq_view(supplier_delta@)),
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
        let key = changed[index];
        proof { changed@.to_set_ensures(); assert(changed@.contains(key)); assert(counts@.contains_key(key)); }
        let count = match counts.get(&key) { Some(value) => value.dup(), None => { proof { assert(false); } return Err(2); } };
        proof {
            assert(count@ == coalesced_part_count(state.parts, counts@, key));
            crate::ready_proofs::lemma_eligible_nonneg(db2, key as int);
        }
        if count.is_neg() { proof { assert(false); } return Err(3); }
        if count.is_zero() { publish_part(&mut next, key, None); }
        else {
            if state.parts.get(key).is_none() {
                proof {
                    assert(*state == previous_state.data);
                    assert(coalesced_part_count(state.parts, counts@, key) == sum(part_delta@, crate::semantic_proofs::part_capture_weight(key as int)));
                    crate::ready_proofs::lemma_activation_member(state.parts, part_delta@, changed@, key);
                }
                if !contains(stocks_for_parts_keys, key) || !contains(shipments_for_parts_keys, key) { proof { assert(false); } return Err(4); }
                activated.insert(key, true);
            }
            touch(state, key, &counts, &mut work, &mut keys);
        }
        proof {
            assert(changed@.take(index as int + 1) =~= changed@.take(index as int).push(key));
            lemma_push_membership(changed@.take(index as int), key);
            assert(count@ == coalesced_part_count(state.parts, counts@, key));
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
        crate::update_proofs::lemma_apply_domain(previous_db, db2, part_delta@, partsupp_delta@, lineitem_delta@, supplier_delta@, nation_delta@,
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
        supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations), names_correct(previous_db, initial_names),
        facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@,
        facts.valid(), facts.parts == state.parts, facts.counts == counts@, facts.activated == activated@, facts.after == db2,
        facts.stocks == stocks_for_parts_rows@, facts.ships == shipments_for_parts_rows@, facts.stock_delta == partsupp_delta@, facts.ship_delta == lineitem_delta@,
        facts.progress(work@, index as int, 0, 0, 0), facts.coverage(work@, 0, 0),
        next.suppliers == initial_suppliers, next.nations == initial_nations, next.names == initial_names, directory_matches(next.parts, next.stock_pairs),
        forall|key: i64| #[trigger] next.parts.lookup(key) == if eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
        parts_shape(state.parts), work_ready(work@, keys@, state.parts, counts@), decreases stocks_for_parts_rows.len() - index, {
        let ghost processed = index as int;
        let ghost before = work@;
        let row = &stocks_for_parts_rows[index];
        if activated.contains_key(&row.ps_partkey) {
            proof { assert(activated@.contains_key(row.ps_partkey)); assert(eligible(db2, row.ps_partkey as int) > 0); assert(work@.contains_key(row.ps_partkey)); }
            stock_change(&mut work, row, 1);
        }
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
        supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations), names_correct(previous_db, initial_names),
        facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@,
        facts.valid(), facts.parts == state.parts, facts.counts == counts@, facts.activated == activated@, facts.after == db2,
        facts.stocks == stocks_for_parts_rows@, facts.ships == shipments_for_parts_rows@, facts.stock_delta == partsupp_delta@, facts.ship_delta == lineitem_delta@,
        facts.progress(work@, facts.stocks.len() as int, index as int, 0, 0), facts.coverage(work@, 0, 0),
        next.suppliers == initial_suppliers, next.nations == initial_nations, next.names == initial_names, directory_matches(next.parts, next.stock_pairs),
        forall|key: i64| #[trigger] next.parts.lookup(key) == if eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
        parts_shape(state.parts), work_ready(work@, keys@, state.parts, counts@), decreases shipments_for_parts_rows.len() - index, {
        let ghost processed = index as int;
        let ghost before = work@;
        let row = &shipments_for_parts_rows[index];
        if activated.contains_key(&row.l_partkey) {
            proof { assert(activated@.contains_key(row.l_partkey)); assert(eligible(db2, row.l_partkey as int) > 0); assert(work@.contains_key(row.l_partkey)); }
            shipment_change(&mut work, row, 1);
        }
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
        supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations), names_correct(previous_db, initial_names),
        facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@,
        facts.valid(), facts.parts == state.parts, facts.counts == counts@, facts.activated == activated@, facts.after == db2,
        facts.stocks == stocks_for_parts_rows@, facts.ships == shipments_for_parts_rows@, facts.stock_delta == partsupp_delta@, facts.ship_delta == lineitem_delta@,
        facts.progress(work@, facts.stocks.len() as int, facts.ships.len() as int, index as int, 0), facts.coverage(work@, index as int, 0),
        next.suppliers == initial_suppliers, next.nations == initial_nations, next.names == initial_names, directory_matches(next.parts, next.stock_pairs),
        forall|key: i64| #[trigger] next.parts.lookup(key) == if eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
        parts_shape(state.parts), work_ready(work@, keys@, state.parts, counts@), decreases partsupp_delta.len() - index, {
        let ghost processed = index as int;
        let ghost before = work@;
        let delta = &partsupp_delta[index]; let key = delta.row.ps_partkey;
        if !activated.contains_key(&key) {
            touch(state, key, &counts, &mut work, &mut keys);
            let ghost loaded = work@;
            if work.contains_key(&key) { stock_change(&mut work, &delta.row, delta.sign as i128); }
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
        supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations), names_correct(previous_db, initial_names),
        facts.before == previous_db, facts.supplier_delta == supplier_delta@, facts.nation_delta == nation_delta@,
        facts.valid(), facts.parts == state.parts, facts.counts == counts@, facts.activated == activated@, facts.after == db2,
        facts.stocks == stocks_for_parts_rows@, facts.ships == shipments_for_parts_rows@, facts.stock_delta == partsupp_delta@, facts.ship_delta == lineitem_delta@,
        facts.progress(work@, facts.stocks.len() as int, facts.ships.len() as int, facts.stock_delta.len() as int, index as int), facts.coverage(work@, facts.stock_delta.len() as int, index as int),
        next.suppliers == initial_suppliers, next.nations == initial_nations, next.names == initial_names, directory_matches(next.parts, next.stock_pairs),
        forall|key: i64| #[trigger] next.parts.lookup(key) == if eligible(db2, key as int) == 0 { None } else { state.parts.lookup(key) },
        parts_shape(state.parts), work_ready(work@, keys@, state.parts, counts@), decreases lineitem_delta.len() - index, {
        let ghost processed = index as int;
        let ghost before = work@;
        let delta = &lineitem_delta[index]; let key = delta.row.l_partkey;
        if !activated.contains_key(&key) {
            touch(state, key, &counts, &mut work, &mut keys);
            let ghost loaded = work@;
            if work.contains_key(&key) { shipment_change(&mut work, &delta.row, delta.sign as i128); }
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
            supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations), names_correct(previous_db, initial_names),
            next.suppliers == initial_suppliers, next.nations == initial_nations, next.names == initial_names,
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
        let raw = match work.remove(&key) { Some(value) => value, None => { proof { assert(false); } return Err(12); } };
        let ghost source = raw;
        proof { facts.finish_ready_part(built, key, source); }
        let part = match raw.finish() { Some(value) => value, None => { proof { assert(false); } return Err(13); } };
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
            supplier_correct(previous_db, initial_suppliers), nation_correct(previous_db, initial_nations), names_correct(previous_db, initial_names),
            next.parts == finished_parts, next.stock_pairs == finished_directory, directory_matches(next.parts, next.stock_pairs),
            forall|key: i64| #![trigger next.parts.lookup(key)] part_correct(db2, key, next.parts.lookup(key)),
            next.suppliers == initial_suppliers, next.nations == initial_nations, next.names == initial_names,
            forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_work.count(initial_suppliers, nation, supplier, name, address) == supplier_count(initial_suppliers, nation, supplier, name, address)
                + sum(supplier_delta@.take(index as int), supplier_delta_weight(nation, supplier, name, address)),
        decreases supplier_delta.len() - index,
    {
        let ghost previous = supplier_work;
        let delta = &supplier_delta[index];
        supplier_work.change(&next.suppliers, &delta.row, delta.sign as i128);
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
    proof { crate::update_proofs::lemma_supplier_ready(previous_db, db2, initial_suppliers, supplier_work, supplier_delta@); }
    if !supplier_work.publish(&mut next.suppliers) { proof { assert(false); } return Err(15); }
    proof {
        lemma_supplier_updated(previous_db, db2, initial_suppliers, next.suppliers, supplier_delta@);
    }
    let ghost finished_suppliers = next.suppliers;
    let mut nation_work = NationWork::new(); let mut index = 0;
    while index < nation_delta.len()
        invariant index <= nation_delta.len(), nation_work.shape(), nation_work.hashes(), domain_valid(db2),
            facts.valid(), facts.after == db2, facts.before == previous_db, facts.nation_delta == nation_delta@,
            nation_correct(previous_db, initial_nations), names_correct(previous_db, initial_names), next.nations == initial_nations, next.names == initial_names,
            next.parts == finished_parts, next.stock_pairs == finished_directory, directory_matches(next.parts, next.stock_pairs),
            forall|key: i64| #![trigger next.parts.lookup(key)] part_correct(db2, key, next.parts.lookup(key)),
            next.suppliers == finished_suppliers, supplier_correct(db2, next.suppliers),
            forall|name: Seq<u8>, key: i64| #[trigger] nation_work.count(initial_nations, name, key) == nation_count(initial_nations, name, key)
                + sum(nation_delta@.take(index as int), nation_delta_weight(name, key)),
        decreases nation_delta.len() - index,
    {
        let ghost previous = nation_work;
        let delta = &nation_delta[index];
        nation_work.change(&next.nations, &delta.row, delta.sign as i128);
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
    proof { crate::update_proofs::lemma_nation_ready(previous_db, db2, initial_nations, nation_work, nation_delta@); }
    if !nation_work.publish(&mut next.nations) { proof { assert(false); } return Err(17); }
    proof { lemma_nation_updated(previous_db, db2, initial_nations, next.nations, nation_delta@); }
    let ghost finished_nations = next.nations;
    let mut name_work = NameWork::new(); let mut index = 0;
    while index < part_delta.len()
        invariant index <= part_delta.len(), name_work.shape(), domain_valid(db2),
            names_correct(previous_db, initial_names), next.names == initial_names,
            bag_step(previous_db.part, db2.part, part_delta_seq_view(part_delta@)),
            next.parts == finished_parts, next.stock_pairs == finished_directory, directory_matches(next.parts, next.stock_pairs),
            forall|key: i64| #![trigger next.parts.lookup(key)] part_correct(db2, key, next.parts.lookup(key)),
            next.suppliers == finished_suppliers, supplier_correct(db2, next.suppliers),
            next.nations == finished_nations, nation_correct(db2, next.nations),
            forall|key: i64, name: Seq<u8>| #[trigger] name_work.count(initial_names, key, name) == name_count(initial_names, key, name)
                + sum(part_delta@.take(index as int), name_delta_weight(key, name)),
        decreases part_delta.len() - index,
    {
        let ghost previous = name_work;
        let delta = &part_delta[index];
        name_work.change(&next.names, &delta.row, delta.sign as i128);
        proof {
            assert forall|key: i64, name: Seq<u8>| #[trigger] name_work.count(initial_names, key, name) == name_count(initial_names, key, name)
                + sum(part_delta@.take(index as int + 1), name_delta_weight(key, name)) by {
                lemma_sum_take(part_delta@, index as int, name_delta_weight(key, name));
                assert(previous.count(initial_names, key, name) == name_count(initial_names, key, name) + sum(part_delta@.take(index as int), name_delta_weight(key, name)));
            }
        }
        index += 1;
    }
    let ghost name_source = name_work;
    proof { assert(part_delta@.take(part_delta.len() as int) =~= part_delta@); }
    proof { crate::update_proofs::lemma_names_ready(previous_db, db2, initial_names, name_work, part_delta@); }
    if !name_work.publish(&mut next.names) { proof { assert(false); } return Err(19); }
    proof {
        lemma_names_updated(previous_db, db2, initial_names, next.names, part_delta@);
        use_type_invariant(&next.names); next.names.reveal_well_formed();
        use_type_invariant(&next.parts); next.parts.reveal_well_formed();
        use_type_invariant(&next.stock_pairs); next.stock_pairs.reveal_well_formed();
        use_type_invariant(&next.suppliers); next.suppliers.reveal_well_formed();
        use_type_invariant(&next.nations); next.nations.reveal_well_formed();
        crate::state_proofs::lemma_state_meaning(db2, next.parts, next.stock_pairs, next.suppliers, next.nations, next.names);
    }
    Ok(State { db: Ghost(db2), data: next })
}
}
