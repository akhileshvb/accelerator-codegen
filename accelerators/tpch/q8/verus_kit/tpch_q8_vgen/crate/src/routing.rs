use vstd::prelude::*;
use verus_common::sums::*;
use crate::table::*;
use crate::num::Num;

verus! {

#[verifier::opaque]
pub open spec fn region_effect(rows: Seq<Cell>, nation: i64, count: int, key: Major, minor: Major, measure: bool) -> int {
    sum(rows, |region: Cell| if key == (nation, 0, 0) && minor == (region.first, 0, 0) && !measure {
        count * region.count@
    } else { 0 })
}

#[verifier::opaque]
pub open spec fn customer_nation_effect(rows: Seq<Cell>, regions: Table, nation: i64, sign: int, key: Major, minor: Major, measure: bool) -> int {
    sum(rows, |row: Cell| region_effect(regions.rows((row.second, 0, 0)), nation, sign * row.count@, key, minor, measure))
}

#[verifier::opaque]
pub open spec fn supplier_nation_effect(rows: Seq<Cell>, nation: i64, sign: int, key: Major, minor: Major, measure: bool) -> int {
    sum(rows, |row: Cell| if key == (nation, 0, 0) && minor == (row.first, 0, 0) && !measure { sign * row.count@ } else { 0 })
}

#[verifier::opaque]
pub open spec fn all_customer_routes(keys: Seq<Major>, nations: Table, regions: Table, key: Major, minor: Major, measure: bool) -> int {
    sum(keys, |entry: Major| customer_nation_effect(nations.rows((entry.0, 0, 0)), regions, entry.0, 1, key, minor, measure))
}

#[verifier::opaque]
pub open spec fn all_supplier_routes(keys: Seq<Major>, nations: Table, key: Major, minor: Major, measure: bool) -> int {
    sum(keys, |entry: Major| supplier_nation_effect(nations.rows((entry.0, 0, 0)), entry.0, 1, key, minor, measure))
}

#[verifier::opaque]
pub open spec fn customer_route_changes(keys: Seq<i64>, nations: Table, regions: Table, key: Major, minor: Major, measure: bool) -> int {
    sum(keys, |nation: i64| customer_nation_effect(nations.rows((nation, 0, 0)), regions, nation, 1, key, minor, measure))
}

#[verifier::opaque]
pub open spec fn supplier_route_changes(keys: Seq<i64>, nations: Table, key: Major, minor: Major, measure: bool) -> int {
    sum(keys, |nation: i64| supplier_nation_effect(nations.rows((nation, 0, 0)), nation, 1, key, minor, measure))
}

pub proof fn lemma_route_sign(nations: Table, regions: Table, nation: i64, key: Major, minor: Major, measure: bool)
    ensures
        customer_nation_effect(nations.rows((nation, 0, 0)), regions, nation, -1, key, minor, measure)
            == -customer_nation_effect(nations.rows((nation, 0, 0)), regions, nation, 1, key, minor, measure),
        supplier_nation_effect(nations.rows((nation, 0, 0)), nation, -1, key, minor, measure)
            == -supplier_nation_effect(nations.rows((nation, 0, 0)), nation, 1, key, minor, measure),
{
    hide(sum);
    reveal(customer_nation_effect);
    reveal(supplier_nation_effect);
    let rows = nations.rows((nation, 0, 0));
    let positive = |row: Cell| region_effect(regions.rows((row.second, 0, 0)), nation, row.count@, key, minor, measure);
    let negative = |row: Cell| region_effect(regions.rows((row.second, 0, 0)), nation, -(row.count@), key, minor, measure);
    assert forall|row: Cell| #[trigger] negative(row) == -positive(row) by {
        reveal(region_effect);
        let weight = |region: Cell| if key == (nation, 0, 0) && minor == (region.first, 0, 0) && !measure {
            row.count@ * region.count@
        } else { 0int };
        let reversed = |region: Cell| if key == (nation, 0, 0) && minor == (region.first, 0, 0) && !measure {
            -(row.count@) * region.count@
        } else { 0int };
        assert forall|region: Cell| #[trigger] reversed(region) == -1 * weight(region) by {
            assert(-(row.count@) * region.count@ == -1 * (row.count@ * region.count@)) by (nonlinear_arith);
        }
        lemma_sum_scale(regions.rows((row.second, 0, 0)), weight, reversed, -1);
    }
    lemma_sum_scale(rows, positive, negative, -1);
    lemma_sum_ext_all(rows, |row: Cell| region_effect(regions.rows((row.second, 0, 0)), nation, -1 * row.count@, key, minor, measure), negative);
    lemma_sum_ext_all(rows, |row: Cell| region_effect(regions.rows((row.second, 0, 0)), nation, 1 * row.count@, key, minor, measure), positive);
    let positive = |row: Cell| if key == (nation, 0, 0) && minor == (row.first, 0, 0) && !measure { row.count@ } else { 0int };
    let negative = |row: Cell| if key == (nation, 0, 0) && minor == (row.first, 0, 0) && !measure { -(row.count@) } else { 0int };
    lemma_sum_scale(rows, positive, negative, -1);
    lemma_sum_ext_all(rows, |row: Cell| if key == (nation, 0, 0) && minor == (row.first, 0, 0) && !measure { -1 * row.count@ } else { 0int }, negative);
    lemma_sum_ext_all(rows, |row: Cell| if key == (nation, 0, 0) && minor == (row.first, 0, 0) && !measure { 1 * row.count@ } else { 0int }, positive);
}

pub fn maintain_nation_routes(changed: &crate::maintenance::Keys, old_nations: &Table, old_regions: &Table,
    nations: &Table, regions: &Table, customer_index: &Table, supplier_index: &Table,
    customer_routes: &mut Work, supplier_routes: &mut Work, affected_pairs: &mut crate::maintenance::Pairs)
    requires old_nations.valid(), old_regions.valid(), nations.valid(), regions.valid(), customer_index.valid(), supplier_index.valid(),
        old(customer_routes).valid(), old(supplier_routes).valid(), changed.valid(), old(affected_pairs).valid(),
    ensures final(customer_routes).valid() && final(supplier_routes).valid(),
        final(affected_pairs).valid(),
        forall|pair: Major| #[trigger] old(affected_pairs).has(pair) ==> final(affected_pairs).has(pair),
        crate::maintenance::reverse_pairs_covered(*final(affected_pairs), *changed, *customer_index, changed.values@.len() as int, false),
        crate::maintenance::reverse_pairs_covered(*final(affected_pairs), *changed, *supplier_index, changed.values@.len() as int, true),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(customer_routes).at(key, minor, measure)
            == old(customer_routes).at(key, minor, measure)
                + customer_route_changes(changed.values@, *nations, *regions, key, minor, measure)
                - customer_route_changes(changed.values@, *old_nations, *old_regions, key, minor, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(supplier_routes).at(key, minor, measure)
            == old(supplier_routes).at(key, minor, measure)
                + supplier_route_changes(changed.values@, *nations, key, minor, measure)
                - supplier_route_changes(changed.values@, *old_nations, key, minor, measure),
{
    let mut index = 0;
    let ghost initial_customer_routes = *customer_routes;
    let ghost initial_supplier_routes = *supplier_routes;
    let ghost initial_pairs = *affected_pairs;
    proof {
        reveal(crate::routing::customer_route_changes);
        reveal(crate::routing::supplier_route_changes);
    }
    while index < changed.values.len()
        invariant index <= changed.values.len(), nations.valid(), regions.valid(), old_nations.valid(), old_regions.valid(),
            customer_index.valid(), supplier_index.valid(), customer_routes.valid(), supplier_routes.valid(),
            affected_pairs.valid(),
            forall|pair: Major| #[trigger] initial_pairs.has(pair) ==> affected_pairs.has(pair),
            crate::maintenance::reverse_pairs_covered(*affected_pairs, *changed, *customer_index, index as int, false),
            crate::maintenance::reverse_pairs_covered(*affected_pairs, *changed, *supplier_index, index as int, true),
            forall|key: Major, minor: Major, measure: bool| #[trigger] customer_routes.at(key, minor, measure)
                == initial_customer_routes.at(key, minor, measure)
                    + crate::routing::customer_route_changes(changed.values@.take(index as int), *nations, *regions, key, minor, measure)
                    - crate::routing::customer_route_changes(changed.values@.take(index as int), *old_nations, *old_regions, key, minor, measure),
            forall|key: Major, minor: Major, measure: bool| #[trigger] supplier_routes.at(key, minor, measure)
                == initial_supplier_routes.at(key, minor, measure)
                    + crate::routing::supplier_route_changes(changed.values@.take(index as int), *nations, key, minor, measure)
                    - crate::routing::supplier_route_changes(changed.values@.take(index as int), *old_nations, key, minor, measure),
        decreases changed.values.len() - index,
    {
        let nation = changed.values[index];
        
        route_nation(old_nations, old_regions, nation, customer_routes, supplier_routes, -1);
        route_nation(nations, regions, nation, customer_routes, supplier_routes, 1);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] customer_routes.at(key, minor, measure)
                == initial_customer_routes.at(key, minor, measure)
                    + crate::routing::customer_route_changes(changed.values@.take(index as int + 1), *nations, *regions, key, minor, measure)
                    - crate::routing::customer_route_changes(changed.values@.take(index as int + 1), *old_nations, *old_regions, key, minor, measure) by {
                crate::routing::lemma_route_sign(*old_nations, *old_regions, nation, key, minor, measure);
                reveal(crate::routing::customer_route_changes);
                lemma_sum_take(changed.values@, index as int, |nation: i64|
                    crate::routing::customer_nation_effect(nations.rows((nation, 0, 0)), *regions, nation, 1, key, minor, measure));
                lemma_sum_take(changed.values@, index as int, |nation: i64|
                    crate::routing::customer_nation_effect(old_nations.rows((nation, 0, 0)), *old_regions, nation, 1, key, minor, measure));
            }
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] supplier_routes.at(key, minor, measure)
                == initial_supplier_routes.at(key, minor, measure)
                    + crate::routing::supplier_route_changes(changed.values@.take(index as int + 1), *nations, key, minor, measure)
                    - crate::routing::supplier_route_changes(changed.values@.take(index as int + 1), *old_nations, key, minor, measure) by {
                crate::routing::lemma_route_sign(*old_nations, *old_regions, nation, key, minor, measure);
                reveal(crate::routing::supplier_route_changes);
                lemma_sum_take(changed.values@, index as int, |nation: i64|
                    crate::routing::supplier_nation_effect(nations.rows((nation, 0, 0)), nation, 1, key, minor, measure));
                lemma_sum_take(changed.values@, index as int, |nation: i64|
                    crate::routing::supplier_nation_effect(old_nations.rows((nation, 0, 0)), nation, 1, key, minor, measure));
            }
        }
        let ghost before_pairs = *affected_pairs;
        crate::maintenance::include_nation_pairs(customer_index, nation, false, affected_pairs);
        crate::maintenance::include_nation_pairs(supplier_index, nation, true, affected_pairs);
        proof {
            crate::maintenance::lemma_reverse_pairs_step(before_pairs, *affected_pairs, *changed, *customer_index, index as int, false);
            crate::maintenance::lemma_reverse_pairs_step(before_pairs, *affected_pairs, *changed, *supplier_index, index as int, true);
        }
        index += 1;
    }
    proof { assert(changed.values@.take(changed.values@.len() as int) =~= changed.values@); }
}

fn add_regions_to_nation(rows: &[Cell], nation: i64, count: &Num, output: &mut Work)
    requires old(output).valid(),
    ensures final(output).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(output).at(key, minor, measure)
            == old(output).at(key, minor, measure) + region_effect(rows@, nation, count@, key, minor, measure),
{
    let ghost initial = *output;
    let mut index = 0;
    proof { reveal(region_effect); }
    while index < rows.len()
        invariant index <= rows.len(), output.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + region_effect(rows@.take(index as int), nation, count@, key, minor, measure),
        decreases rows.len() - index,
    {
        let region = &rows[index];
        output.add((nation, 0, 0), (region.first, 0, 0), count.mul(&region.count), Num::zero());
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] output.at(key, minor, measure)
                == initial.at(key, minor, measure) + region_effect(rows@.take(index as int + 1), nation, count@, key, minor, measure) by {
                reveal(region_effect);
                lemma_sum_take(rows@, index as int, |region: Cell| if key == (nation, 0, 0) && minor == (region.first, 0, 0) && !measure {
                    count@ * region.count@
                } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
}

pub fn route_nation(nations: &Table, regions: &Table, nation: i64, customer_routes: &mut Work, supplier_routes: &mut Work, sign: i128)
    requires nations.valid(), regions.valid(), old(customer_routes).valid(), old(supplier_routes).valid(),
    ensures final(customer_routes).valid() && final(supplier_routes).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(customer_routes).at(key, minor, measure)
            == old(customer_routes).at(key, minor, measure) + customer_nation_effect(nations.rows((nation, 0, 0)), *regions, nation, sign as int, key, minor, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(supplier_routes).at(key, minor, measure)
            == old(supplier_routes).at(key, minor, measure) + supplier_nation_effect(nations.rows((nation, 0, 0)), nation, sign as int, key, minor, measure),
{
    let rows = nations.get((nation, 0, 0));
    let ghost initial_customer = *customer_routes;
    let ghost initial_supplier = *supplier_routes;
    let mut index = 0;
    proof { reveal(customer_nation_effect); reveal(supplier_nation_effect); }
    while index < rows.len()
        invariant index <= rows.len(), regions.valid(), customer_routes.valid(), supplier_routes.valid(),
            (*rows)@ == nations.rows((nation, 0, 0)),
            forall|key: Major, minor: Major, measure: bool| #[trigger] customer_routes.at(key, minor, measure)
                == initial_customer.at(key, minor, measure) + customer_nation_effect((*rows)@.take(index as int), *regions, nation, sign as int, key, minor, measure),
            forall|key: Major, minor: Major, measure: bool| #[trigger] supplier_routes.at(key, minor, measure)
                == initial_supplier.at(key, minor, measure) + supplier_nation_effect((*rows)@.take(index as int), nation, sign as int, key, minor, measure),
        decreases rows.len() - index,
    {
        let row = &rows[index];
        let count = Num::Small(sign).mul(&row.count);
        supplier_routes.add((nation, 0, 0), (row.first, 0, 0), count.dup(), Num::zero());
        let neighbors = regions.get((row.second, 0, 0));
        add_regions_to_nation(neighbors.as_slice(), nation, &count, customer_routes);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] customer_routes.at(key, minor, measure)
                == initial_customer.at(key, minor, measure) + customer_nation_effect((*rows)@.take(index as int + 1), *regions, nation, sign as int, key, minor, measure) by {
                reveal(customer_nation_effect);
                lemma_sum_take((*rows)@, index as int, |row: Cell|
                    region_effect(regions.rows((row.second, 0, 0)), nation, sign as int * row.count@, key, minor, measure));
            }
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] supplier_routes.at(key, minor, measure)
                == initial_supplier.at(key, minor, measure) + supplier_nation_effect((*rows)@.take(index as int + 1), nation, sign as int, key, minor, measure) by {
                reveal(supplier_nation_effect);
                lemma_sum_take((*rows)@, index as int, |row: Cell| if key == (nation, 0, 0) && minor == (row.first, 0, 0) && !measure {
                    sign as int * row.count@
                } else { 0 });
            }
        }
        index += 1;
    }
    proof { assert((*rows)@.take((*rows)@.len() as int) =~= (*rows)@); }
}

#[verifier::opaque]
pub open spec fn nation_pair_effect(rows: Seq<Cell>, group: Cell, region: Cell, sign: int, key: Major, minor: Major, numerator: bool, measure: bool) -> int {
    sum(rows, |nation: Cell| pair_weight(nation, group, region, sign, key, minor, numerator, measure))
}

pub open spec fn pair_weight(nation: Cell, group: Cell, region: Cell, sign: int, key: Major, minor: Major, numerator: bool, measure: bool) -> int {
    if key == (region.first, group.first, if numerator { nation.first } else { 0 }) && minor == (group.second, 0, 0) {
        (if measure { group.volume@ } else { group.count@ }) * (sign * region.count@ * nation.count@)
    } else { 0 }
}

proof fn lemma_pair_prefix(rows: Seq<Cell>, index: int, group: Cell, region: Cell, sign: int, key: Major, minor: Major, numerator: bool, measure: bool)
    requires 0 <= index < rows.len(),
    ensures nation_pair_effect(rows.take(index + 1), group, region, sign, key, minor, numerator, measure)
        == nation_pair_effect(rows.take(index), group, region, sign, key, minor, numerator, measure)
            + pair_weight(rows[index], group, region, sign, key, minor, numerator, measure),
{
    reveal(nation_pair_effect);
    lemma_sum_take(rows, index, |nation: Cell| pair_weight(nation, group, region, sign, key, minor, numerator, measure));
}

#[verifier::opaque]
pub open spec fn region_pair_effect(rows: Seq<Cell>, nations: Seq<Cell>, group: Cell, sign: int, key: Major, minor: Major, numerator: bool, measure: bool) -> int {
    sum(rows, |region: Cell| nation_pair_effect(nations, group, region, sign, key, minor, numerator, measure))
}

#[verifier::opaque]
pub open spec fn group_pair_effect(rows: Seq<Cell>, regions: Seq<Cell>, nations: Seq<Cell>, sign: int, key: Major, minor: Major, numerator: bool, measure: bool) -> int {
    sum(rows, |group: Cell| region_pair_effect(regions, nations, group, sign, key, minor, numerator, measure))
}

pub open spec fn route_key_effect(pair: Major, base: Table, customer_routes: Table, supplier_routes: Table, sign: int, key: Major, minor: Major, numerator: bool, measure: bool) -> int {
    group_pair_effect(base.rows(pair), customer_routes.rows((pair.0, 0, 0)), supplier_routes.rows((pair.1, 0, 0)), sign, key, minor, numerator, measure)
}

#[verifier::opaque]
pub open spec fn routed_effect(keys: Seq<Major>, base: Table, customer_routes: Table, supplier_routes: Table, sign: int, key: Major, minor: Major, numerator: bool, measure: bool) -> int {
    sum(keys, |pair: Major| route_key_effect(pair, base, customer_routes, supplier_routes, sign, key, minor, numerator, measure))
}

pub fn maintain_pair_routes(pairs: &crate::maintenance::Pairs, old_base: &Table, old_customer_routes: &Table, old_supplier_routes: &Table,
    base: &Table, customer_routes: &Table, supplier_routes: &Table, totals: &mut Work, numerators: &mut Work)
    requires old_base.valid(), old_customer_routes.valid(), old_supplier_routes.valid(),
        base.valid(), customer_routes.valid(), supplier_routes.valid(), old(totals).valid(), old(numerators).valid(),
    ensures final(totals).valid() && final(numerators).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(totals).at(key, minor, measure)
            == old(totals).at(key, minor, measure)
                + routed_effect(pairs.values@, *old_base, *old_customer_routes, *old_supplier_routes, -1, key, minor, false, measure)
                + routed_effect(pairs.values@, *base, *customer_routes, *supplier_routes, 1, key, minor, false, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(numerators).at(key, minor, measure)
            == old(numerators).at(key, minor, measure)
                + routed_effect(pairs.values@, *old_base, *old_customer_routes, *old_supplier_routes, -1, key, minor, true, measure)
                + routed_effect(pairs.values@, *base, *customer_routes, *supplier_routes, 1, key, minor, true, measure),
{
    let ghost initial_totals = *totals;
    let ghost initial_numerators = *numerators;
    let mut index = 0;
    proof { reveal(routed_effect); }
    while index < pairs.values.len()
        invariant index <= pairs.values.len(),
            old_base.valid(), old_customer_routes.valid(), old_supplier_routes.valid(),
            base.valid(), customer_routes.valid(), supplier_routes.valid(), totals.valid(), numerators.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] totals.at(key, minor, measure)
                == initial_totals.at(key, minor, measure)
                    + routed_effect(pairs.values@.take(index as int), *old_base, *old_customer_routes, *old_supplier_routes, -1, key, minor, false, measure)
                    + routed_effect(pairs.values@.take(index as int), *base, *customer_routes, *supplier_routes, 1, key, minor, false, measure),
            forall|key: Major, minor: Major, measure: bool| #[trigger] numerators.at(key, minor, measure)
                == initial_numerators.at(key, minor, measure)
                    + routed_effect(pairs.values@.take(index as int), *old_base, *old_customer_routes, *old_supplier_routes, -1, key, minor, true, measure)
                    + routed_effect(pairs.values@.take(index as int), *base, *customer_routes, *supplier_routes, 1, key, minor, true, measure),
        decreases pairs.values.len() - index,
    {
        let pair = pairs.values[index];
        route_pair(old_base, old_customer_routes, old_supplier_routes, pair, -1, totals, numerators);
        route_pair(base, customer_routes, supplier_routes, pair, 1, totals, numerators);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] totals.at(key, minor, measure)
                == initial_totals.at(key, minor, measure)
                    + routed_effect(pairs.values@.take(index as int + 1), *old_base, *old_customer_routes, *old_supplier_routes, -1, key, minor, false, measure)
                    + routed_effect(pairs.values@.take(index as int + 1), *base, *customer_routes, *supplier_routes, 1, key, minor, false, measure) by {
                reveal(routed_effect);
                lemma_sum_take(pairs.values@, index as int, |pair: Major| route_key_effect(pair, *old_base, *old_customer_routes, *old_supplier_routes, -1, key, minor, false, measure));
                lemma_sum_take(pairs.values@, index as int, |pair: Major| route_key_effect(pair, *base, *customer_routes, *supplier_routes, 1, key, minor, false, measure));
            }
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] numerators.at(key, minor, measure)
                == initial_numerators.at(key, minor, measure)
                    + routed_effect(pairs.values@.take(index as int + 1), *old_base, *old_customer_routes, *old_supplier_routes, -1, key, minor, true, measure)
                    + routed_effect(pairs.values@.take(index as int + 1), *base, *customer_routes, *supplier_routes, 1, key, minor, true, measure) by {
                reveal(routed_effect);
                lemma_sum_take(pairs.values@, index as int, |pair: Major| route_key_effect(pair, *old_base, *old_customer_routes, *old_supplier_routes, -1, key, minor, true, measure));
                lemma_sum_take(pairs.values@, index as int, |pair: Major| route_key_effect(pair, *base, *customer_routes, *supplier_routes, 1, key, minor, true, measure));
            }
        }
        index += 1;
    }
    proof { assert(pairs.values@.take(pairs.values@.len() as int) =~= pairs.values@); }
}

fn add_nations_to_pair(rows: &[Cell], group: &Cell, region: &Cell, sign: i128, totals: &mut Work, numerators: &mut Work)
    requires old(totals).valid(), old(numerators).valid(),
    ensures final(totals).valid() && final(numerators).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(totals).at(key, minor, measure)
            == old(totals).at(key, minor, measure) + nation_pair_effect(rows@, *group, *region, sign as int, key, minor, false, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(numerators).at(key, minor, measure)
            == old(numerators).at(key, minor, measure) + nation_pair_effect(rows@, *group, *region, sign as int, key, minor, true, measure),
{
    let ghost initial_total = *totals;
    let ghost initial_numerator = *numerators;
    let mut index = 0;
    proof { reveal(nation_pair_effect); }
    while index < rows.len()
        invariant index <= rows.len(), totals.valid(), numerators.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] totals.at(key, minor, measure)
                == initial_total.at(key, minor, measure) + nation_pair_effect(rows@.take(index as int), *group, *region, sign as int, key, minor, false, measure),
            forall|key: Major, minor: Major, measure: bool| #[trigger] numerators.at(key, minor, measure)
                == initial_numerator.at(key, minor, measure) + nation_pair_effect(rows@.take(index as int), *group, *region, sign as int, key, minor, true, measure),
        decreases rows.len() - index,
    {
        let nation = &rows[index];
        let weight = Num::Small(sign).mul(&region.count).mul(&nation.count);
        let count = group.count.mul(&weight);
        let volume = group.volume.mul(&weight);
        proof {
            assert(weight@ == sign as int * region.count@ * nation.count@);
            assert(count@ == group.count@ * (sign as int * region.count@ * nation.count@));
            assert(volume@ == group.volume@ * (sign as int * region.count@ * nation.count@));
        }
        totals.add((region.first, group.first, 0), (group.second, 0, 0), count.dup(), volume.dup());
        numerators.add((region.first, group.first, nation.first), (group.second, 0, 0), count, volume);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] totals.at(key, minor, measure)
                == initial_total.at(key, minor, measure) + nation_pair_effect(rows@.take(index as int + 1), *group, *region, sign as int, key, minor, false, measure) by {
                lemma_pair_prefix(rows@, index as int, *group, *region, sign as int, key, minor, false, measure);
            }
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] numerators.at(key, minor, measure)
                == initial_numerator.at(key, minor, measure) + nation_pair_effect(rows@.take(index as int + 1), *group, *region, sign as int, key, minor, true, measure) by {
                lemma_pair_prefix(rows@, index as int, *group, *region, sign as int, key, minor, true, measure);
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
}

fn add_regions_to_pair(rows: &[Cell], nations: &[Cell], group: &Cell, sign: i128, totals: &mut Work, numerators: &mut Work)
    requires old(totals).valid(), old(numerators).valid(),
    ensures final(totals).valid() && final(numerators).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(totals).at(key, minor, measure)
            == old(totals).at(key, minor, measure) + region_pair_effect(rows@, nations@, *group, sign as int, key, minor, false, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(numerators).at(key, minor, measure)
            == old(numerators).at(key, minor, measure) + region_pair_effect(rows@, nations@, *group, sign as int, key, minor, true, measure),
{
    let ghost initial_total = *totals;
    let ghost initial_numerator = *numerators;
    let mut index = 0;
    proof { reveal(region_pair_effect); }
    while index < rows.len()
        invariant index <= rows.len(), totals.valid(), numerators.valid(),
            forall|key: Major, minor: Major, measure: bool| #[trigger] totals.at(key, minor, measure)
                == initial_total.at(key, minor, measure) + region_pair_effect(rows@.take(index as int), nations@, *group, sign as int, key, minor, false, measure),
            forall|key: Major, minor: Major, measure: bool| #[trigger] numerators.at(key, minor, measure)
                == initial_numerator.at(key, minor, measure) + region_pair_effect(rows@.take(index as int), nations@, *group, sign as int, key, minor, true, measure),
        decreases rows.len() - index,
    {
        add_nations_to_pair(nations, group, &rows[index], sign, totals, numerators);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] totals.at(key, minor, measure)
                == initial_total.at(key, minor, measure) + region_pair_effect(rows@.take(index as int + 1), nations@, *group, sign as int, key, minor, false, measure) by {
                reveal(region_pair_effect);
                lemma_sum_take(rows@, index as int, |region: Cell| nation_pair_effect(nations@, *group, region, sign as int, key, minor, false, measure));
            }
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] numerators.at(key, minor, measure)
                == initial_numerator.at(key, minor, measure) + region_pair_effect(rows@.take(index as int + 1), nations@, *group, sign as int, key, minor, true, measure) by {
                reveal(region_pair_effect);
                lemma_sum_take(rows@, index as int, |region: Cell| nation_pair_effect(nations@, *group, region, sign as int, key, minor, true, measure));
            }
        }
        index += 1;
    }
    proof { assert(rows@.take(rows@.len() as int) =~= rows@); }
}

pub fn route_pair(base: &Table, customer_routes: &Table, supplier_routes: &Table, pair: Major, sign: i128, totals: &mut Work, numerators: &mut Work)
    requires base.valid(), customer_routes.valid(), supplier_routes.valid(), old(totals).valid(), old(numerators).valid(),
    ensures final(totals).valid() && final(numerators).valid(),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(totals).at(key, minor, measure)
            == old(totals).at(key, minor, measure) + group_pair_effect(base.rows(pair), customer_routes.rows((pair.0, 0, 0)), supplier_routes.rows((pair.1, 0, 0)), sign as int, key, minor, false, measure),
        forall|key: Major, minor: Major, measure: bool| #[trigger] final(numerators).at(key, minor, measure)
            == old(numerators).at(key, minor, measure) + group_pair_effect(base.rows(pair), customer_routes.rows((pair.0, 0, 0)), supplier_routes.rows((pair.1, 0, 0)), sign as int, key, minor, true, measure),
{
    let groups = base.get(pair);
    let regions = customer_routes.get((pair.0, 0, 0));
    let nations = supplier_routes.get((pair.1, 0, 0));
    let ghost initial_total = *totals;
    let ghost initial_numerator = *numerators;
    let mut index = 0;
    proof { reveal(group_pair_effect); }
    while index < groups.len()
        invariant index <= groups.len(), totals.valid(), numerators.valid(),
            (*groups)@ == base.rows(pair), (*regions)@ == customer_routes.rows((pair.0, 0, 0)), (*nations)@ == supplier_routes.rows((pair.1, 0, 0)),
            forall|key: Major, minor: Major, measure: bool| #[trigger] totals.at(key, minor, measure)
                == initial_total.at(key, minor, measure) + group_pair_effect((*groups)@.take(index as int), (*regions)@, (*nations)@, sign as int, key, minor, false, measure),
            forall|key: Major, minor: Major, measure: bool| #[trigger] numerators.at(key, minor, measure)
                == initial_numerator.at(key, minor, measure) + group_pair_effect((*groups)@.take(index as int), (*regions)@, (*nations)@, sign as int, key, minor, true, measure),
        decreases groups.len() - index,
    {
        add_regions_to_pair(regions.as_slice(), nations.as_slice(), &groups[index], sign, totals, numerators);
        proof {
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] totals.at(key, minor, measure)
                == initial_total.at(key, minor, measure) + group_pair_effect((*groups)@.take(index as int + 1), (*regions)@, (*nations)@, sign as int, key, minor, false, measure) by {
                reveal(group_pair_effect);
                lemma_sum_take((*groups)@, index as int, |group: Cell| region_pair_effect((*regions)@, (*nations)@, group, sign as int, key, minor, false, measure));
            }
            assert forall|key: Major, minor: Major, measure: bool| #[trigger] numerators.at(key, minor, measure)
                == initial_numerator.at(key, minor, measure) + group_pair_effect((*groups)@.take(index as int + 1), (*regions)@, (*nations)@, sign as int, key, minor, true, measure) by {
                reveal(group_pair_effect);
                lemma_sum_take((*groups)@, index as int, |group: Cell| region_pair_effect((*regions)@, (*nations)@, group, sign as int, key, minor, true, measure));
            }
        }
        index += 1;
    }
    proof { assert((*groups)@.take((*groups)@.len() as int) =~= (*groups)@); }
}

}
