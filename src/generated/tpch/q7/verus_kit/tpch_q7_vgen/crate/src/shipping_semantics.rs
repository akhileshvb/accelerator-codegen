use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::gen_obligations::params;
use crate::model::*;
use crate::domains::*;
use crate::query_proofs::*;
use crate::answer_semantics::named_numeric;

verus! {
broadcast use vstd::seq_lib::group_seq_properties;

pub struct Measure {
    pub first: Seq<u8>,
    pub second: Seq<u8>,
    pub year: i64,
    pub revenue: bool,
}

pub open spec fn scalar(q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow,
    customer: CustomerRow, left: NationRow, right: NationRow) -> int {
    if supplier.s_suppkey == line.l_suppkey && order.o_orderkey == line.l_orderkey
        && customer.c_custkey == order.o_custkey && supplier.s_nationkey == left.n_nationkey
        && customer.c_nationkey == right.n_nationkey && left.n_name == q.first && right.n_name == q.second
        && 9131 <= line.l_shipdate <= 9861 && (if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == q.year {
        if q.revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1int }
    } else { 0int }
}

pub open spec fn suffix5(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow,
    customer: CustomerRow, left: NationRow) -> int {
    sum(db.nation, |right: NationRow| scalar(q, supplier, line, order, customer, left, right))
}
pub open spec fn suffix4(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow,
    customer: CustomerRow) -> int {
    sum(db.nation, |left: NationRow| suffix5(db, q, supplier, line, order, customer, left))
}
pub open spec fn suffix3(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow) -> int {
    sum(db.customer, |customer: CustomerRow| suffix4(db, q, supplier, line, order, customer))
}
pub open spec fn suffix2(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow) -> int {
    sum(db.orders, |order: OrdersRow| suffix3(db, q, supplier, line, order))
}
pub open spec fn suffix1(db: Db, q: Measure, supplier: SupplierRow) -> int {
    sum(db.lineitem, |line: LineitemRow| suffix2(db, q, supplier, line))
}
pub open spec fn suffix0(db: Db, q: Measure) -> int {
    sum(db.supplier, |supplier: SupplierRow| suffix1(db, q, supplier))
}

pub proof fn lemma_shipping_scalar(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow,
    customer: CustomerRow, left: NationRow, right: NationRow)
    ensures sum(shipping_tuples_6(db, params(q.first, q.second), supplier, line, order, customer, left, right),
        shipping_weight(q.first, q.second, q.year as int, q.revenue)) == scalar(q, supplier, line, order, customer, left, right),
{
    let tuple = ShippingTuple { supplier, lineitem: line, orders: order, customer, n1: left, n2: right };
    lemma_sum_opt(shipping_where_clause(db, params(q.first, q.second), supplier, line, order, customer, left, right),
        tuple, shipping_weight(q.first, q.second, q.year as int, q.revenue));
    if 9131 <= line.l_shipdate <= 9861 { lemma_window_year(line.l_shipdate); }
}

pub proof fn lemma_shipping5(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow,
    customer: CustomerRow, left: NationRow)
    ensures sum(shipping_tuples_5(db, params(q.first, q.second), supplier, line, order, customer, left),
        shipping_weight(q.first, q.second, q.year as int, q.revenue)) == suffix5(db, q, supplier, line, order, customer, left),
{
    let mapper = |right: NationRow| shipping_tuples_6(db, params(q.first, q.second), supplier, line, order, customer, left, right);
    let weight = shipping_weight(q.first, q.second, q.year as int, q.revenue);
    let nested = |right: NationRow| scalar(q, supplier, line, order, customer, left, right);
    assert forall|right: NationRow| #[trigger] nested(right) == sum(mapper(right), weight) by {
        lemma_shipping_scalar(db, q, supplier, line, order, customer, left, right);
    }
    lemma_flat_map_weight(db.nation, mapper, weight, nested);
}

pub proof fn lemma_shipping4(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow, customer: CustomerRow)
    ensures sum(shipping_tuples_4(db, params(q.first, q.second), supplier, line, order, customer),
        shipping_weight(q.first, q.second, q.year as int, q.revenue)) == suffix4(db, q, supplier, line, order, customer),
{
    let mapper = |left: NationRow| shipping_tuples_5(db, params(q.first, q.second), supplier, line, order, customer, left);
    let weight = shipping_weight(q.first, q.second, q.year as int, q.revenue);
    let nested = |left: NationRow| suffix5(db, q, supplier, line, order, customer, left);
    assert forall|left: NationRow| #[trigger] nested(left) == sum(mapper(left), weight) by {
        lemma_shipping5(db, q, supplier, line, order, customer, left);
    }
    lemma_flat_map_weight(db.nation, mapper, weight, nested);
}

pub proof fn lemma_shipping3(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow)
    ensures sum(shipping_tuples_3(db, params(q.first, q.second), supplier, line, order),
        shipping_weight(q.first, q.second, q.year as int, q.revenue)) == suffix3(db, q, supplier, line, order),
{
    let mapper = |customer: CustomerRow| shipping_tuples_4(db, params(q.first, q.second), supplier, line, order, customer);
    let weight = shipping_weight(q.first, q.second, q.year as int, q.revenue);
    let nested = |customer: CustomerRow| suffix4(db, q, supplier, line, order, customer);
    assert forall|customer: CustomerRow| #[trigger] nested(customer) == sum(mapper(customer), weight) by {
        lemma_shipping4(db, q, supplier, line, order, customer);
    }
    lemma_flat_map_weight(db.customer, mapper, weight, nested);
}

pub proof fn lemma_shipping2(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow)
    ensures sum(shipping_tuples_2(db, params(q.first, q.second), supplier, line),
        shipping_weight(q.first, q.second, q.year as int, q.revenue)) == suffix2(db, q, supplier, line),
{
    let mapper = |order: OrdersRow| shipping_tuples_3(db, params(q.first, q.second), supplier, line, order);
    let weight = shipping_weight(q.first, q.second, q.year as int, q.revenue);
    let nested = |order: OrdersRow| suffix3(db, q, supplier, line, order);
    assert forall|order: OrdersRow| #[trigger] nested(order) == sum(mapper(order), weight) by {
        lemma_shipping3(db, q, supplier, line, order);
    }
    lemma_flat_map_weight(db.orders, mapper, weight, nested);
}

pub proof fn lemma_shipping1(db: Db, q: Measure, supplier: SupplierRow)
    ensures sum(shipping_tuples_1(db, params(q.first, q.second), supplier),
        shipping_weight(q.first, q.second, q.year as int, q.revenue)) == suffix1(db, q, supplier),
{
    let mapper = |line: LineitemRow| shipping_tuples_2(db, params(q.first, q.second), supplier, line);
    let weight = shipping_weight(q.first, q.second, q.year as int, q.revenue);
    let nested = |line: LineitemRow| suffix2(db, q, supplier, line);
    assert forall|line: LineitemRow| #[trigger] nested(line) == sum(mapper(line), weight) by {
        lemma_shipping2(db, q, supplier, line);
    }
    lemma_flat_map_weight(db.lineitem, mapper, weight, nested);
}

pub proof fn lemma_shipping0(db: Db, q: Measure)
    ensures sum(shipping_tuples(db, params(q.first, q.second)), shipping_weight(q.first, q.second, q.year as int, q.revenue)) == suffix0(db, q),
{
    let mapper = |supplier: SupplierRow| shipping_tuples_1(db, params(q.first, q.second), supplier);
    let weight = shipping_weight(q.first, q.second, q.year as int, q.revenue);
    let nested = |supplier: SupplierRow| suffix1(db, q, supplier);
    assert forall|supplier: SupplierRow| #[trigger] nested(supplier) == sum(mapper(supplier), weight) by {
        lemma_shipping1(db, q, supplier);
    }
    lemma_flat_map_weight(db.supplier, mapper, weight, nested);
}

pub open spec fn customer_fold(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow,
    left: NationRow, right: NationRow) -> int {
    sum(db.customer, |customer: CustomerRow| scalar(q, supplier, line, order, customer, left, right))
}
pub open spec fn order_fold(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, left: NationRow, right: NationRow) -> int {
    sum(db.orders, |order: OrdersRow| customer_fold(db, q, supplier, line, order, left, right))
}
pub open spec fn line_fold(db: Db, q: Measure, supplier: SupplierRow, left: NationRow, right: NationRow) -> int {
    sum(db.lineitem, |line: LineitemRow| order_fold(db, q, supplier, line, left, right))
}
pub open spec fn supplier_fold(db: Db, q: Measure, left: NationRow, right: NationRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| line_fold(db, q, supplier, left, right))
}

pub open spec fn numeric_customer(db: Db, line: LineitemRow, order: OrdersRow, second: i64, revenue: bool) -> int {
    sum(db.customer, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == second {
        if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1int }
    } else { 0int })
}
pub open spec fn numeric_order(db: Db, line: LineitemRow, second: i64, revenue: bool) -> int {
    sum(db.orders, |order: OrdersRow| if order.o_orderkey == line.l_orderkey { numeric_customer(db, line, order, second, revenue) } else { 0int })
}
pub open spec fn numeric_supplier(db: Db, line: LineitemRow, first: i64, second: i64, supplier: SupplierRow, revenue: bool) -> int {
    if supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == first { numeric_order(db, line, second, revenue) } else { 0int }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_fact_normalize(db: Db, line: LineitemRow, first: i64, second: i64, year: i64, revenue: bool)
    ensures fact_line(db, line, first, second, year, revenue) ==
        if 9131 <= line.l_shipdate <= 9861 && (if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == year {
            sum(db.supplier, |supplier: SupplierRow| numeric_supplier(db, line, first, second, supplier, revenue))
        } else { 0int },
{
    reveal(fact_line);
    let original_order = |order: OrdersRow| if order.o_orderkey == line.l_orderkey {
        sum(db.customer, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == second {
            if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1int }
        } else { 0int })
    } else { 0int };
    let normalized_order = |order: OrdersRow| if order.o_orderkey == line.l_orderkey { numeric_customer(db, line, order, second, revenue) } else { 0int };
    lemma_sum_ext_all(db.orders, original_order, normalized_order);
    let original_supplier = |supplier: SupplierRow| if supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == first {
        sum(db.orders, original_order)
    } else { 0int };
    let normalized_supplier = |supplier: SupplierRow| numeric_supplier(db, line, first, second, supplier, revenue);
    lemma_sum_ext_all(db.supplier, original_supplier, normalized_supplier);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_order_numeric(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, left: NationRow, right: NationRow)
    requires nation_domain(left), nation_domain(right),
    ensures order_fold(db, q, supplier, line, left, right) ==
        if left.n_name == q.first && right.n_name == q.second && 9131 <= line.l_shipdate <= 9861
            && (if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == q.year {
            numeric_supplier(db, line, left.n_nationkey as i64, right.n_nationkey as i64, supplier, q.revenue)
        } else { 0int },
{
    assert(left.n_nationkey as i64 as int == left.n_nationkey);
    assert(right.n_nationkey as i64 as int == right.n_nationkey);
    let original = |order: OrdersRow| customer_fold(db, q, supplier, line, order, left, right);
    if left.n_name == q.first && right.n_name == q.second && 9131 <= line.l_shipdate <= 9861
        && (if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == q.year
        && supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == left.n_nationkey {
        let expected = |order: OrdersRow| if order.o_orderkey == line.l_orderkey {
            numeric_customer(db, line, order, right.n_nationkey as i64, q.revenue)
        } else { 0int };
        assert forall|order: OrdersRow| #[trigger] original(order) == expected(order) by {
            if order.o_orderkey == line.l_orderkey {
                lemma_sum_ext_all(db.customer, |customer: CustomerRow| scalar(q, supplier, line, order, customer, left, right),
                    |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == right.n_nationkey as i64 {
                        if q.revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1int }
                    } else { 0int });
            } else { lemma_sum_zero(db.customer, |customer: CustomerRow| scalar(q, supplier, line, order, customer, left, right)); }
        }
        lemma_sum_ext_all(db.orders, original, expected);
    } else {
        assert forall|order: OrdersRow| #[trigger] original(order) == 0int by {
            lemma_sum_zero(db.customer, |customer: CustomerRow| scalar(q, supplier, line, order, customer, left, right));
        }
        lemma_sum_zero(db.orders, original);
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_named_fact(db: Db, q: Measure, line: LineitemRow, left: NationRow, right: NationRow)
    requires nation_domain(left), nation_domain(right),
    ensures sum(db.supplier, |supplier: SupplierRow| order_fold(db, q, supplier, line, left, right)) ==
        if left.n_name == q.first && right.n_name == q.second {
            fact_line(db, line, left.n_nationkey as i64, right.n_nationkey as i64, q.year, q.revenue)
        } else { 0int },
{
    let original = |supplier: SupplierRow| order_fold(db, q, supplier, line, left, right);
    if left.n_name == q.first && right.n_name == q.second && 9131 <= line.l_shipdate <= 9861
        && (if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == q.year {
        let expected = |supplier: SupplierRow| numeric_supplier(db, line, left.n_nationkey as i64, right.n_nationkey as i64, supplier, q.revenue);
        assert forall|supplier: SupplierRow| #[trigger] original(supplier) == expected(supplier) by {
            lemma_order_numeric(db, q, supplier, line, left, right);
        }
        lemma_sum_ext_all(db.supplier, original, expected);
    } else {
        assert forall|supplier: SupplierRow| #[trigger] original(supplier) == 0int by { lemma_order_numeric(db, q, supplier, line, left, right); }
        lemma_sum_zero(db.supplier, original);
    }
    lemma_fact_normalize(db, line, left.n_nationkey as i64, right.n_nationkey as i64, q.year, q.revenue);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_supplier_numeric(db: Db, q: Measure, left: NationRow, right: NationRow)
    requires nation_domain(left), nation_domain(right),
    ensures supplier_fold(db, q, left, right) == if left.n_name == q.first && right.n_name == q.second {
        numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, q.year, q.revenue)
    } else { 0int },
{
    reveal(numeric_group);
    let weight = |supplier: SupplierRow, line: LineitemRow| order_fold(db, q, supplier, line, left, right);
    let original = |supplier: SupplierRow| line_fold(db, q, supplier, left, right);
    let transposed = |line: LineitemRow| sum(db.supplier, |supplier: SupplierRow| order_fold(db, q, supplier, line, left, right));
    assert forall|supplier: SupplierRow| #[trigger] original(supplier) == sum(db.lineitem, |line: LineitemRow| weight(supplier, line)) by {
        lemma_sum_ext_all(db.lineitem, |line: LineitemRow| weight(supplier, line), |line: LineitemRow| order_fold(db, q, supplier, line, left, right));
    }
    assert forall|line: LineitemRow| #[trigger] transposed(line) == sum(db.supplier, |supplier: SupplierRow| weight(supplier, line)) by {
        lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| weight(supplier, line), |supplier: SupplierRow| order_fold(db, q, supplier, line, left, right));
    }
    lemma_sum_swap(db.supplier, db.lineitem, weight, original, transposed);
    if left.n_name == q.first && right.n_name == q.second {
        let expected = |line: LineitemRow| fact_line(db, line, left.n_nationkey as i64, right.n_nationkey as i64, q.year, q.revenue);
        assert forall|line: LineitemRow| #[trigger] transposed(line) == expected(line) by { lemma_named_fact(db, q, line, left, right); }
        lemma_sum_ext_all(db.lineitem, transposed, expected);
    } else {
        assert forall|line: LineitemRow| #[trigger] transposed(line) == 0int by { lemma_named_fact(db, q, line, left, right); }
        lemma_sum_zero(db.lineitem, transposed);
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_named_sql(db: Db, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool)
    requires database_domain(db),
    ensures named_numeric(db, first, second, year, revenue) == if revenue {
        row_of(db, params(first, second), first, second, year as int).revenue
    } else { group_count(db, params(first, second), first, second, year as int) },
{
    let q = Measure { first, second, year, revenue };
    lemma_shipping0(db, q);
    lemma_rotate_supplier(db, q);
    let original = |left: NationRow| sum(db.nation, |right: NationRow| supplier_fold(db, q, left, right));
    let expected = |left: NationRow| sum(db.nation, |right: NationRow| if left.n_name == first && right.n_name == second {
        numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int });
    assert forall|left_index: int| 0 <= left_index < db.nation.len() implies original(#[trigger] db.nation[left_index]) == expected(db.nation[left_index]) by {
        let left = db.nation[left_index];
        let inner = |right: NationRow| supplier_fold(db, q, left, right);
        let target = |right: NationRow| if left.n_name == first && right.n_name == second {
            numeric_group(db, left.n_nationkey as i64, right.n_nationkey as i64, year, revenue) } else { 0int };
        assert forall|right_index: int| 0 <= right_index < db.nation.len() implies inner(#[trigger] db.nation[right_index]) == target(db.nation[right_index]) by {
            lemma_supplier_numeric(db, q, left, db.nation[right_index]);
        }
        lemma_sum_ext(db.nation, inner, target);
    }
    lemma_sum_ext(db.nation, original, expected);
    lemma_group_shipping(db, params(first, second), first, second, year as int, revenue);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_rotate_customer(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow, order: OrdersRow)
    ensures suffix3(db, q, supplier, line, order) == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow|
        customer_fold(db, q, supplier, line, order, left, right))),
{
    let weight = |customer: CustomerRow, left: NationRow, right: NationRow| scalar(q, supplier, line, order, customer, left, right);
    let original = |customer: CustomerRow| suffix4(db, q, supplier, line, order, customer);
    let target = |left: NationRow, right: NationRow| customer_fold(db, q, supplier, line, order, left, right);
    assert forall|customer: CustomerRow| #[trigger] original(customer) == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow| weight(customer, left, right))) by {
        let nested = |left: NationRow| sum(db.nation, |right: NationRow| weight(customer, left, right));
        let expected = |left: NationRow| suffix5(db, q, supplier, line, order, customer, left);
        assert forall|left: NationRow| #[trigger] nested(left) == expected(left) by {
            lemma_sum_ext_all(db.nation, |right: NationRow| weight(customer, left, right), |right: NationRow| scalar(q, supplier, line, order, customer, left, right));
        }
        lemma_sum_ext_all(db.nation, nested, expected);
    }
    assert forall|left: NationRow, right: NationRow| #[trigger] target(left, right) == sum(db.customer, |customer: CustomerRow| weight(customer, left, right)) by {
        lemma_sum_ext_all(db.customer, |customer: CustomerRow| weight(customer, left, right), |customer: CustomerRow| scalar(q, supplier, line, order, customer, left, right));
    }
    lemma_sum_rotate_named(db.customer, db.nation, db.nation, weight, original, target);
    let normalized = |left: NationRow| sum(db.nation, |right: NationRow| target(left, right));
    let expected = |left: NationRow| sum(db.nation, |right: NationRow| customer_fold(db, q, supplier, line, order, left, right));
    assert forall|left: NationRow| #[trigger] normalized(left) == expected(left) by {
        lemma_sum_ext_all(db.nation, |right: NationRow| target(left, right), |right: NationRow| customer_fold(db, q, supplier, line, order, left, right));
    }
    lemma_sum_ext_all(db.nation, normalized, expected);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_rotate_order(db: Db, q: Measure, supplier: SupplierRow, line: LineitemRow)
    ensures suffix2(db, q, supplier, line) == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow|
        order_fold(db, q, supplier, line, left, right))),
{
    let weight = |order: OrdersRow, left: NationRow, right: NationRow| customer_fold(db, q, supplier, line, order, left, right);
    let original = |order: OrdersRow| suffix3(db, q, supplier, line, order);
    let target = |left: NationRow, right: NationRow| order_fold(db, q, supplier, line, left, right);
    assert forall|order: OrdersRow| #[trigger] original(order) == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow| weight(order, left, right))) by {
        lemma_rotate_customer(db, q, supplier, line, order);
        let nested = |left: NationRow| sum(db.nation, |right: NationRow| weight(order, left, right));
        let expected = |left: NationRow| sum(db.nation, |right: NationRow| customer_fold(db, q, supplier, line, order, left, right));
        assert forall|left: NationRow| #[trigger] nested(left) == expected(left) by {
            lemma_sum_ext_all(db.nation, |right: NationRow| weight(order, left, right), |right: NationRow| customer_fold(db, q, supplier, line, order, left, right));
        }
        lemma_sum_ext_all(db.nation, nested, expected);
    }
    assert forall|left: NationRow, right: NationRow| #[trigger] target(left, right) == sum(db.orders, |order: OrdersRow| weight(order, left, right)) by {
        lemma_sum_ext_all(db.orders, |order: OrdersRow| weight(order, left, right), |order: OrdersRow| customer_fold(db, q, supplier, line, order, left, right));
    }
    lemma_sum_rotate_named(db.orders, db.nation, db.nation, weight, original, target);
    let normalized = |left: NationRow| sum(db.nation, |right: NationRow| target(left, right));
    let expected = |left: NationRow| sum(db.nation, |right: NationRow| order_fold(db, q, supplier, line, left, right));
    assert forall|left: NationRow| #[trigger] normalized(left) == expected(left) by {
        lemma_sum_ext_all(db.nation, |right: NationRow| target(left, right), |right: NationRow| order_fold(db, q, supplier, line, left, right));
    }
    lemma_sum_ext_all(db.nation, normalized, expected);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_rotate_line(db: Db, q: Measure, supplier: SupplierRow)
    ensures suffix1(db, q, supplier) == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow|
        line_fold(db, q, supplier, left, right))),
{
    let weight = |line: LineitemRow, left: NationRow, right: NationRow| order_fold(db, q, supplier, line, left, right);
    let original = |line: LineitemRow| suffix2(db, q, supplier, line);
    let target = |left: NationRow, right: NationRow| line_fold(db, q, supplier, left, right);
    assert forall|line: LineitemRow| #[trigger] original(line) == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow| weight(line, left, right))) by {
        lemma_rotate_order(db, q, supplier, line);
        let nested = |left: NationRow| sum(db.nation, |right: NationRow| weight(line, left, right));
        let expected = |left: NationRow| sum(db.nation, |right: NationRow| order_fold(db, q, supplier, line, left, right));
        assert forall|left: NationRow| #[trigger] nested(left) == expected(left) by {
            lemma_sum_ext_all(db.nation, |right: NationRow| weight(line, left, right), |right: NationRow| order_fold(db, q, supplier, line, left, right));
        }
        lemma_sum_ext_all(db.nation, nested, expected);
    }
    assert forall|left: NationRow, right: NationRow| #[trigger] target(left, right) == sum(db.lineitem, |line: LineitemRow| weight(line, left, right)) by {
        lemma_sum_ext_all(db.lineitem, |line: LineitemRow| weight(line, left, right), |line: LineitemRow| order_fold(db, q, supplier, line, left, right));
    }
    lemma_sum_rotate_named(db.lineitem, db.nation, db.nation, weight, original, target);
    let normalized = |left: NationRow| sum(db.nation, |right: NationRow| target(left, right));
    let expected = |left: NationRow| sum(db.nation, |right: NationRow| line_fold(db, q, supplier, left, right));
    assert forall|left: NationRow| #[trigger] normalized(left) == expected(left) by {
        lemma_sum_ext_all(db.nation, |right: NationRow| target(left, right), |right: NationRow| line_fold(db, q, supplier, left, right));
    }
    lemma_sum_ext_all(db.nation, normalized, expected);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_rotate_supplier(db: Db, q: Measure)
    ensures suffix0(db, q) == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow| supplier_fold(db, q, left, right))),
{
    let weight = |supplier: SupplierRow, left: NationRow, right: NationRow| line_fold(db, q, supplier, left, right);
    let original = |supplier: SupplierRow| suffix1(db, q, supplier);
    let target = |left: NationRow, right: NationRow| supplier_fold(db, q, left, right);
    assert forall|supplier: SupplierRow| #[trigger] original(supplier) == sum(db.nation, |left: NationRow| sum(db.nation, |right: NationRow| weight(supplier, left, right))) by {
        lemma_rotate_line(db, q, supplier);
        let nested = |left: NationRow| sum(db.nation, |right: NationRow| weight(supplier, left, right));
        let expected = |left: NationRow| sum(db.nation, |right: NationRow| line_fold(db, q, supplier, left, right));
        assert forall|left: NationRow| #[trigger] nested(left) == expected(left) by {
            lemma_sum_ext_all(db.nation, |right: NationRow| weight(supplier, left, right), |right: NationRow| line_fold(db, q, supplier, left, right));
        }
        lemma_sum_ext_all(db.nation, nested, expected);
    }
    assert forall|left: NationRow, right: NationRow| #[trigger] target(left, right) == sum(db.supplier, |supplier: SupplierRow| weight(supplier, left, right)) by {
        lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| weight(supplier, left, right), |supplier: SupplierRow| line_fold(db, q, supplier, left, right));
    }
    lemma_sum_rotate_named(db.supplier, db.nation, db.nation, weight, original, target);
    let normalized = |left: NationRow| sum(db.nation, |right: NationRow| target(left, right));
    let expected = |left: NationRow| sum(db.nation, |right: NationRow| supplier_fold(db, q, left, right));
    assert forall|left: NationRow| #[trigger] normalized(left) == expected(left) by {
        lemma_sum_ext_all(db.nation, |right: NationRow| target(left, right), |right: NationRow| supplier_fold(db, q, left, right));
    }
    lemma_sum_ext_all(db.nation, normalized, expected);
}

}
