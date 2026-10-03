use vstd::prelude::*;
use crate::gen_spec::*;
use crate::joins::*;
use crate::profiles::*;
use verus_common::sums::*;
use verus_common::rel::*;

verus! {

pub open spec fn line_weight(params: Params, key: int, value: spec_fn(LineitemRow) -> int) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row.l_orderkey == key && row.l_shipdate > params.p2 { value(row) } else { 0 }
}

pub open spec fn tuple_weight(key: int, date: int, priority: int, value: spec_fn(LineitemRow) -> int) -> spec_fn(Tuple) -> int {
    |row: Tuple| if row.lineitem.l_orderkey == key && row.orders.o_orderdate == date && row.orders.o_shippriority == priority {
        value(row.lineitem)
    } else { 0 }
}

pub open spec fn order_matches(params: Params, key: int, date: int, priority: int, row: OrdersRow) -> bool {
    row.o_orderkey == key && row.o_orderdate == date && row.o_shippriority == priority && date < params.p2
}

pub open spec fn joined_weight(params: Params, key: int, date: int, priority: int, subtotal: int) -> spec_fn(CustomerRow, OrdersRow) -> int {
    |customer: CustomerRow, orders: OrdersRow|
        if customer.c_mktsegment == params.p1 && customer.c_custkey == orders.o_custkey && order_matches(params, key, date, priority, orders) {
            subtotal
        } else { 0 }
}

pub proof fn lemma_order_lines(db: Db, params: Params, key: int, date: int, priority: int, customer: CustomerRow, orders: OrdersRow, value: spec_fn(LineitemRow) -> int)
    ensures sum(tuples_2(db, params, customer, orders), tuple_weight(key, date, priority, value)) ==
        joined_weight(params, key, date, priority, sum(db.lineitem, line_weight(params, key, value)))(customer, orders),
{
    let weight = tuple_weight(key, date, priority, value);
    let extend = |line: LineitemRow| tuples_3(db, params, customer, orders, line);
    let actual = |line: LineitemRow| sum(extend(line), weight);
    let active = customer.c_mktsegment == params.p1 && customer.c_custkey == orders.o_custkey && order_matches(params, key, date, priority, orders);
    let expected = |line: LineitemRow| if active { line_weight(params, key, value)(line) } else { 0 };
    assert forall|line: LineitemRow| #[trigger] actual(line) == expected(line) by {
        lemma_sum_opt(where_clause(db, params, customer, orders, line), Tuple { customer, orders, lineitem: line }, weight);
    }
    lemma_sum_flat_map(db.lineitem, extend, weight);
    lemma_sum_ext_all(db.lineitem, actual, expected);
    if active {
        lemma_sum_ext_all(db.lineitem, expected, line_weight(params, key, value));
    } else {
        lemma_sum_zero(db.lineitem, expected);
    }
}

pub proof fn lemma_customer_orders(db: Db, params: Params, key: int, date: int, priority: int, customer: CustomerRow, value: spec_fn(LineitemRow) -> int)
    ensures sum(tuples_1(db, params, customer), tuple_weight(key, date, priority, value)) ==
        sum(db.orders, |orders: OrdersRow| joined_weight(params, key, date, priority, sum(db.lineitem, line_weight(params, key, value)))(customer, orders)),
{
    let weight = tuple_weight(key, date, priority, value);
    let extend = |orders: OrdersRow| tuples_2(db, params, customer, orders);
    let actual = |orders: OrdersRow| sum(extend(orders), weight);
    let expected = |orders: OrdersRow| joined_weight(params, key, date, priority, sum(db.lineitem, line_weight(params, key, value)))(customer, orders);
    assert forall|orders: OrdersRow| #[trigger] actual(orders) == expected(orders) by {
        lemma_order_lines(db, params, key, date, priority, customer, orders, value);
    }
    lemma_sum_flat_map(db.orders, extend, weight);
    lemma_sum_ext_all(db.orders, actual, expected);
}

pub proof fn lemma_group_factor(db: Db, params: Params, key: int, date: int, priority: int, value: spec_fn(LineitemRow) -> int)
    ensures group_sum(db, params, key, date, priority, |tuple: Tuple| value(tuple.lineitem)) ==
        if date < params.p2 { join_count(db, params.p1, date, key, priority) * sum(db.lineitem, line_weight(params, key, value)) } else { 0 },
{
    let weight = tuple_weight(key, date, priority, value);
    let subtotal = sum(db.lineitem, line_weight(params, key, value));
    let requested = |tuple: Tuple| value(tuple.lineitem);
    lemma_sum_ext_all(tuples(db, params),
        |tuple: Tuple| if tuple.lineitem.l_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_shippriority == priority {
            requested(tuple)
        } else { 0 }, weight);
    assert(group_sum(db, params, key, date, priority, |tuple: Tuple| value(tuple.lineitem)) == sum(tuples(db, params), weight));
    let joined = joined_weight(params, key, date, priority, subtotal);
    let extend = |customer: CustomerRow| tuples_1(db, params, customer);
    let actual = |customer: CustomerRow| sum(extend(customer), weight);
    let customer_total = |customer: CustomerRow| sum(db.orders, |orders: OrdersRow| joined(customer, orders));
    let orders_total = |orders: OrdersRow| sum(db.customer, |customer: CustomerRow| joined(customer, orders));
    assert forall|customer: CustomerRow| #[trigger] actual(customer) == customer_total(customer) by {
        lemma_customer_orders(db, params, key, date, priority, customer, value);
    }
    lemma_sum_flat_map(db.customer, extend, weight);
    lemma_sum_ext_all(db.customer, actual, customer_total);
    assert(sum(tuples(db, params), weight) == sum(db.customer, customer_total));
    lemma_sum_swap(db.customer, db.orders, joined, customer_total, orders_total);
    let joined_customers = |orders: OrdersRow| if order_matches(params, key, date, priority, orders) {
        customer_count(db, orders.o_custkey, params.p1)
    } else { 0 };
    assert forall|orders: OrdersRow| #[trigger] orders_total(orders) == subtotal * joined_customers(orders) by {
        let indicator = |customer: CustomerRow| if customer.c_custkey == orders.o_custkey && customer.c_mktsegment == params.p1 { 1int } else { 0 };
        let actual_weight = |customer: CustomerRow| joined(customer, orders);
        if order_matches(params, key, date, priority, orders) {
            lemma_sum_scale(db.customer, indicator, actual_weight, subtotal);
        } else {
            lemma_sum_zero(db.customer, actual_weight);
        }
    }
    lemma_sum_scale(db.orders, joined_customers, orders_total, subtotal);
    assert(sum(tuples(db, params), weight) == subtotal * sum(db.orders, joined_customers));
    if date < params.p2 {
        let expected = |orders: OrdersRow| if orders.o_orderdate == date && orders.o_orderkey == key && orders.o_shippriority == priority {
            customer_count(db, orders.o_custkey, params.p1)
        } else { 0 };
        lemma_sum_ext_all(db.orders, joined_customers, expected);
        assert(sum(db.orders, joined_customers) == join_count(db, params.p1, date, key, priority));
        assert(subtotal * join_count(db, params.p1, date, key, priority) == join_count(db, params.p1, date, key, priority) * subtotal) by(nonlinear_arith);
    } else {
        lemma_sum_zero(db.orders, joined_customers);
    }
}

pub proof fn lemma_group_count_factor(db: Db, params: Params, key: int, date: int, priority: int)
    ensures group_count(db, params, key, date, priority) ==
        if date < params.p2 { join_count(db, params.p1, date, key, priority) * line_count(db, key, params.p2 + 1) } else { 0 },
{
    let value = |line: LineitemRow| 1int;
    lemma_group_factor(db, params, key, date, priority, value);
    assert((|tuple: Tuple| value(tuple.lineitem)) =~= (|tuple: Tuple| 1int));
    let requested = |tuple: Tuple| 1int;
    lemma_sum_ext_all(tuples(db, params),
        |tuple: Tuple| if tuple.lineitem.l_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_shippriority == priority { 1int } else { 0 },
        |tuple: Tuple| if tuple.lineitem.l_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_shippriority == priority { requested(tuple) } else { 0 });
    assert(group_count(db, params, key, date, priority) == group_sum(db, params, key, date, priority, |tuple: Tuple| 1int));
    lemma_sum_ext_all(db.lineitem, line_weight(params, key, value),
        |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= params.p2 + 1 { 1int } else { 0 });
}

pub proof fn lemma_group_revenue_factor(db: Db, params: Params, key: int, date: int, priority: int)
    ensures row_of(db, params, key, date, priority).revenue ==
        if date < params.p2 { join_count(db, params.p1, date, key, priority) * line_revenue(db, key, params.p2 + 1) } else { 0 },
{
    let value = |line: LineitemRow| line.l_extendedprice * (100 - line.l_discount);
    lemma_group_factor(db, params, key, date, priority, value);
    assert((|tuple: Tuple| value(tuple.lineitem)) =~= (|tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount)));
    lemma_sum_ext_all(db.lineitem, line_weight(params, key, value),
        |line: LineitemRow| if line.l_orderkey == key && line.l_shipdate >= params.p2 + 1 { line.l_extendedprice * (100 - line.l_discount) } else { 0 });
}

pub proof fn lemma_empty_query(db: Db, params: Params)
    requires forall|key: int, date: int, priority: int| #[trigger] group_count(db, params, key, date, priority) == 0,
    ensures full_result(db, params, Seq::<OutRow>::empty()), query_result(db, params, Seq::<OutRow>::empty()),
{
    let full = Seq::<OutRow>::empty();
    assert(full_result(db, params, full));
    assert(full == limit_rows(full, 10));
    assert(exists|rows: Seq<OutRow>| #[trigger] full_result(db, params, rows) && full == limit_rows(rows, 10));
}

pub proof fn lemma_segment_absent(db: Db, params: Params)
    requires forall|position: int| 0 <= position < db.customer.len() ==> (#[trigger] db.customer[position]).c_mktsegment != params.p1,
    ensures query_result(db, params, Seq::<OutRow>::empty()),
{
    assert forall|key: int| #[trigger] customer_count(db, key, params.p1) == 0 by {
        lemma_sum_zero(db.customer, |row: CustomerRow| if row.c_custkey == key && row.c_mktsegment == params.p1 { 1int } else { 0 });
    }
    assert forall|key: int, date: int, priority: int| #[trigger] group_count(db, params, key, date, priority) == 0 by {
        lemma_sum_zero(db.orders, |row: OrdersRow| if row.o_orderdate == date && row.o_orderkey == key && row.o_shippriority == priority {
            customer_count(db, row.o_custkey, params.p1)
        } else { 0 });
        lemma_group_count_factor(db, params, key, date, priority);
    }
    lemma_empty_query(db, params);
}

}
