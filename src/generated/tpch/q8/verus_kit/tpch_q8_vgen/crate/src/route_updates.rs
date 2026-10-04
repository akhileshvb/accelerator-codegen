use vstd::prelude::*;
use verus_common::bag::*;
use verus_common::sums::*;
use crate::dictionary::*;
use crate::gen_obligations::*;
use crate::gen_spec::*;
use crate::maintenance::*;
use crate::model::*;
use crate::route_proofs::*;
use crate::routing::*;
use crate::table::*;

verus! {

pub open spec fn customer_db_route(dictionary: Dictionary, db: Db, nation: i64, name: i64) -> int {
    sum(db.nation, |row: NationRow| if row.n_nationkey == nation as int {
        sum(db.region, |other: RegionRow| if row.n_regionkey == other.r_regionkey
            && text_matches(dictionary, name, other.r_name) { 1int } else { 0int })
    } else { 0int })
}

pub open spec fn supplier_db_route(dictionary: Dictionary, db: Db, nation: i64, name: i64) -> int {
    sum(db.nation, |row: NationRow| if row.n_nationkey == nation as int && text_matches(dictionary, name, row.n_name) { 1int } else { 0int })
}

pub open spec fn route_key_coverage(keys: Keys, nations: Seq<NationDelta>, regions: Seq<RegionDelta>, reverse: Table) -> bool {
    &&& forall|index: int| 0 <= index < nations.len() ==> keys.has((#[trigger] nations[index]).row.n_nationkey)
    &&& forall|index: int, offset: int| 0 <= index < regions.len()
        && 0 <= offset < reverse.rows((regions[index].row.r_regionkey, 0, 0)).len() ==>
        keys.has((#[trigger] reverse.rows((regions[index].row.r_regionkey, 0, 0))[offset]).first)
}

pub proof fn lemma_route_changes(keys: Keys, nations: Table, regions: Table, key: Major, minor: Major, measure: bool)
    requires keys.valid(), key.1 == 0, key.2 == 0,
    ensures customer_route_changes(keys.values@, nations, regions, key, minor, measure) ==
        if keys.has(key.0) { customer_nation_effect(nations.rows(key), regions, key.0, 1, key, minor, measure) } else { 0int },
        supplier_route_changes(keys.values@, nations, key, minor, measure) ==
        if keys.has(key.0) { supplier_nation_effect(nations.rows(key), key.0, 1, key, minor, measure) } else { 0int },
{
    hide(sum);
    reveal(customer_route_changes);
    reveal(supplier_route_changes);
    let customer_weight = |nation: i64| customer_nation_effect(nations.rows((nation, 0, 0)), regions, nation, 1, key, minor, measure);
    let supplier_weight = |nation: i64| supplier_nation_effect(nations.rows((nation, 0, 0)), nation, 1, key, minor, measure);
    let customer_value = customer_nation_effect(nations.rows(key), regions, key.0, 1, key, minor, measure);
    let supplier_value = supplier_nation_effect(nations.rows(key), key.0, 1, key, minor, measure);
    assert forall|nation: i64| #[trigger] customer_weight(nation) == if nation == key.0 { customer_value } else { 0int } by {
        if nation != key.0 { lemma_route_off_nation(nations.rows((nation, 0, 0)), regions, nation, 1, key, minor, measure); }
    }
    assert forall|nation: i64| #[trigger] supplier_weight(nation) == if nation == key.0 { supplier_value } else { 0int } by {
        if nation != key.0 { lemma_route_off_nation(nations.rows((nation, 0, 0)), regions, nation, 1, key, minor, measure); }
    }
    assert(distinct(keys.values@));
    if keys.has(key.0) {
        lemma_sum_indicator_distinct(keys.values@, key.0, customer_value, customer_weight);
        lemma_sum_indicator_distinct(keys.values@, key.0, supplier_value, supplier_weight);
    } else {
        assert forall|index: int| 0 <= index < keys.values@.len() implies customer_weight(#[trigger] keys.values@[index]) == 0 by {
            assert(keys.values@.contains(keys.values@[index]));
        }
        assert forall|index: int| 0 <= index < keys.values@.len() implies supplier_weight(#[trigger] keys.values@[index]) == 0 by {
            assert(keys.values@.contains(keys.values@[index]));
        }
        lemma_sum_zero(keys.values@, customer_weight);
        lemma_sum_zero(keys.values@, supplier_weight);
    }
}

pub proof fn lemma_no_changed_region(before: Data, db: Db, keys: Keys, nations: Seq<NationDelta>, regions: Seq<RegionDelta>, nation: i64, index: int)
    requires dimension_meaning(before, db), before.nations_by_region.valid(), route_key_coverage(keys, nations, regions, before.nations_by_region),
        !keys.has(nation), 0 <= index < db.nation.len(), db.nation[index].n_nationkey == nation as int,
    ensures forall|offset: int| 0 <= offset < regions.len() ==> (#[trigger] regions[offset]).row.r_regionkey as int != db.nation[index].n_regionkey,
{
    hide(sum);
    reveal(dimension_meaning);
    assert forall|offset: int| 0 <= offset < regions.len() implies (#[trigger] regions[offset]).row.r_regionkey as int != db.nation[index].n_regionkey by {
        let region = regions[offset].row.r_regionkey;
        if region as int == db.nation[index].n_regionkey {
            let weight = |row: NationRow| if row.n_regionkey == region as int && row.n_nationkey == nation as int { 1int } else { 0int };
            lemma_sum_pos(db.nation, weight, index);
            assert(before.nations_by_region.at((region, 0, 0), (nation, 0, 0), false) > 0);
            reveal(Table::at);
            reveal(cell_sum);
            let rows = before.nations_by_region.rows((region, 0, 0));
            let cell_weighting = |cell: Cell| cell_weight(cell, (nation, 0, 0), false);
            let position = lemma_sum_nonzero_witness(rows, cell_weighting);
            assert(rows[position].first == nation);
            assert(keys.has(rows[position].first));
        }
    }
}

pub proof fn lemma_database_routes_stable(before: Data, after: Data, db: Db, db2: Db,
    keys: Keys, nations: Seq<NationDelta>, regions: Seq<RegionDelta>, nation: i64, name: i64)
    requires dimension_meaning(before, db), extension(before.dictionary, after.dictionary), before.nations_by_region.valid(),
        route_key_coverage(keys, nations, regions, before.nations_by_region), !keys.has(nation),
        bag_step(db.nation, db2.nation, nation_delta_seq_view(nations)), bag_step(db.region, db2.region, region_delta_seq_view(regions)),
    ensures customer_db_route(before.dictionary, db, nation, name) == customer_db_route(after.dictionary, db2, nation, name),
        supplier_db_route(before.dictionary, db, nation, name) == supplier_db_route(after.dictionary, db2, nation, name),
{
    hide(sum);
    reveal(dimension_meaning);
    let old_dictionary = before.dictionary;
    let dictionary = after.dictionary;
    let customer_old = |row: NationRow| if row.n_nationkey == nation as int {
        sum(db.region, |other: RegionRow| if row.n_regionkey == other.r_regionkey && text_matches(old_dictionary, name, other.r_name) { 1int } else { 0int })
    } else { 0int };
    let customer_new = |row: NationRow| if row.n_nationkey == nation as int {
        sum(db2.region, |other: RegionRow| if row.n_regionkey == other.r_regionkey && text_matches(dictionary, name, other.r_name) { 1int } else { 0int })
    } else { 0int };
    assert forall|index: int| 0 <= index < db.nation.len() implies customer_old(#[trigger] db.nation[index]) == customer_new(db.nation[index]) by {
        let row = db.nation[index];
        if row.n_nationkey == nation as int {
            lemma_no_changed_region(before, db, keys, nations, regions, nation, index);
            let old_weight = |other: RegionRow| if row.n_regionkey == other.r_regionkey && text_matches(old_dictionary, name, other.r_name) { 1int } else { 0int };
            let weight = |other: RegionRow| if row.n_regionkey == other.r_regionkey && text_matches(dictionary, name, other.r_name) { 1int } else { 0int };
            assert forall|offset: int| 0 <= offset < db.region.len() implies old_weight(#[trigger] db.region[offset]) == weight(db.region[offset]) by {
                lemma_known_text_extension(old_dictionary, dictionary, db.region[offset].r_name);
            }
            lemma_sum_ext(db.region, old_weight, weight);
            let deltas = region_delta_seq_view(regions);
            let signed = |change: (RegionRow, int)| change.1 * weight(change.0);
            assert forall|offset: int| 0 <= offset < deltas.len() implies signed(#[trigger] deltas[offset]) == 0 by {
                assert(deltas[offset].0 == region_input_view(regions[offset].row));
            }
            lemma_sum_zero(deltas, signed);
            assert(sum(db2.region, weight) == sum(db.region, weight) + sum(deltas, signed));
        }
    }
    lemma_sum_ext(db.nation, customer_old, customer_new);
    let deltas = nation_delta_seq_view(nations);
    let customer_signed = |change: (NationRow, int)| change.1 * customer_new(change.0);
    assert forall|index: int| 0 <= index < deltas.len() implies customer_signed(#[trigger] deltas[index]) == 0 by {
        assert(deltas[index].0 == nation_input_view(nations[index].row));
        assert(keys.has(nations[index].row.n_nationkey));
    }
    lemma_sum_zero(deltas, customer_signed);
    assert(sum(db2.nation, customer_new) == sum(db.nation, customer_new) + sum(deltas, customer_signed));
    let supplier_old = |row: NationRow| if row.n_nationkey == nation as int && text_matches(old_dictionary, name, row.n_name) { 1int } else { 0int };
    let supplier_new = |row: NationRow| if row.n_nationkey == nation as int && text_matches(dictionary, name, row.n_name) { 1int } else { 0int };
    assert forall|index: int| 0 <= index < db.nation.len() implies supplier_old(#[trigger] db.nation[index]) == supplier_new(db.nation[index]) by {
        lemma_known_text_extension(old_dictionary, dictionary, db.nation[index].n_name);
    }
    lemma_sum_ext(db.nation, supplier_old, supplier_new);
    let supplier_signed = |change: (NationRow, int)| change.1 * supplier_new(change.0);
    assert forall|index: int| 0 <= index < deltas.len() implies supplier_signed(#[trigger] deltas[index]) == 0 by {
        assert(deltas[index].0 == nation_input_view(nations[index].row));
        assert(keys.has(nations[index].row.n_nationkey));
    }
    lemma_sum_zero(deltas, supplier_signed);
    assert(sum(db2.nation, supplier_new) == sum(db.nation, supplier_new) + sum(deltas, supplier_signed));
}

pub proof fn lemma_apply_routes(before: Data, after: Data, db: Db, db2: Db, keys: Keys, nations: Seq<NationDelta>, regions: Seq<RegionDelta>)
    requires dimension_meaning(before, db), dimension_meaning(after, db2), route_ready(before), route_ready(after), route_meaning(before, db),
        before.nations_by_region.valid(), extension(before.dictionary, after.dictionary), keys.valid(),
        route_key_coverage(keys, nations, regions, before.nations_by_region),
        bag_step(db.nation, db2.nation, nation_delta_seq_view(nations)), bag_step(db.region, db2.region, region_delta_seq_view(regions)),
        forall|key: Major, minor: Major, measure: bool| #[trigger] after.customer_routes.at(key, minor, measure)
            == before.customer_routes.at(key, minor, measure)
                + customer_route_changes(keys.values@, after.nations, after.regions, key, minor, measure)
                - customer_route_changes(keys.values@, before.nations, before.regions, key, minor, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] after.supplier_routes.at(key, minor, measure)
            == before.supplier_routes.at(key, minor, measure)
                + supplier_route_changes(keys.values@, after.nations, key, minor, measure)
                - supplier_route_changes(keys.values@, before.nations, key, minor, measure),
    ensures route_meaning(after, db2),
{
    hide(sum);
    reveal(route_meaning);
    assert forall|nation: i64, name: i64| #[trigger] after.customer_routes.at((nation, 0, 0), (name, 0, 0), false)
        == customer_db_route(after.dictionary, db2, nation, name) by {
        lemma_route_changes(keys, before.nations, before.regions, (nation, 0, 0), (name, 0, 0), false);
        lemma_route_changes(keys, after.nations, after.regions, (nation, 0, 0), (name, 0, 0), false);
        lemma_customer_nation(before, db, nation, name);
        lemma_customer_nation(after, db2, nation, name);
        if !keys.has(nation) { lemma_database_routes_stable(before, after, db, db2, keys, nations, regions, nation, name); }
    }
    assert forall|nation: i64, name: i64| #[trigger] after.supplier_routes.at((nation, 0, 0), (name, 0, 0), false)
        == supplier_db_route(after.dictionary, db2, nation, name) by {
        lemma_route_changes(keys, before.nations, before.regions, (nation, 0, 0), (name, 0, 0), false);
        lemma_route_changes(keys, after.nations, after.regions, (nation, 0, 0), (name, 0, 0), false);
        lemma_supplier_nation(before, db, nation, (nation, 0, 0), (name, 0, 0), 1, false);
        lemma_supplier_nation(after, db2, nation, (nation, 0, 0), (name, 0, 0), 1, false);
        if !keys.has(nation) { lemma_database_routes_stable(before, after, db, db2, keys, nations, regions, nation, name); }
    }
}

pub proof fn lemma_runtime_route_ready(data: Data)
    requires runtime_valid(data),
    ensures route_ready(data),
{
    lemma_runtime_access(data);
    assert forall|key: Major, index: int| 0 <= index < data.nations.rows(key).len() implies
        nation_cell_shape(key, #[trigger] data.nations.rows(key)[index]) by {}
}

pub proof fn lemma_route_change_support(keys: Keys, nations: Table, regions: Table, dictionary: Dictionary, key: Major, minor: Major, measure: bool)
    requires text_domain(nations, dictionary), text_domain(regions, dictionary),
        measure || !crate::dimensions::dimension_support(key, minor, dictionary, 0),
    ensures customer_route_changes(keys.values@, nations, regions, key, minor, measure) == 0,
        supplier_route_changes(keys.values@, nations, key, minor, measure) == 0,
{
    hide(sum);
    reveal(customer_route_changes);
    reveal(supplier_route_changes);
    let customer_weight = |nation: i64| customer_nation_effect(nations.rows((nation, 0, 0)), regions, nation, 1, key, minor, measure);
    let supplier_weight = |nation: i64| supplier_nation_effect(nations.rows((nation, 0, 0)), nation, 1, key, minor, measure);
    assert forall|index: int| 0 <= index < keys.values@.len() implies customer_weight(#[trigger] keys.values@[index]) == 0 by {
        lemma_route_support(nations, regions, dictionary, keys.values@[index], 1, key, minor, measure);
    }
    assert forall|index: int| 0 <= index < keys.values@.len() implies supplier_weight(#[trigger] keys.values@[index]) == 0 by {
        lemma_route_support(nations, regions, dictionary, keys.values@[index], 1, key, minor, measure);
    }
    lemma_sum_zero(keys.values@, customer_weight);
    lemma_sum_zero(keys.values@, supplier_weight);
}

pub proof fn lemma_apply_route_cells(before: Data, after: Data, db2: Db, keys: Keys)
    requires runtime_valid(before), route_ready(after), route_meaning(after, db2), extension(before.dictionary, after.dictionary),
        after.customer_routes.valid(), after.supplier_routes.valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] after.customer_routes.at(key, minor, measure)
            == before.customer_routes.at(key, minor, measure)
                + customer_route_changes(keys.values@, after.nations, after.regions, key, minor, measure)
                - customer_route_changes(keys.values@, before.nations, before.regions, key, minor, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] after.supplier_routes.at(key, minor, measure)
            == before.supplier_routes.at(key, minor, measure)
                + supplier_route_changes(keys.values@, after.nations, key, minor, measure)
                - supplier_route_changes(keys.values@, before.nations, key, minor, measure),
    ensures positive(after.customer_routes), positive(after.supplier_routes), scalar(after.customer_routes), scalar(after.supplier_routes),
        text_domain(after.customer_routes, after.dictionary), text_domain(after.supplier_routes, after.dictionary),
{
    hide(sum);
    lemma_runtime_access(before);
    assert forall|key: Major, index: int| 0 <= index < before.nations.rows(key).len() implies
        0 <= (#[trigger] before.nations.rows(key)[index]).first < after.dictionary.length by {}
    assert forall|key: Major, index: int| 0 <= index < before.regions.rows(key).len() implies
        0 <= (#[trigger] before.regions.rows(key)[index]).first < after.dictionary.length by {}
    assert forall|key: Major, index: int| 0 <= index < before.customer_routes.rows(key).len() implies
        crate::dimensions::dimension_support(key, minor_of(#[trigger] before.customer_routes.rows(key)[index]), after.dictionary, 0)
            && before.customer_routes.rows(key)[index].volume == 0 by {
        assert(scalar_cell(key, before.customer_routes.rows(key)[index]));
        assert(0 <= before.customer_routes.rows(key)[index].first < before.dictionary.length);
    }
    assert forall|key: Major, index: int| 0 <= index < before.supplier_routes.rows(key).len() implies
        crate::dimensions::dimension_support(key, minor_of(#[trigger] before.supplier_routes.rows(key)[index]), after.dictionary, 0)
            && before.supplier_routes.rows(key)[index].volume == 0 by {
        assert(scalar_cell(key, before.supplier_routes.rows(key)[index]));
        assert(0 <= before.supplier_routes.rows(key)[index].first < before.dictionary.length);
    }
    crate::dimensions::lemma_support_from_cells(before.customer_routes, after.dictionary, 0);
    crate::dimensions::lemma_support_from_cells(before.supplier_routes, after.dictionary, 0);
    assert forall|key: Major, minor: Major, measure: bool| measure || !crate::dimensions::dimension_support(key, minor, after.dictionary, 0)
        implies #[trigger] after.customer_routes.at(key, minor, measure) == 0 by {
        lemma_route_change_support(keys, after.nations, after.regions, after.dictionary, key, minor, measure);
        lemma_route_change_support(keys, before.nations, before.regions, after.dictionary, key, minor, measure);
    }
    assert forall|key: Major, minor: Major, measure: bool| measure || !crate::dimensions::dimension_support(key, minor, after.dictionary, 0)
        implies #[trigger] after.supplier_routes.at(key, minor, measure) == 0 by {
        lemma_route_change_support(keys, after.nations, after.regions, after.dictionary, key, minor, measure);
        lemma_route_change_support(keys, before.nations, before.regions, after.dictionary, key, minor, measure);
    }
    assert forall|key: Major, minor: Major| #[trigger] after.customer_routes.at(key, minor, false) >= 0 by {
        if crate::dimensions::dimension_support(key, minor, after.dictionary, 0) {
            lemma_database_routes_nonneg(after, db2, key.0, minor.0);
        }
    }
    assert forall|key: Major, minor: Major| #[trigger] after.supplier_routes.at(key, minor, false) >= 0 by {
        if crate::dimensions::dimension_support(key, minor, after.dictionary, 0) {
            lemma_database_routes_nonneg(after, db2, key.0, minor.0);
        }
    }
    crate::dimensions::lemma_dimension_cells(after.customer_routes, after.dictionary, 0);
    crate::dimensions::lemma_dimension_cells(after.supplier_routes, after.dictionary, 0);
}

}
