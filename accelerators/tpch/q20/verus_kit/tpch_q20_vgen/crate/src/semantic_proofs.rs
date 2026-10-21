use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use verus_common::sums::*;
use verus_common::sqlfns::like_match;
use verus_common::order::nondesc;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::Table;
use crate::profiles::Part;
use crate::dimensions::{Suppliers, Nations, PartNames};

verus! {
pub open spec fn qualifying_weight(key: int) -> spec_fn(PartRow) -> int {
    |row: PartRow| if row.p_partkey == key { 1 } else { 0 }
}
pub open spec fn part_capture_weight(key: int) -> spec_fn(PartDelta) -> int {
    |row: PartDelta| if row.row.p_partkey == key { row.sign as int } else { 0 }
}

pub proof fn lemma_build_part_count(db: Db, rows: Seq<PartInput>, key: int)
    requires scan_parts_ok(db, rows),
    ensures eligible(db, key) == sum(rows, |row: PartInput| if row.p_partkey == key { 1 } else { 0 }),
{
    let weight = qualifying_weight(key);
    let runtime_weight = |row: PartInput| if row.p_partkey == key { 1int } else { 0int };
    lemma_sum_map(rows, |row: PartInput| part_input_view(row), weight, runtime_weight);
    assert(sum(part_input_seq_view(rows), weight) == sum(db.part, weight));
}

pub proof fn lemma_apply_part_count(before: Db, after: Db, rows: Seq<PartDelta>, key: int)
    requires bag_step(before.part, after.part, part_delta_seq_view(rows)),
    ensures eligible(after, key) == eligible(before, key) + sum(rows, part_capture_weight(key)),
{
    let weight = qualifying_weight(key);
    let signed = |entry: (PartRow, int)| entry.1 * weight(entry.0);
    assert(sum(after.part, weight) == sum(before.part, weight) + sum(part_delta_seq_view(rows), signed));
    lemma_sum_map(rows, |delta: PartDelta| (part_input_view(delta.row), delta.sign as int), signed, part_capture_weight(key));
}
pub proof fn lemma_coalesced_after(before: Db, after: Db, rows: Seq<PartDelta>, parts: Table<Part>, counts: Map<i64, Wide>)
    requires bag_step(before.part, after.part, part_delta_seq_view(rows)),
        forall|key: i64| #![trigger part_count(parts, key)] part_count(parts, key) == eligible(before, key as int),
        forall|key: i64| #![trigger coalesced_part_count(parts, counts, key)]
            coalesced_part_count(parts, counts, key) == part_count(parts, key) + sum(rows, part_capture_weight(key as int)),
    ensures forall|key: i64| #![trigger coalesced_part_count(parts, counts, key)]
        coalesced_part_count(parts, counts, key) == eligible(after, key as int),
{
    assert forall|key: i64| #![trigger coalesced_part_count(parts, counts, key)]
        coalesced_part_count(parts, counts, key) == eligible(after, key as int) by {
        lemma_apply_part_count(before, after, rows, key as int);
        assert(part_count(parts, key) == eligible(before, key as int));
    }
}

pub proof fn lemma_execute_result(db: Db, parameters: Params, out: Seq<OutRow>)
    requires out.to_multiset() == crate::gen_spec::rows(db, parameters).to_multiset(),
        nondesc(out, ext_order()),
    ensures query_result(db, parameters, out),
{
    assert forall|left: int, right: int| #![trigger out[left], out[right]]
        0 <= left < right < out.len() implies !row_lt(out[right], out[left]) by {
        lemma_row_lt_ext(out[right], out[left]);
    }
}

pub proof fn lemma_eligible_presence(db: Db, key: int)
    ensures eligible(db, key) >= 0,
        (eligible(db, key) > 0) == (exists|index: int| #![trigger db.part[index]]
            0 <= index < db.part.len() && qualifying_weight(key)(db.part[index]) == 1),
{
    let weight = qualifying_weight(key);
    lemma_sum_nonneg(db.part, weight);
    if exists|index: int| #![trigger db.part[index]] 0 <= index < db.part.len() && weight(db.part[index]) == 1 {
        let index = choose|index: int| #![trigger db.part[index]] 0 <= index < db.part.len() && weight(db.part[index]) == 1;
        lemma_sum_pos(db.part, weight, index);
    } else { lemma_sum_zero(db.part, weight); }
}

pub proof fn lemma_stock_scan_weight(db: Db, rows: Seq<StockInput>, key: int, weight: spec_fn(PartsuppRow) -> int)
    requires scan_stocks_ok(db, rows),
    ensures sum(stock_input_seq_view(rows), weight) == sum(db.partsupp, weight),
{
}

pub proof fn lemma_shipment_scan_weight(db: Db, rows: Seq<ShipmentInput>, key: int, weight: spec_fn(LineitemRow) -> int)
    requires scan_shipments_ok(db, rows),
    ensures sum(shipment_input_seq_view(rows), weight) == sum(db.lineitem, weight),
{
}

pub open spec fn scan_db(db: Db, stocks: Seq<StockInput>, shipments: Seq<ShipmentInput>, suppliers: Seq<SupplierInput>, nations: Seq<NationInput>) -> Db {
    Db { part: db.part, partsupp: stock_input_seq_view(stocks), lineitem: shipment_input_seq_view(shipments),
        supplier: supplier_input_seq_view(suppliers), nation: nation_input_seq_view(nations) }
}

pub proof fn lemma_build_meaning(
    db: Db, stocks: Seq<StockInput>, shipments: Seq<ShipmentInput>, supplier_rows: Seq<SupplierInput>, nation_rows: Seq<NationInput>,
    parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations, names: PartNames,
)
    requires scan_stocks_ok(db, stocks), scan_shipments_ok(db, shipments),
        scan_suppliers_ok(db, supplier_rows), scan_nations_ok(db, nation_rows),
        meaning(scan_db(db, stocks, shipments, supplier_rows, nation_rows), parts, directory, suppliers, nations, names),
    ensures meaning(db, parts, directory, suppliers, nations, names),
{
    let projected = scan_db(db, stocks, shipments, supplier_rows, nation_rows);
    assert forall|part: i64, supplier: i64| #![trigger pair_of(parts, part, supplier)]
        match pair_of(parts, part, supplier) {
            Some(pair) => pair_valid(pair) && eligible(db, part as int) > 0
                && (pair.stocks@.len() > 0 || pair.ship@.len() > 0)
                && (forall|index: int| #![trigger pair.ship@[index]] 0 <= index < pair.ship@.len() ==>
                    pair.ship@[index].count@ == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { 1 } else { 0 })
                    && pair.ship@[index].quantity@ == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { row.l_quantity } else { 0 }))
                && (forall|value: int| #![trigger stored_stock(pair, value)] stored_stock(pair, value) == stock_count(db, part as int, supplier as int, value))
                && (forall|date: int| #![trigger stored_day_count(pair, date)] stored_day_count(pair, date) == daily_count(db, part as int, supplier as int, date)
                    && stored_day_quantity(pair, date) == daily_quantity(db, part as int, supplier as int, date)),
            None => eligible(db, part as int) > 0 ==>
                ((forall|value: int| #![trigger stock_count(db, part as int, supplier as int, value)] stock_count(db, part as int, supplier as int, value) == 0)
                && (forall|date: int| #![trigger daily_count(db, part as int, supplier as int, date)] daily_count(db, part as int, supplier as int, date) == 0)),
        } by {
        assert(eligible(projected, part as int) == eligible(db, part as int));
        if let Some(pair) = pair_of(parts, part, supplier) {
            assert(eligible(projected, part as int) > 0);
            assert(stored_stock(pair, 0) == stock_count(projected, part as int, supplier as int, 0));
        }
        if eligible(db, part as int) > 0 {
            assert forall|value: int| #![trigger stock_count(db, part as int, supplier as int, value)]
                stock_count(db, part as int, supplier as int, value) == stock_count(projected, part as int, supplier as int, value) by {
                lemma_stock_scan_weight(db, stocks, part as int, |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1 } else { 0 });
            }
            assert forall|date: int| #![trigger daily_count(db, part as int, supplier as int, date)]
                daily_count(db, part as int, supplier as int, date) == daily_count(projected, part as int, supplier as int, date)
                && daily_quantity(db, part as int, supplier as int, date) == daily_quantity(projected, part as int, supplier as int, date) by {
                lemma_shipment_scan_weight(db, shipments, part as int, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1 } else { 0 });
                lemma_shipment_scan_weight(db, shipments, part as int, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0 });
            }
            if let Some(pair) = pair_of(parts, part, supplier) {
                assert forall|index: int| #![trigger pair.ship@[index]] 0 <= index < pair.ship@.len() implies
                    pair.ship@[index].count@ == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { 1 } else { 0 })
                    && pair.ship@[index].quantity@ == sum(db.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { row.l_quantity } else { 0 }) by {
                    lemma_shipment_scan_weight(db, shipments, part as int, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { 1 } else { 0 });
                    lemma_shipment_scan_weight(db, shipments, part as int, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { row.l_quantity } else { 0 });
                }
            }
        }
    }
    assert forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #![trigger supplier_count(suppliers, nation, supplier, name, address)]
        supplier_count(suppliers, nation, supplier, name, address) == sum(db.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 }) by {
        let weight = |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1int } else { 0int };
        assert(sum(supplier_input_seq_view(supplier_rows), weight) == sum(db.supplier, weight));
    }
    assert forall|name: Seq<u8>, key: i64| #![trigger nation_count(nations, name, key)]
        nation_count(nations, name, key) == sum(db.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 }) by {
        let weight = |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1int } else { 0int };
        assert(sum(nation_input_seq_view(nation_rows), weight) == sum(db.nation, weight));
    }
    assert(names_correct(db, names));
    assert(parts.well_formed() && directory.well_formed() && suppliers.well_formed() && nations.well_formed() && names.well_formed());
    assert(forall|key: i64| #![trigger part_count(parts, key)] part_count(parts, key) == eligible(db, key as int));
    assert(forall|key: i64| #![trigger parts.lookup(key)] parts.lookup(key) matches Some(part) ==>
        part.count@ > 0 && part.pairs@.map_values(|entry: (i64, crate::profiles::Pair)| entry.0).no_duplicates());
    assert(forall|nation: i64| #![trigger suppliers.lookup(nation)]
        suppliers.lookup(nation) matches Some(bucket) ==> bucket.well_formed()
            && forall|supplier: i64| #![trigger bucket.lookup(supplier)]
                bucket.lookup(supplier) matches Some(rows) ==>
                    forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==> rows@[index].count@ > 0);
    assert(forall|hash: i64| #![trigger nations.lookup(hash)]
        nations.lookup(hash) matches Some(rows) ==>
            forall|index: int| #![trigger rows@[index]] 0 <= index < rows@.len() ==>
                rows@[index].count@ > 0 && crate::profiles::name_hash(rows@[index].name@) == hash);
}

pub proof fn lemma_meaning_equal_sums(
    before: Db, after: Db, parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations, names: PartNames,
)
    requires meaning(before, parts, directory, suppliers, nations, names),
        forall|weight: spec_fn(PartRow) -> int| #[trigger] sum(after.part, weight) == sum(before.part, weight),
        forall|weight: spec_fn(PartsuppRow) -> int| #[trigger] sum(after.partsupp, weight) == sum(before.partsupp, weight),
        forall|weight: spec_fn(LineitemRow) -> int| #[trigger] sum(after.lineitem, weight) == sum(before.lineitem, weight),
        forall|weight: spec_fn(SupplierRow) -> int| #[trigger] sum(after.supplier, weight) == sum(before.supplier, weight),
        forall|weight: spec_fn(NationRow) -> int| #[trigger] sum(after.nation, weight) == sum(before.nation, weight),
    ensures meaning(after, parts, directory, suppliers, nations, names),
{
    assert forall|key: i64| #[trigger] eligible(after, key as int) == eligible(before, key as int) by {
        let weight = qualifying_weight(key as int);
        assert(sum(after.part, weight) == sum(before.part, weight));
    }
    assert forall|part: int, supplier: int, value: int| #[trigger] stock_count(after, part, supplier, value) == stock_count(before, part, supplier, value) by {
        let weight = |row: PartsuppRow| if row.ps_partkey == part && row.ps_suppkey == supplier && row.ps_availqty == value { 1int } else { 0int };
        assert(sum(after.partsupp, weight) == sum(before.partsupp, weight));
    }
    assert forall|part: int, supplier: int, date: int| #![trigger daily_count(after, part, supplier, date)]
        daily_count(after, part, supplier, date) == daily_count(before, part, supplier, date)
        && daily_quantity(after, part, supplier, date) == daily_quantity(before, part, supplier, date) by {
        let count = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { 1int } else { 0int };
        let quantity = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate == date { row.l_quantity } else { 0int };
        assert(sum(after.lineitem, count) == sum(before.lineitem, count));
        assert(sum(after.lineitem, quantity) == sum(before.lineitem, quantity));
    }
    assert forall|part: i64, supplier: i64| #![trigger pair_of(parts, part, supplier)]
        pair_of(parts, part, supplier) matches Some(pair) ==>
            (forall|index: int| #![trigger pair.ship@[index]] 0 <= index < pair.ship@.len() ==>
                pair.ship@[index].count@ == sum(after.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { 1 } else { 0 })
                && pair.ship@[index].quantity@ == sum(after.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { row.l_quantity } else { 0 })) by {
        if let Some(pair) = pair_of(parts, part, supplier) {
            assert(stored_stock(pair, 0) == stock_count(before, part as int, supplier as int, 0));
            assert forall|index: int| #![trigger pair.ship@[index]] 0 <= index < pair.ship@.len() implies
                pair.ship@[index].count@ == sum(after.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { 1 } else { 0 })
                && pair.ship@[index].quantity@ == sum(after.lineitem, |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { row.l_quantity } else { 0 }) by {
                let count = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { 1int } else { 0int };
                let quantity = |row: LineitemRow| if row.l_partkey == part && row.l_suppkey == supplier && row.l_shipdate <= pair.ship@[index].date { row.l_quantity } else { 0int };
                assert(sum(after.lineitem, count) == sum(before.lineitem, count));
                assert(sum(after.lineitem, quantity) == sum(before.lineitem, quantity));
            }
        }
    }
    assert forall|nation: i64, supplier: i64, name: Seq<u8>, address: Seq<u8>| #![trigger supplier_count(suppliers, nation, supplier, name, address)]
        supplier_count(suppliers, nation, supplier, name, address) == sum(after.supplier, |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1 } else { 0 }) by {
        let weight = |row: SupplierRow| if row.s_nationkey == nation && row.s_suppkey == supplier && row.s_name == name && row.s_address == address { 1int } else { 0int };
        assert(sum(after.supplier, weight) == sum(before.supplier, weight));
    }
    assert forall|name: Seq<u8>, key: i64| #![trigger nation_count(nations, name, key)]
        nation_count(nations, name, key) == sum(after.nation, |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1 } else { 0 }) by {
        let weight = |row: NationRow| if row.n_name == name && row.n_nationkey == key { 1int } else { 0int };
        assert(sum(after.nation, weight) == sum(before.nation, weight));
    }    assert forall|key: i64, name: Seq<u8>| #[trigger] name_count(names, key, name) ==
        sum(after.part, |row: PartRow| if row.p_partkey == key && row.p_name == name { 1 } else { 0 }) by {
        let weight = |row: PartRow| if row.p_partkey == key && row.p_name == name { 1int } else { 0int };
        assert(sum(after.part, weight) == sum(before.part, weight));
    }
}

pub proof fn lemma_apply_meaning(
    before: Db, candidate: Db, after: Db,
    lineitem: Seq<(LineitemRow, int)>, nation: Seq<(NationRow, int)>, part: Seq<(PartRow, int)>,
    partsupp: Seq<(PartsuppRow, int)>, supplier: Seq<(SupplierRow, int)>,
    parts: Table<Part>, directory: Table<Vec<i64>>, suppliers: Suppliers, nations: Nations, names: PartNames,
)
    requires capture_ok(before, candidate, lineitem, nation, part, partsupp, supplier),
        capture_ok(before, after, lineitem, nation, part, partsupp, supplier),
        meaning(candidate, parts, directory, suppliers, nations, names),
    ensures meaning(after, parts, directory, suppliers, nations, names),
{
    assert forall|weight: spec_fn(PartRow) -> int| #[trigger] sum(after.part, weight) == sum(candidate.part, weight) by {
        assert(sum(after.part, weight) == sum(before.part, weight) + sum(part, |change: (PartRow, int)| change.1 * weight(change.0)));
        assert(sum(candidate.part, weight) == sum(before.part, weight) + sum(part, |change: (PartRow, int)| change.1 * weight(change.0)));
    }
    assert forall|weight: spec_fn(PartsuppRow) -> int| #[trigger] sum(after.partsupp, weight) == sum(candidate.partsupp, weight) by {
        assert(sum(after.partsupp, weight) == sum(before.partsupp, weight) + sum(partsupp, |change: (PartsuppRow, int)| change.1 * weight(change.0)));
        assert(sum(candidate.partsupp, weight) == sum(before.partsupp, weight) + sum(partsupp, |change: (PartsuppRow, int)| change.1 * weight(change.0)));
    }
    assert forall|weight: spec_fn(LineitemRow) -> int| #[trigger] sum(after.lineitem, weight) == sum(candidate.lineitem, weight) by {
        assert(sum(after.lineitem, weight) == sum(before.lineitem, weight) + sum(lineitem, |change: (LineitemRow, int)| change.1 * weight(change.0)));
        assert(sum(candidate.lineitem, weight) == sum(before.lineitem, weight) + sum(lineitem, |change: (LineitemRow, int)| change.1 * weight(change.0)));
    }
    assert forall|weight: spec_fn(SupplierRow) -> int| #[trigger] sum(after.supplier, weight) == sum(candidate.supplier, weight) by {
        assert(sum(after.supplier, weight) == sum(before.supplier, weight) + sum(supplier, |change: (SupplierRow, int)| change.1 * weight(change.0)));
        assert(sum(candidate.supplier, weight) == sum(before.supplier, weight) + sum(supplier, |change: (SupplierRow, int)| change.1 * weight(change.0)));
    }
    assert forall|weight: spec_fn(NationRow) -> int| #[trigger] sum(after.nation, weight) == sum(candidate.nation, weight) by {
        assert(sum(after.nation, weight) == sum(before.nation, weight) + sum(nation, |change: (NationRow, int)| change.1 * weight(change.0)));
        assert(sum(candidate.nation, weight) == sum(before.nation, weight) + sum(nation, |change: (NationRow, int)| change.1 * weight(change.0)));
    }
    lemma_meaning_equal_sums(candidate, after, parts, directory, suppliers, nations, names);
}
}
