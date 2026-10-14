use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::storage::*;
use crate::catalog::{Binding, binding_count};
use crate::query_profiles::*;
use crate::query_proofs::*;
use crate::output;

verus! {

pub open spec fn catalog_gate(parameters: Params, group: Seq<u8>, nation: NationRow, region: RegionRow) -> bool {
    nation.n_name == group && nation.n_regionkey == region.r_regionkey && region.r_name == parameters.p1
}

pub proof fn lemma_domestic(db: Db, parameters: Params, group: Seq<u8>, revenue: bool,
    nation: NationRow, region: RegionRow, orders: OrdersRow, lineitem: LineitemRow)
    ensures stage_10_4(db, parameters, group, revenue, nation, region, orders, lineitem) ==
        if catalog_gate(parameters, group, nation, region) && parameters.p2 <= orders.o_orderdate < parameters.p3 {
            order_line_count(db, orders, lineitem, nation.n_nationkey) * line_value(lineitem, revenue)
        } else { 0 },
{
    let gate = catalog_gate(parameters, group, nation, region) && parameters.p2 <= orders.o_orderdate < parameters.p3;
    let by_customer = |customer: CustomerRow| stage_10_5(db, parameters, group, revenue, nation, region, orders, lineitem, customer);
    let count = |customer: CustomerRow| customer_weight(db, customer, orders, lineitem, nation.n_nationkey);
    if gate && orders.o_orderkey == lineitem.l_orderkey {
        assert forall|customer: CustomerRow| #[trigger] by_customer(customer) == line_value(lineitem, revenue) * count(customer) by {
            let by_supplier = |supplier: SupplierRow| stage_10_6(db, parameters, group, revenue, nation, region, orders, lineitem, customer, supplier);
            if customer.c_custkey == orders.o_custkey && customer.c_nationkey == nation.n_nationkey {
                let supplier_count = |supplier: SupplierRow| supplier_weight(supplier, lineitem, nation.n_nationkey);
                assert forall|supplier: SupplierRow| #[trigger] by_supplier(supplier) == line_value(lineitem, revenue) * supplier_count(supplier) by {
                    assert(by_supplier(supplier) == atom(db, parameters, group, revenue, customer, orders, lineitem, supplier, nation, region));
                    assert(where_clause(db, parameters, customer, orders, lineitem, supplier, nation, region) <==>
                        supplier.s_suppkey == lineitem.l_suppkey && supplier.s_nationkey == nation.n_nationkey);
                }
                lemma_sum_scale(db.supplier, supplier_count, by_supplier, line_value(lineitem, revenue));
            } else { lemma_sum_zero(db.supplier, by_supplier); }
        }
        lemma_sum_scale(db.customer, count, by_customer, line_value(lineitem, revenue));
        assert(line_value(lineitem, revenue) * sum(db.customer, count) == sum(db.customer, count) * line_value(lineitem, revenue)) by (nonlinear_arith);
    } else {
        assert forall|customer: CustomerRow| #[trigger] by_customer(customer) == 0 by {
            lemma_sum_zero(db.supplier, |supplier: SupplierRow| stage_10_6(db, parameters, group, revenue, nation, region, orders, lineitem, customer, supplier));
        }
        lemma_sum_zero(db.customer, by_customer);
    }
}

pub proof fn lemma_nation_window(db: Db, parameters: Params, group: Seq<u8>, revenue: bool,
    nation: NationRow, start: i32, end: i32)
    requires parameters.p2 == start, parameters.p3 == end, nation_representable(nation),
    ensures stage_10_1(db, parameters, group, revenue, nation) == if nation.n_name == group {
        region_count(db, nation.n_regionkey as i64, parameters.p1) * database_window(db, nation.n_nationkey, start, end, revenue)
    } else { 0 },
{
    let by_region = |region: RegionRow| stage_10_2(db, parameters, group, revenue, nation, region);
    let count = |region: RegionRow| ind(nation.n_name == group && nation.n_regionkey == region.r_regionkey && region.r_name == parameters.p1);
    let window = database_window(db, nation.n_nationkey, start, end, revenue);
    assert forall|region: RegionRow| #[trigger] by_region(region) == window * count(region) by {
        let by_order = |orders: OrdersRow| stage_10_3(db, parameters, group, revenue, nation, region, orders);
        let selected = |orders: OrdersRow| if start <= orders.o_orderdate < end { order_total(db, orders, nation.n_nationkey, revenue) } else { 0 };
        assert forall|orders: OrdersRow| #[trigger] by_order(orders) == if catalog_gate(parameters, group, nation, region) { selected(orders) } else { 0 } by {
            let by_line = |lineitem: LineitemRow| stage_10_4(db, parameters, group, revenue, nation, region, orders, lineitem);
            let weighted = |lineitem: LineitemRow| order_line_count(db, orders, lineitem, nation.n_nationkey) * line_value(lineitem, revenue);
            assert forall|lineitem: LineitemRow| #[trigger] by_line(lineitem) ==
                if catalog_gate(parameters, group, nation, region) && start <= orders.o_orderdate < end { weighted(lineitem) } else { 0 } by {
                lemma_domestic(db, parameters, group, revenue, nation, region, orders, lineitem);
            }
            if catalog_gate(parameters, group, nation, region) && start <= orders.o_orderdate < end {
                lemma_sum_ext_all(db.lineitem, by_line, weighted);
            } else { lemma_sum_zero(db.lineitem, by_line); }
        }
        if catalog_gate(parameters, group, nation, region) {
            lemma_sum_ext_all(db.orders, by_order, selected);
        } else { lemma_sum_zero(db.orders, by_order); }
    }
    lemma_sum_scale(db.region, count, by_region, window);
    if nation.n_name == group {
        lemma_sum_ext_all(db.region, count, |region: RegionRow| ind(region.r_regionkey == nation.n_regionkey && region.r_name == parameters.p1));
        assert(window * region_count(db, nation.n_regionkey as i64, parameters.p1) == region_count(db, nation.n_regionkey as i64, parameters.p1) * window) by (nonlinear_arith);
    } else { lemma_sum_zero(db.region, count); }
}

pub proof fn lemma_region_binding(regions: Seq<Binding>, db: Db, key: i64, name: Seq<u8>)
    requires forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(regions, key, region, name) ==
            if region == 0 { region_count(db, key, name) } else { 0 },
        forall|position: int| 0 <= position < regions.len() ==> (#[trigger] regions[position]).count > 0,
    ensures sum(regions, |region: Binding| output::region_count_weight(region, key, name)) == region_count(db, key, name),
{
    reveal(binding_count);
    assert forall|position: int| 0 <= position < regions.len() implies (#[trigger] regions[position]).region == 0 by {
        let row = regions[position];
        if row.region != 0 {
            let weight = |region: Binding| if region.key == row.key && region.region == row.region && region.name@ == row.name@ { region.count as int } else { 0 };
            lemma_sum_nonneg(regions, weight);
            lemma_sum_ge_term(regions, weight, position);
            assert(sum(regions, weight) == binding_count(regions, row.key, row.region, row.name@));
        }
    }
    crate::catalog::lemma_binding_weight(regions, key, 0, name, |region: Binding| output::region_count_weight(region, key, name));
    assert(binding_count(regions, key, 0, name) == region_count(db, key, name));
}

pub proof fn lemma_catalog_database(profiles: Table, nations: Seq<Binding>, regions: Seq<Binding>, db: Db,
    name: Seq<u8>, start: i32, end: i32, group: Seq<u8>, revenue: bool)
    requires profiles.wf(), profiles.daily(), projections_representable(db),
        forall|nation: i64, day: i32| #[trigger] profiles.at(nation, 0, day) == profile(db, nation, day),
        forall|key: i64, region: i64, text: Seq<u8>| #[trigger] binding_count(nations, key, region, text) == nation_count(db, key, region, text),
        forall|key: i64, region: i64, text: Seq<u8>| #[trigger] binding_count(regions, key, region, text) == if region == 0 { region_count(db, key, text) } else { 0 },
        forall|position: int| 0 <= position < regions.len() ==> (#[trigger] regions[position]).count > 0,
    ensures (if revenue { output::catalog_value(profiles, nations, regions, name, start, end, group).1 }
        else { output::catalog_value(profiles, nations, regions, name, start, end, group).0 }) ==
        sum(tuples(db, params(name, start as int, end as int)), |tuple: Tuple| tuple_weight(tuple, group, revenue)),
{
    let parameters = params(name, start as int, end as int);
    let left_project = |nation: Binding| (nation.key, nation.region, nation.name@);
    let right_project = |nation: NationRow| (nation.n_nationkey as i64, nation.n_regionkey as i64, nation.n_name);
    let left_multiplicity = |nation: Binding| nation.count as int;
    let right_multiplicity = |nation: NationRow| 1int;
    let weight = |key: (i64, i64, Seq<u8>)| if key.2 == group {
        region_count(db, key.1, name) * database_window(db, key.0 as int, start, end, revenue)
    } else { 0 };
    assert forall|key: (i64, i64, Seq<u8>)| #[trigger] verus_prooflib::class_total(nations, left_project, left_multiplicity, key) ==
        verus_prooflib::class_total(db.nation, right_project, right_multiplicity, key) by {
        lemma_sum_ext_all(nations, |nation: Binding| if left_project(nation) == key { left_multiplicity(nation) } else { 0 },
            |nation: Binding| if nation.key == key.0 && nation.region == key.1 && nation.name@ == key.2 { nation.count as int } else { 0 });
        let projected = |nation: NationRow| if right_project(nation) == key { 1int } else { 0 };
        let uncast = |nation: NationRow| ind(nation.n_nationkey == key.0 && nation.n_regionkey == key.1 && nation.n_name == key.2);
        assert forall|position: int| 0 <= position < db.nation.len() implies projected(#[trigger] db.nation[position]) == uncast(db.nation[position]) by {
            assert(nation_representable(db.nation[position]));
        }
        lemma_sum_ext(db.nation, projected, uncast);
        lemma_sum_ext_all(db.nation, |nation: NationRow| if right_project(nation) == key { right_multiplicity(nation) } else { 0 }, projected);
        assert(binding_count(nations, key.0, key.1, key.2) == nation_count(db, key.0, key.1, key.2));
    }
    verus_prooflib::lemma_equal_classes(nations, db.nation, left_project, right_project, left_multiplicity, right_multiplicity, weight);
    let executable = |nation: Binding| if revenue { output::catalog_nation_value(profiles, nation, regions, name, start, end, group).1 }
        else { output::catalog_nation_value(profiles, nation, regions, name, start, end, group).0 };
    let projected_left = |nation: Binding| left_multiplicity(nation) * weight(left_project(nation));
    assert forall|nation: Binding| #[trigger] executable(nation) == projected_left(nation) by {
        lemma_window_database(profiles, db, nation.key, start, end, revenue);
        lemma_region_binding(regions, db, nation.region, name);
        assert(output::catalog_weight(regions, nation, name) == region_count(db, nation.region, name));
        if nation.name@ == group {
            let window = database_window(db, nation.key as int, start, end, revenue);
            assert(nation.count as int * region_count(db, nation.region, name) * window == nation.count as int * (region_count(db, nation.region, name) * window)) by (nonlinear_arith);
        }
    }
    lemma_sum_ext_all(nations, executable, projected_left);
    let generated = |nation: NationRow| stage_10_1(db, parameters, group, revenue, nation);
    let projected_right = |nation: NationRow| right_multiplicity(nation) * weight(right_project(nation));
    assert forall|position: int| 0 <= position < db.nation.len() implies generated(#[trigger] db.nation[position]) == projected_right(db.nation[position]) by {
        assert(nation_representable(db.nation[position]));
        lemma_nation_window(db, parameters, group, revenue, db.nation[position], start, end);
    }
    lemma_sum_ext(db.nation, generated, projected_right);
    lemma_normalize(db, parameters, group, revenue);
    if revenue {
        lemma_sum_ext_all(nations, executable, |nation: Binding| output::catalog_nation_value(profiles, nation, regions, name, start, end, group).1);
    } else {
        lemma_sum_ext_all(nations, executable, |nation: Binding| output::catalog_nation_value(profiles, nation, regions, name, start, end, group).0);
    }
}

}
