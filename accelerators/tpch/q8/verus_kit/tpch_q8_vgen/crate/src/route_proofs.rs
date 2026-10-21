use vstd::prelude::*;
use verus_common::sums::*;
use crate::dictionary::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::routing::*;
use crate::table::*;

verus! {

pub open spec fn nation_cell_shape(key: Major, cell: Cell) -> bool {
    key.1 == 0 && key.2 == 0 && cell.third == 0
}

pub open spec fn nation_shape(nations: Table) -> bool {
    forall|key: Major, index: int| 0 <= index < nations.rows(key).len() ==>
        nation_cell_shape(key, #[trigger] nations.rows(key)[index])
}

pub open spec fn route_ready(data: Data) -> bool {
    &&& data.dictionary.valid() && data.nations.valid() && data.regions.valid()
    &&& scalar(data.regions) && text_domain(data.nations, data.dictionary, 1) && text_domain(data.regions, data.dictionary, 2)
    &&& positive(data.nations) && positive(data.regions)
    &&& nation_shape(data.nations)
}

pub proof fn lemma_route_off_nation(rows: Seq<Cell>, regions: Table, nation: i64, sign: int, key: Major, minor: Major, measure: bool)
    requires key != (nation, 0, 0),
    ensures customer_nation_effect(rows, regions, nation, sign, key, minor, measure) == 0,
        supplier_nation_effect(rows, nation, sign, key, minor, measure) == 0,
{
    hide(sum);
    reveal(customer_nation_effect);
    reveal(supplier_nation_effect);
    let weight = |row: Cell| region_effect(regions.rows((row.second, 0, 0)), nation, sign * row.count@, key, minor, measure);
    assert forall|row: Cell| #[trigger] weight(row) == 0 by {
        reveal(region_effect);
        lemma_sum_zero(regions.rows((row.second, 0, 0)), |region: Cell| if key == (nation, 0, 0)
            && minor == (region.first, 0, 0) && !measure { sign * row.count@ * region.count@ } else { 0int });
    }
    lemma_sum_zero(rows, weight);
    lemma_sum_zero(rows, |row: Cell| if key == (nation, 0, 0) && minor == (row.first, 0, 0) && !measure { sign * row.count@ } else { 0int });
}

pub proof fn lemma_route_support(nations: Table, regions: Table, dictionary: Dictionary, nation: i64, sign: int, key: Major, minor: Major, measure: bool)
    requires text_domain(nations, dictionary, 1), text_domain(regions, dictionary, 2),
    ensures (measure || !crate::dimensions::dimension_support(key, minor, dictionary, 5)) ==> customer_nation_effect(nations.rows((nation, 0, 0)), regions, nation, sign, key, minor, measure) == 0,
        (measure || !crate::dimensions::dimension_support(key, minor, dictionary, 6)) ==> supplier_nation_effect(nations.rows((nation, 0, 0)), nation, sign, key, minor, measure) == 0,
{
    hide(sum);
    reveal(customer_nation_effect);
    reveal(supplier_nation_effect);
    if (measure || !crate::dimensions::dimension_support(key, minor, dictionary, 5)) {
    let customer_weight = |row: Cell| region_effect(regions.rows((row.second, 0, 0)), nation, sign * row.count@, key, minor, measure);
    assert forall|row: Cell| #[trigger] customer_weight(row) == 0 by {
        reveal(region_effect);
        let rows = regions.rows((row.second, 0, 0));
        let weight = |region: Cell| if key == (nation, 0, 0) && minor == (region.first, 0, 0) && !measure {
            sign * row.count@ * region.count@
        } else { 0int };
        assert forall|index: int| 0 <= index < rows.len() implies weight(#[trigger] rows[index]) == 0 by {
            assert(dictionary.regions.known(rows[index].first));
        }
        lemma_sum_zero(rows, weight);
    }
    lemma_sum_zero(nations.rows((nation, 0, 0)), customer_weight);
    }
    if (measure || !crate::dimensions::dimension_support(key, minor, dictionary, 6)) {
    let rows = nations.rows((nation, 0, 0));
    let supplier_weight = |row: Cell| if key == (nation, 0, 0) && minor == (row.first, 0, 0) && !measure {
        sign * row.count@
    } else { 0int };
    assert forall|index: int| 0 <= index < rows.len() implies supplier_weight(#[trigger] rows[index]) == 0 by {
        assert(dictionary.names.known(rows[index].first));
    }
    lemma_sum_zero(rows, supplier_weight);
    }
}

pub proof fn lemma_all_route_support(nations: Table, regions: Table, dictionary: Dictionary, key: Major, minor: Major, measure: bool)
    requires text_domain(nations, dictionary, 1), text_domain(regions, dictionary, 2),
    ensures (measure || !crate::dimensions::dimension_support(key, minor, dictionary, 5)) ==> all_customer_routes(nations.keys_view(), nations, regions, key, minor, measure) == 0,
        (measure || !crate::dimensions::dimension_support(key, minor, dictionary, 6)) ==> all_supplier_routes(nations.keys_view(), nations, key, minor, measure) == 0,
{
    hide(sum);
    reveal(all_customer_routes);
    reveal(all_supplier_routes);
    let keys = nations.keys_view();
    let customer_weight = |entry: Major| customer_nation_effect(nations.rows((entry.0, 0, 0)), regions, entry.0, 1, key, minor, measure);
    let supplier_weight = |entry: Major| supplier_nation_effect(nations.rows((entry.0, 0, 0)), entry.0, 1, key, minor, measure);
    if (measure || !crate::dimensions::dimension_support(key, minor, dictionary, 5)) {
        assert forall|index: int| 0 <= index < keys.len() implies customer_weight(#[trigger] keys[index]) == 0 by {
            lemma_route_support(nations, regions, dictionary, keys[index].0, 1, key, minor, measure);
        }
        lemma_sum_zero(keys, customer_weight);
    }
    if (measure || !crate::dimensions::dimension_support(key, minor, dictionary, 6)) {
        assert forall|index: int| 0 <= index < keys.len() implies supplier_weight(#[trigger] keys[index]) == 0 by {
            lemma_route_support(nations, regions, dictionary, keys[index].0, 1, key, minor, measure);
        }
        lemma_sum_zero(keys, supplier_weight);
    }
}

pub proof fn lemma_database_routes_nonneg(data: Data, db: Db, nation: i64, name: i64)
    requires route_meaning(data, db),
    ensures data.customer_routes.at((nation, 0, 0), (name, 0, 0), false) >= 0,
        data.supplier_routes.at((nation, 0, 0), (name, 0, 0), false) >= 0,
{
    hide(sum);
    reveal(route_meaning);
    let dictionary = data.dictionary;
    let customer_weight = |row: NationRow| if row.n_nationkey == nation as int {
        sum(db.region, |other: RegionRow| if row.n_regionkey == other.r_regionkey
            && text_matches(dictionary, 2, name, other.r_name) { 1int } else { 0int })
    } else { 0int };
    assert forall|index: int| 0 <= index < db.nation.len() implies customer_weight(#[trigger] db.nation[index]) >= 0 by {
        let row = db.nation[index];
        lemma_sum_nonneg(db.region, |other: RegionRow| if row.n_regionkey == other.r_regionkey
            && text_matches(dictionary, 2, name, other.r_name) { 1int } else { 0int });
    }
    lemma_sum_nonneg(db.nation, customer_weight);
    lemma_sum_nonneg(db.nation, |row: NationRow| if row.n_nationkey == nation as int
        && text_matches(dictionary, 1, name, row.n_name) { 1int } else { 0int });
}

pub proof fn lemma_build_route_cells(data: Data, db: Db)
    requires route_ready(data), route_meaning(data, db), data.customer_routes.valid(), data.supplier_routes.valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.customer_routes.at(key, minor, measure)
            == all_customer_routes(data.nations.keys_view(), data.nations, data.regions, key, minor, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.supplier_routes.at(key, minor, measure)
            == all_supplier_routes(data.nations.keys_view(), data.nations, key, minor, measure),
    ensures positive(data.customer_routes), positive(data.supplier_routes), scalar(data.customer_routes), scalar(data.supplier_routes),
        text_domain(data.customer_routes, data.dictionary, 2), text_domain(data.supplier_routes, data.dictionary, 1),
{
    hide(sum);
    assert forall|key: Major, minor: Major, measure: bool| measure || !crate::dimensions::dimension_support(key, minor, data.dictionary, 5)
        implies #[trigger] data.customer_routes.at(key, minor, measure) == 0 by {
        lemma_all_route_support(data.nations, data.regions, data.dictionary, key, minor, measure);
    }
    assert forall|key: Major, minor: Major, measure: bool| measure || !crate::dimensions::dimension_support(key, minor, data.dictionary, 6)
        implies #[trigger] data.supplier_routes.at(key, minor, measure) == 0 by {
        lemma_all_route_support(data.nations, data.regions, data.dictionary, key, minor, measure);
    }
    assert forall|key: Major, minor: Major| #[trigger] data.customer_routes.at(key, minor, false) >= 0 by {
        if crate::dimensions::dimension_support(key, minor, data.dictionary, 5) {
            lemma_database_routes_nonneg(data, db, key.0, minor.0);
        }
    }
    assert forall|key: Major, minor: Major| #[trigger] data.supplier_routes.at(key, minor, false) >= 0 by {
        if crate::dimensions::dimension_support(key, minor, data.dictionary, 6) {
            lemma_database_routes_nonneg(data, db, key.0, minor.0);
        }
    }
    crate::dimensions::lemma_dimension_cells(data.customer_routes, data.dictionary, 5);
    crate::dimensions::lemma_dimension_cells(data.supplier_routes, data.dictionary, 6);
}

pub proof fn lemma_all_routes(nations: Table, regions: Table, key: Major, minor: Major, measure: bool)
    requires nations.valid(), key.1 == 0, key.2 == 0,
        nation_shape(nations),
    ensures all_customer_routes(nations.keys_view(), nations, regions, key, minor, measure)
        == customer_nation_effect(nations.rows(key), regions, key.0, 1, key, minor, measure),
        all_supplier_routes(nations.keys_view(), nations, key, minor, measure)
        == supplier_nation_effect(nations.rows(key), key.0, 1, key, minor, measure),
{
    hide(sum);
    lemma_keys_complete(nations);
    lemma_keys_unique(nations);
    reveal(all_customer_routes);
    reveal(all_supplier_routes);
    let keys = nations.keys_view();
    let customer_value = customer_nation_effect(nations.rows(key), regions, key.0, 1, key, minor, measure);
    let supplier_value = supplier_nation_effect(nations.rows(key), key.0, 1, key, minor, measure);
    let customer_raw = |entry: Major| customer_nation_effect(nations.rows((entry.0, 0, 0)), regions, entry.0, 1, key, minor, measure);
    let supplier_raw = |entry: Major| supplier_nation_effect(nations.rows((entry.0, 0, 0)), entry.0, 1, key, minor, measure);
    let customer_selected = |entry: Major| if entry == key { customer_value } else { 0int };
    let supplier_selected = |entry: Major| if entry == key { supplier_value } else { 0int };
    assert forall|index: int| 0 <= index < keys.len() implies customer_raw(#[trigger] keys[index]) == customer_selected(keys[index])
        && supplier_raw(keys[index]) == supplier_selected(keys[index]) by {
        let entry = keys[index];
        assert(keys.contains(entry));
        assert(nations.rows(entry).len() > 0);
        assert((nations.rows(entry)[0]).third == 0);
        assert(entry.1 == 0 && entry.2 == 0);
        if entry != key { lemma_route_off_nation(nations.rows(entry), regions, entry.0, 1, key, minor, measure); }
    }
    lemma_sum_ext(keys, customer_raw, customer_selected);
    lemma_sum_ext(keys, supplier_raw, supplier_selected);
    if !keys.contains(key) {
        assert(nations.rows(key).len() == 0);
        reveal(customer_nation_effect);
        reveal(supplier_nation_effect);
        lemma_sum_empty(nations.rows(key), |row: Cell| region_effect(regions.rows((row.second, 0, 0)), key.0, 1 * row.count@, key, minor, measure));
        lemma_sum_empty(nations.rows(key), |row: Cell| if key == (key.0, 0, 0) && minor == (row.first, 0, 0) && !measure { 1 * row.count@ } else { 0int });
    }
    lemma_sum_indicator_distinct(keys, key, customer_value, customer_selected);
    lemma_sum_indicator_distinct(keys, key, supplier_value, supplier_selected);
}

pub proof fn lemma_build_routes(data: Data, db: Db)
    requires dimension_meaning(data, db), route_ready(data),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.customer_routes.at(key, minor, measure)
            == all_customer_routes(data.nations.keys_view(), data.nations, data.regions, key, minor, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] data.supplier_routes.at(key, minor, measure)
            == all_supplier_routes(data.nations.keys_view(), data.nations, key, minor, measure),
    ensures route_meaning(data, db),
{
    hide(sum);
    reveal(route_meaning);
    reveal(route_ready);
    assert forall|nation: i64, name: i64| #[trigger] data.customer_routes.at((nation, 0, 0), (name, 0, 0), false)
        == sum(db.nation, |row: NationRow| if row.n_nationkey == nation as int {
            sum(db.region, |other: RegionRow| if row.n_regionkey == other.r_regionkey
                && text_matches(data.dictionary, 2, name, other.r_name) { 1int } else { 0int })
        } else { 0int }) by {
        lemma_all_routes(data.nations, data.regions, (nation, 0, 0), (name, 0, 0), false);
        lemma_customer_nation(data, db, nation, name);
    }
    assert forall|nation: i64, name: i64| #[trigger] data.supplier_routes.at((nation, 0, 0), (name, 0, 0), false)
        == sum(db.nation, |row: NationRow| if row.n_nationkey == nation as int && text_matches(data.dictionary, 1, name, row.n_name) { 1int } else { 0int }) by {
        lemma_all_routes(data.nations, data.regions, (nation, 0, 0), (name, 0, 0), false);
        lemma_supplier_nation(data, db, nation, (nation, 0, 0), (name, 0, 0), 1, false);
    }
}

pub proof fn lemma_nation_projection(data: Data, db: Db, nation: i64, weight: spec_fn((int, int)) -> int)
    requires dimension_meaning(data, db), route_ready(data),
    ensures sum(data.nations.rows((nation, 0, 0)), |cell: Cell| cell.count@ * weight((cell.first as int, cell.second as int)))
        == sum(db.nation, |row: NationRow| if row.n_nationkey == nation as int {
            weight((data.dictionary.names.text_code(row.n_name) as int, row.n_regionkey))
        } else { 0int }),
{
    hide(sum);
    reveal(dimension_meaning);
    let rows = data.nations.rows((nation, 0, 0));
    let left_project = |cell: Cell| (cell.first as int, cell.second as int);
    let right_project = |row: NationRow| (data.dictionary.names.text_code(row.n_name) as int, row.n_regionkey);
    let left_count = |cell: Cell| cell.count@;
    let right_count = |row: NationRow| if row.n_nationkey == nation as int { 1int } else { 0int };
    assert forall|class: (int, int)| #[trigger] verus_prooflib::class_total(rows, left_project, left_count, class)
        == verus_prooflib::class_total(db.nation, right_project, right_count, class) by {
        reveal(verus_prooflib::class_total);
        let left = |cell: Cell| if left_project(cell) == class { cell.count@ } else { 0int };
        let right = |row: NationRow| if right_project(row) == class { right_count(row) } else { 0int };
        lemma_sum_ext_all(rows, |cell: Cell| if left_project(cell) == class { left_count(cell) } else { 0int }, left);
        lemma_sum_ext_all(db.nation, |row: NationRow| if right_project(row) == class { right_count(row) } else { 0int }, right);
        if i64::MIN <= class.0 <= i64::MAX && i64::MIN <= class.1 <= i64::MAX {
            let minor = (class.0 as i64, class.1 as i64, 0i64);
            assert forall|index: int| 0 <= index < rows.len() implies left(#[trigger] rows[index]) == cell_weight(rows[index], minor, false) by {
                assert(rows[index].third == 0);
            }
            lemma_sum_ext(rows, left, |cell: Cell| cell_weight(cell, minor, false));
            reveal(Table::at);
            reveal(cell_sum);
            let canonical = |row: NationRow| if row.n_nationkey == nation as int && row.n_regionkey == class.1
                && text_matches(data.dictionary, 1, class.0 as i64, row.n_name) { 1int } else { 0int };
            assert forall|index: int| 0 <= index < db.nation.len() implies right(#[trigger] db.nation[index]) == canonical(db.nation[index]) by {
                lemma_text_code(data.dictionary, 1, db.nation[index].n_name);
            }
            lemma_sum_ext(db.nation, right, canonical);
            assert(data.nations.at((nation, 0, 0), minor, false) == sum(db.nation, canonical));
        } else {
            lemma_sum_zero(rows, left);
            assert forall|index: int| 0 <= index < db.nation.len() implies right(#[trigger] db.nation[index]) == 0 by {
                lemma_text_code(data.dictionary, 1, db.nation[index].n_name);
                assert(nation_represented(db.nation[index]));
            }
            lemma_sum_zero(db.nation, right);
        }
    }
    verus_prooflib::lemma_equal_classes(rows, db.nation, left_project, right_project, left_count, right_count, weight);
    lemma_sum_ext_all(rows, |cell: Cell| left_count(cell) * weight(left_project(cell)),
        |cell: Cell| cell.count@ * weight((cell.first as int, cell.second as int)));
    lemma_sum_ext_all(db.nation, |row: NationRow| right_count(row) * weight(right_project(row)),
        |row: NationRow| if row.n_nationkey == nation as int { weight(right_project(row)) } else { 0int });
    lemma_sum_ext_all(db.nation, |row: NationRow| if row.n_nationkey == nation as int { weight(right_project(row)) } else { 0int },
        |row: NationRow| if row.n_nationkey == nation as int { weight((data.dictionary.names.text_code(row.n_name) as int, row.n_regionkey)) } else { 0int });
}

pub proof fn lemma_region_factor(regions: Table, regionkey: i64, nation: i64, count: int, key: Major, minor: Major, measure: bool)
    requires scalar(regions), key == (nation, 0, 0), minor.1 == 0, minor.2 == 0, !measure,
    ensures region_effect(regions.rows((regionkey, 0, 0)), nation, count, key, minor, measure)
        == count * regions.at((regionkey, 0, 0), minor, measure),
{
    hide(sum);
    reveal(region_effect);
    let rows = regions.rows((regionkey, 0, 0));
    let raw = |region: Cell| if key == (nation, 0, 0) && minor == (region.first, 0, 0) && !measure { count * region.count@ } else { 0int };
    let weight = |cell: Cell| cell_weight(cell, minor, measure);
    let scaled = |cell: Cell| count * weight(cell);
    assert forall|index: int| 0 <= index < rows.len() implies raw(#[trigger] rows[index]) == scaled(rows[index]) by {
        assert(rows[index].second == 0 && rows[index].third == 0);
    }
    lemma_sum_ext(rows, raw, scaled);
    lemma_sum_scale(rows, weight, scaled, count);
    reveal(Table::at);
    reveal(cell_sum);
    assert(region_effect(rows, nation, count, key, minor, measure) == sum(rows, raw));
    assert(regions.at((regionkey, 0, 0), minor, measure) == sum(rows, weight));
}

pub proof fn lemma_customer_nation(data: Data, db: Db, nation: i64, name: i64)
    requires dimension_meaning(data, db), route_ready(data),
    ensures customer_nation_effect(data.nations.rows((nation, 0, 0)), data.regions, nation, 1,
        (nation, 0, 0), (name, 0, 0), false)
        == sum(db.nation, |row: NationRow| if row.n_nationkey == nation as int {
            sum(db.region, |other: RegionRow| if row.n_regionkey == other.r_regionkey
                && text_matches(data.dictionary, 2, name, other.r_name) { 1int } else { 0int })
        } else { 0int }),
{
    hide(sum);
    reveal(dimension_meaning);
    reveal(customer_nation_effect);
    let rows = data.nations.rows((nation, 0, 0));
    let raw = |cell: Cell| region_effect(data.regions.rows((cell.second, 0, 0)), nation, cell.count@,
        (nation, 0, 0), (name, 0, 0), false);
    let weight = |class: (int, int)| data.regions.at((class.1 as i64, 0, 0), (name, 0, 0), false);
    let weighted = |cell: Cell| cell.count@ * weight((cell.first as int, cell.second as int));
    assert forall|index: int| 0 <= index < rows.len() implies raw(#[trigger] rows[index]) == weighted(rows[index]) by {
        let cell = rows[index];
        lemma_region_factor(data.regions, cell.second, nation, cell.count@, (nation, 0, 0), (name, 0, 0), false);
    }
    lemma_sum_ext(rows, raw, weighted);
    lemma_sum_ext_all(rows, |cell: Cell| region_effect(data.regions.rows((cell.second, 0, 0)), nation, 1 * cell.count@,
        (nation, 0, 0), (name, 0, 0), false), raw);
    lemma_nation_projection(data, db, nation, weight);
    let projected = |row: NationRow| if row.n_nationkey == nation as int {
        weight((data.dictionary.names.text_code(row.n_name) as int, row.n_regionkey))
    } else { 0int };
    let database = |row: NationRow| if row.n_nationkey == nation as int {
        sum(db.region, |other: RegionRow| if row.n_regionkey == other.r_regionkey
            && text_matches(data.dictionary, 2, name, other.r_name) { 1int } else { 0int })
    } else { 0int };
    assert forall|index: int| 0 <= index < db.nation.len() implies projected(#[trigger] db.nation[index]) == database(db.nation[index]) by {
        let row = db.nation[index];
        assert(nation_represented(row));
        let canonical = |other: RegionRow| if other.r_regionkey == row.n_regionkey as i64 as int
            && text_matches(data.dictionary, 2, name, other.r_name) { 1int } else { 0int };
        lemma_sum_ext_all(db.region, canonical, |other: RegionRow| if row.n_regionkey == other.r_regionkey
            && text_matches(data.dictionary, 2, name, other.r_name) { 1int } else { 0int });
    }
    lemma_sum_ext(db.nation, projected, database);
}

pub proof fn lemma_supplier_nation(data: Data, db: Db, nation: i64, key: Major, minor: Major, sign: int, measure: bool)
    requires dimension_meaning(data, db), route_ready(data), key == (nation, 0, 0), minor.1 == 0, minor.2 == 0, sign == 1, !measure,
    ensures supplier_nation_effect(data.nations.rows((nation, 0, 0)), nation, sign, key, minor, measure)
        == sum(db.nation, |row: NationRow| if row.n_nationkey == nation as int && text_matches(data.dictionary, 1, minor.0, row.n_name) { 1int } else { 0int }),
{
    hide(sum);
    reveal(dimension_meaning);
    reveal(supplier_nation_effect);
    let weight = |class: (int, int)| if class.0 == minor.0 as int { 1int } else { 0int };
    lemma_nation_projection(data, db, nation, weight);
    lemma_sum_ext_all(data.nations.rows((nation, 0, 0)),
        |row: Cell| if key == (nation, 0, 0) && minor == (row.first, 0, 0) && !measure { sign * row.count@ } else { 0int },
        |cell: Cell| if cell.first == minor.0 { cell.count@ } else { 0int });
    lemma_sum_ext_all(data.nations.rows((nation, 0, 0)),
        |cell: Cell| if cell.first == minor.0 { cell.count@ } else { 0int },
        |cell: Cell| cell.count@ * weight((cell.first as int, cell.second as int)));
    let projected = |row: NationRow| if row.n_nationkey == nation as int {
        weight((data.dictionary.names.text_code(row.n_name) as int, row.n_regionkey))
    } else { 0int };
    let database = |row: NationRow| if row.n_nationkey == nation as int && text_matches(data.dictionary, 1, minor.0, row.n_name) { 1int } else { 0int };
    assert forall|index: int| 0 <= index < db.nation.len() implies projected(#[trigger] db.nation[index]) == database(db.nation[index]) by {
        lemma_text_code(data.dictionary, 1, db.nation[index].n_name);
    }
    lemma_sum_ext(db.nation, projected, database);
}

}
