// Apply: one function per table, applying its changes to the previous version's table.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::table::*;
use crate::dictionary::*;
use crate::dimensions::*;
use crate::maintenance::*;
use crate::model;
use crate::num::Num;
use crate::counting::*;
use crate::contributions::line_contribution;

verus! {

/// A transaction's order changes never flag an order for both years.
pub proof fn lemma_order_flags(delta: Seq<OrderDelta>, full: Seq<(OrdersRow, int)>, index: int)
    requires orders_delta_ok(delta, full), 0 <= index < delta.len(),
    ensures !(delta[index].in_1995 != 0 && delta[index].in_1996 != 0),
{
    let date = full[index].0.o_orderdate;
    assert((delta[index].in_1995 != 0) == (date >= 9131 && date <= 9495));
    assert((delta[index].in_1996 != 0) == (date >= 9496 && date <= 9861));
}

/// `after` is the part table `before` with the delta's rows added, their types read in `dictionary`.
#[verifier::opaque]
pub open spec fn parts_applied(before: Table, after: Table, dictionary: Dictionary, delta: Seq<PartDelta>) -> bool {
    forall|key: i64, kind: i64| #[trigger] after.at((key, 0, 0), (kind, 0, 0), false) ==
        before.at((key, 0, 0), (kind, 0, 0), false)
            + sum(part_delta_seq_view(delta), |change: (PartRow, int)|
                if change.0.p_partkey == key as int && model::text_matches(dictionary, 0, kind, change.0.p_type) { change.1 } else { 0 })
}

#[verifier::opaque]
pub open spec fn suppliers_applied(before: Table, after: Table, delta: Seq<SupplierDelta>) -> bool {
    forall|key: i64, nation: i64| #[trigger] after.at((key, 0, 0), (nation, 0, 0), false) ==
        before.at((key, 0, 0), (nation, 0, 0), false)
            + sum(supplier_delta_seq_view(delta), |change: (SupplierRow, int)|
                if change.0.s_suppkey == key as int && change.0.s_nationkey == nation as int { change.1 } else { 0 })
}

#[verifier::opaque]
pub open spec fn customers_applied(before: Table, after: Table, delta: Seq<CustomerDelta>) -> bool {
    forall|key: i64, nation: i64| #[trigger] after.at((key, 0, 0), (nation, 0, 0), false) ==
        before.at((key, 0, 0), (nation, 0, 0), false)
            + sum(customer_delta_seq_view(delta), |change: (CustomerRow, int)|
                if change.0.c_custkey == key as int && change.0.c_nationkey == nation as int { change.1 } else { 0 })
}

/// The order tables: per orderkey, customer and year, and per customer and orderkey.
#[verifier::opaque]
pub open spec fn orders_applied(before: Table, after: Table, by_customer_before: Table, by_customer_after: Table, delta: Seq<OrderDelta>) -> bool {
    &&& forall|customer: i64, order: i64| #[trigger] by_customer_after.at((customer, 0, 0), (order, 0, 0), false) ==
        by_customer_before.at((customer, 0, 0), (order, 0, 0), false)
            + sum(delta, |change: OrderDelta|
                if change.o_orderkey == order && change.o_custkey == customer
                    && (change.in_1995 != 0 || change.in_1996 != 0) { change.sign as int } else { 0 })
    &&& forall|key: i64, customer: i64, year: i64| #[trigger] after.at((key, 0, 0), (customer, year, 0), false) ==
        before.at((key, 0, 0), (customer, year, 0), false)
            + sum(delta, |change: OrderDelta|
                if change.o_orderkey == key && change.o_custkey == customer
                    && ((year == 1995 && change.in_1995 != 0) || (year == 1996 && change.in_1996 != 0))
                { change.sign as int } else { 0 })
}

/// The nation tables: per nationkey, name id and regionkey, and per regionkey and nationkey.
#[verifier::opaque]
pub open spec fn nations_applied(before: Table, after: Table, by_region_before: Table, by_region_after: Table,
    dictionary: Dictionary, delta: Seq<NationDelta>) -> bool {
    &&& forall|region: i64, nation: i64| #[trigger] by_region_after.at((region, 0, 0), (nation, 0, 0), false) ==
        by_region_before.at((region, 0, 0), (nation, 0, 0), false)
            + sum(nation_delta_seq_view(delta), |change: (NationRow, int)|
                if change.0.n_regionkey == region as int && change.0.n_nationkey == nation as int { change.1 } else { 0 })
    &&& forall|key: i64, name: i64, region: i64| #[trigger] after.at((key, 0, 0), (name, region, 0), false) ==
        before.at((key, 0, 0), (name, region, 0), false)
            + sum(nation_delta_seq_view(delta), |change: (NationRow, int)|
                if change.0.n_nationkey == key as int && change.0.n_regionkey == region as int
                    && model::text_matches(dictionary, 1, name, change.0.n_name) { change.1 } else { 0 })
}

#[verifier::opaque]
pub open spec fn regions_applied(before: Table, after: Table, dictionary: Dictionary, delta: Seq<RegionDelta>) -> bool {
    forall|key: i64, name: i64| #[trigger] after.at((key, 0, 0), (name, 0, 0), false) ==
        before.at((key, 0, 0), (name, 0, 0), false)
            + sum(region_delta_seq_view(delta), |change: (RegionRow, int)|
                if change.0.r_regionkey == key as int && model::text_matches(dictionary, 2, name, change.0.r_name) { change.1 } else { 0 })
}

/// The part table after a transaction, with the types its rows add to the dictionary.
pub fn apply_parts(delta: &Vec<PartDelta>, before: &Table, initial: Dictionary) -> (result: (Table, Dictionary))
    requires before.valid(), supported_table(*before, initial, 0), initial.valid(),
        initial.types.length as int + delta@.len() < u64::MAX as int,
    ensures result.1.valid(), extension(initial, result.1), result.1.names == initial.names, result.1.regions == initial.regions,
        result.1.types.length as int <= initial.types.length as int + delta@.len(),
        supported_table(result.0, result.1, 0),
        parts_applied(*before, result.0, result.1, delta@),
        forall|offset: int| 0 <= offset < delta@.len() ==> model::text_present(result.1, 0, (#[trigger] delta@[offset]).row.p_type@),
{
    proof { reveal(parts_applied); }
    let mut dictionary = initial;
    let mut parts = Work::new(before);
    let mut index = 0;
    let ghost part_delta_rows = delta@;
    while index < delta.len()
        invariant index <= delta.len(), part_delta_rows == delta@, parts.valid(), dictionary.valid(),
            initial.types.length as int + delta@.len() < u64::MAX as int,
            dictionary.types.length as int <= initial.types.length as int + index,
            crate::dimensions::work_support(parts, dictionary, 0),
            words_extension(initial.types, dictionary.types), dictionary.names == initial.names, dictionary.regions == initial.regions,
            forall|offset: int| 0 <= offset < index ==> exists|id: i64| dictionary.types.known(id)
                && #[trigger] dictionary.types.text(id) == (#[trigger] part_delta_rows[offset]).row.p_type@,
            forall|key: i64, kind: i64| #[trigger] parts.at((key, 0, 0), (kind, 0, 0), false) ==
                before.at((key, 0, 0), (kind, 0, 0), false)
                    + sum(part_delta_seq_view(part_delta_rows.take(index as int)), |change: (PartRow, int)|
                        if change.0.p_partkey == key as int && model::text_matches(dictionary, 0, kind, change.0.p_type) { change.1 } else { 0 }),
        decreases delta.len() - index,
    {
        let ghost processed = part_delta_rows.take(index as int);
        let change = &delta[index];
        let ghost before_dictionary = dictionary;
        let kind = dictionary.types.intern(change.row.p_type.as_slice());
        parts.add((change.row.p_partkey, 0, 0), (kind, 0, 0), Num::Small(change.sign as i128), Num::zero());
        proof {
            lemma_words_extension_transitive(initial.types, before_dictionary.types, dictionary.types);
            lemma_text_identifier(dictionary, 0, kind, change.row.p_type@);
            assert forall|offset: int| 0 <= offset < index implies exists|id: i64| dictionary.types.known(id)
                && #[trigger] dictionary.types.text(id) == (#[trigger] part_delta_rows[offset]).row.p_type@ by {
                lemma_known_text_extension(before_dictionary, dictionary, 0, part_delta_rows[offset].row.p_type@);
            }
            assert forall|key: i64, selected: i64| #[trigger] parts.at((key, 0, 0), (selected, 0, 0), false) ==
                before.at((key, 0, 0), (selected, 0, 0), false)
                    + sum(part_delta_seq_view(part_delta_rows.take(index as int + 1)), |change: (PartRow, int)|
                        if change.0.p_partkey == key as int && model::text_matches(dictionary, 0, selected, change.0.p_type) { change.1 } else { 0 }) by {
                let prefix = part_delta_seq_view(part_delta_rows.take(index as int));
                let before_weight = |change: (PartRow, int)| if change.0.p_partkey == key as int
                    && model::text_matches(before_dictionary, 0, selected, change.0.p_type) { change.1 } else { 0int };
                let weight = |change: (PartRow, int)| if change.0.p_partkey == key as int
                    && model::text_matches(dictionary, 0, selected, change.0.p_type) { change.1 } else { 0int };
                assert forall|offset: int| 0 <= offset < prefix.len() implies before_weight(#[trigger] prefix[offset]) == weight(prefix[offset]) by {
                    lemma_known_text_extension(before_dictionary, dictionary, 0, part_delta_rows[offset].row.p_type@);
                }
                let next = (part_input_view(part_delta_rows[index as int].row), part_delta_rows[index as int].sign as int);
                assert(part_delta_seq_view(part_delta_rows.take(index as int + 1)) =~= prefix.push(next));
                lemma_grown_prefix_step(prefix, next, before_weight, weight);
            }
        }
        index += 1;
    }
    let table = parts.finish();
    proof {
        assert(part_delta_rows.take(part_delta_rows.len() as int) =~= part_delta_rows);
        assert(extension(initial, dictionary));
        assert forall|offset: int| 0 <= offset < part_delta_rows.len() implies
            model::text_present(dictionary, 0, (#[trigger] part_delta_rows[offset]).row.p_type@) by {}
    }
    (table, dictionary)
}

/// The supplier table after a transaction.
pub fn apply_suppliers(delta: &[SupplierDelta], before: &Table, dictionary: &Dictionary) -> (result: Table)
    requires before.valid(), supported_table(*before, *dictionary, 1),
    ensures supported_table(result, *dictionary, 1),
        suppliers_applied(*before, result, delta@),
{
    proof { reveal(suppliers_applied); }
    let mut suppliers = Work::new(before);
    let mut index = 0;
    let ghost supplier_delta_rows = delta@;
    while index < delta.len()
        invariant index <= delta.len(), supplier_delta_rows == delta@, suppliers.valid(),
            crate::dimensions::work_support(suppliers, *dictionary, 1),
            forall|key: i64, nation: i64| #[trigger] suppliers.at((key, 0, 0), (nation, 0, 0), false) ==
                before.at((key, 0, 0), (nation, 0, 0), false)
                    + sum(supplier_delta_seq_view(supplier_delta_rows.take(index as int)), |change: (SupplierRow, int)|
                        if change.0.s_suppkey == key as int && change.0.s_nationkey == nation as int { change.1 } else { 0 }),
        decreases delta.len() - index,
    {
        let ghost processed = supplier_delta_rows.take(index as int);
        let change = delta[index];
        suppliers.add((change.row.s_suppkey, 0, 0), (change.row.s_nationkey, 0, 0), Num::Small(change.sign as i128), Num::zero());
        proof {
            assert forall|key: i64, nation: i64| #[trigger] suppliers.at((key, 0, 0), (nation, 0, 0), false) ==
                before.at((key, 0, 0), (nation, 0, 0), false)
                    + sum(supplier_delta_seq_view(supplier_delta_rows.take(index as int + 1)), |change: (SupplierRow, int)|
                        if change.0.s_suppkey == key as int && change.0.s_nationkey == nation as int { change.1 } else { 0 }) by {
                lemma_mapped_prefix_step(supplier_delta_rows, |d: SupplierDelta| (supplier_input_view(d.row), d.sign as int), index as int, |change: (SupplierRow, int)| if change.0.s_suppkey == key as int
                    && change.0.s_nationkey == nation as int { change.1 } else { 0 });
            }
        }
        index += 1;
    }
    let table = suppliers.finish();
    proof { assert(supplier_delta_rows.take(supplier_delta_rows.len() as int) =~= supplier_delta_rows); }
    table
}

/// The customer table after a transaction.
pub fn apply_customers(delta: &[CustomerDelta], before: &Table, dictionary: &Dictionary) -> (result: Table)
    requires before.valid(), supported_table(*before, *dictionary, 2),
    ensures supported_table(result, *dictionary, 2),
        customers_applied(*before, result, delta@),
{
    proof { reveal(customers_applied); }
    let mut customers = Work::new(before);
    let mut index = 0;
    let ghost customer_delta_rows = delta@;
    while index < delta.len()
        invariant index <= delta.len(), customer_delta_rows == delta@, customers.valid(),
            crate::dimensions::work_support(customers, *dictionary, 2),
            forall|key: i64, nation: i64| #[trigger] customers.at((key, 0, 0), (nation, 0, 0), false) ==
                before.at((key, 0, 0), (nation, 0, 0), false)
                    + sum(customer_delta_seq_view(customer_delta_rows.take(index as int)), |change: (CustomerRow, int)|
                        if change.0.c_custkey == key as int && change.0.c_nationkey == nation as int { change.1 } else { 0 }),
        decreases delta.len() - index,
    {
        let ghost processed = customer_delta_rows.take(index as int);
        let change = delta[index];
        customers.add((change.row.c_custkey, 0, 0), (change.row.c_nationkey, 0, 0), Num::Small(change.sign as i128), Num::zero());
        proof {
            assert forall|key: i64, nation: i64| #[trigger] customers.at((key, 0, 0), (nation, 0, 0), false) ==
                before.at((key, 0, 0), (nation, 0, 0), false)
                    + sum(customer_delta_seq_view(customer_delta_rows.take(index as int + 1)), |change: (CustomerRow, int)|
                        if change.0.c_custkey == key as int && change.0.c_nationkey == nation as int { change.1 } else { 0 }) by {
                lemma_mapped_prefix_step(customer_delta_rows, |d: CustomerDelta| (customer_input_view(d.row), d.sign as int), index as int, |change: (CustomerRow, int)| if change.0.c_custkey == key as int
                    && change.0.c_nationkey == nation as int { change.1 } else { 0 });
            }
        }
        index += 1;
    }
    let table = customers.finish();
    proof { assert(customer_delta_rows.take(customer_delta_rows.len() as int) =~= customer_delta_rows); }
    table
}

/// The order tables after a transaction: per orderkey, customer and year, and per customer and orderkey.
pub fn apply_orders(delta: &[OrderDelta], orders_before: &Table, by_customer_before: &Table, dictionary: &Dictionary,
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>) -> (result: (Table, Table))
    requires orders_delta_ok(delta@, orders_full), orders_before.valid(), by_customer_before.valid(),
        supported_table(*orders_before, *dictionary, 3), supported_table(*by_customer_before, *dictionary, 1),
    ensures supported_table(result.0, *dictionary, 3), supported_table(result.1, *dictionary, 1),
        orders_applied(*orders_before, result.0, *by_customer_before, result.1, delta@),
{
    proof { reveal(orders_applied); }
    let mut orders = Work::new(orders_before);
    let mut reverse_orders = Work::new(by_customer_before);
    let mut index = 0;
    let ghost orders_delta_rows = delta@;
    while index < delta.len()
        invariant index <= delta.len(), orders_delta_rows == delta@, orders.valid(), reverse_orders.valid(), orders_delta_ok(delta@, orders_full),
            crate::dimensions::work_support(reverse_orders, *dictionary, 1),
            crate::dimensions::work_support(orders, *dictionary, 3),
            forall|customer: i64, order: i64| #[trigger] reverse_orders.at((customer, 0, 0), (order, 0, 0), false) ==
                by_customer_before.at((customer, 0, 0), (order, 0, 0), false)
                    + sum(orders_delta_rows.take(index as int), |change: OrderDelta|
                        if change.o_orderkey == order && change.o_custkey == customer
                            && (change.in_1995 != 0 || change.in_1996 != 0) { change.sign as int } else { 0 }),
            forall|key: i64, customer: i64, year: i64| #[trigger] orders.at((key, 0, 0), (customer, year, 0), false) ==
                orders_before.at((key, 0, 0), (customer, year, 0), false)
                    + sum(orders_delta_rows.take(index as int), |change: OrderDelta|
                        if change.o_orderkey == key && change.o_custkey == customer
                            && ((year == 1995 && change.in_1995 != 0) || (year == 1996 && change.in_1996 != 0))
                        { change.sign as int } else { 0 }),
        decreases delta.len() - index,
    {
        let ghost processed = orders_delta_rows.take(index as int);
        let change = delta[index];
        proof { lemma_order_flags(delta@, orders_full, index as int); }
        if change.in_1995 != 0 || change.in_1996 != 0 {
            let year: i64 = if change.in_1995 != 0 { 1995 } else { 1996 };
            orders.add((change.o_orderkey, 0, 0), (change.o_custkey, year, 0), Num::Small(change.sign as i128), Num::zero());
            reverse_orders.add((change.o_custkey, 0, 0), (change.o_orderkey, 0, 0), Num::Small(change.sign as i128), Num::zero());
        }
        proof {
            assert forall|customer: i64, order: i64| #[trigger] reverse_orders.at((customer, 0, 0), (order, 0, 0), false) ==
                by_customer_before.at((customer, 0, 0), (order, 0, 0), false)
                    + sum(orders_delta_rows.take(index as int + 1), |change: OrderDelta|
                        if change.o_orderkey == order && change.o_custkey == customer
                            && (change.in_1995 != 0 || change.in_1996 != 0) { change.sign as int } else { 0 }) by {
                lemma_sum_take(orders_delta_rows, index as int, |change: OrderDelta|
                    if change.o_orderkey == order && change.o_custkey == customer
                        && (change.in_1995 != 0 || change.in_1996 != 0) { change.sign as int } else { 0 });
            }
            assert forall|key: i64, customer: i64, year: i64| #[trigger] orders.at((key, 0, 0), (customer, year, 0), false) ==
                orders_before.at((key, 0, 0), (customer, year, 0), false)
                    + sum(orders_delta_rows.take(index as int + 1), |change: OrderDelta|
                        if change.o_orderkey == key && change.o_custkey == customer
                            && ((year == 1995 && change.in_1995 != 0) || (year == 1996 && change.in_1996 != 0))
                        { change.sign as int } else { 0 }) by {
                lemma_sum_take(orders_delta_rows, index as int, |change: OrderDelta|
                    if change.o_orderkey == key && change.o_custkey == customer
                        && ((year == 1995 && change.in_1995 != 0) || (year == 1996 && change.in_1996 != 0))
                    { change.sign as int } else { 0 });
            }
        }
        index += 1;
    }
    let table = orders.finish();
    let by_customer = reverse_orders.finish();
    proof { assert(orders_delta_rows.take(orders_delta_rows.len() as int) =~= orders_delta_rows); }
    (table, by_customer)
}

/// The nation tables after a transaction, with the names its rows add to the dictionary, and the changed nationkeys.
pub fn apply_nations(delta: &Vec<NationDelta>, before: &Table, by_region_before: &Table, initial: Dictionary) -> (result: (Table, Table, Dictionary, Keys))
    requires before.valid(), by_region_before.valid(), initial.valid(),
        supported_table(*before, initial, 4), supported_table(*by_region_before, initial, 1),
        initial.names.length as int + delta@.len() < u64::MAX as int,
    ensures result.2.valid(), extension(initial, result.2), result.2.types == initial.types, result.2.regions == initial.regions,
        result.2.names.length as int <= initial.names.length as int + delta@.len(),
        supported_table(result.0, result.2, 4), supported_table(result.1, result.2, 1),
        nations_applied(*before, result.0, *by_region_before, result.1, result.2, delta@),
        forall|offset: int| 0 <= offset < delta@.len() ==> model::text_present(result.2, 1, (#[trigger] delta@[offset]).row.n_name@),
        result.3.valid(),
        forall|offset: int| 0 <= offset < delta@.len() ==> result.3.has((#[trigger] delta@[offset]).row.n_nationkey),
{
    proof { reveal(nations_applied); }
    let mut dictionary = initial;
    let mut nations = Work::new(before);
    let mut reverse_nations = Work::new(by_region_before);
    let mut changed_nations = Keys::new();
    let mut index = 0;
    let ghost nation_delta_rows = delta@;
    while index < delta.len()
        invariant index <= delta.len(), nation_delta_rows == delta@, nations.valid(), reverse_nations.valid(), dictionary.valid(),
            initial.names.length as int + delta@.len() < u64::MAX as int,
            dictionary.names.length as int <= initial.names.length as int + index,
            crate::dimensions::work_support(reverse_nations, dictionary, 1),
            changed_nations.valid(),
            forall|offset: int| 0 <= offset < index ==> changed_nations.has((#[trigger] nation_delta_rows[offset]).row.n_nationkey),
            crate::dimensions::work_support(nations, dictionary, 4),
            words_extension(initial.names, dictionary.names), dictionary.types == initial.types, dictionary.regions == initial.regions,
            forall|region: i64, nation: i64| #[trigger] reverse_nations.at((region, 0, 0), (nation, 0, 0), false) ==
                by_region_before.at((region, 0, 0), (nation, 0, 0), false)
                    + sum(nation_delta_seq_view(nation_delta_rows.take(index as int)), |change: (NationRow, int)|
                        if change.0.n_regionkey == region as int && change.0.n_nationkey == nation as int { change.1 } else { 0 }),
            forall|offset: int| 0 <= offset < index ==> exists|id: i64| dictionary.names.known(id)
                && #[trigger] dictionary.names.text(id) == (#[trigger] nation_delta_rows[offset]).row.n_name@,
            forall|key: i64, name: i64, region: i64| #[trigger] nations.at((key, 0, 0), (name, region, 0), false) ==
                before.at((key, 0, 0), (name, region, 0), false)
                    + sum(nation_delta_seq_view(nation_delta_rows.take(index as int)), |change: (NationRow, int)|
                        if change.0.n_nationkey == key as int && change.0.n_regionkey == region as int
                            && model::text_matches(dictionary, 1, name, change.0.n_name) { change.1 } else { 0 }),
        decreases delta.len() - index,
    {
        let ghost processed = nation_delta_rows.take(index as int);
        let change = &delta[index];
        let ghost before_dictionary = dictionary;
        let name = dictionary.names.intern(change.row.n_name.as_slice());
        nations.add((change.row.n_nationkey, 0, 0), (name, change.row.n_regionkey, 0), Num::Small(change.sign as i128), Num::zero());
        reverse_nations.add((change.row.n_regionkey, 0, 0), (change.row.n_nationkey, 0, 0), Num::Small(change.sign as i128), Num::zero());
        changed_nations.add(change.row.n_nationkey);
        proof {
            lemma_words_extension_transitive(initial.names, before_dictionary.names, dictionary.names);
            assert forall|region: i64, nation: i64| #[trigger] reverse_nations.at((region, 0, 0), (nation, 0, 0), false) ==
                by_region_before.at((region, 0, 0), (nation, 0, 0), false)
                    + sum(nation_delta_seq_view(nation_delta_rows.take(index as int + 1)), |change: (NationRow, int)|
                        if change.0.n_regionkey == region as int && change.0.n_nationkey == nation as int { change.1 } else { 0 }) by {
                lemma_mapped_prefix_step(nation_delta_rows, |d: NationDelta| (nation_input_view(d.row), d.sign as int), index as int, |change: (NationRow, int)|
                    if change.0.n_regionkey == region as int && change.0.n_nationkey == nation as int { change.1 } else { 0 });
            }
            lemma_text_identifier(dictionary, 1, name, change.row.n_name@);
            assert forall|offset: int| 0 <= offset < index implies exists|id: i64| dictionary.names.known(id)
                && #[trigger] dictionary.names.text(id) == (#[trigger] nation_delta_rows[offset]).row.n_name@ by {
                lemma_known_text_extension(before_dictionary, dictionary, 1, nation_delta_rows[offset].row.n_name@);
            }
            assert forall|key: i64, selected: i64, region: i64| #[trigger] nations.at((key, 0, 0), (selected, region, 0), false) ==
                before.at((key, 0, 0), (selected, region, 0), false)
                    + sum(nation_delta_seq_view(nation_delta_rows.take(index as int + 1)), |change: (NationRow, int)|
                        if change.0.n_nationkey == key as int && change.0.n_regionkey == region as int
                            && model::text_matches(dictionary, 1, selected, change.0.n_name) { change.1 } else { 0 }) by {
                let prefix = nation_delta_seq_view(nation_delta_rows.take(index as int));
                let before_weight = |change: (NationRow, int)| if change.0.n_nationkey == key as int && change.0.n_regionkey == region as int
                    && model::text_matches(before_dictionary, 1, selected, change.0.n_name) { change.1 } else { 0int };
                let weight = |change: (NationRow, int)| if change.0.n_nationkey == key as int && change.0.n_regionkey == region as int
                    && model::text_matches(dictionary, 1, selected, change.0.n_name) { change.1 } else { 0int };
                assert forall|offset: int| 0 <= offset < prefix.len() implies before_weight(#[trigger] prefix[offset]) == weight(prefix[offset]) by {
                    lemma_known_text_extension(before_dictionary, dictionary, 1, nation_delta_rows[offset].row.n_name@);
                }
                let next = (nation_input_view(nation_delta_rows[index as int].row), nation_delta_rows[index as int].sign as int);
                assert(nation_delta_seq_view(nation_delta_rows.take(index as int + 1)) =~= prefix.push(next));
                lemma_grown_prefix_step(prefix, next, before_weight, weight);
            }
        }
        index += 1;
    }
    let table = nations.finish();
    let by_region = reverse_nations.finish();
    proof {
        assert(nation_delta_rows.take(nation_delta_rows.len() as int) =~= nation_delta_rows);
        assert(extension(initial, dictionary));
        assert forall|offset: int| 0 <= offset < nation_delta_rows.len() implies
            model::text_present(dictionary, 1, (#[trigger] nation_delta_rows[offset]).row.n_name@) by {}
    }
    (table, by_region, dictionary, changed_nations)
}

/// The region table after a transaction, with the names its rows add to the dictionary, and the nations
/// of the changed regions added to the changed nationkeys.
pub fn apply_regions(delta: &Vec<RegionDelta>, before: &Table, nations_by_region_before: &Table, initial: Dictionary,
    changed: Keys, Ghost(nation_rows): Ghost<Seq<NationDelta>>) -> (result: (Table, Dictionary, Keys))
    requires before.valid(), nations_by_region_before.valid(), initial.valid(), supported_table(*before, initial, 5),
        initial.regions.length as int + delta@.len() < u64::MAX as int,
        changed.valid(), forall|offset: int| 0 <= offset < nation_rows.len() ==> changed.has((#[trigger] nation_rows[offset]).row.n_nationkey),
    ensures result.1.valid(), extension(initial, result.1), result.1.types == initial.types, result.1.names == initial.names,
        result.1.regions.length as int <= initial.regions.length as int + delta@.len(),
        supported_table(result.0, result.1, 5),
        regions_applied(*before, result.0, result.1, delta@),
        forall|offset: int| 0 <= offset < delta@.len() ==> model::text_present(result.1, 2, (#[trigger] delta@[offset]).row.r_name@),
        result.2.valid(),
        forall|offset: int| 0 <= offset < nation_rows.len() ==> result.2.has((#[trigger] nation_rows[offset]).row.n_nationkey),
        forall|earlier: int, offset: int| 0 <= earlier < delta@.len()
            && 0 <= offset < nations_by_region_before.rows((delta@[earlier].row.r_regionkey, 0, 0)).len() ==>
            result.2.has((#[trigger] nations_by_region_before.rows((delta@[earlier].row.r_regionkey, 0, 0))[offset]).first),
{
    proof { reveal(regions_applied); }
    let mut dictionary = initial;
    let mut regions = Work::new(before);
    let mut changed_nations = changed;
    let mut index = 0;
    let ghost region_delta_rows = delta@;
    while index < delta.len()
        invariant index <= delta.len(), region_delta_rows == delta@, regions.valid(), dictionary.valid(), nations_by_region_before.valid(),
            initial.regions.length as int + delta@.len() < u64::MAX as int,
            dictionary.regions.length as int <= initial.regions.length as int + index,
            changed_nations.valid(),
            forall|offset: int| 0 <= offset < nation_rows.len() ==> changed_nations.has((#[trigger] nation_rows[offset]).row.n_nationkey),
            forall|earlier: int, offset: int| 0 <= earlier < index
                && 0 <= offset < nations_by_region_before.rows((region_delta_rows[earlier].row.r_regionkey, 0, 0)).len() ==>
                changed_nations.has((#[trigger] nations_by_region_before.rows((region_delta_rows[earlier].row.r_regionkey, 0, 0))[offset]).first),
            crate::dimensions::work_support(regions, dictionary, 5),
            words_extension(initial.regions, dictionary.regions), dictionary.types == initial.types, dictionary.names == initial.names,
            forall|offset: int| 0 <= offset < index ==> exists|id: i64| dictionary.regions.known(id)
                && #[trigger] dictionary.regions.text(id) == (#[trigger] region_delta_rows[offset]).row.r_name@,
            forall|key: i64, name: i64| #[trigger] regions.at((key, 0, 0), (name, 0, 0), false) ==
                before.at((key, 0, 0), (name, 0, 0), false)
                    + sum(region_delta_seq_view(region_delta_rows.take(index as int)), |change: (RegionRow, int)|
                        if change.0.r_regionkey == key as int && model::text_matches(dictionary, 2, name, change.0.r_name) { change.1 } else { 0 }),
        decreases delta.len() - index,
    {
        let ghost processed = region_delta_rows.take(index as int);
        let change = &delta[index];
        let ghost before_dictionary = dictionary;
        let name = dictionary.regions.intern(change.row.r_name.as_slice());
        regions.add((change.row.r_regionkey, 0, 0), (name, 0, 0), Num::Small(change.sign as i128), Num::zero());
        proof {
            lemma_words_extension_transitive(initial.regions, before_dictionary.regions, dictionary.regions);
            lemma_text_identifier(dictionary, 2, name, change.row.r_name@);
            assert forall|offset: int| 0 <= offset < index implies exists|id: i64| dictionary.regions.known(id)
                && #[trigger] dictionary.regions.text(id) == (#[trigger] region_delta_rows[offset]).row.r_name@ by {
                lemma_known_text_extension(before_dictionary, dictionary, 2, region_delta_rows[offset].row.r_name@);
            }
            assert forall|key: i64, selected: i64| #[trigger] regions.at((key, 0, 0), (selected, 0, 0), false) ==
                before.at((key, 0, 0), (selected, 0, 0), false)
                    + sum(region_delta_seq_view(region_delta_rows.take(index as int + 1)), |change: (RegionRow, int)|
                        if change.0.r_regionkey == key as int && model::text_matches(dictionary, 2, selected, change.0.r_name) { change.1 } else { 0 }) by {
                let prefix = region_delta_seq_view(region_delta_rows.take(index as int));
                let before_weight = |change: (RegionRow, int)| if change.0.r_regionkey == key as int
                    && model::text_matches(before_dictionary, 2, selected, change.0.r_name) { change.1 } else { 0int };
                let weight = |change: (RegionRow, int)| if change.0.r_regionkey == key as int
                    && model::text_matches(dictionary, 2, selected, change.0.r_name) { change.1 } else { 0int };
                assert forall|offset: int| 0 <= offset < prefix.len() implies before_weight(#[trigger] prefix[offset]) == weight(prefix[offset]) by {
                    lemma_known_text_extension(before_dictionary, dictionary, 2, region_delta_rows[offset].row.r_name@);
                }
                let next = (region_input_view(region_delta_rows[index as int].row), region_delta_rows[index as int].sign as int);
                assert(region_delta_seq_view(region_delta_rows.take(index as int + 1)) =~= prefix.push(next));
                lemma_grown_prefix_step(prefix, next, before_weight, weight);
            }
        }
        include_region_nations(nations_by_region_before, change.row.r_regionkey, &mut changed_nations);
        index += 1;
    }
    let table = regions.finish();
    proof {
        assert(region_delta_rows.take(region_delta_rows.len() as int) =~= region_delta_rows);
        assert(extension(initial, dictionary));
        assert forall|offset: int| 0 <= offset < region_delta_rows.len() implies
            model::text_present(dictionary, 2, (#[trigger] region_delta_rows[offset]).row.r_name@) by {}
    }
    (table, dictionary, changed_nations)
}

/// The base's change: the lineitem changes against the previous version's dimensions, and the rescanned
/// lines of the changed orders, parts and suppliers against the new dimensions less the previous ones.
pub fn changed_lines_delta(before: &model::Data, parts: &Table, suppliers: &Table, customers: &Table, orders: &Table,
    delta: &[LineDelta], orders_rows: &[LineInput], parts_rows: &[LineInput], suppliers_rows: &[LineInput],
    required_orders: &Keys, required_parts: &Keys, required_suppliers: &Keys) -> (result: Table)
    requires model::runtime_valid(*before), parts.valid(), suppliers.valid(), customers.valid(), orders.valid(),
    ensures result.valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] result.at(key, minor, measure) ==
            sum(line_delta_seq_view(delta@), |change: (LineitemRow, int)|
                crate::contributions::line_effect(before.parts, before.suppliers, before.customers, before.orders, change.0, key, minor, change.1, measure))
            + crate::contributions::rescan_effect(orders_rows@, *required_orders, *required_parts, *required_suppliers, 0,
                *parts, *suppliers, *customers, *orders, *before, key, minor, measure)
            + crate::contributions::rescan_effect(parts_rows@, *required_orders, *required_parts, *required_suppliers, 1,
                *parts, *suppliers, *customers, *orders, *before, key, minor, measure)
            + crate::contributions::rescan_effect(suppliers_rows@, *required_orders, *required_parts, *required_suppliers, 2,
                *parts, *suppliers, *customers, *orders, *before, key, minor, measure),
{
    let mut delta_base = Work::new(&Table::empty(0));
    let mut index = 0;
    let ghost line_changes = delta@;
    while index < delta.len()
        invariant index <= delta.len(), line_changes == delta@, delta_base.valid(), model::runtime_valid(*before),
            forall|key: Major, minor: Major, volume: bool| #[trigger] delta_base.at(key, minor, volume) ==
                sum(line_delta_seq_view(line_changes.take(index as int)), |change: (LineitemRow, int)|
                    crate::contributions::line_effect(before.parts, before.suppliers, before.customers, before.orders, change.0, key, minor, change.1, volume)),
        decreases delta.len() - index,
    {
        let ghost processed = line_changes.take(index as int);
        let change = delta[index];
        proof { model::lemma_runtime_access(*before); }
        line_contribution(&before.parts, &before.suppliers, &before.customers, &before.orders, change.row, change.sign as i128, &mut delta_base);
        proof {
            assert forall|key: Major, minor: Major, volume: bool| #[trigger] delta_base.at(key, minor, volume) ==
                sum(line_delta_seq_view(line_changes.take(index as int + 1)), |change: (LineitemRow, int)|
                    crate::contributions::line_effect(before.parts, before.suppliers, before.customers, before.orders, change.0, key, minor, change.1, volume)) by {
                lemma_mapped_prefix_step(line_changes, |d: LineDelta| (line_input_view(d.row), d.sign as int), index as int, |change: (LineitemRow, int)|
                    crate::contributions::line_effect(before.parts, before.suppliers, before.customers, before.orders, change.0, key, minor, change.1, volume));
            }
        }
        index += 1;
    }
    index = 0;
    let ghost orders_rows_input = orders_rows@;
    let ghost before_orders = delta_base;
    proof { reveal(crate::contributions::rescan_effect); }
    while index < orders_rows.len()
        invariant index <= orders_rows.len(), delta_base.valid(), model::runtime_valid(*before), parts.valid(), suppliers.valid(), customers.valid(), orders.valid(),
            orders_rows_input == orders_rows@,
            forall|key: Major, minor: Major, measure: bool| #[trigger] delta_base.at(key, minor, measure) == before_orders.at(key, minor, measure)
                + crate::contributions::rescan_effect(orders_rows_input.take(index as int), *required_orders, *required_parts, *required_suppliers, 0,
                    *parts, *suppliers, *customers, *orders, *before, key, minor, measure),
        decreases orders_rows.len() - index,
    {
        let ghost processed = orders_rows_input.take(index as int);
        let line = orders_rows[index];
        proof { model::lemma_runtime_access(*before); }
        if required_orders.contains(line.l_orderkey) {
            line_contribution(parts, suppliers, customers, orders, line, 1, &mut delta_base);
            line_contribution(&before.parts, &before.suppliers, &before.customers, &before.orders, line, -1, &mut delta_base);
        }
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] delta_base.at(key, minor, measure) == before_orders.at(key, minor, measure)
                + crate::contributions::rescan_effect(orders_rows_input.take(index as int + 1), *required_orders, *required_parts, *required_suppliers, 0,
                    *parts, *suppliers, *customers, *orders, *before, key, minor, measure) by {
                reveal(crate::contributions::rescan_effect);
                lemma_sum_take(orders_rows_input, index as int, |line: LineInput|
                    crate::contributions::rescan_weight(line, *required_orders, *required_parts, *required_suppliers, 0,
                        *parts, *suppliers, *customers, *orders, *before, key, minor, measure));
            }
        }
        index += 1;
    }
    index = 0;
    let ghost parts_rows_input = parts_rows@;
    let ghost before_parts = delta_base;
    proof { reveal(crate::contributions::rescan_effect); }
    while index < parts_rows.len()
        invariant index <= parts_rows.len(), delta_base.valid(), model::runtime_valid(*before), parts.valid(), suppliers.valid(), customers.valid(), orders.valid(),
            parts_rows_input == parts_rows@,
            forall|key: Major, minor: Major, measure: bool| #[trigger] delta_base.at(key, minor, measure) == before_parts.at(key, minor, measure)
                + crate::contributions::rescan_effect(parts_rows_input.take(index as int), *required_orders, *required_parts, *required_suppliers, 1,
                    *parts, *suppliers, *customers, *orders, *before, key, minor, measure),
        decreases parts_rows.len() - index,
    {
        let ghost processed = parts_rows_input.take(index as int);
        let line = parts_rows[index];
        proof { model::lemma_runtime_access(*before); }
        if required_parts.contains(line.l_partkey) && !required_orders.contains(line.l_orderkey) {
            line_contribution(parts, suppliers, customers, orders, line, 1, &mut delta_base);
            line_contribution(&before.parts, &before.suppliers, &before.customers, &before.orders, line, -1, &mut delta_base);
        }
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] delta_base.at(key, minor, measure) == before_parts.at(key, minor, measure)
                + crate::contributions::rescan_effect(parts_rows_input.take(index as int + 1), *required_orders, *required_parts, *required_suppliers, 1,
                    *parts, *suppliers, *customers, *orders, *before, key, minor, measure) by {
                reveal(crate::contributions::rescan_effect);
                lemma_sum_take(parts_rows_input, index as int, |line: LineInput|
                    crate::contributions::rescan_weight(line, *required_orders, *required_parts, *required_suppliers, 1,
                        *parts, *suppliers, *customers, *orders, *before, key, minor, measure));
            }
        }
        index += 1;
    }
    index = 0;
    let ghost suppliers_rows_input = suppliers_rows@;
    let ghost before_suppliers = delta_base;
    proof { reveal(crate::contributions::rescan_effect); }
    while index < suppliers_rows.len()
        invariant index <= suppliers_rows.len(), delta_base.valid(), model::runtime_valid(*before), parts.valid(), suppliers.valid(), customers.valid(), orders.valid(),
            suppliers_rows_input == suppliers_rows@,
            forall|key: Major, minor: Major, measure: bool| #[trigger] delta_base.at(key, minor, measure) == before_suppliers.at(key, minor, measure)
                + crate::contributions::rescan_effect(suppliers_rows_input.take(index as int), *required_orders, *required_parts, *required_suppliers, 2,
                    *parts, *suppliers, *customers, *orders, *before, key, minor, measure),
        decreases suppliers_rows.len() - index,
    {
        let ghost processed = suppliers_rows_input.take(index as int);
        let line = suppliers_rows[index];
        proof { model::lemma_runtime_access(*before); }
        if required_suppliers.contains(line.l_suppkey) && !required_orders.contains(line.l_orderkey) && !required_parts.contains(line.l_partkey) {
            line_contribution(parts, suppliers, customers, orders, line, 1, &mut delta_base);
            line_contribution(&before.parts, &before.suppliers, &before.customers, &before.orders, line, -1, &mut delta_base);
        }
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] delta_base.at(key, minor, measure) == before_suppliers.at(key, minor, measure)
                + crate::contributions::rescan_effect(suppliers_rows_input.take(index as int + 1), *required_orders, *required_parts, *required_suppliers, 2,
                    *parts, *suppliers, *customers, *orders, *before, key, minor, measure) by {
                reveal(crate::contributions::rescan_effect);
                lemma_sum_take(suppliers_rows_input, index as int, |line: LineInput|
                    crate::contributions::rescan_weight(line, *required_orders, *required_parts, *required_suppliers, 2,
                        *parts, *suppliers, *customers, *orders, *before, key, minor, measure));
            }
        }
        index += 1;
    }
    let result = delta_base.finish();
    proof {
        assert(line_changes.take(line_changes.len() as int) =~= line_changes);
        assert(orders_rows_input.take(orders_rows_input.len() as int) =~= orders_rows_input);
        assert(parts_rows_input.take(parts_rows_input.len() as int) =~= parts_rows_input);
        assert(suppliers_rows_input.take(suppliers_rows_input.len() as int) =~= suppliers_rows_input);
    }
    result
}

/// The nations of the regions that a transaction changes, in the new nations-by-region index, are changed too.
pub fn include_changed_region_nations(delta: &Vec<RegionDelta>, nations_by_region: &Table, changed: Keys,
    Ghost(nation_rows): Ghost<Seq<NationDelta>>) -> (result: Keys)
    requires nations_by_region.valid(), changed.valid(),
        forall|offset: int| 0 <= offset < nation_rows.len() ==> changed.has((#[trigger] nation_rows[offset]).row.n_nationkey),
    ensures result.valid(),
        forall|key: i64| changed.has(key) ==> #[trigger] result.has(key),
        forall|offset: int| 0 <= offset < nation_rows.len() ==> result.has((#[trigger] nation_rows[offset]).row.n_nationkey),
        forall|earlier: int, offset: int| 0 <= earlier < delta@.len()
            && 0 <= offset < nations_by_region.rows((delta@[earlier].row.r_regionkey, 0, 0)).len() ==>
            result.has((#[trigger] nations_by_region.rows((delta@[earlier].row.r_regionkey, 0, 0))[offset]).first),
{
    let ghost original = changed;
    let mut changed_nations = changed;
    let mut index = 0;
    while index < delta.len()
        invariant index <= delta.len(), nations_by_region.valid(), changed_nations.valid(),
            forall|key: i64| original.has(key) ==> #[trigger] changed_nations.has(key),
            forall|offset: int| 0 <= offset < nation_rows.len() ==> changed_nations.has((#[trigger] nation_rows[offset]).row.n_nationkey),
            forall|earlier: int, offset: int| 0 <= earlier < index
                && 0 <= offset < nations_by_region.rows((delta@[earlier].row.r_regionkey, 0, 0)).len() ==>
                changed_nations.has((#[trigger] nations_by_region.rows((delta@[earlier].row.r_regionkey, 0, 0))[offset]).first),
        decreases delta.len() - index,
    {
        let ghost previous = changed_nations;
        include_region_nations(nations_by_region, delta[index].row.r_regionkey, &mut changed_nations);
        proof {
            assert forall|key: i64| original.has(key) implies changed_nations.has(key) by { assert(previous.has(key)); }
        }
        index += 1;
    }
    changed_nations
}

}
