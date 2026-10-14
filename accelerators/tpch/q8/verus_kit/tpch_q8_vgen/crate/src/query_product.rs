use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::sql_bridge::*;

verus! {

pub struct CoreTuple {
    pub part: PartRow,
    pub supplier: SupplierRow,
    pub lineitem: LineitemRow,
    pub orders: OrdersRow,
    pub customer: CustomerRow,
}

pub open spec fn core_in_db(db: Db, row: CoreTuple) -> bool {
    db.part.contains(row.part) && db.supplier.contains(row.supplier) && db.lineitem.contains(row.lineitem) && db.orders.contains(row.orders) && db.customer.contains(row.customer)
}

#[verifier::opaque]
pub open spec fn core_rows_5(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow) -> Seq<CoreTuple> {
    seq![CoreTuple { part, supplier, lineitem, orders, customer }]
}

#[verifier::opaque]
pub open spec fn core_sum_5(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, weight: spec_fn(CoreTuple) -> int) -> int {
    weight(CoreTuple { part, supplier, lineitem, orders, customer })
}

pub proof fn lemma_core_sum_5(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, weight: spec_fn(CoreTuple) -> int)
    ensures sum(core_rows_5(db, part, supplier, lineitem, orders, customer), weight) == core_sum_5(db, part, supplier, lineitem, orders, customer, weight),
{
    hide(sum);
    reveal(core_rows_5);
    reveal(core_sum_5);
    lemma_sum_singleton(CoreTuple { part, supplier, lineitem, orders, customer }, weight);
}

pub proof fn lemma_core_member_5(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, row: CoreTuple)
    requires db.part.contains(part), db.supplier.contains(supplier), db.lineitem.contains(lineitem), db.orders.contains(orders), db.customer.contains(customer), core_rows_5(db, part, supplier, lineitem, orders, customer).contains(row),
    ensures core_in_db(db, row),
{
    reveal(core_rows_5);
    assert(row == CoreTuple { part, supplier, lineitem, orders, customer });
}

#[verifier::opaque]
pub open spec fn core_rows_4(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow) -> Seq<CoreTuple> {
    flat(db.customer.map_values(|customer: CustomerRow| core_rows_5(db, part, supplier, lineitem, orders, customer)))
}

#[verifier::opaque]
pub open spec fn core_sum_4(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, weight: spec_fn(CoreTuple) -> int) -> int {
    sum(db.customer, |customer: CustomerRow| core_sum_5(db, part, supplier, lineitem, orders, customer, weight))
}

pub proof fn lemma_core_sum_4(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, weight: spec_fn(CoreTuple) -> int)
    ensures sum(core_rows_4(db, part, supplier, lineitem, orders), weight) == core_sum_4(db, part, supplier, lineitem, orders, weight),
{
    hide(sum);
    reveal(core_rows_4);
    reveal(core_sum_4);
    let expand = |customer: CustomerRow| core_rows_5(db, part, supplier, lineitem, orders, customer);
    let raw = |customer: CustomerRow| sum(expand(customer), weight);
    let nested = |customer: CustomerRow| core_sum_5(db, part, supplier, lineitem, orders, customer, weight);
    assert forall|customer: CustomerRow| #[trigger] raw(customer) == nested(customer) by {
        lemma_core_sum_5(db, part, supplier, lineitem, orders, customer, weight);
    }
    lemma_sum_flat_map(db.customer, expand, weight);
    lemma_sum_ext_all(db.customer, raw, nested);
}

pub proof fn lemma_core_member_4(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, row: CoreTuple)
    requires db.part.contains(part), db.supplier.contains(supplier), db.lineitem.contains(lineitem), db.orders.contains(orders), core_rows_4(db, part, supplier, lineitem, orders).contains(row),
    ensures core_in_db(db, row),
{
    reveal(core_rows_4);
    let expand = |customer: CustomerRow| core_rows_5(db, part, supplier, lineitem, orders, customer);
    lemma_flat_map_contains(db.customer, expand, row);
    let index = choose|index: int| 0 <= index < db.customer.len() && #[trigger] expand(db.customer[index]).contains(row);
    let customer = db.customer[index];
    assert(db.customer.contains(customer));
    lemma_core_member_5(db, part, supplier, lineitem, orders, customer, row);
}

#[verifier::opaque]
pub open spec fn core_rows_3(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow) -> Seq<CoreTuple> {
    flat(db.orders.map_values(|orders: OrdersRow| core_rows_4(db, part, supplier, lineitem, orders)))
}

#[verifier::opaque]
pub open spec fn core_sum_3(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, weight: spec_fn(CoreTuple) -> int) -> int {
    sum(db.orders, |orders: OrdersRow| core_sum_4(db, part, supplier, lineitem, orders, weight))
}

pub proof fn lemma_core_sum_3(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, weight: spec_fn(CoreTuple) -> int)
    ensures sum(core_rows_3(db, part, supplier, lineitem), weight) == core_sum_3(db, part, supplier, lineitem, weight),
{
    hide(sum);
    reveal(core_rows_3);
    reveal(core_sum_3);
    let expand = |orders: OrdersRow| core_rows_4(db, part, supplier, lineitem, orders);
    let raw = |orders: OrdersRow| sum(expand(orders), weight);
    let nested = |orders: OrdersRow| core_sum_4(db, part, supplier, lineitem, orders, weight);
    assert forall|orders: OrdersRow| #[trigger] raw(orders) == nested(orders) by {
        lemma_core_sum_4(db, part, supplier, lineitem, orders, weight);
    }
    lemma_sum_flat_map(db.orders, expand, weight);
    lemma_sum_ext_all(db.orders, raw, nested);
}

pub proof fn lemma_core_member_3(db: Db, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, row: CoreTuple)
    requires db.part.contains(part), db.supplier.contains(supplier), db.lineitem.contains(lineitem), core_rows_3(db, part, supplier, lineitem).contains(row),
    ensures core_in_db(db, row),
{
    reveal(core_rows_3);
    let expand = |orders: OrdersRow| core_rows_4(db, part, supplier, lineitem, orders);
    lemma_flat_map_contains(db.orders, expand, row);
    let index = choose|index: int| 0 <= index < db.orders.len() && #[trigger] expand(db.orders[index]).contains(row);
    let orders = db.orders[index];
    assert(db.orders.contains(orders));
    lemma_core_member_4(db, part, supplier, lineitem, orders, row);
}

#[verifier::opaque]
pub open spec fn core_rows_2(db: Db, part: PartRow, supplier: SupplierRow) -> Seq<CoreTuple> {
    flat(db.lineitem.map_values(|lineitem: LineitemRow| core_rows_3(db, part, supplier, lineitem)))
}

#[verifier::opaque]
pub open spec fn core_sum_2(db: Db, part: PartRow, supplier: SupplierRow, weight: spec_fn(CoreTuple) -> int) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| core_sum_3(db, part, supplier, lineitem, weight))
}

pub proof fn lemma_core_sum_2(db: Db, part: PartRow, supplier: SupplierRow, weight: spec_fn(CoreTuple) -> int)
    ensures sum(core_rows_2(db, part, supplier), weight) == core_sum_2(db, part, supplier, weight),
{
    hide(sum);
    reveal(core_rows_2);
    reveal(core_sum_2);
    let expand = |lineitem: LineitemRow| core_rows_3(db, part, supplier, lineitem);
    let raw = |lineitem: LineitemRow| sum(expand(lineitem), weight);
    let nested = |lineitem: LineitemRow| core_sum_3(db, part, supplier, lineitem, weight);
    assert forall|lineitem: LineitemRow| #[trigger] raw(lineitem) == nested(lineitem) by {
        lemma_core_sum_3(db, part, supplier, lineitem, weight);
    }
    lemma_sum_flat_map(db.lineitem, expand, weight);
    lemma_sum_ext_all(db.lineitem, raw, nested);
}

pub proof fn lemma_core_member_2(db: Db, part: PartRow, supplier: SupplierRow, row: CoreTuple)
    requires db.part.contains(part), db.supplier.contains(supplier), core_rows_2(db, part, supplier).contains(row),
    ensures core_in_db(db, row),
{
    reveal(core_rows_2);
    let expand = |lineitem: LineitemRow| core_rows_3(db, part, supplier, lineitem);
    lemma_flat_map_contains(db.lineitem, expand, row);
    let index = choose|index: int| 0 <= index < db.lineitem.len() && #[trigger] expand(db.lineitem[index]).contains(row);
    let lineitem = db.lineitem[index];
    assert(db.lineitem.contains(lineitem));
    lemma_core_member_3(db, part, supplier, lineitem, row);
}

#[verifier::opaque]
pub open spec fn core_rows_1(db: Db, part: PartRow) -> Seq<CoreTuple> {
    flat(db.supplier.map_values(|supplier: SupplierRow| core_rows_2(db, part, supplier)))
}

#[verifier::opaque]
pub open spec fn core_sum_1(db: Db, part: PartRow, weight: spec_fn(CoreTuple) -> int) -> int {
    sum(db.supplier, |supplier: SupplierRow| core_sum_2(db, part, supplier, weight))
}

pub proof fn lemma_core_sum_1(db: Db, part: PartRow, weight: spec_fn(CoreTuple) -> int)
    ensures sum(core_rows_1(db, part), weight) == core_sum_1(db, part, weight),
{
    hide(sum);
    reveal(core_rows_1);
    reveal(core_sum_1);
    let expand = |supplier: SupplierRow| core_rows_2(db, part, supplier);
    let raw = |supplier: SupplierRow| sum(expand(supplier), weight);
    let nested = |supplier: SupplierRow| core_sum_2(db, part, supplier, weight);
    assert forall|supplier: SupplierRow| #[trigger] raw(supplier) == nested(supplier) by {
        lemma_core_sum_2(db, part, supplier, weight);
    }
    lemma_sum_flat_map(db.supplier, expand, weight);
    lemma_sum_ext_all(db.supplier, raw, nested);
}

pub proof fn lemma_core_member_1(db: Db, part: PartRow, row: CoreTuple)
    requires db.part.contains(part), core_rows_1(db, part).contains(row),
    ensures core_in_db(db, row),
{
    reveal(core_rows_1);
    let expand = |supplier: SupplierRow| core_rows_2(db, part, supplier);
    lemma_flat_map_contains(db.supplier, expand, row);
    let index = choose|index: int| 0 <= index < db.supplier.len() && #[trigger] expand(db.supplier[index]).contains(row);
    let supplier = db.supplier[index];
    assert(db.supplier.contains(supplier));
    lemma_core_member_2(db, part, supplier, row);
}

#[verifier::opaque]
pub open spec fn core_rows_0(db: Db) -> Seq<CoreTuple> {
    flat(db.part.map_values(|part: PartRow| core_rows_1(db, part)))
}

#[verifier::opaque]
pub open spec fn core_sum_0(db: Db, weight: spec_fn(CoreTuple) -> int) -> int {
    sum(db.part, |part: PartRow| core_sum_1(db, part, weight))
}

pub proof fn lemma_core_sum_0(db: Db, weight: spec_fn(CoreTuple) -> int)
    ensures sum(core_rows_0(db), weight) == core_sum_0(db, weight),
{
    hide(sum);
    reveal(core_rows_0);
    reveal(core_sum_0);
    let expand = |part: PartRow| core_rows_1(db, part);
    let raw = |part: PartRow| sum(expand(part), weight);
    let nested = |part: PartRow| core_sum_1(db, part, weight);
    assert forall|part: PartRow| #[trigger] raw(part) == nested(part) by {
        lemma_core_sum_1(db, part, weight);
    }
    lemma_sum_flat_map(db.part, expand, weight);
    lemma_sum_ext_all(db.part, raw, nested);
}

pub proof fn lemma_core_member_0(db: Db, row: CoreTuple)
    requires core_rows_0(db).contains(row),
    ensures core_in_db(db, row),
{
    reveal(core_rows_0);
    let expand = |part: PartRow| core_rows_1(db, part);
    lemma_flat_map_contains(db.part, expand, row);
    let index = choose|index: int| 0 <= index < db.part.len() && #[trigger] expand(db.part[index]).contains(row);
    let part = db.part[index];
    assert(db.part.contains(part));
    lemma_core_member_1(db, part, row);
}

pub struct RouteTuple {
    pub n1: NationRow,
    pub n2: NationRow,
    pub region: RegionRow,
}

pub open spec fn route_in_db(db: Db, row: RouteTuple) -> bool {
    db.nation.contains(row.n1) && db.nation.contains(row.n2) && db.region.contains(row.region)
}

#[verifier::opaque]
pub open spec fn route_rows_3(db: Db, n1: NationRow, n2: NationRow, region: RegionRow) -> Seq<RouteTuple> {
    seq![RouteTuple { n1, n2, region }]
}

#[verifier::opaque]
pub open spec fn route_sum_3(db: Db, n1: NationRow, n2: NationRow, region: RegionRow, weight: spec_fn(RouteTuple) -> int) -> int {
    weight(RouteTuple { n1, n2, region })
}

pub proof fn lemma_route_sum_3(db: Db, n1: NationRow, n2: NationRow, region: RegionRow, weight: spec_fn(RouteTuple) -> int)
    ensures sum(route_rows_3(db, n1, n2, region), weight) == route_sum_3(db, n1, n2, region, weight),
{
    hide(sum);
    reveal(route_rows_3);
    reveal(route_sum_3);
    lemma_sum_singleton(RouteTuple { n1, n2, region }, weight);
}

pub proof fn lemma_route_member_3(db: Db, n1: NationRow, n2: NationRow, region: RegionRow, row: RouteTuple)
    requires db.nation.contains(n1), db.nation.contains(n2), db.region.contains(region), route_rows_3(db, n1, n2, region).contains(row),
    ensures route_in_db(db, row),
{
    reveal(route_rows_3);
    assert(row == RouteTuple { n1, n2, region });
}

#[verifier::opaque]
pub open spec fn route_rows_2(db: Db, n1: NationRow, n2: NationRow) -> Seq<RouteTuple> {
    flat(db.region.map_values(|region: RegionRow| route_rows_3(db, n1, n2, region)))
}

#[verifier::opaque]
pub open spec fn route_sum_2(db: Db, n1: NationRow, n2: NationRow, weight: spec_fn(RouteTuple) -> int) -> int {
    sum(db.region, |region: RegionRow| route_sum_3(db, n1, n2, region, weight))
}

pub proof fn lemma_route_sum_2(db: Db, n1: NationRow, n2: NationRow, weight: spec_fn(RouteTuple) -> int)
    ensures sum(route_rows_2(db, n1, n2), weight) == route_sum_2(db, n1, n2, weight),
{
    hide(sum);
    reveal(route_rows_2);
    reveal(route_sum_2);
    let expand = |region: RegionRow| route_rows_3(db, n1, n2, region);
    let raw = |region: RegionRow| sum(expand(region), weight);
    let nested = |region: RegionRow| route_sum_3(db, n1, n2, region, weight);
    assert forall|region: RegionRow| #[trigger] raw(region) == nested(region) by {
        lemma_route_sum_3(db, n1, n2, region, weight);
    }
    lemma_sum_flat_map(db.region, expand, weight);
    lemma_sum_ext_all(db.region, raw, nested);
}

pub proof fn lemma_route_member_2(db: Db, n1: NationRow, n2: NationRow, row: RouteTuple)
    requires db.nation.contains(n1), db.nation.contains(n2), route_rows_2(db, n1, n2).contains(row),
    ensures route_in_db(db, row),
{
    reveal(route_rows_2);
    let expand = |region: RegionRow| route_rows_3(db, n1, n2, region);
    lemma_flat_map_contains(db.region, expand, row);
    let index = choose|index: int| 0 <= index < db.region.len() && #[trigger] expand(db.region[index]).contains(row);
    let region = db.region[index];
    assert(db.region.contains(region));
    lemma_route_member_3(db, n1, n2, region, row);
}

#[verifier::opaque]
pub open spec fn route_rows_1(db: Db, n1: NationRow) -> Seq<RouteTuple> {
    flat(db.nation.map_values(|n2: NationRow| route_rows_2(db, n1, n2)))
}

#[verifier::opaque]
pub open spec fn route_sum_1(db: Db, n1: NationRow, weight: spec_fn(RouteTuple) -> int) -> int {
    sum(db.nation, |n2: NationRow| route_sum_2(db, n1, n2, weight))
}

pub proof fn lemma_route_sum_1(db: Db, n1: NationRow, weight: spec_fn(RouteTuple) -> int)
    ensures sum(route_rows_1(db, n1), weight) == route_sum_1(db, n1, weight),
{
    hide(sum);
    reveal(route_rows_1);
    reveal(route_sum_1);
    let expand = |n2: NationRow| route_rows_2(db, n1, n2);
    let raw = |n2: NationRow| sum(expand(n2), weight);
    let nested = |n2: NationRow| route_sum_2(db, n1, n2, weight);
    assert forall|n2: NationRow| #[trigger] raw(n2) == nested(n2) by {
        lemma_route_sum_2(db, n1, n2, weight);
    }
    lemma_sum_flat_map(db.nation, expand, weight);
    lemma_sum_ext_all(db.nation, raw, nested);
}

pub proof fn lemma_route_member_1(db: Db, n1: NationRow, row: RouteTuple)
    requires db.nation.contains(n1), route_rows_1(db, n1).contains(row),
    ensures route_in_db(db, row),
{
    reveal(route_rows_1);
    let expand = |n2: NationRow| route_rows_2(db, n1, n2);
    lemma_flat_map_contains(db.nation, expand, row);
    let index = choose|index: int| 0 <= index < db.nation.len() && #[trigger] expand(db.nation[index]).contains(row);
    let n2 = db.nation[index];
    assert(db.nation.contains(n2));
    lemma_route_member_2(db, n1, n2, row);
}

#[verifier::opaque]
pub open spec fn route_rows_0(db: Db) -> Seq<RouteTuple> {
    flat(db.nation.map_values(|n1: NationRow| route_rows_1(db, n1)))
}

#[verifier::opaque]
pub open spec fn route_sum_0(db: Db, weight: spec_fn(RouteTuple) -> int) -> int {
    sum(db.nation, |n1: NationRow| route_sum_1(db, n1, weight))
}

pub proof fn lemma_route_sum_0(db: Db, weight: spec_fn(RouteTuple) -> int)
    ensures sum(route_rows_0(db), weight) == route_sum_0(db, weight),
{
    hide(sum);
    reveal(route_rows_0);
    reveal(route_sum_0);
    let expand = |n1: NationRow| route_rows_1(db, n1);
    let raw = |n1: NationRow| sum(expand(n1), weight);
    let nested = |n1: NationRow| route_sum_1(db, n1, weight);
    assert forall|n1: NationRow| #[trigger] raw(n1) == nested(n1) by {
        lemma_route_sum_1(db, n1, weight);
    }
    lemma_sum_flat_map(db.nation, expand, weight);
    lemma_sum_ext_all(db.nation, raw, nested);
}

pub proof fn lemma_route_member_0(db: Db, row: RouteTuple)
    requires route_rows_0(db).contains(row),
    ensures route_in_db(db, row),
{
    reveal(route_rows_0);
    let expand = |n1: NationRow| route_rows_1(db, n1);
    lemma_flat_map_contains(db.nation, expand, row);
    let index = choose|index: int| 0 <= index < db.nation.len() && #[trigger] expand(db.nation[index]).contains(row);
    let n1 = db.nation[index];
    assert(db.nation.contains(n1));
    lemma_route_member_1(db, n1, row);
}

pub open spec fn completed(core: CoreTuple, route: RouteTuple) -> AllNationsTuple {
    AllNationsTuple { part: core.part, supplier: core.supplier, lineitem: core.lineitem,
        orders: core.orders, customer: core.customer, n1: route.n1, n2: route.n2, region: route.region }
}

pub open spec fn joined_weight(db: Db, core: CoreTuple, route: RouteTuple, parameters: Params, year: int, weight: spec_fn(Tuple) -> int) -> int {
    let tuple = completed(core, route);
    if all_nations_where_clause(db, parameters, tuple.part, tuple.supplier, tuple.lineitem,
        tuple.orders, tuple.customer, tuple.n1, tuple.n2, tuple.region) {
        grouped_weight(tuple, year, weight)
    } else { 0 }
}

pub proof fn lemma_sql_route_3(db: Db, parameters: Params, core: CoreTuple, n1: NationRow, n2: NationRow, region: RegionRow, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_8(db, parameters, core.part, core.supplier, core.lineitem, core.orders, core.customer, n1, n2, region, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == route_sum_3(db, n1, n2, region, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)),
{
    hide(sum);
    reveal(sql_sum_8);
    reveal(route_sum_3);
}

pub proof fn lemma_sql_route_2(db: Db, parameters: Params, core: CoreTuple, n1: NationRow, n2: NationRow, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_7(db, parameters, core.part, core.supplier, core.lineitem, core.orders, core.customer, n1, n2, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == route_sum_2(db, n1, n2, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)),
{
    hide(sum);
    reveal(sql_sum_7);
    reveal(route_sum_2);
    let left = |region: RegionRow| sql_sum_8(db, parameters, core.part, core.supplier, core.lineitem, core.orders, core.customer, n1, n2, region, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight));
    let right = |region: RegionRow| route_sum_3(db, n1, n2, region, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight));
    assert forall|region: RegionRow| #[trigger] left(region) == right(region) by {
        lemma_sql_route_3(db, parameters, core, n1, n2, region, year, weight);
    }
    lemma_sum_ext_all(db.region, left, right);
}

pub proof fn lemma_sql_route_1(db: Db, parameters: Params, core: CoreTuple, n1: NationRow, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_6(db, parameters, core.part, core.supplier, core.lineitem, core.orders, core.customer, n1, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == route_sum_1(db, n1, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)),
{
    hide(sum);
    reveal(sql_sum_6);
    reveal(route_sum_1);
    let left = |n2: NationRow| sql_sum_7(db, parameters, core.part, core.supplier, core.lineitem, core.orders, core.customer, n1, n2, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight));
    let right = |n2: NationRow| route_sum_2(db, n1, n2, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight));
    assert forall|n2: NationRow| #[trigger] left(n2) == right(n2) by {
        lemma_sql_route_2(db, parameters, core, n1, n2, year, weight);
    }
    lemma_sum_ext_all(db.nation, left, right);
}

pub proof fn lemma_sql_route_0(db: Db, parameters: Params, core: CoreTuple, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_5(db, parameters, core.part, core.supplier, core.lineitem, core.orders, core.customer, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)),
{
    hide(sum);
    reveal(sql_sum_5);
    reveal(route_sum_0);
    let left = |n1: NationRow| sql_sum_6(db, parameters, core.part, core.supplier, core.lineitem, core.orders, core.customer, n1, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight));
    let right = |n1: NationRow| route_sum_1(db, n1, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight));
    assert forall|n1: NationRow| #[trigger] left(n1) == right(n1) by {
        lemma_sql_route_1(db, parameters, core, n1, year, weight);
    }
    lemma_sum_ext_all(db.nation, left, right);
}

pub proof fn lemma_sql_core_5(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, customer: CustomerRow, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == core_sum_5(db, part, supplier, lineitem, orders, customer, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight))),
{
    hide(sum);
    reveal(core_sum_5);
    lemma_sql_route_0(db, parameters, CoreTuple { part, supplier, lineitem, orders, customer }, year, weight);
}

pub proof fn lemma_sql_core_4(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, orders: OrdersRow, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_4(db, parameters, part, supplier, lineitem, orders, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == core_sum_4(db, part, supplier, lineitem, orders, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight))),
{
    hide(sum);
    reveal(core_sum_4);
    reveal(sql_sum_4);
    let left = |customer: CustomerRow| sql_sum_5(db, parameters, part, supplier, lineitem, orders, customer, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight));
    let right = |customer: CustomerRow| core_sum_5(db, part, supplier, lineitem, orders, customer, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)));
    assert forall|customer: CustomerRow| #[trigger] left(customer) == right(customer) by {
        lemma_sql_core_5(db, parameters, part, supplier, lineitem, orders, customer, year, weight);
    }
    lemma_sum_ext_all(db.customer, left, right);
}

pub proof fn lemma_sql_core_3(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, lineitem: LineitemRow, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_3(db, parameters, part, supplier, lineitem, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == core_sum_3(db, part, supplier, lineitem, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight))),
{
    hide(sum);
    reveal(core_sum_3);
    reveal(sql_sum_3);
    let left = |orders: OrdersRow| sql_sum_4(db, parameters, part, supplier, lineitem, orders, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight));
    let right = |orders: OrdersRow| core_sum_4(db, part, supplier, lineitem, orders, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)));
    assert forall|orders: OrdersRow| #[trigger] left(orders) == right(orders) by {
        lemma_sql_core_4(db, parameters, part, supplier, lineitem, orders, year, weight);
    }
    lemma_sum_ext_all(db.orders, left, right);
}

pub proof fn lemma_sql_core_2(db: Db, parameters: Params, part: PartRow, supplier: SupplierRow, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_2(db, parameters, part, supplier, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == core_sum_2(db, part, supplier, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight))),
{
    hide(sum);
    reveal(core_sum_2);
    reveal(sql_sum_2);
    let left = |lineitem: LineitemRow| sql_sum_3(db, parameters, part, supplier, lineitem, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight));
    let right = |lineitem: LineitemRow| core_sum_3(db, part, supplier, lineitem, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)));
    assert forall|lineitem: LineitemRow| #[trigger] left(lineitem) == right(lineitem) by {
        lemma_sql_core_3(db, parameters, part, supplier, lineitem, year, weight);
    }
    lemma_sum_ext_all(db.lineitem, left, right);
}

pub proof fn lemma_sql_core_1(db: Db, parameters: Params, part: PartRow, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_1(db, parameters, part, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == core_sum_1(db, part, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight))),
{
    hide(sum);
    reveal(core_sum_1);
    reveal(sql_sum_1);
    let left = |supplier: SupplierRow| sql_sum_2(db, parameters, part, supplier, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight));
    let right = |supplier: SupplierRow| core_sum_2(db, part, supplier, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)));
    assert forall|supplier: SupplierRow| #[trigger] left(supplier) == right(supplier) by {
        lemma_sql_core_2(db, parameters, part, supplier, year, weight);
    }
    lemma_sum_ext_all(db.supplier, left, right);
}

pub proof fn lemma_sql_core_0(db: Db, parameters: Params, year: int, weight: spec_fn(Tuple) -> int)
    ensures sql_sum_0(db, parameters, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight)) == core_sum_0(db, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight))),
{
    hide(sum);
    reveal(core_sum_0);
    reveal(sql_sum_0);
    let left = |part: PartRow| sql_sum_1(db, parameters, part, |tuple: AllNationsTuple| grouped_weight(tuple, year, weight));
    let right = |part: PartRow| core_sum_1(db, part, |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)));
    assert forall|part: PartRow| #[trigger] left(part) == right(part) by {
        lemma_sql_core_1(db, parameters, part, year, weight);
    }
    lemma_sum_ext_all(db.part, left, right);
}

pub proof fn lemma_group_product(db: Db, parameters: Params, year: int, weight: spec_fn(Tuple) -> int)
    ensures group_sum(db, parameters, year, weight) == sum(route_rows_0(db), |route: RouteTuple|
        sum(core_rows_0(db), |core: CoreTuple| joined_weight(db, core, route, parameters, year, weight))),
{
    hide(group_sum);
    hide(sum);
    lemma_group_sum(db, parameters, year, weight);
    lemma_sql_core_0(db, parameters, year, weight);
    let nested = |core: CoreTuple| route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight));
    let per_core = |core: CoreTuple| sum(route_rows_0(db), |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight));
    assert forall|core: CoreTuple| #[trigger] nested(core) == per_core(core) by {
        lemma_route_sum_0(db, |route: RouteTuple| joined_weight(db, core, route, parameters, year, weight));
    }
    lemma_core_sum_0(db, nested);
    lemma_sum_ext_all(core_rows_0(db), nested, per_core);
    let product = |core: CoreTuple, route: RouteTuple| joined_weight(db, core, route, parameters, year, weight);
    let per_route = |route: RouteTuple| sum(core_rows_0(db), |core: CoreTuple| joined_weight(db, core, route, parameters, year, weight));
    assert forall|core: CoreTuple| #[trigger] per_core(core) == sum(route_rows_0(db), |route: RouteTuple| product(core, route)) by {
        assert((|route: RouteTuple| product(core, route)) =~= (|route: RouteTuple| joined_weight(db, core, route, parameters, year, weight)));
    }
    assert forall|route: RouteTuple| #[trigger] per_route(route) == sum(core_rows_0(db), |core: CoreTuple| product(core, route)) by {
        assert((|core: CoreTuple| product(core, route)) =~= (|core: CoreTuple| joined_weight(db, core, route, parameters, year, weight)));
    }
    lemma_sum_swap(core_rows_0(db), route_rows_0(db), product, per_core, per_route);
}

}
