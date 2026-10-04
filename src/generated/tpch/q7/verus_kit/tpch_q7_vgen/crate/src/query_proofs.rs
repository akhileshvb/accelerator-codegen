use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::sqlfns::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::params;

verus! {

pub open spec fn selected(p: Params, first: Seq<u8>, second: Seq<u8>) -> bool {
    (first == p.p1 && second == p.p2) || (first == p.p2 && second == p.p1)
}

pub open spec fn shipping_weight(first: Seq<u8>, second: Seq<u8>, year: int, revenue: bool)
    -> spec_fn(ShippingTuple) -> int
{
    |tuple: ShippingTuple| if tuple.n1.n_name == first && tuple.n2.n_name == second
        && civil_year(tuple.lineitem.l_shipdate) == year {
        if revenue { tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) } else { 1int }
    } else { 0int }
}

pub proof fn lemma_group_shipping(db: Db, p: Params, first: Seq<u8>, second: Seq<u8>, year: int, revenue: bool)
    ensures
        sum(shipping_tuples(db, p), shipping_weight(first, second, year, revenue))
            == if revenue { row_of(db, p, first, second, year).revenue }
                else { group_count(db, p, first, second, year) },
{
    let tuple_weight = |tuple: Tuple| if tuple.shipping.supp_nation == first
        && tuple.shipping.cust_nation == second && tuple.shipping.l_year == year {
        if revenue { tuple.shipping.volume } else { 1int }
    } else { 0int };
    let row_weight = |row: ShippingRow| if row.supp_nation == first
        && row.cust_nation == second && row.l_year == year {
        if revenue { row.volume } else { 1int }
    } else { 0int };
    let tuple_of = |row: ShippingRow| Tuple { shipping: row };
    assert((|row: ShippingRow| tuples_1(db, p, row)) =~= (|row: ShippingRow| seq![tuple_of(row)]));
    lemma_flat_singleton_map(shipping_rows(db, p), tuple_of);
    assert(tuples(db, p) == shipping_rows(db, p).map_values(tuple_of));
    lemma_sum_map(shipping_rows(db, p), tuple_of, tuple_weight, row_weight);
    let row_of_tuple = |tuple: ShippingTuple| ShippingRow {
        supp_nation: tuple.n1.n_name, cust_nation: tuple.n2.n_name,
        l_year: civil_year(tuple.lineitem.l_shipdate),
        volume: tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount),
    };
    lemma_sum_map(shipping_tuples(db, p), row_of_tuple, row_weight,
        shipping_weight(first, second, year, revenue));
    assert(shipping_rows(db, p) == shipping_tuples(db, p).map_values(row_of_tuple));
    assert(sum(shipping_tuples(db, p), shipping_weight(first, second, year, revenue)) == sum(shipping_rows(db, p), row_weight));
    assert(sum(shipping_rows(db, p), row_weight) == sum(tuples(db, p), tuple_weight));
    if revenue {
        let value = |tuple: Tuple| tuple.shipping.volume;
        let grouped = |tuple: Tuple| if tuple.shipping.supp_nation == first
            && tuple.shipping.cust_nation == second && tuple.shipping.l_year == year { value(tuple) } else { 0int };
        lemma_sum_ext_all(tuples(db, p), tuple_weight, grouped);
        assert(group_sum(db, p, first, second, year, value) == sum(tuples(db, p), grouped));
        assert(row_of(db, p, first, second, year).revenue == group_sum(db, p, first, second, year, value));
        assert(tuple_weight =~= (|tuple: Tuple| if tuple.shipping.supp_nation == first
            && tuple.shipping.cust_nation == second && tuple.shipping.l_year == year {
            tuple.shipping.volume } else { 0int }));
        assert(sum(tuples(db, p), tuple_weight) == row_of(db, p, first, second, year).revenue);
    } else {
        assert(tuple_weight =~= (|tuple: Tuple| if tuple.shipping.supp_nation == first
            && tuple.shipping.cust_nation == second && tuple.shipping.l_year == year { 1int } else { 0int }));
        assert(sum(tuples(db, p), tuple_weight) == group_count(db, p, first, second, year));
    }
}

proof fn lemma_parameter6(db: Db, p: Params, supplier: SupplierRow, line: LineitemRow,
    order: OrdersRow, customer: CustomerRow, nation: NationRow, other: NationRow,
    first: Seq<u8>, second: Seq<u8>, year: int, revenue: bool)
    ensures sum(shipping_tuples_6(db, p, supplier, line, order, customer, nation, other), shipping_weight(first, second, year, revenue))
        == (if selected(p, first, second) { 1int } else { 0int })
            * sum(shipping_tuples_6(db, params(first, second), supplier, line, order, customer, nation, other), shipping_weight(first, second, year, revenue)),
{
    let tuple = ShippingTuple { supplier, lineitem: line, orders: order, customer, n1: nation, n2: other };
    let weight = shipping_weight(first, second, year, revenue);
    lemma_sum_opt(shipping_where_clause(db, p, supplier, line, order, customer, nation, other), tuple, weight);
    lemma_sum_opt(shipping_where_clause(db, params(first, second), supplier, line, order, customer, nation, other), tuple, weight);
}

proof fn lemma_parameter5(db: Db, p: Params, supplier: SupplierRow, line: LineitemRow,
    order: OrdersRow, customer: CustomerRow, nation: NationRow,
    first: Seq<u8>, second: Seq<u8>, year: int, revenue: bool)
    ensures sum(shipping_tuples_5(db, p, supplier, line, order, customer, nation), shipping_weight(first, second, year, revenue))
        == (if selected(p, first, second) { 1int } else { 0int })
            * sum(shipping_tuples_5(db, params(first, second), supplier, line, order, customer, nation), shipping_weight(first, second, year, revenue)),
{
    let weight = shipping_weight(first, second, year, revenue);
    let left = |other: NationRow| shipping_tuples_6(db, p, supplier, line, order, customer, nation, other);
    let right = |other: NationRow| shipping_tuples_6(db, params(first, second), supplier, line, order, customer, nation, other);
    assert forall|other: NationRow| #[trigger] sum(left(other), weight)
        == (if selected(p, first, second) { 1int } else { 0int }) * sum(right(other), weight) by {
        lemma_parameter6(db, p, supplier, line, order, customer, nation, other, first, second, year, revenue);
    }
    lemma_flat_map_weight_equal(db.nation, left, right, weight, if selected(p, first, second) { 1int } else { 0int });
}

proof fn lemma_parameter4(db: Db, p: Params, supplier: SupplierRow, line: LineitemRow,
    order: OrdersRow, customer: CustomerRow, first: Seq<u8>, second: Seq<u8>, year: int, revenue: bool)
    ensures sum(shipping_tuples_4(db, p, supplier, line, order, customer), shipping_weight(first, second, year, revenue))
        == (if selected(p, first, second) { 1int } else { 0int })
            * sum(shipping_tuples_4(db, params(first, second), supplier, line, order, customer), shipping_weight(first, second, year, revenue)),
{
    let weight = shipping_weight(first, second, year, revenue);
    let left = |nation: NationRow| shipping_tuples_5(db, p, supplier, line, order, customer, nation);
    let right = |nation: NationRow| shipping_tuples_5(db, params(first, second), supplier, line, order, customer, nation);
    assert forall|nation: NationRow| #[trigger] sum(left(nation), weight)
        == (if selected(p, first, second) { 1int } else { 0int }) * sum(right(nation), weight) by {
        lemma_parameter5(db, p, supplier, line, order, customer, nation, first, second, year, revenue);
    }
    lemma_flat_map_weight_equal(db.nation, left, right, weight, if selected(p, first, second) { 1int } else { 0int });
}

proof fn lemma_parameter3(db: Db, p: Params, supplier: SupplierRow, line: LineitemRow,
    order: OrdersRow, first: Seq<u8>, second: Seq<u8>, year: int, revenue: bool)
    ensures sum(shipping_tuples_3(db, p, supplier, line, order), shipping_weight(first, second, year, revenue))
        == (if selected(p, first, second) { 1int } else { 0int })
            * sum(shipping_tuples_3(db, params(first, second), supplier, line, order), shipping_weight(first, second, year, revenue)),
{
    let weight = shipping_weight(first, second, year, revenue);
    let left = |customer: CustomerRow| shipping_tuples_4(db, p, supplier, line, order, customer);
    let right = |customer: CustomerRow| shipping_tuples_4(db, params(first, second), supplier, line, order, customer);
    assert forall|customer: CustomerRow| #[trigger] sum(left(customer), weight)
        == (if selected(p, first, second) { 1int } else { 0int }) * sum(right(customer), weight) by {
        lemma_parameter4(db, p, supplier, line, order, customer, first, second, year, revenue);
    }
    lemma_flat_map_weight_equal(db.customer, left, right, weight, if selected(p, first, second) { 1int } else { 0int });
}

proof fn lemma_parameter2(db: Db, p: Params, supplier: SupplierRow, line: LineitemRow,
    first: Seq<u8>, second: Seq<u8>, year: int, revenue: bool)
    ensures sum(shipping_tuples_2(db, p, supplier, line), shipping_weight(first, second, year, revenue))
        == (if selected(p, first, second) { 1int } else { 0int })
            * sum(shipping_tuples_2(db, params(first, second), supplier, line), shipping_weight(first, second, year, revenue)),
{
    let weight = shipping_weight(first, second, year, revenue);
    let left = |order: OrdersRow| shipping_tuples_3(db, p, supplier, line, order);
    let right = |order: OrdersRow| shipping_tuples_3(db, params(first, second), supplier, line, order);
    assert forall|order: OrdersRow| #[trigger] sum(left(order), weight)
        == (if selected(p, first, second) { 1int } else { 0int }) * sum(right(order), weight) by {
        lemma_parameter3(db, p, supplier, line, order, first, second, year, revenue);
    }
    lemma_flat_map_weight_equal(db.orders, left, right, weight, if selected(p, first, second) { 1int } else { 0int });
}

proof fn lemma_parameter1(db: Db, p: Params, supplier: SupplierRow,
    first: Seq<u8>, second: Seq<u8>, year: int, revenue: bool)
    ensures sum(shipping_tuples_1(db, p, supplier), shipping_weight(first, second, year, revenue))
        == (if selected(p, first, second) { 1int } else { 0int })
            * sum(shipping_tuples_1(db, params(first, second), supplier), shipping_weight(first, second, year, revenue)),
{
    let weight = shipping_weight(first, second, year, revenue);
    let left = |line: LineitemRow| shipping_tuples_2(db, p, supplier, line);
    let right = |line: LineitemRow| shipping_tuples_2(db, params(first, second), supplier, line);
    assert forall|line: LineitemRow| #[trigger] sum(left(line), weight)
        == (if selected(p, first, second) { 1int } else { 0int }) * sum(right(line), weight) by {
        lemma_parameter2(db, p, supplier, line, first, second, year, revenue);
    }
    lemma_flat_map_weight_equal(db.lineitem, left, right, weight, if selected(p, first, second) { 1int } else { 0int });
}

pub proof fn lemma_group_parameter(db: Db, p: Params, first: Seq<u8>, second: Seq<u8>, year: int)
    ensures
        group_count(db, p, first, second, year) == if selected(p, first, second) {
            group_count(db, params(first, second), first, second, year) } else { 0int },
        row_of(db, p, first, second, year).revenue == if selected(p, first, second) {
            row_of(db, params(first, second), first, second, year).revenue } else { 0int },
{
    assert forall|revenue: bool|
        #![trigger sum(shipping_tuples(db, p), shipping_weight(first, second, year, revenue))]
        sum(shipping_tuples(db, p), shipping_weight(first, second, year, revenue))
            == (if selected(p, first, second) { 1int } else { 0int })
                * sum(shipping_tuples(db, params(first, second)), shipping_weight(first, second, year, revenue)) by {
        let weight = shipping_weight(first, second, year, revenue);
        let left = |supplier: SupplierRow| shipping_tuples_1(db, p, supplier);
        let right = |supplier: SupplierRow| shipping_tuples_1(db, params(first, second), supplier);
        assert forall|supplier: SupplierRow| #[trigger] sum(left(supplier), weight)
            == (if selected(p, first, second) { 1int } else { 0int }) * sum(right(supplier), weight) by {
            lemma_parameter1(db, p, supplier, first, second, year, revenue);
        }
        lemma_flat_map_weight_equal(db.supplier, left, right, weight, if selected(p, first, second) { 1int } else { 0int });
    }
    lemma_group_shipping(db, p, first, second, year, false);
    lemma_group_shipping(db, params(first, second), first, second, year, false);
    lemma_group_shipping(db, p, first, second, year, true);
    lemma_group_shipping(db, params(first, second), first, second, year, true);
}

pub proof fn lemma_window_year(date: int)
    requires 9131 <= date <= 9861,
    ensures civil_year(date) == if date <= 9495 { 1995int } else { 1996int },
{
    assert((date + 719468) / 146097 == 4);
    assert(civil_doe(date) == date + 719468 - 4 * 146097);
    if date <= 9189 {
        assert(civil_yoe(date) == 394);
        assert(civil_month(date) <= 2);
    } else if date <= 9555 {
        assert(civil_yoe(date) == 395);
        if date <= 9495 {
            assert(civil_month(date) > 2);
        } else {
            assert(civil_month(date) <= 2);
        }
    } else {
        assert(civil_yoe(date) == 396);
        assert(civil_month(date) > 2);
    }
}

pub proof fn lemma_shipping_member(db: Db, p: Params, tuple: ShippingTuple)
    requires shipping_tuples(db, p).contains(tuple),
    ensures shipping_where_clause(db, p, tuple.supplier, tuple.lineitem, tuple.orders,
        tuple.customer, tuple.n1, tuple.n2),
{
    lemma_flat_map_contains(db.supplier, |supplier: SupplierRow| shipping_tuples_1(db, p, supplier), tuple);
    let supplier_index = choose|index: int| 0 <= index < db.supplier.len()
        && #[trigger] shipping_tuples_1(db, p, db.supplier[index]).contains(tuple);
    let supplier = db.supplier[supplier_index];
    lemma_flat_map_contains(db.lineitem, |line: LineitemRow| shipping_tuples_2(db, p, supplier, line), tuple);
    let line_index = choose|index: int| 0 <= index < db.lineitem.len()
        && #[trigger] shipping_tuples_2(db, p, supplier, db.lineitem[index]).contains(tuple);
    let line = db.lineitem[line_index];
    lemma_flat_map_contains(db.orders, |order: OrdersRow| shipping_tuples_3(db, p, supplier, line, order), tuple);
    let order_index = choose|index: int| 0 <= index < db.orders.len()
        && #[trigger] shipping_tuples_3(db, p, supplier, line, db.orders[index]).contains(tuple);
    let order = db.orders[order_index];
    lemma_flat_map_contains(db.customer, |customer: CustomerRow| shipping_tuples_4(db, p, supplier, line, order, customer), tuple);
    let customer_index = choose|index: int| 0 <= index < db.customer.len()
        && #[trigger] shipping_tuples_4(db, p, supplier, line, order, db.customer[index]).contains(tuple);
    let customer = db.customer[customer_index];
    lemma_flat_map_contains(db.nation, |nation: NationRow| shipping_tuples_5(db, p, supplier, line, order, customer, nation), tuple);
    let nation_index = choose|index: int| 0 <= index < db.nation.len()
        && #[trigger] shipping_tuples_5(db, p, supplier, line, order, customer, db.nation[index]).contains(tuple);
    let nation = db.nation[nation_index];
    lemma_flat_map_contains(db.nation, |other: NationRow| shipping_tuples_6(db, p, supplier, line, order, customer, nation, other), tuple);
    let other_index = choose|index: int| 0 <= index < db.nation.len()
        && #[trigger] shipping_tuples_6(db, p, supplier, line, order, customer, nation, db.nation[index]).contains(tuple);
    let other = db.nation[other_index];
    assert(shipping_where_clause(db, p, supplier, line, order, customer, nation, other));
    assert(tuple == ShippingTuple { supplier, lineitem: line, orders: order, customer, n1: nation, n2: other });
}

pub proof fn lemma_group_support(db: Db, p: Params, first: Seq<u8>, second: Seq<u8>, year: int)
    requires group_count(db, p, first, second, year) > 0,
    ensures selected(p, first, second), year == 1995 || year == 1996,
{
    let position = lemma_sum_nonzero_witness(tuples(db, p), |tuple: Tuple|
        if tuple.shipping.supp_nation == first && tuple.shipping.cust_nation == second
            && tuple.shipping.l_year == year { 1int } else { 0int });
    let tuple_of = |row: ShippingRow| Tuple { shipping: row };
    assert((|row: ShippingRow| tuples_1(db, p, row)) =~= (|row: ShippingRow| seq![tuple_of(row)]));
    lemma_flat_singleton_map(shipping_rows(db, p), tuple_of);
    assert(tuples(db, p) == shipping_rows(db, p).map_values(tuple_of));
    let shipping = shipping_tuples(db, p)[position];
    assert(shipping_tuples(db, p).contains(shipping));
    lemma_shipping_member(db, p, shipping);
    assert(shipping.n1.n_name == first && shipping.n2.n_name == second);
    assert(civil_year(shipping.lineitem.l_shipdate) == year);
    lemma_window_year(shipping.lineitem.l_shipdate);
}

}
