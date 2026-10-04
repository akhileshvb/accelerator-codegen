use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::flat;
use verus_common::rel::min_of;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::bags::*;
use crate::model::*;
use crate::storage::*;
use crate::selection::copies_of;
use crate::output::supported_costs;

verus! {

pub open spec fn projected_output(part: BrassPartScan, supplier: SupplierScan, nation: NationScan) -> OutRow {
    OutRow { s_acctbal: supplier.s_acctbal as int, s_name: supplier.s_name@, n_name: nation.n_name@,
        p_partkey: part.p_partkey as int, p_mfgr: part.p_mfgr@, s_address: supplier.s_address@,
        s_phone: supplier.s_phone@, s_comment: supplier.s_comment@ }
}

pub open spec fn part_output(part: Counted<BrassPartScan>, quote: Counted<BrassPartsuppScan>,
    supplier: Counted<SupplierScan>, nation: Counted<NationScan>, region: i128) -> Seq<OutRow>
{
    copies_of(projected_output(*part.row, *supplier.row, *nation.row),
        (part.count as int * quote.count as int * supplier.count as int * nation.count as int * region as int) as nat)
}

pub open spec fn part_outputs(parts: Seq<Counted<BrassPartScan>>, quote: Counted<BrassPartsuppScan>,
    supplier: Counted<SupplierScan>, nation: Counted<NationScan>, region: i128) -> Seq<OutRow>
{
    flat(parts.map_values(|part: Counted<BrassPartScan>| part_output(part, quote, supplier, nation, region)))
}

pub open spec fn nation_output(nation: Counted<NationScan>, parts: Seq<Counted<BrassPartScan>>,
    quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, regions: Directory<Count>) -> Seq<OutRow>
{
    match regions.lookup((*nation.row).n_regionkey) {
        None => Seq::empty(),
        Some(region) => if (*region).value > 0 { part_outputs(parts, quote, supplier, nation, (*region).value) } else { Seq::empty() },
    }
}

pub open spec fn nation_outputs(nations: Seq<Counted<NationScan>>, parts: Seq<Counted<BrassPartScan>>,
    quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, regions: Directory<Count>) -> Seq<OutRow>
{
    flat(nations.map_values(|nation: Counted<NationScan>| nation_output(nation, parts, quote, supplier, regions)))
}

pub open spec fn supplier_output(supplier: Counted<SupplierScan>, parts: Seq<Counted<BrassPartScan>>,
    quote: Counted<BrassPartsuppScan>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<OutRow>
{
    match nations.lookup((*supplier.row).s_nationkey) {
        None => Seq::empty(), Some(bag) => nation_outputs((*bag).entries@, parts, quote, supplier, regions),
    }
}

pub open spec fn supplier_outputs(suppliers: Seq<Counted<SupplierScan>>, parts: Seq<Counted<BrassPartScan>>,
    quote: Counted<BrassPartsuppScan>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<OutRow>
{
    flat(suppliers.map_values(|supplier: Counted<SupplierScan>| supplier_output(supplier, parts, quote, nations, regions)))
}

pub open spec fn quote_output(quote: Counted<BrassPartsuppScan>, parts: Seq<Counted<BrassPartScan>>, cost: i64,
    suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<OutRow>
{
    if (*quote.row).ps_supplycost != cost { Seq::empty() }
    else { match suppliers.lookup((*quote.row).ps_suppkey) {
        None => Seq::empty(), Some(bag) => supplier_outputs((*bag).entries@, parts, quote, nations, regions),
    } }
}

pub open spec fn quote_outputs(quotes: Seq<Counted<BrassPartsuppScan>>, parts: Seq<Counted<BrassPartScan>>, cost: i64,
    suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<OutRow>
{
    flat(quotes.map_values(|quote: Counted<BrassPartsuppScan>| quote_output(quote, parts, cost, suppliers, nations, regions)))
}

pub(crate) open spec fn key_output(state: State, key: i64, size: i64, regions: Directory<Count>) -> Seq<OutRow> {
    match state.parts.lookup(key) {
        None => Seq::empty(), Some(part) => match (*part).sizes.lookup(size) {
            None => Seq::empty(), Some(parts) => match state.quotes.lookup(key) {
                None => Seq::empty(), Some(quotes) => {
                    let costs = supported_costs(*quotes, state.suppliers, state.nations, regions);
                    if costs.len() == 0 { Seq::empty() }
                    else { quote_outputs((*quotes).entries@, (*parts).entries@, min_of(costs) as i64, state.suppliers, state.nations, regions) }
                },
            },
        },
    }
}

pub(crate) open spec fn index_outputs(keys: Seq<(i64, Arc<Count>)>, state: State, size: i64, regions: Directory<Count>) -> Seq<OutRow> {
    flat(keys.map_values(|entry: (i64, Arc<Count>)| key_output(state, entry.0, size, regions)))
}

pub proof fn lemma_part_outputs_step(parts: Seq<Counted<BrassPartScan>>, index: int,
    quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, nation: Counted<NationScan>, region: i128)
    requires 0 <= index < parts.len(),
    ensures part_outputs(parts.take(index + 1), quote, supplier, nation, region) ==
        part_outputs(parts.take(index), quote, supplier, nation, region) + part_output(parts[index], quote, supplier, nation, region),
{
    assert(parts.take(index + 1) =~= parts.take(index).push(parts[index]));
    verus_prooflib::lemma_flat_map_push(parts.take(index), parts[index],
        |part: Counted<BrassPartScan>| part_output(part, quote, supplier, nation, region));
}

pub proof fn lemma_nation_outputs_step(nations: Seq<Counted<NationScan>>, index: int, parts: Seq<Counted<BrassPartScan>>,
    quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, regions: Directory<Count>)
    requires 0 <= index < nations.len(),
    ensures nation_outputs(nations.take(index + 1), parts, quote, supplier, regions) ==
        nation_outputs(nations.take(index), parts, quote, supplier, regions) + nation_output(nations[index], parts, quote, supplier, regions),
{
    assert(nations.take(index + 1) =~= nations.take(index).push(nations[index]));
    verus_prooflib::lemma_flat_map_push(nations.take(index), nations[index],
        |nation: Counted<NationScan>| nation_output(nation, parts, quote, supplier, regions));
}

pub proof fn lemma_supplier_outputs_step(suppliers: Seq<Counted<SupplierScan>>, index: int, parts: Seq<Counted<BrassPartScan>>,
    quote: Counted<BrassPartsuppScan>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>)
    requires 0 <= index < suppliers.len(),
    ensures supplier_outputs(suppliers.take(index + 1), parts, quote, nations, regions) ==
        supplier_outputs(suppliers.take(index), parts, quote, nations, regions) + supplier_output(suppliers[index], parts, quote, nations, regions),
{
    assert(suppliers.take(index + 1) =~= suppliers.take(index).push(suppliers[index]));
    verus_prooflib::lemma_flat_map_push(suppliers.take(index), suppliers[index],
        |supplier: Counted<SupplierScan>| supplier_output(supplier, parts, quote, nations, regions));
}

pub proof fn lemma_quote_outputs_step(quotes: Seq<Counted<BrassPartsuppScan>>, index: int, parts: Seq<Counted<BrassPartScan>>, cost: i64,
    suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>)
    requires 0 <= index < quotes.len(),
    ensures quote_outputs(quotes.take(index + 1), parts, cost, suppliers, nations, regions) ==
        quote_outputs(quotes.take(index), parts, cost, suppliers, nations, regions) + quote_output(quotes[index], parts, cost, suppliers, nations, regions),
{
    assert(quotes.take(index + 1) =~= quotes.take(index).push(quotes[index]));
    verus_prooflib::lemma_flat_map_push(quotes.take(index), quotes[index],
        |quote: Counted<BrassPartsuppScan>| quote_output(quote, parts, cost, suppliers, nations, regions));
}

pub(crate) proof fn lemma_index_outputs_step(keys: Seq<(i64, Arc<Count>)>, index: int, state: State, size: i64, regions: Directory<Count>)
    requires 0 <= index < keys.len(),
    ensures index_outputs(keys.take(index + 1), state, size, regions) ==
        index_outputs(keys.take(index), state, size, regions) + key_output(state, keys[index].0, size, regions),
{
    assert(keys.take(index + 1) =~= keys.take(index).push(keys[index]));
    verus_prooflib::lemma_flat_map_push(keys.take(index), keys[index],
        |entry: (i64, Arc<Count>)| key_output(state, entry.0, size, regions));
}

}
