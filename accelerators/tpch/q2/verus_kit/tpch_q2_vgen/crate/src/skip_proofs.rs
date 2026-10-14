use vstd::prelude::*;
use std::sync::Arc;
use verus_common::rel::*;
use verus_common::order::*;
use verus_common::sums::*;
use crate::gen_spec::*;
use crate::gen_obligations::*;
use crate::bags::*;
use crate::storage::*;
use crate::model::*;
use crate::selection::*;
use crate::enumeration::*;

verus! {

pub open spec fn all_acctbal(rows: Seq<OutRow>, a: int) -> bool {
    forall|i: int| 0 <= i < rows.len() ==> (#[trigger] rows[i]).s_acctbal == a
}

pub proof fn lemma_flat_acctbal(seqs: Seq<Seq<OutRow>>, a: int)
    requires forall|i: int| 0 <= i < seqs.len() ==> all_acctbal(#[trigger] seqs[i], a),
    ensures all_acctbal(flat(seqs), a),
    decreases seqs.len(),
{
    if seqs.len() > 0 {
        let prefix = seqs.drop_last();
        assert forall|i: int| 0 <= i < prefix.len() implies all_acctbal(#[trigger] prefix[i], a) by { assert(prefix[i] == seqs[i]); }
        lemma_flat_acctbal(prefix, a);
        let last = seqs.last();
        assert(last == seqs[seqs.len() - 1]);
        assert(flat(seqs) == flat(prefix) + last);
        assert forall|i: int| 0 <= i < flat(seqs).len() implies (#[trigger] flat(seqs)[i]).s_acctbal == a by {
            if i < flat(prefix).len() { assert(flat(seqs)[i] == flat(prefix)[i]); }
            else { assert(flat(seqs)[i] == last[i - flat(prefix).len()]); }
        }
    }
}

pub proof fn lemma_part_outputs_acctbal(parts: Seq<PartCnt>, quote: Counted<BrassPartsuppScan>, supplier: Counted<SupplierScan>,
    nation: Counted<NationScan>, region: i128, a: int)
    requires (*supplier.row).s_acctbal as int == a,
    ensures all_acctbal(part_outputs(parts, quote, supplier, nation, region), a),
{
    let mapped = parts.map_values(|part: PartCnt| part_output(part, quote, supplier, nation, region));
    assert forall|i: int| 0 <= i < mapped.len() implies all_acctbal(#[trigger] mapped[i], a) by {
        let part = parts[i];
        assert(mapped[i] == part_output(part, quote, supplier, nation, region));
        let copies = part_output(part, quote, supplier, nation, region);
        assert forall|j: int| 0 <= j < copies.len() implies (#[trigger] copies[j]).s_acctbal == a by {
            assert(copies[j] == projected_output(part.row, *supplier.row, *nation.row));
        }
    }
    lemma_flat_acctbal(mapped, a);
}

pub proof fn lemma_quote_output_acctbal(quote: Counted<BrassPartsuppScan>, parts: Seq<PartCnt>, cost: i64,
    suppliers: Directory<Bag<SupplierScan>>, nations: Directory<Bag<NationScan>>, regions: Directory<Count>, a: int)
    requires match suppliers.lookup((*quote.row).ps_suppkey) {
        None => true,
        Some(bag) => forall|i: int| 0 <= i < (*bag).entries@.len() ==> (*(#[trigger] (*bag).entries@[i]).row).s_acctbal as int == a,
    },
    ensures all_acctbal(quote_output(quote, parts, cost, suppliers, nations, regions), a),
{
    if (*quote.row).ps_supplycost == cost {
        if let Some(bag) = suppliers.lookup((*quote.row).ps_suppkey) {
            let entries = (*bag).entries@;
            let mapped = entries.map_values(|supplier: Counted<SupplierScan>| supplier_output(supplier, parts, quote, nations, regions));
            assert forall|i: int| 0 <= i < mapped.len() implies all_acctbal(#[trigger] mapped[i], a) by {
                let supplier = entries[i];
                assert(mapped[i] == supplier_output(supplier, parts, quote, nations, regions));
                if let Some(nation_bag) = nations.lookup((*supplier.row).s_nationkey) {
                    let nentries = (*nation_bag).entries@;
                    let nmapped = nentries.map_values(|nation: Counted<NationScan>| nation_output(nation, parts, quote, supplier, regions));
                    assert forall|j: int| 0 <= j < nmapped.len() implies all_acctbal(#[trigger] nmapped[j], a) by {
                        let nation = nentries[j];
                        assert(nmapped[j] == nation_output(nation, parts, quote, supplier, regions));
                        if let Some(region) = regions.lookup((*nation.row).n_regionkey) {
                            if (*region).value > 0 { lemma_part_outputs_acctbal(parts, quote, supplier, nation, (*region).value, a); }
                        }
                    }
                    lemma_flat_acctbal(nmapped, a);
                }
            }
            lemma_flat_acctbal(mapped, a);
        }
    }
}

/// Rows that all sort after the last of the kept hundred leave the kept hundred as they are.
pub proof fn lemma_skip_block(ord: Seq<OutRow>, block: Seq<OutRow>) -> (result: Seq<OutRow>)
    requires nondesc(ord, ext_order()), limit_rows(ord, 100).len() == 100,
        forall|i: int| 0 <= i < block.len() ==> ext_lt(limit_rows(ord, 100)[99], #[trigger] block[i]),
    ensures nondesc(result, ext_order()), result.to_multiset() == (ord + block).to_multiset(),
        limit_rows(result, 100) == limit_rows(ord, 100),
    decreases block.len(),
{
    lemma_ext_order();
    if block.len() == 0 {
        assert(ord + block =~= ord);
        ord
    } else {
        let prefix = block.drop_last();
        assert forall|i: int| 0 <= i < prefix.len() implies ext_lt(limit_rows(ord, 100)[99], #[trigger] prefix[i]) by { assert(prefix[i] == block[i]); }
        let before = lemma_skip_block(ord, prefix);
        let row = block.last();
        assert(row == block[block.len() - 1]);
        lemma_repeat_append(before, row, 1);
        let after = repeat_sorted(before, row, 1);
        assert(repeat_sorted(before, row, 0) == before);
        assert(after == insert_by(before, row, ext_order()));
        lemma_repeat_prefix(before, row, 1);
        let kept = limit_rows(before, 100);
        assert(kept == limit_rows(ord, 100));
        assert(!ext_lt(row, kept[99])) by {
            assert(ext_lt(kept[99], row));
            if ext_lt(row, kept[99]) { assert(ext_order()(row, kept[99])); assert(ext_order()(kept[99], row)); assert(ext_order()(row, row)); }
        }
        lemma_reject_limited(kept, row);
        assert(repeat_retained(kept, row, 0) == kept);
        assert(repeat_retained(kept, row, 1) == bounded_insert(kept, row));
        assert(limit_rows(after, 100) == kept);
        broadcast use vstd::multiset::group_multiset_axioms;
        verus_common::sums::lemma_to_multiset_concat(before, copies_of(row, 1));
        verus_common::sums::lemma_to_multiset_concat(ord, prefix);
        verus_common::sums::lemma_to_multiset_concat(ord, block);
        verus_common::sums::lemma_to_multiset_concat(prefix, seq![row]);
        assert(block =~= prefix + seq![row]);
        assert(copies_of(row, 1) =~= seq![row]);
        assert(after.to_multiset() == before.to_multiset().add(seq![row].to_multiset()));
        assert(block.to_multiset() == prefix.to_multiset().add(seq![row].to_multiset()));
        assert(after.to_multiset() =~= (ord + block).to_multiset());
        after
    }
}

}
