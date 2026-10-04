use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::catalog::{Binding, binding_count};
use crate::output;

verus! {

pub open spec fn line_value(line: LineitemRow, revenue: bool) -> int {
    if revenue { line.l_extendedprice * (100 - line.l_discount) } else { 1 }
}

pub open spec fn tuple_weight(tuple: Tuple, group: Seq<u8>, revenue: bool) -> int {
    if tuple.nation.n_name == group { line_value(tuple.lineitem, revenue) } else { 0 }
}

pub open spec fn atom(db: Db, parameters: Params, group: Seq<u8>, revenue: bool,
    customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow,
    supplier: SupplierRow, nation: NationRow, region: RegionRow) -> int {
    if where_clause(db, parameters, customer, orders, lineitem, supplier, nation, region)
        && nation.n_name == group { line_value(lineitem, revenue) } else { 0 }
}

pub open spec fn stage_0_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.customer, |customer: CustomerRow| stage_0_1(db, parameters, group, revenue, customer))
}

pub open spec fn stage_0_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_0_2(db, parameters, group, revenue, customer, orders))
}

pub open spec fn stage_0_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_0_3(db, parameters, group, revenue, customer, orders, lineitem))
}

pub open spec fn stage_0_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_0_4(db, parameters, group, revenue, customer, orders, lineitem, supplier))
}

pub open spec fn stage_0_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow) -> int {
    sum(db.nation, |nation: NationRow| stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation))
}

pub open spec fn stage_0_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow, nation: NationRow) -> int {
    sum(db.region, |region: RegionRow| stage_0_6(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region))
}

pub open spec fn stage_0_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow, nation: NationRow, region: RegionRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_1_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.customer, |customer: CustomerRow| stage_1_1(db, parameters, group, revenue, customer))
}

pub open spec fn stage_1_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_1_2(db, parameters, group, revenue, customer, orders))
}

pub open spec fn stage_1_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_1_3(db, parameters, group, revenue, customer, orders, lineitem))
}

pub open spec fn stage_1_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow) -> int {
    sum(db.nation, |nation: NationRow| stage_1_4(db, parameters, group, revenue, customer, orders, lineitem, nation))
}

pub open spec fn stage_1_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, nation: NationRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_1_5(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier))
}

pub open spec fn stage_1_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, nation: NationRow, supplier: SupplierRow) -> int {
    sum(db.region, |region: RegionRow| stage_1_6(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier, region))
}

pub open spec fn stage_1_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, nation: NationRow, supplier: SupplierRow, region: RegionRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_2_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.customer, |customer: CustomerRow| stage_2_1(db, parameters, group, revenue, customer))
}

pub open spec fn stage_2_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_2_2(db, parameters, group, revenue, customer, orders))
}

pub open spec fn stage_2_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow) -> int {
    sum(db.nation, |nation: NationRow| stage_2_3(db, parameters, group, revenue, customer, orders, nation))
}

pub open spec fn stage_2_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, nation: NationRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_2_4(db, parameters, group, revenue, customer, orders, nation, lineitem))
}

pub open spec fn stage_2_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, nation: NationRow, lineitem: LineitemRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_2_5(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier))
}

pub open spec fn stage_2_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, nation: NationRow, lineitem: LineitemRow, supplier: SupplierRow) -> int {
    sum(db.region, |region: RegionRow| stage_2_6(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier, region))
}

pub open spec fn stage_2_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, nation: NationRow, lineitem: LineitemRow, supplier: SupplierRow, region: RegionRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_3_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.customer, |customer: CustomerRow| stage_3_1(db, parameters, group, revenue, customer))
}

pub open spec fn stage_3_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow) -> int {
    sum(db.nation, |nation: NationRow| stage_3_2(db, parameters, group, revenue, customer, nation))
}

pub open spec fn stage_3_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_3_3(db, parameters, group, revenue, customer, nation, orders))
}

pub open spec fn stage_3_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow, orders: OrdersRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_3_4(db, parameters, group, revenue, customer, nation, orders, lineitem))
}

pub open spec fn stage_3_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow, orders: OrdersRow, lineitem: LineitemRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_3_5(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier))
}

pub open spec fn stage_3_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow) -> int {
    sum(db.region, |region: RegionRow| stage_3_6(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier, region))
}

pub open spec fn stage_3_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow, region: RegionRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_4_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.nation, |nation: NationRow| stage_4_1(db, parameters, group, revenue, nation))
}

pub open spec fn stage_4_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow) -> int {
    sum(db.customer, |customer: CustomerRow| stage_4_2(db, parameters, group, revenue, nation, customer))
}

pub open spec fn stage_4_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_4_3(db, parameters, group, revenue, nation, customer, orders))
}

pub open spec fn stage_4_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_4_4(db, parameters, group, revenue, nation, customer, orders, lineitem))
}

pub open spec fn stage_4_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_4_5(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier))
}

pub open spec fn stage_4_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow) -> int {
    sum(db.region, |region: RegionRow| stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region))
}

pub open spec fn stage_4_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow, region: RegionRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_5_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.nation, |nation: NationRow| stage_5_1(db, parameters, group, revenue, nation))
}

pub open spec fn stage_5_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow) -> int {
    sum(db.customer, |customer: CustomerRow| stage_5_2(db, parameters, group, revenue, nation, customer))
}

pub open spec fn stage_5_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_5_3(db, parameters, group, revenue, nation, customer, orders))
}

pub open spec fn stage_5_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_5_4(db, parameters, group, revenue, nation, customer, orders, lineitem))
}

pub open spec fn stage_5_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow) -> int {
    sum(db.region, |region: RegionRow| stage_5_5(db, parameters, group, revenue, nation, customer, orders, lineitem, region))
}

pub open spec fn stage_5_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, region: RegionRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_5_6(db, parameters, group, revenue, nation, customer, orders, lineitem, region, supplier))
}

pub open spec fn stage_5_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, region: RegionRow, supplier: SupplierRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_6_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.nation, |nation: NationRow| stage_6_1(db, parameters, group, revenue, nation))
}

pub open spec fn stage_6_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow) -> int {
    sum(db.customer, |customer: CustomerRow| stage_6_2(db, parameters, group, revenue, nation, customer))
}

pub open spec fn stage_6_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_6_3(db, parameters, group, revenue, nation, customer, orders))
}

pub open spec fn stage_6_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow) -> int {
    sum(db.region, |region: RegionRow| stage_6_4(db, parameters, group, revenue, nation, customer, orders, region))
}

pub open spec fn stage_6_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, region: RegionRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_6_5(db, parameters, group, revenue, nation, customer, orders, region, lineitem))
}

pub open spec fn stage_6_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, region: RegionRow, lineitem: LineitemRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_6_6(db, parameters, group, revenue, nation, customer, orders, region, lineitem, supplier))
}

pub open spec fn stage_6_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, region: RegionRow, lineitem: LineitemRow, supplier: SupplierRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_7_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.nation, |nation: NationRow| stage_7_1(db, parameters, group, revenue, nation))
}

pub open spec fn stage_7_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow) -> int {
    sum(db.customer, |customer: CustomerRow| stage_7_2(db, parameters, group, revenue, nation, customer))
}

pub open spec fn stage_7_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow) -> int {
    sum(db.region, |region: RegionRow| stage_7_3(db, parameters, group, revenue, nation, customer, region))
}

pub open spec fn stage_7_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, region: RegionRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_7_4(db, parameters, group, revenue, nation, customer, region, orders))
}

pub open spec fn stage_7_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, region: RegionRow, orders: OrdersRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_7_5(db, parameters, group, revenue, nation, customer, region, orders, lineitem))
}

pub open spec fn stage_7_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, region: RegionRow, orders: OrdersRow, lineitem: LineitemRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_7_6(db, parameters, group, revenue, nation, customer, region, orders, lineitem, supplier))
}

pub open spec fn stage_7_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, region: RegionRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_8_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.nation, |nation: NationRow| stage_8_1(db, parameters, group, revenue, nation))
}

pub open spec fn stage_8_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow) -> int {
    sum(db.region, |region: RegionRow| stage_8_2(db, parameters, group, revenue, nation, region))
}

pub open spec fn stage_8_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow) -> int {
    sum(db.customer, |customer: CustomerRow| stage_8_3(db, parameters, group, revenue, nation, region, customer))
}

pub open spec fn stage_8_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, customer: CustomerRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_8_4(db, parameters, group, revenue, nation, region, customer, orders))
}

pub open spec fn stage_8_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, customer: CustomerRow, orders: OrdersRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_8_5(db, parameters, group, revenue, nation, region, customer, orders, lineitem))
}

pub open spec fn stage_8_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_8_6(db, parameters, group, revenue, nation, region, customer, orders, lineitem, supplier))
}

pub open spec fn stage_8_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_9_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.nation, |nation: NationRow| stage_9_1(db, parameters, group, revenue, nation))
}

pub open spec fn stage_9_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow) -> int {
    sum(db.region, |region: RegionRow| stage_9_2(db, parameters, group, revenue, nation, region))
}

pub open spec fn stage_9_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_9_3(db, parameters, group, revenue, nation, region, orders))
}

pub open spec fn stage_9_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow) -> int {
    sum(db.customer, |customer: CustomerRow| stage_9_4(db, parameters, group, revenue, nation, region, orders, customer))
}

pub open spec fn stage_9_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow, customer: CustomerRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem))
}

pub open spec fn stage_9_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow, customer: CustomerRow, lineitem: LineitemRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_9_6(db, parameters, group, revenue, nation, region, orders, customer, lineitem, supplier))
}

pub open spec fn stage_9_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow, customer: CustomerRow, lineitem: LineitemRow, supplier: SupplierRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub open spec fn stage_10_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool) -> int {
    sum(db.nation, |nation: NationRow| stage_10_1(db, parameters, group, revenue, nation))
}

pub open spec fn stage_10_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow) -> int {
    sum(db.region, |region: RegionRow| stage_10_2(db, parameters, group, revenue, nation, region))
}

pub open spec fn stage_10_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow) -> int {
    sum(db.orders, |orders: OrdersRow| stage_10_3(db, parameters, group, revenue, nation, region, orders))
}

pub open spec fn stage_10_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow) -> int {
    sum(db.lineitem, |lineitem: LineitemRow| stage_10_4(db, parameters, group, revenue, nation, region, orders, lineitem))
}

pub open spec fn stage_10_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow, lineitem: LineitemRow) -> int {
    sum(db.customer, |customer: CustomerRow| stage_10_5(db, parameters, group, revenue, nation, region, orders, lineitem, customer))
}

pub open spec fn stage_10_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow, lineitem: LineitemRow, customer: CustomerRow) -> int {
    sum(db.supplier, |supplier: SupplierRow| stage_10_6(db, parameters, group, revenue, nation, region, orders, lineitem, customer, supplier))
}

pub open spec fn stage_10_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow, lineitem: LineitemRow, customer: CustomerRow, supplier: SupplierRow) -> int {
    atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region)
}

pub proof fn lemma_suffix_0_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow, nation: NationRow, region: RegionRow)
    ensures stage_0_6(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region) == stage_1_6(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier, region),
{
}

pub proof fn lemma_suffix_0_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow, nation: NationRow)
    ensures stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation) == stage_1_5(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier),
{
    assert forall|region: RegionRow| #[trigger] stage_0_6(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region) == stage_1_6(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier, region) by {
        lemma_suffix_0_6(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region);
    }
    lemma_sum_ext_all(db.region, |region: RegionRow| stage_0_6(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region), |region: RegionRow| stage_1_6(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier, region));
}

pub proof fn lemma_suffix_1_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, nation: NationRow, supplier: SupplierRow, region: RegionRow)
    ensures stage_1_6(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier, region) == stage_2_6(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier, region),
{
}

pub proof fn lemma_suffix_1_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, nation: NationRow, supplier: SupplierRow)
    ensures stage_1_5(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier) == stage_2_5(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier),
{
    assert forall|region: RegionRow| #[trigger] stage_1_6(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier, region) == stage_2_6(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier, region) by {
        lemma_suffix_1_6(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier, region);
    }
    lemma_sum_ext_all(db.region, |region: RegionRow| stage_1_6(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier, region), |region: RegionRow| stage_2_6(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier, region));
}

pub proof fn lemma_suffix_1_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, nation: NationRow)
    ensures stage_1_4(db, parameters, group, revenue, customer, orders, lineitem, nation) == stage_2_4(db, parameters, group, revenue, customer, orders, nation, lineitem),
{
    assert forall|supplier: SupplierRow| #[trigger] stage_1_5(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier) == stage_2_5(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier) by {
        lemma_suffix_1_5(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier);
    }
    lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| stage_1_5(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier), |supplier: SupplierRow| stage_2_5(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier));
}

pub proof fn lemma_suffix_2_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, nation: NationRow, lineitem: LineitemRow, supplier: SupplierRow, region: RegionRow)
    ensures stage_2_6(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier, region) == stage_3_6(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier, region),
{
}

pub proof fn lemma_suffix_2_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, nation: NationRow, lineitem: LineitemRow, supplier: SupplierRow)
    ensures stage_2_5(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier) == stage_3_5(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier),
{
    assert forall|region: RegionRow| #[trigger] stage_2_6(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier, region) == stage_3_6(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier, region) by {
        lemma_suffix_2_6(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier, region);
    }
    lemma_sum_ext_all(db.region, |region: RegionRow| stage_2_6(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier, region), |region: RegionRow| stage_3_6(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier, region));
}

pub proof fn lemma_suffix_2_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, nation: NationRow, lineitem: LineitemRow)
    ensures stage_2_4(db, parameters, group, revenue, customer, orders, nation, lineitem) == stage_3_4(db, parameters, group, revenue, customer, nation, orders, lineitem),
{
    assert forall|supplier: SupplierRow| #[trigger] stage_2_5(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier) == stage_3_5(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier) by {
        lemma_suffix_2_5(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier);
    }
    lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| stage_2_5(db, parameters, group, revenue, customer, orders, nation, lineitem, supplier), |supplier: SupplierRow| stage_3_5(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier));
}

pub proof fn lemma_suffix_2_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, nation: NationRow)
    ensures stage_2_3(db, parameters, group, revenue, customer, orders, nation) == stage_3_3(db, parameters, group, revenue, customer, nation, orders),
{
    assert forall|lineitem: LineitemRow| #[trigger] stage_2_4(db, parameters, group, revenue, customer, orders, nation, lineitem) == stage_3_4(db, parameters, group, revenue, customer, nation, orders, lineitem) by {
        lemma_suffix_2_4(db, parameters, group, revenue, customer, orders, nation, lineitem);
    }
    lemma_sum_ext_all(db.lineitem, |lineitem: LineitemRow| stage_2_4(db, parameters, group, revenue, customer, orders, nation, lineitem), |lineitem: LineitemRow| stage_3_4(db, parameters, group, revenue, customer, nation, orders, lineitem));
}

pub proof fn lemma_suffix_3_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow, region: RegionRow)
    ensures stage_3_6(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier, region) == stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region),
{
}

pub proof fn lemma_suffix_3_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow)
    ensures stage_3_5(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier) == stage_4_5(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier),
{
    assert forall|region: RegionRow| #[trigger] stage_3_6(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier, region) == stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region) by {
        lemma_suffix_3_6(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier, region);
    }
    lemma_sum_ext_all(db.region, |region: RegionRow| stage_3_6(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier, region), |region: RegionRow| stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region));
}

pub proof fn lemma_suffix_3_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow, orders: OrdersRow, lineitem: LineitemRow)
    ensures stage_3_4(db, parameters, group, revenue, customer, nation, orders, lineitem) == stage_4_4(db, parameters, group, revenue, nation, customer, orders, lineitem),
{
    assert forall|supplier: SupplierRow| #[trigger] stage_3_5(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier) == stage_4_5(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier) by {
        lemma_suffix_3_5(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier);
    }
    lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| stage_3_5(db, parameters, group, revenue, customer, nation, orders, lineitem, supplier), |supplier: SupplierRow| stage_4_5(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier));
}

pub proof fn lemma_suffix_3_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow, orders: OrdersRow)
    ensures stage_3_3(db, parameters, group, revenue, customer, nation, orders) == stage_4_3(db, parameters, group, revenue, nation, customer, orders),
{
    assert forall|lineitem: LineitemRow| #[trigger] stage_3_4(db, parameters, group, revenue, customer, nation, orders, lineitem) == stage_4_4(db, parameters, group, revenue, nation, customer, orders, lineitem) by {
        lemma_suffix_3_4(db, parameters, group, revenue, customer, nation, orders, lineitem);
    }
    lemma_sum_ext_all(db.lineitem, |lineitem: LineitemRow| stage_3_4(db, parameters, group, revenue, customer, nation, orders, lineitem), |lineitem: LineitemRow| stage_4_4(db, parameters, group, revenue, nation, customer, orders, lineitem));
}

pub proof fn lemma_suffix_3_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, nation: NationRow)
    ensures stage_3_2(db, parameters, group, revenue, customer, nation) == stage_4_2(db, parameters, group, revenue, nation, customer),
{
    assert forall|orders: OrdersRow| #[trigger] stage_3_3(db, parameters, group, revenue, customer, nation, orders) == stage_4_3(db, parameters, group, revenue, nation, customer, orders) by {
        lemma_suffix_3_3(db, parameters, group, revenue, customer, nation, orders);
    }
    lemma_sum_ext_all(db.orders, |orders: OrdersRow| stage_3_3(db, parameters, group, revenue, customer, nation, orders), |orders: OrdersRow| stage_4_3(db, parameters, group, revenue, nation, customer, orders));
}

pub proof fn lemma_suffix_4_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow, region: RegionRow)
    ensures stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region) == stage_5_6(db, parameters, group, revenue, nation, customer, orders, lineitem, region, supplier),
{
}

pub proof fn lemma_suffix_5_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, region: RegionRow, supplier: SupplierRow)
    ensures stage_5_6(db, parameters, group, revenue, nation, customer, orders, lineitem, region, supplier) == stage_6_6(db, parameters, group, revenue, nation, customer, orders, region, lineitem, supplier),
{
}

pub proof fn lemma_suffix_5_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, region: RegionRow)
    ensures stage_5_5(db, parameters, group, revenue, nation, customer, orders, lineitem, region) == stage_6_5(db, parameters, group, revenue, nation, customer, orders, region, lineitem),
{
    assert forall|supplier: SupplierRow| #[trigger] stage_5_6(db, parameters, group, revenue, nation, customer, orders, lineitem, region, supplier) == stage_6_6(db, parameters, group, revenue, nation, customer, orders, region, lineitem, supplier) by {
        lemma_suffix_5_6(db, parameters, group, revenue, nation, customer, orders, lineitem, region, supplier);
    }
    lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| stage_5_6(db, parameters, group, revenue, nation, customer, orders, lineitem, region, supplier), |supplier: SupplierRow| stage_6_6(db, parameters, group, revenue, nation, customer, orders, region, lineitem, supplier));
}

pub proof fn lemma_suffix_6_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, region: RegionRow, lineitem: LineitemRow, supplier: SupplierRow)
    ensures stage_6_6(db, parameters, group, revenue, nation, customer, orders, region, lineitem, supplier) == stage_7_6(db, parameters, group, revenue, nation, customer, region, orders, lineitem, supplier),
{
}

pub proof fn lemma_suffix_6_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, region: RegionRow, lineitem: LineitemRow)
    ensures stage_6_5(db, parameters, group, revenue, nation, customer, orders, region, lineitem) == stage_7_5(db, parameters, group, revenue, nation, customer, region, orders, lineitem),
{
    assert forall|supplier: SupplierRow| #[trigger] stage_6_6(db, parameters, group, revenue, nation, customer, orders, region, lineitem, supplier) == stage_7_6(db, parameters, group, revenue, nation, customer, region, orders, lineitem, supplier) by {
        lemma_suffix_6_6(db, parameters, group, revenue, nation, customer, orders, region, lineitem, supplier);
    }
    lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| stage_6_6(db, parameters, group, revenue, nation, customer, orders, region, lineitem, supplier), |supplier: SupplierRow| stage_7_6(db, parameters, group, revenue, nation, customer, region, orders, lineitem, supplier));
}

pub proof fn lemma_suffix_6_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, orders: OrdersRow, region: RegionRow)
    ensures stage_6_4(db, parameters, group, revenue, nation, customer, orders, region) == stage_7_4(db, parameters, group, revenue, nation, customer, region, orders),
{
    assert forall|lineitem: LineitemRow| #[trigger] stage_6_5(db, parameters, group, revenue, nation, customer, orders, region, lineitem) == stage_7_5(db, parameters, group, revenue, nation, customer, region, orders, lineitem) by {
        lemma_suffix_6_5(db, parameters, group, revenue, nation, customer, orders, region, lineitem);
    }
    lemma_sum_ext_all(db.lineitem, |lineitem: LineitemRow| stage_6_5(db, parameters, group, revenue, nation, customer, orders, region, lineitem), |lineitem: LineitemRow| stage_7_5(db, parameters, group, revenue, nation, customer, region, orders, lineitem));
}

pub proof fn lemma_suffix_7_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, region: RegionRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow)
    ensures stage_7_6(db, parameters, group, revenue, nation, customer, region, orders, lineitem, supplier) == stage_8_6(db, parameters, group, revenue, nation, region, customer, orders, lineitem, supplier),
{
}

pub proof fn lemma_suffix_7_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, region: RegionRow, orders: OrdersRow, lineitem: LineitemRow)
    ensures stage_7_5(db, parameters, group, revenue, nation, customer, region, orders, lineitem) == stage_8_5(db, parameters, group, revenue, nation, region, customer, orders, lineitem),
{
    assert forall|supplier: SupplierRow| #[trigger] stage_7_6(db, parameters, group, revenue, nation, customer, region, orders, lineitem, supplier) == stage_8_6(db, parameters, group, revenue, nation, region, customer, orders, lineitem, supplier) by {
        lemma_suffix_7_6(db, parameters, group, revenue, nation, customer, region, orders, lineitem, supplier);
    }
    lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| stage_7_6(db, parameters, group, revenue, nation, customer, region, orders, lineitem, supplier), |supplier: SupplierRow| stage_8_6(db, parameters, group, revenue, nation, region, customer, orders, lineitem, supplier));
}

pub proof fn lemma_suffix_7_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, region: RegionRow, orders: OrdersRow)
    ensures stage_7_4(db, parameters, group, revenue, nation, customer, region, orders) == stage_8_4(db, parameters, group, revenue, nation, region, customer, orders),
{
    assert forall|lineitem: LineitemRow| #[trigger] stage_7_5(db, parameters, group, revenue, nation, customer, region, orders, lineitem) == stage_8_5(db, parameters, group, revenue, nation, region, customer, orders, lineitem) by {
        lemma_suffix_7_5(db, parameters, group, revenue, nation, customer, region, orders, lineitem);
    }
    lemma_sum_ext_all(db.lineitem, |lineitem: LineitemRow| stage_7_5(db, parameters, group, revenue, nation, customer, region, orders, lineitem), |lineitem: LineitemRow| stage_8_5(db, parameters, group, revenue, nation, region, customer, orders, lineitem));
}

pub proof fn lemma_suffix_7_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, customer: CustomerRow, region: RegionRow)
    ensures stage_7_3(db, parameters, group, revenue, nation, customer, region) == stage_8_3(db, parameters, group, revenue, nation, region, customer),
{
    assert forall|orders: OrdersRow| #[trigger] stage_7_4(db, parameters, group, revenue, nation, customer, region, orders) == stage_8_4(db, parameters, group, revenue, nation, region, customer, orders) by {
        lemma_suffix_7_4(db, parameters, group, revenue, nation, customer, region, orders);
    }
    lemma_sum_ext_all(db.orders, |orders: OrdersRow| stage_7_4(db, parameters, group, revenue, nation, customer, region, orders), |orders: OrdersRow| stage_8_4(db, parameters, group, revenue, nation, region, customer, orders));
}

pub proof fn lemma_suffix_8_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow)
    ensures stage_8_6(db, parameters, group, revenue, nation, region, customer, orders, lineitem, supplier) == stage_9_6(db, parameters, group, revenue, nation, region, orders, customer, lineitem, supplier),
{
}

pub proof fn lemma_suffix_8_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow)
    ensures stage_8_5(db, parameters, group, revenue, nation, region, customer, orders, lineitem) == stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem),
{
    assert forall|supplier: SupplierRow| #[trigger] stage_8_6(db, parameters, group, revenue, nation, region, customer, orders, lineitem, supplier) == stage_9_6(db, parameters, group, revenue, nation, region, orders, customer, lineitem, supplier) by {
        lemma_suffix_8_6(db, parameters, group, revenue, nation, region, customer, orders, lineitem, supplier);
    }
    lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| stage_8_6(db, parameters, group, revenue, nation, region, customer, orders, lineitem, supplier), |supplier: SupplierRow| stage_9_6(db, parameters, group, revenue, nation, region, orders, customer, lineitem, supplier));
}

pub proof fn lemma_suffix_8_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, customer: CustomerRow, orders: OrdersRow)
    ensures stage_8_4(db, parameters, group, revenue, nation, region, customer, orders) == stage_9_4(db, parameters, group, revenue, nation, region, orders, customer),
{
    assert forall|lineitem: LineitemRow| #[trigger] stage_8_5(db, parameters, group, revenue, nation, region, customer, orders, lineitem) == stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem) by {
        lemma_suffix_8_5(db, parameters, group, revenue, nation, region, customer, orders, lineitem);
    }
    lemma_sum_ext_all(db.lineitem, |lineitem: LineitemRow| stage_8_5(db, parameters, group, revenue, nation, region, customer, orders, lineitem), |lineitem: LineitemRow| stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem));
}

pub proof fn lemma_suffix_9_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow, customer: CustomerRow, lineitem: LineitemRow, supplier: SupplierRow)
    ensures stage_9_6(db, parameters, group, revenue, nation, region, orders, customer, lineitem, supplier) == stage_10_6(db, parameters, group, revenue, nation, region, orders, lineitem, customer, supplier),
{
}

pub proof fn lemma_suffix_9_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, nation: NationRow, region: RegionRow, orders: OrdersRow, customer: CustomerRow, lineitem: LineitemRow)
    ensures stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem) == stage_10_5(db, parameters, group, revenue, nation, region, orders, lineitem, customer),
{
    assert forall|supplier: SupplierRow| #[trigger] stage_9_6(db, parameters, group, revenue, nation, region, orders, customer, lineitem, supplier) == stage_10_6(db, parameters, group, revenue, nation, region, orders, lineitem, customer, supplier) by {
        lemma_suffix_9_6(db, parameters, group, revenue, nation, region, orders, customer, lineitem, supplier);
    }
    lemma_sum_ext_all(db.supplier, |supplier: SupplierRow| stage_9_6(db, parameters, group, revenue, nation, region, orders, customer, lineitem, supplier), |supplier: SupplierRow| stage_10_6(db, parameters, group, revenue, nation, region, orders, lineitem, customer, supplier));
}

pub proof fn lemma_expand_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow, nation: NationRow)
    ensures sum(tuples_5(db, parameters, customer, orders, lineitem, supplier, nation), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation),
{
    let weight = |tuple: Tuple| tuple_weight(tuple, group, revenue);
    assert forall|region: RegionRow| sum(tuples_6(db, parameters, customer, orders, lineitem, supplier, nation, region), weight) ==
        #[trigger] stage_0_6(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region) by {
        lemma_sum_opt(where_clause(db, parameters, customer, orders, lineitem, supplier, nation, region),
            Tuple { customer, orders, lineitem, supplier, nation, region }, weight);
    }
    let mapper = |region: RegionRow| tuples_6(db, parameters, customer, orders, lineitem, supplier, nation, region);
    let nested = |region: RegionRow| stage_0_6(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region);
    verus_prooflib::lemma_flat_map_weight(db.region, mapper, weight, nested);
}

pub proof fn lemma_expand_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow, supplier: SupplierRow)
    ensures sum(tuples_4(db, parameters, customer, orders, lineitem, supplier), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == stage_0_4(db, parameters, group, revenue, customer, orders, lineitem, supplier),
{
    assert forall|nation: NationRow| sum(tuples_5(db, parameters, customer, orders, lineitem, supplier, nation), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == #[trigger] stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation) by {
        lemma_expand_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation);
    }
    let mapper = |nation: NationRow| tuples_5(db, parameters, customer, orders, lineitem, supplier, nation);
    let weight = |tuple: Tuple| tuple_weight(tuple, group, revenue);
    let nested = |nation: NationRow| stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation);
    verus_prooflib::lemma_flat_map_weight(db.nation, mapper, weight, nested);
}

pub proof fn lemma_expand_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow, lineitem: LineitemRow)
    ensures sum(tuples_3(db, parameters, customer, orders, lineitem), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == stage_0_3(db, parameters, group, revenue, customer, orders, lineitem),
{
    assert forall|supplier: SupplierRow| sum(tuples_4(db, parameters, customer, orders, lineitem, supplier), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == #[trigger] stage_0_4(db, parameters, group, revenue, customer, orders, lineitem, supplier) by {
        lemma_expand_4(db, parameters, group, revenue, customer, orders, lineitem, supplier);
    }
    let mapper = |supplier: SupplierRow| tuples_4(db, parameters, customer, orders, lineitem, supplier);
    let weight = |tuple: Tuple| tuple_weight(tuple, group, revenue);
    let nested = |supplier: SupplierRow| stage_0_4(db, parameters, group, revenue, customer, orders, lineitem, supplier);
    verus_prooflib::lemma_flat_map_weight(db.supplier, mapper, weight, nested);
}

pub proof fn lemma_expand_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow, orders: OrdersRow)
    ensures sum(tuples_2(db, parameters, customer, orders), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == stage_0_2(db, parameters, group, revenue, customer, orders),
{
    assert forall|lineitem: LineitemRow| sum(tuples_3(db, parameters, customer, orders, lineitem), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == #[trigger] stage_0_3(db, parameters, group, revenue, customer, orders, lineitem) by {
        lemma_expand_3(db, parameters, group, revenue, customer, orders, lineitem);
    }
    let mapper = |lineitem: LineitemRow| tuples_3(db, parameters, customer, orders, lineitem);
    let weight = |tuple: Tuple| tuple_weight(tuple, group, revenue);
    let nested = |lineitem: LineitemRow| stage_0_3(db, parameters, group, revenue, customer, orders, lineitem);
    verus_prooflib::lemma_flat_map_weight(db.lineitem, mapper, weight, nested);
}

pub proof fn lemma_expand_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool, customer: CustomerRow)
    ensures sum(tuples_1(db, parameters, customer), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == stage_0_1(db, parameters, group, revenue, customer),
{
    assert forall|orders: OrdersRow| sum(tuples_2(db, parameters, customer, orders), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == #[trigger] stage_0_2(db, parameters, group, revenue, customer, orders) by {
        lemma_expand_2(db, parameters, group, revenue, customer, orders);
    }
    let mapper = |orders: OrdersRow| tuples_2(db, parameters, customer, orders);
    let weight = |tuple: Tuple| tuple_weight(tuple, group, revenue);
    let nested = |orders: OrdersRow| stage_0_2(db, parameters, group, revenue, customer, orders);
    verus_prooflib::lemma_flat_map_weight(db.orders, mapper, weight, nested);
}

pub proof fn lemma_expand_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures sum(tuples(db, parameters), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == stage_0_0(db, parameters, group, revenue),
{
    assert forall|customer: CustomerRow| sum(tuples_1(db, parameters, customer), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == #[trigger] stage_0_1(db, parameters, group, revenue, customer) by {
        lemma_expand_1(db, parameters, group, revenue, customer);
    }
    let mapper = |customer: CustomerRow| tuples_1(db, parameters, customer);
    let weight = |tuple: Tuple| tuple_weight(tuple, group, revenue);
    let nested = |customer: CustomerRow| stage_0_1(db, parameters, group, revenue, customer);
    verus_prooflib::lemma_flat_map_weight(db.customer, mapper, weight, nested);
}

pub proof fn lemma_swap_0(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_0_0(db, parameters, group, revenue) == stage_1_0(db, parameters, group, revenue),
{
    assert forall|customer: CustomerRow| #[trigger] stage_0_1(db, parameters, group, revenue, customer) == stage_1_1(db, parameters, group, revenue, customer) by {
        assert forall|orders: OrdersRow| #[trigger] stage_0_2(db, parameters, group, revenue, customer, orders) == stage_1_2(db, parameters, group, revenue, customer, orders) by {
            assert forall|lineitem: LineitemRow| #[trigger] stage_0_3(db, parameters, group, revenue, customer, orders, lineitem) == stage_1_3(db, parameters, group, revenue, customer, orders, lineitem) by {
                let weight = |supplier: SupplierRow, nation: NationRow| stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation);
                let left_total = |supplier: SupplierRow| stage_0_4(db, parameters, group, revenue, customer, orders, lineitem, supplier);
                let right_total = |nation: NationRow| stage_1_4(db, parameters, group, revenue, customer, orders, lineitem, nation);
                assert forall|supplier: SupplierRow| #[trigger] left_total(supplier) == sum(db.nation, |nation: NationRow| weight(supplier, nation)) by {
                    assert((|nation: NationRow| weight(supplier, nation)) =~= (|nation: NationRow| stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation)));
                }
                assert forall|nation: NationRow| #[trigger] right_total(nation) == sum(db.supplier, |supplier: SupplierRow| weight(supplier, nation)) by {
                    assert((|supplier: SupplierRow| weight(supplier, nation)) =~= (|supplier: SupplierRow| stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation)));
                    assert forall|supplier: SupplierRow| #[trigger] stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation) == stage_1_5(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier) by {
                        lemma_suffix_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation);
                    }
                    assert((|supplier: SupplierRow| stage_0_5(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation)) =~= (|supplier: SupplierRow| stage_1_5(db, parameters, group, revenue, customer, orders, lineitem, nation, supplier)));
                }
                lemma_sum_swap(db.supplier, db.nation, weight, left_total, right_total);
            }
            lemma_sum_ext_all(db.lineitem, |lineitem: LineitemRow| stage_0_3(db, parameters, group, revenue, customer, orders, lineitem), |lineitem: LineitemRow| stage_1_3(db, parameters, group, revenue, customer, orders, lineitem));
        }
        lemma_sum_ext_all(db.orders, |orders: OrdersRow| stage_0_2(db, parameters, group, revenue, customer, orders), |orders: OrdersRow| stage_1_2(db, parameters, group, revenue, customer, orders));
    }
    lemma_sum_ext_all(db.customer, |customer: CustomerRow| stage_0_1(db, parameters, group, revenue, customer), |customer: CustomerRow| stage_1_1(db, parameters, group, revenue, customer));
}

pub proof fn lemma_swap_1(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_1_0(db, parameters, group, revenue) == stage_2_0(db, parameters, group, revenue),
{
    assert forall|customer: CustomerRow| #[trigger] stage_1_1(db, parameters, group, revenue, customer) == stage_2_1(db, parameters, group, revenue, customer) by {
        assert forall|orders: OrdersRow| #[trigger] stage_1_2(db, parameters, group, revenue, customer, orders) == stage_2_2(db, parameters, group, revenue, customer, orders) by {
            let weight = |lineitem: LineitemRow, nation: NationRow| stage_1_4(db, parameters, group, revenue, customer, orders, lineitem, nation);
            let left_total = |lineitem: LineitemRow| stage_1_3(db, parameters, group, revenue, customer, orders, lineitem);
            let right_total = |nation: NationRow| stage_2_3(db, parameters, group, revenue, customer, orders, nation);
            assert forall|lineitem: LineitemRow| #[trigger] left_total(lineitem) == sum(db.nation, |nation: NationRow| weight(lineitem, nation)) by {
                assert((|nation: NationRow| weight(lineitem, nation)) =~= (|nation: NationRow| stage_1_4(db, parameters, group, revenue, customer, orders, lineitem, nation)));
            }
            assert forall|nation: NationRow| #[trigger] right_total(nation) == sum(db.lineitem, |lineitem: LineitemRow| weight(lineitem, nation)) by {
                assert((|lineitem: LineitemRow| weight(lineitem, nation)) =~= (|lineitem: LineitemRow| stage_1_4(db, parameters, group, revenue, customer, orders, lineitem, nation)));
                assert forall|lineitem: LineitemRow| #[trigger] stage_1_4(db, parameters, group, revenue, customer, orders, lineitem, nation) == stage_2_4(db, parameters, group, revenue, customer, orders, nation, lineitem) by {
                    lemma_suffix_1_4(db, parameters, group, revenue, customer, orders, lineitem, nation);
                }
                assert((|lineitem: LineitemRow| stage_1_4(db, parameters, group, revenue, customer, orders, lineitem, nation)) =~= (|lineitem: LineitemRow| stage_2_4(db, parameters, group, revenue, customer, orders, nation, lineitem)));
            }
            lemma_sum_swap(db.lineitem, db.nation, weight, left_total, right_total);
        }
        lemma_sum_ext_all(db.orders, |orders: OrdersRow| stage_1_2(db, parameters, group, revenue, customer, orders), |orders: OrdersRow| stage_2_2(db, parameters, group, revenue, customer, orders));
    }
    lemma_sum_ext_all(db.customer, |customer: CustomerRow| stage_1_1(db, parameters, group, revenue, customer), |customer: CustomerRow| stage_2_1(db, parameters, group, revenue, customer));
}

pub proof fn lemma_swap_2(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_2_0(db, parameters, group, revenue) == stage_3_0(db, parameters, group, revenue),
{
    assert forall|customer: CustomerRow| #[trigger] stage_2_1(db, parameters, group, revenue, customer) == stage_3_1(db, parameters, group, revenue, customer) by {
        let weight = |orders: OrdersRow, nation: NationRow| stage_2_3(db, parameters, group, revenue, customer, orders, nation);
        let left_total = |orders: OrdersRow| stage_2_2(db, parameters, group, revenue, customer, orders);
        let right_total = |nation: NationRow| stage_3_2(db, parameters, group, revenue, customer, nation);
        assert forall|orders: OrdersRow| #[trigger] left_total(orders) == sum(db.nation, |nation: NationRow| weight(orders, nation)) by {
            assert((|nation: NationRow| weight(orders, nation)) =~= (|nation: NationRow| stage_2_3(db, parameters, group, revenue, customer, orders, nation)));
        }
        assert forall|nation: NationRow| #[trigger] right_total(nation) == sum(db.orders, |orders: OrdersRow| weight(orders, nation)) by {
            assert((|orders: OrdersRow| weight(orders, nation)) =~= (|orders: OrdersRow| stage_2_3(db, parameters, group, revenue, customer, orders, nation)));
            assert forall|orders: OrdersRow| #[trigger] stage_2_3(db, parameters, group, revenue, customer, orders, nation) == stage_3_3(db, parameters, group, revenue, customer, nation, orders) by {
                lemma_suffix_2_3(db, parameters, group, revenue, customer, orders, nation);
            }
            assert((|orders: OrdersRow| stage_2_3(db, parameters, group, revenue, customer, orders, nation)) =~= (|orders: OrdersRow| stage_3_3(db, parameters, group, revenue, customer, nation, orders)));
        }
        lemma_sum_swap(db.orders, db.nation, weight, left_total, right_total);
    }
    lemma_sum_ext_all(db.customer, |customer: CustomerRow| stage_2_1(db, parameters, group, revenue, customer), |customer: CustomerRow| stage_3_1(db, parameters, group, revenue, customer));
}

pub proof fn lemma_swap_3(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_3_0(db, parameters, group, revenue) == stage_4_0(db, parameters, group, revenue),
{
    let weight = |customer: CustomerRow, nation: NationRow| stage_3_2(db, parameters, group, revenue, customer, nation);
    let left_total = |customer: CustomerRow| stage_3_1(db, parameters, group, revenue, customer);
    let right_total = |nation: NationRow| stage_4_1(db, parameters, group, revenue, nation);
    assert forall|customer: CustomerRow| #[trigger] left_total(customer) == sum(db.nation, |nation: NationRow| weight(customer, nation)) by {
        assert((|nation: NationRow| weight(customer, nation)) =~= (|nation: NationRow| stage_3_2(db, parameters, group, revenue, customer, nation)));
    }
    assert forall|nation: NationRow| #[trigger] right_total(nation) == sum(db.customer, |customer: CustomerRow| weight(customer, nation)) by {
        assert((|customer: CustomerRow| weight(customer, nation)) =~= (|customer: CustomerRow| stage_3_2(db, parameters, group, revenue, customer, nation)));
        assert forall|customer: CustomerRow| #[trigger] stage_3_2(db, parameters, group, revenue, customer, nation) == stage_4_2(db, parameters, group, revenue, nation, customer) by {
            lemma_suffix_3_2(db, parameters, group, revenue, customer, nation);
        }
        assert((|customer: CustomerRow| stage_3_2(db, parameters, group, revenue, customer, nation)) =~= (|customer: CustomerRow| stage_4_2(db, parameters, group, revenue, nation, customer)));
    }
    lemma_sum_swap(db.customer, db.nation, weight, left_total, right_total);
}

pub proof fn lemma_swap_4(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_4_0(db, parameters, group, revenue) == stage_5_0(db, parameters, group, revenue),
{
    assert forall|nation: NationRow| #[trigger] stage_4_1(db, parameters, group, revenue, nation) == stage_5_1(db, parameters, group, revenue, nation) by {
        assert forall|customer: CustomerRow| #[trigger] stage_4_2(db, parameters, group, revenue, nation, customer) == stage_5_2(db, parameters, group, revenue, nation, customer) by {
            assert forall|orders: OrdersRow| #[trigger] stage_4_3(db, parameters, group, revenue, nation, customer, orders) == stage_5_3(db, parameters, group, revenue, nation, customer, orders) by {
                assert forall|lineitem: LineitemRow| #[trigger] stage_4_4(db, parameters, group, revenue, nation, customer, orders, lineitem) == stage_5_4(db, parameters, group, revenue, nation, customer, orders, lineitem) by {
                    let weight = |supplier: SupplierRow, region: RegionRow| stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region);
                    let left_total = |supplier: SupplierRow| stage_4_5(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier);
                    let right_total = |region: RegionRow| stage_5_5(db, parameters, group, revenue, nation, customer, orders, lineitem, region);
                    assert forall|supplier: SupplierRow| #[trigger] left_total(supplier) == sum(db.region, |region: RegionRow| weight(supplier, region)) by {
                        assert((|region: RegionRow| weight(supplier, region)) =~= (|region: RegionRow| stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region)));
                    }
                    assert forall|region: RegionRow| #[trigger] right_total(region) == sum(db.supplier, |supplier: SupplierRow| weight(supplier, region)) by {
                        assert((|supplier: SupplierRow| weight(supplier, region)) =~= (|supplier: SupplierRow| stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region)));
                        assert forall|supplier: SupplierRow| #[trigger] stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region) == stage_5_6(db, parameters, group, revenue, nation, customer, orders, lineitem, region, supplier) by {
                            lemma_suffix_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region);
                        }
                        assert((|supplier: SupplierRow| stage_4_6(db, parameters, group, revenue, nation, customer, orders, lineitem, supplier, region)) =~= (|supplier: SupplierRow| stage_5_6(db, parameters, group, revenue, nation, customer, orders, lineitem, region, supplier)));
                    }
                    lemma_sum_swap(db.supplier, db.region, weight, left_total, right_total);
                }
                lemma_sum_ext_all(db.lineitem, |lineitem: LineitemRow| stage_4_4(db, parameters, group, revenue, nation, customer, orders, lineitem), |lineitem: LineitemRow| stage_5_4(db, parameters, group, revenue, nation, customer, orders, lineitem));
            }
            lemma_sum_ext_all(db.orders, |orders: OrdersRow| stage_4_3(db, parameters, group, revenue, nation, customer, orders), |orders: OrdersRow| stage_5_3(db, parameters, group, revenue, nation, customer, orders));
        }
        lemma_sum_ext_all(db.customer, |customer: CustomerRow| stage_4_2(db, parameters, group, revenue, nation, customer), |customer: CustomerRow| stage_5_2(db, parameters, group, revenue, nation, customer));
    }
    lemma_sum_ext_all(db.nation, |nation: NationRow| stage_4_1(db, parameters, group, revenue, nation), |nation: NationRow| stage_5_1(db, parameters, group, revenue, nation));
}

pub proof fn lemma_swap_5(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_5_0(db, parameters, group, revenue) == stage_6_0(db, parameters, group, revenue),
{
    assert forall|nation: NationRow| #[trigger] stage_5_1(db, parameters, group, revenue, nation) == stage_6_1(db, parameters, group, revenue, nation) by {
        assert forall|customer: CustomerRow| #[trigger] stage_5_2(db, parameters, group, revenue, nation, customer) == stage_6_2(db, parameters, group, revenue, nation, customer) by {
            assert forall|orders: OrdersRow| #[trigger] stage_5_3(db, parameters, group, revenue, nation, customer, orders) == stage_6_3(db, parameters, group, revenue, nation, customer, orders) by {
                let weight = |lineitem: LineitemRow, region: RegionRow| stage_5_5(db, parameters, group, revenue, nation, customer, orders, lineitem, region);
                let left_total = |lineitem: LineitemRow| stage_5_4(db, parameters, group, revenue, nation, customer, orders, lineitem);
                let right_total = |region: RegionRow| stage_6_4(db, parameters, group, revenue, nation, customer, orders, region);
                assert forall|lineitem: LineitemRow| #[trigger] left_total(lineitem) == sum(db.region, |region: RegionRow| weight(lineitem, region)) by {
                    assert((|region: RegionRow| weight(lineitem, region)) =~= (|region: RegionRow| stage_5_5(db, parameters, group, revenue, nation, customer, orders, lineitem, region)));
                }
                assert forall|region: RegionRow| #[trigger] right_total(region) == sum(db.lineitem, |lineitem: LineitemRow| weight(lineitem, region)) by {
                    assert((|lineitem: LineitemRow| weight(lineitem, region)) =~= (|lineitem: LineitemRow| stage_5_5(db, parameters, group, revenue, nation, customer, orders, lineitem, region)));
                    assert forall|lineitem: LineitemRow| #[trigger] stage_5_5(db, parameters, group, revenue, nation, customer, orders, lineitem, region) == stage_6_5(db, parameters, group, revenue, nation, customer, orders, region, lineitem) by {
                        lemma_suffix_5_5(db, parameters, group, revenue, nation, customer, orders, lineitem, region);
                    }
                    assert((|lineitem: LineitemRow| stage_5_5(db, parameters, group, revenue, nation, customer, orders, lineitem, region)) =~= (|lineitem: LineitemRow| stage_6_5(db, parameters, group, revenue, nation, customer, orders, region, lineitem)));
                }
                lemma_sum_swap(db.lineitem, db.region, weight, left_total, right_total);
            }
            lemma_sum_ext_all(db.orders, |orders: OrdersRow| stage_5_3(db, parameters, group, revenue, nation, customer, orders), |orders: OrdersRow| stage_6_3(db, parameters, group, revenue, nation, customer, orders));
        }
        lemma_sum_ext_all(db.customer, |customer: CustomerRow| stage_5_2(db, parameters, group, revenue, nation, customer), |customer: CustomerRow| stage_6_2(db, parameters, group, revenue, nation, customer));
    }
    lemma_sum_ext_all(db.nation, |nation: NationRow| stage_5_1(db, parameters, group, revenue, nation), |nation: NationRow| stage_6_1(db, parameters, group, revenue, nation));
}

pub proof fn lemma_swap_6(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_6_0(db, parameters, group, revenue) == stage_7_0(db, parameters, group, revenue),
{
    assert forall|nation: NationRow| #[trigger] stage_6_1(db, parameters, group, revenue, nation) == stage_7_1(db, parameters, group, revenue, nation) by {
        assert forall|customer: CustomerRow| #[trigger] stage_6_2(db, parameters, group, revenue, nation, customer) == stage_7_2(db, parameters, group, revenue, nation, customer) by {
            let weight = |orders: OrdersRow, region: RegionRow| stage_6_4(db, parameters, group, revenue, nation, customer, orders, region);
            let left_total = |orders: OrdersRow| stage_6_3(db, parameters, group, revenue, nation, customer, orders);
            let right_total = |region: RegionRow| stage_7_3(db, parameters, group, revenue, nation, customer, region);
            assert forall|orders: OrdersRow| #[trigger] left_total(orders) == sum(db.region, |region: RegionRow| weight(orders, region)) by {
                assert((|region: RegionRow| weight(orders, region)) =~= (|region: RegionRow| stage_6_4(db, parameters, group, revenue, nation, customer, orders, region)));
            }
            assert forall|region: RegionRow| #[trigger] right_total(region) == sum(db.orders, |orders: OrdersRow| weight(orders, region)) by {
                assert((|orders: OrdersRow| weight(orders, region)) =~= (|orders: OrdersRow| stage_6_4(db, parameters, group, revenue, nation, customer, orders, region)));
                assert forall|orders: OrdersRow| #[trigger] stage_6_4(db, parameters, group, revenue, nation, customer, orders, region) == stage_7_4(db, parameters, group, revenue, nation, customer, region, orders) by {
                    lemma_suffix_6_4(db, parameters, group, revenue, nation, customer, orders, region);
                }
                assert((|orders: OrdersRow| stage_6_4(db, parameters, group, revenue, nation, customer, orders, region)) =~= (|orders: OrdersRow| stage_7_4(db, parameters, group, revenue, nation, customer, region, orders)));
            }
            lemma_sum_swap(db.orders, db.region, weight, left_total, right_total);
        }
        lemma_sum_ext_all(db.customer, |customer: CustomerRow| stage_6_2(db, parameters, group, revenue, nation, customer), |customer: CustomerRow| stage_7_2(db, parameters, group, revenue, nation, customer));
    }
    lemma_sum_ext_all(db.nation, |nation: NationRow| stage_6_1(db, parameters, group, revenue, nation), |nation: NationRow| stage_7_1(db, parameters, group, revenue, nation));
}

pub proof fn lemma_swap_7(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_7_0(db, parameters, group, revenue) == stage_8_0(db, parameters, group, revenue),
{
    assert forall|nation: NationRow| #[trigger] stage_7_1(db, parameters, group, revenue, nation) == stage_8_1(db, parameters, group, revenue, nation) by {
        let weight = |customer: CustomerRow, region: RegionRow| stage_7_3(db, parameters, group, revenue, nation, customer, region);
        let left_total = |customer: CustomerRow| stage_7_2(db, parameters, group, revenue, nation, customer);
        let right_total = |region: RegionRow| stage_8_2(db, parameters, group, revenue, nation, region);
        assert forall|customer: CustomerRow| #[trigger] left_total(customer) == sum(db.region, |region: RegionRow| weight(customer, region)) by {
            assert((|region: RegionRow| weight(customer, region)) =~= (|region: RegionRow| stage_7_3(db, parameters, group, revenue, nation, customer, region)));
        }
        assert forall|region: RegionRow| #[trigger] right_total(region) == sum(db.customer, |customer: CustomerRow| weight(customer, region)) by {
            assert((|customer: CustomerRow| weight(customer, region)) =~= (|customer: CustomerRow| stage_7_3(db, parameters, group, revenue, nation, customer, region)));
            assert forall|customer: CustomerRow| #[trigger] stage_7_3(db, parameters, group, revenue, nation, customer, region) == stage_8_3(db, parameters, group, revenue, nation, region, customer) by {
                lemma_suffix_7_3(db, parameters, group, revenue, nation, customer, region);
            }
            assert((|customer: CustomerRow| stage_7_3(db, parameters, group, revenue, nation, customer, region)) =~= (|customer: CustomerRow| stage_8_3(db, parameters, group, revenue, nation, region, customer)));
        }
        lemma_sum_swap(db.customer, db.region, weight, left_total, right_total);
    }
    lemma_sum_ext_all(db.nation, |nation: NationRow| stage_7_1(db, parameters, group, revenue, nation), |nation: NationRow| stage_8_1(db, parameters, group, revenue, nation));
}

pub proof fn lemma_swap_8(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_8_0(db, parameters, group, revenue) == stage_9_0(db, parameters, group, revenue),
{
    assert forall|nation: NationRow| #[trigger] stage_8_1(db, parameters, group, revenue, nation) == stage_9_1(db, parameters, group, revenue, nation) by {
        assert forall|region: RegionRow| #[trigger] stage_8_2(db, parameters, group, revenue, nation, region) == stage_9_2(db, parameters, group, revenue, nation, region) by {
            let weight = |customer: CustomerRow, orders: OrdersRow| stage_8_4(db, parameters, group, revenue, nation, region, customer, orders);
            let left_total = |customer: CustomerRow| stage_8_3(db, parameters, group, revenue, nation, region, customer);
            let right_total = |orders: OrdersRow| stage_9_3(db, parameters, group, revenue, nation, region, orders);
            assert forall|customer: CustomerRow| #[trigger] left_total(customer) == sum(db.orders, |orders: OrdersRow| weight(customer, orders)) by {
                assert((|orders: OrdersRow| weight(customer, orders)) =~= (|orders: OrdersRow| stage_8_4(db, parameters, group, revenue, nation, region, customer, orders)));
            }
            assert forall|orders: OrdersRow| #[trigger] right_total(orders) == sum(db.customer, |customer: CustomerRow| weight(customer, orders)) by {
                assert((|customer: CustomerRow| weight(customer, orders)) =~= (|customer: CustomerRow| stage_8_4(db, parameters, group, revenue, nation, region, customer, orders)));
                assert forall|customer: CustomerRow| #[trigger] stage_8_4(db, parameters, group, revenue, nation, region, customer, orders) == stage_9_4(db, parameters, group, revenue, nation, region, orders, customer) by {
                    lemma_suffix_8_4(db, parameters, group, revenue, nation, region, customer, orders);
                }
                assert((|customer: CustomerRow| stage_8_4(db, parameters, group, revenue, nation, region, customer, orders)) =~= (|customer: CustomerRow| stage_9_4(db, parameters, group, revenue, nation, region, orders, customer)));
            }
            lemma_sum_swap(db.customer, db.orders, weight, left_total, right_total);
        }
        lemma_sum_ext_all(db.region, |region: RegionRow| stage_8_2(db, parameters, group, revenue, nation, region), |region: RegionRow| stage_9_2(db, parameters, group, revenue, nation, region));
    }
    lemma_sum_ext_all(db.nation, |nation: NationRow| stage_8_1(db, parameters, group, revenue, nation), |nation: NationRow| stage_9_1(db, parameters, group, revenue, nation));
}

pub proof fn lemma_swap_9(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures stage_9_0(db, parameters, group, revenue) == stage_10_0(db, parameters, group, revenue),
{
    assert forall|nation: NationRow| #[trigger] stage_9_1(db, parameters, group, revenue, nation) == stage_10_1(db, parameters, group, revenue, nation) by {
        assert forall|region: RegionRow| #[trigger] stage_9_2(db, parameters, group, revenue, nation, region) == stage_10_2(db, parameters, group, revenue, nation, region) by {
            assert forall|orders: OrdersRow| #[trigger] stage_9_3(db, parameters, group, revenue, nation, region, orders) == stage_10_3(db, parameters, group, revenue, nation, region, orders) by {
                let weight = |customer: CustomerRow, lineitem: LineitemRow| stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem);
                let left_total = |customer: CustomerRow| stage_9_4(db, parameters, group, revenue, nation, region, orders, customer);
                let right_total = |lineitem: LineitemRow| stage_10_4(db, parameters, group, revenue, nation, region, orders, lineitem);
                assert forall|customer: CustomerRow| #[trigger] left_total(customer) == sum(db.lineitem, |lineitem: LineitemRow| weight(customer, lineitem)) by {
                    assert((|lineitem: LineitemRow| weight(customer, lineitem)) =~= (|lineitem: LineitemRow| stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem)));
                }
                assert forall|lineitem: LineitemRow| #[trigger] right_total(lineitem) == sum(db.customer, |customer: CustomerRow| weight(customer, lineitem)) by {
                    assert((|customer: CustomerRow| weight(customer, lineitem)) =~= (|customer: CustomerRow| stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem)));
                    assert forall|customer: CustomerRow| #[trigger] stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem) == stage_10_5(db, parameters, group, revenue, nation, region, orders, lineitem, customer) by {
                        lemma_suffix_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem);
                    }
                    assert((|customer: CustomerRow| stage_9_5(db, parameters, group, revenue, nation, region, orders, customer, lineitem)) =~= (|customer: CustomerRow| stage_10_5(db, parameters, group, revenue, nation, region, orders, lineitem, customer)));
                }
                lemma_sum_swap(db.customer, db.lineitem, weight, left_total, right_total);
            }
            lemma_sum_ext_all(db.orders, |orders: OrdersRow| stage_9_3(db, parameters, group, revenue, nation, region, orders), |orders: OrdersRow| stage_10_3(db, parameters, group, revenue, nation, region, orders));
        }
        lemma_sum_ext_all(db.region, |region: RegionRow| stage_9_2(db, parameters, group, revenue, nation, region), |region: RegionRow| stage_10_2(db, parameters, group, revenue, nation, region));
    }
    lemma_sum_ext_all(db.nation, |nation: NationRow| stage_9_1(db, parameters, group, revenue, nation), |nation: NationRow| stage_10_1(db, parameters, group, revenue, nation));
}

pub proof fn lemma_normalize(db: Db, parameters: Params, group: Seq<u8>, revenue: bool)
    ensures sum(tuples(db, parameters), |tuple: Tuple| tuple_weight(tuple, group, revenue)) == stage_10_0(db, parameters, group, revenue),
{
    lemma_expand_0(db, parameters, group, revenue);
    lemma_swap_0(db, parameters, group, revenue);
    lemma_swap_1(db, parameters, group, revenue);
    lemma_swap_2(db, parameters, group, revenue);
    lemma_swap_3(db, parameters, group, revenue);
    lemma_swap_4(db, parameters, group, revenue);
    lemma_swap_5(db, parameters, group, revenue);
    lemma_swap_6(db, parameters, group, revenue);
    lemma_swap_7(db, parameters, group, revenue);
    lemma_swap_8(db, parameters, group, revenue);
    lemma_swap_9(db, parameters, group, revenue);
}

}
