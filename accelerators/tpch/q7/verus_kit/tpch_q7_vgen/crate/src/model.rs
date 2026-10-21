use vstd::prelude::*;
use verus_prooflib::wide::Wide;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::values::*;

verus! {

#[verifier::opaque]
pub open spec fn supplier_count(db: Db, key: i64, nation: i64) -> int {
    sum(db.supplier, |row: SupplierRow| if row.s_suppkey == key && row.s_nationkey == nation { 1 } else { 0 })
}
#[verifier::opaque]
pub open spec fn customer_count(db: Db, key: i64, nation: i64) -> int {
    sum(db.customer, |row: CustomerRow| if row.c_custkey == key && row.c_nationkey == nation { 1 } else { 0 })
}
#[verifier::opaque]
pub open spec fn nation_count(db: Db, key: i64, name: Seq<u8>) -> int {
    sum(db.nation, |row: NationRow| if row.n_nationkey == key && row.n_name == name { 1 } else { 0 })
}
#[verifier::opaque]
pub open spec fn order_count(db: Db, key: i64, customer: i64) -> int {
    sum(db.orders, |row: OrdersRow| if row.o_orderkey == key && row.o_custkey == customer
        && scan_relevant_orders_exists_1(db, row) { 1 } else { 0 })
}
#[verifier::opaque]
pub open spec fn numeric_group(db: Db, first: i64, second: i64, year: i64, revenue: bool) -> int {
    sum(db.lineitem, |line: LineitemRow| fact_line(db, line, first, second, year, revenue))
}
#[verifier::opaque]
pub open spec fn fact_line(db: Db, line: LineitemRow, first: i64, second: i64, year: i64, revenue: bool) -> int {
    if 9131 <= line.l_shipdate <= 9861
        && (if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 }) == year {
        sum(db.supplier, |supplier: SupplierRow| if supplier.s_suppkey == line.l_suppkey && supplier.s_nationkey == first {
            sum(db.orders, |order: OrdersRow| if order.o_orderkey == line.l_orderkey {
                sum(db.customer, |customer: CustomerRow| if customer.c_custkey == order.o_custkey && customer.c_nationkey == second {
                    if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1 }
                } else { 0 })
            } else { 0 })
        } else { 0 })
    } else { 0 }
}

pub open spec fn dimension_value(map: Layers<Counts>, key: i64, other: i64) -> int {
    match map.at(key) { Some(bucket) => count_at(bucket.rows@, other), None => 0 }
}
pub open spec fn patched_count(old: Layers<Counts>, changes: Sparse<Counts>, key: i64, other: i64) -> int {
    match changes.at(key) {
        Some(bucket) => count_at(bucket.rows@, other), None => dimension_value(old, key, other),
    }
}
pub open spec fn counts_nonzero(bucket: Counts) -> bool {
    forall|offset: int| #![trigger bucket.rows@[offset]] 0 <= offset < bucket.rows@.len() ==> bucket.rows@[offset].1@ != 0
}
pub open spec fn counts_unique(bucket: Counts) -> bool {
    count_keys(bucket.rows@).no_duplicates() && counts_nonzero(bucket)
}
pub open spec fn patched_counts_unique(old: Layers<Counts>, changes: Sparse<Counts>, key: i64) -> bool {
    match changes.at(key) {
        Some(bucket) => counts_unique(bucket),
        None => match old.at(key) {
            Some(bucket) => counts_unique(bucket), None => true,
        },
    }
}
pub open spec fn patched_counts_positive(old: Layers<Counts>, changes: Sparse<Counts>, key: i64) -> bool {
    match changes.at(key) {
        Some(bucket) => counts_positive(bucket),
        None => match old.at(key) { Some(bucket) => counts_positive(bucket), None => true },
    }
}
pub open spec fn names_nonzero(bucket: Names) -> bool {
    forall|offset: int| #![trigger bucket.rows@[offset]] 0 <= offset < bucket.rows@.len() ==> bucket.rows@[offset].1@ != 0
}
pub open spec fn names_unique(bucket: Names) -> bool {
    name_keys(bucket.rows@).no_duplicates() && names_nonzero(bucket)
}
pub open spec fn patched_names_unique(old: Layers<Names>, changes: Sparse<Names>, key: i64) -> bool {
    match changes.at(key) {
        Some(bucket) => names_unique(bucket),
        None => match old.at(key) {
            Some(bucket) => names_unique(bucket), None => true,
        },
    }
}
pub open spec fn patched_names_positive(old: Layers<Names>, changes: Sparse<Names>, key: i64) -> bool {
    match changes.at(key) {
        Some(bucket) => names_positive(bucket),
        None => match old.at(key) { Some(bucket) => names_positive(bucket), None => true },
    }
}
pub open spec fn nation_value(map: Layers<Names>, key: i64, name: Seq<u8>) -> int {
    match map.at(key) { Some(bucket) => name_at(bucket.rows@, name), None => 0 }
}
pub open spec fn patched_name(old: Layers<Names>, changes: Sparse<Names>, key: i64, name: Seq<u8>) -> int {
    match changes.at(key) {
        Some(bucket) => name_at(bucket.rows@, name), None => nation_value(old, key, name),
    }
}
pub open spec fn group_value(map: Layers<Groups>, first: i64, second: i64, year: i64, revenue: bool) -> int {
    match map.at(first) { Some(bucket) => group_at(bucket.rows@, second, year, revenue), None => 0 }
}
pub open spec fn sparse_group(map: Sparse<Groups>, first: i64, second: i64, year: i64, revenue: bool) -> int {
    match map.at(first) { Some(bucket) => group_at(bucket.rows@, second, year, revenue), None => 0 }
}
pub open spec fn patched_group(previous: Layers<Groups>, changes: Sparse<Groups>, first: i64, second: i64, year: i64, revenue: bool) -> int {
    match changes.at(first) {
        Some(bucket) => group_at(bucket.rows@, second, year, revenue),
        None => group_value(previous, first, second, year, revenue),
    }
}
#[verifier::opaque]
pub open spec fn projected_contribution(suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    order: i64, supplier: i64, line_year: i64, volume: int,
    first: i64, second: i64, year: i64, revenue: bool) -> int
{
    if line_year != year { 0 } else {
        dimension_value(suppliers, supplier, first)
        * (match orders.at(order) {
            Some(bucket) => sum(bucket.rows@, |row: (i64, Wide)|
                row.1@ * dimension_value(customers, row.0, second)),
            None => 0,
        }) * if revenue { volume } else { 1 }
    }
}
pub proof fn lemma_projected_absent(suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    order: i64, supplier: i64, line_year: i64, volume: int, first: i64, second: i64, year: i64, revenue: bool)
    requires suppliers.at(supplier).is_none() || orders.at(order).is_none(),
    ensures projected_contribution(suppliers, customers, orders, order, supplier, line_year, volume,
        first, second, year, revenue) == 0,
{
    reveal(projected_contribution);
    let supplier_count = dimension_value(suppliers, supplier, first);
    let order_count = match orders.at(order) {
        Some(bucket) => sum(bucket.rows@, |row: (i64, Wide)| row.1@ * dimension_value(customers, row.0, second)),
        None => 0int,
    };
    let measure = if revenue { volume } else { 1int };
    if suppliers.at(supplier).is_none() { assert(supplier_count == 0); }
    else { assert(order_count == 0); }
    assert(supplier_count * order_count * measure == 0) by (nonlinear_arith)
        requires supplier_count == 0 || order_count == 0;
    assert(projected_contribution(suppliers, customers, orders, order, supplier, line_year, volume,
        first, second, year, revenue) == if line_year != year { 0int } else { supplier_count * order_count * measure });
}
pub open spec fn order_weight(rows: Seq<(i64, Wide)>, customers: Layers<Counts>, second: i64) -> int {
    sum(rows, |row: (i64, Wide)| row.1@ * dimension_value(customers, row.0, second))
}
pub open spec fn prefix_product(supplier_rows: Seq<(i64, Wide)>, order_rows: Seq<(i64, Wide)>, customers: Layers<Counts>,
    first: i64, second: i64, year: i64, line_year: i64, volume: int, sign: int, revenue: bool) -> int
{
    if year != line_year { 0 } else {
        count_at(supplier_rows, first) * (order_weight(order_rows, customers, second) * (sign * if revenue { volume } else { 1 }))
    }
}
pub open spec fn customer_piece(rows: Seq<(i64, Wide)>, supplier: (i64, Wide), order: (i64, Wide),
    first: i64, second: i64, year: i64, line_year: i64, volume: int, sign: int, revenue: bool) -> int
{
    if year != line_year || supplier.0 != first { 0 } else {
        count_at(rows, second) * (supplier.1@ * (order.1@ * (sign * if revenue { volume } else { 1 })))
    }
}
#[verifier::opaque]
pub open spec fn line_weight(suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    line: LineInput, first: i64, second: i64, year: i64, revenue: bool) -> int
{
    projected_contribution(suppliers, customers, orders, line.l_orderkey, line.l_suppkey,
        if line.l_shipdate <= 9495 { 1995i64 } else { 1996i64 },
        line.l_extendedprice as int * (100 - line.l_discount as int), first, second, year, revenue)
}
#[verifier::opaque]
pub open spec fn captured_weight(suppliers: Layers<Counts>, customers: Layers<Counts>, orders: Layers<Counts>,
    line: LineDelta, first: i64, second: i64, year: i64, revenue: bool) -> int
{
    if line.ship_1995 == 0 && line.ship_1996 == 0 { 0 } else {
        line.sign as int * projected_contribution(suppliers, customers, orders, line.l_orderkey, line.l_suppkey,
            if line.ship_1995 != 0 { 1995i64 } else { 1996i64 },
            line.l_extendedprice as int * (100 - line.l_discount as int), first, second, year, revenue)
    }
}
pub open spec fn answer_value(map: Layers<Answers>, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int {
    match map.at(pair_hash_spec(first, second) as i64) {
        Some(bucket) => answer_at(bucket.rows@, first, second, year, revenue), None => 0,
    }
}
pub open spec fn patched_answer(old: Layers<Answers>, changes: Sparse<Answers>, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int {
    match changes.at(pair_hash_spec(first, second) as i64) {
        Some(bucket) => answer_at(bucket.rows@, first, second, year, revenue),
        None => answer_value(old, first, second, year, revenue),
    }
}
pub open spec fn patched_member(old: Layers<Layers<i128>>, changes: Sparse<Layers<i128>>, key: i64, other: i64) -> bool {
    match changes.at(key) {
        Some(bucket) => match bucket.at(other) { Some(value) => value > 0, None => false },
        None => member_value(old, key, other),
    }
}
pub open spec fn expanded_prefix(first_rows: Seq<(Vec<u8>, Wide)>, second_rows: Seq<(Vec<u8>, Wide)>,
    cell: Cell, sign: int, first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int
{
    if cell.year != year { 0int } else {
        name_at(first_rows, first) * (name_at(second_rows, second)
            * (sign * if revenue { cell.revenue@ } else { cell.count@ }))
    }
}
pub open spec fn cell_expansion(nations: Layers<Names>, key: i64, cell: Cell, sign: int,
    first: Seq<u8>, second: Seq<u8>, year: i64, revenue: bool) -> int
{
    if cell.year != year { 0int } else {
        nation_value(nations, key, first) * (nation_value(nations, cell.other, second)
            * (sign * if revenue { cell.revenue@ } else { cell.count@ }))
    }
}
pub open spec fn member_value(map: Layers<Layers<i128>>, key: i64, other: i64) -> bool {
    match map.at(key) {
        Some(bucket) => match bucket.at(other) { Some(value) => value > 0, None => false },
        None => false,
    }
}

pub open spec fn counts_positive(bucket: Counts) -> bool {
    forall|offset: int| #![trigger bucket.rows@[offset]]
        0 <= offset < bucket.rows@.len() ==> bucket.rows@[offset].1@ > 0
}
pub open spec fn counts_valid(bucket: Counts) -> bool {
    &&& counts_positive(bucket)
    &&& count_keys(bucket.rows@).no_duplicates()
}
pub open spec fn groups_positive(bucket: Groups) -> bool {
    forall|offset: int| #![trigger bucket.rows@[offset]]
        0 <= offset < bucket.rows@.len() ==> bucket.rows@[offset].count@ > 0
}
pub open spec fn groups_valid(bucket: Groups) -> bool {
    &&& groups_positive(bucket)
    &&& groups_shape(bucket)
}
pub open spec fn groups_shape(bucket: Groups) -> bool {
    &&& forall|offset: int| #![trigger bucket.rows@[offset]]
        0 <= offset < bucket.rows@.len() ==> bucket.rows@[offset].year == 1995 || bucket.rows@[offset].year == 1996
    &&& group_keys(bucket.rows@).no_duplicates()
    &&& forall|offset: int| #![trigger bucket.rows@[offset]]
        0 <= offset < bucket.rows@.len() ==> !(bucket.rows@[offset].count@ == 0 && bucket.rows@[offset].revenue@ == 0)
}
pub open spec fn sparse_groups_shape(map: Sparse<Groups>) -> bool {
    forall|key: i64| #![trigger map.at(key)] map.at(key) matches Some(bucket) ==> groups_shape(bucket)
}
pub open spec fn names_positive(bucket: Names) -> bool {
    forall|offset: int| #![trigger bucket.rows@[offset]]
        0 <= offset < bucket.rows@.len() ==> bucket.rows@[offset].1@ > 0
}
pub open spec fn names_valid(bucket: Names) -> bool {
    &&& names_positive(bucket)
    &&& name_keys(bucket.rows@).no_duplicates()
}
pub open spec fn answers_positive(bucket: Answers) -> bool {
    forall|offset: int| #![trigger bucket.rows@[offset]]
        0 <= offset < bucket.rows@.len() ==> bucket.rows@[offset].count@ > 0
}
pub open spec fn answers_valid(bucket: Answers, key: i64) -> bool {
    &&& answers_positive(bucket)
    &&& answers_shape(bucket, key)
}
pub open spec fn answers_shape(bucket: Answers, key: i64) -> bool {
    &&& forall|offset: int| #![trigger bucket.rows@[offset]]
        0 <= offset < bucket.rows@.len() ==> (bucket.rows@[offset].year == 1995 || bucket.rows@[offset].year == 1996)
            && pair_hash_spec(bucket.rows@[offset].first@, bucket.rows@[offset].second@) == key
    &&& answer_keys(bucket.rows@).no_duplicates()
    &&& forall|offset: int| #![trigger bucket.rows@[offset]]
        0 <= offset < bucket.rows@.len() ==> !(bucket.rows@[offset].count@ == 0 && bucket.rows@[offset].revenue@ == 0)
}
pub open spec fn patched_answers_shape(previous: Layers<Answers>, changes: Sparse<Answers>) -> bool {
    forall|key: i64| #![trigger changes.at(key), previous.at(key)]
        match changes.at(key) {
            Some(bucket) => answers_shape(bucket, key),
            None => previous.at(key) matches Some(bucket) ==> answers_shape(bucket, key),
        }
}

}
