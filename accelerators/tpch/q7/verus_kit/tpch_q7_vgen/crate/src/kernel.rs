#![allow(unused_imports)]
use vstd::prelude::*;
use verus_common::rat::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::values::*;
use crate::values::value;
use crate::model::*;
use crate::joins::*;
use crate::output::*;
use crate::query_proofs::*;
use crate::dimensions::*;
use crate::join_semantics::*;
use crate::domains::*;
use crate::construction::*;
use crate::directories::*;
use crate::expansion::*;
use crate::answer_semantics::*;
use crate::shipping_semantics::lemma_named_sql;
use crate::transaction::*;
use crate::incident_updates::*;
use crate::answer_updates::*;
use crate::answer_transactions::*;
use std::sync::Arc;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub struct State {
    db: Ghost<Db>,
    suppliers: Layers<Counts>,
    customers: Layers<Counts>,
    orders: Layers<Counts>,
    nations: Layers<Names>,
    reverse: Layers<Layers<i128>>,
    groups: Layers<Groups>,
    incident: Layers<Layers<i128>>,
    answers: Layers<Answers>,
}

impl State {
    pub closed spec fn db(self) -> Db {
        self.db@
    }

    pub closed spec fn reverse_has(self, customer: i64, order: i64) -> bool {
        member_value(self.reverse, customer, order)
    }

    #[verifier::type_invariant]
    pub closed spec fn valid(self) -> bool {
        &&& database_domain(self.db())
        &&& self.suppliers.valid() && self.customers.valid() && self.orders.valid()
        &&& self.nations.valid() && self.reverse.valid() && self.groups.valid()
        &&& self.incident.valid() && self.answers.valid()
        &&& forall|key: i64, other: i64| #![trigger dimension_value(self.suppliers, key, other)]
            dimension_value(self.suppliers, key, other) == supplier_count(self.db(), key, other)
        &&& forall|key: i64, other: i64| #![trigger dimension_value(self.customers, key, other)]
            dimension_value(self.customers, key, other) == customer_count(self.db(), key, other)
        &&& forall|key: i64, other: i64| #![trigger dimension_value(self.orders, key, other)]
            dimension_value(self.orders, key, other) == order_count(self.db(), key, other)
        &&& forall|key: i64, name: Seq<u8>| #![trigger nation_value(self.nations, key, name)]
            nation_value(self.nations, key, name) == nation_count(self.db(), key, name)
        &&& forall|first: i64, second: i64, year: i64, revenue: bool|
            #![trigger group_value(self.groups, first, second, year, revenue)]
            group_value(self.groups, first, second, year, revenue) == numeric_group(self.db(), first, second, year, revenue)
        &&& forall|first: Seq<u8>, second: Seq<u8>, year: i64|
            #![trigger answer_value(self.answers, first, second, year, false)]
            answer_value(self.answers, first, second, year, false)
                == group_count(self.db(), params(first, second), first, second, year as int)
        &&& forall|first: Seq<u8>, second: Seq<u8>, year: i64|
            #![trigger answer_value(self.answers, first, second, year, true)]
            answer_value(self.answers, first, second, year, true)
                == row_of(self.db(), params(first, second), first, second, year as int).revenue
        &&& forall|customer: i64, order: i64| #![trigger member_value(self.reverse, customer, order)]
            member_value(self.reverse, customer, order) == (order_count(self.db(), order, customer) > 0)
        &&& forall|second: i64, first: i64| #![trigger member_value(self.incident, second, first)]
            member_value(self.incident, second, first) ==
                (numeric_group(self.db(), first, second, 1995, false) > 0
                || numeric_group(self.db(), first, second, 1996, false) > 0)
        &&& forall|key: i64| #![trigger self.suppliers.at(key)]
            self.suppliers.at(key) matches Some(bucket) ==> counts_valid(bucket)
        &&& forall|key: i64| #![trigger self.customers.at(key)]
            self.customers.at(key) matches Some(bucket) ==> counts_valid(bucket)
        &&& forall|key: i64| #![trigger self.orders.at(key)]
            self.orders.at(key) matches Some(bucket) ==> counts_valid(bucket)
        &&& forall|key: i64| #![trigger self.nations.at(key)]
            self.nations.at(key) matches Some(bucket) ==> names_valid(bucket)
        &&& forall|key: i64| #![trigger self.groups.at(key)]
            self.groups.at(key) matches Some(bucket) ==> groups_valid(bucket)
        &&& forall|key: i64| #![trigger self.answers.at(key)]
            self.answers.at(key) matches Some(bucket) ==> answers_valid(bucket, key)
        &&& forall|key: i64| #![trigger self.reverse.at(key)]
            self.reverse.at(key) matches Some(bucket) ==> bucket.valid()
        &&& forall|key: i64| #![trigger self.incident.at(key)]
            self.incident.at(key) matches Some(bucket) ==> bucket.valid()
    }
}

pub fn build(
    suppliers: &[SupplierInput],
    customers: &[CustomerInput],
    nations: Vec<NationInput>,
    relevant_orders: &[OrderInput],
    window_lines: &[LineInput],
    Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires
        scan_suppliers_ok(db, suppliers@),
        scan_customers_ok(db, customers@),
        scan_nations_ok(db, nations@),
        scan_relevant_orders_ok(db, relevant_orders@),
        scan_window_lines_ok(db, window_lines@),
    ensures
        r matches Some(s) ==> s.db() == db,
{
    hide(group_count);
    hide(row_of);
    let ghost scanned_suppliers = suppliers@;
    let ghost scanned_customers = customers@;
    let ghost scanned_orders = relevant_orders@;
    let ghost scanned_nations = nations@;
    proof { lemma_build_domain(db, scanned_suppliers, scanned_customers, scanned_nations, window_lines@); }
    let empty_counts = Layers::new(Sparse::new());
    let empty_names = Layers::new(Sparse::new());
    let mut supplier_rows: Sparse<Counts> = Sparse::new();
    let mut customer_rows: Sparse<Counts> = Sparse::new();
    let mut nation_rows: Sparse<Names> = Sparse::new();
    let mut group_rows = Sparse::new();
    let mut index = 0;
    while index < suppliers.len()
        invariant index <= suppliers@.len(), supplier_rows.valid(),
            forall|key: i64| #![trigger patched_counts_unique(empty_counts, supplier_rows, key)]
                patched_counts_unique(empty_counts, supplier_rows, key),
            forall|key: i64| #![trigger patched_counts_positive(empty_counts, supplier_rows, key)]
                patched_counts_positive(empty_counts, supplier_rows, key),
            forall|key: i64| #![trigger empty_counts.at(key)] empty_counts.at(key).is_none(),
            forall|key: i64, other: i64| #![trigger sparse_count(supplier_rows, key, other)]
                sparse_count(supplier_rows, key, other)
                == verus_common::sums::sum(suppliers@.take(index as int), |row: SupplierInput|
                    if row.s_suppkey == key && row.s_nationkey == other { 1 } else { 0 }),
        decreases suppliers@.len() - index,
    {
        let ghost processed = index as int;
        let ghost prefix_rows = supplier_rows;
        value!(count_patch(&empty_counts, &mut supplier_rows, suppliers[index].s_suppkey, suppliers[index].s_nationkey, 1));
        proof {
            assert forall|key: i64, other: i64| #![trigger sparse_count(supplier_rows, key, other)]
                sparse_count(supplier_rows, key, other) == verus_common::sums::sum(suppliers@.take(index as int + 1),
                    |row: SupplierInput| if row.s_suppkey == key && row.s_nationkey == other { 1int } else { 0int }) by {
                assert(patched_count(empty_counts, supplier_rows, key, other) == sparse_count(supplier_rows, key, other));
                assert(patched_count(empty_counts, supplier_rows, key, other) == patched_count(empty_counts, prefix_rows, key, other)
                    + if key == suppliers@[index as int].s_suppkey && other == suppliers@[index as int].s_nationkey { 1int } else { 0int });
                assert(patched_count(empty_counts, prefix_rows, key, other) == sparse_count(prefix_rows, key, other));
                assert(sparse_count(prefix_rows, key, other) == verus_common::sums::sum(suppliers@.take(index as int),
                    |row: SupplierInput| if row.s_suppkey == key && row.s_nationkey == other { 1int } else { 0int }));
                verus_common::sums::lemma_sum_take(suppliers@, index as int,
                    |row: SupplierInput| if row.s_suppkey == key && row.s_nationkey == other { 1int } else { 0int });
            }
        }
        index += 1;
    }
    let mut index = 0;
    while index < customers.len()
        invariant index <= customers@.len(), customer_rows.valid(),
            forall|key: i64| #![trigger patched_counts_unique(empty_counts, customer_rows, key)]
                patched_counts_unique(empty_counts, customer_rows, key),
            forall|key: i64| #![trigger patched_counts_positive(empty_counts, customer_rows, key)]
                patched_counts_positive(empty_counts, customer_rows, key),
            forall|key: i64| #![trigger empty_counts.at(key)] empty_counts.at(key).is_none(),
            forall|key: i64, other: i64| #![trigger sparse_count(customer_rows, key, other)]
                sparse_count(customer_rows, key, other)
                == verus_common::sums::sum(customers@.take(index as int), |row: CustomerInput|
                    if row.c_custkey == key && row.c_nationkey == other { 1 } else { 0 }),
        decreases customers@.len() - index,
    {
        let ghost processed = index as int;
        let ghost prefix_rows = customer_rows;
        value!(count_patch(&empty_counts, &mut customer_rows, customers[index].c_custkey, customers[index].c_nationkey, 1));
        proof {
            assert forall|key: i64, other: i64| #![trigger sparse_count(customer_rows, key, other)]
                sparse_count(customer_rows, key, other) == verus_common::sums::sum(customers@.take(index as int + 1),
                    |row: CustomerInput| if row.c_custkey == key && row.c_nationkey == other { 1int } else { 0int }) by {
                assert(patched_count(empty_counts, customer_rows, key, other) == sparse_count(customer_rows, key, other));
                assert(patched_count(empty_counts, customer_rows, key, other) == patched_count(empty_counts, prefix_rows, key, other)
                    + if key == customers@[index as int].c_custkey && other == customers@[index as int].c_nationkey { 1int } else { 0int });
                assert(patched_count(empty_counts, prefix_rows, key, other) == sparse_count(prefix_rows, key, other));
                assert(sparse_count(prefix_rows, key, other) == verus_common::sums::sum(customers@.take(index as int),
                    |row: CustomerInput| if row.c_custkey == key && row.c_nationkey == other { 1int } else { 0int }));
                verus_common::sums::lemma_sum_take(customers@, index as int,
                    |row: CustomerInput| if row.c_custkey == key && row.c_nationkey == other { 1int } else { 0int });
            }
        }
        index += 1;
    }
    let (order_rows, reverse_rows) = value!(build_orders(relevant_orders));
    let mut index = 0;
    while index < nations.len()
        invariant index <= nations@.len(), nation_rows.valid(),
            forall|key: i64| #![trigger patched_names_unique(empty_names, nation_rows, key)]
                patched_names_unique(empty_names, nation_rows, key),
            forall|key: i64| #![trigger patched_names_positive(empty_names, nation_rows, key)]
                patched_names_positive(empty_names, nation_rows, key),
            forall|key: i64| #![trigger empty_names.at(key)] empty_names.at(key).is_none(),
            forall|key: i64, name: Seq<u8>| #![trigger sparse_name(nation_rows, key, name)]
                sparse_name(nation_rows, key, name)
                == nation_rows_count(nations@.take(index as int), key, name),
        decreases nations@.len() - index,
    {
        let ghost processed = index as int;
        let ghost prefix_rows = nation_rows;
        value!(name_patch(&empty_names, &mut nation_rows, nations[index].n_nationkey, &nations[index].n_name, 1));
        proof {
            assert forall|key: i64, name: Seq<u8>| #![trigger sparse_name(nation_rows, key, name)]
                sparse_name(nation_rows, key, name) == nation_rows_count(nations@.take(index as int + 1), key, name) by {
                assert(patched_name(empty_names, nation_rows, key, name) == sparse_name(nation_rows, key, name));
                assert(patched_name(empty_names, nation_rows, key, name) == patched_name(empty_names, prefix_rows, key, name)
                    + if key == nations@[index as int].n_nationkey && name == nations@[index as int].n_name@ { 1int } else { 0int });
                assert(patched_name(empty_names, prefix_rows, key, name) == sparse_name(prefix_rows, key, name));
                assert(sparse_name(prefix_rows, key, name) == nation_rows_count(nations@.take(index as int), key, name));
                verus_common::sums::lemma_sum_take(nations@, index as int, nation_input_weight(key, name));
            }
        }
        index += 1;
    }
    let suppliers = Layers::new(supplier_rows);
    let customers = Layers::new(customer_rows);
    let orders = Layers::new(order_rows);
    let nations = Layers::new(nation_rows);
    proof {
        assert forall|key: i64| #![trigger supplier_rows.at(key)]
            supplier_rows.at(key) matches Some(bucket) ==> counts_positive(bucket) by {
            assert(patched_counts_positive(empty_counts, supplier_rows, key));
        }
        assert forall|key: i64| #![trigger customer_rows.at(key)]
            customer_rows.at(key) matches Some(bucket) ==> counts_positive(bucket) by {
            assert(patched_counts_positive(empty_counts, customer_rows, key));
        }
        assert forall|key: i64| #![trigger nation_rows.at(key)]
            nation_rows.at(key) matches Some(bucket) ==> names_positive(bucket) by {
            assert(patched_names_positive(empty_names, nation_rows, key));
        }
        lemma_count_patch_shape(empty_counts, supplier_rows, suppliers);
        lemma_count_patch_shape(empty_counts, customer_rows, customers);
        lemma_count_patch_shape(empty_counts, order_rows, orders);
        lemma_name_patch_shape(empty_names, nation_rows, nations);
        assert forall|key: i64, other: i64| #![trigger dimension_value(suppliers, key, other)]
            dimension_value(suppliers, key, other) == supplier_count(db, key, other) by {
            lemma_count_install(empty_counts, supplier_rows, suppliers, key, other);
            assert(scanned_suppliers.take(scanned_suppliers.len() as int) == scanned_suppliers);
            lemma_supplier_scan(db, scanned_suppliers, key, other);
            assert(patched_count(empty_counts, supplier_rows, key, other) == sparse_count(supplier_rows, key, other));
        }
        assert forall|key: i64, other: i64| #![trigger dimension_value(customers, key, other)]
            dimension_value(customers, key, other) == customer_count(db, key, other) by {
            lemma_count_install(empty_counts, customer_rows, customers, key, other);
            assert(scanned_customers.take(scanned_customers.len() as int) == scanned_customers);
            lemma_customer_scan(db, scanned_customers, key, other);
            assert(patched_count(empty_counts, customer_rows, key, other) == sparse_count(customer_rows, key, other));
        }
        assert forall|key: i64, other: i64| #![trigger dimension_value(orders, key, other)]
            dimension_value(orders, key, other) == order_count(db, key, other) by {
            lemma_count_install(empty_counts, order_rows, orders, key, other);
            assert(scanned_orders.take(scanned_orders.len() as int) == scanned_orders);
            lemma_order_scan(db, scanned_orders, key, other);
            assert(patched_count(empty_counts, order_rows, key, other) == sparse_count(order_rows, key, other));
        }
        assert forall|key: i64, name: Seq<u8>| #![trigger nation_value(nations, key, name)]
            nation_value(nations, key, name) == nation_count(db, key, name) by {
            lemma_name_install(empty_names, nation_rows, nations, key, name);
            assert(scanned_nations.take(scanned_nations.len() as int) == scanned_nations);
            lemma_nation_scan(db, scanned_nations, key, name);
            assert(patched_name(empty_names, nation_rows, key, name) == sparse_name(nation_rows, key, name));
            assert(sparse_name(nation_rows, key, name) == nation_rows_count(scanned_nations, key, name));
            assert(nation_count(db, key, name) == nation_rows_count(scanned_nations, key, name));
        }
    }
    let mut index = 0;
    while index < window_lines.len()
        invariant index <= window_lines@.len(), group_rows.valid(), suppliers.valid(), customers.valid(), orders.valid(),
            sparse_groups_shape(group_rows),
            forall|first: i64, second: i64, year: i64, revenue: bool|
                #![trigger sparse_group(group_rows, first, second, year, revenue)]
                sparse_group(group_rows, first, second, year, revenue)
                == verus_common::sums::sum(window_lines@.take(index as int), |line: LineInput|
                    line_weight(suppliers, customers, orders, line, first, second, year, revenue)),
        decreases window_lines@.len() - index,
    {
        let ghost processed = index as int;
        let line = window_lines[index];
        let year = if line.l_shipdate <= 9495 { 1995 } else { 1996 };
        let volume = value!((line.l_extendedprice as i128).checked_mul(100i128 - line.l_discount as i128));
        let ghost prefix_groups = group_rows;
        value!(line_contribution(&suppliers, &customers, &orders, &mut group_rows,
            line.l_orderkey, line.l_suppkey, year, volume, 1));
        proof {
            reveal(line_weight);
            assert forall|first: i64, second: i64, target_year: i64, measure: bool|
                #![trigger sparse_group(group_rows, first, second, target_year, measure)]
                sparse_group(group_rows, first, second, target_year, measure)
                == verus_common::sums::sum(window_lines@.take(index as int + 1), |line: LineInput|
                    line_weight(suppliers, customers, orders, line, first, second, target_year, measure)) by {
                assert(sparse_group(group_rows, first, second, target_year, measure)
                    == sparse_group(prefix_groups, first, second, target_year, measure)
                        + line_weight(suppliers, customers, orders, line, first, second, target_year, measure));
                verus_common::sums::lemma_sum_take(window_lines@, index as int, |line: LineInput|
                    line_weight(suppliers, customers, orders, line, first, second, target_year, measure));
            }
        }
        index += 1;
    }
    if !positive_groups(&group_rows) { return None; }
    let incident = value!(build_incident(&group_rows));
    proof {
        assert(window_lines@.take(window_lines@.len() as int) == window_lines@);
        assert forall|first: i64, second: i64, year: i64, revenue: bool|
            #![trigger sparse_group(group_rows, first, second, year, revenue)]
            sparse_group(group_rows, first, second, year, revenue) == numeric_group(db, first, second, year, revenue) by {
            lemma_build_groups(db, scanned_orders, window_lines@, suppliers, customers, orders, first, second, year, revenue);
        }
    }
    let answers = value!(build_answers(&group_rows, &nations));
    proof {
        assert forall|first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool|
            #![trigger answer_value(answers, first, second, year, revenue)]
            answer_value(answers, first, second, year, revenue) == if revenue {
                row_of(db, params(first, second), first, second, year as int).revenue
            } else { group_count(db, params(first, second), first, second, year as int) } by {
            lemma_expansion_named(db, group_rows, nations, first, second, year, revenue);
            lemma_named_sql(db, first, second, year, revenue);
        }
    }
    let groups = Layers::new(group_rows);
    proof { lemma_group_layer_meaning(db, group_rows, groups); lemma_group_layer_shape(group_rows, groups); }
    let reverse = Layers::new(reverse_rows);
    proof {
        assert forall|customer: i64, order: i64| #![trigger member_value(reverse, customer, order)]
            member_value(reverse, customer, order) == (order_count(db, order, customer) > 0) by {
            lemma_reverse_install(reverse_rows, reverse, customer, order);
            lemma_order_scan(db, scanned_orders, order, customer);
        }
        assert forall|second: i64, first: i64| #![trigger member_value(incident, second, first)]
            member_value(incident, second, first)
                == (numeric_group(db, first, second, 1995, false) > 0
                    || numeric_group(db, first, second, 1996, false) > 0) by {
            assert(sparse_group(group_rows, first, second, 1995, false) == numeric_group(db, first, second, 1995, false));
            assert(sparse_group(group_rows, first, second, 1996, false) == numeric_group(db, first, second, 1996, false));
        }
    }
    Some(State { db: Ghost(db), suppliers, customers, orders, nations,
        reverse, groups,
        incident, answers })
}

pub fn execute(
    s: &State,
    p1: &[u8],
    p2: &[u8],
) -> (r: Option<Vec<(Vec<u8>, Vec<u8>, i64, i128)>>)
    ensures
        r matches Some(out) ==> query_result(s.db(), params(p1@, p2@), out_view(out@)),
{
    hide(query_result);
    proof { use_type_invariant(s); }
    let mut out = Vec::new();
    if verus_common::bytes::bytes_lt_exec(p2, p1) {
        emit_pair(&s.answers, p2, p1, &mut out);
        emit_pair(&s.answers, p1, p2, &mut out);
    } else {
        emit_pair(&s.answers, p1, p2, &mut out);
        if !bytes_equal(p1, p2) { emit_pair(&s.answers, p2, p1, &mut out); }
    }
    proof {
        reveal(ordered_output);
        assert(out_view(out@) == ordered_output(s.answers, p1@, p2@));
        lemma_cached_query(s.db(), s.answers, p1@, p2@);
    }
    Some(out)
}

pub fn rescan_relevant_orders_for_keys_keys(
    s: &State,
    customer_delta: &[CustomerDelta],
    lineitem_delta: &[LineDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrderDelta],
    supplier_delta: &[SupplierDelta],
) -> (r: Vec<i64>)
{
    touched_orders(lineitem_delta, orders_delta)
}

pub fn rescan_window_lines_for_orders_keys(
    s: &State,
    customer_delta: &[CustomerDelta],
    lineitem_delta: &[LineDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrderDelta],
    supplier_delta: &[SupplierDelta],
) -> (r: Vec<i64>)
    ensures r@.no_duplicates(),
        forall|position: int| #![trigger orders_delta@[position]] 0 <= position < orders_delta@.len() ==>
            r@.contains(orders_delta@[position].o_orderkey),
        forall|position: int| #![trigger lineitem_delta@[position]] 0 <= position < lineitem_delta@.len()
            && (lineitem_delta@[position].ship_1995 != 0 || lineitem_delta@[position].ship_1996 != 0) ==>
                r@.contains(lineitem_delta@[position].l_orderkey),
        forall|position: int, order: i64|
            #![trigger s.reverse_has(customer_delta@[position].row.c_custkey, order)]
            0 <= position < customer_delta@.len()
                && s.reverse_has(customer_delta@[position].row.c_custkey, order) ==> r@.contains(order),
{
    proof { use_type_invariant(s); }
    let mut keys = touched_orders(lineitem_delta, orders_delta);
    let mut index = 0;
    while index < customer_delta.len()
        invariant index <= customer_delta@.len(), keys@.no_duplicates(),
            forall|position: int| #![trigger orders_delta@[position]] 0 <= position < orders_delta@.len() ==>
                keys@.contains(orders_delta@[position].o_orderkey),
            forall|position: int| #![trigger lineitem_delta@[position]] 0 <= position < lineitem_delta@.len()
                && (lineitem_delta@[position].ship_1995 != 0 || lineitem_delta@[position].ship_1996 != 0) ==>
                    keys@.contains(lineitem_delta@[position].l_orderkey),
            forall|position: int, order: i64|
                #![trigger member_value(s.reverse, customer_delta@[position].row.c_custkey, order)]
                0 <= position < index && member_value(s.reverse, customer_delta@[position].row.c_custkey, order)
                    ==> keys@.contains(order),
        decreases customer_delta@.len() - index,
    {
        let ghost processed = index as int;
        if let Some(bucket) = s.reverse.get(customer_delta[index].row.c_custkey) {
            let order_keys = bucket.keys();
            let mut offset = 0;
            while offset < order_keys.len()
                invariant offset <= order_keys@.len(), index < customer_delta@.len(), keys@.no_duplicates(),
                    s.reverse.at(customer_delta@[index as int].row.c_custkey) == Some(*bucket),
                    forall|order: i64| #![trigger bucket.at(order)] bucket.at(order).is_some() == order_keys@.contains(order),
                    forall|position: int| #![trigger order_keys@[position]] 0 <= position < offset
                        && (bucket.at(order_keys@[position]) matches Some(value) && value > 0) ==>
                            keys@.contains(order_keys@[position]),
                    forall|position: int| #![trigger orders_delta@[position]] 0 <= position < orders_delta@.len() ==>
                        keys@.contains(orders_delta@[position].o_orderkey),
                    forall|position: int| #![trigger lineitem_delta@[position]] 0 <= position < lineitem_delta@.len()
                        && (lineitem_delta@[position].ship_1995 != 0 || lineitem_delta@[position].ship_1996 != 0) ==>
                            keys@.contains(lineitem_delta@[position].l_orderkey),
                    forall|position: int, order: i64|
                        #![trigger member_value(s.reverse, customer_delta@[position].row.c_custkey, order)]
                        0 <= position < index && member_value(s.reverse, customer_delta@[position].row.c_custkey, order)
                            ==> keys@.contains(order),
                decreases order_keys@.len() - offset,
            {
                let key = order_keys[offset];
                if let Some(present) = bucket.get(key) { if *present > 0 { add_key(&mut keys, key); } }
                offset += 1;
            }
            proof {
                assert forall|order: i64|
                    #![trigger member_value(s.reverse, customer_delta@[index as int].row.c_custkey, order)]
                    member_value(s.reverse, customer_delta@[index as int].row.c_custkey, order)
                        implies keys@.contains(order) by {
                    assert(bucket.at(order).is_some());
                    assert(order_keys@.contains(order));
                    let position = choose|position: int| #![trigger order_keys@[position]]
                        0 <= position < order_keys@.len() && order_keys@[position] == order;
                    assert(bucket.at(order_keys@[position]) matches Some(value) && value > 0);
                    assert(keys@.contains(order_keys@[position]));
                }
            }
        }
        index += 1;
    }
    keys
}

pub fn rescan_window_lines_for_suppliers_keys(
    s: &State,
    customer_delta: &[CustomerDelta],
    lineitem_delta: &[LineDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrderDelta],
    supplier_delta: &[SupplierDelta],
) -> (r: Vec<i64>)
    ensures r@.no_duplicates(),
        forall|position: int| #![trigger supplier_delta@[position]] 0 <= position < supplier_delta@.len() ==>
            r@.contains(supplier_delta@[position].row.s_suppkey),
{
    let mut keys = Vec::new();
    let mut index = 0;
    while index < supplier_delta.len()
        invariant index <= supplier_delta@.len(), keys@.no_duplicates(),
            forall|position: int| #![trigger supplier_delta@[position]] 0 <= position < index ==>
                keys@.contains(supplier_delta@[position].row.s_suppkey),
        decreases supplier_delta@.len() - index,
    {
        let ghost processed = index as int;
        add_key(&mut keys, supplier_delta[index].row.s_suppkey);
        index += 1;
    }
    keys
}

pub fn apply(
    s: &State,
    customer_delta: &[CustomerDelta],
    lineitem_delta: &[LineDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrderDelta],
    supplier_delta: &[SupplierDelta],
    relevant_orders_for_keys_keys: &[i64],
    relevant_orders_for_keys_rows: &[OrderInput],
    window_lines_for_orders_keys: &[i64],
    window_lines_for_orders_rows: &[LineInput],
    window_lines_for_suppliers_keys: &[i64],
    window_lines_for_suppliers_rows: &[LineInput],
    Ghost(lineitem_full): Ghost<Seq<(LineitemRow, int)>>,
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        lineitem_delta_ok(lineitem_delta@, lineitem_full),
        orders_delta_ok(orders_delta@, orders_full),
        capture_ok(s.db(), db2, customer_delta_seq_view(customer_delta@), lineitem_full, nation_delta_seq_view(nation_delta@), orders_full, supplier_delta_seq_view(supplier_delta@)),
        rescan_relevant_orders_for_keys_ok(db2, relevant_orders_for_keys_keys@, relevant_orders_for_keys_rows@),
        rescan_window_lines_for_orders_ok(db2, window_lines_for_orders_keys@, window_lines_for_orders_rows@),
        rescan_window_lines_for_suppliers_ok(db2, window_lines_for_suppliers_keys@, window_lines_for_suppliers_rows@),
    ensures
        r matches Ok(s2) ==> s2.db() == db2,
{
    match maintain(s, customer_delta, lineitem_delta, nation_delta, orders_delta, supplier_delta,
        relevant_orders_for_keys_keys, relevant_orders_for_keys_rows,
        window_lines_for_orders_keys, window_lines_for_orders_rows,
        window_lines_for_suppliers_keys, window_lines_for_suppliers_rows,
        Ghost(lineitem_full), Ghost(orders_full), Ghost(db2)) {
        Some(state) => Ok(state), None => Err(1),
    }
}

pub fn touched_orders(lines: &[LineDelta], orders: &[OrderDelta]) -> (result: Vec<i64>)
    ensures result@.no_duplicates(),
        forall|position: int| #![trigger orders@[position]] 0 <= position < orders@.len() ==>
            result@.contains(orders@[position].o_orderkey),
        forall|position: int| #![trigger lines@[position]] 0 <= position < lines@.len()
            && (lines@[position].ship_1995 != 0 || lines@[position].ship_1996 != 0) ==>
                result@.contains(lines@[position].l_orderkey),
{
    let mut keys = Vec::new();
    let mut index = 0;
    while index < orders.len()
        invariant index <= orders@.len(), keys@.no_duplicates(),
            forall|position: int| #![trigger orders@[position]] 0 <= position < index ==>
                keys@.contains(orders@[position].o_orderkey),
        decreases orders@.len() - index,
    {
        let ghost processed = index as int;
        add_key(&mut keys, orders[index].o_orderkey);
        index += 1;
    }
    let mut index = 0;
    while index < lines.len()
        invariant index <= lines@.len(), keys@.no_duplicates(),
            forall|position: int| #![trigger orders@[position]] 0 <= position < orders@.len() ==>
                keys@.contains(orders@[position].o_orderkey),
            forall|position: int| #![trigger lines@[position]] 0 <= position < index
                && (lines@[position].ship_1995 != 0 || lines@[position].ship_1996 != 0) ==>
                    keys@.contains(lines@[position].l_orderkey),
        decreases lines@.len() - index,
    {
        let ghost processed = index as int;
        if lines[index].ship_1995 != 0 || lines[index].ship_1996 != 0 { add_key(&mut keys, lines[index].l_orderkey); }
        index += 1;
    }
    keys
}

pub open spec fn cell_marked(affected: Sparse<Groups>, first: i64, second: i64, year: i64) -> bool {
    affected.at(first) matches Some(bucket) && group_keys(bucket.rows@).contains((second, year))
}

pub fn mark_cell(affected: &mut Sparse<Groups>, first: i64, second: i64, year: i64)
    requires old(affected).valid(),
    ensures final(affected).valid(),
        sparse_groups_shape(*old(affected)) && (year == 1995 || year == 1996) ==> sparse_groups_shape(*final(affected)),
        forall|key: i64, other: i64, cell_year: i64| #![trigger cell_marked(*final(affected), key, other, cell_year)]
            cell_marked(*final(affected), key, other, cell_year) ==
                (cell_marked(*old(affected), key, other, cell_year) || (key == first && other == second && cell_year == year)),
{
    let ghost initial = *affected;
    let mut bucket = match affected.get(first) { Some(bucket) => copy_groups(bucket), None => Groups { rows: Vec::new() } };
    let ghost before = bucket.rows@;
    let mut offset = 0;
    while offset < bucket.rows.len()
        invariant offset <= bucket.rows@.len(), affected.valid(), *affected == initial, initial == *old(affected),
            bucket.rows@ == before,
            initial.at(first).is_some() ==> bucket.rows@ == initial.at(first).unwrap().rows@,
            initial.at(first).is_none() ==> bucket.rows@.len() == 0,
            sparse_groups_shape(initial) ==> groups_shape(bucket),
            forall|position: int| #![trigger before[position]] 0 <= position < offset ==>
                (before[position].other, before[position].year) != (second, year),
        decreases bucket.rows@.len() - offset,
    {
        if bucket.rows[offset].other == second && bucket.rows[offset].year == year {
            proof {
                assert(group_keys(before)[offset as int] == (second, year));
                assert(group_keys(before).contains((second, year)));
            }
            return;
        }
        offset += 1;
    }
    bucket.rows.push(Cell { other: second, year, count: 1, revenue: 0 });
    proof {
        assert(group_keys(bucket.rows@) =~= group_keys(before).push((second, year)));
        verus_prooflib::lemma_contains_push(group_keys(before), (second, year));
        if sparse_groups_shape(initial) && (year == 1995 || year == 1996) {
            assert(!group_keys(before).contains((second, year))) by {
                if group_keys(before).contains((second, year)) {
                    let position = choose|position: int| #![trigger group_keys(before)[position]]
                        0 <= position < group_keys(before).len() && group_keys(before)[position] == (second, year);
                    assert(before[position].other == second && before[position].year == year);
                }
            }
            verus_prooflib::lemma_unique_push(group_keys(before), (second, year));
            assert(groups_shape(bucket));
        }
    }
    affected.put(first, Arc::new(bucket));
}

pub fn mark_bucket(groups: &Layers<Groups>, affected: &mut Sparse<Groups>, first: i64, second: Option<i64>)
    requires old(affected).valid(),
        forall|key: i64| #![trigger groups.at(key)] groups.at(key) matches Some(bucket) ==> groups_shape(bucket),
    ensures final(affected).valid(),
        sparse_groups_shape(*old(affected)) ==> sparse_groups_shape(*final(affected)),
        forall|key: i64, other: i64, year: i64| #![trigger cell_marked(*final(affected), key, other, year)]
            cell_marked(*old(affected), key, other, year) ==> cell_marked(*final(affected), key, other, year),
        forall|other: i64, year: i64| #![trigger cell_marked(*final(affected), first, other, year)]
            group_present(*groups, first, other, year) && (second.is_none() || second == Some(other)) ==>
                cell_marked(*final(affected), first, other, year),
{
    let ghost initial = *affected;
    if let Some(bucket) = groups.get(first) {
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(), affected.valid(), initial == *old(affected),
                groups.at(first) == Some(*bucket), groups_shape(*bucket),
                sparse_groups_shape(initial) ==> sparse_groups_shape(*affected),
                forall|key: i64, other: i64, year: i64| #![trigger cell_marked(*affected, key, other, year)]
                    cell_marked(initial, key, other, year) ==> cell_marked(*affected, key, other, year),
                forall|position: int| #![trigger bucket.rows@[position]] 0 <= position < offset &&
                    (second.is_none() || second == Some(bucket.rows@[position].other)) ==>
                        cell_marked(*affected, first, bucket.rows@[position].other, bucket.rows@[position].year),
            decreases bucket.rows@.len() - offset,
        {
            let cell = bucket.rows[offset];
            if second.is_none() || second == Some(cell.other) { mark_cell(affected, first, cell.other, cell.year); }
            offset += 1;
        }
        proof {
            assert forall|other: i64, year: i64| #![trigger cell_marked(*affected, first, other, year)]
                group_present(*groups, first, other, year) && (second.is_none() || second == Some(other)) implies
                    cell_marked(*affected, first, other, year) by {
                let position = choose|position: int| #![trigger group_keys(bucket.rows@)[position]]
                    0 <= position < bucket.rows@.len() && group_keys(bucket.rows@)[position] == (other, year);
                assert(bucket.rows@[position].other == other && bucket.rows@[position].year == year);
            }
        }
    }
}

pub fn find_cell(groups: &Layers<Groups>, first: i64, second: i64, year: i64) -> (result: Cell)
    ensures result.other == second, result.year == year,
        (groups.at(first) matches Some(bucket) ==> groups_shape(bucket)) ==> {
            &&& result.count as int == group_value(*groups, first, second, year, false)
            &&& result.revenue as int == group_value(*groups, first, second, year, true)
        },
{
    if let Some(bucket) = groups.get(first) {
        let mut offset = 0;
        while offset < bucket.rows.len()
            invariant offset <= bucket.rows@.len(), groups.at(first) == Some(*bucket),
                forall|position: int| #![trigger bucket.rows@[position]] 0 <= position < offset ==>
                    (bucket.rows@[position].other, bucket.rows@[position].year) != (second, year),
            decreases bucket.rows@.len() - offset,
        {
            let cell = bucket.rows[offset];
            if cell.other == second && cell.year == year {
                proof {
                    if groups_shape(*bucket) {
                        let keys = group_keys(bucket.rows@);
                        assert forall|position: int| 0 <= position < bucket.rows@.len() && position != offset as int implies
                            (#[trigger] bucket.rows@[position]).other != second || bucket.rows@[position].year != year by {
                            assert(keys[position] == (bucket.rows@[position].other, bucket.rows@[position].year));
                            assert(keys[offset as int] == (second, year));
                        }
                        verus_common::sums::lemma_sum_single_match(bucket.rows@, group_weight(second, year, false), offset as int);
                        verus_common::sums::lemma_sum_single_match(bucket.rows@, group_weight(second, year, true), offset as int);
                    }
                }
                return cell;
            }
            offset += 1;
        }
        proof {
            verus_common::sums::lemma_sum_zero(bucket.rows@, group_weight(second, year, false));
            verus_common::sums::lemma_sum_zero(bucket.rows@, group_weight(second, year, true));
        }
    }
    Cell { other: second, year, count: 0, revenue: 0 }
}

pub closed spec fn neighborhood_sum(state: State, suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    rows: Seq<LineInput>, excluded: Seq<i64>, first: i64, second: i64, year: i64, revenue: bool) -> int
{
    verus_common::sums::sum(rows, runtime_difference(state.suppliers, state.customers, state.orders,
        suppliers, customers, orders, excluded, first, second, year, revenue))
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub fn merge_numeric_delta(previous: &Layers<Groups>, delta: &Sparse<Groups>, affected: &mut Sparse<Groups>)
    -> (result: Option<Sparse<Groups>>)
    requires old(affected).valid(), delta.valid(), sparse_groups_shape(*delta),
        forall|key: i64| #![trigger previous.at(key)] previous.at(key) matches Some(bucket) ==> groups_shape(bucket),
    ensures final(affected).valid(),
        sparse_groups_shape(*old(affected)) && sparse_groups_shape(*delta) ==> sparse_groups_shape(*final(affected)),
        result matches Some(changes) ==> {
            &&& changes.valid()
            &&& sparse_groups_shape(*delta) ==> sparse_groups_shape(changes)
            &&& forall|first: i64, second: i64, year: i64, revenue: bool|
                #![trigger patched_group(*previous, changes, first, second, year, revenue)]
                patched_group(*previous, changes, first, second, year, revenue)
                    == group_value(*previous, first, second, year, revenue) + sparse_group(*delta, first, second, year, revenue)
            &&& forall|first: i64, second: i64, year: i64| #![trigger cell_marked(*final(affected), first, second, year)]
                cell_marked(*old(affected), first, second, year) ==> cell_marked(*final(affected), first, second, year)
            &&& forall|position: int, offset: int| #![trigger delta.view()[position].1.rows@[offset]]
                0 <= position < delta.view().len() && 0 <= offset < delta.view()[position].1.rows@.len() ==>
                    cell_marked(*final(affected), delta.view()[position].0,
                        delta.view()[position].1.rows@[offset].other, delta.view()[position].1.rows@[offset].year)
        },
{
    let ghost original_marks = *affected;
    let mut changes: Sparse<Groups> = Sparse::new();
    proof { delta.row_facts(); }
    let mut index = 0;
    while index < delta.rows().len()
        invariant index <= delta.view().len(), delta.valid(), sparse_groups_shape(*delta), changes.valid(), affected.valid(), original_marks == *old(affected),
            forall|key: i64| #![trigger previous.at(key)] previous.at(key) matches Some(bucket) ==> groups_shape(bucket),
            sparse_groups_shape(*delta) ==> sparse_groups_shape(changes),
            sparse_groups_shape(original_marks) ==> sparse_groups_shape(*affected),
            forall|position: int| #![trigger delta.view()[position]] 0 <= position < delta.view().len() ==>
                delta.at(delta.view()[position].0) == Some(*delta.view()[position].1),
            forall|first: i64, second: i64, year: i64, revenue: bool|
                #![trigger patched_group(*previous, changes, first, second, year, revenue)]
                patched_group(*previous, changes, first, second, year, revenue)
                    == group_value(*previous, first, second, year, revenue)
                        + if delta.view().take(index as int).map_values(|entry: (i64, Arc<Groups>)| entry.0).contains(first) {
                            sparse_group(*delta, first, second, year, revenue) } else { 0int },
            forall|first: i64, second: i64, year: i64| #![trigger cell_marked(*affected, first, second, year)]
                cell_marked(original_marks, first, second, year) ==> cell_marked(*affected, first, second, year),
            forall|position: int, offset: int| #![trigger delta.view()[position].1.rows@[offset]]
                0 <= position < index && 0 <= offset < delta.view()[position].1.rows@.len() ==>
                    cell_marked(*affected, delta.view()[position].0,
                        delta.view()[position].1.rows@[offset].other, delta.view()[position].1.rows@[offset].year),
        decreases delta.view().len() - index,
    {
        let first = delta.rows()[index].0;
        let cells = &*delta.rows()[index].1;
        let mut bucket = match previous.get(first) { Some(bucket) => copy_groups(bucket), None => Groups { rows: Vec::new() } };
        let ghost before_changes = changes;
        let mut offset = 0;
        while offset < cells.rows.len()
            invariant offset <= cells.rows@.len(), index < delta.view().len(), changes.valid(), affected.valid(),
                original_marks == *old(affected),
                first == delta.view()[index as int].0, *cells == *delta.view()[index as int].1,
                delta.at(first) == Some(*cells), groups_shape(*cells), changes == before_changes,
                groups_shape(bucket),
                forall|target: i64, second: i64, year: i64, revenue: bool|
                    #![trigger patched_group(*previous, before_changes, target, second, year, revenue)]
                    patched_group(*previous, before_changes, target, second, year, revenue)
                        == group_value(*previous, target, second, year, revenue)
                            + if delta.view().take(index as int).map_values(|entry: (i64, Arc<Groups>)| entry.0).contains(target) {
                                sparse_group(*delta, target, second, year, revenue) } else { 0int },
                sparse_groups_shape(original_marks) ==> sparse_groups_shape(*affected),
                forall|second: i64, year: i64, revenue: bool| #![trigger group_at(bucket.rows@, second, year, revenue)]
                    group_at(bucket.rows@, second, year, revenue)
                        == group_value(*previous, first, second, year, revenue)
                            + group_at(cells.rows@.take(offset as int), second, year, revenue),
                forall|target: i64, second: i64, year: i64| #![trigger cell_marked(*affected, target, second, year)]
                    cell_marked(original_marks, target, second, year) ==> cell_marked(*affected, target, second, year),
                forall|position: int, cell_offset: int| #![trigger delta.view()[position].1.rows@[cell_offset]]
                    0 <= position < index && 0 <= cell_offset < delta.view()[position].1.rows@.len() ==>
                        cell_marked(*affected, delta.view()[position].0,
                            delta.view()[position].1.rows@[cell_offset].other, delta.view()[position].1.rows@[cell_offset].year),
                forall|position: int| #![trigger cells.rows@[position]] 0 <= position < offset ==>
                    cell_marked(*affected, first, cells.rows@[position].other, cells.rows@[position].year),
            decreases cells.rows@.len() - offset,
        {
            let ghost before_bucket = bucket;
            let cell = cells.rows[offset];
            value!(adjust_group(&mut bucket, cell.other, cell.year, cell.count, cell.revenue));
            mark_cell(affected, first, cell.other, cell.year);
            proof {
                assert forall|second: i64, year: i64, revenue: bool| #![trigger group_at(bucket.rows@, second, year, revenue)]
                    group_at(bucket.rows@, second, year, revenue)
                        == group_value(*previous, first, second, year, revenue)
                            + group_at(cells.rows@.take(offset as int + 1), second, year, revenue) by {
                    verus_common::sums::lemma_sum_take(cells.rows@, offset as int, group_weight(second, year, revenue));
                }
            }
            offset += 1;
        }
        changes.put(first, Arc::new(bucket));
        proof {
            assert(cells.rows@.take(cells.rows@.len() as int) == cells.rows@);
            let processed = delta.view().take(index as int).map_values(|entry: (i64, Arc<Groups>)| entry.0);
            assert(delta.view().take(index as int + 1).map_values(|entry: (i64, Arc<Groups>)| entry.0) =~= processed.push(first));
            verus_prooflib::lemma_contains_push(processed, first);
            assert forall|target: i64, second: i64, year: i64, revenue: bool|
                #![trigger patched_group(*previous, changes, target, second, year, revenue)]
                patched_group(*previous, changes, target, second, year, revenue)
                    == group_value(*previous, target, second, year, revenue)
                        + if delta.view().take(index as int + 1).map_values(|entry: (i64, Arc<Groups>)| entry.0).contains(target) {
                            sparse_group(*delta, target, second, year, revenue) } else { 0int } by {
                if target == first {
                    assert(delta.at(first) == Some(*cells));
                    assert(sparse_group(*delta, first, second, year, revenue) == group_at(cells.rows@, second, year, revenue));
                    assert(group_at(bucket.rows@, second, year, revenue)
                        == group_value(*previous, first, second, year, revenue) + group_at(cells.rows@, second, year, revenue));
                    assert(changes.at(first) == Some(bucket));
                } else {
                    assert(changes.at(target) == before_changes.at(target));
                    assert(patched_group(*previous, changes, target, second, year, revenue)
                        == patched_group(*previous, before_changes, target, second, year, revenue));
                    assert(patched_group(*previous, before_changes, target, second, year, revenue)
                        == group_value(*previous, target, second, year, revenue)
                            + if delta.view().take(index as int).map_values(|entry: (i64, Arc<Groups>)| entry.0).contains(target) {
                                sparse_group(*delta, target, second, year, revenue) } else { 0int });
                }
            }
        }
        index += 1;
    }
    proof {
        assert(delta.view().take(delta.view().len() as int) == delta.view());
        assert forall|first: i64, second: i64, year: i64, revenue: bool|
            #![trigger patched_group(*previous, changes, first, second, year, revenue)]
            patched_group(*previous, changes, first, second, year, revenue)
                == group_value(*previous, first, second, year, revenue) + sparse_group(*delta, first, second, year, revenue) by {
            if delta.at(first).is_some() {
                let position = choose|position: int| #![trigger delta.view()[position]]
                    0 <= position < delta.view().len() && delta.view()[position].0 == first && delta.at(first) == Some(*delta.view()[position].1);
                assert(delta.view().map_values(|entry: (i64, Arc<Groups>)| entry.0)[position] == first);
                assert(delta.view().map_values(|entry: (i64, Arc<Groups>)| entry.0).contains(first));
            }
        }
    }
    Some(changes)
}

pub fn maintain(
    state: &State,
    customer_delta: &[CustomerDelta],
    lineitem_delta: &[LineDelta],
    nation_delta: &Vec<NationDelta>,
    orders_delta: &[OrderDelta],
    supplier_delta: &[SupplierDelta],
    order_keys: &[i64], order_rows: &[OrderInput],
    line_order_keys: &[i64], line_order_rows: &[LineInput],
    line_supplier_keys: &[i64], line_supplier_rows: &[LineInput],
    Ghost(lineitem_full): Ghost<Seq<(LineitemRow, int)>>,
    Ghost(orders_full): Ghost<Seq<(OrdersRow, int)>>,
    Ghost(db2): Ghost<Db>,
) -> (result: Option<State>)
    requires
        lineitem_delta_ok(lineitem_delta@, lineitem_full),
        orders_delta_ok(orders_delta@, orders_full),
        capture_ok(state.db(), db2, customer_delta_seq_view(customer_delta@), lineitem_full,
            nation_delta_seq_view(nation_delta@), orders_full, supplier_delta_seq_view(supplier_delta@)),
        rescan_relevant_orders_for_keys_ok(db2, order_keys@, order_rows@),
        rescan_window_lines_for_orders_ok(db2, line_order_keys@, line_order_rows@),
        rescan_window_lines_for_suppliers_ok(db2, line_supplier_keys@, line_supplier_rows@),
    ensures result matches Some(next) ==> next.db() == db2,
{
    hide(group_count); hide(row_of);
    proof { use_type_invariant(state); }
    proof { lemma_capture_domain(state.db(), db2, supplier_delta@, customer_delta@, nation_delta@,
        lineitem_delta@, lineitem_full, orders_full); }
    let touched = touched_orders(lineitem_delta, orders_delta);
    let needed_orders = rescan_window_lines_for_orders_keys(state, customer_delta, lineitem_delta, nation_delta, orders_delta, supplier_delta);
    let needed_suppliers = rescan_window_lines_for_suppliers_keys(state, customer_delta, lineitem_delta, nation_delta, orders_delta, supplier_delta);
    if !covered(&touched, order_keys) || !covered(&needed_orders, line_order_keys)
        || !covered(&needed_suppliers, line_supplier_keys) { return None; }

    let mut supplier_changes = Sparse::new();
    let mut customer_changes = Sparse::new();
    let mut nation_changes = Sparse::new();
    let mut order_changes = Sparse::new();
    let mut reverse_changes: Sparse<Layers<i128>> = Sparse::new();
    let mut numeric_delta = Sparse::new();
    let mut affected = Sparse::new();

    let mut index = 0;
    while index < supplier_delta.len()
        invariant index <= supplier_delta@.len(), supplier_changes.valid(),
            forall|key: i64| #![trigger patched_counts_unique(state.suppliers, supplier_changes, key)]
                patched_counts_unique(state.suppliers, supplier_changes, key),
            forall|key: i64, other: i64| #![trigger patched_count(state.suppliers, supplier_changes, key, other)]
                patched_count(state.suppliers, supplier_changes, key, other)
                == dimension_value(state.suppliers, key, other)
                    + verus_common::sums::sum(supplier_delta@.take(index as int), |change: SupplierDelta|
                        if change.row.s_suppkey == key && change.row.s_nationkey == other { change.sign as int } else { 0 }),
        decreases supplier_delta@.len() - index,
    {
        let ghost processed = index as int;
        let change = supplier_delta[index];
        let ghost prefix_changes = supplier_changes;
        value!(count_patch(&state.suppliers, &mut supplier_changes, change.row.s_suppkey, change.row.s_nationkey, change.sign as i128));
        proof {
            assert forall|key: i64, other: i64| #![trigger patched_count(state.suppliers, supplier_changes, key, other)]
                patched_count(state.suppliers, supplier_changes, key, other)
                == dimension_value(state.suppliers, key, other)
                    + verus_common::sums::sum(supplier_delta@.take(index as int + 1), |change: SupplierDelta|
                        if change.row.s_suppkey == key && change.row.s_nationkey == other { change.sign as int } else { 0int }) by {
                assert(patched_count(state.suppliers, supplier_changes, key, other)
                    == patched_count(state.suppliers, prefix_changes, key, other)
                        + if change.row.s_suppkey == key && change.row.s_nationkey == other { change.sign as int } else { 0int });
                verus_common::sums::lemma_sum_take(supplier_delta@, index as int, |change: SupplierDelta|
                    if change.row.s_suppkey == key && change.row.s_nationkey == other { change.sign as int } else { 0int });
            }
        }
        index += 1;
    }
    let mut index = 0;
    while index < customer_delta.len()
        invariant index <= customer_delta@.len(), customer_changes.valid(),
            forall|key: i64| #![trigger patched_counts_unique(state.customers, customer_changes, key)]
                patched_counts_unique(state.customers, customer_changes, key),
            forall|key: i64, other: i64| #![trigger patched_count(state.customers, customer_changes, key, other)]
                patched_count(state.customers, customer_changes, key, other)
                == dimension_value(state.customers, key, other)
                    + verus_common::sums::sum(customer_delta@.take(index as int), |change: CustomerDelta|
                        if change.row.c_custkey == key && change.row.c_nationkey == other { change.sign as int } else { 0 }),
        decreases customer_delta@.len() - index,
    {
        let ghost processed = index as int;
        let change = customer_delta[index];
        let ghost prefix_changes = customer_changes;
        value!(count_patch(&state.customers, &mut customer_changes, change.row.c_custkey, change.row.c_nationkey, change.sign as i128));
        proof {
            assert forall|key: i64, other: i64| #![trigger patched_count(state.customers, customer_changes, key, other)]
                patched_count(state.customers, customer_changes, key, other)
                == dimension_value(state.customers, key, other)
                    + verus_common::sums::sum(customer_delta@.take(index as int + 1), |change: CustomerDelta|
                        if change.row.c_custkey == key && change.row.c_nationkey == other { change.sign as int } else { 0int }) by {
                assert(patched_count(state.customers, customer_changes, key, other)
                    == patched_count(state.customers, prefix_changes, key, other)
                        + if change.row.c_custkey == key && change.row.c_nationkey == other { change.sign as int } else { 0int });
                verus_common::sums::lemma_sum_take(customer_delta@, index as int, |change: CustomerDelta|
                    if change.row.c_custkey == key && change.row.c_nationkey == other { change.sign as int } else { 0int });
            }
        }
        index += 1;
    }
    let mut index = 0;
    while index < nation_delta.len()
        invariant index <= nation_delta@.len(), nation_changes.valid(), affected.valid(), sparse_groups_shape(affected),
            forall|key: i64| #![trigger patched_names_unique(state.nations, nation_changes, key)]
                patched_names_unique(state.nations, nation_changes, key),
            forall|key: i64, name: Seq<u8>| #![trigger patched_name(state.nations, nation_changes, key, name)]
                patched_name(state.nations, nation_changes, key, name)
                == nation_value(state.nations, key, name)
                    + nation_delta_count(nation_delta@.take(index as int), key, name),
        decreases nation_delta@.len() - index,
    {
        let ghost processed = index as int;
        let change = &nation_delta[index];
        let ghost prefix_changes = nation_changes;
        value!(name_patch(&state.nations, &mut nation_changes, change.row.n_nationkey, &change.row.n_name, change.sign as i128));
        proof {
            assert forall|key: i64, name: Seq<u8>| #![trigger patched_name(state.nations, nation_changes, key, name)]
                patched_name(state.nations, nation_changes, key, name)
                == nation_value(state.nations, key, name)
                    + nation_delta_count(nation_delta@.take(index as int + 1), key, name) by {
                assert(patched_name(state.nations, nation_changes, key, name)
                    == patched_name(state.nations, prefix_changes, key, name)
                        + if change.row.n_nationkey == key && change.row.n_name@ == name { change.sign as int } else { 0int });
                verus_common::sums::lemma_sum_take(nation_delta@, index as int, nation_signed_weight(key, name));
            }
        }
        index += 1;
    }
    if !positive_counts(&supplier_changes) || !positive_counts(&customer_changes) || !positive_names(&nation_changes) { return None; }
    let suppliers = value!(state.suppliers.patch(supplier_changes));
    let customers = value!(state.customers.patch(customer_changes));
    let nations = value!(state.nations.patch(nation_changes));
    proof {
        lemma_count_patch_shape(state.suppliers, supplier_changes, suppliers);
        lemma_count_patch_shape(state.customers, customer_changes, customers);
        lemma_name_patch_shape(state.nations, nation_changes, nations);
        assert forall|key: i64, other: i64| #![trigger dimension_value(suppliers, key, other)]
            dimension_value(suppliers, key, other) == supplier_count(db2, key, other) by {
            lemma_count_install(state.suppliers, supplier_changes, suppliers, key, other);
            assert(supplier_delta@.take(supplier_delta@.len() as int) == supplier_delta@);
            lemma_supplier_capture(state.db(), db2, supplier_delta@, key, other);
            assert(dimension_value(state.suppliers, key, other) == supplier_count(state.db(), key, other));
        }
        assert forall|key: i64, other: i64| #![trigger dimension_value(customers, key, other)]
            dimension_value(customers, key, other) == customer_count(db2, key, other) by {
            lemma_count_install(state.customers, customer_changes, customers, key, other);
            assert(customer_delta@.take(customer_delta@.len() as int) == customer_delta@);
            lemma_customer_capture(state.db(), db2, customer_delta@, key, other);
            assert(dimension_value(state.customers, key, other) == customer_count(state.db(), key, other));
        }
        assert forall|key: i64, name: Seq<u8>| #![trigger nation_value(nations, key, name)]
            nation_value(nations, key, name) == nation_count(db2, key, name) by {
            lemma_name_install(state.nations, nation_changes, nations, key, name);
            assert(nation_delta@.take(nation_delta@.len() as int) == nation_delta@);
            lemma_nation_capture(state.db(), db2, nation_delta@, key, name);
            assert(nation_value(state.nations, key, name) == nation_count(state.db(), key, name));
            assert(patched_name(state.nations, nation_changes, key, name) == nation_value(state.nations, key, name)
                + nation_delta_count(nation_delta@, key, name));
        }
    }

    let mut index = 0;
    while index < touched.len()
        invariant index <= touched@.len(), touched@.no_duplicates(), order_changes.valid(), reverse_changes.valid(),
            forall|customer: i64| #![trigger reverse_changes.at(customer)]
                reverse_changes.at(customer) matches Some(bucket) ==> bucket.valid(),
            forall|customer: i64, order: i64| #![trigger patched_member(state.reverse, reverse_changes, customer, order)]
                patched_member(state.reverse, reverse_changes, customer, order)
                    == (patched_count(state.orders, order_changes, order, customer) > 0),
            forall|key: i64| #![trigger patched_counts_unique(state.orders, order_changes, key)]
                patched_counts_unique(state.orders, order_changes, key),
            forall|key: i64| #![trigger patched_counts_positive(state.orders, order_changes, key)]
                patched_counts_positive(state.orders, order_changes, key),
            forall|key: i64, other: i64| #![trigger patched_count(state.orders, order_changes, key, other)]
                patched_count(state.orders, order_changes, key, other)
                    == if touched@.take(index as int).contains(key) { 0int }
                        else { dimension_value(state.orders, key, other) },
        decreases touched@.len() - index,
    {
        let key = touched[index];
        let ghost previous_changes = order_changes;
        let ghost previous_reverse = reverse_changes;
        proof { verus_prooflib::lemma_prefix_fresh(touched@, index as int); }
        order_changes.put(key, Arc::new(Counts { rows: Vec::new() }));
        proof {
            assert(touched@.take(index as int + 1) =~= touched@.take(index as int).push(key));
            verus_prooflib::lemma_contains_push(touched@.take(index as int), key);
            assert forall|target: i64| #![trigger patched_counts_unique(state.orders, order_changes, target)]
                patched_counts_unique(state.orders, order_changes, target) by {
                if target != key {
                    assert(order_changes.at(target) == previous_changes.at(target));
                    assert(patched_counts_unique(state.orders, previous_changes, target));
                }
            }
            assert forall|target: i64| #![trigger patched_counts_positive(state.orders, order_changes, target)]
                patched_counts_positive(state.orders, order_changes, target) by {
                if target != key {
                    assert(order_changes.at(target) == previous_changes.at(target));
                    assert(patched_counts_positive(state.orders, previous_changes, target));
                }
            }
            assert forall|target: i64, other: i64| #![trigger patched_count(state.orders, order_changes, target, other)]
                patched_count(state.orders, order_changes, target, other)
                    == if touched@.take(index as int + 1).contains(target) { 0int }
                        else { dimension_value(state.orders, target, other) } by {
                if target != key {
                    assert(order_changes.at(target) == previous_changes.at(target));
                    assert(patched_count(state.orders, order_changes, target, other)
                        == patched_count(state.orders, previous_changes, target, other));
                }
            }
        }
        if let Some(old_bucket) = state.orders.get(key) {
            proof {
                assert forall|customer: i64| #![trigger patched_member(state.reverse, reverse_changes, customer, key)]
                    patched_member(state.reverse, reverse_changes, customer, key) ==> count_at(old_bucket.rows@, customer) > 0 by {
                    assert(patched_count(state.orders, previous_changes, key, customer) == dimension_value(state.orders, key, customer));
                }
            }
            value!(clear_reverse_order(&state.reverse, &mut reverse_changes, old_bucket, key));
        }
        proof {
            assert forall|customer: i64, order: i64| #![trigger patched_member(state.reverse, reverse_changes, customer, order)]
                patched_member(state.reverse, reverse_changes, customer, order)
                    == (patched_count(state.orders, order_changes, order, customer) > 0) by {
                if order == key {
                    if state.orders.at(key).is_none() {
                        assert(dimension_value(state.orders, key, customer) == 0);
                        assert(patched_count(state.orders, previous_changes, key, customer) == 0);
                    }
                } else {
                    assert(order_changes.at(order) == previous_changes.at(order));
                    assert(patched_member(state.reverse, previous_reverse, customer, order)
                        == (patched_count(state.orders, previous_changes, order, customer) > 0));
                }
            }
        }
        index += 1;
    }
    proof { assert(touched@.take(touched@.len() as int) == touched@); }
    let mut index = 0;
    while index < order_rows.len()
        invariant index <= order_rows@.len(), order_changes.valid(), reverse_changes.valid(),
            forall|customer: i64| #![trigger reverse_changes.at(customer)]
                reverse_changes.at(customer) matches Some(bucket) ==> bucket.valid(),
            forall|customer: i64, order: i64| #![trigger patched_member(state.reverse, reverse_changes, customer, order)]
                patched_member(state.reverse, reverse_changes, customer, order)
                    == (patched_count(state.orders, order_changes, order, customer) > 0),
            forall|key: i64| #![trigger patched_counts_unique(state.orders, order_changes, key)]
                patched_counts_unique(state.orders, order_changes, key),
            forall|key: i64| #![trigger patched_counts_positive(state.orders, order_changes, key)]
                patched_counts_positive(state.orders, order_changes, key),
            forall|key: i64, other: i64| #![trigger patched_count(state.orders, order_changes, key, other)]
                patched_count(state.orders, order_changes, key, other)
                    == if touched@.contains(key) { verus_common::sums::sum(order_rows@.take(index as int),
                        |row: OrderInput| if row.o_orderkey == key && row.o_custkey == other { 1int } else { 0int }) }
                        else { dimension_value(state.orders, key, other) },
        decreases order_rows@.len() - index,
    {
        let ghost processed = index as int;
        let ghost previous_changes = order_changes;
        let ghost previous_reverse = reverse_changes;
        let row = order_rows[index];
        if has_key(&touched, row.o_orderkey) {
            value!(count_patch(&state.orders, &mut order_changes, row.o_orderkey, row.o_custkey, 1));
            value!(membership_patch(&state.reverse, &mut reverse_changes, row.o_custkey, row.o_orderkey, true));
        }
        proof {
            assert forall|customer: i64, order: i64| #![trigger patched_member(state.reverse, reverse_changes, customer, order)]
                patched_member(state.reverse, reverse_changes, customer, order)
                    == (patched_count(state.orders, order_changes, order, customer) > 0) by {
                if touched@.contains(row.o_orderkey) && order == row.o_orderkey && customer == row.o_custkey {
                    verus_common::sums::lemma_sum_nonneg(order_rows@.take(index as int),
                        |input: OrderInput| if input.o_orderkey == order && input.o_custkey == customer { 1int } else { 0int });
                    assert(patched_count(state.orders, previous_changes, order, customer) >= 0);
                } else {
                    assert(patched_member(state.reverse, reverse_changes, customer, order)
                        == patched_member(state.reverse, previous_reverse, customer, order));
                    assert(patched_count(state.orders, order_changes, order, customer)
                        == patched_count(state.orders, previous_changes, order, customer));
                }
            }
            assert forall|key: i64, other: i64| #![trigger patched_count(state.orders, order_changes, key, other)]
                patched_count(state.orders, order_changes, key, other)
                    == if touched@.contains(key) { verus_common::sums::sum(order_rows@.take(index as int + 1),
                        |row: OrderInput| if row.o_orderkey == key && row.o_custkey == other { 1int } else { 0int }) }
                        else { dimension_value(state.orders, key, other) } by {
                if touched@.contains(row.o_orderkey) {
                    assert(patched_count(state.orders, order_changes, key, other)
                        == patched_count(state.orders, previous_changes, key, other)
                            + if key == row.o_orderkey && other == row.o_custkey { 1int } else { 0int });
                }
                verus_common::sums::lemma_sum_take(order_rows@, index as int,
                    |row: OrderInput| if row.o_orderkey == key && row.o_custkey == other { 1int } else { 0int });
            }
        }
        index += 1;
    }
    let orders = value!(state.orders.patch(order_changes));
    let reverse = value!(state.reverse.patch(reverse_changes));
    proof {
        assert(order_rows@.take(order_rows@.len() as int) == order_rows@);
        assert forall|key: i64| #![trigger order_changes.at(key)]
            order_changes.at(key) matches Some(bucket) ==> counts_positive(bucket) by {
            assert(patched_counts_positive(state.orders, order_changes, key));
        }
        lemma_count_patch_shape(state.orders, order_changes, orders);
        assert forall|key: i64, other: i64| #![trigger dimension_value(orders, key, other)]
            dimension_value(orders, key, other) == order_count(db2, key, other) by {
            lemma_count_install(state.orders, order_changes, orders, key, other);
            if touched@.contains(key) {
                assert(order_keys@.contains(key));
                lemma_order_rescan(db2, order_keys@, order_rows@, key, other);
            } else {
                assert forall|position: int| #![trigger orders_full[position]]
                    0 <= position < orders_full.len() implies orders_full[position].0.o_orderkey != key by {
                    assert(0 <= position < orders_delta@.len());
                    assert(touched@.contains(orders_delta@[position].o_orderkey));
                }
                assert forall|position: int| #![trigger lineitem_full[position]]
                    0 <= position < lineitem_full.len() && 9131 <= lineitem_full[position].0.l_shipdate <= 9861
                        implies lineitem_full[position].0.l_orderkey != key by {
                    assert(0 <= position < lineitem_delta@.len());
                    assert(lineitem_delta@[position].ship_1995 != 0 || lineitem_delta@[position].ship_1996 != 0);
                    assert(touched@.contains(lineitem_delta@[position].l_orderkey));
                }
                lemma_order_untouched(state.db(), db2, lineitem_full, orders_full, key, other);
            }
        }
        assert forall|customer: i64, order: i64| #![trigger member_value(reverse, customer, order)]
            member_value(reverse, customer, order) == (order_count(db2, order, customer) > 0) by {
            lemma_member_install(state.reverse, reverse_changes, reverse, customer, order);
            lemma_count_install(state.orders, order_changes, orders, order, customer);
        }
        assert forall|customer: i64| #![trigger reverse.at(customer)]
            reverse.at(customer) matches Some(bucket) ==> bucket.valid() by {
            if reverse_changes.at(customer).is_none() { assert(reverse.at(customer) == state.reverse.at(customer)); }
        }
        assert forall|position: int| #![trigger orders_full[position]] 0 <= position < orders_full.len() implies
            seq_view_i64(line_order_keys@).contains(orders_full[position].0.o_orderkey) by {
            assert(needed_orders@.contains(orders_delta@[position].o_orderkey));
            assert(line_order_keys@.contains(orders_delta@[position].o_orderkey));
            lemma_native_keys(line_order_keys@, orders_delta@[position].o_orderkey);
        }
        assert forall|position: int| #![trigger lineitem_full[position]]
            0 <= position < lineitem_full.len() && 9131 <= lineitem_full[position].0.l_shipdate <= 9861 implies
                seq_view_i64(line_order_keys@).contains(lineitem_full[position].0.l_orderkey) by {
            assert(lineitem_delta@[position].ship_1995 != 0 || lineitem_delta@[position].ship_1996 != 0);
            assert(needed_orders@.contains(lineitem_delta@[position].l_orderkey));
            assert(line_order_keys@.contains(lineitem_delta@[position].l_orderkey));
            lemma_native_keys(line_order_keys@, lineitem_delta@[position].l_orderkey);
        }
        assert forall|position: int, order: i64| #![trigger dimension_value(state.orders, order, customer_delta@[position].row.c_custkey)]
            0 <= position < customer_delta@.len() && dimension_value(state.orders, order, customer_delta@[position].row.c_custkey) > 0 implies
                line_order_keys@.contains(order) by {
            assert(state.reverse_has(customer_delta@[position].row.c_custkey, order));
            assert(needed_orders@.contains(order));
        }
        assert forall|position: int| #![trigger supplier_delta@[position]] 0 <= position < supplier_delta@.len() implies
            line_supplier_keys@.contains(supplier_delta@[position].row.s_suppkey) by {
            assert(needed_suppliers@.contains(supplier_delta@[position].row.s_suppkey));
        }
        lemma_unselected_facts(state.db(), db2, state.suppliers, state.customers, state.orders, suppliers, customers, orders,
            supplier_delta@, customer_delta@, lineitem_full, orders_full, line_order_keys@, line_supplier_keys@);
    }

    let mut index = 0;
    while index < line_order_rows.len()
        invariant index <= line_order_rows@.len(), numeric_delta.valid(), sparse_groups_shape(numeric_delta), suppliers.valid(), customers.valid(), orders.valid(),
            forall|first: i64, second: i64, year: i64, revenue: bool|
                #![trigger sparse_group(numeric_delta, first, second, year, revenue)]
                sparse_group(numeric_delta, first, second, year, revenue)
                == neighborhood_sum(*state, suppliers, customers, orders,
                    line_order_rows@.take(index as int), Seq::empty(), first, second, year, revenue),
        decreases line_order_rows@.len() - index,
    {
        let ghost processed = index as int;
        let ghost prefix_groups = numeric_delta;
        let line = line_order_rows[index];
        let year = if line.l_shipdate <= 9495 { 1995 } else { 1996 };
        let volume = value!((line.l_extendedprice as i128).checked_mul(100i128 - line.l_discount as i128));
        value!(line_contribution(&suppliers, &customers, &orders, &mut numeric_delta, line.l_orderkey, line.l_suppkey, year, volume, 1));
        value!(line_contribution(&state.suppliers, &state.customers, &state.orders, &mut numeric_delta, line.l_orderkey, line.l_suppkey, year, volume, -1));
        proof {
            reveal(line_weight);
            assert forall|first: i64, second: i64, year: i64, revenue: bool|
                #![trigger sparse_group(numeric_delta, first, second, year, revenue)]
                sparse_group(numeric_delta, first, second, year, revenue)
                == neighborhood_sum(*state, suppliers, customers, orders,
                    line_order_rows@.take(index as int + 1), Seq::empty(), first, second, year, revenue) by {
                assert(sparse_group(numeric_delta, first, second, year, revenue)
                    == sparse_group(prefix_groups, first, second, year, revenue) + (line_weight(suppliers, customers, orders, line, first, second, year, revenue) - line_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue)));
                verus_common::sums::lemma_sum_take(line_order_rows@, index as int,
                    |line: LineInput| line_weight(suppliers, customers, orders, line, first, second, year, revenue) - line_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue));
                verus_common::sums::lemma_sum_ext(line_order_rows@.take(index as int + 1),
                    |line: LineInput| line_weight(suppliers, customers, orders, line, first, second, year, revenue)
                        - line_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue),
                    |line: LineInput| if Seq::<i64>::empty().contains(line.l_orderkey) { 0int } else {
                        line_weight(suppliers, customers, orders, line, first, second, year, revenue)
                            - line_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue) });
                verus_common::sums::lemma_sum_ext(line_order_rows@.take(index as int),
                    |line: LineInput| line_weight(suppliers, customers, orders, line, first, second, year, revenue)
                        - line_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue),
                    |line: LineInput| if Seq::<i64>::empty().contains(line.l_orderkey) { 0int } else {
                        line_weight(suppliers, customers, orders, line, first, second, year, revenue)
                            - line_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue) });
            }
        }
        index += 1;
    }
    let mut index = 0;
    while index < line_supplier_rows.len()
        invariant index <= line_supplier_rows@.len(), numeric_delta.valid(), sparse_groups_shape(numeric_delta), suppliers.valid(), customers.valid(), orders.valid(),
            forall|first: i64, second: i64, year: i64, revenue: bool|
                #![trigger sparse_group(numeric_delta, first, second, year, revenue)]
                sparse_group(numeric_delta, first, second, year, revenue)
                == neighborhood_sum(*state, suppliers, customers, orders,
                    line_order_rows@, Seq::empty(), first, second, year, revenue)
                    + neighborhood_sum(*state, suppliers, customers, orders,
                        line_supplier_rows@.take(index as int), line_order_keys@, first, second, year, revenue),
        decreases line_supplier_rows@.len() - index,
    {
        let ghost processed = index as int;
        let ghost prefix_groups = numeric_delta;
        let line = line_supplier_rows[index];
        if !has_key(line_order_keys, line.l_orderkey) {
            let year = if line.l_shipdate <= 9495 { 1995 } else { 1996 };
            let volume = value!((line.l_extendedprice as i128).checked_mul(100i128 - line.l_discount as i128));
            value!(line_contribution(&suppliers, &customers, &orders, &mut numeric_delta, line.l_orderkey, line.l_suppkey, year, volume, 1));
            let ghost added_groups = numeric_delta;
            value!(line_contribution(&state.suppliers, &state.customers, &state.orders, &mut numeric_delta, line.l_orderkey, line.l_suppkey, year, volume, -1));
            proof {
                reveal(line_weight);
                assert forall|first: i64, second: i64, target_year: i64, revenue: bool|
                    #![trigger sparse_group(numeric_delta, first, second, target_year, revenue)]
                    sparse_group(numeric_delta, first, second, target_year, revenue)
                        == sparse_group(prefix_groups, first, second, target_year, revenue)
                            + line_weight(suppliers, customers, orders, line, first, second, target_year, revenue)
                            - line_weight(state.suppliers, state.customers, state.orders, line, first, second, target_year, revenue) by {
                    assert(sparse_group(added_groups, first, second, target_year, revenue)
                        == sparse_group(prefix_groups, first, second, target_year, revenue)
                            + line_weight(suppliers, customers, orders, line, first, second, target_year, revenue));
                }
            }
        }
        proof {
            reveal(line_weight);
            assert forall|first: i64, second: i64, year: i64, revenue: bool|
                #![trigger sparse_group(numeric_delta, first, second, year, revenue)]
                sparse_group(numeric_delta, first, second, year, revenue)
                == neighborhood_sum(*state, suppliers, customers, orders,
                    line_order_rows@, Seq::empty(), first, second, year, revenue)
                    + neighborhood_sum(*state, suppliers, customers, orders,
                        line_supplier_rows@.take(index as int + 1), line_order_keys@, first, second, year, revenue) by {
                assert(sparse_group(numeric_delta, first, second, year, revenue)
                    == sparse_group(prefix_groups, first, second, year, revenue) + (if line_order_keys@.contains(line.l_orderkey) { 0int } else { line_weight(suppliers, customers, orders, line, first, second, year, revenue) - line_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue) }));
                verus_common::sums::lemma_sum_take(line_supplier_rows@, index as int,
                    |line: LineInput| if line_order_keys@.contains(line.l_orderkey) { 0int } else { line_weight(suppliers, customers, orders, line, first, second, year, revenue) - line_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue) });
            }
        }
        index += 1;
    }
    let mut index = 0;
    while index < lineitem_delta.len()
        invariant index <= lineitem_delta@.len(), numeric_delta.valid(), sparse_groups_shape(numeric_delta), state.valid(),
            forall|first: i64, second: i64, year: i64, revenue: bool|
                #![trigger sparse_group(numeric_delta, first, second, year, revenue)]
                sparse_group(numeric_delta, first, second, year, revenue)
                == neighborhood_sum(*state, suppliers, customers, orders,
                    line_order_rows@, Seq::empty(), first, second, year, revenue)
                    + neighborhood_sum(*state, suppliers, customers, orders,
                        line_supplier_rows@, line_order_keys@, first, second, year, revenue)
                    + verus_common::sums::sum(lineitem_delta@.take(index as int), |line: LineDelta|
                        captured_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue)),
        decreases lineitem_delta@.len() - index,
    {
        let ghost processed = index as int;
        let ghost prefix_groups = numeric_delta;
        let line = lineitem_delta[index];
        if line.ship_1995 != 0 || line.ship_1996 != 0 {
            if line.ship_1995 != 0 && line.ship_1996 != 0 { return None; }
            let year = if line.ship_1995 != 0 { 1995 } else { 1996 };
            let volume = value!((line.l_extendedprice as i128).checked_mul(100i128 - line.l_discount as i128));
            value!(line_contribution(&state.suppliers, &state.customers, &state.orders, &mut numeric_delta,
                line.l_orderkey, line.l_suppkey, year, volume, line.sign as i128));
        }
        proof {
            reveal(captured_weight);
            assert forall|first: i64, second: i64, year: i64, revenue: bool|
                #![trigger sparse_group(numeric_delta, first, second, year, revenue)]
                sparse_group(numeric_delta, first, second, year, revenue)
                == neighborhood_sum(*state, suppliers, customers, orders,
                    line_order_rows@, Seq::empty(), first, second, year, revenue)
                    + neighborhood_sum(*state, suppliers, customers, orders,
                        line_supplier_rows@, line_order_keys@, first, second, year, revenue)
                    + verus_common::sums::sum(lineitem_delta@.take(index as int + 1), |line: LineDelta|
                        captured_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue)) by {
                assert(sparse_group(numeric_delta, first, second, year, revenue)
                    == sparse_group(prefix_groups, first, second, year, revenue) + (captured_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue)));
                verus_common::sums::lemma_sum_take(lineitem_delta@, index as int,
                    |line: LineDelta| captured_weight(state.suppliers, state.customers, state.orders, line, first, second, year, revenue));
            }
        }
        index += 1;
    }

    proof {
        assert(lineitem_delta@.take(lineitem_delta@.len() as int) == lineitem_delta@);
        assert forall|first: i64, second: i64, year: i64, revenue: bool|
            #![trigger sparse_group(numeric_delta, first, second, year, revenue)]
            group_value(state.groups, first, second, year, revenue) + sparse_group(numeric_delta, first, second, year, revenue)
                == numeric_group(db2, first, second, year, revenue) by {
            lemma_numeric_transaction(state.db(), db2, state.suppliers, state.customers, state.orders, suppliers, customers, orders,
                lineitem_full, lineitem_delta@, line_order_keys@, line_order_rows@, line_supplier_keys@, line_supplier_rows@,
                first, second, year, revenue);
        }
    }
    let group_changes = value!(merge_numeric_delta(&state.groups, &numeric_delta, &mut affected));
    if !positive_groups(&group_changes) { return None; }
    proof {
        assert forall|second: i64, first: i64| #![trigger member_value(state.incident, second, first)]
            member_value(state.incident, second, first) == group_has(state.groups, first, second) by {
            lemma_group_has(state.groups, first, second);
        }
    }
    let incident = value!(update_incident(&state.groups, &state.incident, &group_changes));
    let groups = value!(state.groups.patch(group_changes));
    proof {
        assert forall|first: i64, second: i64, year: i64, revenue: bool|
            #![trigger group_value(groups, first, second, year, revenue)]
            group_value(groups, first, second, year, revenue) == numeric_group(db2, first, second, year, revenue) by {
            lemma_group_install(state.groups, group_changes, groups, first, second, year, revenue);
            assert(group_value(groups, first, second, year, revenue) == patched_group(state.groups, group_changes, first, second, year, revenue));
        }
        assert forall|key: i64| #![trigger groups.at(key)] groups.at(key) matches Some(bucket) ==> groups_valid(bucket) by {
            if group_changes.at(key).is_some() {
                assert(group_changes.at(key) matches Some(bucket) && groups_shape(bucket) && groups_positive(bucket));
            } else { assert(groups.at(key) == state.groups.at(key)); }
        }
        assert forall|second: i64, first: i64| #![trigger member_value(incident, second, first)]
            member_value(incident, second, first) == (numeric_group(db2, first, second, 1995, false) > 0
                || numeric_group(db2, first, second, 1996, false) > 0) by {
            lemma_replacement_has(state.groups, group_changes, groups, first, second);
            lemma_group_has(groups, first, second);
        }
    }

    proof {
        assert forall|second: i64, first: i64| #![trigger member_value(incident, second, first)]
            member_value(incident, second, first) == group_has(groups, first, second) by {
            lemma_group_has(groups, first, second);
        }
        lemma_numeric_marks(state.groups, groups, numeric_delta, affected);
        assert forall|key: i64, name: Seq<u8>| #![trigger nation_value(nations, key, name)]
            nation_value(nations, key, name) == nation_value(state.nations, key, name) + nation_delta_count(nation_delta@, key, name) by {
            lemma_name_install(state.nations, nation_changes, nations, key, name);
            assert(nation_delta@.take(nation_delta@.len() as int) == nation_delta@);
        }
        lemma_nations_unchanged(state.nations, nations, nation_delta@);
    }
    let ghost numeric_affected = affected;
    mark_nations(&state.groups, &groups, &state.incident, &incident, nation_delta, &mut affected);
    proof {
        assert forall|first: i64, second: i64, year: i64, revenue: bool|
            #![trigger group_value(groups, first, second, year, revenue), cell_marked(affected, first, second, year)]
            !cell_marked(affected, first, second, year) implies
                group_value(groups, first, second, year, revenue) == group_value(state.groups, first, second, year, revenue) by {
            assert(!cell_marked(numeric_affected, first, second, year));
        }
    }
    let answers = value!(update_answers(&state.nations, &nations, &state.groups, &groups, &state.answers, &affected));
    proof {
        assert forall|first: Seq<u8>, second: Seq<u8>, year: i64| #![trigger answer_value(answers, first, second, year, false)]
            answer_value(answers, first, second, year, false) == group_count(db2, params(first, second), first, second, year as int) by {
            lemma_answer_transaction(state.db(), db2, affected, state.nations, nations, state.groups, groups, nation_delta@, first, second, year, false);
            lemma_named_sql(state.db(), first, second, year, false);
            lemma_named_sql(db2, first, second, year, false);
        }
        assert forall|first: Seq<u8>, second: Seq<u8>, year: i64| #![trigger answer_value(answers, first, second, year, true)]
            answer_value(answers, first, second, year, true) == row_of(db2, params(first, second), first, second, year as int).revenue by {
            lemma_answer_transaction(state.db(), db2, affected, state.nations, nations, state.groups, groups, nation_delta@, first, second, year, true);
            lemma_named_sql(state.db(), first, second, year, true);
            lemma_named_sql(db2, first, second, year, true);
        }
    }
    Some(State { db: Ghost(db2), suppliers, customers, orders, nations, reverse, groups, incident,
        answers })
}

} // verus!
