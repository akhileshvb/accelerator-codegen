use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::order::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;

verus! {

pub proof fn empty_sums<A>(rows: Seq<A>)
    requires rows.len() == 0,
    ensures forall|weight: spec_fn(A) -> int| #[trigger] sum(rows, weight) == 0,
{
    assert forall|weight: spec_fn(A) -> int| #[trigger] sum(rows, weight) == 0 by {
        lemma_sum_empty(rows, weight);
    }
}

pub proof fn projected_prefix<A, B>(rows: Seq<A>, index: int, project: spec_fn(A) -> B, weight: spec_fn(B) -> int)
    requires 0 <= index < rows.len(),
    ensures sum(rows.take(index + 1).map_values(project), weight) ==
        sum(rows.take(index).map_values(project), weight) + weight(project(rows[index])),
{
    assert(rows.take(index + 1).map_values(project) =~=
        rows.take(index).map_values(project).push(project(rows[index])));
    lemma_sum_push(rows.take(index).map_values(project), project(rows[index]), weight);
}

pub open spec fn nw(key: int) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row.l_orderkey == key { 1int } else { 0 }
}

pub open spec fn qw(key: int) -> spec_fn(LineitemRow) -> int {
    |row: LineitemRow| if row.l_orderkey == key { row.l_quantity } else { 0 }
}

pub open spec fn ow(key: int, customer: int, date: int, price: int) -> spec_fn(OrdersRow) -> int {
    |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer &&
        row.o_orderdate == date && row.o_totalprice == price { 1int } else { 0 }
}

pub open spec fn cw(customer: int, name: Seq<u8>) -> spec_fn(CustomerRow) -> int {
    |row: CustomerRow| if row.c_custkey == customer && row.c_name == name { 1int } else { 0 }
}

pub open spec fn dn(key: int) -> spec_fn((LineitemRow, int)) -> int {
    |delta: (LineitemRow, int)| delta.1 * nw(key)(delta.0)
}

pub open spec fn dq(key: int) -> spec_fn((LineitemRow, int)) -> int {
    |delta: (LineitemRow, int)| delta.1 * qw(key)(delta.0)
}

pub open spec fn dorder(key: int, customer: int, date: int, price: int) -> spec_fn((OrdersRow, int)) -> int {
    |delta: (OrdersRow, int)| delta.1 * ow(key, customer, date, price)(delta.0)
}

pub open spec fn dcustomer(customer: int, name: Seq<u8>) -> spec_fn((CustomerRow, int)) -> int {
    |delta: (CustomerRow, int)| delta.1 * cw(customer, name)(delta.0)
}

pub proof fn build_facts(db: Db, lines: Seq<LineQuantityScan>, orders: Seq<OrderGroupScan>, customers: Seq<CustomerNameScan>)
    requires scan_line_quantities_ok(db, lines), scan_order_groups_ok(db, orders), scan_customer_names_ok(db, customers),
    ensures
        forall|key: int| #[trigger] sum(line_quantity_scan_seq_view(lines), nw(key)) == sum(db.lineitem, nw(key)),
        forall|key: int| #[trigger] sum(line_quantity_scan_seq_view(lines), qw(key)) == sum(db.lineitem, qw(key)),
        forall|key: int, customer: int, date: int, price: int| #[trigger]
            sum(order_group_scan_seq_view(orders), ow(key, customer, date, price)) == sum(db.orders, ow(key, customer, date, price)),
        forall|customer: int, name: Seq<u8>| #[trigger]
            sum(customer_name_scan_seq_view(customers), cw(customer, name)) == sum(db.customer, cw(customer, name)),
{
}

pub proof fn line_range(lines: Seq<LineQuantityScan>, key: int)
    requires key < i64::MIN || key > i64::MAX,
    ensures sum(line_quantity_scan_seq_view(lines), nw(key)) == 0, sum(line_quantity_scan_seq_view(lines), qw(key)) == 0,
{
    let rows = line_quantity_scan_seq_view(lines);
    assert forall|index: int| 0 <= index < rows.len() implies nw(key)(#[trigger] rows[index]) == 0 && qw(key)(rows[index]) == 0 by {
        assert(rows[index] == line_quantity_scan_view(lines[index]));
    }
    lemma_sum_zero(rows, nw(key));
    lemma_sum_zero(rows, qw(key));
}

pub proof fn order_range(orders: Seq<OrderGroupScan>, key: int, customer: int, date: int, price: int)
    requires key < i64::MIN || key > i64::MAX,
    ensures sum(order_group_scan_seq_view(orders), ow(key, customer, date, price)) == 0,
{
    let rows = order_group_scan_seq_view(orders);
    assert forall|index: int| 0 <= index < rows.len() implies ow(key, customer, date, price)(#[trigger] rows[index]) == 0 by {
        assert(rows[index] == order_group_scan_view(orders[index]));
    }
    lemma_sum_zero(rows, ow(key, customer, date, price));
}

pub proof fn customer_range(customers: Seq<CustomerNameScan>, key: int, name: Seq<u8>)
    requires key < i64::MIN || key > i64::MAX,
    ensures sum(customer_name_scan_seq_view(customers), cw(key, name)) == 0,
{
    let rows = customer_name_scan_seq_view(customers);
    assert forall|index: int| 0 <= index < rows.len() implies cw(key, name)(#[trigger] rows[index]) == 0 by {
        assert(rows[index] == customer_name_scan_view(customers[index]));
    }
    lemma_sum_zero(rows, cw(key, name));
}

pub proof fn apply_facts(db: Db, after: Db, customers: Seq<(CustomerRow, int)>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>)
    requires capture_ok(db, after, customers, lines, orders),
    ensures
        forall|key: int| #[trigger] sum(after.lineitem, nw(key)) == sum(db.lineitem, nw(key)) +
            sum(lines, |delta: (LineitemRow, int)| delta.1 * nw(key)(delta.0)),
        forall|key: int| #[trigger] sum(after.lineitem, qw(key)) == sum(db.lineitem, qw(key)) +
            sum(lines, |delta: (LineitemRow, int)| delta.1 * qw(key)(delta.0)),
        forall|key: int, customer: int, date: int, price: int| #[trigger]
            sum(after.orders, ow(key, customer, date, price)) == sum(db.orders, ow(key, customer, date, price)) +
                sum(orders, |delta: (OrdersRow, int)| delta.1 * ow(key, customer, date, price)(delta.0)),
        forall|customer: int, name: Seq<u8>| #[trigger] sum(after.customer, cw(customer, name)) == sum(db.customer, cw(customer, name)) +
            sum(customers, |delta: (CustomerRow, int)| delta.1 * cw(customer, name)(delta.0)),
{
}

pub proof fn apply_canonical(db: Db, after: Db, customers: Seq<(CustomerRow, int)>, lines: Seq<(LineitemRow, int)>, orders: Seq<(OrdersRow, int)>)
    requires capture_ok(db, after, customers, lines, orders),
    ensures
        forall|key: int| #[trigger] sum(after.lineitem, nw(key)) == sum(db.lineitem, nw(key)) + sum(lines, dn(key)),
        forall|key: int| #[trigger] sum(after.lineitem, qw(key)) == sum(db.lineitem, qw(key)) + sum(lines, dq(key)),
        forall|key: int, customer: int, date: int, price: int| #[trigger] sum(after.orders, ow(key, customer, date, price)) == sum(db.orders, ow(key, customer, date, price)) + sum(orders, dorder(key, customer, date, price)),
        forall|customer: int, name: Seq<u8>| #[trigger] sum(after.customer, cw(customer, name)) == sum(db.customer, cw(customer, name)) + sum(customers, dcustomer(customer, name)),
{
    apply_facts(db, after, customers, lines, orders);
}

pub proof fn line_delta_range(lines: Seq<LineQuantityDelta>, key: int)
    requires key < i64::MIN || key > i64::MAX,
    ensures sum(line_quantity_delta_seq_view(lines), dn(key)) == 0, sum(line_quantity_delta_seq_view(lines), dq(key)) == 0,
{
    let rows = line_quantity_delta_seq_view(lines);
    assert forall|index: int| 0 <= index < rows.len() implies dn(key)(#[trigger] rows[index]) == 0 && dq(key)(rows[index]) == 0 by {
        assert(rows[index].0 == line_quantity_input_view(lines[index].row));
    }
    lemma_sum_zero(rows, dn(key));
    lemma_sum_zero(rows, dq(key));
}

pub proof fn order_delta_range(orders: Seq<OrderGroupDelta>, key: int, customer: int, date: int, price: int)
    requires key < i64::MIN || key > i64::MAX,
    ensures sum(order_group_delta_seq_view(orders), dorder(key, customer, date, price)) == 0,
{
    let rows = order_group_delta_seq_view(orders);
    assert forall|index: int| 0 <= index < rows.len() implies dorder(key, customer, date, price)(#[trigger] rows[index]) == 0 by {
        assert(rows[index].0 == order_group_input_view(orders[index].row));
    }
    lemma_sum_zero(rows, dorder(key, customer, date, price));
}

pub proof fn customer_delta_range(customers: Seq<CustomerNameDelta>, key: int, name: Seq<u8>)
    requires key < i64::MIN || key > i64::MAX,
    ensures sum(customer_name_delta_seq_view(customers), dcustomer(key, name)) == 0,
{
    let rows = customer_name_delta_seq_view(customers);
    assert forall|index: int| 0 <= index < rows.len() implies dcustomer(key, name)(#[trigger] rows[index]) == 0 by {
        assert(rows[index].0 == customer_name_input_view(customers[index].row));
    }
    lemma_sum_zero(rows, dcustomer(key, name));
}

pub proof fn singleton_flat<A, B>(rows: Seq<A>, project: spec_fn(A) -> B)
    ensures flat(rows.map_values(|row: A| seq![project(row)])) == rows.map_values(project),
    decreases rows.len(),
{
    reveal_with_fuel(flat, 2);
    if rows.len() > 0 {
        singleton_flat(rows.drop_last(), project);
        assert(rows.map_values(|row: A| seq![project(row)]).drop_last() =~=
            rows.drop_last().map_values(|row: A| seq![project(row)]));
        assert(rows.map_values(project) =~= rows.drop_last().map_values(project).push(project(rows.last())));
    }
}

pub proof fn subquery_sums(db: Db, parameters: Params, key: int)
    ensures in_1_group_count(db, parameters, key) == sum(db.lineitem, nw(key)),
        in_1_group_sum(db, parameters, key, |tuple: In1Tuple| tuple.lineitem_2.l_quantity) == sum(db.lineitem, qw(key)),
        in_1_tuples(db, parameters) == db.lineitem.map_values(|row: LineitemRow| In1Tuple { lineitem_2: row }),
{
    let project = |row: LineitemRow| In1Tuple { lineitem_2: row };
    singleton_flat(db.lineitem, project);
    assert(db.lineitem.map_values(|row: LineitemRow| in_1_tuples_1(db, parameters, row)) =~=
        db.lineitem.map_values(|row: LineitemRow| seq![project(row)]));
    assert(in_1_tuples(db, parameters) == db.lineitem.map_values(project));
    lemma_sum_map(db.lineitem, project, |tuple: In1Tuple| if tuple.lineitem_2.l_orderkey == key { 1int } else { 0 }, nw(key));
    lemma_sum_map(db.lineitem, project, |tuple: In1Tuple| if tuple.lineitem_2.l_orderkey == key { tuple.lineitem_2.l_quantity } else { 0 }, qw(key));
    let quantity = |tuple: In1Tuple| tuple.lineitem_2.l_quantity;
    lemma_sum_ext_all(in_1_tuples(db, parameters),
        |tuple: In1Tuple| if tuple.lineitem_2.l_orderkey == key { quantity(tuple) } else { 0 },
        |tuple: In1Tuple| if tuple.lineitem_2.l_orderkey == key { tuple.lineitem_2.l_quantity } else { 0 });
}

pub proof fn subquery_membership(db: Db, parameters: Params, key: int)
    ensures in_1(db, parameters, key) <==> sum(db.lineitem, nw(key)) > 0 && sum(db.lineitem, qw(key)) > parameters.p1,
{
    subquery_sums(db, parameters, key);
    let source = in_1_tuples(db, parameters);
    let keep = |tuple: In1Tuple| in_1_having(db, parameters, tuple.lineitem_2.l_orderkey);
    let project = |tuple: In1Tuple| tuple.lineitem_2.l_orderkey;
    let selected = map_where(source, keep, project);
    let keys = dedup(selected);
    lemma_map_where(source, keep, project);
    lemma_dedup(selected);
    if in_1(db, parameters, key) {
        let result = in_1_rows(db, parameters);
        let position = choose|index: int| #[trigger] result[index].l_orderkey == key && 0 <= index < result.len();
        assert(keys[position] == key);
        assert(keys.contains(key));
        let witness = choose|index: int| 0 <= index < source.len() && keep(#[trigger] source[index]) && project(source[index]) == key;
        assert(source[witness].lineitem_2 == db.lineitem[witness]);
        lemma_sum_pos(db.lineitem, nw(key), witness);
    } else if sum(db.lineitem, nw(key)) > 0 && sum(db.lineitem, qw(key)) > parameters.p1 {
        let witness = lemma_sum_nonzero_witness(db.lineitem, nw(key));
        assert(source[witness].lineitem_2 == db.lineitem[witness]);
        assert(keep(source[witness]));
        assert(project(source[witness]) == key);
        assert(selected.contains(key));
        assert(keys.contains(key));
        let position = choose|index: int| 0 <= index < keys.len() && #[trigger] keys[index] == key;
        assert(in_1_rows(db, parameters)[position].l_orderkey == key);
    }
}

pub open spec fn tuple_weight(name: Seq<u8>, customer: int, key: int, date: int, price: int, weighted: bool) -> spec_fn(Tuple) -> int {
    |tuple: Tuple| if tuple.customer.c_name == name && tuple.customer.c_custkey == customer &&
        tuple.orders.o_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_totalprice == price {
        if weighted { tuple.lineitem.l_quantity } else { 1int }
    } else { 0 }
}

pub proof fn join_factor(db: Db, parameters: Params, name: Seq<u8>, customer: int, key: int, date: int, price: int, weighted: bool)
    ensures sum(tuples(db, parameters), tuple_weight(name, customer, key, date, price, weighted)) ==
        if in_1(db, parameters, key) {
            sum(db.lineitem, if weighted { qw(key) } else { nw(key) }) *
                sum(db.orders, ow(key, customer, date, price)) * sum(db.customer, cw(customer, name))
        } else { 0 },
{
    let weight = tuple_weight(name, customer, key, date, price, weighted);
    let line_weight = if weighted { qw(key) } else { nw(key) };
    let quantity = sum(db.lineitem, line_weight);
    let order_weight = ow(key, customer, date, price);
    let customer_weight = cw(customer, name);
    assert forall|person: CustomerRow, order: OrdersRow|
        #[trigger] sum(tuples_2(db, parameters, person, order), weight) ==
            if customer_weight(person) == 1 && order_weight(order) == 1 && in_1(db, parameters, key) { quantity } else { 0 } by {
        let expand = |line: LineitemRow| tuples_3(db, parameters, person, order, line);
        let inner = |line: LineitemRow| sum(expand(line), weight);
        let scale = if customer_weight(person) == 1 && order_weight(order) == 1 && in_1(db, parameters, key) { 1int } else { 0int };
        lemma_sum_flat_map(db.lineitem, expand, weight);
        assert forall|line: LineitemRow| #[trigger] inner(line) == scale * line_weight(line) by {
            lemma_sum_opt(where_clause(db, parameters, person, order, line), Tuple { customer: person, orders: order, lineitem: line }, weight);
        }
        lemma_sum_scale(db.lineitem, line_weight, inner, scale);
    }
    assert forall|person: CustomerRow| #[trigger] sum(tuples_1(db, parameters, person), weight) ==
        customer_weight(person) * (if in_1(db, parameters, key) { quantity * sum(db.orders, order_weight) } else { 0 }) by {
        let expand = |order: OrdersRow| tuples_2(db, parameters, person, order);
        let inner = |order: OrdersRow| sum(expand(order), weight);
        let scale = if customer_weight(person) == 1 && in_1(db, parameters, key) { quantity } else { 0int };
        lemma_sum_flat_map(db.orders, expand, weight);
        assert forall|order: OrdersRow| #[trigger] inner(order) == scale * order_weight(order) by {}
        lemma_sum_scale(db.orders, order_weight, inner, scale);
    }
    let expand = |person: CustomerRow| tuples_1(db, parameters, person);
    let inner = |person: CustomerRow| sum(expand(person), weight);
    let scale = if in_1(db, parameters, key) { quantity * sum(db.orders, order_weight) } else { 0int };
    lemma_sum_flat_map(db.customer, expand, weight);
    assert forall|person: CustomerRow| #[trigger] inner(person) == scale * customer_weight(person) by {
        assert(customer_weight(person) * scale == scale * customer_weight(person)) by (nonlinear_arith);
    }
    lemma_sum_scale(db.customer, customer_weight, inner, scale);
}

pub proof fn execute_facts(db: Db, parameters: Params, name: Seq<u8>, customer: int, key: int, date: int, price: int)
    ensures
        group_count(db, parameters, name, customer, key, date, price) ==
            if sum(db.lineitem, nw(key)) > 0 && sum(db.lineitem, qw(key)) > parameters.p1 {
                sum(db.lineitem, nw(key)) * sum(db.orders, ow(key, customer, date, price)) * sum(db.customer, cw(customer, name))
            } else { 0 },
        group_sum(db, parameters, name, customer, key, date, price, |tuple: Tuple| tuple.lineitem.l_quantity) ==
            if sum(db.lineitem, nw(key)) > 0 && sum(db.lineitem, qw(key)) > parameters.p1 {
                sum(db.lineitem, qw(key)) * sum(db.orders, ow(key, customer, date, price)) * sum(db.customer, cw(customer, name))
            } else { 0 },
{
    subquery_membership(db, parameters, key);
    join_factor(db, parameters, name, customer, key, date, price, false);
    join_factor(db, parameters, name, customer, key, date, price, true);
    lemma_sum_ext_all(tuples(db, parameters),
        |tuple: Tuple| if tuple.customer.c_name == name && tuple.customer.c_custkey == customer &&
            tuple.orders.o_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_totalprice == price { 1int } else { 0 },
        tuple_weight(name, customer, key, date, price, false));
    lemma_sum_ext_all(tuples(db, parameters),
        |tuple: Tuple| if tuple.customer.c_name == name && tuple.customer.c_custkey == customer &&
            tuple.orders.o_orderkey == key && tuple.orders.o_orderdate == date && tuple.orders.o_totalprice == price { (|source: Tuple| source.lineitem.l_quantity)(tuple) } else { 0 },
        tuple_weight(name, customer, key, date, price, true));
}

}
