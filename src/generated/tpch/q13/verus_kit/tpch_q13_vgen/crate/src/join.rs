use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_common::order::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::kernel::*;
use crate::facts::*;

verus! {

pub open spec fn inner_weight(key: int, nonnull: bool) -> spec_fn(COrdersTuple) -> int {
    |tuple: COrdersTuple| ind(tuple.customer.c_custkey == key && (!nonnull || tuple.orders is Some))
}

pub proof fn lemma_join_one(db: Db, p: Seq<u8>, customer: CustomerRow, key: int, nonnull: bool)
    ensures sum(c_orders_tuples_1(db, params(p), customer), inner_weight(key, nonnull))
        == ind(customer.c_custkey == key) * if nonnull { order_count(db, p, customer.c_custkey) }
            else if order_count(db, p, customer.c_custkey) > 0 { order_count(db, p, customer.c_custkey) } else { 1 },
{
    let select = |order: OrdersRow| if c_orders_on_orders(db, params(p), customer, order) {
        seq![Some(order)]
    } else { Seq::<Option<OrdersRow>>::empty() };
    let matched = flat(db.orders.map_values(select));
    let one = |option: Option<OrdersRow>| 1int;
    let qualifies = |order: OrdersRow| ind(c_orders_on_orders(db, params(p), customer, order));
    lemma_sum_flat_map(db.orders, select, |option: Option<OrdersRow>| 1int);
    assert forall|order: OrdersRow| #[trigger] sum(select(order), one)
        == qualifies(order) by {
        lemma_sum_opt(c_orders_on_orders(db, params(p), customer, order), Some(order),
            |option: Option<OrdersRow>| 1int);
    }
    lemma_sum_ext_all(db.orders, |order: OrdersRow| sum(select(order), |option: Option<OrdersRow>| 1int), qualifies);
    lemma_sum_ext_all(db.orders, qualifies,
        |order: OrdersRow| ind(c_orders_on_orders(db, params(p), CustomerRow { c_custkey: customer.c_custkey }, order)));
    lemma_sum_one(matched);
    assert(matched.len() == order_count(db, p, customer.c_custkey));
    let option_weight = |option: Option<OrdersRow>|
        ind(customer.c_custkey == key && (!nonnull || option is Some));
    lemma_sum_flat_map(db.orders, select, option_weight);
    let projected = |order: OrdersRow| ind(customer.c_custkey == key) * qualifies(order);
    assert forall|order: OrdersRow| #[trigger] sum(select(order), option_weight) == projected(order) by {
        lemma_sum_opt(c_orders_on_orders(db, params(p), customer, order), Some(order), option_weight);
    }
    lemma_sum_ext_all(db.orders, |order: OrdersRow| sum(select(order), option_weight), projected);
    lemma_sum_scale(db.orders, qualifies, projected, ind(customer.c_custkey == key));
    let effective = if matched.len() > 0 { matched } else { seq![None::<OrdersRow>] };
    let tuple_weight = inner_weight(key, nonnull);
    let extend = |option: Option<OrdersRow>| c_orders_tuples_2(db, params(p), customer, option);
    assert(c_orders_tuples_1(db, params(p), customer) =~= flat(effective.map_values(extend)));
    lemma_sum_flat_map(effective, extend, tuple_weight);
    assert forall|option: Option<OrdersRow>| #[trigger] sum(extend(option), tuple_weight) == option_weight(option) by {
        lemma_sum_singleton(COrdersTuple { customer, orders: option }, tuple_weight);
    }
    lemma_sum_ext_all(effective, |option: Option<OrdersRow>| sum(extend(option), tuple_weight), option_weight);
    if matched.len() == 0 { lemma_sum_singleton(None::<OrdersRow>, option_weight); }
}

pub proof fn lemma_inner_count(db: Db, p: Seq<u8>, key: int, nonnull: bool)
    ensures sum(c_orders_tuples(db, params(p)), inner_weight(key, nonnull))
        == customer_count(db, key) * if nonnull { order_count(db, p, key) }
            else if order_count(db, p, key) > 0 { order_count(db, p, key) } else { 1 },
{
    hide(c_orders_tuples_1);
    hide(c_orders_tuples_2);
    hide(flat);
    hide(order_count);
    let weight = inner_weight(key, nonnull);
    let extend = |customer: CustomerRow| c_orders_tuples_1(db, params(p), customer);
    let per = |customer: CustomerRow| sum(extend(customer), weight);
    let factor = if nonnull { order_count(db, p, key) }
        else if order_count(db, p, key) > 0 { order_count(db, p, key) } else { 1 };
    let indicator = |customer: CustomerRow| ind(customer.c_custkey == key);
    let scaled = |customer: CustomerRow| factor * indicator(customer);
    lemma_sum_flat_map(db.customer, extend, weight);
    assert forall|customer: CustomerRow| #[trigger] per(customer) == scaled(customer) by {
        lemma_join_one(db, p, customer, key, nonnull);
        assert(per(customer) == ind(customer.c_custkey == key) * if nonnull { order_count(db, p, customer.c_custkey) }
            else if order_count(db, p, customer.c_custkey) > 0 { order_count(db, p, customer.c_custkey) } else { 1 });
        if customer.c_custkey == key {
            assert(order_count(db, p, customer.c_custkey) == order_count(db, p, key));
            assert(per(customer) == factor);
            assert(scaled(customer) == factor);
        } else {
            assert(per(customer) == 0);
            assert(scaled(customer) == 0);
        }
    }
    lemma_sum_ext_all(db.customer, per, scaled);
    lemma_sum_scale(db.customer, indicator, scaled, factor);
    assert(factor * customer_count(db, key) == customer_count(db, key) * factor) by (nonlinear_arith);
}

pub proof fn lemma_inner_keys(db: Db, p: Seq<u8>, key: int)
    ensures dedup(map_where(c_orders_tuples(db, params(p)),
        |tuple: COrdersTuple| c_orders_having(db, params(p), tuple.customer.c_custkey),
        |tuple: COrdersTuple| tuple.customer.c_custkey)).contains(key)
        <==> customer_count(db, key) > 0,
{
    hide(c_orders_tuples);
    hide(c_orders_tuples_1);
    hide(c_orders_tuples_2);
    hide(flat);
    hide(order_count);
    let tuples = c_orders_tuples(db, params(p));
    let keep = |tuple: COrdersTuple| c_orders_having(db, params(p), tuple.customer.c_custkey);
    let projection = |tuple: COrdersTuple| tuple.customer.c_custkey;
    let keys = map_where(tuples, keep, projection);
    lemma_map_where(tuples, keep, projection);
    lemma_dedup(keys);
    lemma_inner_count(db, p, key, false);
    lemma_customer_support(db, key);
    let factor = if order_count(db, p, key) > 0 { order_count(db, p, key) } else { 1int };
    assert(factor > 0);
    lemma_positive_product(customer_count(db, key), factor);
    let weight = |tuple: COrdersTuple| ind(tuple.customer.c_custkey == key);
    lemma_indicator(tuples, weight);
    lemma_sum_ext_all(tuples, inner_weight(key, false), weight);
    assert(sum(tuples, weight) == customer_count(db, key) * factor);
    if keys.contains(key) {
        let index = choose|index: int| 0 <= index < tuples.len()
            && keep(#[trigger] tuples[index]) && projection(tuples[index]) == key;
        assert(tuples[index].customer.c_custkey == key);
        lemma_sum_pos(tuples, weight, index);
    }
    if customer_count(db, key) > 0 {
        let index = lemma_sum_nonzero_witness(tuples, weight);
        assert(keep(tuples[index]) && projection(tuples[index]) == key);
        assert(keys.contains(key));
    }
}

pub proof fn lemma_inner_value(db: Db, p: Seq<u8>, key: int)
    ensures c_orders_row_of(db, params(p), key).c_count == customer_count(db, key) * order_count(db, p, key),
{
    hide(c_orders_tuples);
    hide(c_orders_tuples_1);
    hide(c_orders_tuples_2);
    hide(order_count);
    hide(flat);
    lemma_inner_count(db, p, key, true);
    let tuples = c_orders_tuples(db, params(p));
    let nullable = |tuple: COrdersTuple| if tuple.orders is Some { 1int } else { 0 };
    let grouped = |tuple: COrdersTuple| if tuple.customer.c_custkey == key { nullable(tuple) } else { 0 };
    let weight = |tuple: COrdersTuple| ind(tuple.customer.c_custkey == key && tuple.orders is Some);
    lemma_sum_ext_all(tuples, grouped, weight);
    lemma_sum_ext_all(tuples, inner_weight(key, true), weight);
    assert(c_orders_row_of(db, params(p), key).c_count == sum(tuples, grouped));
}

pub proof fn lemma_query_count(db: Db, p: Seq<u8>, bucket: int)
    ensures group_count(db, params(p), bucket) == histogram_count(db, p, bucket),
{
    let tuples = c_orders_tuples(db, params(p));
    let keys = dedup(map_where(tuples,
        |tuple: COrdersTuple| c_orders_having(db, params(p), tuple.customer.c_custkey),
        |tuple: COrdersTuple| tuple.customer.c_custkey));
    lemma_dedup(map_where(tuples,
        |tuple: COrdersTuple| c_orders_having(db, params(p), tuple.customer.c_custkey),
        |tuple: COrdersTuple| tuple.customer.c_custkey));
    lemma_dedup(db.customer.map_values(|row: CustomerRow| row.c_custkey));
    let weight = |tuple: Tuple| ind(tuple.c_orders.c_count == bucket);
    let extend = |row: COrdersRow| tuples_1(db, params(p), row);
    lemma_sum_flat_map(c_orders_rows(db, params(p)), extend, weight);
    assert forall|row: COrdersRow| #[trigger] sum(extend(row), weight) == ind(row.c_count == bucket) by {
        lemma_sum_singleton(Tuple { c_orders: row }, weight);
    }
    lemma_sum_ext_all(c_orders_rows(db, params(p)), |row: COrdersRow| sum(extend(row), weight),
        |row: COrdersRow| ind(row.c_count == bucket));
    let make_row = |key: int| c_orders_row_of(db, params(p), key);
    let per = |key: int| ind(make_row(key).c_count == bucket);
    lemma_sum_map(keys, make_row, |row: COrdersRow| ind(row.c_count == bucket), per);
    let contribution_weight = |key: int| contribution(db, p, key, bucket);
    assert forall|index: int| 0 <= index < keys.len() implies
        per(#[trigger] keys[index]) == contribution_weight(keys[index]) by {
        let key = keys[index];
        assert(keys.contains(key));
        lemma_inner_keys(db, p, key);
        lemma_inner_value(db, p, key);
    }
    lemma_sum_ext(keys, per, contribution_weight);
    assert forall|key: int| #[trigger] contribution_weight(key) != 0 implies
        keys.contains(key) && customer_keys(db).contains(key) by {
        lemma_inner_keys(db, p, key);
        lemma_customer_support(db, key);
    }
    lemma_sum_support(keys, customer_keys(db), contribution_weight);
    lemma_sum_ext_all(crate::gen_spec::tuples(db, params(p)),
        |tuple: Tuple| if tuple.c_orders.c_count == bucket { 1int } else { 0 }, weight);
}

}
