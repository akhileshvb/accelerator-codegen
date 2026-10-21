use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::*;
use crate::gen_spec::*;
use crate::dictionary::*;
use crate::model::*;
use crate::table::*;
use crate::routing::*;
use crate::query_product::*;
use crate::query_core::*;

verus! {

pub proof fn lemma_sql_statistics(data: Data, db: Db, region: i64, kind: i64, name: i64, year: i64, numerator: bool, volume: bool)
    requires dimension_meaning(data, db), line_domain(db), base_meaning(data, db), route_meaning(data, db), aggregate_ready(data),
        data.dictionary.regions.known(region), data.dictionary.types.known(kind), numerator ==> data.dictionary.names.known(name),
    ensures routed_effect(data.base.keys_view(), data.base, data.customer_routes, data.supplier_routes, 1,
        (region, kind, if numerator { name } else { 0 }), (year, 0, 0), numerator, volume) ==
        group_sum(db, Params { p1: if numerator { data.dictionary.names.text(name) } else { Seq::empty() },
            p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) }, year as int,
            query_weight(Params { p1: if numerator { data.dictionary.names.text(name) } else { Seq::empty() },
                p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) }, numerator, volume)),
{
    hide(group_sum);
    if year == 1995 || year == 1996 {
        lemma_query_routing(data, db, region, kind, name, year, numerator, volume);
    } else {
        let parameters = Params { p1: if numerator { data.dictionary.names.text(name) } else { Seq::empty() },
            p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) };
        crate::sql_bridge::lemma_missing_group(data, db, parameters, year as int, query_weight(parameters, numerator, volume));
        crate::aggregate_cells::lemma_routed_support(data, data.base.keys_view(), (region, kind, if numerator { name } else { 0 }), (year, 0, 0), numerator, volume);
    }
}

pub proof fn lemma_aggregate_meaning(data: Data, db: Db)
    requires dimension_meaning(data, db), line_domain(db), base_meaning(data, db), route_meaning(data, db), aggregate_ready(data), routed_meaning(data),
    ensures aggregate_meaning(data, db),
{
    hide(group_sum);
    hide(group_count);
    reveal(routed_meaning);
    reveal(aggregate_meaning);
    assert forall|region: i64, kind: i64, year: i64, volume: bool| #[trigger] data.totals.at((region, kind, 0), (year, 0, 0), volume) ==
        if data.dictionary.regions.known(region) && data.dictionary.types.known(kind) {
            let parameters = Params { p1: Seq::empty(), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) };
            if volume { group_sum(db, parameters, year as int, |tuple: Tuple| tuple.all_nations.volume) } else { group_count(db, parameters, year as int) }
        } else { 0int } by {
        if data.dictionary.regions.known(region) && data.dictionary.types.known(kind) {
            lemma_sql_statistics(data, db, region, kind, 0, year, false, volume);
            let parameters = Params { p1: Seq::empty(), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) };
            if volume {
                crate::sql_bridge::lemma_group_weights(db, parameters, year as int, query_weight(parameters, false, volume), |tuple: Tuple| tuple.all_nations.volume);
            } else {
                crate::sql_bridge::lemma_group_weights(db, parameters, year as int, query_weight(parameters, false, volume), |tuple: Tuple| 1int);
                crate::sql_bridge::lemma_count_sum(db, parameters, year as int);
            }
        } else {
            crate::aggregate_cells::lemma_routed_support(data, data.base.keys_view(), (region, kind, 0), (year, 0, 0), false, volume);
        }
    }
    assert forall|region: i64, kind: i64, name: i64, year: i64, volume: bool| #[trigger] data.numerators.at((region, kind, name), (year, 0, 0), volume) ==
        if data.dictionary.regions.known(region) && data.dictionary.types.known(kind) && data.dictionary.names.known(name) {
            let parameters = Params { p1: data.dictionary.names.text(name), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) };
            group_sum(db, parameters, year as int, numerator_weight(parameters, volume))
        } else { 0int } by {
        if data.dictionary.regions.known(region) && data.dictionary.types.known(kind) && data.dictionary.names.known(name) {
            lemma_sql_statistics(data, db, region, kind, name, year, true, volume);
            let parameters = Params { p1: data.dictionary.names.text(name), p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) };
            crate::sql_bridge::lemma_group_weights(db, parameters, year as int, query_weight(parameters, true, volume), numerator_weight(parameters, volume));
        } else {
            crate::aggregate_cells::lemma_routed_support(data, data.base.keys_view(), (region, kind, name), (year, 0, 0), true, volume);
        }
    }
}

pub proof fn lemma_build_aggregates(data: Data, db: Db)
    requires dimension_meaning(data, db), line_domain(db), base_meaning(data, db), route_meaning(data, db), aggregate_ready(data), routed_meaning(data),
        data.totals.valid(), data.numerators.valid(),
    ensures aggregate_meaning(data, db), positive(data.totals), positive(data.numerators),
        forall|key: Major, index: int| 0 <= index < data.totals.rows(key).len() ==>
            crate::aggregate_cells::output_support(key, minor_of(#[trigger] data.totals.rows(key)[index]), data.dictionary, false),
        forall|key: Major, index: int| 0 <= index < data.numerators.rows(key).len() ==>
            crate::aggregate_cells::output_support(key, minor_of(#[trigger] data.numerators.rows(key)[index]), data.dictionary, true),
{
    reveal(routed_meaning);
    crate::aggregate_cells::lemma_output_cells(data.totals, data, false);
    crate::aggregate_cells::lemma_output_cells(data.numerators, data, true);
    lemma_aggregate_meaning(data, db);
}

pub open spec fn region_database_count(db: Db, nation: NationRow, parameters: Params) -> int {
    sum(db.region, |region: RegionRow| if nation.n_regionkey == region.r_regionkey && region.r_name == parameters.p2 { 1int } else { 0int })
}

pub open spec fn supplier_database_count(db: Db, supplier: i64, parameters: Params, numerator: bool) -> int {
    sum(db.nation, |nation: NationRow| if nation.n_nationkey == supplier as int && (!numerator || nation.n_name == parameters.p1) { 1int } else { 0int })
}

pub open spec fn route_project(route: RouteTuple) -> Major {
    (route.n1.n_nationkey as i64, route.n2.n_nationkey as i64, 0)
}

pub open spec fn route_multiplicity(parameters: Params, numerator: bool) -> spec_fn(RouteTuple) -> int {
    |route: RouteTuple| if route_active(route, parameters, numerator) { 1int } else { 0int }
}

pub open spec fn normalized_route_count(pair: Major, parameters: Params, numerator: bool) -> spec_fn(RouteTuple) -> int {
    |route: RouteTuple| if pair.2 == 0 && route.n1.n_nationkey == pair.0 as int && route.n2.n_nationkey == pair.1 as int
        && route_active(route, parameters, numerator) { 1int } else { 0int }
}

pub proof fn lemma_route_region_count(db: Db, first: NationRow, second: NationRow, pair: Major, parameters: Params, numerator: bool)
    ensures route_sum_2(db, first, second, normalized_route_count(pair, parameters, numerator)) ==
        (if pair.2 == 0 && first.n_nationkey == pair.0 as int && second.n_nationkey == pair.1 as int
            && (!numerator || second.n_name == parameters.p1) { 1int } else { 0int }) * region_database_count(db, first, parameters),
{
    hide(sum);
    reveal(route_sum_2);
    reveal(route_sum_3);
    let coefficient = if pair.2 == 0 && first.n_nationkey == pair.0 as int && second.n_nationkey == pair.1 as int
        && (!numerator || second.n_name == parameters.p1) { 1int } else { 0int };
    let count = |region: RegionRow| if first.n_regionkey == region.r_regionkey && region.r_name == parameters.p2 { 1int } else { 0int };
    let raw = |region: RegionRow| route_sum_3(db, first, second, region, normalized_route_count(pair, parameters, numerator));
    assert forall|region: RegionRow| #[trigger] raw(region) == coefficient * count(region) by {}
    lemma_sum_scale(db.region, count, raw, coefficient);
}

pub proof fn lemma_route_nation_count(db: Db, first: NationRow, pair: Major, parameters: Params, numerator: bool)
    ensures route_sum_1(db, first, normalized_route_count(pair, parameters, numerator)) ==
        (if pair.2 == 0 && first.n_nationkey == pair.0 as int { region_database_count(db, first, parameters) } else { 0int })
            * supplier_database_count(db, pair.1, parameters, numerator),
{
    hide(sum);
    reveal(route_sum_1);
    let coefficient = if pair.2 == 0 && first.n_nationkey == pair.0 as int { region_database_count(db, first, parameters) } else { 0int };
    let count = |second: NationRow| if second.n_nationkey == pair.1 as int && (!numerator || second.n_name == parameters.p1) { 1int } else { 0int };
    let raw = |second: NationRow| route_sum_2(db, first, second, normalized_route_count(pair, parameters, numerator));
    let scaled = |second: NationRow| coefficient * count(second);
    assert forall|second: NationRow| #[trigger] raw(second) == scaled(second) by {
        lemma_route_region_count(db, first, second, pair, parameters, numerator);
    }
    lemma_sum_ext_all(db.nation, raw, scaled);
    lemma_sum_scale(db.nation, count, scaled, coefficient);
}

pub proof fn lemma_route_total_count(data: Data, db: Db, pair: Major, parameters: Params, region: i64, name: i64, numerator: bool)
    requires dimension_meaning(data, db), route_meaning(data, db), aggregate_ready(data),
        data.dictionary.regions.known(region), data.dictionary.regions.text(region) == parameters.p2,
        numerator ==> data.dictionary.names.known(name) && data.dictionary.names.text(name) == parameters.p1,
    ensures route_sum_0(db, normalized_route_count(pair, parameters, numerator)) == route_coefficient(data, pair, region, name, numerator),
{
    hide(sum);
    reveal(route_sum_0);
    let supplier = supplier_database_count(db, pair.1, parameters, numerator);
    let count = |first: NationRow| if pair.2 == 0 && first.n_nationkey == pair.0 as int { region_database_count(db, first, parameters) } else { 0int };
    let raw = |first: NationRow| route_sum_1(db, first, normalized_route_count(pair, parameters, numerator));
    let scaled = |first: NationRow| supplier * count(first);
    assert forall|first: NationRow| #[trigger] raw(first) == scaled(first) by {
        lemma_route_nation_count(db, first, pair, parameters, numerator);
        assert(count(first) * supplier == supplier * count(first)) by (nonlinear_arith);
    }
    lemma_sum_ext_all(db.nation, raw, scaled);
    lemma_sum_scale(db.nation, count, scaled, supplier);
    if pair.2 == 0 {
        reveal(route_meaning);
        let canonical = |first: NationRow| if first.n_nationkey == pair.0 as int {
            sum(db.region, |other: RegionRow| if first.n_regionkey == other.r_regionkey && text_matches(data.dictionary, 2, region, other.r_name) { 1int } else { 0int })
        } else { 0int };
        assert forall|first: NationRow| #[trigger] count(first) == canonical(first) by {
            lemma_sum_ext_all(db.region,
                |other: RegionRow| if first.n_regionkey == other.r_regionkey && other.r_name == parameters.p2 { 1int } else { 0int },
                |other: RegionRow| if first.n_regionkey == other.r_regionkey && text_matches(data.dictionary, 2, region, other.r_name) { 1int } else { 0int });
        }
        lemma_sum_ext_all(db.nation, count, canonical);
        assert(data.customer_routes.at((pair.0, 0, 0), (region, 0, 0), false) == sum(db.nation, canonical));
        lemma_supplier_filter(data, db, pair.1, parameters, name, numerator);
        let customer = data.customer_routes.at((pair.0, 0, 0), (region, 0, 0), false);
        assert(supplier * customer == customer * supplier) by (nonlinear_arith);
    } else { lemma_sum_zero(db.nation, count); }
}

pub proof fn lemma_route_class(data: Data, db: Db, pair: Major, parameters: Params, region: i64, name: i64, numerator: bool)
    requires dimension_meaning(data, db), route_meaning(data, db), aggregate_ready(data),
        data.dictionary.regions.known(region), data.dictionary.regions.text(region) == parameters.p2,
        numerator ==> data.dictionary.names.known(name) && data.dictionary.names.text(name) == parameters.p1,
    ensures class_total(route_rows_0(db), |route: RouteTuple| route_project(route), route_multiplicity(parameters, numerator), pair) ==
        route_coefficient(data, pair, region, name, numerator),
{
    hide(sum);
    let rows = route_rows_0(db);
    let multiplicity = route_multiplicity(parameters, numerator);
    let normalized = normalized_route_count(pair, parameters, numerator);
    assert forall|index: int| 0 <= index < rows.len() implies normalized(#[trigger] rows[index]) ==
        if route_project(rows[index]) == pair { multiplicity(rows[index]) } else { 0int } by {
        assert(rows.contains(rows[index]));
        lemma_route_member_0(db, rows[index]);
        lemma_route_bounds(data, db, rows[index]);
    }
    lemma_class_total_weight(rows, |route: RouteTuple| route_project(route), multiplicity, pair, normalized);
    lemma_route_sum_0(db, normalized);
    lemma_route_total_count(data, db, pair, parameters, region, name, numerator);
}

pub proof fn lemma_entry_routing(data: Data, region: i64, kind: i64, name: i64, year: i64, numerator: bool, volume: bool)
    requires aggregate_ready(data),
    ensures routed_effect(data.base.keys_view(), data.base, data.customer_routes, data.supplier_routes, 1,
        (region, kind, if numerator { name } else { 0 }), (year, 0, 0), numerator, volume) ==
        sum(base_entries(data.base), |entry: (Major, Cell)| cell_weight(entry.1, (kind, year, 0), volume) * route_coefficient(data, entry.0, region, name, numerator)),
{
    hide(sum);
    reveal(base_entries);
    reveal(routed_effect);
    let key = (region, kind, if numerator { name } else { 0 });
    let target = |entry: (Major, Cell)| cell_weight(entry.1, (kind, year, 0), volume) * route_coefficient(data, entry.0, region, name, numerator);
    let expand = |pair: Major| data.base.rows(pair).map_values(|cell: Cell| (pair, cell));
    lemma_sum_flat_map(data.base.keys_view(), expand, target);
    let raw = |pair: Major| route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, (year, 0, 0), numerator, volume);
    let expected = |pair: Major| sum(expand(pair), target);
    assert forall|pair: Major| #[trigger] raw(pair) == expected(pair) by {
        lemma_routed_pair(data, pair, key, year, numerator, volume);
        let mapped = |cell: Cell| (pair, cell);
        let count = |cell: Cell| cell_weight(cell, (kind, year, 0), volume);
        let coefficient = route_coefficient(data, pair, region, name, numerator);
        let scaled = |cell: Cell| coefficient * count(cell);
        let row_weight = |cell: Cell| count(cell) * coefficient;
        lemma_sum_map(data.base.rows(pair), mapped, target, row_weight);
        assert forall|cell: Cell| #[trigger] row_weight(cell) == scaled(cell) by {
            assert(count(cell) * coefficient == coefficient * count(cell)) by (nonlinear_arith);
        }
        lemma_sum_ext_all(data.base.rows(pair), row_weight, scaled);
        lemma_sum_scale(data.base.rows(pair), count, scaled, coefficient);
        reveal(Table::at);
        reveal(cell_sum);
        assert(data.base.at(pair, (kind, year, 0), volume) == sum(data.base.rows(pair), count));
        assert(coefficient * sum(data.base.rows(pair), count) == sum(data.base.rows(pair), count) * coefficient) by (nonlinear_arith);
    }
    lemma_sum_ext_all(data.base.keys_view(), raw, expected);
}

pub proof fn lemma_query_routing(data: Data, db: Db, region: i64, kind: i64, name: i64, year: i64, numerator: bool, volume: bool)
    requires dimension_meaning(data, db), line_domain(db), base_meaning(data, db), route_meaning(data, db), aggregate_ready(data),
        data.dictionary.regions.known(region), data.dictionary.types.known(kind),
        numerator ==> data.dictionary.names.known(name), year == 1995 || year == 1996,
    ensures routed_effect(data.base.keys_view(), data.base, data.customer_routes, data.supplier_routes, 1,
        (region, kind, if numerator { name } else { 0 }), (year, 0, 0), numerator, volume) ==
        group_sum(db, Params { p1: if numerator { data.dictionary.names.text(name) } else { Seq::empty() },
            p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) }, year as int,
            query_weight(Params { p1: if numerator { data.dictionary.names.text(name) } else { Seq::empty() },
                p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) }, numerator, volume)),
{
    hide(sum);
    hide(group_sum);
    let parameters = Params { p1: if numerator { data.dictionary.names.text(name) } else { Seq::empty() },
        p2: data.dictionary.regions.text(region), p3: data.dictionary.types.text(kind) };
    let weight = query_weight(parameters, numerator, volume);
    lemma_group_product(db, parameters, year as int, weight);
    let rows = route_rows_0(db);
    let per_route = |route: RouteTuple| sum(core_rows_0(db), |core: CoreTuple| joined_weight(db, core, route, parameters, year as int, weight));
    let left_project = |route: RouteTuple| route_project(route);
    let left_count = route_multiplicity(parameters, numerator);
    let left_weight = |pair: Major| data.base.at(pair, (kind, year, 0), volume);
    let right_project = |entry: (Major, Cell)| entry.0;
    let right_count = |entry: (Major, Cell)| cell_weight(entry.1, (kind, year, 0), volume);
    let right_weight = |pair: Major| route_coefficient(data, pair, region, name, numerator);
    let factored = |route: RouteTuple| left_count(route) * left_weight(left_project(route));
    assert forall|index: int| 0 <= index < rows.len() implies per_route(#[trigger] rows[index]) == factored(rows[index]) by {
        assert(rows.contains(rows[index]));
        lemma_route_member_0(db, rows[index]);
        lemma_core_base(data, db, rows[index], parameters, kind, year, numerator, volume);
    }
    lemma_sum_ext(rows, per_route, factored);
    assert forall|pair: Major| #[trigger] class_total(rows, left_project, left_count, pair) * left_weight(pair) ==
        class_total(base_entries(data.base), right_project, right_count, pair) * right_weight(pair) by {
        lemma_route_class(data, db, pair, parameters, region, name, numerator);
        lemma_base_entry_class(data.base, pair, (kind, year, 0), volume);
        assert(right_weight(pair) * left_weight(pair) == left_weight(pair) * right_weight(pair)) by (nonlinear_arith);
    }
    lemma_equal_weighted_classes(rows, base_entries(data.base), left_project, right_project, left_count, right_count, left_weight, right_weight);
    lemma_entry_routing(data, region, kind, name, year, numerator, volume);
    lemma_sum_ext_all(base_entries(data.base), |entry: (Major, Cell)| right_count(entry) * right_weight(right_project(entry)),
        |entry: (Major, Cell)| cell_weight(entry.1, (kind, year, 0), volume) * route_coefficient(data, entry.0, region, name, numerator));
}

pub open spec fn aggregate_ready(data: Data) -> bool {
    data.dictionary.valid() && data.base.valid() && data.customer_routes.valid() && data.supplier_routes.valid()
        && positive(data.base) && positive(data.customer_routes) && positive(data.supplier_routes)
        && scalar(data.customer_routes) && scalar(data.supplier_routes)
        && text_domain(data.customer_routes, data.dictionary, 2) && text_domain(data.supplier_routes, data.dictionary, 1)
        && forall|key: Major, index: int| 0 <= index < data.base.rows(key).len() ==>
            crate::base_proofs::base_support(key, minor_of(#[trigger] data.base.rows(key)[index]), data.dictionary)
}

pub open spec fn supplier_filter(data: Data, supplier: i64, name: i64, numerator: bool) -> int {
    if numerator { data.supplier_routes.at((supplier, 0, 0), (name, 0, 0), false) }
    else { sum(data.supplier_routes.rows((supplier, 0, 0)), |cell: Cell| cell.count@) }
}

pub open spec fn route_coefficient(data: Data, pair: Major, region: i64, name: i64, numerator: bool) -> int {
    if pair.2 == 0 { data.customer_routes.at((pair.0, 0, 0), (region, 0, 0), false) * supplier_filter(data, pair.1, name, numerator) }
    else { 0 }
}

#[verifier::opaque]
pub open spec fn base_entries(base: Table) -> Seq<(Major, Cell)> {
    flat(base.keys_view().map_values(|pair: Major| base.rows(pair).map_values(|cell: Cell| (pair, cell))))
}

pub proof fn lemma_base_entry_class(base: Table, pair: Major, minor: Major, volume: bool)
    requires base.valid(),
    ensures class_total(base_entries(base), |entry: (Major, Cell)| entry.0,
        |entry: (Major, Cell)| cell_weight(entry.1, minor, volume), pair) == base.at(pair, minor, volume),
{
    hide(sum);
    reveal(base_entries);
    reveal(Table::at);
    reveal(cell_sum);
    lemma_keys_unique(base);
    lemma_keys_complete(base);
    let project = |entry: (Major, Cell)| entry.0;
    let multiplicity = |entry: (Major, Cell)| cell_weight(entry.1, minor, volume);
    let weight = |entry: (Major, Cell)| if project(entry) == pair { multiplicity(entry) } else { 0int };
    let expand = |key: Major| base.rows(key).map_values(|cell: Cell| (key, cell));
    assert(base_entries(base) =~= flat(base.keys_view().map_values(expand)));
    lemma_sum_flat_map(base.keys_view(), expand, weight);
    let raw = |key: Major| sum(expand(key), weight);
    let selected = |key: Major| if key == pair { base.at(pair, minor, volume) } else { 0int };
    assert forall|key: Major| #[trigger] raw(key) == selected(key) by {
        let mapped = |cell: Cell| (key, cell);
        let row_weight = |cell: Cell| if key == pair { cell_weight(cell, minor, volume) } else { 0int };
        lemma_sum_map(base.rows(key), mapped, weight, row_weight);
        if key == pair {
            lemma_sum_ext_all(base.rows(key), row_weight, |cell: Cell| cell_weight(cell, minor, volume));
        } else { lemma_sum_zero(base.rows(key), row_weight); }
    }
    lemma_sum_ext_all(base.keys_view(), raw, selected);
    if !base.keys_view().contains(pair) {
        assert(base.rows(pair).len() == 0);
        lemma_sum_empty(base.rows(pair), |cell: Cell| cell_weight(cell, minor, volume));
    }
    lemma_sum_indicator_distinct(base.keys_view(), pair, base.at(pair, minor, volume), selected);
    lemma_class_total_weight(base_entries(base), project, multiplicity, pair, weight);
}

pub proof fn lemma_supplier_projection(data: Data, db: Db, supplier: i64, weight: spec_fn(i64) -> int)
    requires dimension_meaning(data, db), route_meaning(data, db), aggregate_ready(data),
    ensures sum(data.supplier_routes.rows((supplier, 0, 0)), |cell: Cell| cell.count@ * weight(cell.first)) ==
        sum(db.nation, |row: NationRow| if row.n_nationkey == supplier as int { weight(data.dictionary.names.text_code(row.n_name)) } else { 0int }),
{
    hide(sum);
    reveal(dimension_meaning);
    reveal(route_meaning);
    let rows = data.supplier_routes.rows((supplier, 0, 0));
    let left_project = |cell: Cell| cell.first;
    let right_project = |row: NationRow| data.dictionary.names.text_code(row.n_name);
    let left_count = |cell: Cell| cell.count@;
    let right_count = |row: NationRow| if row.n_nationkey == supplier as int { 1int } else { 0int };
    assert forall|name: i64| #[trigger] class_total(rows, left_project, left_count, name) ==
        class_total(db.nation, right_project, right_count, name) by {
        let left = |cell: Cell| if cell.first == name { cell.count@ } else { 0int };
        let right = |row: NationRow| if right_project(row) == name { right_count(row) } else { 0int };
        lemma_class_total_weight(rows, left_project, left_count, name, left);
        lemma_class_total_weight(db.nation, right_project, right_count, name, right);
        assert forall|index: int| 0 <= index < rows.len() implies left(#[trigger] rows[index]) == cell_weight(rows[index], (name, 0, 0), false) by {}
        lemma_sum_ext(rows, left, |cell: Cell| cell_weight(cell, (name, 0, 0), false));
        reveal(Table::at);
        reveal(cell_sum);
        let canonical = |row: NationRow| if row.n_nationkey == supplier as int && text_matches(data.dictionary, 1, name, row.n_name) { 1int } else { 0int };
        assert forall|index: int| 0 <= index < db.nation.len() implies right(#[trigger] db.nation[index]) == canonical(db.nation[index]) by {
            lemma_text_code(data.dictionary, 1, db.nation[index].n_name);
        }
        lemma_sum_ext(db.nation, right, canonical);
        assert(data.supplier_routes.at((supplier, 0, 0), (name, 0, 0), false) == sum(db.nation, canonical));
    }
    lemma_equal_classes(rows, db.nation, left_project, right_project, left_count, right_count, weight);
    lemma_sum_ext_all(rows, |cell: Cell| left_count(cell) * weight(left_project(cell)), |cell: Cell| cell.count@ * weight(cell.first));
    lemma_sum_ext_all(db.nation, |row: NationRow| right_count(row) * weight(right_project(row)),
        |row: NationRow| if row.n_nationkey == supplier as int { weight(data.dictionary.names.text_code(row.n_name)) } else { 0int });
}

pub proof fn lemma_supplier_filter(data: Data, db: Db, supplier: i64, parameters: Params, name: i64, numerator: bool)
    requires dimension_meaning(data, db), route_meaning(data, db), aggregate_ready(data),
        numerator ==> data.dictionary.names.known(name) && data.dictionary.names.text(name) == parameters.p1,
    ensures supplier_filter(data, supplier, name, numerator) ==
        sum(db.nation, |row: NationRow| if row.n_nationkey == supplier as int && (!numerator || row.n_name == parameters.p1) { 1int } else { 0int }),
{
    hide(sum);
    if numerator {
        reveal(route_meaning);
        let raw = |row: NationRow| if row.n_nationkey == supplier as int && text_matches(data.dictionary, 1, name, row.n_name) { 1int } else { 0int };
        let target = |row: NationRow| if row.n_nationkey == supplier as int && (!numerator || row.n_name == parameters.p1) { 1int } else { 0int };
        lemma_sum_ext_all(db.nation, raw, target);
    } else {
        lemma_supplier_projection(data, db, supplier, |name: i64| 1int);
        lemma_sum_ext_all(data.supplier_routes.rows((supplier, 0, 0)), |cell: Cell| cell.count@ * (|name: i64| 1int)(cell.first), |cell: Cell| cell.count@);
        lemma_sum_ext_all(db.nation,
            |row: NationRow| if row.n_nationkey == supplier as int { (|name: i64| 1int)(data.dictionary.names.text_code(row.n_name)) } else { 0int },
            |row: NationRow| if row.n_nationkey == supplier as int && (!numerator || row.n_name == parameters.p1) { 1int } else { 0int });
    }
}

pub proof fn lemma_routed_nations(data: Data, pair: Major, group: Cell, region: Cell, key: Major, minor: Major, numerator: bool, volume: bool)
    requires scalar(data.supplier_routes),
    ensures nation_pair_effect(data.supplier_routes.rows((pair.1, 0, 0)), group, region, 1, key, minor, numerator, volume) ==
        if key.0 == region.first && key.1 == group.first && (!numerator ==> key.2 == 0) && minor == (group.second, 0, 0) {
            measure(group, volume) * region.count@ * supplier_filter(data, pair.1, key.2, numerator)
        } else { 0int },
{
    hide(sum);
    reveal(nation_pair_effect);
    let rows = data.supplier_routes.rows((pair.1, 0, 0));
    let active = key.0 == region.first && key.1 == group.first && (!numerator ==> key.2 == 0) && minor == (group.second, 0, 0);
    let count = |nation: Cell| if !numerator || nation.first == key.2 { nation.count@ } else { 0int };
    let coefficient = if active { measure(group, volume) * region.count@ } else { 0int };
    let raw = |nation: Cell| pair_weight(nation, group, region, 1, key, minor, numerator, volume);
    let scaled = |nation: Cell| coefficient * count(nation);
    assert forall|nation: Cell| #[trigger] raw(nation) == scaled(nation) by {
        assert(raw(nation) == scaled(nation)) by (nonlinear_arith)
            requires raw(nation) == if key == (region.first, group.first, if numerator { nation.first } else { 0 }) && minor == (group.second, 0, 0) {
                measure(group, volume) * (region.count@ * nation.count@)
            } else { 0int },
                coefficient == if active { measure(group, volume) * region.count@ } else { 0int },
                scaled(nation) == coefficient * count(nation), count(nation) == if !numerator || nation.first == key.2 { nation.count@ } else { 0int },
                active == (key.0 == region.first && key.1 == group.first && (!numerator ==> key.2 == 0) && minor == (group.second, 0, 0));
    }
    lemma_sum_ext_all(rows, raw, scaled);
    lemma_sum_scale(rows, count, scaled, coefficient);
    if numerator {
        assert forall|index: int| 0 <= index < rows.len() implies count(#[trigger] rows[index]) == cell_weight(rows[index], (key.2, 0, 0), false) by {}
        lemma_sum_ext(rows, count, |cell: Cell| cell_weight(cell, (key.2, 0, 0), false));
        reveal(Table::at);
        reveal(cell_sum);
    } else {
        lemma_sum_ext_all(rows, count, |cell: Cell| cell.count@);
    }
}

pub proof fn lemma_routed_regions(data: Data, pair: Major, group: Cell, key: Major, minor: Major, numerator: bool, volume: bool)
    requires scalar(data.customer_routes), scalar(data.supplier_routes),
    ensures region_pair_effect(data.customer_routes.rows((pair.0, 0, 0)), data.supplier_routes.rows((pair.1, 0, 0)), group, 1, key, minor, numerator, volume) ==
        if key.1 == group.first && (!numerator ==> key.2 == 0) && minor == (group.second, 0, 0) {
            measure(group, volume) * data.customer_routes.at((pair.0, 0, 0), (key.0, 0, 0), false) * supplier_filter(data, pair.1, key.2, numerator)
        } else { 0int },
{
    hide(sum);
    reveal(region_pair_effect);
    let rows = data.customer_routes.rows((pair.0, 0, 0));
    let raw = |region: Cell| nation_pair_effect(data.supplier_routes.rows((pair.1, 0, 0)), group, region, 1, key, minor, numerator, volume);
    let count = |region: Cell| cell_weight(region, (key.0, 0, 0), false);
    let active = key.1 == group.first && (!numerator ==> key.2 == 0) && minor == (group.second, 0, 0);
    let coefficient = if active { measure(group, volume) * supplier_filter(data, pair.1, key.2, numerator) } else { 0int };
    let scaled = |region: Cell| coefficient * count(region);
    assert forall|index: int| 0 <= index < rows.len() implies raw(#[trigger] rows[index]) == scaled(rows[index]) by {
        let region = rows[index];
        lemma_routed_nations(data, pair, group, region, key, minor, numerator, volume);
        assert(raw(region) == scaled(region)) by (nonlinear_arith)
            requires raw(region) == if key.0 == region.first && active { measure(group, volume) * region.count@ * supplier_filter(data, pair.1, key.2, numerator) } else { 0int },
                coefficient == if active { measure(group, volume) * supplier_filter(data, pair.1, key.2, numerator) } else { 0int },
                scaled(region) == coefficient * count(region),
                count(region) == if region.first == key.0 { region.count@ } else { 0int };
    }
    lemma_sum_ext(rows, raw, scaled);
    lemma_sum_scale(rows, count, scaled, coefficient);
    reveal(Table::at);
    reveal(cell_sum);
    assert(data.customer_routes.at((pair.0, 0, 0), (key.0, 0, 0), false) == sum(rows, count));
    let customer = data.customer_routes.at((pair.0, 0, 0), (key.0, 0, 0), false);
    let supplier = supplier_filter(data, pair.1, key.2, numerator);
    assert(coefficient * customer == if active { measure(group, volume) * customer * supplier } else { 0int }) by (nonlinear_arith)
        requires coefficient == if active { measure(group, volume) * supplier } else { 0int };
}

pub proof fn lemma_routed_pair(data: Data, pair: Major, key: Major, year: i64, numerator: bool, volume: bool)
    requires aggregate_ready(data), !numerator ==> key.2 == 0,
    ensures route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, (year, 0, 0), numerator, volume) ==
        data.base.at(pair, (key.1, year, 0), volume) * route_coefficient(data, pair, key.0, key.2, numerator),
{
    hide(sum);
    reveal(group_pair_effect);
    let rows = data.base.rows(pair);
    let raw = |group: Cell| region_pair_effect(data.customer_routes.rows((pair.0, 0, 0)), data.supplier_routes.rows((pair.1, 0, 0)), group, 1, key, (year, 0, 0), numerator, volume);
    let count = |group: Cell| cell_weight(group, (key.1, year, 0), volume);
    let coefficient = route_coefficient(data, pair, key.0, key.2, numerator);
    let scaled = |group: Cell| coefficient * count(group);
    assert forall|index: int| 0 <= index < rows.len() implies raw(#[trigger] rows[index]) == scaled(rows[index]) by {
        let group = rows[index];
        assert(pair.2 == 0 && group.third == 0);
        lemma_routed_regions(data, pair, group, key, (year, 0, 0), numerator, volume);
        assert(raw(group) == scaled(group)) by (nonlinear_arith)
            requires raw(group) == if key.1 == group.first && year == group.second {
                measure(group, volume) * data.customer_routes.at((pair.0, 0, 0), (key.0, 0, 0), false) * supplier_filter(data, pair.1, key.2, numerator)
            } else { 0int }, coefficient == data.customer_routes.at((pair.0, 0, 0), (key.0, 0, 0), false) * supplier_filter(data, pair.1, key.2, numerator),
                scaled(group) == coefficient * count(group), count(group) == if key.1 == group.first && year == group.second { measure(group, volume) } else { 0int };
    }
    lemma_sum_ext(rows, raw, scaled);
    lemma_sum_scale(rows, count, scaled, coefficient);
    reveal(Table::at);
    reveal(cell_sum);
    assert(data.base.at(pair, (key.1, year, 0), volume) == sum(rows, count));
    assert(coefficient * sum(rows, count) == sum(rows, count) * coefficient) by (nonlinear_arith);
}

}
