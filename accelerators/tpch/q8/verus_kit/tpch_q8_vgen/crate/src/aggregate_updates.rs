use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::table::*;
use crate::model::*;
use crate::maintenance::*;
use crate::routing::*;
use crate::aggregate_bridge::*;

verus! {
pub proof fn lemma_nation_sign(rows: Seq<Cell>, group: Cell, region: Cell, sign: int, key: Major, minor: Major, numerator: bool, volume: bool)
    ensures nation_pair_effect(rows, group, region, sign, key, minor, numerator, volume) == sign * nation_pair_effect(rows, group, region, 1, key, minor, numerator, volume),
{
    hide(sum);
    reveal(nation_pair_effect);
    let raw = |nation: Cell| pair_weight(nation, group, region, sign, key, minor, numerator, volume);
    let positive = |nation: Cell| pair_weight(nation, group, region, 1, key, minor, numerator, volume);
    assert forall|nation: Cell| #[trigger] raw(nation) == sign * positive(nation) by {
        let active = key == (region.first, group.first, if numerator { nation.first } else { 0 }) && minor == (group.second, 0, 0);
        assert(raw(nation) == sign * positive(nation)) by (nonlinear_arith)
            requires raw(nation) == if active { measure(group, volume) * (sign * region.count@ * nation.count@) } else { 0int },
                positive(nation) == if active { measure(group, volume) * (region.count@ * nation.count@) } else { 0int };
    }
    lemma_sum_scale(rows, positive, raw, sign);
}

pub proof fn lemma_region_sign(regions: Seq<Cell>, nations: Seq<Cell>, group: Cell, sign: int, key: Major, minor: Major, numerator: bool, volume: bool)
    ensures region_pair_effect(regions, nations, group, sign, key, minor, numerator, volume) == sign * region_pair_effect(regions, nations, group, 1, key, minor, numerator, volume),
{
    hide(sum);
    reveal(region_pair_effect);
    let raw = |region: Cell| nation_pair_effect(nations, group, region, sign, key, minor, numerator, volume);
    let positive = |region: Cell| nation_pair_effect(nations, group, region, 1, key, minor, numerator, volume);
    assert forall|region: Cell| #[trigger] raw(region) == sign * positive(region) by {
        lemma_nation_sign(nations, group, region, sign, key, minor, numerator, volume);
    }
    lemma_sum_scale(regions, positive, raw, sign);
}

pub proof fn lemma_group_sign(groups: Seq<Cell>, regions: Seq<Cell>, nations: Seq<Cell>, sign: int, key: Major, minor: Major, numerator: bool, volume: bool)
    ensures group_pair_effect(groups, regions, nations, sign, key, minor, numerator, volume) == sign * group_pair_effect(groups, regions, nations, 1, key, minor, numerator, volume),
{
    hide(sum);
    reveal(group_pair_effect);
    let raw = |group: Cell| region_pair_effect(regions, nations, group, sign, key, minor, numerator, volume);
    let positive = |group: Cell| region_pair_effect(regions, nations, group, 1, key, minor, numerator, volume);
    assert forall|group: Cell| #[trigger] raw(group) == sign * positive(group) by {
        lemma_region_sign(regions, nations, group, sign, key, minor, numerator, volume);
    }
    lemma_sum_scale(groups, positive, raw, sign);
}

pub proof fn lemma_routed_sign(data: Data, keys: Seq<Major>, sign: int, key: Major, minor: Major, numerator: bool, volume: bool)
    ensures routed_effect(keys, data.base, data.customer_routes, data.supplier_routes, sign, key, minor, numerator, volume) ==
        sign * routed_effect(keys, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume),
{
    hide(sum);
    reveal(routed_effect);
    let raw = |pair: Major| route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, sign, key, minor, numerator, volume);
    let positive = |pair: Major| route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume);
    assert forall|pair: Major| #[trigger] raw(pair) == sign * positive(pair) by {
        lemma_group_sign(data.base.rows(pair), data.customer_routes.rows((pair.0, 0, 0)), data.supplier_routes.rows((pair.1, 0, 0)), sign, key, minor, numerator, volume);
    }
    lemma_sum_scale(keys, positive, raw, sign);
}

pub proof fn lemma_apply_aggregates(before: Data, after: Data, changes: Table, nations: Keys, pairs: Pairs)
    requires maintenance_ready(before, after, changes, nations, pairs), routed_meaning(before),
        forall|key: Major, minor: Major, volume: bool| #[trigger] after.totals.at(key, minor, volume) == before.totals.at(key, minor, volume)
            + routed_effect(pairs.values@, before.base, before.customer_routes, before.supplier_routes, -1, key, minor, false, volume)
            + routed_effect(pairs.values@, after.base, after.customer_routes, after.supplier_routes, 1, key, minor, false, volume),
        forall|key: Major, minor: Major, volume: bool| #[trigger] after.numerators.at(key, minor, volume) == before.numerators.at(key, minor, volume)
            + routed_effect(pairs.values@, before.base, before.customer_routes, before.supplier_routes, -1, key, minor, true, volume)
            + routed_effect(pairs.values@, after.base, after.customer_routes, after.supplier_routes, 1, key, minor, true, volume),
    ensures routed_meaning(after),
{
    reveal(routed_meaning);
    assert forall|key: Major, minor: Major, volume: bool| #[trigger] after.totals.at(key, minor, volume) ==
        routed_effect(after.base.keys_view(), after.base, after.customer_routes, after.supplier_routes, 1, key, minor, false, volume) by {
        lemma_routed_delta(before, after, changes, nations, pairs, key, minor, false, volume);
        lemma_routed_sign(before, pairs.values@, -1, key, minor, false, volume);
    }
    assert forall|key: Major, minor: Major, volume: bool| #[trigger] after.numerators.at(key, minor, volume) ==
        routed_effect(after.base.keys_view(), after.base, after.customer_routes, after.supplier_routes, 1, key, minor, true, volume) by {
        lemma_routed_delta(before, after, changes, nations, pairs, key, minor, true, volume);
        lemma_routed_sign(before, pairs.values@, -1, key, minor, true, volume);
    }
}

pub proof fn lemma_runtime_aggregate_ready(data: Data)
    requires runtime_valid(data),
    ensures aggregate_ready(data),
{
    lemma_runtime_access(data);
    assert forall|key: Major, index: int| 0 <= index < data.base.rows(key).len() implies
        crate::base_proofs::base_support(key, minor_of(#[trigger] data.base.rows(key)[index]), data.dictionary) by {}
}


pub open spec fn maintenance_ready(before: Data, after: Data, changes: Table, nations: Keys, pairs: Pairs) -> bool {
    &&& runtime_valid(before) && aggregate_ready(before) && aggregate_ready(after) && index_meaning(before)
    &&& changes.valid() && nations.valid() && pairs.valid() && crate::dictionary::extension(before.dictionary, after.dictionary)
    &&& forall|key: Major, minor: Major, volume: bool| #[trigger] after.base.at(key, minor, volume) == before.base.at(key, minor, volume) + changes.at(key, minor, volume)
    &&& forall|index: int| 0 <= index < changes.keys_view().len() ==> pairs.has(#[trigger] changes.keys_view()[index])
    &&& reverse_pairs_covered(pairs, nations, before.base_by_customer, nations.values@.len() as int, false)
    &&& reverse_pairs_covered(pairs, nations, before.base_by_supplier, nations.values@.len() as int, true)
    &&& forall|key: Major, minor: Major, volume: bool| #[trigger] after.customer_routes.at(key, minor, volume) == before.customer_routes.at(key, minor, volume)
        + customer_route_changes(nations.values@, after.nations, after.regions, key, minor, volume)
        - customer_route_changes(nations.values@, before.nations, before.regions, key, minor, volume)
    &&& forall|key: Major, minor: Major, volume: bool| #[trigger] after.supplier_routes.at(key, minor, volume) == before.supplier_routes.at(key, minor, volume)
        + supplier_route_changes(nations.values@, after.nations, key, minor, volume)
        - supplier_route_changes(nations.values@, before.nations, key, minor, volume)
}

pub proof fn lemma_table_class(table: Table, key: Major, minor: Major, volume: bool)
    ensures class_total(table.rows(key), |cell: Cell| minor_of(cell), |cell: Cell| measure(cell, volume), minor) == table.at(key, minor, volume),
{
    hide(sum);
    reveal(Table::at);
    reveal(cell_sum);
    lemma_class_total_weight(table.rows(key), |cell: Cell| minor_of(cell), |cell: Cell| measure(cell, volume), minor,
        |cell: Cell| cell_weight(cell, minor, volume));
}

pub proof fn lemma_equal_table_counts(before: Table, after: Table, key: Major)
    requires forall|minor: Major| #[trigger] before.at(key, minor, false) == after.at(key, minor, false),
    ensures sum(before.rows(key), |cell: Cell| cell.count@) == sum(after.rows(key), |cell: Cell| cell.count@),
{
    hide(sum);
    let project = |cell: Cell| minor_of(cell);
    let count = |cell: Cell| cell.count@;
    assert forall|minor: Major| #[trigger] class_total(before.rows(key), project, count, minor) == class_total(after.rows(key), project, count, minor) by {
        lemma_table_class(before, key, minor, false);
        lemma_table_class(after, key, minor, false);
        assert((|cell: Cell| measure(cell, false)) =~= count);
    }
    lemma_equal_classes(before.rows(key), after.rows(key), project, project, count, count, |minor: Major| 1int);
    lemma_sum_ext_all(before.rows(key), |cell: Cell| count(cell) * (|minor: Major| 1int)(project(cell)), count);
    lemma_sum_ext_all(after.rows(key), |cell: Cell| count(cell) * (|minor: Major| 1int)(project(cell)), count);
}

pub proof fn lemma_empty_pair(data: Data, pair: Major, key: Major, minor: Major, numerator: bool, volume: bool)
    requires data.base.rows(pair).len() == 0,
    ensures route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume) == 0,
{
    hide(sum);
    reveal(group_pair_effect);
    lemma_sum_empty(data.base.rows(pair), |group: Cell| region_pair_effect(data.customer_routes.rows((pair.0, 0, 0)),
        data.supplier_routes.rows((pair.1, 0, 0)), group, 1, key, minor, numerator, volume));
}

pub proof fn lemma_missing_changes(changes: Table, pairs: Pairs, pair: Major)
    requires changes.valid(), pairs.valid(), !pairs.has(pair),
        forall|index: int| 0 <= index < changes.keys_view().len() ==> pairs.has(#[trigger] changes.keys_view()[index]),
    ensures changes.rows(pair).len() == 0,
        forall|minor: Major, volume: bool| #[trigger] changes.at(pair, minor, volume) == 0,
{
    hide(sum);
    lemma_keys_complete(changes);
    if changes.keys_view().contains(pair) {
        let index = choose|index: int| 0 <= index < changes.keys_view().len() && #[trigger] changes.keys_view()[index] == pair;
    }
    assert(changes.rows(pair).len() == 0);
    assert forall|minor: Major, volume: bool| #[trigger] changes.at(pair, minor, volume) == 0 by {
        reveal(Table::at);
        reveal(cell_sum);
        lemma_sum_empty(changes.rows(pair), |cell: Cell| cell_weight(cell, minor, volume));
    }
}

pub proof fn lemma_marked_pair_empty(before: Data, after: Data, changes: Table, nations: Keys, pairs: Pairs, pair: Major)
    requires maintenance_ready(before, after, changes, nations, pairs), pair.2 == 0, !pairs.has(pair), nations.has(pair.0) || nations.has(pair.1),
    ensures before.base.rows(pair).len() == 0, after.base.rows(pair).len() == 0,
{
    hide(sum);
    lemma_runtime_access(before);
    reveal(index_meaning);
    lemma_missing_changes(changes, pairs, pair);
    let supplier = !nations.has(pair.0);
    let nation = if supplier { pair.1 } else { pair.0 };
    let neighbor = if supplier { pair.0 } else { pair.1 };
    let reverse = if supplier { before.base_by_supplier } else { before.base_by_customer };
    let index = choose|index: int| 0 <= index < nations.values@.len() && #[trigger] nations.values@[index] == nation;
    let rows = reverse.rows((nation, 0, 0));
    let weight = |cell: Cell| cell_weight(cell, (neighbor, 0, 0), false);
    assert forall|offset: int| 0 <= offset < rows.len() implies weight(#[trigger] rows[offset]) == 0 by {
        if rows[offset].first == neighbor {
            assert(pairs.has(if supplier { (rows[offset].first, nation, 0) } else { (nation, rows[offset].first, 0) }));
        }
    }
    lemma_sum_zero(rows, weight);
    reveal(Table::at);
    reveal(cell_sum);
    assert(reverse.at((nation, 0, 0), (neighbor, 0, 0), false) == 0);
    assert(sum(before.base.rows(pair), |cell: Cell| cell.count@) == 0);
    if before.base.rows(pair).len() > 0 {
        lemma_sum_pos(before.base.rows(pair), |cell: Cell| cell.count@, 0);
    }
    if after.base.rows(pair).len() > 0 {
        let cells = after.base.rows(pair);
        let minor = minor_of(cells[0]);
        lemma_cell_lookup(cells, minor, 0);
        assert(after.base.at(pair, minor, false) == cells[0].count@);
        lemma_sum_empty(before.base.rows(pair), |cell: Cell| cell_weight(cell, minor, false));
        assert(before.base.at(pair, minor, false) == 0);
        assert(after.base.at(pair, minor, false) == 0);
        assert(cells[0].count@ > 0);
    }
}

pub proof fn lemma_routes_unchanged(before: Data, after: Data, changes: Table, nations: Keys, pairs: Pairs, nation: i64)
    requires maintenance_ready(before, after, changes, nations, pairs), !nations.has(nation),
    ensures forall|name: i64| #[trigger] after.customer_routes.at((nation, 0, 0), (name, 0, 0), false) == before.customer_routes.at((nation, 0, 0), (name, 0, 0), false),
        forall|name: i64| #[trigger] after.supplier_routes.at((nation, 0, 0), (name, 0, 0), false) == before.supplier_routes.at((nation, 0, 0), (name, 0, 0), false),
        sum(after.supplier_routes.rows((nation, 0, 0)), |cell: Cell| cell.count@) == sum(before.supplier_routes.rows((nation, 0, 0)), |cell: Cell| cell.count@),
{
    hide(sum);
    assert forall|name: i64| #[trigger] after.customer_routes.at((nation, 0, 0), (name, 0, 0), false) == before.customer_routes.at((nation, 0, 0), (name, 0, 0), false) by {
        crate::route_updates::lemma_route_changes(nations, after.nations, after.regions, (nation, 0, 0), (name, 0, 0), false);
        crate::route_updates::lemma_route_changes(nations, before.nations, before.regions, (nation, 0, 0), (name, 0, 0), false);
    }
    assert forall|name: i64| #[trigger] after.supplier_routes.at((nation, 0, 0), (name, 0, 0), false) == before.supplier_routes.at((nation, 0, 0), (name, 0, 0), false) by {
        crate::route_updates::lemma_route_changes(nations, after.nations, after.regions, (nation, 0, 0), (name, 0, 0), false);
        crate::route_updates::lemma_route_changes(nations, before.nations, before.regions, (nation, 0, 0), (name, 0, 0), false);
    }
    crate::dimensions::lemma_support_from_cells(before.supplier_routes, before.dictionary, 1);
    crate::dimensions::lemma_support_from_cells(after.supplier_routes, after.dictionary, 1);
    assert forall|minor: Major| #[trigger] before.supplier_routes.at((nation, 0, 0), minor, false) == after.supplier_routes.at((nation, 0, 0), minor, false) by {
        if minor.1 == 0 && minor.2 == 0 {
            assert(minor == (minor.0, 0, 0));
            crate::route_updates::lemma_route_changes(nations, after.nations, after.regions, (nation, 0, 0), minor, false);
            crate::route_updates::lemma_route_changes(nations, before.nations, before.regions, (nation, 0, 0), minor, false);
        }
    }
    lemma_equal_table_counts(before.supplier_routes, after.supplier_routes, (nation, 0, 0));
}

pub proof fn lemma_unaffected_pair(before: Data, after: Data, changes: Table, nations: Keys, pairs: Pairs,
    pair: Major, key: Major, minor: Major, numerator: bool, volume: bool)
    requires maintenance_ready(before, after, changes, nations, pairs), !pairs.has(pair),
    ensures route_key_effect(pair, after.base, after.customer_routes, after.supplier_routes, 1, key, minor, numerator, volume) ==
        route_key_effect(pair, before.base, before.customer_routes, before.supplier_routes, 1, key, minor, numerator, volume),
{
    hide(sum);
    if !crate::aggregate_cells::output_support(key, minor, after.dictionary, numerator) {
        crate::aggregate_cells::lemma_group_support(after, pair, key, minor, numerator, volume);
        crate::aggregate_cells::lemma_group_support(before, pair, key, minor, numerator, volume);
    } else if pair.2 != 0 {
        if before.base.rows(pair).len() > 0 { assert(crate::base_proofs::base_support(pair, minor_of(before.base.rows(pair)[0]), before.dictionary)); }
        if after.base.rows(pair).len() > 0 { assert(crate::base_proofs::base_support(pair, minor_of(after.base.rows(pair)[0]), after.dictionary)); }
        lemma_empty_pair(before, pair, key, minor, numerator, volume);
        lemma_empty_pair(after, pair, key, minor, numerator, volume);
    } else if nations.has(pair.0) || nations.has(pair.1) {
        lemma_marked_pair_empty(before, after, changes, nations, pairs, pair);
        lemma_empty_pair(before, pair, key, minor, numerator, volume);
        lemma_empty_pair(after, pair, key, minor, numerator, volume);
    } else {
        lemma_missing_changes(changes, pairs, pair);
        lemma_routes_unchanged(before, after, changes, nations, pairs, pair.0);
        lemma_routes_unchanged(before, after, changes, nations, pairs, pair.1);
        lemma_routed_pair(before, pair, key, minor.0, numerator, volume);
        lemma_routed_pair(after, pair, key, minor.0, numerator, volume);
        assert(after.base.at(pair, (key.1, minor.0, 0), volume) == before.base.at(pair, (key.1, minor.0, 0), volume));
        assert(route_coefficient(after, pair, key.0, key.2, numerator) == route_coefficient(before, pair, key.0, key.2, numerator));
    }
}

pub proof fn lemma_pair_partition(data: Data, pair: Major, key: Major, minor: Major, numerator: bool, volume: bool)
    requires data.base.valid(),
    ensures route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume) ==
        sum(data.base.keys_view(), |entry: Major| if entry == pair { route_key_effect(entry, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume) } else { 0int }),
{
    hide(sum);
    lemma_keys_complete(data.base);
    lemma_keys_unique(data.base);
    let value = route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume);
    let selected = |entry: Major| if entry == pair { value } else { 0int };
    if !data.base.keys_view().contains(pair) {
        assert(data.base.rows(pair).len() == 0);
        lemma_empty_pair(data, pair, key, minor, numerator, volume);
    }
    lemma_sum_indicator_distinct(data.base.keys_view(), pair, value, selected);
    lemma_sum_ext_all(data.base.keys_view(), selected,
        |entry: Major| if entry == pair { route_key_effect(entry, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume) } else { 0int });
}

pub proof fn lemma_routed_delta(before: Data, after: Data, changes: Table, nations: Keys, pairs: Pairs, key: Major, minor: Major, numerator: bool, volume: bool)
    requires maintenance_ready(before, after, changes, nations, pairs),
    ensures routed_effect(after.base.keys_view(), after.base, after.customer_routes, after.supplier_routes, 1, key, minor, numerator, volume) ==
        routed_effect(before.base.keys_view(), before.base, before.customer_routes, before.supplier_routes, 1, key, minor, numerator, volume)
        + routed_effect(pairs.values@, after.base, after.customer_routes, after.supplier_routes, 1, key, minor, numerator, volume)
        - routed_effect(pairs.values@, before.base, before.customer_routes, before.supplier_routes, 1, key, minor, numerator, volume),
{
    hide(sum);
    reveal(routed_effect);
    let identity = |pair: Major| pair;
    let old_weight = |pair: Major| route_key_effect(pair, before.base, before.customer_routes, before.supplier_routes, 1, key, minor, numerator, volume);
    let new_weight = |pair: Major| route_key_effect(pair, after.base, after.customer_routes, after.supplier_routes, 1, key, minor, numerator, volume);
    lemma_keys_complete(before.base);
    lemma_keys_unique(before.base);
    lemma_keys_complete(after.base);
    lemma_keys_unique(after.base);
    assert forall|pair: Major| #[trigger] old_weight(pair) == sum(before.base.keys_view(), |entry: Major| if identity(entry) == pair { old_weight(entry) } else { 0int }) by {
        let selected = |entry: Major| if identity(entry) == pair { old_weight(entry) } else { 0int };
        if !before.base.keys_view().contains(pair) {
            assert(before.base.rows(pair).len() == 0);
            lemma_empty_pair(before, pair, key, minor, numerator, volume);
            assert(old_weight(pair) == 0);
        }
        lemma_sum_indicator_distinct(before.base.keys_view(), pair, old_weight(pair), selected);
    }
    assert forall|pair: Major| #[trigger] new_weight(pair) == sum(after.base.keys_view(), |entry: Major| if identity(entry) == pair { new_weight(entry) } else { 0int }) by {
        let selected = |entry: Major| if identity(entry) == pair { new_weight(entry) } else { 0int };
        if !after.base.keys_view().contains(pair) {
            assert(after.base.rows(pair).len() == 0);
            lemma_empty_pair(after, pair, key, minor, numerator, volume);
            assert(new_weight(pair) == 0);
        }
        lemma_sum_indicator_distinct(after.base.keys_view(), pair, new_weight(pair), selected);
    }
    assert forall|pair: Major| !pairs.values@.contains(pair) implies #[trigger] new_weight(pair) == old_weight(pair) by {
        lemma_unaffected_pair(before, after, changes, nations, pairs, pair, key, minor, numerator, volume);
    }
    lemma_sparse_partition_delta(before.base.keys_view(), after.base.keys_view(), pairs.values@, identity, old_weight, new_weight, old_weight, new_weight);
    let difference = |pair: Major| new_weight(pair) - old_weight(pair);
    lemma_sum_sub(pairs.values@, new_weight, old_weight, difference);
}

}
