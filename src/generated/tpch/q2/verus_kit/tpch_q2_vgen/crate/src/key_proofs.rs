use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use crate::gen_spec::*;
use crate::model::*;
use crate::bags::*;
use crate::storage::*;
use crate::maintenance::*;
use crate::relational::*;
use crate::result_proofs::*;
use crate::query_transfer::*;
use crate::minimum_proofs::*;
use crate::enumeration::*;
use crate::enumeration_proofs::*;
use crate::join_parts::*;

verus! {

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_quote_join_ext(db: Db, name: Seq<u8>, key: int, left: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int, right: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int)
    requires forall|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| quote.ps_partkey == key ==> #[trigger] left(quote, supplier, nation) == right(quote, supplier, nation),
    ensures quote_join(db, name, key, left) == quote_join(db, name, key, right),
{
    let left_raw = |quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| left(quote, supplier, nation)) } else { 0int };
    let right_raw = |quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| right(quote, supplier, nation)) } else { 0int };
    assert forall|quote: PartsuppRow| #[trigger] left_raw(quote) == right_raw(quote) by {
        if quote.ps_partkey == key {
            assert((|supplier: SupplierRow, nation: NationRow| left(quote, supplier, nation)) =~= (|supplier: SupplierRow, nation: NationRow| right(quote, supplier, nation)));
        }
    }
    lemma_quote_join_weight(db, name, key, left, left_raw);
    lemma_quote_join_weight(db, name, key, right, right_raw);
    lemma_sum_ext_all(db.partsupp, left_raw, right_raw);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_quote_join_zero(db: Db, name: Seq<u8>, key: int)
    ensures quote_join(db, name, key, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| 0int) == 0,
{
    let joined = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| 0int;
    let raw = |quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| joined(quote, supplier, nation)) } else { 0int };
    assert forall|quote: PartsuppRow| #[trigger] raw(quote) == 0 by {
        let supplier_weight = |supplier: SupplierRow, nation: NationRow| joined(quote, supplier, nation);
        let supplier_raw = |supplier: SupplierRow| if supplier.s_suppkey == quote.ps_suppkey { nation_join(db, name, supplier, |nation: NationRow| supplier_weight(supplier, nation)) } else { 0int };
        assert forall|supplier: SupplierRow| #[trigger] supplier_raw(supplier) == 0 by {
            let nation_weight = |nation: NationRow| supplier_weight(supplier, nation);
            lemma_nation_join_weight(db, name, supplier, nation_weight, |nation: NationRow| 0int);
            lemma_sum_zero(db.nation, |nation: NationRow| 0int);
        }
        lemma_supplier_join_weight(db, name, quote, supplier_weight, supplier_raw);
        lemma_sum_zero(db.supplier, supplier_raw);
    }
    lemma_quote_join_weight(db, name, key, joined, raw);
    lemma_sum_zero(db.partsupp, raw);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_quote_join_empty(db: Db, name: Seq<u8>, key: int, weight: spec_fn(PartsuppRow, SupplierRow, NationRow) -> int)
    requires forall|index: int| 0 <= index < db.partsupp.len() ==> (#[trigger] db.partsupp[index]).ps_partkey != key,
    ensures quote_join(db, name, key, weight) == 0,
{
    let raw = |quote: PartsuppRow| if quote.ps_partkey == key { supplier_join(db, name, quote, |supplier: SupplierRow, nation: NationRow| weight(quote, supplier, nation)) } else { 0int };
    lemma_quote_join_weight(db, name, key, weight, raw);
    lemma_sum_zero(db.partsupp, raw);
}

pub(crate) open spec fn key_query_weight(state: State, size: i64, name: Seq<u8>, key: i64, weight: spec_fn(OutRow) -> int) -> int {
    sum(state.db().part, |part: PartRow| if part.p_partkey == key as int { part_query_weight(state.db(), Params { p1: size as int, p2: name }, part, weight) } else { 0int })
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_key_query_live(state: State, size: i64, name: Seq<u8>, key: i64, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.db() == state.database@, manufacturer_bag(state.parts, key, size) is Some,
        state.quotes.lookup(key) is Some, key_costs(state, regions, key).len() > 0,
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
    ensures sum(key_output(state, key, size, regions), weight) == key_query_weight(state, size, name, key, weight),
{
    let db = state.db();
    let p = Params { p1: size as int, p2: name };
    let bag = manufacturer_bag(state.parts, key, size)->0;
    let parts = (*bag).entries@;
    let costs = key_costs(state, regions, key);
    let cost = min_of(costs);
    lemma_cost_bounds(state, regions, key, costs.len() as int);
    assert(costs.take(costs.len() as int) =~= costs);
    let position = choose|position: int| 0 <= position < costs.len() && #[trigger] costs[position] == cost;
    let mapper = |quote: Counted<crate::gen_obligations::BrassPartsuppScan>| if quote.count > 0 && crate::output::supported(*quote.row, state.suppliers, state.nations, regions) { seq![(*quote.row).ps_supplycost as int] } else { Seq::<int>::empty() };
    lemma_costs_mapper(state, regions, key, mapper);
    verus_prooflib::lemma_flat_map_contains((state.quotes.lookup(key)->0).entries@, mapper, cost);
    let quote_position = choose|index: int| 0 <= index < (state.quotes.lookup(key)->0).entries@.len() && #[trigger] mapper((state.quotes.lookup(key)->0).entries@[index]).contains(cost);
    assert(fits_i64(cost));
    lemma_regions_nonnegative(state, regions, name);
    lemma_quote_outputs_weight(state, key, parts, cost as i64, regions, weight);
    let indexed = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost == cost { part_fold(parts, supplier, nation, weight) } else { 0int };
    lemma_index_quote(state, regions, name, key, indexed);
    let keep = |part: PartRow| brass(part) && part.p_partkey == key as int && part.p_size == size as int;
    let row_weight = |part: PartRow, quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost == cost { weight(row_projection(part, supplier, nation)) } else { 0int };
    let joined = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| sum(db.part, |part: PartRow| if keep(part) { row_weight(part, quote, supplier, nation) } else { 0int });
    assert forall|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| #[trigger] indexed(quote, supplier, nation) == joined(quote, supplier, nation) by {
        if quote.ps_supplycost == cost {
            lemma_part_fold_meaning(state, key, size, supplier, nation, weight);
            assert((|part: PartRow| if keep(part) { row_weight(part, quote, supplier, nation) } else { 0int }) =~=
                (|part: PartRow| if brass(part) && part.p_partkey == key as int && part.p_size == size as int { weight(row_projection(part, supplier, nation)) } else { 0int }));
        } else { lemma_sum_zero(db.part, |part: PartRow| if keep(part) { row_weight(part, quote, supplier, nation) } else { 0int }); }
    }
    assert(indexed =~= joined);
    lemma_quote_parts(db, name, key as int, keep, row_weight);
    let normalized = |part: PartRow| if keep(part) { quote_join(db, name, key as int, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| row_weight(part, quote, supplier, nation)) } else { 0int };
    let desired = |part: PartRow| if part.p_partkey == key as int { part_query_weight(db, p, part, weight) } else { 0int };
    assert forall|part: PartRow| #[trigger] normalized(part) == desired(part) by {
        if keep(part) {
            lemma_minimum_query(state, regions, name, key, part);
            let left = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| row_weight(part, quote, supplier, nation);
            let right = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote_selected(db, p, part, quote) { weight(row_projection(part, supplier, nation)) } else { 0int };
            lemma_quote_join_ext(db, name, key as int, left, right);
        }
    }
    lemma_sum_ext_all(db.part, normalized, desired);
    assert(key_output(state, key, size, regions) == quote_outputs((state.quotes.lookup(key)->0).entries@, parts, cost as i64, state.suppliers, state.nations, regions));
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_key_query_weight(state: State, size: i64, name: Seq<u8>, key: i64, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.db() == state.database@,
        forall|region: i64| #[trigger] count_at(regions, region) == region_amount(state.regions, name, region),
    ensures sum(key_output(state, key, size, regions), weight) == key_query_weight(state, size, name, key, weight),
{
    let db = state.db();
    let p = Params { p1: size as int, p2: name };
    let desired = |part: PartRow| if part.p_partkey == key as int { part_query_weight(db, p, part, weight) } else { 0int };
    if manufacturer_bag(state.parts, key, size) is None {
        lemma_manufacturer_weight(state, key, size, |mfgr: Seq<u8>| 1int);
        let indicator = |part: PartRow| if brass(part) && part.p_partkey == key as int && part.p_size == size as int { 1int } else { 0int };
        assert(manufacturer_weight(state.parts, key, size, |mfgr: Seq<u8>| 1int) == 0);
        assert((|part: PartRow| if brass(part) && part.p_partkey == key as int && part.p_size == size as int { (|mfgr: Seq<u8>| 1int)(part.p_mfgr) } else { 0int }) =~= indicator);
        assert(sum(db.part, indicator) == 0);
        assert forall|index: int| 0 <= index < db.part.len() implies desired(#[trigger] db.part[index]) == 0 by {
            if brass(db.part[index]) && db.part[index].p_partkey == key as int && db.part[index].p_size == size as int { lemma_sum_pos(db.part, indicator, index); }
        }
        lemma_sum_zero(db.part, desired);
        assert(key_output(state, key, size, regions).len() == 0);
        lemma_sum_empty(key_output(state, key, size, regions), weight);
    } else if state.quotes.lookup(key) is None {
        assert(state.parts.lookup(key) is Some);
        assert(part_support(state.parts, key) > 0);
        assert(eligible(db, key) > 0);
        lemma_quote_weight(state, key, |quote: PartsuppRow| 1int);
        let indicator = |quote: PartsuppRow| if quote.ps_partkey == key as int { 1int } else { 0int };
        assert((|quote: PartsuppRow| if quote.ps_partkey == key as int { (|quote: PartsuppRow| 1int)(quote) } else { 0int }) =~= indicator);
        assert(sum(db.partsupp, indicator) == 0);
        assert forall|index: int| 0 <= index < db.partsupp.len() implies (#[trigger] db.partsupp[index]).ps_partkey != key as int by {
            if db.partsupp[index].ps_partkey == key as int { lemma_sum_pos(db.partsupp, indicator, index); }
        }
        assert forall|part: PartRow| #[trigger] desired(part) == 0 by {
            if part_selected(part, p) && part.p_partkey == key as int {
                lemma_quote_join_empty(db, name, key as int, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote_selected(db, p, part, quote) { weight(row_projection(part, supplier, nation)) } else { 0int });
            }
        }
        lemma_sum_zero(db.part, desired);
        assert(key_output(state, key, size, regions).len() == 0);
        lemma_sum_empty(key_output(state, key, size, regions), weight);
    } else if key_costs(state, regions, key).len() == 0 {
        assert forall|part: PartRow| #[trigger] desired(part) == 0 by {
            if part_selected(part, p) && part.p_partkey == key as int {
                lemma_minimum_query(state, regions, name, key, part);
                let joined = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote_selected(db, p, part, quote) { weight(row_projection(part, supplier, nation)) } else { 0int };
                assert(joined =~= (|quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| 0int));
                lemma_quote_join_zero(db, name, key as int);
            }
        }
        lemma_sum_zero(db.part, desired);
        assert(key_output(state, key, size, regions).len() == 0);
        lemma_sum_empty(key_output(state, key, size, regions), weight);
    } else { lemma_key_query_live(state, size, name, key, regions, weight); }
}

}
