use vstd::prelude::*;
use std::sync::Arc;
use std::collections::HashMap;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::storage::*;
use crate::catalog::{self, Binding, binding_count};
use crate::meaning::*;
use crate::maintenance;
use crate::output;
use verus_prooflib::wide::Wide;

verus! {

pub open spec fn history(s: State) -> nat { 0 }

pub closed spec fn rescan_lines_for_orders_wanted(s: State, customer_delta: Seq<CustomerDelta>, lineitem_delta: Seq<LineitemDelta>,
    nation_delta: Seq<NationDelta>, orders_delta: Seq<OrdersDelta>, region_delta: Seq<RegionDelta>, supplier_delta: Seq<SupplierDelta>) -> Seq<i64> {
    maintenance::wanted_orders(s.reverse, customer_delta, orders_delta)
}

pub open spec fn rescan_lines_for_suppliers_wanted(s: State, customer_delta: Seq<CustomerDelta>, lineitem_delta: Seq<LineitemDelta>,
    nation_delta: Seq<NationDelta>, orders_delta: Seq<OrdersDelta>, region_delta: Seq<RegionDelta>, supplier_delta: Seq<SupplierDelta>) -> Seq<i64> {
    maintenance::wanted_suppliers(supplier_delta)
}

pub struct State {
    db: Ghost<Db>,
    customers: Table,
    orders: Table,
    suppliers: Table,
    reverse: Table,
    profiles: Table,
    nations: Arc<Vec<Binding>>,
    regions: Arc<Vec<Binding>>,
    nation_regions: Arc<HashMap<i64, Vec<usize>>>,
    region_names: Arc<HashMap<u64, Vec<usize>>>,
}

impl State {
    pub closed spec fn db(self) -> Db { self.db@ }

    #[verifier::type_invariant]
    spec fn wf(self) -> bool {
        &&& projections_representable(self.db())
        &&& self.customers.wf() && self.orders.wf() && self.suppliers.wf()
        &&& self.reverse.wf() && self.profiles.wf()
        &&& self.customers.dimension() && self.suppliers.dimension() && self.reverse.dimension()
        &&& self.orders.counts_nonnegative() && self.profiles.daily()
        &&& forall|key: i64, nation: i64, day: i32| #[trigger] self.customers.at(key, nation, day) ==
            (if day == 0 { customer_count(self.db(), key, nation) } else { 0 }, 0)
        &&& forall|key: i64, nation: i64, day: i32| #[trigger] self.suppliers.at(key, nation, day) ==
            (if day == 0 { supplier_count(self.db(), key, nation) } else { 0 }, 0)
        &&& forall|key: i64, customer: i64, day: i32| #[trigger] self.orders.at(key, customer, day) ==
            (order_count(self.db(), key, customer, day), 0)
        &&& forall|customer: i64, order: i64, day: i32| #[trigger] self.reverse.at(customer, order, day) ==
            (if day == 0 { order_references(self.db(), customer, order) } else { 0 }, 0)
        &&& forall|nation: i64, sub: i64, day: i32| #[trigger] self.profiles.at(nation, sub, day) ==
            if sub == 0 { profile(self.db(), nation, day) } else { (0, 0) }
        &&& forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count((*self.nations)@, key, region, name) ==
            nation_count(self.db(), key, region, name)
        &&& forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count((*self.regions)@, key, region, name) ==
            if region == 0 { region_count(self.db(), key, name) } else { 0 }
        &&& forall|index: int| 0 <= index < (*self.nations)@.len() ==> (#[trigger] (*self.nations)@[index]).count@ > 0
        &&& forall|index: int| 0 <= index < (*self.regions)@.len() ==> (#[trigger] (*self.regions)@[index]).count@ > 0
        &&& catalog::bindings_unique((*self.nations)@) && catalog::bindings_unique((*self.regions)@)
        &&& catalog::regions_indexed((*self.nations)@, (*self.nation_regions)@)
        &&& catalog::names_indexed((*self.regions)@, (*self.region_names)@)
    }
}

pub fn build(
    customers: &[CustomerInput], orders: &[OrdersInput], suppliers: &[SupplierInput],
    nations: Vec<NationInput>, regions: Vec<RegionInput>, lines: &[LineitemInput], Ghost(db): Ghost<Db>,
) -> (r: Option<State>)
    requires scan_customers_ok(db, customers@), scan_orders_ok(db, orders@),
        scan_suppliers_ok(db, suppliers@), scan_nations_ok(db, nations@),
        scan_regions_ok(db, regions@), scan_lines_ok(db, lines@),
    ensures r matches Some(s) ==> s.db() == db, r is Some,
{
    let customers = crate::construction::customers(customers, Ghost(db));
    let suppliers = crate::construction::suppliers(suppliers, Ghost(db));
    let (orders, reverse) = crate::construction::orders(orders, Ghost(db));
    let profiles = crate::construction::profiles(&customers, &orders, &suppliers, lines, Ghost(db));
    let nation_rows = crate::construction::nations(&nations, Ghost(db));
    let region_rows = crate::construction::regions(&regions, Ghost(db));
    let nation_regions = Arc::new(catalog::region_index(&nation_rows));
    let region_names = Arc::new(catalog::name_index(&region_rows));
    Some(State { db: Ghost(db), customers, orders, suppliers, reverse, profiles, nation_regions, region_names,
        nations: Arc::new(nation_rows), regions: Arc::new(region_rows) })
}

pub open spec fn rows_model(rows: Seq<(Vec<u8>, Wide)>) -> Seq<OutRow> {
    rows.map_values(|row: (Vec<u8>, Wide)| OutRow { n_name: row.0@, revenue: row.1@ })
}

fn narrow(rows: &Vec<(Vec<u8>, Wide)>) -> (out: Option<Vec<(Vec<u8>, i128)>>)
    ensures out matches Some(narrowed) ==> out_view(narrowed@) == rows_model(rows@),
        out is None ==> exists|index: int| 0 <= index < rows@.len() && !out_fits(rows_model(rows@)[index]),
{
    let mut out: Vec<(Vec<u8>, i128)> = Vec::new();
    let mut index = 0;
    while index < rows.len()
        invariant index <= rows@.len(), out_view(out@) == rows_model(rows@).take(index as int),
        decreases rows@.len() - index,
    {
        let row = &rows[index];
        match row.1.to_i128() {
            Some(revenue) => {
                let ghost previous = out@;
                out.push((catalog::bytes_copy(&row.0), revenue));
                proof {
                    let model = rows_model(rows@);
                    assert(out@ =~= previous.push(out@.last()));
                    assert(out_view(out@) =~= out_view(previous).push(out_view(out@).last()));
                    assert(out_view(out@).last() == model[index as int]);
                    assert(model.take(index as int + 1) =~= model.take(index as int).push(model[index as int]));
                    assert(out_view(out@) =~= model.take(index as int + 1));
                }
            },
            None => {
                proof { assert(!out_fits(rows_model(rows@)[index as int])); }
                return None;
            },
        }
        index += 1;
    }
    proof { assert(rows_model(rows@).take(index as int) =~= rows_model(rows@)); }
    Some(out)
}

pub fn execute(s: &State, p1: &[u8], p2: i32, p3: i32) -> (r: Option<Vec<(Vec<u8>, i128)>>)
    ensures r matches Some(out) ==> query_result(s.db(), params(p1@, p2 as int, p3 as int), out_view(out@)),
        answer_fits(s.db(), params(p1@, p2 as int, p3 as int)) ==> r is Some,
{
    proof { use_type_invariant(s); }
    let rows = output::run(&s.profiles, &s.nations, &s.regions, &s.nation_regions, &s.region_names, p1, p2, p3);
    let ghost parameters = params(p1@, p2 as int, p3 as int);
    proof {
        assert forall|group: Seq<u8>| #[trigger] output::catalog_value(s.profiles, (*s.nations)@, (*s.regions)@, p1@, p2, p3, group) ==
            (group_count(s.db(), parameters, group), group_sum(s.db(), parameters, group, |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount))) by {
            crate::query_catalog::lemma_catalog_database(s.profiles, (*s.nations)@, (*s.regions)@, s.db(), p1@, p2, p3, group, false);
            crate::query_catalog::lemma_catalog_database(s.profiles, (*s.nations)@, (*s.regions)@, s.db(), p1@, p2, p3, group, true);
            lemma_sum_ext_all(tuples(s.db(), parameters), |tuple: Tuple| crate::query_proofs::tuple_weight(tuple, group, false),
                |tuple: Tuple| if tuple.nation.n_name == group { 1int } else { 0 });
            lemma_sum_ext_all(tuples(s.db(), parameters), |tuple: Tuple| crate::query_proofs::tuple_weight(tuple, group, true),
                |tuple: Tuple| if tuple.nation.n_name == group { tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) } else { 0 });
            let revenue_weight = |tuple: Tuple| tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount);
            assert((|tuple: Tuple| if tuple.nation.n_name == group { revenue_weight(tuple) } else { 0 }) =~=
                (|tuple: Tuple| if tuple.nation.n_name == group { tuple.lineitem.l_extendedprice * (100 - tuple.lineitem.l_discount) } else { 0 }));
        }
        let out = rows_model(rows@);
        assert forall|index: int| 0 <= index < out.len() implies row_ok(s.db(), parameters, #[trigger] out[index]) by {
            assert(output::catalog_value(s.profiles, (*s.nations)@, (*s.regions)@, p1@, p2, p3, rows@[index].0@).0 > 0);
        }
        assert forall|first: int, second: int| 0 <= first < second < out.len() implies
            !same_group(#[trigger] out[first], #[trigger] out[second]) && !row_lt(out[second], out[first]) by {
            assert(rows@[first].0@ != rows@[second].0@ && rows@[first].1@ >= rows@[second].1@);
        }
        assert forall|group: Seq<u8>| #[trigger] group_count(s.db(), parameters, group) > 0 && having(s.db(), parameters, group) implies
            exists|index: int| 0 <= index < out.len() && (#[trigger] out[index]).n_name == group by {
            assert(output::catalog_value(s.profiles, (*s.nations)@, (*s.regions)@, p1@, p2, p3, group).0 > 0);
            let index = choose|index: int| 0 <= index < rows@.len() && (#[trigger] rows@[index]).0@ == group;
            assert(out[index].n_name == group);
        }
        assert(query_result(s.db(), parameters, out));
    }
    match narrow(&rows) {
        Some(narrowed) => Some(narrowed),
        None => {
            proof {
                if answer_fits(s.db(), parameters) {
                    assert(query_result(s.db(), parameters, rows_model(rows@)));
                }
            }
            None
        },
    }
}

pub fn rescan_lines_for_orders_keys(
    s: &State, customer_delta: &[CustomerDelta], lineitem_delta: &[LineitemDelta],
    nation_delta: &Vec<NationDelta>, orders_delta: &[OrdersDelta],
    region_delta: &Vec<RegionDelta>, supplier_delta: &[SupplierDelta],
) -> (r: Vec<i64>)
    ensures r@ == rescan_lines_for_orders_wanted(*s, customer_delta@, lineitem_delta@, nation_delta@, orders_delta@, region_delta@, supplier_delta@),
{
    proof { use_type_invariant(s); }
    maintenance::order_keys(&s.reverse, customer_delta, orders_delta)
}

pub fn rescan_lines_for_suppliers_keys(
    s: &State, customer_delta: &[CustomerDelta], lineitem_delta: &[LineitemDelta],
    nation_delta: &Vec<NationDelta>, orders_delta: &[OrdersDelta],
    region_delta: &Vec<RegionDelta>, supplier_delta: &[SupplierDelta],
) -> (r: Vec<i64>)
    ensures r@ == rescan_lines_for_suppliers_wanted(*s, customer_delta@, lineitem_delta@, nation_delta@, orders_delta@, region_delta@, supplier_delta@),
{ maintenance::supplier_keys(supplier_delta) }

pub fn apply(
    s: &State, customer_delta: &[CustomerDelta], lineitem_delta: &[LineitemDelta],
    nation_delta: &Vec<NationDelta>, orders_delta: &[OrdersDelta],
    region_delta: &Vec<RegionDelta>, supplier_delta: &[SupplierDelta],
    lines_for_orders_keys: &[i64], lines_for_orders_rows: &[LineitemInput],
    lines_for_suppliers_keys: &[i64], lines_for_suppliers_rows: &[LineitemInput], Ghost(db2): Ghost<Db>,
) -> (r: Result<State, u32>)
    requires
        capture_ok(s.db(), db2, customer_delta_seq_view(customer_delta@), lineitem_delta_seq_view(lineitem_delta@), nation_delta_seq_view(nation_delta@), orders_delta_seq_view(orders_delta@), region_delta_seq_view(region_delta@), supplier_delta_seq_view(supplier_delta@)),
        lines_for_orders_keys@ == rescan_lines_for_orders_wanted(*s, customer_delta@, lineitem_delta@, nation_delta@, orders_delta@, region_delta@, supplier_delta@),
        rescan_lines_for_orders_ok(db2, lines_for_orders_keys@, lines_for_orders_rows@),
        lines_for_suppliers_keys@ == rescan_lines_for_suppliers_wanted(*s, customer_delta@, lineitem_delta@, nation_delta@, orders_delta@, region_delta@, supplier_delta@),
        rescan_lines_for_suppliers_ok(db2, lines_for_suppliers_keys@, lines_for_suppliers_rows@),
    ensures r matches Ok(s2) ==> s2.db() == db2, r is Ok,
{
    proof {
        use_type_invariant(s);
        crate::update_catalog::lemma_projections(s.db(), db2, customer_delta@, orders_delta@, supplier_delta@, nation_delta@, region_delta@);
    }
    let required_orders = lines_for_orders_keys;
    let required_suppliers = lines_for_suppliers_keys;
    let order_keys = maintenance::key_map(required_orders);
    let supplier_keys = maintenance::key_map(required_suppliers);
    proof {
        assert forall|key: i64| #[trigger] order_keys@.contains_key(key) <==> maintenance::affected_order(s.reverse, customer_delta@, orders_delta@, key) by {
            maintenance::lemma_wanted_orders(s.reverse, customer_delta@, orders_delta@, key);
        }
        assert forall|key: i64| #[trigger] supplier_keys@.contains_key(key) <==> maintenance::affected_supplier(supplier_delta@, key) by {
            maintenance::lemma_wanted_suppliers(supplier_delta@, key);
        }
    }
    let customers = crate::updates::customers(&s.customers, customer_delta, Ghost(s.db()), Ghost(db2));
    let orders = crate::updates::orders(&s.orders, orders_delta, Ghost(s.db()), Ghost(db2));
    let suppliers = crate::updates::suppliers(&s.suppliers, supplier_delta, Ghost(s.db()), Ghost(db2));
    let reverse = crate::updates::reverse(&s.reverse, orders_delta, Ghost(s.db()), Ghost(db2));
    let ghost before_dimensions = (s.customers, s.orders, s.suppliers);
    let ghost after_dimensions = (customers, orders, suppliers);
    let mut profile_changes = Bucket::new();
    crate::profile_changes::rescan(&s.customers, &s.orders, &s.suppliers, &customers, &orders, &suppliers, &order_keys, &supplier_keys, lines_for_orders_rows, false, &mut profile_changes);
    let ghost after_order_rescan = profile_changes;
    crate::profile_changes::rescan(&s.customers, &s.orders, &s.suppliers, &customers, &orders, &suppliers, &order_keys, &supplier_keys, lines_for_suppliers_rows, true, &mut profile_changes);
    let ghost after_supplier_rescan = profile_changes;
    crate::profile_changes::capture(&s.customers, &s.orders, &s.suppliers, lineitem_delta, &mut profile_changes);
    proof {
        assert forall|nation: i64, sub: i64, day: i32| #[trigger] profile_changes.at(nation, sub, day) ==
            if sub == 0 { (profile(db2, nation, day).0 - profile(s.db(), nation, day).0,
                profile(db2, nation, day).1 - profile(s.db(), nation, day).1) } else { (0, 0) } by {
            if sub == 0 {
                crate::maintenance_proofs::lemma_profile_delta(s.db(), db2, s.reverse, before_dimensions, after_dimensions,
                    customer_delta@, orders_delta@, supplier_delta@, lineitem_delta@, order_keys@, supplier_keys@,
                    required_orders@, lines_for_orders_keys@, lines_for_orders_rows@,
                    required_suppliers@, lines_for_suppliers_keys@, lines_for_suppliers_rows@, nation, day, false);
                crate::maintenance_proofs::lemma_profile_delta(s.db(), db2, s.reverse, before_dimensions, after_dimensions,
                    customer_delta@, orders_delta@, supplier_delta@, lineitem_delta@, order_keys@, supplier_keys@,
                    required_orders@, lines_for_orders_keys@, lines_for_orders_rows@,
                    required_suppliers@, lines_for_suppliers_keys@, lines_for_suppliers_rows@, nation, day, true);
                assert(after_order_rescan.at(nation, 0, day).0 == sum(lines_for_orders_rows@, |line: LineitemInput|
                    crate::profile_changes::rescan_weight(before_dimensions, after_dimensions, order_keys@, supplier_keys@, line, false, nation, day, false)));
                assert(after_order_rescan.at(nation, 0, day).1 == sum(lines_for_orders_rows@, |line: LineitemInput|
                    crate::profile_changes::rescan_weight(before_dimensions, after_dimensions, order_keys@, supplier_keys@, line, false, nation, day, true)));
                assert(after_supplier_rescan.at(nation, 0, day).0 == after_order_rescan.at(nation, 0, day).0 + sum(lines_for_suppliers_rows@, |line: LineitemInput|
                    crate::profile_changes::rescan_weight(before_dimensions, after_dimensions, order_keys@, supplier_keys@, line, true, nation, day, false)));
                assert(after_supplier_rescan.at(nation, 0, day).1 == after_order_rescan.at(nation, 0, day).1 + sum(lines_for_suppliers_rows@, |line: LineitemInput|
                    crate::profile_changes::rescan_weight(before_dimensions, after_dimensions, order_keys@, supplier_keys@, line, true, nation, day, true)));
            }
        }
    }
    let mut profiles = s.profiles.share();
    proof {
        assert forall|nation: i64, sub: i64, day: i32| #[trigger] profile_changes.at(nation, sub, day).0 + s.profiles.at(nation, sub, day).0 >= 0 by {
            assert(profile_changes.at(nation, sub, day) == if sub == 0 {
                (profile(db2, nation, day).0 - profile(s.db(), nation, day).0, profile(db2, nation, day).1 - profile(s.db(), nation, day).1)
            } else { (0, 0) });
            if sub == 0 { lemma_profile_shape(db2, nation, day); }
        }
    }
    merge(&mut profiles, &profile_changes, true);
    proof {
        assert forall|nation: i64, sub: i64, day: i32| #[trigger] profiles.at(nation, sub, day) ==
            if sub == 0 { profile(db2, nation, day) } else { (0, 0) } by {
            assert(profile_changes.at(nation, sub, day) == if sub == 0 {
                (profile(db2, nation, day).0 - profile(s.db(), nation, day).0, profile(db2, nation, day).1 - profile(s.db(), nation, day).1)
            } else { (0, 0) });
        }
        assert forall|nation: i64, sub: i64, day: i32| (#[trigger] profiles.at(nation, sub, day)).0 == 0 implies profiles.at(nation, sub, day).1 == 0 by {
            if sub == 0 { lemma_profile_shape(db2, nation, day); }
        }
        assert(profiles.daily());
    }
    let nations = crate::update_catalog::nations(&s.nations, nation_delta, Ghost(s.db()), Ghost(db2));
    let regions = crate::update_catalog::regions(&s.regions, region_delta, Ghost(s.db()), Ghost(db2));
    let nation_regions = if nation_delta.len() == 0 { s.nation_regions.clone() }
        else { Arc::new(catalog::region_index(&nations)) };
    let region_names = if region_delta.len() == 0 { s.region_names.clone() }
        else { Arc::new(catalog::name_index(&regions)) };
    Ok(State { db: Ghost(db2), customers, orders, suppliers, reverse, profiles, nations, regions, nation_regions, region_names })
}

}
