use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::{lemma_flat_map_contains, lemma_positive_product};
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::bags::*;
use crate::storage::*;
use crate::maintenance::*;
use crate::relational::*;
use crate::output::*;
use crate::query_transfer::lemma_quote_entries;

verus! {

pub(crate) open spec fn key_costs(state: State, regions: Directory<Count>, key: i64) -> Seq<int> {
    supported_costs(*state.quotes.lookup(key)->0, state.suppliers, state.nations, regions)
}

pub(crate) open spec fn nation_has_support(state: State, regions: Directory<Count>, key: i64) -> bool {
    match state.nations.lookup(key) { None => false, Some(bag) => nation_support(*bag, regions) }
}

pub(crate) open spec fn supplier_has_support(state: State, regions: Directory<Count>, key: i64) -> bool {
    match state.suppliers.lookup(key) { None => false, Some(bag) => supplier_bag_support(*bag, state.nations, regions) }
}

pub(crate) proof fn lemma_costs_mapper(state: State, regions: Directory<Count>, key: i64, mapper: spec_fn(Counted<BrassPartsuppScan>) -> Seq<int>)
    requires state.quotes.lookup(key) is Some,
        forall|entry: Counted<BrassPartsuppScan>| #[trigger] mapper(entry) == if entry.count > 0 && supported(*entry.row, state.suppliers, state.nations, regions) { seq![(*entry.row).ps_supplycost as int] } else { Seq::<int>::empty() },
    ensures key_costs(state, regions, key) == flat((state.quotes.lookup(key)->0).entries@.map_values(mapper)),
{
    assert((|entry: Counted<BrassPartsuppScan>| if entry.count > 0 && supported(*entry.row, state.suppliers, state.nations, regions) { seq![(*entry.row).ps_supplycost as int] } else { Seq::<int>::empty() }) =~= mapper);
}

pub(crate) proof fn lemma_cost_bounds(state: State, regions: Directory<Count>, key: i64, length: int)
    requires state.quotes.lookup(key) is Some,
        0 < length <= supported_costs(*state.quotes.lookup(key)->0, state.suppliers, state.nations, regions).len(),
    ensures ({ let prefix = supported_costs(*state.quotes.lookup(key)->0, state.suppliers, state.nations, regions).take(length);
        prefix.contains(min_of(prefix)) && forall|index: int| 0 <= index < prefix.len() ==> min_of(prefix) <= #[trigger] prefix[index] }),
    decreases length,
{
    let costs = key_costs(state, regions, key);
    let prefix = costs.take(length);
    if length == 1 { assert(prefix.contains(prefix[0])); }
    else {
        lemma_cost_bounds(state, regions, key, length - 1);
        assert(prefix.drop_last() =~= costs.take(length - 1));
        if min_of(prefix) == prefix.last() { assert(prefix.contains(prefix.last())); }
        else {
            let index = choose|index: int| 0 <= index < prefix.drop_last().len() && #[trigger] prefix.drop_last()[index] == min_of(prefix.drop_last());
            assert(prefix[index] == min_of(prefix));
            assert(prefix.contains(min_of(prefix)));
        }
        assert forall|index: int| 0 <= index < prefix.len() implies min_of(prefix) <= #[trigger] prefix[index] by {
            if index < length - 1 { assert(prefix[index] == prefix.drop_last()[index]); }
        }
    }
}

pub proof fn lemma_scalar_bounds(db: Db, p: Params, part: PartRow, length: int)
    requires 0 < length <= scalar_1_group_vals(db, p, part, |t: Scalar1Tuple| t.partsupp_2.ps_supplycost).len(),
    ensures ({ let prefix = scalar_1_group_vals(db, p, part, |t: Scalar1Tuple| t.partsupp_2.ps_supplycost).take(length);
        prefix.contains(min_of(prefix)) && forall|index: int| 0 <= index < prefix.len() ==> min_of(prefix) <= #[trigger] prefix[index] }),
    decreases length,
{
    let values = scalar_1_group_vals(db, p, part, |t: Scalar1Tuple| t.partsupp_2.ps_supplycost);
    let prefix = values.take(length);
    if length == 1 { assert(prefix.contains(prefix[0])); }
    else {
        lemma_scalar_bounds(db, p, part, length - 1);
        assert(prefix.drop_last() =~= values.take(length - 1));
        if min_of(prefix) == prefix.last() { assert(prefix.contains(prefix.last())); }
        else {
            let index = choose|index: int| 0 <= index < prefix.drop_last().len() && #[trigger] prefix.drop_last()[index] == min_of(prefix.drop_last());
            assert(prefix[index] == min_of(prefix));
            assert(prefix.contains(min_of(prefix)));
        }
        assert forall|index: int| 0 <= index < prefix.len() implies min_of(prefix) <= #[trigger] prefix[index] by {
            if index < length - 1 { assert(prefix[index] == prefix.drop_last()[index]); }
        }
    }
}

pub(crate) proof fn lemma_regions_nonnegative(state: State, regions: Directory<Count>, name: Seq<u8>)
    requires state.meaning(), state.db() == state.database@,
        forall|key: i64| #[trigger] count_at(regions, key) == region_amount(state.regions, name, key),
    ensures forall|key: i64| #[trigger] count_at(regions, key) >= 0,
{
    assert forall|key: i64| #[trigger] count_at(regions, key) >= 0 by {
        lemma_sum_nonneg(state.db().region, |row: RegionRow| if row.r_name == name && row.r_regionkey == key as int { 1int } else { 0int });
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_nation_multiplicity(state: State, regions: Directory<Count>, key: i64)
    requires state.meaning(), forall|region: i64| #[trigger] count_at(regions, region) >= 0,
    ensures index_nation(state, regions, key, |nation: NationRow| 1int) >= 0,
        (index_nation(state, regions, key, |nation: NationRow| 1int) > 0) ==
            match state.nations.lookup(key) { None => false, Some(bag) => nation_support(*bag, regions) },
{
    if state.nations.lookup(key) is Some {
        let bag = state.nations.lookup(key)->0;
        let raw = |entry: Counted<NationScan>| entry.count as int * count_at(regions, (*entry.row).n_regionkey);
        assert((|entry: Counted<NationScan>| entry.count as int * (|nation: NationRow| count_at(regions, nation.n_regionkey as i64) * (|nation: NationRow| 1int)(nation))((*entry.row).rv())) =~= raw);
        assert(index_nation(state, regions, key, |nation: NationRow| 1int) == sum((*bag).entries@, raw));
        assert forall|index: int| 0 <= index < (*bag).entries@.len() implies raw(#[trigger] (*bag).entries@[index]) >= 0 by {
            let entry = (*bag).entries@[index];
            assert(raw(entry) >= 0) by (nonlinear_arith)
                requires entry.count > 0, count_at(regions, (*entry.row).n_regionkey) >= 0,
                    raw(entry) == entry.count as int * count_at(regions, (*entry.row).n_regionkey);
        }
        lemma_sum_nonneg((*bag).entries@, raw);
        let has_support = nation_has_support(state, regions, key);
        if has_support {
            let index = choose|index: int| 0 <= index < (*bag).entries@.len() &&
                (#[trigger] (*bag).entries@[index]).count > 0 && count_at(regions, (*(*bag).entries@[index].row).n_regionkey) > 0;
            lemma_positive_product((*bag).entries@[index].count as int, count_at(regions, (*(*bag).entries@[index].row).n_regionkey));
            lemma_sum_pos((*bag).entries@, raw, index);
        } else {
            assert forall|index: int| 0 <= index < (*bag).entries@.len() implies raw(#[trigger] (*bag).entries@[index]) == 0 by {
                assert(count_at(regions, (*(*bag).entries@[index].row).n_regionkey) == 0);
            }
            lemma_sum_zero((*bag).entries@, raw);
        }
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_supplier_multiplicity(state: State, regions: Directory<Count>, key: i64)
    requires state.meaning(), forall|region: i64| #[trigger] count_at(regions, region) >= 0,
    ensures index_supplier(state, regions, key, |supplier: SupplierRow, nation: NationRow| 1int) >= 0,
        (index_supplier(state, regions, key, |supplier: SupplierRow, nation: NationRow| 1int) > 0) ==
            match state.suppliers.lookup(key) { None => false, Some(bag) => supplier_bag_support(*bag, state.nations, regions) },
{
    if state.suppliers.lookup(key) is Some {
        let bag = state.suppliers.lookup(key)->0;
        let raw = |entry: Counted<SupplierScan>| entry.count as int * index_nation(state, regions, (*entry.row).s_nationkey, |nation: NationRow| 1int);
        assert forall|entry: Counted<SupplierScan>| (#[trigger] raw(entry)) == entry.count as int *
            (|supplier: SupplierRow| index_nation(state, regions, supplier.s_nationkey as i64, |nation: NationRow| (|supplier: SupplierRow, nation: NationRow| 1int)(supplier, nation)))((*entry.row).rv()) by {
            assert((|nation: NationRow| (|supplier: SupplierRow, nation: NationRow| 1int)((*entry.row).rv(), nation)) =~= (|nation: NationRow| 1int));
        }
        assert((|entry: Counted<SupplierScan>| entry.count as int * (|supplier: SupplierRow| index_nation(state, regions, supplier.s_nationkey as i64, |nation: NationRow| (|supplier: SupplierRow, nation: NationRow| 1int)(supplier, nation)))((*entry.row).rv())) =~= raw);
        assert(index_supplier(state, regions, key, |supplier: SupplierRow, nation: NationRow| 1int) == sum((*bag).entries@, raw));
        assert forall|index: int| 0 <= index < (*bag).entries@.len() implies raw(#[trigger] (*bag).entries@[index]) >= 0 by {
            lemma_nation_multiplicity(state, regions, (*(*bag).entries@[index].row).s_nationkey);
            let entry = (*bag).entries@[index];
            let amount = index_nation(state, regions, (*entry.row).s_nationkey, |nation: NationRow| 1int);
            assert(raw(entry) >= 0) by (nonlinear_arith)
                requires entry.count > 0, amount >= 0, raw(entry) == entry.count as int * amount;
        }
        lemma_sum_nonneg((*bag).entries@, raw);
        let has_support = supplier_has_support(state, regions, key);
        if has_support {
            let index = choose|index: int| 0 <= index < (*bag).entries@.len() && (#[trigger] (*bag).entries@[index]).count > 0 &&
                supplier_supported(*(*bag).entries@[index].row, state.nations, regions);
            lemma_nation_multiplicity(state, regions, (*(*bag).entries@[index].row).s_nationkey);
            lemma_positive_product((*bag).entries@[index].count as int, index_nation(state, regions, (*(*bag).entries@[index].row).s_nationkey, |nation: NationRow| 1int));
            lemma_sum_pos((*bag).entries@, raw, index);
        } else {
            assert forall|index: int| 0 <= index < (*bag).entries@.len() implies raw(#[trigger] (*bag).entries@[index]) == 0 by {
                lemma_nation_multiplicity(state, regions, (*(*bag).entries@[index].row).s_nationkey);
                assert(!supplier_supported(*(*bag).entries@[index].row, state.nations, regions));
                assert(index_nation(state, regions, (*(*bag).entries@[index].row).s_nationkey, |nation: NationRow| 1int) == 0);
            }
            lemma_sum_zero((*bag).entries@, raw);
        }
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_index_zero(state: State, regions: Directory<Count>, key: i64)
    ensures index_supplier(state, regions, key, |supplier: SupplierRow, nation: NationRow| 0int) == 0,
{
    let raw = |supplier: SupplierRow| index_nation(state, regions, supplier.s_nationkey as i64, |nation: NationRow| 0int);
    assert forall|supplier: SupplierRow| #[trigger] raw(supplier) == 0 by {
        if state.nations.lookup(supplier.s_nationkey as i64) is Some {
            let bag = state.nations.lookup(supplier.s_nationkey as i64)->0;
            lemma_sum_zero((*bag).entries@, |entry: Counted<NationScan>| entry.count as int * (|nation: NationRow| count_at(regions, nation.n_regionkey as i64) * 0int)((*entry.row).rv()));
            assert((|entry: Counted<NationScan>| entry.count as int * (|nation: NationRow| count_at(regions, nation.n_regionkey as i64) * 0int)((*entry.row).rv())) =~=
                (|entry: Counted<NationScan>| entry.count as int * (|nation: NationRow| count_at(regions, nation.n_regionkey as i64) * (|nation: NationRow| 0int)(nation))((*entry.row).rv())));
        }
    }
    if state.suppliers.lookup(key) is Some {
        let bag = state.suppliers.lookup(key)->0;
        lemma_sum_zero((*bag).entries@, |entry: Counted<SupplierScan>| entry.count as int * raw((*entry.row).rv()));
        assert forall|supplier: SupplierRow| #[trigger] raw(supplier) ==
            index_nation(state, regions, supplier.s_nationkey as i64, |nation: NationRow| (|supplier: SupplierRow, nation: NationRow| 0int)(supplier, nation)) by {
            assert((|nation: NationRow| 0int) =~= (|nation: NationRow| (|supplier: SupplierRow, nation: NationRow| 0int)(supplier, nation)));
        }
        assert((|entry: Counted<SupplierScan>| entry.count as int * raw((*entry.row).rv())) =~=
            (|entry: Counted<SupplierScan>| entry.count as int * (|supplier: SupplierRow| index_nation(state, regions, supplier.s_nationkey as i64, |nation: NationRow| (|supplier: SupplierRow, nation: NationRow| 0int)(supplier, nation)))((*entry.row).rv())));
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_cost_supplier(state: State, regions: Directory<Count>, quote: BrassPartsuppScan, cost: int)
    requires state.meaning(), forall|region: i64| #[trigger] count_at(regions, region) >= 0,
    ensures index_supplier(state, regions, quote.ps_suppkey, |supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost as int == cost { 1int } else { 0int }) >= 0,
        (index_supplier(state, regions, quote.ps_suppkey, |supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost as int == cost { 1int } else { 0int }) > 0) ==
            (quote.ps_supplycost as int == cost && supported(quote, state.suppliers, state.nations, regions)),
{
    if quote.ps_supplycost as int == cost {
        assert((|supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost as int == cost { 1int } else { 0int }) =~= (|supplier: SupplierRow, nation: NationRow| 1int));
        lemma_supplier_multiplicity(state, regions, quote.ps_suppkey);
    } else {
        assert((|supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost as int == cost { 1int } else { 0int }) =~= (|supplier: SupplierRow, nation: NationRow| 0int));
        lemma_index_zero(state, regions, quote.ps_suppkey);
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_index_cost_support(state: State, regions: Directory<Count>, key: i64, cost: int)
    requires state.meaning(), state.quotes.lookup(key) is Some,
        forall|region: i64| #[trigger] count_at(regions, region) >= 0,
    ensures index_quote(state, regions, key, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost == cost { 1int } else { 0int }) >= 0,
        (index_quote(state, regions, key, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost == cost { 1int } else { 0int }) > 0) ==
            key_costs(state, regions, key).contains(cost),
{
    let bag = state.quotes.lookup(key)->0;
    let costs = key_costs(state, regions, key);
    let mapper = |entry: Counted<BrassPartsuppScan>| if entry.count > 0 && supported(*entry.row, state.suppliers, state.nations, regions) { seq![(*entry.row).ps_supplycost as int] } else { Seq::<int>::empty() };
    lemma_costs_mapper(state, regions, key, mapper);
    lemma_flat_map_contains((*bag).entries@, mapper, cost);
    let weight = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost == cost { 1int } else { 0int };
    let raw = |entry: Counted<BrassPartsuppScan>| entry.count as int * index_supplier(state, regions, (*entry.row).ps_suppkey, |supplier: SupplierRow, nation: NationRow| weight((*entry.row).rv(), supplier, nation));
    lemma_quote_entries((*bag).entries@,
        |quote: PartsuppRow| index_supplier(state, regions, quote.ps_suppkey as i64, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation)), raw);
    assert(index_quote(state, regions, key, weight) == sum((*bag).entries@, raw));
    assert forall|index: int| 0 <= index < (*bag).entries@.len() implies raw(#[trigger] (*bag).entries@[index]) >= 0 &&
        (raw((*bag).entries@[index]) > 0) == mapper((*bag).entries@[index]).contains(cost) by {
        let entry = (*bag).entries@[index];
        assert(entry.count > 0);
        if supported(*entry.row, state.suppliers, state.nations, regions) {
            assert(mapper(entry) == seq![(*entry.row).ps_supplycost as int]);
            assert(mapper(entry).len() == 1);
            assert(mapper(entry)[0] == (*entry.row).ps_supplycost as int);
            assert(mapper(entry).contains((*entry.row).ps_supplycost as int));
        } else { assert(mapper(entry).len() == 0); }
        lemma_cost_supplier(state, regions, *entry.row, cost);
        assert((|supplier: SupplierRow, nation: NationRow| weight((*entry.row).rv(), supplier, nation)) =~=
            (|supplier: SupplierRow, nation: NationRow| if (*entry.row).ps_supplycost as int == cost { 1int } else { 0int }));
        let amount = index_supplier(state, regions, (*entry.row).ps_suppkey, |supplier: SupplierRow, nation: NationRow| if (*entry.row).ps_supplycost as int == cost { 1int } else { 0int });
        assert(raw(entry) >= 0 && (raw(entry) > 0) == (amount > 0)) by (nonlinear_arith)
            requires entry.count > 0, amount >= 0, raw(entry) == entry.count as int * amount;
        assert((raw(entry) > 0) == mapper(entry).contains(cost));
    }
    lemma_sum_nonneg((*bag).entries@, raw);
    if costs.contains(cost) {
        let index = choose|index: int| 0 <= index < (*bag).entries@.len() && #[trigger] mapper((*bag).entries@[index]).contains(cost);
        lemma_sum_pos((*bag).entries@, raw, index);
    } else { lemma_sum_zero((*bag).entries@, raw); }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_minimum_query(state: State, regions: Directory<Count>, name: Seq<u8>, key: i64, part: PartRow)
    requires state.meaning(), state.db() == state.database@, eligible(state.db(), key) > 0,
        part.p_partkey == key as int, state.quotes.lookup(key) is Some,
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
    ensures scalar_1_value(state.db(), Params { p1: part.p_size, p2: name }, part) ==
        if key_costs(state, regions, key).len() == 0 { None }
        else { Some(min_of(key_costs(state, regions, key))) },
{
    let db = state.db();
    let p = Params { p1: part.p_size, p2: name };
    let costs = key_costs(state, regions, key);
    let values = scalar_1_group_vals(db, p, part, |t: Scalar1Tuple| t.partsupp_2.ps_supplycost);
    lemma_regions_nonnegative(state, regions, name);
    assert forall|cost: int| #[trigger] values.contains(cost) == costs.contains(cost) by {
        let weight = |value: int| if value == cost { 1int } else { 0int };
        let joined = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| weight(quote.ps_supplycost);
        lemma_scalar_weight(db, p, part, weight);
        lemma_index_quote(state, regions, name, key, joined);
        lemma_index_cost_support(state, regions, key, cost);
        assert(joined =~= (|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost == cost { 1int } else { 0int }));
        assert(sum(values, weight) == index_quote(state, regions, key, joined));
        if values.contains(cost) {
            let index = choose|index: int| 0 <= index < values.len() && #[trigger] values[index] == cost;
            lemma_sum_pos(values, weight, index);
        } else { lemma_sum_zero(values, weight); }
    }
    assert(scalar_1_value(db, p, part) == if values.len() > 0 { Some(min_of(values)) } else { None });
    if values.len() == 0 {
        assert(values =~= Seq::<int>::empty());
        if costs.len() > 0 {
            assert(costs.contains(costs[0]));
            assert(values.contains(costs[0]) == costs.contains(costs[0]));
            assert(!values.contains(costs[0]));
        }
        assert(costs.len() == 0);
    } else {
        assert(values.contains(values[0]));
        assert(costs.len() > 0);
        lemma_cost_bounds(state, regions, key, costs.len() as int);
        lemma_scalar_bounds(db, p, part, values.len() as int);
        assert(costs.take(costs.len() as int) =~= costs);
        assert(values.take(values.len() as int) =~= values);
        assert(values.contains(min_of(costs)));
        assert forall|index: int| 0 <= index < values.len() implies min_of(costs) <= #[trigger] values[index] by {
            assert(values.contains(values[index]));
            let position = choose|position: int| 0 <= position < costs.len() && #[trigger] costs[position] == values[index];
        }
        let value_position = choose|position: int| 0 <= position < values.len() && #[trigger] values[position] == min_of(costs);
        assert(costs.contains(min_of(values)));
        let cost_position = choose|position: int| 0 <= position < costs.len() && #[trigger] costs[position] == min_of(values);
        assert(min_of(values) == min_of(costs));
    }
    assert(scalar_1_value(db, p, part) == if costs.len() == 0 { None } else { Some(min_of(costs)) });
}

}
