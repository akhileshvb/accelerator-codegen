use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::profiles::*;
use crate::profile_proofs::*;
use crate::storage::*;
use crate::dimensions::*;
use verus_common::sqlfns::like_match;

verus! {
pub open spec fn database_prefix(db: Db, part: i64, supplier: i64, date: int, quantity: bool) -> int {
    sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= date {
        if quantity { row.l_quantity } else { 1 }
    } else { 0 })
}
pub proof fn lemma_database_prefix_weight(db: Db, part: i64, supplier: i64, date: int, quantity: bool, weight: spec_fn(LineitemRow) -> int)
    requires forall|row: LineitemRow| #[trigger] weight(row) == if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= date { if quantity { row.l_quantity } else { 1int } } else { 0int },
    ensures database_prefix(db, part, supplier, date, quantity) == sum(db.lineitem, weight),
{
    lemma_sum_ext_all(db.lineitem,
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= date { if quantity { row.l_quantity } else { 1int } } else { 0int }, weight);
}
pub proof fn lemma_build_domain(db: Db, parts: Seq<PartInput>, stocks: Seq<StockInput>, shipments: Seq<ShipmentInput>, suppliers: Seq<SupplierInput>, nations: Seq<NationInput>)
    requires scan_parts_ok(db, parts), scan_stocks_ok(db, stocks), scan_shipments_ok(db, shipments),
        scan_suppliers_ok(db, suppliers), scan_nations_ok(db, nations),
    ensures domain_valid(db),
{
    lemma_scan_property(supplier_input_seq_view(suppliers), db.supplier,
        |row: SupplierRow| machine_key(row.s_suppkey) && machine_key(row.s_nationkey));
    lemma_scan_property(nation_input_seq_view(nations), db.nation, |row: NationRow| machine_key(row.n_nationkey));
    lemma_scan_property(part_input_seq_view(parts), db.part, |row: PartRow| machine_key(row.p_partkey));
    lemma_scan_property(stock_input_seq_view(stocks), db.partsupp,
        |row: PartsuppRow| machine_key(row.ps_partkey) && machine_key(row.ps_suppkey));
    lemma_scan_property(shipment_input_seq_view(shipments), db.lineitem,
        |row: LineitemRow| machine_key(row.l_partkey) && machine_key(row.l_suppkey));
}
pub proof fn lemma_supplier_build(db: Db, state: Suppliers, rows: Seq<SupplierInput>)
    requires db.supplier == supplier_input_seq_view(rows), suppliers_positive(state),
        forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_count(state, nation, supplier, name, address) ==
            sum(rows, |row: SupplierInput| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name@ == name && row.s_address@ == address { 1 } else { 0 }),
    ensures supplier_correct(db, state),
{
    assert forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_count(state, nation, supplier, name, address) ==
        sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 }) by {
        let weight = |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1int } else { 0int };
        let direct = |row: SupplierInput| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name@ == name && row.s_address@ == address { 1int } else { 0int };
        lemma_sum_map(rows, |row: SupplierInput| supplier_input_view(row), weight, direct);
        assert(supplier_count(state, nation, supplier, name, address) == sum(rows, direct));
    }
}
pub proof fn lemma_nation_build(db: Db, state: Nations, rows: Seq<NationInput>)
    requires db.nation == nation_input_seq_view(rows), nations_positive(state),
        forall|name: Seq<u8>, key: i64| #[trigger] nation_count(state, name, key) ==
            sum(rows, |row: NationInput| if row.n_name@ == name && row.n_nationkey == key { 1 } else { 0 }),
    ensures nation_correct(db, state),
{
    assert forall|name: Seq<u8>, key: i64| #[trigger] nation_count(state, name, key) ==
        sum(db.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 }) by {
        let weight = |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1int } else { 0int };
        let direct = |row: NationInput| if row.n_name@ == name && row.n_nationkey == key { 1int } else { 0int };
        lemma_sum_map(rows, |row: NationInput| nation_input_view(row), weight, direct);
        assert(nation_count(state, name, key) == sum(rows, direct));
    }
}
pub proof fn lemma_names_build(db: Db, state: PartNames, rows: Seq<PartInput>)
    requires scan_parts_ok(db, rows), names_positive(state),
        forall|key: i64, name: Seq<u8>| #[trigger] name_count(state, key, name) ==
            sum(rows, |row: PartInput| if row.p_partkey == key && row.p_name@ == name { 1 } else { 0 }),
    ensures names_correct(db, state),
{
    assert forall|key: i64, name: Seq<u8>| #[trigger] name_count(state, key, name) ==
        sum(db.part, |row: PartRow| if row.p_partkey == key && row.p_name == name { 1 } else { 0 }) by {
        let weight = |row: PartRow| if row.p_partkey == key && row.p_name == name { 1int } else { 0int };
        let direct = |row: PartInput| if row.p_partkey == key && row.p_name@ == name { 1int } else { 0int };
        lemma_sum_map(rows, |row: PartInput| part_input_view(row), weight, direct);
        assert(sum(part_input_seq_view(rows), weight) == sum(db.part, weight));
        assert(name_count(state, key, name) == sum(rows, direct));
    }
}
pub proof fn lemma_build_part(db: Db, key: i64, source: PartWork, part: &Part, stocks: Seq<StockInput>, shipments: Seq<ShipmentInput>)
    requires db.partsupp == stock_input_seq_view(stocks), db.lineitem == shipment_input_seq_view(shipments),
        source.shape(), part_shape(*part), part.count == source.count, part.count > 0, source.count == eligible(db, key as int),
        forall|supplier: i64, value: int| #![trigger source.stock_for(supplier, value)] source.stock_for(supplier, value) ==
            sum(stocks, |row: StockInput| if row.ps_partkey == key && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 }),
        forall|supplier: i64, date: int| #![trigger source.day_for(supplier, date, false)]
            source.day_for(supplier, date, false) == sum(shipments, |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 })
            && source.day_for(supplier, date, true) == sum(shipments, |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0 }),
        forall|supplier: i64, value: int| #![trigger part_stock(part.pairs@, supplier, value)] part_stock(part.pairs@, supplier, value) == source.stock_for(supplier, value),
        forall|supplier: i64, date: int, quantity: bool| #![trigger part_day(part.pairs@, supplier, date, quantity)] part_day(part.pairs@, supplier, date, quantity) == source.day_for(supplier, date, quantity),
    ensures part_correct(db, key, Some(*part)),
{
    assert forall|supplier: i64, value: int| #![trigger part_stock(part.pairs@, supplier, value)] part_stock(part.pairs@, supplier, value) == stock_count(db, key as int, supplier as int, value) by {
        let weight = |row: PartsuppRow| if row.ps_partkey == key && row.ps_suppkey == supplier && row.ps_availqty == value { 1int } else { 0int };
        let direct = |row: StockInput| if row.ps_partkey == key && row.ps_suppkey == supplier && row.ps_availqty == value { 1int } else { 0int };
        lemma_sum_map(stocks, |row: StockInput| stock_input_view(row), weight, direct);
        assert(source.stock_for(supplier, value) == sum(stocks, direct));
    }
    assert forall|supplier: i64, date: int| #![trigger part_day(part.pairs@, supplier, date, false)]
        part_day(part.pairs@, supplier, date, false) == daily_count(db, key as int, supplier as int, date)
            && part_day(part.pairs@, supplier, date, true) == daily_quantity(db, key as int, supplier as int, date) by {
        let count = |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0int };
        let quantity = |row: LineitemRow| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0int };
        let direct_count = |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0int };
        let direct_quantity = |row: ShipmentInput| if row.l_partkey == key && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity as int } else { 0int };
        lemma_sum_map(shipments, |row: ShipmentInput| shipment_input_view(row), count, direct_count);
        lemma_sum_map(shipments, |row: ShipmentInput| shipment_input_view(row), quantity, direct_quantity);
        assert(source.day_for(supplier, date, false) == sum(shipments, direct_count));
        assert(source.day_for(supplier, date, true) == sum(shipments, direct_quantity));
    }
}
pub proof fn lemma_profile_prefix(db: Db, part: i64, supplier: i64, pair: &Pair, bound: int, quantity: bool)
    requires pair_valid(*pair), 0 <= bound < pair.ship@.len(),
        forall|date: int| #![trigger stored_day_count(*pair, date)] stored_day_count(*pair, date) == daily_count(db, part as int, supplier as int, date)
            && stored_day_quantity(*pair, date) == daily_quantity(db, part as int, supplier as int, date),
    ensures (if quantity { pair.ship@[bound].quantity as int } else { pair.ship@[bound].count as int }) ==
        database_prefix(db, part, supplier, pair.ship@[bound].date as int, quantity),
{
    lemma_pair_order(pair);
    let positions = rng(pair.ship@.len() as int);
    let project = |index: int| pair.ship@[index].date as int;
    let delta = |index: int| (if quantity { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
        - if index == 0 { 0 } else if quantity { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int };
    let date_of = |row: LineitemRow| row.l_shipdate;
    let selected = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier { if quantity { row.l_quantity } else { 1int } } else { 0int };
    assert forall|date: int| #[trigger] class_total(positions, project, delta, date) == class_total(db.lineitem, date_of, selected, date) by {
        assert(stored_day_count(*pair, date) == daily_count(db, part as int, supplier as int, date));
        assert(stored_day_quantity(*pair, date) == daily_quantity(db, part as int, supplier as int, date));
        lemma_sum_ext_all(positions, |index: int| if project(index) == date { delta(index) } else { 0 },
            |index: int| if pair.ship@[index].date == date {
                (if quantity { pair.ship@[index].quantity as int } else { pair.ship@[index].count as int })
                    - if index == 0 { 0 } else if quantity { pair.ship@[index - 1].quantity as int } else { pair.ship@[index - 1].count as int }
            } else { 0 });
        assert(class_total(positions, project, delta, date) == ship_measure(pair.ship@, date, quantity));
        if quantity {
            lemma_sum_ext_all(db.lineitem, |row: LineitemRow| if date_of(row) == date { selected(row) } else { 0 },
                |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0 });
        } else {
            lemma_sum_ext_all(db.lineitem, |row: LineitemRow| if date_of(row) == date { selected(row) } else { 0 },
                |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0 });
        }
    }
    let keep = |date: int| if date <= pair.ship@[bound].date { 1int } else { 0int };
    lemma_equal_classes(positions, db.lineitem, project, date_of, delta, selected, keep);
    let values = pair.ship@.map_values(|cell: Prefix| if quantity { cell.quantity as int } else { cell.count as int });
    let prefix = |index: int| pair.ship@[index].date <= pair.ship@[bound].date;
    assert forall|index: int| #![trigger prefix(index)] 0 <= index < pair.ship@.len() implies prefix(index) == (index < bound + 1) by {
        if index < bound { assert(pair.ship@[index].date < pair.ship@[bound].date); }
        if bound < index { assert(pair.ship@[bound].date < pair.ship@[index].date); }
    }
    lemma_prefix_cutoff(values, prefix, bound + 1, pair.ship@.len() as int);
    lemma_sum_ext(positions, |index: int| delta(index) * keep(project(index)),
        |index: int| if prefix(index) { values[index] - if index == 0 { 0 } else { values[index - 1] } } else { 0 });
    lemma_sum_ext_all(db.lineitem, |row: LineitemRow| selected(row) * keep(date_of(row)),
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[bound].date as int {
            if quantity { row.l_quantity } else { 1int }
        } else { 0int });
    let date = pair.ship@[bound].date as int;
    lemma_sum_ext_all(db.lineitem,
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[bound].date as int { if quantity { row.l_quantity } else { 1int } } else { 0int },
        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= date { if quantity { row.l_quantity } else { 1int } } else { 0int });
}
pub proof fn lemma_stock_support_lookup(part: &Part, supplier: i64)
    requires part_shape(*part),
    ensures stock_support(part.pairs@, supplier) == match pair_lookup(part.pairs@, supplier) { Some(pair) => pair.stocks@.len() > 0, None => false },
{
    lemma_unique_key_lookup(part.pairs@, supplier);
    if stock_support(part.pairs@, supplier) {
        let index = choose|index: int| #![trigger part.pairs@[index]] 0 <= index < part.pairs@.len() && part.pairs@[index].0 == supplier && part.pairs@[index].1.stocks@.len() > 0;
        assert(pair_lookup(part.pairs@, supplier) == Some(part.pairs@[index].1));
    }
    if let Some(pair) = pair_lookup(part.pairs@, supplier) {
        let index = choose|index: int| #![trigger part.pairs@[index]] 0 <= index < part.pairs@.len() && part.pairs@[index].0 == supplier;
        assert(pair == part.pairs@[index].1);
    }
}
pub proof fn lemma_state_meaning(db: Db, parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations, names: PartNames)
    requires forall|key: i64| #![trigger parts.lookup(key)] part_correct(db, key, parts.lookup(key)),
        parts.well_formed(), directory.well_formed(), suppliers.well_formed(), nations.well_formed(), names.well_formed(), names_correct(db, names),
        directory_matches(parts, directory), suppliers_positive(suppliers), nations_positive(nations),
        forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #[trigger] supplier_count(suppliers, nation, supplier, name, address) ==
            sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 }),
        forall|name: Seq<u8>, key: i64| #[trigger] nation_count(nations, name, key) ==
            sum(db.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 }),
    ensures meaning(db, parts, directory, suppliers, nations, names),
{
    assert forall|key: i64| #![trigger part_count(parts, key)] part_count(parts, key) == eligible(db, key as int) by {
        assert(part_correct(db, key, parts.lookup(key)));
    }
    assert forall|part: i64, supplier: i64| #![trigger pair_of(parts, part, supplier)]
        match pair_of(parts, part, supplier) {
            Some(pair) => pair_valid(pair) && eligible(db, part as int) > 0 && (pair.stocks@.len() > 0 || pair.ship@.len() > 0)
                && (forall|index: int| #![trigger pair.ship@[index]] 0 <= index < pair.ship@.len() ==>
                    pair.ship@[index].count == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { 1 } else { 0 })
                    && pair.ship@[index].quantity == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { row.l_quantity } else { 0 }))
                && (forall|value: int| #![trigger stored_stock(pair, value)] stored_stock(pair, value) == stock_count(db, part as int, supplier as int, value))
                && (forall|date: int| #![trigger stored_day_count(pair, date)] stored_day_count(pair, date) == daily_count(db, part as int, supplier as int, date)
                    && stored_day_quantity(pair, date) == daily_quantity(db, part as int, supplier as int, date)),
            None => eligible(db, part as int) > 0 ==> ((forall|value: int| #![trigger stock_count(db, part as int, supplier as int, value)] stock_count(db, part as int, supplier as int, value) == 0)
                && (forall|date: int| #![trigger daily_count(db, part as int, supplier as int, date)] daily_count(db, part as int, supplier as int, date) == 0)),
        } by {
        assert(part_correct(db, part, parts.lookup(part)));
        if let Some(data) = parts.lookup(part) {
            if let Some(pair) = pair_lookup(data.pairs@, supplier) {
                lemma_unique_key_lookup(data.pairs@, supplier);
                let index = choose|index: int| #![trigger data.pairs@[index]] 0 <= index < data.pairs@.len() && data.pairs@[index].0 == supplier;
                assert(pair == data.pairs@[index].1);
                assert forall|value: int| #![trigger stored_stock(pair, value)] stored_stock(pair, value) == stock_count(db, part as int, supplier as int, value) by {
                    assert(part_stock(data.pairs@, supplier, value) == stock_count(db, part as int, supplier as int, value));
                }
                assert forall|date: int| #![trigger stored_day_count(pair, date)] stored_day_count(pair, date) == daily_count(db, part as int, supplier as int, date)
                    && stored_day_quantity(pair, date) == daily_quantity(db, part as int, supplier as int, date) by {
                    assert(part_day(data.pairs@, supplier, date, false) == daily_count(db, part as int, supplier as int, date));
                }
                assert forall|offset: int| #![trigger pair.ship@[offset]] 0 <= offset < pair.ship@.len() implies
                    pair.ship@[offset].count == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[offset].date { 1 } else { 0 })
                    && pair.ship@[offset].quantity == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[offset].date { row.l_quantity } else { 0 }) by {
                    lemma_profile_prefix(db, part, supplier, &pair, offset, false);
                    lemma_profile_prefix(db, part, supplier, &pair, offset, true);
                    assert(pair.ship@[offset].count as int == database_prefix(db, part, supplier, pair.ship@[offset].date as int, false));
                    assert(pair.ship@[offset].quantity as int == database_prefix(db, part, supplier, pair.ship@[offset].date as int, true));
                    let date = pair.ship@[offset].date as int;
                    lemma_database_prefix_weight(db, part, supplier, date, false,
                        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[offset].date { 1int } else { 0int });
                    lemma_database_prefix_weight(db, part, supplier, date, true,
                        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[offset].date { row.l_quantity } else { 0int });
                    lemma_sum_ext_all(db.lineitem,
                        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= date { if false { row.l_quantity } else { 1int } } else { 0int },
                        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[offset].date { 1int } else { 0int });
                    lemma_sum_ext_all(db.lineitem,
                        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= date { if true { row.l_quantity } else { 1int } } else { 0int },
                        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[offset].date { row.l_quantity } else { 0int });
                    assert(database_prefix(db, part, supplier, date, false) == sum(db.lineitem,
                        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[offset].date { 1int } else { 0int }));
                    assert(database_prefix(db, part, supplier, date, true) == sum(db.lineitem,
                        |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[offset].date { row.l_quantity } else { 0int }));
                }
            } else {
                assert forall|value: int| #![trigger stock_count(db, part as int, supplier as int, value)] stock_count(db, part as int, supplier as int, value) == 0 by {
                    assert(part_stock(data.pairs@, supplier, value) == stock_count(db, part as int, supplier as int, value));
                }
                assert forall|date: int| #![trigger daily_count(db, part as int, supplier as int, date)] daily_count(db, part as int, supplier as int, date) == 0 by {
                    assert(part_day(data.pairs@, supplier, date, false) == daily_count(db, part as int, supplier as int, date));
                }
            }
        }
    }
}
pub proof fn lemma_daily_empty(db: Db, part: i64, supplier: i64, date: int)
    requires daily_count(db, part as int, supplier as int, date) == 0,
    ensures daily_quantity(db, part as int, supplier as int, date) == 0,
{
    let count = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0int };
    let quantity = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0int };
    assert forall|index: int| #![trigger quantity(db.lineitem[index])] 0 <= index < db.lineitem.len() implies quantity(db.lineitem[index]) == 0 by {
        if count(db.lineitem[index]) != 0 { lemma_sum_pos(db.lineitem, count, index); }
    }
    lemma_sum_zero(db.lineitem, quantity);
}
pub proof fn lemma_meaning_components(db: Db, parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations, names: PartNames)
    requires meaning(db, parts, directory, suppliers, nations, names),
    ensures parts_shape(parts), directory_matches(parts, directory), supplier_correct(db, suppliers), nation_correct(db, nations), names_correct(db, names),
        forall|key: i64| #![trigger parts.lookup(key)] part_correct(db, key, parts.lookup(key)),
{
    assert forall|key: i64| #![trigger parts.lookup(key)] part_correct(db, key, parts.lookup(key)) by {
        assert(part_count(parts, key) == eligible(db, key as int));
        if let Some(data) = parts.lookup(key) {
            assert forall|index: int| #![trigger data.pairs@[index]] 0 <= index < data.pairs@.len() implies
                pair_valid(data.pairs@[index].1) && (data.pairs@[index].1.stocks@.len() > 0 || data.pairs@[index].1.ship@.len() > 0) by {
                lemma_pair_at(parts, key, data.pairs@[index].0, &data, index);
            }
            assert forall|supplier: i64, value: int| #![trigger part_stock(data.pairs@, supplier, value)] part_stock(data.pairs@, supplier, value) == stock_count(db, key as int, supplier as int, value) by {
                if let Some(pair) = pair_of(parts, key, supplier) { assert(stored_stock(pair, value) == stock_count(db, key as int, supplier as int, value)); }
                else { assert(stock_count(db, key as int, supplier as int, value) == 0); }
            }
            assert forall|supplier: i64, date: int| #![trigger part_day(data.pairs@, supplier, date, false)]
                part_day(data.pairs@, supplier, date, false) == daily_count(db, key as int, supplier as int, date)
                && part_day(data.pairs@, supplier, date, true) == daily_quantity(db, key as int, supplier as int, date) by {
                if let Some(pair) = pair_of(parts, key, supplier) {
                    assert(stored_stock(pair, 0) == stock_count(db, key as int, supplier as int, 0));
                    assert(stored_day_count(pair, date) == daily_count(db, key as int, supplier as int, date));
                    assert(stored_day_quantity(pair, date) == daily_quantity(db, key as int, supplier as int, date));
                } else {
                    assert(daily_count(db, key as int, supplier as int, date) == 0);
                    lemma_daily_empty(db, key, supplier, date);
                }
            }
        }
    }
}
}
