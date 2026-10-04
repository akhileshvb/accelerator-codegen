use vstd::prelude::*;
use verus_common::rel::*;
use verus_prooflib::ordered::{Index, Key, KeyView};
use crate::model::*;
use crate::gen_spec::OutRow;
use crate::gen_obligations::out_view;
use crate::output::OutputRow;
use crate::values::copy_bytes;

verus! {
pub open spec fn variant_output(supplier: int, variant: Variant, revenue: int) -> OutRow {
    OutRow { s_suppkey: supplier, s_name: variant.name@, s_address: variant.address@, s_phone: variant.phone@, total_revenue: revenue }
}
pub open spec fn repeated(row: OutRow, count: int) -> Seq<OutRow> {
    Seq::new(if count >= 0 { count as nat } else { 0nat }, |position: int| row)
}
pub open spec fn variant_rows(supplier: int, variant: Variant, revenue: int) -> Seq<OutRow> {
    repeated(variant_output(supplier, variant, revenue), variant.count as int)
}
pub open spec fn inner_rows(supplier: int, variants: Index<Variant>, revenue: int) -> Seq<OutRow> {
    flat(variants.entries().map_values(|entry: (KeyView, Variant)| variant_rows(supplier, entry.1, revenue)))
}
pub open spec fn supplier_rows(dimensions: Dimensions, supplier: int, revenue: int) -> Seq<OutRow> {
    match dimensions.at(number_key(supplier)) { Some(variants) => inner_rows(supplier, variants, revenue), None => Seq::empty() }
}
pub open spec fn emitted_rows(dimensions: Dimensions, winners: Seq<i64>, revenue: int) -> Seq<OutRow> {
    flat(winners.map_values(|supplier: i64| supplier_rows(dimensions, supplier as int, revenue)))
}
pub proof fn lemma_repeat_push(row: OutRow, count: int)
    requires count >= 0,
    ensures repeated(row, count + 1) == repeated(row, count).push(row),
{
    assert(repeated(row, count + 1) =~= repeated(row, count).push(row));
}
pub proof fn lemma_inner_step(entries: Seq<(KeyView, Variant)>, position: int, supplier: int, revenue: int)
    requires 0 <= position < entries.len(),
    ensures flat(entries.take(position + 1).map_values(|entry: (KeyView, Variant)| variant_rows(supplier, entry.1, revenue))) ==
        flat(entries.take(position).map_values(|entry: (KeyView, Variant)| variant_rows(supplier, entry.1, revenue))) + variant_rows(supplier, entries[position].1, revenue),
{
    let before = entries.take(position).map_values(|entry: (KeyView, Variant)| variant_rows(supplier, entry.1, revenue));
    let after = entries.take(position + 1).map_values(|entry: (KeyView, Variant)| variant_rows(supplier, entry.1, revenue));
    assert(after =~= before.push(variant_rows(supplier, entries[position].1, revenue)));
    assert(after.drop_last() =~= before);
}
pub proof fn lemma_winner_step(dimensions: Dimensions, winners: Seq<i64>, position: int, revenue: int)
    requires 0 <= position < winners.len(),
    ensures emitted_rows(dimensions, winners.take(position + 1), revenue) ==
        emitted_rows(dimensions, winners.take(position), revenue) + supplier_rows(dimensions, winners[position] as int, revenue),
{
    let before = winners.take(position).map_values(|supplier: i64| supplier_rows(dimensions, supplier as int, revenue));
    let after = winners.take(position + 1).map_values(|supplier: i64| supplier_rows(dimensions, supplier as int, revenue));
    assert(after =~= before.push(supplier_rows(dimensions, winners[position] as int, revenue)));
    assert(after.drop_last() =~= before);
}
pub open spec fn rows_ordered(rows: Seq<OutRow>) -> bool {
    forall|left: int, right: int| #![trigger rows[left], rows[right]] 0 <= left < right < rows.len() ==> rows[left].s_suppkey <= rows[right].s_suppkey
}
pub proof fn lemma_output_push(rows: Seq<OutRow>, row: OutRow)
    requires rows_ordered(rows), forall|position: int| 0 <= position < rows.len() ==> (#[trigger] rows[position]).s_suppkey <= row.s_suppkey,
    ensures rows_ordered(rows.push(row)), forall|position: int| 0 <= position < rows.push(row).len() ==> (#[trigger] rows.push(row)[position]).s_suppkey <= row.s_suppkey,
{
    assert forall|left: int, right: int| #![trigger rows.push(row)[left], rows.push(row)[right]] 0 <= left < right < rows.push(row).len() implies rows.push(row)[left].s_suppkey <= rows.push(row)[right].s_suppkey by {
        if right < rows.len() { assert(rows.push(row)[left] == rows[left]); assert(rows.push(row)[right] == rows[right]); }
    }
}
fn emit_inner(output: &mut Vec<OutputRow>, variants: &Index<Variant>, supplier: i64, revenue: i128) -> (result: bool)
    requires rows_ordered(out_view(old(output)@)), forall|position: int| 0 <= position < old(output).len() ==> (#[trigger] out_view(old(output)@)[position]).s_suppkey <= supplier as int,
    ensures result ==> out_view(final(output)@) == out_view(old(output)@) + inner_rows(supplier as int, *variants, revenue as int),
        result ==> rows_ordered(out_view(final(output)@)),
        result ==> forall|position: int| 0 <= position < final(output).len() ==> (#[trigger] out_view(final(output)@)[position]).s_suppkey <= supplier as int,
{
    let ghost initial = out_view(output@);
    let mut cursor = variants.cursor();
    let ghost entries = variants.entries();
    let mut processed = Ghost(0int);
    loop
        invariant 0 <= processed@ <= entries.len(), cursor.remaining() == entries.skip(processed@),
            entries == variants.entries(),
            initial == out_view(old(output)@),
            rows_ordered(out_view(output@)), forall|position: int| 0 <= position < output.len() ==> (#[trigger] out_view(output@)[position]).s_suppkey <= supplier as int,
            out_view(output@) == initial + flat(entries.take(processed@).map_values(|entry: (KeyView, Variant)| variant_rows(supplier as int, entry.1, revenue as int))),
        decreases cursor.work(),
    {
        let (key, variant) = match cursor.next() { Some(value) => value, None => {
            proof { assert(processed@ == entries.len()); assert(entries.take(processed@) =~= entries); }
            return true;
        } };
        let ghost profile = entries[processed@].1;
        let ghost row = variant_output(supplier as int, profile, revenue as int);
        let ghost before = out_view(output@);
        proof { assert(entries[processed@] == (key.view(), profile)); }
        let mut emitted = 0u64;
        while emitted < variant.count
            invariant emitted <= variant.count, out_view(output@) == before + repeated(row, emitted as int),
                profile == *variant, row == variant_output(supplier as int, profile, revenue as int),
                rows_ordered(out_view(output@)), forall|position: int| 0 <= position < output.len() ==> (#[trigger] out_view(output@)[position]).s_suppkey <= supplier as int,
            decreases variant.count - emitted,
        {
            if output.len() == usize::MAX { return false; }
            let ghost previous = out_view(output@);
            output.push((supplier, copy_bytes(&variant.name), copy_bytes(&variant.address), copy_bytes(&variant.phone), revenue));
            proof {
                lemma_repeat_push(row, emitted as int);
                assert(out_view(output@) =~= previous.push(row));
                lemma_output_push(previous, row);
                assert((before + repeated(row, emitted as int)).push(row) =~= before + repeated(row, emitted as int + 1));
            }
            emitted += 1;
        }
        proof { lemma_inner_step(entries, processed@, supplier as int, revenue as int); }
        processed = Ghost(processed@ + 1);
    }
}
pub fn emit(dimensions: &Dimensions, winners: &Vec<i64>, revenue: i128) -> (result: Option<Vec<OutputRow>>)
    requires forall|left: int, right: int| #![trigger winners@[left], winners@[right]] 0 <= left < right < winners.len() ==> winners@[left] < winners@[right],
    ensures result matches Some(rows) ==> out_view(rows@) == emitted_rows(*dimensions, winners@, revenue as int),
        dimensions.wf(), result matches Some(rows) ==> rows_ordered(out_view(rows@)),
{
    let shared_dimensions = dimensions.share();
    let mut output = Vec::new();
    let mut position = 0;
    while position < winners.len()
        invariant position <= winners.len(), out_view(output@) == emitted_rows(*dimensions, winners@.take(position as int), revenue as int),
            dimensions.wf(), rows_ordered(out_view(output@)),
            forall|left: int, right: int| #![trigger winners@[left], winners@[right]] 0 <= left < right < winners.len() ==> winners@[left] < winners@[right],
            position > 0 ==> forall|index: int| 0 <= index < output.len() ==> (#[trigger] out_view(output@)[index]).s_suppkey <= winners@[position as int - 1] as int,
        decreases winners.len() - position,
    {
        let supplier = winners[position];
        let ghost before = out_view(output@);
        proof {
            assert forall|index: int| 0 <= index < output.len() implies (#[trigger] out_view(output@)[index]).s_suppkey <= supplier as int by {
                if position > 0 { assert(winners@[position as int - 1] < supplier); }
                else { assert(before.len() == 0); }
            }
        }
        if let Some(variants) = dimensions.get(&Key::number(supplier)) {
            if !emit_inner(&mut output, &*variants, supplier, revenue) { return None; }
        }
        proof {
            assert(out_view(output@) == before + supplier_rows(*dimensions, supplier as int, revenue as int));
            lemma_winner_step(*dimensions, winners@, position as int, revenue as int);
        }
        position += 1;
    }
    proof { assert(winners@.take(position as int) =~= winners@); }
    Some(output)
}
}
