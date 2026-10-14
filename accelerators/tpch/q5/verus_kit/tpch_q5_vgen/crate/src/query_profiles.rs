use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::meaning::*;
use crate::storage::*;
use crate::query_proofs::line_value;
use crate::output;

verus! {

pub open spec fn supplier_weight(supplier: SupplierRow, line: LineitemRow, nation: int) -> int {
    ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation)
}

pub open spec fn customer_weight(db: Db, customer: CustomerRow, order: OrdersRow, line: LineitemRow, nation: int) -> int {
    if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
        sum(db.supplier, |supplier: SupplierRow| supplier_weight(supplier, line, nation))
    } else { 0 }
}

pub open spec fn order_line_count(db: Db, order: OrdersRow, line: LineitemRow, nation: int) -> int {
    if order.o_orderkey == line.l_orderkey {
        sum(db.customer, |customer: CustomerRow| customer_weight(db, customer, order, line, nation))
    } else { 0 }
}

pub open spec fn order_total(db: Db, order: OrdersRow, nation: int, revenue: bool) -> int {
    sum(db.lineitem, |line: LineitemRow| order_line_count(db, order, line, nation) * line_value(line, revenue))
}

pub open spec fn profile_weight(db: Db, nation: i64, day: i32, revenue: bool) -> int {
    sum(db.lineitem, |line: LineitemRow| line_weight(db, line, nation, day) * line_value(line, revenue))
}

pub open spec fn database_window(db: Db, nation: int, start: i32, end: i32, revenue: bool) -> int {
    sum(db.orders, |order: OrdersRow| if start <= order.o_orderdate < end { order_total(db, order, nation, revenue) } else { 0 })
}

pub proof fn lemma_profile_weight(db: Db, nation: i64, day: i32, revenue: bool)
    ensures profile_weight(db, nation, day, revenue) == if revenue { profile(db, nation, day).1 } else { profile(db, nation, day).0 },
{
    if revenue {
        assert forall|line: LineitemRow| line_weight(db, line, nation, day) * #[trigger] line_value(line, revenue) ==
            line_weight(db, line, nation, day) * line.l_extendedprice * (100 - line.l_discount) by {
            assert(line_value(line, revenue) == line.l_extendedprice * (100 - line.l_discount));
            assert(line_weight(db, line, nation, day) * (line.l_extendedprice * (100 - line.l_discount)) ==
                line_weight(db, line, nation, day) * line.l_extendedprice * (100 - line.l_discount)) by (nonlinear_arith);
        }
        lemma_sum_ext_all(db.lineitem, |line: LineitemRow| line_weight(db, line, nation, day) * line_value(line, revenue),
            |line: LineitemRow| line_weight(db, line, nation, day) * line.l_extendedprice * (100 - line.l_discount));
    } else {
        lemma_sum_ext_all(db.lineitem, |line: LineitemRow| line_weight(db, line, nation, day) * line_value(line, revenue),
            |line: LineitemRow| line_weight(db, line, nation, day));
    }
}

pub proof fn lemma_order_expansion(db: Db, line: LineitemRow, nation: i64, day: i32)
    ensures line_weight(db, line, nation, day) == sum(db.orders, |order: OrdersRow|
        if order.o_orderdate == day { order_line_count(db, order, line, nation as int) } else { 0 }),
{
    let original = |order: OrdersRow| if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
        sum(db.customer, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
            sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
        } else { 0 })
    } else { 0 };
    let expanded = |order: OrdersRow| if order.o_orderdate == day { order_line_count(db, order, line, nation as int) } else { 0 };
    assert forall|order: OrdersRow| #[trigger] original(order) == expanded(order) by {
        if order.o_orderkey == line.l_orderkey && order.o_orderdate == day {
            assert((|supplier: SupplierRow| supplier_weight(supplier, line, nation as int)) =~=
                (|supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation)));
            assert((|customer: CustomerRow| customer_weight(db, customer, order, line, nation as int)) =~=
                (|customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == nation {
                    sum(db.supplier, |supplier: SupplierRow| ind(supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == nation))
                } else { 0 }));
        }
    }
    lemma_sum_ext_all(db.orders, original, expanded);
}

pub proof fn lemma_profile_orders(db: Db, nation: i64, day: i32, revenue: bool)
    ensures profile_weight(db, nation, day, revenue) == sum(db.orders, |order: OrdersRow|
        if order.o_orderdate == day { order_total(db, order, nation as int, revenue) } else { 0 }),
{
    let joint = |line: LineitemRow, order: OrdersRow| if order.o_orderdate == day {
        order_line_count(db, order, line, nation as int) * line_value(line, revenue)
    } else { 0 };
    let by_line = |line: LineitemRow| line_weight(db, line, nation, day) * line_value(line, revenue);
    let by_order = |order: OrdersRow| if order.o_orderdate == day { order_total(db, order, nation as int, revenue) } else { 0 };
    assert forall|line: LineitemRow| #[trigger] by_line(line) == sum(db.orders, |order: OrdersRow| joint(line, order)) by {
        lemma_order_expansion(db, line, nation, day);
        let count = |order: OrdersRow| if order.o_orderdate == day { order_line_count(db, order, line, nation as int) } else { 0 };
        let scaled = |order: OrdersRow| joint(line, order);
        assert forall|order: OrdersRow| #[trigger] scaled(order) == line_value(line, revenue) * count(order) by {
            assert(order_line_count(db, order, line, nation as int) * line_value(line, revenue) ==
                line_value(line, revenue) * order_line_count(db, order, line, nation as int)) by (nonlinear_arith);
        }
        lemma_sum_scale(db.orders, count, scaled, line_value(line, revenue));
        assert(sum(db.orders, count) * line_value(line, revenue) == line_value(line, revenue) * sum(db.orders, count)) by (nonlinear_arith);
    }
    assert forall|order: OrdersRow| #[trigger] by_order(order) == sum(db.lineitem, |line: LineitemRow| joint(line, order)) by {
        if order.o_orderdate == day {
            assert((|line: LineitemRow| joint(line, order)) =~=
                (|line: LineitemRow| order_line_count(db, order, line, nation as int) * line_value(line, revenue)));
        } else { lemma_sum_zero(db.lineitem, |line: LineitemRow| joint(line, order)); }
    }
    lemma_sum_swap(db.lineitem, db.orders, joint, by_line, by_order);
}

pub proof fn lemma_window_database(profiles: Table, db: Db, nation: i64, start: i32, end: i32, revenue: bool)
    requires profiles.wf(), profiles.daily(), projections_representable(db),
        forall|day: i32| #[trigger] profiles.at(nation, 0, day) == profile(db, nation, day),
    ensures (if revenue { output::window_value(profiles, nation, start, end).1 } else { output::window_value(profiles, nation, start, end).0 }) ==
        database_window(db, nation as int, start, end, revenue),
{
    let rows = profiles.rows(nation);
    let left_project = |cell: Cell| cell.day;
    let right_project = |order: OrdersRow| order.o_orderdate as i32;
    let left_multiplicity = |cell: Cell| if revenue { cell.revenue as int } else { cell.count as int };
    let right_multiplicity = |order: OrdersRow| order_total(db, order, nation as int, revenue);
    let weight = |day: i32| ind(start <= day < end);
    assert forall|day: i32| #[trigger] verus_prooflib::class_total(rows, left_project, left_multiplicity, day) ==
        verus_prooflib::class_total(db.orders, right_project, right_multiplicity, day) by {
        profiles.lemma_rows(nation, 0, day);
        let left = |cell: Cell| if cell.day == day { left_multiplicity(cell) } else { 0 };
        let matching = |cell: Cell| if cell.sub == 0 && cell.day == day { left_multiplicity(cell) } else { 0 };
        assert forall|position: int| 0 <= position < rows.len() implies left(#[trigger] rows[position]) == matching(rows[position]) by {
            if rows[position].sub != 0 {
                assert(profiles.at(nation, rows[position].sub, rows[position].day) == (0, 0));
            }
        }
        lemma_sum_ext(rows, left, matching);
        lemma_sum_ext_all(rows, |cell: Cell| if left_project(cell) == day { left_multiplicity(cell) } else { 0 }, left);
        let right = |order: OrdersRow| if right_project(order) == day { right_multiplicity(order) } else { 0 };
        let uncast = |order: OrdersRow| if order.o_orderdate == day { order_total(db, order, nation as int, revenue) } else { 0 };
        assert forall|position: int| 0 <= position < db.orders.len() implies right(#[trigger] db.orders[position]) == uncast(db.orders[position]) by {
            assert(order_representable(db.orders[position]));
        }
        lemma_sum_ext(db.orders, right, uncast);
        lemma_sum_ext_all(db.orders, |order: OrdersRow| if right_project(order) == day { right_multiplicity(order) } else { 0 }, right);
        lemma_profile_orders(db, nation, day, revenue);
        lemma_profile_weight(db, nation, day, revenue);
        if revenue {
            lemma_sum_ext_all(rows, matching, |cell: Cell| if cell.sub == 0 && cell.day == day { cell.revenue as int } else { 0 });
        } else {
            lemma_sum_ext_all(rows, matching, |cell: Cell| if cell.sub == 0 && cell.day == day { cell.count as int } else { 0 });
        }
    }
    verus_prooflib::lemma_equal_classes(rows, db.orders, left_project, right_project, left_multiplicity, right_multiplicity, weight);
    if revenue {
        lemma_sum_ext_all(rows, |cell: Cell| left_multiplicity(cell) * weight(left_project(cell)),
            |cell: Cell| if start <= cell.day < end { cell.revenue as int } else { 0 });
    } else {
        lemma_sum_ext_all(rows, |cell: Cell| left_multiplicity(cell) * weight(left_project(cell)),
            |cell: Cell| if start <= cell.day < end { cell.count as int } else { 0 });
    }
    let projected = |order: OrdersRow| right_multiplicity(order) * weight(right_project(order));
    let selected = |order: OrdersRow| if start <= order.o_orderdate < end { order_total(db, order, nation as int, revenue) } else { 0 };
    assert forall|position: int| 0 <= position < db.orders.len() implies projected(#[trigger] db.orders[position]) == selected(db.orders[position]) by {
        assert(order_representable(db.orders[position]));
    }
    lemma_sum_ext(db.orders, projected, selected);
}

}
