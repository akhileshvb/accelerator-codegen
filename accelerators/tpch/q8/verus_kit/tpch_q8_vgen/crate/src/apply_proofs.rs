// Apply: what the new version's tables mean, stage by stage.
use vstd::prelude::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::table::*;
use crate::dictionary::*;
use crate::dimensions::*;
use crate::deltas::*;
use crate::maintenance::Keys;
use crate::model::*;

verus! {

/// The dimension tables of the new version mean the changed database's dimensions, ready for routing.
pub proof fn lemma_dimensions_applied(before: Data, before_db: Db, data: Data, db2: Db,
    part_dictionary: Dictionary, nation_dictionary: Dictionary,
    part_rows: Seq<PartDelta>, supplier_rows: Seq<SupplierDelta>, customer_rows: Seq<CustomerDelta>,
    order_rows: Seq<OrderDelta>, nation_rows: Seq<NationDelta>, region_rows: Seq<RegionDelta>,
    orders_full: Seq<(OrdersRow, int)>)
    requires
        valid(before, before_db),
        extension(before.dictionary, part_dictionary), extension(part_dictionary, nation_dictionary), extension(nation_dictionary, data.dictionary),
        bag_step(before_db.part, db2.part, part_delta_seq_view(part_rows)),
        bag_step(before_db.supplier, db2.supplier, supplier_delta_seq_view(supplier_rows)),
        bag_step(before_db.customer, db2.customer, customer_delta_seq_view(customer_rows)),
        bag_step(before_db.orders, db2.orders, orders_full), orders_delta_ok(order_rows, orders_full),
        bag_step(before_db.nation, db2.nation, nation_delta_seq_view(nation_rows)),
        bag_step(before_db.region, db2.region, region_delta_seq_view(region_rows)),
        parts_applied(before.parts, data.parts, part_dictionary, part_rows),
        suppliers_applied(before.suppliers, data.suppliers, supplier_rows),
        customers_applied(before.customers, data.customers, customer_rows),
        orders_applied(before.orders, data.orders, before.orders_by_customer, data.orders_by_customer, order_rows),
        nations_applied(before.nations, data.nations, before.nations_by_region, data.nations_by_region, nation_dictionary, nation_rows),
        regions_applied(before.regions, data.regions, data.dictionary, region_rows),
        supported_table(data.parts, data.dictionary, 0), supported_table(data.suppliers, data.dictionary, 1),
        supported_table(data.customers, data.dictionary, 2), supported_table(data.orders, data.dictionary, 3),
        supported_table(data.nations, data.dictionary, 4), supported_table(data.regions, data.dictionary, 5),
        supported_table(data.orders_by_customer, data.dictionary, 1), supported_table(data.nations_by_region, data.dictionary, 1),
        forall|offset: int| 0 <= offset < part_rows.len() ==> text_present(part_dictionary, 0, (#[trigger] part_rows[offset]).row.p_type@),
        forall|offset: int| 0 <= offset < nation_rows.len() ==> text_present(nation_dictionary, 1, (#[trigger] nation_rows[offset]).row.n_name@),
        forall|offset: int| 0 <= offset < region_rows.len() ==> text_present(data.dictionary, 2, (#[trigger] region_rows[offset]).row.r_name@),
    ensures dimension_meaning(data, db2), crate::route_proofs::route_ready(data), order_domain(db2), nation_domain(db2),
        positive(data.parts), positive(data.suppliers), positive(data.customers), positive(data.orders), positive(data.nations), positive(data.regions),
        positive(data.orders_by_customer), positive(data.nations_by_region), scalar(data.orders_by_customer), scalar(data.nations_by_region),
        scalar(data.parts), scalar(data.suppliers), scalar(data.customers), scalar(data.regions),
        text_domain(data.parts, data.dictionary, 0), text_domain(data.nations, data.dictionary, 1), text_domain(data.regions, data.dictionary, 2),
        crate::base_proofs::order_years(data.orders),
        forall|key: Major, index: int| 0 <= index < data.orders.rows(key).len() ==>
            dimension_support(key, minor_of(#[trigger] data.orders.rows(key)[index]), data.dictionary, 3) && data.orders.rows(key)[index].volume@ == 0,
        forall|key: Major, index: int| 0 <= index < data.nations.rows(key).len() ==>
            dimension_support(key, minor_of(#[trigger] data.nations.rows(key)[index]), data.dictionary, 4) && data.nations.rows(key)[index].volume@ == 0,
{
    reveal(valid);
    lemma_runtime_access(before);
    reveal(meaning);
    reveal(parts_applied); reveal(suppliers_applied); reveal(customers_applied);
    reveal(orders_applied); reveal(nations_applied); reveal(regions_applied);
    lemma_extension_transitive(part_dictionary, nation_dictionary, data.dictionary);
    lemma_extension_transitive(before.dictionary, part_dictionary, nation_dictionary);
    lemma_extension_transitive(before.dictionary, nation_dictionary, data.dictionary);
    lemma_supplier_capture(data.suppliers, before, supplier_rows, before_db, db2);
    lemma_customer_capture(data.customers, before, customer_rows, before_db, db2);
    lemma_order_capture(data.orders, data.orders_by_customer, before, order_rows, orders_full, before_db, db2);
    lemma_part_capture(data.parts, before, data.dictionary, part_dictionary, part_rows, before_db, db2);
    lemma_nation_capture(data.nations, data.nations_by_region, before, data.dictionary, nation_dictionary, nation_rows, before_db, db2);
    lemma_region_capture(data.regions, before, data.dictionary, region_rows, before_db, db2);
    reveal(dimension_meaning);
    assert(dimension_meaning(data, db2));
    lemma_reverse_cells(data.orders_by_customer, data, db2, true);
    lemma_reverse_cells(data.nations_by_region, data, db2, false);
    lemma_dimension_nonneg(data.parts, data, db2, 0);
    lemma_dimension_nonneg(data.suppliers, data, db2, 1);
    lemma_dimension_nonneg(data.customers, data, db2, 2);
    lemma_dimension_nonneg(data.orders, data, db2, 3);
    lemma_dimension_nonneg(data.nations, data, db2, 4);
    lemma_dimension_nonneg(data.regions, data, db2, 5);
    lemma_dimension_cells(data.parts, data.dictionary, 0);
    lemma_dimension_cells(data.suppliers, data.dictionary, 1);
    lemma_dimension_cells(data.customers, data.dictionary, 2);
    lemma_dimension_cells(data.orders, data.dictionary, 3);
    lemma_dimension_cells(data.nations, data.dictionary, 4);
    lemma_dimension_cells(data.regions, data.dictionary, 5);
    reveal(crate::route_proofs::route_ready);
    assert forall|key: Major, offset: int| 0 <= offset < data.nations.rows(key).len() implies
        crate::route_proofs::nation_cell_shape(key, #[trigger] data.nations.rows(key)[offset]) by {
        assert(dimension_support(key, minor_of(data.nations.rows(key)[offset]), data.dictionary, 4));
    }
    assert(crate::route_proofs::route_ready(data));
    assert forall|key: Major, index: int| 0 <= index < data.orders.rows(key).len() implies
        (#[trigger] data.orders.rows(key)[index]).second == 1995 || data.orders.rows(key)[index].second == 1996 by {
        assert(dimension_support(key, minor_of(data.orders.rows(key)[index]), data.dictionary, 3));
    }
}

/// The routes of the new version mean the changed database's routes.
pub proof fn lemma_routes_applied(before: Data, before_db: Db, data: Data, db2: Db, keys: Keys,
    nation_rows: Seq<NationDelta>, region_rows: Seq<RegionDelta>)
    requires
        valid(before, before_db), dimension_meaning(data, db2), crate::route_proofs::route_ready(data),
        extension(before.dictionary, data.dictionary), keys.valid(),
        crate::route_updates::route_key_coverage(keys, nation_rows, region_rows, before.nations_by_region),
        bag_step(before_db.nation, db2.nation, nation_delta_seq_view(nation_rows)),
        bag_step(before_db.region, db2.region, region_delta_seq_view(region_rows)),
        data.customer_routes.valid(), data.supplier_routes.valid(),
        crate::route_updates::routes_changed(before, data, keys),
    ensures route_meaning(data, db2),
        positive(data.customer_routes), positive(data.supplier_routes), scalar(data.customer_routes), scalar(data.supplier_routes),
        text_domain(data.customer_routes, data.dictionary, 2), text_domain(data.supplier_routes, data.dictionary, 1),
{
    lemma_valid_parts(before, before_db);
    lemma_runtime_access(before);
    lemma_meaning_parts(before, before_db);
    crate::route_updates::lemma_runtime_route_ready(before);
    crate::route_updates::lemma_apply_routes(before, data, before_db, db2, keys, nation_rows, region_rows);
    crate::route_updates::lemma_apply_route_cells(before, data, db2, keys);
}

}
