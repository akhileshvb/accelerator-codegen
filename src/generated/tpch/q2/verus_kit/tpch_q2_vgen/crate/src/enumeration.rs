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
use verus_common::sqlfns::like_match;
use verus_common::sums::*;

verus! {

/// A part row with the number of copies (a spec-level stand-in for a bag entry).
pub struct PartCnt {
    pub row: PartRow,
    pub count: i128,
}

pub open spec fn projected_output(part: PartRow, supplier: SupplierScan, nation: NationScan) -> OutRow {
    OutRow { s_acctbal: supplier.s_acctbal as int, s_name: supplier.s_name@, n_name: nation.n_name@,
        p_partkey: part.p_partkey, p_mfgr: part.p_mfgr, s_address: supplier.s_address@,
        s_phone: supplier.s_phone@, s_comment: supplier.s_comment@ }
}

pub open spec fn part_output(part: PartCnt, quote: Counted<BrassPartsuppScan>,
    supplier: Counted<SupplierScan>, nation: Counted<NationScan>, region: i128) -> Seq<OutRow>
{
    copies_of(projected_output(part.row, *supplier.row, *nation.row),
        (part.count as int * quote.count as int * supplier.count as int * nation.count as int * region as int) as nat)
}

pub open spec fn part_outputs(parts: Seq<PartCnt>, quote: Counted<BrassPartsuppScan>,
    supplier: Counted<SupplierScan>, nation: Counted<NationScan>, region: i128) -> Seq<OutRow>
{
    flat(parts.map_values(|part: PartCnt| part_output(part, quote, supplier, nation, region)))
}

pub open spec fn nation_output(nation: Counted<NationScan>, parts: Seq<PartCnt>,
    quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, regions: Directory<Count>) -> Seq<OutRow>
{
    match regions.lookup((*nation.row).n_regionkey) {
        None => Seq::empty(),
        Some(region) => if (*region).value > 0 { part_outputs(parts, quote, supplier, nation, (*region).value) } else { Seq::empty() },
    }
}

pub open spec fn nation_outputs(nations: Seq<Counted<NationScan>>, parts: Seq<PartCnt>,
    quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, regions: Directory<Count>) -> Seq<OutRow>
{
    flat(nations.map_values(|nation: Counted<NationScan>| nation_output(nation, parts, quote, supplier, regions)))
}

pub open spec fn supplier_output(supplier: Counted<SupplierScan>, parts: Seq<PartCnt>,
    quote: Counted<BrassPartsuppScan>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<OutRow>
{
    match nations.lookup((*supplier.row).s_nationkey) {
        None => Seq::empty(), Some(bag) => nation_outputs((*bag).entries@, parts, quote, supplier, regions),
    }
}

pub open spec fn supplier_outputs(suppliers: Seq<Counted<SupplierScan>>, parts: Seq<PartCnt>,
    quote: Counted<BrassPartsuppScan>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<OutRow>
{
    flat(suppliers.map_values(|supplier: Counted<SupplierScan>| supplier_output(supplier, parts, quote, nations, regions)))
}

pub open spec fn quote_output(quote: Counted<BrassPartsuppScan>, parts: Seq<PartCnt>, cost: i64,
    suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<OutRow>
{
    if (*quote.row).ps_supplycost != cost { Seq::empty() }
    else { match suppliers.lookup((*quote.row).ps_suppkey) {
        None => Seq::empty(), Some(bag) => supplier_outputs((*bag).entries@, parts, quote, nations, regions),
    } }
}

pub open spec fn quote_outputs(quotes: Seq<Counted<BrassPartsuppScan>>, parts: Seq<PartCnt>, cost: i64,
    suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>) -> Seq<OutRow>
{
    flat(quotes.map_values(|quote: Counted<BrassPartsuppScan>| quote_output(quote, parts, cost, suppliers, nations, regions)))
}

pub proof fn lemma_part_outputs_step(parts: Seq<PartCnt>, index: int,
    quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, nation: Counted<NationScan>, region: i128)
    requires 0 <= index < parts.len(),
    ensures part_outputs(parts.take(index + 1), quote, supplier, nation, region) ==
        part_outputs(parts.take(index), quote, supplier, nation, region) + part_output(parts[index], quote, supplier, nation, region),
{
    assert(parts.take(index + 1) =~= parts.take(index).push(parts[index]));
    verus_prooflib::lemma_flat_map_push(parts.take(index), parts[index],
        |part: PartCnt| part_output(part, quote, supplier, nation, region));
}

pub proof fn lemma_nation_outputs_step(nations: Seq<Counted<NationScan>>, index: int, parts: Seq<PartCnt>,
    quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>, regions: Directory<Count>)
    requires 0 <= index < nations.len(),
    ensures nation_outputs(nations.take(index + 1), parts, quote, supplier, regions) ==
        nation_outputs(nations.take(index), parts, quote, supplier, regions) + nation_output(nations[index], parts, quote, supplier, regions),
{
    assert(nations.take(index + 1) =~= nations.take(index).push(nations[index]));
    verus_prooflib::lemma_flat_map_push(nations.take(index), nations[index],
        |nation: Counted<NationScan>| nation_output(nation, parts, quote, supplier, regions));
}

pub proof fn lemma_supplier_outputs_step(suppliers: Seq<Counted<SupplierScan>>, index: int, parts: Seq<PartCnt>,
    quote: Counted<BrassPartsuppScan>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>)
    requires 0 <= index < suppliers.len(),
    ensures supplier_outputs(suppliers.take(index + 1), parts, quote, nations, regions) ==
        supplier_outputs(suppliers.take(index), parts, quote, nations, regions) + supplier_output(suppliers[index], parts, quote, nations, regions),
{
    assert(suppliers.take(index + 1) =~= suppliers.take(index).push(suppliers[index]));
    verus_prooflib::lemma_flat_map_push(suppliers.take(index), suppliers[index],
        |supplier: Counted<SupplierScan>| supplier_output(supplier, parts, quote, nations, regions));
}

pub proof fn lemma_quote_outputs_step(quotes: Seq<Counted<BrassPartsuppScan>>, index: int, parts: Seq<PartCnt>, cost: i64,
    suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>)
    requires 0 <= index < quotes.len(),
    ensures quote_outputs(quotes.take(index + 1), parts, cost, suppliers, nations, regions) ==
        quote_outputs(quotes.take(index), parts, cost, suppliers, nations, regions) + quote_output(quotes[index], parts, cost, suppliers, nations, regions),
{
    assert(quotes.take(index + 1) =~= quotes.take(index).push(quotes[index]));
    verus_prooflib::lemma_flat_map_push(quotes.take(index), quotes[index],
        |quote: Counted<BrassPartsuppScan>| quote_output(quote, parts, cost, suppliers, nations, regions));
}

pub open spec fn pents_positive(entries: Seq<PartCnt>) -> bool {
    forall|index: int| 0 <= index < entries.len() ==> (#[trigger] entries[index]).count > 0
}

pub proof fn lemma_no_part_outputs(quotes: Seq<Counted<BrassPartsuppScan>>, cost: i64,
    suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>)
    ensures quote_outputs(quotes, Seq::empty(), cost, suppliers, nations, regions).len() == 0,
{
    let none = Seq::<PartCnt>::empty();
    assert forall|quote: Counted<BrassPartsuppScan>| #[trigger] quote_output(quote, none, cost, suppliers, nations, regions).len() == 0 by {
        if (*quote.row).ps_supplycost == cost {
            if let Some(bag) = suppliers.lookup((*quote.row).ps_suppkey) {
                assert forall|supplier: Counted<SupplierScan>| #[trigger] supplier_output(supplier, none, quote, nations, regions).len() == 0 by {
                    if let Some(nation_bag) = nations.lookup((*supplier.row).s_nationkey) {
                        assert forall|nation: Counted<NationScan>| #[trigger] nation_output(nation, none, quote, supplier, regions).len() == 0 by {
                            assert(part_outputs(none, quote, supplier, nation, 0).len() == 0) by {
                                assert(none.map_values(|part: PartCnt| part_output(part, quote, supplier, nation, 0)) =~= Seq::<Seq<OutRow>>::empty());
                            }
                            if let Some(region) = regions.lookup((*nation.row).n_regionkey) {
                                if (*region).value > 0 {
                                    assert(none.map_values(|part: PartCnt| part_output(part, quote, supplier, nation, (*region).value)) =~= Seq::<Seq<OutRow>>::empty());
                                }
                            }
                        }
                        verus_prooflib::lemma_flat_map_empty((*nation_bag).entries@, |nation: Counted<NationScan>| nation_output(nation, none, quote, supplier, regions));
                    }
                }
                verus_prooflib::lemma_flat_map_empty((*bag).entries@, |supplier: Counted<SupplierScan>| supplier_output(supplier, none, quote, nations, regions));
            }
        }
    }
    verus_prooflib::lemma_flat_map_empty(quotes, |quote: Counted<BrassPartsuppScan>| quote_output(quote, none, cost, suppliers, nations, regions));
}


}
