use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::storage::Table;
use crate::storage::Cell;
use crate::gen_obligations::*;
use crate::maintenance;

verus! {

pub open spec fn customer_representable(row: CustomerRow) -> bool {
    i64::MIN <= row.c_custkey <= i64::MAX && i64::MIN <= row.c_nationkey <= i64::MAX
}

pub open spec fn supplier_representable(row: SupplierRow) -> bool {
    i64::MIN <= row.s_suppkey <= i64::MAX && i64::MIN <= row.s_nationkey <= i64::MAX
}

pub open spec fn order_representable(row: OrdersRow) -> bool {
    i64::MIN <= row.o_orderkey <= i64::MAX && i64::MIN <= row.o_custkey <= i64::MAX
        && i32::MIN <= row.o_orderdate <= i32::MAX
}

pub open spec fn nation_representable(row: NationRow) -> bool {
    i64::MIN <= row.n_nationkey <= i64::MAX && i64::MIN <= row.n_regionkey <= i64::MAX
}

pub open spec fn region_representable(row: RegionRow) -> bool {
    i64::MIN <= row.r_regionkey <= i64::MAX
}

pub open spec fn projections_representable(db: Db) -> bool {
    &&& forall|index: int| 0 <= index < db.customer.len() ==> #[trigger] customer_representable(db.customer[index])
    &&& forall|index: int| 0 <= index < db.supplier.len() ==> #[trigger] supplier_representable(db.supplier[index])
    &&& forall|index: int| 0 <= index < db.orders.len() ==> #[trigger] order_representable(db.orders[index])
    &&& forall|index: int| 0 <= index < db.nation.len() ==> #[trigger] nation_representable(db.nation[index])
    &&& forall|index: int| 0 <= index < db.region.len() ==> #[trigger] region_representable(db.region[index])
}

pub open spec fn customers_match(table: Table, db: Db) -> bool {
    &&& table.wf() && table.dimension()
    &&& forall|key: i64, nation: i64, day: i32| #[trigger] table.at(key, nation, day) ==
        (if day == 0 { customer_count(db, key, nation) } else { 0 }, 0)
}

pub open spec fn suppliers_match(table: Table, db: Db) -> bool {
    &&& table.wf() && table.dimension()
    &&& forall|key: i64, nation: i64, day: i32| #[trigger] table.at(key, nation, day) ==
        (if day == 0 { supplier_count(db, key, nation) } else { 0 }, 0)
}

pub open spec fn orders_match(table: Table, db: Db) -> bool {
    &&& table.wf() && table.counts_nonnegative()
    &&& forall|key: i64, customer: i64, day: i32| #[trigger] table.at(key, customer, day) ==
        (order_count(db, key, customer, day), 0)
}

pub open spec fn reverse_match(table: Table, db: Db) -> bool {
    &&& table.wf() && table.dimension()
    &&& forall|customer: i64, order: i64, day: i32| #[trigger] table.at(customer, order, day) ==
        (if day == 0 { order_references(db, customer, order) } else { 0 }, 0)
}

pub open spec fn customer_count(db: Db, key: i64, nation: i64) -> int {
    sum(db.customer, |row: CustomerRow| ind(row.c_custkey == key && row.c_nationkey == nation))
}

pub open spec fn supplier_count(db: Db, key: i64, nation: i64) -> int {
    sum(db.supplier, |row: SupplierRow| ind(row.s_suppkey == key && row.s_nationkey == nation))
}

pub open spec fn order_count(db: Db, key: i64, customer: i64, day: i32) -> int {
    sum(db.orders, |row: OrdersRow| ind(row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == day))
}

pub open spec fn order_references(db: Db, customer: i64, order: i64) -> int {
    sum(db.orders, |row: OrdersRow| ind(row.o_orderkey == order && row.o_custkey == customer))
}

pub open spec fn line_weight(db: Db, line: LineitemRow, nation: i64, day: i32) -> int {
    sum(db.orders, |order: OrdersRow|
        if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
            sum(db.customer, |customer: CustomerRow|
                if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
                    sum(db.supplier, |supplier: SupplierRow|
                        ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
                } else { 0 })
        } else { 0 })
}

pub open spec fn profile(db: Db, nation: i64, day: i32) -> (int, int) {
    (sum(db.lineitem, |line: LineitemRow| line_weight(db, line, nation, day)),
     sum(db.lineitem, |line: LineitemRow| line_weight(db, line, nation, day) * line.l_extendedprice * (100 - line.l_discount)))
}

pub open spec fn nation_count(db: Db, key: i64, region: i64, name: Seq<u8>) -> int {
    sum(db.nation, |row: NationRow| ind(row.n_nationkey == key && row.n_regionkey == region && row.n_name == name))
}

pub open spec fn region_count(db: Db, key: i64, name: Seq<u8>) -> int {
    sum(db.region, |row: RegionRow| ind(row.r_regionkey == key && row.r_name == name))
}

pub proof fn lemma_factored_weight(db: Db, line: LineitemRow, nation: i64, day: i32)
    requires forall|position: int| 0 <= position < db.orders.len() ==> #[trigger] order_representable(db.orders[position]),
        i64::MIN <= line.l_suppkey <= i64::MAX,
    ensures line_weight(db, line, nation, day) == sum(db.orders, |order: OrdersRow|
        if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
            customer_count(db, order.o_custkey as i64, nation) * supplier_count(db, line.l_suppkey as i64, nation)
        } else { 0 }),
{
    assert(i64::MIN <= line.l_suppkey <= i64::MAX);
    let expanded = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
        sum(db.customer, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
            sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
        } else { 0 })
    } else { 0 };
    let factored = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
        customer_count(db, order.o_custkey as i64, nation) * supplier_count(db, line.l_suppkey as i64, nation)
    } else { 0 };
    assert forall|position: int| 0 <= position < db.orders.len() implies expanded(#[trigger] db.orders[position]) == factored(db.orders[position]) by {
        let order = db.orders[position];
        if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
            assert(order_representable(order));
            let raw = |customer: CustomerRow| ind(customer.c_custkey == order.o_custkey && customer.c_nationkey == nation);
            let scaled = |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
                sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
            } else { 0 };
            lemma_sum_scale(db.customer, raw, scaled, supplier_count(db, line.l_suppkey as i64, nation));
            assert(supplier_count(db, line.l_suppkey as i64, nation) * customer_count(db, order.o_custkey as i64, nation) ==
                customer_count(db, order.o_custkey as i64, nation) * supplier_count(db, line.l_suppkey as i64, nation)) by (nonlinear_arith);
        }
    }
    lemma_sum_ext(db.orders, expanded, factored);
}

pub proof fn lemma_line_weight(customers: Table, orders: Table, suppliers: Table, db: Db,
    line: LineitemInput, nation: i64, day: i32)
    requires customers_match(customers, db), orders_match(orders, db), suppliers_match(suppliers, db),
        forall|position: int| 0 <= position < db.orders.len() ==> #[trigger] order_representable(db.orders[position]),
    ensures maintenance::contribution(customers, orders, suppliers, line, nation, day) ==
        (line_weight(db, lineitem_input_view(line), nation, day),
         line_weight(db, lineitem_input_view(line), nation, day) * line.l_extendedprice * (100 - line.l_discount as int)),
{
    let rows = orders.rows(line.l_orderkey);
    let left_project = |cell: Cell| (cell.sub, cell.day);
    let right_project = |order: OrdersRow| (order.o_custkey as i64, order.o_orderdate as i32);
    let left_multiplicity = |cell: Cell| cell.count as int;
    let right_multiplicity = |order: OrdersRow| ind(order.o_orderkey == line.l_orderkey);
    let weight = |key: (i64, i32)| if key.1 == day {
        customer_count(db, key.0, nation) * supplier_count(db, line.l_suppkey, nation)
    } else { 0 };
    assert forall|key: (i64, i32)| #[trigger] verus_prooflib::class_total(rows, left_project, left_multiplicity, key) ==
        verus_prooflib::class_total(db.orders, right_project, right_multiplicity, key) by {
        orders.lemma_rows(line.l_orderkey, key.0, key.1);
        lemma_sum_ext_all(rows, |cell: Cell| if left_project(cell) == key { left_multiplicity(cell) } else { 0 },
            |cell: Cell| if cell.sub == key.0 && cell.day == key.1 { cell.count as int } else { 0 });
        let projected = |order: OrdersRow| if right_project(order) == key { right_multiplicity(order) } else { 0 };
        let matching = |order: OrdersRow| ind(order.o_orderkey == line.l_orderkey && order.o_custkey == key.0 && order.o_orderdate == key.1);
        assert forall|position: int| 0 <= position < db.orders.len() implies projected(#[trigger] db.orders[position]) == matching(db.orders[position]) by {
            assert(order_representable(db.orders[position]));
        }
        lemma_sum_ext(db.orders, projected, matching);
    }
    verus_prooflib::lemma_equal_classes(rows, db.orders, left_project, right_project, left_multiplicity, right_multiplicity, weight);
    let runtime = |order: Cell| if order.day == day { order.count as int * customers.at(order.sub, nation, 0).0 * suppliers.at(line.l_suppkey, nation, 0).0 } else { 0 };
    assert forall|order: Cell| #[trigger] runtime(order) == left_multiplicity(order) * weight(left_project(order)) by {
        assert(order.count as int * customer_count(db, order.sub, nation) * supplier_count(db, line.l_suppkey, nation) ==
            order.count as int * (customer_count(db, order.sub, nation) * supplier_count(db, line.l_suppkey, nation))) by (nonlinear_arith);
    }
    lemma_sum_ext_all(rows, runtime, |order: Cell| left_multiplicity(order) * weight(left_project(order)));
    let factored = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
        customer_count(db, order.o_custkey as i64, nation) * supplier_count(db, line.l_suppkey, nation)
    } else { 0 };
    assert forall|position: int| 0 <= position < db.orders.len() implies
        right_multiplicity(#[trigger] db.orders[position]) * weight(right_project(db.orders[position])) == factored(db.orders[position]) by {
        assert(order_representable(db.orders[position]));
    }
    lemma_sum_ext(db.orders, |order: OrdersRow| right_multiplicity(order) * weight(right_project(order)), factored);
    lemma_factored_weight(db, lineitem_input_view(line), nation, day);
}

pub proof fn lemma_excluded_line(db: Db, line: LineitemRow, nation: i64, day: i32)
    requires !scan_lines_exists_1(db, line),
    ensures line_weight(db, line, nation, day) == 0,
{
    let outer = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
        sum(db.customer, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
            sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
        } else { 0 })
    } else { 0 };
    assert forall|order_index: int| 0 <= order_index < db.orders.len() implies outer(#[trigger] db.orders[order_index]) == 0 by {
        let order = db.orders[order_index];
        if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
            assert(!scan_lines_exists_2(db, order, line));
            let inner = |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
                sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
            } else { 0 };
            assert forall|customer_index: int| 0 <= customer_index < db.customer.len() implies inner(#[trigger] db.customer[customer_index]) == 0 by {
                let customer = db.customer[customer_index];
                if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
                    assert(!scan_lines_exists_3(db, line, customer));
                    assert forall|supplier_index: int| 0 <= supplier_index < db.supplier.len() implies
                        (#[trigger] db.supplier[supplier_index]).s_suppkey != line.l_suppkey || db.supplier[supplier_index].s_nationkey != nation by {
                        assert(!scan_lines_exists_3_where(line, customer, db.supplier[supplier_index]));
                    }
                    lemma_sum_zero(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation));
                }
            }
            lemma_sum_zero(db.customer, inner);
        }
    }
    lemma_sum_zero(db.orders, outer);
}

pub proof fn lemma_line_nonnegative(db: Db, line: LineitemRow, nation: i64, day: i32)
    ensures line_weight(db, line, nation, day) >= 0,
{
    let supplier_weight = |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation);
    lemma_sum_nonneg(db.supplier, supplier_weight);
    assert forall|order_index: int| 0 <= order_index < db.orders.len() implies
        sum(db.customer, |customer: CustomerRow|
            if customer.c_custkey == (#[trigger] db.orders[order_index]).o_custkey && customer.c_nationkey == nation {
                sum(db.supplier, supplier_weight)
            } else { 0 }) >= 0 by {
        lemma_sum_nonneg(db.customer, |customer: CustomerRow|
            if customer.c_custkey == db.orders[order_index].o_custkey && customer.c_nationkey == nation { sum(db.supplier, supplier_weight) } else { 0 });
    }
    lemma_sum_nonneg(db.orders, |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
        sum(db.customer, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
            sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
        } else { 0 })
    } else { 0 });
}

pub proof fn lemma_profile_shape(db: Db, nation: i64, day: i32)
    ensures profile(db, nation, day).0 >= 0,
        profile(db, nation, day).0 == 0 ==> profile(db, nation, day).1 == 0,
{
    let weight = |line: LineitemRow| line_weight(db, line, nation, day);
    assert forall|position: int| 0 <= position < db.lineitem.len() implies weight(#[trigger] db.lineitem[position]) >= 0 by {
        lemma_line_nonnegative(db, db.lineitem[position], nation, day);
    }
    lemma_sum_nonneg(db.lineitem, weight);
    if profile(db, nation, day).0 == 0 {
        let revenue = |line: LineitemRow| line_weight(db, line, nation, day) * line.l_extendedprice * (100 - line.l_discount);
        assert forall|position: int| 0 <= position < db.lineitem.len() implies revenue(#[trigger] db.lineitem[position]) == 0 by {
            lemma_sum_ge_term(db.lineitem, weight, position);
            assert(weight(db.lineitem[position]) == 0);
            assert(line_weight(db, db.lineitem[position], nation, day) == 0);
            assert(0int * db.lineitem[position].l_extendedprice * (100 - db.lineitem[position].l_discount) == 0) by (nonlinear_arith);
        }
        lemma_sum_zero(db.lineitem, revenue);
    }
}

}
