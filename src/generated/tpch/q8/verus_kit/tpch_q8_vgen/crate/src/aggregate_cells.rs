use vstd::prelude::*;
use verus_common::sums::*;
use verus_prooflib::*;
use crate::model::*;
use crate::table::*;
use crate::routing::*;
use crate::dictionary::*;
use crate::aggregate_bridge::*;

verus! {

pub open spec fn output_support(key: Major, minor: Major, dictionary: Dictionary, numerator: bool) -> bool {
    0 <= key.0 < dictionary.length && 0 <= key.1 < dictionary.length
        && (if numerator { 0 <= key.2 < dictionary.length } else { key.2 == 0 })
        && (minor.0 == 1995 || minor.0 == 1996) && minor.1 == 0 && minor.2 == 0
}

pub proof fn lemma_nation_measures(rows: Seq<Cell>, group: Cell, region: Cell, key: Major, minor: Major, numerator: bool)
    requires group.count > 0, region.count > 0,
        forall|index: int| 0 <= index < rows.len() ==> (#[trigger] rows[index]).count > 0,
    ensures nation_pair_effect(rows, group, region, 1, key, minor, numerator, false) >= 0,
        nation_pair_effect(rows, group, region, 1, key, minor, numerator, false) == 0 ==>
            nation_pair_effect(rows, group, region, 1, key, minor, numerator, true) == 0,
{
    hide(sum);
    reveal(nation_pair_effect);
    let count = |nation: Cell| pair_weight(nation, group, region, 1, key, minor, numerator, false);
    let volume = |nation: Cell| pair_weight(nation, group, region, 1, key, minor, numerator, true);
    assert forall|index: int| 0 <= index < rows.len() implies count(#[trigger] rows[index]) >= 0
        && (count(rows[index]) == 0 ==> volume(rows[index]) == 0) by {
        let nation = rows[index];
        let active = key == (region.first, group.first, if numerator { nation.first } else { 0 }) && minor == (group.second, 0, 0);
        assert(count(nation) >= 0 && (count(nation) == 0 ==> volume(nation) == 0)) by (nonlinear_arith)
            requires group.count > 0, region.count > 0, nation.count > 0,
                count(nation) == if active { group.count as int * (region.count as int * nation.count as int) } else { 0int },
                volume(nation) == if active { group.volume as int * (region.count as int * nation.count as int) } else { 0int };
    }
    lemma_sum_measure_support(rows, count, volume);
}

pub proof fn lemma_region_measures(regions: Seq<Cell>, nations: Seq<Cell>, group: Cell, key: Major, minor: Major, numerator: bool)
    requires group.count > 0,
        forall|index: int| 0 <= index < regions.len() ==> (#[trigger] regions[index]).count > 0,
        forall|index: int| 0 <= index < nations.len() ==> (#[trigger] nations[index]).count > 0,
    ensures region_pair_effect(regions, nations, group, 1, key, minor, numerator, false) >= 0,
        region_pair_effect(regions, nations, group, 1, key, minor, numerator, false) == 0 ==>
            region_pair_effect(regions, nations, group, 1, key, minor, numerator, true) == 0,
{
    hide(sum);
    reveal(region_pair_effect);
    let count = |region: Cell| nation_pair_effect(nations, group, region, 1, key, minor, numerator, false);
    let volume = |region: Cell| nation_pair_effect(nations, group, region, 1, key, minor, numerator, true);
    assert forall|index: int| 0 <= index < regions.len() implies count(#[trigger] regions[index]) >= 0
        && (count(regions[index]) == 0 ==> volume(regions[index]) == 0) by {
        lemma_nation_measures(nations, group, regions[index], key, minor, numerator);
    }
    lemma_sum_measure_support(regions, count, volume);
}

pub proof fn lemma_group_measures(data: Data, pair: Major, key: Major, minor: Major, numerator: bool)
    requires aggregate_ready(data),
    ensures route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, false) >= 0,
        route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, false) == 0 ==>
            route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, true) == 0,
{
    hide(sum);
    reveal(group_pair_effect);
    let rows = data.base.rows(pair);
    let regions = data.customer_routes.rows((pair.0, 0, 0));
    let nations = data.supplier_routes.rows((pair.1, 0, 0));
    let count = |group: Cell| region_pair_effect(regions, nations, group, 1, key, minor, numerator, false);
    let volume = |group: Cell| region_pair_effect(regions, nations, group, 1, key, minor, numerator, true);
    assert forall|index: int| 0 <= index < rows.len() implies count(#[trigger] rows[index]) >= 0
        && (count(rows[index]) == 0 ==> volume(rows[index]) == 0) by {
        lemma_region_measures(regions, nations, rows[index], key, minor, numerator);
    }
    lemma_sum_measure_support(rows, count, volume);
}

pub proof fn lemma_routed_measures(data: Data, keys: Seq<Major>, key: Major, minor: Major, numerator: bool)
    requires aggregate_ready(data),
    ensures routed_effect(keys, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, false) >= 0,
        routed_effect(keys, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, false) == 0 ==>
            routed_effect(keys, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, true) == 0,
{
    hide(sum);
    reveal(routed_effect);
    let count = |pair: Major| route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, false);
    let volume = |pair: Major| route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, true);
    assert forall|index: int| 0 <= index < keys.len() implies count(#[trigger] keys[index]) >= 0
        && (count(keys[index]) == 0 ==> volume(keys[index]) == 0) by {
        lemma_group_measures(data, keys[index], key, minor, numerator);
    }
    lemma_sum_measure_support(keys, count, volume);
}

pub proof fn lemma_group_support(data: Data, pair: Major, key: Major, minor: Major, numerator: bool, volume: bool)
    requires aggregate_ready(data), !output_support(key, minor, data.dictionary, numerator),
    ensures route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume) == 0,
{
    hide(sum);
    reveal(group_pair_effect);
    let rows = data.base.rows(pair);
    let regions = data.customer_routes.rows((pair.0, 0, 0));
    let nations = data.supplier_routes.rows((pair.1, 0, 0));
    let group_weight = |group: Cell| region_pair_effect(regions, nations, group, 1, key, minor, numerator, volume);
    assert forall|index: int| 0 <= index < rows.len() implies group_weight(#[trigger] rows[index]) == 0 by {
        let group = rows[index];
        assert(crate::base_proofs::base_support(pair, minor_of(group), data.dictionary));
        reveal(region_pair_effect);
        let region_weight = |region: Cell| nation_pair_effect(nations, group, region, 1, key, minor, numerator, volume);
        assert forall|offset: int| 0 <= offset < regions.len() implies region_weight(#[trigger] regions[offset]) == 0 by {
            let region = regions[offset];
            assert(0 <= region.first < data.dictionary.length);
            reveal(nation_pair_effect);
            let nation_weight = |nation: Cell| pair_weight(nation, group, region, 1, key, minor, numerator, volume);
            assert forall|position: int| 0 <= position < nations.len() implies nation_weight(#[trigger] nations[position]) == 0 by {
                assert(0 <= nations[position].first < data.dictionary.length);
            }
            lemma_sum_zero(nations, nation_weight);
        }
        lemma_sum_zero(regions, region_weight);
    }
    lemma_sum_zero(rows, group_weight);
}

pub proof fn lemma_routed_support(data: Data, keys: Seq<Major>, key: Major, minor: Major, numerator: bool, volume: bool)
    requires aggregate_ready(data), !output_support(key, minor, data.dictionary, numerator),
    ensures routed_effect(keys, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume) == 0,
{
    hide(sum);
    reveal(routed_effect);
    let weight = |pair: Major| route_key_effect(pair, data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume);
    assert forall|pair: Major| #[trigger] weight(pair) == 0 by {
        lemma_group_support(data, pair, key, minor, numerator, volume);
    }
    lemma_sum_zero(keys, weight);
}

pub proof fn lemma_output_cells(table: Table, data: Data, numerator: bool)
    requires table.valid(), aggregate_ready(data),
        forall|key: Major, minor: Major, volume: bool| #[trigger] table.at(key, minor, volume) ==
            routed_effect(data.base.keys_view(), data.base, data.customer_routes, data.supplier_routes, 1, key, minor, numerator, volume),
    ensures positive(table),
        forall|key: Major, index: int| 0 <= index < table.rows(key).len() ==>
            output_support(key, minor_of(#[trigger] table.rows(key)[index]), data.dictionary, numerator),
{
    hide(sum);
    assert forall|key: Major, index: int| 0 <= index < table.rows(key).len() implies (#[trigger] table.rows(key)[index]).count > 0
        && output_support(key, minor_of(table.rows(key)[index]), data.dictionary, numerator) by {
        let rows = table.rows(key);
        let minor = minor_of(rows[index]);
        lemma_cell_lookup(rows, minor, index);
        reveal(Table::at);
        assert(table.at(key, minor, false) == rows[index].count as int);
        assert(table.at(key, minor, true) == rows[index].volume as int);
        if !output_support(key, minor, data.dictionary, numerator) {
            lemma_routed_support(data, data.base.keys_view(), key, minor, numerator, false);
            lemma_routed_support(data, data.base.keys_view(), key, minor, numerator, true);
            assert(rows[index].count == 0 && rows[index].volume == 0);
        }
        lemma_routed_measures(data, data.base.keys_view(), key, minor, numerator);
        if rows[index].count == 0 { assert(rows[index].volume == 0); }
    }
}

}
