use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::meaning::*;

verus! {

pub fn customers(rows: &[CustomerInput], Ghost(db): Ghost<Db>) -> (out: Option<Table>)
    requires scan_customers_ok(db, rows@),
    ensures out matches Some(table) ==> customers_match(table, db),
        forall|position: int| 0 <= position < db.customer.len() ==> #[trigger] customer_representable(db.customer[position]),
{
    proof {
        verus_prooflib::lemma_scan_property(customer_input_seq_view(rows@), db.customer, |row: CustomerRow| customer_representable(row));
    }
    let mut hints = count_hints();
    let mut hint_index = 0;
    while hint_index < rows.len()
        invariant hint_index <= rows.len(), hints@.len() == 1048576,
        decreases rows.len() - hint_index,
    {
        bump_hint(&mut hints, rows[hint_index].c_custkey);
        hint_index += 1;
    }
    let mut bucket = Builder::with_counts(&hints);
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index, bucket.wf(),
            forall|position: int| 0 <= position < db.customer.len() ==> #[trigger] customer_representable(db.customer[position]),
            forall|key: i64, nation: i64, day: i32| #[trigger] bucket.at(key, nation, day) ==
                (sum(rows@.take(processed@), |row: CustomerInput| ind(row.c_custkey == key && row.c_nationkey == nation && day == 0)), 0),
        decreases rows.len() - index,
    {
        let row = rows[index];
        if !bucket.add(row.c_custkey, row.c_nationkey, 0, 1, 0) { return None; }
        proof {
            assert forall|key: i64, nation: i64, day: i32| #[trigger] bucket.at(key, nation, day) ==
                (sum(rows@.take(index as int + 1), |row: CustomerInput| ind(row.c_custkey == key && row.c_nationkey == nation && day == 0)), 0) by {
                lemma_sum_take(rows@, index as int, |row: CustomerInput| ind(row.c_custkey == key && row.c_nationkey == nation && day == 0));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let table = bucket.freeze();
    proof {
        assert(rows@.take(rows@.len() as int) =~= rows@);
        assert forall|key: i64, nation: i64, day: i32| #[trigger] table.at(key, nation, day) ==
            (if day == 0 { customer_count(db, key, nation) } else { 0 }, 0) by {
            let input_weight = |row: CustomerInput| ind(row.c_custkey == key && row.c_nationkey == nation && day == 0);
            let weight = |row: CustomerRow| ind(row.c_custkey == key && row.c_nationkey == nation && day == 0);
            lemma_sum_map(rows@, |row: CustomerInput| customer_input_view(row), weight, input_weight);
            assert(sum(customer_input_seq_view(rows@), weight) == sum(db.customer, weight));
            if day != 0 { lemma_sum_zero(db.customer, weight); }
            else { lemma_sum_ext_all(db.customer, weight, |row: CustomerRow| ind(row.c_custkey == key && row.c_nationkey == nation)); }
        }
        assert forall|key: i64, nation: i64, day: i32| (#[trigger] table.at(key, nation, day)).0 >= 0 by {
            lemma_sum_nonneg(db.customer, |row: CustomerRow| ind(row.c_custkey == key && row.c_nationkey == nation));
        }
        assert(table.dimension());
    }
    Some(table)
}

pub fn nations(rows: &Vec<NationInput>, Ghost(db): Ghost<Db>) -> (out: Option<Vec<crate::catalog::Binding>>)
    requires scan_nations_ok(db, rows@),
    ensures out matches Some(values) ==> (forall|key: i64, area: i64, text: Seq<u8>| #[trigger] crate::catalog::binding_count(values@, key, area, text) == nation_count(db, key, area, text))
        && (forall|position: int| 0 <= position < values@.len() ==> (#[trigger] values@[position]).count > 0),
        forall|position: int| 0 <= position < db.nation.len() ==> #[trigger] nation_representable(db.nation[position]),
{
    proof { verus_prooflib::lemma_scan_property(nation_input_seq_view(rows@), db.nation, |row: NationRow| nation_representable(row)); }
    let mut values: Vec<crate::catalog::Binding> = Vec::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index,
            forall|position: int| 0 <= position < db.nation.len() ==> #[trigger] nation_representable(db.nation[position]),
            forall|position: int| 0 <= position < values@.len() ==> (#[trigger] values@[position]).count > 0,
            forall|key: i64, area: i64, text: Seq<u8>| #[trigger] crate::catalog::binding_count(values@, key, area, text) ==
                sum(rows@.take(processed@), |row: NationInput| ind(row.n_nationkey == key && row.n_regionkey == area && row.n_name@ == text)),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        if !crate::catalog::add(&mut values, row.n_nationkey, row.n_regionkey, &row.n_name, 1) { return None; }
        proof {
            assert forall|key: i64, area: i64, text: Seq<u8>| #[trigger] crate::catalog::binding_count(values@, key, area, text) ==
                sum(rows@.take(index as int + 1), |row: NationInput| ind(row.n_nationkey == key && row.n_regionkey == area && row.n_name@ == text)) by {
                lemma_sum_take(rows@, index as int, |row: NationInput| ind(row.n_nationkey == key && row.n_regionkey == area && row.n_name@ == text));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(rows@.take(rows@.len() as int) =~= rows@);
        assert forall|key: i64, area: i64, text: Seq<u8>| #[trigger] crate::catalog::binding_count(values@, key, area, text) == nation_count(db, key, area, text) by {
            let weight = |row: NationRow| ind(row.n_nationkey == key && row.n_regionkey == area && row.n_name == text);
            lemma_sum_map(rows@, |row: NationInput| nation_input_view(row), weight, |row: NationInput| ind(row.n_nationkey == key && row.n_regionkey == area && row.n_name@ == text));
            assert(sum(nation_input_seq_view(rows@), weight) == sum(db.nation, weight));
        }
    }
    Some(values)
}

pub fn regions(rows: &Vec<RegionInput>, Ghost(db): Ghost<Db>) -> (out: Option<Vec<crate::catalog::Binding>>)
    requires scan_regions_ok(db, rows@),
    ensures out matches Some(values) ==> (forall|key: i64, area: i64, text: Seq<u8>| #[trigger] crate::catalog::binding_count(values@, key, area, text) == if area == 0 { region_count(db, key, text) } else { 0 })
        && (forall|position: int| 0 <= position < values@.len() ==> (#[trigger] values@[position]).count > 0),
        forall|position: int| 0 <= position < db.region.len() ==> #[trigger] region_representable(db.region[position]),
{
    proof { verus_prooflib::lemma_scan_property(region_input_seq_view(rows@), db.region, |row: RegionRow| region_representable(row)); }
    let mut values: Vec<crate::catalog::Binding> = Vec::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index,
            forall|position: int| 0 <= position < db.region.len() ==> #[trigger] region_representable(db.region[position]),
            forall|position: int| 0 <= position < values@.len() ==> (#[trigger] values@[position]).count > 0,
            forall|key: i64, area: i64, text: Seq<u8>| #[trigger] crate::catalog::binding_count(values@, key, area, text) ==
                sum(rows@.take(processed@), |row: RegionInput| ind(row.r_regionkey == key && area == 0 && row.r_name@ == text)),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        if !crate::catalog::add(&mut values, row.r_regionkey, 0, &row.r_name, 1) { return None; }
        proof {
            assert forall|key: i64, area: i64, text: Seq<u8>| #[trigger] crate::catalog::binding_count(values@, key, area, text) ==
                sum(rows@.take(index as int + 1), |row: RegionInput| ind(row.r_regionkey == key && area == 0 && row.r_name@ == text)) by {
                lemma_sum_take(rows@, index as int, |row: RegionInput| ind(row.r_regionkey == key && area == 0 && row.r_name@ == text));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(rows@.take(rows@.len() as int) =~= rows@);
        assert forall|key: i64, area: i64, text: Seq<u8>| #[trigger] crate::catalog::binding_count(values@, key, area, text) == if area == 0 { region_count(db, key, text) } else { 0 } by {
            let weight = |row: RegionRow| ind(row.r_regionkey == key && area == 0 && row.r_name == text);
            lemma_sum_map(rows@, |row: RegionInput| region_input_view(row), weight, |row: RegionInput| ind(row.r_regionkey == key && area == 0 && row.r_name@ == text));
            assert(sum(region_input_seq_view(rows@), weight) == sum(db.region, weight));
            if area != 0 { lemma_sum_zero(db.region, weight); }
            else { lemma_sum_ext_all(db.region, weight, |row: RegionRow| ind(row.r_regionkey == key && row.r_name == text)); }
        }
    }
    Some(values)
}

pub fn suppliers(rows: &[SupplierInput], Ghost(db): Ghost<Db>) -> (out: Option<Table>)
    requires scan_suppliers_ok(db, rows@),
    ensures out matches Some(table) ==> suppliers_match(table, db),
        forall|position: int| 0 <= position < db.supplier.len() ==> #[trigger] supplier_representable(db.supplier[position]),
{
    proof {
        verus_prooflib::lemma_scan_property(supplier_input_seq_view(rows@), db.supplier, |row: SupplierRow| supplier_representable(row));
    }
    let mut hints = count_hints();
    let mut hint_index = 0;
    while hint_index < rows.len()
        invariant hint_index <= rows.len(), hints@.len() == 1048576,
        decreases rows.len() - hint_index,
    {
        bump_hint(&mut hints, rows[hint_index].s_suppkey);
        hint_index += 1;
    }
    let mut bucket = Builder::with_counts(&hints);
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index, bucket.wf(),
            forall|position: int| 0 <= position < db.supplier.len() ==> #[trigger] supplier_representable(db.supplier[position]),
            forall|key: i64, nation: i64, day: i32| #[trigger] bucket.at(key, nation, day) ==
                (sum(rows@.take(processed@), |row: SupplierInput| ind(row.s_suppkey == key && row.s_nationkey == nation && day == 0)), 0),
        decreases rows.len() - index,
    {
        let row = rows[index];
        if !bucket.add(row.s_suppkey, row.s_nationkey, 0, 1, 0) { return None; }
        proof {
            assert forall|key: i64, nation: i64, day: i32| #[trigger] bucket.at(key, nation, day) ==
                (sum(rows@.take(index as int + 1), |row: SupplierInput| ind(row.s_suppkey == key && row.s_nationkey == nation && day == 0)), 0) by {
                lemma_sum_take(rows@, index as int, |row: SupplierInput| ind(row.s_suppkey == key && row.s_nationkey == nation && day == 0));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let table = bucket.freeze();
    proof {
        assert(rows@.take(rows@.len() as int) =~= rows@);
        assert forall|key: i64, nation: i64, day: i32| #[trigger] table.at(key, nation, day) ==
            (if day == 0 { supplier_count(db, key, nation) } else { 0 }, 0) by {
            let input_weight = |row: SupplierInput| ind(row.s_suppkey == key && row.s_nationkey == nation && day == 0);
            let weight = |row: SupplierRow| ind(row.s_suppkey == key && row.s_nationkey == nation && day == 0);
            lemma_sum_map(rows@, |row: SupplierInput| supplier_input_view(row), weight, input_weight);
            assert(sum(supplier_input_seq_view(rows@), weight) == sum(db.supplier, weight));
            if day != 0 { lemma_sum_zero(db.supplier, weight); }
            else { lemma_sum_ext_all(db.supplier, weight, |row: SupplierRow| ind(row.s_suppkey == key && row.s_nationkey == nation)); }
        }
        assert forall|key: i64, nation: i64, day: i32| (#[trigger] table.at(key, nation, day)).0 >= 0 by {
            lemma_sum_nonneg(db.supplier, |row: SupplierRow| ind(row.s_suppkey == key && row.s_nationkey == nation));
        }
        assert(table.dimension());
    }
    Some(table)
}

pub fn orders(rows: &[OrdersInput], Ghost(db): Ghost<Db>) -> (out: Option<(Table, Table)>)
    requires scan_orders_ok(db, rows@),
    ensures out matches Some(tables) ==> orders_match(tables.0, db) && reverse_match(tables.1, db),
        forall|position: int| 0 <= position < db.orders.len() ==> #[trigger] order_representable(db.orders[position]),
{
    proof { verus_prooflib::lemma_scan_property(orders_input_seq_view(rows@), db.orders, |row: OrdersRow| order_representable(row)); }
    let mut order_hints = count_hints();
    let mut reverse_hints = count_hints();
    let mut hint_index = 0;
    while hint_index < rows.len()
        invariant hint_index <= rows.len(), order_hints@.len() == 1048576, reverse_hints@.len() == 1048576,
        decreases rows.len() - hint_index,
    {
        bump_hint(&mut order_hints, rows[hint_index].o_orderkey);
        bump_hint(&mut reverse_hints, rows[hint_index].o_custkey);
        hint_index += 1;
    }
    let mut order_rows = Builder::with_counts(&order_hints);
    let mut reverse_rows = Builder::with_counts(&reverse_hints);
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index, order_rows.wf(), reverse_rows.wf(),
            forall|position: int| 0 <= position < db.orders.len() ==> #[trigger] order_representable(db.orders[position]),
            forall|key: i64, customer: i64, day: i32| #[trigger] order_rows.at(key, customer, day) ==
                (sum(rows@.take(processed@), |row: OrdersInput| ind(row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == day)), 0),
            forall|customer: i64, key: i64, day: i32| #[trigger] reverse_rows.at(customer, key, day) ==
                (sum(rows@.take(processed@), |row: OrdersInput| ind(row.o_orderkey == key && row.o_custkey == customer && day == 0)), 0),
        decreases rows.len() - index,
    {
        let row = rows[index];
        if !order_rows.add(row.o_orderkey, row.o_custkey, row.o_orderdate, 1, 0) { return None; }
        proof {
            assert forall|key: i64, customer: i64, day: i32| #[trigger] order_rows.at(key, customer, day) ==
                (sum(rows@.take(index as int + 1), |row: OrdersInput| ind(row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == day)), 0) by {
                lemma_sum_take(rows@, index as int, |row: OrdersInput| ind(row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == day));
            }
        }
        if !reverse_rows.add(row.o_custkey, row.o_orderkey, 0, 1, 0) { return None; }
        proof {
            assert forall|customer: i64, key: i64, day: i32| #[trigger] reverse_rows.at(customer, key, day) ==
                (sum(rows@.take(index as int + 1), |row: OrdersInput| ind(row.o_orderkey == key && row.o_custkey == customer && day == 0)), 0) by {
                let weight = |row: OrdersInput| ind(row.o_orderkey == key && row.o_custkey == customer && day == 0);
                lemma_sum_take(rows@, index as int, weight);
                lemma_sum_nonneg(rows@.take(index as int), weight);
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let orders = order_rows.freeze();
    let reverse = reverse_rows.freeze();
    proof {
        assert(rows@.take(rows@.len() as int) =~= rows@);
        assert forall|key: i64, customer: i64, day: i32| #[trigger] orders.at(key, customer, day) ==
            (order_count(db, key, customer, day), 0) by {
            let input_weight = |row: OrdersInput| ind(row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == day);
            let weight = |row: OrdersRow| ind(row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == day);
            lemma_sum_map(rows@, |row: OrdersInput| orders_input_view(row), weight, input_weight);
            assert(sum(orders_input_seq_view(rows@), weight) == sum(db.orders, weight));
            lemma_sum_nonneg(db.orders, weight);
        }
        assert forall|customer: i64, key: i64, day: i32| #[trigger] reverse.at(customer, key, day) ==
            (if day == 0 { order_references(db, customer, key) } else { 0 }, 0) by {
            let input_weight = |row: OrdersInput| ind(row.o_orderkey == key && row.o_custkey == customer && day == 0);
            let weight = |row: OrdersRow| ind(row.o_orderkey == key && row.o_custkey == customer && day == 0);
            lemma_sum_map(rows@, |row: OrdersInput| orders_input_view(row), weight, input_weight);
            assert(sum(orders_input_seq_view(rows@), weight) == sum(db.orders, weight));
            if day == 0 {
                lemma_sum_ext_all(db.orders, weight, |row: OrdersRow| ind(row.o_orderkey == key && row.o_custkey == customer));
            } else { lemma_sum_zero(db.orders, weight); }
            lemma_sum_nonneg(db.orders, |row: OrdersRow| ind(row.o_orderkey == key && row.o_custkey == customer));
        }
        assert forall|key: i64, customer: i64, day: i32| (#[trigger] orders.at(key, customer, day)).0 >= 0 by {
            lemma_sum_nonneg(db.orders, |row: OrdersRow| ind(row.o_orderkey == key && row.o_custkey == customer && row.o_orderdate == day));
        }
        assert forall|customer: i64, key: i64, day: i32| (#[trigger] reverse.at(customer, key, day)).0 >= 0 by {
            lemma_sum_nonneg(db.orders, |row: OrdersRow| ind(row.o_orderkey == key && row.o_custkey == customer));
        }
        assert(reverse.dimension());
    }
    Some((orders, reverse))
}

pub fn profiles(customers: &Table, orders: &Table, suppliers: &Table,
    rows: &[LineitemInput], Ghost(db): Ghost<Db>) -> (out: Option<Table>)
    requires customers_match(*customers, db), orders_match(*orders, db), suppliers_match(*suppliers, db),
        scan_lines_ok(db, rows@),
        forall|position: int| 0 <= position < db.orders.len() ==> #[trigger] order_representable(db.orders[position]),
    ensures out matches Some(table) ==> table.wf() && table.daily() &&
        forall|nation: i64, sub: i64, day: i32| #[trigger] table.at(nation, sub, day) ==
            if sub == 0 { profile(db, nation, day) } else { (0, 0) },
{
    let mut bucket = Bucket::new();
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < rows.len()
        invariant index <= rows.len(), processed@ == index, bucket.wf(),
            customers_match(*customers, db), orders_match(*orders, db), suppliers_match(*suppliers, db),
            forall|nation: i64, sub: i64, day: i32| #[trigger] bucket.at(nation, sub, day) ==
                (sum(rows@.take(processed@), |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).0 } else { 0 }),
                 sum(rows@.take(processed@), |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).1 } else { 0 })),
        decreases rows.len() - index,
    {
        if !crate::maintenance::contribute(customers, orders, suppliers, rows[index], 1, &mut bucket) { return None; }
        proof {
            assert forall|nation: i64, sub: i64, day: i32| #[trigger] bucket.at(nation, sub, day) ==
                (sum(rows@.take(index as int + 1), |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).0 } else { 0 }),
                 sum(rows@.take(index as int + 1), |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).1 } else { 0 })) by {
                lemma_sum_take(rows@, index as int, |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).0 } else { 0 });
                lemma_sum_take(rows@, index as int, |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).1 } else { 0 });
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    let table = match freeze(bucket) { Some(table) => table, None => return None };
    proof {
        assert(rows@.take(rows@.len() as int) =~= rows@);
        assert forall|nation: i64, sub: i64, day: i32| #[trigger] table.at(nation, sub, day) ==
            if sub == 0 { profile(db, nation, day) } else { (0, 0) } by {
            if sub != 0 {
                lemma_sum_zero(rows@, |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).0 } else { 0 });
                lemma_sum_zero(rows@, |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).1 } else { 0 });
            } else {
                let count = |line: LineitemRow| line_weight(db, line, nation, day);
                let revenue = |line: LineitemRow| line_weight(db, line, nation, day) * line.l_extendedprice * (100 - line.l_discount);
                let input_count = |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).0 } else { 0 };
                let input_revenue = |line: LineitemInput| if sub == 0 { crate::maintenance::contribution(*customers, *orders, *suppliers, line, nation, day).1 } else { 0 };
                assert forall|line: LineitemInput| #[trigger] input_count(line) == count(lineitem_input_view(line))
                    && input_revenue(line) == revenue(lineitem_input_view(line)) by {
                    lemma_line_weight(*customers, *orders, *suppliers, db, line, nation, day);
                }
                assert forall|line: LineitemInput| #[trigger] input_revenue(line) == revenue(lineitem_input_view(line)) by {
                    lemma_line_weight(*customers, *orders, *suppliers, db, line, nation, day);
                }
                lemma_sum_map(rows@, |line: LineitemInput| lineitem_input_view(line), count, input_count);
                lemma_sum_map(rows@, |line: LineitemInput| lineitem_input_view(line), revenue, input_revenue);
                assert(sum(lineitem_input_seq_view(rows@), count) == sum(db.lineitem, |line: LineitemRow| if scan_lines_exists_1(db, line) { count(line) } else { 0 }));
                assert(sum(lineitem_input_seq_view(rows@), revenue) == sum(db.lineitem, |line: LineitemRow| if scan_lines_exists_1(db, line) { revenue(line) } else { 0 }));
                assert forall|line: LineitemRow| #[trigger] count(line) == if scan_lines_exists_1(db, line) { count(line) } else { 0 }
                    && revenue(line) == if scan_lines_exists_1(db, line) { revenue(line) } else { 0 } by {
                    if !scan_lines_exists_1(db, line) { lemma_excluded_line(db, line, nation, day); }
                }
                assert forall|line: LineitemRow| #[trigger] revenue(line) == if scan_lines_exists_1(db, line) { revenue(line) } else { 0 } by {
                    if !scan_lines_exists_1(db, line) { lemma_excluded_line(db, line, nation, day); }
                }
                lemma_sum_ext_all(db.lineitem, count, |line: LineitemRow| if scan_lines_exists_1(db, line) { count(line) } else { 0 });
                lemma_sum_ext_all(db.lineitem, revenue, |line: LineitemRow| if scan_lines_exists_1(db, line) { revenue(line) } else { 0 });
            }
        }
        assert forall|nation: i64, sub: i64, day: i32| (#[trigger] table.at(nation, sub, day)).0 >= 0
            && (table.at(nation, sub, day).0 == 0 ==> table.at(nation, sub, day).1 == 0) by {
            lemma_profile_shape(db, nation, day);
        }
        assert(table.daily());
    }
    Some(table)
}

}
