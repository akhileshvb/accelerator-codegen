use vstd::prelude::*;
use verus_common::sums::*;
use verus_common::rel::*;
use verus_prooflib::lemma_flat_map_weight;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::model::*;
use crate::bags::*;
use crate::storage::*;
use crate::maintenance::*;
use crate::relational::*;
use crate::result_proofs::*;
use crate::enumeration::*;
use crate::selection::copies_of;
use verus_common::sqlfns::like_match;
use crate::query_transfer::*;
use crate::minimum_proofs::*;

verus! {

pub open spec fn manufacturer_projection(key: int, mfgr: Seq<u8>, supplier: SupplierRow, nation: NationRow) -> OutRow {
    OutRow { s_acctbal: supplier.s_acctbal, s_name: supplier.s_name, n_name: nation.n_name, p_partkey: key, p_mfgr: mfgr,
        s_address: supplier.s_address, s_phone: supplier.s_phone, s_comment: supplier.s_comment }
}

pub open spec fn part_fold_weight(supplier: SupplierRow, nation: NationRow, weight: spec_fn(OutRow) -> int) -> spec_fn(PartCnt) -> int {
    |part: PartCnt| part.count as int * weight(row_projection(part.row, supplier, nation))
}

pub open spec fn part_fold(parts: Seq<PartCnt>, supplier: SupplierRow, nation: NationRow, weight: spec_fn(OutRow) -> int) -> int {
    sum(parts, part_fold_weight(supplier, nation, weight))
}

pub proof fn lemma_output_copies_weight(row: OutRow, copies: nat, weight: spec_fn(OutRow) -> int)
    ensures sum(copies_of(row, copies), weight) == copies as int * weight(row),
    decreases copies,
{
    if copies > 0 {
        lemma_output_copies_weight(row, (copies - 1) as nat, weight);
        assert(copies_of(row, copies) =~= copies_of(row, (copies - 1) as nat).push(row));
        lemma_sum_push(copies_of(row, (copies - 1) as nat), row, weight);
        assert((copies as int - 1) * weight(row) + weight(row) == copies as int * weight(row)) by (nonlinear_arith);
    } else { assert(copies_of(row, copies) =~= Seq::<OutRow>::empty()); }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_part_output_weight(part: PartCnt, quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, nation: Counted<NationScan>, region: i128, weight: spec_fn(OutRow) -> int)
    requires part.count > 0, quote.count > 0, supplier.count > 0, nation.count > 0, region >= 0,
    ensures sum(part_output(part, quote, supplier, nation, region), weight) ==
        (quote.count as int * supplier.count as int * nation.count as int * region as int) *
        (part.count as int * weight(row_projection(part.row, (*supplier.row).rv(), (*nation.row).rv()))),
{
    let product = part.count as int * quote.count as int * supplier.count as int * nation.count as int * region as int;
    assert(product >= 0) by (nonlinear_arith)
        requires part.count > 0, quote.count > 0, supplier.count > 0, nation.count > 0, region >= 0,
            product == part.count as int * quote.count as int * supplier.count as int * nation.count as int * region as int;
    lemma_output_copies_weight(row_projection(part.row, (*supplier.row).rv(), (*nation.row).rv()), product as nat, weight);
    assert(product * weight(projected_output(part.row, *supplier.row, *nation.row)) ==
        (quote.count as int * supplier.count as int * nation.count as int * region as int) *
        (part.count as int * weight(projected_output(part.row, *supplier.row, *nation.row)))) by (nonlinear_arith)
        requires product == part.count as int * quote.count as int * supplier.count as int * nation.count as int * region as int;
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_part_outputs_weight(parts: Seq<PartCnt>, quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, nation: Counted<NationScan>, region: i128, weight: spec_fn(OutRow) -> int)
    requires quote.count > 0, supplier.count > 0, nation.count > 0, region >= 0,
        forall|index: int| 0 <= index < parts.len() ==> (#[trigger] parts[index]).count > 0,
    ensures sum(part_outputs(parts, quote, supplier, nation, region), weight) ==
        (quote.count as int * supplier.count as int * nation.count as int * region as int) * part_fold(parts, (*supplier.row).rv(), (*nation.row).rv(), weight),
{
    let factor = quote.count as int * supplier.count as int * nation.count as int * region as int;
    let base = |part: PartCnt| part.count as int * weight(row_projection(part.row, (*supplier.row).rv(), (*nation.row).rv()));
    let raw = |part: PartCnt| sum(part_output(part, quote, supplier, nation, region), weight);
    lemma_flat_map_weight(parts, |part: PartCnt| part_output(part, quote, supplier, nation, region), weight, raw);
    assert forall|index: int| 0 <= index < parts.len() implies raw(#[trigger] parts[index]) == factor * base(parts[index]) by {
        lemma_part_output_weight(parts[index], quote, supplier, nation, region, weight);
    }
    let scaled = |part: PartCnt| factor * base(part);
    lemma_sum_ext(parts, raw, scaled);
    lemma_sum_scale(parts, base, scaled, factor);
    lemma_sum_ext_all(parts, base, part_fold_weight((*supplier.row).rv(), (*nation.row).rv(), weight));
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub proof fn lemma_nation_output_weight(nation: Counted<NationScan>, parts: Seq<PartCnt>, quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires quote.count > 0, supplier.count > 0, nation.count > 0,
        forall|index: int| 0 <= index < parts.len() ==> (#[trigger] parts[index]).count > 0,
        forall|key: i64| #[trigger] count_at(regions, key) >= 0,
    ensures sum(nation_output(nation, parts, quote, supplier, regions), weight) ==
        (quote.count as int * supplier.count as int) *
        (nation.count as int * (count_at(regions, (*nation.row).n_regionkey) * part_fold(parts, (*supplier.row).rv(), (*nation.row).rv(), weight))),
{
    if regions.lookup((*nation.row).n_regionkey) is Some {
        let region = regions.lookup((*nation.row).n_regionkey)->0;
        assert(count_at(regions, (*nation.row).n_regionkey) == (*region).value as int);
        if (*region).value > 0 {
            assert(nation_output(nation, parts, quote, supplier, regions) == part_outputs(parts, quote, supplier, nation, (*region).value));
            lemma_part_outputs_weight(parts, quote, supplier, nation, (*region).value, weight);
            assert((quote.count as int * supplier.count as int * nation.count as int * (*region).value as int) * part_fold(parts, (*supplier.row).rv(), (*nation.row).rv(), weight) ==
                (quote.count as int * supplier.count as int) * (nation.count as int * ((*region).value as int * part_fold(parts, (*supplier.row).rv(), (*nation.row).rv(), weight)))) by (nonlinear_arith);
            assert(sum(nation_output(nation, parts, quote, supplier, regions), weight) ==
                (quote.count as int * supplier.count as int) * (nation.count as int * (count_at(regions, (*nation.row).n_regionkey) * part_fold(parts, (*supplier.row).rv(), (*nation.row).rv(), weight))));
        } else {
            assert((*region).value == 0);
            assert(nation_output(nation, parts, quote, supplier, regions).len() == 0);
            lemma_sum_empty(nation_output(nation, parts, quote, supplier, regions), weight);
            assert((quote.count as int * supplier.count as int) * (nation.count as int * (count_at(regions, (*nation.row).n_regionkey) * part_fold(parts, (*supplier.row).rv(), (*nation.row).rv(), weight))) == 0) by (nonlinear_arith)
                requires count_at(regions, (*nation.row).n_regionkey) == 0;
        }
    } else {
        assert(count_at(regions, (*nation.row).n_regionkey) == 0);
        assert(nation_output(nation, parts, quote, supplier, regions).len() == 0);
        lemma_sum_empty(nation_output(nation, parts, quote, supplier, regions), weight);
        assert((quote.count as int * supplier.count as int) * (nation.count as int * (count_at(regions, (*nation.row).n_regionkey) * part_fold(parts, (*supplier.row).rv(), (*nation.row).rv(), weight))) == 0) by (nonlinear_arith)
            requires count_at(regions, (*nation.row).n_regionkey) == 0;
    }
}

pub proof fn lemma_nation_entries(entries: Seq<Counted<NationScan>>, weight: spec_fn(NationRow) -> int, raw: spec_fn(Counted<NationScan>) -> int)
    requires forall|entry: Counted<NationScan>| #[trigger] raw(entry) == entry.count as int * weight((*entry.row).rv()),
    ensures weighted_entries(entries, weight) == sum(entries, raw),
{
    lemma_sum_ext_all(entries, |entry: Counted<NationScan>| entry.count as int * weight((*entry.row).rv()), raw);
}

pub proof fn lemma_supplier_entries(entries: Seq<Counted<SupplierScan>>, weight: spec_fn(SupplierRow) -> int, raw: spec_fn(Counted<SupplierScan>) -> int)
    requires forall|entry: Counted<SupplierScan>| #[trigger] raw(entry) == entry.count as int * weight((*entry.row).rv()),
    ensures weighted_entries(entries, weight) == sum(entries, raw),
{
    lemma_sum_ext_all(entries, |entry: Counted<SupplierScan>| entry.count as int * weight((*entry.row).rv()), raw);
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_supplier_output_weight(state: State, supplier: Counted<SupplierScan>, parts: Seq<PartCnt>, quote: Counted<BrassPartsuppScan>, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), quote.count > 0, supplier.count > 0,
        forall|index: int| 0 <= index < parts.len() ==> (#[trigger] parts[index]).count > 0,
        forall|key: i64| #[trigger] count_at(regions, key) >= 0,
    ensures sum(supplier_output(supplier, parts, quote, state.nations, regions), weight) ==
        (quote.count as int * supplier.count as int) * index_nation(state, regions, (*supplier.row).s_nationkey, |nation: NationRow| part_fold(parts, (*supplier.row).rv(), nation, weight)),
{
    if state.nations.lookup((*supplier.row).s_nationkey) is Some {
        let bag = state.nations.lookup((*supplier.row).s_nationkey)->0;
        let raw = |nation: Counted<NationScan>| sum(nation_output(nation, parts, quote, supplier, regions), weight);
        let base = |nation: Counted<NationScan>| nation.count as int * (count_at(regions, (*nation.row).n_regionkey) * part_fold(parts, (*supplier.row).rv(), (*nation.row).rv(), weight));
        lemma_flat_map_weight((*bag).entries@, |nation: Counted<NationScan>| nation_output(nation, parts, quote, supplier, regions), weight, raw);
        assert forall|index: int| 0 <= index < (*bag).entries@.len() implies raw(#[trigger] (*bag).entries@[index]) ==
            (quote.count as int * supplier.count as int) * base((*bag).entries@[index]) by {
            lemma_nation_output_weight((*bag).entries@[index], parts, quote, supplier, regions, weight);
        }
        let scaled = |nation: Counted<NationScan>| (quote.count as int * supplier.count as int) * base(nation);
        lemma_sum_ext((*bag).entries@, raw, scaled);
        lemma_sum_scale((*bag).entries@, base, scaled, quote.count as int * supplier.count as int);
        lemma_nation_entries((*bag).entries@, |nation: NationRow| count_at(regions, nation.n_regionkey as i64) * (|nation: NationRow| part_fold(parts, (*supplier.row).rv(), nation, weight))(nation), base);
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_quote_output_weight(state: State, quote: Counted<BrassPartsuppScan>, parts: Seq<PartCnt>, cost: i64, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), quote.count > 0,
        forall|index: int| 0 <= index < parts.len() ==> (#[trigger] parts[index]).count > 0,
        forall|key: i64| #[trigger] count_at(regions, key) >= 0,
    ensures sum(quote_output(quote, parts, cost, state.suppliers, state.nations, regions), weight) ==
        if (*quote.row).ps_supplycost == cost {
            quote.count as int * index_supplier(state, regions, (*quote.row).ps_suppkey, |supplier: SupplierRow, nation: NationRow| part_fold(parts, supplier, nation, weight))
        } else { 0int },
{
    if (*quote.row).ps_supplycost == cost && state.suppliers.lookup((*quote.row).ps_suppkey) is Some {
        let bag = state.suppliers.lookup((*quote.row).ps_suppkey)->0;
        let raw = |supplier: Counted<SupplierScan>| sum(supplier_output(supplier, parts, quote, state.nations, regions), weight);
        let base = |supplier: Counted<SupplierScan>| supplier.count as int * index_nation(state, regions, (*supplier.row).s_nationkey, |nation: NationRow| part_fold(parts, (*supplier.row).rv(), nation, weight));
        lemma_flat_map_weight((*bag).entries@, |supplier: Counted<SupplierScan>| supplier_output(supplier, parts, quote, state.nations, regions), weight, raw);
        assert forall|index: int| 0 <= index < (*bag).entries@.len() implies raw(#[trigger] (*bag).entries@[index]) == quote.count as int * base((*bag).entries@[index]) by {
            lemma_supplier_output_weight(state, (*bag).entries@[index], parts, quote, regions, weight);
            let entry = (*bag).entries@[index];
            let amount = index_nation(state, regions, (*entry.row).s_nationkey, |nation: NationRow| part_fold(parts, (*entry.row).rv(), nation, weight));
            assert((quote.count as int * entry.count as int) * amount == quote.count as int * base(entry)) by (nonlinear_arith)
                requires base(entry) == entry.count as int * amount;
        }
        let scaled = |supplier: Counted<SupplierScan>| quote.count as int * base(supplier);
        lemma_sum_ext((*bag).entries@, raw, scaled);
        lemma_sum_scale((*bag).entries@, base, scaled, quote.count as int);
        let pair_weight = |supplier: SupplierRow, nation: NationRow| part_fold(parts, supplier, nation, weight);
        assert forall|entry: Counted<SupplierScan>| #[trigger] base(entry) == entry.count as int *
            (|supplier: SupplierRow| index_nation(state, regions, supplier.s_nationkey as i64, |nation: NationRow| pair_weight(supplier, nation)))((*entry.row).rv()) by {
            assert((|nation: NationRow| part_fold(parts, (*entry.row).rv(), nation, weight)) =~= (|nation: NationRow| pair_weight((*entry.row).rv(), nation)));
        }
        lemma_supplier_entries((*bag).entries@, |supplier: SupplierRow| index_nation(state, regions, supplier.s_nationkey as i64, |nation: NationRow| pair_weight(supplier, nation)), base);
    }
}

#[verifier::auto_ext_equal(assert, assert_by, ensures)]
pub(crate) proof fn lemma_quote_outputs_weight(state: State, key: i64, parts: Seq<PartCnt>, cost: i64, regions: Directory<Count>, weight: spec_fn(OutRow) -> int)
    requires state.meaning(), state.quotes.lookup(key) is Some,
        forall|index: int| 0 <= index < parts.len() ==> (#[trigger] parts[index]).count > 0,
        forall|region: i64| #[trigger] count_at(regions, region) >= 0,
    ensures sum(quote_outputs((state.quotes.lookup(key)->0).entries@, parts, cost, state.suppliers, state.nations, regions), weight) ==
        index_quote(state, regions, key, |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow|
            if quote.ps_supplycost == cost as int { part_fold(parts, supplier, nation, weight) } else { 0int }),
{
    let bag = state.quotes.lookup(key)->0;
    let joined = |quote: PartsuppRow, supplier: SupplierRow, nation: NationRow| if quote.ps_supplycost == cost as int { part_fold(parts, supplier, nation, weight) } else { 0int };
    let raw = |quote: Counted<BrassPartsuppScan>| sum(quote_output(quote, parts, cost, state.suppliers, state.nations, regions), weight);
    lemma_flat_map_weight((*bag).entries@, |quote: Counted<BrassPartsuppScan>| quote_output(quote, parts, cost, state.suppliers, state.nations, regions), weight, raw);
    let base = |quote: Counted<BrassPartsuppScan>| quote.count as int * index_supplier(state, regions, (*quote.row).ps_suppkey,
        |supplier: SupplierRow, nation: NationRow| joined((*quote.row).rv(), supplier, nation));
    assert forall|index: int| 0 <= index < (*bag).entries@.len() implies raw(#[trigger] (*bag).entries@[index]) == base((*bag).entries@[index]) by {
        let quote = (*bag).entries@[index];
        lemma_quote_output_weight(state, quote, parts, cost, regions, weight);
        if (*quote.row).ps_supplycost == cost {
            assert((|supplier: SupplierRow, nation: NationRow| joined((*quote.row).rv(), supplier, nation)) =~= (|supplier: SupplierRow, nation: NationRow| part_fold(parts, supplier, nation, weight)));
        } else {
            assert((|supplier: SupplierRow, nation: NationRow| joined((*quote.row).rv(), supplier, nation)) =~= (|supplier: SupplierRow, nation: NationRow| 0int));
            lemma_index_zero(state, regions, (*quote.row).ps_suppkey);
        }
    }
    lemma_sum_ext((*bag).entries@, raw, base);
    lemma_quote_entries((*bag).entries@, |quote: PartsuppRow| index_supplier(state, regions, quote.ps_suppkey as i64, |supplier: SupplierRow, nation: NationRow| joined(quote, supplier, nation)), base);
}


}
