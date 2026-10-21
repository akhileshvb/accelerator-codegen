use vstd::prelude::*;
use std::sync::Arc;
use verus_common::sums::*;
use verus_common::bag::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::meaning::*;
use crate::catalog::{self, Binding, binding_count, bindings_unique};

verus! {

pub fn nations(before: &Arc<Vec<Binding>>, changes: &Vec<NationDelta>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (after: Arc<Vec<Binding>>)
    requires bag_step(db.nation, db2.nation, nation_delta_seq_view(changes@)),
        forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count((**before)@, key, region, name) == nation_count(db, key, region, name),
        forall|position: int| 0 <= position < (**before)@.len() ==> (#[trigger] (**before)@[position]).count@ > 0,
        bindings_unique((**before)@),
    ensures
        (forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count((*after)@, key, region, name) == nation_count(db2, key, region, name))
        && (forall|position: int| 0 <= position < (*after)@.len() ==> (#[trigger] (*after)@[position]).count@ > 0)
        && bindings_unique((*after)@)
        && (changes@.len() == 0 ==> (*after)@ == (**before)@),
{
    if changes.len() == 0 {
        proof {
            assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count((**before)@, key, region, name) == nation_count(db2, key, region, name) by {
                let weight = |row: NationRow| ind(row.n_nationkey == key && row.n_regionkey == region && row.n_name == name);
                let delta_weight = |delta: (NationRow, int)| delta.1 * weight(delta.0);
                lemma_sum_empty(nation_delta_seq_view(changes@), delta_weight);
                assert(sum(db2.nation, weight) == sum(db.nation, weight));
            }
        }
        return before.clone();
    }
    let mut rows = catalog::copy(before);
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index, bindings_unique(rows@),
            forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@, key, region, name) ==
                binding_count((**before)@, key, region, name) + sum(changes@.take(processed@), |delta: NationDelta| delta.sign * ind(delta.row.n_nationkey == key && delta.row.n_regionkey == region && delta.row.n_name@ == name)),
        decreases changes.len() - index,
    {
        let delta = &changes[index];
        catalog::add(&mut rows, delta.row.n_nationkey, delta.row.n_regionkey, &delta.row.n_name, delta.sign);
        proof {
            assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@, key, region, name) ==
                binding_count((**before)@, key, region, name) + sum(changes@.take(index as int + 1), |delta: NationDelta| delta.sign * ind(delta.row.n_nationkey == key && delta.row.n_regionkey == region && delta.row.n_name@ == name)) by {
                lemma_sum_take(changes@, index as int, |delta: NationDelta| delta.sign * ind(delta.row.n_nationkey == key && delta.row.n_regionkey == region && delta.row.n_name@ == name));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(changes@.take(changes@.len() as int) =~= changes@);
        assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@, key, region, name) == nation_count(db2, key, region, name) by {
            let weight = |row: NationRow| ind(row.n_nationkey == key && row.n_regionkey == region && row.n_name == name);
            let delta_weight = |delta: NationDelta| delta.sign * ind(delta.row.n_nationkey == key && delta.row.n_regionkey == region && delta.row.n_name@ == name);
            let captured = |delta: (NationRow, int)| delta.1 * weight(delta.0);
            lemma_sum_map(changes@, |delta: NationDelta| (nation_input_view(delta.row), delta.sign as int), captured, delta_weight);
            assert(sum(db2.nation, weight) == sum(db.nation, weight) + sum(nation_delta_seq_view(changes@), captured));
        }
        assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@, key, region, name) >= 0 by {
            lemma_sum_nonneg(db2.nation, |row: NationRow| ind(row.n_nationkey == key && row.n_regionkey == region && row.n_name == name));
        }
    }
    catalog::compact(&mut rows);
    Arc::new(rows)
}

pub fn regions(before: &Arc<Vec<Binding>>, changes: &Vec<RegionDelta>, Ghost(db): Ghost<Db>, Ghost(db2): Ghost<Db>) -> (after: Arc<Vec<Binding>>)
    requires bag_step(db.region, db2.region, region_delta_seq_view(changes@)),
        forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count((**before)@, key, region, name) == if region == 0 { region_count(db, key, name) } else { 0 },
        forall|position: int| 0 <= position < (**before)@.len() ==> (#[trigger] (**before)@[position]).count@ > 0,
        bindings_unique((**before)@),
    ensures
        (forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count((*after)@, key, region, name) == if region == 0 { region_count(db2, key, name) } else { 0 })
        && (forall|position: int| 0 <= position < (*after)@.len() ==> (#[trigger] (*after)@[position]).count@ > 0)
        && bindings_unique((*after)@)
        && (changes@.len() == 0 ==> (*after)@ == (**before)@),
{
    if changes.len() == 0 {
        proof {
            assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count((**before)@, key, region, name) == if region == 0 { region_count(db2, key, name) } else { 0 } by {
                let weight = |row: RegionRow| ind(row.r_regionkey == key && region == 0 && row.r_name == name);
                let delta_weight = |delta: (RegionRow, int)| delta.1 * weight(delta.0);
                lemma_sum_empty(region_delta_seq_view(changes@), delta_weight);
                assert(sum(db2.region, weight) == sum(db.region, weight));
                if region != 0 { lemma_sum_zero(db2.region, weight); }
                else { lemma_sum_ext_all(db.region, weight, |row: RegionRow| ind(row.r_regionkey == key && row.r_name == name)); lemma_sum_ext_all(db2.region, weight, |row: RegionRow| ind(row.r_regionkey == key && row.r_name == name)); }
            }
        }
        return before.clone();
    }
    let mut rows = catalog::copy(before);
    let mut index = 0;
    let mut processed: Ghost<int> = Ghost(0int);
    while index < changes.len()
        invariant index <= changes.len(), processed@ == index, bindings_unique(rows@),
            forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@, key, region, name) ==
                binding_count((**before)@, key, region, name) + sum(changes@.take(processed@), |delta: RegionDelta| delta.sign * ind(delta.row.r_regionkey == key && region == 0 && delta.row.r_name@ == name)),
        decreases changes.len() - index,
    {
        let delta = &changes[index];
        catalog::add(&mut rows, delta.row.r_regionkey, 0, &delta.row.r_name, delta.sign);
        proof {
            assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@, key, region, name) ==
                binding_count((**before)@, key, region, name) + sum(changes@.take(index as int + 1), |delta: RegionDelta| delta.sign * ind(delta.row.r_regionkey == key && region == 0 && delta.row.r_name@ == name)) by {
                lemma_sum_take(changes@, index as int, |delta: RegionDelta| delta.sign * ind(delta.row.r_regionkey == key && region == 0 && delta.row.r_name@ == name));
            }
        }
        index += 1;
        processed = Ghost(index as int);
    }
    proof {
        assert(changes@.take(changes@.len() as int) =~= changes@);
        assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@, key, region, name) == if region == 0 { region_count(db2, key, name) } else { 0 } by {
            let weight = |row: RegionRow| ind(row.r_regionkey == key && region == 0 && row.r_name == name);
            let delta_weight = |delta: RegionDelta| delta.sign * ind(delta.row.r_regionkey == key && region == 0 && delta.row.r_name@ == name);
            let captured = |delta: (RegionRow, int)| delta.1 * weight(delta.0);
            lemma_sum_map(changes@, |delta: RegionDelta| (region_input_view(delta.row), delta.sign as int), captured, delta_weight);
            assert(sum(db2.region, weight) == sum(db.region, weight) + sum(region_delta_seq_view(changes@), captured));
            if region != 0 { lemma_sum_zero(db.region, weight); lemma_sum_zero(db2.region, weight); }
            else { lemma_sum_ext_all(db.region, weight, |row: RegionRow| ind(row.r_regionkey == key && row.r_name == name)); lemma_sum_ext_all(db2.region, weight, |row: RegionRow| ind(row.r_regionkey == key && row.r_name == name)); }
        }
        assert forall|key: i64, region: i64, name: Seq<u8>| #[trigger] binding_count(rows@, key, region, name) >= 0 by {
            if region == 0 { lemma_sum_nonneg(db2.region, |row: RegionRow| ind(row.r_regionkey == key && row.r_name == name)); }
        }
    }
    catalog::compact(&mut rows);
    Arc::new(rows)
}

pub proof fn lemma_projections(db: Db, db2: Db, customers: Seq<CustomerDelta>, orders: Seq<OrdersDelta>, suppliers: Seq<SupplierDelta>, nations: Seq<NationDelta>, regions: Seq<RegionDelta>)
    requires projections_representable(db),
        bag_step(db.customer, db2.customer, customer_delta_seq_view(customers)),
        bag_step(db.orders, db2.orders, orders_delta_seq_view(orders)),
        bag_step(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers)),
        bag_step(db.nation, db2.nation, nation_delta_seq_view(nations)),
        bag_step(db.region, db2.region, region_delta_seq_view(regions)),
    ensures projections_representable(db2),
{
    verus_prooflib::lemma_bag_step_property(db.customer, db2.customer, customer_delta_seq_view(customers), |row: CustomerRow| customer_representable(row));
    verus_prooflib::lemma_bag_step_property(db.orders, db2.orders, orders_delta_seq_view(orders), |row: OrdersRow| order_representable(row));
    verus_prooflib::lemma_bag_step_property(db.supplier, db2.supplier, supplier_delta_seq_view(suppliers), |row: SupplierRow| supplier_representable(row));
    verus_prooflib::lemma_bag_step_property(db.nation, db2.nation, nation_delta_seq_view(nations), |row: NationRow| nation_representable(row));
    verus_prooflib::lemma_bag_step_property(db.region, db2.region, region_delta_seq_view(regions), |row: RegionRow| region_representable(row));
}

}
