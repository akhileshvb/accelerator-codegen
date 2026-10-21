use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::table::*;
use crate::query_product::*;

verus! {
#[verifier::opaque]
pub open spec fn original_line_sum(db: Db, part: PartRow, supplier: SupplierRow, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool) -> int {
    sum(db.lineitem, |line: LineitemRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume))
}

#[verifier::opaque]
pub open spec fn original_supplier_sum(db: Db, part: PartRow, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool) -> int {
    sum(db.supplier, |supplier: SupplierRow| original_line_sum(db, part, supplier, route, parameters, year, numerator, volume))
}

#[verifier::opaque]
pub open spec fn original_sum(db: Db, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool) -> int {
    sum(db.part, |part: PartRow| original_supplier_sum(db, part, route, parameters, year, numerator, volume))
}

pub proof fn lemma_core_raw_5(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, order: OrdersRow, customer: CustomerRow, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires year == 1995 || year == 1996,
    ensures core_sum_5(db, part, supplier, line, order, customer, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume))) == core_term(CoreTuple { part, supplier, lineitem: line, orders: order, customer }, route, parameters, year, numerator, volume),
{
    hide(sum);
    reveal(core_sum_5);
    lemma_core_term(db, CoreTuple { part, supplier, lineitem: line, orders: order, customer }, route, parameters, year, numerator, volume);
}

pub proof fn lemma_core_raw_4(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, order: OrdersRow, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires year == 1995 || year == 1996,
    ensures core_sum_4(db, part, supplier, line, order, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume))) == customer_sum(db, part, supplier, line, order, route, parameters, year, numerator, volume),
{
    hide(sum);
    reveal(core_sum_4);
    reveal(customer_sum);
    let left = |customer: CustomerRow| core_sum_5(db, part, supplier, line, order, customer, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume)));
    let right = |customer: CustomerRow| core_term(CoreTuple { part, supplier, lineitem: line, orders: order, customer }, route, parameters, year, numerator, volume);
    assert forall|customer: CustomerRow| #[trigger] left(customer) == right(customer) by {
        lemma_core_raw_5(db, part, supplier, line, order, customer, route, parameters, year, numerator, volume);
    }
    lemma_sum_ext_all(db.customer, left, right);
}

pub proof fn lemma_core_raw_3(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires year == 1995 || year == 1996,
    ensures core_sum_3(db, part, supplier, line, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume))) == order_sum(db, part, supplier, line, route, parameters, year, numerator, volume),
{
    hide(sum);
    reveal(core_sum_3);
    reveal(order_sum);
    let left = |order: OrdersRow| core_sum_4(db, part, supplier, line, order, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume)));
    let right = |order: OrdersRow| customer_sum(db, part, supplier, line, order, route, parameters, year, numerator, volume);
    assert forall|order: OrdersRow| #[trigger] left(order) == right(order) by {
        lemma_core_raw_4(db, part, supplier, line, order, route, parameters, year, numerator, volume);
    }
    lemma_sum_ext_all(db.orders, left, right);
}

pub proof fn lemma_core_raw_2(db: Db, part: PartRow, supplier: SupplierRow, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires year == 1995 || year == 1996,
    ensures core_sum_2(db, part, supplier, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume))) == original_line_sum(db, part, supplier, route, parameters, year, numerator, volume),
{
    hide(sum);
    reveal(core_sum_2);
    reveal(original_line_sum);
    let left = |line: LineitemRow| core_sum_3(db, part, supplier, line, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume)));
    let right = |line: LineitemRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume);
    assert forall|line: LineitemRow| #[trigger] left(line) == right(line) by {
        lemma_core_raw_3(db, part, supplier, line, route, parameters, year, numerator, volume);
    }
    lemma_sum_ext_all(db.lineitem, left, right);
}

pub proof fn lemma_core_raw_1(db: Db, part: PartRow, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires year == 1995 || year == 1996,
    ensures core_sum_1(db, part, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume))) == original_supplier_sum(db, part, route, parameters, year, numerator, volume),
{
    hide(sum);
    reveal(core_sum_1);
    reveal(original_supplier_sum);
    let left = |supplier: SupplierRow| core_sum_2(db, part, supplier, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume)));
    let right = |supplier: SupplierRow| original_line_sum(db, part, supplier, route, parameters, year, numerator, volume);
    assert forall|supplier: SupplierRow| #[trigger] left(supplier) == right(supplier) by {
        lemma_core_raw_2(db, part, supplier, route, parameters, year, numerator, volume);
    }
    lemma_sum_ext_all(db.supplier, left, right);
}

pub proof fn lemma_core_raw_0(db: Db, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires year == 1995 || year == 1996,
    ensures core_sum_0(db, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume))) == original_sum(db, route, parameters, year, numerator, volume),
{
    hide(sum);
    reveal(core_sum_0);
    reveal(original_sum);
    let left = |part: PartRow| core_sum_1(db, part, |core: CoreTuple| joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume)));
    let right = |part: PartRow| original_supplier_sum(db, part, route, parameters, year, numerator, volume);
    assert forall|part: PartRow| #[trigger] left(part) == right(part) by {
        lemma_core_raw_1(db, part, route, parameters, year, numerator, volume);
    }
    lemma_sum_ext_all(db.part, left, right);
}


pub open spec fn query_weight(parameters: Params, numerator: bool, volume: bool) -> spec_fn(Tuple) -> int {
    |tuple: Tuple| if !numerator || tuple.all_nations.nation == parameters.p1 {
        if volume { tuple.all_nations.volume } else { 1int }
    } else { 0int }
}

pub open spec fn route_active(route: RouteTuple, parameters: Params, numerator: bool) -> bool {
    route.n1.n_regionkey == route.region.r_regionkey && route.region.r_name == parameters.p2
        && (!numerator || route.n2.n_name == parameters.p1)
}

pub open spec fn part_active(part: PartRow, line: LineitemRow, parameters: Params) -> bool {
    part.p_partkey == line.l_partkey && part.p_type == parameters.p3
}

pub open spec fn supplier_active(supplier: SupplierRow, line: LineitemRow, route: RouteTuple) -> bool {
    supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == route.n2.n_nationkey
}

pub open spec fn order_active(order: OrdersRow, line: LineitemRow, year: int) -> bool {
    order.o_orderkey == line.l_orderkey && year_of(order.o_orderdate) == year && year != 0
}

pub open spec fn core_term(core: CoreTuple, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool) -> int {
    if route_active(route, parameters, numerator) && part_active(core.part, core.lineitem, parameters)
        && supplier_active(core.supplier, core.lineitem, route) && order_active(core.orders, core.lineitem, year)
        && core.customer.c_custkey == core.orders.o_custkey && core.customer.c_nationkey == route.n1.n_nationkey {
        line_measure(core.lineitem, volume)
    } else { 0 }
}

pub proof fn lemma_core_term(db: Db, core: CoreTuple, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires year == 1995 || year == 1996,
    ensures joined_weight(db, core, route, parameters, year, query_weight(parameters, numerator, volume)) ==
        core_term(core, route, parameters, year, numerator, volume),
{
    if 9131 <= core.orders.o_orderdate <= 9861 {
        crate::sql_bridge::lemma_query_year(core.orders.o_orderdate);
    }
}

#[verifier::opaque]
pub open spec fn customer_sum(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, order: OrdersRow,
    route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool) -> int {
    sum(db.customer, |customer: CustomerRow| core_term(CoreTuple { part, supplier, lineitem: line, orders: order, customer },
        route, parameters, year, numerator, volume))
}

#[verifier::opaque]
pub open spec fn order_sum(db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow,
    route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool) -> int {
    sum(db.orders, |order: OrdersRow| customer_sum(db, part, supplier, line, order, route, parameters, year, numerator, volume))
}

#[verifier::opaque]
pub open spec fn raw_line(db: Db, line: LineitemRow, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool) -> int {
    sum(db.part, |part: PartRow| sum(db.supplier, |supplier: SupplierRow|
        order_sum(db, part, supplier, line, route, parameters, year, numerator, volume)))
}

pub proof fn lemma_route_bounds(data: Data, db: Db, route: RouteTuple)
    requires dimension_meaning(data, db), route_in_db(db, route),
    ensures i64::MIN <= route.n1.n_nationkey <= i64::MAX, i64::MIN <= route.n2.n_nationkey <= i64::MAX,
{
    reveal(dimension_meaning);
    let first = choose|index: int| 0 <= index < db.nation.len() && #[trigger] db.nation[index] == route.n1;
    let second = choose|index: int| 0 <= index < db.nation.len() && #[trigger] db.nation[index] == route.n2;
    assert(nation_represented(db.nation[first]));
    assert(nation_represented(db.nation[second]));
}

pub proof fn lemma_customer_factor(data: Data, db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow, order: OrdersRow,
    route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires dimension_meaning(data, db), db.orders.contains(order), i64::MIN <= route.n1.n_nationkey <= i64::MAX,
    ensures customer_sum(db, part, supplier, line, order, route, parameters, year, numerator, volume) ==
        if route_active(route, parameters, numerator) && part_active(part, line, parameters) && supplier_active(supplier, line, route)
            && order_active(order, line, year) {
            line_measure(line, volume) * data.customers.at((order.o_custkey as i64, 0, 0), (route.n1.n_nationkey as i64, 0, 0), false)
        } else { 0 },
{
    hide(sum);
    reveal(customer_sum);
    let raw = |customer: CustomerRow| core_term(CoreTuple { part, supplier, lineitem: line, orders: order, customer },
        route, parameters, year, numerator, volume);
    let active = route_active(route, parameters, numerator) && part_active(part, line, parameters) && supplier_active(supplier, line, route)
        && order_active(order, line, year);
    if active {
        reveal(dimension_meaning);
        let index = choose|index: int| 0 <= index < db.orders.len() && #[trigger] db.orders[index] == order;
        assert(order_represented(db.orders[index]));
        assert(i64::MIN <= order.o_custkey <= i64::MAX);
        let count = |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == route.n1.n_nationkey { 1int } else { 0int };
        assert forall|customer: CustomerRow| #[trigger] raw(customer) == line_measure(line, volume) * count(customer) by {}
        lemma_sum_scale(db.customer, count, raw, line_measure(line, volume));
        assert(data.customers.at((order.o_custkey as i64, 0, 0), (route.n1.n_nationkey as i64, 0, 0), false) == sum(db.customer, count));
    } else {
        lemma_sum_zero(db.customer, raw);
    }
}

pub open spec fn database_order_weight(data: Data, line: LineitemRow, route: RouteTuple, year: int) -> spec_fn(OrdersRow) -> int {
    |order: OrdersRow| if order_active(order, line, year) {
        data.customers.at((order.o_custkey as i64, 0, 0), (route.n1.n_nationkey as i64, 0, 0), false)
    } else { 0int }
}

pub proof fn lemma_order_factor(data: Data, db: Db, part: PartRow, supplier: SupplierRow, line: LineitemRow,
    route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires dimension_meaning(data, db), i64::MIN <= route.n1.n_nationkey <= i64::MAX,
    ensures order_sum(db, part, supplier, line, route, parameters, year, numerator, volume) ==
        (if route_active(route, parameters, numerator) && part_active(part, line, parameters) && supplier_active(supplier, line, route) {
            line_measure(line, volume)
        } else { 0int }) * sum(db.orders, database_order_weight(data, line, route, year)),
{
    hide(sum);
    reveal(order_sum);
    let coefficient = if route_active(route, parameters, numerator) && part_active(part, line, parameters) && supplier_active(supplier, line, route) {
        line_measure(line, volume)
    } else { 0int };
    let raw = |order: OrdersRow| customer_sum(db, part, supplier, line, order, route, parameters, year, numerator, volume);
    let count = database_order_weight(data, line, route, year);
    let scaled = |order: OrdersRow| coefficient * count(order);
    assert forall|index: int| 0 <= index < db.orders.len() implies raw(#[trigger] db.orders[index]) == scaled(db.orders[index]) by {
        assert(db.orders.contains(db.orders[index]));
        lemma_customer_factor(data, db, part, supplier, line, db.orders[index], route, parameters, year, numerator, volume);
    }
    lemma_sum_ext(db.orders, raw, scaled);
    lemma_sum_scale(db.orders, count, scaled, coefficient);
}

pub proof fn lemma_supplier_factor(data: Data, db: Db, part: PartRow, line: LineitemRow,
    route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires dimension_meaning(data, db), i64::MIN <= route.n1.n_nationkey <= i64::MAX,
        i64::MIN <= route.n2.n_nationkey <= i64::MAX, i64::MIN <= line.l_suppkey <= i64::MAX,
    ensures sum(db.supplier, |supplier: SupplierRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume)) ==
        (if route_active(route, parameters, numerator) && part_active(part, line, parameters) { line_measure(line, volume) } else { 0int })
        * sum(db.orders, database_order_weight(data, line, route, year))
        * data.suppliers.at((line.l_suppkey as i64, 0, 0), (route.n2.n_nationkey as i64, 0, 0), false),
{
    hide(sum);
    reveal(dimension_meaning);
    let coefficient = (if route_active(route, parameters, numerator) && part_active(part, line, parameters) { line_measure(line, volume) } else { 0int })
        * sum(db.orders, database_order_weight(data, line, route, year));
    let raw = |supplier: SupplierRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume);
    let count = |supplier: SupplierRow| if supplier_active(supplier, line, route) { 1int } else { 0int };
    let scaled = |supplier: SupplierRow| coefficient * count(supplier);
    assert forall|supplier: SupplierRow| #[trigger] raw(supplier) == scaled(supplier) by {
        lemma_order_factor(data, db, part, supplier, line, route, parameters, year, numerator, volume);
    }
    lemma_sum_ext_all(db.supplier, raw, scaled);
    lemma_sum_scale(db.supplier, count, scaled, coefficient);
    let stored = |supplier: SupplierRow| if supplier.s_suppkey == line.l_suppkey as i64 as int
        && supplier.s_nationkey == route.n2.n_nationkey as i64 as int { 1int } else { 0int };
    lemma_sum_ext_all(db.supplier, count, stored);
    assert(data.suppliers.at((line.l_suppkey as i64, 0, 0), (route.n2.n_nationkey as i64, 0, 0), false) == sum(db.supplier, count));
}

pub proof fn lemma_represented_line(data: Data, db: Db, line: LineitemRow, route: RouteTuple,
    parameters: Params, kind: i64, year: i64, numerator: bool, volume: bool)
    requires dimension_meaning(data, db), line_represented(line), route_in_db(db, route),
        data.dictionary.types.known(kind), data.dictionary.types.text(kind) == parameters.p3,
    ensures raw_line(db, line, route, parameters, year as int, numerator, volume) ==
        if route_active(route, parameters, numerator) {
            line_weight(data, db, line, route.n1.n_nationkey as i64, route.n2.n_nationkey as i64, kind, year) * line_measure(line, volume)
        } else { 0int },
{
    hide(sum);
    reveal(raw_line);
    reveal(dimension_meaning);
    lemma_route_bounds(data, db, route);
    let supplier_count = data.suppliers.at((line.l_suppkey as i64, 0, 0), (route.n2.n_nationkey as i64, 0, 0), false);
    let order_count = sum(db.orders, database_order_weight(data, line, route, year as int));
    let coefficient = if route_active(route, parameters, numerator) { line_measure(line, volume) * order_count * supplier_count } else { 0int };
    let raw = |part: PartRow| sum(db.supplier, |supplier: SupplierRow| order_sum(db, part, supplier, line, route, parameters, year as int, numerator, volume));
    let count = |part: PartRow| if part_active(part, line, parameters) { 1int } else { 0int };
    let scaled = |part: PartRow| coefficient * count(part);
    assert forall|part: PartRow| #[trigger] raw(part) == scaled(part) by {
        lemma_supplier_factor(data, db, part, line, route, parameters, year as int, numerator, volume);
        assert(raw(part) == scaled(part)) by (nonlinear_arith)
            requires raw(part) == (if route_active(route, parameters, numerator) && part_active(part, line, parameters) { line_measure(line, volume) } else { 0int })
                * order_count * supplier_count,
                coefficient == if route_active(route, parameters, numerator) { line_measure(line, volume) * order_count * supplier_count } else { 0int },
                scaled(part) == coefficient * count(part), count(part) == if part_active(part, line, parameters) { 1int } else { 0int };
    }
    lemma_sum_ext_all(db.part, raw, scaled);
    lemma_sum_scale(db.part, count, scaled, coefficient);
    let part_count = data.parts.at((line.l_partkey as i64, 0, 0), (kind, 0, 0), false);
    let stored_parts = |part: PartRow| if part.p_partkey == line.l_partkey as i64 as int && text_matches(data.dictionary, 0, kind, part.p_type) { 1int } else { 0int };
    lemma_sum_ext_all(db.part, count, stored_parts);
    assert(part_count == sum(db.part, count));
    reveal(line_weight);
    let raw_orders = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && year_of(order.o_orderdate) == year as int && year != 0 {
        data.customers.at((order.o_custkey as i64, 0, 0), (route.n1.n_nationkey as i64, 0, 0), false)
    } else { 0int };
    lemma_sum_ext_all(db.orders, database_order_weight(data, line, route, year as int), raw_orders);
    assert(coefficient * part_count == if route_active(route, parameters, numerator) {
        part_count * supplier_count * order_count * line_measure(line, volume)
    } else { 0int }) by (nonlinear_arith)
        requires coefficient == if route_active(route, parameters, numerator) { line_measure(line, volume) * order_count * supplier_count } else { 0int };
}

pub proof fn lemma_excluded_core_line(db: Db, line: LineitemRow, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    requires !scan_lines_exists_1(db, line),
    ensures raw_line(db, line, route, parameters, year, numerator, volume) == 0,
{
    hide(sum);
    reveal(raw_line);
    let per_part = |part: PartRow| sum(db.supplier, |supplier: SupplierRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume));
    assert forall|part: PartRow| #[trigger] per_part(part) == 0 by {
        let per_supplier = |supplier: SupplierRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume);
        assert forall|supplier: SupplierRow| #[trigger] per_supplier(supplier) == 0 by {
            reveal(order_sum);
            let per_order = |order: OrdersRow| customer_sum(db, part, supplier, line, order, route, parameters, year, numerator, volume);
            assert forall|index: int| 0 <= index < db.orders.len() implies per_order(#[trigger] db.orders[index]) == 0 by {
                let order = db.orders[index];
                if order_active(order, line, year) {
                    assert(scan_lines_exists_1_where(line, order));
                    assert(scan_lines_exists_1(db, line));
                }
                reveal(customer_sum);
                lemma_sum_zero(db.customer, |customer: CustomerRow| core_term(CoreTuple { part, supplier, lineitem: line, orders: order, customer },
                    route, parameters, year, numerator, volume));
            }
            lemma_sum_zero(db.orders, per_order);
        }
        lemma_sum_zero(db.supplier, per_supplier);
    }
    lemma_sum_zero(db.part, per_part);
}

pub proof fn lemma_core_reorder(db: Db, route: RouteTuple, parameters: Params, year: int, numerator: bool, volume: bool)
    ensures original_sum(db, route, parameters, year, numerator, volume) ==
        sum(db.lineitem, |line: LineitemRow| raw_line(db, line, route, parameters, year, numerator, volume)),
{
    hide(sum);
    reveal(original_sum);
    let weight = |part: PartRow, supplier: SupplierRow, line: LineitemRow|
        order_sum(db, part, supplier, line, route, parameters, year, numerator, volume);
    let original = |part: PartRow| original_supplier_sum(db, part, route, parameters, year, numerator, volume);
    let target = |supplier: SupplierRow, line: LineitemRow| sum(db.part, |part: PartRow|
        order_sum(db, part, supplier, line, route, parameters, year, numerator, volume));
    assert forall|part: PartRow| #[trigger] original(part) == sum(db.supplier, |supplier: SupplierRow|
        sum(db.lineitem, |line: LineitemRow| weight(part, supplier, line))) by {
        reveal(original_supplier_sum);
        let left = |supplier: SupplierRow| original_line_sum(db, part, supplier, route, parameters, year, numerator, volume);
        let right = |supplier: SupplierRow| sum(db.lineitem, |line: LineitemRow| weight(part, supplier, line));
        assert forall|supplier: SupplierRow| #[trigger] left(supplier) == right(supplier) by {
            reveal(original_line_sum);
            assert((|line: LineitemRow| weight(part, supplier, line)) =~=
                (|line: LineitemRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume)));
        }
        lemma_sum_ext_all(db.supplier, left, right);
    }
    assert forall|supplier: SupplierRow, line: LineitemRow| #[trigger] target(supplier, line) ==
        sum(db.part, |part: PartRow| weight(part, supplier, line)) by {
        assert((|part: PartRow| weight(part, supplier, line)) =~=
            (|part: PartRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume)));
    }
    lemma_sum_rotate_named(db.part, db.supplier, db.lineitem, weight, original, target);
    let middle = |supplier: SupplierRow| sum(db.lineitem, |line: LineitemRow| target(supplier, line));
    let second_weight = |supplier: SupplierRow, line: LineitemRow, part: PartRow|
        order_sum(db, part, supplier, line, route, parameters, year, numerator, volume);
    let final_target = |line: LineitemRow, part: PartRow| sum(db.supplier, |supplier: SupplierRow|
        order_sum(db, part, supplier, line, route, parameters, year, numerator, volume));
    assert forall|supplier: SupplierRow| #[trigger] middle(supplier) == sum(db.lineitem, |line: LineitemRow|
        sum(db.part, |part: PartRow| second_weight(supplier, line, part))) by {
        let left = |line: LineitemRow| target(supplier, line);
        let right = |line: LineitemRow| sum(db.part, |part: PartRow| second_weight(supplier, line, part));
        assert forall|line: LineitemRow| #[trigger] left(line) == right(line) by {
            assert((|part: PartRow| second_weight(supplier, line, part)) =~=
                (|part: PartRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume)));
        }
        lemma_sum_ext_all(db.lineitem, left, right);
    }
    assert forall|line: LineitemRow, part: PartRow| #[trigger] final_target(line, part) ==
        sum(db.supplier, |supplier: SupplierRow| second_weight(supplier, line, part)) by {
        assert((|supplier: SupplierRow| second_weight(supplier, line, part)) =~=
            (|supplier: SupplierRow| order_sum(db, part, supplier, line, route, parameters, year, numerator, volume)));
    }
    lemma_sum_rotate_named(db.supplier, db.lineitem, db.part, second_weight, middle, final_target);
    let final_rows = |line: LineitemRow| sum(db.part, |part: PartRow| final_target(line, part));
    let expected = |line: LineitemRow| raw_line(db, line, route, parameters, year, numerator, volume);
    assert forall|line: LineitemRow| #[trigger] final_rows(line) == expected(line) by {
        reveal(raw_line);
        assert((|part: PartRow| final_target(line, part)) =~= (|part: PartRow| sum(db.supplier, |supplier: SupplierRow|
            order_sum(db, part, supplier, line, route, parameters, year, numerator, volume))));
    }
    lemma_sum_ext_all(db.lineitem, final_rows, expected);
}

pub proof fn lemma_core_base(data: Data, db: Db, route: RouteTuple, parameters: Params, kind: i64, year: i64, numerator: bool, volume: bool)
    requires dimension_meaning(data, db), line_domain(db), base_meaning(data, db), route_in_db(db, route),
        data.dictionary.types.known(kind), data.dictionary.types.text(kind) == parameters.p3, year == 1995 || year == 1996,
    ensures sum(core_rows_0(db), |core: CoreTuple| joined_weight(db, core, route, parameters, year as int, query_weight(parameters, numerator, volume))) ==
        if route_active(route, parameters, numerator) {
            data.base.at((route.n1.n_nationkey as i64, route.n2.n_nationkey as i64, 0), (kind, year, 0), volume)
        } else { 0int },
{
    hide(sum);
    lemma_route_bounds(data, db, route);
    let callback = |core: CoreTuple| joined_weight(db, core, route, parameters, year as int, query_weight(parameters, numerator, volume));
    lemma_core_sum_0(db, callback);
    lemma_core_raw_0(db, route, parameters, year as int, numerator, volume);
    lemma_core_reorder(db, route, parameters, year as int, numerator, volume);
    let raw = |line: LineitemRow| raw_line(db, line, route, parameters, year as int, numerator, volume);
    let weight = |line: LineitemRow| line_weight(data, db, line, route.n1.n_nationkey as i64, route.n2.n_nationkey as i64, kind, year) * line_measure(line, volume);
    let selected = |line: LineitemRow| if route_active(route, parameters, numerator) { weight(line) } else { 0int };
    assert forall|index: int| 0 <= index < db.lineitem.len() implies raw(#[trigger] db.lineitem[index]) == selected(db.lineitem[index]) by {
        let line = db.lineitem[index];
        if scan_lines_exists_1(db, line) {
            assert(line_represented(line));
            lemma_represented_line(data, db, line, route, parameters, kind, year, numerator, volume);
        } else {
            lemma_excluded_core_line(db, line, route, parameters, year as int, numerator, volume);
            crate::join_proofs::lemma_excluded_line(data, db, line, route.n1.n_nationkey as i64, route.n2.n_nationkey as i64, kind, year);
            assert(weight(line) == 0) by (nonlinear_arith)
                requires line_weight(data, db, line, route.n1.n_nationkey as i64, route.n2.n_nationkey as i64, kind, year) == 0,
                    weight(line) == line_weight(data, db, line, route.n1.n_nationkey as i64, route.n2.n_nationkey as i64, kind, year) * line_measure(line, volume);
        }
    }
    lemma_sum_ext(db.lineitem, raw, selected);
    if route_active(route, parameters, numerator) {
        lemma_sum_ext_all(db.lineitem, selected, weight);
        reveal(base_meaning);
        assert(data.base.at((route.n1.n_nationkey as i64, route.n2.n_nationkey as i64, 0), (kind, year, 0), volume) == sum(db.lineitem, weight));
    } else {
        lemma_sum_zero(db.lineitem, selected);
    }
}

}
