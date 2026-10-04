use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::bag::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::storage::*;
use crate::values::*;

verus! {

pub proof fn lemma_count_patch_shape(previous: Layers<Counts>, changes: Sparse<Counts>, next: Layers<Counts>)
    requires
        forall|key: i64| #![trigger previous.at(key)] previous.at(key) matches Some(bucket) ==> counts_valid(bucket),
        forall|key: i64| #![trigger changes.at(key)] changes.at(key) matches Some(bucket) ==> counts_positive(bucket),
        forall|key: i64| #![trigger patched_counts_unique(previous, changes, key)] patched_counts_unique(previous, changes, key),
        forall|key: i64| #![trigger next.at(key)]
            next.at(key) == if changes.at(key).is_some() { normalize(changes.at(key)) } else { previous.at(key) },
    ensures forall|key: i64| #![trigger next.at(key)] next.at(key) matches Some(bucket) ==> counts_valid(bucket),
{
    assert forall|key: i64| #![trigger next.at(key)] next.at(key) matches Some(bucket) ==> counts_valid(bucket) by {
        assert(patched_counts_unique(previous, changes, key));
        if let Some(bucket) = changes.at(key) { assert(counts_positive(bucket)); }
    }
}

pub proof fn lemma_name_patch_shape(previous: Layers<Names>, changes: Sparse<Names>, next: Layers<Names>)
    requires
        forall|key: i64| #![trigger previous.at(key)] previous.at(key) matches Some(bucket) ==> names_valid(bucket),
        forall|key: i64| #![trigger changes.at(key)] changes.at(key) matches Some(bucket) ==> names_positive(bucket),
        forall|key: i64| #![trigger patched_names_unique(previous, changes, key)] patched_names_unique(previous, changes, key),
        forall|key: i64| #![trigger next.at(key)]
            next.at(key) == if changes.at(key).is_some() { normalize(changes.at(key)) } else { previous.at(key) },
    ensures forall|key: i64| #![trigger next.at(key)] next.at(key) matches Some(bucket) ==> names_valid(bucket),
{
    assert forall|key: i64| #![trigger next.at(key)] next.at(key) matches Some(bucket) ==> names_valid(bucket) by {
        assert(patched_names_unique(previous, changes, key));
        if let Some(bucket) = changes.at(key) { assert(names_positive(bucket)); }
    }
}

pub open spec fn nation_input_weight(key: i64, name: Seq<u8>) -> spec_fn(NationInput) -> int {
    |row: NationInput| if row.n_nationkey == key && row.n_name@ == name { 1int } else { 0int }
}

pub open spec fn nation_signed_weight(key: i64, name: Seq<u8>) -> spec_fn(NationDelta) -> int {
    |change: NationDelta| if change.row.n_nationkey == key && change.row.n_name@ == name { change.sign as int } else { 0int }
}

pub open spec fn nation_rows_count(rows: Seq<NationInput>, key: i64, name: Seq<u8>) -> int {
    sum(rows, nation_input_weight(key, name))
}

pub open spec fn nation_delta_count(rows: Seq<NationDelta>, key: i64, name: Seq<u8>) -> int {
    sum(rows, nation_signed_weight(key, name))
}

pub proof fn lemma_supplier_scan(db: Db, rows: Seq<SupplierInput>, key: i64, nation: i64)
    requires scan_suppliers_ok(db, rows),
    ensures supplier_count(db, key, nation) == sum(rows, |row: SupplierInput|
        if row.s_suppkey == key && row.s_nationkey == nation { 1int } else { 0int }),
{
    reveal(supplier_count);
    let weight = |row: SupplierRow| if row.s_suppkey == key && row.s_nationkey == nation { 1int } else { 0int };
    let projected = |row: SupplierInput| if row.s_suppkey == key && row.s_nationkey == nation { 1int } else { 0int };
    lemma_sum_map(rows, |row: SupplierInput| supplier_input_view(row), weight, projected);
    assert(sum(supplier_input_seq_view(rows), weight) == sum(db.supplier, weight));
}

pub proof fn lemma_customer_scan(db: Db, rows: Seq<CustomerInput>, key: i64, nation: i64)
    requires scan_customers_ok(db, rows),
    ensures customer_count(db, key, nation) == sum(rows, |row: CustomerInput|
        if row.c_custkey == key && row.c_nationkey == nation { 1int } else { 0int }),
{
    reveal(customer_count);
    let weight = |row: CustomerRow| if row.c_custkey == key && row.c_nationkey == nation { 1int } else { 0int };
    let projected = |row: CustomerInput| if row.c_custkey == key && row.c_nationkey == nation { 1int } else { 0int };
    lemma_sum_map(rows, |row: CustomerInput| customer_input_view(row), weight, projected);
    assert(sum(customer_input_seq_view(rows), weight) == sum(db.customer, weight));
}

pub proof fn lemma_order_scan(db: Db, rows: Seq<OrderInput>, key: i64, customer: i64)
    requires scan_relevant_orders_ok(db, rows),
    ensures order_count(db, key, customer) == sum(rows, |row: OrderInput|
        if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int }),
{
    reveal(order_count);
    let weight = |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int };
    let projected = |row: OrderInput| if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int };
    lemma_sum_map(rows, |row: OrderInput| order_input_view(row), weight, projected);
    assert(sum(order_input_seq_view(rows), weight)
        == sum(db.orders, |row: OrdersRow| if scan_relevant_orders_exists_1(db, row) { weight(row) } else { 0int }));
    lemma_sum_ext_all(db.orders,
        |row: OrdersRow| if scan_relevant_orders_exists_1(db, row) { weight(row) } else { 0int },
        |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer && scan_relevant_orders_exists_1(db, row) { 1int } else { 0int });
}

pub proof fn lemma_order_rescan(db: Db, keys: Seq<i64>, rows: Seq<OrderInput>, key: i64, customer: i64)
    requires rescan_relevant_orders_for_keys_ok(db, keys, rows), keys.contains(key),
    ensures order_count(db, key, customer) == sum(rows, |row: OrderInput|
        if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int }),
{
    broadcast use vstd::seq_lib::group_seq_properties;
    reveal(order_count);
    let position = choose|position: int| #![trigger keys[position]] 0 <= position < keys.len() && keys[position] == key;
    assert(seq_view_i64(keys)[position] == key as int);
    assert(seq_view_i64(keys).contains(key as int));
    let weight = |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int };
    let projected = |row: OrderInput| if row.o_orderkey == key && row.o_custkey == customer { 1int } else { 0int };
    lemma_sum_map(rows, |row: OrderInput| order_input_view(row), weight, projected);
    let filtered = |row: OrdersRow| if rescan_relevant_orders_for_keys_exists_1(db, row)
        && seq_view_i64(keys).contains(row.o_orderkey) { weight(row) } else { 0int };
    let relevant = |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer
        && scan_relevant_orders_exists_1(db, row) { 1int } else { 0int };
    assert forall|row: OrdersRow| #[trigger] filtered(row) == relevant(row) by {
        assert(rescan_relevant_orders_for_keys_exists_1(db, row) == scan_relevant_orders_exists_1(db, row));
    }
    lemma_sum_ext_all(db.orders, filtered, relevant);
    assert(sum(order_input_seq_view(rows), weight) == sum(db.orders, filtered));
}

pub open spec fn window_key_predicate(key: int) -> spec_fn(LineitemRow) -> bool {
    |line: LineitemRow| line.l_orderkey == key && 9131 <= line.l_shipdate <= 9861
}

pub proof fn lemma_relevant_support(db: Db, order: OrdersRow)
    ensures scan_relevant_orders_exists_1(db, order)
        == (predicate_count(db.lineitem, window_key_predicate(order.o_orderkey)) > 0),
{
    lemma_predicate_support(db.lineitem, window_key_predicate(order.o_orderkey));
}

pub proof fn lemma_order_untouched(db: Db, db2: Db, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>, key: i64, customer: i64)
    requires bag_step(db.lineitem, db2.lineitem, lines), bag_step(db.orders, db2.orders, orders),
        forall|position: int| #![trigger lines[position]] 0 <= position < lines.len()
            && 9131 <= lines[position].0.l_shipdate <= 9861 ==> lines[position].0.l_orderkey != key,
        forall|position: int| #![trigger orders[position]] 0 <= position < orders.len() ==> orders[position].0.o_orderkey != key,
    ensures order_count(db2, key, customer) == order_count(db, key, customer),
{
    reveal(order_count);
    let predicate = window_key_predicate(key as int);
    let line_weight = predicate_weight(predicate);
    let signed_lines = |change: (LineitemRow, int)| change.1 * line_weight(change.0);
    lemma_sum_zero(lines, signed_lines);
    assert(sum(db2.lineitem, line_weight) == sum(db.lineitem, line_weight) + sum(lines, signed_lines));
    assert(predicate_count(db2.lineitem, predicate) == predicate_count(db.lineitem, predicate));
    let before_weight = |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer
        && scan_relevant_orders_exists_1(db, row) { 1int } else { 0int };
    let after_weight = |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer
        && scan_relevant_orders_exists_1(db2, row) { 1int } else { 0int };
    assert forall|row: OrdersRow| #[trigger] after_weight(row) == before_weight(row) by {
        if row.o_orderkey == key {
            lemma_relevant_support(db, row);
            lemma_relevant_support(db2, row);
            assert(window_key_predicate(row.o_orderkey) == predicate);
        }
    }
    lemma_sum_ext_all(db2.orders, after_weight, before_weight);
    let signed_orders = |change: (OrdersRow, int)| change.1 * before_weight(change.0);
    lemma_sum_zero(orders, signed_orders);
    assert(sum(db2.orders, before_weight) == sum(db.orders, before_weight) + sum(orders, signed_orders));
}

pub proof fn lemma_nation_scan(db: Db, rows: Seq<NationInput>, key: i64, name: Seq<u8>)
    requires scan_nations_ok(db, rows),
    ensures nation_count(db, key, name) == nation_rows_count(rows, key, name),
{
    reveal(nation_count);
    reveal(nation_rows_count);
    let weight = |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1int } else { 0int };
    let projected = nation_input_weight(key, name);
    lemma_sum_map(rows, |row: NationInput| nation_input_view(row), weight, projected);
    assert(sum(nation_input_seq_view(rows), weight) == sum(db.nation, weight));
    assert(projected =~= (|row: NationInput| if row.n_nationkey == key && row.n_name@ == name { 1int } else { 0int }));
    assert(nation_rows_count(rows, key, name) == sum(rows, projected));
    assert(nation_count(db, key, name) == sum(db.nation, weight));
}

pub proof fn lemma_count_install(previous: Layers<Counts>, changes: Sparse<Counts>, next: Layers<Counts>, key: i64, other: i64)
    requires next.at(key) == if changes.at(key).is_some() { normalize(changes.at(key)) } else { previous.at(key) },
    ensures dimension_value(next, key, other) == patched_count(previous, changes, key, other),
{
    if let Some(bucket) = changes.at(key) {
        if bucket.empty() {
            lemma_sum_empty(bucket.rows@, |row: (i64, i128)| if row.0 == other { row.1 as int } else { 0int });
        }
    }
}

pub proof fn lemma_name_install(previous: Layers<Names>, changes: Sparse<Names>, next: Layers<Names>, key: i64, name: Seq<u8>)
    requires next.at(key) == if changes.at(key).is_some() { normalize(changes.at(key)) } else { previous.at(key) },
    ensures nation_value(next, key, name) == patched_name(previous, changes, key, name),
{
    if let Some(bucket) = changes.at(key) {
        if bucket.empty() {
            lemma_sum_empty(bucket.rows@, |row: (Vec<u8>, i128)| if row.0@ == name { row.1 as int } else { 0int });
        }
    }
}

pub proof fn lemma_supplier_capture(db: Db, db2: Db, rows: Seq<SupplierDelta>, key: i64, nation: i64)
    requires bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(rows)),
    ensures supplier_count(db2, key, nation) == supplier_count(db, key, nation)
        + sum(rows, |row: SupplierDelta| if row.row.s_suppkey == key && row.row.s_nationkey == nation { row.sign as int } else { 0int }),
{
    reveal(supplier_count);
    let weight = |row: SupplierRow| if row.s_suppkey == key && row.s_nationkey == nation { 1int } else { 0int };
    let signed = |change: (SupplierRow, int)| change.1 * weight(change.0);
    let projected = |row: SupplierDelta| if row.row.s_suppkey == key && row.row.s_nationkey == nation { row.sign as int } else { 0int };
    lemma_sum_map(rows, |row: SupplierDelta| (supplier_input_view(row.row), row.sign as int), signed, projected);
    assert(sum(db2.supplier, weight) == sum(db.supplier, weight) + sum(supplier_delta_seq_view(rows), signed));
}

pub proof fn lemma_customer_capture(db: Db, db2: Db, rows: Seq<CustomerDelta>, key: i64, nation: i64)
    requires bag_step(db.customer, db2.customer, customer_delta_seq_view(rows)),
    ensures customer_count(db2, key, nation) == customer_count(db, key, nation)
        + sum(rows, |row: CustomerDelta| if row.row.c_custkey == key && row.row.c_nationkey == nation { row.sign as int } else { 0int }),
{
    reveal(customer_count);
    let weight = |row: CustomerRow| if row.c_custkey == key && row.c_nationkey == nation { 1int } else { 0int };
    let signed = |change: (CustomerRow, int)| change.1 * weight(change.0);
    let projected = |row: CustomerDelta| if row.row.c_custkey == key && row.row.c_nationkey == nation { row.sign as int } else { 0int };
    lemma_sum_map(rows, |row: CustomerDelta| (customer_input_view(row.row), row.sign as int), signed, projected);
    assert(sum(db2.customer, weight) == sum(db.customer, weight) + sum(customer_delta_seq_view(rows), signed));
}

pub proof fn lemma_nation_capture(db: Db, db2: Db, rows: Seq<NationDelta>, key: i64, name: Seq<u8>)
    requires bag_step(db.nation, db2.nation, nation_delta_seq_view(rows)),
    ensures nation_count(db2, key, name) == nation_count(db, key, name)
        + nation_delta_count(rows, key, name),
{
    reveal(nation_count);
    reveal(nation_delta_count);
    let weight = |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1int } else { 0int };
    let signed = |change: (NationRow, int)| change.1 * weight(change.0);
    let projected = nation_signed_weight(key, name);
    assert forall|row: NationDelta| #[trigger] projected(row)
        == signed((nation_input_view(row.row), row.sign as int)) by {
        assert(nation_input_view(row.row).n_name == row.row.n_name@);
    }
    lemma_sum_map(rows, |row: NationDelta| (nation_input_view(row.row), row.sign as int), signed, projected);
    assert(sum(db2.nation, weight) == sum(db.nation, weight) + sum(nation_delta_seq_view(rows), signed));
    assert(projected =~= (|change: NationDelta| if change.row.n_nationkey == key && change.row.n_name@ == name { change.sign as int } else { 0int }));
    assert(nation_delta_count(rows, key, name) == sum(rows, projected));
    assert(nation_count(db, key, name) == sum(db.nation, weight));
    assert(nation_count(db2, key, name) == sum(db2.nation, weight));
}

}
