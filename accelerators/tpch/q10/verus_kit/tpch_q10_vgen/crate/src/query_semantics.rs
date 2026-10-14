use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::model::line_weight;

verus! {

pub open spec fn customer_matches(customer: CustomerRow, group: OutRow) -> bool {
    customer.c_custkey == group.c_custkey && customer.c_name == group.c_name
        && customer.c_acctbal == group.c_acctbal && customer.c_phone == group.c_phone
        && customer.c_address == group.c_address && customer.c_comment == group.c_comment
}

pub open spec fn nation_weight(key: int, name: Seq<u8>) -> spec_fn(NationRow) -> int {
    |nation: NationRow| if nation.n_nationkey == key && nation.n_name == name { 1int } else { 0int }
}

#[verifier::opaque]
pub open spec fn dimension_multiplicity(db: Db, group: OutRow) -> int {
    sum(db.customer, |customer: CustomerRow| if customer_matches(customer, group) {
        sum(db.nation, nation_weight(customer.c_nationkey, group.n_name))
    } else { 0int })
}

pub open spec fn order_window_weight(db: Db, parameters: Params, customer: int, component: int)
    -> spec_fn(OrdersRow) -> int
{
    |order: OrdersRow| if order.o_custkey == customer && parameters.p1 <= order.o_orderdate < parameters.p2 {
        sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0int })
    } else { 0int }
}

#[verifier::opaque]
pub open spec fn window_fact(db: Db, parameters: Params, customer: int, component: int) -> int {
    sum(db.orders, order_window_weight(db, parameters, customer, component))
}

pub open spec fn tuple_group_weight(group: OutRow, component: int) -> spec_fn(Tuple) -> int {
    |tuple: Tuple| if customer_matches(tuple.customer, group) && tuple.nation.n_name == group.n_name {
        if component == 0 { 1int } else { tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) }
    } else { 0int }
}

pub proof fn lemma_nation_factor(db: Db, parameters: Params, group: OutRow, component: int,
    customer: CustomerRow, order: OrdersRow, line: LineitemRow)
    requires 0 <= component <= 1,
    ensures sum(tuples_3(db, parameters, customer, order, line), tuple_group_weight(group, component))
        == (if customer_matches(customer, group) && order.o_custkey == customer.c_custkey
            && parameters.p1 <= order.o_orderdate < parameters.p2 && line.l_orderkey == order.o_orderkey {
                line_weight(line, component)
            } else { 0int }) * sum(db.nation, nation_weight(customer.c_nationkey, group.n_name)),
{
    let tuples_for = |nation: NationRow| tuples_4(db, parameters, customer, order, line, nation);
    let weight = tuple_group_weight(group, component);
    let nested = |nation: NationRow| sum(tuples_for(nation), weight);
    let names = nation_weight(customer.c_nationkey, group.n_name);
    let factor = if customer_matches(customer, group) && order.o_custkey == customer.c_custkey
        && parameters.p1 <= order.o_orderdate < parameters.p2 && line.l_orderkey == order.o_orderkey {
            line_weight(line, component)
        } else { 0int };
    lemma_sum_flat_map(db.nation, tuples_for, weight);
    assert forall|nation: NationRow| #[trigger] nested(nation) == factor * names(nation) by {
        lemma_sum_opt(where_clause(db, parameters, customer, order, line, nation),
            Tuple { customer, orders: order, lineitem: line, nation }, weight);
    }
    lemma_sum_scale(db.nation, names, nested, factor);
}

pub proof fn lemma_line_factor(db: Db, parameters: Params, group: OutRow, component: int,
    customer: CustomerRow, order: OrdersRow)
    requires 0 <= component <= 1,
    ensures sum(tuples_2(db, parameters, customer, order), tuple_group_weight(group, component))
        == (if customer_matches(customer, group) && order.o_custkey == customer.c_custkey
            && parameters.p1 <= order.o_orderdate < parameters.p2 {
                sum(db.lineitem, |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0int })
            } else { 0int }) * sum(db.nation, nation_weight(customer.c_nationkey, group.n_name)),
{
    let tuples_for = |line: LineitemRow| tuples_3(db, parameters, customer, order, line);
    let weight = tuple_group_weight(group, component);
    let nested = |line: LineitemRow| sum(tuples_for(line), weight);
    let lines = |line: LineitemRow| if customer_matches(customer, group) && order.o_custkey == customer.c_custkey
        && parameters.p1 <= order.o_orderdate < parameters.p2 && line.l_orderkey == order.o_orderkey {
            line_weight(line, component)
        } else { 0int };
    let names = sum(db.nation, nation_weight(customer.c_nationkey, group.n_name));
    lemma_sum_flat_map(db.lineitem, tuples_for, weight);
    assert forall|line: LineitemRow| #[trigger] nested(line) == names * lines(line) by {
        lemma_nation_factor(db, parameters, group, component, customer, order, line);
        assert(lines(line) * names == names * lines(line)) by (nonlinear_arith);
    }
    lemma_sum_scale(db.lineitem, lines, nested, names);
    if !(customer_matches(customer, group) && order.o_custkey == customer.c_custkey
        && parameters.p1 <= order.o_orderdate < parameters.p2) {
        lemma_sum_zero(db.lineitem, lines);
    } else {
        lemma_sum_ext_all(db.lineitem, lines,
            |line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0int });
    }
    assert(sum(db.lineitem, lines) * names == names * sum(db.lineitem, lines)) by (nonlinear_arith);
}

pub proof fn lemma_order_factor(db: Db, parameters: Params, group: OutRow, component: int, customer: CustomerRow)
    requires 0 <= component <= 1,
    ensures sum(tuples_1(db, parameters, customer), tuple_group_weight(group, component))
        == window_fact(db, parameters, group.c_custkey, component)
            * (if customer_matches(customer, group) {
                sum(db.nation, nation_weight(customer.c_nationkey, group.n_name))
            } else { 0int }),
{
    let tuples_for = |order: OrdersRow| tuples_2(db, parameters, customer, order);
    let weight = tuple_group_weight(group, component);
    let nested = |order: OrdersRow| sum(tuples_for(order), weight);
    let orders = order_window_weight(db, parameters, group.c_custkey, component);
    let names = if customer_matches(customer, group) {
        sum(db.nation, nation_weight(customer.c_nationkey, group.n_name))
    } else { 0int };
    lemma_sum_flat_map(db.orders, tuples_for, weight);
    assert forall|order: OrdersRow| #[trigger] nested(order) == names * orders(order) by {
        lemma_line_factor(db, parameters, group, component, customer, order);
        if customer_matches(customer, group) {
            assert(orders(order) * names == names * orders(order)) by (nonlinear_arith);
        }
    }
    lemma_sum_scale(db.orders, orders, nested, names);
    reveal(window_fact);
    assert(sum(db.orders, orders) * names == names * sum(db.orders, orders)) by (nonlinear_arith);
}

pub proof fn lemma_group_factorization(db: Db, parameters: Params, group: OutRow, component: int)
    requires 0 <= component <= 1,
    ensures sum(tuples(db, parameters), tuple_group_weight(group, component))
        == window_fact(db, parameters, group.c_custkey, component) * dimension_multiplicity(db, group),
{
    let tuples_for = |customer: CustomerRow| tuples_1(db, parameters, customer);
    let weight = tuple_group_weight(group, component);
    let nested = |customer: CustomerRow| sum(tuples_for(customer), weight);
    let dimensions = |customer: CustomerRow| if customer_matches(customer, group) {
        sum(db.nation, nation_weight(customer.c_nationkey, group.n_name))
    } else { 0int };
    let facts = window_fact(db, parameters, group.c_custkey, component);
    lemma_sum_flat_map(db.customer, tuples_for, weight);
    assert forall|customer: CustomerRow| #[trigger] nested(customer) == facts * dimensions(customer) by {
        lemma_order_factor(db, parameters, group, component, customer);
    }
    lemma_sum_scale(db.customer, dimensions, nested, facts);
    reveal(dimension_multiplicity);
}

pub proof fn lemma_group_aggregates(db: Db, parameters: Params, group: OutRow, revenue: spec_fn(Tuple) -> int)
    requires forall|tuple: Tuple| #[trigger] revenue(tuple)
        == tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount),
    ensures group_count(db, parameters, group.c_custkey, group.c_name, group.c_acctbal,
            group.c_phone, group.n_name, group.c_address, group.c_comment)
        == window_fact(db, parameters, group.c_custkey, 0) * dimension_multiplicity(db, group),
        group_sum(db, parameters, group.c_custkey, group.c_name, group.c_acctbal,
            group.c_phone, group.n_name, group.c_address, group.c_comment, revenue)
        == window_fact(db, parameters, group.c_custkey, 1) * dimension_multiplicity(db, group),
{
    lemma_group_factorization(db, parameters, group, 0);
    lemma_group_factorization(db, parameters, group, 1);
    let counts = |tuple: Tuple| if tuple.customer.c_custkey == group.c_custkey
        && tuple.customer.c_name == group.c_name && tuple.customer.c_acctbal == group.c_acctbal
        && tuple.customer.c_phone == group.c_phone && tuple.nation.n_name == group.n_name
        && tuple.customer.c_address == group.c_address && tuple.customer.c_comment == group.c_comment { 1int } else { 0int };
    let revenues = |tuple: Tuple| if tuple.customer.c_custkey == group.c_custkey
        && tuple.customer.c_name == group.c_name && tuple.customer.c_acctbal == group.c_acctbal
        && tuple.customer.c_phone == group.c_phone && tuple.nation.n_name == group.n_name
        && tuple.customer.c_address == group.c_address && tuple.customer.c_comment == group.c_comment {
            revenue(tuple)
        } else { 0int };
    lemma_sum_ext_all(tuples(db, parameters), tuple_group_weight(group, 0), counts);
    lemma_sum_ext_all(tuples(db, parameters), tuple_group_weight(group, 1), revenues);
}

pub open spec fn keyed_line_weight(key: int, component: int) -> spec_fn(LineitemRow) -> int {
    |line: LineitemRow| if line.l_orderkey == key { line_weight(line, component) } else { 0int }
}

pub proof fn lemma_line_envelope(db: Db, key: int)
    ensures sum(db.lineitem, keyed_line_weight(key, 0)) >= 0,
        sum(db.lineitem, keyed_line_weight(key, 2)) >= 0,
        sum(db.lineitem, keyed_line_weight(key, 1)) <= sum(db.lineitem, keyed_line_weight(key, 2)),
        sum(db.lineitem, keyed_line_weight(key, 0)) == 0 ==> {
            &&& sum(db.lineitem, keyed_line_weight(key, 1)) == 0
            &&& sum(db.lineitem, keyed_line_weight(key, 2)) == 0
        },
{
    let count = keyed_line_weight(key, 0);
    let revenue = keyed_line_weight(key, 1);
    let upper = keyed_line_weight(key, 2);
    lemma_sum_nonneg(db.lineitem, count);
    lemma_sum_nonneg(db.lineitem, upper);
    lemma_sum_le(db.lineitem, revenue, upper);
    if sum(db.lineitem, count) == 0 {
        assert forall|position: int| #![trigger db.lineitem[position]]
            0 <= position < db.lineitem.len() implies count(db.lineitem[position]) == 0 by {
            lemma_sum_ge_term(db.lineitem, count, position);
        }
        lemma_sum_zero(db.lineitem, revenue);
        lemma_sum_zero(db.lineitem, upper);
    }
}

pub proof fn lemma_window_envelope(db: Db, parameters: Params, customer: int)
    ensures window_fact(db, parameters, customer, 0) >= 0,
        window_fact(db, parameters, customer, 2) >= 0,
        window_fact(db, parameters, customer, 1) <= window_fact(db, parameters, customer, 2),
        window_fact(db, parameters, customer, 0) == 0 ==> {
            &&& window_fact(db, parameters, customer, 1) == 0
            &&& window_fact(db, parameters, customer, 2) == 0
        },
{
    let count = order_window_weight(db, parameters, customer, 0);
    let revenue = order_window_weight(db, parameters, customer, 1);
    let upper = order_window_weight(db, parameters, customer, 2);
    assert forall|order: OrdersRow| #![trigger count(order)] #![trigger upper(order)] #![trigger revenue(order)]
        count(order) >= 0 && upper(order) >= 0 && revenue(order) <= upper(order) by {
        lemma_line_envelope(db, order.o_orderkey);
        assert forall|component: int| #![trigger order_window_weight(db, parameters, customer, component)(order)]
            (order.o_custkey == customer && parameters.p1 <= order.o_orderdate < parameters.p2) implies
                order_window_weight(db, parameters, customer, component)(order)
                    == sum(db.lineitem, keyed_line_weight(order.o_orderkey, component)) by {
            assert(keyed_line_weight(order.o_orderkey, component) =~=
                (|line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0int }));
        }
    }
    lemma_sum_nonneg(db.orders, count);
    lemma_sum_nonneg(db.orders, upper);
    lemma_sum_le(db.orders, revenue, upper);
    reveal(window_fact);
    if sum(db.orders, count) == 0 {
        assert forall|position: int| #![trigger db.orders[position]]
            0 <= position < db.orders.len() implies revenue(db.orders[position]) == 0 && upper(db.orders[position]) == 0 by {
            lemma_sum_ge_term(db.orders, count, position);
            let order = db.orders[position];
            assert(count(order) == 0);
            lemma_line_envelope(db, order.o_orderkey);
            if order.o_custkey == customer && parameters.p1 <= order.o_orderdate < parameters.p2 {
                assert forall|component: int| #![trigger order_window_weight(db, parameters, customer, component)(order)]
                    order_window_weight(db, parameters, customer, component)(order)
                        == sum(db.lineitem, keyed_line_weight(order.o_orderkey, component)) by {
                    assert(keyed_line_weight(order.o_orderkey, component) =~=
                        (|line: LineitemRow| if line.l_orderkey == order.o_orderkey { line_weight(line, component) } else { 0int }));
                }
            }
        }
        lemma_sum_zero(db.orders, revenue);
        lemma_sum_zero(db.orders, upper);
    }
}

pub proof fn lemma_dimension_nonnegative(db: Db, group: OutRow)
    ensures dimension_multiplicity(db, group) >= 0,
{
    let weight = |customer: CustomerRow| if customer_matches(customer, group) {
        sum(db.nation, nation_weight(customer.c_nationkey, group.n_name))
    } else { 0int };
    assert forall|customer: CustomerRow| #[trigger] weight(customer) >= 0 by {
        lemma_sum_nonneg(db.nation, nation_weight(customer.c_nationkey, group.n_name));
    }
    lemma_sum_nonneg(db.customer, weight);
    reveal(dimension_multiplicity);
}

pub proof fn lemma_row_factor(db: Db, parameters: Params, group: OutRow)
    ensures row_ok(db, parameters, group) == {
        &&& window_fact(db, parameters, group.c_custkey, 0) > 0
        &&& dimension_multiplicity(db, group) > 0
        &&& group.revenue == window_fact(db, parameters, group.c_custkey, 1) * dimension_multiplicity(db, group)
    },
{
    lemma_group_aggregates(db, parameters, group,
        |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount));
    lemma_window_envelope(db, parameters, group.c_custkey);
    lemma_dimension_nonnegative(db, group);
    assert((window_fact(db, parameters, group.c_custkey, 0) * dimension_multiplicity(db, group) > 0)
        == (window_fact(db, parameters, group.c_custkey, 0) > 0 && dimension_multiplicity(db, group) > 0)) by (nonlinear_arith)
        requires window_fact(db, parameters, group.c_custkey, 0) >= 0, dimension_multiplicity(db, group) >= 0;
}

pub open spec fn nation_total(db: Db, key: int) -> int {
    sum(db.nation, crate::dimension_updates::nation_key_weight(key))
}

pub proof fn lemma_dimension_bound(db: Db, group: OutRow, customer_max: int, nation_max: int)
    requires customer_max >= 0, nation_max >= 0,
        sum(db.customer, |customer: CustomerRow| if customer.c_custkey == group.c_custkey { 1int } else { 0int }) <= customer_max,
        forall|key: int| #[trigger] nation_total(db, key) <= nation_max,
    ensures dimension_multiplicity(db, group) <= customer_max * nation_max,
{
    let customers = |customer: CustomerRow| if customer.c_custkey == group.c_custkey { 1int } else { 0int };
    let dimensions = |customer: CustomerRow| if customer_matches(customer, group) {
        sum(db.nation, nation_weight(customer.c_nationkey, group.n_name))
    } else { 0int };
    let upper = |customer: CustomerRow| nation_max * customers(customer);
    assert forall|customer: CustomerRow| #[trigger] dimensions(customer) <= upper(customer) by {
        if customer_matches(customer, group) {
            let all_names = |nation: NationRow| if nation.n_nationkey == customer.c_nationkey { 1int } else { 0int };
            lemma_sum_le(db.nation, nation_weight(customer.c_nationkey, group.n_name), all_names);
            assert(all_names =~= crate::dimension_updates::nation_key_weight(customer.c_nationkey));
            assert(nation_total(db, customer.c_nationkey) <= nation_max);
            assert(sum(db.nation, all_names) <= nation_max);
        }
    }
    lemma_sum_le(db.customer, dimensions, upper);
    lemma_sum_scale(db.customer, customers, upper, nation_max);
    assert(nation_max * sum(db.customer, customers) <= nation_max * customer_max) by (nonlinear_arith)
        requires sum(db.customer, customers) <= customer_max, nation_max >= 0;
    assert(nation_max * customer_max == customer_max * nation_max) by (nonlinear_arith);
    reveal(dimension_multiplicity);
}

pub proof fn lemma_unseen_bound(db: Db, parameters: Params, group: OutRow, bound: int, multiplier: int)
    requires row_ok(db, parameters, group), multiplier >= 0,
        dimension_multiplicity(db, group) <= multiplier,
        window_fact(db, parameters, group.c_custkey, 2) <= bound,
    ensures group.revenue <= bound * multiplier,
{
    lemma_row_factor(db, parameters, group);
    lemma_window_envelope(db, parameters, group.c_custkey);
    let revenue = window_fact(db, parameters, group.c_custkey, 1);
    let upper = window_fact(db, parameters, group.c_custkey, 2);
    let multiplicity = dimension_multiplicity(db, group);
    assert(revenue * multiplicity <= bound * multiplier) by (nonlinear_arith)
        requires revenue <= upper, 0 <= upper <= bound, 0 < multiplicity <= multiplier;
}

}
