// Build: the dimension tables from the host's scans, one function per table.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::table::*;
use crate::dictionary::*;
use crate::dimensions::*;
use crate::model;
use crate::num::Num;
use crate::counting::*;
use crate::maintenance::*;

verus! {

/// The scanned parts as a table of counts: per partkey and type id.
pub fn scan_parts(parts: &[PartInput], initial: Dictionary) -> (result: (Table, Dictionary))
    requires initial.valid(), initial.types.length as int + parts@.len() < u64::MAX as int,
    ensures result.1.valid(), result.1.types.length as int <= initial.types.length as int + parts@.len(),
        extension(initial, result.1), result.1.names == initial.names, result.1.regions == initial.regions,
        dimension_table(result.0, result.1, 0),
        forall|key: i64, kind: i64| #[trigger] result.0.at((key, 0, 0), (kind, 0, 0), false) ==
            sum(part_input_seq_view(parts@), |row: PartRow|
                if row.p_partkey == key as int && model::text_matches(result.1, 0, kind, row.p_type) { 1 } else { 0 }),
        forall|offset: int| 0 <= offset < parts@.len() ==> model::text_present(result.1, 0, (#[trigger] parts@[offset]).p_type@),
{
    let mut dictionary = initial;
    let mut part_work = Work::new(&Table::empty(parts.len()));
    let mut index = 0;
    let ghost part_rows = parts@;
    while index < parts.len()
        invariant index <= parts.len(), part_rows == parts@, part_work.valid(), dictionary.valid(), initial.types.length as int + parts@.len() < u64::MAX as int,
            dictionary.types.length as int <= initial.types.length as int + index,
            words_extension(initial.types, dictionary.types), dictionary.names == initial.names, dictionary.regions == initial.regions,
            crate::dimensions::work_support(part_work, dictionary, 0),
            forall|key: Major, minor: Major| #[trigger] part_work.at(key, minor, false) >= 0,
            forall|offset: int| 0 <= offset < index ==> exists|id: i64| dictionary.types.known(id)
                && #[trigger] dictionary.types.text(id) == (#[trigger] part_rows[offset]).p_type@,
            forall|key: i64, kind: i64| #[trigger] part_work.at((key, 0, 0), (kind, 0, 0), false) ==
                sum(part_input_seq_view(part_rows.take(index as int)), |row: PartRow|
                    if row.p_partkey == key as int && model::text_matches(dictionary, 0, kind, row.p_type) { 1 } else { 0 }),
        decreases parts.len() - index,
    {
        let ghost processed = index as int;
        let row = &parts[index];
        let ghost before_dictionary = dictionary;
        let kind = dictionary.types.intern(row.p_type.as_slice());
        proof { lemma_words_extension_transitive(initial.types, before_dictionary.types, dictionary.types); }
        part_work.add((row.p_partkey, 0, 0), (kind, 0, 0), Num::Small(1), Num::zero());
        proof {
            lemma_text_identifier(dictionary, 0, kind, row.p_type@);
            assert forall|offset: int| 0 <= offset < index implies exists|id: i64| dictionary.types.known(id)
                && #[trigger] dictionary.types.text(id) == (#[trigger] part_rows[offset]).p_type@ by {
                lemma_known_text_extension(before_dictionary, dictionary, 0, part_rows[offset].p_type@);
            }
            assert(dictionary.types.text(kind) == part_rows[index as int].p_type@);
            assert forall|key: i64, selected: i64| #[trigger] part_work.at((key, 0, 0), (selected, 0, 0), false) ==
                sum(part_input_seq_view(part_rows.take(index as int + 1)), |row: PartRow|
                    if row.p_partkey == key as int && model::text_matches(dictionary, 0, selected, row.p_type) { 1 } else { 0 }) by {
                let prefix = part_input_seq_view(part_rows.take(index as int));
                let before_weight = |row: PartRow| if row.p_partkey == key as int && model::text_matches(before_dictionary, 0, selected, row.p_type) { 1int } else { 0int };
                let weight = |row: PartRow| if row.p_partkey == key as int && model::text_matches(dictionary, 0, selected, row.p_type) { 1int } else { 0int };
                assert forall|offset: int| 0 <= offset < prefix.len() implies before_weight(#[trigger] prefix[offset]) == weight(prefix[offset]) by {
                    lemma_known_text_extension(before_dictionary, dictionary, 0, part_rows[offset].p_type@);
                }
                assert(part_input_seq_view(part_rows.take(index as int + 1)) =~= prefix.push(part_input_view(part_rows[index as int])));
                lemma_grown_prefix_step(prefix, part_input_view(part_rows[index as int]), before_weight, weight);
            }
        }
        index += 1;
    }
    let table = part_work.finish();
    proof {
        assert(part_rows.take(part_rows.len() as int) =~= part_rows);
        assert(extension(initial, dictionary));
        assert forall|offset: int| 0 <= offset < part_rows.len() implies
            model::text_present(dictionary, 0, (#[trigger] part_rows[offset]).p_type@) by {}
    }
    (table, dictionary)
}

/// The scanned suppliers as a table of counts: per suppkey and nationkey.
pub fn scan_suppliers(suppliers: &[SupplierInput], dictionary: &Dictionary) -> (result: Table)
    ensures dimension_table(result, *dictionary, 1),
        forall|key: i64, nation: i64| #[trigger] result.at((key, 0, 0), (nation, 0, 0), false) ==
            sum(supplier_input_seq_view(suppliers@), |row: SupplierRow|
                if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1 } else { 0 }),
{
    let mut supplier_work = Work::new(&Table::empty(suppliers.len()));
    let mut index = 0;
    let ghost supplier_rows = suppliers@;
    while index < suppliers.len()
        invariant index <= suppliers.len(), supplier_rows == suppliers@, supplier_work.valid(),
            crate::dimensions::work_support(supplier_work, *dictionary, 1),
            forall|key: Major, minor: Major| #[trigger] supplier_work.at(key, minor, false) >= 0,
            forall|key: i64, nation: i64| #[trigger] supplier_work.at((key, 0, 0), (nation, 0, 0), false) ==
                sum(supplier_input_seq_view(supplier_rows.take(index as int)), |row: SupplierRow|
                    if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1 } else { 0 }),
        decreases suppliers.len() - index,
    {
        let ghost processed = index as int;
        let row = suppliers[index];
        supplier_work.add((row.s_suppkey, 0, 0), (row.s_nationkey, 0, 0), Num::Small(1), Num::zero());
        proof {
            assert forall|key: i64, nation: i64| #[trigger] supplier_work.at((key, 0, 0), (nation, 0, 0), false) ==
                sum(supplier_input_seq_view(supplier_rows.take(index as int + 1)), |row: SupplierRow|
                    if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1 } else { 0 }) by {
                lemma_mapped_prefix_step(supplier_rows, |row: SupplierInput| supplier_input_view(row), index as int,
                    |row: SupplierRow| if row.s_suppkey == key as int && row.s_nationkey == nation as int { 1 } else { 0 });
            }
        }
        index += 1;
    }
    let table = supplier_work.finish();
    proof { assert(supplier_rows.take(supplier_rows.len() as int) =~= supplier_rows); }
    table
}

/// The scanned customers as a table of counts: per custkey and nationkey.
pub fn scan_customers(customers: &[CustomerInput], dictionary: &Dictionary) -> (result: Table)
    ensures dimension_table(result, *dictionary, 2),
        forall|key: i64, nation: i64| #[trigger] result.at((key, 0, 0), (nation, 0, 0), false) ==
            sum(customer_input_seq_view(customers@), |row: CustomerRow|
                if row.c_custkey == key as int && row.c_nationkey == nation as int { 1 } else { 0 }),
{
    let mut customer_work = Work::new(&Table::empty(customers.len()));
    let mut index = 0;
    let ghost customer_rows = customers@;
    while index < customers.len()
        invariant index <= customers.len(), customer_rows == customers@, customer_work.valid(),
            crate::dimensions::work_support(customer_work, *dictionary, 2),
            forall|key: Major, minor: Major| #[trigger] customer_work.at(key, minor, false) >= 0,
            forall|key: i64, nation: i64| #[trigger] customer_work.at((key, 0, 0), (nation, 0, 0), false) ==
                sum(customer_input_seq_view(customer_rows.take(index as int)), |row: CustomerRow|
                    if row.c_custkey == key as int && row.c_nationkey == nation as int { 1 } else { 0 }),
        decreases customers.len() - index,
    {
        let ghost processed = index as int;
        let row = customers[index];
        customer_work.add((row.c_custkey, 0, 0), (row.c_nationkey, 0, 0), Num::Small(1), Num::zero());
        proof {
            assert forall|key: i64, nation: i64| #[trigger] customer_work.at((key, 0, 0), (nation, 0, 0), false) ==
                sum(customer_input_seq_view(customer_rows.take(index as int + 1)), |row: CustomerRow|
                    if row.c_custkey == key as int && row.c_nationkey == nation as int { 1 } else { 0 }) by {
                lemma_mapped_prefix_step(customer_rows, |row: CustomerInput| customer_input_view(row), index as int,
                    |row: CustomerRow| if row.c_custkey == key as int && row.c_nationkey == nation as int { 1 } else { 0 });
            }
        }
        index += 1;
    }
    let table = customer_work.finish();
    proof { assert(customer_rows.take(customer_rows.len() as int) =~= customer_rows); }
    table
}

/// The scanned window orders: counts per orderkey, customer and year, and per customer and orderkey.
pub fn scan_orders(window_orders: &[OrderInput], dictionary: &Dictionary, Ghost(db): Ghost<Db>) -> (result: (Table, Table))
    requires scan_window_orders_ok(db, window_orders@),
    ensures dimension_table(result.0, *dictionary, 3), supported_table(result.1, *dictionary, 1),
        forall|customer: i64, order: i64| #[trigger] result.1.at((customer, 0, 0), (order, 0, 0), false) ==
            sum(order_input_seq_view(window_orders@), |row: OrdersRow|
                if row.o_orderkey == order as int && row.o_custkey == customer as int
                    && model::year_of(row.o_orderdate) != 0 { 1 } else { 0 }),
        forall|key: i64, customer: i64, year: i64| #[trigger] result.0.at((key, 0, 0), (customer, year, 0), false) ==
            sum(order_input_seq_view(window_orders@), |row: OrdersRow|
                if row.o_orderkey == key as int && row.o_custkey == customer as int
                    && year != 0 && model::year_of(row.o_orderdate) == year as int { 1 } else { 0 }),
{
    let mut order_work = Work::new(&Table::empty(window_orders.len()));
    let mut reverse_orders = Work::new(&Table::empty(window_orders.len()));
    let mut index = 0;
    let ghost order_rows = window_orders@;
    proof { crate::bounds::lemma_window_rows(db, order_rows); }
    while index < window_orders.len()
        invariant index <= window_orders.len(), order_rows == window_orders@, order_work.valid(), reverse_orders.valid(),
            forall|i: int| 0 <= i < order_rows.len() ==> 9131 <= (#[trigger] order_rows[i]).o_orderdate <= 9861,
            crate::dimensions::work_support(reverse_orders, *dictionary, 1),
            crate::dimensions::work_support(order_work, *dictionary, 3),
            forall|key: Major, minor: Major| #[trigger] order_work.at(key, minor, false) >= 0,
            forall|customer: i64, order: i64| #[trigger] reverse_orders.at((customer, 0, 0), (order, 0, 0), false) ==
                sum(order_input_seq_view(order_rows.take(index as int)), |row: OrdersRow|
                    if row.o_orderkey == order as int && row.o_custkey == customer as int
                        && model::year_of(row.o_orderdate) != 0 { 1 } else { 0 }),
            forall|key: i64, customer: i64, year: i64| #[trigger] order_work.at((key, 0, 0), (customer, year, 0), false) ==
                sum(order_input_seq_view(order_rows.take(index as int)), |row: OrdersRow|
                    if row.o_orderkey == key as int && row.o_custkey == customer as int
                        && year != 0 && model::year_of(row.o_orderdate) == year as int { 1 } else { 0 }),
        decreases window_orders.len() - index,
    {
        let ghost processed = index as int;
        let row = window_orders[index];
        let year: i64 = if row.o_orderdate <= 9495 { 1995 } else { 1996 };
        order_work.add((row.o_orderkey, 0, 0), (row.o_custkey, year, 0), Num::Small(1), Num::zero());
        reverse_orders.add((row.o_custkey, 0, 0), (row.o_orderkey, 0, 0), Num::Small(1), Num::zero());
        proof {
            assert forall|customer: i64, order: i64| #[trigger] reverse_orders.at((customer, 0, 0), (order, 0, 0), false) ==
                sum(order_input_seq_view(order_rows.take(index as int + 1)), |row: OrdersRow|
                    if row.o_orderkey == order as int && row.o_custkey == customer as int
                        && model::year_of(row.o_orderdate) != 0 { 1 } else { 0 }) by {
                lemma_mapped_prefix_step(order_rows, |row: OrderInput| order_input_view(row), index as int,
                    |row: OrdersRow| if row.o_orderkey == order as int && row.o_custkey == customer as int
                        && model::year_of(row.o_orderdate) != 0 { 1 } else { 0 });
            }
            assert forall|key: i64, customer: i64, selected: i64| #[trigger] order_work.at((key, 0, 0), (customer, selected, 0), false) ==
                sum(order_input_seq_view(order_rows.take(index as int + 1)), |row: OrdersRow|
                    if row.o_orderkey == key as int && row.o_custkey == customer as int && selected != 0
                        && model::year_of(row.o_orderdate) == selected as int { 1 } else { 0 }) by {
                lemma_mapped_prefix_step(order_rows, |row: OrderInput| order_input_view(row), index as int,
                    |row: OrdersRow| if row.o_orderkey == key as int && row.o_custkey == customer as int && selected != 0
                        && model::year_of(row.o_orderdate) == selected as int { 1 } else { 0 });
            }
        }
        index += 1;
    }
    let orders = order_work.finish();
    let orders_by_customer = reverse_orders.finish();
    proof { assert(order_rows.take(order_rows.len() as int) =~= order_rows); }
    (orders, orders_by_customer)
}

/// The scanned nations: counts per nationkey, name id and regionkey, and per regionkey and nationkey.
pub fn scan_nations(nations: &[NationInput], initial: Dictionary) -> (result: (Table, Table, Dictionary))
    requires initial.valid(), initial.names.length as int + nations@.len() < u64::MAX as int,
    ensures result.2.valid(), result.2.names.length as int <= initial.names.length as int + nations@.len(),
        extension(initial, result.2), result.2.types == initial.types, result.2.regions == initial.regions,
        dimension_table(result.0, result.2, 4), supported_table(result.1, result.2, 1),
        forall|region: i64, nation: i64| #[trigger] result.1.at((region, 0, 0), (nation, 0, 0), false) ==
            sum(nation_input_seq_view(nations@), |row: NationRow|
                if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1 } else { 0 }),
        forall|key: i64, name: i64, region: i64| #[trigger] result.0.at((key, 0, 0), (name, region, 0), false) ==
            sum(nation_input_seq_view(nations@), |row: NationRow|
                if row.n_nationkey == key as int && row.n_regionkey == region as int
                    && model::text_matches(result.2, 1, name, row.n_name) { 1 } else { 0 }),
        forall|offset: int| 0 <= offset < nations@.len() ==> model::text_present(result.2, 1, (#[trigger] nations@[offset]).n_name@),
{
    let mut dictionary = initial;
    let mut nation_work = Work::new(&Table::empty(nations.len()));
    let mut reverse_nations = Work::new(&Table::empty(0));
    let mut index = 0;
    let ghost nation_rows = nations@;
    while index < nations.len()
        invariant index <= nations.len(), nation_rows == nations@, nation_work.valid(), reverse_nations.valid(), dictionary.valid(),
            initial.names.length as int + nations@.len() < u64::MAX as int,
            dictionary.names.length as int <= initial.names.length as int + index,
            words_extension(initial.names, dictionary.names), dictionary.types == initial.types, dictionary.regions == initial.regions,
            crate::dimensions::work_support(reverse_nations, dictionary, 1),
            crate::dimensions::work_support(nation_work, dictionary, 4),
            forall|key: Major, minor: Major| #[trigger] nation_work.at(key, minor, false) >= 0,
            forall|region: i64, nation: i64| #[trigger] reverse_nations.at((region, 0, 0), (nation, 0, 0), false) ==
                sum(nation_input_seq_view(nation_rows.take(index as int)), |row: NationRow|
                    if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1 } else { 0 }),
            forall|offset: int| 0 <= offset < index ==> exists|id: i64| dictionary.names.known(id)
                && #[trigger] dictionary.names.text(id) == (#[trigger] nation_rows[offset]).n_name@,
            forall|key: i64, name: i64, region: i64| #[trigger] nation_work.at((key, 0, 0), (name, region, 0), false) ==
                sum(nation_input_seq_view(nation_rows.take(index as int)), |row: NationRow|
                    if row.n_nationkey == key as int && row.n_regionkey == region as int
                        && model::text_matches(dictionary, 1, name, row.n_name) { 1 } else { 0 }),
        decreases nations.len() - index,
    {
        let ghost processed = index as int;
        let row = &nations[index];
        let ghost before_dictionary = dictionary;
        let name = dictionary.names.intern(row.n_name.as_slice());
        nation_work.add((row.n_nationkey, 0, 0), (name, row.n_regionkey, 0), Num::Small(1), Num::zero());
        reverse_nations.add((row.n_regionkey, 0, 0), (row.n_nationkey, 0, 0), Num::Small(1), Num::zero());
        proof {
            lemma_words_extension_transitive(initial.names, before_dictionary.names, dictionary.names);
            assert forall|region: i64, nation: i64| #[trigger] reverse_nations.at((region, 0, 0), (nation, 0, 0), false) ==
                sum(nation_input_seq_view(nation_rows.take(index as int + 1)), |row: NationRow|
                    if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1 } else { 0 }) by {
                lemma_mapped_prefix_step(nation_rows, |row: NationInput| nation_input_view(row), index as int,
                    |row: NationRow| if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1 } else { 0 });
            }
            lemma_text_identifier(dictionary, 1, name, row.n_name@);
            assert forall|offset: int| 0 <= offset < index implies exists|id: i64| dictionary.names.known(id)
                && #[trigger] dictionary.names.text(id) == (#[trigger] nation_rows[offset]).n_name@ by {
                lemma_known_text_extension(before_dictionary, dictionary, 1, nation_rows[offset].n_name@);
            }
            assert forall|key: i64, selected: i64, region: i64| #[trigger] nation_work.at((key, 0, 0), (selected, region, 0), false) ==
                sum(nation_input_seq_view(nation_rows.take(index as int + 1)), |row: NationRow|
                    if row.n_nationkey == key as int && row.n_regionkey == region as int
                        && model::text_matches(dictionary, 1, selected, row.n_name) { 1 } else { 0 }) by {
                let prefix = nation_input_seq_view(nation_rows.take(index as int));
                let before_weight = |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
                    && model::text_matches(before_dictionary, 1, selected, row.n_name) { 1int } else { 0int };
                let weight = |row: NationRow| if row.n_nationkey == key as int && row.n_regionkey == region as int
                    && model::text_matches(dictionary, 1, selected, row.n_name) { 1int } else { 0int };
                assert forall|offset: int| 0 <= offset < prefix.len() implies before_weight(#[trigger] prefix[offset]) == weight(prefix[offset]) by {
                    lemma_known_text_extension(before_dictionary, dictionary, 1, nation_rows[offset].n_name@);
                }
                assert(nation_input_seq_view(nation_rows.take(index as int + 1)) =~= prefix.push(nation_input_view(nation_rows[index as int])));
                lemma_grown_prefix_step(prefix, nation_input_view(nation_rows[index as int]), before_weight, weight);
            }
        }
        index += 1;
    }
    let table = nation_work.finish();
    let by_region = reverse_nations.finish();
    proof {
        assert(nation_rows.take(nation_rows.len() as int) =~= nation_rows);
        assert(extension(initial, dictionary));
        assert forall|offset: int| 0 <= offset < nation_rows.len() implies
            model::text_present(dictionary, 1, (#[trigger] nation_rows[offset]).n_name@) by {}
    }
    (table, by_region, dictionary)
}

/// The scanned regions as a table of counts: per regionkey and name id.
pub fn scan_regions(regions: &[RegionInput], initial: Dictionary) -> (result: (Table, Dictionary))
    requires initial.valid(), initial.regions.length as int + regions@.len() < u64::MAX as int,
    ensures result.1.valid(), result.1.regions.length as int <= initial.regions.length as int + regions@.len(),
        extension(initial, result.1), result.1.types == initial.types, result.1.names == initial.names,
        dimension_table(result.0, result.1, 5),
        forall|key: i64, name: i64| #[trigger] result.0.at((key, 0, 0), (name, 0, 0), false) ==
            sum(region_input_seq_view(regions@), |row: RegionRow|
                if row.r_regionkey == key as int && model::text_matches(result.1, 2, name, row.r_name) { 1 } else { 0 }),
        forall|offset: int| 0 <= offset < regions@.len() ==> model::text_present(result.1, 2, (#[trigger] regions@[offset]).r_name@),
{
    let mut dictionary = initial;
    let mut region_work = Work::new(&Table::empty(regions.len()));
    let mut index = 0;
    let ghost region_rows = regions@;
    while index < regions.len()
        invariant index <= regions.len(), region_rows == regions@, region_work.valid(), dictionary.valid(),
            initial.regions.length as int + regions@.len() < u64::MAX as int,
            dictionary.regions.length as int <= initial.regions.length as int + index,
            words_extension(initial.regions, dictionary.regions), dictionary.types == initial.types, dictionary.names == initial.names,
            crate::dimensions::work_support(region_work, dictionary, 5),
            forall|key: Major, minor: Major| #[trigger] region_work.at(key, minor, false) >= 0,
            forall|offset: int| 0 <= offset < index ==> exists|id: i64| dictionary.regions.known(id)
                && #[trigger] dictionary.regions.text(id) == (#[trigger] region_rows[offset]).r_name@,
            forall|key: i64, name: i64| #[trigger] region_work.at((key, 0, 0), (name, 0, 0), false) ==
                sum(region_input_seq_view(region_rows.take(index as int)), |row: RegionRow|
                    if row.r_regionkey == key as int && model::text_matches(dictionary, 2, name, row.r_name) { 1 } else { 0 }),
        decreases regions.len() - index,
    {
        let ghost processed = index as int;
        let row = &regions[index];
        let ghost before_dictionary = dictionary;
        let name = dictionary.regions.intern(row.r_name.as_slice());
        region_work.add((row.r_regionkey, 0, 0), (name, 0, 0), Num::Small(1), Num::zero());
        proof {
            lemma_words_extension_transitive(initial.regions, before_dictionary.regions, dictionary.regions);
            lemma_text_identifier(dictionary, 2, name, row.r_name@);
            assert forall|offset: int| 0 <= offset < index implies exists|id: i64| dictionary.regions.known(id)
                && #[trigger] dictionary.regions.text(id) == (#[trigger] region_rows[offset]).r_name@ by {
                lemma_known_text_extension(before_dictionary, dictionary, 2, region_rows[offset].r_name@);
            }
            assert forall|key: i64, selected: i64| #[trigger] region_work.at((key, 0, 0), (selected, 0, 0), false) ==
                sum(region_input_seq_view(region_rows.take(index as int + 1)), |row: RegionRow|
                    if row.r_regionkey == key as int && model::text_matches(dictionary, 2, selected, row.r_name) { 1 } else { 0 }) by {
                let prefix = region_input_seq_view(region_rows.take(index as int));
                let before_weight = |row: RegionRow| if row.r_regionkey == key as int && model::text_matches(before_dictionary, 2, selected, row.r_name) { 1int } else { 0int };
                let weight = |row: RegionRow| if row.r_regionkey == key as int && model::text_matches(dictionary, 2, selected, row.r_name) { 1int } else { 0int };
                assert forall|offset: int| 0 <= offset < prefix.len() implies before_weight(#[trigger] prefix[offset]) == weight(prefix[offset]) by {
                    lemma_known_text_extension(before_dictionary, dictionary, 2, region_rows[offset].r_name@);
                }
                assert(region_input_seq_view(region_rows.take(index as int + 1)) =~= prefix.push(region_input_view(region_rows[index as int])));
                lemma_grown_prefix_step(prefix, region_input_view(region_rows[index as int]), before_weight, weight);
            }
        }
        index += 1;
    }
    let table = region_work.finish();
    proof {
        assert(region_rows.take(region_rows.len() as int) =~= region_rows);
        assert(extension(initial, dictionary));
        assert forall|offset: int| 0 <= offset < region_rows.len() implies
            model::text_present(dictionary, 2, (#[trigger] region_rows[offset]).r_name@) by {}
    }
    (table, dictionary)
}

/// The base: the joined effect of every scanned line, per customer and supplier nation, type and year.
pub fn scan_lines(lines: &[LineInput], nation_count: usize, parts: &Table, suppliers: &Table, customers: &Table, orders: &Table) -> (result: Table)
    requires parts.valid(), suppliers.valid(), customers.valid(), orders.valid(),
    ensures result.valid(),
        forall|key: Major, minor: Major, volume: bool| #[trigger] result.at(key, minor, volume) ==
            sum(line_input_seq_view(lines@), |line: LineitemRow|
                crate::contributions::line_effect(*parts, *suppliers, *customers, *orders, line, key, minor, 1, volume)),
{
    let mut base_work = Work::new(&Table::empty(size_hint(size_hint(nation_count, nation_count), 16)));
    let mut index = 0;
    let ghost line_rows = lines@;
    while index < lines.len()
        invariant index <= lines.len(), line_rows == lines@, base_work.valid(), parts.valid(), suppliers.valid(), customers.valid(), orders.valid(),
            forall|key: Major, minor: Major, volume: bool| #[trigger] base_work.at(key, minor, volume) ==
                sum(line_input_seq_view(line_rows.take(index as int)), |line: LineitemRow|
                    crate::contributions::line_effect(*parts, *suppliers, *customers, *orders, line, key, minor, 1, volume)),
        decreases lines.len() - index,
    {
        let ghost processed = line_rows.take(index as int);
        line_contribution(&parts, &suppliers, &customers, &orders, lines[index], 1, &mut base_work);
        proof {
            assert forall|key: Major, minor: Major, volume: bool| #[trigger] base_work.at(key, minor, volume) ==
                sum(line_input_seq_view(line_rows.take(index as int + 1)), |line: LineitemRow|
                    crate::contributions::line_effect(*parts, *suppliers, *customers, *orders, line, key, minor, 1, volume)) by {
                lemma_mapped_prefix_step(line_rows, |row: LineInput| line_input_view(row), index as int, |line: LineitemRow|
                    crate::contributions::line_effect(*parts, *suppliers, *customers, *orders, line, key, minor, 1, volume));
            }
        }
        index += 1;
    }
    let base = base_work.finish();
    proof { assert(line_rows.take(line_rows.len() as int) =~= line_rows); }
    base
}

/// The routes of every nation: per nation, the regions of its customers and the names of its suppliers.
pub fn route_all_nations(nations: &Table, regions: &Table) -> (result: (Table, Table))
    requires nations.valid(), regions.valid(),
    ensures result.0.valid(), result.1.valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] result.0.at(key, minor, measure)
            == crate::routing::all_customer_routes(nations.keys_view(), *nations, *regions, key, minor, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] result.1.at(key, minor, measure)
            == crate::routing::all_supplier_routes(nations.keys_view(), *nations, key, minor, measure),
{
    let empty = Table::empty(0);
    let mut customer_routes = Work::new(&empty);
    let mut supplier_routes = Work::new(&empty);
    let mut index = 0;
    let nation_keys = nations.keys();
    index = 0;
    proof { reveal(crate::routing::all_customer_routes); reveal(crate::routing::all_supplier_routes); }
    while index < nation_keys.len()
        invariant index <= nation_keys.len(), nations.valid(), regions.valid(), customer_routes.valid(), supplier_routes.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] customer_routes.at(key, minor, measure)
                == crate::routing::all_customer_routes(nation_keys@.take(index as int), *nations, *regions, key, minor, measure),
            forall|key: Major, minor: Major, measure: bool| #[trigger] supplier_routes.at(key, minor, measure)
                == crate::routing::all_supplier_routes(nation_keys@.take(index as int), *nations, key, minor, measure),
        decreases nation_keys.len() - index,
    {
        route_nation(&nations, &regions, nation_keys[index].0, &mut customer_routes, &mut supplier_routes, 1);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] customer_routes.at(key, minor, measure)
                == crate::routing::all_customer_routes(nation_keys@.take(index as int + 1), *nations, *regions, key, minor, measure) by {
                reveal(crate::routing::all_customer_routes);
                lemma_sum_take(nation_keys@, index as int, |entry: Major|
                    crate::routing::customer_nation_effect(nations.rows((entry.0, 0, 0)), *regions, entry.0, 1, key, minor, measure));
            }
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] supplier_routes.at(key, minor, measure)
                == crate::routing::all_supplier_routes(nation_keys@.take(index as int + 1), *nations, key, minor, measure) by {
                reveal(crate::routing::all_supplier_routes);
                lemma_sum_take(nation_keys@, index as int, |entry: Major|
                    crate::routing::supplier_nation_effect(nations.rows((entry.0, 0, 0)), entry.0, 1, key, minor, measure));
            }
        }
        index += 1;
    }
    proof { assert(nation_keys@.take(nation_keys@.len() as int) =~= nation_keys@); }
    (customer_routes.finish(), supplier_routes.finish())
}

}
