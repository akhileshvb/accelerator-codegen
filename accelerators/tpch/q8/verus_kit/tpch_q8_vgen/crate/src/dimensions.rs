use vstd::prelude::*;
use verus_common::sums::*;
use crate::table::*;
use crate::dictionary::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use verus_common::bag::*;

verus! {

/// The column of the ids that a table of this mode holds in `first`.
pub open spec fn mode_column(mode: int) -> int {
    if mode == 0 { 0 } else if mode == 4 || mode == 6 { 1 } else { 2 }
}

pub open spec fn dimension_support(key: Major, minor: Major, dictionary: Dictionary, mode: int) -> bool {
    key.1 == 0 && key.2 == 0 && minor.2 == 0
        && if mode == 0 { minor.1 == 0 && dictionary.types.known(minor.0) }
            else if mode == 5 { minor.1 == 0 && dictionary.regions.known(minor.0) }
            else if mode == 3 { minor.1 == 1995 || minor.1 == 1996 }
            else if mode == 4 { dictionary.names.known(minor.0) }
            else if mode == 6 { minor.1 == 0 && dictionary.names.known(minor.0) }
            else { minor.1 == 0 }
}

pub open spec fn work_support(work: Work, dictionary: Dictionary, mode: int) -> bool {
    &&& forall|key: Major, minor: Major| #[trigger] work.at(key, minor, true) == 0
    &&& forall|key: Major, minor: Major| !dimension_support(key, minor, dictionary, mode) ==> #[trigger] work.at(key, minor, false) == 0
}

/// A scanned table without volumes and with nothing outside its support.
pub open spec fn supported_table(table: Table, dictionary: Dictionary, mode: int) -> bool {
    &&& table.valid()
    &&& forall|key: Major, minor: Major| #[trigger] table.at(key, minor, true) == 0
    &&& forall|key: Major, minor: Major| !dimension_support(key, minor, dictionary, mode) ==> #[trigger] table.at(key, minor, false) == 0
}

/// A scanned dimension table: supported, and no negative counts.
pub open spec fn dimension_table(table: Table, dictionary: Dictionary, mode: int) -> bool {
    supported_table(table, dictionary, mode) && forall|key: Major, minor: Major| #[trigger] table.at(key, minor, false) >= 0
}

pub proof fn lemma_support_from_cells(table: Table, dictionary: Dictionary, mode: int)
    requires forall|key: Major, index: int| 0 <= index < table.rows(key).len() ==>
        dimension_support(key, minor_of(#[trigger] table.rows(key)[index]), dictionary, mode)
            && table.rows(key)[index].volume@ == 0,
    ensures
        forall|key: Major, minor: Major| #[trigger] table.at(key, minor, true) == 0,
        forall|key: Major, minor: Major| !dimension_support(key, minor, dictionary, mode) ==> #[trigger] table.at(key, minor, false) == 0,
{
    hide(sum);
    assert forall|key: Major, minor: Major| #[trigger] table.at(key, minor, true) == 0 by {
        reveal(Table::at);
        reveal(cell_sum);
        lemma_sum_zero(table.rows(key), |cell: Cell| cell_weight(cell, minor, true));
    }
    assert forall|key: Major, minor: Major| !dimension_support(key, minor, dictionary, mode) implies #[trigger] table.at(key, minor, false) == 0 by {
        assert forall|index: int| 0 <= index < table.rows(key).len() implies
            cell_weight(#[trigger] table.rows(key)[index], minor, false) == 0 by {
            assert(dimension_support(key, minor_of(table.rows(key)[index]), dictionary, mode));
        }
        reveal(Table::at);
        reveal(cell_sum);
        lemma_sum_zero(table.rows(key), |cell: Cell| cell_weight(cell, minor, false));
    }
}

pub proof fn lemma_dimension_nonneg(table: Table, data: Data, db: Db, mode: int)
    requires dimension_meaning(data, db), 0 <= mode <= 5,
        table == if mode == 0 { data.parts } else if mode == 1 { data.suppliers } else if mode == 2 { data.customers }
            else if mode == 3 { data.orders } else if mode == 4 { data.nations } else { data.regions },
        forall|key: Major, minor: Major| !dimension_support(key, minor, data.dictionary, mode) ==> #[trigger] table.at(key, minor, false) == 0,
    ensures forall|key: Major, minor: Major| #[trigger] table.at(key, minor, false) >= 0,
{
    hide(sum);
    reveal(dimension_meaning);
    assert forall|key: Major, minor: Major| #[trigger] table.at(key, minor, false) >= 0 by {
        if dimension_support(key, minor, data.dictionary, mode) {
            if mode == 0 {
                lemma_sum_nonneg(db.part, |row: PartRow| if row.p_partkey == key.0 as int && text_matches(data.dictionary, 0, minor.0, row.p_type) { 1int } else { 0int });
            } else if mode == 1 {
                lemma_sum_nonneg(db.supplier, |row: SupplierRow| if row.s_suppkey == key.0 as int && row.s_nationkey == minor.0 as int { 1int } else { 0int });
            } else if mode == 2 {
                lemma_sum_nonneg(db.customer, |row: CustomerRow| if row.c_custkey == key.0 as int && row.c_nationkey == minor.0 as int { 1int } else { 0int });
            } else if mode == 3 {
                lemma_sum_nonneg(db.orders, |row: OrdersRow| if row.o_orderkey == key.0 as int && row.o_custkey == minor.0 as int
                    && minor.1 != 0 && year_of(row.o_orderdate) == minor.1 as int { 1int } else { 0int });
            } else if mode == 4 {
                lemma_sum_nonneg(db.nation, |row: NationRow| if row.n_nationkey == key.0 as int && row.n_regionkey == minor.1 as int
                    && text_matches(data.dictionary, 1, minor.0, row.n_name) { 1int } else { 0int });
            } else {
                lemma_sum_nonneg(db.region, |row: RegionRow| if row.r_regionkey == key.0 as int && text_matches(data.dictionary, 2, minor.0, row.r_name) { 1int } else { 0int });
            }
        }
    }
}

pub proof fn lemma_reverse_cells(table: Table, data: Data, db: Db, orders: bool)
    requires dimension_meaning(data, db), table == if orders { data.orders_by_customer } else { data.nations_by_region }, table.valid(),
        forall|key: Major, minor: Major| #[trigger] table.at(key, minor, true) == 0,
        forall|key: Major, minor: Major| !dimension_support(key, minor, data.dictionary, 1) ==>
            #[trigger] table.at(key, minor, false) == 0,
    ensures positive(table), scalar(table),
{
    hide(sum);
    reveal(dimension_meaning);
    assert forall|key: Major, minor: Major| #[trigger] table.at(key, minor, false) >= 0 by {
        if dimension_support(key, minor, data.dictionary, 1) {
            if orders {
                lemma_sum_nonneg(db.orders, |row: OrdersRow| if row.o_orderkey == minor.0 as int && row.o_custkey == key.0 as int
                    && year_of(row.o_orderdate) != 0 { 1int } else { 0int });
            } else {
                lemma_sum_nonneg(db.nation, |row: NationRow| if row.n_regionkey == key.0 as int && row.n_nationkey == minor.0 as int { 1int } else { 0int });
            }
        }
    }
    lemma_dimension_cells(table, data.dictionary, 1);
}

pub proof fn lemma_dimension_cells(table: Table, dictionary: Dictionary, mode: int)
    requires table.valid(), 0 <= mode <= 6,
        forall|key: Major, minor: Major| #[trigger] table.at(key, minor, true) == 0,
        forall|key: Major, minor: Major| #[trigger] table.at(key, minor, false) >= 0,
        forall|key: Major, minor: Major| !dimension_support(key, minor, dictionary, mode) ==> #[trigger] table.at(key, minor, false) == 0,
    ensures positive(table),
        (mode == 0 || mode == 1 || mode == 2 || mode == 5) ==> scalar(table),
        (mode == 0 || mode == 4 || mode == 5 || mode == 6) ==> text_domain(table, dictionary, mode_column(mode)),
        forall|key: Major, index: int| 0 <= index < table.rows(key).len() ==>
            dimension_support(key, minor_of(#[trigger] table.rows(key)[index]), dictionary, mode)
                && table.rows(key)[index].volume@ == 0,
{
    hide(sum);
    assert forall|key: Major, index: int| 0 <= index < table.rows(key).len() implies
        (#[trigger] table.rows(key)[index]).count@ > 0
            && dimension_support(key, minor_of(table.rows(key)[index]), dictionary, mode)
            && table.rows(key)[index].volume@ == 0 by {
        let rows = table.rows(key);
        let minor = minor_of(rows[index]);
        reveal(rows_valid);
        assert(rows_valid(rows));
        lemma_cell_lookup(rows, minor, index);
        reveal(Table::at);
        assert(rows[index].count@ == table.at(key, minor, false));
        assert(rows[index].volume@ == table.at(key, minor, true));
        assert(rows[index].count@ > 0);
    }
    if mode == 0 || mode == 1 || mode == 2 || mode == 5 {
        assert forall|key: Major, index: int| 0 <= index < table.rows(key).len() implies
            scalar_cell(key, #[trigger] table.rows(key)[index]) by {
            assert(dimension_support(key, minor_of(table.rows(key)[index]), dictionary, mode));
        }
        assert(scalar(table));
    }
    if mode == 0 || mode == 4 || mode == 5 || mode == 6 {
        assert forall|key: Major, index: int| 0 <= index < table.rows(key).len() implies
            dictionary.col(mode_column(mode)).known((#[trigger] table.rows(key)[index]).first) by {
            assert(dimension_support(key, minor_of(table.rows(key)[index]), dictionary, mode));
        }
        assert(text_domain(table, dictionary, mode_column(mode)));
    }
}

pub proof fn lemma_part_scan(table: Table, dictionary: Dictionary, scanned_dictionary: Dictionary, rows: Seq<PartInput>, db: Db)
    requires scan_parts_ok(db, rows), extension(scanned_dictionary, dictionary),
        forall|index: int| 0 <= index < rows.len() ==> text_present(scanned_dictionary, 0, (#[trigger] rows[index]).p_type@),
        forall|key: i64, kind: i64| #[trigger] table.at((key, 0, 0), (kind, 0, 0), false) ==
            sum(part_input_seq_view(rows), |row: PartRow| if row.p_partkey == key as int
                && text_matches(scanned_dictionary, 0, kind, row.p_type) { 1 } else { 0 }),
    ensures
        forall|index: int| 0 <= index < db.part.len() ==> text_present(dictionary, 0, (#[trigger] db.part[index]).p_type),
        forall|key: i64, kind: i64| #[trigger] table.at((key, 0, 0), (kind, 0, 0), false) ==
            sum(db.part, |row: PartRow| if row.p_partkey == key as int && text_matches(dictionary, 0, kind, row.p_type) { 1 } else { 0 }),
{
    hide(sum);
    let scanned = part_input_seq_view(rows);
    let represented = |row: PartRow| exists|id: i64| scanned_dictionary.types.known(id)
        && #[trigger] scanned_dictionary.types.text(id) == row.p_type;
    assert forall|index: int| 0 <= index < scanned.len() implies represented(#[trigger] scanned[index]) by {
        assert(scanned[index] == part_input_view(rows[index]));
    }
    verus_prooflib::lemma_weighted_equal_preserves_predicate(scanned, db.part, represented);
    assert forall|index: int| 0 <= index < db.part.len() implies text_present(dictionary, 0, (#[trigger] db.part[index]).p_type) by {
        lemma_known_text_extension(scanned_dictionary, dictionary, 0, db.part[index].p_type);
    }
    assert forall|key: i64, kind: i64| #[trigger] table.at((key, 0, 0), (kind, 0, 0), false) ==
        sum(db.part, |row: PartRow| if row.p_partkey == key as int && text_matches(dictionary, 0, kind, row.p_type) { 1 } else { 0 }) by {
        let before_weight = |row: PartRow| if row.p_partkey == key as int && text_matches(scanned_dictionary, 0, kind, row.p_type) { 1int } else { 0int };
        let weight = |row: PartRow| if row.p_partkey == key as int && text_matches(dictionary, 0, kind, row.p_type) { 1int } else { 0int };
        assert forall|index: int| 0 <= index < scanned.len() implies before_weight(#[trigger] scanned[index]) == weight(scanned[index]) by {
            lemma_known_text_extension(scanned_dictionary, dictionary, 0, rows[index].p_type@);
        }
        lemma_sum_ext(scanned, before_weight, weight);
        assert(sum(scanned, weight) == sum(db.part, weight));
    }
}

pub proof fn lemma_supplier_scan(table: Table, rows: Seq<SupplierInput>, db: Db)
    requires scan_suppliers_ok(db, rows),
        forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
            sum(supplier_input_seq_view(rows), |row: SupplierRow| if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1 } else { 0 }),
    ensures forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
        sum(db.supplier, |row: SupplierRow| if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1 } else { 0 }),
{
    hide(sum);
}

pub proof fn lemma_customer_scan(table: Table, rows: Seq<CustomerInput>, db: Db)
    requires scan_customers_ok(db, rows),
        forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
            sum(customer_input_seq_view(rows), |row: CustomerRow| if row.c_custkey == key as int && row.c_nationkey == nation as int { 1 } else { 0 }),
    ensures forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
        sum(db.customer, |row: CustomerRow| if row.c_custkey == key as int && row.c_nationkey == nation as int { 1 } else { 0 }),
{
    hide(sum);
}

pub proof fn lemma_order_scan(table: Table, reverse: Table, rows: Seq<OrderInput>, db: Db)
    requires scan_window_orders_ok(db, rows),
        forall|key: i64, customer: i64, year: i64| #[trigger] table.at((key, 0, 0), (customer, year, 0), false) ==
            sum(order_input_seq_view(rows), |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
                && year != 0 && year_of(row.o_orderdate) == year as int { 1 } else { 0 }),
        forall|customer: i64, order: i64| #[trigger] reverse.at((customer, 0, 0), (order, 0, 0), false) ==
            sum(order_input_seq_view(rows), |row: OrdersRow| if row.o_orderkey == order as int && row.o_custkey == customer as int
                && year_of(row.o_orderdate) != 0 { 1 } else { 0 }),
    ensures
        order_domain(db),
        forall|key: i64, customer: i64, year: i64| #[trigger] table.at((key, 0, 0), (customer, year, 0), false) ==
            sum(db.orders, |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
                && year != 0 && year_of(row.o_orderdate) == year as int { 1 } else { 0 }),
        forall|customer: i64, order: i64| #[trigger] reverse.at((customer, 0, 0), (order, 0, 0), false) ==
            sum(db.orders, |row: OrdersRow| if row.o_orderkey == order as int && row.o_custkey == customer as int
                && year_of(row.o_orderdate) != 0 { 1 } else { 0 }),
{
    hide(sum);
    let scanned = order_input_seq_view(rows);
    let bad = |row: OrdersRow| if i64::MIN <= row.o_orderkey <= i64::MAX && i64::MIN <= row.o_custkey <= i64::MAX { 0int } else { 1int };
    let filtered_bad = |row: OrdersRow| if (row.o_orderdate >= 9131) && (row.o_orderdate <= 9861) { bad(row) } else { 0int };
    assert forall|index: int| 0 <= index < scanned.len() implies bad(#[trigger] scanned[index]) == 0 by {
        assert(scanned[index] == order_input_view(rows[index]));
    }
    lemma_sum_zero(scanned, bad);
    assert(sum(db.orders, filtered_bad) == 0);
    assert forall|index: int| 0 <= index < db.orders.len() implies order_represented(#[trigger] db.orders[index]) by {
        if 9131 <= db.orders[index].o_orderdate <= 9861 && bad(db.orders[index]) != 0 {
            lemma_sum_pos(db.orders, filtered_bad, index);
        }
    }
    assert forall|key: i64, customer: i64, year: i64| #[trigger] table.at((key, 0, 0), (customer, year, 0), false) ==
        sum(db.orders, |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
            && year != 0 && year_of(row.o_orderdate) == year as int { 1 } else { 0 }) by {
        let weight = |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
            && year != 0 && year_of(row.o_orderdate) == year as int { 1int } else { 0int };
        let filtered = |row: OrdersRow| if (row.o_orderdate >= 9131) && (row.o_orderdate <= 9861) { weight(row) } else { 0int };
        assert(sum(order_input_seq_view(rows), weight) == sum(db.orders, filtered));
        lemma_sum_ext(db.orders, filtered, weight);
    }
    assert forall|customer: i64, order: i64| #[trigger] reverse.at((customer, 0, 0), (order, 0, 0), false) ==
        sum(db.orders, |row: OrdersRow| if row.o_orderkey == order as int && row.o_custkey == customer as int
            && year_of(row.o_orderdate) != 0 { 1 } else { 0 }) by {
        let weight = |row: OrdersRow| if row.o_orderkey == order as int && row.o_custkey == customer as int
            && year_of(row.o_orderdate) != 0 { 1int } else { 0int };
        let filtered = |row: OrdersRow| if (row.o_orderdate >= 9131) && (row.o_orderdate <= 9861) { weight(row) } else { 0int };
        assert(sum(order_input_seq_view(rows), weight) == sum(db.orders, filtered));
        lemma_sum_ext(db.orders, filtered, weight);
    }
}

pub proof fn lemma_nation_scan(table: Table, reverse: Table, dictionary: Dictionary, scanned_dictionary: Dictionary, rows: Seq<NationInput>, db: Db)
    requires scan_nations_ok(db, rows), extension(scanned_dictionary, dictionary),
        forall|index: int| 0 <= index < rows.len() ==> text_present(scanned_dictionary, 1, (#[trigger] rows[index]).n_name@),
        forall|key: i64, name: i64, region: i64| #[trigger] table.at((key, 0, 0), (name, region, 0), false) ==
            sum(nation_input_seq_view(rows), |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
                && text_matches(scanned_dictionary, 1, name, row.n_name) { 1 } else { 0 }),
        forall|region: i64, nation: i64| #[trigger] reverse.at((region, 0, 0), (nation, 0, 0), false) ==
            sum(nation_input_seq_view(rows), |row: NationRow| if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1 } else { 0 }),
    ensures
        nation_domain(db),
        forall|index: int| 0 <= index < db.nation.len() ==> text_present(dictionary, 1, (#[trigger] db.nation[index]).n_name),
        forall|key: i64, name: i64, region: i64| #[trigger] table.at((key, 0, 0), (name, region, 0), false) ==
            sum(db.nation, |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
                && text_matches(dictionary, 1, name, row.n_name) { 1 } else { 0 }),
        forall|region: i64, nation: i64| #[trigger] reverse.at((region, 0, 0), (nation, 0, 0), false) ==
            sum(db.nation, |row: NationRow| if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1 } else { 0 }),
{
    hide(sum);
    let scanned = nation_input_seq_view(rows);
    let represented = |row: NationRow| nation_represented(row) && text_present(scanned_dictionary, 1, row.n_name);
    assert forall|index: int| 0 <= index < scanned.len() implies represented(#[trigger] scanned[index]) by {
        assert(scanned[index] == nation_input_view(rows[index]));
    }
    verus_prooflib::lemma_weighted_equal_preserves_predicate(scanned, db.nation, represented);
    assert forall|index: int| 0 <= index < db.nation.len() implies text_present(dictionary, 1, (#[trigger] db.nation[index]).n_name) by {
        lemma_known_text_extension(scanned_dictionary, dictionary, 1, db.nation[index].n_name);
    }
    assert forall|key: i64, name: i64, region: i64| #[trigger] table.at((key, 0, 0), (name, region, 0), false) ==
        sum(db.nation, |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
            && text_matches(dictionary, 1, name, row.n_name) { 1 } else { 0 }) by {
        let before_weight = |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
            && text_matches(scanned_dictionary, 1, name, row.n_name) { 1int } else { 0int };
        let weight = |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
            && text_matches(dictionary, 1, name, row.n_name) { 1int } else { 0int };
        assert forall|index: int| 0 <= index < scanned.len() implies before_weight(#[trigger] scanned[index]) == weight(scanned[index]) by {
            lemma_known_text_extension(scanned_dictionary, dictionary, 1, rows[index].n_name@);
        }
        lemma_sum_ext(scanned, before_weight, weight);
        assert(sum(scanned, weight) == sum(db.nation, weight));
    }
}

pub proof fn lemma_region_scan(table: Table, dictionary: Dictionary, rows: Seq<RegionInput>, db: Db)
    requires scan_regions_ok(db, rows), dictionary.valid(),
        forall|index: int| 0 <= index < rows.len() ==> text_present(dictionary, 2, (#[trigger] rows[index]).r_name@),
        forall|key: i64, name: i64| #[trigger] table.at((key, 0, 0), (name, 0, 0), false) ==
            sum(region_input_seq_view(rows), |row: RegionRow| if row.r_regionkey == key as int && text_matches(dictionary, 2, name, row.r_name) { 1 } else { 0 }),
    ensures
        forall|index: int| 0 <= index < db.region.len() ==> text_present(dictionary, 2, (#[trigger] db.region[index]).r_name),
        forall|key: i64, name: i64| #[trigger] table.at((key, 0, 0), (name, 0, 0), false) ==
            sum(db.region, |row: RegionRow| if row.r_regionkey == key as int && text_matches(dictionary, 2, name, row.r_name) { 1 } else { 0 }),
{
    hide(sum);
    let scanned = region_input_seq_view(rows);
    let represented = |row: RegionRow| exists|id: i64| dictionary.regions.known(id)
        && #[trigger] dictionary.regions.text(id) == row.r_name;
    assert forall|index: int| 0 <= index < scanned.len() implies represented(#[trigger] scanned[index]) by {
        assert(scanned[index] == region_input_view(rows[index]));
    }
    verus_prooflib::lemma_weighted_equal_preserves_predicate(scanned, db.region, represented);
    assert forall|index: int| 0 <= index < db.region.len() implies text_present(dictionary, 2, (#[trigger] db.region[index]).r_name) by {
        assert(represented(db.region[index]));
    }
}

pub proof fn lemma_part_capture(table: Table, before: Data, dictionary: Dictionary, scanned_dictionary: Dictionary, rows: Seq<PartDelta>, db: Db, db2: Db)
    requires dimension_meaning(before, db), extension(before.dictionary, dictionary), extension(scanned_dictionary, dictionary),
        bag_step(db.part, db2.part, part_delta_seq_view(rows)),
        forall|index: int| 0 <= index < rows.len() ==> text_present(scanned_dictionary, 0, (#[trigger] rows[index]).row.p_type@),
        forall|key: i64, kind: i64| #[trigger] table.at((key, 0, 0), (kind, 0, 0), false) ==
            before.parts.at((key, 0, 0), (kind, 0, 0), false)
                + sum(part_delta_seq_view(rows), |change: (PartRow, int)|
                    if change.0.p_partkey == key as int && text_matches(scanned_dictionary, 0, kind, change.0.p_type) { change.1 } else { 0 }),
    ensures
        forall|index: int| 0 <= index < db2.part.len() ==> text_present(dictionary, 0, (#[trigger] db2.part[index]).p_type),
        forall|key: i64, kind: i64| #[trigger] table.at((key, 0, 0), (kind, 0, 0), false) ==
            sum(db2.part, |row: PartRow| if row.p_partkey == key as int && text_matches(dictionary, 0, kind, row.p_type) { 1 } else { 0 }),
{
    hide(sum);
    reveal(dimension_meaning);
    let deltas = part_delta_seq_view(rows);
    let represented = |row: PartRow| text_present(dictionary, 0, row.p_type);
    assert forall|index: int| 0 <= index < db.part.len() implies represented(#[trigger] db.part[index]) by {
        lemma_known_text_extension(before.dictionary, dictionary, 0, db.part[index].p_type);
    }
    assert forall|index: int| 0 <= index < deltas.len() implies represented((#[trigger] deltas[index]).0) by {
        assert(deltas[index].0 == part_input_view(rows[index].row));
        lemma_known_text_extension(scanned_dictionary, dictionary, 0, rows[index].row.p_type@);
    }
    verus_prooflib::lemma_bag_step_preserves_predicate(db.part, db2.part, deltas, represented);
    assert forall|key: i64, kind: i64| #[trigger] table.at((key, 0, 0), (kind, 0, 0), false) ==
        sum(db2.part, |row: PartRow| if row.p_partkey == key as int && text_matches(dictionary, 0, kind, row.p_type) { 1 } else { 0 }) by {
        let old_weight = |row: PartRow| if row.p_partkey == key as int && text_matches(before.dictionary, 0, kind, row.p_type) { 1int } else { 0int };
        let weight = |row: PartRow| if row.p_partkey == key as int && text_matches(dictionary, 0, kind, row.p_type) { 1int } else { 0int };
        assert forall|index: int| 0 <= index < db.part.len() implies old_weight(#[trigger] db.part[index]) == weight(db.part[index]) by {
            lemma_known_text_extension(before.dictionary, dictionary, 0, db.part[index].p_type);
        }
        lemma_sum_ext(db.part, old_weight, weight);
        let scanned = |change: (PartRow, int)| if change.0.p_partkey == key as int
            && text_matches(scanned_dictionary, 0, kind, change.0.p_type) { change.1 } else { 0int };
        let signed = |change: (PartRow, int)| change.1 * weight(change.0);
        assert forall|index: int| 0 <= index < deltas.len() implies scanned(#[trigger] deltas[index]) == signed(deltas[index]) by {
            lemma_known_text_extension(scanned_dictionary, dictionary, 0, rows[index].row.p_type@);
        }
        lemma_sum_ext(deltas, scanned, signed);
        assert(sum(db2.part, weight) == sum(db.part, weight) + sum(deltas, signed));
    }
}

pub proof fn lemma_nation_capture(table: Table, reverse: Table, before: Data, dictionary: Dictionary, scanned_dictionary: Dictionary, rows: Seq<NationDelta>, db: Db, db2: Db)
    requires dimension_meaning(before, db), extension(before.dictionary, dictionary), extension(scanned_dictionary, dictionary),
        bag_step(db.nation, db2.nation, nation_delta_seq_view(rows)),
        forall|index: int| 0 <= index < rows.len() ==> text_present(scanned_dictionary, 1, (#[trigger] rows[index]).row.n_name@),
        forall|key: i64, name: i64, region: i64| #[trigger] table.at((key, 0, 0), (name, region, 0), false) ==
            before.nations.at((key, 0, 0), (name, region, 0), false)
                + sum(nation_delta_seq_view(rows), |change: (NationRow, int)| if change.0.n_nationkey == key as int
                    && change.0.n_regionkey == region as int && text_matches(scanned_dictionary, 1, name, change.0.n_name) { change.1 } else { 0 }),
        forall|region: i64, nation: i64| #[trigger] reverse.at((region, 0, 0), (nation, 0, 0), false) ==
            before.nations_by_region.at((region, 0, 0), (nation, 0, 0), false)
                + sum(nation_delta_seq_view(rows), |change: (NationRow, int)|
                    if change.0.n_regionkey == region as int && change.0.n_nationkey == nation as int { change.1 } else { 0 }),
    ensures
        nation_domain(db2),
        forall|index: int| 0 <= index < db2.nation.len() ==> text_present(dictionary, 1, (#[trigger] db2.nation[index]).n_name),
        forall|key: i64, name: i64, region: i64| #[trigger] table.at((key, 0, 0), (name, region, 0), false) ==
            sum(db2.nation, |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
                && text_matches(dictionary, 1, name, row.n_name) { 1 } else { 0 }),
        forall|region: i64, nation: i64| #[trigger] reverse.at((region, 0, 0), (nation, 0, 0), false) ==
            sum(db2.nation, |row: NationRow| if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1 } else { 0 }),
{
    hide(sum);
    reveal(dimension_meaning);
    let deltas = nation_delta_seq_view(rows);
    let represented = |row: NationRow| nation_represented(row) && text_present(dictionary, 1, row.n_name);
    assert forall|index: int| 0 <= index < db.nation.len() implies represented(#[trigger] db.nation[index]) by {
        lemma_known_text_extension(before.dictionary, dictionary, 1, db.nation[index].n_name);
    }
    assert forall|index: int| 0 <= index < deltas.len() implies represented((#[trigger] deltas[index]).0) by {
        assert(deltas[index].0 == nation_input_view(rows[index].row));
        lemma_known_text_extension(scanned_dictionary, dictionary, 1, rows[index].row.n_name@);
    }
    verus_prooflib::lemma_bag_step_preserves_predicate(db.nation, db2.nation, deltas, represented);
    assert forall|key: i64, name: i64, region: i64| #[trigger] table.at((key, 0, 0), (name, region, 0), false) ==
        sum(db2.nation, |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
            && text_matches(dictionary, 1, name, row.n_name) { 1 } else { 0 }) by {
        let old_weight = |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
            && text_matches(before.dictionary, 1, name, row.n_name) { 1int } else { 0int };
        let weight = |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
            && text_matches(dictionary, 1, name, row.n_name) { 1int } else { 0int };
        assert forall|index: int| 0 <= index < db.nation.len() implies old_weight(#[trigger] db.nation[index]) == weight(db.nation[index]) by {
            lemma_known_text_extension(before.dictionary, dictionary, 1, db.nation[index].n_name);
        }
        lemma_sum_ext(db.nation, old_weight, weight);
        let scanned = |change: (NationRow, int)| if change.0.n_nationkey == key as int && change.0.n_regionkey == region as int
            && text_matches(scanned_dictionary, 1, name, change.0.n_name) { change.1 } else { 0int };
        let signed = |change: (NationRow, int)| change.1 * weight(change.0);
        assert forall|index: int| 0 <= index < deltas.len() implies scanned(#[trigger] deltas[index]) == signed(deltas[index]) by {
            lemma_known_text_extension(scanned_dictionary, dictionary, 1, rows[index].row.n_name@);
        }
        lemma_sum_ext(deltas, scanned, signed);
        assert(sum(db2.nation, weight) == sum(db.nation, weight) + sum(deltas, signed));
    }
    assert forall|region: i64, nation: i64| #[trigger] reverse.at((region, 0, 0), (nation, 0, 0), false) ==
        sum(db2.nation, |row: NationRow| if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1 } else { 0 }) by {
        let weight = |row: NationRow| if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1int } else { 0int };
        lemma_sum_ext_all(deltas, |change: (NationRow, int)| change.1 * weight(change.0),
            |change: (NationRow, int)| if change.0.n_regionkey == region as int && change.0.n_nationkey == nation as int { change.1 } else { 0int });
        assert(sum(db2.nation, weight) == sum(db.nation, weight) + sum(deltas, |change: (NationRow, int)| change.1 * weight(change.0)));
    }
}

pub proof fn lemma_region_capture(table: Table, before: Data, dictionary: Dictionary, rows: Seq<RegionDelta>, db: Db, db2: Db)
    requires dimension_meaning(before, db), extension(before.dictionary, dictionary), bag_step(db.region, db2.region, region_delta_seq_view(rows)),
        forall|index: int| 0 <= index < rows.len() ==> text_present(dictionary, 2, (#[trigger] rows[index]).row.r_name@),
        forall|key: i64, name: i64| #[trigger] table.at((key, 0, 0), (name, 0, 0), false) ==
            before.regions.at((key, 0, 0), (name, 0, 0), false)
                + sum(region_delta_seq_view(rows), |change: (RegionRow, int)|
                    if change.0.r_regionkey == key as int && text_matches(dictionary, 2, name, change.0.r_name) { change.1 } else { 0 }),
    ensures
        forall|index: int| 0 <= index < db2.region.len() ==> text_present(dictionary, 2, (#[trigger] db2.region[index]).r_name),
        forall|key: i64, name: i64| #[trigger] table.at((key, 0, 0), (name, 0, 0), false) ==
            sum(db2.region, |row: RegionRow| if row.r_regionkey == key as int && text_matches(dictionary, 2, name, row.r_name) { 1 } else { 0 }),
{
    hide(sum);
    reveal(dimension_meaning);
    let deltas = region_delta_seq_view(rows);
    let represented = |row: RegionRow| text_present(dictionary, 2, row.r_name);
    assert forall|index: int| 0 <= index < db.region.len() implies represented(#[trigger] db.region[index]) by {
        lemma_known_text_extension(before.dictionary, dictionary, 2, db.region[index].r_name);
    }
    assert forall|index: int| 0 <= index < deltas.len() implies represented((#[trigger] deltas[index]).0) by {
        assert(deltas[index].0 == region_input_view(rows[index].row));
    }
    verus_prooflib::lemma_bag_step_preserves_predicate(db.region, db2.region, deltas, represented);
    assert forall|key: i64, name: i64| #[trigger] table.at((key, 0, 0), (name, 0, 0), false) ==
        sum(db2.region, |row: RegionRow| if row.r_regionkey == key as int && text_matches(dictionary, 2, name, row.r_name) { 1 } else { 0 }) by {
        let old_weight = |row: RegionRow| if row.r_regionkey == key as int && text_matches(before.dictionary, 2, name, row.r_name) { 1int } else { 0int };
        let weight = |row: RegionRow| if row.r_regionkey == key as int && text_matches(dictionary, 2, name, row.r_name) { 1int } else { 0int };
        assert forall|index: int| 0 <= index < db.region.len() implies old_weight(#[trigger] db.region[index]) == weight(db.region[index]) by {
            lemma_known_text_extension(before.dictionary, dictionary, 2, db.region[index].r_name);
        }
        lemma_sum_ext(db.region, old_weight, weight);
        lemma_sum_ext_all(deltas, |change: (RegionRow, int)| change.1 * weight(change.0),
            |change: (RegionRow, int)| if change.0.r_regionkey == key as int && text_matches(dictionary, 2, name, change.0.r_name) { change.1 } else { 0int });
        assert(sum(db2.region, weight) == sum(db.region, weight) + sum(deltas, |change: (RegionRow, int)| change.1 * weight(change.0)));
    }
}

pub proof fn lemma_supplier_capture(table: Table, before: Data, rows: Seq<SupplierDelta>, db: Db, db2: Db)
    requires dimension_meaning(before, db), bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(rows)),
        forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
            before.suppliers.at((key, 0, 0), (nation, 0, 0), false)
                + sum(supplier_delta_seq_view(rows), |change: (SupplierRow, int)|
                    if change.0.s_suppkey == key as int && change.0.s_nationkey == nation as int { change.1 } else { 0 }),
    ensures forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
        sum(db2.supplier, |row: SupplierRow| if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1 } else { 0 }),
{
    hide(sum);
    reveal(dimension_meaning);
    assert forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
        sum(db2.supplier, |row: SupplierRow| if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1 } else { 0 }) by {
        let weight = |row: SupplierRow| if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1int } else { 0int };
        lemma_sum_ext_all(supplier_delta_seq_view(rows), |change: (SupplierRow, int)| change.1 * weight(change.0),
            |change: (SupplierRow, int)| if change.0.s_suppkey == key as int && change.0.s_nationkey == nation as int { change.1 } else { 0int });
        assert(sum(db2.supplier, weight) == sum(db.supplier, weight)
            + sum(supplier_delta_seq_view(rows), |change: (SupplierRow, int)| change.1 * weight(change.0)));
    }
}

pub proof fn lemma_customer_capture(table: Table, before: Data, rows: Seq<CustomerDelta>, db: Db, db2: Db)
    requires dimension_meaning(before, db), bag_step(db.customer, db2.customer, customer_delta_seq_view(rows)),
        forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
            before.customers.at((key, 0, 0), (nation, 0, 0), false)
                + sum(customer_delta_seq_view(rows), |change: (CustomerRow, int)|
                    if change.0.c_custkey == key as int && change.0.c_nationkey == nation as int { change.1 } else { 0 }),
    ensures forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
        sum(db2.customer, |row: CustomerRow| if row.c_custkey == key as int && row.c_nationkey == nation as int { 1 } else { 0 }),
{
    hide(sum);
    reveal(dimension_meaning);
    assert forall|key: i64, nation: i64| #[trigger] table.at((key, 0, 0), (nation, 0, 0), false) ==
        sum(db2.customer, |row: CustomerRow| if row.c_custkey == key as int && row.c_nationkey == nation as int { 1 } else { 0 }) by {
        let weight = |row: CustomerRow| if row.c_custkey == key as int && row.c_nationkey == nation as int { 1int } else { 0int };
        lemma_sum_ext_all(customer_delta_seq_view(rows), |change: (CustomerRow, int)| change.1 * weight(change.0),
            |change: (CustomerRow, int)| if change.0.c_custkey == key as int && change.0.c_nationkey == nation as int { change.1 } else { 0int });
        assert(sum(db2.customer, weight) == sum(db.customer, weight)
            + sum(customer_delta_seq_view(rows), |change: (CustomerRow, int)| change.1 * weight(change.0)));
    }
}

pub proof fn lemma_order_capture(table: Table, reverse: Table, before: Data, rows: Seq<OrderDelta>, full: Seq<(OrdersRow, int)>, db: Db, db2: Db)
    requires dimension_meaning(before, db), orders_delta_ok(rows, full), bag_step(db.orders, db2.orders, full),
        forall|key: i64, customer: i64, year: i64| #[trigger] table.at((key, 0, 0), (customer, year, 0), false) ==
            before.orders.at((key, 0, 0), (customer, year, 0), false)
                + sum(rows, |change: OrderDelta| if change.o_orderkey == key && change.o_custkey == customer
                    && ((year == 1995 && change.in_1995 != 0) || (year == 1996 && change.in_1996 != 0)) { change.sign as int } else { 0 }),
        forall|customer: i64, order: i64| #[trigger] reverse.at((customer, 0, 0), (order, 0, 0), false) ==
            before.orders_by_customer.at((customer, 0, 0), (order, 0, 0), false)
                + sum(rows, |change: OrderDelta| if change.o_orderkey == order && change.o_custkey == customer
                    && (change.in_1995 != 0 || change.in_1996 != 0) { change.sign as int } else { 0 }),
    ensures
        order_domain(db2),
        forall|key: i64, customer: i64, year: i64| #[trigger] table.at((key, 0, 0), (customer, year, 0), false) ==
            sum(db2.orders, |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
                && year != 0 && year_of(row.o_orderdate) == year as int { 1 } else { 0 }),
        forall|customer: i64, order: i64| #[trigger] reverse.at((customer, 0, 0), (order, 0, 0), false) ==
            sum(db2.orders, |row: OrdersRow| if row.o_orderkey == order as int && row.o_custkey == customer as int
                && year_of(row.o_orderdate) != 0 { 1 } else { 0 }),
{
    hide(sum);
    reveal(dimension_meaning);
    let represented = |row: OrdersRow| order_represented(row);
    assert forall|index: int| 0 <= index < full.len() implies represented((#[trigger] full[index]).0) by {
        assert(rows[index].o_orderkey as int == full[index].0.o_orderkey);
        assert(rows[index].o_custkey as int == full[index].0.o_custkey);
    }
    verus_prooflib::lemma_bag_step_preserves_predicate(db.orders, db2.orders, full, represented);
    assert forall|key: i64, customer: i64, year: i64| #[trigger] table.at((key, 0, 0), (customer, year, 0), false) ==
        sum(db2.orders, |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
            && year != 0 && year_of(row.o_orderdate) == year as int { 1 } else { 0 }) by {
        let weight = |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int
            && year != 0 && year_of(row.o_orderdate) == year as int { 1int } else { 0int };
        let projected = |change: OrderDelta| if change.o_orderkey == key && change.o_custkey == customer
            && ((year == 1995 && change.in_1995 != 0) || (year == 1996 && change.in_1996 != 0)) { change.sign as int } else { 0int };
        let signed = |change: (OrdersRow, int)| change.1 * weight(change.0);
        let by_index = |index: int| projected(rows[index]);
        lemma_sum_index(rows, projected, by_index);
        lemma_sum_index(full, signed, by_index);
        assert(sum(db2.orders, weight) == sum(db.orders, weight) + sum(full, signed));
    }
    assert forall|customer: i64, order: i64| #[trigger] reverse.at((customer, 0, 0), (order, 0, 0), false) ==
        sum(db2.orders, |row: OrdersRow| if row.o_orderkey == order as int && row.o_custkey == customer as int
            && year_of(row.o_orderdate) != 0 { 1 } else { 0 }) by {
        let weight = |row: OrdersRow| if row.o_orderkey == order as int && row.o_custkey == customer as int
            && year_of(row.o_orderdate) != 0 { 1int } else { 0int };
        let projected = |change: OrderDelta| if change.o_orderkey == order && change.o_custkey == customer
            && (change.in_1995 != 0 || change.in_1996 != 0) { change.sign as int } else { 0int };
        let signed = |change: (OrdersRow, int)| change.1 * weight(change.0);
        let by_index = |index: int| projected(rows[index]);
        lemma_sum_index(rows, projected, by_index);
        lemma_sum_index(full, signed, by_index);
        assert(sum(db2.orders, weight) == sum(db.orders, weight) + sum(full, signed));
    }
}

}
